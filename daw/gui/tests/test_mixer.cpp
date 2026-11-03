// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The mixer's own gestures (gui.md §4.4): the fx button, a resizable chain
// section, plug-in reorder and copy by drag, strip reorder by drag; the
// reorder-index helper the TCP shares.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "app_actions.h"
#include "audio_device.h"
#include "meter_feed.h"
#include "mixer.h"
#include "preferences.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
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
    void refresh() {
        mixer->sync();
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
};

}  // namespace

TEST_CASE("mixer: a master looks like the others, and \"fx\" sits small at the strip's top right") {
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
    // Small, at the top right ("fx", then "+"), with the name beside them.
    const ui::Rect strip = sa.node->bounds(), fx = sa.fx->bounds(), plus = sa.addFx->bounds();
    CHECK(sa.fx->valueText() == "fx");
    CHECK(sa.addFx->valueText() == "+");
    CHECK(fx.w <= 34);
    CHECK(fx.h <= 16);
    CHECK(plus.right() >= strip.right() - 8);
    CHECK(fx.right() <= plus.x);
    CHECK(fx.y < strip.y + 12);
    CHECK(sa.name->bounds().right() <= fx.x + 1);
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

TEST_CASE("mixer: the chain takes the room unused sends and arm rows would leave empty") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto bus = f.addTrack("buss", "Bus");
    f.exec("route.add", R"({"from":")" + b + R"(","to":")" + bus + R"("})");
    f.refresh();
    // No sends: the chain reaches down to the arm, then the input/output row.
    CHECK_FALSE(f.node("mixer.sends." + a)->visible);
    CHECK(f.node("mixer.rec." + a)->bounds().y - f.strip(a).chain->bounds().bottom() <= 4.5f);
    CHECK(f.node("mixer.io." + a)->bounds().y - f.node("mixer.rec." + a)->bounds().bottom() <=
          4.5f);
    // Polarity, monitoring and mono share one row, before the pan.
    CHECK(near(f.node("mixer.monitor." + a)->bounds().y + 10,
               f.node("mixer.polarity." + a)->bounds().y + 7));
    CHECK(f.node("mixer.polarity." + a)->bounds().right() <=
          f.node("mixer.monitor." + a)->bounds().x);
    CHECK(f.node("mixer.monitor." + a)->bounds().right() <= f.node("mixer.mono." + a)->bounds().x);
    CHECK(f.node("mixer.mono." + a)->bounds().bottom() <= f.strip(a).pan->bounds().y);
    // A send takes one row out of the chain section.
    CHECK(f.node("mixer.sends." + b)->visible);
    CHECK(near(f.strip(a).chain->bounds().h - f.strip(b).chain->bounds().h, 14 + 4));
    // A buss has no arm row (nor an input): its chain takes that room.
    CHECK_FALSE(f.root->find("mixer.rec." + bus));
    CHECK_FALSE(f.root->find("mixer.input." + bus));
    CHECK(f.root->find("mixer.out." + bus));
    CHECK(f.strip(bus).chain->bounds().h > f.strip(a).chain->bounds().h);
    CHECK(near(f.strip(bus).fader->bounds().y, f.strip(a).fader->bounds().y));  // faders line up
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
    // A right-click there: channel, buss or master (the TCP's "+ strip" menu).
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
    const ui::Point del{menu->bounds().x + 40, menu->bounds().y + 8 * 26 + 13};
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
    // Dropped where it already is: nothing.
    f.refresh();
    const auto steps = f.steps();
    const ui::Point same = f.centre(f.node("mixer.fx." + p2));
    f.drag(same, {same.x + 20, same.y});
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
    // Dropped somewhere that is nobody's strip: cancelled.
    f.drag(f.centre(f.node("mixer.fx." + p1)), {700, 20});
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
    const ui::Point dup{menu->bounds().x + 40, menu->bounds().y + 7 * 26 + 13};
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
    f.pointer(ui::PointerEvent::Type::Up, {rc.x + rc.w - 10, rc.y + 200});
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
    f.root->key(SDLK_ESCAPE);
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
    // A second drag onto a strip it already feeds marks nothing and adds nothing.
    f.drag(f.centre(f.node("mixer.out." + a)), f.centre(f.node("mixer.faderrow." + bus)), false);
    CHECK(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Send);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::None);
    f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node("mixer.faderrow." + bus)));
    CHECK(f.steps() == before + 1);
    CHECK(f.app->head().routes.size() == 1);
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
    std::vector<model::Id> picked;
    f.mixer->pickInput = [&](const model::Id& track, const ui::Rect&) { picked.push_back(track); };
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
}

TEST_CASE("preferences: the dialog applies each setting at once and saves it") {
    Fixture f;
    if (!f.ok) return;
    views::MouseMap map;
    gui::MouseDialog mouse(*f.root, map, {});
    const auto file = f.dir / "config" / "preferences.json";
    gui::PreferencesDialog prefs(*f.root, *f.app, mouse, gui::Preferences{}, file);
    CHECK(f.app->defaultMonitor() == model::Monitor::On);
    CHECK(f.app->copyImportedMedia());
    prefs.open();
    f.root->frame(false);
    REQUIRE(prefs.isOpen());
    // New channels' monitoring.
    prefs.setDefaultMonitor(model::Monitor::Off);
    CHECK(f.node("prefs.monitor.set")->valueText() == "Off");
    CHECK(f.track(f.addTrack("channel", "A")).record.monitor == model::Monitor::Off);
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
    // The render folder: absolute only.
    CHECK_FALSE(prefs.setRenderDirectory("relative/renders"));
    REQUIRE(prefs.setRenderDirectory(f.dir / "my renders"));
    CHECK(f.app->rendersDir() == f.dir / "my renders");
    auto saved = gui::Preferences::load(file);
    REQUIRE(saved);
    CHECK(saved->defaultMonitor == model::Monitor::Off);
    CHECK(saved->audioThreads == 4);
    CHECK_FALSE(saved->copyImports);
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
    values.renderOpenFolder = true;
    values.audio.backend = "portaudio";
    values.audio.hostApi = "Core Audio";
    values.audio.outputDevice = "Studio Out";
    values.audio.inputDevice = "Studio In";
    values.audio.record = false;
    values.audio.rate = 96000;
    values.audio.block = 128;
    values.audio.forceGraph = true;
    REQUIRE(values.save(file));
    auto loaded = gui::Preferences::load(file);
    REQUIRE(loaded);
    CHECK(loaded->defaultMonitor == model::Monitor::Auto);
    CHECK(loaded->audioThreads == 3);
    CHECK(loaded->lockMemory);
    CHECK(loaded->preventDeepIdle);
    CHECK(loaded->renderDirectory == f.dir / "renders");
    CHECK_FALSE(loaded->copyImports);
    CHECK(loaded->renderOpenFolder);
    CHECK(loaded->audio == values.audio);
    const auto bytes = readFile(file).value();
    CHECK(std::string(bytes.begin(), bytes.end()).find("supple.preferences/1.6") !=
          std::string::npos);
    REQUIRE(loaded->save(file));
    CHECK(readFile(file).value() == bytes);
    // Older schemas load, with defaults for what they lack.
    for (const char* version : {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6"}) {
        REQUIRE(writeFileAtomic(
            file, std::string("{\"schema\":\"supple.preferences/") + version + "\"}"));
        auto old = gui::Preferences::load(file);
        REQUIRE(old);
        CHECK(old->audioThreads == 0);
        CHECK_FALSE(old->lockMemory);
        CHECK_FALSE(old->preventDeepIdle);
        CHECK(old->renderDirectory.empty());
        CHECK(old->copyImports);
        CHECK_FALSE(old->renderOpenFolder);
        CHECK(old->audio == gui::AudioDeviceSettings{});
    }
    // A wrong value names its field.
    for (auto [field, bad] : {std::pair{"audio_threads", "65"},
                              {"audio_threads", "1.5"},
                              {"lock_memory", "1"},
                              {"prevent_deep_idle", "\"true\""},
                              {"render_directory", "\"relative\""},
                              {"copy_imported_media", "1"},
                              {"render_open_folder", "\"no\""}}) {
        REQUIRE(writeFileAtomic(file, std::string(R"({"schema":"supple.preferences/1.6",")") +
                                          field + "\":" + bad + "}"));
        auto rejected = gui::Preferences::load(file);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().path == std::string("/") + field);
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
    // Named devices; every input channel (up to 32); a chosen rate wins.
    s.outputDevice = "Interface";
    s.inputDevice = "Interface";
    s.rate = 44100;
    s.block = 64;
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":64,"host_api":"Windows WASAPI",)"
                           R"("output_device":"Interface","input_device":"Interface","inputs":8,)"
                           R"("rate":44100.0})"));
    // Playback only opens no input.
    s.record = false;
    s.rate = 0;
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":64,"host_api":"Windows WASAPI",)"
                           R"("output_device":"Interface","rate":96000.0})"));
    // ASIO: one device both ways, whatever input was chosen before.
    s = {};
    s.backend = "portaudio";
    s.hostApi = "ASIO";
    s.inputDevice = "Microphone";
    CHECK(args(s) == canon(R"({"backend":"portaudio","block":256,"host_api":"ASIO",)"
                           R"("input_device":"Interface ASIO","inputs":18,"rate":48000.0})"));
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
    prefs.open();
    f.root->frame(false);
    const float offHeight = f.node("prefs")->bounds().h;
    CHECK(offHeight < 460);  // no space reserved for empty notes
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
    prefs.open();
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
