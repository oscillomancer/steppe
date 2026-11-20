// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The mixer's own gestures (gui.md §4.4): the fx button, a resizable chain
// section, plug-in reorder and copy by drag, strip reorder by drag; the
// reorder-index helper the TCP shares.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include <stb_image.h>
#include "product_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "app_actions.h"
#include "audio_device.h"
#include "meter_feed.h"
#include "mixer.h"
#include "preferences.h"
#include "selection_edit.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/engine/realtime.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/control_names.h"
#include "supple/views/mouse_map.h"

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Test, "mixer-test", {}};

bool near(float a, float b) { return std::fabs(a - b) < 0.6f; }

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState ui;
    std::unique_ptr<ui::Root> root;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<gui::MeterFeed> feed;
    std::unique_ptr<gui::Mixer> mixer;
    std::vector<std::string> editors, errors;  // ui.fx.editor calls; reported errors
    std::vector<std::string> enables;          // ui.fx.enable calls
    bool ok = false;

    Fixture(std::shared_ptr<engine::RuntimeProtectionOps> operations = {}) {
        dir = std::filesystem::temp_directory_path() /
              ("supple-mixer-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        testConfigHome(dir / "config");
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"mixer-test", 1400, 900, true});
#if !defined(__linux__)
        REQUIRE_MESSAGE(w, (w ? "" : w.error().message));
#endif
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
#if !defined(__linux__)
        REQUIRE_MESSAGE(f, (f ? "" : f.error().message));
#endif
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        options.runtimeProtectionOps = std::move(operations);
        options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
        actions->addUi("ui.fx.editor", "FX: open or close editor", "fx",
                       [this](const json::Value& v) -> Status {
                           editors.push_back(v.asString());
                           return {};
                       });
        actions->addUi("ui.fx.enable", "FX: enable a disabled plug-in", "fx",
                       [this](const json::Value& v) -> Status {
                           enables.push_back(v.asString());
                           return {};
                       });
        actions->onError = [this](const std::string& e) { errors.push_back(e); };
        feed = std::make_unique<gui::MeterFeed>([] { return std::vector<gui::StripReading>{}; });
        mixer = std::make_unique<gui::Mixer>(*root, *actions, *dispatcher, *app, *feed);
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->emplace<ui::Node>("arrange").layout.height = ui::SizeRule::fraction();
        frame->add(mixer->build());
        root->setContent(std::move(frame));
        mixer->setVisible(true);
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        mixer.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }

    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
    std::string addTrack(const char* type, const char* name) {
        return exec("track.add",
                    std::string(R"({"type":")") + type + R"(","name":")" + name + R"("})")["track"]
            .asString();
    }
    std::string addFx(const std::string& track, const char* plugin) {
        return exec("fx.add", R"({"track":")" + track + R"(","plugin":")" + plugin + R"("})")["fx"]
            .asString();
    }
    // The time of every pass the fixture makes. A test moves it on by hand:
    // no pass reads the real clock, so nothing here depends on how long the
    // machine has been up or how fast it runs.
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
    void refresh() {
        mixer->sync(now);
        root->frame(false);
    }
    const model::Track& track(const std::string& id) {
        return *app->head().tracks.find(*model::Id::parse(id));
    }
    std::vector<std::string> chain(const std::string& id) {
        std::vector<std::string> out;
        for (model::Id fx : track(id).chain) out.push_back(fx.str());
        return out;
    }
    std::vector<std::string> children(const std::string& id) {
        std::vector<std::string> out;
        for (model::Id c : track(id).children) out.push_back(c.str());
        return out;
    }
    std::size_t steps() { return exec("history.list", "{}")["versions"].size(); }
    const gui::Mixer::Strip& strip(const std::string& id) {
        for (const auto& s : mixer->strips())
            if (s.track == id) return s;
        FAIL("no strip " << id);
        return mixer->strips().front();
    }
    ui::Node* node(const std::string& id) {
        ui::Node* n = root->find(id);
        REQUIRE_MESSAGE(n, id);
        return n;
    }
    ui::Point centre(const ui::Node* n) {
        const ui::Rect b = n->bounds();
        return {b.x + b.w / 2, b.y + b.h / 2};
    }
    void pointer(ui::PointerEvent::Type type, ui::Point p, int clicks = 1, unsigned mods = 0,
                 int button = 1) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = type == ui::PointerEvent::Type::Move ? 0 : button;
        e.clicks = clicks;
        e.mods = mods;
        root->pointer(e);
        root->frame(false);
    }
    // Down, a few moves, up: a drag the way SDL would deliver it.
    void drag(ui::Point from, ui::Point to, bool release = true, unsigned mods = 0) {
        pointer(ui::PointerEvent::Type::Down, from, 1, mods);
        for (int i = 1; i <= 4; ++i)
            pointer(ui::PointerEvent::Type::Move,
                    {from.x + (to.x - from.x) * static_cast<float>(i) / 4,
                     from.y + (to.y - from.y) * static_cast<float>(i) / 4},
                    1, mods);
        if (release) pointer(ui::PointerEvent::Type::Up, to, 1, mods);
    }
    // A point on a strip no control uses: its name.
    ui::Point empty(const std::string& track) { return centre(node("mixer.name." + track)); }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        root->damageAll();
        root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, root->readBack());
    }
    // The window against tests/golden/<name>: written by SUPPLE_UPDATE_GOLDEN=1
    // or when missing; off Linux the picture is only checked to hold something.
    void golden(const char* name) {
        root->damageAll();
        root->frame(false);
        const gui::Image image = root->readBack();
#if !defined(__linux__)
        REQUIRE(image.width > 0);
        REQUIRE(image.height > 0);
        REQUIRE(image.rgba.size() == static_cast<std::size_t>(image.width * image.height * 4));
        bool varied = false;
        for (std::size_t i = 4; i < image.rgba.size(); i += 4)
            if (image.rgba[i] != image.rgba[0] || image.rgba[i + 1] != image.rgba[1] ||
                image.rgba[i + 2] != image.rgba[2]) {
                varied = true;
                break;
            }
        CHECK(varied);
        return;
#endif

        const auto path = std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / name;
        if (std::getenv("SUPPLE_UPDATE_GOLDEN") || !std::filesystem::exists(path)) {
            REQUIRE(gui::writePng(path, image));
            MESSAGE("golden written: " << path.string());
            return;
        }
        int w = 0, h = 0, n = 0;
        unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
        REQUIRE(data);
        REQUIRE(w == image.width);
        REQUIRE(h == image.height);
        std::size_t off = 0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            int d = 0;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(int(image.rgba[i + c]) - int(data[i + c])));
            off += d > 48;
        }
        stbi_image_free(data);
        CHECK(off < image.rgba.size() / 4 / 200);
    }
};

}  // namespace

TEST_CASE("mixer: a master looks like the others; \"fx\" and \"+\" head the plug-in list") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto master = std::string("tr_0");
    f.refresh();
    const auto& sa = f.strip(a);
    const auto& sm = f.strip(master);
    // The same colour just inside the left edge below the stripe, on both.
    auto pixel = [&](const ui::Rect& r) {
        const gui::Image image = f.root->readBack();
        const float ratio = f.window->pixelRatio();
        const int x = static_cast<int>((r.x + 1) * ratio);
        const int y = static_cast<int>((r.y + 120) * ratio);
        const std::size_t at =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
             static_cast<std::size_t>(x)) *
            4;
        return std::vector<std::uint8_t>(image.rgba.begin() + static_cast<std::ptrdiff_t>(at),
                                         image.rgba.begin() + static_cast<std::ptrdiff_t>(at) + 4);
    };
    CHECK(pixel(sm.node->bounds()) == pixel(sa.node->bounds()));
    // The name has the strip's width; under it, small, "fx" at the left and
    // "+" at the right head the plug-in list.
    const ui::Rect strip = sa.node->bounds(), fx = sa.fx->bounds(), plus = sa.addFx->bounds();
    CHECK(sa.fx->valueText() == "fx");
    CHECK(sa.addFx->valueText() == "+");
    CHECK(fx.w <= 34);
    CHECK(fx.h <= 16);
    CHECK(plus.right() >= strip.right() - 8);
    CHECK(fx.right() <= plus.x);
    CHECK(fx.x <= strip.x + 8);
    CHECK(fx.y >= sa.name->bounds().bottom() - 1);
    CHECK(fx.bottom() <= sa.chain->bounds().y);
    CHECK(sa.name->bounds().right() >= strip.right() - 8);
    // No plug-in count beside "fx", however long the chain.
    f.addFx(a, "Hickory");
    f.refresh();
    CHECK(f.strip(a).fx->valueText() == "fx");
}

TEST_CASE(
    "mixer: the chain section lists the whole chain, scrolled when it is longer than it fits") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    for (int i = 0; i < 12; ++i) f.addFx(a, "Hickory");
    f.refresh();
    ui::Node* list = f.strip(a).chain;
    // The default height fits five plug-ins whole.
    int whole = 0;
    for (const auto& c : list->children())
        if (c->id().rfind("mixer.fx.fx_", 0) == 0 &&
            c->bounds().bottom() <= list->bounds().bottom() + 0.5f)
            ++whole;
    CHECK(whole >= 5);
    CHECK(list->children().size() >= 13);  // twelve slots and the bar
    // The wheel scrolls the chain, not the strips.
    const float y0 = f.node("mixer.fx.fx_1")->bounds().y;
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = f.centre(f.node("mixer.fx.fx_2"));
    wheel.wheelY = -2;
    f.root->pointer(wheel);
    f.root->frame(false);
    CHECK(f.node("mixer.fx.fx_1")->bounds().y < y0 - 10);
    // Slots that are scrolled out are not hit.
    CHECK(f.root->content()->hitTest({list->bounds().x + 5, list->bounds().y - 6}) !=
          f.node("mixer.fx.fx_1"));
}

TEST_CASE("mixer: a strip is one grid: routing, stereo, pan, the state row, then the fader") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto bus = f.addTrack("buss", "Bus");
    const std::string master = "tr_0";
    f.exec("route.add", R"({"from":")" + b + R"(","to":")" + bus + R"("})");
    f.refresh();
    auto box = [&](const std::string& id) { return f.node(id)->bounds(); };
    // The rows under the plug-ins, top to bottom, a gap apart: routing (in
    // and out), stereo (polarity, mono), the pan, the state row, the fader.
    CHECK_FALSE(f.node("mixer.sends." + a)->visible);
    const std::vector<ui::Rect> rows = {f.strip(a).chain->bounds(), box("mixer.io." + a),
                                        box("mixer.util." + a),     f.strip(a).pan->bounds(),
                                        box("mixer.state." + a),    box("mixer.faderrow." + a)};
    for (std::size_t i = 1; i < rows.size(); ++i)
        CHECK_MESSAGE(near(rows[i].y - rows[i - 1].bottom(), 4), i);
    // Every button is one height, and each row spans the strip's columns.
    const float left = rows[1].x, right = rows[1].right();
    for (const char* id : {"mixer.input.", "mixer.out.", "mixer.polarity.", "mixer.mono.",
                           "mixer.mute.", "mixer.solo.", "mixer.arm.", "mixer.monitor."})
        CHECK_MESSAGE(near(box(id + a).h, 16), id);
    CHECK(near(box("mixer.input." + a).x, left));
    CHECK(near(box("mixer.out." + a).right(), right));
    // Stereo: two halves.
    CHECK(near(box("mixer.polarity." + a).x, left));
    CHECK(near(box("mixer.mono." + a).right(), right));
    CHECK(near(box("mixer.polarity." + a).w, box("mixer.mono." + a).w));
    // State: four equal cells in one order, arm, monitor, mute, solo.
    const std::vector<ui::Rect> cells = {box("mixer.arm." + a), box("mixer.monitor." + a),
                                         box("mixer.mute." + a), box("mixer.solo." + a)};
    CHECK(near(cells.front().x, left));
    CHECK(near(cells.back().right(), right));
    for (std::size_t i = 1; i < cells.size(); ++i) {
        CHECK_MESSAGE(near(cells[i].w, cells[0].w), i);
        CHECK_MESSAGE(near(cells[i].x - cells[i - 1].right(), 3), i);
        CHECK_MESSAGE(near(cells[i].y, cells[0].y), i);
    }
    // Every cell takes a click anywhere inside it: its corner arms the track.
    {
        const bool was = f.strip(a).arm->on();
        f.pointer(ui::PointerEvent::Type::Down, {cells[0].x + 1, cells[0].y + 1});
        f.pointer(ui::PointerEvent::Type::Up, {cells[0].x + 1, cells[0].y + 1});
        f.refresh();
        CHECK(f.strip(a).arm->on() != was);
        f.pointer(ui::PointerEvent::Type::Down, {cells[0].x + 1, cells[0].y + 1});
        f.pointer(ui::PointerEvent::Type::Up, {cells[0].x + 1, cells[0].y + 1});
        f.refresh();
    }
    // The four cells are two halves split again: the grid is one.
    CHECK(near(cells[1].right(), box("mixer.polarity." + a).right()));
    // A cell is a control's place. A buss has no arm, monitor or input and a
    // master no monitor or input: their cells stay empty, and mute and solo
    // are where they are on a channel.
    CHECK_FALSE(f.root->find("mixer.arm." + bus));
    CHECK_FALSE(f.root->find("mixer.monitor." + bus));
    CHECK_FALSE(f.root->find("mixer.input." + bus));
    CHECK_FALSE(f.root->find("mixer.monitor." + master));
    CHECK(near(box("mixer.out." + bus).w, right - left));
    for (const std::string& other : {bus, master}) {
        const float dx = f.strip(other).node->bounds().x - f.strip(a).node->bounds().x;
        CHECK(near(box("mixer.mute." + other).x - dx, cells[2].x));
        CHECK(near(box("mixer.solo." + other).x - dx, cells[3].x));
        CHECK(near(box("mixer.solo." + other).w, cells[3].w));
    }
    CHECK(near(box("mixer.arm." + master).x -
                   (f.strip(master).node->bounds().x - f.strip(a).node->bounds().x),
               cells[0].x));
    // Every type has the same rows, so every chain is as tall and every
    // fader lines up; a send takes one row out of its own strip's chain.
    CHECK(near(f.strip(bus).chain->bounds().h, f.strip(a).chain->bounds().h));
    CHECK(near(f.strip(master).chain->bounds().h, f.strip(a).chain->bounds().h));
    CHECK(near(f.strip(bus).fader->bounds().y, f.strip(a).fader->bounds().y));
    CHECK(f.node("mixer.sends." + b)->visible);
    CHECK(near(f.strip(a).chain->bounds().h - f.strip(b).chain->bounds().h, 14 + 4));
    // The type is a mark before the name, not a colour: none on a channel.
    CHECK_FALSE(f.root->find("mixer.type." + a));
    CHECK(f.node("mixer.type." + bus)->valueText() == "buss");
    CHECK(f.node("mixer.type." + master)->valueText() == "master");
    CHECK(f.node("mixer.type." + bus)->bounds().right() <= f.strip(bus).name->bounds().x);
    // The colour bar across the top is the track's own, the strip's width.
    CHECK(near(box("mixer.color." + a).y, f.strip(a).node->bounds().y));
    CHECK(box("mixer.color." + a).w >= f.strip(a).node->bounds().w - 1.5f);
}

// gui.md §2, Names: a strip's controls are called what the track's row calls them.
TEST_CASE("mixer: a strip's controls have the names the track panel gives its row's") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    auto over = [&](const std::string& id) {
        f.pointer(ui::PointerEvent::Type::Move, f.centre(f.node(id)));
        return f.root->hint();
    };
    namespace names = views::names;
    CHECK(over("mixer.mute." + a) == names::kMute);
    CHECK(over("mixer.solo." + a) == names::kSolo);
    CHECK(over("mixer.polarity." + a) == names::kPolarity);
    CHECK(over("mixer.mono." + a) == names::kMono);
    CHECK(over("mixer.pan." + a) == names::kPan);
    CHECK(over("mixer.fader." + a) == names::kVolume);
    CHECK(over("mixer.input." + a) == names::kInput);
    CHECK(over("mixer.out." + a) == names::kRouting);
    CHECK(over("mixer.fxadd." + a) == names::kChain);
    CHECK(over("mixer.addfx." + a) == names::kAddFx);
    CHECK(over("mixer.arm." + a).name == "Arm for recording");
    CHECK(over("mixer.monitor." + a).name.starts_with("Monitor the input: "));
    // A master's "out" picks a hardware output; it does not open the routing.
    CHECK(over("mixer.out.tr_0") == names::kHardwareOutput);
    // A strip's name, and the mixer's empty space, are called nothing.
    f.pointer(ui::PointerEvent::Type::Move, f.empty(a));
    CHECK(f.root->hint().empty());
}

TEST_CASE("mixer: a double-click on the strips' empty space adds a channel") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    const std::size_t before = f.app->head().tracks.size();
    auto doubleClick = [&](ui::Point at) {
        f.pointer(ui::PointerEvent::Type::Down, at, 1);
        f.pointer(ui::PointerEvent::Type::Up, at);
        f.pointer(ui::PointerEvent::Type::Down, at, 2);
        f.pointer(ui::PointerEvent::Type::Up, at);
    };
    // On a strip, even its empty part: nothing.
    doubleClick(f.empty(a));
    CHECK(f.app->head().tracks.size() == before);
    // Between the last strip and the pinned masters: a channel.
    const ui::Rect strip = f.strip(a).node->bounds();
    doubleClick({strip.right() + 200, strip.y + strip.h / 2});
    CHECK(f.app->head().tracks.size() == before + 1);
    f.refresh();
    REQUIRE(f.mixer->strips().size() >= 3);
    const auto& added = f.track(f.mixer->strips()[1].track);
    CHECK(added.type == model::StripType::Channel);
    // A right-click there: channel, buss or master (the TCP's "+ track" menu).
    const ui::Point at{strip.right() + 200, strip.y + strip.h / 2};
    ui::PointerEvent right;
    right.type = ui::PointerEvent::Type::Down;
    right.pos = at;
    right.button = 3;
    f.root->pointer(right);
    right.type = ui::PointerEvent::Type::Up;
    f.root->pointer(right);
    f.root->frame(false);
    ui::Node* menu = f.node("mixer.addmenu");
    const ui::Point master{menu->bounds().x + 40, menu->bounds().y + 2 * 26 + 13};
    f.pointer(ui::PointerEvent::Type::Move, master);
    f.pointer(ui::PointerEvent::Type::Down, master);
    f.pointer(ui::PointerEvent::Type::Up, master);
    CHECK(f.app->head().tracks.size() == before + 2);
    CHECK(f.app->head().project->masters.size() == 2);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: strips select like the TCP's rows, and the menu deletes the selection") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto c = f.addTrack("channel", "C");
    f.refresh();
    auto press = [&](const std::string& track, int button, unsigned mods) {
        ui::PointerEvent e;
        e.type = ui::PointerEvent::Type::Down;
        e.pos = f.empty(track);
        e.button = button;
        e.mods = mods;
        f.root->pointer(e);
        e.type = ui::PointerEvent::Type::Up;
        f.root->pointer(e);
        f.refresh();
    };
    using V = std::vector<std::string>;
    press(a, 1, 0);
    CHECK(f.ui.selectedTracks == V{a});
    press(c, 1, ui::kShift);  // a range, in strip order
    CHECK(f.ui.selectedTracks == V{a, b, c});
    press(b, 1, ui::kCtrl);  // toggled out
    CHECK(f.ui.selectedTracks == V{a, c});
    // Selected strips are highlighted.
    auto pixel = [&](const std::string& track) {
        f.root->damageAll();
        f.root->frame(false);
        const gui::Image image = f.root->readBack();
        const ui::Rect r = f.strip(track).node->bounds();
        const float ratio = static_cast<float>(image.width) / 1400.0f;
        const auto x = static_cast<std::size_t>((r.x + 2) * ratio);
        const auto y = static_cast<std::size_t>((r.y + r.h / 2) * ratio);
        const std::size_t at = (y * static_cast<std::size_t>(image.width) + x) * 4;
        return std::array<int, 3>{image.rgba[at], image.rgba[at + 1], image.rgba[at + 2]};
    };
    CHECK(pixel(a) == pixel(c));
    CHECK(pixel(a) != pixel(b));
    // A right-click on a selected strip keeps the selection; Delete removes it all.
    press(c, 3, 0);
    CHECK(f.ui.selectedTracks == V{a, c});
    ui::Node* menu = f.root->find("track.menu." + c);
    REQUIRE(menu);
    const auto before = f.steps();
    const ui::Point del{menu->bounds().x + 40,
                        menu->bounds().y +
                            static_cast<float>(ui::menuItems(*menu).size() - 1) * 26 +
                            13};  // Delete
    f.pointer(ui::PointerEvent::Type::Move, del);
    f.pointer(ui::PointerEvent::Type::Down, del);
    f.pointer(ui::PointerEvent::Type::Up, del);
    f.refresh();
    CHECK_FALSE(f.app->head().tracks.find(*model::Id::parse(a)));
    CHECK_FALSE(f.app->head().tracks.find(*model::Id::parse(c)));
    CHECK(f.app->head().tracks.find(*model::Id::parse(b)));
    CHECK(f.steps() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: Alt+click on a plug-in removes it, in one undo step") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Hickory");
    const auto p2 = f.addFx(a, "Hickory");
    f.refresh();
    const auto before = f.steps();
    ui::PointerEvent down;
    down.type = ui::PointerEvent::Type::Down;
    down.pos = f.centre(f.node("mixer.fx." + p1));
    down.button = 1;
    down.mods = ui::kAlt;
    f.root->pointer(down);
    ui::PointerEvent up = down;
    up.type = ui::PointerEvent::Type::Up;
    f.root->pointer(up);
    f.root->frame(false);
    CHECK(f.chain(a) == std::vector<std::string>{p2});
    CHECK(f.steps() == before + 1);
    f.refresh();
    CHECK(f.root->find("mixer.fx." + p1) == nullptr);
    f.exec("history.undo", "{}");
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2});
}

TEST_CASE("mixer: dragging the top edge resizes only the chain section; double-click resets") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    ui::Node* panel = f.node("mixer");
    const float standard = panel->bounds().h;
    CHECK(near(standard, gui::Mixer::defaultHeight()));
    // About five plug-ins: the chain section is five rows tall.
    CHECK(near(f.strip(a).chain->bounds().h, 5 * 18 + 4 * 2));
    const ui::Rect fader = f.strip(a).fader->bounds();
    const float meter = f.strip(a).meter->bounds().h;
    const float chain = f.strip(a).chain->bounds().h;
    const float edge = f.node("mixer.grip")->bounds().y + 2;
    f.pointer(ui::PointerEvent::Type::Move, {700, edge});
    CHECK(f.root->cursor() == ui::Cursor::ResizeVertical);  // the panel's top edge is dragged
    f.drag({700, edge}, {700, edge - 120});
    CHECK(near(panel->bounds().h, standard + 120));
    CHECK(near(f.mixer->height(), standard + 120));
    CHECK(near(f.strip(a).chain->bounds().h, chain + 120));
    CHECK(near(f.strip(a).fader->bounds().h, fader.h));  // the rest stays
    CHECK(near(f.strip(a).meter->bounds().h, meter));
    // Down is shorter, to a floor of two plug-ins; up stops short of the arrange view.
    f.drag({700, f.node("mixer.grip")->bounds().y + 2}, {700, 899});
    CHECK(near(panel->bounds().h, gui::Mixer::defaultHeight() - 3 * 20));
    const float top = f.node("mixer.grip")->bounds().y + 2;
    f.drag({700, top}, {700, -500});
    CHECK(panel->bounds().h <= 900 - 120);
    CHECK(panel->bounds().h > standard + 120);
    // Double-click on the edge: the default again.
    const ui::Point grip = f.centre(f.node("mixer.grip"));
    f.pointer(ui::PointerEvent::Type::Down, grip, 1);
    f.pointer(ui::PointerEvent::Type::Up, grip);
    f.pointer(ui::PointerEvent::Type::Down, grip, 2);
    f.pointer(ui::PointerEvent::Type::Up, grip);
    CHECK(near(panel->bounds().h, standard));
    CHECK(near(f.mixer->height(), standard));
    // The height is state the app saves and restores, within the window.
    f.mixer->setHeight(500);
    f.refresh();
    CHECK(near(panel->bounds().h, 500));
    f.mixer->setHeight(5000);
    f.refresh();
    CHECK(panel->bounds().h <= 900 - 120);
    f.mixer->setHeight(470);
    const json::Value saved = f.mixer->saveLayout();
    CHECK(near(static_cast<float>(saved.find("height")->asDouble()), 470));
    f.mixer->setHeight(500);
    f.mixer->loadLayout(saved);
    CHECK(near(f.mixer->height(), 470));
    f.mixer->setHeight(10);
    CHECK(f.mixer->height() >= gui::Mixer::defaultHeight() - 4 * 20);
}

TEST_CASE("mixer: a click on a plug-in still opens it; a drag within the chain reorders it") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Hickory"), p2 = f.addFx(a, "Hickory"), p3 = f.addFx(a, "Hickory");
    f.refresh();
    const auto before = f.steps();
    // A click, or a nudge under the threshold, is not a drag.
    REQUIRE(f.root->click("mixer.fx." + p2));
    CHECK(f.editors == std::vector<std::string>{p2});
    const ui::Point c = f.centre(f.node("mixer.fx." + p1));
    f.pointer(ui::PointerEvent::Type::Down, c);
    f.pointer(ui::PointerEvent::Type::Move, {c.x + 2, c.y + 1});
    f.pointer(ui::PointerEvent::Type::Up, {c.x + 2, c.y + 1});
    CHECK(f.editors.size() == 2);
    CHECK(f.steps() == before);
    // The first plug-in dragged below the last: the chain's end, one step.
    const ui::Point from = f.centre(f.node("mixer.fx." + p1));
    const ui::Rect last = f.node("mixer.fx." + p3)->bounds();
    f.drag(from, {last.x + 20, last.bottom() + 1}, false);
    CHECK(f.mixer->drag().active);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Reorder);
    f.snapshot("mixer-drag-fx.png");
    f.pointer(ui::PointerEvent::Type::Up, {last.x + 20, last.bottom() + 1});
    CHECK_FALSE(f.mixer->drag().active);
    CHECK(f.editors.size() == 2);  // the release was not a click
    CHECK(f.chain(a) == std::vector<std::string>{p2, p3, p1});
    CHECK(f.steps() == before + 1);
    f.refresh();
    // Back to the front.
    const ui::Point again = f.centre(f.node("mixer.fx." + p1));
    f.drag(again, {again.x, f.node("mixer.fx." + p2)->bounds().y - 1});
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2, p3});
    // Dropped where it already is: nothing, and the pointer does not call
    // its own place refused.
    f.refresh();
    const auto steps = f.steps();
    const ui::Point same = f.centre(f.node("mixer.fx." + p2));
    f.drag(same, {same.x + 20, same.y}, false);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::None);
    CHECK(f.mixer->drag().home);
    CHECK(f.root->cursor() == ui::Cursor::Move);
    f.pointer(ui::PointerEvent::Type::Up, {same.x + 20, same.y});
    CHECK(f.steps() == steps);
    // Escape cancels the drag, and the release after it is no click.
    const ui::Point esc = f.centre(f.node("mixer.fx." + p1));
    f.drag(esc, {esc.x, esc.y + 50}, false);
    CHECK(f.mixer->drag().active);
    f.root->key(SDLK_ESCAPE);
    CHECK_FALSE(f.mixer->drag().active);
    f.pointer(ui::PointerEvent::Type::Up, {esc.x, esc.y + 50});
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2, p3});
    CHECK(f.steps() == steps);
    CHECK(f.editors.size() == 2);
    // Dropped somewhere that is nobody's strip: cancelled, and "not allowed"
    // on the way.
    f.drag(f.centre(f.node("mixer.fx." + p1)), {700, 20}, false);
    CHECK_FALSE(f.mixer->drag().home);
    CHECK(f.root->cursor() == ui::Cursor::NotAllowed);
    f.pointer(ui::PointerEvent::Type::Up, {700, 20});
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2, p3});
    CHECK(f.steps() == steps);
    CHECK(f.errors.empty());
}

TEST_CASE(
    "mixer: dragging a plug-in onto another strip copies it, settings and all, last in its chain") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto master = std::string("tr_0");
    const auto p1 = f.addFx(a, "Hickory");
    const auto b1 = f.addFx(b, "Hickory");
    f.exec("fx.set", R"({"fx":")" + p1 + R"(","label":"Mine","wet":40})");
    f.refresh();
    const auto before = f.steps();
    // Onto any part of the other strip: here its meter column.
    f.drag(f.centre(f.node("mixer.fx." + p1)), f.centre(f.node("mixer.faderrow." + b)), false);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Copy);
    CHECK(f.mixer->drag().target == b);
    f.snapshot("mixer-drag-copy.png");
    f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node("mixer.faderrow." + b)));
    REQUIRE(f.chain(b).size() == 2);
    CHECK(f.chain(b)[0] == b1);
    CHECK(f.chain(a) == std::vector<std::string>{p1});  // the original stays
    const auto& copy = *f.app->head().fx.find(*model::Id::parse(f.chain(b)[1]));
    CHECK(copy.label == "Mine");
    CHECK(copy.wet == 40);
    CHECK(f.steps() == before + 1);
    f.refresh();
    CHECK(f.chain(b).size() == 2);
    // Onto a master too; and one undo takes the copy away.
    f.drag(f.centre(f.node("mixer.fx." + p1)), f.empty(master));
    CHECK(f.chain(master).size() == 1);
    f.exec("history.undo", "{}");
    CHECK(f.chain(master).empty());
    CHECK(f.errors.empty());
    f.refresh();
    f.snapshot("mixer-chain.png");
}

TEST_CASE("mixer: Ctrl+dragging a plug-in onto another strip copies its whole chain") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto p1 = f.addFx(a, "Hickory");
    const auto p2 = f.addFx(a, "Hickory");
    const auto b1 = f.addFx(b, "Hickory");
    f.exec("fx.set", R"({"fx":")" + p2 + R"(","label":"Second"})");
    f.refresh();
    const auto before = f.steps();
    const ui::Point to = f.centre(f.node("mixer.faderrow." + b));
    f.drag(f.centre(f.node("mixer.fx." + p1)), to, false, ui::kCtrl);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Copy);
    CHECK(f.mixer->drag().chain);
    CHECK(f.mixer->drag().label == "chain (2)");
    f.pointer(ui::PointerEvent::Type::Up, to, 1, ui::kCtrl);
    REQUIRE(f.chain(b).size() == 3);  // its own first, then the chain in order
    CHECK(f.chain(b)[0] == b1);
    CHECK(f.app->head().fx.find(*model::Id::parse(f.chain(b)[2]))->label == "Second");
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2});
    CHECK(f.steps() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a plug-in's right-click menu renames that instance") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Hickory");
    f.refresh();
    const ui::Point at = f.centre(f.node("mixer.fx." + p1));
    f.pointer(ui::PointerEvent::Type::Down, at, 1, 0, 3);
    f.pointer(ui::PointerEvent::Type::Up, at, 1, 0, 3);
    CHECK_FALSE(f.root->find("track.menu." + a));  // its own menu, not the track's
    ui::Node* menu = f.node("mixer.fxmenu." + p1);
    const ui::Point rename{menu->bounds().x + 40, menu->bounds().y + 13};
    f.pointer(ui::PointerEvent::Type::Move, rename);
    f.pointer(ui::PointerEvent::Type::Down, rename);
    f.pointer(ui::PointerEvent::Type::Up, rename);
    auto* field = dynamic_cast<ui::TextField*>(f.node("mixer.fxrename." + p1));
    REQUIRE(field);
    CHECK(field->text() == "Hickory");
    field->setText("Lead trim");
    f.root->key(SDLK_RETURN);
    f.refresh();
    CHECK(f.app->head().fx.find(*model::Id::parse(p1))->label == "Lead trim");
    CHECK(f.node("mixer.fx." + p1)->valueText() == "Lead trim");
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a disabled plug-in is marked on its strip, with Enable first in its menu") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Hickory");
    f.refresh();
    auto* slot = dynamic_cast<ui::Button*>(f.node("mixer.fx." + p1));
    auto* fxButton = dynamic_cast<ui::Button*>(f.node("mixer.fxadd." + a));
    REQUIRE(slot);
    REQUIRE(fxButton);
    CHECK(slot->valueText().find("(disabled)") == std::string::npos);
    CHECK(fxButton->textColor().empty());
    {
        // Nothing in the project changes, and the strip follows (an override is not in the model).
        const auto version = f.app->head().version;
        f.exec("plugin.override", R"({"plugin":"Hickory","mode":"disable"})");
        CHECK(f.app->head().version == version);
    }
    f.refresh();
    slot = dynamic_cast<ui::Button*>(f.node("mixer.fx." + p1));
    fxButton = dynamic_cast<ui::Button*>(f.node("mixer.fxadd." + a));
    REQUIRE(slot);
    CHECK(slot->valueText().ends_with(" (disabled)"));
    CHECK(fxButton->textColor() == "meter-hot");
    f.snapshot("mixer-disabled.png");
    // A click on it opens no editor: it says why instead.
    {
        const std::size_t shown = f.editors.size();
        const ui::Point click = f.centre(f.node("mixer.fx." + p1));
        f.pointer(ui::PointerEvent::Type::Down, click);
        f.pointer(ui::PointerEvent::Type::Up, click);
        CHECK(f.editors.size() == shown);
        REQUIRE_FALSE(f.errors.empty());
        CHECK(f.errors.back().find("not loaded") != std::string::npos);
        f.errors.clear();
    }
    // Enable comes first in the slot's menu.
    const ui::Point at = f.centre(f.node("mixer.fx." + p1));
    f.pointer(ui::PointerEvent::Type::Down, at, 1, 0, 3);
    f.pointer(ui::PointerEvent::Type::Up, at, 1, 0, 3);
    ui::Node* menu = f.node("mixer.fxmenu." + p1);
    const ui::Point first{menu->bounds().x + 40, menu->bounds().y + 13};
    f.pointer(ui::PointerEvent::Type::Move, first);
    f.pointer(ui::PointerEvent::Type::Down, first);
    f.pointer(ui::PointerEvent::Type::Up, first);
    REQUIRE(f.enables.size() == 1);
    CHECK(f.enables[0] == p1);
    // Loaded again: the marks go, and the menu has its three rows.
    f.exec("plugin.override", R"({"plugin":"Hickory","mode":"normal"})");
    f.refresh();
    CHECK(dynamic_cast<ui::Button*>(f.node("mixer.fx." + p1))->valueText().find("(disabled)") ==
          std::string::npos);
    CHECK(dynamic_cast<ui::Button*>(f.node("mixer.fxadd." + a))->textColor().empty());
}

namespace {
// The pixel at the middle of a slot's left edge, inside its outline: the bar's place.
std::array<int, 3> barPixel(Fixture& f, const std::string& slot) {
    f.root->damageAll();
    f.root->frame(false);
    const gui::Image image = f.root->readBack();
    const ui::Rect b = f.node(slot)->bounds();
    const int x = static_cast<int>(b.x) + 2, y = static_cast<int>(b.y + b.h / 2);
    const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                            static_cast<std::size_t>(x)) *
                           4;
    return {image.rgba[at], image.rgba[at + 1], image.rgba[at + 2]};
}
bool is(const std::array<int, 3>& pixel, const ui::Color& c) {
    return std::abs(pixel[0] - static_cast<int>(std::lround(c.r * 255))) <= 2 &&
           std::abs(pixel[1] - static_cast<int>(std::lround(c.g * 255))) <= 2 &&
           std::abs(pixel[2] - static_cast<int>(std::lround(c.b * 255))) <= 2;
}
}  // namespace

TEST_CASE(
    "mixer: a slot is disabled or failed in the same place and size, a bar inside its outline") {
    Fixture f;
    if (!f.ok) return;
    // A frozen clock: the marks follow what the test applies, not the real engine status.
    f.mixer->attention().now = [] { return std::chrono::steady_clock::time_point{}; };
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Hickory");
    const auto p2 = f.addFx(a, "Hickory");
    const auto p3 = f.addFx(a, "Hickory");
    f.exec("fx.set", R"({"fx":")" + p1 + R"(","bypass":true})");
    f.exec("fx.set", R"({"fx":")" + p2 + R"(","offline":true})");
    f.refresh();
    const ui::Theme& theme = f.root->theme();
    const ui::Rect before = f.node("mixer.fx." + p3)->bounds();
    const auto beforeX = static_cast<double>(before.x), beforeW = static_cast<double>(before.w),
               beforeH = static_cast<double>(before.h);
    CHECK(f.node("mixer.fx." + p1)->valueText().ends_with(" (bypassed)"));
    CHECK(f.node("mixer.fx." + p2)->valueText().ends_with(" (offline)"));
    CHECK_FALSE(is(barPixel(f, "mixer.fx." + p1), theme.color("meter-hot")));
    CHECK(f.node("mixer.fxadd." + a)->valueText() == "fx");
    CHECK(dynamic_cast<ui::Button*>(f.node("mixer.fxadd." + a))->textColor().empty());

    SUBCASE("disabled wins over bypassed and over offline; the bar is amber") {
        f.exec("plugin.override", R"({"plugin":"Hickory","mode":"disable"})");
        f.refresh();
        for (const auto& id : {p1, p2, p3}) {
            CHECK(f.node("mixer.fx." + id)->valueText().ends_with(" (disabled)"));
            CHECK(is(barPixel(f, "mixer.fx." + id), theme.color("meter-hot")));
        }
        // The slot keeps its place and its 18-unit height.
        CHECK(static_cast<double>(f.node("mixer.fx." + p3)->bounds().h) ==
              doctest::Approx(beforeH));
        CHECK(static_cast<double>(f.node("mixer.fx." + p3)->bounds().x) ==
              doctest::Approx(beforeX));
        CHECK(static_cast<double>(f.node("mixer.fx." + p3)->bounds().w) ==
              doctest::Approx(beforeW));
        CHECK(beforeH == doctest::Approx(18.0));
        f.snapshot("mixer-slot-states-disabled.png");
    }
    SUBCASE(
        "failed: the same slot, a red bar, \" (failed)\", and it wins over offline and bypassed") {
        engine::EngineStatus status;
        status.failedFx = {p1, p2};
        f.mixer->attention().apply(status);
        f.refresh();
        CHECK(f.node("mixer.fx." + p1)->valueText().ends_with(" (failed)"));
        CHECK(f.node("mixer.fx." + p2)->valueText().ends_with(" (failed)"));
        CHECK(is(barPixel(f, "mixer.fx." + p1), theme.color("meter-clip")));
        CHECK_FALSE(f.node("mixer.fx." + p3)->valueText().ends_with(" (failed)"));
        CHECK(static_cast<double>(f.node("mixer.fx." + p3)->bounds().h) ==
              doctest::Approx(beforeH));
        // "fx" is in the warning colour while the chain holds one: failed counts as disabled does.
        CHECK(dynamic_cast<ui::Button*>(f.node("mixer.fxadd." + a))->textColor() == "meter-hot");
        // A click opens no editor, and says why.
        const std::size_t shown = f.editors.size();
        const ui::Point click = f.centre(f.node("mixer.fx." + p1));
        f.pointer(ui::PointerEvent::Type::Down, click);
        f.pointer(ui::PointerEvent::Type::Up, click);
        CHECK(f.editors.size() == shown);
        REQUIRE_FALSE(f.errors.empty());
        CHECK(f.errors.back().find("failed") != std::string::npos);
        f.errors.clear();
        f.snapshot("mixer-slot-states-failed.png");
        // Restarted (no longer failed): the marks go.
        f.mixer->attention().apply(engine::EngineStatus{});
        f.refresh();
        CHECK(f.node("mixer.fx." + p1)->valueText().ends_with(" (bypassed)"));
        CHECK(dynamic_cast<ui::Button*>(f.node("mixer.fxadd." + a))->textColor().empty());
    }
    SUBCASE("all the states in one strip, for the picture") {
        // Another plug-in for the ones that are not disabled: an override is per plug-in.
        std::string other;
        const auto listed = f.exec("plugin.list", "{}");
        for (const auto& p : listed.find("plugins")->asArray())
            if (other.empty() && p.find("name")->asString() != "Hickory")
                other = p.find("id")->asString();
        if (other.empty()) return;  // a partial build has one plug-in only
        const auto q1 =
            f.exec("fx.add", R"({"track":")" + a + R"(","plugin":")" + other + R"("})")["fx"]
                .asString();
        const auto q2 =
            f.exec("fx.add", R"({"track":")" + a + R"(","plugin":")" + other + R"("})")["fx"]
                .asString();
        f.exec("fx.set", R"({"fx":")" + q1 + R"(","bypass":true})");
        engine::EngineStatus status;
        status.failedFx = {q2};
        f.mixer->attention().apply(status);
        f.exec("plugin.override", R"({"plugin":"Hickory","mode":"disable"})");
        f.refresh();
        CHECK(f.node("mixer.fx." + q1)->valueText().ends_with(" (bypassed)"));
        CHECK(f.node("mixer.fx." + q2)->valueText().ends_with(" (failed)"));
        CHECK(f.node("mixer.fx." + p1)->valueText().ends_with(" (disabled)"));
        f.snapshot("mixer-slot-states.png");
    }
    SUBCASE("a disabled plug-in that is also listed failed reads as disabled") {
        f.exec("plugin.override", R"({"plugin":"Hickory","mode":"disable"})");
        engine::EngineStatus status;
        status.failedFx = {p1};
        f.mixer->attention().apply(status);
        f.refresh();
        CHECK(f.node("mixer.fx." + p1)->valueText().ends_with(" (disabled)"));
        CHECK(is(barPixel(f, "mixer.fx." + p1), theme.color("meter-hot")));
    }
}

TEST_CASE("mixer: a plug-in's right-click menu duplicates it after itself or deletes it") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Hickory");
    const auto p2 = f.addFx(a, "Hickory");
    f.exec("fx.set", R"({"fx":")" + p1 + R"(","label":"First"})");
    f.refresh();
    // Menu rows are 26 px: Rename…, Duplicate, Delete.
    auto choose = [&](const std::string& fx, int row) {
        const ui::Point at = f.centre(f.node("mixer.fx." + fx));
        f.pointer(ui::PointerEvent::Type::Down, at, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, at, 1, 0, 3);
        ui::Node* menu = f.node("mixer.fxmenu." + fx);
        const ui::Point item{menu->bounds().x + 40, menu->bounds().y + row * 26 + 13};
        f.pointer(ui::PointerEvent::Type::Move, item);
        f.pointer(ui::PointerEvent::Type::Down, item);
        f.pointer(ui::PointerEvent::Type::Up, item);
        f.refresh();
    };
    const auto before = f.steps();
    choose(p1, 1);
    REQUIRE(f.chain(a).size() == 3);
    const auto copy = f.chain(a)[1];
    CHECK(f.chain(a) == std::vector<std::string>{p1, copy, p2});
    CHECK(f.app->head().fx.find(*model::Id::parse(copy))->label == "First");
    CHECK(f.steps() == before + 1);
    choose(p1, 2);
    CHECK(f.chain(a) == std::vector<std::string>{copy, p2});
    CHECK(f.steps() == before + 2);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: the track menu duplicates the selected strips, and selects the copies") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto c = f.addTrack("channel", "C");
    f.addFx(a, "Hickory");
    f.refresh();
    f.ui.selectedTracks = {a, c};
    f.refresh();
    const ui::Point at = f.empty(c);
    f.pointer(ui::PointerEvent::Type::Down, at, 1, 0, 3);
    f.pointer(ui::PointerEvent::Type::Up, at, 1, 0, 3);
    ui::Node* menu = f.node("track.menu." + c);
    const auto before = f.steps();
    const ui::Point dup{menu->bounds().x + 40,
                        menu->bounds().y +
                            static_cast<float>(ui::menuItems(*menu).size() - 2) * 26 +
                            13};  // Duplicate
    f.pointer(ui::PointerEvent::Type::Move, dup);
    f.pointer(ui::PointerEvent::Type::Down, dup);
    f.pointer(ui::PointerEvent::Type::Up, dup);
    f.refresh();
    const auto kids = f.children("tr_0");
    REQUIRE(kids.size() == 5);  // each copy right after its original
    CHECK(kids[0] == a);
    CHECK(kids[2] == b);
    CHECK(kids[3] == c);
    CHECK(f.track(kids[1]).name == "A");
    CHECK(f.chain(kids[1]).size() == 1);
    CHECK(f.track(kids[4]).name == "C");
    CHECK(f.ui.selectedTracks == std::vector<std::string>{kids[1], kids[4]});
    CHECK(f.steps() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a strip drag tolerates a deleted destination before refresh") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A"), b = f.addTrack("channel", "B"),
               c = f.addTrack("channel", "C");
    f.refresh();
    const auto from = f.empty(c);
    const auto to = f.empty(b);
    f.pointer(ui::PointerEvent::Type::Down, from);
    // A model edit earlier in the input batch can leave the old strip nodes
    // visible until Mixer::sync. The deleted B is the candidate after the gap.
    f.exec("track.remove", "{\"track\":\"" + b + "\"}");
    f.pointer(ui::PointerEvent::Type::Move, {to.x - 8, to.y});
    CHECK(f.mixer->drag().active);
    f.pointer(ui::PointerEvent::Type::Up, {to.x - 8, to.y});
    CHECK(f.children("tr_0") == std::vector<std::string>{a, c});
    CHECK(f.errors.empty());
    f.refresh();
    CHECK_FALSE(f.mixer->drag().active);
}

TEST_CASE("mixer: dragging a strip's empty part reorders it among its own kind, in the model") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A"), b = f.addTrack("channel", "B"),
               c = f.addTrack("channel", "C");
    const auto bus = f.addTrack("buss", "Bus");
    const auto master = std::string("tr_0");
    f.refresh();
    // Channels, then the buss, then the master.
    REQUIRE(f.mixer->strips().size() == 5);
    CHECK(f.children(master) == std::vector<std::string>{a, b, c, bus});
    const auto before = f.steps();
    // A to after C: the insertion bar is shown while it moves.
    const ui::Rect rc = f.strip(c).node->bounds();
    f.drag(f.empty(a), {rc.x + rc.w - 10, rc.y + 200}, false);
    CHECK(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Strip);
    f.snapshot("mixer-drag-strip.png");
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Reorder);
    CHECK(f.root->cursor() == ui::Cursor::Move);  // a release here moves it
    f.pointer(ui::PointerEvent::Type::Move, f.empty(bus));
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::None);
    CHECK(f.root->cursor() == ui::Cursor::NotAllowed);  // not among the busses
    // Over its own place a release changes nothing, and nothing refuses it.
    f.pointer(ui::PointerEvent::Type::Move, f.empty(a));
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::None);
    CHECK(f.mixer->drag().home);
    CHECK(f.root->cursor() == ui::Cursor::Move);
    f.pointer(ui::PointerEvent::Type::Move, {rc.x + rc.w - 10, rc.y + 200});
    CHECK(f.root->cursor() == ui::Cursor::Move);
    f.pointer(ui::PointerEvent::Type::Up, {rc.x + rc.w - 10, rc.y + 200});
    CHECK_FALSE(f.root->dragCursor());
    CHECK(f.children(master) == std::vector<std::string>{b, c, a, bus});  // not after the buss
    CHECK(f.steps() == before + 1);
    f.refresh();
    CHECK(f.mixer->strips()[0].track == b);
    CHECK(f.mixer->strips()[2].track == a);
    // C to the front.
    f.drag(f.empty(c), {f.strip(b).node->bounds().x + 5, f.empty(b).y});
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    f.refresh();
    // Over the buss strip or the master: no drop. Escape: none either.
    const auto steps = f.steps();
    f.drag(f.empty(c), f.empty(bus));
    f.drag(f.empty(c), f.empty(master));
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    f.drag(f.empty(c), f.empty(a), false);
    CHECK(f.root->dragCursor());
    f.root->key(SDLK_ESCAPE);
    CHECK_FALSE(f.root->dragCursor());  // given up: the pointer is the nodes' again
    f.pointer(ui::PointerEvent::Type::Up, f.empty(a));
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    // A drag of the empty bottom (nobody's strip): cancelled.
    f.drag(f.empty(c), {700, 20});
    CHECK(f.steps() == steps);
    // The master (tr_0) stays put; a button press is not a drag.
    f.drag(f.empty(master), f.empty(c));
    CHECK(f.steps() == steps);
    f.drag(f.centre(f.node("mixer.mute." + c)), f.empty(a));
    CHECK(f.steps() == steps);  // a control is not a handle: nothing moved
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    // Undo restores the order, a step at a time.
    f.exec("history.undo", "{}");
    CHECK(f.children(master) == std::vector<std::string>{b, c, a, bus});
    f.exec("history.undo", "{}");
    CHECK(f.children(master) == std::vector<std::string>{a, b, c, bus});
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: dragging one of several selected strips moves them all, one undo step") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto c = f.addTrack("channel", "C");
    f.refresh();
    f.ui.selectedTracks = {a, b};
    f.refresh();
    const auto before = f.steps();
    const ui::Rect last = f.strip(c).node->bounds();
    f.drag(f.empty(a), {last.right() - 10, last.y + 200});
    f.refresh();
    CHECK(f.children("tr_0") == std::vector<std::string>{c, a, b});
    CHECK(f.steps() == before + 1);
    CHECK(f.ui.selectedTracks == std::vector<std::string>{a, b});
    // A click without a drag selects the strip alone.
    f.pointer(ui::PointerEvent::Type::Down, f.empty(b));
    f.pointer(ui::PointerEvent::Type::Up, f.empty(b));
    CHECK(f.ui.selectedTracks == std::vector<std::string>{b});
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a strip held at the strips' edge scrolls them, and drops out of first view") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> ids;
    for (int i = 0; i < 30; ++i) ids.push_back(f.addTrack("channel", "T"));
    f.refresh();
    using T = ui::PointerEvent::Type;
    const ui::Rect area = f.node("mixer.scrolled")->bounds();
    const std::string first = ids.front(), last = ids.back();
    // More strips than fit: the last one starts out of view.
    REQUIRE(f.strip(last).node->bounds().x > area.right());
    const auto x = [&](const std::string& id) { return f.strip(id).node->bounds().x; };
    const float y = f.empty(first).y;
    // In the middle a drag scrolls nothing.
    f.pointer(T::Down, f.empty(first));
    f.pointer(T::Move, {area.x + area.w / 2, y});
    REQUIRE(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Strip);
    CHECK_FALSE(f.mixer->autoScrolling());
    CHECK_FALSE(f.mixer->autoScroll(1.0));
    // Held at the right edge the strips scroll on; further out is faster.
    const float start = x(last);
    f.pointer(T::Move, {area.right() - 4, y});
    REQUIRE(f.mixer->autoScrolling());
    // The insertion bar stays among the strips that scroll, off the master.
    CHECK(f.mixer->drag().marker.right() <= area.right());
    f.snapshot("mixer-edge-scroll-1-held.png");
    CHECK(f.mixer->autoScroll(0.25));
    f.root->frame(false);
    const float once = x(last);
    CHECK(once < start);
    f.pointer(T::Move, {area.right() + 40, y});
    CHECK(f.mixer->autoScroll(0.25));
    f.root->frame(false);
    CHECK(once - x(last) > start - once);
    // The loop's pass steps it by the time since the pass before, and by no
    // more than the cap: a frame that stalled for two seconds is a short
    // step, not a jump. The first pass of a scroll only starts the clock.
    {
        using namespace std::chrono;
        const auto at = [&] {
            f.root->frame(false);
            return x(last);
        };
        const float s0 = at();
        // No pass has run since this scroll began; the last one before it was
        // the fixture's, ten seconds earlier.
        f.mixer->sync(f.now += seconds(10));
        CHECK(at() == s0);
        f.mixer->sync(f.now += milliseconds(25));
        const double step = static_cast<double>(s0 - at());
        CHECK(step > 0);
        f.mixer->sync(f.now += seconds(2));
        CHECK(static_cast<double>(s0 - at()) == doctest::Approx(3.0 * step).epsilon(0.02));
    }
    // Back inside it stops.
    f.pointer(T::Move, {area.x + area.w / 2, y});
    CHECK_FALSE(f.mixer->autoScrolling());
    const float held = x(last);
    CHECK_FALSE(f.mixer->autoScroll(1.0));
    f.root->frame(false);
    CHECK(x(last) == held);
    // To the end: it stops there by itself, rests, and the drop lands after
    // the last strip, which the first view did not show.
    f.pointer(T::Move, {area.right() - 2, y});
    for (int i = 0; i < 400 && f.mixer->autoScroll(0.05); ++i) f.root->frame(false);
    f.root->frame(false);
    CHECK(f.strip(last).node->bounds().right() <= area.right() + 1);
    CHECK_FALSE(f.mixer->autoScroll(0.05));  // nothing more until the pointer moves
    CHECK(f.mixer->autoScrolling());         // still held at the edge
    f.snapshot("mixer-edge-scroll-2-at-the-end.png");
    const auto before = f.app->head().version;
    f.pointer(T::Move, {area.right() - 3, y});
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Reorder);
    f.pointer(T::Up, {area.right() - 3, y});
    CHECK_FALSE(f.mixer->autoScrolling());
    CHECK(f.app->head().version == before + 1);  // one edit
    CHECK(f.children("tr_0").back() == first);
    f.refresh();
    // And back: held at the left edge the strips return to the start.
    const float scrolled = x(ids[1]);
    f.pointer(T::Down, f.empty(first));
    f.pointer(T::Move, {area.x + 3, y});
    REQUIRE(f.mixer->autoScrolling());
    CHECK(f.mixer->autoScroll(0.25));
    f.root->frame(false);
    CHECK(x(ids[1]) > scrolled);
    f.root->key(SDLK_ESCAPE);
    CHECK_FALSE(f.mixer->autoScrolling());  // a drag given up scrolls no more
    f.pointer(T::Up, {area.x + 3, y});
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a plug-in held at an end of its strip's plug-in list scrolls the list") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    std::vector<std::string> fx;
    for (int i = 0; i < 20; ++i) fx.push_back(f.addFx(a, "Hickory"));
    // The other strip's chain is longer than its list shows too.
    std::vector<std::string> others;
    for (int i = 0; i < 12; ++i) others.push_back(f.addFx(b, "Hickory"));
    f.refresh();
    using T = ui::PointerEvent::Type;
    using Drop = gui::Mixer::DragView::Drop;
    const ui::Rect list = f.strip(a).chain->bounds();
    const std::string last = fx.back();
    const auto slot = [&](const std::string& id) { return f.node("mixer.fx." + id)->bounds(); };
    const auto y = [&](const std::string& id) { return slot(id).y; };
    // A chain longer than the list shows: its last plug-in starts out of view.
    REQUIRE(y(last) > list.bottom());
    REQUIRE(y(others.back()) > f.strip(b).chain->bounds().bottom());
    const float otherAt = y(others.front());
    const float x = list.x + list.w / 2, middle = list.y + list.h / 2;
    // The third plug-in, picked up in the list's middle: no scroll there.
    REQUIRE(std::abs(slot(fx[2]).y + slot(fx[2]).h / 2 - middle) < list.h / 2 - 24);
    f.pointer(T::Down, f.centre(f.node("mixer.fx." + fx[2])));
    f.pointer(T::Move, {x, middle});
    REQUIRE(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Fx);
    CHECK_FALSE(f.mixer->autoScrolling());
    CHECK_FALSE(f.mixer->autoScroll(1.0));
    // Held at the list's bottom it scrolls on; further out is faster.
    const float start = y(last);
    f.pointer(T::Move, {x, list.bottom() - 4});
    REQUIRE(f.mixer->autoScrolling());
    f.snapshot("mixer-fx-edge-scroll-1-held.png");
    CHECK(f.mixer->autoScroll(0.05));
    f.root->frame(false);
    const float once = y(last);
    CHECK(once < start);
    f.pointer(T::Move, {x, list.bottom() + 40});
    CHECK(f.mixer->autoScroll(0.05));
    f.root->frame(false);
    CHECK(once - y(last) > start - once);
    // Only the dragged plug-in's own list scrolls: the other strip's, which
    // could scroll as far, stays where it is.
    CHECK(y(others.front()) == otherAt);
    // The loop's pass steps it by the time since the pass before, and by no
    // more than the cap: a frame that stalled for two seconds is a short
    // step, not a jump. The first pass of a scroll only starts the clock.
    f.pointer(T::Move, {x, list.bottom() - 20});
    {
        using namespace std::chrono;
        const auto at = [&] {
            f.root->frame(false);
            return y(last);
        };
        const float s0 = at();
        // No pass has run since this scroll began; the last one before it was
        // the fixture's, ten seconds earlier.
        f.mixer->sync(f.now += seconds(10));
        CHECK(at() == s0);
        f.mixer->sync(f.now += milliseconds(25));
        const double step = static_cast<double>(s0 - at());
        CHECK(step > 0);
        f.mixer->sync(f.now += seconds(2));
        CHECK(static_cast<double>(s0 - at()) == doctest::Approx(3.0 * step).epsilon(0.02));
    }
    // Back in the middle it stops, and off its own strip too (a held plug-in
    // scrolls no list but its own).
    f.pointer(T::Move, {x, middle});
    CHECK_FALSE(f.mixer->autoScrolling());
    const float held = y(last);
    CHECK_FALSE(f.mixer->autoScroll(1.0));
    f.root->frame(false);
    CHECK(y(last) == held);
    const ui::Rect other = f.strip(b).chain->bounds();
    f.pointer(T::Move, {other.x + other.w / 2, other.bottom() - 4});
    CHECK(f.mixer->drag().drop == Drop::Copy);
    CHECK_FALSE(f.mixer->autoScrolling());
    // To the end: it stops there by itself, rests, and the drop lands after
    // the last plug-in, which the first view did not show. The insertion bar
    // is in view all the way.
    f.pointer(T::Move, {x, list.bottom() + 6});
    CHECK(f.mixer->drag().drop == Drop::Reorder);
    CHECK(f.mixer->drag().marker.h >= 2.5f);
    CHECK(f.mixer->drag().marker.bottom() <= list.bottom());
    for (int i = 0; i < 400 && f.mixer->autoScroll(0.05); ++i) f.root->frame(false);
    f.root->frame(false);
    CHECK(slot(last).bottom() <= list.bottom() + 1);
    CHECK_FALSE(f.mixer->autoScroll(0.05));  // nothing more until the pointer moves
    CHECK(f.mixer->autoScrolling());         // still held at the end
    CHECK(y(others.front()) == otherAt);     // and the other strip's list has not moved
    f.snapshot("mixer-fx-edge-scroll-2-at-the-end.png");
    const auto before = f.app->head().version;
    f.pointer(T::Move, {x, list.bottom() - 3});
    CHECK(f.mixer->drag().drop == Drop::Reorder);
    f.pointer(T::Up, {x, list.bottom() - 3});
    CHECK_FALSE(f.mixer->autoScrolling());
    CHECK(f.app->head().version == before + 1);  // one edit
    CHECK(f.chain(a).back() == fx[2]);
    // The rebuilt list is where the old one was scrolled to: the plug-in is
    // in view where it was dropped.
    f.refresh();
    CHECK(slot(fx[2]).bottom() <= list.bottom() + 1);
    CHECK(slot(fx[2]).y >= list.y);
    f.snapshot("mixer-fx-edge-scroll-3-dropped.png");
    // A plug-in picked up in the list's last row is inside the bottom zone:
    // that zone waits until the pointer has left it once.
    const ui::Point low = f.centre(f.node("mixer.fx." + fx[2]));
    REQUIRE(low.y > list.bottom() - 24);
    f.pointer(T::Down, low);
    f.pointer(T::Move, {low.x + 12, low.y});
    REQUIRE(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Fx);
    CHECK_FALSE(f.mixer->autoScrolling());
    f.pointer(T::Move, {low.x + 12, low.y + 3});  // still in it
    CHECK_FALSE(f.mixer->autoScrolling());
    f.pointer(T::Move, {x, middle});
    f.pointer(T::Move, {x, list.y + 4});  // the other zone was never held
    REQUIRE(f.mixer->autoScrolling());
    const float down = y(fx.front());
    CHECK(f.mixer->autoScroll(0.05));
    f.root->frame(false);
    CHECK(y(fx.front()) > down);
    f.pointer(T::Move, {low.x + 12, low.y});  // and the first one scrolls now
    CHECK(f.mixer->autoScrolling());
    f.root->key(SDLK_ESCAPE);
    CHECK_FALSE(f.mixer->autoScrolling());  // a drag given up scrolls no more
    f.pointer(T::Up, {low.x + 12, low.y});
    // Picked up in the last row and taken straight past the list's end: it
    // has left the zone, and scrolls.
    f.pointer(T::Down, low);
    f.pointer(T::Move, {low.x + 12, low.y});
    CHECK_FALSE(f.mixer->autoScrolling());
    f.pointer(T::Move, {low.x + 12, list.bottom() + 10});
    CHECK(f.mixer->autoScrolling());
    f.root->key(SDLK_ESCAPE);
    f.pointer(T::Up, {low.x + 12, list.bottom() + 10});
    CHECK(f.chain(a).back() == fx[2]);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a plug-in or an output held over a pinned master scrolls no strips") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> ids;
    for (int i = 0; i < 30; ++i) ids.push_back(f.addTrack("channel", "T"));
    const auto m2 = f.addTrack("master", "M2");
    const std::string first = ids.front();
    const auto fx = f.addFx(first, "Hickory");
    f.refresh();
    using T = ui::PointerEvent::Type;
    using Drop = gui::Mixer::DragView::Drop;
    const ui::Rect area = f.node("mixer.scrolled")->bounds();
    // More strips than fit, and the masters pinned past the edge of the
    // strips that scroll.
    REQUIRE(f.strip(ids.back()).node->bounds().x > area.right());
    const ui::Point master = f.centre(f.node("mixer.faderrow." + m2));
    REQUIRE(master.x > area.right());
    const auto x = [&] {
        f.root->frame(false);
        return f.strip(ids[1]).node->bounds().x;
    };
    const float x0 = x();
    const auto rests = [&] {
        CHECK_FALSE(f.mixer->autoScrolling());
        CHECK_FALSE(f.mixer->autoScroll(0.25));
        CHECK(x() == x0);
    };
    // A plug-in: at the edge of the strips that scroll it would scroll them;
    // over the master, where it can be dropped, it scrolls nothing.
    f.pointer(T::Down, f.centre(f.node("mixer.fx." + fx)));
    f.pointer(T::Move, {area.right() - 4, master.y});
    REQUIRE(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Fx);
    CHECK(f.mixer->autoScrolling());
    f.pointer(T::Move, master);
    CHECK(f.mixer->drag().drop == Drop::Copy);
    CHECK(f.mixer->drag().target == m2);
    rests();
    f.root->key(SDLK_ESCAPE);
    f.pointer(T::Up, master);
    // An output, held over the master to send to it.
    f.pointer(T::Down, f.centre(f.node("mixer.out." + first)));
    f.pointer(T::Move, master);
    REQUIRE(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Send);
    CHECK(f.mixer->drag().drop == Drop::Copy);
    CHECK(f.mixer->drag().target == m2);
    rests();
    f.root->key(SDLK_ESCAPE);
    f.pointer(T::Up, master);
    // A strip there is past the edge of its own kind's strips: they scroll
    // on, so that it can be put last.
    f.pointer(T::Down, f.empty(first));
    f.pointer(T::Move, {master.x, f.empty(first).y});
    REQUIRE(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Strip);
    CHECK(f.mixer->autoScrolling());
    CHECK(f.mixer->autoScroll(0.25));
    CHECK(x() < x0);
    f.root->key(SDLK_ESCAPE);
    f.pointer(T::Up, master);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a drag held still at the edge shows its target as the strips scroll under it") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> ids;
    for (int i = 0; i < 30; ++i) ids.push_back(f.addTrack("channel", "T"));
    const std::string first = ids.front();
    const auto fx = f.addFx(first, "Hickory");
    f.refresh();
    using T = ui::PointerEvent::Type;
    using namespace std::chrono;
    using DragView = gui::Mixer::DragView;
    const ui::Rect area = f.node("mixer.scrolled")->bounds();
    REQUIRE(f.strip(ids.back()).node->bounds().x > area.right());
    const ui::Point edge{area.right() - 4, f.empty(first).y};
    const ui::Point middle{area.x + area.w / 2, edge.y};
    // The strip under a point, from where the layout has put the strips.
    const auto over = [&](ui::Point p) {
        for (const auto& s : f.mixer->strips())
            if (s.node->bounds().contains(p)) return s.track;
        return std::string();
    };
    const auto pass = [&](milliseconds step) {  // the loop's
        f.mixer->sync(f.now += step);
        f.root->frame(false);
    };
    const auto same = [](const ui::Rect& a, const ui::Rect& b) {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    };

    // A plug-in held at the right edge, and not moved again: the strips
    // scroll under it, and the strip it would be copied to is the one that
    // has come under the pointer.
    f.pointer(T::Down, f.centre(f.node("mixer.fx." + fx)));
    f.pointer(T::Move, edge);
    REQUIRE(f.mixer->drag().kind == DragView::Kind::Fx);
    REQUIRE(f.mixer->autoScrolling());
    const std::string began = over(edge);
    REQUIRE_FALSE(began.empty());
    CHECK(f.mixer->drag().target == began);
    for (int i = 0; i < 200 && (over(edge).empty() || over(edge) == began); ++i)
        pass(milliseconds(25));
    const std::string now = over(edge);
    REQUIRE_FALSE(now.empty());
    REQUIRE(now != began);
    pass(milliseconds(0));  // a pass that takes no time moves nothing
    REQUIRE(over(edge) == now);
    CHECK(f.mixer->drag().drop == DragView::Drop::Copy);
    CHECK(f.mixer->drag().target == now);
    const ui::Rect strip = f.strip(now).node->bounds();
    CHECK(same(f.mixer->drag().marker, strip.intersect(f.strip(now).node->parent()->bounds())));
    f.snapshot("mixer-edge-scroll-3-still-pointer.png");
    f.root->key(SDLK_ESCAPE);
    f.pointer(T::Up, edge);

    // A strip held there: the insertion bar and the moves a release would
    // make are those a fresh look at the same point gives.
    std::string held;
    for (const auto& id : ids)
        if (held.empty() && f.strip(id).node->bounds().x >= area.x + 8) held = id;
    REQUIRE_FALSE(held.empty());
    f.pointer(T::Down, f.empty(held));
    f.pointer(T::Move, edge);
    REQUIRE(f.mixer->drag().kind == DragView::Kind::Strip);
    REQUIRE(f.mixer->autoScrolling());
    const auto moves = [&] {
        std::vector<std::pair<std::string, std::size_t>> out;
        for (const auto& m : f.mixer->drag().moves) out.emplace_back(m.track.str(), m.index);
        return out;
    };
    const auto moves0 = moves();
    REQUIRE_FALSE(moves0.empty());
    const float x0 = f.strip(ids.back()).node->bounds().x;
    for (int i = 0; i < 12; ++i) pass(milliseconds(25));
    CHECK(x0 - f.strip(ids.back()).node->bounds().x > 96 + 48);  // more than a strip and a half
    pass(milliseconds(0));
    const DragView still = f.mixer->drag();
    const auto stillMoves = moves();
    CHECK(still.drop == DragView::Drop::Reorder);
    CHECK(stillMoves != moves0);
    f.pointer(T::Move, edge);  // the fresh look
    CHECK(f.mixer->drag().drop == still.drop);
    CHECK(same(f.mixer->drag().marker, still.marker));
    CHECK(moves() == stillMoves);

    // Leaving the zone ends the scroll's clock: held at the edge again after
    // a pause in the middle, the first pass only starts it, however long the
    // pause was.
    const auto at = [&] {
        f.root->frame(false);
        return f.strip(ids.back()).node->bounds().x;
    };
    f.pointer(T::Move, middle);
    CHECK_FALSE(f.mixer->autoScrolling());
    pass(milliseconds(10));
    f.pointer(T::Move, edge);
    REQUIRE(f.mixer->autoScrolling());
    const float s0 = at();
    pass(seconds(10));
    CHECK(at() == s0);
    pass(milliseconds(25));
    CHECK(at() < s0);
    f.root->key(SDLK_ESCAPE);
    f.pointer(T::Up, edge);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a plug-in list keeps its place when the strips are rebuilt") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    std::vector<std::string> fx;
    for (int i = 0; i < 12; ++i) fx.push_back(f.addFx(a, "Hickory"));
    f.refresh();
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = f.centre(f.node("mixer.fx." + fx[2]));
    wheel.wheelY = -3;
    f.root->pointer(wheel);
    f.root->frame(false);
    const ui::Node* old = f.node("mixer.fx." + fx[0]);
    const float y = old->bounds().y;
    REQUIRE(y < f.strip(a).chain->bounds().y - 10);
    // Another strip gets a plug-in: every strip is rebuilt.
    f.addFx(b, "Hickory");
    f.refresh();
    CHECK(f.node("mixer.fx." + fx[0])->bounds().y == y);
    // A chain that got shorter than the list shows comes back to its top.
    for (int i = 3; i < 12; ++i) f.exec("fx.remove", R"({"fx":")" + fx[i] + R"("})");
    f.refresh();
    CHECK(f.node("mixer.fx." + fx[0])->bounds().y == f.strip(a).chain->bounds().y);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: masters reorder among masters; tr_0 stays first") {
    Fixture f;
    if (!f.ok) return;
    const auto m2 = f.addTrack("master", "M2"), m3 = f.addTrack("master", "M3");
    f.refresh();
    const auto order = [&] {
        std::vector<std::string> out;
        for (model::Id m : f.app->head().project->masters) out.push_back(m.str());
        return out;
    };
    REQUIRE(order() == std::vector<std::string>{"tr_0", m2, m3});
    // M3 to the left of M2 (tr_0 still first).
    f.drag(f.empty(m3), {f.strip(m2).node->bounds().x + 5, f.empty(m2).y});
    CHECK(order() == std::vector<std::string>{"tr_0", m3, m2});
    f.refresh();
    // Left of tr_0 is not allowed: it stays first.
    f.drag(f.empty(m2), {f.strip("tr_0").node->bounds().x + 5, f.empty("tr_0").y});
    CHECK(order()[0] == "tr_0");
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a selected strip's controls act on every selected track, one undo step each") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto bus = f.addTrack("buss", "Bus");
    const auto c = f.addTrack("channel", "C");
    f.exec("track.set", R"({"track":")" + b + R"(","volume_db":-6,"pan":-20})");
    f.ui.selectedTracks = {a, b, bus};
    f.refresh();
    auto steps = [&] { return f.steps(); };
    // Mute: all three, one step.
    auto s0 = steps();
    REQUIRE(f.root->click("mixer.mute." + a));
    CHECK(f.track(a).mute);
    CHECK(f.track(b).mute);
    CHECK(f.track(bus).mute);
    CHECK_FALSE(f.track(c).mute);
    CHECK(steps() == s0 + 1);
    f.refresh();
    // Arm and monitoring: the channels only (a buss records nothing).
    REQUIRE(f.root->click("mixer.arm." + b));
    CHECK(f.track(a).record.armed);
    CHECK(f.track(b).record.armed);
    CHECK_FALSE(f.track(c).record.armed);
    f.refresh();
    REQUIRE(f.root->click("mixer.monitor." + a));  // on (the default) -> auto
    CHECK(f.track(a).record.monitor == model::Monitor::Auto);
    CHECK(f.track(b).record.monitor == model::Monitor::Auto);
    CHECK(f.track(bus).record.monitor == model::Monitor::Off);
    CHECK(f.track(c).record.monitor == model::Monitor::On);
    f.refresh();
    // A fader drag moves each selected strip by the same amount; one step.
    s0 = steps();
    const ui::Rect fb = f.strip(a).fader->bounds();
    const ui::Point cap{fb.x + fb.w / 2,
                        fb.bottom() - 8 - static_cast<float>(ui::faderPosition(0)) * (fb.h - 16)};
    f.drag(cap, {cap.x, cap.y + 30});
    const double moved = f.track(a).volumeDb;
    CHECK(moved < -1);
    CHECK(f.track(b).volumeDb == doctest::Approx(-6 + moved));
    CHECK(f.track(bus).volumeDb == doctest::Approx(moved));
    CHECK(f.track(c).volumeDb == 0.0);
    CHECK(steps() == s0 + 1);
    f.refresh();
    // Pan the same way: relative, clamped.
    const ui::Rect pb = f.strip(a).pan->bounds();
    f.drag({pb.x + pb.w / 2, pb.y + 8}, {pb.right() - 2, pb.y + 8});
    CHECK(f.track(a).pan > 50);
    CHECK(f.track(b).pan == doctest::Approx(std::clamp(-20 + f.track(a).pan, -100.0, 100.0)));
    f.refresh();
    // A strip outside the selection acts alone.
    REQUIRE(f.root->click("mixer.solo." + c));
    CHECK(f.track(c).solo == model::Solo::On);
    CHECK(f.track(a).solo == model::Solo::Off);
    f.refresh();
    // Polarity and mono (ADR-0022) spread like mute, busses included.
    s0 = steps();
    REQUIRE(f.root->click("mixer.polarity." + bus));
    CHECK(f.track(a).phaseInvert);
    CHECK(f.track(b).phaseInvert);
    CHECK(f.track(bus).phaseInvert);
    CHECK_FALSE(f.track(c).phaseInvert);
    CHECK(steps() == s0 + 1);
    f.refresh();
    CHECK(f.strip(b).polarity->on());
    REQUIRE(f.root->click("mixer.mono." + c));
    CHECK(f.track(c).mono);
    CHECK_FALSE(f.track(a).mono);
    f.refresh();
    REQUIRE(f.root->click("mixer.mono.tr_0"));  // the master's: a mono check of the mix
    CHECK(f.track("tr_0").mono);
    f.refresh();
    CHECK(f.strip("tr_0").mono->on());
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a double-click on a name renames it in place; a click elsewhere keeps it") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    const ui::Point name = f.centre(f.node("mixer.name." + a));
    f.pointer(ui::PointerEvent::Type::Down, name);
    f.pointer(ui::PointerEvent::Type::Up, name);
    f.pointer(ui::PointerEvent::Type::Down, name, 2);
    f.pointer(ui::PointerEvent::Type::Up, name);
    auto* field = dynamic_cast<ui::TextField*>(f.root->find("mixer.rename." + a));
    REQUIRE(field);
    CHECK(field->visible);
    CHECK(f.root->focused() == field);
    f.root->text("Lead");  // replaces the selected name
    // A click on empty space elsewhere ends the edit and keeps it.
    const ui::Rect strips = f.node("mixer.strips")->bounds();
    f.pointer(ui::PointerEvent::Type::Down, {strips.right() - 300, strips.y + 40});
    f.pointer(ui::PointerEvent::Type::Up, {strips.right() - 300, strips.y + 40});
    CHECK(f.track(a).name == "Lead");
    CHECK(f.root->focused() == nullptr);  // Space is play/stop again
    f.refresh();
    CHECK(f.strip(a).name->text() == "Lead");
    // Escape drops an edit.
    const ui::Point again = f.centre(f.node("mixer.name." + a));
    f.pointer(ui::PointerEvent::Type::Down, again, 2);
    f.pointer(ui::PointerEvent::Type::Up, again);
    f.root->text("Nope");
    f.root->key(SDLK_ESCAPE);
    CHECK(f.track(a).name == "Lead");
}

TEST_CASE("mixer: \"out\" opens the routing matrix; a drag onto a buss makes one send") {
    Fixture f;
    if (!f.ok) return;
    int routing = 0;
    f.actions->addUi("ui.routing", "View: routing matrix", [&] { ++routing; });
    const auto a = f.addTrack("channel", "A");
    const auto bus = f.addTrack("buss", "Bus");
    f.refresh();
    REQUIRE(f.root->click("mixer.out." + a));
    CHECK(routing == 1);
    const auto before = f.steps();
    f.drag(f.centre(f.node("mixer.out." + a)), f.centre(f.node("mixer.faderrow." + bus)), false);
    CHECK(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Send);
    CHECK(f.mixer->drag().target == bus);
    // A wire from "out" (as in the TCP), no label chip.
    CHECK(f.mixer->drag().label.empty());
    CHECK(f.mixer->drag().source.contains(f.centre(f.node("mixer.out." + a))));
    f.snapshot("mixer-send-wire.png");
    f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node("mixer.faderrow." + bus)));
    CHECK(routing == 1);  // a drag is not a click
    CHECK(f.steps() == before + 1);
    bool sent = false;
    for (const auto& e : f.app->head().routes)
        sent = sent || (e.value->from.track && e.value->from.track->str() == a &&
                        e.value->to.track && e.value->to.track->str() == bus);
    CHECK(sent);
    // A second drag onto a strip it already feeds marks nothing and adds
    // nothing: the buss refuses it. Over its own strip nothing refuses it.
    f.pointer(ui::PointerEvent::Type::Down, f.centre(f.node("mixer.out." + a)));
    f.pointer(ui::PointerEvent::Type::Move, f.centre(f.node("mixer.faderrow." + a)));
    CHECK(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Send);
    CHECK(f.mixer->drag().home);
    CHECK(f.root->cursor() == ui::Cursor::Move);
    f.pointer(ui::PointerEvent::Type::Move, f.centre(f.node("mixer.faderrow." + bus)));
    CHECK(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Send);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::None);
    CHECK_FALSE(f.mixer->drag().home);
    CHECK(f.root->cursor() == ui::Cursor::NotAllowed);
    f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node("mixer.faderrow." + bus)));
    CHECK(f.steps() == before + 1);
    CHECK(f.app->head().routes.size() == 1);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: sends scroll past two; a click opens the matrix, Delete or Alt+click removes") {
    Fixture f;
    if (!f.ok) return;
    int routing = 0;
    f.actions->addUi("ui.routing", "View: routing matrix", [&] { ++routing; });
    const auto a = f.addTrack("channel", "A");
    std::vector<std::string> sends;
    for (const char* name : {"B1", "B2", "B3"}) {
        const auto bus = f.addTrack("buss", name);
        sends.push_back(
            f.exec("route.add", R"({"from":")" + a + R"(","to":")" + bus + R"("})")["id"]
                .asString());
    }
    f.refresh();
    // Two rows in view and a bar; the wheel brings the third in.
    ui::Node* list = f.node("mixer.sends." + a);
    CHECK(near(list->bounds().h, 28));
    CHECK(f.node("mixer.sends." + a + ".bar")->visible);
    CHECK(f.node("mixer.send." + sends[2])->bounds().y >= list->bounds().bottom() - 0.5f);
    f.pointer(ui::PointerEvent::Type::Move, f.centre(f.node("mixer.send." + sends[1])));
    f.snapshot("mixer-sends.png");  // the second brightened under the pointer
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = f.centre(list);
    wheel.wheelY = -1;
    f.root->pointer(wheel);
    f.root->frame(false);
    CHECK(near(f.node("mixer.send." + sends[2])->bounds().bottom(), list->bounds().bottom()));
    // A click opens the routing matrix.
    REQUIRE(f.root->click("mixer.send." + sends[2]));
    CHECK(routing == 1);
    // Its right-click menu has one item, Delete.
    const auto before = f.steps();
    const ui::Point at = f.centre(f.node("mixer.send." + sends[2]));
    f.pointer(ui::PointerEvent::Type::Down, at, 1, 0, 3);
    f.pointer(ui::PointerEvent::Type::Up, at, 1, 0, 3);
    ui::Node* menu = f.node("mixer.sendmenu." + sends[2]);
    const ui::Point item{menu->bounds().x + 40, menu->bounds().y + 13};
    f.pointer(ui::PointerEvent::Type::Move, item);
    f.pointer(ui::PointerEvent::Type::Down, item);
    f.pointer(ui::PointerEvent::Type::Up, item);
    f.refresh();
    CHECK_FALSE(f.app->head().routes.find(*model::Id::parse(sends[2])));
    CHECK(f.steps() == before + 1);
    // Two left: no bar. Alt+click removes one.
    CHECK_FALSE(f.node("mixer.sends." + a + ".bar")->visible);
    const ui::Point first = f.centre(f.node("mixer.send." + sends[0]));
    f.pointer(ui::PointerEvent::Type::Down, first, 1, ui::kAlt);
    f.pointer(ui::PointerEvent::Type::Up, first, 1, ui::kAlt);
    f.refresh();
    CHECK_FALSE(f.app->head().routes.find(*model::Id::parse(sends[0])));
    CHECK(f.steps() == before + 2);
    CHECK(routing == 1);  // neither opened the matrix
    f.exec("history.undo", "{}");
    CHECK(f.app->head().routes.find(*model::Id::parse(sends[0])));
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: the chain's empty part takes a double-click (plugin browser); input picks") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> added;
    f.actions->addUi("ui.fx.add", "FX: add plugin…", "track", [&](const json::Value& v) -> Status {
        added.push_back(v.isString() ? v.asString() : v.find("track")->asString());
        return {};
    });
    std::vector<model::Id> picked, outputs;
    f.mixer->pickInput = [&](const model::Id& track, const ui::Rect&) { picked.push_back(track); };
    f.mixer->pickOutput = [&](const model::Id& track, const ui::Rect&) {
        outputs.push_back(track);
    };
    f.mixer->outputText = [](const model::Id&) { return std::string("out 1/2"); };
    f.mixer->inputText = [](const model::Id&) { return std::string("AUX0"); };
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    const auto strips = f.mixer->strips().size();
    const ui::Point chain = f.centre(f.strip(a).chain);
    f.pointer(ui::PointerEvent::Type::Down, chain);
    f.pointer(ui::PointerEvent::Type::Up, chain);
    f.pointer(ui::PointerEvent::Type::Down, chain, 2);
    f.pointer(ui::PointerEvent::Type::Up, chain);
    CHECK(added == std::vector<std::string>{a});
    f.refresh();
    CHECK(f.mixer->strips().size() == strips);  // not a new channel
    REQUIRE(f.strip(a).input);
    CHECK(f.strip(a).input->valueText() == "AUX0");
    REQUIRE(f.root->click("mixer.input." + a));
    REQUIRE(picked.size() == 1);
    CHECK(picked[0].str() == a);
    // A master's out button: its hardware outputs (ADR-0056), not a send.
    REQUIRE(f.strip(model::kMaster.str()).out);
    CHECK(f.strip(model::kMaster.str()).out->valueText() == "out 1/2");
    REQUIRE(f.root->click("mixer.out.tr_0"));
    REQUIRE(outputs.size() == 1);
    CHECK(outputs[0] == model::kMaster);
}

TEST_CASE("preferences: remembered plug-in choices are listed, forgotten one by one or all") {
    Fixture f;
    if (!f.ok) return;
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    const auto file = f.dir / "config" / "preferences.json";
    gui::Preferences start;
    start.rememberedLoads = {
        {"clap", "com.acme.canary", "0123abcd", "bridge", "Canary Verb", "Acme", "1.2.0", 10},
        {"vst3", "com.acme.other", "ff00", "disable", "", "", "", 20},
        {"clap", "com.acme.fine", "aa", "normal", "Fine EQ", "Acme", "3.0", 30}};
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, start, file);
    std::vector<std::string> problems;
    prefs.onError = [&](const std::string& message) { problems.push_back(message); };
    prefs.open();
    f.root->frame(false);
    // A section of its own: the heading, the count, Forget all and a rule, then a row to a choice.
    REQUIRE(f.root->find("prefs.remembered"));
    CHECK(f.node("prefs.remembered.head.title")->valueText() == "Plug-in choices");
    CHECK(f.node("prefs.remembered.count")->valueText() == "3 choices");
    CHECK(f.root->find("prefs.remembered.head.rule"));
    CHECK(f.node("prefs.remembered.row.0.name")->valueText() == "Canary Verb  Acme \u00b7 1.2.0");
    CHECK(f.node("prefs.remembered.row.0.choice")->valueText() == "In a separate process");
    CHECK(f.node("prefs.remembered.row.1.name")->valueText() == "Unknown plug-in");
    CHECK(f.node("prefs.remembered.row.1.choice")->valueText() == "Disabled");
    CHECK(f.node("prefs.remembered.row.2.choice")->valueText() == "Normally");
    // The columns sit on the form's grid: the choice where the other rows' controls start, Forget
    // at the right edge, Forget all at the right of the heading.
    CHECK(f.node("prefs.remembered.row.0.choice")->bounds().x ==
          f.node("prefs.monitor.set")->bounds().x);
    const float edge = f.node("prefs.monitor.set")->bounds().right();
    CHECK(std::abs(f.node("prefs.remembered.row.0.forget")->bounds().right() - edge) < 2.0f);
    CHECK(std::abs(f.node("prefs.remembered.forget_all")->bounds().right() - edge) < 2.0f);
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS")) {
        f.root->damageAll();
        f.root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / "prefs-remembered.png",
                            f.root->readBack());
    }
    // Forget the middle one: saved, the rest keep their order.
    REQUIRE(f.root->click("prefs.remembered.row.1.forget"));
    f.root->frame(false);
    CHECK(problems.empty());
    REQUIRE(prefs.preferences().rememberedLoads.size() == 2);
    CHECK(prefs.preferences().rememberedLoads[1].id == "com.acme.fine");
    CHECK(f.node("prefs.remembered.row.1.name")->valueText().rfind("Fine EQ", 0) == 0);
    CHECK_FALSE(f.root->find("prefs.remembered.row.2"));
    auto saved = gui::Preferences::load(file);
    REQUIRE(saved);
    CHECK(saved->rememberedLoads.size() == 2);
    // Forget all: the sentence stands where the rows were, and no button to clear nothing.
    REQUIRE(f.root->click("prefs.remembered.forget_all"));
    f.root->frame(false);
    CHECK(prefs.preferences().rememberedLoads.empty());
    CHECK(f.root->find("prefs.remembered.empty"));
    CHECK_FALSE(f.root->find("prefs.remembered.forget_all"));
    CHECK(f.node("prefs.remembered.count")->valueText() == "none");
    saved = gui::Preferences::load(file);
    REQUIRE(saved);
    CHECK(saved->rememberedLoads.empty());
    // A new dialog on no choices says so too.
    prefs.open();
    f.root->frame(false);
    CHECK(f.root->find("prefs.remembered.empty"));
}

TEST_CASE("mixer: the strips match their golden picture") {
    Fixture f;
    if (!f.ok) return;
    // One strip of each kind and each state a strip can be at rest in: a
    // send, a mute, a solo, an armed input, a moved fader and pan, an empty
    // chain. No plug-in (the stock ones are not in every build) and no level.
    const auto drums = f.addTrack("channel", "Drums");
    const auto bass = f.addTrack("channel", "Bass");
    const auto vocal = f.addTrack("channel", "Lead vocal");
    const auto band = f.addTrack("buss", "Band");
    f.exec("route.add", R"({"from":")" + drums + R"(","to":")" + band + R"("})");
    f.exec("track.set", R"({"track":")" + drums + R"(","volume_db":-6,"pan":-20})");
    f.exec("track.set", R"({"track":")" + bass + R"(","mute":true})");
    f.exec("track.set", R"({"track":")" + vocal + R"(","solo":"on"})");
    f.exec("track.set", R"({"track":")" + vocal + R"(","armed":true})");
    f.refresh();
    CHECK(f.mixer->strips().size() == 5);
    f.snapshot("mixer-strips.png");
    f.golden("mixer-strips.png");
}

TEST_CASE("mixer: meters at rising levels, for the picture: green, yellow, orange, red") {
    Fixture f;
    if (!f.ok) return;
    // Six channels at -30, -18, -12, -6, -2 and +1 dB peak (the RMS 6 dB
    // under): the bars show where on the scale each colour begins.
    const float levels[] = {-30, -18, -12, -6, -2, 1};
    std::vector<std::string> tracks;
    for (const float level : levels) {
        char name[16];
        std::snprintf(name, sizeof name, "%+.0f dB", static_cast<double>(level));
        tracks.push_back(f.addTrack("channel", name));
    }
    f.refresh();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const float peak = std::pow(10.0f, levels[i] / 20), rms = peak / 2;
        const float p[2] = {peak, peak * 0.7f}, r[2] = {rms, rms * 0.7f};
        f.strip(tracks[i]).meter->push(p, r);
    }
    f.root->frame(false);
    f.snapshot("mixer-levels.png");
    CHECK(f.strip(tracks.back()).meter->clipped());
    CHECK_FALSE(f.strip(tracks.front()).meter->clipped());
    // The scale has five marks, and the meter draws the same.
    CHECK(f.strip(tracks[0]).meter->marks() == std::vector<float>{0, -12, -24, -36, -48});
}

TEST_CASE("mixer: a strip shows its meter's highest peak under it; a click clears it") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    REQUIRE(f.strip(a).meter);
    REQUIRE(f.strip(a).peak);
    REQUIRE(f.node("mixer.scale." + a));
    CHECK(f.strip(a).peak->valueText().empty());  // nothing heard yet
    const float half[2] = {0.5f, 0.25f}, quiet[2] = {0.1f, 0.1f}, rms[2] = {0.1f, 0.1f};
    f.strip(a).meter->push(half, rms);
    CHECK(f.strip(a).peak->valueText() == "-6.0");
    f.strip(a).meter->push(quiet, rms);
    CHECK(f.strip(a).peak->valueText() == "-6.0");  // the highest, not the last
    const float over[2] = {0.5f, 1.2f};
    f.strip(a).meter->push(over, rms);
    CHECK(f.strip(a).peak->valueText() == "+1.6");
    f.root->frame(false);
    f.snapshot("mixer-peak.png");
    // The value under the fader is beside it, as before.
    CHECK(f.strip(a).value->valueText() == "+0.0");
    CHECK(f.strip(a).value->bounds().right() <= f.strip(a).peak->bounds().x + 0.5f);
    const ui::Point at = f.centre(f.strip(a).peak);
    f.pointer(ui::PointerEvent::Type::Down, at);
    f.pointer(ui::PointerEvent::Type::Up, at);
    CHECK(f.strip(a).peak->valueText().empty());
    CHECK_FALSE(f.strip(a).meter->clipped());
}

TEST_CASE("mixer: an empty chain is bare and a double-click adds; bypassed and offline say so") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> added;
    f.actions->addUi("ui.fx.add", "FX: add plugin…", "track", [&](const json::Value& v) -> Status {
        added.push_back(v.isString() ? v.asString() : v.find("track")->asString());
        return {};
    });
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto p1 = f.addFx(b, "Hickory");
    const auto p2 = f.addFx(b, "Hickory");
    const auto p3 = f.addFx(b, "Hickory");
    f.exec("fx.set", R"({"fx":")" + p2 + R"(","bypass":true})");
    f.exec("fx.set", R"({"fx":")" + p3 + R"(","offline":true})");
    f.refresh();
    // An empty chain shows no text. One click on it, at its top as anywhere,
    // does nothing; a double-click adds a plug-in, as the strip's "+" does.
    const ui::Rect chain = f.strip(a).chain->bounds();
    for (const float y : {9.0f, 40.0f}) {
        const ui::Point at{chain.x + chain.w / 2, chain.y + y};
        const std::size_t before = added.size();
        f.pointer(ui::PointerEvent::Type::Down, at);
        f.pointer(ui::PointerEvent::Type::Up, at);
        CHECK(added.size() == before);
        f.pointer(ui::PointerEvent::Type::Down, at, 2);
        f.pointer(ui::PointerEvent::Type::Up, at);
        CHECK(added.size() == before + 1);
    }
    CHECK(added == std::vector<std::string>{a, a});
    // A plug-in's name is its own; its state is told apart from it.
    const std::string name = f.node("mixer.fx." + p1)->valueText();
    CHECK(name.find('~') == std::string::npos);
    CHECK(name.find('(') == std::string::npos);
    CHECK(f.node("mixer.fx." + p2)->valueText() == name + " (bypassed)");
    CHECK(f.node("mixer.fx." + p3)->valueText() == name + " (offline)");
    f.exec("fx.set", R"({"fx":")" + p2 + R"(","bypass":false})");
    f.refresh();
    CHECK(f.node("mixer.fx." + p2)->valueText() == name);
    f.snapshot("mixer-chain-states.png");
}

TEST_CASE("preferences: the dialog applies each setting at once and saves it") {
    Fixture f;
    if (!f.ok) return;
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    const auto file = f.dir / "config" / "preferences.json";
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, gui::Preferences{}, file);
    prefs.onLinux = true;  // the rows every platform can test
    CHECK(f.app->defaultMonitor() == model::Monitor::On);
    CHECK(f.app->copyImportedMedia());
    auto port = json::Value::object();
    port["clock"] = true;
    port["offset_ms"] = -2.5;
    REQUIRE(f.app->setMidiPreferences({}, {{"Synth", port}}, false));
    prefs.open();
    f.root->frame(false);
    REQUIRE(prefs.isOpen());
    // New channels' monitoring.
    prefs.setDefaultMonitor(model::Monitor::Off);
    CHECK(f.node("prefs.monitor.set")->valueText() == "Off");
    CHECK(f.track(f.addTrack("channel", "A")).record.monitor == model::Monitor::Off);
    // The low-level settings are under Advanced, closed until asked for; the
    // dialog grows to show them, and remembers while the app runs.
    for (const char* id : {"prefs.section.audio.title", "prefs.section.recording.title",
                           "prefs.section.plugins.title", "prefs.section.editing.title",
                           "prefs.section.advanced.title"})
        CHECK(f.node(id)->visible);
    CHECK(f.node("prefs.audio")->bounds().y < f.node("prefs.monitor")->bounds().y);
    CHECK(f.node("prefs.monitor")->bounds().y < f.node("prefs.isolation")->bounds().y);
    CHECK_FALSE(f.node("prefs.advanced")->visible);
    CHECK(f.node("prefs.advanced.toggle")->valueText() == "Show");
    CHECK_FALSE(f.node("prefs.threads.set")->visibleInTree());
    const float folded = f.node("prefs")->bounds().h;
    // The everyday settings alone, the section of remembered plug-in choices (topic 7d) with its
    // sentence among them.
    CHECK(folded < 540);
    REQUIRE(f.root->click("prefs.advanced.toggle"));
    f.root->frame(false);
    CHECK(f.node("prefs.advanced")->visible);
    CHECK(f.node("prefs.advanced.toggle")->valueText() == "Hide");
    CHECK(f.node("prefs")->bounds().h > folded + 100);
    CHECK(f.node("prefs.latency.set")->bounds().bottom() <= f.node("prefs")->bounds().bottom());
    // Their notes name Linux's files on Linux only; elsewhere one line says
    // that the setting is Linux's.
    CHECK(gui::PreferencesDialog::memoryNotes(true).size() == 3);
    CHECK(gui::PreferencesDialog::idleNotes(true).size() == 2);
    for (const auto& notes :
         {gui::PreferencesDialog::memoryNotes(false), gui::PreferencesDialog::idleNotes(false)}) {
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].find("Linux only") == 0);
        CHECK(notes[0].find('/') == std::string::npos);
    }
    // Off Linux a row is there only while its setting is on.
    CHECK(gui::PreferencesDialog::showsLinuxOnly(true, false));
    CHECK(gui::PreferencesDialog::showsLinuxOnly(false, true));
    CHECK_FALSE(gui::PreferencesDialog::showsLinuxOnly(false, false));
    CHECK(f.node("prefs.latency.permission")->valueText().find("udev") != std::string::npos);
    // As macOS and Windows see it: neither row while both settings are off.
    REQUIRE(f.root->click("prefs.close"));
    prefs.onLinux = false;
    prefs.open();
    f.root->frame(false);
    CHECK(f.root->find("prefs.latency.set") == nullptr);
    CHECK(f.root->find("prefs.memory.set") == nullptr);
    CHECK(f.root->find("prefs.threads.set") != nullptr);
    REQUIRE(f.root->click("prefs.close"));
    prefs.onLinux = true;
    prefs.open();
    f.root->frame(false);
    // The window here holds it all: nothing to scroll.
    auto* scroll = dynamic_cast<ui::ScrollView*>(f.node("prefs.scroll"));
    REQUIRE(scroll);
    CHECK(scroll->contentHeight() <= scroll->bounds().h + 0.5f);
    REQUIRE(f.root->click("prefs.close"));
    // With less room than the settings need, the dialog stops growing and
    // the settings scroll under the header; the wheel brings the last one up.
    prefs.bodyLimit = 240;
    prefs.open();
    f.root->frame(false);
    scroll = dynamic_cast<ui::ScrollView*>(f.node("prefs.scroll"));
    REQUIRE(scroll);
    CHECK(scroll->bounds().h == doctest::Approx(240));
    CHECK(scroll->contentHeight() > 400);
    CHECK(f.node("prefs")->bounds().h < 320);
    CHECK(f.node("prefs.close")->bounds().bottom() <= scroll->bounds().y);
    const float before = f.node("prefs.latency.set")->bounds().y;
    CHECK(before > scroll->bounds().bottom());  // out of view
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = f.centre(scroll);
    wheel.wheelY = -20;
    f.root->pointer(wheel);
    f.root->frame(false);
    CHECK(f.node("prefs.latency.set")->bounds().bottom() <= scroll->bounds().bottom() + 0.5f);
    CHECK(f.node("prefs.close")->bounds().bottom() <= scroll->bounds().y);  // the header stayed
    f.snapshot("preferences-scrolled.png");
    REQUIRE(f.root->click("prefs.close"));
    prefs.bodyLimit = 0;
    prefs.open();
    f.root->frame(false);
    // The window is the limit otherwise: in one too low for the settings the
    // dialog stays inside it and they scroll; it grows back with the window.
    const float whole = f.node("prefs")->bounds().h;
    f.window->setSize(1400, 420);
    f.root->frame(false);
    scroll = dynamic_cast<ui::ScrollView*>(f.node("prefs.scroll"));
    CHECK(f.node("prefs")->bounds().y >= 8);
    CHECK(f.node("prefs")->bounds().bottom() < 420);
    CHECK(scroll->bounds().bottom() <= f.node("prefs")->bounds().bottom());
    CHECK(scroll->contentHeight() > scroll->bounds().h + 100);
    CHECK(f.node("prefs.close")->bounds().bottom() <= scroll->bounds().y);
    f.snapshot("preferences-low-window.png");
    f.window->setSize(1400, 900);
    f.root->frame(false);
    CHECK(near(f.node("prefs")->bounds().h, whole));
    scroll = dynamic_cast<ui::ScrollView*>(f.node("prefs.scroll"));
    CHECK(scroll->contentHeight() <= scroll->bounds().h + 0.5f);
    CHECK(f.node("prefs.advanced")->visible);  // as it was left
    f.snapshot("preferences-advanced.png");
    // Audio threads, from the menu; out of range refused.
    REQUIRE(f.root->click("prefs.threads.set"));
    auto* menu = dynamic_cast<ui::List*>(f.root->find("prefs.threads.menu"));
    REQUIRE(menu);
    auto activate = menu->onActivate;  // activation closes/destroys the menu
    activate(3);                       // Limit: 4
    CHECK(f.app->audioThreadLimit() == 4);
    CHECK_FALSE(prefs.setAudioThreads(65));
    // Copying imports, toggled.
    REQUIRE(f.root->click("prefs.imports.set"));
    CHECK_FALSE(f.app->copyImportedMedia());
    // Third-party plug-ins' isolation (ADR-0081), from the menu.
    CHECK(f.node("prefs.isolation.set")->valueText() == "In the app");
    REQUIRE(f.root->click("prefs.isolation.set"));
    auto* isolationMenu = dynamic_cast<ui::List*>(f.root->find("prefs.isolation.menu"));
    REQUIRE(isolationMenu);
    auto choose = isolationMenu->onActivate;
    choose(1);  // In a shared bridge process
    CHECK(f.app->pluginIsolation() == model::Isolation::SharedBridge);
    CHECK(f.node("prefs.isolation.set")->valueText() == "In a shared bridge process");
    CHECK_FALSE(prefs.setPluginIsolation(model::Isolation::Inherit));
    CHECK(f.app->pluginIsolation() == model::Isolation::SharedBridge);
    // The render folder: absolute only.
    CHECK_FALSE(prefs.setRenderDirectory("relative/renders"));
    REQUIRE(prefs.setRenderDirectory(f.dir / "my renders"));
    CHECK(f.app->rendersDir() == f.dir / "my renders");
    auto saved = gui::Preferences::load(file);
    REQUIRE(saved);
    CHECK(saved->defaultMonitor == model::Monitor::Off);
    CHECK(saved->midiOutputs.at("Synth").find("clock")->asBool());
    CHECK(engine::midiPortSettings(saved->midiOutputs, "Synth").offsetMs == -2.5);
    CHECK(saved->audioThreads == 4);
    CHECK_FALSE(saved->copyImports);
    CHECK(saved->pluginIsolation == model::Isolation::SharedBridge);
    CHECK(saved->renderDirectory == f.dir / "my renders");
    // A failed save changes neither the shown nor the effective setting.
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    CHECK_FALSE(prefs.setAudioThreads(8));
    CHECK(f.app->audioThreadLimit() == 4);
    CHECK(f.node("prefs.threads.set")->valueText() == "Limit: 4");
    REQUIRE(f.root->click("prefs.close"));
    // A loaded file applies to the app when the dialog is made.
    f.app->setRendersDir({});
    gui::PreferencesDialog again(*f.root, *f.app, mouse, *saved, {});
    CHECK(f.app->rendersDir() == f.dir / "my renders");
}

TEST_CASE("preferences: canonical round trip, older schemas and rejected values") {
    Fixture f;
    REQUIRE(f.ok);
    const auto file = f.dir / "preferences.json";
    // Every setting set: saving what was loaded writes the same bytes.
    gui::Preferences values;
    values.defaultMonitor = model::Monitor::Auto;
    values.audioThreads = 3;
    values.lockMemory = true;
    values.preventDeepIdle = true;
    values.renderDirectory = f.dir / "renders";
    values.copyImports = false;
    values.pluginIsolation = model::Isolation::Dedicated;
    values.renderOpenFolder = true;
    values.audio.backend = "portaudio";
    values.audio.hostApi = "Core Audio";
    values.audio.outputDevice = "Studio Out";
    values.audio.inputDevice = "Studio In";
    values.audio.record = false;
    values.audio.rate = 96000;
    values.audio.block = 128;
    values.audio.forceGraph = true;
    values.sync.source = "mtc";
    values.sync.midiInput = "Interface MIDI 1";
    values.sync.offsetMs = 12.5;
    values.sync.onLoss = "freewheel";
    values.sync.ltcOutput = true;
    values.sync.ltcOutputChannel = 4;
    values.rememberedLoads = {{"clap", "com.acme.canary", "0123abcd", "bridge", "Canary Verb",
                               "Acme", "1.2.0", 1763400000000},
                              {"vst3", "com.acme.other", "ff00", "disable", "", "", "", 5}};
    REQUIRE(values.save(file));
    auto loaded = gui::Preferences::load(file);
    REQUIRE(loaded);
    CHECK(loaded->defaultMonitor == model::Monitor::Auto);
    CHECK(loaded->audioThreads == 3);
    CHECK(loaded->lockMemory);
    CHECK(loaded->preventDeepIdle);
    CHECK(loaded->renderDirectory == f.dir / "renders");
    CHECK_FALSE(loaded->copyImports);
    CHECK(loaded->pluginIsolation == model::Isolation::Dedicated);
    CHECK(loaded->renderOpenFolder);
    CHECK(loaded->audio == values.audio);
    CHECK(loaded->sync == values.sync);
    CHECK(loaded->rememberedLoads == values.rememberedLoads);  // 1.10, topic 7d
    CHECK(loaded->midiOutputs.empty());  // the engine writes these records, not the GUI
    const auto bytes = readFile(file).value();
    CHECK(std::string(bytes.begin(), bytes.end()).find("supple.preferences/1.10") !=
          std::string::npos);
    REQUIRE(loaded->save(file));
    CHECK(readFile(file).value() == bytes);
    // Older schemas load, with defaults for what they lack.
    for (const char* version :
         {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "1.10"}) {
        REQUIRE(writeFileAtomic(
            file, std::string("{\"schema\":\"supple.preferences/") + version + "\"}"));
        auto old = gui::Preferences::load(file);
        REQUIRE(old);
        CHECK(old->audioThreads == 0);
        CHECK_FALSE(old->lockMemory);
        CHECK_FALSE(old->preventDeepIdle);
        CHECK(old->renderDirectory.empty());
        CHECK(old->copyImports);
        CHECK(old->pluginIsolation == model::Isolation::InProcess);
        CHECK_FALSE(old->renderOpenFolder);
        CHECK(old->audio == gui::AudioDeviceSettings{});
        CHECK(old->sync == engine::SyncSettings{});
        CHECK(old->rememberedLoads.empty());
        CHECK(old->midiOutputs.empty());
    }
    // A 1.8 clock record on disk (the engine's midi.outputs.set) loads, and a
    // GUI save keeps it as written.
    const std::string records =
        R"({"Interface MIDI 1":{"clock":true,"offset_ms":-3.5,"spp":true,"transport":true}})";
    REQUIRE(writeFileAtomic(
        file, R"({"schema":"supple.preferences/1.8","midi_outputs":)" + records + "}"));
    auto clocked = gui::Preferences::load(file);
    REQUIRE(clocked);
    const auto clock = engine::midiPortSettings(clocked->midiOutputs, "Interface MIDI 1");
    CHECK((clock.clock && clock.transport && clock.spp && !clock.mtc));
    CHECK(clock.offsetMs == -3.5);
    REQUIRE(clocked->save(file));
    const auto kept = readFile(file).value();
    auto keptDoc = json::parse(std::string(kept.begin(), kept.end())).value();
    REQUIRE(keptDoc.find("midi_outputs"));
    CHECK(json::writeCanonical(*keptDoc.find("midi_outputs")) ==
          json::writeCanonical(json::parse(records).value()));
    // A wrong value names its field.
    for (auto [field, bad] : {std::pair{"audio_threads", "65"},
                              {"audio_threads", "1.5"},
                              {"lock_memory", "1"},
                              {"prevent_deep_idle", "\"true\""},
                              {"render_directory", "\"relative\""},
                              {"copy_imported_media", "1"},
                              {"plugin_isolation", "\"inherit\""},
                              {"plugin_isolation", "1"},
                              {"render_open_folder", "\"no\""}}) {
        REQUIRE(writeFileAtomic(file, std::string(R"({"schema":"supple.preferences/1.6",")") +
                                          field + "\":" + bad + "}"));
        auto rejected = gui::Preferences::load(file);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().path == std::string("/") + field);
        CHECK_FALSE(rejected.error().hint.empty());
    }
    // A remembered choice that cannot be read names its entry.
    for (const char* bad : {R"([{"format":"clap","id":"a","build_id":"b","mode":"maybe"}])",
                            R"([{"format":"clap","id":"a","mode":"bridge"}])", R"({"a":1})"}) {
        REQUIRE(writeFileAtomic(
            file,
            std::string(R"({"schema":"supple.preferences/1.10","plugin_overrides":)") + bad + "}"));
        auto rejected = gui::Preferences::load(file);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().path.rfind("/plugin_overrides", 0) == 0);
        CHECK_FALSE(rejected.error().hint.empty());
    }
    // So does a wrong audio device value, under /audio.
    for (auto [field, bad] : {std::pair{"backend", "\"alsa\""},
                              {"host_api", "1"},
                              {"record", "\"yes\""},
                              {"rate", "-1"},
                              {"rate", "\"48000\""},
                              {"block", "8"},
                              {"block", "256.5"},
                              {"force_graph", "0"}}) {
        REQUIRE(
            writeFileAtomic(file, std::string(R"({"schema":"supple.preferences/1.5","audio":{")") +
                                      field + "\":" + bad + "}}"));
        auto rejected = gui::Preferences::load(file);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "preferences.audio");
        CHECK(rejected.error().path == std::string("/audio/") + field);
        CHECK_FALSE(rejected.error().hint.empty());
    }
    // And a wrong sync value, under /sync (1.7).
    for (auto [field, bad] : {std::pair{"source", "\"smpte\""},
                              {"offset_ms", "\"5\""},
                              {"ltc_input", "0"},
                              {"freewheel_frames", "1000"}}) {
        REQUIRE(
            writeFileAtomic(file, std::string(R"({"schema":"supple.preferences/1.7","sync":{")") +
                                      field + "\":" + bad + "}}"));
        auto rejected = gui::Preferences::load(file);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "preferences.sync");
        CHECK(rejected.error().path == std::string("/sync/") + field);
        CHECK_FALSE(rejected.error().hint.empty());
    }
    // A wrong MIDI output record is skipped, not fatal: the engine reports it
    // (midi.outputs.get warnings), and its valid peers load (1.8).
    REQUIRE(writeFileAtomic(file, R"({"schema":"supple.preferences/1.8","midi_outputs":)"
                                  R"({"A":{"mtc":1},"B":{"clock":true}}})"));
    auto skipped = gui::Preferences::load(file);
    REQUIRE(skipped);
    CHECK_FALSE(skipped->midiOutputs.contains("A"));
    CHECK(engine::midiPortSettings(skipped->midiOutputs, "B").clock);
}

namespace {
std::vector<engine::AudioDevice> fakeDevices() {
    // Two host APIs in PortAudio's Windows order: ASIO is listed (and would be
    // its default) first, but an unset host API means WASAPI.
    return {{"ASIO", "Interface ASIO", 18, 20, 48000, true, true},
            {"Windows WASAPI", "Speakers", 0, 2, 48000, false, true},
            {"Windows WASAPI", "Microphone", 1, 0, 44100, true, false},
            {"Windows WASAPI", "Interface", 8, 10, 96000, false, false}};
}
}  // namespace

TEST_CASE("audio device: settings become engine.start arguments") {
    const auto devices = fakeDevices();
    auto args = [&](const gui::AudioDeviceSettings& s) {
        return json::writeCanonical(gui::engineStartArgs(s, devices));
    };
    auto canon = [](std::string_view text) {
        return json::writeCanonical(json::parse(text).value());
    };
    gui::AudioDeviceSettings s;
    s.backend = "portaudio";
    // Defaults: WASAPI, its default devices, the output's own rate.
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":256,"host_api":"Windows WASAPI",)"
                           R"("inputs":1,"rate":48000.0})"));
    // Named devices; every input channel (up to 32) and output (up to 64); a
    // chosen rate wins.
    s.outputDevice = "Interface";
    s.inputDevice = "Interface";
    s.rate = 44100;
    s.block = 64;
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":64,"host_api":"Windows WASAPI",)"
                           R"("output_device":"Interface","input_device":"Interface","inputs":8,)"
                           R"("outputs":10,"rate":44100.0})"));
    // Playback only opens no input.
    s.record = false;
    s.rate = 0;
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":64,"host_api":"Windows WASAPI",)"
                           R"("output_device":"Interface","outputs":10,"rate":96000.0})"));
    // ASIO: one device both ways, whatever input was chosen before.
    s = {};
    s.backend = "portaudio";
    s.hostApi = "ASIO";
    s.inputDevice = "Microphone";
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":256,"host_api":"ASIO",)"
                           R"("input_device":"Interface ASIO","inputs":18,"outputs":20,)"
                           R"("rate":48000.0})"));
    // A device that is not listed now is still asked for: engine.start names it.
    s = {};
    s.backend = "portaudio";
    s.inputDevice = "Unplugged";
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":256,"host_api":"Windows WASAPI",)"
                           R"("input_device":"Unplugged","inputs":2,"rate":48000.0})"));
    // JACK: a rate is a request; force_graph only when chosen. Dummy: the block.
    s = {};
    s.backend = "jack";
    CHECK(args(s) == canon(R"({"backend":"jack","block":256})"));
    s.rate = 96000;
    s.forceGraph = true;
    CHECK(args(s) == canon(R"({"backend":"jack","block":256,"force_graph":true,"rate":96000.0})"));
    s = {};
    s.backend = "dummy";
    s.block = 512;
    CHECK(args(s) == canon(R"({"backend":"dummy","block":512})"));
    // Without WASAPI (macOS), the first listed host API.
    const std::vector<engine::AudioDevice> mac{{"Core Audio", "Built-in", 1, 2, 44100, true, true}};
    s = {};
    s.backend = "portaudio";
    CHECK(json::writeCanonical(gui::engineStartArgs(s, mac)) ==
          canon(R"({"backend":"portaudio","block":256,"host_api":"Core Audio",)"
                R"("inputs":1,"rate":44100.0})"));
    // The platform's default backend.
    CHECK(gui::effectiveBackend({}) == gui::defaultAudioBackend());
}

TEST_CASE("audio device: the dialog saves each choice and restarts the engine with it") {
    Fixture f;
    if (!f.ok) return;
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    const auto file = f.dir / "config" / "preferences.json";
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, gui::Preferences{}, file);
    prefs.onLinux = true;  // the rows every platform can test
    gui::AudioDeviceSettings initial;
    initial.backend = "portaudio";
    gui::AudioDeviceDialog audio(
        *f.root, *f.dispatcher, *f.app, initial,
        [&](const gui::AudioDeviceSettings& s) { return prefs.setAudioDevice(s); });
    std::vector<std::string> starts;
    int stops = 0;
    bool running = false, refuse = false;
    audio.enumerate = [] { return Result<std::vector<engine::AudioDevice>>(fakeDevices()); };
    audio.startEngine = [&](const json::Value& args) -> Result<json::Value> {
        starts.push_back(json::writeCompact(args));
        if (refuse && args.find("output_device"))
            return Error("backend.device", "device was not found", "/output_device",
                         "choose an exact name from engine.devices");
        // A device-free engine stands in for the device.
        json::Value dummy = json::Value::object();
        dummy["backend"] = "dummy";
        dummy["block"] = args.find("block")->asInt();
        auto st =
            f.dispatcher->execute({"engine.start", dummy, {cmd::Origin::Type::Test, "t", {}}});
        running = static_cast<bool>(st);
        return st;
    };
    audio.stopEngine = [&] {
        ++stops;
        (void)f.dispatcher->execute(
            {"engine.stop", json::Value::object(), {cmd::Origin::Type::Test, "t", {}}});
        running = false;
    };
    std::vector<std::string> errors;
    audio.onError = [&](const std::string& e) { errors.push_back(e); };

    REQUIRE(audio.start(true));
    REQUIRE(starts.size() == 1);
    CHECK(running);
    // Preferences shows the device and opens the dialog from its row.
    prefs.onAudioDevice = [&] { audio.open(); };
    prefs.audioSummary = [&] { return audio.summary(); };
    prefs.open();
    f.root->frame(false);
    CHECK(f.node("prefs.audio.open")->valueText().find("Native device") != std::string::npos);
    REQUIRE(f.root->click("prefs.audio.open"));
    f.root->frame(false);
    REQUIRE(audio.isOpen());
    CHECK_FALSE(prefs.isOpen());
    CHECK(f.node("audio.status.0")->valueText().find("Running") == 0);
    // Choose an output from the menu: saved, and the engine restarts with it.
    REQUIRE(f.root->click("audio.output.set"));
    auto* menu = dynamic_cast<ui::List*>(f.root->find("audio.output.menu"));
    REQUIRE(menu);
    auto activate = menu->onActivate;
    activate(2);  // System default, Speakers, Interface
    f.root->frame(false);
    CHECK(audio.settings().outputDevice == "Interface");
    REQUIRE(starts.size() == 2);
    CHECK(stops == 1);
    CHECK(starts.back().find(R"("output_device":"Interface")") != std::string::npos);
    CHECK(gui::Preferences::load(file)->audio.outputDevice == "Interface");
    REQUIRE(audio.isOpen());  // rebuilt with the new choice
    CHECK(f.node("audio.output.set")->valueText() == "Interface");
    // Rate and buffer size from their menus.
    REQUIRE(f.root->click("audio.rate.set"));
    menu = dynamic_cast<ui::List*>(f.root->find("audio.rate.menu"));
    REQUIRE(menu);
    activate = menu->onActivate;
    activate(4);  // automatic, 44100, 48000, 88200, 96000
    f.root->frame(false);
    CHECK(audio.settings().rate == 96000);
    REQUIRE(f.root->click("audio.block.set"));
    menu = dynamic_cast<ui::List*>(f.root->find("audio.block.menu"));
    REQUIRE(menu);
    activate = menu->onActivate;
    activate(1);  // 64 frames
    f.root->frame(false);
    CHECK(audio.settings().block == 64);
    CHECK(starts.back().find(R"("block":64)") != std::string::npos);
    CHECK(starts.back().find(R"("rate":96000.0)") != std::string::npos);
    auto saved = gui::Preferences::load(file);
    REQUIRE(saved);
    CHECK(saved->audio == audio.settings());
    // A refused device leaves the engine stopped and says why; the choice is kept.
    refuse = true;
    REQUIRE_FALSE(audio.restart());
    CHECK_FALSE(running);
    CHECK(f.node("audio.status.0")->valueText().find("Not running: device was not found") == 0);
    // At app start a refused choice falls back to the defaults, and says so.
    REQUIRE(audio.start(true));
    CHECK(running);
    CHECK(starts.back().find("output_device") == std::string::npos);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("using the default device") != std::string::npos);
    CHECK(audio.settings().outputDevice == "Interface");
    refuse = false;
    // Stop audio: later changes are saved without starting the engine.
    f.root->frame(false);
    REQUIRE(f.root->click("audio.stop"));
    CHECK_FALSE(running);
    const auto count = starts.size();
    gui::AudioDeviceSettings dummy = audio.settings();
    dummy.backend = "dummy";
    REQUIRE(audio.apply(dummy));
    CHECK(starts.size() == count);
    CHECK(gui::Preferences::load(file)->audio.backend == "dummy");
    // An invalid value is refused before anything is saved.
    gui::AudioDeviceSettings bad = audio.settings();
    bad.block = 8;
    auto refused = audio.apply(bad);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().path == "/audio/block");
    CHECK(gui::Preferences::load(file)->audio.block == 64);
    REQUIRE(f.root->click("audio.close"));
}

TEST_CASE("audit3: hiding a mixer ends its captured fader gesture") {
    Fixture f;
    REQUIRE(f.ok);
    auto t = f.addTrack("channel", "hidden drag");
    f.refresh();
    auto p = f.centre(f.node("mixer.fader." + t));
    auto before = f.track(t).volumeDb;
    auto steps = f.steps();
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Move, {p.x, p.y - 10});
    REQUIRE(f.track(t).volumeDb != before);
    before = f.track(t).volumeDb;
    f.mixer->setVisible(false);
    CHECK_FALSE(dynamic_cast<ui::Fader*>(f.node("mixer.fader." + t))->dragging());
    f.pointer(ui::PointerEvent::Type::Move, {p.x, p.y - 30});
    CHECK(f.track(t).volumeDb == before);
    f.pointer(ui::PointerEvent::Type::Up, {p.x, p.y - 30});
    CHECK(f.steps() == steps + 1);
    f.mixer->setVisible(true);
    f.refresh();
    f.pointer(ui::PointerEvent::Type::Move, {p.x, p.y - 40});
    CHECK(f.track(t).volumeDb == before);
}

TEST_CASE("audit3: mixer resnapshots identical IDs and versions in a replacement project") {
    Fixture f;
    REQUIRE(f.ok);
    auto track = f.addTrack("channel", "old");
    f.exec("track.set", R"({"track":")" + track + R"(","volume_db":-6})");
    REQUIRE(f.app->save(f.dir / "old.json"));
    REQUIRE(f.app->newProject({}));
    REQUIRE(f.addTrack("channel", "new") == track);
    f.exec("track.set", R"({"track":")" + track + R"(","volume_db":-20})");
    REQUIRE(f.app->save(f.dir / "new.json"));
    REQUIRE(f.app->open(f.dir / "old.json"));
    f.refresh();
    auto version = f.app->head().version;
    const auto name = f.centre(f.node("mixer.name." + track));
    f.pointer(ui::PointerEvent::Type::Down, name, 2);
    f.pointer(ui::PointerEvent::Type::Up, name);
    f.root->text("old pending rename");
    REQUIRE(f.app->open(f.dir / "new.json"));
    REQUIRE(f.app->head().version == version);
    f.mixer->setVisible(false);
    CHECK(f.track(track).name == "new");
    f.mixer->setVisible(true);
    f.refresh();
    auto* fader = dynamic_cast<ui::Fader*>(f.node("mixer.fader." + track));
    REQUIRE(fader);
    CHECK(fader->value() == -20);
}

TEST_CASE(
    "preferences: memory toggle shows intent and effective failure, and follows save-error "
    "pattern") {
    auto ops = std::make_shared<engine::RuntimeProtectionOps>();
    int lockError = EPERM, locks = 0, unlocks = 0;
    ops->lockMemory = [&](int) {
        ++locks;
        errno = lockError;
        return lockError ? -1 : 0;
    };
    ops->unlockMemory = [&] {
        ++unlocks;
        return 0;
    };
    ops->memoryLimit = [](engine::MemoryLimit* limit) {
        limit->rlim_cur = 0;
        return 0;
    };
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    Fixture f(ops);
    REQUIRE(f.ok);
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    const auto file = f.dir / "preferences.json";
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, {}, file);
    prefs.onLinux = true;  // the rows every platform can test
    prefs.open();
    f.root->frame(false);
    REQUIRE(f.root->click("prefs.advanced.toggle"));  // the low-level settings
    f.root->frame(false);
    const float offHeight = f.node("prefs")->bounds().h;
    CHECK(offHeight < 760);  // no space reserved for empty notes (eight would add ~190)
    CHECK_FALSE(f.node("prefs.memory.note.0")->visible);
    CHECK_FALSE(f.node("prefs.latency.note.0")->visible);
    for (const char* id : {"prefs.memory.set", "prefs.latency.set"}) {
        CHECK(f.node(id)->bounds().x == f.node("prefs.threads.set")->bounds().x);
        CHECK(f.node(id)->bounds().w == f.node("prefs.threads.set")->bounds().w);
    }
    REQUIRE(f.root->click("prefs.memory.set"));
    CHECK(prefs.preferences().lockMemory);
    CHECK(f.node("prefs.memory.set")->valueText() == "On");
    CHECK(f.node("prefs.memory.note.0")->valueText() == "Applies when the engine starts");
    CHECK(locks == 0);
    f.root->frame(false);
    const float pendingHeight = f.node("prefs")->bounds().h;
    CHECK(pendingHeight > offHeight);
    CHECK(f.node("prefs.memory.note.0")->visible);
    CHECK_FALSE(f.node("prefs.memory.note.1")->visible);
    REQUIRE(f.app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
    prefs.refresh();
    CHECK(f.node("prefs.memory.note.0")->valueText().find("errno") != std::string::npos);
    CHECK(gui::Preferences::load(file)->lockMemory);
    f.root->frame(false);
    CHECK(f.node("prefs")->bounds().h > pendingHeight);
    REQUIRE(prefs.setMemoryLock(false));
    f.root->frame(false);
    CHECK_FALSE(f.node("prefs.memory.note.0")->visible);
    CHECK(f.node("prefs")->bounds().h == offHeight);
    lockError = 0;
    REQUIRE(prefs.setMemoryLock(true));
    CHECK(f.node("prefs.memory.note.0")->valueText() == "Locked");
    f.root->frame(false);
    CHECK(f.node("prefs")->bounds().h == pendingHeight);
    f.app->stopEngine();
    prefs.refresh();
    CHECK(f.node("prefs.memory.note.0")->valueText() == "Applies when the engine starts");
    CHECK(unlocks == 1);
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    CHECK_FALSE(prefs.setMemoryLock(false));
    CHECK(prefs.preferences().lockMemory);
    CHECK(f.app->memoryLockStatus().requested);
}

TEST_CASE(
    "preferences: CPU idle toggle persists failures and refreshes effective lifecycle status") {
    auto ops = std::make_shared<engine::RuntimeProtectionOps>();
    int openError = EACCES, opens = 0, closes = 0;
    ops->openLatency = [&](const char*, int) {
        ++opens;
        errno = openError;
        return openError ? -1 : 73;
    };
    ops->writeLatency = [](int, const void*, std::size_t size) -> engine::ProtectionWriteSize {
        return static_cast<engine::ProtectionWriteSize>(size);
    };
    ops->closeLatency = [&](int) {
        ++closes;
        return 0;
    };
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    Fixture f(ops);
    REQUIRE(f.ok);
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    const auto file = f.dir / "preferences.json";
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, {}, file);
    prefs.onLinux = true;  // the rows every platform can test
    prefs.open();
    f.root->frame(false);
    REQUIRE(f.root->click("prefs.advanced.toggle"));  // the low-level settings
    f.root->frame(false);
    const float offHeight = f.node("prefs")->bounds().h;
    CHECK_FALSE(f.node("prefs.latency.note.0")->visible);
    REQUIRE(f.root->click("prefs.latency.set"));
    CHECK(prefs.preferences().preventDeepIdle);
    CHECK(f.node("prefs.latency.note.0")->valueText() == "Applies when the engine starts");
    CHECK(opens == 0);
    REQUIRE(f.app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
    prefs.refresh();
    CHECK(f.node("prefs.latency.note.0")->valueText().find("errno 13") != std::string::npos);
    CHECK(gui::Preferences::load(file)->preventDeepIdle);
    REQUIRE(prefs.setPreventDeepIdle(false));
    f.root->frame(false);
    CHECK_FALSE(f.node("prefs.latency.note.0")->visible);
    CHECK(f.node("prefs")->bounds().h == offHeight);
    CHECK_FALSE(prefs.setPreventDeepIdle(true));
    CHECK(gui::Preferences::load(file)->preventDeepIdle);
    CHECK(prefs.preferences().preventDeepIdle);
    CHECK(f.node("prefs.latency.set")->valueText() == "On");
    REQUIRE(prefs.setPreventDeepIdle(false));
    openError = 0;
    REQUIRE(prefs.setPreventDeepIdle(true));
    CHECK(f.node("prefs.latency.note.0")->valueText() == "Requested: 20 µs");
    f.app->stopEngine();
    prefs.refresh();
    CHECK(f.node("prefs.latency.note.0")->valueText() == "Applies when the engine starts");
    CHECK(closes == 1);
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    CHECK_FALSE(prefs.setPreventDeepIdle(false));
    CHECK(prefs.preferences().preventDeepIdle);
    CHECK(f.app->cpuIdleLatencyStatus().requested);
}

TEST_CASE("mixer: group faders deduplicate selected peers, Alt overrides and VCA stays separate") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "Kick"), b = f.addTrack("channel", "Snare");
    f.exec("track.set", R"({"track":"tr_2","volume_db":-6})");
    f.exec("group.add", R"({"name":"Drums","color":"teal","members":["tr_1","tr_2"]})");
    f.exec("vca.add", R"({"group":"gr_1"})");
    f.ui.selectedTracks = {a, b};
    f.refresh();
    REQUIRE(f.root->find("mixer.vca.gr_1"));
    REQUIRE(f.root->find("mixer.group.tr_1.gr_1"));
    auto rect = f.strip(a).fader->bounds();
    ui::Point cap{rect.x + rect.w / 2,
                  rect.bottom() - 8 -
                      static_cast<float>(ui::faderPosition(f.track(a).volumeDb)) * (rect.h - 16)};
    auto steps = f.steps();
    f.drag(cap, {cap.x, cap.y - 18});
    CHECK(f.track(b).volumeDb == doctest::Approx(-6 + f.track(a).volumeDb));
    CHECK(f.steps() == steps + 1);
    f.refresh();
    const auto other = f.track(b).volumeDb;
    rect = f.strip(a).fader->bounds();
    cap = {rect.x + rect.w / 2,
           rect.bottom() - 8 -
               static_cast<float>(ui::faderPosition(f.track(a).volumeDb)) * (rect.h - 16)};
    f.pointer(ui::PointerEvent::Type::Down, cap, 1, ui::kAlt);
    f.pointer(ui::PointerEvent::Type::Move, {cap.x, cap.y + 12}, 1, ui::kAlt);
    f.pointer(ui::PointerEvent::Type::Up, {cap.x, cap.y + 12}, 1, ui::kAlt);
    CHECK(f.track(b).volumeDb == other);
    f.refresh();
    const auto member = f.track(a).volumeDb;
    REQUIRE(f.root->click("mixer.vca.mute.gr_1"));
    CHECK_FALSE(f.track(a).mute);
    CHECK(f.track(a).volumeDb == member);
    CHECK(f.app->head().groups.find({model::Kind::Group, 1})->vca->mute);
    f.exec("group.set", R"({"group":"gr_1","links":["volume","selection"]})");
    f.ui.selectedTracks.clear();
    f.refresh();
    const auto at = f.empty(a);
    f.pointer(ui::PointerEvent::Type::Down, at);
    f.pointer(ui::PointerEvent::Type::Up, at);
    CHECK(f.ui.selectedTracks == std::vector<std::string>{a, b});
    f.pointer(ui::PointerEvent::Type::Down, at, 1, ui::kCtrl);
    f.pointer(ui::PointerEvent::Type::Up, at, 1, ui::kCtrl);
    CHECK(f.ui.selectedTracks.empty());
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a value reset preserves offsets in each selected group") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A"), b = f.addTrack("channel", "B"),
               c = f.addTrack("channel", "C"), d = f.addTrack("channel", "D");
    f.exec("track.set", R"({"track":"tr_1","volume_db":-6})");
    f.exec("track.set", R"({"track":"tr_2","volume_db":-9})");
    f.exec("track.set", R"({"track":"tr_3","volume_db":-12})");
    f.exec("track.set", R"({"track":"tr_4","volume_db":-15})");
    f.exec("group.add", R"({"name":"AB","members":["tr_1","tr_2"]})");
    f.exec("group.add", R"({"name":"CD","members":["tr_3","tr_4"]})");
    f.ui.selectedTracks = {c, a};
    f.refresh();
    const auto before = f.steps();
    f.strip(a).fader->onChange(0, ui::Gesture::Set);
    CHECK(f.track(a).volumeDb == 0);
    CHECK(f.track(b).volumeDb == -3);
    CHECK(f.track(c).volumeDb == 0);
    CHECK(f.track(d).volumeDb == -3);
    CHECK(f.steps() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: freeze and unfreeze use the track menu and keep the name visible") {
    Fixture f;
    if (!f.ok) return;
    const auto id = f.addTrack("channel", "Keys");
    f.exec("item.add", R"({"track":"tr_1","notation":"1: r:w"})");
    f.refresh();
    auto freezeRow = [&] {
        const auto at = f.empty(id);
        f.pointer(ui::PointerEvent::Type::Down, at, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, at, 1, 0, 3);
        const auto* menu = f.node("track.menu." + id);
        const ui::Point pick{menu->bounds().x + 40, menu->bounds().y + 3 * 26 + 13};
        f.pointer(ui::PointerEvent::Type::Move, pick);
        f.pointer(ui::PointerEvent::Type::Down, pick);
        f.pointer(ui::PointerEvent::Type::Up, pick);
    };
    const auto before = f.steps();
    freezeRow();
    f.exec("job.wait", R"({"job":"jb_1","timeout_s":30})");
    f.refresh();
    REQUIRE(f.track(id).freeze);
    CHECK(f.steps() == before + 1);
    CHECK(f.root->find("mixer.frozen." + id));
    CHECK(f.strip(id).name->text() == "Keys");
    CHECK_FALSE(f.strip(id).addFx->enabled);
    freezeRow();
    f.refresh();
    CHECK_FALSE(f.track(id).freeze);
    CHECK_FALSE(f.root->find("mixer.frozen." + id));
    CHECK(f.strip(id).addFx->enabled);
    CHECK(f.steps() == before + 2);
    CHECK(f.errors.empty());
}

TEST_CASE(
    "mixer: a right-click on a fader or button opens its parameter menu, elsewhere the track "
    "menu") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    const auto open = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Down, p, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 1, 0, 3);
    };
    const auto labels = [](const ui::Node& menu) {
        std::vector<std::string> out;
        for (const auto& item : ui::menuItems(menu)) out.push_back(item.label);
        return out;
    };
    // MIDI learn first; a lane where the parameter can be automated; Reset
    // where the control has a default.
    using Rows = std::vector<std::string>;
    for (auto [node, rows] :
         {std::pair{static_cast<ui::Node*>(f.strip(a).fader),
                    Rows{"MIDI learn: Volume…", "Modulate: Volume…", "Show lane: Volume",
                         "Reset to 0 dB"}},
          {f.strip(a).mute, Rows{"MIDI learn: Mute…", "Modulate: Mute…", "Show lane: Mute"}},
          {f.strip(a).solo, Rows{"MIDI learn: Solo…"}},
          {f.strip(a).arm, Rows{"MIDI learn: Record arm…"}}}) {
        REQUIRE(node);
        open(f.centre(node));
        ui::Node* menu = f.root->find("strip.param.menu");
        REQUIRE(menu);
        CHECK(labels(*menu) == rows);
        if (rows.size() == 4) f.snapshot("param-menu-mixer-fader.png");
        f.root->closePopup();
    }
    open(f.empty(a));
    CHECK_FALSE(f.root->find("strip.param.menu"));
    CHECK(f.root->popupOpen());  // the track's menu
}

TEST_CASE("mixer: a pan's parameter menu sets its mode and law; its bars follow (ADR-0066)") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    const auto open = [&] {
        const ui::Point p = f.centre(f.strip(a).pan);
        f.pointer(ui::PointerEvent::Type::Down, p, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 1, 0, 3);
        ui::Node* menu = f.root->find("strip.param.menu");
        REQUIRE(menu);
        return menu;
    };
    const auto labels = [](const ui::Node& menu) {
        std::vector<std::string> out;
        for (const auto& item : ui::menuItems(menu)) out.push_back(item.label);
        return out;
    };
    const auto row = [](ui::Node* menu, const std::string& label) {
        const auto items = ui::menuItems(*menu);
        const auto it = std::find_if(items.begin(), items.end(),
                                     [&](const auto& i) { return i.label == label; });
        REQUIRE(it != items.end());
        return *it;
    };
    const auto pick = [&](ui::Node* menu, const std::string& label) {
        auto run = row(menu, label).run;
        f.root->closePopup();
        run();
        f.refresh();
    };
    // The pan's own rows (learn, lane, reset), then its modes, then its law.
    ui::Node* menu = open();
    CHECK(labels(*menu) ==
          std::vector<std::string>{"MIDI learn: Pan…", "MIDI learn: Gain compensation…",
                                   "Modulate: Pan…", "Show lane: Pan", "Reset to centre", "",
                                   "Pan mode: pan", "Pan mode: dual pan", "Pan mode: pan and width",
                                   "", "Pan law: project (0 dB)", "Pan law: 0 dB",
                                   "Pan law: −2.5 dB", "Pan law: −3 dB", "Pan law: −4.5 dB",
                                   "Pan law: −6 dB", "Gain compensation (boost pans)"});
    CHECK(row(menu, "Pan mode: pan").checked);
    CHECK(row(menu, "Pan law: project (0 dB)").checked);
    CHECK_FALSE(row(menu, "Gain compensation (boost pans)").checked);
    f.snapshot("pan-menu-pan.png");
    pick(menu, "Pan mode: dual pan");
    CHECK(f.track(a).panMode == model::PanMode::Dual);
    CHECK(f.strip(a).pan->split() == ui::PanControl::Split::Dual);
    CHECK(f.strip(a).pan->value() == -100);
    CHECK(f.strip(a).pan->second() == 100);
    menu = open();
    auto rows = labels(*menu);
    REQUIRE(rows.size() >= 8);
    CHECK(std::vector<std::string>(rows.begin(), rows.begin() + 8) ==
          std::vector<std::string>{"MIDI learn: Pan left…", "MIDI learn: Pan right…",
                                   "MIDI learn: Gain compensation…", "Modulate: Pan left…",
                                   "Modulate: Pan right…", "Show lane: Pan left",
                                   "Show lane: Pan right", "Reset to full left and right"});
    // A dual pan's bars take no modulation (ADR-0096 targets pan and width).
    CHECK_FALSE(row(menu, "Modulate: Pan left…").enabled);
    CHECK_FALSE(row(menu, "Modulate: Pan right…").enabled);
    f.snapshot("pan-menu-dual.png");
    f.root->closePopup();
    pick(open(), "Pan law: −3 dB");  // -3 dB of its own
    REQUIRE(f.track(a).panLaw);
    CHECK(f.track(a).panLaw->db == -3);
    CHECK_FALSE(f.track(a).panLaw->boost);
    pick(open(), "Gain compensation (boost pans)");  // boost its own law
    CHECK(f.track(a).panLaw->boost);
    menu = open();
    CHECK(row(menu, "Gain compensation (boost pans)").checked);
    CHECK_FALSE(row(menu, "Pan law: project (0 dB)").checked);
    pick(menu, "Pan law: project (0 dB)");  // back to the project's
    CHECK_FALSE(f.track(a).panLaw);
    pick(open(), "Pan mode: pan and width");
    CHECK(f.strip(a).pan->split() == ui::PanControl::Split::Width);
    CHECK(f.strip(a).pan->second() == 100);
    menu = open();
    rows = labels(*menu);
    REQUIRE(rows.size() >= 8);
    CHECK(std::vector<std::string>(rows.begin(), rows.begin() + 8) ==
          std::vector<std::string>{"MIDI learn: Pan…", "MIDI learn: Width…",
                                   "MIDI learn: Gain compensation…", "Modulate: Pan…",
                                   "Modulate: Width…", "Show lane: Pan", "Show lane: Width",
                                   "Reset to centre, full width"});
    CHECK(row(menu, "Modulate: Pan…").enabled);
    CHECK(row(menu, "Modulate: Width…").enabled);
    f.snapshot("pan-menu-width.png");
    f.root->closePopup();
    // The right bar is the width: a drag left narrows the strip.
    const ui::Rect b = f.strip(a).pan->bounds();
    const ui::Point right{b.x + b.w * 0.9f, b.y + b.h / 2};
    f.pointer(ui::PointerEvent::Type::Down, right);
    f.pointer(ui::PointerEvent::Type::Move, {right.x - b.w * 0.2f, right.y});
    f.pointer(ui::PointerEvent::Type::Up, {right.x - b.w * 0.2f, right.y});
    f.refresh();
    CHECK(f.track(a).width < 100);
    CHECK(f.track(a).pan == 0);
    // Reset puts both bars home in one step.
    const auto before = f.steps();
    pick(open(), "Reset to centre, full width");
    CHECK(f.track(a).width == 100);
    CHECK(f.track(a).pan == 0);
    CHECK(f.steps() == before + 1);
    // A master's pan is a balance: learn, lane and reset, no modes or law.
    const ui::Point master = f.centre(f.strip("tr_0").pan);
    f.pointer(ui::PointerEvent::Type::Down, master, 1, 0, 3);
    f.pointer(ui::PointerEvent::Type::Up, master, 1, 0, 3);
    menu = f.root->find("strip.param.menu");
    REQUIRE(menu);
    CHECK(labels(*menu) == std::vector<std::string>{"MIDI learn: Pan…", "Modulate: Pan…",
                                                    "Show lane: Pan", "Reset to centre"});
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: Show and Hide lane follow whatever holds the lane, a route or the envelope") {
    // Review round 56 F2: a route with lane true keeps the target's lane
    // (gui.md §4.3), so the menu reads and writes the routes' lane too.
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    json::Value made = f.exec(
        "modulate", R"({"target":{"track":")" + a + R"(","param":"volume"},"source":"lfo"})");
    const auto route = *model::Id::parse(made["modulation"].asString());
    f.refresh();
    const auto lane = [&] { return f.app->head().modulations.find(route)->lane; };
    const auto envelopes = [&] { return f.app->head().envelopes.size(); };
    const auto pick = [&](const std::string& label, bool mute = false) {
        const ui::Point p =
            f.centre(mute ? static_cast<ui::Node*>(f.strip(a).mute) : f.strip(a).fader);
        f.pointer(ui::PointerEvent::Type::Down, p, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 1, 0, 3);
        ui::Node* menu = f.root->find("strip.param.menu");
        REQUIRE(menu);
        const auto items = ui::menuItems(*menu);
        const auto it = std::find_if(items.begin(), items.end(),
                                     [&](const auto& i) { return i.label == label; });
        REQUIRE_MESSAGE(it != items.end(), label);
        auto run = it->run;
        f.root->closePopup();
        const auto before = f.steps();
        run();
        f.refresh();
        CHECK(f.steps() == before + 1);
    };
    // The route holds the lane: Hide clears its lane and adds no envelope.
    REQUIRE(lane());
    REQUIRE(envelopes() == 0);
    pick("Hide lane: Volume");
    CHECK_FALSE(lane());
    CHECK(envelopes() == 0);
    // Show turns the route's lane back on rather than adding a flat envelope.
    pick("Show lane: Volume");
    CHECK(lane());
    CHECK(envelopes() == 0);
    // With an envelope too, Hide clears both in one step, and Show sets both.
    f.exec("env.add",
           R"({"track":")" + a + R"(","param":"volume","points":[{"beat":0,"value":0}]})");
    f.refresh();
    REQUIRE(envelopes() == 1);
    const auto visible = [&] { return f.app->head().envelopes.begin()->value->visible; };
    pick("Hide lane: Volume");
    CHECK_FALSE(lane());
    CHECK_FALSE(visible());
    pick("Show lane: Volume");
    CHECK(lane());
    CHECK(visible());
    // Without a route or an envelope, Show still adds the flat envelope.
    pick("Show lane: Mute", true);
    CHECK(envelopes() == 2);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: Modulate… on a fader opens the sources, and one makes the route (modulate)") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    f.addTrack("buss", "Verb");
    f.refresh();
    const auto open = [&](ui::Node* node) {
        const ui::Point p = f.centre(node);
        f.pointer(ui::PointerEvent::Type::Down, p, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 1, 0, 3);
        ui::Node* menu = f.root->find("strip.param.menu");
        REQUIRE(menu);
        return menu;
    };
    const auto row = [](ui::Node* menu, const std::string& label) {
        const auto items = ui::menuItems(*menu);
        const auto it = std::find_if(items.begin(), items.end(),
                                     [&](const auto& i) { return i.label == label; });
        REQUIRE_MESSAGE(it != items.end(), label);
        return *it;
    };
    const auto labels = [](const ui::Node& menu) {
        std::vector<std::string> out;
        for (const auto& item : ui::menuItems(menu)) out.push_back(item.label);
        return out;
    };
    // Mute is a switch: greyed.
    CHECK_FALSE(row(open(f.strip(a).mute), "Modulate: Mute…").enabled);
    f.root->closePopup();
    // The fader's row opens the sources in its place: the followers are the
    // other channels and busses (a follower cannot hear its own track).
    ui::Node* menu = open(f.strip(a).fader);
    auto modulate = row(menu, "Modulate: Volume…");
    CHECK(modulate.enabled);
    f.root->closePopup();
    modulate.run();
    ui::Node* sources = f.root->find("param.modulate.menu");
    REQUIRE(sources);
    CHECK(labels(*sources) ==
          std::vector<std::string>{"LFO", "Random", "Macro", "", "Follower: B", "Follower: Verb"});
    f.snapshot("param-modulate-menu.png");
    auto lfo = row(sources, "LFO");
    f.root->closePopup();
    const auto before = f.steps();
    lfo.run();
    f.refresh();
    CHECK(f.steps() == before + 1);  // the whole gesture is one undo step
    const model::Session s = f.app->head();
    REQUIRE(s.modulations.size() == 1);
    const model::Modulation& route = *s.modulations.begin()->value;
    CHECK(route.target == model::EnvTarget{*model::Id::parse(a), "volume"});
    CHECK(route.depth == 6);  // modulate's default for a volume
    // A follower names the track it listens to (Verb: A would close a cycle
    // through the Modulators rack that already drives A, which modulate
    // refuses with a message in the status line).
    menu = open(f.strip(b).pan);
    modulate = row(menu, "Modulate: Pan…");
    f.root->closePopup();
    modulate.run();
    sources = f.root->find("param.modulate.menu");
    REQUIRE(sources);
    auto follow = row(sources, "Follower: Verb");
    f.root->closePopup();
    follow.run();
    f.refresh();
    for (const auto& e : f.errors) MESSAGE(e);
    CHECK(f.app->head().modulations.size() == 2);
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a modulated fader and pan bar show their routes; a bar drag sets the depth") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    CHECK_FALSE(f.strip(a).fader->modulation());
    CHECK_FALSE(f.strip(a).pan->modulation(0));
    json::Value made = f.exec(
        "modulate", R"({"target":{"track":")" + a + R"(","param":"volume"},"source":"lfo"})");
    const auto route = *model::Id::parse(made["modulation"].asString());
    f.exec("modulate", R"({"target":{"track":")" + a + R"(","param":"pan"},"source":"lfo"})");
    f.refresh();
    // An LFO is -1..1: ±6 dB on the fader, ±50 % on the pan bar.
    const auto& fader = f.strip(a).fader->modulation();
    REQUIRE(fader);
    CHECK(fader->low == -6);
    CHECK(fader->high == 6);
    CHECK(fader->depth == 6);
    REQUIRE(f.strip(a).pan->modulation(0));
    CHECK(f.strip(a).pan->modulation(0)->high == 50);
    CHECK_FALSE(f.strip(a).pan->modulation(1));  // no width in pan mode
    f.snapshot("mixer-modulated-strip.png");
    // A drag that starts on the fader's bar moves the depth, not the level:
    // one modulation.set per move, one undo step for the drag.
    const ui::Rect zone = f.strip(a).fader->depthZone();
    REQUIRE(zone.w > 0);
    const ui::Point p{zone.x + zone.w / 2, zone.y + zone.h / 2};
    const auto before = f.steps();
    const double volume = f.track(a).volumeDb;
    f.pointer(ui::PointerEvent::Type::Down, p);
    for (int i = 1; i <= 10; ++i) {
        f.pointer(ui::PointerEvent::Type::Move, {p.x, p.y - i * 3.0f});
        f.refresh();
    }
    f.pointer(ui::PointerEvent::Type::Up, {p.x, p.y - 30.0f});
    f.refresh();
    CHECK(f.steps() == before + 1);
    CHECK(f.track(a).volumeDb == volume);
    const double depth = f.app->head().modulations.find(route)->depth;
    CHECK(depth > 6);
    REQUIRE(f.strip(a).fader->modulation());
    CHECK(f.strip(a).fader->modulation()->depth == doctest::Approx(depth));
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: a right-click on a VCA's fader learns MIDI for it, elsewhere the group menu") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.exec("group.add", R"({"name":"Drums","members":[")" + a + R"("]})");
    f.exec("vca.add", R"({"group":"gr_1"})");
    f.refresh();
    const auto open = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Down, p, 1, 0, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 1, 0, 3);
    };
    open(f.centre(f.node("mixer.vca.fader.gr_1")));
    ui::Node* menu = f.root->find("strip.param.menu");
    REQUIRE(menu);
    {
        const auto items = ui::menuItems(*menu);
        REQUIRE(items.size() == 4);
        CHECK(items[0].label == "MIDI learn: Volume…");
        CHECK(items[1].label == "Modulate: Volume…");
        CHECK_FALSE(items[1].enabled);  // a VCA is no modulation target
        CHECK(items[2].label == "Show lane: Volume");
        CHECK(items[3].label == "Reset to 0 dB");
    }
    // The entry opens the learn dialog; a moved control and OK bind it.
    const ui::Point p{menu->bounds().x + 40, menu->bounds().y + 13};
    f.pointer(ui::PointerEvent::Type::Move, p);
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Up, p);
    REQUIRE(f.root->find("learn"));
    engine::EngineApp::ControlEvent e;
    e.midi.bytes[0] = 0xb0;
    e.midi.bytes[1] = 9;
    e.midi.bytes[2] = 127;
    f.actions->control({e});
    REQUIRE(f.root->click("learn.ok"));
    const model::Session s = f.app->head();
    const auto& learn = s.groups.find(*model::Id::parse("gr_1"))->vca->learn;
    REQUIRE(learn.size() == 1);
    CHECK(learn[0].param == "volume");
    CHECK(learn[0].control.number == 9);
    CHECK(f.errors.empty());

    open(f.centre(f.node("mixer.vca.name.gr_1")));
    CHECK_FALSE(f.root->find("strip.param.menu"));
    CHECK(f.root->popupOpen());  // the group's menu
}

TEST_CASE("mixer: a group's tab and a VCA's members and env buttons open their menus beside them") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "Kick");
    f.exec("group.add", R"({"name":"Drums","members":[")" + a + R"("]})");
    f.exec("vca.add", R"({"group":"gr_1"})");
    std::optional<ui::Rect> envAnchor;
    f.mixer->envelopes = [&](const model::Id&, const ui::Rect& anchor) { envAnchor = anchor; };
    f.refresh();
    // The menu touches the button: below it, or above when there is no room.
    const auto beside = [&](const char* button) {
        const ui::Rect b = f.node(button)->bounds();
        f.pointer(ui::PointerEvent::Type::Down, f.centre(f.node(button)));
        f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node(button)));
        const ui::Rect m = f.node("group.menu.gr_1")->bounds();
        CHECK(m.x < b.right());
        CHECK(m.right() > b.x);
        CHECK((std::abs(m.y - b.bottom()) < 4 || std::abs(m.bottom() - b.y) < 4));
        f.root->closePopup();
        f.root->frame(false);
    };
    beside("mixer.group.tr_1.gr_1");
    beside("mixer.vca.members.gr_1");
    f.pointer(ui::PointerEvent::Type::Down, f.centre(f.node("mixer.vca.env.gr_1")));
    f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node("mixer.vca.env.gr_1")));
    REQUIRE(envAnchor);
    const ui::Rect env = f.node("mixer.vca.env.gr_1")->bounds();
    CHECK(envAnchor->x == env.x);
    CHECK(envAnchor->y == env.y);
    CHECK(envAnchor->w == env.w);
}

TEST_CASE("mixer: a channel fed by a rack shows a rack slot above its plug-ins") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> opened;
    f.mixer->openRack = [&](const model::Id& rack) { opened.push_back(rack.str()); };
    json::Value made = f.exec("rack.add", R"({"name":"Lead"})");
    const std::string rack = made["rack"].asString(), track = made["track"].asString();
    const auto fx = f.addFx(track, "Hickory");
    const auto plain = f.addTrack("channel", "Plain");
    f.refresh();
    // One line, named after the rack, above the plug-ins; none elsewhere.
    const auto& st = f.strip(track);
    REQUIRE(st.racks.size() == 1);
    ui::Button* slot = st.racks.front();
    CHECK(slot->id() == "mixer.rack." + track + "." + rack);
    CHECK(slot->valueText() == "Lead");
    const ui::Rect r = slot->bounds(), plug = f.node("mixer.fx." + fx)->bounds();
    CHECK(near(r.h, plug.h));
    CHECK(r.bottom() <= plug.y);
    CHECK(near(r.x, plug.x));
    CHECK(f.strip(plain).racks.empty());
    f.snapshot("mixer-rack-slot.png");
    // A click: the rack window on it, not the chain window.
    f.pointer(ui::PointerEvent::Type::Down, f.centre(slot), 1);
    f.pointer(ui::PointerEvent::Type::Up, f.centre(slot), 1);
    CHECK(opened == std::vector<std::string>{rack});
    // A double-click's second press opens nothing more.
    f.pointer(ui::PointerEvent::Type::Down, f.centre(slot), 2);
    f.pointer(ui::PointerEvent::Type::Up, f.centre(slot), 2);
    CHECK(opened == std::vector<std::string>{rack});
    CHECK(f.editors.empty());
    // Renamed, the slot follows.
    f.exec("rack.set", R"({"rack":")" + rack + R"(","name":"Bells"})");
    f.refresh();
    REQUIRE(f.strip(track).racks.size() == 1);
    CHECK(f.strip(track).racks.front()->valueText() == "Bells");
    // Another rack homed there becomes the channel's input: its slot.
    const std::string second =
        f.exec("rack.add", R"({"track":")" + track + R"(","name":"Pad"})")["rack"].asString();
    f.refresh();
    REQUIRE(f.strip(track).racks.size() == 1);
    CHECK(f.strip(track).racks.front()->id() == "mixer.rack." + track + "." + second);
    CHECK(f.strip(track).racks.front()->valueText() == "Pad");
}

TEST_CASE("mixer: a strip's modulator racks show as mod slots") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> opened;
    f.mixer->openRack = [&](const model::Id& rack) { opened.push_back(rack.str()); };
    json::Value made = f.exec("rack.add", R"({"name":"Lead"})");
    const std::string lead = made["rack"].asString(), leadTrack = made["track"].asString();
    const auto keys = f.addTrack("channel", "Keys");
    const auto fx = f.addFx(keys, "Hickory");
    // No command adds a Mod Out yet (ADR-0096 M1): the file gets them, as a
    // project from a later build would. Keys homes "Mods" (a Mod Out) and
    // "Idle" (none); the Lead rack, which feeds its
    // channel, gains a Mod Out too. (Only a channel homes a rack.)
    const std::string path = (f.dir / "mods.json").generic_string();
    f.exec("project.save", R"({"path":")" + path + R"("})");
    const auto bytes = readFile(path).value();
    json::Value v = json::parse(std::string(bytes.begin(), bytes.end())).value();
    auto put = [&](const char* section, const char* id, const std::string& body) {
        v[section][id] = json::parse(body).value();
    };
    auto module = [](const std::string& rack, const char* type) {
        return R"({"rack":")" + rack + R"(","type":")" + type +
               R"(","version":"1.0.0","x":40,"y":40})";
    };
    const std::string modOut = std::string(model::kModOutModule);
    put("racks", "rk_40", R"({"track":")" + keys + R"(","name":"Mods"})");
    put("racks", "rk_42", R"({"track":")" + keys + R"(","name":"Idle"})");
    put("modules", "mo_40", module("rk_40", modOut.c_str()));
    put("modules", "mo_42", module(lead, modOut.c_str()));
    put("modules", "mo_43", module("rk_42", "com.suppleaudio.rack.lfo"));
    v["project"]["next_ids"]["rk"] = 50.0;
    v["project"]["next_ids"]["mo"] = 50.0;
    REQUIRE(writeFileAtomic(path, json::writeCanonical(v)));
    f.exec("project.open", R"({"path":")" + path + R"("})");
    f.refresh();
    // Keys: one mod slot, Mods, above its plug-ins; Idle holds no Mod Out.
    const auto& st = f.strip(keys);
    REQUIRE(st.racks.size() == 1);
    ui::Button* slot = st.racks.front();
    CHECK(slot->id() == "mixer.mod." + keys + ".rk_40");
    CHECK(slot->valueText() == "Mods");
    const ui::Rect r = slot->bounds(), plug = f.node("mixer.fx." + fx)->bounds();
    CHECK(near(r.h, plug.h));
    CHECK(r.bottom() <= plug.y);
    // A rack that feeds its channel and holds a Mod Out shows once, where it
    // feeds, as a mod slot.
    REQUIRE(f.strip(leadTrack).racks.size() == 1);
    CHECK(f.strip(leadTrack).racks.front()->id() == "mixer.mod." + leadTrack + "." + lead);
    f.snapshot("mixer-mod-slot.png");
    // A click opens the rack window on it.
    f.pointer(ui::PointerEvent::Type::Down, f.centre(slot), 1);
    f.pointer(ui::PointerEvent::Type::Up, f.centre(slot), 1);
    CHECK(opened == std::vector<std::string>{"rk_40"});
    // Without its Mod Out the rack is no modulator: the slot goes.
    f.exec("module.remove", R"({"module":"mo_40"})");
    f.refresh();
    CHECK(f.strip(keys).racks.empty());
    REQUIRE(f.strip(leadTrack).racks.size() == 1);
}

TEST_CASE("pan controls: selected width drags keep offsets and skip masters") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("buss", "B");
    f.exec("track.set", R"({"track":")" + b + R"(","width":20})");
    f.ui.selectedTracks = {a, b, "tr_0"};
    gui::SelectionEdit selection(*f.dispatcher, *f.app, f.ui);
    for (double width : {80., 60., 40.}) {
        auto args = json::Value::object();
        args["track"] = a;
        args["width"] = width;
        REQUIRE(selection.set(args, "width:1"));
    }
    CHECK(f.track(a).width == 40);
    CHECK(f.track(b).width == -40);
    CHECK(f.track("tr_0").width == 100);
    auto args = json::Value::object();
    args["track"] = a;
    args["pan_mode"] = "width";
    REQUIRE(selection.set(args, {}));
    CHECK(f.track(b).panMode == model::PanMode::Width);
    CHECK(f.track("tr_0").panMode == model::PanMode::Pan);
}

TEST_CASE("pan controls: a mixer's second bar records a writing envelope") {
    Fixture f;
    if (!f.ok) return;
    const auto track = f.addTrack("channel", "Stereo");
    f.exec("track.set", R"({"track":")" + track + R"(","pan_mode":"width"})");
    const auto lane = *model::Id::parse(
        f.exec("env.add", R"({"track":")" + track +
                              R"(","param":"width","points":[{"beat":0,"value":0}]})")["env"]
            .asString());
    f.exec("env.set", R"({"env":")" + lane.str() + R"(","mode":"latch"})");
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    struct Stop {
        engine::EngineApp& app;
        ~Stop() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                               {.anticipative = false, .workers = 0}));
    f.exec("transport.play", "{}");
    backend.run(4);
    f.refresh();
    const auto bar = f.strip(track).pan->bounds();
    f.drag({bar.x + .9f * bar.w, bar.y + bar.h / 2}, {bar.x + .65f * bar.w, bar.y + bar.h / 2});
    CHECK(f.track(track).width < 100);
    const auto* env = f.app->head().envelopes.find(lane);
    REQUIRE(env);
    CHECK(env->points.back().value == f.track(track).width);
    const model::EnvTarget target{*model::Id::parse(track), "width"};
    CHECK(f.app->engine()->writeHolds() == std::vector<model::EnvTarget>{target});
}

TEST_CASE("preferences: stale GUI snapshots preserve engine-owned MIDI records") {
    Fixture f;
    if (!f.ok) return;
    const auto file = f.dir / "preferences.json";
    gui::Preferences stale;
    stale.midiOutputs["Synth"] = json::parse(R"({"clock":true,"offset_ms":-5})").value();
    REQUIRE(stale.save(file));
    const auto current = json::parse(R"({"clock":false,"offset_ms":12})").value();
    REQUIRE(f.app->setMidiPreferences({}, {{"Synth", current}}, false));
    REQUIRE(f.app->writeMidiPreferences(file));
    REQUIRE(stale.save(file));
    auto loaded = gui::Preferences::load(file);
    REQUIRE(loaded);
    CHECK_FALSE(loaded->midiOutputs.at("Synth").find("clock")->asBool());
    CHECK(engine::midiPortSettings(loaded->midiOutputs, "Synth").offsetMs == 12);
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, stale, file);
    prefs.onLinux = true;  // the rows every platform can test
    CHECK(engine::midiPortSettings(f.app->midiOutputsPreferences(), "Synth").offsetMs == 12);
    REQUIRE(prefs.setAudioThreads(2));
    loaded = gui::Preferences::load(file);
    REQUIRE(loaded);
    CHECK_FALSE(loaded->midiOutputs.at("Synth").find("clock")->asBool());
}

TEST_CASE("preferences: malformed MIDI records do not block GUI startup") {
    Fixture f;
    REQUIRE(f.ok);
    const auto file = f.dir / "bad-midi-preferences.json";
    REQUIRE(writeFileAtomic(
        file,
        R"({"schema":"supple.preferences/1.6","midi_outputs":{"Bad":{"offset_ms":2000},"Good":{"enabled":false}},"midi_inputs":{"Bad":{"enabled":"yes"},"Good":{"offset_ms":5}}})"));
    const auto loaded = gui::Preferences::load(file);
    REQUIRE(loaded);
    CHECK_FALSE(loaded->midiOutputs.contains("Bad"));
    CHECK_FALSE(loaded->midiInputs.contains("Bad"));
    CHECK_FALSE(engine::midiPortSettings(loaded->midiOutputs, "Good").enabled);
    CHECK(engine::midiPortSettings(loaded->midiInputs, "Good").offsetMs == 5);
}
