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
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app_actions.h"
#include "audio_device.h"
#include "launch_capture.h"
#include "shell.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/realtime.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/peaks/peaks.h"
#include "supple/product.h"
#include "supple/time/timeline.h"
#include "supple/ui/forms.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
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
    std::vector<json::Value> sent;  // each command's arguments, as sent
    std::vector<Error> errors;
    std::vector<std::string> notices;  // the status line's notes (onNotice)
    std::vector<model::Id> opened, selected, takeFx;
    gui::RefusalWatch refusals;  // as the shell keeps them, each pass

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
                 sent.push_back(args);
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
        refusals.update(*app, *view);
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
                                           "Insert at end", "Follow the song's key and chords",
                                           "Write harmony to the context track", "Lay out scenes…",
                                           "", "Delete"});
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

TEST_CASE("launcher: selected items dragged into a cell make a clip each, in one undo step") {
    LauncherFixture f;
    f.song();
    // The gestures run their steps as the app does: one transaction.
    f.view->gestures().run = [&f](const std::string& label,
                                  const std::vector<views::EditStep>& steps) {
        return f.dispatcher->transact(label, kLauncherUi, [&]() -> Result<json::Value> {
            json::Value last = json::Value::object();
            for (const views::EditStep& step : steps) {
                f.commands.push_back(step.command);
                auto r = f.dispatcher->execute({step.command, step.args, kLauncherUi});
                if (!r) return r;
                last = std::move(*r);
            }
            return last;
        });
    };
    auto itemOn = [&](const char* track) {
        const json::Value made =
            f.exec("item.add", std::string(R"({"track":")") + track +
                                   R"(","position":8,"length":4,"notation":"1: C4:q E4:q G4:h"})");
        return *model::Id::parse(made.find("item")->asString());
    };
    const model::Id bass = *model::Id::parse("tr_2"), keys = *model::Id::parse("tr_3");
    const model::Id verse = *model::Id::parse("sc_2"), chorus = *model::Id::parse("sc_3");
    const model::Id onBass = itemOn("tr_2"), onKeys = itemOn("tr_3");
    f.sync();
    views::LauncherPane& pane = f.view->launcherPane();
    auto beatX = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    auto grabOn = [&](model::Id track) {
        const ui::Rect lane = f.view->rowRect(f.row(track.str().c_str()));
        return ui::Point{beatX(9.5), lane.y + lane.h / 2};
    };
    auto rowOf = [&](model::Id track) { return f.row(track.str().c_str()); };
    // Both selected, the Bass item pressed, held over Bass's Chorus cell: two
    // cells outlined, Bass's and the Keys' one row down.
    // `on`: the pressed item's track.
    auto dragInto = [&](model::Id on, std::size_t row, std::size_t column, bool hold) {
        f.view->gestures().setSelection({onBass, onKeys});
        const ui::Point grab = grabOn(on);
        const ui::Rect cell = pane.cellRect(row, column);
        const ui::Point into{cell.x + 8, cell.bottom() - 8};
        f.pointer(Pointer::Down, grab);
        f.pointer(Pointer::Move, {grab.x - 20, grab.y});
        f.pointer(Pointer::Move, into);
        f.sync();
        const std::size_t shown = pane.stats.dropShown;
        if (hold) f.snapshot("launcher-items-drop.png");
        f.pointer(Pointer::Up, into);
        f.sync();
        return shown;
    };
    const std::size_t items = f.app->head().items.size();
    const std::size_t clips = f.app->head().clips.size();
    auto steps = [&] { return f.exec("history.list", "{}").find("versions")->size(); };
    const std::size_t before = steps();
    CHECK(dragInto(bass, rowOf(bass), 2, true) == 2);
    CHECK(f.errors.empty());
    CHECK(f.commands == std::vector<std::string>{"clip.add", "clip.add"});
    const auto onBassClip = views::clipIn(f.app->head(), bass, chorus);
    const auto onKeysClip = views::clipIn(f.app->head(), keys, chorus);
    REQUIRE(onBassClip);
    REQUIRE(onKeysClip);
    CHECK(f.app->head().clips.size() == clips + 2);
    CHECK(f.app->head().items.size() == items);  // the items stay where they were
    CHECK(steps() == before + 1);                // one undo step
    std::vector<model::Id> selected = f.view->selectedClips();
    std::sort(selected.begin(), selected.end());
    std::vector<model::Id> made{*onBassClip, *onKeysClip};
    std::sort(made.begin(), made.end());
    CHECK(selected == made);
    f.exec("history.undo", "{}");
    f.sync();
    CHECK(f.app->head().clips.size() == clips);  // both go with one undo
    // A taken cell refuses the whole drop with clip.add's message: Bass's
    // Verse cell holds Root, so the Keys' Verse cell stays empty too.
    f.commands.clear();
    CHECK(dragInto(bass, rowOf(bass), 1, false) == 2);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.commands == std::vector<std::string>{"clip.add"});  // the first refused: rolled back
    CHECK(f.errors[0].code == "clip.occupied");
    CHECK(f.errors[0].message == "Root is already in Bass's Verse");  // names, not ids
    CHECK_FALSE(views::clipIn(f.app->head(), keys, verse));
    CHECK(f.app->head().clips.size() == clips);
    // A track that holds no clips: the Keys' item lands on Verb (a buss), and
    // clip.add refuses it, so nothing is made.
    f.errors.clear();
    CHECK(dragInto(bass, rowOf(keys), 2, false) == 2);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code != "gui.launcher.drop");
    CHECK_FALSE(views::clipIn(f.app->head(), keys, chorus));
    CHECK(f.app->head().clips.size() == clips);
    // Off the rows: the Keys' item pressed, Bass's one row up from the Drums
    // row is above the first track. Refused before anything is sent.
    f.errors.clear();
    f.commands.clear();
    CHECK(dragInto(keys, rowOf(*model::Id::parse("tr_1")), 2, false) == 1);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "gui.launcher.drop");
    CHECK(f.errors[0].message.find("above the first track") != std::string::npos);
    CHECK(f.commands.empty());
    CHECK(f.app->head().clips.size() == clips);
    // One item alone onto a taken cell: clip.add says so too.
    f.errors.clear();
    f.view->gestures().setSelection({});
    const ui::Point grab = grabOn(bass);
    const ui::Rect taken = pane.cellRect(rowOf(bass), 1);
    f.pointer(Pointer::Down, grab);
    f.pointer(Pointer::Move, {grab.x - 20, grab.y});
    f.pointer(Pointer::Move, {taken.x + 8, taken.bottom() - 8});
    f.pointer(Pointer::Up, {taken.x + 8, taken.bottom() - 8});
    f.sync();
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "clip.occupied");
    CHECK(f.app->head().clips.size() == clips);
}

TEST_CASE("launcher: items on one track dropped into cells take the scenes after, in time order") {
    LauncherFixture f;
    f.song();  // Intro, Verse, Chorus; Bass's Verse holds Root
    f.view->gestures().run = [&f](const std::string& label,
                                  const std::vector<views::EditStep>& steps) {
        return f.dispatcher->transact(label, kLauncherUi, [&]() -> Result<json::Value> {
            json::Value last = json::Value::object();
            for (const views::EditStep& step : steps) {
                f.commands.push_back(step.command);
                auto r = f.dispatcher->execute({step.command, step.args, kLauncherUi});
                if (!r) return r;
                last = std::move(*r);
            }
            return last;
        });
    };
    // Items told apart by length: 2, 4 and 6 beats.
    auto itemAt = [&](const char* track, double position, double length) {
        json::Value args = json::Value::object();
        args["track"] = track;
        args["position"] = position;
        args["length"] = length;
        args["notation"] = "1: C4:q r:*";
        const json::Value made = f.exec("item.add", *json::write(args));
        return *model::Id::parse(made.find("item")->asString());
    };
    const model::Id keys = *model::Id::parse("tr_3"), bass = *model::Id::parse("tr_2");
    const model::Id intro = *model::Id::parse("sc_1"), verse = *model::Id::parse("sc_2"),
                    chorus = *model::Id::parse("sc_3");
    const model::Id early = itemAt("tr_3", 8, 2), middle = itemAt("tr_3", 16, 4),
                    late = itemAt("tr_3", 24, 6), onBass = itemAt("tr_2", 8, 2);
    f.sync();
    views::LauncherPane& pane = f.view->launcherPane();
    auto beatX = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    auto lengthIn = [&](model::Id track, model::Id scene) {
        const auto clip = views::clipIn(f.app->head(), track, scene);
        return clip ? f.app->head().clips.find(*clip)->length : -1.0;
    };
    std::size_t hidden = 0;  // the last drop's cells out of view, while held
    // `press`: the item pressed (at its start); `at`: the point in the cell.
    auto drop = [&](std::vector<model::Id> selection, model::Id press, double beat, std::size_t row,
                    std::size_t column, bool centre, const char* shot = nullptr) {
        f.commands.clear();
        f.errors.clear();
        f.view->gestures().setSelection(selection);
        const model::Item* it = f.app->head().items.find(press);
        REQUIRE(it);
        const ui::Rect lane = f.view->rowRect(f.row(it->track.str().c_str()));
        const ui::Point grab{beatX(beat + 0.5), lane.y + lane.h / 2};
        const ui::Rect cell = pane.cellRect(row, column);
        const ui::Point into = centre ? ui::Point{cell.x + cell.w / 2, cell.y + cell.h / 2}
                                      : ui::Point{cell.x + 8, cell.bottom() - 8};
        f.pointer(Pointer::Down, grab);
        f.pointer(Pointer::Move, {grab.x - 20, grab.y});
        f.pointer(Pointer::Move, into);
        f.sync();
        const auto held = std::pair{pane.stats.dropShown, pane.stats.dropTakes};
        hidden = pane.stats.dropHidden;
        if (shot) f.snapshot(shot);
        f.pointer(Pointer::Up, into);
        f.sync();
        return held;
    };
    const std::size_t clips = f.app->head().clips.size();
    // Two on Keys, the later one pressed, dropped on Keys's Intro: the earlier
    // takes Intro, the later Verse, both outlined while held.
    auto held = drop({early, middle}, middle, 16, f.row("tr_3"), 0, false);
    CHECK(held == std::pair<std::size_t, bool>{2, true});
    CHECK(f.errors.empty());
    CHECK(lengthIn(keys, intro) == doctest::Approx(2));
    CHECK(lengthIn(keys, verse) == doctest::Approx(4));
    CHECK(f.app->head().clips.size() == clips + 2);
    f.exec("history.undo", "{}");
    f.sync();
    // Three on Keys and one on Bass, dropped on Keys's Intro: Keys's run
    // takes all three scenes, Bass's its own from Intro, one row up.
    held = drop({early, middle, late, onBass}, early, 8, f.row("tr_3"), 0, false,
                "launcher-items-run.png");
    CHECK(held == std::pair<std::size_t, bool>{4, true});
    CHECK(f.errors.empty());
    CHECK(f.commands.size() == 4);
    CHECK(lengthIn(keys, intro) == doctest::Approx(2));
    CHECK(lengthIn(keys, verse) == doctest::Approx(4));
    CHECK(lengthIn(keys, chorus) == doctest::Approx(6));
    CHECK(lengthIn(bass, intro) == doctest::Approx(2));
    CHECK(f.app->head().clips.size() == clips + 4);
    f.exec("history.undo", "{}");
    f.sync();
    CHECK(f.app->head().clips.size() == clips);
    // Too few scenes: three on Keys from Verse need three, and there are two.
    // Refused before anything is sent; the cell under the pointer says so.
    held = drop({early, middle, late}, early, 8, f.row("tr_3"), 1, false);
    CHECK(held == std::pair<std::size_t, bool>{2, false});
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "gui.launcher.drop");
    CHECK(f.errors[0].message == "Keys's 3 items need 3 scenes from Verse, and there are 2");
    CHECK(f.errors[0].hint == "add 1 scene after the last, or drop further left");
    CHECK(f.commands.empty());
    CHECK(f.app->head().clips.size() == clips);
    // A taken cell in a run refuses the whole drop with clip.add's answer:
    // two on Bass from Intro reach Verse, which holds Root.
    const model::Id bassLater = itemAt("tr_2", 16, 4);
    f.sync();
    held = drop({onBass, bassLater}, onBass, 8, f.row("tr_2"), 0, false);
    CHECK(held == std::pair<std::size_t, bool>{2, true});  // Verse's outline says no
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "clip.occupied");
    CHECK(f.errors[0].message.find("Root") != std::string::npos);
    CHECK_FALSE(views::clipIn(f.app->head(), bass, intro));  // rolled back
    // A pane two scenes wide: of Keys's three, Chorus's cell is out of view,
    // said as "+1" at the pane's edge while held; the drop still makes all
    // three, and the pane does not scroll.
    f.view->setLauncherWidth(views::LauncherColumns::kMinWidth);
    f.sync();
    held =
        drop({early, middle, late}, early, 8, f.row("tr_3"), 0, false, "launcher-items-hidden.png");
    CHECK(held == std::pair<std::size_t, bool>{3, true});
    CHECK(hidden == 1);
    CHECK(f.errors.empty());
    CHECK(lengthIn(keys, chorus) == doctest::Approx(6));
    f.exec("history.undo", "{}");
    f.view->setLauncherWidth(0);
    f.sync();
    // In a pane wide enough, nothing is out of view.
    held = drop({early, middle}, early, 8, f.row("tr_3"), 0, false);
    CHECK(hidden == 0);
    f.exec("history.undo", "{}");
    f.sync();
    // The centre of an empty cell (its stop square) takes a drop as the rest
    // of the cell does: outlined as taking, and a clip made.
    held = drop({}, early, 8, f.row("tr_3"), 2, true);
    CHECK(held == std::pair<std::size_t, bool>{1, true});
    CHECK(f.errors.empty());
    CHECK(lengthIn(keys, chorus) == doctest::Approx(2));
}

TEST_CASE("launcher: an item on a hidden track refuses a drop of the selection into cells") {
    LauncherFixture f;
    f.song();
    f.exec("track.add", R"({"type":"channel","name":"Strings"})");  // tr_5
    f.exec("item.add",
           R"({"track":"tr_2","position":8,"length":4,"notation":"1: C4:q r:*"})");  // it_1
    f.exec("item.add",
           R"({"track":"tr_5","position":8,"length":4,"notation":"1: C4:q r:*"})");  // it_2
    // Strings hidden from the track panel, as an imported project can have it.
    const auto path = f.dir / ("hidden." + std::string(product::kSlug));
    json::Value save = json::Value::object();
    save["path"] = path.string();
    f.exec("project.save", *json::write(save));
    std::stringstream text;
    {
        std::ifstream in(path);
        text << in.rdbuf();
    }
    auto doc = json::parse(text.str()).value();
    doc["tracks"]["tr_5"]["hidden_tcp"] = true;
    {
        std::ofstream out(path, std::ios::trunc);
        out << json::writeCanonical(doc);
    }
    f.exec("project.open", *json::write(save));
    f.sync();
    REQUIRE_FALSE(f.view->trackLayout().indexOf(*model::Id::parse("tr_5")));
    const model::Id onBass = *model::Id::parse("it_1"), hidden = *model::Id::parse("it_2");
    REQUIRE(f.app->head().items.find(hidden));
    f.commands.clear();
    f.view->gestures().setSelection({onBass, hidden});
    views::LauncherPane& pane = f.view->launcherPane();
    const ui::Rect lane = f.view->rowRect(f.row("tr_2"));
    const ui::Point grab{f.view->xAt(f.app->head().timeMap->secondsAt(8.5)), lane.y + lane.h / 2};
    const ui::Rect cell = pane.cellRect(f.row("tr_2"), 2);
    const ui::Point into{cell.x + 8, cell.bottom() - 8};
    f.pointer(Pointer::Down, grab);
    f.pointer(Pointer::Move, {grab.x - 20, grab.y});
    f.pointer(Pointer::Move, into);
    f.sync();
    CHECK_FALSE(pane.stats.dropTakes);  // refused while held
    f.pointer(Pointer::Up, into);
    f.sync();
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "gui.launcher.drop");
    CHECK(f.errors[0].message.find("is on a hidden track") != std::string::npos);
    CHECK(f.commands.empty());
    CHECK_FALSE(views::clipIn(f.app->head(), *model::Id::parse("tr_2"), *model::Id::parse("sc_3")));
}

TEST_CASE("launcher: Stop all, Arrangement and Capture share one row; narrow, they are glyphs") {
    LauncherFixture f;
    f.song();
    views::LauncherHeader& header = f.view->launcherHeader();
    auto apart = [](const ui::Rect& a, const ui::Rect& b) {
        return a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y;
    };
    std::vector<float> widths{views::LauncherColumns::kMinWidth, 180, 200, 233, 234, 260, 400};
    bool sawGlyphs = false, sawWords = false;
    for (const float width : widths) {
        f.view->setLauncherWidth(width);
        f.sync();
        f.root->damageAll();
        f.root->frame(false);
        const ui::Rect b = header.bounds();
        const ui::Rect stop = header.stopRect(), arr = header.arrangementRect(),
                       cap = header.captureRect();
        INFO("width " << width);
        // One row, inside the header and above the scene names, apart.
        CHECK(stop.y == arr.y);
        CHECK(arr.y == cap.y);
        for (const ui::Rect& r : {stop, arr, cap}) {
            CHECK(r.x >= b.x);
            CHECK(r.right() <= b.right());
            CHECK(r.bottom() <= header.scenesRow().y);
        }
        CHECK(apart(stop, arr));
        CHECK(apart(arr, cap));
        CHECK(apart(stop, cap));
        CHECK(header.stats.glyphCells == header.glyphCells());
        if (header.glyphCells()) {
            sawGlyphs = true;
            CHECK(stop.w == views::LauncherHeader::kGlyphCell);
            CHECK(cap.w == views::LauncherHeader::kGlyphCell);
        } else {
            sawWords = true;
        }
        // Each says its name where the pointer rests, words or glyph.
        CHECK(header.hintAt({stop.x + stop.w / 2, stop.y + stop.h / 2}).name == "Stop all");
        CHECK(header.hintAt({arr.x + arr.w / 2, arr.y + arr.h / 2}).name == "Arrangement");
        CHECK(header.hintAt({cap.x + cap.w / 2, cap.y + cap.h / 2}).name == "Capture");
        if (width == views::LauncherColumns::kMinWidth) f.golden("launcher-header-glyphs.png");
        if (width == 260) f.snapshot("launcher-header-words.png");
    }
    CHECK(sawGlyphs);
    CHECK(sawWords);
    // A pane fitted to three scenes (the + cell and Arr after them) has the words.
    f.view->setLauncherWidth(0);
    f.sync();
    CHECK(f.view->launcherWidth() >= 3 * views::LauncherColumns::kColumn);
    CHECK_FALSE(header.glyphCells());
    // The glyph cells still press: Stop all and Capture at the narrowest.
    f.view->setLauncherWidth(views::LauncherColumns::kMinWidth);
    f.sync();
    f.commands.clear();
    const ui::Rect stop = header.stopRect();
    f.click({stop.x + stop.w / 2, stop.y + stop.h / 2});
    CHECK(f.commands == std::vector<std::string>{"launch.stop"});
}

TEST_CASE("launcher: Lay out's rows follow the rows before them until edited by hand") {
    LauncherFixture f;
    f.song();
    // 3/4 from bar 2: Verse's four beats take one bar at bar 1, two from bar 2.
    f.exec("timesig.set", R"({"bar":2,"meter":"3/4"})");
    views::LauncherHeader& header = f.view->launcherHeader();
    f.click({header.sceneRect(0).x + 40, header.sceneRect(0).y + 8}, 0, 1, 3);
    ui::Node* menu = f.root->find("launcher.scene.menu");
    REQUIRE(menu);
    std::function<void()> layOut;
    for (const auto& item : ui::menuItems(*menu))
        if (item.label == "Lay out scenes…") layOut = item.run;
    f.root->closePopups();
    REQUIRE(layOut);
    layOut();
    f.sync();
    REQUIRE(f.root->find("launcher.layout"));
    auto field = [&](const std::string& id) {
        auto* t = dynamic_cast<ui::TextField*>(f.root->find(id));
        REQUIRE(t);
        return t;
    };
    auto use = [&](std::size_t i) {
        auto* c =
            dynamic_cast<ui::Checkbox*>(f.root->find("launcher.layout.use." + std::to_string(i)));
        REQUIRE(c);
        return c;
    };
    auto type = [&](const std::string& id, const std::string& text) {
        field(id)->setText(text);
        REQUIRE(field(id)->onChange);
        field(id)->onChange(text);  // as typing does
    };
    REQUIRE(field("launcher.layout.bar")->text() == "1");
    CHECK(field("launcher.layout.bars.0")->text() == "1");  // Intro at bar 1, 4/4
    CHECK(field("launcher.layout.bars.1")->text() == "2");  // Verse at bar 2, 3/4
    // Intro unticked: Verse lands at bar 1, in 4/4, and is one bar.
    use(0)->toggle();
    CHECK_FALSE(use(0)->checked());
    CHECK(field("launcher.layout.bars.1")->text() == "1");
    use(0)->toggle();
    CHECK(field("launcher.layout.bars.1")->text() == "2");
    // Intro's bars by hand: 2, so Verse starts at bar 3 (3/4), still two.
    type("launcher.layout.bars.0", "2");
    CHECK(field("launcher.layout.bars.1")->text() == "2");
    // From bar 2: Intro keeps its 2 (edited); Verse at bar 4 is two bars.
    type("launcher.layout.bar", "2");
    CHECK(field("launcher.layout.bars.0")->text() == "2");
    CHECK(field("launcher.layout.bars.1")->text() == "2");
    // Back to bar 1 with Intro at 0 bars typed: Verse lands at bar 1, one bar.
    type("launcher.layout.bar", "1");
    type("launcher.layout.bars.0", "0");
    CHECK(field("launcher.layout.bars.1")->text() == "1");
    // Verse edited by hand is never worked out again, whatever moves before it.
    type("launcher.layout.bars.1", "5");
    type("launcher.layout.bars.0", "1");
    CHECK(field("launcher.layout.bars.1")->text() == "5");
    use(0)->toggle();
    CHECK(field("launcher.layout.bars.1")->text() == "5");
    // A From bar that is not a whole number from 1 changes no row.
    const std::string chorus = field("launcher.layout.bars.2")->text();
    type("launcher.layout.bar", "x");
    CHECK(field("launcher.layout.bars.2")->text() == chorus);
    f.root->closePopups();
}

TEST_CASE("launcher: refusals name clips, tracks and scenes, never their ids") {
    LauncherFixture f;
    f.song();
    // An unnamed scene and clip: Scene 4 (its place), Keys's clip in Scene 4.
    f.exec("scene.add", "{}");
    f.exec("clip.add", R"({"track":"tr_3","scene":"sc_4","notation":"1: C4:w"})");
    const model::Session& s = f.app->head();
    CHECK(views::objectNames(s, "cl_3 is already in tr_2's sc_2") ==
          "Root is already in Bass's Verse");
    CHECK(views::objectNames(s, "sc_4 is empty") == "Scene 4 is empty");
    CHECK(views::objectNames(s, "cl_4 can't play") == "Keys's clip in Scene 4 can't play");
    CHECK(views::objectNames(s, "sc_9 and cl_9 and tr_9") ==
          "a removed scene and a removed clip and a removed track");
    // Only whole ids: part of a word, or a prefix with no number, stays.
    CHECK(views::objectNames(s, "disc_3, tr_2x, sc_ and (tr_1)") ==
          "disc_3, tr_2x, sc_ and (Drums)");
    // A message that gives the name and the id says the name once (round
    // 165 F4: freeze.cpp's "Bass (tr_2) is frozen"); another name stays.
    CHECK(views::objectNames(s, "Bass (tr_2) is frozen") == "Bass is frozen");
    CHECK(views::objectNames(s, "Old (tr_2) is frozen") == "Old (Bass) is frozen");
    // Only a whole name collapses (round 165 re-check 4): a longer name that
    // ends in the id's is another name, and keeps both.
    CHECK(views::objectNames(s, "Sub Bass (tr_2) is frozen") == "Sub Bass (Bass) is frozen");
    CHECK(views::objectNames(s, "SubBass (tr_2)") == "SubBass (Bass)");
    // Joined by a hyphen, dot or slash: another, longer name (second re-check F3).
    CHECK(views::objectNames(s, "Sub-Bass (tr_2) is frozen") == "Sub-Bass (Bass) is frozen");
    CHECK(views::objectNames(s, "Sub.Bass (tr_2)") == "Sub.Bass (Bass)");
    CHECK(views::objectNames(s, "Sub/Bass (tr_2)") == "Sub/Bass (Bass)");
    CHECK(views::objectNames(s, "frozen: Bass (tr_2)") == "frozen: Bass");
    CHECK(views::objectNames(s, "\"Bass (tr_2)\"") == "\"Bass\"");
    CHECK(views::objectNames(s, "track Bass (tr_2): kept") == "track Bass: kept");
    CHECK(views::objectNames(s, "Drums (tr_1), Bass (tr_2)") == "Drums, Bass");
    // The Error form: the message, the hint and each listed error; the code
    // and path stay as the command gave them.
    Error e("clip.occupied", "cl_3 is in sc_2", "/clip", "move cl_3 out of tr_2 first");
    e.details.push_back(Error("x", "sc_1 has none", "/scene", "add one to sc_1"));
    const Error named = views::objectNames(s, e);
    CHECK(named.code == "clip.occupied");
    CHECK(named.path == "/clip");
    CHECK(named.message == "Root is in Verse");
    CHECK(named.hint == "move Root out of Bass first");
    REQUIRE(named.details.size() == 1);
    CHECK(named.details[0].message == "Intro has none");
    CHECK(named.details[0].hint == "add one to Intro");
    // Through the pane: Insert at end of an empty scene names it.
    views::LauncherHeader& header = f.view->launcherHeader();
    f.click({header.sceneRect(2).x + 40, header.sceneRect(2).y + 8}, 0, 1, 3);
    ui::Node* menu = f.root->find("launcher.scene.menu");
    REQUIRE(menu);
    std::function<void()> insertAtEnd;
    for (const auto& item : ui::menuItems(*menu))
        if (item.label == "Insert at end") insertAtEnd = item.run;
    f.root->closePopups();
    REQUIRE(insertAtEnd);
    insertAtEnd();
    f.sync();
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "scene.empty");
    CHECK(f.errors[0].message.find("sc_") == std::string::npos);
    CHECK(f.errors[0].message.find("Chorus") != std::string::npos);
    // Items, takes, envelopes and sections too. An unnamed MIDI item on Keys
    // at bar 5: its track and bar; its take, as its item.
    const json::Value made =
        f.exec("item.add", R"({"track":"tr_3","position":16,"length":4,"notation":"1: C4:w"})");
    const model::Id item = *model::Id::parse(made.find("item")->asString());
    const model::Item* it = f.app->head().items.find(item);
    REQUIRE(it);
    REQUIRE(it->activeTake);
    const model::Session& now = f.app->head();
    CHECK(views::objectNames(now, item.str() + " is in the way") ==
          "Keys's item at bar 5 is in the way");
    CHECK(views::objectNames(now, it->activeTake->str() + " is warped") ==
          "Keys's item at bar 5 is warped");
    {  // Another unnamed item in bar 5: each says its beat (round 165 S1).
        const json::Value other = f.exec(
            "item.add", R"({"track":"tr_3","position":18,"length":1,"notation":"1: E4:q r:*"})");
        const model::Session& two = f.app->head();
        CHECK(views::objectNames(two, item.str()) == "Keys's item at bar 5, beat 1");
        CHECK(views::objectNames(two, other.find("item")->asString()) ==
              "Keys's item at bar 5, beat 3");
        f.exec("history.undo", "{}");
        CHECK(views::objectNames(f.app->head(), item.str()) == "Keys's item at bar 5");
    }
    const json::Value env = f.exec("env.add", R"({"track":"tr_2","param":"volume"})");
    CHECK(
        views::objectNames(f.app->head(), env.find("env")->asString() + " has automation items") ==
        "Bass's volume automation has automation items");
    f.exec("region.add", R"({"start":8,"end":16})");
    f.exec("region.add", R"({"start":16,"end":20,"name":"Bridge"})");
    std::vector<std::string> regions;
    for (const auto& entry : f.app->head().regions) regions.push_back(entry.id.str());
    REQUIRE(regions.size() == 2);
    CHECK(views::objectNames(f.app->head(), regions[0] + " and " + regions[1]) ==
          "the section at bar 3 and Bridge");
    CHECK(views::objectNames(f.app->head(), "it_99, tk_99, en_99, rg_99") ==
          "a removed item, a removed take, a removed envelope, a removed section");
    // The other kinds the GUI refers to: a marker, a meter change and a send
    // without names of their own, a group; any whole id of a known kind.
    f.exec("marker.add", R"({"pos":8})");
    f.exec("timesig.set", R"({"bar":9,"meter":"3/4"})");
    f.exec("route.add", R"({"from":"tr_1","to":"tr_4"})");
    f.exec("group.add", R"({"name":"Rhythm","members":["tr_1","tr_2"]})");
    auto last = [](const auto& map) {
        std::string id;
        for (const auto& entry : map) id = entry.id.str();
        return id;
    };
    const model::Session& more = f.app->head();
    CHECK(views::objectNames(more, last(more.markers)) == "the marker at bar 3");
    CHECK(views::objectNames(more, last(more.meters)) == "the meter change at bar 9");
    CHECK(views::objectNames(more, last(more.routes)) == "the send from Drums to Verb");
    CHECK(views::objectNames(more, last(more.groups)) == "Rhythm");
    CHECK(views::objectNames(more, "fx_99 and mo_99") == "a removed plug-in and a removed module");
    // Not an id: an unknown prefix, upper case, or no digits.
    CHECK(views::objectNames(more, "zz_3, TR_1 and tr_x") == "zz_3, TR_1 and tr_x");
    {  // Unnamed groups by their place, as tracks (round 165 re-check 3).
        const std::string first = last(f.app->head().groups);
        f.exec("group.add", R"({"name":"Pads"})");
        const std::string second = last(f.app->head().groups);
        f.exec("group.set", R"({"group":")" + first + R"(","name":""})");
        f.exec("group.set", R"({"group":")" + second + R"(","name":""})");
        const model::Session& unnamed = f.app->head();
        CHECK(views::objectNames(unnamed, first + " and " + second) == "Group 1 and Group 2");
        for (int i = 0; i < 3; ++i) f.exec("history.undo", "{}");
        CHECK(views::objectNames(f.app->head(), first) == "Rhythm");
    }
    // An unnamed item draws no name on its lane, never its id.
    f.root->damageAll();
    f.root->frame(false);
    const auto& labels = f.view->lanes().stats().labels;
    CHECK(std::find(labels.begin(), labels.end(), std::string()) != labels.end());
    for (const std::string& label : labels) CHECK(label.find("it_") == std::string::npos);
    // The header draws what refusals say: an unnamed scene as "Scene 4".
    f.root->damageAll();
    f.root->frame(false);
    CHECK(header.stats.names == std::vector<std::string>{"Intro", "Verse", "Chorus", "Scene 4"});
    // An unnamed clip's cell draws no name (its row and column say it),
    // never an id: the song's three named clips, then Scene 4's empty name.
    CHECK(f.view->launcherPane().stats.names ==
          std::vector<std::string>{"Kit A", "Kit B", "Root", ""});
    // Insert at playhead over that item: scene.insert.occupied names it.
    f.errors.clear();
    f.view->setPlayhead(f.app->head().timeMap->secondsAt(16));  // bar 5
    f.click({header.sceneRect(2).x + 40, header.sceneRect(2).y + 8}, 0, 1, 3);
    menu = f.root->find("launcher.scene.menu");
    REQUIRE(menu);
    std::function<void()> atPlayhead;
    for (const auto& row : ui::menuItems(*menu))
        if (row.label == "Insert at playhead") atPlayhead = row.run;
    f.root->closePopups();
    REQUIRE(atPlayhead);
    f.exec("clip.add", R"({"track":"tr_3","scene":"sc_3","notation":"1: C4:w"})");
    atPlayhead();
    f.sync();
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "scene.insert.occupied");
    CHECK(f.errors[0].message.find("Keys's item at bar 5") != std::string::npos);
    for (const char* prefix : {"it_", "tr_", "sc_", "cl_", "tk_"})
        CHECK(f.errors[0].message.find(prefix) == std::string::npos);
}

// Round 165 F5: the lanes' drawn labels are this paint's, also when the GL
// layers failed (paintGl, which resets the stats, then draws nothing).
TEST_CASE("lanes: the drawn labels are each paint's own when the GL layers failed") {
    LauncherFixture f;
    f.song();
    f.exec("item.add", R"({"track":"tr_3","position":0,"length":4,"notation":"1: C4:w"})");
    f.exec("item.add", R"({"track":"tr_2","position":4,"length":4,"notation":"1: C2:w"})");
    f.root->damageAll();
    f.root->frame(false);
    const std::size_t drawn = f.view->lanes().stats().labels.size();
    REQUIRE(drawn > 0);
    f.view->lanes().failGlForTests();
    for (int i = 0; i < 3; ++i) {
        f.root->damageAll();
        f.root->frame(false);
        CHECK(f.view->lanes().stats().labels.size() == drawn);
    }
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

TEST_CASE("launcher: a take kept across Restart audio is committed by the pane") {
    // Review round 168 F1: a restart of the audio engine while Capture is
    // lit keeps the take (EngineApp); the pane's next pass commits it, as a
    // stop would have, though the new engine has no capture of its own.
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    REQUIRE(capture.set(true));
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    const auto service = [&] { capture.service(); };
    REQUIRE(f.runUntil(playing, at, "Kit A heard", service));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at, "a beat of Kit A", service));
    CHECK(f.app->head().items.empty());
    const std::uint64_t before = f.app->head().version;
    // Restart audio: the engine stops (the take is kept, and said) and starts.
    f.exec("engine.stop", "{}");
    CHECK(f.app->captureUncommitted());
    REQUIRE(f.app->captureKeptWarning());
    CHECK(f.app->captureKeptWarning()->find("was running when the engine stopped") !=
          std::string::npos);
    f.exec("engine.start", R"({"backend":"dummy"})");
    capture.service();
    CHECK(refused.empty());
    CHECK(f.app->head().version == before + 1);  // the kept take, one undo step
    // Nothing arms by itself after an engine stop (round 170 F3).
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    bool laid = false;
    for (const auto& e : f.app->head().items) laid = laid || e.value->track == drums;
    CHECK(laid);
    CHECK_FALSE(f.app->captureUncommitted());
    // Not written twice: a later pass finds it committed.
    capture.service();
    CHECK(f.app->head().version == before + 1);
    // Undone, the commit is declined: another restart keeps the take, and
    // the pane's pass (which commits a capture it finds gone) does not
    // write it again; it arms a new capture, as a click would.
    f.exec("history.undo", "{}");
    CHECK(f.app->launchCapture()->commit == engine::EngineApp::CaptureCommit::Undone);
    f.exec("engine.stop", "{}");
    f.exec("engine.start", R"({"backend":"dummy"})");
    REQUIRE(f.app->launchCapture());
    CHECK(f.app->launchCapture()->kept);
    capture.service();
    REQUIRE(capture.set(true));
    CHECK(capture.armed());
    CHECK(f.app->launchCapturing());
    CHECK(refused.empty());
    CHECK(f.app->head().version == before);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

namespace {
// Capture armed by the pane, Kit A launched and a beat of it heard, as the
// app's loop sees it (the shell's pass: service).
void captureKitA(LauncherFixture& f, gui::CaptureControl& capture) {
    REQUIRE(capture.set(true));
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    const auto service = [&] { capture.service(); };
    REQUIRE(f.runUntil(playing, at, "Kit A heard", service));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at, "a beat of Kit A", service));
}
bool laidOnDrums(const model::Session& s) {
    bool laid = false;
    for (const auto& e : s.items) laid = laid || e.value->track == *model::Id::parse("tr_1");
    return laid;
}
}  // namespace

TEST_CASE("launcher: Stop audio, then Start: the pane commits the kept take and arms nothing") {
    // Review round 168 F1: the engine stops while Capture is lit; the app
    // keeps the take. The pane's next pass, with no engine, commits it (one
    // undo step) and says so in its own words; after Start nothing is armed,
    // so the next Play replaces nothing.
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    std::vector<std::string> said;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    capture.onNotice = [&](const std::string& n) { said.push_back(n); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    captureKitA(f, capture);
    const std::uint64_t before = f.app->head().version;
    f.exec("engine.stop", "{}");
    REQUIRE(f.app->captureUncommitted());
    capture.service();
    CHECK(refused.empty());
    CHECK(f.app->head().version == before + 1);
    CHECK(laidOnDrums(f.app->head()));
    CHECK_FALSE(f.app->captureUncommitted());
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(capture.lit());
    CHECK(capture.keptLine().empty());
    REQUIRE(said.size() == 1);
    INFO(said[0]);
    CHECK(said[0].rfind("The engine stopped during the capture: its take (bar", 0) == 0);
    CHECK(said[0].find(", 1 launch) is committed.") != std::string::npos);
    CHECK(said[0].find("launch.commit") == std::string::npos);  // no command names
    f.exec("engine.start", R"({"backend":"dummy"})");
    capture.service();
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK(f.app->head().version == before + 1);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE(
    "launcher: Restart audio in the audio dialog: the pane says what the stop kept, committed") {
    // Round 168: the dialog's stop discards engine.stop's warning; the pane
    // says it instead, in its own words, from the app's view of the take.
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<std::string> said;
    capture.onNotice = [&](const std::string& n) { said.push_back(n); };
    gui::AudioDeviceSettings settings;
    settings.backend = "dummy";
    gui::AudioDeviceDialog audio(*f.root, *f.dispatcher, *f.app, settings, {});
    REQUIRE(audio.start());
    captureKitA(f, capture);
    const std::uint64_t before = f.app->head().version;
    REQUIRE(audio.restart());
    capture.service();
    CHECK(f.app->head().version == before + 1);
    CHECK_FALSE(capture.armed());
    REQUIRE(said.size() == 1);
    CHECK(said[0].rfind("The engine stopped during the capture: its take (", 0) == 0);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a device restart while capturing commits the take; nothing arms after it") {
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    std::vector<std::string> said;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    capture.onNotice = [&](const std::string& n) { said.push_back(n); };
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Thread, .paced = true});
    struct Stop {
        engine::EngineApp& app;
        ~Stop() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256}, {}));
    captureKitA(f, capture);
    const std::uint64_t before = f.app->head().version;
    backend.requestRestart("the device went away");
    f.app->tick();  // stops the engine, keeping the take (endedBy Device)
    REQUIRE(f.app->launchCapture());
    CHECK(f.app->launchCapture()->endedBy == engine::EngineApp::CaptureEnd::Device);
    for (int pass = 0; pass < 3; ++pass) {  // the restart's own passes too
        capture.service();
        f.app->tick();
    }
    CHECK(refused.empty());
    CHECK(f.app->head().version == before + 1);
    CHECK(laidOnDrums(f.app->head()));
    CHECK_FALSE(f.app->captureUncommitted());
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    REQUIRE(said.size() == 1);
    CHECK(said[0].rfind("The audio device restarted during the capture: its take (", 0) == 0);
    CHECK(f.errors.empty());
}

TEST_CASE(
    "launcher: a refused take kept across Stop audio shows kept; a click commits it, then arms") {
    // The refused take outlives the engine. The cell shows it kept, with the
    // refusal in the pane's words; a click while still locked is refused and
    // never arms over it (the next Play would replace it); unlocked, a click
    // commits the kept take, not the new engine's empty capture.
    LauncherFixture f;
    f.song();
    f.exec("item.add",
           R"({"track":"tr_1","bar":1,"notation":"voice d; 1: C2:w | C2:w | C2:w | C2:w | )"
           R"(C2:w | C2:w | C2:w | C2:w"})");
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
    captureKitA(f, capture);
    const std::uint64_t locked = f.app->head().version;
    f.exec("engine.stop", "{}");
    capture.service();
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].code == "item.locked");
    CHECK(refused[0].path.ends_with("/performance/launches/0"));  // the launch it met
    CHECK_FALSE(capture.armed());
    CHECK(capture.kept());
    const std::string line = capture.keptLine();
    INFO(line);
    CHECK(line.rfind("A take is kept, not committed: bar", 0) == 0);
    // In the pane's words, from the refusal's code and the model.
    CHECK(line.find("Its commit was refused: the item at bar 1 on Drums is locked.") !=
          std::string::npos);
    CHECK(line.find("Click Capture to commit it.") != std::string::npos);
    CHECK(line.find("item.set") == std::string::npos);  // never the hint
    // The cell: dark, its dot in outline, the line its hint.
    f.view->setCaptureKept(line);
    f.sync();
    views::LauncherHeader& header = f.view->launcherHeader();
    CHECK(header.stats.captureKept);
    CHECK_FALSE(header.stats.captureLit);
    CHECK(header.hintAt(centre(header.captureRect())).name == line);
    // Refresh is not held by a kept take (the stop keeps it); it is while a
    // capture is armed or runs (gui::refreshHeldBy, the app's hook).
    {
        gui::AudioDeviceSettings settings;
        settings.backend = "dummy";
        gui::AudioDeviceDialog audio(*f.root, *f.dispatcher, *f.app, settings, {});
        audio.enumerate = [] {
            return Result<std::vector<engine::AudioDevice>>(std::vector<engine::AudioDevice>{});
        };
        audio.captureHeld = gui::refreshHeldBy(capture);
        REQUIRE(audio.start());  // Refresh restarts a running engine
        CHECK(audio.refreshBlocked().empty());
        REQUIRE(audio.rescan());
        CHECK(f.app->engine() != nullptr);
        capture.service();
        CHECK(capture.kept());  // still the app's, still refused
        CHECK(f.app->head().version == locked);
    }
    capture.service();
    CHECK(capture.kept());
    // Still locked: refused, nothing armed, the take still the app's.
    const Status again = capture.set(true);
    REQUIRE_FALSE(again);
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK(f.app->captureUncommitted());
    CHECK(capture.keptLine().find("Click Capture again to drop it and arm.") != std::string::npos);
    lock(false);
    const std::uint64_t unlocked = f.app->head().version;
    REQUIRE(capture.set(true));
    CHECK(f.app->head().version == unlocked + 1);
    CHECK(f.app->head().version > locked);
    CHECK_FALSE(f.app->captureUncommitted());
    CHECK(capture.armed());
    CHECK(capture.keptLine().empty());
    CHECK(gui::refreshHeldBy(capture)());  // armed: Refresh would cut it short
    f.view->setCaptureKept(capture.keptLine());
    f.sync();
    CHECK_FALSE(header.stats.captureKept);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a kept take's refusal is worded from its code and path") {
    // The clip it played deleted after a refused commit (the app's kept
    // copy: the engine's own capture drops a removed clip's launches):
    // launch.commit at /performance/launches/N/clip, said from launch N.
    LauncherFixture f;
    f.song();
    f.exec("item.add",
           R"({"track":"tr_1","bar":1,"notation":"voice d; 1: C2:w | C2:w | C2:w | C2:w"})");
    const std::string item = f.app->head().items.begin()->id.str();
    f.exec("item.set", R"({"item":")" + item + R"(","locked":true})");
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    captureKitA(f, capture);
    // Kept by the app across Stop audio (its commit refused by the lock).
    f.exec("engine.stop", "{}");
    capture.service();
    REQUIRE(refused.size() == 1);
    REQUIRE(capture.kept());
    f.exec("clip.remove", R"({"clip":"cl_1"})");
    const Status again = capture.set(true);
    REQUIRE_FALSE(again);
    CHECK(again.error().code == "launch.commit");
    const std::string line = capture.keptLine();
    INFO(line);
    CHECK(line.find("Its commit was refused: a clip it played was deleted.") != std::string::npos);
    CHECK(line.find("cl_1") == std::string::npos);
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a refused take's words name the launch its refusal met") {
    // Review round 173 F3: the refusal's /performance/launches/N names the
    // launch's track (only its locked items count) and, for a clip's loop
    // window, the launch's clip on its track.
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    const auto lockOn = [&](const char* track, const char* notation) {
        const std::size_t had = f.app->head().items.size();
        f.exec("item.add", std::string(R"({"track":")") + track + R"(","bar":1,"notation":")" +
                               notation + R"("})");
        REQUIRE(f.app->head().items.size() == had + 1);
        std::string id;
        for (const auto& e : f.app->head().items)
            if (e.value->track == *model::Id::parse(track) && !e.value->locked) id = e.id.str();
        f.exec("item.set", R"({"item":")" + id + R"(","locked":true})");
    };
    std::string line;
    const auto refuse = [&](const char* code) {
        f.exec("engine.stop", "{}");
        capture.service();
        REQUIRE(refused.size() == 1);
        CHECK(refused[0].code == code);
        CHECK(refused[0].path.ends_with("/performance/launches/0"));
        REQUIRE(capture.kept());
        line = capture.keptLine();
    };
    f.exec("engine.start", R"({"backend":"dummy"})");
    SUBCASE("two locked items on the launch's track") {
        lockOn("tr_1", "voice d; 1: C2:w");
        f.exec("item.add", R"({"track":"tr_1","bar":2,"notation":"voice d; 1: C2:w | C2:w"})");
        for (const auto& e : f.app->head().items)
            if (!e.value->locked)
                f.exec("item.set", R"({"item":")" + e.id.str() + R"(","locked":true})");
        captureKitA(f, capture);
        // Into bar 2: both items under the take, in Kit A's way.
        REQUIRE(f.runUntil([&] { return f.playingBeat() >= 6; }, 6, "bar 2",
                           [&] { capture.service(); }));
        refuse("item.locked");
        INFO(line);
        CHECK(line.find("Its commit was refused: a locked item on Drums is in the way.") !=
              std::string::npos);
    }
    SUBCASE("a locked item on another launched track is not counted") {
        // Bass follows the arrangement until Root lands, past its item: the
        // item is under the take but not in the way, and Drums' one is named.
        lockOn("tr_1", "voice d; 1: C2:w | C2:w | C2:w | C2:w");
        lockOn("tr_2", "voice b; 1: C2:w | C2:w");
        captureKitA(f, capture);
        const auto service = [&] { capture.service(); };
        REQUIRE(f.runUntil([&] { return f.playingBeat() >= 8; }, 8, "past Bass's item", service));
        const double rootAt = f.exec("launch.clip", R"({"clip":"cl_3"})")["beat"].asDouble();
        CHECK(rootAt >= 8);
        REQUIRE(f.runUntil([&] { return f.playingBeat() >= rootAt + 1; }, rootAt, "Root heard",
                           service));
        refuse("item.locked");
        INFO(line);
        CHECK(line.find("Its commit was refused: the item at bar 1 on Drums is locked.") !=
              std::string::npos);
    }
    SUBCASE("a clip's loop window names the clip on its track") {
        captureKitA(f, capture);
        f.exec("clip.set", R"({"clip":"cl_1","length":8,"loop":{"on":true,"start":1,"length":2}})");
        refuse("clip.unsupported");
        INFO(line);
        CHECK(line.find("Its commit was refused: Kit A on Drums loops only part of its content, "
                        "which an item cannot.") != std::string::npos);
    }
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a take whose commit was undone is declined: a click arms over it") {
    // Q1 (co-director): the undone take is not committed again, not shown
    // kept; a click arms, and the take stays the app's (redo restores it)
    // until the new capture hears a frame, which the status line says once.
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    std::vector<std::string> said;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    capture.onNotice = [&](const std::string& n) { said.push_back(n); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    captureKitA(f, capture);
    REQUIRE(capture.set(false));  // ends it on the quantize boundary
    const auto service = [&] { capture.service(); };
    REQUIRE(
        f.runUntil([&] { return !capture.lit(); }, f.playingBeat() + 4, "the end heard", service));
    f.exec("transport.stop", "{}");
    REQUIRE(laidOnDrums(f.app->head()));
    f.exec("history.undo", "{}");
    REQUIRE(f.app->launchCapture());
    CHECK(f.app->launchCapture()->commit == engine::EngineApp::CaptureCommit::Undone);
    capture.service();
    CHECK(capture.keptLine().empty());
    CHECK_FALSE(capture.kept());
    const std::uint64_t undone = f.app->head().version;
    said.clear();
    REQUIRE(capture.set(true));
    CHECK(capture.armed());
    CHECK(f.app->head().version == undone);  // not committed again
    CHECK(f.app->launchCapture()->kept);     // the declined take, until a frame
    CHECK(said.empty());
    f.exec("transport.play", "{}");
    // The beat bound is the transport's, wherever the stop left it.
    REQUIRE(f.runUntil([&] { return !f.app->launchCapture()->kept; }, 1e6, "the new capture heard",
                       service));
    capture.service();
    REQUIRE(said.size() == 1);
    CHECK(said[0] == "The earlier capture was replaced by this one.");
    CHECK(f.app->head().version == undone);
    REQUIRE(capture.set(false));
    f.exec("engine.stop", "{}");
    CHECK(refused.empty());
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: Capture turned off while playing ends on the next bar, then commits") {
    // gui.md §4.15: off while playing ends the capture on the project's launch
    // quantize boundary, as a launch made then would land; the commit follows
    // once the engine has heard it, and nothing is armed after it.
    LauncherFixture f;
    f.song();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    REQUIRE(capture.set(true));
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    const auto service = [&] { capture.service(); };
    REQUIRE(f.runUntil(playing, at, "Kit A heard", service));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at, "a beat of Kit A", service));
    const std::uint64_t before = f.app->head().version;
    REQUIRE(capture.set(false));
    const auto pending = f.app->engine()->launchCapture();
    REQUIRE(pending);
    REQUIRE(pending->endsBeat);
    const double ends = *pending->endsBeat;
    CHECK(ends > at + 1);
    CHECK(std::fmod(ends, 4.0) == 0.0);  // a bar line (4/4)
    // Still capturing, lit, and nothing written until the boundary is heard.
    CHECK(capture.ending());
    CHECK_FALSE(capture.armed());
    CHECK(capture.lit());
    CHECK(f.app->head().version == before);
    REQUIRE(
        f.runUntil([&] { return !f.app->launchCapturing(); }, ends, "the capture's end", service));
    capture.service();
    CHECK(refused.empty());
    CHECK_FALSE(capture.ending());
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK(f.app->head().version == before + 1);  // one undo step
    bool laid = false;
    for (const auto& e : f.app->head().items) laid = laid || e.value->track == drums;
    CHECK(laid);
    // It ended on that boundary exactly.
    const auto done = f.app->engine()->launchCapture();
    REQUIRE(done);
    const model::Session s = f.app->head();
    const double rate = f.app->engine()->config().sampleRate;
    CHECK(done->startSample + done->frames == time::samplesAt(s.timeMap->secondsAt(ends), rate));
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: the ending Capture cell is dashed; a second click ends it at once") {
    // Review round 104 Y3 (gui.md §4.15): turned off while playing, the cell
    // stays lit and is outlined dashed, as a queued cell, until the boundary
    // is heard. A second click before then ends the capture at once and
    // commits it, short of the boundary: an impatient click means stop.
    LauncherFixture f;
    f.song();
    views::LauncherHeader& header = f.view->launcherHeader();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    views::LauncherCallbacks callbacks = f.view->launcherCallbacks();
    callbacks.capture = [&](bool on) { REQUIRE(capture.set(on)); };
    f.view->setLauncherCallbacks(callbacks);
    auto show = [&] {  // as the shell does once a pass
        capture.service();
        f.view->setCaptureArmed(capture.lit());
        f.view->setCaptureEnding(capture.ending());
        f.sync();
    };
    f.exec("engine.start", R"({"backend":"dummy"})");
    f.click(centre(header.captureRect()));  // armed
    show();
    CHECK(capture.armed());
    CHECK(header.stats.captureLit);
    CHECK_FALSE(header.stats.captureEnding);
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
    const std::uint64_t before = f.app->head().version;
    f.click(centre(header.captureRect()));  // off: ending on the next bar
    show();
    const auto pending = f.app->engine()->launchCapture();
    REQUIRE(pending);
    REQUIRE(pending->endsBeat);
    const double ends = *pending->endsBeat;
    CHECK(capture.ending());
    CHECK(header.stats.captureLit);
    CHECK(header.stats.captureEnding);
    CHECK(f.app->head().version == before);
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        REQUIRE(gui::writePng(std::filesystem::path(out) / "launcher-capture-ending.png",
                              f.root->readBack()));
    f.click(centre(header.captureRect()));  // again: ends now
    show();
    CHECK(refused.empty());
    CHECK_FALSE(capture.ending());
    CHECK_FALSE(capture.armed());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK_FALSE(header.stats.captureLit);
    CHECK_FALSE(header.stats.captureEnding);
    CHECK(f.app->head().version == before + 1);  // committed, one undo step
    const auto done = f.app->engine()->launchCapture();
    REQUIRE(done);
    CHECK_FALSE(done->endsBeat);
    const model::Session s = f.app->head();
    const double rate = f.app->engine()->config().sampleRate;
    CHECK(done->startSample + done->frames < time::samplesAt(s.timeMap->secondsAt(ends), rate));
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: an agent's quantized capture end dashes the cell; a click ends it at once") {
    // Review round 107 X1 (gui.md §4.15): the cell shows the engine's
    // pending end, not only the toggle's own. An agent ends its capture on
    // the next beat while the project quantizes to the bar: the cell is
    // dashed, and a click ends the capture at once and commits it, rather
    // than moving the end to the project's bar.
    LauncherFixture f;
    f.song();
    views::LauncherHeader& header = f.view->launcherHeader();
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    views::LauncherCallbacks callbacks = f.view->launcherCallbacks();
    callbacks.capture = [&](bool on) { REQUIRE(capture.set(on)); };
    f.view->setLauncherCallbacks(callbacks);
    auto show = [&] {  // as the shell does once a pass
        capture.service();
        f.view->setCaptureArmed(capture.lit());
        f.view->setCaptureEnding(capture.ending());
        f.sync();
    };
    REQUIRE(f.app->head().project->launchQuantize == model::LaunchQuantize::Bar);
    f.exec("engine.start", R"({"backend":"dummy"})");
    f.exec("launch.capture", R"({"on":true})");
    show();
    CHECK_FALSE(capture.armed());
    CHECK(header.stats.captureLit);
    CHECK_FALSE(header.stats.captureEnding);
    const double at = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    REQUIRE(f.runUntil(playing, at, "Kit A heard", show));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 0.25; }, at, "into Kit A", show));
    const std::uint64_t before = f.app->head().version;
    const double ends =
        f.exec("launch.capture", R"({"on":false,"quantize":"beat"})")["ends_at"].asDouble();
    show();
    CHECK(capture.ending());
    CHECK(header.stats.captureLit);
    CHECK(header.stats.captureEnding);
    f.click(centre(header.captureRect()));  // ends now, not on the project's bar
    show();
    CHECK(refused.empty());
    CHECK_FALSE(capture.ending());
    CHECK_FALSE(f.app->launchCapturing());
    CHECK_FALSE(header.stats.captureLit);
    CHECK_FALSE(header.stats.captureEnding);
    CHECK(f.app->head().version == before + 1);  // committed, one undo step
    const auto done = f.app->engine()->launchCapture();
    REQUIRE(done);
    CHECK_FALSE(done->endsBeat);
    const model::Session s = f.app->head();
    const double rate = f.app->engine()->config().sampleRate;
    CHECK(done->startSample + done->frames < time::samplesAt(s.timeMap->secondsAt(ends), rate));
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a Capture commit says which clips it copied under another key") {
    // gui.md §4.15: the commit's mismatches in the status line, as a lay's.
    LauncherFixture f;
    f.song();
    f.exec("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key D minor ~:w"})");
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    std::vector<std::string> notices;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    capture.onNotice = [&](const std::string& n) { notices.push_back(n); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    REQUIRE(capture.set(true));
    // Root (Bass), written in D minor, launched under C minor.
    const double at = f.exec("launch.clip", R"({"clip":"cl_3"})")["beat"].asDouble();
    const model::Id bass = *model::Id::parse("tr_2");
    auto playing = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == bass && t.plays == engine::Engine::TrackLaunch::Plays::Clip) return true;
        return false;
    };
    const auto service = [&] { capture.service(); };
    REQUIRE(f.runUntil(playing, at, "Root heard", service));
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= at + 1; }, at, "a beat of Root", service));
    f.exec("transport.stop", "{}");
    f.app->tick();
    capture.service();
    CHECK(refused.empty());
    REQUIRE(notices.size() == 1);
    CHECK(notices[0] ==
          "Committed: Root is laid as written: its key D minor differs from C minor at bar 1");
    // Kit A, with no key, commits with nothing to say.
    const double kit = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
    const model::Id drums = *model::Id::parse("tr_1");
    auto kitPlaying = [&] {
        for (const auto& t : f.app->launchState())
            if (t.track == drums && t.plays == engine::Engine::TrackLaunch::Plays::Clip)
                return true;
        return false;
    };
    REQUIRE(f.runUntil(kitPlaying, kit, "Kit A heard", service));
    REQUIRE(
        f.runUntil([&] { return f.playingBeat() >= kit + 1; }, kit, "a beat of Kit A", service));
    REQUIRE(capture.set(false));
    CHECK(notices.size() == 1);
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
    // Dropped: the app keeps no copy of it either (ADR-0097, a capture kept).
    CHECK_FALSE(f.app->captureUncommitted());
    REQUIRE(capture.set(false));
    CHECK(f.app->head().version == locked);
    f.exec("engine.stop", "{}");
    CHECK(f.errors.empty());
}

TEST_CASE(
    "launcher: the Capture cell is lit by an agent's capture; a click ends it on the quantize and "
    "commits it") {
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
    // A click on the lit cell ends that capture on the next launch quantize
    // boundary, as the toggle's own; the cell stays lit until it is heard,
    // then the app commits it.
    const std::uint64_t before = f.app->head().version;
    f.click(centre(header.captureRect()));
    show();
    CHECK(capture.ending());
    CHECK(capture.lit());
    CHECK(header.stats.captureLit);
    CHECK(f.app->head().version == before);
    const auto pending = f.app->engine()->launchCapture();
    REQUIRE(pending);
    REQUIRE(pending->endsBeat);
    REQUIRE(f.runUntil([&] { return !f.app->launchCapturing(); }, *pending->endsBeat,
                       "the capture's end", show));
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

namespace {
// The pane against the engine (step E): the clips drawn dimmed and the scenes
// whose ▶ is dimmed are exactly those a press would be refused for now
// (EngineApp::launchRefusal, sceneLaunchRefusal), each clip by the code the
// view holds. The refused clips' ids.
std::set<model::Id> agreeWithEngine(LauncherFixture& f, const char* when) {
    INFO(when);
    f.sync();
    f.root->damageAll();
    f.root->frame(false);
    const views::LauncherPane& pane = f.view->launcherPane();
    const views::LauncherHeader& header = f.view->launcherHeader();
    const views::LaunchRefusals& held = f.view->launchRefusals();
    const model::Session& s = f.app->head();
    std::set<model::Id> clips, scenes;
    for (const auto& e : s.clips)
        if (const auto refused = f.app->launchRefusal(e.id)) {
            clips.insert(e.id);
            const auto it = held.clips.find(e.id);
            INFO(e.id.str() << " " << refused->code);
            CHECK((it != held.clips.end() && it->second == refused->code));
        }
    for (const auto& e : s.scenes)
        if (f.app->sceneLaunchRefusal(e.id)) scenes.insert(e.id);
    CHECK(std::set<model::Id>(pane.stats.refusedClips.begin(), pane.stats.refusedClips.end()) ==
          clips);
    CHECK(pane.stats.refused == static_cast<int>(clips.size()));
    CHECK(std::set<model::Id>(header.stats.refusedScenes.begin(),
                              header.stats.refusedScenes.end()) == scenes);
    CHECK(header.stats.refused == static_cast<int>(scenes.size()));
    return clips;
}

// A mono file of one second at `level`, at the fixture's 48 kHz.
std::string writeTone(LauncherFixture& f, const char* name, float level) {
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(1, std::vector<float>(48000, level));
    const auto path = f.dir / name;
    REQUIRE(audio::writeWav(path, tone));
    return path.string();
}

std::string addAudioClip(LauncherFixture& f, const char* track, const char* scene,
                         const std::string& file) {
    json::Value add = json::Value::object();
    add["track"] = track;
    add["scene"] = scene;
    add["audio_file"] = file;
    return f.exec("clip.add", *json::write(add))["clip"].asString();
}
}  // namespace

TEST_CASE("launcher: a locked item under a fade-out's tail counts in a refused take's words") {
    // Review round 173 F4: the commit clears under an audio clip's fade-out
    // still sounding at the take's end, past endBeat. A locked item there is
    // the one in the way; a locked item earlier on the track, where it still
    // followed the arrangement, must not be named instead.
    LauncherFixture f;
    f.song();
    REQUIRE(f.app->head().timeMap->secondsAt(4) == doctest::Approx(2));  // 120 bpm, 4/4
    // Twelve beats a pass, the last eight fading out.
    const std::string pad = addAudioClip(f, "tr_1", "sc_3", [&] {
        audio::AudioData tone;
        tone.sampleRate = 48000;
        tone.channels.assign(1, std::vector<float>(6 * 48000, 0.3f));
        const auto path = f.dir / "pad.wav";
        REQUIRE(audio::writeWav(path, tone));
        return path.string();
    }());
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_out":4})");
    gui::CaptureControl capture(*f.app, *f.dispatcher);
    std::vector<Error> refused;
    capture.onError = [&](const Error& e) { refused.push_back(e); };
    f.exec("engine.start", R"({"backend":"dummy"})");
    const auto service = [&] { capture.service(); };
    // The take starts with Root on Bass; Drums follows the arrangement.
    REQUIRE(capture.set(true));
    const double b0 = f.exec("launch.clip", R"({"clip":"cl_3"})")["beat"].asDouble();
    REQUIRE(f.runUntil([&] { return f.playingBeat() >= b0 + 1; }, b0, "Root heard", service));
    SUBCASE("the pad alone") {}
    SUBCASE("Kit A, then the pad: the run's last launch has the tail") {
        // Review round 173 re-check H1: the refusal names Kit A, the run's
        // first launch; the tail is the pad's.
        const double kit = f.exec("launch.clip", R"({"clip":"cl_1"})")["beat"].asDouble();
        REQUIRE(kit >= b0 + 4);
        REQUIRE(
            f.runUntil([&] { return f.playingBeat() >= kit + 1; }, kit, "Kit A heard", service));
    }
    const double b1 = f.exec("launch.clip", R"({"clip":")" + pad + R"("})")["beat"].asDouble();
    REQUIRE(b1 >= b0 + 4);
    const auto lockAt = [&](double beat) {
        json::Value add = json::Value::object();
        add["track"] = "tr_1";
        add["bar"] = static_cast<int>(beat / 4) + 1;
        add["notation"] = "voice d; 1: C2:w";
        f.exec("item.add", *json::write(add));
        for (const auto& e : f.app->head().items)
            if (!e.value->locked)
                f.exec("item.set", R"({"item":")" + e.id.str() + R"(","locked":true})");
    };
    lockAt(b0);      // under the take, before the pad: not in the way
    lockAt(b1 + 8);  // past the take's end, under the pad's fade-out
    // Stopped six beats into the pad's pass: inside its fade-out.
    REQUIRE(
        f.runUntil([&] { return f.playingBeat() >= b1 + 6; }, b1 + 6, "into the fade", service));
    f.exec("engine.stop", "{}");
    capture.service();
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].code == "item.locked");
    CHECK(refused[0].path.ends_with("/performance/launches/1"));  // Drums' first launch
    REQUIRE(f.app->launchCapture());
    REQUIRE(f.app->launchCapture()->endBeat < b1 + 8);  // the tail's item is past the take
    const std::string line = capture.keptLine();
    INFO(line);
    CHECK(line.find("Its commit was refused: a locked item on Drums is in the way.") !=
          std::string::npos);
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: the pane dims what a press would be refused for, as the engine answers") {
    // Step E: the pane no longer copies the engine's rule; it asks
    // EngineApp::launchRefusal and sceneLaunchRefusal, again whenever
    // launchRefusalGeneration moves. After step D a plain audio clip launches
    // under a pitch-keeping playrate; one read through a take chain (here a
    // take envelope) is refused (launch.playrate), and so is the scene
    // holding it. A frozen track's clips are refused (track.frozen), engine
    // or not; a scene left with nothing to play answers launch.scene.
    LauncherFixture f;
    f.song();
    using model::Id;
    const auto id = [](const char* text) { return *Id::parse(text); };
    // Keys: a plain audio clip in Chorus, and one with a take envelope in Intro.
    const Id pad = id(addAudioClip(f, "tr_3", "sc_3", writeTone(f, "pad.wav", 0.25f)).c_str());
    const Id chained =
        id(addAudioClip(f, "tr_3", "sc_1", writeTone(f, "chained.wav", 0.5f)).c_str());
    json::Value env = json::Value::object();
    env["take"] = f.app->head().clips.find(chained)->activeTake->str();
    env["param"] = "volume";
    env["points"] =
        json::parse(R"([{"seconds":0,"value":0},{"seconds":1,"value":"-6 dB"}])").value();
    f.exec("env.add", *json::write(env));
    views::LauncherPane& pane = f.view->launcherPane();
    views::LauncherHeader& header = f.view->launcherHeader();
    // No engine: nothing is refused for the clip itself (a press answers
    // engine.stopped, which is not shown).
    CHECK(agreeWithEngine(f, "stopped").empty());
    f.exec("engine.start", R"({"backend":"dummy"})");
    CHECK(agreeWithEngine(f, "running at 1").empty());
    // A rate that moves the pitch: every clip launches.
    f.exec("transport.playrate.set", R"({"rate":1.5})");
    CHECK(agreeWithEngine(f, "1.5 moving the pitch").empty());
    // Keeping the pitch: the take-envelope clip and Intro's ▶ dim; the plain
    // audio clip, the MIDI clips and the other scenes do not.
    f.exec("transport.playrate.set", R"({"rate":1.5,"preserve_pitch":true})");
    CHECK(agreeWithEngine(f, "1.5 keeping the pitch") == std::set<Id>{chained});
    CHECK(f.view->launchRefusals().clips.at(chained) == "launch.playrate");
    CHECK_FALSE(f.app->launchRefusal(pad));  // plain audio launches (step D)
    CHECK(header.stats.refusedScenes == std::vector<Id>{id("sc_1")});
    CHECK(f.view->launchRefusals().scenes.at(id("sc_1")) == "launch.playrate");
    f.golden("launcher-refused-dark.png");
    f.snapshot("launcher-refusals-playrate.png");
    // Its ▶ still asks, and the refusal reaches the status line with the
    // code the pane holds.
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(f.row("tr_3"), 0))));
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().code == "launch.playrate");
    // A MIDI clip's launches.
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(f.row("tr_1"), 0))));
    CHECK(f.errors.size() == 1);
    // At 1 with the pitch still kept, and with it off: whatever the plan
    // then answers, the pane says the same; off, nothing dims.
    f.exec("transport.playrate.set", R"({"rate":1.0,"preserve_pitch":true})");
    agreeWithEngine(f, "1 keeping the pitch");
    f.exec("transport.playrate.set", R"({"rate":1.0,"preserve_pitch":false})");
    CHECK(agreeWithEngine(f, "1, pitch not kept").empty());
    // A clip with new audio, just added: its load waits for the audio's
    // preparation, so until then a press answers launch.clip (wait for the
    // edit) and the pane dims it, and Outro's ▶ with it (Outro holds only
    // this clip); prepared and loaded, nothing is dimmed.
    f.exec("scene.add", R"({"name":"Outro"})");  // sc_4
    const Id fresh = id(addAudioClip(f, "tr_2", "sc_4", writeTone(f, "fresh.wav", 0.4f)).c_str());
    REQUIRE(f.app->preparingAudio());  // the load waits for it
    CHECK(agreeWithEngine(f, "a clip added, its audio preparing") == std::set<Id>{fresh});
    CHECK(f.view->launchRefusals().clips.at(fresh) == "launch.clip");
    CHECK(f.view->launchRefusals().scenes.at(id("sc_4")) == "launch.clip");
    // Prepared, then loaded on a later service (a load also waits while a
    // rate change is in flight, as the one above may be).
    for (int i = 0; f.app->preparingAudio() || f.app->launchRefusal(fresh); ++i) {
        REQUIRE(i < 100'000);  // a hang guard, not a timing check
        f.app->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        const std::set<Id> loaded = agreeWithEngine(f, "the added clip prepared and loaded");
        std::string held;
        for (const auto& [clip, code] : f.view->launchRefusals().clips)
            held += clip.str() + " " + code + "; ";
        for (const auto& [scene, code] : f.view->launchRefusals().scenes)
            held += scene.str() + " " + code + "; ";
        INFO("refused: " << held);
        CHECK(loaded.empty());
    }
    // Freezing needs an item on the track: Bass gets an audio one.
    {
        json::Value item = json::Value::object();
        item["track"] = "tr_2";
        item["bar"] = 1;
        item["audio_file"] = writeTone(f, "bass.wav", 0.3f);
        f.exec("item.add", *json::write(item));
    }
    // Freezing Bass: its clips are refused from the edit on, before its print
    // plays; Outro has nothing left to play (launch.scene), Verse still
    // launches Kit B.
    f.exec("transport.stop", "{}");
    const std::uint64_t plans = f.app->engine()->planGeneration();
    f.exec("track.freeze", R"({"track":"tr_2"})");
    CHECK(agreeWithEngine(f, "frozen, before its print") == std::set<Id>{id("cl_3"), fresh});
    CHECK(f.view->launchRefusals().clips.at(fresh) == "track.frozen");
    CHECK(header.stats.refusedScenes == std::vector<Id>{id("sc_4")});
    CHECK(f.view->launchRefusals().scenes.at(id("sc_4")) == "launch.scene");
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(f.row("tr_2"), 1))));
    REQUIRE(f.errors.size() == 2);
    CHECK(f.errors.back().code == "track.frozen");
    // Outro's dimmed ▶ still asks, and launch.scene's refusal reaches the
    // status line.
    const ui::Rect outro = header.sceneRect(3);
    f.click({outro.x + 8, outro.y + outro.h / 2});
    REQUIRE(f.errors.size() == 3);
    CHECK(f.errors.back().code == "launch.scene");
    // Once the print's plan is published the answers are the same.
    const auto start = std::chrono::steady_clock::now();
    while (f.app->engine()->planGeneration() == plans &&
           std::chrono::steady_clock::now() - start < std::chrono::minutes(2)) {
        f.app->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(f.app->engine()->planGeneration() != plans);  // a hang guard, not a timing check
    CHECK(agreeWithEngine(f, "frozen, its print published") == std::set<Id>{id("cl_3"), fresh});
    // (A frozen track locks timing: the playrate cannot change now.)
    f.snapshot("launcher-refusals-frozen.png");
    // Stopped: a frozen track's clips are still refused (launch.clip checks
    // the freeze before the engine); no scene is (launch.scene asks for the
    // engine first), and the take-envelope clip is not.
    f.exec("engine.stop", "{}");
    CHECK(agreeWithEngine(f, "stopped, Bass frozen") == std::set<Id>{id("cl_3"), fresh});
    CHECK(header.stats.refused == 0);
}

TEST_CASE("launcher: a clip whose audio did not fit dims alone; its scene still launches") {
    // ADR-0097 "Memory", as test_clip_launcher.cpp's "clip audio is prepared
    // after the arrangement…": three mono sources made 2^26 frames long in
    // the saved file (the estimate reads the model) and two fill the budget.
    // In launch order (scene, then track) cl_3, cl_1, cl_2, cl_4: cl_2's
    // source is skipped (launch.memory); cl_4 shares cl_1's and still fits,
    // so Verse launches it and its ▶ stays bright.
    LauncherFixture f;
    f.exec("track.add", R"({"type":"channel","name":"Drums"})");     // tr_1
    f.exec("track.add", R"({"type":"channel","name":"Pad"})");       // tr_2
    f.exec("scene.add", R"({"name":"Intro"})");                      // sc_1
    f.exec("scene.add", R"({"name":"Verse"})");                      // sc_2
    addAudioClip(f, "tr_2", "sc_1", writeTone(f, "a.wav", 0.25f));   // cl_1
    addAudioClip(f, "tr_1", "sc_2", writeTone(f, "b.wav", 0.5f));    // cl_2
    addAudioClip(f, "tr_1", "sc_1", writeTone(f, "c.wav", 0.125f));  // cl_3
    f.exec("clip.duplicate", R"({"clip":"cl_1","scene":"sc_2"})");   // cl_4
    const auto path = f.dir / ("memory." + std::string(product::kSlug));
    json::Value save = json::Value::object();
    save["path"] = path.string();
    f.exec("project.save", *json::write(save));
    std::stringstream text;
    {
        std::ifstream in(path);
        text << in.rdbuf();
    }
    auto doc = json::parse(text.str()).value();
    for (auto& m : doc["sources"]["media"].asObject()) m.value["frames"] = 1 << 26;
    {
        std::ofstream out(path, std::ios::trunc);
        out << json::writeCanonical(doc);
    }
    f.exec("project.open", *json::write(save));
    f.exec("engine.start", R"({"backend":"dummy"})");
    const model::Id skipped = *model::Id::parse("cl_2");
    CHECK(agreeWithEngine(f, "cl_2 skipped") == std::set<model::Id>{skipped});
    CHECK(f.view->launchRefusals().clips.at(skipped) == "launch.memory");
    views::LauncherPane& pane = f.view->launcherPane();
    views::LauncherHeader& header = f.view->launcherHeader();
    CHECK(header.stats.refused == 0);
    // Its ▶ asks and is refused, naming why; Verse's ▶ launches the rest.
    f.click(centre(views::LauncherPane::playZone(pane.cellRect(f.row("tr_1"), 1))));
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().code == "launch.memory");
    const ui::Rect verse = header.sceneRect(1);
    f.click({verse.x + 8, verse.y + verse.h / 2});
    CHECK(f.errors.size() == 1);
    f.exec("engine.stop", "{}");
}

TEST_CASE("launcher: every refusal code dims its clip as a muted one; the set comes from the app") {
    // The views do not see the engine: whatever the app sets is drawn, each
    // code the same way (no text, no mark of its own).
    LauncherFixture f;
    f.song();
    using model::Id;
    const auto id = [](const char* text) { return *Id::parse(text); };
    views::LaunchRefusals refusals;
    refusals.clips = {{id("cl_1"), "launch.memory"},
                      {id("cl_2"), "track.frozen"},
                      {id("cl_3"), "launch.playrate"}};
    refusals.scenes = {{id("sc_2"), "launch.playrate"}};
    f.view->setLaunchRefusals(refusals);
    f.root->damageAll();
    f.root->frame(false);
    views::LauncherPane& pane = f.view->launcherPane();
    views::LauncherHeader& header = f.view->launcherHeader();
    CHECK(pane.stats.refusedClips == std::vector<Id>{id("cl_1"), id("cl_2"), id("cl_3")});
    CHECK(header.stats.refusedScenes == std::vector<Id>{id("sc_2")});
    // The clips draw no more than they did: names and key marks unchanged.
    CHECK(pane.stats.names == std::vector<std::string>{"Kit A", "Kit B", "Root"});
    f.view->setLaunchRefusals({});
    f.root->damageAll();
    f.root->frame(false);
    CHECK(pane.stats.refused == 0);
    CHECK(header.stats.refused == 0);
}

TEST_CASE(
    "launcher: an audio clip's cell draws its fades where there is room; a MIDI clip's none") {
    LauncherFixture f;
    f.song();
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(1, std::vector<float>(96000));
    for (std::size_t i = 0; i < tone.channels[0].size(); ++i)
        tone.channels[0][i] = 0.6f * static_cast<float>(std::sin(static_cast<double>(i) * 0.03));
    const auto wav = f.dir / "pad.wav";
    REQUIRE(audio::writeWav(wav, tone));
    json::Value add = json::Value::object();
    add["track"] = "tr_3";
    add["scene"] = "sc_3";
    add["audio_file"] = wav.string();
    const std::string pad = f.exec("clip.add", *json::write(add))["clip"].asString();
    f.exec("clip.set", R"({"clip":")" + pad + R"(","name":"Pad"})");
    views::LauncherPane& pane = f.view->launcherPane();
    // Frames until the waveform's peaks are in (a deadline: the worker can be slow).
    auto draw = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        do {
            f.root->damageAll();
            f.root->frame(false);
            if (pane.stats.pending == 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("peaks never became ready");
    };
    draw();
    CHECK(pane.stats.fades == 0);  // none yet; the MIDI clips have none
    f.exec("clip.set",
           R"({"clip":")" + pad + R"(","fade_in":0.5,"fade_out":0.75,"fade_out_shape":"cos"})");
    draw();
    CHECK(pane.stats.columns > 0);
    CHECK(pane.stats.fades == 2);
    f.golden("launcher-fades-dark.png");
    // One fade, one curve.
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_out":0})");
    draw();
    CHECK(pane.stats.fades == 1);
    // A fade under 2 units wide is not drawn (a 2 s period across 64 units).
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_in":0.04})");
    draw();
    CHECK(pane.stats.fades == 0);
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_in":0.5})");
    draw();
    CHECK(pane.stats.fades == 1);
    // A body too low for them shows none.
    f.view->trackLayout().setTrackHeight(*model::Id::parse("tr_3"), 26);
    f.sync();
    draw();
    REQUIRE(views::LauncherPane::bodyRect(pane.cellRect(f.row("tr_3"), 2)).h <
            views::LauncherPane::kFadeRoom);
    CHECK(pane.stats.fades == 0);
    CHECK(f.errors.empty());
}

// Round 136 M1: a fade longer than the period is accepted, and the cell shows
// only the share a pass reaches, as the engine plays it: an 8 s linear fade-in
// on a 2 s pass ends at gain 0.25, so at the pass's end the shade covers the
// top three quarters of the body. Overlapping fades are each drawn on their own.
TEST_CASE("launcher: a fade longer than its clip's period shows the share a pass reaches") {
    LauncherFixture f;
    f.song();
    audio::AudioData silence;  // no waveform: the shade alone changes the pixels
    silence.sampleRate = 48000;
    silence.channels.assign(1, std::vector<float>(96000, 0.0f));
    const auto wav = f.dir / "silence.wav";
    REQUIRE(audio::writeWav(wav, silence));
    json::Value add = json::Value::object();
    add["track"] = "tr_3";
    add["scene"] = "sc_3";
    add["audio_file"] = wav.string();
    const std::string pad = f.exec("clip.add", *json::write(add))["clip"].asString();
    views::LauncherPane& pane = f.view->launcherPane();
    auto frame = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        do {
            f.root->damageAll();
            f.root->frame(false);
            if (pane.stats.pending == 0) return f.root->readBack();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("peaks never became ready");
        return gui::Image{};
    };
    const ui::Rect body = views::LauncherPane::bodyRect(pane.cellRect(f.row("tr_3"), 2));
    REQUIRE(body.h >= 20);
    // How far a pixel moved from the unfaded cell's (RGBA, summed).
    auto moved = [](const gui::Image& a, const gui::Image& b, float x, float y) {
        const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) +
                        static_cast<std::size_t>(x)) *
                       4;
        int d = 0;
        for (std::size_t c = 0; c < 4; ++c) d += std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c]));
        return d;
    };
    const gui::Image plain = frame();
    const float high = body.y + body.h * 0.35f, low = body.y + body.h * 0.9f;
    // Fade-in: at the pass's end the gain is 0.25 (the curve a quarter up).
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_in":8})");
    gui::Image faded = frame();
    CHECK(pane.stats.fades == 1);
    const float end = body.right() - 2;
    CHECK(moved(plain, faded, end, high) > 12);  // above the curve: shaded
    CHECK(moved(plain, faded, end, low) < 6);    // under it: as it was
    // Fade-out: it starts a pass at 0.25, the same at the pass's start.
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_in":0,"fade_out":8})");
    faded = frame();
    CHECK(pane.stats.fades == 1);
    const float start = body.x + 2;
    CHECK(moved(plain, faded, start, high) > 12);
    CHECK(moved(plain, faded, start, low) < 6);
    // Overlapping fades, 1.5 s each on a 2 s pass: two curves, each on its own.
    f.exec("clip.set", R"({"clip":")" + pad + R"(","fade_in":1.5,"fade_out":1.5})");
    (void)frame();
    CHECK(pane.stats.fades == 2);
    CHECK(f.errors.empty());
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
                                           "Insert at end", "Follow the song's key and chords",
                                           "Write harmony to the context track", "Lay out scenes…",
                                           "", "Delete"});
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

TEST_CASE("launcher: the scene menu's checks say how scenes are laid; Lay out scenes… builds") {
    LauncherFixture f;
    f.song();
    // Root, Bass's clip in Verse, was written in D minor.
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key D minor ~:w"})");
    views::LauncherHeader& header = f.view->launcherHeader();
    const float y = header.sceneRect(0).y + 8;
    auto menu = [&](std::size_t column) {
        f.click({header.sceneRect(column).x + 40, y}, 0, 1, 3);
        ui::Node* m = f.root->find("launcher.scene.menu");
        REQUIRE(m);
        std::vector<ui::MenuItem> rows(ui::menuItems(*m).begin(), ui::menuItems(*m).end());
        f.root->closePopups();
        return rows;
    };
    auto menuRun = [&](std::size_t column, std::size_t row) {
        const auto rows = menu(column);
        rows[row].run();
        f.sync();
    };
    // Both checks start unchecked: Insert at end sends neither.
    auto rows = menu(1);
    CHECK(rows[4].label == "Follow the song's key and chords");
    CHECK(rows[5].label == "Write harmony to the context track");
    CHECK_FALSE(rows[4].checked);
    CHECK_FALSE(rows[5].checked);
    menuRun(0, 3);  // Intro at the end: bar 1
    REQUIRE(f.commands.back() == "scene.insert");
    CHECK_FALSE(f.sent.back().find("follow"));
    CHECK_FALSE(f.sent.back().find("write_context"));
    // Checked, each pick toggling one: the menu shows them, Insert sends them.
    menuRun(1, 4);
    menuRun(1, 5);
    CHECK(f.view->sceneLaying() == views::SceneLaying{true, true});
    rows = menu(1);
    CHECK(rows[4].checked);
    CHECK(rows[5].checked);
    f.click({header.sceneRect(1).x + 40, y}, 0, 1, 3);
    f.snapshot("launcher-scene-menu-checks.png");
    f.root->closePopups();
    menuRun(1, 3);  // Verse at the end: bar 2, its harmony written under it
    REQUIRE(f.commands.back() == "scene.insert");
    CHECK(f.errors.empty());
    CHECK(f.sent.back().find("follow")->asString() == "chords");
    CHECK(f.sent.back().find("write_context")->asBool());
    const model::Session* s = &f.app->head();
    REQUIRE(s->context.size() == 1);
    CHECK(s->context.begin()->value->beat == doctest::Approx(4));
    // One state per project view, in the sidecar's clips entry, and back.
    const json::Value saved = f.view->saveLayout();
    const json::Value* clips = saved.find("clips");
    REQUIRE(clips);
    CHECK(clips->find("follow_song")->asBool());
    CHECK(clips->find("write_context")->asBool());
    f.view->setSceneLaying({});
    CHECK_FALSE(f.view->saveLayout().find("clips"));  // nothing chosen: nothing saved
    f.view->loadLayout(saved);
    CHECK(f.view->sceneLaying() == views::SceneLaying{true, true});
    // A project choice, not screen layout: a screenset neither stores the
    // checks nor changes them when recalled, whatever it holds.
    const json::Value screen = f.view->saveScreenLayout();
    CHECK_FALSE(screen.find("clips"));
    f.view->setSceneLaying({});
    f.view->loadScreenLayout(saved);  // one holding them, as an old file might
    CHECK(f.view->sceneLaying() == views::SceneLaying{});
    f.view->setSceneLaying({true, true});
    f.view->loadScreenLayout(screen);  // one without them (an older screenset)
    CHECK(f.view->sceneLaying() == views::SceneLaying{true, true});
    // A drop uses them too.
    const ui::Rect intro = header.sceneRect(0);
    const ui::Rect lane = f.view->rowRect(f.row("tr_1"));
    const double bar5 = f.app->head().timeMap->beatOfBar(5) + 0.5;
    const ui::Point to{f.view->xAt(f.app->head().timeMap->secondsAt(bar5)), lane.y + lane.h / 2};
    f.drag({intro.x + 40, intro.y + intro.h / 2}, to);
    REQUIRE(f.commands.back() == "scene.insert");
    CHECK(f.sent.back().find("bar")->asInt() == 5);
    CHECK(f.sent.back().find("follow")->asString() == "chords");
    CHECK(f.sent.back().find("write_context")->asBool());
    f.view->setSceneLaying({});
    // Lay out scenes…: every scene in pane order, checked, with its bars,
    // from the first bar after the arrangement (bar 6).
    menuRun(0, 6);
    REQUIRE(f.root->find("launcher.layout"));
    auto field = [&](const std::string& id) {
        auto* t = dynamic_cast<ui::TextField*>(f.root->find(id));
        REQUIRE(t);
        return t;
    };
    auto check = [&](const std::string& id) {
        auto* c = dynamic_cast<ui::Checkbox*>(f.root->find(id));
        REQUIRE(c);
        return c;
    };
    CHECK(check("launcher.layout.use.0")->text() == "Intro");
    CHECK(check("launcher.layout.use.1")->text() == "Verse");
    CHECK(check("launcher.layout.use.2")->text() == "Chorus");
    CHECK(check("launcher.layout.use.2")->checked());
    CHECK(field("launcher.layout.bars.0")->text() == "1");
    CHECK(field("launcher.layout.bars.1")->text() == "1");
    CHECK(field("launcher.layout.bar")->text() == "6");
    CHECK_FALSE(check("launcher.layout.follow")->checked());
    // The dialog's checks are the menu's state.
    check("launcher.layout.follow")->toggle();
    CHECK(f.view->sceneLaying() == views::SceneLaying{true, false});
    f.snapshot("launcher-layout-dialog.png");
    // Chorus has no clips: the command's refusal shows, by the scene's name
    // (not sc_3), with its hint and Chorus's row marked; the dialog stays.
    const std::size_t regions = f.app->head().regions.size();
    f.errors.clear();
    REQUIRE(f.root->click("launcher.layout.run"));
    f.sync();
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "scene.empty");
    CHECK(f.errors[0].path == "/args/sequence/2/scene");
    REQUIRE(f.root->find("launcher.layout"));
    ui::Node* status = f.root->find("launcher.layout.status");
    REQUIRE(status);
    // Names, not ids, in what reaches the user (objectNames).
    CHECK(f.errors[0].message.find("sc_") == std::string::npos);
    CHECK(f.errors[0].message.find("Chorus") != std::string::npos);
    CHECK(status->valueText().find("sc_") == std::string::npos);
    CHECK(status->valueText().find("Chorus") != std::string::npos);
    CHECK(status->valueText().ends_with("; " + f.errors[0].hint));
    auto mark = [&](const std::string& row) {
        auto* m = dynamic_cast<ui::Label*>(f.root->find("launcher.layout.mark." + row));
        REQUIRE(m);
        return m->text();
    };
    CHECK(mark("2") == "•");
    CHECK(mark("0").empty());
    CHECK(mark("from").empty());
    CHECK(check("launcher.layout.use.2")->checked());  // the entries are kept
    // Bars that are not a whole number from 1 are the dialog's to say:
    // nothing is sent. Spaces at the ends are trimmed; one between digits is
    // not a number.
    check("launcher.layout.use.2")->toggle();
    f.errors.clear();
    const std::size_t sentBefore = f.commands.size();
    for (const char* bad : {"two", "0", "1 6", "-2", "1.5", ""}) {
        field("launcher.layout.bars.1")->setText(bad);
        REQUIRE(f.root->click("launcher.layout.run"));
        f.sync();
        CHECK(f.commands.size() == sentBefore);
        CHECK(status->valueText() == "Verse: give its bars as a whole number from 1");
        CHECK(mark("1") == "•");
    }
    field("launcher.layout.bars.1")->setText("2");
    field("launcher.layout.bar")->setText("");
    REQUIRE(f.root->click("launcher.layout.run"));
    CHECK(f.commands.size() == sentBefore);
    CHECK(status->valueText() == "From bar: give a whole number from 1");
    CHECK(mark("from") == "•");
    CHECK(mark("1").empty());
    // A whole number of any length goes to the command, whose limit answers:
    // Verse named, its row marked, the hint after.
    field("launcher.layout.bar")->setText("6");
    for (const char* many : {"100001", "20000000", "99999999999999", " 100001 "}) {
        field("launcher.layout.bars.1")->setText(many);
        f.errors.clear();
        REQUIRE(f.root->click("launcher.layout.run"));
        f.sync();
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0].code == "scene.bars");
        CHECK(f.errors[0].path == "/args/sequence/1/bars");
        CHECK(status->valueText() == "Verse: " + f.errors[0].message + "; " + f.errors[0].hint);
        CHECK(mark("1") == "•");
    }
    f.snapshot("launcher-layout-refused.png");
    field("launcher.layout.bars.1")->setText("2");
    f.errors.clear();
    // Past the command's last bar: its own refusal, From bar named and marked.
    field("launcher.layout.bar")->setText("999999");
    REQUIRE(f.root->click("launcher.layout.run"));
    f.sync();
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].code == "command.invalid_args");
    REQUIRE_FALSE(f.errors[0].details.empty());
    CHECK(status->valueText().starts_with("From bar: " + f.errors[0].message + ": " +
                                          f.errors[0].details[0].message));
    CHECK(mark("from") == "•");
    REQUIRE(f.root->find("launcher.layout"));
    field("launcher.layout.bar")->setText("6");
    // Nothing checked: Lay out is disabled, and nothing is sent.
    check("launcher.layout.use.0")->toggle();
    check("launcher.layout.use.1")->toggle();
    f.sync();
    REQUIRE(f.root->find("launcher.layout.run"));
    CHECK_FALSE(f.root->find("launcher.layout.run")->enabled);
    (void)f.root->click("launcher.layout.run");
    f.sync();
    CHECK(f.commands.size() == sentBefore + 5);
    check("launcher.layout.use.0")->toggle();
    check("launcher.layout.use.1")->toggle();
    CHECK(f.root->find("launcher.layout.run")->enabled);
    // Corrected: Intro for one bar, then Verse for two, from bar 6.
    field("launcher.layout.bars.1")->setText("2");
    REQUIRE(f.root->click("launcher.layout.run"));
    f.sync();
    CHECK(f.errors.size() == 1);  // the one above
    CHECK_FALSE(f.root->find("launcher.layout"));
    REQUIRE(f.commands.back() == "arrangement.build");
    const json::Value& args = f.sent.back();
    CHECK(args.find("bar")->asInt() == 6);
    CHECK(args.find("follow")->asString() == "chords");
    CHECK_FALSE(args.find("write_context"));
    const auto& sequence = args.find("sequence")->asArray();
    REQUIRE(sequence.size() == 2);
    CHECK(sequence[0].find("scene")->asString() == "sc_1");
    CHECK(sequence[0].find("bars")->asInt() == 1);
    CHECK(sequence[1].find("scene")->asString() == "sc_2");
    CHECK(sequence[1].find("bars")->asInt() == 2);
    CHECK(f.app->head().regions.size() == regions + 2);
    CHECK(views::barAfterArrangement(f.app->head()) == 9);
    // Past eight scenes the dialog's rows scroll; it stays in the window.
    for (int i = 4; i <= 12; ++i)
        f.exec("scene.add", R"({"name":"S)" + std::to_string(i) + R"("})");
    menuRun(0, 6);
    f.sync();
    auto* list = dynamic_cast<ui::ScrollView*>(f.root->find("launcher.layout.scenes"));
    REQUIRE(list);
    CHECK(list->bounds().h <= 8 * 26 + 7 * 6 + 0.5f);
    CHECK(list->contentHeight() > list->bounds().h);
    REQUIRE(f.root->find("launcher.layout"));
    CHECK(f.root->find("launcher.layout")->bounds().bottom() <= 600);
    f.root->closePopups();
}

TEST_CASE("launcher: what write_context left unwritten reaches the status line") {
    LauncherFixture f;
    f.song();
    const model::Session& s = f.app->head();
    const json::Value present = *json::parse(
        R"({"items":[],"context":{"written":false,"reason":"present","message":"…"}})");
    CHECK(views::followNotice(s, "scene.insert", *json::parse(R"({"scene":"sc_2","bar":3})"),
                              present) ==
          "Verse at bar 3: Verse's harmony is not written: the context track has points there");
    const json::Value disagree = *json::parse(
        R"({"items":[],"context":{"written":false,"reason":"disagree","bar":4,"clips":["cl_2","cl_3"]}})");
    CHECK(views::followNotice(s, "scene.insert", *json::parse(R"({"scene":"sc_2","bar":3})"),
                              disagree) ==
          "Verse at bar 3: Verse's harmony is not written: its clips disagree at bar 4");
    // Written, held already or nothing to write: nothing to say.
    for (const char* quiet :
         {R"({"written":true,"keys":1,"chords":0})", R"({"written":false,"reason":"holds"})",
          R"({"written":false,"reason":"no_context"})"})
        CHECK_FALSE(views::followNotice(
            s, "scene.insert", *json::parse(R"({"scene":"sc_2","bar":3})"),
            *json::parse(std::string(R"({"items":[],"context":)") + quiet + "}")));
    // arrangement.build: each segment as scene.insert's, under one head.
    const json::Value built = *json::parse(
        R"({"segments":[{"scene":"sc_1","items":[],"context":{"written":true}},)"
        R"({"scene":"sc_2","items":[],"context":{"written":false,"reason":"present"}}]})");
    CHECK(views::followNotice(s, "arrangement.build", *json::parse(R"({"bar":5})"), built) ==
          "2 scenes from bar 5: Verse's harmony is not written: the context track has points "
          "there");
    const json::Value one = *json::parse(
        R"({"segments":[{"scene":"sc_2","items":[],"context":{"written":false,"reason":"present"}}]})");
    CHECK(views::followNotice(s, "arrangement.build", *json::parse(R"({"bar":5})"), one) ==
          "Verse from bar 5: Verse's harmony is not written: the context track has points there");
    CHECK_FALSE(views::followNotice(s, "arrangement.build", *json::parse(R"({"bar":5})"),
                                    *json::parse(R"({"segments":[{"scene":"sc_1","items":[]}]})")));
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

TEST_CASE("launcher: a cell shows its clip's key, with a mark when the clip follows") {
    // gui.md §4.15: under the preview, kFollowMark before the key for Follow.
    LauncherFixture f;
    f.song();
    // Root (Bass, Verse) in D minor, following the key; a Keys clip in G
    // dorian as written; Kits A and B have no key.
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key D minor ~:w","follow":"key"})");
    f.exec("clip.add", R"({"track":"tr_3","scene":"sc_2","notation":"voice k; 1: D4:w",)"
                       R"("context":"1: key G dorian ~:w","name":"Modal"})");
    const views::LauncherPane& pane = f.view->launcherPane();
    std::map<std::string, std::string> keys;
    for (std::size_t i = 0; i < pane.stats.names.size(); ++i)
        keys[pane.stats.names[i]] = pane.stats.keys.at(i);
    CHECK(keys ==
          std::map<std::string, std::string>{{"Kit A", ""},
                                             {"Kit B", ""},
                                             {"Root", std::string(views::kFollowMark) + "Dm"},
                                             {"Modal", "G dor"}});
    // Following the chords marks it too; none takes the mark away.
    f.exec("clip.set", R"({"clip":"cl_3","follow":"chords"})");
    CHECK(views::clipKeyMark(*f.app->head().clips.find(*model::Id::parse("cl_3"))) ==
          std::string(views::kFollowMark) + "Dm");
    f.exec("clip.set", R"({"clip":"cl_3","follow":"none"})");
    CHECK(views::clipKeyMark(*f.app->head().clips.find(*model::Id::parse("cl_3"))) == "Dm");
    // Short names: minor "m", major bare, the minors' variants and the modes.
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key Eb harmonic_minor ~:w"})");
    CHECK(views::clipKeyMark(*f.app->head().clips.find(*model::Id::parse("cl_3"))) == "Eb hm");
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key F# major ~:w"})");
    CHECK(views::clipKeyMark(*f.app->head().clips.find(*model::Id::parse("cl_3"))) == "F#");
    f.snapshot("launcher-keys.png");
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
    CHECK(f.notices[0] == "Root follows: sounds in C minor, −2");
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
    CHECK(f.notices[1] == "Pad is laid as written: its key D minor differs from F minor at bar 9");
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
          "Verse at bar 21: Root sounds in F minor, +3; Pad is laid as written: its key D "
          "minor differs from F minor at bar 21");
    CHECK(f.errors.empty());
}

TEST_CASE("launcher: a clip laid as written over another chord says the chords, not empty keys") {
    // Review round 88 K3: followed.mismatch {kind: chord} (the same key, another chord) has no
    // keys to name; the line names the chords, as the engine's answer gives them.
    LauncherFixture f;
    f.song();
    f.exec("key.set", R"({"at": 0, "tonic": "C", "scale": "major"})");
    f.exec("chord.set", R"({"at": 0, "symbol": "C"})");
    f.exec("clip.set", R"({"clip":"cl_3","context":"1: key C major {Am}:w"})");
    const std::string add = R"({"clip":"cl_3","track":"tr_2","bar":1,"follow":"none"})";
    const json::Value args = *json::parse(add);
    const json::Value answer = f.exec("item.add", add);
    const json::Value* followed = answer.find("followed");
    const json::Value* m = followed ? followed->find("mismatch") : nullptr;
    REQUIRE(m);
    REQUIRE(m->find("kind"));
    CHECK(m->find("kind")->asString() == "chord");
    CHECK(views::followNotice(f.app->head(), "item.add", args, answer) ==
          "Root is laid as written: its chord Am differs from C at bar 1");
    // A key mismatch still names the keys, with or without its kind; a commit's list the same.
    auto laid = [&](const std::string& mismatch) {
        return views::followNotice(
            f.app->head(), "item.add", args,
            *json::parse(R"({"item":"it_1","followed":{"mode":"none","mismatch":)" + mismatch +
                         "}}"));
    };
    CHECK(laid(R"({"kind":"key","clip_key":"D minor","key":"C major","bar":3})") ==
          "Root is laid as written: its key D minor differs from C major at bar 3");
    CHECK(laid(R"({"clip_key":"D minor","key":"C major","bar":3})") ==
          "Root is laid as written: its key D minor differs from C major at bar 3");
    CHECK(views::followNotice(
              f.app->head(), "launch.commit", json::Value::object(),
              *json::parse(R"({"items":["it_2"],"mismatches":[{"clip":"cl_3","kind":"chord",)"
                           R"("clip_chord":"Am","chord":"F","bar":5}]})")) ==
          "Committed: Root is laid as written: its chord Am differs from F at bar 5");
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
        "Root follows: sounds in C minor, then F minor from bar 7 (2 folded into range)");
    // Chords; a run already in the key (copy) is not a change.
    CHECK(views::followNotice(s, "item.add", root,
                              item(R"({"mode":"chords","regions":[{"from_bar":1,"to_bar":1,)"
                                   R"("from_key":"C minor","to_key":"C minor","by":"copy"},)"
                                   R"({"from_bar":2,"to_bar":2,"from_key":"C minor",)"
                                   R"("to_key":"C minor","by":"chords"}]})")) ==
          "Root follows the chords: sounds in C minor");
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
          "Root is laid as written: drums don't follow");
    CHECK(views::followNotice(s, "item.add", root, item(R"({"mode":"none","copied":"audio"})")) ==
          "Root is laid as written: audio doesn't follow");
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
          "Verse at bar 3: 2 follow (Kit A, Kit B); 1 laid as written (Root)");
    // Other commands, and refusals' empty answers, say nothing.
    CHECK_FALSE(views::followNotice(s, "clip.duplicate", root, item(moved)));
}

TEST_CASE("launcher: a fader drag asks for the refusals once a frame while shown, never hidden") {
    // Round 147 F1: a fader drag sends a coalesced track.set every frame, a
    // new head version each time, so the refusals' generation moves every
    // frame. Shown, the pane asks once per frame that moved it, no more,
    // and each ask is one EngineApp::launchRefusals call (every clip and
    // scene in one pass); hidden, it asks nothing during the drag and once
    // when shown again.
    LauncherFixture f;
    f.song();
    std::size_t batches = 0;  // EngineApp::launchRefusals calls
    f.refusals = gui::RefusalWatch([&](const engine::EngineApp& app) {
        ++batches;
        return app.launchRefusals();
    });
    views::TcpCallbacks cb;
    cb.commit = [&](std::string_view command, json::Value args, std::string gesture) {
        cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
        origin.gesture = std::move(gesture);
        REQUIRE(f.dispatcher->execute({std::string(command), std::move(args), std::move(origin)}));
    };
    f.view->setTcpCallbacks(std::move(cb));
    f.exec("engine.start", R"({"backend":"dummy"})");
    const model::Id drums = *model::Id::parse("tr_1");
    // One drag of `moves` frames on Drums' fader; the frames in which the
    // head moved.
    auto drag = [&](int moves) {
        ui::Node* fader = f.view->tcpPanel().controlNode(drums, "fader");
        REQUIRE(fader);
        const ui::Rect fb = fader->bounds();
        const ui::Point at{fb.x + fb.w / 2, fb.y + fb.h / 2};
        std::size_t moved = 0;
        auto frame = [&](Pointer type, ui::Point p) {
            const std::uint64_t version = f.app->head().version;
            f.pointer(type, p);
            f.sync();
            moved += f.app->head().version != version;
        };
        frame(Pointer::Down, at);
        for (int i = 1; i <= moves; ++i)
            frame(Pointer::Move, {at.x - static_cast<float>(i) * 2, at.y});
        frame(Pointer::Up, {at.x - static_cast<float>(moves) * 2, at.y});
        return moved;
    };
    REQUIRE(f.view->launcherShown());
    f.sync();
    std::size_t before = f.refusals.asked();
    std::size_t batchesBefore = batches;
    REQUIRE(before >= 1);  // the watch asked on its first pass
    CHECK(batches == before);
    const double startDb = f.app->head().tracks.find(drums)->volumeDb;
    const std::size_t moved = drag(12);
    CHECK(f.app->head().tracks.find(drums)->volumeDb < startDb);  // the drag edited
    CHECK(moved >= 12);
    CHECK(f.refusals.asked() - before == moved);  // one ask per frame that moved the head
    CHECK(batches - batchesBefore == moved);      // each ask one batch call
    // Hidden: the same drag asks nothing; shown again, one ask, current.
    f.view->setLauncherShown(false);
    f.sync();
    REQUIRE_FALSE(f.view->launcherShown());
    before = f.refusals.asked();
    batchesBefore = batches;
    CHECK(drag(12) >= 12);
    CHECK(f.refusals.asked() == before);
    CHECK(batches == batchesBefore);
    f.view->setLauncherShown(true);
    f.sync();
    CHECK(f.refusals.asked() == before + 1);
    CHECK(batches == batchesBefore + 1);
    agreeWithEngine(f, "shown again after a hidden drag");
    CHECK(f.errors.empty());
    f.exec("engine.stop", "{}");
}
