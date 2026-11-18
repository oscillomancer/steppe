// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "floating_panel.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

#include "supple/gui/png.h"

namespace supple::gui {
namespace {

class PanelSurface : public ui::Node {
  public:
    explicit PanelSurface(std::unique_ptr<ui::Node> content) : Node("floating.surface") {
        layout.dir = ui::Layout::Dir::Column;
        add(std::move(content));
    }
    void paint(ui::Canvas& canvas) override {
        canvas.fillRect(bounds(), canvas.theme().color("raised"));
    }
};

}  // namespace

FloatingPanel::FloatingPanel(const Platform& platform, Window& owner, ui::Root& ownerRoot,
                             std::filesystem::path fonts, std::string title, int width, int height,
                             int minWidth, int minHeight)
    : platform_(platform),
      owner_(owner),
      ownerRoot_(ownerRoot),
      fonts_(std::move(fonts)),
      title_(std::move(title)),
      width_(width),
      height_(height),
      minWidth_(minWidth),
      minHeight_(minHeight) {}

FloatingPanel::~FloatingPanel() {
    close();
    if (window_) window_->forceCurrent();
    root_.reset();
    window_.reset();
    owner_.forceCurrent();
}

Status FloatingPanel::ensure() {
    if (window_) return {};
    WindowOptions options;
    options.title = title_;
    options.width = width_;
    options.height = height_;
    options.hidden = true;
    options.utility = true;
    options.parent = &owner_;
    auto w = Window::open(platform_, options);
    if (!w) {
        owner_.forceCurrent();
        return std::move(w).error();
    }
    auto fonts = loadFonts((*w)->vg(), fonts_);
    if (!fonts) {
        w->reset();
        owner_.forceCurrent();
        return std::move(fonts).error();
    }
    window_ = std::move(*w);
    root_ = std::make_unique<ui::Root>(*window_, *fonts, ownerRoot_.theme());
    root_->onUnhandledKey = [this](const ui::KeyEvent& e) {
        if (e.key == SDLK_ESCAPE) {
            close();
            return true;
        }
        return onUnhandledKey && onUnhandledKey(e, *root_);
    };
    minimumSize();
    owner_.forceCurrent();
    return {};
}

void FloatingPanel::minimumSize() {
    const float scale = window_->inputScale();
    SDL_SetWindowMinimumSize(window_->sdl(),
                             static_cast<int>(static_cast<float>(minWidth_) * scale),
                             static_cast<int>(static_cast<float>(minHeight_) * scale));
}

void FloatingPanel::place(std::optional<ui::Rect> anchor) {
    // Match PopupFrame's first-open geometry, including its edge clamping and
    // above-anchor fallback. Desktop coordinates use SDL units, not GL pixels.
    int width = std::max(minWidth_, std::min(width_, static_cast<int>(owner_.width()) - 16));
    int height = std::max(minHeight_, std::min(height_, static_cast<int>(owner_.height()) - 16));
    if (const auto* v = placement_.find("width")) width = static_cast<int>(v->asInt());
    if (const auto* v = placement_.find("height")) height = static_cast<int>(v->asInt());
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    float x = anchor ? anchor->x + 1 : (owner_.width() - w) / 2;
    float y = anchor ? anchor->bottom() + 3 : owner_.height() * 0.18f + 1;
    if (anchor && y + h + 1 > owner_.height() - 8 && anchor->y - h - 4 > 0) y = anchor->y - h - 3;
    x = std::clamp(x, 9.0f, std::max(9.0f, owner_.width() - w - 9));
    y = std::clamp(y, 9.0f, std::max(9.0f, owner_.height() - h - 9));
    window_->setSize(width, height);
    const auto [ox, oy] = owner_.position();
    int px = ox + static_cast<int>(std::lround(x * owner_.inputScale()));
    int py = oy + static_cast<int>(std::lround(y * owner_.inputScale()));
    if (const auto* savedX = placement_.find("x")) {
        const int sx = static_cast<int>(savedX->asInt());
        const int sy = static_cast<int>(placement_["y"].asInt());
        const float scale = window_->inputScale();
        if (onDisplay(sx, sy, static_cast<int>(window_->width() * scale),
                      static_cast<int>(window_->height() * scale))) {
            px = sx;
            py = sy;
        }
    }
    window_->setPosition(px, py);
}

void FloatingPanel::open(std::unique_ptr<ui::Node> content, std::function<void()> closed,
                         std::optional<ui::Rect> anchor) {
    root_->setTheme(ownerRoot_.theme());
    root_->setContent(std::make_unique<PanelSurface>(std::move(content)));
    closed_ = std::move(closed);
    place(anchor);
    open_ = true;
    std::function<ui::Node*(ui::Node&)> first = [&](ui::Node& n) -> ui::Node* {
        if (n.visible && n.enabled) {
            if (n.focusable) return &n;
            for (const auto& c : n.children())
                if (auto* hit = first(*c)) return hit;
        }
        return nullptr;
    };
    root_->focus(first(*root_->content()));
    raise();
}

void FloatingPanel::raise() {
    if (!open_ || platform_.driver() == Platform::Driver::Offscreen) return;
    window_->show();
    // Reopening deliberately raises this panel, even if already key or this
    // app is in the background; it follows an explicit user action.
    // Popup activation uses Window::activate instead and never re-raises an
    // already-key owner (notably the main window above Cocoa bridge editors).
    if (SDL_Window* focused = SDL_GetKeyboardFocus(); !focused || focused == window_->sdl())
        SDL_RaiseWindow(window_->sdl());
    else
        window_->activate();
}

void FloatingPanel::close() {
    if (!open_) return;
    placement_ = saveLayout();
    open_ = false;
    root_->closePopups();
    root_->releaseWithin(root_->content());
    root_->setContent(nullptr);
    window_->hide();
    if (auto closed = std::exchange(closed_, {})) closed();
}

bool FloatingPanel::handle(const SDL_Event& event) {
    if (!window_ || SDL_GetWindowFromEvent(&event) != window_->sdl()) return false;
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        close();
    else if (open_) {
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) minimumSize();
        root_->handle(event);
    }
    return true;
}

void FloatingPanel::frame(bool present) {
    if (!open_) return;
    const bool due =
        present ? root_->frameReady(std::chrono::steady_clock::now()) : root_->wantsFrame();
    if (due) root_->frame(present);  // Root binds with forceCurrent
}

std::optional<std::chrono::steady_clock::time_point> FloatingPanel::frameDeadline() const {
    return open_ ? root_->frameDeadline() : std::nullopt;
}

void FloatingPanel::setTheme(const ui::Theme& theme) {
    if (root_) root_->setTheme(theme);
}

void FloatingPanel::setTitle(std::string title) {
    if (title_ == title) return;
    title_ = std::move(title);
    if (window_) window_->setTitle(title_);
}

bool FloatingPanel::hasFocus() const {
    return open_ && root_->focused() &&
           (SDL_GetWindowFlags(window_->sdl()) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

json::Value FloatingPanel::saveLayout() const {
    if (!open_) return placement_;
    auto out = json::Value::object();
    const auto [x, y] = window_->position();
    out["x"] = x;
    out["y"] = y;
    out["width"] = static_cast<int>(window_->width());
    out["height"] = static_cast<int>(window_->height());
    return out;
}

void FloatingPanel::loadLayout(const json::Value& layout) {
    placement_ = json::Value();
    auto number = [&](const char* key) -> std::optional<int> {
        const auto* v = layout.find(key);
        if (!v || !v->isNumber() || !std::isfinite(v->asDouble())) return std::nullopt;
        return static_cast<int>(std::clamp(v->asDouble(), -1e6, 1e6));
    };
    const auto x = number("x"), y = number("y"), w = number("width"), h = number("height");
    if (x && y) {
        placement_["x"] = *x;
        placement_["y"] = *y;
    }
    if (w && h && *w > 0 && *h > 0) {
        placement_["width"] = std::clamp(*w, minWidth_, 8192);
        placement_["height"] = std::clamp(*h, minHeight_, 8192);
    }
    if (open_) place(std::nullopt);
}

Status FloatingPanel::snapshot(const std::filesystem::path& path) {
    if (!open_) return {};
    root_->damageAll();
    root_->frame(false);
    return writePng(path, opaque(root_->readBack()));
}

}  // namespace supple::gui
