// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A DAW window: SDL3 (X11 driver, or the offscreen driver for tests and
// benchmarks), one OpenGL 3.3 core context, NanoVG GL3 for chrome (ADR-0005).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "supple/base/native_window.h"
#include "supple/base/result.h"

struct NVGcontext;
struct SDL_Window;
union SDL_Event;

namespace supple::gui {

// Video subsystem lifetime. One per process, before any Window.
class Platform {
  public:
    enum class Driver { Native, X11 = Native, Offscreen };
    static Result<std::unique_ptr<Platform>> start(Driver driver);
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Driver driver() const { return driver_; }
    // An SDL event type that other threads may push to wake the event loop.
    std::uint32_t wakeEvent() const { return wakeEvent_; }
    // Thread-safe: interrupts SDL_WaitEvent* on the UI thread.
    void wake() const;
    // The last of this app's windows to gain keyboard focus (0 before any),
    // and a count of focus gains, both kept by an SDL event watch.
    std::uint32_t lastFocused() const { return lastFocused_.load(std::memory_order_relaxed); }
    std::uint64_t focusGains() const { return focusGains_.load(std::memory_order_relaxed); }

  private:
    explicit Platform(Driver d) : driver_(d) {}
    static bool watch(void* platform, SDL_Event* e);
    Driver driver_;
    std::uint32_t wakeEvent_ = 0;
    std::atomic<std::uint32_t> lastFocused_{0};
    std::atomic<std::uint64_t> focusGains_{0};
};

struct WindowOptions {
    std::string title;
    int width = 1280, height = 800;  // design units
    bool hidden = false;
    // false: a plain window without GL or NanoVG (a parent for plugin editors).
    bool gl = true;
    bool resizable = true;
    // A utility window owned by `parent` (plugin editors): window managers
    // float it above its owner instead of tiling it (Hyprland, i3, sway).
    bool utility = false;
    const class Window* parent = nullptr;
};

// RGBA8, rows top to bottom.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Fonts {
    int ui = -1, uiBold = -1, mono = -1, monoBold = -1;
};

class Window {
  public:
    static Result<std::unique_ptr<Window>> open(const Platform& platform,
                                                const WindowOptions& options);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    NVGcontext* vg() const { return vg_; }
    SDL_Window* sdl() const { return window_; }
    std::uint32_t id() const;
    // Size in design units and the design-unit → pixel factor.
    float width() const;
    float height() const;
    float pixelRatio() const;
    float inputScale() const;   // SDL window coordinates per design unit
    float refreshRate() const;  // of the window's display in Hz; 0 when unknown
    // Borrowed native editor parent; unavailable for Linux offscreen windows.
    NativeWindow nativeParent() const;
    // Compatibility accessor for existing X11 callers.
    unsigned long x11Window() const;

    void makeCurrent() const;
    // makeCurrent that always reaches GL: plugin editors (Pugl, …) make their
    // own contexts current on this thread behind SDL's back, and SDL skips a
    // bind it believes is already in place.
    void forceCurrent() const;
    void show();
    // Move focus between this app's visible windows. Background apps and
    // already-key windows are not raised; hidden/offscreen windows stay hidden.
    void activate() const;
    void hide();
    void setSize(int width, int height);  // design units
    // The position on the desktop (screen coordinates of the client area).
    void setPosition(int x, int y);
    std::pair<int, int> position() const;
    void setResizable(bool resizable);
    void setTitle(const std::string& title);
    std::string renderer() const;  // GL_RENDERER and GL_VERSION, for reports
    // Makes the context current, clears to `rgb` (0xRRGGBB) and starts a
    // NanoVG frame in design units.
    void beginFrame(std::uint32_t rgb);
    void endFrame();  // flushes NanoVG's draw calls
    // Swaps buffers. Only the window last focused waits for vsync: a window
    // the desktop is not drawing (behind a maximized one, on another
    // workspace) may get no vblank, and its vsynced swap then waits for the
    // compositor's fallback, about a second under XWayland, holding up
    // every window on the thread. Other windows swap at once and are paced by
    // ui::Root's frame interval. A vsynced swap that still takes over 250 ms
    // turns vsync off for that window until it gains focus again.
    void present();
    bool vsynced() const { return interval_ == 1; }
    // The currently bound read framebuffer (or the window back buffer), RGBA8.
    Image readPixels() const;

  private:
    Window() = default;
    const Platform* platform_ = nullptr;
    SDL_Window* window_ = nullptr;
    bool vsync_ = false;             // the swap interval can be set
    int interval_ = 0;               // as set
    std::uint64_t stalled_ = ~0ull;  // Platform::focusGains() at a stalled swap
    void* context_ = nullptr;        // SDL_GLContext
    NVGcontext* vg_ = nullptr;
};

// True when a window at (x, y) of this size would show at least a corner
// (48 x 48) on one of the current displays: a remembered position from a
// monitor that is gone is not used.
bool onDisplay(int x, int y, int width, int height);

// Loads the bundled fonts (Inter, IBM Plex Mono) from `dir` into `vg`.
Result<Fonts> loadFonts(NVGcontext* vg, const std::filesystem::path& dir);

}  // namespace supple::gui
