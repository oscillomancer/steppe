// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Ruler edits driven through the same pointer, keymap and text paths as the app.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include "test_support.h"

#include <filesystem>

#include "app_actions.h"
#include "supple/gui/window.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/views/arrange.h"

using namespace supple;

namespace {
const cmd::Origin kRulerUi{cmd::Origin::Type::Ui, "gui", {}};
using Pointer = ui::PointerEvent::Type;

struct RulerFixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState state;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<peaks::PeakService> peaks;
    std::unique_ptr<ui::Root> root;
    views::ArrangeView* view = nullptr;
    std::vector<std::string> commands;
    std::vector<Error> errors;

    RulerFixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-ruler-test-" + std::to_string(testProcessId()));
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        REQUIRE(p);
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"ruler-test", 1100, 600, true});
        REQUIRE(w);
        window = std::move(*w);
        auto fonts = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        REQUIRE(fonts);
        engine::AppOptions options;
        options.workdir = dir;
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        actions = std::make_unique<gui::AppActions>(registry, *dispatcher, *app, state,
                                                    dir / "keys.json");
        peaks = std::make_unique<peaks::PeakService>(dir / "cache", [](std::function<void()>) {});
        root = std::make_unique<ui::Root>(*window, *fonts, ui::Theme::dark());
        auto arrange = std::make_unique<views::ArrangeView>(
            *peaks, [](const model::MediaSource& s) { return std::filesystem::path(s.path); });
        view = arrange.get();
        root->setContent(std::move(arrange));
        view->setRulerCallbacks({[this](const std::string& command, json::Value args) {
                                     commands.push_back(command);
                                     return dispatcher->execute(
                                         {command, std::move(args), kRulerUi});
                                 },
                                 [this](const Error& error) { errors.push_back(error); }});
        actions->addUi("ui.marker.add", "Marker: add at playhead", [this] { view->addMarker(); });
        actions->addUi("ui.region.add", "Region: add over loop range",
                       [this] { view->addRegion(); });
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {
            return actions->key(e.key, e.mods,
                                root->focused() && root->focused()->role() == ui::Role::TextField);
        };
        auto v = view->viewport();
        v.pixelsPerSecond = 80;  // 40 px per beat: quarter-beat grid
        view->setViewport(v);
        sync();
    }
    ~RulerFixture() {
        root.reset();
        peaks.reset();
        actions.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    void sync() {
        view->show(app->head());
        const auto loop = app->loop();
        view->setLoopRange(loop.start, loop.end, loop.enabled);
        const auto punch = app->punch();
        view->setPunchRange(punch.start, punch.end, punch.enabled);
        root->frame(false);
    }
    void exec(const std::string& command, json::Value args) {
        REQUIRE(dispatcher->execute({command, std::move(args), kRulerUi}));
        sync();
    }
    std::size_t history() {
        auto r = dispatcher->execute({"history.list", json::Value::object(), kRulerUi});
        REQUIRE(r);
        return r->find("versions")->size();
    }
    ui::Point at(double beat, float rowY = 10) {
        return {view->xAt(app->head().timeMap->secondsAt(beat)), view->ruler().bounds().y + rowY};
    }
    void pointer(Pointer type, ui::Point p, unsigned mods = 0, int clicks = 1) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = 1;
        e.mods = mods;
        e.clicks = clicks;
        root->pointer(e);
    }
    void click(ui::Point p, unsigned mods = 0, int clicks = 1) {
        pointer(Pointer::Down, p, mods, clicks);
        pointer(Pointer::Up, p, mods, clicks);
        sync();
    }
    void drag(ui::Point from, ui::Point to, unsigned mods = 0) {
        pointer(Pointer::Down, from, mods);
        const auto before = history();
        pointer(Pointer::Move, to, mods);
        root->frame(false);  // paint the ghost without a model edit
        CHECK(history() == before);
        pointer(Pointer::Up, to, mods);
        sync();
        CHECK(errors.empty());
    }
    void submit(const std::string& text) {
        REQUIRE(root->popupOpen());
        root->frame(false);
        root->text(text);
        root->key(SDLK_RETURN);
        sync();
    }
    void loop(double start, double end, bool enabled) {
        json::Value args = json::Value::object();
        args["start_beat"] = start;
        args["end_beat"] = end;
        args["enabled"] = enabled;
        exec("transport.loop", std::move(args));
    }
};
}  // namespace

TEST_CASE("ruler: loop selection, seek, edges, body and cancellation are transport actions") {
    RulerFixture f;
    json::Value start = json::Value::object();
    start["backend"] = "dummy";  // seek needs a running engine; no audio device
    f.exec("engine.start", std::move(start));
    const auto n = f.history();
    f.pointer(Pointer::Down, f.at(2.1));
    f.pointer(Pointer::Move, f.at(6.12));
    CHECK(f.app->loop().end == 0);
    f.pointer(Pointer::Up, f.at(6.12));
    f.sync();
    CHECK_FALSE(f.app->loop().enabled);
    CHECK(f.app->loop().start == doctest::Approx(1));
    CHECK(f.app->loop().end == doctest::Approx(3));
    CHECK(f.commands == std::vector<std::string>{"transport.loop"});
    f.drag(f.at(2), f.at(1));
    CHECK(f.app->loop().start == doctest::Approx(0.5));
    f.drag(f.at(6), f.at(7));
    CHECK(f.app->loop().end == doctest::Approx(3.5));
    f.drag(f.at(3), f.at(5));
    CHECK(f.app->loop().start == doctest::Approx(1.5));
    CHECK(f.app->loop().end == doctest::Approx(4.5));
    f.drag(f.at(9), f.at(11.13), ui::kShift);
    CHECK(f.app->loop().end == doctest::Approx(5.565));
    f.click(f.at(12.13));
    auto t = f.dispatcher->execute({"transport.get", json::Value::object(), kRulerUi});
    REQUIRE(t);
    CHECK(t->find("seconds")->asDouble() == doctest::Approx(6.065));
    const auto count = f.commands.size();
    f.pointer(Pointer::Down, f.at(15));
    f.pointer(Pointer::Move, f.at(18));
    f.root->key(SDLK_ESCAPE);
    f.pointer(Pointer::Up, f.at(18));
    CHECK(f.commands.size() == count);
    CHECK(f.history() == n);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: enabled reversed selection, tiny drag and grid at different zooms") {
    RulerFixture f;
    f.loop(2, 4, true);
    const auto n = f.history();
    f.drag(f.at(12), f.at(8));
    CHECK(f.app->loop().enabled);
    CHECK(f.app->loop().start == doctest::Approx(4));
    CHECK(f.app->loop().end == doctest::Approx(6));
    f.drag(f.at(15), f.at(15.1));  // both ends snap to the same point; no invalid command
    CHECK(f.errors.empty());
    f.loop(2, 4, false);
    auto v = f.view->viewport();
    v.pixelsPerSecond = 20;  // one beat per grid cell
    f.view->setViewport(v);
    f.sync();
    f.drag(f.at(8.4), f.at(12.4));
    CHECK(f.app->loop().start == doctest::Approx(4));
    CHECK(f.app->loop().end == doctest::Approx(6));
    CHECK(f.history() == n);
}

TEST_CASE("ruler: marker key, snapped and free drags, rename, cancel and removal") {
    RulerFixture f;
    CHECK(f.actions->keymap().conflicts().empty());
    f.view->setPlayhead(2);
    auto n = f.history();
    f.root->key(SDLK_M);
    f.sync();
    REQUIRE(f.app->head().markers.size() == 1);
    const auto id = f.app->head().markers.begin()->id;
    CHECK(f.app->head().markers.find(id)->pos == 4);
    CHECK(f.history() == ++n);
    f.drag(f.at(4, 80), f.at(6.12, 80));
    CHECK(f.app->head().markers.find(id)->pos == doctest::Approx(6));
    CHECK(f.history() == ++n);
    f.drag(f.at(6, 80), f.at(7.13, 80), ui::kShift);
    CHECK(f.app->head().markers.find(id)->pos == doctest::Approx(7.13));
    CHECK(f.history() == ++n);
    f.click(f.at(7.13, 80), 0, 2);
    f.submit("Chorus");
    CHECK(f.app->head().markers.find(id)->name == "Chorus");
    CHECK(f.history() == ++n);
    f.click(f.at(7.13, 80), 0, 2);
    f.root->text("Discarded");
    f.root->key(SDLK_ESCAPE);
    CHECK(f.history() == n);
    f.click(f.at(7.13, 80), ui::kAlt);
    CHECK(f.app->head().markers.empty());
    CHECK(f.history() == ++n);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: region key, body move, both edges, rename and removal") {
    RulerFixture f;
    f.loop(4, 8, false);
    auto n = f.history();
    f.root->key(SDLK_R, ui::kShift);
    f.sync();
    REQUIRE(f.app->head().regions.size() == 1);
    const auto id = f.app->head().regions.begin()->id;
    CHECK(f.app->head().regions.find(id)->start == 4);
    CHECK(f.app->head().regions.find(id)->end == 8);
    CHECK(f.history() == ++n);
    f.drag(f.at(6, 64), f.at(8.12, 64));
    CHECK(f.app->head().regions.find(id)->start == 6);
    CHECK(f.app->head().regions.find(id)->end == 10);
    CHECK(f.history() == ++n);
    f.drag(f.at(6, 64), f.at(5, 64));
    CHECK(f.app->head().regions.find(id)->start == 5);
    CHECK(f.history() == ++n);
    f.drag(f.at(10, 64), f.at(11.13, 64), ui::kShift);
    CHECK(f.app->head().regions.find(id)->end == doctest::Approx(11.13));
    CHECK(f.history() == ++n);
    f.click(f.at(8, 64), 0, 2);
    f.submit("Verse");
    CHECK(f.app->head().regions.find(id)->name == "Verse");
    CHECK(f.history() == ++n);
    f.click(f.at(8, 64), ui::kAlt);
    CHECK(f.app->head().regions.empty());
    CHECK(f.history() == ++n);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: tempo insertion and BPM field preserve other points and ramps") {
    RulerFixture f;
    auto args =
        json::parse(R"({"points":[{"beat":0,"bpm":120,"ramp":true},{"beat":12,"bpm":90}]})");
    REQUIRE(args);
    f.exec("tempo.set", *args);
    auto n = f.history();
    const double bpm = f.app->head().timeMap->bpmAt(6);
    f.click(f.at(6, 42), 0, 2);
    CHECK(f.history() == ++n);
    REQUIRE(f.app->head().timeMap->tempo().size() == 3);
    CHECK(f.app->head().timeMap->tempo()[0].shape == time::TempoShape::Ramp);
    CHECK(f.app->head().timeMap->tempo()[1].beat == 6);
    CHECK(f.app->head().timeMap->tempo()[1].bpm == doctest::Approx(bpm));
    CHECK(f.app->head().timeMap->tempo()[1].shape == time::TempoShape::Ramp);
    f.click(f.at(6, 42), 0, 2);
    f.submit("137.5");
    CHECK(f.history() == ++n);
    const auto& points = f.app->head().timeMap->tempo();
    CHECK(points[0].bpm == 120);
    CHECK(points[0].shape == time::TempoShape::Ramp);
    CHECK(points[1].bpm == 137.5);
    CHECK(points[2].beat == 12);
    CHECK(points[2].bpm == 90);
    f.click(f.at(6, 42), 0, 2);
    f.root->key(SDLK_ESCAPE);
    CHECK(f.history() == n);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: invalid BPM stays editable without a history entry") {
    RulerFixture f;
    const auto n = f.history();
    f.click(f.at(0, 42), 0, 2);
    f.submit("nan");
    CHECK(f.root->popupOpen());
    REQUIRE(f.errors.size() == 1);
    CHECK(f.history() == n);
    f.root->key(SDLK_A, ui::kCtrl);
    f.submit("140");
    CHECK_FALSE(f.root->popupOpen());
    CHECK(f.history() == n + 1);
    CHECK(f.app->head().timeMap->tempo()[0].bpm == 140);
}

TEST_CASE("ruler: nearby flags target the closest marker and tempo point") {
    RulerFixture f;
    f.view->setPlayhead(2);
    f.root->key(SDLK_M);
    f.sync();
    const auto first = f.app->head().markers.begin()->id;
    f.view->setPlayhead(2.5);
    f.root->key(SDLK_M);
    f.sync();
    const auto second = (f.app->head().markers.begin() + 1)->id;
    f.click(f.at(5, 80), 0, 2);
    f.submit("Second");
    CHECK(f.app->head().markers.find(first)->name.empty());
    CHECK(f.app->head().markers.find(second)->name == "Second");
    auto args = json::parse(R"({"points":[{"beat":0,"bpm":120},{"beat":1,"bpm":123.456789}]})");
    REQUIRE(args);
    f.exec("tempo.set", *args);
    f.click(f.at(1, 42), 0, 2);
    REQUIRE(f.root->focused());
    auto* field = dynamic_cast<ui::TextField*>(f.root->focused());
    REQUIRE(field);
    CHECK(field->text() == "123.456789");
    f.submit("140");
    CHECK(f.app->head().timeMap->tempo()[0].bpm == 120);
    CHECK(f.app->head().timeMap->tempo()[1].bpm == 140);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: region captures outside the ruler, clamps at zero and undoes once") {
    RulerFixture f;
    f.loop(4, 8, true);
    f.root->key(SDLK_R, ui::kShift);
    f.sync();
    const auto id = f.app->head().regions.begin()->id;
    const auto n = f.history();
    f.drag(f.at(6, 64), {f.view->ruler().bounds().x - 200, 400});
    CHECK(f.app->head().regions.find(id)->start == 0);
    CHECK(f.app->head().regions.find(id)->end == 4);
    CHECK(f.history() == n + 1);
    f.root->key(SDLK_Z, ui::kCtrl);
    f.sync();
    CHECK(f.app->head().regions.find(id)->start == 4);
    CHECK(f.app->head().regions.find(id)->end == 8);
    const auto count = f.commands.size();
    f.pointer(Pointer::Down, f.at(6, 64));
    f.pointer(Pointer::Move, f.at(9, 64));
    f.root->key(SDLK_ESCAPE);
    f.pointer(Pointer::Up, f.at(9, 64));
    CHECK(f.commands.size() == count);
    CHECK(f.app->head().regions.find(id)->start == 4);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: moving a loop across tempo changes preserves its beat span") {
    RulerFixture f;
    auto args = json::parse(R"({"points":[{"beat":0,"bpm":120},{"beat":8,"bpm":60}]})");
    REQUIRE(args);
    f.exec("tempo.set", *args);
    f.loop(4, 8, false);
    const auto n = f.history();
    f.drag(f.at(6), f.at(10));
    CHECK(f.app->loop().start == doctest::Approx(4));  // beat 8
    CHECK(f.app->loop().end == doctest::Approx(8));    // beat 12
    CHECK(f.history() == n);
    CHECK_FALSE(f.app->loop().enabled);
}

TEST_CASE("ruler: Alt+click on a tempo point toggles its ramp") {
    RulerFixture f;
    auto args = json::parse(R"({"points":[{"beat":0,"bpm":120},{"beat":12,"bpm":90}]})");
    REQUIRE(args);
    f.exec("tempo.set", *args);
    auto n = f.history();
    REQUIRE(f.app->head().timeMap->tempo()[0].shape == time::TempoShape::Step);
    f.click(f.at(0.2, 42), ui::kAlt, 1);
    CHECK(f.history() == ++n);
    CHECK(f.app->head().timeMap->tempo()[0].shape == time::TempoShape::Ramp);
    CHECK(f.app->head().timeMap->tempo()[1].bpm == 90);  // the others unchanged
    f.click(f.at(0.2, 42), ui::kAlt, 1);
    CHECK(f.app->head().timeMap->tempo()[0].shape == time::TempoShape::Step);
}

TEST_CASE("ruler: double-click in the meter half adds or edits a meter change") {
    RulerFixture f;
    auto n = f.history();
    // At bar 3 (beat 8 in 4/4): a new change.
    f.click(f.at(8.5, 55), 0, 2);
    f.submit("3/4");
    CHECK(f.history() == ++n);
    const auto& meter = f.app->head().timeMap->meter();
    REQUIRE(meter.size() == 2);
    CHECK(meter[1].bar == 3);
    CHECK(meter[1].numerator == 3);
    // On its flag: that change is edited, not a new one.
    f.click(f.at(8.0, 55), 0, 2);
    f.submit("6/8");
    CHECK(f.history() == ++n);
    REQUIRE(f.app->head().timeMap->meter().size() == 2);
    CHECK(f.app->head().timeMap->meter()[1].numerator == 6);
    CHECK(f.app->head().timeMap->meter()[1].denominator == 8);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: a right-click deletes a tempo change, a meter change, a region or a marker") {
    RulerFixture f;
    auto rightClick = [&](ui::Point p) {
        ui::PointerEvent e;
        e.type = Pointer::Down;
        e.pos = p;
        e.button = 3;
        f.root->pointer(e);
        e.type = Pointer::Up;
        f.root->pointer(e);
        f.root->frame(false);
    };
    auto pick = [&](int row) {  // menu rows are 26 px
        ui::Node* menu = f.root->find("ruler.menu");
        REQUIRE(menu);
        const ui::Point p{menu->bounds().x + 30,
                          menu->bounds().y + static_cast<float>(row) * 26 + 13};
        f.pointer(Pointer::Move, p);
        f.pointer(Pointer::Down, p);
        f.pointer(Pointer::Up, p);
        f.sync();
    };
    f.exec(
        "tempo.set",
        json::parse(R"({"points": [{"beat": 0, "bpm": 120}, {"beat": 16, "bpm": 90}]})").value());
    f.exec("timesig.set", json::parse(R"({"bar": 3, "meter": "3/4"})").value());
    f.exec("marker.add", json::parse(R"({"pos": 4, "name": "Verse"})").value());
    f.exec("region.add", json::parse(R"({"start": 8, "end": 12, "name": "Hook"})").value());
    auto n = f.history();
    // The tempo half of the tempo row.
    rightClick(f.at(16.2, 41));
    pick(0);
    CHECK(f.app->head().timeMap->tempo().size() == 1);
    CHECK(f.history() == ++n);
    rightClick(f.at(0.2, 41));  // the first point stays (its item is disabled)
    pick(0);
    CHECK(f.app->head().timeMap->tempo().size() == 1);
    CHECK(f.history() == n);
    f.root->closePopups();
    // The meter half.
    rightClick(f.at(8.2, 55));
    pick(0);
    CHECK(f.app->head().timeMap->meter().size() == 1);
    CHECK(f.history() == ++n);
    // Regions and markers: Rename…, Delete.
    rightClick(f.at(10, 64));
    pick(1);
    CHECK(f.app->head().regions.size() == 0);
    rightClick(f.at(4.2, 78));
    pick(1);
    CHECK(f.app->head().markers.size() == 0);
    CHECK(f.history() == n + 2);
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: a drag in the time row sets the punch range; the menu copies ranges") {
    RulerFixture f;
    const auto n = f.history();
    // Bars 2-3 at 120 BPM: 2 s .. 6 s; the time row is y 22..37.
    f.drag(f.at(4.0, 30), f.at(12.0, 30));
    auto punch = f.app->punch();
    CHECK(punch.enabled);  // a new punch range is meant to be used
    CHECK(punch.start == doctest::Approx(2.0));
    CHECK(punch.end == doctest::Approx(6.0));
    CHECK(f.view->punchEnd() == doctest::Approx(6.0));
    CHECK_FALSE(f.app->loop().end > f.app->loop().start);  // the loop is untouched
    // Its right edge moves it.
    f.drag(f.at(12.0, 30), f.at(16.0, 30));
    CHECK(f.app->punch().end == doctest::Approx(8.0));
    CHECK(f.app->punch().start == doctest::Approx(2.0));
    // A bars-row drag still sets the loop.
    f.drag(f.at(16.0, 10), f.at(20.0, 10));
    CHECK(f.app->loop().start == doctest::Approx(8.0));
    CHECK(f.app->loop().end == doctest::Approx(10.0));
    // Right-click in the time row: "Punch range = loop range" (row 2).
    ui::PointerEvent e;
    e.type = Pointer::Down;
    e.pos = f.at(1.0, 30);
    e.button = 3;
    f.root->pointer(e);
    e.type = Pointer::Up;
    f.root->pointer(e);
    f.root->frame(false);
    ui::Node* menu = f.root->find("ruler.menu");
    REQUIRE(menu);
    const ui::Point row{menu->bounds().x + 30, menu->bounds().y + 2 * 26 + 13};
    f.pointer(Pointer::Move, row);
    f.pointer(Pointer::Down, row);
    f.pointer(Pointer::Up, row);
    f.sync();
    punch = f.app->punch();
    CHECK(punch.start == doctest::Approx(8.0));
    CHECK(punch.end == doctest::Approx(10.0));
    CHECK(punch.enabled);
    CHECK(f.history() == n);  // transport state, never an undo step
    CHECK(f.errors.empty());
}

TEST_CASE("ruler: an edge dragged onto the other removes a range; the menu deletes ranges") {
    RulerFixture f;
    // A punch range at 2 s .. 6 s turns punch on; its left edge dragged past
    // its right one removes it and turns punch off.
    f.drag(f.at(4.0, 30), f.at(12.0, 30));
    REQUIRE(f.app->punch().enabled);
    f.drag(f.at(4.0, 30), f.at(16.0, 30));
    CHECK_FALSE(f.app->punch().enabled);
    CHECK_FALSE(f.app->punch().end > f.app->punch().start);
    CHECK_FALSE(f.view->punchEnd() > f.view->punchStart());
    // Right-click in the ruler: Delete loop (row 4), Delete punch (row 5),
    // Delete loop and punch (row 6).
    const auto choose = [&](int index) {
        ui::PointerEvent e;
        e.type = Pointer::Down;
        e.pos = f.at(1.0, 30);
        e.button = 3;
        f.root->pointer(e);
        e.type = Pointer::Up;
        f.root->pointer(e);
        f.root->frame(false);
        ui::Node* menu = f.root->find("ruler.menu");
        REQUIRE(menu);
        const ui::Point row{menu->bounds().x + 30, menu->bounds().y + index * 26.0f + 13};
        f.pointer(Pointer::Move, row);
        f.pointer(Pointer::Down, row);
        f.pointer(Pointer::Up, row);
        f.sync();
    };
    f.loop(2, 4, true);
    f.drag(f.at(4.0, 30), f.at(12.0, 30));
    choose(5);
    CHECK_FALSE(f.app->punch().enabled);
    CHECK_FALSE(f.app->punch().end > f.app->punch().start);
    CHECK(f.app->loop().enabled);  // the loop is untouched
    choose(4);
    CHECK_FALSE(f.app->loop().enabled);
    CHECK_FALSE(f.app->loop().end > f.app->loop().start);
    f.loop(2, 4, true);
    f.drag(f.at(4.0, 30), f.at(12.0, 30));
    choose(6);
    CHECK_FALSE(f.app->loop().end > f.app->loop().start);
    CHECK_FALSE(f.app->punch().enabled);
    CHECK_FALSE(f.app->punch().end > f.app->punch().start);
    CHECK(f.errors.empty());
}
