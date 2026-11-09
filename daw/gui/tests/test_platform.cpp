// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <nanovg.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

#include "supple/gui/png.h"
#include "supple/gui/process.h"
#include "supple/gui/window.h"

using namespace supple;

TEST_CASE("process startup timing is available on the native platform") {
    const double elapsed = gui::millisecondsSinceProcessStart();
    CHECK(std::isfinite(elapsed));
    CHECK(elapsed >= 0);  // API availability, not a wall-clock performance threshold
}

TEST_CASE("the PNG writer produces files a decoder reads back exactly") {
    gui::Image image;
    image.width = 300;  // > one 65535-byte stored block per image
    image.height = 120;
    image.rgba.resize(static_cast<std::size_t>(image.width * image.height * 4));
    for (std::size_t i = 0; i < image.rgba.size(); ++i)
        image.rgba[i] = static_cast<std::uint8_t>((i * 31 + i / 7) & 0xff);
    const auto png = gui::encodePng(image);
    int w = 0, h = 0, channels = 0;
    unsigned char* decoded =
        stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);
    REQUIRE(decoded != nullptr);
    CHECK(w == image.width);
    CHECK(h == image.height);
    CHECK(std::equal(image.rgba.begin(), image.rgba.end(), decoded));
    stbi_image_free(decoded);
}

TEST_CASE("an offscreen window draws with GL and NanoVG and reads back its pixels") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
    REQUIRE_MESSAGE(platform, (platform ? "" : platform.error().message));
#endif
    if (!platform) {
        MESSAGE("skipped: no offscreen GL here: " << platform.error().message);
        return;
    }
    gui::WindowOptions options{"test", 64, 48, true};
    options.resizable = false;
    options.utility = true;
    auto window = gui::Window::open(**platform, options);
    REQUIRE_MESSAGE(window, (window ? "" : window.error().message));
    gui::Window& w = **window;
    CHECK((SDL_GetWindowFlags(w.sdl()) & SDL_WINDOW_RESIZABLE) == 0);
    CHECK((SDL_GetWindowFlags(w.sdl()) & SDL_WINDOW_UTILITY) != 0);
#if defined(__linux__)
    CHECK(w.x11Window() == 0);  // offscreen: nothing to parent plugin editors to
#endif
    auto fonts = gui::loadFonts(w.vg(), SUPPLE_GUI_FONTS_DIR);
    REQUIRE_MESSAGE(fonts, (fonts ? "" : fonts.error().message));
    w.beginFrame(0x102030);
    nvgBeginPath(w.vg());
    nvgRect(w.vg(), 0, 0, 16, 16);  // top-left corner in design units
    nvgFillColor(w.vg(), nvgRGB(250, 10, 20));
    nvgFill(w.vg());
    w.endFrame();
    const gui::Image image = w.readPixels();
    w.present();
    REQUIRE(gui::writePng(SUPPLE_GUI_SMOKE_PNG, image));
    CHECK(image.width == static_cast<int>(w.width() * w.pixelRatio()));
    CHECK(image.height == static_cast<int>(w.height() * w.pixelRatio()));
    REQUIRE(image.width >= 64);
    auto at = [&](int x, int y) {
        const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(x)) *
                       4;
        return std::array<int, 3>{image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]};
    };
    CHECK(at(4, 4) == std::array<int, 3>{250, 10, 20});  // rows are top to bottom
    CHECK(at(image.width - 4, image.height - 4) == std::array<int, 3>{0x10, 0x20, 0x30});
}

TEST_CASE("only the window last focused waits for vsync when it presents") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    if (!platform) return;
    gui::WindowOptions options{"a", 64, 48, true};
    auto a = gui::Window::open(**platform, options);
    options.title = "b";
    auto b = gui::Window::open(**platform, options);
    REQUIRE(a);
    REQUIRE(b);
    auto present = [](gui::Window& w) {
        w.beginFrame(0);
        w.endFrame();
        w.present();
    };
    auto focus = [](const gui::Window& w) {
        SDL_Event e{};
        e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        e.window.windowID = w.id();
        REQUIRE(SDL_PushEvent(&e));  // the platform's event watch sees it
    };
    present(**a);
    CHECK_FALSE((*a)->vsynced());  // nothing focused yet
    focus(**a);
    CHECK((*platform)->lastFocused() == (*a)->id());
    present(**a);
    present(**b);
    if (!(*a)->vsynced()) {
        MESSAGE("skipped: no swap interval here");
        return;
    }
    CHECK_FALSE((*b)->vsynced());
    focus(**b);
    present(**a);
    present(**b);
    CHECK_FALSE((*a)->vsynced());
    CHECK((*b)->vsynced());
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}

TEST_CASE("the platform wake event can be pushed from another thread") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    if (!platform) return;
    std::thread([&] { (*platform)->wake(); }).join();
    SDL_Event e;
    bool woke = false;
    while (SDL_WaitEventTimeout(&e, 100))
        if (e.type == (*platform)->wakeEvent()) {
            woke = true;
            break;
        }
    CHECK(woke);
}

TEST_CASE("PNG encoding refuses invalid dimensions and truncated RGBA data") {
    CHECK(gui::encodePng({0, 1, {}}).empty());
    CHECK(gui::encodePng({-1, 1, {}}).empty());
    CHECK(gui::encodePng({1, 1, {0, 0, 0}}).empty());
    CHECK(gui::encodePng({1, 1, {0, 0, 0, 0, 0}}).empty());
    CHECK_FALSE(gui::writePng("/tmp/supple-invalid-image.png", {1, 1, {}}));
}
