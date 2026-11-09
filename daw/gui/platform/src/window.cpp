// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/gui/window.h"

#include <SDL3/SDL.h>
#if defined(__linux__)
#include <X11/Xlib.h>
#undef None
#undef Status
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#include <objc/runtime.h>
#endif
#include <nanovg.h>
#include "supple/product.h"
#if defined(SUPPLE_APP_ICON)
#include <stb_image.h>
#include "app_icon.h"
#endif
#include "supple/gui/gl_api.h"
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include "supple/base/files.h"

namespace supple::gui {
namespace {
constexpr const char* desktopDriver() {
#if defined(__APPLE__)
    return "cocoa";
#elif defined(_WIN32)
    return "windows";
#else
    return "x11";
#endif
}
constexpr const char* offscreenDriver() {
#if defined(__linux__)
    return "offscreen";
#else
    return desktopDriver();  // hidden native windows: no EGL dependency
#endif
}
void setAppIcon(SDL_Window* window) {
#if defined(SUPPLE_APP_ICON)
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        kAppIconPng, static_cast<int>(sizeof(kAppIconPng)), &width, &height, &channels, 4);
    if (!pixels) return;
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    if (surface) {
        (void)SDL_SetWindowIcon(window, surface);
        SDL_DestroySurface(surface);
    }
    stbi_image_free(pixels);
#else
    (void)window;  // macOS uses the .app's icns.
#endif
}
Error sdlError(std::string code, std::string what) {
    return Error(std::move(code), what + ": " + SDL_GetError(), {},
                 "the GUI needs a desktop session and OpenGL 3.3 core (4.1 on macOS)");
}
}  // namespace

Result<std::unique_ptr<Platform>> Platform::start(Driver driver) {
    // Plugin editors are X11 children of our windows, so DAW windows are X11
    // windows even on Wayland desktops (ADR-0005 §1).
    SDL_SetHint(SDL_HINT_APP_ID, std::string(product::kSlug).c_str());
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,
                driver == Driver::Offscreen ? offscreenDriver() : desktopDriver());
    if (!SDL_Init(SDL_INIT_VIDEO)) return sdlError("gui.platform", "SDL_Init");
    std::unique_ptr<Platform> p(new Platform(driver));
    p->wakeEvent_ = SDL_RegisterEvents(1);
    if (p->wakeEvent_ == 0) return sdlError("gui.platform", "SDL_RegisterEvents");
    if (!SDL_AddEventWatch(&Platform::watch, p.get()))
        return sdlError("gui.platform", "SDL_AddEventWatch");
    return p;
}

Platform::~Platform() {
    SDL_RemoveEventWatch(&Platform::watch, this);
    SDL_Quit();
}

bool Platform::watch(void* platform, SDL_Event* e) {
    // Runs on the thread that queues the event: focus events come from the
    // UI thread's event pump, wake events from any thread.
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        auto* p = static_cast<Platform*>(platform);
        p->lastFocused_.store(e->window.windowID, std::memory_order_relaxed);
        p->focusGains_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Platform::wake() const {
    SDL_Event e{};
    e.type = wakeEvent_;
    (void)SDL_PushEvent(&e);
}

Result<std::unique_ptr<Window>> Window::open(const Platform& platform,
                                             const WindowOptions& options) {
    if (!options.gl) {
        std::unique_ptr<Window> w(new Window);
        w->platform_ = &platform;
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
        if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
        if (options.utility) flags |= SDL_WINDOW_UTILITY;
        w->window_ = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
        if (!w->window_) return sdlError("gui.window", "SDL_CreateWindow");
        setAppIcon(w->window_);
        if (options.parent && options.parent->window_)
            (void)SDL_SetWindowParent(w->window_, options.parent->window_);
        return w;
    }
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    std::unique_ptr<Window> w(new Window);
    w->platform_ = &platform;
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (options.utility) flags |= SDL_WINDOW_UTILITY;
    if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
    w->window_ = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
    if (!w->window_) return sdlError("gui.window", "SDL_CreateWindow");
    setAppIcon(w->window_);
    w->setSize(options.width, options.height);
    if (options.parent && options.parent->window_)
        (void)SDL_SetWindowParent(w->window_, options.parent->window_);
    w->context_ = SDL_GL_CreateContext(w->window_);
    if (!w->context_) return sdlError("gui.window", "SDL_GL_CreateContext");
    SDL_GL_MakeCurrent(w->window_, static_cast<SDL_GLContext>(w->context_));
    // Event-driven: frames only when something changed, paced by vsync in
    // the focused window (present()) and by ui::Root's frame interval.
    w->vsync_ = SDL_GL_SetSwapInterval(1);
    if (!w->vsync_ && platform.driver() == Platform::Driver::Native)
        std::fprintf(stderr, "no vsync (%s): frames are paced by a timer\n", SDL_GetError());
    w->interval_ = w->vsync_ ? 1 : 0;
#if defined(_WIN32)
    if (!suppleLoadGl()) return sdlError("gui.gl", "loading OpenGL entry points");
#endif
    w->vg_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!w->vg_) return Error("gui.window", "NanoVG could not create its GL3 renderer");
    return w;
}

Window::~Window() {
    if (vg_) {
        SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
        nvgDeleteGL3(vg_);
    }
    if (context_) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
    if (window_) SDL_DestroyWindow(window_);
}

std::uint32_t Window::id() const { return SDL_GetWindowID(window_); }

void Window::activate() const {
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) != 0) return;
    // Automatic popup activation only transfers focus within this app.
    // Raising an already-key Cocoa owner would also cover bridged editors
    // without a FOCUS_GAINED event to restore their ordering.
    SDL_Window* focused = SDL_GetKeyboardFocus();
    if (!focused || focused == window_) return;
    SDL_RaiseWindow(window_);
#if defined(__linux__)
    // EWMH activation requests may be declined (e.g. Hyprland's default
    // focus policy). Transfer X11 focus only within this already-active app;
    // never force focus away from another application. Cocoa/Win32 use SDL's
    // native activation path above. libX11 is an existing OS dependency.
    SDL_Window* source = SDL_GetKeyboardFocus();
    if (!source) return;
    const auto props = SDL_GetWindowProperties(window_);
    auto* display = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    const auto xid =
        static_cast<::Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (!display || !xid) return;  // includes the offscreen driver
    struct X11Focus {
        SDL_SharedObject* library = SDL_LoadObject("libX11.so.6");
        decltype(&XGetWindowAttributes) attributes =
            library ? reinterpret_cast<decltype(attributes)>(
                          SDL_LoadFunction(library, "XGetWindowAttributes"))
                    : nullptr;
        decltype(&XGetInputFocus) current =
            library
                ? reinterpret_cast<decltype(current)>(SDL_LoadFunction(library, "XGetInputFocus"))
                : nullptr;
        decltype(&XSetInputFocus) focus =
            library ? reinterpret_cast<decltype(focus)>(SDL_LoadFunction(library, "XSetInputFocus"))
                    : nullptr;
        decltype(&XFlush) flush =
            library ? reinterpret_cast<decltype(flush)>(SDL_LoadFunction(library, "XFlush"))
                    : nullptr;
        ~X11Focus() {
            if (library) SDL_UnloadObject(library);
        }
    };
    static X11Focus api;
    XWindowAttributes attributes{};
    ::Window current = 0;
    int revert = 0;
    const auto sourceId = static_cast<::Window>(SDL_GetNumberProperty(
        SDL_GetWindowProperties(source), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (sourceId && api.attributes && api.current && api.focus && api.flush &&
        api.current(display, &current, &revert) && current == sourceId &&
        api.attributes(display, xid, &attributes) && attributes.map_state == IsViewable) {
        api.focus(display, xid, RevertToParent, CurrentTime);
        api.flush(display);
    }
#endif
}

void Window::setSize(int width, int height) {
    SDL_SetWindowSize(window_, static_cast<int>(static_cast<float>(width) * inputScale()),
                      static_cast<int>(static_cast<float>(height) * inputScale()));
}

void Window::setPosition(int x, int y) { SDL_SetWindowPosition(window_, x, y); }

std::pair<int, int> Window::position() const {
    int x = 0, y = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    return {x, y};
}

bool onDisplay(int x, int y, int width, int height) {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays) return false;
    bool shown = false;
    for (int i = 0; i < count && !shown; ++i) {
        SDL_Rect b{};
        if (!SDL_GetDisplayBounds(displays[i], &b)) continue;
        const int w = std::min(x + width, b.x + b.w) - std::max(x, b.x);
        const int h = std::min(y + height, b.y + b.h) - std::max(y, b.y);
        shown = w >= std::min(48, width) && h >= std::min(48, height);
    }
    SDL_free(displays);
    return shown;
}

void Window::setResizable(bool resizable) { SDL_SetWindowResizable(window_, resizable); }

void Window::setTitle(const std::string& title) { SDL_SetWindowTitle(window_, title.c_str()); }

float Window::width() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return static_cast<float>(w) / inputScale();
}

float Window::height() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return static_cast<float>(h) / inputScale();
}

float Window::inputScale() const {
#if defined(_WIN32)
    const float density = SDL_GetWindowPixelDensity(window_);
    const float scale = SDL_GetWindowDisplayScale(window_);
    return density > 0 && scale > 0 ? scale / density : 1.0f;
#else
    return 1.0f;
#endif
}

float Window::pixelRatio() const {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    return w > 0 ? static_cast<float>(pw) / static_cast<float>(w) * inputScale() : 1.0f;
}

float Window::refreshRate() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode ? mode->refresh_rate : 0.0f;
}

NativeWindow Window::nativeParent() const {
    const auto props = SDL_GetWindowProperties(window_);
#if defined(__APPLE__)
    void* window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!window) return {};
    // SDL exposes NSWindow*. The embedded plugin API requires its NSView*.
    // SDL has already loaded Cocoa. Resolve its runtime here so this accessor
    // does not add a host-library or Objective-C link dependency to gui-platform.
    using Selector = SEL (*)(const char*);
    using ContentView = void* (*)(void*, SEL);
    static const auto selector =
        reinterpret_cast<Selector>(dlsym(RTLD_DEFAULT, "sel_registerName"));
    static const auto contentView =
        reinterpret_cast<ContentView>(dlsym(RTLD_DEFAULT, "objc_msgSend"));
    if (!selector || !contentView) return {};
    void* view = contentView(window, selector("contentView"));
    return {NativeWindow::Kind::Cocoa, reinterpret_cast<std::uintptr_t>(view)};
#elif defined(_WIN32)
    void* window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    return {NativeWindow::Kind::Win32, reinterpret_cast<std::uintptr_t>(window)};
#else
    return {NativeWindow::Kind::X11, static_cast<std::uintptr_t>(SDL_GetNumberProperty(
                                         props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0))};
#endif
}

unsigned long Window::x11Window() const {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    return static_cast<unsigned long>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
}

std::string Window::renderer() const {
    makeCurrent();
    const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return std::string(r ? r : "?") + " | " + (v ? v : "?");
}

void Window::makeCurrent() const {
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
}

void Window::forceCurrent() const {
    if (!context_) return;
    SDL_GL_MakeCurrent(window_, nullptr);  // SDL now knows nothing is current
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
}

void Window::show() { SDL_ShowWindow(window_); }

void Window::hide() { SDL_HideWindow(window_); }

void Window::beginFrame(std::uint32_t rgb) {
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    glViewport(0, 0, pw, ph);
    glClearColor(static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                 static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                 static_cast<float>(rgb & 0xff) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    nvgBeginFrame(vg_, width(), height(), pixelRatio());
}

void Window::endFrame() { nvgEndFrame(vg_); }

void Window::present() {
    using Clock = std::chrono::steady_clock;
    const std::uint64_t gains = platform_->focusGains();
    if (vsync_) {  // the context is current (beginFrame, ui::Root::frame)
        const int want = platform_->lastFocused() == id() && gains != stalled_ ? 1 : 0;
        if (want != interval_ && SDL_GL_SetSwapInterval(want)) interval_ = want;
    }
    const Clock::time_point start = Clock::now();
    SDL_GL_SwapWindow(window_);
    if (interval_ == 1 && Clock::now() - start > std::chrono::milliseconds(250)) stalled_ = gains;
}

Image Window::readPixels() const {
    Image image;
    SDL_GetWindowSizeInPixels(window_, &image.width, &image.height);
    const auto row = static_cast<std::size_t>(image.width) * 4;
    std::vector<std::uint8_t> bottomUp(row * static_cast<std::size_t>(image.height));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    GLint target = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &target);
    glReadBuffer(target == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    image.rgba.resize(bottomUp.size());
    for (int y = 0; y < image.height; ++y)
        std::memcpy(image.rgba.data() + static_cast<std::size_t>(y) * row,
                    bottomUp.data() + static_cast<std::size_t>(image.height - 1 - y) * row, row);
    return image;
}

Result<Fonts> loadFonts(NVGcontext* vg, const std::filesystem::path& dir) {
    Fonts f;
    auto load = [&](const char* name, const char* file) -> Result<int> {
        const auto path = dir / file;
        auto bytes = readFile(path, 32 * 1024 * 1024);
        if (!bytes) return std::move(bytes).error();
        auto* data = static_cast<unsigned char*>(std::malloc(bytes->size()));
        if (!data) return Error("gui.font", "cannot allocate font data");
        std::memcpy(data, bytes->data(), bytes->size());
        const int id = nvgCreateFontMem(vg, name, data, static_cast<int>(bytes->size()), 1);
        if (id < 0)
            return Error("gui.font", "cannot load font " + path.string(), {},
                         "the bundled fonts live in assets/fonts/");
        return id;
    };
    for (auto [slot, name, file] : {std::tuple{&f.ui, "ui", "Inter-Regular.ttf"},
                                    std::tuple{&f.uiBold, "ui-bold", "Inter-SemiBold.ttf"},
                                    std::tuple{&f.mono, "mono", "PlexMono-Regular.ttf"},
                                    std::tuple{&f.monoBold, "mono-bold", "PlexMono-Medium.ttf"}}) {
        auto id = load(name, file);
        if (!id) return std::move(id).error();
        *slot = *id;
    }
    return f;
}

}  // namespace supple::gui
