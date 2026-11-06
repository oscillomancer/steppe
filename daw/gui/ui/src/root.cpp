// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/root.h"

#include "supple/ui/gl.h"

#include <SDL3/SDL.h>
#include <nanovg.h>
#include "supple/gui/gl_api.h"
#define NANOVG_GL3 1
#include <nanovg_gl.h>
extern "C" {  // compiled as C in nanovg_gl3.c; the header has no guards of its own
#include <nanovg_gl_utils.h>
}

#include <algorithm>
#include <cmath>
#include <utility>

namespace supple::ui {

struct Root::Cache {
    NVGLUframebuffer* fb = nullptr;
    NVGLUframebuffer* snapshot = nullptr;
    bool captured = false;
    int width = 0, height = 0;  // pixels
};

namespace {
unsigned modsFrom(SDL_Keymod m) {
    unsigned out = 0;
    if (m & SDL_KMOD_SHIFT) out |= kShift;
#if defined(__APPLE__)
    if (m & SDL_KMOD_GUI) out |= kCtrl;
    if (m & SDL_KMOD_CTRL) out |= kSuper;
#else
    if (m & SDL_KMOD_CTRL) out |= kCtrl;
    if (m & SDL_KMOD_GUI) out |= kSuper;
#endif
    if (m & SDL_KMOD_ALT) out |= kAlt;
    return out;
}

int buttonFrom(Uint8 b) {
    return b == SDL_BUTTON_LEFT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : b == SDL_BUTTON_RIGHT ? 3 : 0;
}

// Pointer events bubble from the target to its ancestors until handled.
bool deliver(Root& root, Node* target, const PointerEvent& e) {
    for (Node* n = target; n && n->root() == &root;) {
        if (n->enabled && n->onPointer(e)) return true;
        n = n->root() == &root ? n->parent() : nullptr;
    }
    return false;
}

void collectFocusable(Node& n, std::vector<Node*>& out) {
    if (!n.visible) return;
    if (n.focusable && n.enabled) out.push_back(&n);
    for (const auto& c : n.children()) collectFocusable(*c, out);
}
}  // namespace

Root::Root(gui::Window& window, const gui::Fonts& fonts, Theme theme)
    : window_(window),
      fonts_(fonts),
      theme_(std::move(theme)),
      overlay_(std::make_unique<Node>("overlay")),
      alive_(std::make_shared<Root*>(this)) {
    overlay_->attach(this);
}

Root::~Root() {
    *alive_ = nullptr;
    window_.forceCurrent();
    if (cache_ && cache_->fb) nvgluDeleteFramebuffer(cache_->fb);
    if (cache_ && cache_->snapshot) nvgluDeleteFramebuffer(cache_->snapshot);
}

void Root::setContent(std::unique_ptr<Node> content) {
    focused_ = hovered_ = captured_ = nullptr;
    pendingMotion_.reset();
    if (content_) {
        content_->attach(nullptr);
        retire(std::move(content_));
    }
    content_ = std::move(content);
    if (content_) content_->attach(this);
    relayout();
    damageAll();
}

void Root::retire(std::shared_ptr<Node> node) {
    if (dispatchDepth_ && node) retired_.push_back(std::move(node));
}

bool Root::attached(const Node* node) const { return node && node->root() == this; }

void Root::capture(Node* node) { captured_ = attached(node) ? node : nullptr; }

void Root::releaseWithin(const Node* subtree) {
    Dispatch dispatch(*this);
    if (!subtree) return;
    auto within = [&](const Node* node) {
        for (; node; node = node->parent())
            if (node == subtree) return true;
        return false;
    };
    if (within(captured_)) {
        pendingMotion_.reset();  // a hidden control takes no more of its drag
        Node* target = std::exchange(captured_, nullptr);
        // Cancel reaches disabled controls too: their gesture still needs ending.
        for (Node* n = target; n && attached(n);) {
            if (n->onPointer({PointerEvent::Type::Cancel, {}, 1})) break;
            n = attached(n) ? n->parent() : nullptr;
        }
    }
    if (within(focused_)) focus(nullptr);
}

Root::OverlayHandle::OverlayHandle(OverlayHandle&& other) noexcept
    : root_(std::move(other.root_)), node_(std::exchange(other.node_, nullptr)) {}

Root::OverlayHandle& Root::OverlayHandle::operator=(OverlayHandle&& other) noexcept {
    if (this != &other) {
        reset();
        root_ = std::move(other.root_);
        node_ = std::exchange(other.node_, nullptr);
    }
    return *this;
}

void Root::OverlayHandle::reset() {
    if (auto owner = root_.lock(); owner && *owner && node_)
        (void)(*owner)->overlay().remove(node_);
    node_ = nullptr;
    root_.reset();
}

Root::OverlayHandle Root::addOverlay(std::unique_ptr<Node> node) {
    Node* added = &overlay_->add(std::move(node));
    return OverlayHandle(alive_, added);
}

Node* Root::pointerTarget(Point point) {
    if (layoutPending_) layoutNow(Canvas(window_.vg(), fonts_, theme_));
    if (attached(captured_)) return captured_;
    if (Node* target = overlay_->hitTest(point)) return target;
    return content_ ? content_->hitTest(point) : nullptr;
}

Node* Root::find(std::string_view id) const {
    if (Node* n = overlay_->find(id)) return n;
    return content_ ? content_->find(id) : nullptr;
}

void Root::setTheme(Theme theme) {
    theme_ = std::move(theme);
    relayout();
    damageAll();
}

bool Root::wantsFrame() const { return !damage_.empty() || layoutPending_ || animate_; }

std::chrono::steady_clock::duration minFrameInterval(float refreshHz) {
    const double hz = refreshHz > 0 ? static_cast<double>(refreshHz) : 60.0;
    const double rate = hz / std::ceil(hz / 120.0);
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(0.75 / rate));
}

bool Root::frameReady(std::chrono::steady_clock::time_point now) const {
    return wantsFrame() && (!presented_ || now - *presented_ >= frameInterval_);
}

std::optional<std::chrono::steady_clock::time_point> Root::frameDeadline() const {
    if (!wantsFrame() || !presented_) return std::nullopt;
    return *presented_ + frameInterval_;
}

void Root::damage(const Rect& area) {
    const Rect r = area.intersect({0, 0, window_.width(), window_.height()});
    if (r.empty()) return;
    // Merge overlapping regions; past a handful, one bounding region is cheaper.
    for (Rect& d : damage_)
        if (!d.intersect(r).empty()) {
            d = d.unite(r);
            return;
        }
    damage_.push_back(r);
    if (damage_.size() > 8) {
        Rect all;
        for (const Rect& d : damage_) all = all.unite(d);
        damage_ = {all};
    }
}

void Root::damageAll() {
    damage_.clear();
    damage_.push_back({0, 0, window_.width(), window_.height()});
}

void Root::layoutNow(const Canvas& canvas) {
    const Rect full{0, 0, window_.width(), window_.height()};
    if (content_) content_->arrange(canvas, full);
    overlay_->setBounds(full);
    for (const auto& c : overlay_->children()) {
        const bool popup = std::find(popups_.begin(), popups_.end(), c.get()) != popups_.end();
        c->arrange(canvas, popup ? full : c->bounds());  // popups place themselves in the window
    }
    laidOut_ = {full.w, full.h};
    layoutPending_ = false;
    damageAll();  // positions may have moved anywhere
}

void Root::ensureCache() {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_.sdl(), &pw, &ph);
    if (cache_ && cache_->width == pw && cache_->height == ph) return;
    if (!cache_) cache_ = std::make_unique<Cache>();
    if (cache_->fb) nvgluDeleteFramebuffer(cache_->fb);
    if (cache_->snapshot) nvgluDeleteFramebuffer(cache_->snapshot);
    cache_->snapshot = nullptr;
    cache_->captured = false;
    cache_->fb = nvgluCreateFramebuffer(window_.vg(), pw, ph, 0);
    if (!cache_->fb) {  // never draw through a missing cache: try again next frame
        cache_.reset();
        return;
    }
    cache_->width = pw;
    cache_->height = ph;
    damageAll();
}

void Root::paintTree(Node& node, Canvas& canvas, const Rect& clip) {
    if (!node.visible) return;
    if (&node != overlay_.get() && node.bounds().intersect(clip).empty()) return;
    node.paint(canvas);
    if (node.drawsGl) paintGl(node, clip);
    const Rect inner = node.clipsChildren ? clip.intersect(node.bounds()) : clip;
    if (inner.empty()) return;
    if (node.clipsChildren) nvgScissor(window_.vg(), inner.x, inner.y, inner.w, inner.h);
    for (const auto& c : node.children()) paintTree(*c, canvas, inner);
    if (node.clipsChildren) nvgScissor(window_.vg(), clip.x, clip.y, clip.w, clip.h);
}

// NanoVG batches until nvgEndFrame, so a GL layer flushes what is pending,
// draws under a GL scissor, and resumes NanoVG with the same clip.
void Root::paintGl(Node& node, const Rect& clip) {
    NVGcontext* vg = window_.vg();
    nvgEndFrame(vg);
    const float w = window_.width(), h = window_.height();
    const Rect r = clip.intersect(node.bounds()).snapOut(ratio_);
    if (!r.empty()) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(std::lround(r.x * ratio_)),
                  static_cast<GLint>(std::lround((h - r.bottom()) * ratio_)),
                  static_cast<GLsizei>(std::lround(r.w * ratio_)),
                  static_cast<GLsizei>(std::lround(r.h * ratio_)));
        node.paintGl(GlFrame{w, h, ratio_, r});
        glDisable(GL_SCISSOR_TEST);
    }
    nvgBeginFrame(vg, w, h, ratio_);
    nvgScissor(vg, clip.x, clip.y, clip.w, clip.h);
}

void Root::frame(bool present) {
    flushMotion();  // the drag's latest position, painted in this frame
    Canvas canvas(window_.vg(), fonts_, theme_);
    NVGcontext* vg = window_.vg();
    window_.forceCurrent();  // a plugin editor may have switched contexts since the last frame
    const float w = window_.width(), h = window_.height(), ratio = window_.pixelRatio();
    ratio_ = ratio;
    if (laidOut_.w != w || laidOut_.h != h) layoutPending_ = true;
    if (layoutPending_) layoutNow(canvas);
    ensureCache();
    if (!cache_) {  // the framebuffer could not be made: skip this frame, retry on the next
        animate_ = true;
        return;
    }

    // 1. Repaint the damaged regions of the content into the cache.
    if (!damage_.empty()) {
        nvgluBindFramebuffer(cache_->fb);
        glViewport(0, 0, cache_->width, cache_->height);
        const Color ground = theme_.color("ground");
        glEnable(GL_SCISSOR_TEST);
        glClearColor(ground.r, ground.g, ground.b, 1.0f);
        std::vector<Rect> regions;
        for (const Rect& d : damage_) regions.push_back(d.snapOut(ratio));
        for (const Rect& r : regions) {
            // GL's origin is bottom-left.
            const auto px = static_cast<GLint>(std::lround(r.x * ratio));
            const auto py = static_cast<GLint>(std::lround((h - r.bottom()) * ratio));
            glScissor(px, py, static_cast<GLsizei>(std::lround(r.w * ratio)),
                      static_cast<GLsizei>(std::lround(r.h * ratio)));
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        nvgBeginFrame(vg, w, h, ratio);
        for (const Rect& r : regions) {
            nvgScissor(vg, r.x, r.y, r.w, r.h);
            if (content_) paintTree(*content_, canvas, r);
            nvgResetScissor(vg);
            stats_.repaintedArea += static_cast<double>(r.w) * static_cast<double>(r.h);
        }
        nvgEndFrame(vg);
        ++stats_.repaints;
        damage_.clear();
    }

    // 2. Compose snapshots into a size-matched target: SDL's offscreen EGL
    // surface can retain its creation size after a resize. Include overlays,
    // just as in a presented frame, without relying on that surface's size.
    if (!present && !cache_->snapshot)
        cache_->snapshot = nvgluCreateFramebuffer(vg, cache_->width, cache_->height, 0);
    cache_->captured = !present && cache_->snapshot;
    const GLuint target = cache_->captured ? cache_->snapshot->fbo : 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, cache_->fb->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(0, 0, cache_->width, cache_->height, 0, 0, cache_->width, cache_->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, cache_->width, cache_->height);
    if (!overlay_->children().empty()) {
        glClear(GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, w, h, ratio);
        paintTree(*overlay_, canvas, {0, 0, w, h});
        nvgEndFrame(vg);
    }
    animate_ = false;
    ++stats_.frames;
    if (present) {
        window_.present();
        presented_ = std::chrono::steady_clock::now();
        frameInterval_ = minFrameInterval(window_.refreshRate());
    }
}

gui::Image Root::readBack() const {
    window_.forceCurrent();
    glBindFramebuffer(GL_FRAMEBUFFER, cache_ && cache_->captured ? cache_->snapshot->fbo : 0);
    auto image = window_.readPixels();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return image;
}

void Root::focus(Node* node) {
    Dispatch dispatch(*this);
    if (!attached(node)) node = nullptr;
    if (node == focused_) return;
    Node* old = focused_;
    focused_ = node;
    if (old) {
        old->invalidate();
        old->onFocus(false);
    }
    if (focused_ == node && attached(node)) {
        node->invalidate();
        node->onFocus(true);
    }
    if (!attached(focused_)) focused_ = nullptr;
    const bool wantsText = focused_ && focused_->role() == Role::TextField;
    if (wantsText)
        SDL_StartTextInput(window_.sdl());
    else
        SDL_StopTextInput(window_.sdl());
}

void Root::forget(Node* node) {
    if (focused_ == node) focused_ = nullptr;
    if (hovered_ == node) hovered_ = nullptr;
    if (captured_ == node) {
        captured_ = nullptr;
        pendingMotion_.reset();
    }
}

void Root::flushMotion() {
    if (auto motion = std::exchange(pendingMotion_, std::nullopt)) pointer(*motion);
}

void Root::pointer(PointerEvent e) {
    Dispatch dispatch(*this);
    if (e.type != PointerEvent::Type::Cancel) lastPointer_ = e.pos;
    // A press outside the topmost popup closes every popup above the one it
    // lands in (all of them when it lands in none) and goes no further, so a
    // dialog's menu button closes its own menu instead of opening another.
    if (e.type == PointerEvent::Type::Down && !popups_.empty() && !captured_) {
        const auto hit = std::find_if(popups_.rbegin(), popups_.rend(),
                                      [&](const Node* p) { return p->bounds().contains(e.pos); });
        if (hit != popups_.rbegin()) {
            const auto keep = static_cast<std::size_t>(popups_.rend() - hit);
            while (popups_.size() > keep) closePopup();
            return;
        }
    }
    Node* target = pointerTarget(e.pos);
    if (e.type == PointerEvent::Type::Move && !captured_ && target != hovered_) {
        if (hovered_) {
            Node* old = std::exchange(hovered_, nullptr);
            old->invalidate();
            old->onHover(false);
        }
        hovered_ = target = pointerTarget(e.pos);
        if (hovered_) {
            hovered_->invalidate();
            hovered_->onHover(true);
        }
        target = pointerTarget(e.pos);
        if (hovered_ != target) hovered_ = nullptr;
    }
    if (e.type == PointerEvent::Type::Down) {
        for (unsigned retry = 0; retry < 16; ++retry) {
            Node* f = target;
            while (f && !f->focusable) f = f->parent();
            if (f == focused_) break;
            focus(f);
            target = pointerTarget(e.pos);
            if (retry == 15) return;  // callbacks continually replace their targets
        }
        captured_ = target;  // implicit capture for drags
    }
    if (target) deliver(*this, target, e);
    if (e.type == PointerEvent::Type::Up) captured_ = nullptr;
}

bool Root::click(std::string_view id, int button) {
    Node* n = find(id);
    if (!n || !n->visible || !n->enabled) return false;
    const Point c{n->bounds().x + n->bounds().w / 2, n->bounds().y + n->bounds().h / 2};
    pointer({PointerEvent::Type::Move, c});
    pointer({PointerEvent::Type::Down, c, button});
    pointer({PointerEvent::Type::Up, c, button});
    return true;
}

void Root::keyUp(int keycode, unsigned mods) {
    Dispatch dispatch(*this);
    const KeyEvent e{keycode, mods, false};
    for (Node* n = focused_; attached(n); n = attached(n) ? n->parent() : nullptr)
        if (n->enabled && n->onKeyUp(e)) return;
}

void Root::key(int keycode, unsigned mods, bool repeat) {
    Dispatch dispatch(*this);
    const KeyEvent e{keycode, mods, repeat};
    for (Node* n = focused_; attached(n); n = attached(n) ? n->parent() : nullptr)
        if (n->enabled && n->onKey(e)) return;
    if (keycode == SDLK_ESCAPE && !popups_.empty()) {
        closePopup();
        return;
    }
    if (auto callback = onUnhandledKey; popups_.empty() && callback && callback(e)) return;
    if (keycode == SDLK_TAB) {  // focus traversal
        std::vector<Node*> order;
        collectFocusable(*overlay_, order);
        if (content_) collectFocusable(*content_, order);
        if (order.empty()) return;
        auto it = std::find(order.begin(), order.end(), focused_);
        if (mods & kShift)
            focus(it == order.end() || it == order.begin() ? order.back() : *(it - 1));
        else
            focus(it == order.end() || it + 1 == order.end() ? order.front() : *(it + 1));
    }
}

void Root::text(std::string_view utf8) {
    Dispatch dispatch(*this);
    if (focused_ && focused_->enabled) focused_->onText(utf8);
}

void Root::composition(std::string_view utf8, int cursor) {
    Dispatch dispatch(*this);
    if (focused_ && focused_->enabled) focused_->onComposition(utf8, cursor);
}

void Root::setTextInputArea(const Rect& caret) {
    const float scale = window_.inputScale();
    const SDL_Rect r{static_cast<int>(caret.x * scale), static_cast<int>(caret.y * scale),
                     std::max(1, static_cast<int>(caret.w * scale)),
                     std::max(1, static_cast<int>(caret.h * scale))};
    SDL_SetTextInputArea(window_.sdl(), &r, 0);
}

std::string Root::clipboard() const {
    char* text = SDL_GetClipboardText();
    std::string out = text ? text : "";
    SDL_free(text);
    return out;
}

void Root::setClipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

void Root::handle(const SDL_Event& e) {
    // Input and window events of another window (a plug-in editor) are not
    // ours: their coordinates are that window's.
    if (SDL_Window* target = SDL_GetWindowFromEvent(&e); target && target != window_.sdl()) return;
    // Other input comes after the drag's latest position (a release ends the
    // drag where it was); wake-ups and other user events leave it pending.
    if (e.type != SDL_EVENT_MOUSE_MOTION && e.type < SDL_EVENT_USER) flushMotion();
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            const PointerEvent motion{
                PointerEvent::Type::Move,
                {e.motion.x / window_.inputScale(), e.motion.y / window_.inputScale()},
                0,
                1,
                modsFrom(SDL_GetModState())};
            if (attached(captured_) && captured_->coalescesDrag()) {
                pendingMotion_ = motion;  // one edit per frame, not per motion event
                animate_ = true;
                break;
            }
            flushMotion();
            pointer(motion);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            pointer({e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PointerEvent::Type::Down
                                                           : PointerEvent::Type::Up,
                     {e.button.x / window_.inputScale(), e.button.y / window_.inputScale()},
                     buttonFrom(e.button.button),
                     e.button.clicks,
                     modsFrom(SDL_GetModState())});
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            PointerEvent p{
                PointerEvent::Type::Wheel,
                {e.wheel.mouse_x / window_.inputScale(), e.wheel.mouse_y / window_.inputScale()}};
            p.mods = modsFrom(SDL_GetModState());
            p.wheelX = e.wheel.x;
            p.wheelY = e.wheel.y;
            pointer(p);
            break;
        }
        case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
            Dispatch dispatch(*this);
            if (hovered_) {
                Node* old = std::exchange(hovered_, nullptr);
                old->invalidate();
                old->onHover(false);
            }
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            key(static_cast<int>(e.key.key), modsFrom(e.key.mod), e.key.repeat);
            break;
        case SDL_EVENT_KEY_UP:
            keyUp(static_cast<int>(e.key.key), modsFrom(e.key.mod));
            break;
        case SDL_EVENT_TEXT_INPUT:
            text(e.text.text);
            break;
        case SDL_EVENT_TEXT_EDITING:
            composition(e.edit.text ? e.edit.text : "", e.edit.start);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            relayout();
            damageAll();
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            animate_ = true;  // the cache is intact; just present it again
            break;
        default:
            break;
    }
}

}  // namespace supple::ui
