// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher pane (gui.md §4.15; ADR-0097 step L3), driven through the
// same pointer paths as the app.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include <stb_image.h>
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app_actions.h"
#include "launch_capture.h"
#include "shell.h"
#include "supple/audio/audio_file.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"

using namespace supple;

namespace {
const cmd::Origin kLauncherUi{cmd::Origin::Type::Ui, "gui", {}};
using Pointer = ui::PointerEvent::Type;
// Geometry is in floats; doctest compares in doubles.
template <typename T>
doctest::Approx near(T value) {
    return doctest::Approx(static_cast<double>(value));
}

struct LauncherFixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<peaks::PeakService> peaks;
    std::unique_ptr<ui::Root> root;
    views::ArrangeView* view = nullptr;
    std::vector<std::string> commands;
    std::vector<Error> errors;
    std::vector<std::string> notices;  // the status line's notes (onNotice)
    std::vector<model::Id> opened, selected, takeFx;

    LauncherFixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-launcher-test-" + std::to_string(testProcessId()));
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        REQUIRE(p);
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"launcher-test", 1100, 600, true});
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
        peaks = std::make_unique<peaks::PeakService>(dir / "cache", [](std::function<void()>) {});
        root = std::make_unique<ui::Root>(*window, *fonts, ui::Theme::dark());
        auto arrange = std::make_unique<views::ArrangeView>(
            *peaks, [](const model::MediaSource& s) { return std::filesystem::path(s.path); });
        view = arrange.get();
        root->setContent(std::move(arrange));
        view->setLauncherCallbacks(
            {[this](const std::string& command, json::Value args) {
                 commands.push_back(command);
                 return dispatcher->execute({command, std::move(args), kLauncherUi});
             },
             [this](const Error& error) { errors.push_back(error); },
             [this](model::Id clip) { opened.push_back(clip); },
             [this](const std::vector<model::Id>& clips) { selected = clips; },
             {},
             {},
             [this](model::Id take) { takeFx.push_back(take); },
             [this](const std::string& notice) { notices.push_back(notice); }});
        sync();
    }
    ~LauncherFixture() {
        root.reset();
        peaks.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    void sync() {
        view->show(app->head());
        root->frame(false);
    }
    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kLauncherUi});
        INFO(command << ": " << (r ? std::string() : r.error().toString()));
        REQUIRE(r);
        sync();
        return *r;
    }
    // The song beat the transport is at while it plays, else -1.
    double playingBeat() {
        const auto t = app->transport("get");
        return t && t->playing ? t->beat : -1;
    }
    // Runs the app's main-loop service (EngineApp::tick, which services the
    // engine: a launch's ack reaches the launch log there, so launchState()
    // can report it heard), then `pass`, until `done`. The engine decides: a
    // launch placed at `beat` is heard before the transport has played a
    // further bar, so that fails it. The wall-clock bound only stops an
    // engine that does not run at all. A failure prints launch.state and the
    // transport, as the engine reports them.
    bool runUntil(const std::function<bool()>& done, double beat, const std::string& what,
                  const std::function<void()>& pass = {}) {
        const auto start = std::chrono::steady_clock::now();
        std::string why;
        for (;;) {
            app->tick();
            if (pass) pass();
            if (done()) return true;
            if (const double at = playingBeat(); at > beat + 4) {  // a 4/4 bar past it
                why = "the transport reached beat " + std::to_string(at) + " without it";
                break;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(2)) {
                why = "the engine did not reach a bar past beat " + std::to_string(beat) +
                      " in two minutes (a hang guard, not a timing check)";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto state = dispatcher->execute({"launch.state", json::Value::object(), kLauncherUi});
        auto transport = dispatcher->execute({"transport.get", json::Value::object(), kLauncherUi});
        MESSAGE(
            what << ": " << why << "\n  launch.state "
                 << (state ? json::writeCompact(*state) : state.error().toString())
                 << "\n  transport "
                 << (transport ? json::writeCompact(*transport) : transport.error().toString()));
        return false;
    }
    void pointer(Pointer type, ui::Point p, unsigned mods = 0, int clicks = 1, int button = 1) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = button;
        e.mods = mods;
        e.clicks = clicks;
        root->pointer(e);
    }
    void click(ui::Point p, unsigned mods = 0, int clicks = 1, int button = 1) {
        pointer(Pointer::Down, p, mods, clicks, button);
        pointer(Pointer::Up, p, mods, clicks, button);
        sync();
    }
    void drag(ui::Point from, ui::Point to, unsigned mods = 0) {
        pointer(Pointer::Down, from, mods);
        pointer(Pointer::Move, {(from.x + to.x) / 2, (from.y + to.y) / 2}, mods);
        pointer(Pointer::Move, to, mods);
        pointer(Pointer::Up, to, mods);
        sync();
    }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        root->damageAll();
        root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, root->readBack());
    }
    // The window against a committed image (tests/golden; SUPPLE_UPDATE_GOLDEN
    // rewrites it): at most 1 pixel in 200 off by more than 48 in a channel.
    // Elsewhere than Linux the drivers differ: the frame must only be drawn.
    void golden(const char* name) {
        root->damageAll();
        root->frame(false);
        const gui::Image image = root->readBack();
#if !defined(__linux__)
        REQUIRE(image.width > 0);
        REQUIRE(image.height > 0);
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
    // A song with three channels, a buss, three scenes and clips in some cells.
    void song() {
        for (const char* name : {"Drums", "Bass", "Keys"})
            exec("track.add", std::string(R"({"type":"channel","name":")") + name + R"("})");
        exec("track.add", R"({"type":"buss","name":"Verb"})");
        for (const char* name : {"Intro", "Verse", "Chorus"})
            exec("scene.add", std::string(R"({"name":")") + name + R"("})");
        exec("clip.add",
             R"({"track":"tr_1","scene":"sc_1","notation":"voice d; 1: C2:q D2:q C2:q D2:q"})");
        exec("clip.add",
             R"({"track":"tr_1","scene":"sc_2","notation":"voice d; 1: C2:e C2:e D2:h."})");
        exec("clip.add", R"({"track":"tr_2","scene":"sc_2","notation":"voice b; 1: C2:h G1:h"})");
        exec("clip.set", R"({"clip":"cl_1","name":"Kit A"})");
        exec("clip.set", R"({"clip":"cl_2","name":"Kit B"})");
        exec("clip.set", R"({"clip":"cl_3","name":"Root"})");
    }
    std::size_t row(const char* track) {
        auto r = view->trackLayout().indexOf(*model::Id::parse(track));
        REQUIRE(r);
        return *r;
    }
};
}  // namespace

TEST_CASE("launcher: hidden without scenes; with them, a column per scene on the track rows") {
    LauncherFixture f;
    f.exec("track.add", R"({"type":"channel","name":"Lead"})");
    CHECK_FALSE(f.view->launcherShown());
    CHECK_FALSE(f.view->launcherPane().visible);
    const float lanesX = f.view->lanesRect().x;
    f.snapshot("launcher-hidden.png");
    f.song();
    REQUIRE(f.view->launcherShown());
    views::LauncherPane& pane = f.view->launcherPane();
    views::LauncherHeader& header = f.view->launcherHeader();
    // Between the track panel and the lanes, wide enough for its columns
    // (three scenes, the + cell and the Arr column), its header cell above it.
    CHECK(pane.bounds().x == near(lanesX));
    CHECK(pane.bounds().w == near(3 * 64 + 2 * 22));
    CHECK(f.view->lanesRect().x == near(pane.bounds().right()));
    CHECK(header.bounds().x == near(pane.bounds().x));
    CHECK(header.bounds().w == near(pane.bounds().w));
    CHECK(header.bounds().bottom() == near(pane.bounds().y));
    // The scene headers sit in the ruler band's lowest row.
    CHECK(header.scenesRow().h == near(f.view->rulerRows().marker));
    // One row model: a cell's top and height are its track row's.
    for (const char* track : {"tr_1", "tr_2", "tr_3"}) {
        const std::size_t r = f.row(track);
        CHECK(pane.cellRect(r, 0).y == near(f.view->rowRect(r).y));
        CHECK(pane.cellRect(r, 0).h == near(f.view->rowRect(r).h));
    }
    CHECK(pane.cellRect(f.row("tr_1"), 1).x == near(pane.bounds().x + 64));
    // Channels have cells (the buss none): three clips and six empty cells.
    CHECK(pane.stats.cells == 12);  // Lead, Drums, Bass and Keys
    CHECK(pane.stats.clips == 3);
    CHECK(pane.stats.empty == 9);
    CHECK(pane.stats.names == std::vector<std::string>{"Kit A", "Kit B", "Root"});
    // Each clip's content by the shared item-body drawing: 4 + 3 + 2 notes.
    CHECK(pane.stats.notes == 9);
    f.snapshot("launcher-pane.png");
}

TEST_CASE(
    "launcher: View > Clip launcher chooses; a drag sets the width, all the way fills the view") {
    LauncherFixture f;
    f.song();
    views::LauncherPane& pane = f.view->launcherPane();
    // The choice beats the project's rule, and the UI file keeps it.
    f.view->setLauncherShown(false);
    f.sync();
    CHECK_FALSE(pane.visible);
    json::Value saved = f.view->saveLayout();
    REQUIRE(saved.find("clips"));
    CHECK_FALSE(saved["clips"]["shown"].asBool());
    f.view->loadLayout(json::Value::object());  // a project that never chose
    f.sync();
    CHECK(pane.visible);
    // A drag on the pane's right edge sets its width.
    const float edge = pane.bounds().right() - 2;
    const float y = pane.bounds().y + 200;
    f.drag({edge, y}, {edge + 60, y});
    CHECK(pane.bounds().w == near(3 * 64 + 2 * 22 + 60));
    CHECK(f.view->lanesRect().x == near(pane.bounds().right()));
    // Past the room the lanes need, it fills the view: the lanes and the
    // ruler hide; a drag back brings them.
    f.drag({pane.bounds().right() - 2, y}, {1090, y});
    CHECK(f.view->launcherFull());
    CHECK_FALSE(f.view->ruler().visible);
    saved = f.view->saveLayout();
    CHECK(saved["clips"]["full"].asBool());
    f.drag({pane.bounds().right() - 2, y}, {500, y});
    CHECK_FALSE(f.view->launcherFull());
    CHECK(f.view->ruler().visible);
    CHECK(f.view->lanesRect().w > 100);
    CHECK(f.view->lanesRect().x == near(pane.bounds().right()));
    // A reopened project gets the width back.
    saved = f.view->saveLayout();
    const float width = pane.bounds().w;
    f.view->loadLayout(json::Value::object());
    f.sync();
    CHECK(pane.bounds().w == near(3 * 64 + 2 * 22));
    f.view->loadLayout(saved);
    f.sync();
    CHECK(pane.bounds().w == near(width));
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: playing, held and queued cells, the Arr dots, a playing scene and dim lanes") {
    LauncherFixture f;
    f.song();
    views::LauncherPane& pane = f.view->launcherPane();
    const model::Id kit = *model::Id::parse("cl_1"), kitB = *model::Id::parse("cl_2");
    const model::Id root = *model::Id::parse("cl_3");
    const model::Id drums = *model::Id::parse("tr_1"), bass = *model::Id::parse("tr_2");
    // Nothing plays: every channel follows the arrangement.
    CHECK(pane.stats.arrLit == 3);
    CHECK(pane.stats.playing == 0);
    // Drums play Kit A with Kit B queued; the bass holds its ended once clip.
    views::LaunchViews state;
    state[drums] = {views::LaunchView::Plays::Clip, kit, 1.0, views::LaunchView::Queued::Clip,
                    kitB};
    state[bass] = {views::LaunchView::Plays::Held, root, std::nullopt,
                   views::LaunchView::Queued::None, std::nullopt};
    f.view->setLaunchState(state);
    f.sync();
    CHECK(pane.stats.playing == 1);
    CHECK(pane.stats.held == 1);
    CHECK(pane.stats.queued == 1);
    CHECK(pane.stats.arrLit == 1);  // Keys alone follows the arrangement
    CHECK(f.view->trackLayout().heldTracks == std::set<model::Id>{drums, bass});
    // Verse plays when every one of its clips does.
    const model::Id verse = *model::Id::parse("sc_2");
    CHECK_FALSE(views::scenePlaying(f.app->head(), state, verse));
    state[drums] = {views::LaunchView::Plays::Clip, kitB, 0.0, views::LaunchView::Queued::None,
                    std::nullopt};
    state[bass].plays = views::LaunchView::Plays::Clip;
    f.view->setLaunchState(state);
    f.sync();
    CHECK(views::scenePlaying(f.app->head(), state, verse));
    f.snapshot("launcher-playing.png");
    f.view->setLaunchState({});
    f.sync();
    CHECK(f.view->trackLayout().heldTracks.empty());
    CHECK(pane.stats.arrLit == 3);
}

TEST_CASE("launcher: the engine's launch state reaches the pane (launch.clip, dummy audio)") {
    LauncherFixture f;
    f.song();
    f.exec("engine.start", R"({"backend":"dummy"})");
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    // Until the engine hears it, as the app's loop sees it each pass.
    CHECK(f.runUntil(
        [&] {
            f.view->setLaunchState(gui::launchViews(f.app->launchState()));
            f.sync();
            return f.view->launcherPane().stats.playing == 1;
        },
        at, "Kit A heard"));
    const auto& state = f.view->launchState();
    const auto it = state.find(*model::Id::parse("tr_1"));
    REQUIRE(it != state.end());
    CHECK(it->second.clip == model::Id::parse("cl_1"));
    f.exec("engine.stop", "{}");
}

namespace {
ui::Point centre(const ui::Rect& r) { return {r.x + r.w / 2, r.y + r.h / 2}; }
}  // namespace

TEST_CASE("launcher: click zones: a clip's ▶ launches, its body selects, the square stops") {
    LauncherFixture f;
    f.song();
    f.exec("engine.start", R"({"backend":"dummy"})");
    views::LauncherPane& pane = f.view->launcherPane();
    const std::size_t drums = f.row("tr_1"), bass = f.row("tr_2"), keys = f.row("tr_3");
    const model::Id kitA = *model::Id::parse("cl_1"), kitB = *model::Id::parse("cl_2");
    // Empty cells show the stop square only on tracks with clips: one on the
    // Drums, two on the Bass; the Keys have no clips, so nothing to stop.
    CHECK(pane.stats.squares == 3);
    // A click on a body selects it and launches nothing.
    f.click(centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 1))));
    CHECK(f.view->selectedClips() == std::vector<model::Id>{kitB});
    CHECK(f.selected == std::vector<model::Id>{kitB});
    CHECK(f.commands.empty());
    // Ctrl adds one.
    f.click(centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 0))), ui::kCtrl);
    CHECK(f.view->selectedClips() == std::vector<model::Id>{kitB, kitA});
    CHECK(pane.stats.selected == 2);
    f.snapshot("launcher-selected.png");
    // A click on a ▶ launches and keeps the selection.
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(drums, 0))));
    CHECK(f.commands == std::vector<std::string>{"launch.clip"});
    CHECK(f.view->selectedClips() == std::vector<model::Id>{kitB, kitA});
    // An empty cell's square stops the track and selects nothing.
    f.commands.clear();
    f.click(centre(views::LauncherPane::stopZone(pane.cellRect(bass, 0))));
    CHECK(f.commands == std::vector<std::string>{"launch.stop"});
    CHECK(f.view->selectedClips() == std::vector<model::Id>{kitB, kitA});
    CHECK_FALSE(f.view->selectedCell());
    // Beside the square, a click selects the cell and stops nothing; so does
    // the middle of a cell on a track without clips.
    f.commands.clear();
    f.click({pane.cellRect(bass, 2).x + 8, pane.cellRect(bass, 2).bottom() - 8});
    CHECK(f.commands.empty());
    CHECK(f.view->selectedCell()->scene == model::Id::parse("sc_3"));
    const ui::Rect cell = pane.cellRect(keys, 0);
    f.click(centre(views::LauncherPane::stopZone(cell)));
    CHECK(f.commands.empty());
    REQUIRE(f.view->selectedCell());
    CHECK(f.view->selectedCell()->scene == model::Id::parse("sc_1"));
    CHECK(f.view->selectedClips().empty());
    CHECK(pane.stats.selected == 1);
    // A press on the lanes clears it.
    f.click({f.view->lanesRect().x + 300, f.view->lanesRect().y + 400});
    CHECK_FALSE(f.view->selectedCell());
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a double-click opens a clip, or makes a one-bar MIDI clip and opens it") {
    LauncherFixture f;
    f.song();
    views::LauncherPane& pane = f.view->launcherPane();
    const std::size_t drums = f.row("tr_1"), keys = f.row("tr_3");
    f.click(centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 0))), 0, 2);
    CHECK(f.opened == std::vector<model::Id>{*model::Id::parse("cl_1")});
    const ui::Rect cell = pane.cellRect(keys, 2);
    f.click({cell.x + 8, cell.bottom() - 8}, 0, 2);
    const auto made =
        views::clipIn(f.app->head(), *model::Id::parse("tr_3"), *model::Id::parse("sc_3"));
    REQUIRE(made);
    const model::Clip* clip = f.app->head().clips.find(*made);
    CHECK(clip->kind == model::ClipKind::Midi);
    CHECK(clip->length == doctest::Approx(4));  // one 4/4 bar
    CHECK(f.opened.back() == *made);
    CHECK(f.view->selectedClips() == std::vector<model::Id>{*made});
    CHECK(f.commands == std::vector<std::string>{"clip.add"});
}

TEST_CASE("launcher: a clip drags to an empty cell (Ctrl copies); an occupied one refuses it") {
    LauncherFixture f;
    f.song();
    views::LauncherPane& pane = f.view->launcherPane();
    const std::size_t drums = f.row("tr_1"), bass = f.row("tr_2"), keys = f.row("tr_3");
    const ui::Point kitA = centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 0)));
    // Onto Root's cell: refused, nothing changes.
    f.drag(kitA, centre(pane.cellRect(bass, 1)));
    CHECK(f.commands.empty());
    // Onto the Keys' empty Chorus cell: moved.
    const ui::Rect target = pane.cellRect(keys, 2);
    f.drag(kitA, {target.x + 8, target.bottom() - 8});
    CHECK(f.commands == std::vector<std::string>{"clip.move"});
    const model::Clip* moved = f.app->head().clips.find(*model::Id::parse("cl_1"));
    CHECK(moved->track == model::Id::parse("tr_3"));
    CHECK(moved->scene == model::Id::parse("sc_3"));
    // With Ctrl, Kit B is copied into the Drums' Chorus cell and the copy selected.
    f.commands.clear();
    const ui::Rect empty = pane.cellRect(drums, 2);
    f.drag(centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 1))),
           {empty.x + 8, empty.bottom() - 8}, ui::kCtrl);
    CHECK(f.commands == std::vector<std::string>{"clip.duplicate"});
    const auto copy =
        views::clipIn(f.app->head(), *model::Id::parse("tr_1"), *model::Id::parse("sc_3"));
    REQUIRE(copy);
    CHECK(f.view->selectedClips() == std::vector<model::Id>{*copy});
    CHECK(f.app->head().clips.find(*model::Id::parse("cl_2")));  // the original stays
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: the header's buttons, scene ▶, rename, move, + and the scene and clip menus") {
    LauncherFixture f;
    f.song();
    f.exec("engine.start", R"({"backend":"dummy"})");
    views::LauncherHeader& header = f.view->launcherHeader();
    views::LauncherPane& pane = f.view->launcherPane();
    f.click(centre(header.stopRect()));
    f.click(centre(header.arrangementRect()));
    const ui::Rect verse = header.sceneRect(1);
    f.click({verse.x + 8, verse.y + verse.h / 2});
    CHECK(f.commands ==
          std::vector<std::string>{"launch.stop", "launch.arrangement", "launch.scene"});
    // A double-click on a name renames the scene.
    f.commands.clear();
    f.click({verse.x + 40, verse.y + verse.h / 2}, 0, 2);
    REQUIRE(f.root->find("launcher-name"));
    f.root->frame(false);
    f.root->text("Hook");
    f.root->key(SDLK_RETURN);
    f.sync();
    CHECK(f.app->head().scenes.find(*model::Id::parse("sc_2"))->name == "Hook");
    // A drag moves the column: Intro to the end.
    const ui::Rect intro = header.sceneRect(0);
    f.drag({intro.x + 40, intro.y + 8}, {header.sceneRect(2).x + 40, intro.y + 8});
    CHECK(views::launcherColumns(f.app->head()).scenes.back() == model::Id::parse("sc_1"));
    // + adds a scene at the end.
    f.click(centre(header.addRect()));
    CHECK(f.app->head().scenes.size() == 4);
    // The scene menu.
    f.click({header.sceneRect(0).x + 40, intro.y + 8}, 0, 1, 3);
    ui::Node* menu = f.root->find("launcher.scene.menu");
    REQUIRE(menu);
    std::vector<std::string> rows;
    for (const auto& item : ui::menuItems(*menu)) rows.push_back(item.label);
    CHECK(rows == std::vector<std::string>{"Rename…", "Colour…", "Insert at playhead",
                                           "Insert at end", "", "Delete"});
    f.root->closePopups();
    // The clip menu: its rows, and Mode sets the clip's.
    const std::size_t drums = f.row("tr_1");
    f.click(centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 2))), 0, 1, 3);
    menu = f.root->find("launcher.clip.menu");
    REQUIRE(menu);
    rows.clear();
    for (const auto& item : ui::menuItems(*menu)) rows.push_back(item.label);
    CHECK(rows == std::vector<std::string>{"Rename…", "Colour…", "", "Mode: Loop", "Mode: Once",
                                           "Quantize…", "", "Duplicate",
                                           "Lay on timeline at playhead", "Take FX…", "Delete"});
    const auto once = ui::menuItems(*menu)[4].run;
    const auto takeFx = ui::menuItems(*menu)[9].run;
    f.root->closePopups();
    // Take FX… hands the app the clip's active take (it opens that chain).
    takeFx();
    {
        const auto fill = views::clipIn(f.app->head(), *model::Id::parse("tr_1"),
                                        views::launcherColumns(f.app->head()).scenes[2]);
        REQUIRE(fill);
        REQUIRE(f.takeFx.size() == 1);
        CHECK(f.takeFx.front() == f.app->head().clips.find(*fill)->activeTake);
    }
    once();
    f.sync();
    const auto clip = views::clipIn(f.app->head(), *model::Id::parse("tr_1"),
                                    views::launcherColumns(f.app->head()).scenes[2]);
    REQUIRE(clip);
    CHECK(f.app->head().clips.find(*clip)->mode == model::ClipMode::Once);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE(
    "launcher: a clip drags onto the timeline as an item; an item into an empty cell as a clip") {
    LauncherFixture f;
    f.song();
    views::LauncherPane& pane = f.view->launcherPane();
    const std::size_t drums = f.row("tr_1"), keys = f.row("tr_3");
    const model::Id keysTrack = *model::Id::parse("tr_3");
    auto beatX = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    // Kit A onto the Keys' lane at beat 4: a ghost while held, an item on drop.
    const ui::Point from = centre(views::LauncherPane::bodyRect(pane.cellRect(drums, 0)));
    const ui::Rect lane = f.view->rowRect(keys);
    const ui::Point to{beatX(4.0) + 1, lane.y + lane.h / 2};
    REQUIRE(f.view->lanesRect().contains(to));
    f.pointer(Pointer::Down, from);
    f.pointer(Pointer::Move, {from.x + 12, from.y});
    f.pointer(Pointer::Move, to);
    const auto& ghost = f.view->gestures().clipGhost();
    REQUIRE(ghost);
    CHECK(ghost->track == keysTrack);
    CHECK(ghost->start == doctest::Approx(4));
    const double length = f.app->head().clips.find(*model::Id::parse("cl_1"))->length;
    CHECK(ghost->end == doctest::Approx(4 + length));
    f.pointer(Pointer::Up, to);
    f.sync();
    CHECK_FALSE(f.view->gestures().clipGhost());
    CHECK(f.commands == std::vector<std::string>{"item.add"});
    const model::Item* laid = nullptr;
    for (const auto& e : f.app->head().items)
        if (e.value->track == keysTrack) laid = &*e.value;
    REQUIRE(laid);
    CHECK(model::itemSpan(f.app->head(), *laid).startBeat == doctest::Approx(4));
    CHECK(f.app->head().clips.find(*model::Id::parse("cl_1")));  // the clip stays

    // That item into the Keys' empty Intro cell: outlined while held, a clip on drop.
    f.commands.clear();
    const std::size_t items = f.app->head().items.size();
    const ui::Point grab{beatX(4.5), lane.y + 8};
    const ui::Rect cell = pane.cellRect(keys, 0);
    const ui::Point into{cell.x + 8, cell.bottom() - 8};
    f.pointer(Pointer::Down, grab);
    f.pointer(Pointer::Move, {grab.x - 20, grab.y});
    f.pointer(Pointer::Move, into);
    f.sync();
    CHECK(pane.stats.dropShown == 1);
    f.pointer(Pointer::Up, into);
    f.sync();
    CHECK(pane.stats.dropShown == 0);
    CHECK(f.commands == std::vector<std::string>{"clip.add"});
    const auto made = views::clipIn(f.app->head(), keysTrack, *model::Id::parse("sc_1"));
    REQUIRE(made);
    CHECK(f.view->selectedClips() == std::vector<model::Id>{*made});
    CHECK(f.app->head().items.size() == items);  // the item stays where it was
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: the pane's look, playing, queued, held, empty and selected, in both themes") {
    LauncherFixture f;
    f.song();
    const model::Id drums = *model::Id::parse("tr_1"), bass = *model::Id::parse("tr_2");
    views::LaunchViews state;
    state[drums] = {views::LaunchView::Plays::Clip, *model::Id::parse("cl_1"), 2.0,
                    views::LaunchView::Queued::Clip, *model::Id::parse("cl_2")};
    state[bass] = {views::LaunchView::Plays::Held, *model::Id::parse("cl_3"), std::nullopt,
                   views::LaunchView::Queued::None, std::nullopt};
    f.view->setLaunchState(state);
    f.view->selectCell(views::ArrangeView::Cell{bass, *model::Id::parse("sc_3")});
    f.view->setCaptureArmed(true);  // Capture lit
    f.sync();
    f.golden("launcher-dark.png");
    f.root->setTheme(ui::Theme::light());
    f.sync();
    f.golden("launcher-light.png");
}

TEST_CASE("launcher: a playing clip's ▶ reads on every clip colour, in both themes") {
    LauncherFixture f;
    f.exec("scene.add", R"({"name":"Play"})");
    views::LaunchViews state;
    int i = 0;
    for (const std::string_view colour : model::kTrackColors) {
        ++i;
        f.exec("track.add", R"({"type":"channel","name":")" + std::string(colour) + R"("})");
        const std::string track = "tr_" + std::to_string(i), clip = "cl_" + std::to_string(i);
        f.exec("clip.add", R"({"track":")" + track +
                               R"(","scene":"sc_1","notation":"voice v; 1: C4:q E4:q G4:h"})");
        f.exec("clip.set", R"({"clip":")" + clip + R"(","color":")" + std::string(colour) +
                               R"(","name":")" + std::string(colour) + R"("})");
        state[*model::Id::parse(track)] = {views::LaunchView::Plays::Clip, *model::Id::parse(clip),
                                           1.0 + 0.3 * i, views::LaunchView::Queued::None,
                                           std::nullopt};
    }
    f.view->setLaunchState(state);
    f.sync();
    CHECK(f.view->launcherPane().stats.playing == model::kTrackColors.size());
    f.golden("launcher-playing-dark.png");
    f.root->setTheme(ui::Theme::light());
    f.sync();
    f.golden("launcher-playing-light.png");
}

TEST_CASE("launcher: Capture arms while lit; a stop commits the performance, one undo step") {
    LauncherFixture f;
    f.song();
    views::LauncherHeader& header = f.view->launcherHeader();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    // The header's toggle asks the app; the app's state lights it.
    std::vector<bool> asked;
    views::LauncherCallbacks callbacks = f.view->launcherCallbacks();
    callbacks.capture = [&](bool on) { asked.push_back(on); };
    f.view->setLauncherCallbacks(callbacks);
    f.click(centre(header.captureRect()));
    CHECK(asked == std::vector<bool>{true});
    CHECK_FALSE(header.stats.captureLit);
    f.view->setCaptureArmed(true);
    f.sync();
    CHECK(header.stats.captureLit);
    f.click(centre(header.captureRect()));
    CHECK(asked == std::vector<bool>{true, false});
    f.view->setCaptureArmed(false);
    // Without an engine: refused, dark.
    const Status cold = capture.set(true);
    REQUIRE_FALSE(cold);
    CHECK(cold.error().code == "engine.stopped");
    CHECK_FALSE(capture.armed());
    // Armed, Kit A launched (the transport starts), then a stop: committed.
    f.exec("engine.start", R"({"backend":"dummy"})");
    REQUIRE(capture.set(true));
    CHECK(capture.armed());
    CHECK(f.app->launchCapturing());
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    // Until the engine hears it, and a beat after, as the app's loop sees it.
    const auto service = [&] { capture.service(); };
    REQUIRE(f.runUntil(playing, at, "Kit A heard", service));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at, "a beat of Kit A", service));
    CHECK(f.app->head().items.empty());
    const std::uint64_t before = f.app->head().version;
    // A stop ends the capture on the main thread (the transport relocates):
    // the next pass commits it.
    f.exec("transport.stop", "{}");
    f.app->tick();
    capture.service();
    CHECK(refused.empty());
    bool laid = false;
    for (const auto& e : f.app->head().items) laid = laid || e.value->track == drums;
    CHECK(laid);
    CHECK(f.app->head().version == before + 1);  // one undo step
    // Committed while stopped: the tracks return to the arrangement where
    // playing starts, so play plays the take, not Kit A again.
    using Plays = engine::Engine::TrackLaunch::Plays;
    auto drumsNow = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums) return t;
        return engine::Engine::TrackLaunch{};
    };
    {
        const auto now = drumsNow();
        CHECK((now.plays == Plays::Arrangement ||
               (now.queued &&
                now.queued->request.action == engine::LaunchLogEntry::Action::Arrangement)));
    }
    // Still lit, and armed for the next take.
    CHECK(capture.armed());
    CHECK(f.app->launchCapturing());
    // Off with nothing played since: nothing more is written.
    REQUIRE(capture.set(false));
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK(f.app->head().version == before + 1);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a refused capture commit keeps the performance; a click commits it") {
    LauncherFixture f;
    f.song();
    // A locked arrangement item on Drums, under the whole capture: the commit
    // that clears it is refused (item.locked).
    f.exec(
        "item.add",
        R"({"track":"tr_1","bar":1,"notation":"voice d; 1: C2:w | C2:w | C2:w | C2:w | )"
        R"(C2:w | C2:w | C2:w | C2:w | C2:w | C2:w | C2:w | C2:w | C2:w | C2:w | C2:w | C2:w"})");
    REQUIRE(f.app->head().items.size() == 1);
    const std::string item = f.app->head().items.begin()->id.str();
    auto lock = [&](bool on) {
        f.exec("item.set",
               R"({"item":")" + item + R"(","locked":)" + (on ? "true" : "false") + "}");
    };
    lock(true);
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    const model::Id drums = *model::Id::parse("tr_1");
    // Kit A launched and heard, then a stop. The bounds only keep a broken
    // build from hanging.
    auto perform = [&](const std::string& take) {
        const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
        auto playing = [&] {
            for (const auto& t : f.app->launchState())
                if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                    return true;
            return false;
        };
        const auto service = [&] { capture.service(); };
        REQUIRE(f.runUntil(playing, at, take + ": Kit A heard", service));
        REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at,
                           take + ": a beat of Kit A", service));
        // The stop ends the capture on the main thread; the next pass commits.
        f.exec("transport.stop", "{}");
        f.app->tick();
        capture.service();
    };
    auto heldFrames = [&] {
        const auto held = f.app->engine()->launchCapture();
        return held && !held->launches.empty() ? held->frames : 0;
    };
    REQUIRE(capture.set(true));
    const std::uint64_t armedAt = f.app->head().version;
    perform("the first take");
    // Refused: nothing written, the toggle dark, the performance kept.
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].code == "item.locked");
    CHECK(refused[0].message.find("the performance is kept") != std::string::npos);
    CHECK(f.app->head().version == armedAt);
    CHECK_FALSE(capture.armed());
    CHECK(capture.kept());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK(heldFrames() > 0);
    // A click while it is still locked: refused again, still kept, dark.
    const Status again = capture.set(true);
    REQUIRE_FALSE(again);
    CHECK(again.error().code == "item.locked");
    CHECK(again.error().message.find("click Capture again to drop it") != std::string::npos);
    CHECK_FALSE(capture.armed());
    CHECK(capture.kept());
    CHECK(heldFrames() > 0);
    // Unlocked, a click commits it in one undo step, then arms the next.
    lock(false);
    const std::uint64_t unlocked = f.app->head().version;
    REQUIRE(capture.set(true));
    CHECK(f.app->head().version == unlocked + 1);
    CHECK_FALSE(capture.kept());
    CHECK(capture.armed());
    CHECK(f.app->launchCapturing());
    bool laid = false;
    for (const auto& e : f.app->head().items)
        laid = laid || (e.value->track == drums && e.id.str() != item);
    CHECK(laid);
    // Refused at stop and at the click after it, a third click drops it and
    // arms: nothing is written.
    for (const auto& e : f.app->head().items)
        if (e.value->track == drums)
            f.exec("item.set", R"({"item":")" + e.id.str() + R"(","locked":true})");
    perform("the second take");
    REQUIRE(refused.size() == 2);
    CHECK_FALSE(capture.armed());
    const std::uint64_t locked = f.app->head().version;
    REQUIRE_FALSE(capture.set(true));
    REQUIRE(capture.set(true));
    CHECK_FALSE(capture.kept());
    CHECK(capture.armed());
    CHECK(f.app->head().version == locked);
    REQUIRE(capture.set(false));
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: the Capture cell is lit by an agent's capture; a click ends and commits it") {
    LauncherFixture f;
    f.song();
    views::LauncherHeader& header = f.view->launcherHeader();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    views::LauncherCallbacks callbacks = f.view->launcherCallbacks();
    callbacks.capture = [&](bool on) { REQUIRE(capture.set(on)); };
    f.view->setLauncherCallbacks(callbacks);
    auto show = [&] {  // as the shell does once a pass
        capture.service();
        f.view->setCaptureArmed(capture.lit());
        f.sync();
    };
    f.exec("engine.start", R"({"backend":"dummy"})");
    show();
    CHECK_FALSE(header.stats.captureLit);
    // Another caller arms it: the cell lights, though the toggle did not.
    f.exec("launch.capture", R"({"on":true})");
    show();
    CHECK_FALSE(capture.armed());
    CHECK(capture.lit());
    CHECK(header.stats.captureLit);
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    REQUIRE(f.runUntil(playing, at, "Kit A heard", show));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at, "a beat of Kit A", show));
    // A click on the lit cell ends that capture and commits it.
    const std::uint64_t before = f.app->head().version;
    f.click(centre(header.captureRect()));
    show();
    CHECK_FALSE(f.app->launchCapturing());
    CHECK_FALSE(capture.lit());
    CHECK_FALSE(header.stats.captureLit);
    CHECK(f.app->head().version == before + 1);
    bool laid = false;
    for (const auto& e : f.app->head().items) laid = laid || e.value->track == drums;
    CHECK(laid);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a pitch-keeping master playrate dims the audio clips the engine refuses") {
    // The lead's b3af83f5: clips launch under a master playrate; only an
    // audio clip while it keeps the pitch is refused (launch.playrate). The
    // pane dims exactly those, and the scenes whose launch would start one.
    LauncherFixture f;
    f.song();
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(1, std::vector<float>(96000, 0.25f));
    const auto wav = f.dir / "tone.wav";
    REQUIRE(audio::writeWav(wav, tone));
    json::Value add = json::Value::object();
    add["track"] = "tr_3";
    add["scene"] = "sc_3";
    add["audio_file"] = wav.string();
    const std::string pad = f.exec("clip.add", *json::write(add))["clip"].asString();
    f.exec("engine.start", R"({"backend":"dummy"})");
    views::LauncherPane& pane = f.view->launcherPane();
    views::LauncherHeader& header = f.view->launcherHeader();
    auto draw = [&] {
        f.root->damageAll();
        f.root->frame(false);
    };
    draw();
    CHECK(pane.stats.refused == 0);
    CHECK(header.stats.refused == 0);
    // A rate that moves the pitch: every clip launches, nothing dims.
    f.exec("transport.playrate.set", R"({"rate":1.5})");
    draw();
    CHECK(pane.stats.refused == 0);
    CHECK(header.stats.refused == 0);
    // Keeping the pitch: the audio clip and Chorus's ▶ dim, the MIDI clips
    // and the other scenes do not.
    f.exec("transport.playrate.set", R"({"rate":1.5,"preserve_pitch":true})");
    const model::Session& s = f.app->head();
    CHECK(views::launchRefused(s, *s.clips.find(*model::Id::parse(pad))));
    CHECK_FALSE(views::launchRefused(s, *s.clips.find(*model::Id::parse("cl_1"))));
    CHECK(views::sceneLaunchRefused(s, *model::Id::parse("sc_3")));
    CHECK_FALSE(views::sceneLaunchRefused(s, *model::Id::parse("sc_2")));
    draw();
    CHECK(pane.stats.refused == 1);
    CHECK(header.stats.refused == 1);
    f.golden("launcher-refused-dark.png");
    // The audio clip's ▶ still asks, and the refusal reaches the app's error
    // hook (the status line), naming why.
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(f.row("tr_3"), 2))));
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().code == "launch.playrate");
    // A MIDI clip's launches.
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(f.row("tr_1"), 0))));
    CHECK(f.errors.size() == 1);
    // Back at 1: nothing dims.
    f.exec("transport.playrate.set", R"({"rate":1.0})");
    draw();
    CHECK(pane.stats.refused == 0);
    CHECK(header.stats.refused == 0);
    f.exec("engine.stop", "{}");
}

TEST_CASE("launcher: a scene dragged onto the timeline lies there; its menu inserts it") {
    LauncherFixture f;
    f.song();
    f.view->installOverlay(*f.root);  // the ghost layer draws the drop marker
    views::LauncherHeader& header = f.view->launcherHeader();
    const model::Session* s = &f.app->head();
    // Verse's longest clip, two bars on the Keys, sets its span; Intro's takes one.
    f.exec("clip.add", R"({"track":"tr_3","scene":"sc_2","notation":"voice k; 1: C4:w; 2: E4:w"})");
    s = &f.app->head();
    CHECK(views::sceneBars(*s, *model::Id::parse("sc_2"), 1) == 2);
    CHECK(views::sceneBars(*s, *model::Id::parse("sc_1"), 1) == 1);
    CHECK(views::barAtOrAfter(*s->timeMap, 0) == 1);
    CHECK(views::barAtOrAfter(*s->timeMap, 4) == 2);
    CHECK(views::barAtOrAfter(*s->timeMap, 4.5) == 3);
    CHECK(views::barAfterArrangement(*s) == 1);
    auto beatX = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    // Verse onto the lanes in bar 3: a drop marker and its span while held.
    const ui::Rect verse = header.sceneRect(1);
    const ui::Point from{verse.x + 40, verse.y + verse.h / 2};
    const ui::Rect lane = f.view->rowRect(f.row("tr_3"));
    const ui::Point to{beatX(9.5), lane.y + lane.h / 2};
    REQUIRE(f.view->lanesRect().contains(to));
    f.pointer(Pointer::Down, from);
    f.pointer(Pointer::Move, {from.x, from.y + 20});
    f.pointer(Pointer::Move, to);
    const auto& ghost = f.view->gestures().sceneGhost();
    REQUIRE(ghost);
    CHECK(ghost->start == doctest::Approx(8));
    CHECK(ghost->end == doctest::Approx(16));
    CHECK(ghost->tracks.size() == 3);
    f.snapshot("launcher-scene-drop.png");
    f.pointer(Pointer::Up, to);
    f.sync();
    CHECK_FALSE(f.view->gestures().sceneGhost());
    CHECK(f.commands == std::vector<std::string>{"scene.insert"});
    s = &f.app->head();
    REQUIRE(s->regions.size() == 1);
    CHECK(s->regions.begin()->value->name == "Verse");
    CHECK(s->regions.begin()->value->start == doctest::Approx(8));
    CHECK(s->regions.begin()->value->end == doctest::Approx(16));
    CHECK(s->items.size() == 3);
    CHECK(views::launcherColumns(*s).scenes[1] == model::Id::parse("sc_2"));  // not moved
    CHECK(views::barAfterArrangement(*s) == 5);
    // Again on the same bars: refused, and the reason reaches onError.
    f.drag(from, to);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "scene.insert.occupied");
    f.errors.clear();
    // The menu: Insert at playhead (the next bar line), Insert at end.
    auto menuRun = [&](std::size_t column, std::size_t row) {
        f.click({header.sceneRect(column).x + 40, verse.y + 8}, 0, 1, 3);
        ui::Node* menu = f.root->find("launcher.scene.menu");
        REQUIRE(menu);
        const auto run = ui::menuItems(*menu)[row].run;
        f.root->closePopups();
        run();
        f.sync();
    };
    f.click({header.sceneRect(0).x + 40, verse.y + 8}, 0, 1, 3);
    ui::Node* menu = f.root->find("launcher.scene.menu");
    REQUIRE(menu);
    std::vector<std::string> rows;
    for (const auto& item : ui::menuItems(*menu)) rows.push_back(item.label);
    CHECK(rows == std::vector<std::string>{"Rename…", "Colour…", "Insert at playhead",
                                           "Insert at end", "", "Delete"});
    f.root->closePopups();
    menuRun(0, 3);  // Intro at the end: bar 5
    f.view->setPlayhead(f.app->head().timeMap->secondsAt(30.5));
    menuRun(0, 2);  // Intro at the playhead: bar 9
    std::vector<std::pair<double, double>> spans;
    for (const auto& e : f.app->head().regions) spans.push_back({e.value->start, e.value->end});
    std::sort(spans.begin(), spans.end());
    CHECK(spans == std::vector<std::pair<double, double>>{{8, 16}, {16, 20}, {32, 36}});
    // An empty scene refuses: Chorus has no clips.
    menuRun(2, 3);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "scene.empty");
    // A length between bar lines rounds up: five beats take two bars.
    f.exec("clip.set", R"({"clip":"cl_1","length":5})");
    CHECK(views::sceneBars(f.app->head(), *model::Id::parse("sc_1"), 3) == 2);
}

TEST_CASE("launcher: copied clips paste from the selected cell, in their layout, one undo step") {
    LauncherFixture f;
    f.song();
    const model::Session* s = &f.app->head();
    std::vector<std::string> channels;
    for (const views::TrackRow& row : f.view->trackLayout().rows())
        if (!row.lane() && row.type == model::StripType::Channel)
            channels.push_back(row.track.str());
    REQUIRE(channels == std::vector<std::string>{"tr_1", "tr_2", "tr_3"});
    // Kit B (Drums, Verse) and Root (Bass, Verse) pasted from Keys' Chorus... runs
    // past the last channel: refused, nothing changes.
    const std::vector<std::string> both{"cl_2", "cl_3"};
    auto refused = gui::pasteClips(*f.dispatcher, *s, both, "tr_3", "sc_3", channels);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "clip.paste");
    // From Drums' Chorus: Kit B there, Root under it.
    const std::uint64_t before = f.app->head().version;
    auto made = gui::pasteClips(*f.dispatcher, *s, both, "tr_1", "sc_3", channels);
    REQUIRE(made);
    REQUIRE(made->size() == 2);
    s = &f.app->head();
    CHECK(s->version == before + 1);
    const auto kit = views::clipIn(*s, *model::Id::parse("tr_1"), *model::Id::parse("sc_3"));
    const auto root = views::clipIn(*s, *model::Id::parse("tr_2"), *model::Id::parse("sc_3"));
    REQUIRE(kit);
    REQUIRE(root);
    CHECK(kit->str() == (*made)[0]);
    CHECK(s->clips.find(*kit)->name == "Kit B");
    CHECK(s->clips.find(*root)->name == "Root");
    // Onto an occupied cell: refused whole, one transaction rolled back.
    auto onto = gui::pasteClips(*f.dispatcher, *s, both, "tr_1", "sc_3", channels);
    REQUIRE_FALSE(onto);
    CHECK(f.app->head().version == before + 1);
    // After a cut the paste moves them: Kit A from Intro to Keys' Intro.
    auto moved =
        gui::pasteClips(*f.dispatcher, f.app->head(), {"cl_1"}, "tr_3", "sc_1", channels, true);
    REQUIRE(moved);
    CHECK(*moved == std::vector<std::string>{"cl_1"});
    CHECK(f.app->head().clips.find(*model::Id::parse("cl_1"))->track == model::Id::parse("tr_3"));
}

TEST_CASE("launcher: a cut pasted over its own cells moves front first") {
    LauncherFixture f;
    f.song();
    // Kit A (Drums, Intro) and Root (Bass, Verse), cut and pasted from Bass'
    // Verse: Kit A lands where Root is, so Root has to move first.
    const std::uint64_t before = f.app->head().version;
    auto moved = gui::pasteClips(*f.dispatcher, f.app->head(), {"cl_1", "cl_3"}, "tr_2", "sc_2",
                                 {"tr_1", "tr_2", "tr_3"}, true);
    REQUIRE(moved);
    CHECK(*moved == std::vector<std::string>{"cl_1", "cl_3"});  // the clipboard's order
    const model::Session& s = f.app->head();
    CHECK(s.version == before + 1);
    CHECK(views::clipIn(s, *model::Id::parse("tr_2"), *model::Id::parse("sc_2"))->str() == "cl_1");
    CHECK(views::clipIn(s, *model::Id::parse("tr_3"), *model::Id::parse("sc_3"))->str() == "cl_3");
    CHECK_FALSE(views::clipIn(s, *model::Id::parse("tr_1"), *model::Id::parse("sc_1")));
}

TEST_CASE("launcher: the clip clipboard holds only for the project it was filled in") {
    LauncherFixture f;
    f.song();
    gui::ClipClipboard board;
    board.fill({"cl_1"}, true, f.app->projectGeneration());
    CHECK(board.in(f.app->projectGeneration()) == std::vector<std::string>{"cl_1"});
    CHECK(board.cut);
    // Another project with the same ids: cl_1 there is not the clip cut here,
    // so the clipboard empties and the paste refuses, moving nothing.
    f.exec("project.new", "{}");
    f.song();
    REQUIRE(f.app->head().clips.find(*model::Id::parse("cl_1")));
    const std::vector<std::string> clips = board.in(f.app->projectGeneration());
    CHECK(clips.empty());
    CHECK_FALSE(board.cut);
    const std::uint64_t before = f.app->head().version;
    auto pasted = gui::pasteClips(*f.dispatcher, f.app->head(), clips, "tr_3", "sc_1",
                                  {"tr_1", "tr_2", "tr_3"}, board.cut);
    REQUIRE_FALSE(pasted);
    CHECK(pasted.error().code == "clip.paste");
    CHECK(f.app->head().version == before);
    CHECK(f.app->head().clips.find(*model::Id::parse("cl_1"))->track == model::Id::parse("tr_1"));
}

TEST_CASE("launcher: laying clips on the timeline says in the status line what their keys did") {
    // gui.md §4.15: the answers' followed blocks, in a few words, no dialog.
    LauncherFixture f;
    f.song();
    views::LauncherPane& pane = f.view->launcherPane();
    views::LauncherHeader& header = f.view->launcherHeader();
    // C minor, then F minor from bar 7. Root (Verse, Bass) is in D minor and
    // follows the key; Pad (Verse, Keys) is in D minor and follows nothing.
    f.exec("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    f.exec("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key D minor ~:w","follow":"key"})");
    f.exec("clip.add", R"({"track":"tr_3","scene":"sc_2","notation":"voice k; 1: D4:w",)"
                       R"("context":"1: key D minor ~:w","name":"Pad"})");
    auto beatX = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    // Root dragged onto the Bass lane at bar 5: moved down a tone.
    const ui::Point from = centre(views::LauncherPane::bodyRect(pane.cellRect(f.row("tr_2"), 1)));
    const ui::Rect lane = f.view->rowRect(f.row("tr_2"));
    f.drag(from, {beatX(16) + 1, lane.y + lane.h / 2});
    REQUIRE(f.commands == std::vector<std::string>{"item.add"});
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0] == "Root followed to C minor, −2");
    // The clip menu's Lay on timeline at playhead: Pad at bar 9 is copied
    // under F minor, and says so.
    auto layAtPlayhead = [&](std::size_t row, std::size_t column, double beat) {
        f.view->setPlayhead(f.app->head().timeMap->secondsAt(beat));
        f.click(centre(views::LauncherPane::bodyRect(pane.cellRect(row, column))), 0, 1, 3);
        ui::Node* menu = f.root->find("launcher.clip.menu");
        REQUIRE(menu);
        std::function<void()> lay;
        for (const auto& item : ui::menuItems(*menu))
            if (item.label == "Lay on timeline at playhead") lay = item.run;
        REQUIRE(lay);
        f.root->closePopups();
        lay();
        f.sync();
    };
    layAtPlayhead(f.row("tr_3"), 1, 32);
    REQUIRE(f.notices.size() == 2);
    CHECK(f.notices[1] == "Pad copied: its key D minor differs from F minor at bar 9");
    // A clip with nothing to say (Kit B: no key, follows nothing) says nothing.
    layAtPlayhead(f.row("tr_1"), 1, 64);
    CHECK(f.notices.size() == 2);
    // The scene menu's Insert at playhead: Verse at bar 21, each clip that
    // followed or was copied in another key (Kit B says nothing).
    f.view->setPlayhead(f.app->head().timeMap->secondsAt(80));
    f.click({header.sceneRect(1).x + 40, header.sceneRect(1).y + 8}, 0, 1, 3);
    ui::Node* menu = f.root->find("launcher.scene.menu");
    REQUIRE(menu);
    const auto insert = ui::menuItems(*menu)[2].run;
    f.root->closePopups();
    insert();
    f.sync();
    REQUIRE(f.notices.size() == 3);
    CHECK(f.notices[2] ==
          "Verse at bar 21: Root followed to F minor, +3; Pad copied: its key D "
          "minor differs from F minor at bar 21");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: followNotice words key changes, chords, folding, and sums up a long scene") {
    LauncherFixture f;
    f.song();
    const model::Session& s = f.app->head();
    auto item = [](const std::string& followed) {
        return *json::parse(R"({"item":"it_1","followed":)" + followed + "}");
    };
    const json::Value root = *json::parse(R"({"clip":"cl_3"})");
    // Two runs of bars: the second says where it starts.
    CHECK(
        views::followNotice(
            s, "item.add", root,
            item(
                R"({"mode":"key","regions":[)"
                R"({"from_bar":5,"to_bar":6,"from_key":"D minor","to_key":"C minor","by":"transpose","interval":-2},)"
                R"({"from_bar":7,"to_bar":8,"from_key":"D minor","to_key":"F minor","by":"degree"}],"folded":2})")) ==
        "Root followed to C minor, −2, then F minor by degree from bar 7 (2 folded into range)");
    // Chords; a run already in the key (copy) is not a change.
    CHECK(views::followNotice(s, "item.add", root,
                              item(R"({"mode":"chords","regions":[{"from_bar":1,"to_bar":1,)"
                                   R"("from_key":"C minor","to_key":"C minor","by":"copy"},)"
                                   R"({"from_bar":2,"to_bar":2,"from_key":"C minor",)"
                                   R"("to_key":"C minor","by":"chords"}]})")) ==
          "Root followed to C minor by chord");
    CHECK_FALSE(
        views::followNotice(s, "item.add", root,
                            item(R"({"mode":"key","regions":[{"from_bar":1,"to_bar":1,)"
                                 R"("from_key":"C minor","to_key":"C minor","by":"copy"}]})")));
    // Already in the key as the engine answers it (Walk, C major under C
    // major): a transpose by 0, or degrees from a key to itself, say nothing.
    CHECK_FALSE(views::followNotice(
        s, "item.add", root,
        item(R"({"mode":"key","regions":[{"from_bar":23,"to_bar":23,"from_key":"C major",)"
             R"("to_key":"C major","by":"transpose","interval":0}]})")));
    CHECK_FALSE(views::followNotice(
        s, "item.add", root,
        item(R"({"mode":"key","regions":[{"from_bar":23,"to_bar":23,"from_key":"C major",)"
             R"("to_key":"C major","by":"degree"}]})")));
    CHECK(views::followNotice(s, "item.add", root, item(R"({"mode":"none","copied":"drums"})")) ==
          "Root copied: drums don't follow");
    CHECK(views::followNotice(s, "item.add", root, item(R"({"mode":"none","copied":"audio"})")) ==
          "Root copied: audio doesn't follow");
    CHECK_FALSE(views::followNotice(s, "item.add", root, item(R"({"mode":"none"})")));
    // A scene whose phrases would not fit the line: counts and names.
    const std::string moved =
        R"({"mode":"key","regions":[{"from_bar":3,"to_bar":4,"to_key":"G minor","by":"transpose","interval":5}]})";
    const std::string off =
        R"({"mode":"none","mismatch":{"clip_key":"D minor","key":"G minor","bar":3}})";
    const json::Value answer =
        *json::parse(R"({"items":[{"clip":"cl_1","followed":)" + moved +
                     R"(},{"clip":"cl_2","followed":)" + moved + R"(},{"clip":"cl_3","followed":)" +
                     off + R"(},{"clip":"cl_2","followed":{"mode":"none"}}]})");
    CHECK(views::followNotice(s, "scene.insert", *json::parse(R"({"scene":"sc_2","bar":3})"),
                              answer) ==
          "Verse at bar 3: 2 followed the key (Kit A, Kit B); 1 copied as written (Root)");
    // Other commands, and refusals' empty answers, say nothing.
    CHECK_FALSE(views::followNotice(s, "clip.duplicate", root, item(moved)));
}
