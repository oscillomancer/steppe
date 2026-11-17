// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel (gui.md §4.14): its tabs, its saved state, the history as a
// tab and the inspector's account of the selection.
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app_actions.h"
#include "history_panel.h"
#include "inspector.h"
#include "side_panel.h"
#include "supple/engine/app.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/gl.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Test, "side-test", {}};

// The app's right edge: an arrange stand-in with the side panel beside it,
// holding the inspector and the history as the app does.
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
    std::unique_ptr<gui::HistoryPanel> history;
    std::unique_ptr<gui::Inspector> inspector;
    std::unique_ptr<gui::SidePanel> side;
    std::vector<std::string> shown;  // "history+", "history-", … in order
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-side-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        testConfigHome(dir / "config");
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"side-test", 1200, 800, true});
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
        history = std::make_unique<gui::HistoryPanel>(*root, *actions, *dispatcher, *app);
        inspector = std::make_unique<gui::Inspector>(*root, *app, ui);
        side = std::make_unique<gui::SidePanel>(*root);
        history->host = [this] { side->toggle("history"); };
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Row;
        frame->emplace<ui::Node>("arrange").layout.width = ui::SizeRule::fraction();
        side->addTab("inspector", "Inspector", inspector->build(), [this](bool on) {
            shown.push_back(on ? "inspector+" : "inspector-");
            inspector->setVisible(on);
        });
        side->addTab("history", "History", history->build(), [this](bool on) {
            shown.push_back(on ? "history+" : "history-");
            history->setVisible(on);
        });
        frame->add(side->build());
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
    bool inView(const std::string& id) {
        const ui::Node* n = root->find(id);
        return n && n->visibleInTree();
    }
    std::string text(const std::string& id) { return node(id)->valueText(); }
    void pass() {
        inspector->sync();
        history->sync();
        root->frame(false);
    }
};

}  // namespace

TEST_CASE("side panel: closed until asked, one tab in view, the arrange view gives it room") {
    Fixture f;
    if (!f.ok) return;
    CHECK_FALSE(f.side->visible());
    CHECK_FALSE(f.inView("side"));
    const float full = f.node("arrange")->bounds().w;

    f.side->show("inspector");
    f.pass();
    CHECK(f.side->showing("inspector"));
    CHECK(f.inView("inspector"));
    CHECK_FALSE(f.inView("history"));
    CHECK(f.node("side")->bounds().w == gui::SidePanel::kDefaultWidth);
    CHECK(f.node("arrange")->bounds().w == full - gui::SidePanel::kDefaultWidth);
    CHECK(f.text("side.tab.inspector") == "Inspector (shown)");
    CHECK(f.text("side.tab.history") == "History");

    // A click on the other tab swaps what is in view; each tab is told.
    REQUIRE(f.root->click("side.tab.history"));
    f.pass();
    CHECK(f.side->showing("history"));
    CHECK(f.inView("history"));
    CHECK_FALSE(f.inView("inspector"));
    CHECK(f.shown == std::vector<std::string>{"inspector+", "inspector-", "history+"});

    // Toggling the tab in view closes the panel; toggling another opens on it.
    f.side->toggle("history");
    f.pass();
    CHECK_FALSE(f.side->visible());
    CHECK(f.node("arrange")->bounds().w == full);
    CHECK(f.side->tab() == "history");  // remembered while closed
    f.side->toggle("inspector");
    f.pass();
    CHECK(f.side->showing("inspector"));
    f.side->toggle("history");
    f.pass();
    CHECK(f.side->showing("history"));  // open on another tab: it swaps, it does not close
    REQUIRE(f.root->click("side.close"));
    f.pass();
    CHECK_FALSE(f.side->visible());
    // A tab the panel does not have changes nothing but opens it.
    f.side->show("browser");
    CHECK(f.side->showing("history"));
}

TEST_CASE("side panel: its width drags, clamps and resets; its state saves and reloads") {
    Fixture f;
    if (!f.ok) return;
    f.side->show("inspector");
    f.pass();
    const ui::Rect grip = f.node("side.grip")->bounds();
    const ui::Point at{grip.x + grip.w / 2, grip.y + grip.h / 2};
    f.root->pointer({ui::PointerEvent::Type::Down, at, 1});
    f.root->pointer({ui::PointerEvent::Type::Move, {at.x - 80, at.y}, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, {at.x - 80, at.y}, 1});
    f.pass();
    CHECK(f.side->width() == gui::SidePanel::kDefaultWidth + 80);
    CHECK(f.node("side")->bounds().w == gui::SidePanel::kDefaultWidth + 80);
    f.side->setWidth(10);
    CHECK(f.side->width() == gui::SidePanel::kMinWidth);
    f.side->setWidth(5000);
    CHECK(f.side->width() == gui::SidePanel::kMaxWidth);

    // Save, change everything, reload: the same panel comes back.
    f.side->show("history");
    f.side->setWidth(420);
    json::Value saved = f.side->save();
    CHECK(saved["tab"].asString() == "history");
    CHECK(saved["visible"].asBool());
    CHECK(saved["width"].asDouble() == 420);
    f.side->hide();
    f.side->setWidth(gui::SidePanel::kDefaultWidth);
    f.side->show("inspector");
    f.side->hide();
    f.side->load(json::parse(json::writeCanonical(saved)).value());
    f.pass();
    CHECK(f.side->showing("history"));
    CHECK(f.side->width() == 420);
    CHECK(f.inView("history.versions"));
    // What a file cannot mean is ignored field by field.
    f.side->load(json::parse(R"({"tab":"nowhere","visible":"yes","width":"wide"})").value());
    CHECK(f.side->showing("history"));
    CHECK(f.side->width() == 420);
    f.side->load(json::parse(R"({"visible":false})").value());
    CHECK_FALSE(f.side->visible());
    CHECK(f.side->tab() == "history");
}

TEST_CASE("side panel: the history is a tab; its action opens the panel on it") {
    Fixture f;
    if (!f.ok) return;
    // In the panel the history has no title row or Close of its own.
    CHECK(f.root->find("history.title") == nullptr);
    CHECK(f.root->find("history.close") == nullptr);
    f.exec("track.add", R"({"name":"Bass"})");
    REQUIRE(f.actions->run("ui.history"));
    f.pass();
    CHECK(f.side->showing("history"));
    CHECK(f.history->visible());
    REQUIRE(f.history->versions().size() >= 2);
    CHECK(f.history->versions().front().label == "track.add");
    // It follows edits while in view and reads nothing while another tab is.
    f.side->show("inspector");
    f.exec("track.add", R"({"name":"Keys"})");
    f.pass();
    CHECK_FALSE(f.history->visible());
    const auto before = f.history->versions().size();
    f.side->show("history");
    f.pass();
    CHECK(f.history->versions().size() == before + 1);
    REQUIRE(f.actions->run("ui.history"));
    f.pass();
    CHECK_FALSE(f.side->visible());
}

TEST_CASE("inspector: it says what is selected, and follows the selection and the session") {
    Fixture f;
    if (!f.ok) return;
    const std::string bass =
        f.exec("track.add", R"({"name":"Bass","color":"orange"})")["track"].asString();
    const std::string buss =
        f.exec("track.add", R"({"type":"buss","color":""})")["track"].asString();
    const std::string empty =
        f.exec("item.add", R"({"track":")" + bass + R"(","position":0,"length":4})")["item"]
            .asString();
    const std::string midi =
        f.exec("item.add",
               R"({"track":")" + bass +
                   R"(","bar":5,"notation":"voice bass; 1: C2:q r:q C2:q r:q"})")["item"]
            .asString();
    const ui::Theme theme = ui::Theme::dark();
    const model::Session& s = f.app->head();
    using Kind = gui::InspectorSubject::Kind;

    gui::UiState sel;
    gui::InspectorSubject subject = gui::inspectorSubject(s, sel, theme);
    CHECK(subject.kind == Kind::Nothing);
    CHECK(subject.title == "Nothing selected");
    CHECK_FALSE(subject.color);

    sel.selectedItems = {empty};
    subject = gui::inspectorSubject(s, sel, theme);
    CHECK(subject.kind == Kind::Item);
    CHECK(subject.title == empty);  // no take name and no file: its id
    CHECK(subject.detail == "Empty item on Bass");
    REQUIRE(subject.color);
    CHECK(ui::packRgba(*subject.color) == ui::packRgba(theme.color("track-orange")));

    sel.selectedItems = {midi};
    CHECK(gui::inspectorSubject(s, sel, theme).detail == "MIDI item on Bass");

    // Items win over the tracks selected with them, unless tracks came last.
    sel.selectedItems = {empty, midi};
    sel.selectedTracks = {bass};
    subject = gui::inspectorSubject(s, sel, theme);
    CHECK(subject.kind == Kind::Items);
    CHECK(subject.title == "2 items");
    CHECK(subject.detail == "On Bass");
    CHECK_FALSE(subject.color);
    sel.tracksFocused = true;
    subject = gui::inspectorSubject(s, sel, theme);
    CHECK(subject.kind == Kind::Track);
    CHECK(subject.title == "Bass");
    CHECK(subject.detail == "Channel");
    REQUIRE(subject.color);

    sel.selectedTracks = {buss};
    subject = gui::inspectorSubject(s, sel, theme);
    CHECK(subject.title == "buss_1");
    CHECK(subject.detail == "Buss");
    CHECK_FALSE(subject.color);  // a track with no colour has no swatch
    sel.selectedTracks = {bass, buss, "tr_999", "nonsense"};
    subject = gui::inspectorSubject(s, sel, theme);
    CHECK(subject.kind == Kind::Tracks);
    CHECK(subject.title == "2 tracks");  // ids the session does not hold are left out

    // In the panel: it follows the selection, a rename, and a removal.
    f.side->show("inspector");
    f.pass();
    CHECK(f.text("inspector.title") == "Nothing selected");
    f.ui.selectedTracks = {bass};
    f.ui.tracksFocused = true;
    f.pass();
    CHECK(f.text("inspector.title") == "Bass");
    CHECK(f.text("inspector.detail") == "Channel");
    CHECK(f.inView("inspector.swatch"));
    f.exec("track.set", R"({"track":")" + bass + R"(","name":"Upright"})");
    f.pass();
    CHECK(f.text("inspector.title") == "Upright");
    f.ui.selectedItems = {midi};
    f.ui.tracksFocused = false;
    f.pass();
    CHECK(f.text("inspector.detail") == "MIDI item on Upright");
    f.exec("item.remove", R"({"item":")" + midi + R"("})");
    f.pass();
    CHECK(f.text("inspector.title") == "Upright");  // the item is gone: its track remains
    // Hidden, it reads nothing; shown again, it is current.
    f.side->show("history");
    f.ui.selectedTracks = {buss};
    f.pass();
    CHECK(f.text("inspector.title") == "Upright");
    f.side->show("inspector");
    f.pass();
    CHECK(f.text("inspector.title") == "buss_1");
}
