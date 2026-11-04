// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The arrange view (gui.md §4.3) over a real session built through the
// command bus (the same commands an agent uses), drawn offscreen: golden
// images for the ruler and the item states, and scripted tests for zoom,
// alignment, the playhead overlay and virtualization.
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "supple/base/files.h"
#include "test_support.h"
#include "ui_persistence.h"

#include "../app/generate.h"
#include "generated_media.h"
#include "meter_feed.h"
#include "supple/engine/app.h"
#include "supple/gui/gl_api.h"
#include "supple/gui/png.h"
#include "supple/model/session.h"
#include "supple/model/source_mapping.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/views/arrange.h"
#include "supple/views/envelope_edit.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/mouse_map.h"

using namespace supple;

namespace {

// One offscreen window shared by the tests (GL contexts are not free).
struct Gl {
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    bool ok = false;
    Gl() {
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"arrange-test", 1000, 560, true});
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

Gl& gl() {
    static Gl instance;
    return instance;
}

// A session built through the bus, with a peak service.
struct Session {
    std::filesystem::path dir;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<peaks::PeakService> peaks;
    std::vector<std::function<void()>> posted;
    std::mutex m;

    bool build(int trackCount) {
        dir = std::filesystem::temp_directory_path() /
              ("supple-arrange-test-" + std::to_string(testProcessId()) + "-" +
               std::to_string(trackCount));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        engine::AppOptions options;
        options.workdir = dir;
        auto a = engine::EngineApp::create(options);
        if (!a) return false;
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        gui::test::linkGeneratedMedia(app->mediaDir());
        if (!gui::generateSession(*dispatcher, app->mediaDir(), trackCount)) return false;
        peaks =
            std::make_unique<peaks::PeakService>(dir / "cache", [this](std::function<void()> fn) {
                std::lock_guard lock(m);
                posted.push_back(std::move(fn));
            });
        return true;
    }
    ~Session() {
        peaks.reset();
        dispatcher.reset();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void drain() {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard lock(m);
            now.swap(posted);
        }
        for (auto& fn : now) fn();
    }
    views::ArrangeView::Resolve resolve() const {
        return [this](const model::MediaSource& src) {
            const std::filesystem::path p(src.path);
            return p.is_absolute() ? p : app->mediaDir() / p;
        };
    }
};

Session& session6() {
    static Session s;
    static const bool once = s.build(6);
    (void)once;
    return s;
}

// Frames until the view has every visible pyramid. A deadline, not a frame
// count: under a parallel ctest the peak worker can take several seconds.
void settle(ui::Root& root, views::Lanes& lanes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        session6().drain();
        root.damageAll();
        root.frame(false);
        if (lanes.stats().pending == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("peaks never became ready");
}

views::ArrangeView* mount(ui::Root& root) {
    Session& s = session6();
    auto arrange =
        std::make_unique<views::ArrangeView>(*s.peaks, s.resolve(), views::TransportSource{});
    arrange->show(s.app->head());
    views::ArrangeView* view = arrange.get();
    root.setContent(std::move(arrange));
    return view;
}

// Golden images. The session is shared across the suite (and some tests edit
// it: renames, mute/solo, faders), so regenerate goldens with a FULL run
// (SUPPLE_UPDATE_GOLDEN=1, no -tc filter) — a single-test run would write them
// from a different session state.
void golden(ui::Root& root, const char* name) {
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
    CHECK(off < image.rgba.size() / 4 / 200);  // < 0.5 % of pixels
}

}  // namespace

TEST_CASE("the arrange view draws every strip's items where the engine plays them") {
    Gl& g = gl();
    if (!g.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 12;  // 768 px ≈ 64 s: the audio and MIDI items
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    CHECK(arrange->lanes().stats().rows == 6);
    CHECK(arrange->lanes().stats().items == 12);  // an audio and a MIDI item per strip
    CHECK(arrange->lanes().stats().notes == 6 * 64);
    CHECK(arrange->lanes().stats().columns > 6 * 300);
    CHECK(arrange->lanes().stats().fades == 12);  // fade-in and fade-out per audio item
    CHECK(s.peaks->stats().built == 6);
    CHECK(arrange->trackLayout().rowCount() == 6);
}

TEST_CASE("rows and items outside the viewport are not drawn") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->trackLayout().setGlobalHeight(150);
    views::Viewport v;
    v.startSeconds = 40;  // the MIDI items only (bar 21 = 40 s at 120 BPM)
    v.pixelsPerSecond = 40;
    v.scrollY = 1e6f;  // clamped to the bottom: only the last rows show
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    CHECK(arrange->lanes().stats().rows >= 1);
    CHECK(arrange->lanes().stats().rows < 6);  // the top rows scrolled away
    CHECK(arrange->lanes().stats().items > 0);
    CHECK(arrange->lanes().stats().notes > 0);
    CHECK(arrange->lanes().stats().columns == 0);  // the audio items end before 40 s
    CHECK(arrange->rowRect(0).bottom() <= arrange->lanesRect().y);  // row 1 is gone
}

TEST_CASE("the ruler adapts its tick density to the zoom without overlapping labels") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    auto frame = [&](double pps) {
        views::Viewport v;
        v.startSeconds = 0;
        v.pixelsPerSecond = pps;
        arrange->setViewport(v);
        root.damageAll();
        root.frame(false);
        return arrange->rulerStats();
    };
    const views::RulerStats bars = frame(4);     // out: bars only
    const views::RulerStats beats = frame(160);  // in: beats labelled
    const views::RulerStats subs = frame(400);   // closer: sub-beat ticks
    CHECK(bars.beatLabels == 0);
    CHECK(bars.subTicks == 0);
    CHECK(bars.barLabels > 2);
    CHECK(beats.beatLabels > 0);
    CHECK(subs.subTicks > beats.subTicks);
    // No two labels can touch: every gap clears the previous label's width by
    // at least the 12-unit gap the ruler enforces.
    CHECK(bars.minBarSlack >= 0.0f);
    CHECK(beats.minBarSlack >= 0.0f);
    CHECK(subs.minBarSlack >= 0.0f);
    CHECK(bars.minTimeSlack >= 0.0f);
    CHECK(beats.minTimeSlack >= 0.0f);
    CHECK(subs.minTimeSlack >= 0.0f);
    CHECK(bars.minTimeLabelGap > 30.0f);
    CHECK(subs.minTimeLabelGap > 30.0f);
}

TEST_CASE("the ruler shows the tempo and meter changes the session carries") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    const time::TimeMap& tm = *s.app->head().timeMap;
    REQUIRE(tm.tempo().size() == 2);
    CHECK(tm.tempo()[1].bpm == doctest::Approx(140.0));
    CHECK(tm.tempo()[1].beat == doctest::Approx(128.0));
    // Bar 35 (the meter change) is where the 3/4 begins.
    const auto meter = tm.meterAt(35);
    CHECK(meter.numerator == 3);
    CHECK(meter.denominator == 4);
    CHECK(tm.meterAt(34).numerator == 4);
}

TEST_CASE(
    "wheel zoom anchors at the project cursor by default, or under the pointer; keys at the "
    "playhead") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    views::Viewport v;
    v.startSeconds = 40;
    v.pixelsPerSecond = 40;
    arrange->setFollow(views::Follow::Off);  // moving the cursor must not move the view
    arrange->setViewport(v);
    arrange->setPlayhead(50);
    root.damageAll();
    root.frame(false);
    const double before = arrange->viewport().pixelsPerSecond;
    const double time = arrange->timeAt(600);
    ui::PointerEvent wheel{ui::PointerEvent::Type::Wheel, {600, 300}};  // anywhere on the lanes
    wheel.wheelY = 1;
    // The default: the cursor stays where it is on screen, wherever the pointer is.
    CHECK(arrange->gestures().mouse.behaviour("wheel.zoom", 0) == "project_cursor");
    const float cursorX = arrange->playheadX();
    root.pointer(wheel);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(before * 1.25));
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(cursorX)).epsilon(0.001));
    CHECK(arrange->timeAt(600) != doctest::Approx(time).epsilon(0.001));
    wheel.wheelY = -1;  // and back out
    root.pointer(wheel);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(before));
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(cursorX)).epsilon(0.001));
    // The other setting: the time under the mouse pointer stays put.
    REQUIRE(arrange->gestures().mouse.set("wheel.zoom", 0, "mouse_pointer"));
    wheel.wheelY = 1;
    root.pointer(wheel);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(before * 1.25));
    CHECK(arrange->timeAt(600) == doctest::Approx(time).epsilon(0.001));
    wheel.wheelY = -1;
    root.pointer(wheel);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(before));
    // A cursor off screen cannot be held in place: the pointer anchors, as it
    // would with the other setting.
    REQUIRE(arrange->gestures().mouse.set("wheel.zoom", 0, "project_cursor"));
    arrange->setPlayhead(5);  // far left of the view (it starts at 40 s)
    root.frame(false);
    REQUIRE(arrange->playheadX() < arrange->lanesRect().x);
    wheel.wheelY = 1;
    root.pointer(wheel);
    CHECK(arrange->timeAt(600) == doctest::Approx(time).epsilon(0.001));
    arrange->setPlayhead(50);
    const float playhead = arrange->playheadX();
    arrange->zoomStep(true);
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(playhead)));
}

TEST_CASE("zooming back in returns the anchor to where the timeline's start pulled it from") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    views::Viewport v;
    v.startSeconds = 40;
    v.pixelsPerSecond = 40;
    arrange->setFollow(views::Follow::Off);
    arrange->setViewport(v);
    arrange->setPlayhead(50);
    root.damageAll();
    root.frame(false);
    const float cursorX = arrange->playheadX();
    ui::PointerEvent wheel{ui::PointerEvent::Type::Wheel, {600, 300}};
    // Out until the start is a wall: the cursor slides left.
    wheel.wheelY = -1;
    for (int i = 0; i < 10; ++i) root.pointer(wheel);
    CHECK(arrange->viewport().startSeconds == 0);
    CHECK(arrange->playheadX() < cursorX - 50);
    // Back in step by step: the cursor moves right again, never past where it was,
    // and is back there at the scale it started from.
    wheel.wheelY = 1;
    float last = arrange->playheadX();
    for (int i = 0; i < 10; ++i) {
        root.pointer(wheel);
        CHECK(arrange->playheadX() >= last - 0.01f);
        CHECK(arrange->playheadX() <= cursorX + 0.01f);
        last = arrange->playheadX();
    }
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(40));
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(cursorX)).epsilon(0.001));
    // The keys hold the same anchor.
    for (int i = 0; i < 10; ++i) arrange->zoomStep(false);
    for (int i = 0; i < 10; ++i) arrange->zoomStep(true);
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(cursorX)).epsilon(0.001));
    // Anything else that moves the view in time starts afresh: out to the
    // wall again, a scroll right and back, and the cursor stays where it now is.
    wheel.wheelY = -1;
    for (int i = 0; i < 10; ++i) root.pointer(wheel);
    const float pulled = arrange->playheadX();
    REQUIRE(pulled < cursorX - 50);
    ui::PointerEvent scroll{ui::PointerEvent::Type::Wheel, {600, 300}};
    scroll.mods = ui::kShift;
    scroll.wheelY = -1;
    root.pointer(scroll);
    REQUIRE(arrange->viewport().startSeconds > 0);
    scroll.wheelY = 1;
    root.pointer(scroll);
    REQUIRE(arrange->viewport().startSeconds == doctest::Approx(0));
    REQUIRE(arrange->playheadX() == doctest::Approx(static_cast<double>(pulled)));
    wheel.wheelY = 1;
    root.pointer(wheel);
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(pulled)).epsilon(0.001));
    // The pointer anchor too: the time under a still pointer comes back to it.
    REQUIRE(arrange->gestures().mouse.set("wheel.zoom", 0, "mouse_pointer"));
    arrange->setViewport(v);
    const double underPointer = arrange->timeAt(600);
    wheel.wheelY = -1;
    for (int i = 0; i < 10; ++i) root.pointer(wheel);
    CHECK(arrange->viewport().startSeconds == 0);
    wheel.wheelY = 1;
    for (int i = 0; i < 10; ++i) root.pointer(wheel);
    CHECK(arrange->timeAt(600) == doctest::Approx(underPointer).epsilon(0.001));
}

TEST_CASE("the wheel zoom anchor is a single setting that the mouse file keeps") {
    views::MouseMap m;
    const views::MouseMap::Context* c = views::MouseMap::context("wheel.zoom");
    REQUIRE(c);
    CHECK_FALSE(c->modifiers);
    CHECK(m.behaviour("wheel.zoom", 0) == "project_cursor");
    CHECK(m.toJson()["contexts"].asObject().empty());  // the default is not written
    REQUIRE(m.set("wheel.zoom", 0, "mouse_pointer"));
    CHECK_FALSE(m.set("wheel.zoom", 0, "the_moon"));
    auto back = views::MouseMap::fromJson(m.toJson());
    REQUIRE(back);
    CHECK(back->behaviour("wheel.zoom", 0) == "mouse_pointer");
}

TEST_CASE("the TCP column and the lanes stay aligned through scroll and height changes") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    auto aligned = [&] {
        for (std::size_t i = 0; i < arrange->trackLayout().rowCount(); ++i) {
            const ui::Rect lanes = arrange->rowRect(i);
            const ui::Rect tcp = arrange->tcpRowRect(i);
            CHECK(lanes.y == tcp.y);
            CHECK(lanes.h == tcp.h);
            CHECK(lanes.x != tcp.x);  // different columns, same rows
        }
    };
    views::Viewport v;
    v.pixelsPerSecond = 40;
    v.scrollY = 120;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    aligned();
    arrange->trackLayout().setGlobalHeight(48);
    arrange->trackLayout().setTrackHeight(arrange->trackLayout().rows()[1].track, 120);
    root.damageAll();
    root.frame(false);
    aligned();
    CHECK(arrange->rowRect(1).h == doctest::Approx(120));
    CHECK(arrange->rowRect(0).h == doctest::Approx(48));
}

TEST_CASE("the scrollbars move the view") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    ui::Node* hbar = root.find("hscroll");
    ui::Node* vbar = root.find("vscroll");
    REQUIRE(hbar);
    REQUIRE(vbar);
    // The generated session is longer than the view: the horizontal bar is live.
    const ui::Rect hb = hbar->bounds();
    const double start = arrange->viewport().startSeconds;
    const ui::Point hmid{hb.x + 4, hb.y + hb.h / 2};
    const ui::Point hend{hb.right() - 4, hb.y + hb.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, hmid, 1});
    root.pointer({ui::PointerEvent::Type::Move, hend});
    root.pointer({ui::PointerEvent::Type::Up, hend, 1});
    CHECK(arrange->viewport().startSeconds > start);
    // Taller rows overflow the view: the vertical bar scrolls them.
    arrange->trackLayout().setGlobalHeight(150);
    root.damageAll();
    root.frame(false);
    CHECK(arrange->trackLayout().contentHeight() > arrange->lanesRect().h);
    const ui::Rect vb = vbar->bounds();
    const ui::Point vtop{vb.x + vb.w / 2, vb.y + 4};
    const ui::Point vbottom{vb.x + vb.w / 2, vb.bottom() - 4};
    root.pointer({ui::PointerEvent::Type::Down, vtop, 1});
    root.pointer({ui::PointerEvent::Type::Move, vbottom});
    root.pointer({ui::PointerEvent::Type::Up, vbottom, 1});
    CHECK(arrange->viewport().scrollY > 0);
}

TEST_CASE("the arrange view's saved UI state round-trips") {
    // What the per-project UI file stores (gui.md §1.2): the follow mode by
    // name and the per-track height overrides.
    for (views::Follow mode :
         {views::Follow::Off, views::Follow::Page, views::Follow::Continuous}) {
        const auto name = std::string(views::followName(mode));
        REQUIRE(views::followFrom(name).has_value());
        CHECK(*views::followFrom(name) == mode);
    }
    CHECK_FALSE(views::followFrom("sideways").has_value());
    views::TrackLayout layout;
    layout.setGlobalHeight(48);
    model::Id track{model::Kind::Track, 1};
    layout.setTrackHeight(track, 120);
    REQUIRE(layout.heights().size() == 1);
    CHECK(layout.heights().at(track) == doctest::Approx(120));
    views::TrackLayout restored;
    restored.setGlobalHeight(48);
    for (const auto& [id, h] : layout.heights()) restored.setTrackHeight(id, h);
    CHECK(restored.heights() == layout.heights());
}

TEST_CASE("track heights clamp to a complete TCP top line and resize all rows additively") {
    model::EditTxn txn(model::Session::create(), 1);
    std::vector<model::Id> tracks;
    for (int i = 0; i < 5; ++i) {
        model::Track track;
        track.parent = model::kMaster;
        if (i == 3) track.folderCompact = 1;
        if (i == 4) track.hiddenTcp = true;
        tracks.push_back(txn.create(track));
    }
    txn.modify<model::Track>(model::kMaster, [&](auto& t) { t.children = tracks; });
    views::TrackLayout layout;
    const auto session = txn.preview();
    layout.show(session);
    const float min = views::TrackLayout::kMinHeight;
    CHECK(min >= views::TcpRowGeometry::kTopLineBottom + views::TcpRowGeometry::kResizeGrip);
    layout.setGlobalHeight(8);
    CHECK(layout.globalHeight() == min);
    CHECK(layout.heightOf(*layout.indexOf(tracks[3])) == min);  // compact also clamps
    layout.setGlobalHeight(72);
    layout.setTrackHeight(tracks[0], 8);
    layout.setTrackHeight(tracks[1], 398);
    layout.setTrackHeight(tracks[2], 120);
    layout.setTrackHeight(tracks[4], 70);
    int changes = 0;
    layout.onChanged = [&] { ++changes; };
    layout.adjustAllTrackHeights(8);
    CHECK(changes == 1);
    CHECK(layout.heightOf(*layout.indexOf(tracks[0])) == min + 8);
    CHECK(layout.heightOf(*layout.indexOf(tracks[1])) == views::TrackLayout::kMaxHeight);
    CHECK(layout.heightOf(*layout.indexOf(tracks[2])) == 128);
    CHECK(layout.heightOf(*layout.indexOf(tracks[3])) == 44);  // compact grows by eight, too
    CHECK(layout.heights().at(tracks[4]) == 78);               // hidden tracks participate
    layout.adjustAllTrackHeights(-1000);
    for (const auto id : tracks) CHECK(layout.heights().at(id) == min);
    layout.adjustAllTrackHeights(0.5f);
    for (const auto id : tracks) CHECK(layout.heights().at(id) == min + 0.5f);

    // An expanded row has several take lanes: its total height gets the same step.
    model::Item item;
    item.track = tracks[2];
    item.takes = {model::Id{model::Kind::Take, 1}, model::Id{model::Kind::Take, 2}};
    const auto id = txn.create(item);
    layout.show(txn.preview(), nullptr, 1);
    layout.setTrackHeight(tracks[2], 72);
    layout.setTakesExpanded(id, true);
    const float expanded = layout.heightOf(*layout.indexOf(tracks[2]));
    layout.adjustAllTrackHeights(8);
    CHECK(layout.heightOf(*layout.indexOf(tracks[2])) ==
          doctest::Approx(static_cast<double>(expanded) + 8));
}

TEST_CASE("track wheel gestures resize mixed rows additively, anchor and can be rebound") {
    Gl& g = gl();
    REQUIRE(g.ok);
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto* arrange = mount(root);
    auto& layout = arrange->trackLayout();
    layout.setGlobalHeight(120);
    const auto track = layout.rows()[1].track;
    layout.setTrackHeight(track, 160);
    arrange->setViewport({0, 40, 80});
    root.frame(false);
    const auto row = arrange->tcpRowRect(1);
    ui::PointerEvent wheel{ui::PointerEvent::Type::Wheel, {row.x + 100, row.y + row.h / 2}};
    wheel.mods = ui::kCtrl;
    wheel.wheelY = 0.25f;
    const auto version = arrange->session().version;
    root.pointer(wheel);
    root.frame(false);
    CHECK(layout.globalHeight() == 122);
    CHECK(layout.heightOf(*layout.indexOf(track)) == 162);
    const auto after = arrange->tcpRowRect(1);
    CHECK(after.y + after.h / 2 == doctest::Approx(static_cast<double>(wheel.pos.y)));
    CHECK(arrange->session().version == version);
    const auto scroll = arrange->viewport().scrollY;
    wheel.pos.x = arrange->lanesRect().x + 100;
    root.pointer(wheel);
    CHECK(layout.globalHeight() == 122);
    CHECK(layout.heightOf(*layout.indexOf(track)) == 162);
    CHECK(arrange->viewport().scrollY < scroll);  // lanes retain Ctrl+wheel scroll
    wheel.mods = ui::kAlt;
    wheel.wheelY = 1;
    const auto laneRow = arrange->rowRect(1);
    const float fraction = (wheel.pos.y - laneRow.y) / laneRow.h;
    root.pointer(wheel);
    root.frame(false);
    CHECK(layout.globalHeight() == 130);
    CHECK(layout.heightOf(*layout.indexOf(track)) == 170);
    auto anchored = arrange->rowRect(1);
    CHECK(anchored.y + fraction * anchored.h == doctest::Approx(static_cast<double>(wheel.pos.y)));
    wheel.wheelY = -0.25f;
    root.pointer(wheel);
    root.frame(false);
    CHECK(layout.globalHeight() == 128);
    CHECK(layout.heightOf(*layout.indexOf(track)) == 168);
    anchored = arrange->rowRect(1);
    CHECK(anchored.y + fraction * anchored.h == doctest::Approx(static_cast<double>(wheel.pos.y)));
    CHECK(arrange->session().version == version);
    wheel.wheelY = 1;
    wheel.pos.x = row.x + 100;
    root.pointer(wheel);
    CHECK(layout.globalHeight() == 136);  // Alt resizes over the TCP too
    CHECK(layout.heightOf(*layout.indexOf(track)) == 176);
    REQUIRE(arrange->gestures().mouse.set("tcp.wheel", ui::kCtrl, "scroll"));
    auto saved = views::MouseMap::fromJson(arrange->gestures().mouse.toJson());
    REQUIRE(saved);
    CHECK(saved->behaviour("tcp.wheel", ui::kCtrl) == "scroll");
    CHECK(saved->behaviour("tcp.wheel", ui::kAlt) == "resize_all");
    CHECK(saved->behaviour("tcp.wheel", ui::kCtrl | ui::kShift) == "scroll");  // the plain wheel
    wheel.mods = ui::kCtrl;
    root.pointer(wheel);
    CHECK(layout.globalHeight() == 136);
    // Over the ruler too, with no row to anchor.
    wheel.mods = ui::kAlt;
    wheel.pos = {arrange->lanesRect().x + 100, arrange->lanesRect().y - 10};
    root.pointer(wheel);
    CHECK(layout.globalHeight() == 144);
    CHECK(layout.heightOf(*layout.indexOf(track)) == 184);
}

TEST_CASE("retired TCP wheel zoom settings load with current defaults") {
    const auto doc = json::parse(R"({"schema":"supple.mouse/1.0","contexts":{
        "tcp.wheel":{"ctrl":"zoom_height","ctrl+shift":"zoom_height","alt":"resize_all"},
        "item.drag":{"alt":"copy"}}})");
    REQUIRE(doc);
    const auto loaded = views::MouseMap::fromJson(*doc);
    REQUIRE(loaded);
    CHECK(loaded->behaviour("tcp.wheel", ui::kCtrl) == "resize_all");
    CHECK(loaded->behaviour("tcp.wheel", ui::kCtrl | ui::kShift) == "scroll");
    CHECK(loaded->behaviour("tcp.wheel", ui::kAlt) == "resize_all");
    CHECK(loaded->behaviour("item.drag", ui::kAlt) == "copy");
    CHECK_FALSE(loaded->toJson()["contexts"]["tcp.wheel"].find("ctrl+shift"));
    auto edited = *loaded;
    CHECK_FALSE(edited.set("tcp.wheel", ui::kShift, "zoom_height"));
}

TEST_CASE("an old arrange sidecar clamps tiny heights and keeps the TCP top line visible") {
    Gl& g = gl();
    REQUIRE(g.ok);
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto* arrange = mount(root);
    const auto track = arrange->trackLayout().rows()[0].track;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-height-sidecar-" + std::to_string(testProcessId()));
    std::filesystem::create_directories(dir);
    const auto project = dir / "song.json";
    REQUIRE(writeFileAtomic(
        gui::UiPersistence::fileFor(project),
        "{\"schema\":\"supple.ui/1.0\",\"arrange\":{\"row_height\":8,\"track_heights\":{\"" +
            track.str() + "\":9}}}"));
    {
        gui::UiPersistence persistence;
        persistence.add(
            "arrange", [&] { return arrange->saveLayout(); },
            [&](const auto& v) { arrange->loadLayout(v); });
        persistence.tick(project);
        CHECK(arrange->trackLayout().globalHeight() == views::TrackLayout::kMinHeight);
        CHECK(arrange->trackLayout().heightOf(*arrange->trackLayout().indexOf(track)) ==
              views::TrackLayout::kMinHeight);
        // Keep the first row minimal beside normal rows for the real-render check.
        arrange->trackLayout().setGlobalHeight(72);
        root.frame(false);
        const auto row = arrange->tcpRowRect(0);
        for (const auto* name : {"name", "fx", "addfx"}) {
            const auto* control = arrange->tcpPanel().controlNode(track, name);
            REQUIRE(control);
            CHECK(control->visible);
            CHECK(control->bounds().y >= row.y);
            CHECK(control->bounds().bottom() <= row.bottom() - views::TcpRowGeometry::kResizeGrip);
        }
        for (const auto* name : {"mute", "solo", "fader", "pan", "input", "out"}) {
            const auto* control = arrange->tcpPanel().controlNode(track, name);
            REQUIRE(control);
            CHECK_FALSE(control->visible);
        }
        if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
            REQUIRE(
                gui::writePng(std::filesystem::path(out) / "minimum-height.png", root.readBack()));
        REQUIRE(persistence.flush(project));
    }
    arrange->trackLayout().setTrackHeight(track, 120);
    {
        gui::UiPersistence persistence;
        persistence.add(
            "arrange", [&] { return arrange->saveLayout(); },
            [&](const auto& v) { arrange->loadLayout(v); });
        persistence.tick(project);
        CHECK(arrange->trackLayout().heightOf(*arrange->trackLayout().indexOf(track)) ==
              views::TrackLayout::kMinHeight);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("the playhead overlay moves without damaging the content") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    REQUIRE(arrange->installOverlay(root));
    arrange->setFollow(views::Follow::Off);
    views::Viewport v;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    const std::uint64_t repaints = root.stats().repaints;
    const double area = root.stats().repaintedArea;
    const std::uint64_t frames = root.stats().frames;
    for (int i = 1; i <= 30; ++i) {
        arrange->setPlayhead(0.25 * i);
        CHECK(root.wantsFrame());  // the overlay asks for a frame...
        root.frame(false);
    }
    CHECK(root.stats().repaints == repaints);  // ...that repaints no content
    CHECK(root.stats().repaintedArea == area);
    CHECK(root.stats().frames == frames + 30);
    CHECK(arrange->playhead() == doctest::Approx(7.5));
}

TEST_CASE("the zoom keys' step keeps the playhead's time in place") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->setViewport({10, 40, 0});
    arrange->setPlayhead(12.0);
    const float x = arrange->playheadX();
    arrange->zoomStep(true);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(50));
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(x)));
    arrange->zoomStep(false);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(40));
}

TEST_CASE("the arrangement renders a frame with 1000 generated tracks") {
    Gl& g = gl();
    if (!g.ok) return;
    // Built in one transaction: 1000 items through the bus would decode the
    // same 30 s file 1000 times (item.add reads it per call). The rows, the
    // item index and the draw path are the same the generate path produces.
    model::Session s = model::Session::create();
    model::EditTxn txn(s, 1);
    for (int i = 0; i < 1000; ++i) {
        model::Track t;
        t.type = model::StripType::Channel;
        t.parent = model::kMaster;
        t.name = "Track " + std::to_string(i + 1);
        const model::Id tid = txn.create(t);
        txn.modify<model::Track>(model::kMaster,
                                 [&](model::Track& m) { m.children.push_back(tid); });
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        for (int k = 0; k < 4; ++k) {
            midi::Note note;
            note.start = k * 960;
            note.length = 480;
            note.pitch = static_cast<std::uint8_t>(48 + (i + k) % 24);
            note.velocity = 100;
            midi.sequence.notes.push_back(note);
        }
        const model::Id mi = txn.create(std::move(midi));
        const model::Id take = txn.create(model::Take{});
        txn.modify<model::Take>(take, [&](model::Take& tk) { tk.source = mi; });
        model::Item item;
        item.track = tid;
        item.length = 8;
        item.takes = {take};
        item.activeTake = take;
        txn.create(std::move(item));
    }
    auto committed = txn.commit();
    REQUIRE(committed);
    peaks::PeakService peaks(std::filesystem::temp_directory_path() / "supple-arrange-1000-cache",
                             [](std::function<void()>) {});
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto arrange = std::make_unique<views::ArrangeView>(
        peaks, [](const model::MediaSource&) { return std::filesystem::path{}; },
        views::TransportSource{});
    arrange->show(*committed);
    views::ArrangeView* view = arrange.get();
    root.setContent(std::move(arrange));
    views::Viewport v;
    v.pixelsPerSecond = 40;
    view->setViewport(v);
    root.damageAll();
    root.frame(false);
    CHECK(view->trackLayout().rowCount() == 1000);
    CHECK(view->lanes().stats().rows > 0);
    CHECK(view->lanes().stats().rows < 20);  // only what fits: virtualized
}

TEST_CASE("locked items are marked") {
    Gl& g = gl();
    if (!g.ok) return;
    auto build = [](bool locked) {
        model::Session s = model::Session::create();
        model::EditTxn txn(s, 1);
        model::Track track;
        track.type = model::StripType::Channel;
        track.parent = model::kMaster;
        track.name = "T";
        const model::Id tid = txn.create(track);
        txn.modify<model::Track>(model::kMaster,
                                 [&](model::Track& m) { m.children.push_back(tid); });
        model::Item item;
        item.track = tid;
        item.length = 4;
        item.locked = locked;
        item.fadeIn.length = 0.25;
        const model::Id iid = txn.create(item);
        const model::Id take = txn.create(model::Take{});
        txn.modify<model::Item>(iid, [&](model::Item& i) {
            i.takes.push_back(take);
            i.activeTake = take;
        });
        auto committed = txn.commit();
        REQUIRE(committed);
        return *committed;
    };
    peaks::PeakService peaks(std::filesystem::temp_directory_path() / "supple-arrange-lock-cache",
                             [](std::function<void()>) {});
    auto render = [&](const model::Session& s) {
        ui::Root root(*g.window, g.fonts, ui::Theme::dark());
        auto arrange = std::make_unique<views::ArrangeView>(
            peaks, [](const model::MediaSource&) { return std::filesystem::path{}; },
            views::TransportSource{});
        arrange->show(s);
        arrange->setViewport({0, 60, 0});
        arrange->setFollow(views::Follow::Off);
        root.setContent(std::move(arrange));
        root.damageAll();
        root.frame(false);
        return root.readBack();
    };
    const gui::Image plain = render(build(false));
    const gui::Image marked = render(build(true));
    std::size_t diff = 0;
    for (std::size_t i = 0; i < plain.rgba.size(); i += 4)
        if (std::abs(int(plain.rgba[i]) - int(marked.rgba[i])) > 8) ++diff;
    CHECK(diff > 0);  // the padlock paints somewhere
}

// --- golden images ------------------------------------------------------------------

TEST_CASE("golden: items with fades, muted items and multi-take items") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->installOverlay(root);
    arrange->setFollow(views::Follow::Off);
    arrange->setPlayhead(2.0);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    golden(root, "arrange-items.png");
}

// --- the track control panel (P5.5) -------------------------------------------------

namespace {

// Mounts an arrange view whose TCP commits through the fixture's bus, the way
// the shell wires it (selection in a vector, Origin::Ui, drag gestures).
struct TcpHarness {
    Session* session = nullptr;
    std::vector<std::string> selected;
    std::string error;
    std::vector<std::string> gestures;  // every commit's coalescing key (tests)
    std::optional<std::string> anchor;
    gui::MeterFeed* feed = nullptr;
    views::ArrangeView* view = nullptr;
    std::string pickedTrack;  // the input button was clicked for this strip
    ui::Rect pickedAnchor{};  // and it handed this window rect to the app

    views::ArrangeView* mount(ui::Root& root, Session& s, gui::MeterFeed* meters = nullptr) {
        session = &s;
        feed = meters;
        auto arrange =
            std::make_unique<views::ArrangeView>(*s.peaks, s.resolve(), views::TransportSource{});
        views::TcpCallbacks cb;
        cb.selected = [this](const model::Id& id) {
            return std::find(selected.begin(), selected.end(), id.str()) != selected.end();
        };
        cb.select = [this](const model::Id& id, unsigned mods) { select(id, mods); };
        cb.commit = [this](std::string_view command, json::Value args, std::string gesture) {
            gestures.push_back(gesture);
            cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
            origin.gesture = std::move(gesture);
            auto r = session->dispatcher->execute(
                {std::string(command), std::move(args), std::move(origin)});
            if (!r) error = r.error().code;
        };
        cb.addStrip = [this](std::string_view type) {
            json::Value args = json::Value::object();
            args["type"] = std::string(type);
            cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
            auto r =
                session->dispatcher->execute({"track.add", std::move(args), std::move(origin)});
            if (!r) {
                error = r.error().code;
                return;
            }
            selected = {(*r)["track"].asString()};  // the new strip is selected
            anchor = selected.front();
        };
        // The app names the ports (engine.inputs); these tests stand in for it.
        cb.inputText = [](const model::Id&, const std::string& channels) {
            std::string out;
            for (std::size_t i = 0; i < channels.size();) {
                std::size_t j = channels.find('/', i);
                if (j == std::string::npos) j = channels.size();
                out += (out.empty() ? "" : "+") +
                       ("AUX" + std::to_string(std::stoi(channels.substr(i, j - i)) - 1));
                i = j + 1;
            }
            return out.empty() ? std::string("in \u2014") : out;
        };
        cb.pickInput = [this](const model::Id& id, const ui::Rect& rect) {
            pickedTrack = id.str();
            pickedAnchor = rect;
        };
        if (meters) {
            cb.meterShown = [meters](const std::string& track, ui::Meter* meter) {
                meters->attach(track, meter);
            };
            cb.meterHidden = [meters](ui::Meter* meter) { meters->detach(meter); };
        }
        arrange->setTcpCallbacks(std::move(cb));
        arrange->show(s.app->head());
        views::ArrangeView* view_ = arrange.get();
        view = view_;
        root.setContent(std::move(arrange));
        return view_;
    }

    void select(const model::Id& id, unsigned mods) {
        const std::string key = id.str();
        if (mods & ui::kShift) {  // extend from the anchor, in row order
            const auto& rows = view->trackLayout().rows();
            auto index = [&](const std::string& want) {
                for (std::size_t i = 0; i < rows.size(); ++i)
                    if (rows[i].track.str() == want) return static_cast<int>(i);
                return -1;
            };
            const std::string from = anchor.value_or(key);
            const int a = index(from), b = index(key);
            if (a >= 0 && b >= 0) {
                selected.clear();
                for (int i = std::min(a, b); i <= std::max(a, b); ++i)
                    selected.push_back(rows[static_cast<std::size_t>(i)].track.str());
            } else {
                selected = {key};
            }
        } else if (mods & ui::kCtrl) {
            const auto it = std::find(selected.begin(), selected.end(), key);
            if (it == selected.end())
                selected.push_back(key);
            else
                selected.erase(it);
        } else {
            selected = {key};
        }
        anchor = key;
    }
    // The number of versions on the branch (history.list), like the brief's
    // "one undo step" check.
    std::size_t versions() {
        auto r = session->dispatcher->execute(
            {"history.list", json::Value::object(), {cmd::Origin::Type::Test, "gui-test", {}}});
        REQUIRE(r);
        return (*r)["versions"].asArray().size();
    }
    bool is(const char* track) const {
        return std::find(selected.begin(), selected.end(), std::string(track)) != selected.end();
    }
};

// Clicks the middle of a node through the root (the same path SDL input takes).
void press(ui::Root& root, ui::Node* node, int clicks = 1, unsigned mods = 0) {
    const ui::Rect b = node->bounds();
    const ui::Point at{b.x + b.w / 2, b.y + b.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1, clicks, mods});
    root.pointer({ui::PointerEvent::Type::Up, at, 1, clicks, mods});
}

// A control's rect as it is laid out (the toolkit arranges a container's
// children in the container's own space, so this is the point to press).
void pressAt(ui::Root& root, const ui::Node& node) {
    const ui::Rect b = node.bounds();
    const ui::Point at{b.x + b.w / 2, b.y + b.h / 2};
    root.pointer({ui::PointerEvent::Type::Move, at});
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
}

double trackDb(Session& s, const char* id) {
    return s.app->head().tracks.find(*model::Id::parse(id))->volumeDb;
}

}  // namespace

TEST_CASE("the TCP renames a strip in place through the bus") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    views::Viewport v;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Node* row = arrange->tcpPanel().rowNode(tr1);
    REQUIRE(row);
    // Double-click the name: a text field appears, focused, with the old name.
    const ui::Rect name = arrange->tcpPanel().controlNode(tr1, "rename")->bounds();
    const ui::Point at{name.x + 8, name.y + name.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1, 2});
    root.pointer({ui::PointerEvent::Type::Up, at, 1, 2});
    ui::Node* field = arrange->tcpPanel().controlNode(tr1, "rename");
    REQUIRE(field);
    CHECK(field->visible);
    CHECK(root.focused() == field);
    root.text("Bass");
    root.key(SDLK_RETURN);
    CHECK(tcp.error.empty());
    CHECK(s.app->head().tracks.find(tr1)->name == "Bass");
    CHECK_FALSE(arrange->tcpPanel().controlNode(tr1, "rename")->visible);
}

TEST_CASE("the TCP toggles mute, solo, arm and monitor on the strip") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr2 = *model::Id::parse("tr_2");
    auto* panel = &arrange->tcpPanel();
    // The app's loop syncs the panel to the new version after every command.
    auto sync = [&] {
        arrange->show(s.app->head());
        root.damageAll();
        root.frame(false);
    };
    REQUIRE(panel->controlNode(tr2, "mute"));
    press(root, panel->controlNode(tr2, "mute"));
    CHECK(s.app->head().tracks.find(tr2)->mute);
    sync();
    press(root, panel->controlNode(tr2, "solo"));
    CHECK(s.app->head().tracks.find(tr2)->solo == model::Solo::On);
    sync();
    press(root, panel->controlNode(tr2, "arm"));
    CHECK(s.app->head().tracks.find(tr2)->record.armed);
    sync();
    // A new channel monitors (on, once armed) by default.
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::On);
    press(root, panel->controlNode(tr2, "monitor"));  // On -> Auto
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::Auto);
    CHECK(panel->controlNode(tr2, "monitor")->valueText() == "on");  // until the view syncs
    sync();
    CHECK(panel->controlNode(tr2, "monitor")->valueText() == "auto");
    press(root, panel->controlNode(tr2, "monitor"));  // Auto -> Off
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::Off);
    sync();
    press(root, panel->controlNode(tr2, "monitor"));  // Off -> On
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::On);
    sync();
    // Polarity and mono sit on the level row, before the fader (ADR-0022).
    REQUIRE(panel->controlNode(tr2, "polarity"));
    CHECK(panel->controlNode(tr2, "mono")->bounds().right() <=
          panel->controlNode(tr2, "fader")->bounds().x);
    press(root, panel->controlNode(tr2, "polarity"));
    CHECK(s.app->head().tracks.find(tr2)->phaseInvert);
    sync();
    press(root, panel->controlNode(tr2, "mono"));
    CHECK(s.app->head().tracks.find(tr2)->mono);
    sync();
    CHECK(panel->controlNode(tr2, "polarity")->valueText() == "on");
    press(root, panel->controlNode(tr2, "polarity"));
    CHECK_FALSE(s.app->head().tracks.find(tr2)->phaseInvert);
    CHECK(tcp.error.empty());
    // A master has no row until it is armed or holds an item (ADR-0057); then
    // its lane's header has its arm and the lane mute, nothing else.
    const model::Id master = model::kMaster;
    CHECK(panel->controlNode(master, "arm") == nullptr);  // the master is not a row
    REQUIRE(s.dispatcher->execute(
        {"track.set", json::parse(R"({"track": "tr_0", "armed": true})").value(), {}}));
    sync();
    REQUIRE(panel->controlNode(master, "arm"));
    CHECK(panel->controlNode(master, "arm")->visible);
    CHECK(panel->controlNode(master, "lanemute")->visible);
    CHECK_FALSE(panel->controlNode(master, "fader")->visible);
    CHECK_FALSE(panel->controlNode(master, "mute")->visible);
    CHECK_FALSE(panel->controlNode(tr2, "lanemute")->visible);
    press(root, panel->controlNode(master, "lanemute"));
    CHECK(s.app->head().tracks.find(master)->laneMute);
    CHECK_FALSE(s.app->head().tracks.find(master)->mute);
    sync();
    press(root, panel->controlNode(master, "lanemute"));
    sync();
    press(root, panel->controlNode(master, "arm"));
    CHECK_FALSE(s.app->head().tracks.find(master)->record.armed);
    sync();
    CHECK(panel->controlNode(master, "arm") == nullptr);  // no lane again
    CHECK(tcp.error.empty());
}

TEST_CASE("a fader drag is one undo step and lands on the moved value") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Node* fader = arrange->tcpPanel().controlNode(tr1, "fader");
    REQUIRE(fader);
    const ui::Rect fb = fader->bounds();
    const std::size_t before = tcp.versions();
    const double startDb = trackDb(s, "tr_1");
    const ui::Point at{fb.x + fb.w / 2, fb.y + fb.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    for (int i = 1; i <= 20; ++i)  // 20 moves of one drag
        root.pointer({ui::PointerEvent::Type::Move, {at.x - static_cast<float>(i) * 2, at.y}});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(tcp.error.empty());
    const model::Id tr1b = tr1;
    const double moved = s.app->head().tracks.find(tr1b)->volumeDb;
    CHECK(moved < startDb);  // dragged left: quieter
    // Every move of the drag carried the same coalescing key.
    REQUIRE(tcp.gestures.size() == 20);
    for (const std::string& key : tcp.gestures) CHECK(key == tcp.gestures.front());
    CHECK(tcp.gestures.front().find("tcp.volume:tr_1:") == 0);
    CHECK(tcp.versions() == before + 1);  // one undo step for the whole drag
    // A second drag is a second step, not a continuation of the first.
    const std::size_t afterFirst = tcp.versions();
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    root.pointer({ui::PointerEvent::Type::Move, {at.x - 4, at.y}});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(tcp.versions() == afterFirst + 1);
    // A double-click resets to unity, as its own edit.
    root.pointer({ui::PointerEvent::Type::Down, at, 1, 2});
    root.pointer({ui::PointerEvent::Type::Up, at, 1, 2});
    CHECK(trackDb(s, "tr_1") == doctest::Approx(0.0));
    CHECK(tcp.versions() == afterFirst + 2);
}

TEST_CASE("a pan drag is one undo step") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr3 = *model::Id::parse("tr_3");
    ui::Node* pan = arrange->tcpPanel().controlNode(tr3, "pan");
    REQUIRE(pan);
    const ui::Rect pb = pan->bounds();
    const std::size_t before = tcp.versions();
    const ui::Point at{pb.x + pb.w / 2, pb.y + pb.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    for (int i = 1; i <= 6; ++i)
        root.pointer({ui::PointerEvent::Type::Move, {at.x + static_cast<float>(i) * 3, at.y}});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(tcp.error.empty());
    CHECK(s.app->head().tracks.find(tr3)->pan > 0.0);
    CHECK(tcp.versions() == before + 1);
}

TEST_CASE("TCP selection follows click, Ctrl+click and Shift+click") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    auto* panel = &arrange->tcpPanel();
    const model::Id tr1 = *model::Id::parse("tr_1");
    const model::Id tr3 = *model::Id::parse("tr_3");
    REQUIRE(panel->rowNode(tr1));
    press(root, panel->rowNode(tr1));
    CHECK(tcp.is("tr_1"));
    CHECK(tcp.selected.size() == 1);
    press(root, panel->rowNode(tr3), 1, ui::kCtrl);
    CHECK(tcp.is("tr_1"));
    CHECK(tcp.is("tr_3"));
    press(root, panel->rowNode(tr3), 1, ui::kCtrl);  // toggles off
    CHECK_FALSE(tcp.is("tr_3"));
    press(root, panel->rowNode(tr1));
    press(root, panel->rowNode(tr3), 1, ui::kShift);  // extends
    CHECK(tcp.selected.size() == 3);                  // tr_1..tr_3
    CHECK(tcp.error.empty());
}

TEST_CASE("golden: the TCP with one strip of each type") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    const cmd::Origin test{cmd::Origin::Type::Test, "gui-test", {}};
    auto run = [&](const char* command, json::Value args) {
        auto r = s.dispatcher->execute({command, std::move(args), test});
        REQUIRE(r);
        return *r;
    };
    // A buss with a channel under it: the parentage bar and indent show.
    json::Value bussArgs = json::Value::object();
    bussArgs["type"] = "buss";
    bussArgs["name"] = "Drum bus";
    const std::string buss = run("track.add", std::move(bussArgs))["track"].asString();
    json::Value childArgs = json::Value::object();
    childArgs["name"] = "Kick";
    childArgs["parent"] = buss;
    const std::string child = run("track.add", std::move(childArgs))["track"].asString();
    // One muted strip, one soloed, one selected.
    json::Value mute = json::Value::object();
    mute["track"] = buss;
    mute["mute"] = true;
    run("track.set", std::move(mute));
    json::Value solo = json::Value::object();
    solo["track"] = child;
    solo["solo"] = "on";
    run("track.set", std::move(solo));
    // A record input on the child: its button names the device's port (the app
    // supplies them from engine.inputs), not just the channel number.
    json::Value rec = json::Value::object();
    rec["track"] = child;
    rec["record_input"] = json::Value::array();
    rec["record_input"].push_back(json::Value(static_cast<std::int64_t>(2)));
    run("track.set", std::move(rec));
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    tcp.selected = {child};  // and the child is selected
    views::Viewport v;
    v.pixelsPerSecond = 40;
    v.scrollY = 1e6f;  // the buss and its child are the last rows
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    golden(root, "tcp-strips.png");
    // Scrolled to the end, the last strip clears the foot's "mixer" and
    // "+ strip"; rows never paint or take clicks there.
    auto* panel = static_cast<views::TcpPanel*>(root.find("tcp"));
    const ui::Rect foot = root.find("tcp.foot")->bounds();
    REQUIRE(panel->rowNode(*model::Id::parse(child)));
    CHECK(panel->rowNode(*model::Id::parse(child))->bounds().bottom() <= foot.y + 0.5f);
    CHECK(root.find("tcp.rows")->bounds().bottom() <= foot.y + 0.5f);
    CHECK(root.content()->hitTest({foot.x + 10, foot.y + foot.h / 2}) !=
          panel->rowNode(*model::Id::parse(child)));
    const ui::Rect add = root.find("tcp.add")->bounds();
    CHECK(add.y >= foot.y);
    CHECK(root.content()->hitTest({add.x + 5, add.y + 5}) == root.find("tcp.add"));
}

TEST_CASE("the TCP's + offers exactly Channel, Buss and Master") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    REQUIRE(root.find("tcp.add"));
    for (auto [index, type] : {std::pair{0, model::StripType::Channel},
                               {1, model::StripType::Buss},
                               {2, model::StripType::Master}}) {
        press(root, root.find("tcp.add"));
        auto* menu = static_cast<ui::List*>(root.find("tcp.addmenu"));
        REQUIRE(menu);
        CHECK(menu->count() == 3);  // exactly the three strip types
        const std::size_t before = s.app->head().tracks.size();
        menu->onActivate(static_cast<std::size_t>(index));
        CHECK(s.app->head().tracks.size() == before + 1);
        REQUIRE(tcp.selected.size() == 1);
        const model::Track* created = s.app->head().tracks.find(*model::Id::parse(tcp.selected[0]));
        REQUIRE(created);
        CHECK(created->type == type);
    }
    CHECK(tcp.error.empty());
}

TEST_CASE("a double-click on the TCP's empty space adds a channel; on a row it does not") {
    Gl& g = gl();
    if (!g.ok) return;
    Session s;
    REQUIRE(s.build(2));
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const ui::Rect rows = root.find("tcp.rows")->bounds();
    const std::size_t before = s.app->head().tracks.size();
    auto doubleClick = [&](ui::Point at) {
        root.pointer({ui::PointerEvent::Type::Down, at, 1, 1});
        root.pointer({ui::PointerEvent::Type::Up, at, 1, 1});
        root.pointer({ui::PointerEvent::Type::Down, at, 1, 2});
        root.pointer({ui::PointerEvent::Type::Up, at, 1, 2});
        root.frame(false);
    };
    // On a row (its empty part): no new track.
    auto* panel = static_cast<views::TcpPanel*>(root.find("tcp"));
    const ui::Node* first = panel->rowNode(s.app->head().tracks.find(model::kMaster)->children[0]);
    REQUIRE(first);
    doubleClick({first->bounds().x + first->bounds().w * 0.6f, first->bounds().bottom() - 8});
    CHECK(s.app->head().tracks.size() == before);
    // Below the last row: a channel, selected.
    doubleClick({rows.x + rows.w / 2, rows.bottom() - 10});
    CHECK(s.app->head().tracks.size() == before + 1);
    REQUIRE(tcp.selected.size() == 1);
    const model::Track* created = s.app->head().tracks.find(*model::Id::parse(tcp.selected[0]));
    REQUIRE(created);
    CHECK(created->type == model::StripType::Channel);
    // A single click there adds nothing.
    root.pointer({ui::PointerEvent::Type::Down, {rows.x + rows.w / 2, rows.bottom() - 10}, 1, 1});
    root.pointer({ui::PointerEvent::Type::Up, {rows.x + rows.w / 2, rows.bottom() - 10}, 1, 1});
    CHECK(s.app->head().tracks.size() == before + 1);
    // A right-click there: the "+ strip" menu at the pointer; its second row adds a buss.
    const ui::Point at{rows.x + rows.w / 2, rows.bottom() - 10};
    root.pointer({ui::PointerEvent::Type::Down, at, 3, 1});
    root.pointer({ui::PointerEvent::Type::Up, at, 3, 1});
    root.frame(false);
    ui::Node* menu = root.find("tcp.addmenu");
    REQUIRE(menu);
    CHECK(std::abs(menu->bounds().x - at.x) < 2);
    const ui::Point buss{menu->bounds().x + 40, menu->bounds().y + 26 + 13};
    root.pointer({ui::PointerEvent::Type::Move, buss});
    root.pointer({ui::PointerEvent::Type::Down, buss, 1, 1});
    root.pointer({ui::PointerEvent::Type::Up, buss, 1, 1});
    CHECK(s.app->head().tracks.size() == before + 2);
    REQUIRE(tcp.selected.size() == 1);
    CHECK(s.app->head().tracks.find(*model::Id::parse(tcp.selected[0]))->type ==
          model::StripType::Buss);
    CHECK(tcp.error.empty());
}

TEST_CASE("the TCP meters take readings from the feed and hold peaks") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    std::vector<gui::StripReading> readings;
    gui::MeterFeed feed([&readings] { return readings; });
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s, &feed);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Meter* meter = static_cast<ui::Meter*>(arrange->tcpPanel().controlNode(tr1, "meter"));
    REQUIRE(meter);
    // A reading for tr_1 (and one for a track the panel does not show).
    gui::StripReading r;
    r.track = "tr_1";
    r.peak[0] = r.peak[1] = 0.5f;  // -6 dBFS
    r.rms[0] = r.rms[1] = 0.25f;
    readings = {r};
    feed.poll();
    CHECK(meter->shownPeakDb(0) == doctest::Approx(static_cast<double>(-6.02f)).epsilon(0.2));
    // The hold keeps it for a while, then the decay ticks it down.
    readings.clear();
    feed.poll();
    const float held = meter->shownPeakDb(0);
    CHECK(held > -10.0f);
    for (int i = 0; i < 30; ++i) feed.poll();
    CHECK(meter->shownPeakDb(0) < held);
    // A clip (a reading at or over full scale) latches until the meter is
    // clicked, which also clears the hold.
    gui::StripReading hot;
    hot.track = "tr_1";
    hot.peak[0] = hot.peak[1] = 1.2f;
    hot.rms[0] = hot.rms[1] = 0.5f;
    readings = {hot};
    feed.poll();
    CHECK(meter->clipped());
    press(root, meter);
    CHECK_FALSE(meter->clipped());
}

TEST_CASE("the meters repaint only themselves, not the arrange content") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    std::vector<gui::StripReading> readings;
    gui::MeterFeed feed([&readings] { return readings; });
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s, &feed);
    arrange->setViewport({0, 40, 0});
    root.frame(false);
    const std::uint64_t repaints = root.stats().repaints;
    const double area = root.stats().repaintedArea;
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Meter* meter = static_cast<ui::Meter*>(arrange->tcpPanel().controlNode(tr1, "meter"));
    REQUIRE(meter);
    ui::Rect meterRect = meter->bounds();
    gui::StripReading r;
    r.track = "tr_1";
    r.peak[0] = r.peak[1] = 0.7f;
    readings = {r};
    feed.poll();
    root.frame(false);
    // Only the meter's own rectangle (or a little more) was repainted.
    CHECK(root.stats().repaintedArea - area < 4000.0);
    CHECK(root.stats().repaintedArea - area > 0.0);
    (void)repaints;
    (void)meterRect;
}

TEST_CASE("the strip's input button names the ports and hands the anchor to the app (P5.13)") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    const cmd::Origin test{cmd::Origin::Type::Test, "gui-test", {}};
    json::Value add = json::Value::object();
    add["name"] = "Rec";
    auto created = s.dispatcher->execute({"track.add", std::move(add), test});
    REQUIRE(created);
    const std::string track = (*created)["track"].asString();
    json::Value rec = json::Value::object();
    rec["track"] = track;
    rec["record_input"] = json::Value::array();
    rec["record_input"].push_back(json::Value(static_cast<std::int64_t>(1)));
    rec["record_input"].push_back(json::Value(static_cast<std::int64_t>(2)));
    REQUIRE(s.dispatcher->execute({"track.set", std::move(rec), test}));
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    views::Viewport v;
    v.pixelsPerSecond = 40;
    v.scrollY = 1e6f;  // the new strip is the last row
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    ui::Node* button = arrange->tcpPanel().controlNode(*model::Id::parse(track), "input");
    REQUIRE(button);
    // The strip shows the port names; a narrow strip clips the tail, so the
    // name is a prefix (the menu shows them whole).
    CHECK(button->valueText().rfind("AUX0", 0) == 0);
    pressAt(root, *button);  // and the app is asked to pick
    CHECK(tcp.pickedTrack == track);
    CHECK(tcp.pickedAnchor.w > 0);
    CHECK(tcp.pickedAnchor.x > 0);
    CHECK(tcp.pickedAnchor.y > 0);
    CHECK(tcp.pickedAnchor.x == button->bounds().x);
    CHECK(tcp.pickedAnchor.y == button->bounds().y);
}

TEST_CASE("the mouse map: built-in defaults, per-combination overrides, the file round-trips") {
    views::MouseMap m;
    CHECK(m.behaviour("item.drag", 0) == "move");
    CHECK(m.behaviour("item.drag", ui::kAlt) == "slip");
    CHECK(m.behaviour("item.drag", ui::kCtrl | ui::kShift) == "copy_free");
    CHECK(m.behaviour("item.drag", ui::kCtrl | ui::kAlt) == "move");   // unbound: the plain press
    CHECK(m.behaviour("item.drag", ui::kAlt | ui::kSuper) == "slip");  // other modifiers ignored
    CHECK(m.behaviour("envelope", ui::kAlt) == "delete");
    CHECK(m.behaviour("nowhere", 0).empty());
    // The defaults write an empty contexts object.
    CHECK(m.toJson()["contexts"].asObject().empty());
    REQUIRE(m.set("item.drag", ui::kAlt, "copy"));
    REQUIRE(m.set("item.drag", ui::kShift, ""));  // back to the plain press
    REQUIRE(m.set("ruler.tempo", ui::kCtrl | ui::kAlt, "toggle_ramp"));
    CHECK(m.behaviour("item.drag", ui::kShift) == "move");
    json::Value doc = m.toJson();
    CHECK(doc["contexts"]["item.drag"]["alt"].asString() == "copy");
    CHECK(doc["contexts"]["item.drag"]["shift"].asString().empty());
    CHECK(doc["contexts"]["ruler.tempo"]["ctrl+alt"].asString() == "toggle_ramp");
    auto back = views::MouseMap::fromJson(doc);
    REQUIRE(back);
    CHECK(*back == m);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-mouse-test-" + std::to_string(testProcessId()));
    REQUIRE(m.save(dir / "mouse.json"));
    auto loaded = views::MouseMap::load(dir / "mouse.json");
    REQUIRE(loaded);
    CHECK(*loaded == m);
    std::filesystem::remove_all(dir);
    // Errors name the place and the choices.
    auto bad = views::MouseMap::fromJson(
        json::parse(R"({"schema":"supple.mouse/1.0","contexts":{"fade":{"hyper":"drag"}}})")
            .value());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "mouse.combo");
    bad = views::MouseMap::fromJson(
        json::parse(R"({"schema":"supple.mouse/1.0","contexts":{"fade":{"alt":"spin"}}})").value());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().path == "/contexts/fade/alt");
    CHECK(bad.error().hint.find("cycle_shape") != std::string::npos);
    CHECK_FALSE(views::MouseMap::fromJson(json::parse(R"({"schema":"x"})").value()));
}

TEST_CASE("visible source passes preserve offset and rate with a bounded iteration count") {
    model::SourceBeatMapping map{2, 2, 8};
    CHECK(map.itemBeat(4) == doctest::Approx(1));
    CHECK(map.itemBeat(4, 2) == doctest::Approx(9));
    CHECK(map.visibleOrigins(20, true, 8, 10) == std::vector<double>{7});
    CHECK(model::visibleLoopPasses(0, 1e9, 4, true, 100000, 100008) ==
          std::vector<double>{100000, 100004, 100008});
    CHECK(model::visibleLoopPasses(0, 100, 0.001, true, 0, 100, 10).size() == 10);
    CHECK(model::visibleLoopPasses(0, 1, 1e-300, true, 0.5, 0.6).size() <= 1);
    model::SourceBeatMapping crossing{0, 1, 4};
    CHECK(crossing.visibleOrigins(8, true, 0, 8, 0.5) == std::vector<double>{-4, 0, 4});
    CHECK(crossing.visibleOrigins(8, true, 4, 8, 0.5) == std::vector<double>{0, 4});
    CHECK(crossing.visibleOrigins(8, true, 0, 8, 100) == std::vector<double>{-4, 0, 4});
    CHECK(crossing.visibleOrigins(1e9, true, 100000, 100008, 0.5) ==
          std::vector<double>{99996, 100000, 100004, 100008});
    crossing.offset = 3.5;
    CHECK(crossing.visibleOrigins(3, true, 0, 3, 0.5) == std::vector<double>{-3.5, 0.5});
    crossing = {0, 2, 4};
    CHECK(crossing.visibleOrigins(8, true, 0, 8, 0.25) == std::vector<double>{-2, 0, 2, 4, 6});
    crossing = {0, 1, 1e-5};
    CHECK(crossing.visibleOrigins(1e9, true, 0, 1e9, 1).size() == 8192);
    map.rate = 0;
    CHECK(map.visibleOrigins(20, true, 8, 10).empty());
}

TEST_CASE("automation drawing samples playback boundaries without expanding loops") {
    model::EditTxn txn(model::Session::create(), 1);
    model::Envelope env;
    env.target = {model::kMaster, "volume"};
    bool normalized = false, loop = true;
    SUBCASE("empty underlying envelope") {}
    SUBCASE("underlying envelope") { env.points = {{0, 0, model::PointShape::Slow}, {30, 10}}; }
    SUBCASE("nonlooping source") { loop = false; }
    SUBCASE("normalized plugin domain") {
        normalized = true;
        env.points = {{0, 1, model::PointShape::Slow}, {30, 10}};
    }
    const auto id = txn.create(env);
    model::AutomationItem item;
    item.envelope = id;
    item.position = 4;
    item.length = 9.5;
    item.sourceLength = 4;
    item.loop = loop;
    item.points = {{0, 2, model::PointShape::Slow}, {1, 8, model::PointShape::Square}, {3, 4}};
    item.baseline = 2;
    item.amplitude = 0.5;
    const auto ai = txn.create(item);
    auto session = txn.preview();
    views::EnvScale scale;
    if (normalized) {
        auto spec = std::make_shared<params::DescribedParam>();
        spec->spec.min = 1;
        spec->spec.max = 10;
        spec->spec.curve = {params::CurveKind::Log};
        scale.kind = views::EnvScale::Kind::Plugin;
        scale.param = spec;
    }
    auto expanded = model::effectivePoints(session, id);
    if (normalized)
        for (auto& point : expanded) point.value = scale.toNorm(point.value);
    views::EnvelopeCurve curve(session, id, scale, 5, 12);
    for (int i = 0; i <= 160; ++i) {
        const double beat = i * 0.125;
        INFO(beat);
        const double value = model::pointsValueAt(expanded, beat, normalized ? scale.toNorm(0) : 0);
        CHECK(curve.valueAt(beat, 0) ==
              doctest::Approx(normalized ? scale.fromNorm(value) : value));
    }
    txn.modify<model::AutomationItem>(ai, [](auto& x) { x.length = 1e9; });
    views::EnvelopeCurve huge(txn.preview(), id, scale, 1e8, 1e8 + 2);
    if (loop) CHECK(huge.valueAt(100000004.5, 0) == doctest::Approx(curve.valueAt(4.5, 0)));
    txn.modify<model::AutomationItem>(ai, [](auto& x) { x.sourceLength = 1e-300; });
    views::EnvelopeCurve tiny(txn.preview(), id, scale, 1e8, 1e8 + 2);
    CHECK(std::isfinite(tiny.valueAt(1e8, 0)));
}

TEST_CASE("plugin automation drawing interpolates logarithmic parameters in normalized units") {
    model::EditTxn txn(model::Session::create(), 1);
    model::Envelope env;
    env.target = {model::kMaster, "volume"};
    env.points = {{0, 100}, {4, 10000}};
    const auto id = txn.create(env);
    auto spec = std::make_shared<params::DescribedParam>();
    spec->spec.min = 100;
    spec->spec.max = 10000;
    spec->spec.curve = {params::CurveKind::Log};
    views::EnvScale scale;
    scale.kind = views::EnvScale::Kind::Plugin;
    scale.param = spec;
    views::EnvelopeCurve curve(txn.preview(), id, scale, 0, 4);
    CHECK(curve.valueAt(2, 100) == doctest::Approx(1000));
    scale.normalizedInterpolation = false;
    views::EnvelopeCurve plain(txn.preview(), id, scale, 0, 4);
    CHECK(plain.valueAt(2, 100) == doctest::Approx(5050));
}

TEST_CASE("meters ignore hidden ancestors and reset their hold on reopening") {
    Gl& g = gl();
    REQUIRE(g.ok);
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto parent = std::make_unique<ui::Node>("panel");
    auto* panel = parent.get();
    auto& meter = parent->emplace<ui::Meter>("meter");
    meter.layout.width = ui::SizeRule::fixed(24);
    root.setContent(std::move(parent));
    std::vector<gui::StripReading> readings = {{"tr_1", {1.2f, 1.2f}, {0.5f, 0.5f}}};
    gui::MeterFeed feed([&] { return readings; });
    feed.attach("tr_1", &meter);
    root.frame(false);
    feed.poll();
    CHECK(meter.clipped());
    panel->visible = false;
    root.damageAll();
    root.frame(false);
    const auto repaints = root.stats().repaints;
    for (int i = 0; i < 30; ++i) {
        feed.poll();
        root.frame(false);
    }
    CHECK(root.stats().repaints == repaints);
    panel->visible = true;
    readings.clear();
    feed.poll();
    CHECK_FALSE(meter.clipped());
    CHECK(meter.shownPeakDb(0) < -100);
    feed.detach(&meter);
}

TEST_CASE("unrelated FX edits in 200 tracks and 2000 items have bounded invalidation") {
    Gl& g = gl();
    REQUIRE(g.ok);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-invalidation-" + std::to_string(testProcessId()));
    peaks::PeakService peaks(dir, [](std::function<void()>) {});
    auto initial = model::Session::create();
    model::EditTxn build(initial, 1);
    model::FxInstance fx;
    fx.plugin.id = "invalidation-test";
    const auto effect = build.create(fx);
    std::vector<model::Id> tracks, items;
    for (int i = 0; i < 200; ++i) {
        model::Track track;
        track.name = "Track " + std::to_string(i + 1);
        track.parent = model::kMaster;
        if (i == 0) track.chain.push_back(effect);
        const auto id = build.create(track);
        tracks.push_back(id);
        for (int j = 0; j < 10; ++j) {
            model::Item item;
            item.track = id;
            item.position = j * 4;
            item.length = 4;
            items.push_back(build.create(item));
        }
    }
    build.modify<model::Track>(model::kMaster, [&](auto& master) { master.children = tracks; });
    auto built = build.commit();
    REQUIRE(built);
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto owned = std::make_unique<views::ArrangeView>(peaks, views::ArrangeView::Resolve{});
    auto* arrange = owned.get();
    arrange->show(*built);
    root.setContent(std::move(owned));
    root.frame(false);
    const auto layouts = arrange->trackLayout().rebuilds();
    const auto indexes = arrange->lanes().indexStats();
    const auto paints = root.stats();
    model::EditTxn edit(*built, 2);
    REQUIRE(edit.modify<model::FxInstance>(effect, [](auto& value) { value.params[1] = 0.5; }));
    auto edited = edit.commit();
    REQUIRE(edited);
    const auto fxChanges = edit.changes();
    arrange->show(*edited, &fxChanges);
    root.frame(false);
    MESSAGE("layout rebuilds=" << arrange->trackLayout().rebuilds() - layouts << ", index rebuilds="
                               << arrange->lanes().indexStats().rebuilds - indexes.rebuilds
                               << ", indexed items="
                               << arrange->lanes().indexStats().items - indexes.items
                               << ", repaints=" << root.stats().repaints - paints.repaints
                               << ", repaint area="
                               << root.stats().repaintedArea - paints.repaintedArea);
    CHECK(arrange->trackLayout().rebuilds() - layouts == 0);
    CHECK(arrange->lanes().indexStats().rebuilds - indexes.rebuilds == 0);
    CHECK(arrange->lanes().indexStats().items - indexes.items == 0);
    CHECK(root.stats().repaints - paints.repaints == 0);
    CHECK(root.stats().repaintedArea == paints.repaintedArea);
    CHECK(arrange->session().fx.find(effect)->params.at(1) == 0.5);
    arrange->show(*built, &fxChanges);  // undo, same change list in reverse
    root.frame(false);
    CHECK(root.stats().repaints == paints.repaints);
    arrange->show(*edited);  // no history hint: persistent object identity fallback
    root.frame(false);
    CHECK(root.stats().repaints == paints.repaints);

    // A captured fader and a focused rename survive unrelated edits.
    std::vector<json::Value> writes;
    views::TcpCallbacks callbacks;
    callbacks.commit = [&](std::string_view, json::Value args, std::string) {
        writes.push_back(std::move(args));
    };
    arrange->setTcpCallbacks(std::move(callbacks));
    auto* fader = arrange->tcpPanel().controlNode(tracks[0], "fader");
    REQUIRE(fader);
    const auto faderBounds = fader->bounds();
    const ui::Point pressAt{faderBounds.x + faderBounds.w / 2, faderBounds.y + faderBounds.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, pressAt, 1});
    root.frame(false);
    arrange->show(*built, &fxChanges);
    CHECK(arrange->tcpPanel().controlNode(tracks[0], "fader") == fader);
    root.pointer({ui::PointerEvent::Type::Move, {pressAt.x + 10, pressAt.y}, 1});
    root.pointer({ui::PointerEvent::Type::Up, {pressAt.x + 10, pressAt.y}, 1});
    REQUIRE_FALSE(writes.empty());
    CHECK(writes.back()["track"].asString() == tracks[0].str());
    REQUIRE(arrange->tcpPanel().rename(tracks[0]));
    auto* rename = root.focused();
    root.text("kept rename");
    arrange->show(*edited, &fxChanges);
    CHECK(root.focused() == rename);
    CHECK(rename->visible);
    CHECK(static_cast<ui::TextField*>(rename)->text() == "kept rename");
    root.key(SDLK_ESCAPE);
    root.frame(false);

    auto current = *edited;
    std::uint64_t version = 3;
    // Every incremental frame must match a complete repaint of the same model.
    auto apply = [&](model::EditTxn& txn, bool geometry, bool reindex, bool visible) {
        INFO("version " << txn.version());
        const auto hints = txn.changes();
        auto next = txn.commit();
        REQUIRE_MESSAGE(next, (next ? "" : next.error().toString()));
        const auto oldLayouts = arrange->trackLayout().rebuilds();
        const auto oldIndexes = arrange->lanes().indexStats();
        const auto oldPaints = root.stats();
        arrange->show(*next, &hints);
        root.frame(false);
        CHECK(arrange->trackLayout().rebuilds() - oldLayouts == (geometry ? 1 : 0));
        CHECK(arrange->lanes().indexStats().rebuilds - oldIndexes.rebuilds == (reindex ? 1 : 0));
        if (!geometry && reindex && std::none_of(hints.begin(), hints.end(), [](const auto& hint) {
                return hint.id.kind == model::Kind::TempoPoint ||
                       hint.id.kind == model::Kind::Meter;
            }))
            CHECK(arrange->lanes().indexStats().items - oldIndexes.items <= 20);
        CHECK((root.stats().repaints > oldPaints.repaints) == visible);
        if (!geometry && !reindex && visible)
            CHECK(root.stats().repaintedArea - oldPaints.repaintedArea < 100000);
        const auto incremental = root.readBack();
        root.damageAll();
        root.frame(false);
        const auto complete = root.readBack();
        if (incremental.rgba != complete.rgba) {
            std::size_t changed = 0;
            int largest = 0;
            REQUIRE(incremental.rgba.size() == complete.rgba.size());
            for (std::size_t i = 0; i < incremental.rgba.size(); ++i) {
                const int delta = std::abs(int(incremental.rgba[i]) - int(complete.rgba[i]));
                changed += delta != 0;
                largest = std::max(largest, delta);
            }
            MESSAGE("repaint version " << txn.version() << ": " << changed
                                       << " channels differ, largest delta " << largest);
            MESSAGE("renderer " << g.window->renderer() << ", dither "
                                << (glIsEnabled(GL_DITHER) != GL_FALSE));
            const auto suffix = std::to_string(txn.version()) + ".png";
            REQUIRE(gui::writePng("incremental-" + suffix, incremental));
            REQUIRE(gui::writePng("complete-" + suffix, complete));
        }
        CHECK((incremental.rgba == complete.rgba));
        current = *next;
    };
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Item>(items[0], [](auto& item) { item.mute = true; });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Item>(items[0], [&](auto& item) {
            item.track = tracks[1];
            item.position = 1;
        });
        apply(txn, false, true, true);
        CHECK(std::find(arrange->lanes().itemsInRow(0).begin(),
                        arrange->lanes().itemsInRow(0).end(),
                        items[0]) == arrange->lanes().itemsInRow(0).end());
    }
    {
        model::EditTxn txn(current, version++);
        txn.remove<model::Item>(items[0]);
        apply(txn, false, true, true);
    }
    // Jump back over the move and delete without an adjacent-version hint.
    arrange->show(*edited);
    root.frame(false);
    const auto restored = root.readBack();
    root.damageAll();
    root.frame(false);
    CHECK((restored.rgba == root.readBack().rgba));
    current = *edited;
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Track>(tracks[0], [](auto& track) { track.mute = true; });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Item>(items.back(), [](auto& item) { item.mute = true; });
        apply(txn, false, false, false);  // offscreen edit damages no pixels
    }
    model::Id envelope;
    {
        model::EditTxn txn(current, version++);
        model::Envelope env;
        env.target = {tracks[0], "volume"};
        env.points = {{0, -12}, {4, 0}};
        envelope = txn.create(env);
        apply(txn, true, true, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Envelope>(envelope, [](auto& env) { env.points[0].value = -24; });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        model::AutomationItem ai;
        ai.envelope = envelope;
        ai.points = {{0, -6}, {4, 0}};
        txn.create(ai);
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Envelope>(envelope, [](auto& env) { env.visible = false; });
        apply(txn, true, true, true);
    }
    model::Id source, take, takeEnvelope;
    {
        model::EditTxn txn(current, version++);
        model::MidiSource midi;
        midi.sequence.notes.push_back({0, 960, 0, 60, 100});
        source = txn.create(midi);
        model::Take tk;
        tk.source = source;
        take = txn.create(tk);
        txn.modify<model::Item>(items[1], [&](auto& item) {
            item.takes = {take};
            item.activeTake = take;
        });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::MidiSource>(source,
                                      [](auto& midi) { midi.sequence.notes[0].pitch = 72; });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Take>(take, [](auto& tk) {
            tk.name = "edited take";
            tk.playrate = 2;
        });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        model::Envelope env;
        env.target = {take, "pan"};
        env.points = {{0, -50}, {4, 50}};
        takeEnvelope = txn.create(env);
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Envelope>(takeEnvelope, [](auto& env) { env.points[0].value = 25; });
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Track>(tracks[0],
                                 [](auto& track) { track.timebase = model::TrackTimebase::Time; });
        apply(txn, false, true, true);
    }
    {
        model::EditTxn txn(current, version++);
        const auto tempo = current.tempo.begin()->id;
        txn.modify<model::TempoPointObj>(tempo, [](auto& point) { point.bpm = 90; });
        apply(txn, false, true, true);
    }
    {
        model::EditTxn txn(current, version++);
        model::Marker marker;
        marker.pos = 2;
        marker.name = "changed ruler";
        txn.create(marker);
        apply(txn, false, false, true);
    }
    {
        model::EditTxn txn(current, version++);
        txn.modify<model::Track>(tracks[0], [](auto& track) { track.hiddenTcp = true; });
        apply(txn, true, true, true);
    }
}

TEST_CASE(
    "MIDI thumbnails chase source-crossing tails and exclude loop starts outside the source") {
    Gl& g = gl();
    REQUIRE(g.ok);
    model::EditTxn build(model::Session::create(), 1);
    model::Track track;
    track.parent = model::kMaster;
    const auto tid = build.create(track);
    build.modify<model::Track>(model::kMaster, [&](auto& master) { master.children = {tid}; });
    model::MidiSource midi;
    midi.sequence.ppq = 960;
    midi.sequence.length = 4 * 960;
    midi.sequence.notes = {{3360, 960, 0, 72, 127, 0}};
    const auto source = build.create(midi);
    model::Take take;
    take.source = source;
    const auto takeId = build.create(take);
    model::Item item;
    item.track = tid;
    item.length = 8;
    item.loop = true;
    item.takes = {takeId};
    item.activeTake = takeId;
    const auto itemId = build.create(item);
    auto committed = build.commit();
    REQUIRE(committed);
    peaks::PeakService peaks(std::filesystem::temp_directory_path() /
                                 ("supple-thumbnail-cache-" + std::to_string(testProcessId())),
                             [](std::function<void()>) {});
    auto draw = [&](const model::Session& session, double start, double zoom, std::size_t notes) {
        ui::Root root(*g.window, g.fonts, ui::Theme::dark());
        auto owned = std::make_unique<views::ArrangeView>(peaks, views::ArrangeView::Resolve{});
        auto* view = owned.get();
        view->show(session);
        view->setViewport({start, zoom, 0});
        root.setContent(std::move(owned));
        root.frame(false);
        CHECK(view->lanes().stats().notes == notes);
        return root.readBack();
    };
    // Playback's 4-beat source: the previous note is held from 0 through .5,
    // followed by notes at 3.5..4.5 and 7.5..8 (clipped at the item's end).
    const auto canonical = draw(*committed, 0, 160, 3);
    draw(*committed, 2, 160, 2);  // scroll to beat 4: chase the tail from pass 0
    for (double offset : {4., 12., 400.}) {
        model::EditTxn txn(*committed, 2);
        txn.modify<model::Take>(takeId, [&](auto& tk) { tk.startOffset = offset; });
        auto edited = txn.commit();
        REQUIRE(edited);
        const auto image = draw(*edited, 0, 160, 3);
        CHECK((image.rgba == canonical.rgba));
    }
    {
        model::EditTxn txn(*committed, 2);
        txn.modify<model::Take>(takeId, [](auto& tk) { tk.playrate = 2; });
        auto edited = txn.commit();
        REQUIRE(edited);
        draw(*edited, 0, 160, 5);
    }
    {
        model::EditTxn txn(*committed, 2);
        txn.modify<model::MidiSource>(source, [](auto& src) {
            src.sequence.notes.push_back({3840, 960, 0, 0, 127, 0});
            src.sequence.notes.push_back({4800, 960, 0, 127, 127, 0});
        });
        auto edited = txn.commit();
        REQUIRE(edited);
        const auto image = draw(*edited, 0, 160, 3);
        CHECK((image.rgba == canonical.rgba));  // also exclude these pitches from scaling
        model::EditTxn noLoop(*edited, 3);
        noLoop.modify<model::Item>(itemId, [](auto& it) { it.loop = false; });
        auto plain = noLoop.commit();
        REQUIRE(plain);
        draw(*plain, 2.25, 160, 2);  // nonlooping notes beyond source length still play
    }
    {
        model::EditTxn txn(*committed, 2);
        txn.modify<model::Take>(takeId, [](auto& tk) { tk.startOffset = 3.5; });
        txn.modify<model::Item>(itemId, [](auto& it) { it.length = 3; });
        txn.modify<model::MidiSource>(source, [](auto& src) {
            src.sequence.notes.insert(src.sequence.notes.begin(), {0, 480, 0, 72, 127, 0});
        });
        auto edited = txn.commit();
        REQUIRE(edited);
        draw(*edited, 0, 160, 2);  // include the offset's final partial source cycle
    }
    {
        model::EditTxn txn(*committed, 2);
        txn.modify<model::MidiSource>(source, [](auto& src) { src.sequence.length = 0; });
        auto edited = txn.commit();
        REQUIRE(edited);
        draw(*edited, 0, 160, 1);  // playback uses one pass when no loop length exists
    }

    {
        model::EditTxn txn(*committed, 2);
        txn.modify<model::MidiSource>(source, [](auto& src) {
            src.sequence.length = 1;
            src.sequence.notes = {{0, 2, 0, 72, 127, 0}};
        });
        txn.modify<model::Item>(itemId, [](auto& it) { it.length = 100; });
        auto edited = txn.commit();
        REQUIRE(edited);
        draw(*edited, 0, 1, 8192);  // keep the pass/glyph drawing budget
    }
}

TEST_CASE("audit3: arrange and layout ignore version deltas across generations") {
    Gl& g = gl();
    REQUIRE(g.ok);
    Session s;
    REQUIRE(s.build(1));
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto node =
        std::make_unique<views::ArrangeView>(*s.peaks, s.resolve(), views::TransportSource{});
    auto* view = node.get();
    root.setContent(std::move(node));
    auto before = s.app->head();
    view->show(before, nullptr, 1);
    root.frame(false);
    auto track = before.tracks.begin()[1].id;
    auto args = json::Value::object();
    args["track"] = track.str();
    args["name"] = "replacement";
    REQUIRE(s.dispatcher->execute({"track.set", args, {}}));
    auto after = s.app->head();
    after.version = before.version;
    const auto rebuilds = view->trackLayout().rebuilds();
    std::vector<model::Change> emptyDelta;
    view->show(after, &emptyDelta, 2);
    CHECK(view->trackLayout().rebuilds() == rebuilds + 1);
    CHECK(view->session().tracks.find(track)->name == "replacement");
    CHECK_FALSE(view->trackLayout().show(after, nullptr, 2));
    CHECK(view->trackLayout().show(after, &emptyDelta, 3));
}

TEST_CASE("playrate envelope scale: logarithmic ratios, unity and bounded editing") {
    model::Envelope env;
    env.target = {model::kMaster, "playrate"};
    const auto scale = views::envScale(env);
    REQUIRE(scale);
    CHECK(scale->kind == views::EnvScale::Kind::Playrate);
    CHECK(scale->reference() == 1);
    CHECK(scale->toNorm(0.25) == 0);
    CHECK(scale->toNorm(1) == 0.5);
    CHECK(scale->toNorm(4) == 1);
    CHECK(scale->fromNorm(-1) == 0.25);
    CHECK(scale->fromNorm(2) == 4);
    CHECK(scale->text(1) == "1.000x");
    CHECK(views::envTitle(env) == "Playrate");
    CHECK(views::envUnit(env, *scale) == "playrate");
    for (double value : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0})
        CHECK(scale->fromNorm(scale->toNorm(value)) == doctest::Approx(value));
    const std::vector<model::EnvPoint> points{{0, 1}, {4, 2}};
    const auto moved = views::movePoints(points, points, 0, 2, *scale);
    for (const auto& point : moved.points) CHECK(point.value == 4);
    env.target.object = {model::Kind::Track, 1};
    CHECK_FALSE(views::envScale(env));
    env.target.object = {model::Kind::Route, 0};
    CHECK_FALSE(views::envScale(env));
}

TEST_CASE("a master's lane: armed or holding items, above its automation lanes (ADR-0057)") {
    model::EditTxn txn(model::Session::create(), 1);
    model::Track track;
    track.parent = model::kMaster;
    const auto child = txn.create(track);
    txn.modify<model::Track>(model::kMaster,
                             [&](auto& master) { master.children.push_back(child); });
    model::Envelope env;
    env.target = {model::kMaster, "volume"};
    env.points = {{0, 0}};
    const auto masterEnv = txn.create(env);
    auto s = txn.commit().value();
    views::TrackLayout layout;
    REQUIRE(layout.show(s));
    CHECK_FALSE(layout.indexOf(model::kMaster));
    CHECK(layout.indexOfEnv(masterEnv) == 0);
    // Armed: the master's row first, its lanes, then its children at depth 0.
    model::EditTxn arm(s, 2);
    arm.modify<model::Track>(model::kMaster, [](auto& m) { m.record.armed = true; });
    auto armed = arm.commit().value();
    const auto armChanges = arm.changes();
    REQUIRE(layout.show(armed, &armChanges));
    CHECK(layout.indexOf(model::kMaster) == 0);
    CHECK(layout.indexOfEnv(masterEnv) == 1);
    CHECK(layout.indexOf(child) == 2);
    CHECK(layout.rows()[2].depth == 0);
    // Disarmed with a print: the lane stays; without either it goes.
    model::EditTxn print(armed, 3);
    print.modify<model::Track>(model::kMaster, [](auto& m) { m.record.armed = false; });
    model::Take take;
    const auto tk = print.create(take);
    model::Item item;
    item.track = model::kMaster;
    item.length = 1;
    item.takes = {tk};
    item.activeTake = tk;
    const auto it = print.create(item);
    auto printed = print.commit().value();
    const auto printChanges = print.changes();
    layout.show(printed, &printChanges);
    CHECK(layout.indexOf(model::kMaster) == 0);
    model::EditTxn gone(printed, 4);
    gone.remove<model::Item>(it);
    gone.remove<model::Take>(tk);
    auto plain = gone.commit().value();
    const auto goneChanges = gone.changes();
    REQUIRE(layout.show(plain, &goneChanges));
    CHECK_FALSE(layout.indexOf(model::kMaster));
}

TEST_CASE("master automation lanes are visible while hidden track lanes remain hidden") {
    model::EditTxn txn(model::Session::create(), 1);
    model::Track track;
    track.parent = model::kMaster;
    track.hiddenTcp = true;
    const auto hidden = txn.create(track);
    txn.modify<model::Track>(model::kMaster,
                             [&](auto& master) { master.children.push_back(hidden); });
    model::Envelope env;
    env.target = {model::kMaster, "playrate"};
    env.points = {{0, 1}};
    const auto masterEnv = txn.create(env);
    env.target = {hidden, "volume"};
    env.points = {{0, 0}};
    const auto hiddenEnv = txn.create(env);
    views::TrackLayout layout;
    REQUIRE(layout.show(txn.preview()));
    REQUIRE(layout.rowCount() == 1);
    CHECK(layout.indexOfEnv(masterEnv) == 0);
    CHECK_FALSE(layout.indexOf(model::kMaster));
    CHECK_FALSE(layout.indexOfEnv(hiddenEnv));
}

TEST_CASE("envelope point edits: move stops at the start and keeps time order; paste replaces") {
    using model::EnvPoint;
    const views::EnvScale volume{};
    const std::vector<EnvPoint> pts = {{1, -6}, {2, -12}, {4, 0}, {6, -3}};
    // Moving the first and third left by 3 beats: the group stops at 0.
    auto moved = views::movePoints(pts, {pts[0], pts[2]}, -3, 0, volume);
    REQUIRE(moved.points.size() == 4);
    CHECK(moved.points[0].pos == doctest::Approx(0));
    CHECK(moved.points[0].value == doctest::Approx(-6));
    CHECK(moved.points[1].pos == doctest::Approx(2));  // the unselected one, unchanged
    CHECK(moved.points[2].pos == doctest::Approx(3));
    CHECK(moved.selected.size() == 2);
    // Moved past a neighbour, the points stay in time order.
    moved = views::movePoints(pts, {pts[0]}, 2, 0, volume);
    CHECK(moved.points[0].pos == doctest::Approx(2));
    CHECK(moved.points[1].pos == doctest::Approx(3));
    // Values move on the lane and stop at its top.
    moved = views::movePoints(pts, {pts[2]}, 0, 2.0, volume);
    CHECK(volume.toNorm(moved.points[2].value) == doctest::Approx(1.0));
    // A stale selection entry names nothing.
    CHECK(views::selectedIndices(pts, {EnvPoint{5, 0}}).empty());
    // Paste over beats 1..3: the points in that span are replaced.
    const auto clip = views::copyPoints(pts, {pts[2], pts[3]}, volume, "volume");
    REQUIRE(clip.points.size() == 2);
    CHECK(clip.points[0].pos == 0);
    CHECK(clip.points[1].pos == doctest::Approx(2));
    const auto pasted = views::pastePoints(pts, clip, 1, volume, "volume");
    REQUIRE(pasted.points.size() == 4);  // {1,-6} and {2,-12} replaced by two
    CHECK(pasted.points[0].pos == doctest::Approx(1));
    CHECK(pasted.points[0].value == doctest::Approx(0));
    CHECK(pasted.points[1].pos == doctest::Approx(3));
    CHECK(pasted.selected.size() == 2);
    // Deleting and shaping touch only the selection.
    CHECK(views::deletePoints(pts, {pts[1]}).points.size() == 3);
    const auto shaped = views::shapePoints(pts, {pts[3]}, model::PointShape::Square);
    CHECK(shaped.points[3].shape == model::PointShape::Square);
    CHECK(shaped.points[0].shape == model::PointShape::Linear);
}

TEST_CASE("the arrangement's wheel follows the mouse map over the lanes and the TCP") {
    Gl& g = gl();
    REQUIRE(g.ok);
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto* arrange = mount(root);
    arrange->trackLayout().setGlobalHeight(200);  // room to scroll
    arrange->setViewport({10, 40, 0});
    root.frame(false);
    ui::PointerEvent wheel{ui::PointerEvent::Type::Wheel,
                           {arrange->lanesRect().x + 100, arrange->lanesRect().y + 50}};
    wheel.wheelY = -1;
    // Plain over the lanes scrolls the tracks once rebound, instead of zooming.
    REQUIRE(arrange->gestures().mouse.set("arrange.wheel", 0, "scroll"));
    root.pointer(wheel);
    CHECK(arrange->viewport().scrollY > 0);
    CHECK(arrange->viewport().pixelsPerSecond == 40);
    // Over the TCP, a binding to zoom zooms in time.
    REQUIRE(arrange->gestures().mouse.set("tcp.wheel", ui::kAlt, "zoom"));
    wheel.pos.x = arrange->tcpRowRect(0).x + 20;
    wheel.mods = ui::kAlt;
    root.pointer(wheel);
    CHECK(arrange->viewport().pixelsPerSecond < 40);
    // Both are saved and read back.
    auto saved = views::MouseMap::fromJson(arrange->gestures().mouse.toJson());
    REQUIRE(saved);
    CHECK(saved->behaviour("arrange.wheel", 0) == "scroll");
    CHECK(saved->behaviour("arrange.wheel", ui::kShift) == "scroll_time");
    CHECK(saved->behaviour("tcp.wheel", ui::kAlt) == "zoom");
}
