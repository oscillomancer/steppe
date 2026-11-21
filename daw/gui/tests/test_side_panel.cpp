// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel (gui.md §4.14): its tabs, its saved state, the history as a
// tab and the inspector's account of the selection.
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "app_actions.h"
#include "history_panel.h"
#include "inspector.h"
#include "shell.h"
#include "side_panel.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/controls.h"
#include "supple/ui/forms.h"
#include "supple/ui/gl.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_area.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/envelope_lane.h"

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
    f.root->pointer({ui::PointerEvent::Type::Move, at});
    CHECK(f.root->cursor() == ui::Cursor::ResizeHorizontal);  // the panel's edge is dragged
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

TEST_CASE("inspector: one item's Timebase, and Written in while its take has a context") {
    // gui.md §4.14 (ADR-0098 amended, X4e): the take's own key, read-only.
    Fixture f;
    if (!f.ok) return;
    const std::string keys = f.exec("track.add", R"({"name":"Keys"})")["track"].asString();
    const std::string item =
        f.exec("item.add",
               R"({"track":")" + keys + R"(","bar":1,"notation":"voice v; 1: C4:w"})")["item"]
            .asString();
    f.side->show("inspector");
    f.ui.selectedItems = {item};
    f.ui.tracksFocused = false;
    f.pass();
    REQUIRE(f.inView("inspector.item"));
    auto* timebase = dynamic_cast<ui::Label*>(f.node("inspector.label.timebase"));
    REQUIRE(timebase);
    CHECK(timebase->text() == "Timebase");
    CHECK_FALSE(f.inView("inspector.row.written_in"));  // no context: no row
    // The take given a context in D minor through a save and an open (no
    // command sets one before X4d).
    const auto itemId = model::Id::parse(item);
    REQUIRE(itemId);
    const std::string take = f.app->head().items.find(*itemId)->activeTake->str();
    const auto path = f.dir / ("written-in." + std::string(product::kSlug));
    auto args = json::Value::object();
    args["path"] = path.string();
    REQUIRE(f.dispatcher->execute({"project.save", args, kTest}));
    std::string bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto doc = json::parse(bytes);
    REQUIRE(doc);
    (*doc)["takes"][take]["context"] =
        json::parse(R"([{"beat":0,"type":"key","tonic":"D","scale":"minor"}])").value();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << json::writeCompact(*doc);
    }
    REQUIRE(f.app->open(path));
    f.ui.selectedItems = {item};
    f.pass();
    REQUIRE(f.inView("inspector.row.written_in"));
    auto* written = dynamic_cast<ui::Label*>(f.node("inspector.written_in"));
    REQUIRE(written);
    CHECK(written->text() == "D minor");
    // Several items: no row.
    const std::string other =
        f.exec("item.add",
               R"({"track":")" + keys + R"(","bar":3,"notation":"voice v; 1: E4:w"})")["item"]
            .asString();
    f.ui.selectedItems = {item, other};
    f.pass();
    CHECK_FALSE(f.inView("inspector.row.written_in"));
}

TEST_CASE("inspector: a track's Modulation row leads to each source, 'N routes' past four") {
    // gui.md §4.14: shown only while something of the track is modulated.
    Fixture f;
    if (!f.ok) return;
    const std::string bass = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    std::vector<std::string> shown;
    f.actions->addUiFor("ui.modulation.source", "Modulation: show source", "modulation",
                        [&](const json::Value& v) -> Status {
                            shown.push_back(v.asString());
                            return {};
                        });
    f.side->show("inspector");
    f.ui.selectedTracks = {bass};
    f.ui.tracksFocused = true;
    f.pass();
    REQUIRE(f.inView("inspector.track"));
    CHECK_FALSE(f.inView("inspector.row.track.modulation"));  // nothing modulated: no row
    json::Value made = f.exec(
        "modulate", R"({"target":{"track":")" + bass + R"(","param":"volume"},"source":"lfo"})");
    const std::string route = made["modulation"].asString();
    const std::string modOut = made["mod_out"].asString();
    f.pass();
    REQUIRE(f.inView("inspector.row.track.modulation"));
    auto* line = dynamic_cast<ui::Button*>(f.node("inspector.track.modulation." + route));
    REQUIRE(line);
    CHECK(line->valueText() == "Volume");
    CHECK(line->mark() == std::string(views::kModMark) + " LFO");
    CHECK(line->markColor() == "signal-cv");
    // One click: its source.
    f.root->click(line->id());
    CHECK(shown == std::vector<std::string>{route});
    // Its route off: "(off)", the mark dimmed.
    f.exec("modulation.set", R"({"modulation":")" + route + R"(","enabled":false})");
    f.pass();
    line = dynamic_cast<ui::Button*>(f.node("inspector.track.modulation." + route));
    REQUIRE(line);
    CHECK(line->mark() == std::string(views::kModMark) + " LFO (off)");
    CHECK(line->markColor() == "ink-faint");
    f.exec("modulation.set", R"({"modulation":")" + route + R"(","enabled":true})");
    f.pass();
    // Five routes: one line, "5 routes", whose menu lists them.
    for (int i = 2; i <= 5; ++i)
        f.exec("modulation.add", R"({"source":{"module":")" + modOut + R"(","input":"in_)" +
                                     std::to_string(i) + R"("},"target":{"track":")" + bass +
                                     R"(","param":")" + (i % 2 ? "volume" : "pan") +
                                     R"("},"depth":2})");
    f.pass();
    CHECK_FALSE(f.root->find("inspector.track.modulation." + route));
    auto* all = dynamic_cast<ui::Button*>(f.node("inspector.track.modulation.all"));
    REQUIRE(all);
    CHECK(all->valueText() == "5 routes");
    f.root->click(all->id());
    f.pass();
    ui::Node* menu = f.root->find("inspector.modulation.menu");
    REQUIRE(menu);
    const auto items = ui::menuItems(*menu);
    REQUIRE(items.size() == 5);
    CHECK(items[0].label == "Volume " + std::string(views::kModMark) + " LFO");
    CHECK(items[1].label == "Pan " + std::string(views::kModMark) + " Mod 2");
    items[1].run();
    CHECK(shown.size() == 2);
    f.root->closePopups();
    // Two tracks selected: no row (it is one track's).
    const std::string keys = f.exec("track.add", R"({"name":"Keys"})")["track"].asString();
    f.ui.selectedTracks = {bass, keys};
    f.pass();
    CHECK_FALSE(f.inView("inspector.row.track.modulation"));
}

TEST_CASE("inspector: positions read and write as bar.beat.thousandths, in the bar's meter") {
    time::TimeMap four;  // 4/4 throughout
    CHECK(gui::inspectorBarsText(0, four) == "1.1.000");
    CHECK(gui::inspectorBarsText(9.5, four) == "3.2.500");
    CHECK(gui::inspectorBarsText(3.9996, four) == "1.4.999");  // never a fourth digit
    CHECK(*gui::inspectorParseBars("3", four) == 8);
    CHECK(*gui::inspectorParseBars(" 3.2 ", four) == 9);
    CHECK(*gui::inspectorParseBars("3.2.500", four) == 9.5);
    // The third part is a fraction of the beat, not a count of thousandths:
    // fewer digits mean the same as with zeros after them.
    CHECK(*gui::inspectorParseBars("3.2.5", four) == 9.5);
    CHECK(*gui::inspectorParseBars("3.2.50", four) == 9.5);
    CHECK(*gui::inspectorParseBars("3.2.05", four) == doctest::Approx(9.05));
    CHECK(*gui::inspectorParseBars("3.2.005", four) == doctest::Approx(9.005));
    CHECK(*gui::inspectorParseBars("3.2.0", four) == 9.0);
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
    // The same on every platform (supple::fromChars, not the standard
    // library's, which Apple's lacks for floating point).
    CHECK(*gui::inspectorParseNumber(".5", "s") == 0.5);
    CHECK(*gui::inspectorParseNumber("1e2", "s") == 100);
    CHECK(*gui::inspectorParseNumber("-0.25 st", "st") == -0.25);
    for (const char* bad : {"", "dB", "three", "1,5", "1.5 Hz", "nan", "inf", "-inf", "0x10", "1e",
                            "--3", "3 3", "1e999"})
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

TEST_CASE("inspector: an audio take's speed and pitch, and the choice of the take heard") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> errors;
    f.actions->onError = [&](const std::string& e) { errors.push_back(e); };
    audio::AudioData audio;
    audio.sampleRate = 48000;
    audio.channels.assign(1, std::vector<float>(48000, 0.25f));
    const auto path = f.dir / "tone.wav";
    REQUIRE(audio::writeWav(path, audio));
    const std::string track = f.exec("track.add", R"({"name":"Keys"})")["track"].asString();
    json::Value add = json::Value::object();
    add["track"] = track;
    add["audio_file"] = path.string();
    add["position"] = 0;
    auto added = f.dispatcher->execute({"item.add", add, kTest});
    REQUIRE_MESSAGE(added, (added ? "" : added.error().toString()));
    const std::string it = (*added)["item"].asString();
    const std::string first = (*added)["take"].asString();
    auto take = [&](const std::string& id) -> const model::Take& {
        return *f.app->head().takes.find(*model::Id::parse(id));
    };

    f.side->show("inspector");
    f.ui.selectedItems = {it};
    f.pass();
    REQUIRE(f.inView("inspector.take"));
    CHECK(f.field("inspector.playrate").text() == "1.000 x");
    CHECK(f.field("inspector.pitch").text() == "0.00 st");
    CHECK(f.text("inspector.preserve_pitch") == "on");
    CHECK_FALSE(f.inView("inspector.takes"));  // one take: no list
    CHECK(f.text("inspector.file").find("tone.wav") != std::string::npos);
    CHECK(f.text("inspector.file").find("48 kHz, mono, 0:01") != std::string::npos);
    CHECK(f.field("inspector.name").text().empty());  // unnamed: it goes by its file

    auto before = f.steps();
    f.type("inspector.playrate", "0.5");
    CHECK(take(first).playrate == 0.5);
    f.type("inspector.pitch", "+7 st");
    CHECK(take(first).pitch == 7);
    CHECK(f.field("inspector.pitch").text() == "7.00 st");
    REQUIRE(f.root->click("inspector.preserve_pitch"));
    f.pass();
    CHECK_FALSE(take(first).preservePitch);
    CHECK(f.steps() == before + 3);
    f.type("inspector.pitch", "200");  // beyond the command's four octaves
    REQUIRE(errors.size() == 1);
    CHECK(take(first).pitch == 7);
    CHECK(f.field("inspector.pitch").text() == "7.00 st");

    // A second take: the list shows both, the heard one marked; a click on
    // the other makes it the one heard, and the fields show its settings.
    const std::string second =
        f.exec("take.add", R"({"item":")" + it + R"(","from":")" + first + R"("})")["id"]
            .asString();
    f.exec("take.set", R"({"take":")" + second + R"(","name":"Alt","pitch":-2})");
    f.exec("take.activate", R"({"item":")" + it + R"(","take":")" + first + R"("})");
    f.pass();
    REQUIRE(f.inView("inspector.takes"));
    auto* list = dynamic_cast<ui::List*>(f.node("inspector.takes"));
    REQUIRE(list);
    CHECK(list->count() == 2);
    CHECK(list->selected() == std::optional<std::size_t>(0));
    before = f.steps();
    list->select(1, true);
    f.pass();
    CHECK(*f.item(it).activeTake == *model::Id::parse(second));
    CHECK(f.steps() == before + 1);
    CHECK(f.field("inspector.pitch").text() == "-2.00 st");
    CHECK(f.field("inspector.name").text() == "Alt");
    CHECK(f.text("inspector.title") == "Alt");
    list->select(1, true);  // the take already heard: nothing runs
    f.pass();
    CHECK(f.steps() == before + 1);
    CHECK(errors.size() == 1);

    // A MIDI item has no speed or pitch here.
    const std::string midi =
        f.exec("item.add", R"({"track":")" + track +
                               R"(","bar":9,"notation":"voice keys; 1: C4:h E4:h"})")["item"]
            .asString();
    f.ui.selectedItems = {midi};
    f.pass();
    CHECK_FALSE(f.inView("inspector.take"));
    CHECK_FALSE(f.inView("inspector.takes"));
}

TEST_CASE("inspector: several items show what they share; one edit is one undo step for all") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> errors;
    f.actions->onError = [&](const std::string& e) { errors.push_back(e); };
    const std::string track = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    auto add = [&](int bar) {
        return f
            .exec("item.add", R"({"track":")" + track + R"(","bar":)" + std::to_string(bar) +
                                  R"(,"notation":"voice bass; 1: C2:q r:q C2:q r:q"})")["item"]
            .asString();
    };
    const std::string a = add(1), b = add(3), c = add(5);
    f.exec("item.set", R"({"item":")" + b + R"(","gain_db":-3})");
    f.side->show("inspector");
    f.ui.selectedItems = {a, b, c};
    f.pass();
    CHECK(f.text("inspector.title") == "3 items");
    REQUIRE(f.inView("inspector.item"));
    // What only one item can have is not offered.
    CHECK_FALSE(f.inView("inspector.name"));
    CHECK_FALSE(f.inView("inspector.start"));
    CHECK_FALSE(f.inView("inspector.length"));
    CHECK_FALSE(f.inView("inspector.take"));
    // A value they share is shown; one they do not is left empty.
    CHECK(f.field("inspector.fade_in").text() == "0.000 s");
    CHECK(f.field("inspector.gain_db").text().empty());
    CHECK(f.text("inspector.timebase").empty() == false);

    auto before = f.steps();
    f.type("inspector.gain_db", "-6");
    for (const auto& id : {a, b, c}) CHECK(f.item(id).gainDb == -6);
    CHECK(f.field("inspector.gain_db").text() == "-6.0 dB");
    CHECK(f.steps() == before + 1);
    REQUIRE(f.root->click("inspector.mute"));
    f.pass();
    for (const auto& id : {a, b, c}) CHECK(f.item(id).mute);
    dynamic_cast<ui::Dropdown*>(f.node("inspector.timebase"))->choose("beats");
    f.pass();
    for (const auto& id : {a, b, c}) CHECK(f.item(id).timebase == model::ItemTimebase::Beats);
    CHECK(f.steps() == before + 3);
    // Undo takes the whole edit back.
    f.exec("history.undo", "{}");
    f.exec("history.undo", "{}");
    f.pass();
    for (const auto& id : {a, b, c}) CHECK_FALSE(f.item(id).mute);
    CHECK(f.text("inspector.mute") == "off");

    // A cell is lit only when all have the state; with one locked the
    // fields are off, and Lock locks, then unlocks, them all.
    f.exec("item.set", R"({"item":")" + b + R"(","locked":true})");
    f.pass();
    CHECK(f.text("inspector.locked") == "off");
    CHECK(f.inView("inspector.locked.note"));
    CHECK(f.text("inspector.locked.note").find("Some of these") != std::string::npos);
    CHECK_FALSE(f.field("inspector.gain_db").enabled);
    REQUIRE(f.root->click("inspector.locked"));
    f.pass();
    for (const auto& id : {a, b, c}) CHECK(f.item(id).locked);
    REQUIRE(f.root->click("inspector.locked"));
    f.pass();
    for (const auto& id : {a, b, c}) CHECK_FALSE(f.item(id).locked);
    CHECK(f.field("inspector.gain_db").enabled);

    // What the command refuses for one is done for none.
    before = f.steps();
    f.type("inspector.gain_db", "99");
    REQUIRE(errors.size() == 1);
    for (const auto& id : {a, b, c}) CHECK(f.item(id).gainDb == -6);
    CHECK(f.steps() == before);
    // One of them removed meanwhile: the rest are still edited.
    f.exec("item.remove", R"({"item":")" + c + R"("})");
    f.pass();
    CHECK(f.text("inspector.title") == "2 items");
    f.type("inspector.fade_out", "0.5");
    CHECK(f.item(a).fadeOut.length == 0.5);
    CHECK(f.item(b).fadeOut.length == 0.5);
}

TEST_CASE("inspector: a track's name, colour, volume and pan; several tracks as one") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> errors;
    f.actions->onError = [&](const std::string& e) { errors.push_back(e); };
    const std::string bass = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    const std::string keys = f.exec("track.add", "{}")["track"].asString();
    auto track = [&](const std::string& id) -> const model::Track& {
        return *f.app->head().tracks.find(*model::Id::parse(id));
    };
    // A click on the n-th swatch of the colour row (the last is "none").
    auto swatch = [&](std::size_t n) {
        const ui::Rect r = f.node("inspector.track.color")->bounds();
        const ui::Point at{r.x + static_cast<float>(n) * 22 + 9, r.y + r.h / 2};
        f.root->pointer({ui::PointerEvent::Type::Down, at, 1});
        f.root->pointer({ui::PointerEvent::Type::Up, at, 1});
        f.pass();
    };
    f.side->show("inspector");
    f.ui.selectedTracks = {bass};
    f.ui.tracksFocused = true;
    f.pass();
    REQUIRE(f.inView("inspector.track"));
    CHECK_FALSE(f.inView("inspector.item"));
    CHECK(f.field("inspector.track.name").text() == "Bass");
    CHECK(f.field("inspector.track.volume_db").text() == "0.0 dB");
    CHECK(f.field("inspector.track.pan").text() == "0 %");
    CHECK(f.text("inspector.track.color") == "red");  // track.add's first colour

    auto before = f.steps();
    f.type("inspector.track.name", "Upright");
    CHECK(track(bass).name == "Upright");
    CHECK(f.text("inspector.title") == "Upright");
    swatch(3);
    CHECK(track(bass).color == "green");
    CHECK(f.text("inspector.track.color") == "green");
    swatch(model::kTrackColors.size());
    CHECK(track(bass).color.empty());
    CHECK(f.text("inspector.track.color") == "none");
    CHECK_FALSE(f.inView("inspector.swatch"));  // the header's bar goes with the colour
    f.type("inspector.track.volume_db", "-6 dB");
    CHECK(track(bass).volumeDb == -6);
    f.type("inspector.track.pan", "-30");
    CHECK(track(bass).pan == -30);
    CHECK(f.field("inspector.track.pan").text() == "-30 %");
    CHECK(f.steps() == before + 5);
    f.type("inspector.track.pan", "200");  // beyond hard left and right
    REQUIRE(errors.size() == 1);
    CHECK(track(bass).pan == -30);
    CHECK(f.field("inspector.track.pan").text() == "-30 %");

    // Two tracks: no name; what differs is empty; an edit goes to both as
    // one undo step.
    f.ui.selectedTracks = {bass, keys};
    f.pass();
    CHECK(f.text("inspector.title") == "2 tracks");
    CHECK_FALSE(f.inView("inspector.track.name"));
    CHECK(f.field("inspector.track.volume_db").text().empty());
    CHECK(f.text("inspector.track.color") == "mixed");
    before = f.steps();
    f.type("inspector.track.volume_db", "-3");
    CHECK(track(bass).volumeDb == -3);
    CHECK(track(keys).volumeDb == -3);
    CHECK(f.field("inspector.track.volume_db").text() == "-3.0 dB");
    swatch(5);
    CHECK(track(bass).color == "blue");
    CHECK(track(keys).color == "blue");
    CHECK(f.text("inspector.track.color") == "blue");
    CHECK(f.steps() == before + 2);
    f.exec("history.undo", "{}");
    CHECK(track(bass).color.empty());
    CHECK(track(keys).color == "orange");

    // From tracks to an item and back: each shows its own fields.
    const std::string it =
        f.exec("item.add",
               R"({"track":")" + bass +
                   R"(","bar":1,"notation":"voice bass; 1: C2:q r:q C2:q r:q"})")["item"]
            .asString();
    f.ui.selectedItems = {it};
    f.ui.tracksFocused = false;
    f.pass();
    CHECK(f.inView("inspector.item"));
    CHECK_FALSE(f.inView("inspector.track"));
    f.ui.tracksFocused = true;
    f.pass();
    CHECK(f.inView("inspector.track"));
    CHECK_FALSE(f.inView("inspector.item"));
}

TEST_CASE("inspector: the colour swatches are chosen from the keyboard") {
    Fixture f;
    if (!f.ok) return;
    const std::string bass = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    auto track = [&](const std::string& id) -> const model::Track& {
        return *f.app->head().tracks.find(*model::Id::parse(id));
    };
    f.side->show("inspector");
    f.ui.selectedTracks = {bass};
    f.ui.tracksFocused = true;
    f.pass();
    // The focus starts on the colour the track has; Left and Right move,
    // Enter or Space picks.
    ui::Node* row = f.node("inspector.track.color");
    CHECK(row->focusable);
    CHECK(track(bass).color == "red");
    const auto before = f.steps();
    f.root->focus(row);
    f.root->key(SDLK_RIGHT, 0);
    f.root->key(SDLK_RIGHT, 0);
    f.pass();
    CHECK(track(bass).color == "red");  // moving picks nothing
    f.root->key(SDLK_RETURN, 0);
    f.pass();
    CHECK(track(bass).color == model::kTrackColors[2]);
    f.root->key(SDLK_LEFT, 0);
    f.root->key(SDLK_SPACE, 0);
    f.pass();
    CHECK(track(bass).color == model::kTrackColors[1]);
    f.root->key(SDLK_END, 0);
    f.root->key(SDLK_RIGHT, 0);  // stays on the last, "none"
    f.root->key(SDLK_RETURN, 0);
    f.pass();
    CHECK(track(bass).color.empty());
    f.root->key(SDLK_HOME, 0);
    f.root->key(SDLK_LEFT, 0);  // stays on the first
    f.root->key(SDLK_RETURN, 0);
    f.pass();
    CHECK(track(bass).color == "red");
    CHECK(f.steps() == before + 4);
    CHECK(f.root->focused() == row);
    // Focus given again starts where the colour is.
    f.exec("track.set", R"({"track":")" + bass + R"(","color":"blue"})");
    f.pass();
    f.root->focus(nullptr);
    f.root->focus(row);
    f.root->key(SDLK_RIGHT, 0);
    f.root->key(SDLK_RETURN, 0);
    f.pass();
    const auto blue = std::find(model::kTrackColors.begin(), model::kTrackColors.end(), "blue");
    REQUIRE(blue + 1 != model::kTrackColors.end());
    CHECK(track(bass).color == *(blue + 1));
}

TEST_CASE("inspector: a track's notes are typed over several lines and applied on leaving") {
    Fixture f;
    if (!f.ok) return;
    const std::string bass = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    const std::string keys = f.exec("track.add", R"({"name":"Keys"})")["track"].asString();
    auto track = [&](const std::string& id) -> const model::Track& {
        return *f.app->head().tracks.find(*model::Id::parse(id));
    };
    f.exec("track.set", R"({"track":")" + keys + R"(","notes":"Rhodes, take the DI"})");
    f.side->show("inspector");
    f.ui.selectedTracks = {bass};
    f.ui.tracksFocused = true;
    f.pass();
    auto* notes = dynamic_cast<ui::TextArea*>(f.node("inspector.track.notes"));
    REQUIRE(notes);
    REQUIRE(f.inView("inspector.track.notes"));
    CHECK(notes->text().empty());
    CHECK_FALSE(notes->lineNumbers);

    // Enter starts a new line; Ctrl+Enter applies the notes as one step.
    auto before = f.steps();
    REQUIRE(f.root->click("inspector.track.notes"));
    f.root->text("P-bass, flats");
    f.root->key(SDLK_RETURN, 0);
    f.root->text("retake bar 9");
    f.pass();
    CHECK(track(bass).notes.empty());
    CHECK(f.steps() == before);
    f.root->key(SDLK_RETURN, ui::kCtrl);
    f.pass();
    CHECK(track(bass).notes == "P-bass, flats\nretake bar 9");
    CHECK(f.steps() == before + 1);
    // Applying what is already there runs nothing.
    f.root->key(SDLK_RETURN, ui::kCtrl);
    f.pass();
    CHECK(f.steps() == before + 1);
    // Escape puts back what the track has.
    f.root->text(" and more");
    f.root->key(SDLK_ESCAPE, 0);
    f.pass();
    CHECK(notes->text() == "P-bass, flats\nretake bar 9");
    CHECK(f.steps() == before + 1);
    // Leaving the field applies it; an edit elsewhere meanwhile does not
    // take the typing away.
    f.root->text("!");
    f.exec("track.set", R"({"track":")" + bass + R"(","pan":10})");
    f.pass();
    CHECK(notes->text().find('!') != std::string::npos);
    REQUIRE(f.root->click("inspector.track.pan"));
    f.pass();
    CHECK(track(bass).notes.find('!') != std::string::npos);

    // Another track selected while typing: the notes go to the track they
    // were typed for, and the field shows the new track's.
    const std::string typed = track(bass).notes;
    REQUIRE(f.root->click("inspector.track.notes"));
    f.root->text("?");
    f.ui.selectedTracks = {keys};
    f.pass();
    CHECK(track(bass).notes.size() == typed.size() + 1);
    CHECK(track(keys).notes == "Rhodes, take the DI");
    CHECK(notes->text() == "Rhodes, take the DI");
    // Several tracks have no notes field.
    f.ui.selectedTracks = {bass, keys};
    f.pass();
    CHECK_FALSE(f.inView("inspector.track.notes"));
}

TEST_CASE("inspector: a launcher clip's fields each run clip.set; several clips are a count") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::string> errors;
    f.actions->onError = [&](const std::string& e) { errors.push_back(e); };
    const std::string track = f.exec("track.add", R"({"name":"Bass"})")["track"].asString();
    f.exec("scene.add", R"({"name":"Verse"})");
    f.exec("scene.add", R"({"name":"Chorus"})");
    const std::string cl =
        f.exec("clip.add",
               R"({"track":")" + track +
                   R"(","scene":"sc_1","name":"Root","notation":"voice b; 1: C2:h r:h"})")
            .find("clip")
            ->asString();
    const std::string cl2 =
        f.exec("clip.add",
               R"({"track":")" + track + R"(","scene":"sc_2","notation":"voice b; 1: D2:h r:h"})")
            .find("clip")
            ->asString();
    const auto clip = [&]() -> const model::Clip& {
        return *f.app->head().clips.find(*model::Id::parse(cl));
    };
    f.side->show("inspector");
    // Items selected; a launcher click hands the inspector the clip.
    f.ui.selectedClips = {cl};
    f.pass();
    CHECK_FALSE(f.inView("inspector.clip"));  // not focused: the clips wait
    f.ui.clipsFocused = true;
    f.pass();
    REQUIRE(f.inView("inspector.clip"));
    CHECK_FALSE(f.inView("inspector.item"));
    CHECK(f.text("inspector.title") == "Root");
    CHECK(f.text("inspector.detail") == "MIDI clip on Bass, Verse");
    CHECK(f.field("inspector.clip.length").text() == "4 beats");
    CHECK(f.field("inspector.clip.gain_db").text() == "0.0 dB");
    CHECK(f.text("inspector.clip.mode") == "Loop");
    CHECK(f.text("inspector.clip.quantize") == "Project's quantize");
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS")) {  // a look, not a golden
        f.root->damageAll();
        f.root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / "inspector-clip.png", f.root->readBack());
    }
    auto before = f.steps();
    f.type("inspector.clip.name", "Pedal");
    CHECK(clip().name == "Pedal");
    CHECK(f.text("inspector.title") == "Pedal");
    CHECK(f.steps() == before + 1);
    f.type("inspector.clip.length", "8");
    CHECK(clip().length == 8);
    f.type("inspector.clip.gain_db", "-3 dB");
    CHECK(clip().gainDb == -3);
    dynamic_cast<ui::Dropdown*>(f.node("inspector.clip.mode"))->choose("once");
    f.pass();
    CHECK(clip().mode == model::ClipMode::Once);
    dynamic_cast<ui::Dropdown*>(f.node("inspector.clip.quantize"))->choose("beat");
    f.pass();
    CHECK(clip().quantize == model::LaunchQuantize::Beat);
    dynamic_cast<ui::Dropdown*>(f.node("inspector.clip.quantize"))->choose("");
    f.pass();
    CHECK_FALSE(clip().quantize);  // back to the project's
    REQUIRE(f.root->click("inspector.clip.mute"));
    f.pass();
    CHECK(clip().mute);
    // A length that is not positive is refused, and the field shows the clip's.
    f.type("inspector.clip.length", "0");
    CHECK(clip().length == 8);
    CHECK(f.field("inspector.clip.length").text() == "8 beats");
    REQUIRE(errors.size() == 1);
    // Several clips: a count, no fields.
    f.ui.selectedClips = {cl, cl2};
    f.pass();
    CHECK(f.text("inspector.title") == "2 clips");
    CHECK_FALSE(f.inView("inspector.clip"));
}

TEST_CASE("inspector: a clip's Follow needs a MIDI clip with a key at its start") {
    Fixture f;
    if (!f.ok) return;
    const std::string track = f.exec("track.add", R"({"name":"Keys"})")["track"].asString();
    f.exec("scene.add", R"({"name":"Verse"})");
    const std::string cl =
        f.exec("clip.add", R"({"track":")" + track +
                               R"(","scene":"sc_1","name":"Riff","notation":"voice k; 1: C4:w"})")
            .find("clip")
            ->asString();
    const auto clip = [&]() -> const model::Clip& {
        return *f.app->head().clips.find(*model::Id::parse(cl));
    };
    f.side->show("inspector");
    f.ui.selectedClips = {cl};
    f.ui.clipsFocused = true;
    f.pass();
    REQUIRE(f.inView("inspector.clip"));
    auto* follow = dynamic_cast<ui::Dropdown*>(f.node("inspector.clip.follow"));
    REQUIRE(follow);
    // No context: None only; Key and Chords say what they need.
    CHECK(f.text("inspector.clip.follow") == "None");
    REQUIRE(follow->choices().size() == 3);
    CHECK_FALSE(follow->choices()[1].enabled);
    CHECK(follow->choices()[1].label == "Key (needs a key at its start)");
    CHECK_FALSE(follow->choices()[2].enabled);
    // A key at its start: all three, and a pick is one clip.set.
    f.exec("clip.set", R"({"clip":")" + cl + R"(","context":"1: key A minor ~:w"})");
    f.pass();
    CHECK(follow->choices()[1].enabled);
    CHECK(follow->choices()[1].label == "Key");
    const auto before = f.steps();
    follow->choose("chords");
    f.pass();
    CHECK(clip().follow == model::ClipFollow::Chords);
    CHECK(f.text("inspector.clip.follow") == "Chords");
    CHECK(f.steps() == before + 1);
    // The context cleared: follow goes back to none (clip.follow_reset), and
    // the dropdown follows.
    f.exec("clip.set", R"({"clip":")" + cl + R"(","context":""})");
    f.pass();
    CHECK(clip().follow == model::ClipFollow::None);
    CHECK(f.text("inspector.clip.follow") == "None");
    CHECK_FALSE(follow->choices()[1].enabled);
}

TEST_CASE("the status line counts scenes beside tracks only when there are some") {
    Fixture f;
    if (!f.ok) return;
    CHECK(gui::countsText(f.app->head()).find("scene") == std::string::npos);
    f.exec("scene.add", "{}");
    CHECK(gui::countsText(f.app->head()).find("  1 scene  version") != std::string::npos);
    f.exec("scene.add", "{}");
    CHECK(gui::countsText(f.app->head()).find("  2 scenes  version") != std::string::npos);
}
