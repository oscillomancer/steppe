// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The MIDI editor driven like a user: pointer and keys through the root, the
// notation panel typed into, the session read back through commands.
#include <doctest/doctest.h>
#include "product_fixture.h"
#include "test_support.h"

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <set>
#include <thread>

#include "app_actions.h"
#include "control_map.h"
#include "history_panel.h"
#include "midi_editor.h"
#include "midi_event_list.h"
#include "mixer.h"
#include "routing_matrix.h"
#include "supple/base/fader.h"
#include "supple/engine/realtime.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/ui/controls.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_area.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "supple/views/mouse_map.h"

using namespace supple;

namespace {

template <class T>
doctest::Approx near(T v) {
    return doctest::Approx(static_cast<double>(v));
}

const cmd::Origin kTest{cmd::Origin::Type::Ui, "test", {}};

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
    std::unique_ptr<gui::MidiEditor> editor;
    std::string item;
    bool ok = false;

    // `plugins`: the plug-in path (empty: the default).
    explicit Fixture(std::vector<std::filesystem::path> plugins = {}) {
        dir = std::filesystem::temp_directory_path() /
              ("supple-midi-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        testConfigHome(dir / "config");
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"midi-test", 1100, 560, true});
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
        if (!plugins.empty()) options.pluginPath = std::move(plugins);
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
        editor = std::make_unique<gui::MidiEditor>(*root, *actions, *dispatcher, *app, ui);
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->emplace<ui::Node>("arrange").layout.height = ui::SizeRule::fraction();
        frame->add(editor->build());
        root->setContent(std::move(frame));
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {  // as the app routes them
            const bool typing = root->focused() && root->focused()->role() == ui::Role::TextField;
            return actions->key(e.key, e.mods, typing, gui::keySections(*root));
        };
        auto t = exec("track.add", R"({"name":"Keys"})");
        auto it =
            exec("item.add", R"({"track":")" + t["track"].asString() +
                                 R"(","position":4,"notation":"voice v; 1: C4:q E4:q G4:q C5:q"})");
        item = it["item"].asString();
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        editor.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kTest});
        INFO(command << ": " << (r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    }
    json::Value notes() { return exec("midi.notes", R"({"item":")" + item + R"("})")["notes"]; }
    void frame() {
        editor->sync();
        root->frame(false);
    }
    // Pointer input in window coordinates, the same path as SDL events.
    void pointer(ui::PointerEvent::Type type, ui::Point p, int clicks = 1, unsigned mods = 0) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = 1;
        e.clicks = clicks;
        e.mods = mods;
        root->pointer(e);
        frame();
    }
    void drag(ui::Point from, ui::Point to, unsigned mods = 0) {
        pointer(ui::PointerEvent::Type::Down, from, 1, mods);
        const ui::Point mid{(from.x + to.x) / 2, (from.y + to.y) / 2};
        pointer(ui::PointerEvent::Type::Move, mid, 1, mods);
        pointer(ui::PointerEvent::Type::Move, to, 1, mods);
        pointer(ui::PointerEvent::Type::Up, to, 1, mods);
    }
    // Where a note's middle is drawn, from the roll's geometry.
    ui::Point at(double itemBeat, int pitch) const {
        const auto& s = editor->state();
        const ui::Rect b = editor->roll()->bounds();
        return {b.x + static_cast<float>((itemBeat - s.scrollBeat) * s.pixelsPerBeat),
                b.y + static_cast<float>(s.topPitch - pitch) * s.rowHeight + s.rowHeight / 2};
    }
    void wheel(ui::Point p, float y, unsigned mods) {
        ui::PointerEvent e;
        e.type = ui::PointerEvent::Type::Wheel;
        e.pos = p;
        e.wheelY = y;
        e.mods = mods;
        root->pointer(e);
        frame();
    }
    ui::Point grip() const {
        const ui::Rect b = root->find("midi.grip")->bounds();
        return {b.x + b.w / 2, b.y + b.h / 2};
    }
    void snapshot(const char* name, ui::Root* source = nullptr) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        auto& target = source ? *source : *root;
        target.damageAll();
        target.frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, target.readBack());
    }
};

}  // namespace

TEST_CASE("MIDI editor: Alt+E shows and hides it on the item it had last") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.actions->run("ui.midi.toggle"));  // never opened: the first MIDI item
    CHECK(f.editor->isOpen());
    CHECK(f.editor->state().item == f.item);
    REQUIRE(f.actions->run("ui.midi.toggle"));
    CHECK_FALSE(f.editor->isOpen());
    CHECK(f.actions->keysFor("ui.midi.toggle") == "Alt+E");
    CHECK(f.actions->key(SDLK_E, ui::kAlt, false));
    CHECK(f.editor->isOpen());
    // With no MIDI item left, it does nothing.
    f.editor->close();
    f.exec("item.remove", R"({"item":")" + f.item + R"("})");
    REQUIRE(f.actions->run("ui.midi.toggle"));
    CHECK_FALSE(f.editor->isOpen());
}

TEST_CASE("MIDI editor: hidden notation and dock controls release focus") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto* text = f.editor->notation();
    REQUIRE(text);
    REQUIRE(text->visibleInTree());
    f.root->focus(text);
    // Invoke the button without a pointer press moving focus away first.
    auto* toggle = dynamic_cast<ui::Button*>(f.root->find("midi.notation.toggle"));
    REQUIRE(toggle);
    toggle->onClick();
    CHECK(f.root->focused() == nullptr);
    const auto before = text->text();
    f.root->text("hidden");
    CHECK(text->text() == before);
    f.root->focus(f.root->find("midi.close"));
    f.editor->close();
    CHECK(f.root->focused() == nullptr);
}

TEST_CASE("MIDI CC lane edits only its displayed channel in one undo step") {
    Fixture f;
    if (!f.ok) return;
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":1,"channel":1,"points":[{"beat":0,"value":10}]},
      {"op":"cc.set","number":1,"channel":2,"points":[{"beat":0,"value":20}]}]})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    const auto before = f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
    const auto steps = f.app->versions().history(1000).size();
    const auto bounds = f.root->find("midi.cc")->bounds();
    const ui::Point p{f.at(0.05, 60).x, bounds.y + bounds.h / 2};
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Up, p);
    const auto after = f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
    REQUIRE(after.size() == 2);
    bool preserved = false;
    for (const auto& cc : after.asArray())
        if (const auto* channel = cc.find("channel"); channel && channel->asInt() == 2) {
            CHECK(cc.find("value")->asInt() == 20);
            preserved = true;
        }
    CHECK(preserved);
    CHECK(f.app->versions().history(1000).size() == steps + 1);
    f.exec("history.undo", "{}");
    CHECK(f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"] == before);
    f.frame();
    f.root->pointer({ui::PointerEvent::Type::Down, p, 3});
    f.root->pointer({ui::PointerEvent::Type::Up, p, 3});
    f.frame();
    const auto erased = f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
    REQUIRE(erased.size() == 1);
    CHECK(erased.asArray().front().find("channel")->asInt() == 2);
    CHECK(erased.asArray().front().find("value")->asInt() == 20);
    CHECK(f.app->versions().history(1000).size() == steps + 1);
    f.exec("history.undo", "{}");
    CHECK(f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"] == before);
}

TEST_CASE("MIDI editor: dragging the top border resizes the panel; double-click resets") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    REQUIRE(f.editor->open(f.item));
    f.frame();
    ui::Node* panel = f.root->find("midi.editor");
    REQUIRE(panel);
    CHECK(panel->bounds().h == near(gui::MidiEditor::kDefaultHeight));

    // Up makes it taller, down shorter; one drag is one resize, no model edit.
    const auto version = f.app->head().version;
    ui::Point p = f.grip();
    f.drag(p, {p.x, p.y - 20});
    CHECK(panel->bounds().h == near(gui::MidiEditor::kDefaultHeight + 20));
    p = f.grip();
    f.drag(p, {p.x, p.y + 150});
    CHECK(panel->bounds().h == near(gui::MidiEditor::kDefaultHeight - 130));
    CHECK(f.editor->height() == near(gui::MidiEditor::kDefaultHeight - 130));
    CHECK(f.app->head().version == version);

    // Clamped: to the minimum, and to what leaves the window's top a strip.
    p = f.grip();
    f.drag(p, {p.x, p.y + 2000});
    CHECK(panel->bounds().h == near(gui::MidiEditor::kMinHeight));
    p = f.grip();
    f.drag(p, {p.x, -500});
    const float window = f.root->find("window")->bounds().h;
    CHECK(panel->bounds().h > window * 0.7f);
    CHECK(panel->bounds().h < window);
    CHECK(f.root->find("arrange")->bounds().h >= 100);
    CHECK(f.editor->height() == near(panel->bounds().h));

    // Double-click: the default again.
    f.pointer(ui::PointerEvent::Type::Down, f.grip(), 2);
    f.pointer(ui::PointerEvent::Type::Up, f.grip());
    CHECK(panel->bounds().h == near(gui::MidiEditor::kDefaultHeight));
    CHECK(f.editor->roll()->bounds().h > 100);
}

TEST_CASE("MIDI editor: the panel height persists, and a too-large one is shown clamped") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    REQUIRE(f.editor->open(f.item));
    f.frame();
    f.editor->setHeight(300);
    const json::Value saved = f.editor->saveLayout();
    CHECK(saved.find("height")->asDouble() == near(300));
    f.editor->setHeight(200);
    f.editor->loadLayout(saved);
    f.frame();
    CHECK(f.root->find("midi.editor")->bounds().h == near(300));
    // Junk is ignored; an oversized height (a smaller window than the one it
    // was saved in) is clamped to what fits.
    f.editor->loadLayout(json::parse(R"({"height":"tall"})").value());
    CHECK(f.editor->height() == near(300));
    f.editor->loadLayout(json::parse(R"({"height":99999})").value());
    f.frame();
    const float window = f.root->find("window")->bounds().h;
    CHECK(f.root->find("midi.editor")->bounds().h < window);
    CHECK(f.root->find("arrange")->bounds().h >= 100);
    CHECK(f.editor->height() > window);  // the preference stays for a larger window
}

TEST_CASE("MIDI editor: Alt+wheel zooms the rows around the pointer's pitch") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    const unsigned alt = ui::kAlt;
    const double beat = s.scrollBeat, ppb = s.pixelsPerBeat;
    const ui::Rect b = f.editor->roll()->bounds();
    const ui::Point at{b.x + 200, b.y + 90};
    auto pitchAt = [&](float y) {
        return s.topPitch - static_cast<double>((y - b.y) / s.rowHeight);
    };
    const double pitch = pitchAt(at.y);
    const float before = s.rowHeight;

    f.wheel(at, 3, alt);
    CHECK(s.rowHeight > before);
    CHECK(pitchAt(at.y) == near(pitch));  // the pointer's pitch stays under it
    CHECK(s.scrollBeat == near(beat));    // rows only: the time axis is untouched
    CHECK(s.pixelsPerBeat == near(ppb));
    f.wheel(at, -3, alt);
    CHECK(s.rowHeight == near(before));
    CHECK(pitchAt(at.y) == near(pitch));

    // Clamped both ways.
    f.wheel(at, 100, alt);
    CHECK(s.rowHeight == near(gui::MidiEditor::kMaxRowHeight));
    f.wheel(at, -200, alt);
    CHECK(s.rowHeight == near(gui::MidiEditor::kMinRowHeight));
    f.wheel(at, 100, alt);

    // Over the keys and the velocity lane too (anywhere in the editor); the
    // plain wheel zooms time instead, and Ctrl scrolls pitch.
    s.rowHeight = 12;
    const ui::Rect keys = f.root->find("midi.keys")->bounds();
    f.wheel({keys.x + 10, keys.y + 40}, 2, alt);
    CHECK(s.rowHeight > 12);
    s.rowHeight = 12;
    const ui::Rect vel = f.root->find("midi.velocity")->bounds();
    f.wheel({vel.x + 200, vel.y + 20}, 2, alt);
    CHECK(s.rowHeight > 12);
    s.rowHeight = 12;
    f.wheel(at, 2, 0);
    CHECK(s.rowHeight == near(12));
    CHECK(s.pixelsPerBeat > ppb);
    const double top = s.topPitch;
    f.wheel(at, 1, ui::kCtrl);
    CHECK(s.rowHeight == near(12));
    CHECK(s.topPitch == near(top + 3));
    f.snapshot("midi-editor-row-zoom.png");
}

TEST_CASE("MIDI editor: the wheel follows the mouse map") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.root->frame(false);
    gui::RollState& s = f.editor->state();
    views::MouseMap map;
    REQUIRE(map.set("midi.wheel", 0, "zoom_rows"));
    REQUIRE(map.set("midi.wheel", ui::kShift, "zoom"));
    f.editor->setMouseMap(&map);
    s.rowHeight = 12;
    const ui::Rect b = f.editor->roll()->bounds();
    const double ppb = s.pixelsPerBeat;
    f.wheel({b.x + 200, b.y + 90}, 2, 0);  // plain zooms the rows now, not scrolls pitch
    CHECK(s.rowHeight > 12);
    s.rowHeight = 12;
    const ui::Rect keys = f.root->find("midi.keys")->bounds();
    f.wheel({keys.x + 10, keys.y + 40}, 2, 0);  // over the keys too
    CHECK(s.rowHeight > 12);
    f.wheel({b.x + 200, b.y + 90}, 2, ui::kShift);
    CHECK(s.pixelsPerBeat > ppb);
    f.editor->setMouseMap(nullptr);
    s.rowHeight = 12;
    const double zoomed = s.pixelsPerBeat;
    f.wheel({b.x + 200, b.y + 90}, 1, 0);  // the defaults again: plain zooms time
    CHECK(s.pixelsPerBeat > zoomed);
    CHECK(s.rowHeight == near(12));
}

TEST_CASE("MIDI editor: an empty roll can marquee without a note anchor") {
    Fixture f;
    if (!f.ok) return;
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"remove","notes":[0,1,2,3]}]})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    const auto version = f.app->head().version;
    f.drag(f.at(0.5, 60), f.at(2, 64));
    CHECK(f.editor->state().notes.empty());
    CHECK(f.editor->state().selected.empty());
    CHECK(f.app->head().version == version);
}

TEST_CASE("MIDI editor: selected notes painted on top win overlapping hit tests") {
    Fixture f;
    if (!f.ok) return;
    f.exec("midi.edit",
           R"({"item":")" + f.item +
               R"(","ops":[{"op":"add","pitch":60,"start":0,"length":1,"velocity":40}]})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& state = f.editor->state();
    REQUIRE(state.notes.size() == 5);
    state.selected = {0};
    f.frame();
    f.pointer(ui::PointerEvent::Type::Down, f.at(0.4, 60));
    CHECK(state.selected == std::set<std::size_t>{0});
    f.pointer(ui::PointerEvent::Type::Up, f.at(0.4, 60));
    CHECK(state.selected == std::set<std::size_t>{0});
}

TEST_CASE("MIDI editor: external note removal cancels a captured drag") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    const ui::Point start = f.at(3.5, 72);
    f.pointer(ui::PointerEvent::Type::Down, start);
    REQUIRE(f.editor->state().drag == gui::RollState::Drag::Move);
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[{"op":"remove","notes":[3]}]})");
    f.frame();
    CHECK(f.editor->state().drag == gui::RollState::Drag::None);
    const auto version = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Move, {start.x + 20, start.y - 12});
    f.pointer(ui::PointerEvent::Type::Up, {start.x + 20, start.y - 12});
    CHECK(f.app->head().version == version);
    CHECK(f.notes().size() == 3);
}

TEST_CASE("MIDI editor: closing cancels velocity and CC previews before reopening") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    SUBCASE("velocity") {
        const ui::Rect lane = f.root->find("midi.velocity")->bounds();
        f.pointer(ui::PointerEvent::Type::Down, {f.at(0, 60).x, lane.y + 10});
        REQUIRE(f.editor->state().drag == gui::RollState::Drag::Velocity);
    }
    SUBCASE("CC") {
        const ui::Rect lane = f.root->find("midi.cc")->bounds();
        f.pointer(ui::PointerEvent::Type::Down, {f.at(1, 60).x, lane.y + 10});
        REQUIRE_FALSE(f.editor->state().ccDraft.empty());
    }
    f.editor->close();
    REQUIRE(f.editor->open(f.item));
    f.frame();
    CHECK(f.editor->state().drag == gui::RollState::Drag::None);
    CHECK_FALSE(f.editor->state().velocityNote);
    CHECK(f.editor->state().ccDraft.empty());
    const auto version = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Move, f.at(2, 60));
    f.pointer(ui::PointerEvent::Type::Up, f.at(2, 60));
    CHECK(f.app->head().version == version);
}

TEST_CASE("MIDI editor: the grid's menu copies, pastes there, cuts and duplicates notes") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    REQUIRE(s.notes.size() == 4);  // C4 E4 G4 C5, a beat each
    auto rightClick = [&](ui::Point p) {
        ui::PointerEvent e;
        e.type = ui::PointerEvent::Type::Down;
        e.pos = p;
        e.button = 3;
        f.root->pointer(e);
        e.type = ui::PointerEvent::Type::Up;
        f.root->pointer(e);
        f.frame();
    };
    auto pick = [&](int row) {  // Delete, Copy, Cut, Paste, Duplicate
        auto* menu = dynamic_cast<ui::List*>(f.root->find("midi.menu"));
        REQUIRE(menu);
        auto activate = menu->onActivate;  // closes (destroys) the menu
        activate(static_cast<std::size_t>(row));
        f.frame();
    };
    s.selected = {0, 1};
    rightClick(f.at(0.5, 60));  // on a selected note: the selection stays
    CHECK(s.selected == std::set<std::size_t>{0, 1});
    pick(1);                    // Copy
    rightClick(f.at(4.1, 72));  // empty grid, at beat 4: the copy lands there
    pick(3);                    // Paste
    REQUIRE(s.notes.size() == 6);
    REQUIRE(s.selected.size() == 2);  // the pasted notes
    std::vector<double> starts;
    for (std::size_t i : s.selected) starts.push_back(s.notes[i].start);
    CHECK(starts == std::vector<double>{4, 5});
    rightClick(f.at(4.5, 60));
    pick(4);  // Duplicate: right after themselves
    CHECK(s.notes.size() == 8);
    rightClick(f.at(6.5, 60));  // on the duplicated (selected) pair
    pick(2);                    // Cut
    CHECK(s.notes.size() == 6);
    rightClick(f.at(0.5, 60));
    pick(3);  // Paste the cut pair back, at beat 0
    CHECK(s.notes.size() == 8);
}

TEST_CASE("keys act where the pointer is: Alt+C deletes notes over the editor, items elsewhere") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    s.selected = {0};
    f.ui.selectedItems = {f.item};
    f.pointer(ui::PointerEvent::Type::Move, f.at(8, 40));  // over the editor
    f.root->key(SDLK_C, ui::kAlt);
    f.frame();
    CHECK(f.notes().size() == 3);
    CHECK(f.app->head().items.size() == 1);
    // Over the arrange view (the focus still in the editor): the selected item goes.
    const ui::Rect arrange = f.root->find("arrange")->bounds();
    f.pointer(ui::PointerEvent::Type::Move, {arrange.x + 40, arrange.y + 20});
    f.root->key(SDLK_C, ui::kAlt);
    f.frame();
    CHECK(f.app->head().items.empty());
}

TEST_CASE("MIDI editor: open, draw, select, move, resize, transpose, delete, velocity") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    REQUIRE(s.notes.size() == 4);
    CHECK(s.itemPosition == doctest::Approx(4.0));
    CHECK(f.editor->roll()->bounds().w > 300);
    f.snapshot("midi-editor-open.png");

    // Double-click empty space: a note of the grid length, snapped.
    s.grid = 0.5;
    s.lastLength = 0.5;
    const auto v0 = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Down, f.at(2.6, 65), 2);
    f.pointer(ui::PointerEvent::Type::Up, f.at(2.6, 65));
    auto n = f.notes();
    REQUIRE(n.size() == 5);
    CHECK(f.app->head().version == v0 + 1);
    bool found = false;
    for (const auto& x : n.asArray())
        if (x.find("pitch")->asInt() == 65) {
            found = true;
            CHECK(x.find("start")->asDouble() == doctest::Approx(2.5));
            CHECK(x.find("length")->asDouble() == doctest::Approx(0.5));
        }
    CHECK(found);
    REQUIRE(s.selected.size() == 1);  // the new note is selected

    // Drag E4 (beat 1) one beat later and up a tone: snapped, one version.
    const auto v1 = f.app->head().version;
    f.drag(f.at(1.3, 64), {f.at(2.3, 64).x, f.at(2.3, 66).y});
    n = f.notes();
    CHECK(f.app->head().version == v1 + 1);
    found = false;
    for (const auto& x : n.asArray())
        if (x.find("pitch")->asInt() == 66) {
            found = true;
            CHECK(x.find("start")->asDouble() == doctest::Approx(2.0));
        }
    CHECK(found);

    // Resize the end of C4 (beat 0, length 1) by half a beat.
    const ui::Point end = f.at(0.98, 60);
    f.drag(end, {end.x + static_cast<float>(0.5 * s.pixelsPerBeat), end.y});
    CHECK(f.notes().at(0).find("length")->asDouble() == doctest::Approx(1.5));

    // Keys: Up transposes the selection, Delete removes it.
    f.pointer(ui::PointerEvent::Type::Down, f.at(0.5, 60));
    f.pointer(ui::PointerEvent::Type::Up, f.at(0.5, 60));
    REQUIRE(s.selected.size() == 1);
    f.root->key(SDLK_UP, 0);
    f.frame();
    CHECK(f.notes().at(0).find("pitch")->asInt() == 61);
    f.root->key(SDLK_UP, ui::kShift);
    f.frame();
    CHECK(f.notes().at(0).find("pitch")->asInt() == 73);
    const std::size_t before = f.notes().size();
    f.root->key(SDLK_DELETE, 0);
    f.frame();
    CHECK(f.notes().size() == before - 1);

    // Velocity: drag a stem in the lane to the top.
    f.root->key(SDLK_A, ui::kCtrl);
    f.frame();
    f.pointer(ui::PointerEvent::Type::Down, f.at(0.0, 60));  // deselect by clicking empty space
    f.pointer(ui::PointerEvent::Type::Up, f.at(0.0, 60));
    const ui::Rect lane = f.root->find("midi.velocity")->bounds();
    const auto first = f.notes().at(0);
    const float stemX = f.at(first.find("start")->asDouble(), 60).x + 1;
    f.drag({stemX, lane.bottom() - 10}, {stemX, lane.y + 2});
    CHECK(f.notes().at(0).find("velocity")->asInt() >= 120);

    // Quantize everything to the beat after nudging a note off the grid.
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"move","notes":[0],"beats":0.1}]})");
    f.frame();
    s.grid = 1;
    f.editor->quantize();
    const json::Value quantized = f.notes();
    for (const auto& x : quantized.asArray()) {
        const double start = x.find("start")->asDouble() + 4;  // project beats
        CHECK(std::abs(start - std::round(start * 2) / 2) < 1e-6);
    }
    f.snapshot("midi-editor-edited.png");
}

TEST_CASE("MIDI editor: the CC lane draws a ramp in one step, erases, and switches controllers") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    s.grid = 0.5;
    auto cc = [&](int number) {
        std::vector<std::pair<double, int>> out;
        const json::Value all = f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
        for (const auto& c : all.asArray())
            if (c.find("number")->asInt() == number)
                out.emplace_back(c.find("beat")->asDouble(),
                                 static_cast<int>(c.find("value")->asInt()));
        return out;
    };
    auto steps = [&] { return f.exec("history.list", "{}")["versions"].size(); };
    const ui::Rect lane = f.root->find("midi.cc")->bounds();
    REQUIRE(lane.h > 20);
    const auto xAt = [&](double beat) { return f.at(beat, 60).x + 1; };
    const auto s0 = steps();
    // Draw from beat 0 (low) to beat 2 (high).
    f.pointer(ui::PointerEvent::Type::Down, {xAt(0.0), lane.bottom() - 8});
    f.pointer(ui::PointerEvent::Type::Move, {xAt(1.0), lane.y + lane.h / 2});
    f.pointer(ui::PointerEvent::Type::Move, {xAt(2.0), lane.y + 6});
    f.pointer(ui::PointerEvent::Type::Up, {xAt(2.0), lane.y + 6});
    auto drawn = cc(1);
    REQUIRE(drawn.size() == 3);  // one per grid step the pointer visited
    CHECK(drawn.front().first == doctest::Approx(0.0));
    CHECK(drawn.back().first == doctest::Approx(2.0));
    CHECK(drawn.front().second < 30);
    CHECK(drawn.back().second > 100);
    CHECK(steps() == s0 + 1);
    // Right-drag erases the span.
    ui::PointerEvent down{ui::PointerEvent::Type::Down, {xAt(0.9), lane.y + 10}, 3};
    f.root->pointer(down);
    f.pointer(ui::PointerEvent::Type::Move, {xAt(2.2), lane.y + 10});
    ui::PointerEvent up{ui::PointerEvent::Type::Up, {xAt(2.2), lane.y + 10}, 3};
    f.root->pointer(up);
    f.frame();
    CHECK(cc(1).size() == 1);  // the point at 0 stays
    // The next controller (7, volume) has its own lane.
    REQUIRE(f.root->click("midi.cc.number"));
    CHECK(s.ccNumber == 7);
    CHECK(s.cc.empty());
    f.pointer(ui::PointerEvent::Type::Down, {xAt(1.0), lane.y + 10});
    f.pointer(ui::PointerEvent::Type::Up, {xAt(1.0), lane.y + 10});
    CHECK(cc(7).size() == 1);
    CHECK(cc(1).size() == 1);
    f.snapshot("midi-editor-cc.png");
}

TEST_CASE("MIDI editor: the notation panel compiles live, writes back, and follows the roll") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    ui::TextArea* text = f.editor->notation();
    REQUIRE(text);
    CHECK(text->text().find("C4") != std::string::npos);

    CHECK_FALSE(f.editor->nextDeadline());
    // Type a new phrase: after the pause it compiles and is written.
    f.root->focus(text);
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("voice v; 1: D4:h F4:h");
    REQUIRE(f.editor->nextDeadline());
    f.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.frame();
    CHECK_FALSE(f.editor->nextDeadline());
    auto n = f.notes();
    REQUIRE(n.size() == 2);
    CHECK(n.at(0).find("pitch")->asInt() == 62);
    CHECK(f.editor->state().notes.size() == 2);  // the roll followed

    // A mistake: located diagnostics, nothing written.
    const auto v = f.app->head().version;
    f.root->key(SDLK_END, ui::kCtrl);
    f.root->text(" Q9:zz");
    f.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.frame();
    CHECK(f.app->head().version == v);
    REQUIRE_FALSE(text->marks().empty());
    CHECK(text->marks().front().line == 1);
    f.snapshot("midi-editor-diagnostic.png");

    // Fix it; the typing session is one undo step.
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("voice v; 1: A3:w");
    f.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.frame();
    CHECK(text->marks().empty());
    CHECK(f.notes().size() == 1);

    // An edit from elsewhere (an agent) reaches both views when the text is not being typed in.
    f.root->focus(f.editor->roll());
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"add","pitch":72,"start":1,"length":1}]})");
    f.frame();
    CHECK(f.editor->state().notes.size() == 2);
    CHECK(text->text().find("C5") != std::string::npos);

    // The item goes away: the editor closes.
    f.exec("item.remove", R"({"item":")" + f.item + R"("})");
    f.frame();
    CHECK_FALSE(f.editor->isOpen());
}

TEST_CASE("routing matrix: rules shown, a double click sends, the send edits and goes") {
    Fixture f;
    if (!f.ok) return;
    const std::string drums =
        f.exec("track.add", R"({"type":"buss","name":"Drums"})")["track"].asString();
    const std::string verb =
        f.exec("track.add", R"({"type":"buss","name":"Verb"})")["track"].asString();
    f.exec("route.add", R"({"from":")" + verb + R"(","to":")" + drums + R"("})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    REQUIRE(matrix.isOpen());
    CHECK(std::string(SDL_GetWindowTitle(panel.window()->sdl())) == "Routing");
    CHECK(panel.root().find("routing.title")->valueText() ==
          SDL_GetWindowTitle(panel.window()->sdl()));
    const auto& m = matrix.model();
    auto row = [&](const std::string& id) {
        for (std::size_t i = 0; i < m.sources.size(); ++i)
            if (m.sources[i].id == id) return i;
        FAIL("no source " << id);
        return std::size_t{0};
    };
    auto col = [&](const std::string& id) {
        for (std::size_t i = 0; i < m.destinations.size(); ++i)
            if (m.destinations[i].id == id) return i;
        FAIL("no destination " << id);
        return std::size_t{0};
    };
    std::string keys;
    for (const auto& s : m.sources)
        if (s.name == "Keys") keys = s.id;
    REQUIRE_FALSE(keys.empty());
    // Rules: a channel takes no strip audio; the master is terminal; Drums -> Verb feeds back.
    CHECK_FALSE(m.blocked(row(drums), col(keys)).empty());
    CHECK_FALSE(m.blocked(row("tr_0"), col(drums)).empty());
    CHECK(m.blocked(row(drums), col(verb)).find("feed back") != std::string::npos);
    CHECK(m.blocked(row(keys), col(drums)).empty());
    // Hovering a blocked cell says why.
    auto* grid = matrix.grid();
    REQUIRE(grid);
    f.snapshot("routing-open.png", &panel.root());
    // A click on an allowed cell selects it; a double click there sends (one
    // version) and the new send is the one the controls show.
    const auto v = f.app->head().version;
    REQUIRE(matrix.clickCell(row(keys), col(drums)));
    CHECK(f.app->head().version == v);
    CHECK(matrix.isSelected(row(keys), col(drums)));
    REQUIRE(matrix.doubleClickCell(row(keys), col(drums)));
    CHECK(f.app->head().version == v + 1);
    REQUIRE(matrix.selectedSend());
    CHECK(m.sendsAt(row(keys), col(drums)).size() == 1);
    // A blocked double click changes nothing.
    CHECK_FALSE(matrix.doubleClickCell(row(drums), col(verb)));
    CHECK(f.app->head().version == v + 1);
    REQUIRE(matrix.clickCell(row(keys), col(drums)));
    // Edit the send: the level (the fader's one-shot: a double click or the
    // wheel), pre-fader, mute; then remove it.
    REQUIRE(matrix.volumeControl());
    matrix.volumeControl()->onChange(-6.0, ui::Gesture::Set);
    CHECK(m.sendsAt(row(keys), col(drums)).front()->volumeDb == doctest::Approx(-6.0));
    CHECK(panel.root().find("routing.volume.value")->valueText() == "-6.0 dB");
    REQUIRE(panel.root().click("routing.tap.pre"));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->tap == "pre_fader");
    REQUIRE(panel.root().click("routing.mute"));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->mute);
    f.snapshot("routing-send.png", &panel.root());
    // Channels: the menu's second row swaps left and right (route.set channels).
    REQUIRE(panel.root().click("routing.channels"));
    panel.root().frame(false);
    ui::Node* menu = panel.root().find("routing.channels.menu");
    REQUIRE(menu);
    const ui::Point swapRow{menu->bounds().x + 30, menu->bounds().y + 26 + 13};
    panel.root().pointer({ui::PointerEvent::Type::Move, swapRow});
    panel.root().pointer({ui::PointerEvent::Type::Down, swapRow, 1});
    panel.root().pointer({ui::PointerEvent::Type::Up, swapRow, 1});
    panel.root().frame(false);
    CHECK(m.sendsAt(row(keys), col(drums)).front()->mapTo == std::vector<int>{2, 1});
    CHECK(panel.root().find("routing.channels")->valueText() == "Swap R/L");
    // A 4-channel destination offers its aux pair (ADR-0020): row 7, "1/2 -> 3/4".
    f.exec("track.set", R"({"track":")" + drums + R"(","channels":4})");
    matrix.sync();
    REQUIRE(panel.root().click("routing.channels"));
    panel.root().frame(false);
    menu = panel.root().find("routing.channels.menu");
    REQUIRE(menu);
    const ui::Point auxRow{menu->bounds().x + 30, menu->bounds().y + 7 * 26 + 13};
    panel.root().pointer({ui::PointerEvent::Type::Move, auxRow});
    panel.root().pointer({ui::PointerEvent::Type::Down, auxRow, 1});
    panel.root().pointer({ui::PointerEvent::Type::Up, auxRow, 1});
    panel.root().frame(false);
    CHECK(m.sendsAt(row(keys), col(drums)).front()->mapTo == std::vector<int>{3, 4});
    CHECK(panel.root().find("routing.channels")->valueText() == "1/2\u21923/4");
    REQUIRE(panel.root().click("routing.phase"));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->phaseInvert);
    f.snapshot("routing-channels.png", &panel.root());
    REQUIRE(panel.root().click("routing.remove"));
    CHECK(m.sendsAt(row(keys), col(drums)).empty());
    // Changes from elsewhere arrive on sync.
    f.exec("route.add", R"({"from":")" + keys + R"(","to":")" + verb + R"("})");
    matrix.sync();
    CHECK(m.sendsAt(row(keys), col(verb)).size() == 1);
    // A double click on the parent's cell toggles the parent send.
    REQUIRE(matrix.doubleClickCell(row(keys), col("tr_0")));
    CHECK_FALSE(f.app->head().tracks.find(*model::Id::parse(keys))->parentSend.enabled);
    REQUIRE(matrix.doubleClickCell(row(keys), col("tr_0")));
    CHECK(f.app->head().tracks.find(*model::Id::parse(keys))->parentSend.enabled);
    REQUIRE(panel.root().click("routing.close"));
    CHECK_FALSE(matrix.isOpen());
    CHECK(panel.root().focused() == nullptr);
}

TEST_CASE("routing matrix: a rack's In modules are columns; a cell sends into one") {
    Fixture f;
    if (!f.ok) return;
    const std::string synth = f.exec("track.add", R"({"name":"Synth"})")["track"].asString();
    const std::string rack =
        f.exec("rack.add", R"({"track":")" + synth + R"("})")["rack"].asString();
    const std::string in =
        f.exec("module.add", R"({"rack":")" + rack + R"(","type":"in"})")["module"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    REQUIRE(matrix.isOpen());
    const auto& m = matrix.model();
    auto row = [&](const std::string& id, int pair = 1) {
        for (std::size_t i = 0; i < m.sources.size(); ++i)
            if (m.sources[i].id == id && m.sources[i].pair == pair) return i;
        FAIL("no source " << id);
        return std::size_t{0};
    };
    std::size_t col = m.destinations.size();
    for (std::size_t i = 0; i < m.destinations.size(); ++i)
        if (m.destinations[i].id == in) col = i;
    REQUIRE(col < m.destinations.size());
    // The In module's column follows its channel's, named as the input menu names an Out.
    CHECK(m.destinations[col].kind == "module");
    CHECK(m.destinations[col].name == "Synth \u00bb In 1");
    CHECK(m.destinations[col].track == synth);
    REQUIRE(col > 0);
    CHECK(m.destinations[col - 1].id == synth);
    std::string keys;
    for (const auto& s : m.sources)
        if (s.name == "Keys") keys = s.id;
    REQUIRE_FALSE(keys.empty());
    // The rack's own channel would feed back (its Out is the channel's input);
    // the master is terminal; another channel may send.
    CHECK(m.blocked(row(synth), col) ==
          "it would feed back: Synth (rack) -> Synth -> Synth (rack)");  // by name
    CHECK_FALSE(m.blocked(row("tr_0"), col).empty());
    CHECK(m.blocked(row(keys), col).empty());
    // The rack's Out route is the channel's input, not a send.
    CHECK(m.sends.empty());
    CHECK(matrix.status().starts_with("0 sends"));
    CHECK_FALSE(matrix.doubleClickCell(row(synth), col));
    // A double click sends Keys into the In module (route.add {to: mo_N}), one version.
    const auto v = f.app->head().version;
    REQUIRE(matrix.clickCell(row(keys), col));
    REQUIRE(matrix.doubleClickCell(row(keys), col));
    CHECK(f.app->head().version == v + 1);
    REQUIRE(m.sendsAt(row(keys), col).size() == 1);
    const std::string send = m.sendsAt(row(keys), col).front()->id;
    CHECK(matrix.selectedSend() == send);
    const model::Route* r = f.app->head().routes.find(*model::Id::parse(send));
    REQUIRE(r);
    CHECK(r->to.module == model::Id::parse(in));
    CHECK(r->from.track == model::Id::parse(keys));
    CHECK(matrix.status().starts_with("1 sends"));
    // It edits as any send: the level, the tap, mute.
    REQUIRE(matrix.volumeControl());
    matrix.volumeControl()->onChange(-6.0, ui::Gesture::Set);
    CHECK(f.app->head().routes.find(*model::Id::parse(send))->volumeDb == doctest::Approx(-6.0));
    REQUIRE(panel.root().click("routing.tap.pre"));
    CHECK(m.sendsAt(row(keys), col).front()->tap == "pre_fader");
    REQUIRE(panel.root().click("routing.mute"));
    CHECK(m.sendsAt(row(keys), col).front()->mute);
    f.snapshot("routing-rack-in.png", &panel.root());
    // Remove takes it away.
    REQUIRE(panel.root().click("routing.remove"));
    CHECK(m.sendsAt(row(keys), col).empty());
    CHECK_FALSE(f.app->head().routes.find(*model::Id::parse(send)));
    // An auxiliary pair sends into the In module's pair (ADR-0021).
    f.exec("track.set", R"({"track":")" + keys + R"(","channels":4})");
    matrix.sync();
    REQUIRE(matrix.doubleClickCell(row(keys, 3), col));
    REQUIRE(m.sendsAt(row(keys, 3), col).size() == 1);
    CHECK(m.sendsAt(row(keys), col).empty());
    const model::Route* aux =
        f.app->head().routes.find(*model::Id::parse(m.sendsAt(row(keys, 3), col).front()->id));
    REQUIRE(aux);
    CHECK(aux->to.module == model::Id::parse(in));
    CHECK(aux->channels.from == std::vector<int>{3, 4});
    CHECK(aux->channels.to == std::vector<int>{1, 2});
}

TEST_CASE("routing matrix: a channel's hardware or MIDI input is not a send") {
    Fixture f;
    if (!f.ok) return;
    // Both inputs are routes (session-model.md §2) with no source strip.
    const std::string mic = f.exec("track.add", R"({"name":"Mic"})")["track"].asString();
    const std::string keys = f.exec("track.add", R"({"name":"Keyboard"})")["track"].asString();
    f.exec("track.set", R"({"track":")" + mic + R"(","record_input":[1, 2]})");
    f.exec("track.set", R"({"track":")" + keys + R"(","record_midi":"*"})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    REQUIRE(matrix.isOpen());
    CHECK(matrix.model().sends.empty());
    CHECK(matrix.status().starts_with("0 sends"));
    // A real send still counts beside them.
    const std::string verb =
        f.exec("track.add", R"({"type":"buss","name":"Verb"})")["track"].asString();
    f.exec("route.add", R"({"from":")" + mic + R"(","to":")" + verb + R"("})");
    matrix.sync();
    CHECK(matrix.model().sends.size() == 1);
    CHECK(matrix.status().starts_with("1 sends"));
}

TEST_CASE(
    "routing matrix: cells select by click, Ctrl, Shift, row and column; controls act on all") {
    Fixture f;
    if (!f.ok) return;
    const std::string drums =
        f.exec("track.add", R"({"type":"buss","name":"Drums"})")["track"].asString();
    const std::string verb =
        f.exec("track.add", R"({"type":"buss","name":"Verb"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    const auto& m = matrix.model();
    auto row = [&](const std::string& id) {
        for (std::size_t i = 0; i < m.sources.size(); ++i)
            if (m.sources[i].id == id) return i;
        FAIL("no source " << id);
        return std::size_t{0};
    };
    auto col = [&](const std::string& id) {
        for (std::size_t i = 0; i < m.destinations.size(); ++i)
            if (m.destinations[i].id == id) return i;
        FAIL("no destination " << id);
        return std::size_t{0};
    };
    std::string keys;
    for (const auto& s : m.sources)
        if (s.name == "Keys") keys = s.id;
    REQUIRE_FALSE(keys.empty());
    auto steps = [&] { return f.exec("history.list", "{}")["versions"].size(); };
    // Through the grid: a click selects, a double click sends.
    auto* grid = matrix.grid();
    REQUIRE(grid);
    const ui::Point at{grid->bounds().x + 190 + (static_cast<float>(col(drums)) + 0.5f) * 22,
                       grid->bounds().y + 112 + (static_cast<float>(row(keys)) + 0.5f) * 22};
    auto before = steps();
    panel.root().pointer({ui::PointerEvent::Type::Down, at, 1});
    panel.root().pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(steps() == before);
    CHECK(matrix.selectedCells() == 1);
    panel.root().pointer({ui::PointerEvent::Type::Down, at, 1, 2});
    panel.root().pointer({ui::PointerEvent::Type::Up, at, 1, 2});
    CHECK(steps() == before + 1);
    CHECK(m.sendsAt(row(keys), col(drums)).size() == 1);
    // Add, over a cell and Ctrl+click another: the one without a send gets one.
    REQUIRE(matrix.clickCell(row(keys), col(verb), ui::kCtrl));
    CHECK(matrix.selectedCells() == 2);
    before = steps();
    REQUIRE(panel.root().click("routing.add"));
    CHECK(steps() == before + 1);
    REQUIRE(m.sendsAt(row(keys), col(verb)).size() == 1);
    CHECK(matrix.selectedSends().size() == 2);
    // A level drag moves both by the same dB (one undo step); the shown send
    // is the last one clicked.
    f.exec("route.set",
           R"({"route":")" + m.sendsAt(row(keys), col(verb)).front()->id + R"(","volume_db":-3})");
    matrix.sync();
    auto* fader = matrix.volumeControl();
    before = steps();
    fader->onChange(-3, ui::Gesture::Begin);
    fader->onChange(-6, ui::Gesture::Move);
    fader->onChange(-9, ui::Gesture::Move);
    fader->onChange(-9, ui::Gesture::End);
    CHECK(steps() == before + 1);
    CHECK(m.sendsAt(row(keys), col(verb)).front()->volumeDb == doctest::Approx(-9.0));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->volumeDb == doctest::Approx(-6.0));
    // Pan and mute go to both.
    matrix.panControl()->onChange(-40, ui::Gesture::Set);
    REQUIRE(panel.root().click("routing.mute"));
    for (const std::string& to : {drums, verb}) {
        CHECK(m.sendsAt(row(keys), col(to)).front()->pan == doctest::Approx(-40.0));
        CHECK(m.sendsAt(row(keys), col(to)).front()->mute);
    }
    // Shift+click: the rectangle from the anchor. A row's name: all of it.
    REQUIRE(matrix.clickCell(row(drums), col(drums)));
    REQUIRE(matrix.clickCell(row(verb), col(verb), ui::kShift));
    CHECK(matrix.selectedCells() == 4);
    matrix.clickRow(row(keys));
    CHECK(matrix.selectedCells() == m.destinations.size());
    CHECK(matrix.selectedSends().size() == 2);
    matrix.clickColumn(col(drums), ui::kCtrl);
    CHECK(matrix.isSelected(row(verb), col(drums)));
    matrix.selectAll();
    CHECK(matrix.selectedCells() == m.sources.size() * m.destinations.size());
    // Remove over the keys row: both sends, and its parent send, in one step.
    matrix.clickRow(row(keys));
    before = steps();
    REQUIRE(panel.root().click("routing.remove"));
    CHECK(steps() == before + 1);
    CHECK(m.sendsAt(row(keys), col(drums)).empty());
    CHECK(m.sendsAt(row(keys), col(verb)).empty());
    CHECK_FALSE(f.app->head().tracks.find(*model::Id::parse(keys))->parentSend.enabled);
    f.snapshot("routing-selection.png", &panel.root());
}

TEST_CASE(
    "routing matrix: a right-click learns MIDI for the selected sends; a knob follows the last") {
    Fixture f;
    if (!f.ok) return;
    f.app->attachCommands(*f.dispatcher);  // learned controls move through the bus
    const std::string drums =
        f.exec("track.add", R"({"type":"buss","name":"Drums"})")["track"].asString();
    const std::string verb =
        f.exec("track.add", R"({"type":"buss","name":"Verb"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    const auto& m = matrix.model();
    std::size_t keys = m.sources.size(), toDrums = 0, toVerb = 0;
    for (std::size_t i = 0; i < m.sources.size(); ++i)
        if (m.sources[i].name == "Keys") keys = i;
    for (std::size_t i = 0; i < m.destinations.size(); ++i) {
        if (m.destinations[i].id == drums) toDrums = i;
        if (m.destinations[i].id == verb) toVerb = i;
    }
    REQUIRE(keys < m.sources.size());
    REQUIRE(matrix.doubleClickCell(keys, toDrums));
    REQUIRE(matrix.doubleClickCell(keys, toVerb));
    REQUIRE(matrix.clickCell(keys, toDrums));
    REQUIRE(matrix.clickCell(keys, toVerb, ui::kCtrl));  // shown: the Verb send
    REQUIRE(matrix.selectedSends().size() == 2);
    const std::string drumsSend = m.sendsAt(keys, toDrums).front()->id;
    const std::string verbSend = m.sendsAt(keys, toVerb).front()->id;
    auto route = [&](const std::string& id) {
        return *f.app->head().routes.find(*model::Id::parse(id));
    };
    auto steps = [&] { return f.exec("history.list", "{}")["versions"].size(); };

    // A right-click on the level: its menu, then the dialog; OK binds both.
    const ui::Rect level = matrix.volumeControl()->bounds();
    const ui::Point at{level.x + level.w / 2, level.y + level.h / 2};
    panel.root().pointer({ui::PointerEvent::Type::Down, at, 3});
    panel.root().pointer({ui::PointerEvent::Type::Up, at, 3});
    panel.root().frame(false);
    ui::Node* menu = panel.root().find("strip.learn.menu");
    REQUIRE(menu);
    CHECK(ui::menuItems(*menu)[0].label == "MIDI learn: Volume…");
    const ui::Point row{menu->bounds().x + 40, menu->bounds().y + 13};
    panel.root().pointer({ui::PointerEvent::Type::Move, row, 0});
    panel.root().pointer({ui::PointerEvent::Type::Down, row, 1});
    panel.root().pointer({ui::PointerEvent::Type::Up, row, 1});
    panel.root().frame(false);
    REQUIRE(panel.root().find("learn"));
    engine::EngineApp::ControlEvent cc;
    cc.midi.bytes[0] = 0xb0;
    cc.midi.bytes[1] = 30;
    cc.midi.bytes[2] = 100;
    f.actions->control({cc});
    auto before = steps();
    REQUIRE(panel.root().click("learn.ok"));
    CHECK(steps() == before + 1);
    for (const std::string& id : {drumsSend, verbSend}) {
        REQUIRE(route(id).learn.size() == 1);
        CHECK(route(id).learn[0].param == "volume");
        CHECK(route(id).learn[0].control.number == 30);
    }

    // "Routing: last touched send (MIDI CC)" on an encoder moves the send
    // whose level the matrix moved last: the shown one.
    matrix.volumeControl()->onChange(-6, ui::Gesture::Set);
    gui::ActionControl binding;
    binding.action = "control.route.last";
    binding.control = {model::ControlKind::Cc, 1, 31, ""};
    binding.mode = model::ControlMode::Relative1;
    f.actions->controls().bind(binding);
    cc.midi.bytes[1] = 31;
    cc.midi.bytes[2] = 2;  // two steps up
    f.actions->control({cc});
    CHECK(route(verbSend).volumeDb == doctest::Approx(fader::db(fader::position(-6) + 2.0 / 128)));
    CHECK(route(drumsSend).volumeDb == doctest::Approx(-6.0));
}

TEST_CASE("routing matrix: a multi-out instrument's outputs get rows that send them (ADR-0021)") {
    Fixture f({FIXTURE_CLAP_PATH});
    if (!f.ok) return;
    const std::string drums = f.exec("track.add", R"({"name":"Drums"})")["track"].asString();
    const std::string kick =
        f.exec("track.add", R"({"type":"buss","name":"Kick"})")["track"].asString();
    f.exec("fx.add", R"({"track":")" + drums + R"(","plugin":"com.suppleaudio.test.multiout"})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    const auto& m = matrix.model();
    std::size_t main = m.sources.size();
    for (std::size_t i = 0; i < m.sources.size(); ++i)
        if (m.sources[i].id == drums && m.sources[i].pair == 1) main = i;
    REQUIRE(main + 2 < m.sources.size());
    // Under the strip's own row: its outputs' pairs, by their names.
    CHECK(m.sources[main + 1].id == drums);
    CHECK(m.sources[main + 1].pair == 3);
    CHECK(m.sources[main + 1].name == "3/4  Kick");
    CHECK(m.sources[main + 2].name == "5/6  Snare");
    std::size_t col = m.destinations.size();
    for (std::size_t i = 0; i < m.destinations.size(); ++i)
        if (m.destinations[i].id == kick) col = i;
    REQUIRE(col < m.destinations.size());
    // A double click on the Kick row sends 3/4 to the buss's main pair; its
    // cell alone shows it.
    REQUIRE(matrix.doubleClickCell(main + 1, col));
    CHECK(m.sendsAt(main + 1, col).size() == 1);
    CHECK(m.sendsAt(main, col).empty());
    CHECK(m.sendsAt(main + 2, col).empty());
    REQUIRE(f.app->head().routes.size() == 1);
    const model::Route& route = *f.app->head().routes.begin()->value;
    CHECK(route.channels.from == std::vector<int>{3, 4});
    CHECK(route.channels.to == std::vector<int>{1, 2});
    CHECK(panel.root().find("routing.send")->valueText().find("Drums 3/4  Kick") == 0);
    // An auxiliary pair's send leaves room for the main pair's (and a send drag's).
    const auto drumsId = *model::Id::parse(drums), kickId = *model::Id::parse(kick);
    CHECK_FALSE(model::sendsTo(f.app->head(), drumsId, kickId));
    REQUIRE(matrix.doubleClickCell(main, col));
    CHECK(m.sendsAt(main, col).size() == 1);
    CHECK(model::sendsTo(f.app->head(), drumsId, kickId));
    CHECK(f.app->head().routes.size() == 2);
    f.snapshot("routing-multiout.png", &panel.root());
}

TEST_CASE("mixer: grouped strips, a fader drag is one undo step, toggles, pan, Console 9, meters") {
    Fixture f;
    if (!f.ok) return;
    const std::string buss =
        f.exec("track.add", R"({"type":"buss","name":"Drums"})")["track"].asString();
    std::vector<gui::StripReading> readings;
    gui::MeterFeed feed([&] { return readings; });
    gui::Mixer mixer(*f.root, *f.actions, *f.dispatcher, *f.app, feed);
    // Swap the editor's dock for the mixer's in the fixture's window.
    auto* window = f.root->content();
    window->add(mixer.build());
    f.editor->close();
    mixer.setVisible(true);
    f.root->frame(false);
    const auto& strips = mixer.strips();
    REQUIRE(strips.size() == 3);
    CHECK(strips[0].type == "channel");  // channels, busses, masters
    const auto route =
        f.exec("route.add", R"({"from":")" + strips[0].track + R"(","to":")" + buss + R"("})")["id"]
            .asString();
    mixer.sync();
    auto* send = dynamic_cast<ui::Button*>(f.root->find("mixer.send." + route));
    REQUIRE(send);
    const auto originalSend = send->valueText();
    auto* originalFader = strips[0].fader;
    f.exec("route.set", R"({"route":")" + route + R"(","volume_db":-12})");
    mixer.sync();
    CHECK(send->valueText().find("-12.0") != std::string::npos);
    CHECK(strips[0].fader == originalFader);
    f.exec("history.undo", "{}");
    mixer.sync();
    CHECK(send->valueText() == originalSend);
    f.root->frame(false);
    // The masters stay pinned to the right edge; the others start at the left.
    const ui::Rect row = f.root->find("mixer.strips")->bounds();
    CHECK(static_cast<double>(strips[0].node->bounds().x) ==
          doctest::Approx(static_cast<double>(row.x)));
    CHECK(static_cast<double>(strips[2].node->bounds().right()) ==
          doctest::Approx(static_cast<double>(row.right() - 4)));
    CHECK(strips[2].node->bounds().x > strips[1].node->bounds().right() + 100);
    // "fx" on every strip: the plugin browser for an empty chain.
    auto* fxButton = f.root->find("mixer.fxadd." + strips[0].track);
    REQUIRE(fxButton);
    CHECK(fxButton->valueText() == "fx");
    // The colour stripe has its own row between the name and the FX slots.
    f.exec("track.set", R"({"track":")" + strips[0].track + R"(","color":"teal"})");
    mixer.sync();
    f.root->frame(false);
    auto* stripe = f.root->find("mixer.color." + strips[0].track);
    REQUIRE(stripe);
    CHECK(stripe->bounds().y >= f.root->find("mixer.name." + strips[0].track)->bounds().bottom());
    CHECK(stripe->bounds().bottom() <= f.root->find("mixer.fx." + strips[0].track)->bounds().y);
    // A right-click on the strip opens the track menu.
    const ui::Rect fxNow = f.root->find("mixer.fxadd." + strips[0].track)->bounds();  // rebuilt
    ui::PointerEvent rc;
    rc.type = ui::PointerEvent::Type::Down;
    rc.button = 3;
    rc.pos = {fxNow.x + 4, fxNow.y + 4};
    f.root->pointer(rc);
    rc.type = ui::PointerEvent::Type::Up;
    f.root->pointer(rc);
    f.root->frame(false);
    CHECK(f.root->find("track.menu." + strips[0].track));
    f.root->key(SDLK_ESCAPE);
    CHECK(strips[1].track == buss);
    CHECK(strips[2].track == "tr_0");
    CHECK(strips[0].arm != nullptr);
    CHECK(strips[1].arm == nullptr);  // only channels arm
    const std::string keys = strips[0].track;
    auto track = [&](const std::string& id) {
        return f.app->head().tracks.find(*model::Id::parse(id));
    };

    // A fader drag: many moves, one undo step; a second drag is another.
    auto steps = [&] { return f.exec("history.list", "{}")["versions"].size(); };
    const auto s0 = steps();
    const ui::Rect fb = strips[0].fader->bounds();
    REQUIRE(fb.h > 60);
    const ui::Point cap{fb.x + fb.w / 2,
                        fb.bottom() - 8 - static_cast<float>(ui::faderPosition(0)) * (fb.h - 16)};
    f.pointer(ui::PointerEvent::Type::Down, cap);
    for (int i = 1; i <= 10; ++i)
        f.pointer(ui::PointerEvent::Type::Move, {cap.x, cap.y + static_cast<float>(i) * 3});
    f.pointer(ui::PointerEvent::Type::Up, {cap.x, cap.y + 30});
    mixer.sync();
    CHECK(track(keys)->volumeDb < -3);
    CHECK(steps() == s0 + 1);
    const double dragged = track(keys)->volumeDb;
    const ui::Point cap2{cap.x, cap.y + 30};
    f.pointer(ui::PointerEvent::Type::Down, cap2);
    for (int i = 1; i <= 5; ++i)
        f.pointer(ui::PointerEvent::Type::Move, {cap2.x, cap2.y + static_cast<float>(i) * 3});
    f.pointer(ui::PointerEvent::Type::Up, {cap2.x, cap2.y + 15});
    CHECK(steps() == s0 + 2);
    f.exec("history.undo", "{}");
    CHECK(track(keys)->volumeDb == doctest::Approx(dragged));  // only the second drag undone
    mixer.sync();
    // Double-click: unity, its own step.
    f.pointer(ui::PointerEvent::Type::Down, cap2, 2);
    f.pointer(ui::PointerEvent::Type::Up, cap2);
    CHECK(track(keys)->volumeDb == 0.0);
    // Toggles.
    REQUIRE(f.root->click("mixer.mute." + keys));
    CHECK(track(keys)->mute);
    REQUIRE(f.root->click("mixer.solo." + buss));
    CHECK(track(buss)->solo == model::Solo::On);
    REQUIRE(f.root->click("mixer.arm." + keys));
    CHECK(track(keys)->record.armed);
    CHECK(track(keys)->record.monitor == model::Monitor::On);  // the default
    REQUIRE(f.root->click("mixer.monitor." + keys));
    CHECK(track(keys)->record.monitor == model::Monitor::Auto);
    // Pan drag right.
    const ui::Rect pb = strips[0].pan->bounds();
    f.drag({pb.x + pb.w / 2, pb.y + 8}, {pb.right() - 2, pb.y + 8});
    CHECK(track(keys)->pan > 50);
    // Console 9 is a project toggle.
    const bool c9 = f.app->head().project->console9;
    REQUIRE(f.root->click("mixer.console9"));
    CHECK(f.app->head().project->console9 == !c9);
    // Changes from elsewhere reach the strips; a new track adds a strip.
    f.exec("track.set", R"({"track":")" + keys + R"(","volume_db":-12})");
    mixer.sync();
    CHECK(strips[0].fader->value() == doctest::Approx(-12.0));
    f.exec("track.add", R"({"name":"Bass"})");
    mixer.sync();
    CHECK(mixer.strips().size() == 4);
    // Meters: readings land on the strip's meter.
    readings = {{keys, {0.5f, 0.25f}, {0.3f, 0.1f}}};
    feed.poll();
    CHECK(mixer.strips()[0].meter->shownPeakDb(0) ==
          doctest::Approx(20 * std::log10(0.5)).epsilon(0.02));
    f.snapshot("mixer.png");
}

TEST_CASE(
    "history dock: versions, jump back, redo, branches, the command log with agents attributed") {
    Fixture f;
    if (!f.ok) return;
    gui::HistoryPanel history(*f.root, *f.actions, *f.dispatcher, *f.app);
    f.editor->close();
    {  // the app's layout: the dock is a full-height column on the right
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Row;
        frame->emplace<ui::Node>("arrange").layout.width = ui::SizeRule::fraction();
        frame->add(history.build());
        f.root->setContent(std::move(frame));
    }
    history.setVisible(true);
    const cmd::Origin agent{cmd::Origin::Type::Agent, "panel", "model-x"};
    f.exec("track.add", R"({"name":"A"})");
    f.exec("track.add", R"({"name":"B"})");
    REQUIRE(f.dispatcher->execute({"track.add", json::parse(R"({"name":"C"})").value(), agent}));
    history.sync();
    f.root->frame(false);
    REQUIRE(history.versions().size() >= 4);
    CHECK(history.versions().front().origin.rfind("agent:", 0) == 0);  // the head, by the agent
    // The log: newest last, the agent's command attributed.
    REQUIRE(!history.entries().empty());
    CHECK(history.entries().back().origin == "agent:panel/model-x");
    CHECK(history.entries().back().command == "track.add");
    f.snapshot("history.png");
    // Jump back two versions, then redo one.
    const std::size_t tracks = f.app->head().tracks.size();
    REQUIRE(history.jumpTo(2));
    CHECK(f.app->head().tracks.size() == tracks - 2);
    REQUIRE(f.root->click("history.redo"));
    CHECK(f.app->head().tracks.size() == tracks - 1);
    // Branches: create, edit there, back to main.
    REQUIRE(history.createBranch("idea"));
    history.sync();
    CHECK(std::find(history.branches().begin(), history.branches().end(), "idea") !=
          history.branches().end());
    f.exec("track.add", R"({"name":"Idea"})");
    REQUIRE(history.switchBranch("main"));
    CHECK(f.app->head().tracks.size() == tracks - 1);
    history.sync();
    CHECK(history.entries().back().command == "branch.switch");
}

namespace {

void showEventList(Fixture& f) {
    REQUIRE(f.editor->open(f.item));
    f.frame();
    REQUIRE(f.root->click("midi.list.toggle"));
    f.frame();
    REQUIRE(f.root->find("midi.list")->visible);
}

ui::Point eventCell(Fixture& f, std::size_t row, gui::MidiEventList::Column column) {
    const auto* list = f.editor->eventList()->list();
    const auto b = list->bounds();
    return {b.x + (static_cast<float>(column) + 0.5f) * (b.w - ui::ScrollBar::kThickness) / 6,
            b.y + (static_cast<float>(row) + 0.5f) * list->rowHeight - list->scroll()};
}

void editEventCell(Fixture& f, std::size_t row, gui::MidiEventList::Column column,
                   const std::string& text, bool commit = true) {
    const auto p = eventCell(f, row, column);
    f.pointer(ui::PointerEvent::Type::Down, p, 2);
    f.pointer(ui::PointerEvent::Type::Up, p, 2);
    REQUIRE(f.root->focused() == f.root->find("midi.list.edit"));
    f.root->text(text);  // the inline field selects its old value
    f.root->key(commit ? SDLK_RETURN : SDLK_ESCAPE);
    f.frame();
}

std::size_t eventSteps(Fixture& f) { return f.exec("history.list", "{}")["versions"].size(); }

json::Value eventCcs(Fixture& f) {
    return f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
}

}  // namespace

TEST_CASE("MIDI event list: toggle, ordered rows, formatting and shared selection") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":64,"channel":2,"points":[{"beat":0,"value":127}]},
      {"op":"cc.set","number":1,"points":[{"beat":0.5,"value":32}]}]})");
    showEventList(f);
    auto* events = f.editor->eventList();
    REQUIRE(events->list()->count() == 6);
    using Text = gui::MidiEventList::RowText;
    CHECK(events->rowText(0) == Text{"2.1.0", "Note", "1.0", "C4", "96", "1"});
    CHECK(events->rowText(1) == Text{"2.1.0", "CC 64", "", "64", "127", "2"});
    CHECK(events->rowText(2) == Text{"2.1.480", "CC 1", "", "1", "32", "1"});
    CHECK(events->rowText(3)[0] == "2.2.0");
    CHECK(events->rowText(3)[3] == "E4");
    CHECK_FALSE(f.root->find("midi.left")->visible);
    CHECK(f.root->find("midi.notation")->visible);
    f.pointer(ui::PointerEvent::Type::Down, eventCell(f, 3, gui::MidiEventList::Column::Pitch));
    f.pointer(ui::PointerEvent::Type::Up, eventCell(f, 3, gui::MidiEventList::Column::Pitch));
    CHECK(f.editor->state().selected == std::set<std::size_t>{1});
    REQUIRE(f.root->click("midi.list.toggle"));
    f.frame();
    CHECK_FALSE(f.root->find("midi.list")->visible);
    CHECK(f.root->find("midi.left")->visible);
    CHECK(f.editor->roll()->visible);
    CHECK(f.root->find("midi.notation")->visible);
    CHECK(f.editor->state().selected == std::set<std::size_t>{1});
    f.editor->state().selected = {2, 3};
    REQUIRE(f.root->click("midi.list.toggle"));
    f.frame();
    CHECK(events->list()->selected() == 4);
    f.snapshot("midi-event-list.png");
}

TEST_CASE(
    "MIDI event list: text edits each commit one version, invalid input and Escape commit none") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    showEventList(f);
    using Col = gui::MidiEventList::Column;
    const auto initial = eventSteps(f);
    editEventCell(f, 0, Col::Value, "110");
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 110);
    CHECK(eventSteps(f) == initial + 1);
    editEventCell(f, 0, Col::Position, "2.1.480");
    CHECK(f.notes().asArray()[0]["start"].asDouble() == doctest::Approx(0.5));
    CHECK(eventSteps(f) == initial + 2);
    editEventCell(f, 0, Col::Pitch, "C#4");
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 61);
    CHECK(eventSteps(f) == initial + 3);
    editEventCell(f, 0, Col::Pitch, "62");
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 62);
    CHECK(eventSteps(f) == initial + 4);
    editEventCell(f, 0, Col::Length, "1.240");
    CHECK(f.notes().asArray()[0]["length"].asDouble() == doctest::Approx(1.25));
    CHECK(eventSteps(f) == initial + 5);
    for (const auto& bad : {std::pair{Col::Value, "128"},
                            {Col::Value, "-1"},
                            {Col::Position, "2.5.0"},
                            {Col::Position, "2.1.960"},
                            {Col::Position, "garbage"},
                            {Col::Length, "0.0"},
                            {Col::Pitch, "C#"},
                            {Col::Pitch, "9999999999999999"}}) {
        editEventCell(f, 0, bad.first, bad.second);
        CHECK(eventSteps(f) == initial + 5);
        CHECK_FALSE(f.root->find("midi.list.status")->valueText().empty());
        f.root->key(SDLK_ESCAPE);
        f.frame();
    }
    editEventCell(f, 0, Col::Value, "64", false);
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 110);
    CHECK(eventSteps(f) == initial + 5);
    CHECK_FALSE(f.root->find("midi.list.edit")->visible);
    f.exec("history.undo", "{}");
    f.frame();
    CHECK(f.editor->eventList()->rowText(0)[2] == "1.0");
}

TEST_CASE("MIDI event list: multi-row Delete and quarter-note addition") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":1,"channel":2,"points":[{"beat":0.5,"value":32},{"beat":0.5010416667,"value":48}]},
      {"op":"cc.set","number":1,"points":[{"beat":0.5,"value":64}]}]})");
    showEventList(f);
    const auto initial = eventSteps(f);
    using Col = gui::MidiEventList::Column;
    auto p = eventCell(f, 0, Col::Position);
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Up, p);
    p = eventCell(f, 2, Col::Position);  // channel 2 CC
    f.pointer(ui::PointerEvent::Type::Down, p, 1, ui::kCtrl);
    f.pointer(ui::PointerEvent::Type::Up, p, 1, ui::kCtrl);
    f.root->key(SDLK_DELETE);
    f.frame();
    CHECK(f.notes().size() == 3);
    REQUIRE(eventCcs(f).size() == 2);
    CHECK(eventCcs(f).asArray()[0]["value"].asInt() == 64);
    CHECK(eventCcs(f).asArray()[1]["value"].asInt() == 48);  // adjacent tick survived
    CHECK(eventSteps(f) == initial + 1);
    REQUIRE(f.root->click("midi.list.add"));
    f.frame();
    CHECK(f.notes().size() == 4);
    CHECK(f.notes().asArray()[0]["start"].asDouble() == 0);
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 60);
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 96);
    CHECK(f.notes().asArray()[0]["length"].asDouble() == 1);
    CHECK(f.editor->state().selected == std::set<std::size_t>{0});
    CHECK(eventSteps(f) == initial + 2);
    // Add at a selected row, then delete all rows with a Shift selection.
    p = eventCell(f, 4, Col::Position);
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Up, p);
    REQUIRE(f.root->click("midi.list.add"));
    f.frame();
    CHECK(f.notes().size() == 5);
    CHECK(f.notes().asArray()[2]["pitch"].asInt() == 60);
    CHECK(f.notes().asArray()[2]["start"].asDouble() == 2);
    CHECK(eventSteps(f) == initial + 3);
    f.root->key(SDLK_HOME);
    f.root->key(SDLK_END, ui::kShift);
    f.root->key(SDLK_DELETE);
    f.frame();
    CHECK(f.notes().size() == 0);
    CHECK(eventCcs(f).size() == 0);
    CHECK(eventSteps(f) == initial + 4);
}

TEST_CASE("MIDI event list: CC text edits preserve controller channels and neighbors") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":11,"channel":3,"points":[{"beat":0.5,"value":32},{"beat":0.5010416667,"value":48}]}]})");
    showEventList(f);
    using Col = gui::MidiEventList::Column;
    const auto initial = eventSteps(f);
    editEventCell(f, 1, Col::Value, "0");
    CHECK(eventCcs(f).asArray()[0]["value"].asInt() == 0);
    CHECK(eventCcs(f).asArray()[0]["channel"].asInt() == 3);
    CHECK(eventCcs(f).asArray()[1]["value"].asInt() == 48);
    CHECK(eventSteps(f) == initial + 1);
    editEventCell(f, 1, Col::Position, "2.2.480");
    CHECK(eventCcs(f).asArray()[1]["beat"].asDouble() == doctest::Approx(1.5));
    CHECK(eventCcs(f).asArray()[0]["value"].asInt() == 48);
    CHECK(eventSteps(f) == initial + 2);
    editEventCell(f, 3, Col::Pitch, "7");
    CHECK(eventCcs(f).asArray()[1]["number"].asInt() == 7);
    CHECK(eventCcs(f).asArray()[1]["channel"].asInt() == 3);
    CHECK(eventSteps(f) == initial + 3);
}

TEST_CASE("MIDI event list: project meter, source offset and playrate, virtualization") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("timesig.set", R"({"bar":2,"meter":"3/8"})");
    auto take = f.app->head().items.find(*model::Id::parse(f.item))->activeTake;
    REQUIRE(take);
    f.exec("take.set", R"({"take":")" + take->str() + R"(","start_offset":0.5,"playrate":2})");
    showEventList(f);
    CHECK(f.editor->eventList()->rowText(1)[0] == "2.1.480");
    CHECK(f.editor->eventList()->rowText(1)[2] == "0.480");
    editEventCell(f, 1, gui::MidiEventList::Column::Position, "3.1.240");
    CHECK(f.notes().asArray()[3]["start"].asDouble() == doctest::Approx(1.625));
    CHECK(f.editor->state().selected == std::set<std::size_t>{3});
    auto ops = json::Value::array();
    for (int i = 0; i < 2000; ++i) {
        auto op = json::Value::object();
        op["op"] = "add";
        op["pitch"] = i % 128;
        op["start"] = 2.0 + i / 8.0;
        op["length"] = 0.25;
        ops.push_back(std::move(op));
    }
    REQUIRE(f.editor->edit(std::move(ops)));
    f.frame();
    const auto* list = f.editor->eventList()->list();
    CHECK(list->count() == 2004);
    CHECK(list->rowsPainted() < 20);
    f.root->key(SDLK_END);
    f.frame();
    CHECK(list->selected() == 2003);
    CHECK(list->scroll() > 1000);
    CHECK(list->rowsPainted() < 20);
}

TEST_CASE("MIDI event list: stale inline edits and closing cannot target hidden rows") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    showEventList(f);
    const auto p = eventCell(f, 0, gui::MidiEventList::Column::Value);
    f.pointer(ui::PointerEvent::Type::Down, p, 2);
    f.pointer(ui::PointerEvent::Type::Up, p, 2);
    f.root->text("120");
    const auto initial = eventSteps(f);
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"move","notes":[0],"beats":3.5}]})");
    // Another client edited before the next UI sync: refuse the stale index.
    f.root->key(SDLK_RETURN);
    CHECK(eventSteps(f) == initial + 1);
    CHECK(f.notes().asArray()[3]["velocity"].asInt() == 96);
    CHECK_FALSE(f.root->find("midi.list.edit")->visible);
    CHECK_FALSE(f.root->find("midi.list.status")->valueText().empty());
    f.frame();
    f.root->focus(f.editor->eventList()->list());
    f.root->key(SDLK_HOME);
    f.editor->close();
    CHECK(f.root->focused() == nullptr);
    CHECK(f.editor->eventList()->list()->count() == 0);
    f.root->key(SDLK_DELETE);
    CHECK(eventSteps(f) == initial + 1);
    REQUIRE(f.editor->open(f.item));
    f.frame();
    CHECK(f.editor->eventList()->list()->count() == 4);
    CHECK(f.editor->state().selected.empty());
}

TEST_CASE("MIDI external edits clear identity and reject a gesture before the next sync") {
    Fixture f;
    REQUIRE(f.ok);
    REQUIRE(f.editor->open(f.item));
    f.frame();
    f.editor->state().selected = {0};
    const auto version = f.editor->beginGesture();
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"add","pitch":58,"start":0,"length":1}]})");
    const auto external = f.app->head().version;
    json::Value ops = json::parse(R"([{"op":"remove","notes":[0]}])").value();
    auto result = f.editor->edit(std::move(ops), version);
    CHECK_FALSE(result);
    CHECK(f.app->head().version == external);
    CHECK(f.editor->state().selected.empty());
    CHECK(f.notes().size() == 5);
    f.editor->state().selected = {1};
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"velocity","notes":[1],"velocity":50}]})");
    f.frame();
    CHECK(f.editor->state().selected.empty());
}

TEST_CASE("MIDI notation follows project meters at the item's placement") {
    Fixture f;
    REQUIRE(f.ok);
    f.exec("timesig.set", R"({"bar":2,"meter":"3/4"})");
    f.exec("timesig.set", R"({"bar":3,"meter":"5/8"})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    CHECK(f.editor->notation()->text().find("meter 4/4") == std::string::npos);
    // Header disagreement is caught by live compile before midi.write.
    auto* text = f.editor->notation();
    f.root->focus(text);
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("meter 4/4; voice v; 1: C4:w");
    const auto version = f.app->head().version;
    text->onSubmit(text->text());
    CHECK(f.app->head().version == version);
    CHECK_FALSE(text->marks().empty());
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("meter 3/4; meter 5/8 at 2; voice v; 1: C4:h. | 2: D4:q E4:q F4:e");
    text->onSubmit(text->text());
    CHECK(text->marks().empty());
    CHECK(f.notes().size() == 4);
}

TEST_CASE("routing matrix clamps scrolling when channel rows are deleted") {
    Fixture f;
    REQUIRE(f.ok);
    std::vector<std::string> tracks;
    for (int i = 0; i < 30; ++i)
        tracks.push_back(
            f.exec("track.add", R"({"type":"buss","name":"Bus"})")["track"].asString());
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 860,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    const auto bounds = matrix.grid()->bounds();
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = {bounds.x + 201, bounds.y + 123};
    wheel.wheelY = -100;
    wheel.wheelX = -100;
    matrix.grid()->onPointer(wheel);
    for (const auto& track : tracks) f.exec("track.remove", R"({"track":")" + track + R"("})");
    matrix.sync();
    panel.root().frame(false);
    // Row zero and column zero return to their ordinary screen positions.
    ui::PointerEvent hover;
    hover.type = ui::PointerEvent::Type::Move;
    hover.pos = {bounds.x + 201, bounds.y + 123};
    matrix.grid()->onPointer(hover);
    auto* status = panel.root().find("routing.status");
    REQUIRE(status);
    CHECK(status->valueText().find(matrix.model().blocked(0, 0)) != std::string::npos);
    CHECK(status->valueText().find("not allowed") != std::string::npos);
}

TEST_CASE("audit3: empty event-list double-click never edits the old selection") {
    Fixture f;
    REQUIRE(f.ok);
    showEventList(f);
    auto* list = f.root->find("midi.list.rows");
    REQUIRE(list);
    const auto b = list->bounds();
    f.pointer(ui::PointerEvent::Type::Down, {b.x + 30, b.y + 8});
    f.pointer(ui::PointerEvent::Type::Up, {b.x + 30, b.y + 8});
    const auto count = f.editor->eventList()->list()->count();
    const auto y = b.y + static_cast<float>(count) * 24 + 10;
    REQUIRE(y < b.bottom());
    f.pointer(ui::PointerEvent::Type::Down, {b.x + 30, y}, 2);
    CHECK_FALSE(f.root->find("midi.list.edit")->visible);
    CHECK(f.root->focused() != f.root->find("midi.list.edit"));
    f.pointer(ui::PointerEvent::Type::Up, {b.x + 30, y});
    f.pointer(ui::PointerEvent::Type::Down, {b.x + 30, b.y + 26 + 8}, 2);
    CHECK(f.root->find("midi.list.edit")->visible);
    CHECK(f.root->find("midi.list.edit")->valueText() == f.editor->eventList()->rowText(1)[0]);
}

TEST_CASE("audit3: fast take snapped drag reaches its exact source tick") {
    Fixture f;
    REQUIRE(f.ok);
    auto id = *model::Id::parse(f.item);
    auto take = *f.app->head().items.find(id)->activeTake;
    f.exec("take.set", R"({"take":")" + take.str() + R"(","playrate":16})");
    f.exec("midi.edit",
           R"({"item":")" + f.item +
               R"(","ops":[{"op":"move","notes":[0],"beats":0.00006510416666666667}]})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    f.editor->state().pixelsPerBeat = 1200;
    f.frame();
    auto start = f.at(f.editor->state().notes[0].start + .03, 60);
    f.drag(start, {start.x + 300, start.y});
    auto source = *f.app->head().takes.find(take)->source;
    const auto& seq = f.app->head().midi.find(source)->sequence;
    for (const auto& n : seq.notes)
        if (n.pitch == 60) CHECK(n.start == 3840);  // exact quarter-beat grid is 3840 ticks
}

TEST_CASE("audit3: MIDI and event-list snapshots refresh across equal-version projects") {
    Fixture f;
    REQUIRE(f.ok);
    auto old = f.dir / "old.json", next = f.dir / "next.json";
    REQUIRE(f.app->save(old));
    REQUIRE(f.app->newProject({}));
    auto t = f.exec("track.add", R"({"name":"New keys"})");
    auto it =
        f.exec("item.add", R"({"track":")" + t["track"].asString() +
                               R"(","position":4,"notation":"voice v; 1: D4:q F4:q A4:q D5:q"})");
    REQUIRE(it["item"].asString() == f.item);
    REQUIRE(f.app->save(next));
    REQUIRE(f.app->open(old));
    showEventList(f);
    auto version = f.app->head().version;
    REQUIRE(f.editor->state().notes[0].pitch == 60);
    REQUIRE(f.app->open(next));
    REQUIRE(f.app->head().version == version);
    f.frame();
    CHECK(f.editor->state().notes[0].pitch == 62);
    CHECK(f.editor->eventList()->rowText(0)[3] == "D4");
    auto gesture = f.editor->beginGesture();
    REQUIRE(f.app->open(old));
    auto ops = json::parse(R"([{"op":"velocity","notes":[0],"velocity":37}])").value();
    CHECK_FALSE(f.editor->edit(ops, gesture));
    CHECK(f.notes().asArray()[0]["velocity"].asInt() != 37);
}

TEST_CASE("a notation draft does not overwrite MIDI edited elsewhere unless Ctrl+Enter asks to") {
    Fixture f;
    REQUIRE(f.ok);
    REQUIRE(f.editor->open(f.item));
    f.frame();
    f.root->focus(f.editor->notation());
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("voice v; 1: D4:q E4:q G4:q C5:q");
    // Another client edits the item while the draft is open.
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"velocity","notes":[0],"velocity":37}]})");
    f.frame();
    f.editor->close();  // closing does not push the stale draft over that edit
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 37);
    // With Ctrl+Enter, the draft is applied deliberately.
    REQUIRE(f.editor->open(f.item));
    f.frame();
    f.root->focus(f.editor->notation());
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("voice v; 1: D4:q E4:q G4:q C5:q");
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"velocity","notes":[0],"velocity":38}]})");
    f.frame();
    f.root->key(SDLK_RETURN, ui::kCtrl);
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 62);  // D4: the draft won, as asked
}

TEST_CASE("routing matrix: MIDI mode exposes channels and note modules with MIDI controls") {
    Fixture f;
    if (!f.ok) return;
    const auto channel = f.exec("track.add", R"({"name":"MIDI target"})")["track"].asString();
    f.exec("track.add", R"({"name":"Hidden buss","type":"buss"})");
    const auto rack = f.exec("rack.add", R"({"track":")" + channel + R"("})")["rack"].asString();
    const auto notes = f.exec("module.add", R"({"rack":")" + rack +
                                                R"(","type":"notes","source":"none"})")["module"]
                           .asString();
    const auto output =
        f.exec("module.add", R"({"rack":")" + rack + R"(","type":"notes_out"})")["module"]
            .asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    REQUIRE(panel.root().click("routing.mode.midi"));
    panel.root().frame(false);
    REQUIRE(matrix.midiMode());
    // The switch shows the mode: the MIDI tab lit, the Audio tab not.
    const auto tab = [&](const char* id) {
        return static_cast<const ui::Toggle*>(panel.root().find(id))->on();
    };
    CHECK(tab("routing.mode.midi"));
    CHECK_FALSE(tab("routing.mode.audio"));
    REQUIRE(panel.root().click("routing.mode.midi"));  // the lit tab stays lit
    CHECK(tab("routing.mode.midi"));
    CHECK(matrix.midiMode());
    const auto& model = matrix.model();
    bool hasOutput = false, hasNotes = false;
    std::size_t row = 0, col = 0;
    for (std::size_t i = 0; i < model.sources.size(); ++i) {
        const auto& s = model.sources[i];
        CHECK(s.type != "buss");
        CHECK(s.type != "master");
        if (s.id == output) {
            hasOutput = true;
            CHECK(s.name.ends_with("Note Out"));
            row = i;
        }
    }
    for (std::size_t i = 0; i < model.destinations.size(); ++i) {
        const auto& d = model.destinations[i];
        CHECK(d.type != "buss");
        CHECK(d.type != "master");
        if (d.id == notes) {
            hasNotes = true;
            CHECK(d.name.ends_with("Notes"));
        }
        if (d.id == channel) col = i;
    }
    REQUIRE(hasOutput);
    REQUIRE(hasNotes);
    REQUIRE(matrix.doubleClickCell(row, col));
    panel.root().frame(false);
    REQUIRE(model.sendsAt(row, col).size() == 1);
    CHECK_FALSE(matrix.volumeControl()->visible);
    CHECK(panel.root().find("routing.midi.from")->visible);
    CHECK(panel.root().find("routing.midi.to")->visible);
    REQUIRE(panel.root().click("routing.tap.prefx"));
    CHECK(model.sendsAt(row, col).front()->tap == "pre_fx");
    f.snapshot("routing-midi.png", &panel.root());
    REQUIRE(panel.root().click("routing.mute"));
    CHECK(model.sendsAt(row, col).front()->mute);
    REQUIRE(panel.root().click("routing.midi.from"));
    CHECK(panel.root().find("routing.midi.channels") != nullptr);
    panel.root().closePopup();
    // A double-click on a send selects it, in both modes; Remove removes it.
    REQUIRE(matrix.doubleClickCell(row, col));
    CHECK(model.sendsAt(row, col).size() == 1);
    REQUIRE(panel.root().click("routing.remove"));
    CHECK(model.sendsAt(row, col).empty());
    REQUIRE(panel.root().click("routing.mode.audio"));
    CHECK_FALSE(matrix.midiMode());
    CHECK(tab("routing.mode.audio"));
    CHECK_FALSE(tab("routing.mode.midi"));
    f.snapshot("routing-audio.png", &panel.root());
    CHECK(matrix.volumeControl()->visible);
    matrix.setMidiMode(true);
    matrix.close();
    matrix.open();
    CHECK(matrix.midiMode());
}

TEST_CASE("routing matrix: hardware ports share input assignments and output menus") {
    Fixture f;
    if (!f.ok) return;
    auto input = json::Value::object();
    input["enabled"] = true;
    auto output = input;
    output["clock"] = true;
    REQUIRE(f.app->setMidiPreferences({{"Keyboard", input}}, {{"Synth", output}}, false));
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    panel.root().frame(false);
    REQUIRE(panel.root().click("routing.mode.hardware"));
    panel.root().frame(false);
    REQUIRE(matrix.hardwareMidiMode());
    CHECK(matrix.midiMode());
    const auto& model = matrix.model();
    REQUIRE(model.sources.front().type == "midi_input");
    std::size_t row = 0, col = 0, out = 0;
    for (std::size_t i = 0; i < model.sources.size(); ++i)
        if (model.sources[i].type == "channel") row = i;
    for (std::size_t i = 0; i < model.destinations.size(); ++i) {
        if (model.destinations[i].kind == "track") col = i;
        if (model.destinations[i].kind == "midi_output") out = i;
    }
    REQUIRE(matrix.doubleClickCell(0, col));
    CHECK(model.sendsAt(0, col).size() == 1);
    REQUIRE(matrix.clickCell(0, col));
    REQUIRE(matrix.removeSelected());
    CHECK(model.sendsAt(0, col).empty());
    REQUIRE(matrix.doubleClickCell(row, out));
    REQUIRE(model.sendsAt(row, out).size() == 1);
    CHECK_FALSE(matrix.volumeControl()->visible);
    // A stored clock record with no device running sends nothing: no mark.
    CHECK_FALSE(model.destinations[out].clock);
    CHECK_FALSE(static_cast<ui::Toggle*>(panel.root().find("routing.mode.midi"))->on());
    CHECK(static_cast<ui::Toggle*>(panel.root().find("routing.mode.hardware"))->on());
    // Drive the header's actual right-click path, then inspect and invoke its menu.
    const auto b = matrix.grid()->bounds();
    ui::PointerEvent click;
    click.type = ui::PointerEvent::Type::Down;
    click.button = 3;
    click.pos = {b.x + 190 + float(out) * 22 + 11, b.y + 60};
    REQUIRE(matrix.grid()->onPointer(click));
    panel.root().frame(false);
    auto* menu = panel.root().find("routing.port.menu");
    REQUIRE(menu);
    const auto items = ui::menuItems(*menu);
    auto item = [&](const std::string& label) -> const ui::MenuItem& {
        for (const auto& i : items)
            if (i.label == label) return i;
        FAIL("no menu item " << label);
        return items.front();
    };
    REQUIRE(items.size() == 10);  // Enabled | five clock items | Reset, Timing offset
    CHECK(item("Enabled").checked);
    CHECK(item("Send clock").checked);
    CHECK(item("Reset notes on stop").checked);
    CHECK(items.back().label == "Timing offset…");
    auto timing = item("Timing offset…").run;
    panel.root().closePopup();
    timing();
    panel.root().frame(false);
    auto* field = static_cast<ui::TextField*>(panel.root().find("routing.port.offset.value"));
    REQUIRE(field);
    REQUIRE(panel.root().find("routing.port.offset.lead"));
    CHECK(panel.root().find("routing.port.offset.lead")->valueText().find("all audio") !=
          std::string::npos);
    field->setText("-3.5");
    REQUIRE(panel.root().click("routing.port.offset.apply"));
    CHECK(engine::midiPortSettings(f.app->midiOutputsPreferences(), "Synth").offsetMs == -3.5);
    auto settings = matrix.midiPortMenu(true, "Synth");
    REQUIRE(settings[0].label == "Enabled");
    settings[0].run();
    CHECK_FALSE(engine::midiPortSettings(f.app->midiOutputsPreferences(), "Synth").enabled);
    f.snapshot("routing-hardware-midi.png", &panel.root());
    matrix.close();
    matrix.open();
    CHECK(matrix.hardwareMidiMode());
    panel.root().frame(false);
    REQUIRE(panel.root().click("routing.mode.midi"));
    CHECK_FALSE(matrix.hardwareMidiMode());
}

TEST_CASE("routing matrix: wildcard input removal is atomic and replacements are explicit") {
    Fixture f;
    if (!f.ok) return;
    auto record = json::Value::object();
    REQUIRE(f.app->setMidiPreferences({{"Keyboard", record}, {"Pads", record}}, {}, false));
    const auto channel = f.exec("track.add", R"({"name":"Input target"})")["track"].asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_midi":"*"})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareMidiMode();
    matrix.open();
    panel.root().frame(false);
    const auto& model = matrix.model();
    std::size_t col = 0;
    for (std::size_t i = 0; i < model.destinations.size(); ++i)
        if (model.destinations[i].id == channel) col = i;
    REQUIRE(model.sendsAt(0, col).size() == 1);
    CHECK(model.sendsAt(0, col)[0]->allInputs);
    REQUIRE(matrix.clickCell(0, col));
    CHECK_FALSE(matrix.removeSelected());
    CHECK(matrix.status().find("All inputs (*)") != std::string::npos);
    CHECK(model.sendsAt(0, col).size() == 1);
    CHECK(model.sendsAt(1, col).size() == 1);
    matrix.clickColumn(col);
    REQUIRE(matrix.removeSelected());
    CHECK(model.sendsAt(0, col).empty());
    REQUIRE(matrix.doubleClickCell(0, col));
    REQUIRE(matrix.doubleClickCell(1, col));
    CHECK(matrix.status().find("replaces Keyboard") != std::string::npos);
    f.exec("track.set", R"({"track":")" + channel + R"(","record_midi":"*"})");
    matrix.sync();
    f.snapshot("routing-hardware-all-inputs.png", &panel.root());
}

TEST_CASE("routing matrix: output offset previews the prepared shared audio lead") {
    Fixture f;
    if (!f.ok) return;
    engine::DummyBackend backend({.midiOutputPorts = {"Synth"}});
    struct Stop {
        engine::EngineApp& app;
        ~Stop() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .midiOutputs = true},
                               {.anticipative = false, .workers = 0}));
    const auto source = f.exec("track.add", R"({"name":"Hardware notes"})")["track"].asString();
    f.exec("route.add", R"({"from":")" + source + R"(","to_midi":"Synth","midi":{}})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareMidiMode();
    matrix.open();
    panel.root().frame(false);
    auto menu = matrix.midiPortMenu(true, "Synth");
    REQUIRE(menu.back().label == "Timing offset…");
    menu.back().run();
    panel.root().frame(false);
    auto* field = static_cast<ui::TextField*>(panel.root().find("routing.port.offset.value"));
    REQUIRE(field);
    panel.root().focus(field);
    field->selectAll();
    panel.root().text("-20");
    auto* lead = panel.root().find("routing.port.offset.lead");
    REQUIRE(lead);
    CHECK(lead->valueText() == "adds 20.0 ms latency to all audio");
    f.snapshot("routing-hardware-offset-lead.png", &panel.root());
}

TEST_CASE("routing matrix: an output header's menu sets clock output, its mark shows it sends") {
    Fixture f;
    if (!f.ok) return;
    engine::DummyBackend backend({.midiOutputPorts = {"Synth", "Drums"}});
    struct Stop {
        engine::EngineApp& app;
        ~Stop() { app.stopEngine(); }
    } stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .midiOutputs = true},
                               {.anticipative = false, .workers = 0}));
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareMidiMode();
    matrix.open();
    panel.root().frame(false);
    auto column = [&](const std::string& name) {
        for (const auto& d : matrix.model().destinations)
            if (d.kind == "midi_output" && d.name == name) return d;
        FAIL("no column " << name);
        return matrix.model().destinations.front();
    };
    auto run = [&](const std::string& port, const std::string& label) {
        for (auto& item : matrix.midiPortMenu(true, port))
            if (item.label == label) {
                REQUIRE(item.enabled);
                item.run();
                panel.root().frame(false);
                return;
            }
        FAIL("no menu item " << label);
    };
    auto menuItem = [&](const std::string& port, const std::string& label) {
        for (auto& item : matrix.midiPortMenu(true, port))
            if (item.label == label) return item;
        FAIL("no menu item " << label);
        return ui::MenuItem{};
    };
    CHECK_FALSE(column("Synth").clock);
    // Clock while stopped needs the clock.
    CHECK_FALSE(menuItem("Synth", "Clock while stopped").enabled);
    for (const char* label : {"Send clock", "Send transport", "Song Position Pointer", "MTC"})
        run("Synth", label);
    run("Synth", "Clock while stopped");
    const auto s = engine::midiPortSettings(f.app->midiOutputsPreferences(), "Synth");
    CHECK((s.clock && s.transport && s.spp && s.mtc && s.clockWhileStopped));
    CHECK(f.app->engine()->syncConfig().outputs[0].clock);
    CHECK(column("Synth").clock);
    CHECK_FALSE(column("Drums").clock);
    for (const char* label : {"Send clock", "Send transport", "Song Position Pointer", "MTC",
                              "Clock while stopped", "Reset notes on stop"})
        CHECK_MESSAGE(menuItem("Synth", label).checked, label);
    // Transport alone: no clock mark.
    run("Drums", "Send transport");
    CHECK_FALSE(column("Drums").clock);
    run("Synth", "Reset notes on stop");
    CHECK_FALSE(engine::midiPortSettings(f.app->midiOutputsPreferences(), "Synth").resetOnStop);
    f.snapshot("routing-hardware-clock.png", &panel.root());
    // The header's right-click opens the same menu.
    std::size_t out = 0;
    for (std::size_t i = 0; i < matrix.model().destinations.size(); ++i)
        if (matrix.model().destinations[i].name == "Synth") out = i;
    const auto b = matrix.grid()->bounds();
    ui::PointerEvent click;
    click.type = ui::PointerEvent::Type::Down;
    click.button = 3;
    click.pos = {b.x + 190 + float(out) * 22 + 11, b.y + 60};
    REQUIRE(matrix.grid()->onPointer(click));
    panel.root().frame(false);
    auto* menu = panel.root().find("routing.port.menu");
    REQUIRE(menu);
    CHECK(ui::menuItems(*menu).size() == 10);
    f.snapshot("routing-hardware-clock-menu.png", &panel.root());
    panel.root().closePopup();
    // Off again, every member back at its default: the record goes.
    for (const char* label : {"Clock while stopped", "Send clock", "Send transport",
                              "Song Position Pointer", "MTC", "Reset notes on stop"})
        run("Synth", label);
    CHECK_FALSE(f.app->midiOutputsPreferences().contains("Synth"));
    CHECK_FALSE(column("Synth").clock);
}

TEST_CASE("routing matrix: a single wildcard input cell can be removed") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.app->setMidiPreferences({{"Keyboard", json::Value::object()}}, {}, false));
    const auto channel = f.exec("track.add", R"({"name":"Single input"})")["track"].asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_midi":"*"})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Panel", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareMidiMode();
    matrix.open();
    panel.root().frame(false);
    const auto& model = matrix.model();
    std::size_t col = 0;
    for (std::size_t i = 0; i < model.destinations.size(); ++i)
        if (model.destinations[i].id == channel) col = i;
    REQUIRE(model.sendsAt(0, col).size() == 1);
    CHECK(model.sendsAt(0, col)[0]->allInputs);
    REQUIRE(matrix.clickCell(0, col));
    REQUIRE(matrix.removeSelected());
    CHECK(model.sendsAt(0, col).empty());
}

namespace {
struct AudioGridDevice : engine::DummyBackend {
    AudioGridDevice() : DummyBackend({.mode = Mode::Manual}) {}
    std::vector<std::string> inputNames() const override { return {"Mic left", "Mic right"}; }
    std::vector<std::string> outputNames() const override {
        return {"Monitor left", "Monitor right"};
    }
};
struct StopGridDevice {
    engine::EngineApp& app;
    ~StopGridDevice() { app.stopEngine(); }
};
std::size_t audioRow(const gui::RoutingMatrix& matrix, const std::string& signal, int side = 0) {
    const auto& rows = matrix.model().sources;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].signal == signal && rows[i].side == side) return i;
    FAIL("missing audio row " << signal << " side " << side);
    return 0;
}
std::size_t audioCol(const gui::RoutingMatrix& matrix, const std::string& signal, int side = 0) {
    const auto& cols = matrix.model().destinations;
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (cols[i].signal == signal && cols[i].side == side) return i;
    FAIL("missing audio column " << signal << " side " << side);
    return 0;
}
std::vector<int> audioInput(const Fixture& f, const std::string& channel) {
    const auto& session = f.app->head();
    const auto* track = session.tracks.find(*model::Id::parse(channel));
    if (!track || !track->record.input) return {};
    const auto* route = session.routes.find(*track->record.input);
    return route && route->from.hwChannels ? *route->from.hwChannels : std::vector<int>{};
}
std::vector<int> audioOutput(const Fixture& f, const std::string& master = "tr_0") {
    const auto id = *model::Id::parse(master);
    return model::hardwareOutputs(*f.app->head().tracks.find(id), id);
}
}  // namespace

TEST_CASE("Hardware Audio: assignments replace inputs, edit masters and warn about LTC") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 4, .outputs = 4},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Vocal input"})")["track"].asString();
    const auto bus = f.exec("track.add", R"({"name":"Bus","type":"buss"})")["track"].asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_midi":"Keyboard"})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    REQUIRE(f.actions->run("ui.routing.hardware_audio"));
    panel.root().frame(false);
    REQUIRE(matrix.hardwareAudioMode());
    CHECK_FALSE(matrix.hardwareMidiMode());
    CHECK_FALSE(matrix.midiMode());
    CHECK_FALSE(matrix.volumeControl()->visible);
    const auto in = audioRow(matrix, "audio_in:1");
    const auto col = audioCol(matrix, channel);
    CHECK(matrix.model().sources[in].name.find("Mic left") != std::string::npos);
    CHECK(matrix.model().destinations[audioCol(matrix, "audio_out:1")].name.find("Monitor left") !=
          std::string::npos);
    const auto gridBounds = matrix.grid()->bounds();
    ui::PointerEvent hover;
    hover.type = ui::PointerEvent::Type::Move;
    hover.pos = {gridBounds.x + 190 + static_cast<float>(col) * 22 + 11,
                 gridBounds.y + 112 + static_cast<float>(in) * 22 + 11};
    panel.root().pointer(hover);
    CHECK(panel.root().find("routing.status")->valueText().find("replaces MIDI input Keyboard") !=
          std::string::npos);
    REQUIRE(matrix.doubleClickCell(in, col));
    CHECK(audioInput(f, channel) == std::vector<int>{1, 2});
    CHECK_FALSE(model::midiInputRoute(f.app->head(), *model::Id::parse(channel)));
    CHECK(matrix.status().find("replaces MIDI input Keyboard") != std::string::npos);
    REQUIRE(matrix.clickCell(in, col));
    CHECK(panel.root().find("routing.send")->valueText() == "1 cell selected");
    REQUIRE(matrix.removeSelected());
    CHECK(audioInput(f, channel).empty());
    // A rack Out is replaced through the same existing track.set path.
    const auto rack = f.exec("rack.add", R"({"track":")" + channel + R"("})")["rack"].asString();
    const auto module =
        f.exec("module.add",
               R"({"rack":")" + rack + R"(","type":"out","label":"Record return"})")["module"]
            .asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_rack":")" + module + R"("})");
    matrix.sync();
    REQUIRE(matrix.doubleClickCell(in, col));
    CHECK(audioInput(f, channel) == std::vector<int>{1, 2});
    CHECK(matrix.status().find("replaces Record return") != std::string::npos);
    CHECK(matrix.status().find(module) == std::string::npos);
    const auto output = audioCol(matrix, "audio_out:3");
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "tr_0"), output));
    CHECK(audioOutput(f) == std::vector<int>{3, 4});
    CHECK(std::none_of(
        matrix.model().sources.begin(), matrix.model().sources.end(),
        [&](const auto& source) { return source.signal == channel || source.signal == bus; }));
    CHECK(std::all_of(matrix.model().sources.begin(), matrix.model().sources.end(),
                      [](const auto& source) {
                          return source.type == "audio_input" || source.type == "master";
                      }));
    CHECK_FALSE(matrix.doubleClickCell(in, output));
    f.exec("sync.set", R"({"ltc_output":true,"ltc_output_channel":3})");
    matrix.sync();
    CHECK(matrix.model().destinations[output].name.find("[LTC]") != std::string::npos);
    const auto sends = matrix.model().sendsAt(audioRow(matrix, "tr_0"), output);
    REQUIRE(sends.size() == 1);
    CHECK(sends.front()->warning.find("LTC replaces Master's audio") != std::string::npos);
    f.snapshot("routing-hardware-audio.png", &panel.root());
    REQUIRE(matrix.clickCell(audioRow(matrix, "tr_0"), output));
    REQUIRE(panel.root().click("routing.remove"));
    CHECK(audioOutput(f).empty());
    matrix.close();
    matrix.open();
    CHECK(matrix.hardwareAudioMode());
    panel.root().frame(false);
    REQUIRE(panel.root().click("routing.mode.hardware"));
    CHECK_FALSE(matrix.hardwareAudioMode());
    CHECK(matrix.hardwareMidiMode());
    REQUIRE(panel.root().click("routing.mode.hardware_audio"));
    CHECK(matrix.hardwareAudioMode());
    CHECK_FALSE(matrix.hardwareMidiMode());
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:1"), audioCol(matrix, channel)));
    REQUIRE(panel.root().click("routing.mode.hardware_audio"));
    CHECK(matrix.selectedCells() == 1);
    CHECK(static_cast<ui::Toggle*>(panel.root().find("routing.mode.hardware_audio"))->on());
    REQUIRE(panel.root().click("routing.mode.audio"));
    CHECK_FALSE(matrix.hardwareAudioMode());
    CHECK(matrix.selectedCells() == 0);
}

TEST_CASE("Hardware Audio: per-signal and whole-tab split edit only representable sides") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 4, .outputs = 4},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Stereo input"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    panel.root().frame(false);
    const auto version = f.app->head().version;
    // Exercise the row-header right-click path as well as the menu seam.
    ui::PointerEvent context;
    context.type = ui::PointerEvent::Type::Down;
    context.button = 3;
    const auto gridBounds = matrix.grid()->bounds();
    context.pos = {gridBounds.x + 30, gridBounds.y + 112 + 11};
    panel.root().pointer(context);
    REQUIRE(panel.root().find("routing.audio.signal"));
    context.type = ui::PointerEvent::Type::Up;
    panel.root().pointer(context);
    panel.root().closePopup();
    auto menu = matrix.audioSignalMenu(true, audioRow(matrix, "audio_in:1"));
    REQUIRE(menu.size() == 1);
    CHECK(menu[0].label == "Split into L/R");
    menu[0].run();
    CHECK(f.app->head().version == version);
    const auto leftInput = audioRow(matrix, "audio_in:1", 1);
    REQUIRE(matrix.doubleClickCell(leftInput, audioCol(matrix, channel)));
    CHECK(audioInput(f, channel) == std::vector<int>{1});
    menu = matrix.audioSignalMenu(true, leftInput);
    CHECK(menu[0].label == "Join");
    menu[0].run();
    CHECK(audioInput(f, channel) == std::vector<int>{1});
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "audio_in:3"), audioCol(matrix, channel)));
    CHECK(audioInput(f, channel) == std::vector<int>{3, 4});
    panel.root().frame(false);
    REQUIRE(panel.root().click("routing.audio.split"));
    CHECK(matrix.model().destinations[audioCol(matrix, channel, 1)].side == 1);
    REQUIRE(
        matrix.doubleClickCell(audioRow(matrix, "audio_in:1", 1), audioCol(matrix, channel, 2)));
    CHECK(audioInput(f, channel) == std::vector<int>{3, 1});
    // The master retains R=2 while L moves to hardware output 3.
    REQUIRE(
        matrix.doubleClickCell(audioRow(matrix, "tr_0", 1), audioCol(matrix, "audio_out:3", 1)));
    CHECK(audioOutput(f) == std::vector<int>{3, 2});
    f.exec("history.undo", "{}");
    CHECK(audioOutput(f) == std::vector<int>{1, 2});
    f.exec("history.redo", "{}");
    matrix.sync();
    CHECK(audioOutput(f) == std::vector<int>{3, 2});
    panel.root().frame(false);
    f.snapshot("routing-hardware-audio-split.png", &panel.root());
    REQUIRE(matrix.clickCell(audioRow(matrix, "tr_0", 1), audioCol(matrix, "audio_out:3", 1)));
    auto partial = matrix.removeSelected();
    REQUIRE_FALSE(partial);
    CHECK_FALSE(partial.error().hint.empty());
    CHECK(audioOutput(f) == std::vector<int>{3, 2});
    CHECK_FALSE(
        matrix.doubleClickCell(audioRow(matrix, "tr_0", 1), audioCol(matrix, "audio_out:1", 2)));
    // Removing the entire split assignment remains one undoable track edit.
    matrix.clickRow(audioRow(matrix, "tr_0", 1));
    matrix.clickRow(audioRow(matrix, "tr_0", 2), ui::kCtrl);
    REQUIRE(matrix.removeSelected());
    CHECK(audioOutput(f).empty());
    const auto denied =
        matrix.doubleClickCell(audioRow(matrix, "tr_0", 1), audioCol(matrix, "audio_out:3", 1));
    REQUIRE_FALSE(denied);
    CHECK_FALSE(denied.error().hint.empty());
    const auto splits = matrix.saveAudioSplits();
    matrix.loadAudioSplits(json::Value());
    CHECK(matrix.model().sources[audioRow(matrix, "tr_0")].side == 0);
    matrix.loadAudioSplits(splits);
    CHECK(matrix.model().sources[audioRow(matrix, "tr_0", 1)].side == 1);
    panel.root().frame(false);
    REQUIRE(panel.root().click("routing.audio.split"));
    CHECK(matrix.saveAudioSplits().asArray().empty());
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "tr_0"), audioCol(matrix, "audio_out:1")));
    // Joined master -> one output is the existing mono sum, not left-only.
    menu = matrix.audioSignalMenu(false, audioCol(matrix, "audio_out:3"));
    menu[0].run();
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "tr_0"), audioCol(matrix, "audio_out:3", 1)));
    CHECK(audioOutput(f) == std::vector<int>{3});
}

TEST_CASE("Hardware Audio: conflicting selections refuse atomically, input sides need a pair") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 4, .outputs = 4},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Unassigned input"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    const auto version = f.app->head().version;
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:1"), audioCol(matrix, channel)));
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:3"), audioCol(matrix, channel), ui::kCtrl));
    CHECK_FALSE(matrix.addSelected());
    CHECK(audioInput(f, channel).empty());
    CHECK(f.app->head().version == version);
    matrix.splitAllAudio(true);
    auto denied =
        matrix.doubleClickCell(audioRow(matrix, "audio_in:1", 1), audioCol(matrix, channel, 1));
    REQUIRE_FALSE(denied);
    CHECK(denied.error().message.find("assign an input first") != std::string::npos);
    CHECK(audioInput(f, channel).empty());
    // External commands can clear the map before the next GUI sync. A
    // formerly usable split cell must refuse rather than index an empty map.
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":[1,2]})");
    matrix.sync();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":null})");
    denied =
        matrix.doubleClickCell(audioRow(matrix, "audio_in:3", 1), audioCol(matrix, channel, 1));
    REQUIRE_FALSE(denied);
    CHECK_FALSE(denied.error().hint.empty());
    CHECK(audioInput(f, channel).empty());
    matrix.sync();
    f.exec("track.set", R"({"track":"tr_0","hw_output":[]})");
    denied =
        matrix.doubleClickCell(audioRow(matrix, "tr_0", 1), audioCol(matrix, "audio_out:3", 1));
    REQUIRE_FALSE(denied);
    CHECK_FALSE(denied.error().hint.empty());
    CHECK(audioOutput(f).empty());
}

TEST_CASE("Hardware Audio: a 64-channel device stays addressable and refreshes on stop") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 64, .outputs = 64},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Last pair"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    CHECK(audioRow(matrix, "audio_in:63") < matrix.model().sources.size());
    CHECK(audioCol(matrix, "audio_out:63") < matrix.model().destinations.size());
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "audio_in:63"), audioCol(matrix, channel)));
    CHECK(audioInput(f, channel) == std::vector<int>{63, 64});
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "tr_0"), audioCol(matrix, "audio_out:63")));
    CHECK(audioOutput(f) == std::vector<int>{63, 64});
    matrix.splitAllAudio(true);
    CHECK(matrix.model().sources[audioRow(matrix, "audio_in:63", 2)].deviceChannels ==
          std::vector<int>{64});
    CHECK(matrix.model().destinations[audioCol(matrix, "audio_out:63", 2)].deviceChannels ==
          std::vector<int>{64});
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:63", 1), audioCol(matrix, channel, 1)));
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:63", 2), audioCol(matrix, channel, 2),
                             ui::kCtrl));
    CHECK(matrix.selectedCells() == 2);
    CHECK(panel.root().find("routing.send")->valueText() == "2 cells selected");
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:1", 1), audioCol(matrix, channel, 1)));
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:1", 2), audioCol(matrix, channel, 2),
                             ui::kCtrl));
    REQUIRE(matrix.addSelected());
    CHECK(audioInput(f, channel) == std::vector<int>{1, 2});
    f.exec("history.undo", "{}");
    matrix.sync();
    CHECK(audioInput(f, channel) == std::vector<int>{63, 64});
    panel.root().frame(false);
    f.snapshot("routing-hardware-audio-64.png", &panel.root());
    f.app->stopEngine();
    matrix.sync();
    CHECK(matrix.model().sources[audioRow(matrix, "audio_in:63", 2)].name.find("engine stopped") !=
          std::string::npos);
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 2, .outputs = 2},
                               {.anticipative = false, .workers = 0}));
    matrix.sync();
    CHECK(matrix.model().sources[audioRow(matrix, "audio_in:63", 2)].name.find("unavailable") !=
          std::string::npos);
    matrix.selectAll();
    REQUIRE(matrix.removeSelected());
    CHECK(audioInput(f, channel).empty());
    CHECK(audioOutput(f).empty());
    f.exec("history.undo", "{}");
    CHECK(audioInput(f, channel) == std::vector<int>{63, 64});
    CHECK(audioOutput(f) == std::vector<int>{63, 64});
    matrix.loadAudioSplits(json::Value::object());
    CHECK(matrix.saveAudioSplits().asArray().empty());
}

TEST_CASE("Hardware Audio: mono signals and odd device channels stay joined") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 3, .outputs = 3},
                               {.anticipative = false, .workers = 0}));
    const auto mono = f.exec("track.add", R"({"name":"Mono","channels":1})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    CHECK_FALSE(matrix.audioSignalMenu(false, audioCol(matrix, mono))[0].enabled);
    CHECK_FALSE(matrix.audioSignalMenu(true, audioRow(matrix, "audio_in:3"))[0].enabled);
    CHECK_FALSE(matrix.audioSignalMenu(false, audioCol(matrix, "audio_out:3"))[0].enabled);
    matrix.splitAllAudio(true);
    CHECK(matrix.model().destinations[audioCol(matrix, mono)].width == 1);
    REQUIRE(matrix.doubleClickCell(audioRow(matrix, "audio_in:3"), audioCol(matrix, mono)));
    CHECK(audioInput(f, mono) == std::vector<int>{3});
}

TEST_CASE("Hardware Audio: LTC resolves the unavailable default to output two") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 2, .outputs = 2},
                               {.anticipative = false, .workers = 0}));
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    f.exec("sync.set", R"({"ltc_output":true,"ltc_output_channel":3})");
    matrix.sync();
    CHECK(f.app->resolvedLtcOutputChannel() == 2);
    const auto col = audioCol(matrix, "audio_out:1");
    CHECK(matrix.model().destinations[col].name.starts_with("[LTC] "));
    CHECK(std::none_of(matrix.model().destinations.begin(), matrix.model().destinations.end(),
                       [](const auto& d) { return d.signal == "audio_out:3"; }));
    const auto sends = matrix.model().sendsAt(audioRow(matrix, "tr_0"), col);
    REQUIRE(sends.size() == 1);
    CHECK(sends.front()->warning == "LTC replaces Master's audio on output 2");
    panel.root().frame(false);
    const auto b = matrix.grid()->bounds();
    ui::PointerEvent hover;
    hover.type = ui::PointerEvent::Type::Move;
    hover.pos = {b.x + 190 + static_cast<float>(col) * 22 + 11,
                 b.y + 112 + static_cast<float>(audioRow(matrix, "tr_0")) * 22 + 11};
    panel.root().pointer(hover);
    CHECK(panel.root()
              .find("routing.status")
              ->valueText()
              .find("LTC replaces Master's audio on output 2") != std::string::npos);
    f.snapshot("routing-hardware-audio-ltc-fallback.png", &panel.root());
    matrix.splitAllAudio(true);
    CHECK_FALSE(
        matrix.model().destinations[audioCol(matrix, "audio_out:1", 1)].name.starts_with("[LTC] "));
    CHECK(
        matrix.model().destinations[audioCol(matrix, "audio_out:1", 2)].name.starts_with("[LTC] "));
    const auto split =
        matrix.model().sendsAt(audioRow(matrix, "tr_0", 2), audioCol(matrix, "audio_out:1", 2));
    REQUIRE(split.size() == 1);
    CHECK(split.front()->warning == "LTC replaces Master's audio on output 2");
}

TEST_CASE("Hardware Audio: swap sides atomically and refuse mixed blocked selections") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 2, .outputs = 4},
                               {.anticipative = false, .workers = 0}));
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    matrix.splitAllAudio(true);
    const auto l = audioRow(matrix, "tr_0", 1), r = audioRow(matrix, "tr_0", 2);
    REQUIRE(matrix.model().blocked(l, audioCol(matrix, "audio_out:1", 2)).empty());
    REQUIRE(matrix.clickCell(l, audioCol(matrix, "audio_out:1", 2)));
    REQUIRE(matrix.clickCell(r, audioCol(matrix, "audio_out:1", 1), ui::kCtrl));
    REQUIRE(matrix.addSelected());
    CHECK(audioOutput(f) == std::vector<int>{2, 1});
    f.exec("history.undo", "{}");
    matrix.sync();
    CHECK(audioOutput(f) == std::vector<int>{1, 2});
    const auto version = f.app->head().version;
    REQUIRE(matrix.clickCell(l, audioCol(matrix, "audio_out:3", 1)));
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:1", 1), audioCol(matrix, "audio_out:3", 1),
                             ui::kCtrl));
    auto denied = matrix.addSelected();
    REQUIRE_FALSE(denied);
    CHECK(denied.error().message.find("Input 1") != std::string::npos);
    CHECK_FALSE(denied.error().hint.empty());
    CHECK(audioOutput(f) == std::vector<int>{1, 2});
    CHECK(f.app->head().version == version);
}

TEST_CASE("Hardware Audio: joined mono cells identify mono and double-click restores the pair") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 2, .outputs = 6},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Mono input"})")["track"].asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":[1]})");
    f.exec("track.set", R"({"track":"tr_0","hw_output":[5]})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    panel.root().frame(false);
    auto hover = [&](std::size_t row, std::size_t col) {
        const auto b = matrix.grid()->bounds();
        ui::PointerEvent e;
        e.type = ui::PointerEvent::Type::Move;
        e.pos = {b.x + 190 + static_cast<float>(col) * 22 + 11,
                 b.y + 112 + static_cast<float>(row) * 22 + 11};
        panel.root().pointer(e);
        return panel.root().find("routing.status")->valueText();
    };
    const auto input = audioRow(matrix, "audio_in:1"), col = audioCol(matrix, channel);
    const auto master = audioRow(matrix, "tr_0"), output = audioCol(matrix, "audio_out:5");
    CHECK(matrix.model().sendsAt(input, col).front()->mono);
    CHECK(matrix.model().sendsAt(master, output).front()->mono);
    CHECK(hover(input, col).find("mono: input 1 to both sides") != std::string::npos);
    CHECK(hover(master, output).find("mono: (L+R)/2 to output 5") != std::string::npos);
    f.snapshot("routing-hardware-audio-mono.png", &panel.root());
    REQUIRE(matrix.doubleClickCell(input, col));
    CHECK(audioInput(f, channel) == std::vector<int>{1, 2});
    CHECK_FALSE(matrix.model().sendsAt(input, col).front()->mono);
    REQUIRE(matrix.doubleClickCell(master, output));
    CHECK(audioOutput(f) == std::vector<int>{5, 6});
    CHECK_FALSE(matrix.model().sendsAt(master, output).front()->mono);
    f.exec("history.undo", "{}");
    CHECK(audioOutput(f) == std::vector<int>{5});
}

TEST_CASE(
    "Hardware Audio: widen mono record inputs for side edits but preserve mono master refusal") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 4, .outputs = 4},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Mono input"})")["track"].asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":[1]})");
    f.exec("track.set", R"({"track":"tr_0","hw_output":[1]})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    matrix.splitAllAudio(true);
    REQUIRE(
        matrix.doubleClickCell(audioRow(matrix, "audio_in:3", 1), audioCol(matrix, channel, 1)));
    CHECK(audioInput(f, channel) == std::vector<int>{3, 1});
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":[1]})");
    matrix.sync();
    REQUIRE(
        matrix.doubleClickCell(audioRow(matrix, "audio_in:3", 2), audioCol(matrix, channel, 2)));
    CHECK(audioInput(f, channel) == std::vector<int>{1, 4});
    auto denied =
        matrix.doubleClickCell(audioRow(matrix, "tr_0", 1), audioCol(matrix, "audio_out:3", 1));
    REQUIRE_FALSE(denied);
    CHECK_FALSE(denied.error().hint.empty());
    CHECK(audioOutput(f) == std::vector<int>{1});
}

TEST_CASE("Hardware Audio: save keeps project device splits and drops deleted track IDs") {
    Fixture f;
    REQUIRE(f.ok);
    const auto channel = f.exec("track.add", R"({"name":"Transient"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    auto splits = json::Value::array();
    for (const auto& signal :
         {std::string("audio_in:1"), std::string("audio_out:1"), channel, std::string("tr_99999")})
        splits.push_back(signal);
    matrix.loadAudioSplits(splits);
    CHECK(matrix.saveAudioSplits().size() == 3);
    f.exec("track.remove", R"({"track":")" + channel + R"("})");
    const auto version = f.app->head().version;
    auto saved = matrix.saveAudioSplits();
    REQUIRE(saved.size() == 2);
    CHECK(saved.asArray()[0].asString() == "audio_in:1");
    CHECK(saved.asArray()[1].asString() == "audio_out:1");
    CHECK(f.app->head().version == version);
}

TEST_CASE("Hardware Audio: split button eligibility requires a stereo signal") {
    gui::MatrixModel model;
    CHECK_FALSE(model.hasStereoSignals());
    gui::MatrixModel::Source input;
    input.width = 1;
    gui::MatrixModel::Destination output;
    output.width = 1;
    model.sources.push_back(input);
    model.destinations.push_back(output);
    CHECK_FALSE(model.hasStereoSignals());
    model.destinations.front().width = 2;
    CHECK(model.hasStereoSignals());
    model.destinations.front().side = 1;
    CHECK(model.hasStereoSignals());  // Split stereo still needs Join all.
}

TEST_CASE("Hardware Audio: stale deleted targets refuse the whole selection") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 2, .outputs = 4},
                               {.anticipative = false, .workers = 0}));
    const auto channel = f.exec("track.add", R"({"name":"Removed input"})")["track"].asString();
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    REQUIRE(matrix.clickCell(audioRow(matrix, "tr_0"), audioCol(matrix, "audio_out:3")));
    REQUIRE(matrix.clickCell(audioRow(matrix, "audio_in:1"), audioCol(matrix, channel), ui::kCtrl));
    f.exec("track.remove", R"({"track":")" + channel + R"("})");
    const auto version = f.app->head().version;
    auto denied = matrix.addSelected();
    REQUIRE_FALSE(denied);
    CHECK(denied.error().message.find("Removed input") != std::string::npos);
    CHECK(audioOutput(f) == std::vector<int>{1, 2});
    CHECK(f.app->head().version == version);
}

TEST_CASE("Hardware Audio: lit multi-channel input cells preserve the complete recording map") {
    Fixture f;
    REQUIRE(f.ok);
    AudioGridDevice backend;
    StopGridDevice stop{*f.app};
    REQUIRE(f.app->startEngine(backend, {.blockSize = 64, .inputs = 6, .outputs = 2},
                               {.anticipative = false, .workers = 0}));
    const auto channel =
        f.exec("track.add", R"({"name":"Four inputs","channels":4})")["track"].asString();
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":[1,2,3,4]})");
    gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing", 1000,
                             560, 800, 360);
    gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
    matrix.setHardwareAudioMode();
    matrix.open();
    panel.root().frame(false);
    auto check = [&](const std::vector<int>& expected, const std::string& description) {
        const auto row = audioRow(matrix, "audio_in:1"), col = audioCol(matrix, channel);
        REQUIRE(matrix.model().sendsAt(row, col).size() == 1);
        const auto b = matrix.grid()->bounds();
        ui::PointerEvent e;
        e.type = ui::PointerEvent::Type::Move;
        e.pos = {b.x + 190 + static_cast<float>(col) * 22 + 11,
                 b.y + 112 + static_cast<float>(row) * 22 + 11};
        panel.root().pointer(e);
        const auto hover = panel.root().find("routing.status")->valueText();
        CHECK(hover.find(description) != std::string::npos);
        CHECK(hover.find("assign the pair") == std::string::npos);
        CHECK(hover.find("replaces") == std::string::npos);
        const auto version = f.app->head().version;
        REQUIRE(matrix.doubleClickCell(row, col));
        CHECK(audioInput(f, channel) == expected);
        CHECK(f.app->head().version == version);
    };
    check({1, 2, 3, 4}, "4-channel input 1, 2, 3, 4");
    f.exec("track.set", R"({"track":")" + channel + R"(","record_input":[1,3,2,4]})");
    matrix.sync();
    panel.root().frame(false);
    check({1, 3, 2, 4}, "4-channel input 1, 3, 2, 4");
    f.snapshot("routing-hardware-audio-multichannel.png", &panel.root());
}

TEST_CASE("routing matrix: every tab opens with grid focus and immediate keyboard selection") {
    for (int mode = 0; mode < 4; ++mode) {
        CAPTURE(mode);
        Fixture f;
        REQUIRE(f.ok);
        gui::FloatingPanel panel(*f.platform, *f.window, *f.root, SUPPLE_GUI_FONTS_DIR, "Routing",
                                 1000, 560, 800, 360);
        gui::RoutingMatrix matrix(panel, *f.actions, *f.dispatcher, *f.app);
        if (mode == 0) {
            const auto bus =
                f.exec("track.add", R"({"name":"Bus","type":"buss"})")["track"].asString();
            f.exec("route.add", R"({"from":"tr_1","to":")" + bus + R"("})");
        } else if (mode == 1) {
            const auto target =
                f.exec("track.add", R"({"name":"MIDI target"})")["track"].asString();
            f.exec("route.add", R"({"from":"tr_1","to":")" + target + R"(","midi":{}})");
            matrix.setMidiMode(true);
        } else if (mode == 2) {
            REQUIRE(f.app->setMidiPreferences({{"Keyboard", json::Value::object()}},
                                              {{"Synth", json::Value::object()}}, false));
            f.exec("track.set", R"({"track":"tr_1","record_midi":"Keyboard"})");
            f.exec("route.add", R"({"from":"tr_1","to_midi":"Synth","midi":{}})");
            matrix.setHardwareMidiMode();
        } else {
            f.exec("track.set", R"({"track":"tr_1","record_input":[1,2]})");
            matrix.setHardwareAudioMode();
        }
        matrix.open();
        panel.root().frame(false);
        REQUIRE(panel.root().focused() == matrix.grid());
        REQUIRE_FALSE(matrix.model().sends.empty());
        panel.root().key(SDLK_A, ui::kCtrl);
        CHECK(matrix.selectedCells() ==
              matrix.model().sources.size() * matrix.model().destinations.size());
        panel.root().key(SDLK_DELETE);
        CHECK(matrix.model().sends.empty());
        CHECK(matrix.midiMode() == (mode == 1 || mode == 2));
        CHECK(matrix.hardwareMidiMode() == (mode == 2));
        CHECK(matrix.hardwareAudioMode() == (mode == 3));
        matrix.close();
        matrix.open();
        CHECK(panel.root().focused() == matrix.grid());
    }
}
