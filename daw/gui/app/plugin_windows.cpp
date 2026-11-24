// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plugin_windows.h"
#include "supple/model/freeze.h"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <objc/runtime.h>
#endif
#include <clap/clap.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <tuple>

#include "supple/model/session.h"
#include "supple/views/object_name.h"

namespace supple::gui {
namespace {
double editorUnits(const Window& window) {
    // NSView sizes are points; CLAP/VST3 HWND and X11 sizes are device pixels.
    return window.nativeParent().kind == NativeWindow::Kind::Cocoa
               ? 1.0
               : static_cast<double>(window.pixelRatio());
}
void setEditorSize(Window& window, std::uint32_t width, std::uint32_t height) {
    const double pixelRatio = static_cast<double>(window.pixelRatio());
    const float nativeDensity = SDL_GetWindowPixelDensity(window.sdl());
    const double inputScale =
        nativeDensity > 0 ? pixelRatio / static_cast<double>(nativeDensity) : 1;
    const auto [w, h] =
        editorWindowSize(width, height, window.nativeParent().kind, pixelRatio, inputScale);
    SDL_SetWindowSize(window.sdl(), w, h);
}
std::uint32_t pluginSize(float size, const Window& window) {
    return static_cast<std::uint32_t>(
        std::max(1L, std::lround(static_cast<double>(size) * editorUnits(window))));
}
bool floats(const host::Instance& instance) {
    return nativeWindowKind() == NativeWindow::Kind::Cocoa && instance.isolated();
}
}  // namespace

bool editorScaleChanged(double given, double current) {
    return std::isfinite(given) && std::isfinite(current) && given > 0 && current > 0 &&
           std::fabs(current - given) > 1e-3 * given;
}

std::optional<host::Instance::EditorFrame> restoredEditorFrame(int x, int y, int width,
                                                               int height) {
    // The size is not known before the editor opens: any 48-point corner of
    // the window on a display is enough to find and drag it.
    if (!onDisplay(x, y, width > 0 ? width : 48, height > 0 ? height : 48)) return std::nullopt;
    host::Instance::EditorFrame frame;
    frame.x = x;
    frame.y = y;
    if (width > 0 && height > 0) {
        frame.width = static_cast<std::uint32_t>(width);
        frame.height = static_cast<std::uint32_t>(height);
    }
    return frame;
}

std::pair<int, int> editorWindowSize(std::uint32_t width, std::uint32_t height,
                                     NativeWindow::Kind kind, double pixelRatio,
                                     double inputScale) {
    if (kind == NativeWindow::Kind::Cocoa)
        return {static_cast<int>(width), static_cast<int>(height)};
    // Convert once, without an integer design-unit intermediate. For example,
    // 512 pixels at 150% remains 512 SDL pixels, rather than 341 design -> 511.
    const double factor = pixelRatio > 0 && inputScale > 0 ? inputScale / pixelRatio : 1;
    return {std::max(1, static_cast<int>(std::lround(width * factor))),
            std::max(1, static_cast<int>(std::lround(height * factor)))};
}

void applyEditorRequests(Window& window, const host::Instance::EditorRequests& requests) {
    if (requests.closed) return;
    if (requests.resize) setEditorSize(window, requests.width, requests.height);
    if (requests.show) window.show();
    // Both requests can be coalesced by the host; hide wins in that case.
    if (requests.hide) window.hide();
}

// --- PluginWindows

PluginWindows::PluginWindows(const Platform& platform, engine::EngineApp& app,
                             std::function<void()> wake)
    : platform_(platform), app_(app) {
    auto watcher = FdWatcher::create(std::move(wake));
    if (watcher)
        watcher_ = std::move(*watcher);
    else
        watcherError_ = watcher.error();
}

PluginWindows::~PluginWindows() { closeAll(); }

bool PluginWindows::isOpen(model::Id fx) const {
    return std::any_of(editors_.begin(), editors_.end(),
                       [&](const Editor& e) { return e.fx == fx; });
}

bool PluginWindows::hasFocus(model::Id fx) const {
    for (const Editor& e : editors_)
        if (e.fx == fx && e.window && e.window->sdl() &&
            (SDL_GetWindowFlags(e.window->sdl()) & SDL_WINDOW_INPUT_FOCUS) != 0)
            return true;
    return false;
}

void PluginWindows::noteFrame(Editor& editor, const host::Instance::EditorFrame& frame) {
    Placement p;
    p.x = frame.x;
    p.y = frame.y;
    if (editor.resizable && frame.width > 0 && frame.height > 0) {
        p.width = static_cast<int>(frame.width);
        p.height = static_cast<int>(frame.height);
    }
    if (editor.frame == p) return;
    editor.frame = p;
    layoutChanged_ = true;
}

std::optional<PluginWindows::Placement> PluginWindows::placementOf(const Editor& editor) {
    if (editor.fx && !editor.window) return editor.frame;  // bridge-owned (Cocoa)
    if (!editor.fx || !editor.window) return std::nullopt;
    Placement p;
    std::tie(p.x, p.y) = editor.window->position();
    if (editor.resizable) {
        p.width = static_cast<int>(editor.window->width());
        p.height = static_cast<int>(editor.window->height());
    }
    return p;
}

void PluginWindows::remember(const Editor& editor) {
    if (auto p = placementOf(editor)) placements_[*editor.fx] = *p;
}

void PluginWindows::place(Editor& editor) {
    if (!editor.fx || !editor.window) return;
    const auto it = placements_.find(*editor.fx);
    if (it == placements_.end()) return;
    const Placement& p = it->second;
    if (editor.resizable && p.width > 0 && p.height > 0) {
        auto accepted =
            editor.instance->resizeEditor(pluginSize(static_cast<float>(p.width), *editor.window),
                                          pluginSize(static_cast<float>(p.height), *editor.window));
        restoreContext();
        if (accepted && accepted->width > 0 && accepted->height > 0)
            setEditorSize(*editor.window, accepted->width, accepted->height);
    }
    const int w = static_cast<int>(editor.window->width());
    const int h = static_cast<int>(editor.window->height());
    if (onDisplay(p.x, p.y, w, h)) editor.window->setPosition(p.x, p.y);
}

json::Value PluginWindows::saveLayout() const {
    std::map<model::Id, Placement> all = placements_;
    for (const Editor& e : editors_)
        if (auto p = placementOf(e)) all[*e.fx] = *p;
    json::Value out = json::Value::object();
    for (const auto& [fx, p] : all) {
        json::Value v = json::Value::object();
        v["x"] = static_cast<std::int64_t>(p.x);
        v["y"] = static_cast<std::int64_t>(p.y);
        if (p.width > 0 && p.height > 0) {
            v["width"] = static_cast<std::int64_t>(p.width);
            v["height"] = static_cast<std::int64_t>(p.height);
        }
        out[fx.str()] = std::move(v);
    }
    return out;
}

void PluginWindows::loadLayout(const json::Value& layout) {
    placements_.clear();
    if (!layout.isObject()) return;
    for (const auto& m : layout.asObject()) {
        const auto fx = model::Id::parse(m.key);
        if (!fx || fx->kind != model::Kind::Fx || !m.value.isObject()) continue;
        auto number = [&](const char* key) -> std::optional<int> {
            const json::Value* v = m.value.find(key);
            if (!v || !v->isNumber()) return std::nullopt;
            return static_cast<int>(std::clamp<double>(v->asDouble(), -1e6, 1e6));
        };
        const auto x = number("x"), y = number("y");
        if (!x || !y) continue;
        Placement p{*x, *y, number("width").value_or(0), number("height").value_or(0)};
        if (p.width <= 0 || p.height <= 0) p.width = p.height = 0;
        placements_[*fx] = p;
    }
}

NativeWindow PluginWindows::floatingParent() const {
    std::uintptr_t number = 0;
#if defined(__APPLE__)
    if (owner_) {
        const auto props = SDL_GetWindowProperties(owner_->sdl());
        void* window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        using Selector = SEL (*)(const char*);
        using WindowNumber = long (*)(void*, SEL);  // NSInteger on supported 64-bit macOS
        static const auto selector =
            reinterpret_cast<Selector>(dlsym(RTLD_DEFAULT, "sel_registerName"));
        static const auto windowNumber =
            reinterpret_cast<WindowNumber>(dlsym(RTLD_DEFAULT, "objc_msgSend"));
        if (window && selector && windowNumber) {
            const long value = windowNumber(window, selector("windowNumber"));
            if (value > 0 &&
                static_cast<unsigned long>(value) <= std::numeric_limits<std::uint32_t>::max())
                number = static_cast<std::uintptr_t>(value);
        }
    }
#endif
    return {NativeWindow::Kind::Floating, number};
}

void PluginWindows::appActivated() {
    for (auto& editor : editors_) {
        if (editor.window || (editor.fx && app_.instanceOf(*editor.fx) != editor.instance) ||
            !editor.instance->editorOpen())
            continue;
        auto shown = editor.instance->openEditor(floatingParent(), 1);
        if (!shown && onError) onError(shown.error());
    }
}

Status PluginWindows::attach(Editor editor, const std::string& title) {
    editor.serial = nextSerial_;
    nextSerial_ += 2;
    if (watcher_)
        if (auto error = watcher_->error()) watcherError_ = std::move(error);
    if (!editor.instance->hasEditor())
        return Error("gui.editor", title + " has no editor this host can embed", "",
                     "use a plugin with a native editor, or restart its failed bridge");
    if (floats(*editor.instance)) {
        std::optional<host::Instance::EditorFrame> frame;
        if (editor.fx)
            if (const auto it = placements_.find(*editor.fx); it != placements_.end())
                frame = restoredEditorFrame(it->second.x, it->second.y, it->second.width,
                                            it->second.height);
        auto size = editor.instance->openEditor(floatingParent(), 1, frame);
        if (!size) return std::move(size).error();
        editor.resizable = size->resizable;
        editors_.push_back(std::move(editor));
        return {};
    }
    WindowOptions options;
    options.title = title;
    options.width = 640;
    options.height = 400;
    options.gl = false;
    options.resizable = false;
    // Floating above the app from the first map: hidden until the editor is in
    // and sized, owned by the app's window, a utility window.
    options.hidden = true;
    options.utility = true;
    options.parent = owner_;
    auto window = Window::open(platform_, options);
    if (!window) return std::move(window).error();
    const auto parent = (*window)->nativeParent();
    if (!parent.valid())
        return Error("gui.editor.parent", "no native editor parent is available", "/editor/parent",
                     "run the GUI in an interactive native desktop session");
    editor.scale = static_cast<double>((*window)->pixelRatio());
    auto size = editor.instance->openEditor(parent, editor.scale);
    restoreContext();
    if (!size) return std::move(size).error();
    if (watcherError_ && !editor.instance->watchedFds().empty()) {
        editor.instance->closeEditor();
        return *watcherError_;
    }
    if (size->width > 0 && size->height > 0) setEditorSize(**window, size->width, size->height);
    (*window)->setResizable(size->resizable);
    editor.window = std::move(*window);
    editor.resizable = size->resizable;
    place(editor);  // where this effect's editor was last
    editor.window->show();
    editors_.push_back(std::move(editor));
    return {};
}

Status PluginWindows::open(model::Id fx) {
    if (auto track = model::frozenOwner(app_.head(), fx))
        return model::frozenError(app_.head(), *track, "/fx");
    for (std::size_t index = 0; index < editors_.size(); ++index) {
        Editor& e = editors_[index];
        if (e.fx == fx) {
            if (app_.instanceOf(fx) != e.instance) {
                drop(index);
                return open(fx);
            }
            if (e.window) {
                e.window->show();
                SDL_RaiseWindow(e.window->sdl());
                return {};
            }
            auto shown = e.instance->openEditor(floatingParent(), 1);
            if (!shown) return std::move(shown).error();
            return {};
        }
    }
    const model::Session& s = app_.head();
    const model::FxInstance* info = s.fx.find(fx);
    if (!info) return Error("gui.editor", "no effect " + views::objectName(s, fx));
    host::Instance* instance = app_.instanceOf(fx);
    if (!instance)
        return Error("gui.editor", "the effect is not running", "",
                     "start the engine (engine.start) to open its editor");
    std::string title = info->plugin.name.empty() ? info->plugin.id : info->plugin.name;
    if (auto track = model::trackOfFx(s, fx); track && s.tracks.find(*track))
        title += " — " + views::objectName(s, *track);
    Editor editor;
    editor.fx = fx;
    editor.instance = instance;
    return attach(std::move(editor), title);
}

Status PluginWindows::open(host::Instance& instance, const std::string& title) {
    Editor editor;
    editor.instance = &instance;
    return attach(std::move(editor), title);
}

bool PluginWindows::reattach(Editor& editor) {
    host::Instance* instance = app_.instanceOf(*editor.fx);
    if (!instance || !instance->hasEditor()) return false;
    // An isolation-mode change can change Cocoa ownership: rebuild that entry.
    if (floats(*instance) != !editor.window) return false;
    if (editor.window) editor.scale = static_cast<double>(editor.window->pixelRatio());
    // A bridge-owned window of the replacement opens where the last one was.
    std::optional<host::Instance::EditorFrame> frame;
    if (!editor.window && editor.frame)
        frame = restoredEditorFrame(editor.frame->x, editor.frame->y, editor.frame->width,
                                    editor.frame->height);
    auto size = instance->openEditor(
        editor.window ? editor.window->nativeParent() : floatingParent(), editor.scale, frame);
    restoreContext();
    if (!size) return false;
    if (watcherError_ && !instance->watchedFds().empty()) {
        instance->closeEditor();
        return false;
    }
    editor.instance = instance;
    editor.serial = nextSerial_;  // a new registration: old readiness is not its own
    nextSerial_ += 2;
    // A resizable editor keeps the window's size; the window keeps its place.
    if (editor.window && size->resizable && editor.resizable) {
        if (auto kept =
                instance->resizeEditor(pluginSize(editor.window->width(), *editor.window),
                                       pluginSize(editor.window->height(), *editor.window))) {
            size->width = kept->width;
            size->height = kept->height;
        }
        restoreContext();
    }
    editor.resizable = size->resizable;
    if (editor.window) {
        if (size->width > 0 && size->height > 0)
            setEditorSize(*editor.window, size->width, size->height);
        editor.window->setResizable(size->resizable);
    }
    return true;
}

bool PluginWindows::rescale(Editor& ed) {
    // Cocoa editors are in points and observe their backing scale themselves.
    if (!ed.window || !host::editorTakesScale(ed.window->nativeParent().kind) ||
        !ed.instance->editorOpen())
        return false;
    const double now = static_cast<double>(ed.window->pixelRatio());
    if (!editorScaleChanged(ed.scale, now)) return false;
    ed.scale = now;
    auto size = ed.instance->setEditorScale(now);
    restoreContext();
    if (!size) {
        if (onError) onError(size.error());
        return true;
    }
    if (size->width > 0 && size->height > 0) setEditorSize(*ed.window, size->width, size->height);
    return true;
}

void PluginWindows::drop(std::size_t index) {
    Editor& e = editors_[index];
    // The instance may already be gone (the engine replaced it): only close the
    // editor of the instance that is still the effect's.
    if (!e.fx || app_.instanceOf(*e.fx) == e.instance) {
        e.instance->closeEditor();
        // A bridge-owned window reports where it was as it closes.
        if (!e.window)
            if (const auto last = e.instance->takeClosedEditorFrame()) noteFrame(e, *last);
    }
    // closeEditor may supply a newer Cocoa frame than the last service pass.
    remember(e);
    editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(index));
    restoreContext();
}

Status PluginWindows::toggle(model::Id fx) {
    if (isOpen(fx)) {
        close(fx);
        return {};
    }
    return open(fx);
}

void PluginWindows::close(model::Id fx) {
    for (std::size_t i = 0; i < editors_.size(); ++i)
        if (editors_[i].fx == fx) return drop(i);
}

void PluginWindows::closeAll() {
    while (!editors_.empty()) drop(editors_.size() - 1);
}

bool PluginWindows::handle(const SDL_Event& e) {
    // Every event of an editor window is consumed here: the plug-in gets its
    // input from X11 directly, and the app's UI must not see it (a drag in an
    // editor moved the edit cursor, a click opened what was under it).
    SDL_Window* target = SDL_GetWindowFromEvent(&e);
    if (!target) return false;
    for (std::size_t i = 0; i < editors_.size(); ++i) {
        Editor& ed = editors_[i];
        if (!ed.window || ed.window->sdl() != target) continue;
        if (e.type < SDL_EVENT_WINDOW_FIRST || e.type > SDL_EVENT_WINDOW_LAST) return true;
        if (ed.fx && app_.instanceOf(*ed.fx) != ed.instance) {
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) drop(i);  // else service() reopens
            return true;
        }
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            drop(i);
        } else if (rescale(ed)) {
            // A scale change moves and resizes the window itself (WM_DPICHANGED);
            // that is not the user's resize, so the plug-in's answer decides.
        } else if (e.type == SDL_EVENT_WINDOW_RESIZED && ed.resizable &&
                   ed.instance->editorOpen()) {
            const auto width = pluginSize(ed.window->width(), *ed.window);
            const auto height = pluginSize(ed.window->height(), *ed.window);
            auto accepted = ed.instance->resizeEditor(width, height);
            if (accepted && (accepted->width != width || accepted->height != height))
                setEditorSize(*ed.window, accepted->width, accepted->height);
            restoreContext();
        }
        return true;
    }
    return false;
}

void PluginWindows::suspendFdWatches() {
    if (watcher_) watcher_->watch({});
}

bool PluginWindows::dispatchFds() {
    const auto ready = watcher_ ? watcher_->takeReady() : std::vector<FdWatch>{};
    suspendFdWatches();
    bool appReady = false;
    for (const FdWatch& r : ready) {
        if (r.owner == 0 || r.owner == 2) {
            appReady = true;  // consumed by EngineApp::tick / ModelLoop::pump
            continue;
        }
        for (const auto& w : serviceWatches_)
            if (w.serial == r.owner && w.source.fd == r.fd) {
                short flags = 0;
                if (r.flags & CLAP_POSIX_FD_READ) flags |= kWaitRead;
                if (r.flags & CLAP_POSIX_FD_WRITE) flags |= kWaitWrite;
                if (r.flags & CLAP_POSIX_FD_ERROR) flags |= kWaitError;
                app_.serviceFdReady(w.source, flags);
                appReady = true;
            }
        for (Editor& e : editors_)
            if (!e.fx && e.serial == r.owner)
                for (const auto& [fd, flags] : e.instance->watchedFds())
                    if (fd == r.fd) e.instance->onFd(fd, r.flags);
    }
    if (!editors_.empty()) restoreContext();
    return appReady;
}

int PluginWindows::service() {
    dispatchFds();
    if (watcher_)
        if (auto error = watcher_->error()) watcherError_ = std::move(error);
    if (watcherError_) {
        if (!watcherErrorReported_ && onError) {
            onError(*watcherError_);
            watcherErrorReported_ = true;
        }
        for (std::size_t i = editors_.size(); i-- > 0;)
            if ((!editors_[i].fx || app_.instanceOf(*editors_[i].fx) == editors_[i].instance) &&
                !editors_[i].instance->watchedFds().empty())
                drop(i);
    }
    // The engine closed the editor of an instance it replaced (fx.restart, a
    // VST3 key change): it reopens in the same window on the new instance.
    // An effect that is gone, or whose new instance cannot, loses its window.
    for (std::size_t i = editors_.size(); i-- > 0;) {
        Editor& e = editors_[i];
        auto* current = e.fx ? app_.instanceOf(*e.fx) : e.instance;
        if (e.fx && current && current != e.instance && floats(*current) != !e.window) {
            const auto fx = *e.fx;
            drop(i);
            auto reopened = open(fx);
            if (!reopened && onError) onError(reopened.error());
            continue;
        }
        if (e.fx && current != e.instance && !reattach(e)) {
            remember(e);
            editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(i));
            restoreContext();
            continue;
        }
        const auto requests = e.instance->takeEditorRequests();
        if (requests.frame && !e.window) noteFrame(e, *requests.frame);
        if (requests.closed) {
            remember(e);
            editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (e.window) applyEditorRequests(*e.window, requests);
    }
    // App-wide local instances (including closed editors, retired instances,
    // and probes) share this worker with caller-owned editors. The app owns
    // their timers. Remote instances wake only through setMainThreadWake.
    std::vector<FdWatch> fds;
    if (app_.serviceWaitable().valid())
        fds.push_back({app_.serviceWaitable(), CLAP_POSIX_FD_READ, 0});
    if (modelWake_.valid()) fds.push_back({modelWake_, CLAP_POSIX_FD_READ, 2});
    std::vector<ServiceWatch> services;
    for (const auto& source : app_.serviceFds()) {
        auto old = std::find_if(serviceWatches_.begin(), serviceWatches_.end(), [&](const auto& w) {
            return w.source.fd == source.fd && w.source.owner == source.owner &&
                   w.source.events == source.events;
        });
        const auto serial = old != serviceWatches_.end() ? old->serial : nextServiceSerial_;
        if (old == serviceWatches_.end()) nextServiceSerial_ += 2;
        services.push_back({source, serial});
        std::uint32_t flags = 0;
        if (source.events & kWaitRead) flags |= CLAP_POSIX_FD_READ;
        if (source.events & kWaitWrite) flags |= CLAP_POSIX_FD_WRITE;
        fds.push_back({source.fd, flags, serial});
    }
    serviceWatches_ = std::move(services);
    int next = -1;
    for (Editor& e : editors_) {
        if (e.fx || e.instance->isolated()) continue;
        for (const auto& [fd, flags] : e.instance->watchedFds())
            fds.push_back({fd, flags, e.serial});
        int due = e.instance->nextTimerMs();
        if (due == 0) {
            e.instance->serviceMainThread();  // runs the due timers
            due = e.instance->nextTimerMs();
        }
        if (due >= 0) next = next < 0 ? due : std::min(next, due);
    }
    if (watcher_ && !watcherError_) watcher_->watch(std::move(fds));
    if (!editors_.empty()) restoreContext();
    return next;
}

}  // namespace supple::gui
