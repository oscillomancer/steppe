// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Timecode and sync in the GUI (gui.md §4.13): the project timecode dialog,
// the tempo button's sync menu and state, and the sync settings dialog.
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <filesystem>
#include <memory>
#include <string>

#include "app_actions.h"
#include "supple/engine/app.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/ui/forms.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "sync_dialog.h"
#include "timecode_dialog.h"
#include "transport_bar.h"

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Test, "sync-test", {}};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<ui::Root> root;
    gui::UiState ui;
    std::unique_ptr<gui::AppActions> actions;
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-sync-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        testConfigHome(dir / "config");
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"sync-test", 1200, 800, true});
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
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->emplace<ui::Node>("arrange").layout.height = ui::SizeRule::fraction();
        root->setContent(std::move(frame));
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
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
    ui::Node* node(const std::string& id) {
        ui::Node* n = root->find(id);
        REQUIRE_MESSAGE(n, id);
        return n;
    }
    const sync::TimecodeMapping& timecode() { return app->head().project->timecode; }
};

}  // namespace

TEST_CASE("project timecode: the dialog edits the project's rate and start") {
    Fixture f;
    if (!f.ok) return;
    gui::TimecodeDialog dialog(*f.root, *f.dispatcher, *f.app);
    std::vector<std::string> errors;
    dialog.onError = [&](const std::string& e) { errors.push_back(e); };
    dialog.open();
    f.root->frame(false);
    REQUIRE(dialog.isOpen());
    // The defaults: 30 fps from 00:00:00:00.
    CHECK(f.node("timecode.rate.set")->valueText() == "30 fps");
    CHECK(f.node("timecode.start.set")->valueText() == "00:00:00:00");
    // The rate menu: one edit.
    auto* rate = dynamic_cast<ui::Dropdown*>(f.node("timecode.rate.set"));
    REQUIRE(rate);
    rate->choose("25");
    f.root->frame(false);
    CHECK(f.timecode().rate == sync::FrameRate::Fps25);
    CHECK(f.node("timecode.rate.set")->valueText() == "25 fps");
    // The start: Enter commits it.
    auto* start = dynamic_cast<ui::TextField*>(f.node("timecode.start.set"));
    REQUIRE(start);
    REQUIRE(start->onSubmit);
    start->onSubmit("01:00:00:00");
    f.root->frame(false);
    CHECK(f.timecode().start == sync::Timecode{1, 0, 0, 0});
    CHECK(errors.empty());
    // A label the rate does not have is refused, shown, and changes nothing.
    start->onSubmit("01:00:00:25");
    f.root->frame(false);
    CHECK(f.timecode().start == sync::Timecode{1, 0, 0, 0});
    REQUIRE(errors.size() == 1);
    CHECK(f.node("timecode.status")->valueText().find("out of range") != std::string::npos);
    // Drop-frame refuses a start it skips; an agent's edit shows at once.
    CHECK(dialog.setRate("30"));  // 01:00:00:00 is a label at 30 too
    f.exec("project.set", R"({"timecode": {"rate": "29.97df", "start": "00:10:00;00"}})");
    dialog.refresh();
    f.root->frame(false);
    CHECK(f.node("timecode.rate.set")->valueText() == "29.97 fps drop-frame");
    CHECK(f.node("timecode.start.set")->valueText() == "00:10:00;00");
    // project.get reports it; undo restores the previous setting.
    auto info = f.exec("project.get", "{}");
    CHECK(info["timecode"]["rate"].asString() == "29.97df");
    CHECK(info["timecode"]["start"].asString() == "00:10:00;00");
    f.exec("history.undo", "{}");
    dialog.refresh();
    f.root->frame(false);
    CHECK(f.node("timecode.rate.set")->valueText() == "30 fps");
}

TEST_CASE("sync: the tempo button's menu chooses the source and shows the state") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> opened;
    f.actions->addUi("ui.sync.settings", "Transport: sync settings…",
                     [&] { opened.push_back("settings"); });
    gui::TransportBar bar(*f.app, *f.actions, *f.dispatcher, *f.root);
    f.root->setContent(bar.build());
    bar.sync();
    f.root->frame(false);
    CHECK(f.node("transport.tempo")->valueText() == "120");
    // A right click opens the menu: the sources, Off checked, and the dialog.
    REQUIRE(f.root->click("transport.tempo", 3));
    f.root->frame(false);
    auto* menu = f.root->find("transport.sync.menu");
    REQUIRE(menu);
    const auto items = ui::menuItems(*menu);
    REQUIRE(items.size() == 6);
    CHECK(items[0].label == "External sync: Off");
    CHECK(items[0].checked);
    CHECK(items[2].label == "External sync: MTC");
    CHECK(items[5].label == "Sync settings...");
    items[2].run();
    f.root->closePopups();
    CHECK(f.app->syncSettings().source == "mtc");
    // On: "EXT" and the tempo heard, tinted red while nothing arrives (no engine).
    bar.sync();
    f.root->frame(false);
    CHECK(f.node("transport.tempo")->valueText() == "EXT 120.0");
    REQUIRE(f.root->click("transport.tempo", 3));
    f.root->frame(false);
    ui::menuItems(*f.root->find("transport.sync.menu"))[5].run();
    CHECK(opened.size() == 1);
    // A left click still edits the tempo.
    f.root->closePopups();
    REQUIRE(f.root->click("transport.tempo"));
    f.root->frame(false);
    CHECK(f.root->find("tempo.field"));
}

TEST_CASE("sync: the settings dialog sets, saves and follows an agent's changes") {
    Fixture f;
    if (!f.ok) return;
    std::vector<engine::SyncSettings> saved;
    gui::SyncDialog dialog(*f.root, *f.dispatcher, *f.app, [&](const engine::SyncSettings& s) {
        saved.push_back(s);
        return Status{};
    });
    bool timecode = false;
    dialog.onTimecode = [&] { timecode = true; };
    dialog.open();
    f.root->frame(false);
    REQUIRE(dialog.isOpen());
    CHECK(f.node("sync.source.set")->valueText() == "Off (Steppe is the master)");
    CHECK(f.root->find("sync.midi.set") == nullptr);  // rows follow the source
    CHECK(f.node("sync.timecode.value")->valueText() == "30 fps from 00:00:00:00");
    // MIDI clock: its rows appear; each change is saved.
    dynamic_cast<ui::Dropdown*>(f.node("sync.source.set"))->choose("midi_clock");
    f.root->frame(false);
    REQUIRE(f.root->find("sync.midi.set"));
    REQUIRE(f.root->find("sync.follow.set"));
    CHECK(f.app->syncSettings().source == "midi_clock");
    REQUIRE(saved.size() == 1);
    CHECK(saved.back().source == "midi_clock");
    dynamic_cast<ui::Checkbox*>(f.node("sync.follow.set"))->toggle();
    CHECK_FALSE(f.app->syncSettings().followTempo);
    auto* offset = dynamic_cast<ui::TextField*>(f.node("sync.offset.set"));
    REQUIRE(offset);
    offset->onSubmit("-12.5");
    CHECK(f.app->syncSettings().offsetMs == -12.5);
    dynamic_cast<ui::Dropdown*>(f.node("sync.loss.set"))->choose("50");
    CHECK(f.app->syncSettings().onLoss == "freewheel");
    CHECK(f.app->syncSettings().freewheelFrames == 50);
    dynamic_cast<ui::Checkbox*>(f.node("sync.ltc_out.set"))->toggle();
    CHECK(f.app->syncSettings().ltcOutput);
    CHECK(saved.size() == 5);
    CHECK(saved.back() == f.app->syncSettings());
    // The status says what it waits for.
    CHECK(f.node("sync.status.0")->valueText() == "Start the audio engine to follow a master.");
    // The timecode row opens the project's timecode.
    f.root->frame(false);
    REQUIRE(f.root->click("sync.timecode.edit"));
    CHECK(timecode);
    // An agent's sync.set: saved, and shown when the dialog refreshes.
    dialog.open();
    f.exec("sync.set", R"({"source": "ltc", "ltc_input": 2})");
    dialog.refresh();
    f.root->frame(false);
    CHECK(saved.back().source == "ltc");
    REQUIRE(f.root->find("sync.ltc_input.set"));
    CHECK(f.node("sync.ltc_input.set")->valueText().find("Input 2") == 0);
    // A refused value shows its message and changes nothing.
    json::Value bad = json::Value::object();
    bad["offset_ms"] = 99999;
    CHECK_FALSE(dialog.set(bad));
    CHECK(f.app->syncSettings().offsetMs == -12.5);
}
