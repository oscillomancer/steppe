// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher pane (gui.md §4.15; ADR-0097 step L3), driven through the
// same pointer paths as the app.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include "test_support.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app_actions.h"
#include "shell.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/peaks/peaks.h"
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
    std::vector<model::Id> opened;

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
        view->setLauncherCallbacks({[this](const std::string& command, json::Value args) {
                                        commands.push_back(command);
                                        return dispatcher->execute(
                                            {command, std::move(args), kLauncherUi});
                                    },
                                    [this](const Error& error) { errors.push_back(error); },
                                    [this](model::Id clip) { opened.push_back(clip); },
                                    {}});
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
    f.exec("launch.clip", R"({"clip":"cl_1"})");
    // Until the engine hears it; the bound only keeps a broken build from
    // hanging, the check is on what arrives.
    bool playing = false;
    for (int i = 0; i < 300 && !playing; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        f.view->setLaunchState(gui::launchViews(f.app->launchState()));
        f.sync();
        playing = f.view->launcherPane().stats.playing == 1;
    }
    CHECK(playing);
    const auto& state = f.view->launchState();
    const auto it = state.find(*model::Id::parse("tr_1"));
    REQUIRE(it != state.end());
    CHECK(it->second.clip == model::Id::parse("cl_1"));
    f.exec("engine.stop", "{}");
}
