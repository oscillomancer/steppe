// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>

#include "supple/gui/png.h"
#include "supple/json/parse.h"
#include "supple/ui/controls.h"
#include "supple/ui/forms.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/shortcut.h"
#include "supple/ui/text_area.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

using namespace supple;
using namespace supple::ui;

namespace {

// One offscreen window shared by the tests (GL contexts are not free).
struct Fixture {
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    bool ok = false;
    Fixture() {
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"ui-test", 320, 200, true});
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
        ok = true;
    }
};

Fixture& fixture() {
    static Fixture f;
    return f;
}

#define REQUIRE_GL()                                             \
    if (!fixture().ok) {                                         \
        MESSAGE("skipped: no offscreen GL in this environment"); \
        return;                                                  \
    }

Node& panel(Node& n, std::string id, Layout::Dir dir) {
    auto& p = n.emplace<Node>(std::move(id));
    p.layout.dir = dir;
    return p;
}

// A filled box that records how often it paints.
struct Box : Node {
    explicit Box(std::string id, std::string token = "raised")
        : Node(std::move(id)), color(std::move(token)) {}
    std::string color;
    int paints = 0;
    void paint(Canvas& c) override {
        ++paints;
        c.fillRect(bounds(), c.theme().color(color));
    }
};

[[maybe_unused]] double pixelDistance(const gui::Image& a, const gui::Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i)
        worst = std::max(worst,
                         std::abs(static_cast<double>(a.rgba[i]) - static_cast<double>(b.rgba[i])));
    return worst;
}

}  // namespace

TEST_CASE("a list shorter than one row pages by one row") {
    REQUIRE_GL();
    const Theme theme = Theme::dark();
    Canvas canvas(fixture().window->vg(), fixture().fonts, theme);
    List list("short-list");
    list.setCount(10);
    list.arrange(canvas, {0, 0, 100, 10});
    list.select(0);
    CHECK(list.onKey({SDLK_PAGEDOWN}));
    CHECK(list.selected() == 1);
    CHECK(list.onKey({SDLK_PAGEUP}));
    CHECK(list.selected() == 0);
}

TEST_CASE("a list selected before its first layout shows its first row") {
    REQUIRE_GL();
    const Theme theme = Theme::dark();
    Canvas canvas(fixture().window->vg(), fixture().fonts, theme);
    List list("early-list");
    list.setCount(20);
    list.select(0);
    list.arrange(canvas, {0, 0, 100, 280});
    CHECK(list.scroll() == 0);
}

TEST_CASE("root dispatch defers deletion and resolves targets after callbacks") {
    REQUIRE_GL();
    struct Reactive : Node {
        explicit Reactive(std::string id, bool& dead) : Node(std::move(id)), dead_(dead) {
            focusable = interactive = true;
            layout.width = SizeRule::fixed(100);
        }
        ~Reactive() override { dead_ = true; }
        void onFocus(bool value) override {
            if (auto callback = focusChanged) callback(value);
        }
        void onHover(bool value) override {
            if (auto callback = hoverChanged) callback(value);
        }
        std::function<void(bool)> focusChanged, hoverChanged;
        bool& dead_;
    };
    bool oldDead = false, nextDead = false;
    int clicks = 0;
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("content");
    content->layout.dir = Layout::Dir::Row;
    auto& old = content->emplace<Reactive>("old", oldDead);
    auto& next = content->emplace<Reactive>("next", nextDead);
    root.setContent(std::move(content));
    root.frame(false);
    SUBCASE("blur replaces content during a click") {
        root.focus(&old);
        old.focusChanged = [&](bool focused) {
            if (focused) return;
            auto replacement = std::make_unique<Node>("replacement");
            replacement->layout.dir = Layout::Dir::Row;
            replacement->emplace<Node>("spacer").layout.width = SizeRule::fixed(100);
            auto& button = replacement->emplace<Button>("new", "New", [&] { ++clicks; });
            button.layout.width = SizeRule::fixed(100);
            button.layout.height = SizeRule::fraction();
            root.setContent(std::move(replacement));
            CHECK_FALSE(oldDead);
            CHECK_FALSE(nextDead);
        };
        CHECK(root.click("next"));
        CHECK(oldDead);
        CHECK(nextDead);
        CHECK(clicks == 1);
        CHECK(root.focused() == root.find("new"));
    }
    SUBCASE("hover exit removes the next target") {
        bool entered = false;
        next.hoverChanged = [&](bool hovered) {
            if (hovered) entered = true;
        };
        old.hoverChanged = [&](bool hovered) {
            if (hovered) return;
            (void)root.content()->remove(&next);
            CHECK_FALSE(nextDead);
        };
        root.pointer({PointerEvent::Type::Move, {50, 50}});
        root.pointer({PointerEvent::Type::Move, {150, 50}});
        CHECK(nextDead);
        CHECK_FALSE(entered);
        CHECK(root.hovered() == nullptr);
    }
}

TEST_CASE("stack layout: fixed, content and fractions with padding, gaps and alignment") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    content->layout.padding = Insets::all(10);
    content->layout.gap = 4;
    auto& top = content->emplace<Box>("top");
    top.layout.height = SizeRule::fixed(30);
    auto& row = panel(*content, "row", Layout::Dir::Row);  // takes the rest
    row.layout.gap = 2;
    auto& a = row.emplace<Box>("a");
    a.layout.width = SizeRule::fraction(1);
    auto& b = row.emplace<Box>("b");
    b.layout.width = SizeRule::fraction(3);
    auto& c = row.emplace<Box>("c");
    c.layout.width = SizeRule::fixed(50);
    c.layout.height = SizeRule::fixed(20);
    row.layout.align = Layout::Align::Center;
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(top.bounds() == Rect{10, 10, 300, 30});
    CHECK(row.bounds() == Rect{10, 44, 300, 146});
    // 300 − 50 fixed − 2 × 2 gap = 246 shared 1:3.
    CHECK(a.bounds().w == doctest::Approx(61.5));
    CHECK(b.bounds().w == doctest::Approx(184.5));
    CHECK(c.bounds().x == doctest::Approx(10 + 61.5 + 2 + 184.5 + 2));
    CHECK(c.bounds().y == doctest::Approx(44 + (146 - 20) / 2.0));  // centred cross axis
    CHECK(a.bounds().h == doctest::Approx(146));                    // fraction cross: stretch
}

TEST_CASE("a label sizes to its text; a button sizes to its label plus padding") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    auto& label = content->emplace<Label>("label", "Tempo");
    auto& button = content->emplace<Button>("button", "Render…");
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(label.bounds().w > 20);
    CHECK(label.bounds().w < 80);
    CHECK(button.bounds().w > label.bounds().w);
    CHECK(button.bounds().h == 28);
    label.setText("A much longer tempo label");
    root.frame(false);
    CHECK(label.bounds().w > 120);  // content size follows the text
}

TEST_CASE("damage repaints only what changed") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    auto& left = content->emplace<Box>("left");
    auto& right = content->emplace<Box>("right", "panel");
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(left.paints == 1);
    CHECK(right.paints == 1);
    CHECK_FALSE(root.wantsFrame());
    const double before = root.stats().repaintedArea;
    right.invalidate();
    CHECK(root.wantsFrame());
    root.frame(false);
    CHECK(left.paints == 1);  // culled: outside the damage
    CHECK(right.paints == 2);
    CHECK(root.stats().repaintedArea - before == doctest::Approx(160.0 * 200.0));
    // The cache keeps the undamaged half: the frame matches a full repaint.
    const gui::Image partial = root.readBack();
    root.damageAll();
    root.frame(false);
    CHECK(pixelDistance(partial, root.readBack()) == 0);
}

TEST_CASE("presented frames are at least the minimum frame interval apart") {
    using std::chrono::microseconds;
    auto us = [](float hz) {
        return std::chrono::duration_cast<microseconds>(minFrameInterval(hz)).count();
    };
    CHECK(us(60) == 12500);
    CHECK(us(0) == 12500);    // unknown: as 60 Hz
    CHECK(us(144) == 10416);  // 72 fps, every second refresh
    CHECK(us(240) == 6250);   // 120 fps
    // A window that does not wait for vsync is paced at the whole period.
    CHECK(std::chrono::duration_cast<microseconds>(frameInterval(60)).count() == 16666);
    CHECK(std::chrono::duration_cast<microseconds>(frameInterval(144)).count() == 13888);
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    root.setContent(std::make_unique<Node>("root"));
    CHECK(root.frameReady({}));  // the first frame is never held back
    CHECK_FALSE(root.frameDeadline());
    root.frame();
    CHECK_FALSE(root.frameDeadline());  // nothing wanted
    root.requestAnimationFrame();
    const auto deadline = root.frameDeadline();
    REQUIRE(deadline);
    CHECK_FALSE(root.frameReady(*deadline - microseconds(1)));
    CHECK(root.frameReady(*deadline));
}

TEST_CASE("clicks, focus traversal and keys reach the right nodes") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.gap = 8;
    int ok = 0, cancel = 0;
    content->emplace<Button>("ok", "OK", [&] { ++ok; });
    auto& cancelButton = content->emplace<Button>("cancel", "Cancel", [&] { ++cancel; });
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(root.click("ok"));
    CHECK(ok == 1);
    CHECK(root.focused() == root.find("ok"));  // a click focuses a focusable target
    root.key(SDLK_TAB);
    CHECK(root.focused() == &cancelButton);
    root.key(SDLK_RETURN);
    CHECK(cancel == 1);
    root.key(SDLK_TAB, kShift);
    CHECK(root.focused() == root.find("ok"));
    CHECK_FALSE(root.click("missing"));
    cancelButton.enabled = false;
    CHECK_FALSE(root.click("cancel"));
    CHECK(cancel == 1);
    // A press that ends outside the button does not click it.
    const Rect b = root.find("ok")->bounds();
    root.pointer({PointerEvent::Type::Down, {b.x + 2, b.y + 2}, 1});
    root.pointer({PointerEvent::Type::Up, {b.x + 200, b.y + 2}, 1});
    CHECK(ok == 1);
}

TEST_CASE("input addressed to another window (a plug-in editor) never reaches the root") {
    REQUIRE_GL();
    auto other = gui::Window::open(*fixture().platform, {"other", 200, 120, false});
    REQUIRE(other);
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    int clicks = 0, keys = 0;
    content->emplace<Button>("ok", "OK", [&] { ++clicks; });
    root.setContent(std::move(content));
    root.onUnhandledKey = [&](const KeyEvent&) { return ++keys, true; };
    root.frame(false);
    const Rect b = root.find("ok")->bounds();
    REQUIRE(b.w > 4);
    auto press = [&](SDL_WindowID window) {
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event e{};
            e.type = type;
            e.button.windowID = window;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.clicks = 1;
            e.button.x = b.x + 2;
            e.button.y = b.y + 2;
            root.handle(e);
        }
        SDL_Event k{};
        k.type = SDL_EVENT_KEY_DOWN;
        k.key.windowID = window;
        k.key.key = SDLK_T;
        root.handle(k);
    };
    press((*other)->id());
    CHECK(clicks == 0);
    CHECK(keys == 0);
    press(fixture().window->id());
    CHECK(clicks == 1);
    CHECK(keys == 1);
}

TEST_CASE("theme JSON overrides tokens and reports bad values with their path") {
    auto doc = json::parse(R"({"schema": "supple.theme/1.0", "name": "test",
        "colors": {"accent": "#102030"}, "space": {"m": 20}})");
    REQUIRE(doc);
    auto theme = Theme::fromJson(*doc, Theme::dark());
    REQUIRE(theme);
    CHECK(theme->name() == "test");
    CHECK(theme->color("accent") == Color::hex(0x102030));
    CHECK(theme->color("ink") == Theme::dark().color("ink"));  // untouched tokens keep the base
    CHECK(theme->space("m") == 20);
    CHECK(theme->color("no-such-token") == Color::hex(0xff00ff));
    auto bad = json::parse(R"({"schema": "supple.theme/1.0", "colors": {"accent": "blue"}})");
    auto rejected = Theme::fromJson(*bad, Theme::dark());
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().path == "/colors/accent");
}

TEST_CASE("golden image: a small panel in the dark theme") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    content->layout.padding = Insets::all(16);
    content->layout.gap = 12;
    content->emplace<Label>("title", "Render", Font::UiBold, "title");
    auto& row = panel(*content, "row", Layout::Dir::Row);
    row.layout.height = SizeRule::fixed(28);
    row.layout.gap = 8;
    row.emplace<Button>("go", "Render 1 file");
    row.emplace<Button>("dry", "Dry run");
    content->emplace<Label>("hint", "renders/song.wav", Font::Mono, "mono", "ink-quiet");
    root.setContent(std::move(content));
    root.frame(false);
    const gui::Image image = root.readBack();
#if !defined(__linux__)
    REQUIRE(image.width > 0);
    REQUIRE(image.height > 0);
    REQUIRE(image.rgba.size() == static_cast<std::size_t>(image.width * image.height * 4));
    bool varied = false;
    for (std::size_t i = 4; i < image.rgba.size(); i += 4)
        if (image.rgba[i] != image.rgba[0] || image.rgba[i + 1] != image.rgba[1] ||
            image.rgba[i + 2] != image.rgba[2]) {
            varied = true;
            break;
        }
    CHECK(varied);
    return;
#endif

    const std::filesystem::path golden =
        std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / "panel-dark.png";
    if (std::getenv("SUPPLE_UPDATE_GOLDEN") || !std::filesystem::exists(golden)) {
        REQUIRE(gui::writePng(golden, image));
        MESSAGE("golden written: " << golden.string());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(golden.string().c_str(), &w, &h, &n, 4);
    REQUIRE(data);
    gui::Image expected{
        w, h, std::vector<std::uint8_t>(data, data + static_cast<std::size_t>(w * h * 4))};
    stbi_image_free(data);
    // Antialiasing may differ by driver; layout or colour regressions do not.
    CHECK(pixelDistance(image, expected) <= 48);
}

TEST_CASE("a text field edits by UTF-8 character and word, with selection and clipboard") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    content->layout.padding = Insets::all(8);
    auto& field = content->emplace<TextField>("field", "", "Search");
    std::string submitted;
    int changes = 0;
    field.onChange = [&](const std::string&) { ++changes; };
    field.onSubmit = [&](const std::string& s) { submitted = s; };
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(root.click("field"));
    CHECK(root.focused() == &field);
    root.text("héllo wörld");
    CHECK(field.text() == "héllo wörld");
    CHECK(changes == 1);
    root.key(SDLK_BACKSPACE);  // one character, two bytes
    CHECK(field.text() == "héllo wörl");
    root.key(SDLK_BACKSPACE, kCtrl);  // a word
    CHECK(field.text() == "héllo ");
    root.key(SDLK_HOME);
    root.key(SDLK_RIGHT);
    root.key(SDLK_RIGHT, kShift);  // select "é"
    CHECK(field.selection() == std::pair<std::size_t, std::size_t>{1, 3});
    root.text("e");
    CHECK(field.text() == "hello ");
    root.key(SDLK_A, kCtrl);
    root.key(SDLK_C, kCtrl);
    root.key(SDLK_END);
    root.key(SDLK_V, kCtrl);
    CHECK(field.text() == "hello hello ");
    root.key(SDLK_RETURN);
    CHECK(submitted == "hello hello ");
    // IME: composition shows inline and does not change the text until committed.
    root.composition("にほ", 2);
    CHECK(field.composition() == "にほ");
    CHECK(field.text() == "hello hello ");
    root.frame(false);  // paints with the composition
    root.text("日本");
    CHECK(field.composition().empty());
    CHECK(field.text() == "hello hello 日本");
}

TEST_CASE("a list paints only visible rows and navigates by keys") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    auto& list = content->emplace<List>("list");
    list.rowHeight = 20;
    std::size_t activated = 99999;
    list.onActivate = [&](std::size_t i) { activated = i; };
    list.paintRow = [](Canvas& c, std::size_t i, const Rect& r, bool) {
        c.text(r, "row " + std::to_string(i), Font::Ui, 12, c.theme().color("ink"));
    };
    list.setCount(100000);
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(list.rowsPainted() == 10);  // 200 units tall / 20
    root.click("list");
    CHECK(list.selected() == std::optional<std::size_t>(5));  // the centre row of 10
    root.key(SDLK_END);
    CHECK(list.selected() == std::optional<std::size_t>(99999));
    CHECK(list.scroll() == doctest::Approx(100000.0 * 20 - 200));
    root.key(SDLK_PAGEUP);
    CHECK(*list.selected() < 99999);
    root.key(SDLK_RETURN);
    CHECK(activated == *list.selected());
    root.frame(false);
    CHECK(list.rowsPainted() <= 11);
}

TEST_CASE("popups: menus run items, outside clicks and Escape close them, focus returns") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    auto& button = content->emplace<Button>("open", "Open");
    root.setContent(std::move(content));
    root.frame(false);
    root.click("open");
    CHECK(root.focused() == &button);
    int ran = 0;
    std::vector<MenuItem> items = {{"First", "Ctrl+1", [&] { ran = 1; }},
                                   {"", "", {}, true, true},
                                   {"Disabled", "", [&] { ran = 2; }, false},
                                   {"Third", "", [&] { ran = 3; }}};
    Node& menu =
        root.openPopup(makeMenu("menu", items, [&] { root.closePopup(); }), button.bounds());
    root.frame(false);
    CHECK(root.popupOpen());
    CHECK(root.focused() == &menu);
    CHECK(menu.bounds().y >= button.bounds().bottom());  // below its anchor
    root.key(SDLK_DOWN);  // First (index 0) → separator is skipped by keys? select moves to 1
    root.key(SDLK_END);
    root.key(SDLK_RETURN);
    CHECK(ran == 3);
    CHECK_FALSE(root.popupOpen());
    CHECK(root.focused() == &button);  // focus came back
    // Outside click closes without reaching what is under it.
    root.openPopup(makeMenu("menu2", items, [&] { root.closePopup(); }), button.bounds());
    root.frame(false);
    int clicked = 0;
    button.onClick = [&] { ++clicked; };
    root.click("open");
    CHECK_FALSE(root.popupOpen());
    CHECK(clicked == 0);
    root.openPopup(makeMenu("menu3", items, [&] { root.closePopup(); }), button.bounds());
    root.key(SDLK_ESCAPE);
    CHECK_FALSE(root.popupOpen());
    // In a dialog, a press outside its menu closes only the menu, and the
    // button under it does not open another.
    auto dialog = std::make_unique<Node>("dialog");
    dialog->layout.dir = Layout::Dir::Row;  // the menu below "a" leaves "b" clear
    dialog->layout.width = SizeRule::fixed(300);
    dialog->layout.height = SizeRule::fixed(200);
    int menus = 0;
    for (const char* id : {"a", "b"})
        dialog->emplace<Button>(id, id, [&, id] {
            ++menus;
            root.openPopup(makeMenu(std::string(id) + ".menu", items, [&] { root.closePopup(); }),
                           root.find(id)->bounds());
        });
    root.openPopup(std::move(dialog), {}, Placement::Center);
    root.frame(false);
    root.click("a");
    root.frame(false);
    CHECK(menus == 1);
    CHECK(root.find("a.menu"));
    root.click("a");  // the same button closes its menu
    CHECK(menus == 1);
    CHECK_FALSE(root.find("a.menu"));
    CHECK(root.find("dialog"));
    root.click("a");
    root.frame(false);
    root.click("b");  // another button closes it too, without opening its own
    CHECK(menus == 2);
    CHECK_FALSE(root.find("a.menu"));
    CHECK_FALSE(root.find("b.menu"));
    CHECK(root.find("dialog"));
    root.closePopups();
}

TEST_CASE("a scroll bar pages on track clicks and reports drags") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    auto& bar = content->emplace<ScrollBar>("bar", ScrollBar::Axis::Vertical);
    bar.layout.width = SizeRule::fixed(ScrollBar::kThickness);
    float last = -1;
    bar.onScroll = [&](float v) { last = v; };
    root.setContent(std::move(content));
    root.frame(false);
    bar.setRange(1000, 200);
    CHECK(bar.needed());
    const Rect b = bar.bounds();
    root.pointer({PointerEvent::Type::Down, {b.x + 5, b.bottom() - 5}, 1});
    root.pointer({PointerEvent::Type::Up, {b.x + 5, b.bottom() - 5}, 1});
    CHECK(last == doctest::Approx(200));  // one page
    bar.setValue(0);
    root.pointer({PointerEvent::Type::Down, {b.x + 5, b.y + 5}, 1});  // on the thumb
    root.pointer({PointerEvent::Type::Move, {b.x + 5, b.y + 5 + (b.h - 40) / 2}});
    root.pointer({PointerEvent::Type::Up, {b.x + 5, b.y + 5 + (b.h - 40) / 2}, 1});
    CHECK(last == doctest::Approx(400).epsilon(0.02));  // half the travel of an 800-unit range
}

TEST_CASE("a text area edits across lines: vertical motion, indentation, undo, marks") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    auto& area = content->emplace<TextArea>("area", "voice v\n  1: C4:q\nend");
    area.layout.height = SizeRule::fraction();
    std::string submitted;
    int changes = 0;
    area.onChange = [&](const std::string&) { ++changes; };
    area.onSubmit = [&](const std::string& s) { submitted = s; };
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(area.lineCount() == 3);
    root.focus(&area);
    root.key(SDLK_HOME, kCtrl);
    root.key(SDLK_DOWN);
    root.key(SDLK_END);
    CHECK(area.caretLineColumn() == std::pair{2, 10});
    // Enter keeps the indentation.
    root.key(SDLK_RETURN);
    root.text("E4:q");
    CHECK(area.text() == "voice v\n  1: C4:q\n  E4:q\nend");
    // Up keeps the column where it can; shorter lines clamp.
    root.key(SDLK_DOWN);
    CHECK(area.caretLineColumn().first == 4);
    CHECK(area.caretLineColumn().second == 4);  // "end" is 3 long: clamped to its end
    root.key(SDLK_UP);
    CHECK(area.caretLineColumn() == std::pair{3, 7});  // back to the wanted column
    // A selection across lines, replaced.
    root.key(SDLK_HOME, kCtrl);
    root.key(SDLK_DOWN, kShift);
    root.key(SDLK_DOWN, kShift);
    root.text("x\n");
    CHECK(area.text() == "x\n  E4:q\nend");
    // Undo: the replacement, then the typing burst together with the newline.
    root.key(SDLK_Z, kCtrl);
    CHECK(area.text() == "voice v\n  1: C4:q\n  E4:q\nend");
    root.key(SDLK_Z, kCtrl);
    root.key(SDLK_Z, kShift | kCtrl);
    CHECK(changes > 0);
    root.key(SDLK_RETURN, kCtrl);
    CHECK(submitted == area.text());
    // Marks paint and are kept; setText keeps the caret's line.
    area.setMarks({{2, 4, "unknown pitch", true}});
    root.frame(false);
    CHECK(area.marks().size() == 1);
    area.setText("a\nb\nc\nd");
    CHECK(area.lineCount() == 4);
}

TEST_CASE("text area replacement preserves the UTF-8 caret column") {
    TextArea area("text", "é界Z\nold");
    REQUIRE(area.onKey({SDLK_RIGHT, 0}));
    REQUIRE(area.onKey({SDLK_RIGHT, 0}));
    CHECK(area.caretLineColumn() == std::pair{1, 6});  // byte column after two code points
    area.setText("é界Z\nnew");
    CHECK(area.caretLineColumn() == std::pair{1, 6});
    area.setText("abcd\nnew");
    CHECK(area.caretLineColumn() == std::pair{1, 3});  // same code-point column
}

TEST_CASE("mixer controls: fader taper and gestures, pan, meter hold and clip, toggle") {
    // The taper round-trips over its range; -inf at the bottom.
    for (double db = -60; db <= 12; db += 0.5)
        CHECK(faderDb(faderPosition(db)) == doctest::Approx(db).epsilon(1e-9));
    CHECK(faderPosition(kFaderSilenceDb) == 0.0);
    CHECK(faderDb(0.0) == kFaderSilenceDb);
    CHECK(faderPosition(0.0) > 0.6);
    CHECK(faderPosition(0.0) < 0.8);
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.padding = Insets::all(8);
    content->layout.gap = 8;
    auto& fader = content->emplace<Fader>("fader");
    auto& meter = content->emplace<Meter>("meter");
    auto& col = content->emplace<Node>("col");
    col.layout.dir = Layout::Dir::Column;
    col.layout.width = SizeRule::fixed(120);
    auto& pan = col.emplace<PanControl>("pan");
    bool soloed = false;
    auto& solo = col.emplace<Toggle>("solo", "S", "meter-hot", [&](bool on) { soloed = on; });
    std::vector<std::pair<double, Gesture>> calls;
    fader.onChange = [&](double db, Gesture g) { calls.emplace_back(db, g); };
    root.setContent(std::move(content));
    root.frame(false);
    // A drag up: Begin, Moves, End; the value rises; model updates are ignored meanwhile.
    const Rect b = fader.bounds();
    const Point cap{b.x + b.w / 2,
                    b.bottom() - 8 - static_cast<float>(faderPosition(0)) * (b.h - 16)};
    root.pointer({PointerEvent::Type::Down, cap, 1});
    root.pointer({PointerEvent::Type::Move, {cap.x, cap.y - 20}, 1});
    fader.setValue(-30);  // the model echoing an older value mid-drag
    root.pointer({PointerEvent::Type::Move, {cap.x, cap.y - 40}, 1});
    root.pointer({PointerEvent::Type::Up, {cap.x, cap.y - 40}, 1});
    REQUIRE(calls.size() >= 3);
    CHECK(calls.front().second == Gesture::Begin);
    CHECK(calls.back().second == Gesture::End);
    CHECK(fader.value() > 0.5);
    // Double-click resets to unity.
    PointerEvent dbl{PointerEvent::Type::Down, cap, 1};
    dbl.clicks = 2;
    root.pointer(dbl);
    root.pointer({PointerEvent::Type::Up, cap, 1});
    CHECK(fader.value() == 0.0);
    CHECK(calls.back().second == Gesture::Set);
    // Pan: a drag right of the centre pans right.
    const Rect pb = pan.bounds();
    root.pointer({PointerEvent::Type::Down, {pb.x + pb.w / 2, pb.y + 8}, 1});
    root.pointer({PointerEvent::Type::Move, {pb.x + pb.w * 0.75f, pb.y + 8}, 1});
    root.pointer({PointerEvent::Type::Up, {pb.x + pb.w * 0.75f, pb.y + 8}, 1});
    CHECK(pan.value() > 30);
    CHECK(pan.valueText().back() == 'R');
    // Meter: a hot reading shows, clips latch until clicked.
    const float peak[2] = {1.2f, 0.5f}, rms[2] = {0.5f, 0.2f};
    meter.push(peak, rms);
    CHECK(meter.clipped());
    CHECK(meter.shownPeakDb(1) == doctest::Approx(20 * std::log10(0.5)).epsilon(0.01));
    root.frame(false);
    CHECK(root.click("meter"));
    CHECK_FALSE(meter.clipped());
    // Toggle.
    CHECK(root.click("solo"));
    CHECK(soloed);
    CHECK(solo.on());
}

TEST_CASE("a split pan control: two bars, each its own value and home (ADR-0066)") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.padding = Insets::all(8);
    auto& col = content->emplace<Node>("col");
    col.layout.dir = Layout::Dir::Column;
    col.layout.width = SizeRule::fixed(200);
    auto& pan = col.emplace<PanControl>("pan");
    std::vector<std::pair<double, Gesture>> first, second;
    pan.onChange = [&](double v, Gesture g) { first.emplace_back(v, g); };
    pan.onSecondChange = [&](double v, Gesture g) { second.emplace_back(v, g); };
    pan.setSplit(PanControl::Split::Dual);
    pan.setValue(-100);
    pan.setSecond(100);
    root.setContent(std::move(content));
    root.frame(false);
    const Rect b = pan.bounds();
    // A drag in the right half moves the second bar only.
    const Point right{b.x + b.w * 0.9f, b.y + 8};
    root.pointer({PointerEvent::Type::Down, right, 1});
    root.pointer({PointerEvent::Type::Move, {right.x - b.w * 0.2f, right.y}, 1});
    root.pointer({PointerEvent::Type::Up, {right.x - b.w * 0.2f, right.y}, 1});
    CHECK(first.empty());
    REQUIRE_FALSE(second.empty());
    CHECK(pan.second() < 100);
    CHECK(pan.part() == 1);
    CHECK(pan.value() == -100);
    // A double-click in the left half sends dual's left input home: hard left.
    pan.setValue(20);
    PointerEvent dbl{PointerEvent::Type::Down, {b.x + b.w * 0.1f, b.y + 8}, 1};
    dbl.clicks = 2;
    root.pointer(dbl);
    root.pointer({PointerEvent::Type::Up, {b.x + b.w * 0.1f, b.y + 8}, 1});
    REQUIRE_FALSE(first.empty());
    CHECK(first.back().first == -100);
    CHECK(first.back().second == Gesture::Set);
    // Width: the second bar's home is 100%, and the text names both.
    pan.setSplit(PanControl::Split::Width);
    pan.setValue(0);
    pan.setSecond(40);
    CHECK(pan.valueText() == "C, width 40%");
    PointerEvent home{PointerEvent::Type::Down, right, 1};
    home.clicks = 2;
    root.pointer(home);
    root.pointer({PointerEvent::Type::Up, right, 1});
    CHECK(pan.second() == 100);
    // Keys move the chosen bar: down picks the second, up the first.
    root.focus(&pan);
    root.key(SDLK_DOWN);
    root.key(SDLK_LEFT);
    CHECK(pan.second() == 95);
    root.key(SDLK_UP);
    root.key(SDLK_RIGHT);
    CHECK(pan.value() == 5);
    // Unsplit, it is one bar again.
    pan.setSplit(PanControl::Split::None);
    pan.setValue(-30);
    CHECK(pan.valueText() == "30L");
}

TEST_CASE("the normalized slider: relative drags, a default, keys") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.padding = Insets::all(8);
    content->layout.width = SizeRule::fixed(400);
    auto& slider = content->emplace<Slider>("slider");
    slider.layout.width = SizeRule::fixed(300);
    std::vector<std::pair<double, Gesture>> calls;
    slider.onChange = [&](double v, Gesture g) { calls.emplace_back(v, g); };
    root.setContent(std::move(content));
    root.frame(false);
    const Rect b = slider.bounds();
    // A press never jumps the value; it opens the gesture instead.
    slider.setValue(0.6);
    slider.setDefault(0.25);
    root.pointer({PointerEvent::Type::Down, {b.x + 20, b.y + 8}, 1});
    CHECK(slider.value() == doctest::Approx(0.6));
    CHECK(calls.back().second == Gesture::Begin);
    // The drag is relative to the press: +half the travel.
    root.pointer({PointerEvent::Type::Move, {b.x + 20 + 150, b.y + 8}, 1});
    CHECK(calls.back().second == Gesture::Move);
    CHECK(slider.value() > 0.8);
    const double mid = slider.value();
    slider.setValue(0.9);  // the model echoing an older value mid-drag is ignored
    root.pointer({PointerEvent::Type::Move, {b.x + 20 + 150, b.y + 8}, 1});
    CHECK(slider.value() == doctest::Approx(mid).epsilon(1e-9));
    root.pointer({PointerEvent::Type::Up, {b.x + 20 + 150, b.y + 8}, 1});
    CHECK(calls.back().second == Gesture::End);
    // A double click returns to the caller's default.
    PointerEvent dbl{PointerEvent::Type::Down, {b.x + 40, b.y + 8}, 1};
    dbl.clicks = 2;
    root.pointer(dbl);
    root.pointer({PointerEvent::Type::Up, {b.x + 40, b.y + 8}, 1});
    CHECK(slider.value() == doctest::Approx(0.25));
    CHECK(calls.back().second == Gesture::Set);
    // Keys and the wheel are one-shot Sets; Home/End snap to the ends.
    calls.clear();
    root.focus(&slider);
    root.key(SDLK_HOME);
    CHECK(slider.value() == 0.0);
    root.key(SDLK_END);
    CHECK(slider.value() == 1.0);
    root.key(SDLK_LEFT);
    CHECK(slider.value() == doctest::Approx(0.99));
    CHECK(calls.back().second == Gesture::Set);
    CHECK_FALSE(slider.valueText().empty());
}

TEST_CASE("the round knob: vertical relative drag, fine adjustment, readback and reset") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.padding = Insets::all(8);
    auto& knob = content->emplace<Knob>("knob");
    std::vector<std::pair<double, Gesture>> calls;
    knob.onChange = [&](double v, Gesture g) { calls.emplace_back(v, g); };
    root.setContent(std::move(content));
    root.frame(false);
    const Rect b = knob.bounds();
    const Point p{b.x + b.w / 2, b.y + b.h / 2};
    root.pointer({PointerEvent::Type::Down, p, 1});
    CHECK(knob.value() == 0.5);
    CHECK(calls.back().second == Gesture::Begin);
    root.pointer({PointerEvent::Type::Move, {p.x + 100, p.y - 30}, 1});
    CHECK(knob.value() == doctest::Approx(0.7));
    CHECK(calls.back().second == Gesture::Move);
    knob.setValue(0.1);
    CHECK(knob.value() == doctest::Approx(0.7));
    root.pointer({PointerEvent::Type::Up, p, 1});
    CHECK(calls.back().second == Gesture::End);
    knob.setValue(0.5);
    root.pointer({PointerEvent::Type::Down, p, 1});
    root.pointer({PointerEvent::Type::Move, {p.x, p.y - 30}, 1, 1, kShift});
    CHECK(knob.value() == doctest::Approx(0.52));
    root.pointer({PointerEvent::Type::Cancel, p, 1});
    CHECK_FALSE(knob.dragging());
    CHECK(calls.back().second == Gesture::End);
    root.pointer({PointerEvent::Type::Down, p, 1, 2});
    root.pointer({PointerEvent::Type::Up, p, 1});
    CHECK(knob.value() == 0.5);
    CHECK(calls.back().second == Gesture::Set);
    const auto beforeReset = calls.size();
    root.pointer({PointerEvent::Type::Down, p, 1, 2});
    root.pointer({PointerEvent::Type::Up, p, 1});
    CHECK(calls.size() == beforeReset + 1);
    CHECK(calls.back().second == Gesture::Set);
    root.focus(&knob);
    root.key(SDLK_UP);
    CHECK(knob.value() > 0.5);
    root.frame(false);
}

TEST_CASE("a plugin editor switching GL contexts behind SDL's back cannot break the next frame") {
    REQUIRE_GL();
    Fixture& f = fixture();
    Root root(*f.window, f.fonts, Theme::dark());
    auto content = std::make_unique<Node>("content");
    content->layout.dir = Layout::Dir::Row;  // the box fills the window
    Box& box = content->emplace<Box>("box");
    root.setContent(std::move(content));
    root.frame(false);
    REQUIRE(box.paints > 0);
    // What a Pugl editor's timer does on the UI thread: leave GL with another
    // context (here: none) without telling SDL, which then believes ours is
    // still current.
    using MakeCurrent = unsigned (*)(void*, void*, void*, void*);
    auto makeCurrent = reinterpret_cast<MakeCurrent>(SDL_EGL_GetProcAddress("eglMakeCurrent"));
    void* display = SDL_EGL_GetCurrentDisplay();
    if (!makeCurrent || !display) {
        MESSAGE("skipped: the offscreen driver does not use EGL here");
        return;
    }
    REQUIRE(makeCurrent(display, nullptr, nullptr, nullptr));
    const int painted = box.paints;
    f.window->setSize(360, 230);  // a new size needs a new framebuffer
    root.frame(false);
    CHECK(box.paints > painted);
    const gui::Image image = root.readBack();
    CHECK(image.width >= 360);
    // Resized offscreen EGL surfaces may still have their original drawable
    // size. A non-presented frame captures the whole new root, including the
    // newly exposed corner and the top edge, rather than reading past it.
    REQUIRE(image.rgba.size() >= 4);
    CHECK(image.rgba[0] > 0);
    CHECK(image.rgba[image.rgba.size() - 4] > 0);
    f.window->setSize(320, 200);
    root.frame(false);
}

TEST_CASE("widget callbacks survive removing their own node") {
    auto token = std::make_shared<int>(42);
    std::weak_ptr<int> weak = token;
    bool kept = false;
    SUBCASE("button key") {
        auto button = std::make_unique<Button>("self", "B");
        button->onClick = [&, held = token] {
            button.reset();
            kept = !weak.expired();
        };
        token.reset();
        CHECK(button->onKey({SDLK_RETURN}));
    }
    SUBCASE("toggle") {
        auto toggle = std::make_unique<Toggle>("self", "T", "accent");
        toggle->onToggle = [&, held = token](bool) {
            toggle.reset();
            kept = !weak.expired();
        };
        token.reset();
        CHECK(toggle->onKey({SDLK_RETURN}));
    }
    SUBCASE("slider") {
        auto slider = std::make_unique<Slider>("self");
        slider->onChange = [&, held = token](double, Gesture) {
            slider.reset();
            kept = !weak.expired();
        };
        token.reset();
        CHECK(slider->onKey({SDLK_END}));
    }
    SUBCASE("list activation") {
        auto list = std::make_unique<List>("self");
        list->setCount(1);
        list->select(0);
        list->onActivate = [&, held = token](std::size_t) {
            list.reset();
            kept = !weak.expired();
        };
        token.reset();
        CHECK(list->onKey({SDLK_RETURN}));
    }
    SUBCASE("scroll bar") {
        auto bar = std::make_unique<ScrollBar>("self", ScrollBar::Axis::Vertical);
        bar->setRange(100, 20);
        bar->onScroll = [&, held = token](float) {
            bar.reset();
            kept = !weak.expired();
        };
        token.reset();
        CHECK(bar->onPointer({PointerEvent::Type::Down, {1, 1}, 1}));
    }
    SUBCASE("text field submit keeps its text") {
        const std::string expected(200, 'x');
        auto field = std::make_unique<TextField>("self", expected);
        field->onSubmit = [&, held = token](const std::string& text) {
            field.reset();
            kept = !weak.expired();
            CHECK(text == expected);
        };
        token.reset();
        CHECK(field->onKey({SDLK_RETURN}));
    }
    SUBCASE("text field forwarded key") {
        auto field = std::make_unique<TextField>("self");
        field->onOtherKey = [&, held = token](const KeyEvent&) {
            field.reset();
            kept = !weak.expired();
            return true;
        };
        token.reset();
        CHECK(field->onKey({SDLK_DOWN}));
    }
    SUBCASE("text area submit keeps its text") {
        const std::string expected(200, 'x');
        auto area = std::make_unique<TextArea>("self", expected);
        area->onSubmit = [&, held = token](const std::string& text) {
            area.reset();
            kept = !weak.expired();
            CHECK(text == expected);
        };
        token.reset();
        CHECK(area->onKey({SDLK_RETURN, kCtrl}));
    }
    CHECK(kept);
    CHECK(weak.expired());
}

TEST_CASE("collapsed fader and slider tracks keep finite drag values") {
    REQUIRE_GL();
    const Theme theme = Theme::dark();
    Canvas canvas(fixture().window->vg(), fixture().fonts, theme);
    Fader fader("small");
    Slider slider("small");
    for (Node* node : {static_cast<Node*>(&fader), static_cast<Node*>(&slider)}) {
        node->arrange(canvas, {0, 0, 0, 0});
        CHECK(node->onPointer({PointerEvent::Type::Down, {0, 0}, 1}));
        CHECK(node->onPointer({PointerEvent::Type::Move, {0, 0}, 1}));
        CHECK(node->onPointer({PointerEvent::Type::Up, {0, 0}, 1}));
    }
    CHECK(std::isfinite(fader.value()));
    CHECK(std::isfinite(slider.value()));
}

TEST_CASE("text ellipsis removes whole UTF-8 characters") {
    REQUIRE_GL();
    struct Text : Node {
        std::string text;
        float width = 0;
        Text() : Node("text") {}
        void paint(Canvas& c) override {
            c.text({0, 0, width, 30}, text, Font::Ui, 14, c.theme().color("ink"));
        }
    };
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto node = std::make_unique<Text>();
    Text* text = node.get();
    const Theme theme = Theme::dark();
    Canvas canvas(fixture().window->vg(), fixture().fonts, theme);
    text->width = canvas.textWidth("é…", Font::Ui, 14) + 0.25f;
    text->text = "é…";
    root.setContent(std::move(node));
    root.frame(false);
    const auto expected = root.readBack();
    text->text = "éééééé";
    text->invalidate();
    root.frame(false);
    CHECK(pixelDistance(expected, root.readBack()) == 0);
}

TEST_CASE("unchanged button text and scrollbar ranges leave the content cache intact") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("unchanged-controls");
    content->layout.dir = Layout::Dir::Row;
    auto& button = content->emplace<Button>("button", "read");
    auto& bar = content->emplace<ScrollBar>("bar", ScrollBar::Axis::Vertical);
    bar.layout.width = SizeRule::fixed(ScrollBar::kThickness);
    bar.layout.height = SizeRule::fraction();
    bar.setRange(500, 100);
    bar.setValue(20);
    root.setContent(std::move(content));
    root.frame(false);
    const auto repaints = root.stats().repaints;
    button.setText("read");
    bar.setRange(500, 100);
    CHECK_FALSE(root.wantsFrame());
    root.frame(false);
    CHECK(root.stats().repaints == repaints);
    button.setText("write");
    CHECK(root.wantsFrame());
    root.frame(false);
    bar.setRange(10, 100);  // changed range also clamps its value
    CHECK(bar.value() == 0);
    CHECK(root.wantsFrame());
}

TEST_CASE("audit3: subtree release cancels capture without clicking and preserves outside focus") {
    REQUIRE_GL();
    auto& f = fixture();
    Root root(*f.window, f.fonts, Theme::dark());
    auto content = std::make_unique<Node>("content");
    content->layout.dir = Layout::Dir::Column;
    auto& dock = content->emplace<Node>("dock");
    dock.layout.dir = Layout::Dir::Column;
    int clicks = 0, ends = 0;
    auto& button = dock.emplace<Button>("button", "Button", [&] { ++clicks; });
    auto& fader = dock.emplace<Fader>("fader");
    fader.onChange = [&](double, Gesture g) {
        if (g == Gesture::End) ++ends;
    };
    auto& outside = content->emplace<TextField>("outside", "");
    root.setContent(std::move(content));
    root.frame(false);
    auto centre = [](const Node& n) {
        return Point{n.bounds().x + n.bounds().w / 2, n.bounds().y + n.bounds().h / 2};
    };
    const auto b = centre(button);
    root.pointer({PointerEvent::Type::Down, b, 1});
    root.focus(&outside);
    dock.visible = false;
    root.releaseWithin(&dock);
    CHECK(root.focused() == &outside);
    root.pointer({PointerEvent::Type::Up, b, 1});
    CHECK(clicks == 0);
    dock.visible = true;
    root.frame(false);
    auto p = centre(fader);
    root.pointer({PointerEvent::Type::Down, p, 1});
    REQUIRE(fader.dragging());
    fader.enabled = false;
    root.releaseWithin(&dock);
    CHECK_FALSE(fader.dragging());
    CHECK(ends == 1);
    CHECK(root.focused() == nullptr);
    root.releaseWithin(&dock);
    CHECK(ends == 1);
}
TEST_CASE("audit3: popup restoration refuses hidden or disabled prior focus") {
    REQUIRE_GL();
    auto& f = fixture();
    Root root(*f.window, f.fonts, Theme::dark());
    auto content = std::make_unique<Node>("content");
    auto& field = content->emplace<TextField>("field", "");
    root.setContent(std::move(content));
    root.frame(false);
    for (int mode = 0; mode < 4; ++mode) {
        field.visible = true;
        field.enabled = true;
        root.content()->enabled = true;
        root.focus(&field);
        root.openPopup(std::make_unique<TextField>("popup.field", ""), {});
        if (mode == 0)
            field.visible = false;
        else if (mode == 1)
            field.enabled = false;
        if (mode == 3) root.content()->enabled = false;
        root.closePopup();
        CHECK(root.focused() == (mode == 2 ? &field : nullptr));
    }
}

TEST_CASE("audit3: theme colours and sizes are strictly validated") {
    for (const auto* colour : {"#-00001", "#+00001", "# 00001", "#12345g"}) {
        auto doc = json::Value::object();
        doc["schema"] = "supple.theme/1.0";
        doc["colors"] = json::Value::object();
        doc["colors"]["accent"] = colour;
        auto result = ui::Theme::fromJson(doc, ui::Theme::dark());
        CHECK_FALSE(result);
        if (!result) CHECK(result.error().path == "/colors/accent");
        CHECK_FALSE(ui::Theme::dark().trackColor(colour));
    }
    auto result = ui::Theme::fromJson(
        json::parse(R"({"schema":"supple.theme/1.0","type":{"body":1e300}})").value(),
        ui::Theme::dark());
    CHECK_FALSE(result);
    if (!result) CHECK(result.error().path == "/type/body");
}

TEST_CASE("audit3: theme rejects non-finite sizes and escapes error paths") {
    for (double size :
         {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN(), -1.0, 1e-300}) {
        auto doc = json::Value::object();
        doc["schema"] = "supple.theme/1.0";
        doc["radius"] = json::Value::object();
        doc["radius"]["a/b~c"] = size;
        auto result = Theme::fromJson(doc, Theme::dark());
        REQUIRE_FALSE(result);
        CHECK(result.error().path == "/radius/a~1b~0c");
    }
    CHECK(parseColor("#aBcD12") == Color::hex(0xabcd12));
    CHECK_FALSE(parseColor("#1234567"));
    CHECK_FALSE(parseColor("#12345"));
}

TEST_CASE("SDL primary-modifier keys map to the portable shortcut bits") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    unsigned mods = 0;
    root.onUnhandledKey = [&](const KeyEvent& key) {
        mods = key.mods;
        return true;
    };
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_S;
    event.key.mod = SDL_KMOD_GUI;
    root.handle(event);
#if defined(__APPLE__)
    CHECK(mods == kCtrl);
#else
    CHECK(mods == kSuper);
#endif
    event.key.mod = SDL_KMOD_CTRL;
    root.handle(event);
#if defined(__APPLE__)
    CHECK(mods == kSuper);
#else
    CHECK(mods == kCtrl);
#endif
}

TEST_CASE("instructional shortcuts show the platform primary modifier") {
#if defined(__APPLE__)
    CHECK(shortcutLabel("Ctrl+Shift+S / Ctrl+click") == "Cmd+Shift+S / Cmd+click");
#else
    CHECK(shortcutLabel("Ctrl+Shift+S / Ctrl+click") == "Ctrl+Shift+S / Ctrl+click");
#endif
}

TEST_CASE("form controls: dropdown, editable dropdown, checkbox, fold section, checklist") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    auto& format = content->emplace<Dropdown>(
        "format", std::vector<Choice>{{"wav", "WAV"}, {"flac", "FLAC"}, {"mp3", "MP3", false}},
        "wav");
    auto& rate = content->emplace<EditableDropdown>(
        "rate", std::vector<Choice>{{"44100", "44100"}, {"48000", "48000"}}, "", "Project");
    auto& box = content->emplace<Checkbox>("box", "Only turn down");
    auto& fold = content->emplace<FoldSection>("fold", "Loudness");
    fold.body().emplace<Label>("fold.inner", "inside");
    auto& list = content->emplace<Checklist>("list");
    root.setContent(std::move(content));
    std::vector<std::string> changes;
    format.onChange = [&](const std::string& v) { changes.push_back(v); };
    rate.onChange = [&](const std::string& v) { changes.push_back("rate " + v); };
    bool toggled = false;
    box.onToggle = [&](bool on) { toggled = on; };
    std::vector<std::string> checked;
    list.onChange = [&](const std::vector<std::string>& c) { checked = c; };
    list.setItems({{"tr_1", "Drums"}, {"tr_2", "Bass"}}, {"tr_2"});
    root.frame(false);
    // The dropdown opens a menu with the current choice checked; a pick calls back.
    CHECK(format.valueText() == "WAV");
    root.click("format");
    root.frame(false);
    Node* menu = root.find("format.menu");
    REQUIRE(menu);
    auto items = menuItems(*menu);
    REQUIRE(items.size() == 3);
    CHECK(items[0].checked);
    CHECK_FALSE(items[2].enabled);
    items[1].run();
    CHECK(changes == std::vector<std::string>{"flac"});
    CHECK(format.value() == "flac");
    root.closePopups();
    format.setValue("wav");  // from the owner: no callback
    CHECK(changes.size() == 1);
    // The editable dropdown: typed text is free; a suggestion fills it.
    CHECK(rate.field().placeholder() == "Project");
    rate.setPlaceholder("Project (48000)");
    CHECK(rate.field().placeholder() == "Project (48000)");
    root.click("rate.pick");
    root.frame(false);
    REQUIRE(root.find("rate.menu"));
    menuItems(*root.find("rate.menu"))[0].run();
    CHECK(rate.text() == "44100");
    CHECK(changes.back() == "rate 44100");
    root.closePopups();
    // The checkbox toggles on a click, not from the owner.
    root.click("box");
    CHECK(toggled);
    CHECK(box.checked());
    box.setChecked(false);
    CHECK(toggled);
    // The fold section hides its body and shows its summary.
    CHECK_FALSE(fold.isOpen());
    CHECK_FALSE(fold.body().visible);
    fold.setSummary("-14 LUFS");
    root.click("fold.toggle");
    CHECK(fold.isOpen());
    CHECK(fold.body().visible);
    // The checklist: All / None and single boxes report the checked values.
    CHECK(list.valueText() == "tr_2");
    root.frame(false);
    root.click("list.all");
    CHECK(checked == std::vector<std::string>{"tr_1", "tr_2"});
    root.click("list.none");
    CHECK(checked.empty());
    root.frame(false);
    root.click("list.item.0");
    CHECK(checked == std::vector<std::string>{"tr_1"});
}

TEST_CASE("a control's drag reaches its callback once per frame, the latest position first") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.padding = Insets::all(8);
    auto& fader = content->emplace<Fader>("fader");
    int plainMoves = 0;
    struct Probe : Node {
        int* moves;
        Probe(int* m) : Node("probe"), moves(m) { interactive = true; }
        bool onPointer(const PointerEvent& e) override {
            if (e.type == PointerEvent::Type::Down) {
                if (Root* r = root()) r->capture(this);
                return true;
            }
            if (e.type == PointerEvent::Type::Move) ++*moves;
            return true;
        }
    };
    auto& probe = content->emplace<Probe>(&plainMoves);
    probe.layout.width = SizeRule::fixed(100);
    probe.layout.height = SizeRule::fixed(40);
    std::vector<std::pair<double, Gesture>> calls;
    fader.onChange = [&](double db, Gesture g) { calls.emplace_back(db, g); };
    root.setContent(std::move(content));
    root.frame(false);
    const float scale = fixture().window->inputScale();
    auto sdl = [&](SDL_EventType type, Point p) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            e.motion.x = p.x * scale;
            e.motion.y = p.y * scale;
        } else {
            e.button.button = SDL_BUTTON_LEFT;
            e.button.clicks = 1;
            e.button.x = p.x * scale;
            e.button.y = p.y * scale;
        }
        root.handle(e);
    };
    const Rect b = fader.bounds();
    const Point cap{b.x + b.w / 2,
                    b.bottom() - 8 - static_cast<float>(faderPosition(0)) * (b.h - 16)};
    sdl(SDL_EVENT_MOUSE_BUTTON_DOWN, cap);
    REQUIRE(calls.size() == 1);
    CHECK(calls.back().second == Gesture::Begin);
    for (float dy : {10.f, 20.f, 30.f}) sdl(SDL_EVENT_MOUSE_MOTION, {cap.x, cap.y - dy});
    CHECK(calls.size() == 1);  // held for the frame
    CHECK(root.motionPending());
    CHECK(root.wantsFrame());
    root.frame(false);
    REQUIRE(calls.size() == 2);
    CHECK(calls.back().second == Gesture::Move);
    const double top = calls.back().first;
    CHECK(top > 0);
    // A release follows the drag's latest position.
    sdl(SDL_EVENT_MOUSE_MOTION, {cap.x, cap.y - 35});
    sdl(SDL_EVENT_MOUSE_MOTION, {cap.x, cap.y - 40});
    sdl(SDL_EVENT_MOUSE_BUTTON_UP, {cap.x, cap.y - 40});
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].second == Gesture::Move);
    CHECK(calls[2].first > top);
    CHECK(calls[3].second == Gesture::End);
    CHECK_FALSE(root.motionPending());
    // A capture that does not coalesce sees every motion.
    const Rect pb = probe.bounds();
    REQUIRE(pb.h > 10);
    sdl(SDL_EVENT_MOUSE_BUTTON_DOWN, {pb.x + 5, pb.y + 5});
    for (float dx : {1.f, 2.f, 3.f}) sdl(SDL_EVENT_MOUSE_MOTION, {pb.x + 5 + dx, pb.y + 5});
    CHECK(plainMoves == 3);
    sdl(SDL_EVENT_MOUSE_BUTTON_UP, {pb.x + 8, pb.y + 5});
    // A control hidden mid-drag takes no more of it.
    sdl(SDL_EVENT_MOUSE_BUTTON_DOWN, cap);
    const auto before = calls.size();
    sdl(SDL_EVENT_MOUSE_MOTION, {cap.x, cap.y - 50});
    root.releaseWithin(&fader);
    CHECK_FALSE(root.motionPending());
    root.frame(false);
    REQUIRE(calls.size() == before + 1);
    CHECK(calls.back().second == Gesture::End);
}
