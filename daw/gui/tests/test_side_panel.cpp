// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel (gui.md §4.14): its tabs, its saved state, the history as a
// tab and the inspector's account of the selection.
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <SDL3/SDL_keycode.h>

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
#include "supple/ui/controls.h"
#include "supple/ui/forms.h"
#include "supple/ui/gl.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
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
        inspector = std::make_unique<gui::Inspector>(*root, *app, ui, *dispatcher, *actions);
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
    ui::TextField& field(const std::string& id) {
        auto* f = dynamic_cast<ui::TextField*>(node(id));
        REQUIRE_MESSAGE(f, id);
        return *f;
    }
    // Replaces a field's text as a person would: click it, type, and leave
    // it with `key` (Enter applies, Escape gives up); 0 leaves it focused.
    void type(const std::string& id, const std::string& text, int key = SDLK_RETURN) {
        REQUIRE(root->click(id));
        field(id).setText("");
        root->text(text);
        if (key) root->key(key, 0);
        pass();
    }
    const model::Item& item(const std::string& id) {
        return *app->head().items.find(*model::Id::parse(id));
    }
    std::size_t steps() { return exec("history.list", "{}")["versions"].size(); }
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

TEST_CASE("inspector: positions read and write as bar.beat.thousandths, in the bar's meter") {
    time::TimeMap four;  // 4/4 throughout
    CHECK(gui::inspectorBarsText(0, four) == "1.1.000");
    CHECK(gui::inspectorBarsText(9.5, four) == "3.2.500");
    CHECK(gui::inspectorBarsText(3.9996, four) == "1.4.999");  // never a fourth digit
    CHECK(*gui::inspectorParseBars("3", four) == 8);
    CHECK(*gui::inspectorParseBars(" 3.2 ", four) == 9);
    CHECK(*gui::inspectorParseBars("3.2.500", four) == 9.5);
    for (const double beat : {0.0, 1.0, 7.25, 123.456})
        CHECK(*gui::inspectorParseBars(gui::inspectorBarsText(beat, four), four) ==
              doctest::Approx(beat).epsilon(1e-9));
    for (const char* bad : {"", "x", "0", "3.0", "3.5", "3.2.1000", "3..2", "3.2.5.1", "-1", "3."})
        CHECK_MESSAGE(!gui::inspectorParseBars(bad, four), bad);
    CHECK(gui::inspectorParseBars("3.5", four).error().message == "bar 3 has 4 beats");

    CHECK(*gui::inspectorParseNumber("-3", "dB") == -3);
    CHECK(*gui::inspectorParseNumber("-3 dB", "dB") == -3);
    CHECK(*gui::inspectorParseNumber("+1.5db", "dB") == 1.5);
    CHECK(*gui::inspectorParseNumber("0.4s", "s") == 0.4);
    CHECK(*gui::inspectorParseNumber("12 beats", "beats") == 12);
    for (const char* bad : {"", "dB", "three", "1,5", "1.5 Hz", "nan", "inf"})
        CHECK_MESSAGE(!gui::inspectorParseNumber(bad, "dB"), bad);
}

TEST_CASE("inspector: one item's fields each run item.set, one undo step each") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> errors;
    f.actions->onError = [&](const std::string& e) { errors.push_back(e); };
    const std::string track = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    const std::string it =
        f.exec("item.add",
               R"({"track":")" + track +
                   R"(","bar":2,"notation":"voice bass; 1: C2:q r:q C2:q r:q"})")["item"]
            .asString();
    // In 6/8 from bar 3 a beat is an eighth: half a quarter note.
    f.exec("timesig.set", R"({"bar":3,"meter":"6/8"})");
    f.side->show("inspector");
    f.pass();
    CHECK_FALSE(f.inView("inspector.item"));  // nothing selected: no fields
    f.ui.selectedItems = {it};
    f.pass();
    REQUIRE(f.inView("inspector.item"));
    CHECK(f.field("inspector.start").text() == "2.1.000");
    CHECK(f.field("inspector.length").text() == "4 beats");
    CHECK(f.field("inspector.gain_db").text() == "0.0 dB");
    CHECK(f.field("inspector.fade_in").text() == "0.000 s");
    CHECK(f.text("inspector.timebase") == "Its track's setting");
    CHECK_FALSE(f.inView("inspector.locked.note"));
    CHECK_FALSE(f.inView("inspector.file"));  // a MIDI item plays no file

    auto before = f.steps();
    f.type("inspector.name", "Bass riff");
    CHECK(f.app->head().takes.find(*f.item(it).activeTake)->name == "Bass riff");
    CHECK(f.text("inspector.title") == "Bass riff");
    CHECK(f.steps() == before + 1);

    f.type("inspector.start", "3.4");  // bar 3 is 6/8: its fourth eighth
    CHECK(model::itemSpan(f.app->head(), f.item(it)).startBeat == doctest::Approx(8 + 1.5));
    CHECK(f.field("inspector.start").text() == "3.4.000");
    f.type("inspector.length", "6");
    CHECK(model::itemLengthBeats(f.app->head(), f.item(it)) == doctest::Approx(6));
    CHECK(f.field("inspector.length").text() == "6 beats");
    f.type("inspector.gain_db", "-4.5 dB");
    CHECK(f.item(it).gainDb == -4.5);
    f.type("inspector.fade_in", "0.25");
    f.type("inspector.fade_out", "1.5 s");
    CHECK(f.item(it).fadeIn.length == 0.25);
    CHECK(f.item(it).fadeOut.length == 1.5);
    CHECK(f.field("inspector.fade_out").text() == "1.500 s");
    dynamic_cast<ui::Dropdown*>(f.node("inspector.timebase"))->choose("time");
    f.pass();
    CHECK(f.item(it).timebase == model::ItemTimebase::Time);
    REQUIRE(f.root->click("inspector.mute"));
    REQUIRE(f.root->click("inspector.loop"));
    f.pass();
    CHECK(f.item(it).mute);
    CHECK(f.item(it).loop);
    CHECK(f.text("inspector.mute") == "on");
    CHECK(f.steps() == before + 9);  // nine edits, nine steps
    CHECK(errors.empty());

    // Leaving a field applies it too; Escape gives the edit up; the same
    // text again is no edit.
    before = f.steps();
    f.type("inspector.gain_db", "-6", 0);
    REQUIRE(f.root->click("inspector.start"));
    f.pass();
    CHECK(f.item(it).gainDb == -6);
    f.type("inspector.gain_db", "-60", SDLK_ESCAPE);
    CHECK(f.item(it).gainDb == -6);
    CHECK(f.field("inspector.gain_db").text() == "-6.0 dB");
    f.type("inspector.gain_db", "-6.0 dB");
    CHECK(f.steps() == before + 1);

    // What cannot be meant, and what the command refuses, is said and the
    // field shows the session again.
    f.type("inspector.start", "soon");
    REQUIRE(errors.size() == 1);
    CHECK(errors.back().find("3.2.500") != std::string::npos);
    CHECK(f.field("inspector.start").text() == "3.4.000");
    f.type("inspector.gain_db", "99");
    REQUIRE(errors.size() == 2);
    CHECK(f.item(it).gainDb == -6);
    CHECK(f.field("inspector.gain_db").text() == "-6.0 dB");
    f.type("inspector.length", "0");
    REQUIRE(errors.size() == 3);
    CHECK(errors.back().find("greater than 0") != std::string::npos);
    CHECK(f.steps() == before + 1);

    // An agent's edit shows at the next pass, but not over what is being typed.
    f.type("inspector.fade_in", "0.5", 0);
    f.exec("item.set", R"({"item":")" + it + R"(","gain_db":-12,"fade_in":2})");
    f.pass();
    CHECK(f.field("inspector.gain_db").text() == "-12.0 dB");
    CHECK(f.field("inspector.fade_in").text() == "0.5");
    f.root->key(SDLK_RETURN, 0);
    f.pass();
    CHECK(f.item(it).fadeIn.length == 0.5);

    // Locked: every field but Lock is off; unlocking brings them back.
    REQUIRE(f.root->click("inspector.locked"));
    f.pass();
    CHECK(f.item(it).locked);
    CHECK(f.inView("inspector.locked.note"));
    CHECK_FALSE(f.field("inspector.name").enabled);
    CHECK_FALSE(f.node("inspector.mute")->enabled);
    CHECK_FALSE(f.root->click("inspector.mute"));
    REQUIRE(f.root->click("inspector.locked"));
    f.pass();
    CHECK_FALSE(f.item(it).locked);
    CHECK(f.field("inspector.name").enabled);

    // Another item: its own values. What was half typed is applied to the
    // item it was typed for (leaving a field applies it), never to the next.
    const std::string empty =
        f.exec("item.add", R"({"track":")" + track + R"(","position":40,"length":2})")["item"]
            .asString();
    f.type("inspector.gain_db", "-30", 0);
    f.ui.selectedItems = {empty};
    f.pass();
    CHECK(f.field("inspector.gain_db").text() == "0.0 dB");
    CHECK(f.item(it).gainDb == -30);
    CHECK(f.item(empty).gainDb == 0);
    // An empty item has an empty take, which can carry a name.
    f.type("inspector.name", "Gap");
    CHECK(f.app->head().takes.find(*f.item(empty).activeTake)->name == "Gap");
    CHECK(f.text("inspector.title") == "Gap");
    CHECK(f.text("inspector.detail") == "Empty item on Bass");
    // A tab out of view keeps no focus: what was half typed is applied as
    // the field is left, and keys no longer reach it.
    f.type("inspector.fade_in", "0.75", 0);
    CHECK(f.root->focused() == f.node("inspector.fade_in"));
    f.side->show("history");
    f.pass();
    CHECK(f.root->focused() == nullptr);
    CHECK(f.item(empty).fadeIn.length == 0.75);
    f.side->show("inspector");
    f.pass();
    // Removed under the inspector: the fields go.
    f.exec("item.remove", R"({"item":")" + empty + R"("})");
    f.pass();
    CHECK_FALSE(f.inView("inspector.item"));
}
