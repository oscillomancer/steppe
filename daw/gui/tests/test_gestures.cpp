// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The arrange view's editing gestures (P5.6), driven like a user: pointer
// events through the root; the model read back; one history entry each.
#include <doctest/doctest.h>
#include "test_support.h"

#include <filesystem>
#include <mutex>

#include <cmath>
#include <condition_variable>
#include "audio_edit.h"
#include "generate.h"
#include "generated_media.h"
#include "plugin_scales.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/midi/midi.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/tcp.h"

using namespace supple;

namespace {

const cmd::Origin kUi{cmd::Origin::Type::Ui, "gui", {}};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<peaks::PeakService> peaks;
    std::unique_ptr<ui::Root> root;
    views::ArrangeView* view = nullptr;
    std::vector<model::Id> opened;
    std::vector<double> seeks;
    bool ok = false;

    Fixture(std::function<void()> jobFinished = {}) {
        dir = std::filesystem::temp_directory_path() /
              ("supple-gesture-test-" + std::to_string(testProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
#if !defined(__linux__)
        REQUIRE_MESSAGE(p, (p ? "" : p.error().message));
#endif
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"gesture-test", 1100, 600, true});
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
        options.jobFinishedHook = std::move(jobFinished);
        options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};  // the stock plugins only
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        gui::test::linkGeneratedMedia(app->mediaDir());
        REQUIRE(gui::generateSession(*dispatcher, app->mediaDir(), 6));
        peaks = std::make_unique<peaks::PeakService>(dir / "cache", [](std::function<void()>) {});
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        auto arrange = std::make_unique<views::ArrangeView>(
            *peaks,
            [this](const model::MediaSource& src) {
                const std::filesystem::path path(src.path);
                return path.is_absolute() ? path : app->mediaDir() / path;
            },
            views::TransportSource{});
        view = arrange.get();
        root->setContent(std::move(arrange));
        view->installOverlay(*root);
        auto& g = view->gestures();
        g.pinVersion = [this](std::uint64_t version) { return app->versions().pin(version); };
        g.run = [this](const std::string& label, const std::vector<views::EditStep>& steps) {
            return dispatcher->transact(label, kUi, [&]() -> Result<json::Value> {
                std::vector<json::Value> results;
                for (views::EditStep step : steps) {
                    for (auto& [key, value] : step.args.asObject())
                        if (value.isString() && value.asString().rfind("$step", 0) == 0) {
                            const std::string ref = value.asString().substr(5);
                            const auto dot = ref.find('.');
                            value =
                                *results[std::stoul(ref.substr(0, dot))].find(ref.substr(dot + 1));
                        }
                    auto r = dispatcher->execute({step.command, step.args, kUi});
                    if (!r) return r;
                    results.push_back(*r);
                }
                return results.empty() ? json::Value::object()
                                       : results.back();  // the last step's result
            });
        };
        g.onOpenMidi = [this](model::Id id) { opened.push_back(id); };
        g.onSeek = [this](double s) { seeks.push_back(s); };
        g.onError = [](const Error& e) { MESSAGE("gesture refused: " << e.toString()); };
        views::Viewport v = view->viewport();
        v.pixelsPerSecond = 16;  // the audio items and the MIDI items at bar 21 both visible
        view->setViewport(v);
        sync();
        ok = true;
    }
    ~Fixture() {
        view = nullptr;
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
    std::size_t steps() {
        auto r = dispatcher->execute({"history.list", json::Value::object(), kUi});
        return r->find("versions")->size();
    }
    const model::Item& item(model::Id id) { return *app->head().items.find(id); }
    // The first item on a row whose active take is audio (or MIDI).
    model::Id itemOnRow(std::size_t row, bool midi = false) {
        const model::Session& s = app->head();
        const model::Id track = view->trackLayout().rows()[row].track;
        std::optional<model::Id> best;
        double bestPos = 1e300;
        for (const auto& e : s.items) {
            if (e.value->track != track) continue;
            const model::Take* t =
                e.value->activeTake ? s.takes.find(*e.value->activeTake) : nullptr;
            const bool isMidi = t && t->source && t->source->kind == model::Kind::Midi;
            if (isMidi != midi || e.value->position >= bestPos) continue;
            best = e.id;
            bestPos = e.value->position;
        }
        REQUIRE(best);
        return *best;
    }
    ui::Rect rectOf(model::Id id) {
        const model::Item& it = item(id);
        return view->lanes().itemRect(*view->trackLayout().indexOf(it.track), it);
    }
    float beatPx() {
        const model::Session& s = app->head();
        return static_cast<float>((s.timeMap->secondsAt(1) - s.timeMap->secondsAt(0)) *
                                  view->viewport().pixelsPerSecond);
    }
    void pointer(ui::PointerEvent::Type type, ui::Point p, int button = 1, int clicks = 1,
                 unsigned mods = 0) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = button;
        e.clicks = clicks;
        e.mods = mods;
        root->pointer(e);
    }
    void drag(ui::Point from, ui::Point to, unsigned mods = 0, int button = 1) {
        pointer(ui::PointerEvent::Type::Down, from, button, 1, mods);
        pointer(ui::PointerEvent::Type::Move, {(from.x + to.x) / 2, (from.y + to.y) / 2}, button, 1,
                mods);
        pointer(ui::PointerEvent::Type::Move, to, button, 1, mods);
        pointer(ui::PointerEvent::Type::Up, to, button, 1, mods);
        sync();
    }
    void click(ui::Point p, unsigned mods = 0) {
        pointer(ui::PointerEvent::Type::Down, p, 1, 1, mods);
        pointer(ui::PointerEvent::Type::Up, p, 1, 1, mods);
        sync();
    }
    // Picks row `index` of an open menu (rows are 26 px; separators count as rows).
    void menuRow(const std::string& id, int index) {
        root->frame(false);
        ui::Node* menu = root->find(id);
        REQUIRE(menu);
        const ui::Point p{menu->bounds().x + 30, menu->bounds().y + 26.0f * index + 13};
        pointer(ui::PointerEvent::Type::Move, p);
        pointer(ui::PointerEvent::Type::Down, p);
        pointer(ui::PointerEvent::Type::Up, p);
        sync();
    }
};

}  // namespace

TEST_CASE("arrange gestures: copy validates the unselected anchor's destination") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id anchor = f.itemOnRow(0), selected = f.itemOnRow(1);
    const auto env =
        f.dispatcher->execute({"env.add",
                               json::parse(R"({"track":")" + f.item(anchor).track.str() +
                                           R"(","param":"volume","points":[]})")
                                   .value(),
                               kUi});
    REQUIRE(env);
    f.sync();
    g.setSelection({selected});
    const ui::Rect r = f.rectOf(anchor);
    const ui::Point press{r.x + 14, r.y + r.h / 2};
    const ui::Rect lane = f.view->rowRect(1);
    f.pointer(ui::PointerEvent::Type::Down, press, 1, 1, ui::kCtrl);
    f.pointer(ui::PointerEvent::Type::Move, {press.x + 16, lane.y + lane.h / 2}, 1, 1, ui::kCtrl);
    CHECK(g.state().deltaRows == 0);  // the anchor would land on an envelope lane
    f.pointer(ui::PointerEvent::Type::Up, {press.x + 16, lane.y + lane.h / 2}, 1, 1, ui::kCtrl);
}

TEST_CASE(
    "arrange gestures: track deletion between preview and release cannot index a missing row") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id anchor = f.itemOnRow(1), selected = f.itemOnRow(0);
    g.setSelection({selected});
    const ui::Rect r = f.rectOf(anchor);
    const ui::Point press{r.x + 14, r.y + r.h / 2};
    const ui::Point target{press.x + 16, f.view->rowRect(4).y + r.h / 2};
    f.pointer(ui::PointerEvent::Type::Down, press, 1, 1, ui::kCtrl);
    f.pointer(ui::PointerEvent::Type::Move, target, 1, 1, ui::kCtrl);
    REQUIRE(g.state().deltaRows == 3);
    for (std::size_t row : {5u, 4u}) {
        const auto track = f.view->trackLayout().rows()[row].track;
        REQUIRE(f.dispatcher->execute(
            {"track.remove", json::parse(R"({"track":")" + track.str() + R"("})").value(), kUi}));
    }
    f.sync();
    const auto count = f.app->head().items.size();
    f.pointer(ui::PointerEvent::Type::Up, target, 1, 1, ui::kCtrl);
    CHECK(f.app->head().items.size() == count);
}

TEST_CASE("arrange gestures: select, move (snapped, free, across tracks), trim, slip, fades") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0), b = f.itemOnRow(1);
    const ui::Rect ra = f.rectOf(a);
    REQUIRE(ra.w > 40);
    // Click selects; Shift adds; Ctrl toggles.
    f.click({ra.x + ra.w / 2, ra.y + ra.h / 2});
    REQUIRE(g.selection() == std::vector<model::Id>{a});
    const ui::Rect rb = f.rectOf(b);
    f.click({rb.x + rb.w / 2, rb.y + rb.h / 2}, ui::kShift);
    CHECK(g.selection().size() == 2);
    f.click({rb.x + rb.w / 2, rb.y + rb.h / 2}, ui::kCtrl);
    CHECK(g.selection() == std::vector<model::Id>{a});

    // Move one item two beats later: snapped to the grid, one history entry.
    const double grid = g.gridBeats();
    const double posA = f.item(a).position;
    auto s0 = f.steps();
    const ui::Point mid{ra.x + ra.w / 2, ra.y + ra.h / 2};
    f.drag(mid, {mid.x + 2 * f.beatPx() + 1, mid.y});
    CHECK(f.steps() == s0 + 1);
    const double moved = f.item(a).position - posA;
    CHECK(moved > 0);
    CHECK(std::abs(std::round(f.item(a).position / grid) * grid - f.item(a).position) <
          1e-9);  // on the grid

    // Trim the end two grid steps shorter, then the start later (the take's
    // offset follows so the audio stays where it was).
    const ui::Rect rt = f.rectOf(a);
    const double len0 = f.item(a).length;
    const float step = static_cast<float>(grid) * f.beatPx();
    f.drag({rt.right() - 2, rt.y + rt.h / 2}, {rt.right() - 2 - 2 * step, rt.y + rt.h / 2});
    CHECK(f.item(a).length == doctest::Approx(len0 - 2 * grid));
    const ui::Rect rl = f.rectOf(a);
    const model::Take& take0 = *f.app->head().takes.find(*f.item(a).activeTake);
    const double offset0 = take0.startOffset, pos0 = f.item(a).position, len1 = f.item(a).length;
    s0 = f.steps();
    f.drag({rl.x + 2, rl.y + rl.h / 2}, {rl.x + 2 + step, rl.y + rl.h / 2});
    CHECK(f.steps() == s0 + 1);  // item.set and take.set together
    const double d = f.item(a).position - pos0;
    CHECK(d > 0);
    CHECK(f.item(a).length == doctest::Approx(len1 - d));
    const model::Session& sn = f.app->head();
    const double seconds = sn.timeMap->secondsAt(f.item(a).position) - sn.timeMap->secondsAt(pos0);
    CHECK(sn.takes.find(*f.item(a).activeTake)->startOffset == doctest::Approx(offset0 + seconds));

    // Slip: Alt-drag moves the source under the item, not the item (right:
    // earlier material comes into view, the offset shrinks; not snapped).
    const ui::Rect rs = f.rectOf(a);
    const double posS = f.item(a).position;
    const double offS = f.app->head().takes.find(*f.item(a).activeTake)->startOffset;
    REQUIRE(offS > 0.4);
    f.drag({rs.x + rs.w / 2, rs.y + rs.h / 2}, {rs.x + rs.w / 2 + f.beatPx() / 2, rs.y + rs.h / 2},
           ui::kAlt);
    CHECK(f.item(a).position == posS);
    CHECK(f.app->head().takes.find(*f.item(a).activeTake)->startOffset ==
          doctest::Approx(offS - 0.25));

    // Fade in from the top-left handle.
    const ui::Rect rf = f.rectOf(a);
    f.drag({rf.x + 3, rf.y + 3}, {rf.x + 3 + 2 * f.beatPx(), rf.y + 3});
    CHECK(f.item(a).fadeIn.length > 0.3);

    // Ctrl-drag copies the item two grid steps later, on the same track; the
    // original stays (one undo step).
    {
        const std::size_t count = f.app->head().items.size();
        const ui::Rect rc = f.rectOf(a);
        const double posC = f.item(a).position;
        auto s1 = f.steps();
        f.drag({rc.x + rc.w / 2, rc.y + rc.h / 2}, {rc.x + rc.w / 2 + 2 * step, rc.y + rc.h / 2},
               ui::kCtrl);
        CHECK(f.app->head().items.size() == count + 1);
        CHECK(f.item(a).position == posC);
        CHECK(f.steps() == s1 + 1);
    }

    // Down one row (last: row 1 has items of its own over this one) (row 1 is a channel), the item
    // lands on that track.
    const model::Id trackB = f.item(b).track;
    const ui::Rect ra2 = f.rectOf(a);
    const ui::Rect row1 = f.view->rowRect(1);
    f.drag({ra2.x + 14, ra2.y + ra2.h / 2}, {ra2.x + 14, row1.y + row1.h / 2});  // left of the copy
    CHECK(f.item(a).track == trackB);

    // A locked item does not move.
    REQUIRE(f.dispatcher->execute(
        {"item.set", json::parse(R"({"item":")" + b.str() + R"(","locked":true})").value(), kUi}));
    f.sync();
    const ui::Rect rlk = f.rectOf(b);
    const double posB = f.item(b).position;
    f.drag({rlk.x + rlk.w / 2, rlk.y + rlk.h / 2},
           {rlk.x + rlk.w / 2 + 3 * f.beatPx(), rlk.y + rlk.h / 2});
    CHECK(f.item(b).position == posB);
}

// The app re-applies its own copy of the selection every frame (Shell::sync):
// a frame between the marquee's last move and its release must not undo it.
TEST_CASE("arrange gestures: a marquee survives the app re-applying the selection each frame") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    std::vector<model::Id> published;
    g.onSelection = [&](const std::vector<model::Id>& items) { published = items; };
    const ui::Rect lanes = f.view->lanesRect();
    const ui::Rect r0 = f.view->rowRect(0), r1 = f.view->rowRect(1);
    const ui::Point from{lanes.x + 4, r0.y + 4}, to{lanes.x + 200, r1.bottom() - 4};
    using T = ui::PointerEvent::Type;
    f.pointer(T::Down, from, 3);
    f.pointer(T::Move, {(from.x + to.x) / 2, (from.y + to.y) / 2}, 3);
    f.pointer(T::Move, to, 3);
    g.setSelection(published);  // a frame: the app's copy goes back to the view
    f.pointer(T::Up, to, 3);
    CHECK(published.size() >= 2);
    CHECK(g.selection() == published);
}

TEST_CASE("arrange gestures: marquee, click to seek, take badge, double-click MIDI, file drop") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    // A right-drag marquee over the first two rows selects their items.
    const ui::Rect lanes = f.view->lanesRect();
    const ui::Rect r0 = f.view->rowRect(0), r1 = f.view->rowRect(1);
    f.drag({lanes.x + 4, r0.y + 4}, {lanes.x + 200, r1.bottom() - 4}, 0, 3);
    CHECK(g.selection().size() >= 2);
    // A click on empty space clears the selection and seeks there.
    const model::Id midi = f.itemOnRow(0, true);
    const ui::Rect rm = f.rectOf(midi);
    const ui::Point empty{rm.x - 20, rm.y + rm.h / 2};  // between the audio item and the MIDI item
    REQUIRE_FALSE(g.hit(empty));
    f.click(empty);
    CHECK(g.selection().empty());
    REQUIRE(f.seeks.size() == 1);
    CHECK(f.seeks[0] == doctest::Approx(f.view->timeAt(empty.x)));
    // Double-click on a MIDI item asks to open it.
    f.pointer(ui::PointerEvent::Type::Down, {rm.x + rm.w / 2, rm.y + rm.h / 2}, 1, 2);
    f.pointer(ui::PointerEvent::Type::Up, {rm.x + rm.w / 2, rm.y + rm.h / 2});
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0] == midi);
    // The take badge cycles the active take.
    std::optional<model::Id> twoTakes;
    for (const auto& e : f.app->head().items)
        if (e.value->takes.size() > 1) twoTakes = e.id;
    REQUIRE(twoTakes);
    const model::Id before = *f.item(*twoTakes).activeTake;
    const ui::Rect rt = f.rectOf(*twoTakes);
    auto s0 = f.steps();
    f.click({rt.right() - 16, rt.y + 8});
    CHECK(*f.item(*twoTakes).activeTake != before);
    CHECK(f.steps() == s0 + 1);
    // Files dropped: the first on the channel under the point, the next on a new track.
    audio::AudioData tone;
    tone.channels.assign(2, std::vector<float>(24000, 0.2f));
    for (const char* name : {"x.wav", "y.wav"})
        REQUIRE(
            writeFileAtomic(f.dir / name, std::span<const std::uint8_t>(*audio::encodeWav(tone))));
    const std::size_t items = f.app->head().items.size(), tracks = f.app->head().tracks.size();
    s0 = f.steps();
    REQUIRE(g.drop({f.dir / "x.wav", f.dir / "y.wav"}, {lanes.x + 300, r1.y + r1.h / 2}));
    CHECK(f.app->head().items.size() == items + 2);
    CHECK(f.app->head().tracks.size() == tracks + 1);
    CHECK(f.steps() == s0 + 1);  // one undo step
    // A MIDI file (any case of .mid) becomes a MIDI item on a new track.
    midi::Sequence notes;
    notes.notes.push_back({0, 960, 0, 60, 100, 0});
    notes.length = 960;
    REQUIRE(writeFileAtomic(f.dir / "z.MID", *midi::writeSmf(notes, {}, {})));
    const std::size_t sequences = f.app->head().midi.size();
    REQUIRE(g.drop({f.dir / "z.MID"}, {lanes.x + 300, 4000}));
    CHECK(f.app->head().items.size() == items + 3);
    CHECK(f.app->head().midi.size() == sequences + 1);
}

TEST_CASE("automation lanes: shown under their strip, points added, moved and deleted, modes") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::pair<std::string, json::Value>> committed;
    views::TcpCallbacks cb;
    cb.commit = [&](std::string_view command, json::Value args, std::string) {
        committed.emplace_back(std::string(command), args);
        REQUIRE(f.dispatcher->execute({std::string(command), std::move(args), kUi}));
    };
    f.view->setTcpCallbacks(std::move(cb));
    const model::Id track = f.view->trackLayout().rows()[0].track;
    auto added = f.dispatcher->execute(
        {"env.add",
         json::parse(
             R"({"track":")" + track.str() +
             R"(","param":"volume","points":[{"beat":0,"value":0},{"beat":16,"value":-12}]})")
             .value(),
         kUi});
    REQUIRE(added);
    const model::Id env = *model::Id::parse((*added)["env"].asString());
    f.sync();
    // The lane is the row right under its strip, shorter, with no items on it.
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    CHECK(*lane == *f.view->trackLayout().indexOf(track) + 1);
    CHECK(f.view->trackLayout().heightOf(*lane) < f.view->trackLayout().heightOf(*lane - 1));
    const ui::Rect laneRect = f.view->rowRect(*lane);
    auto& g = f.view->gestures();
    CHECK_FALSE(g.hit({laneRect.x + 30, laneRect.y + laneRect.h / 2}));
    // Its header in the TCP.
    REQUIRE(f.view->tcpPanel().envControl(env, "mode"));
    CHECK(f.view->tcpPanel().envControl(env, "mode")->visible);

    auto points = [&] { return f.app->head().envelopes.find(env)->points; };
    const ui::Rect area = views::Lanes::envArea(laneRect);
    const model::Session& s0 = f.app->head();
    auto xOfBeat = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    (void)s0;
    // A click on the lane adds a point there (snapped to the grid), one undo step.
    auto steps = f.steps();
    f.click({xOfBeat(8) + 1, area.y + area.h * 0.25f});
    REQUIRE(points().size() == 3);
    CHECK(points()[1].pos == doctest::Approx(8));
    CHECK(points()[1].value > 0);  // the upper quarter is above unity
    CHECK(f.steps() == steps + 1);
    // Dragging the last point up raises it.
    const double before = points()[2].value;
    const float yLast =
        area.bottom() - static_cast<float>(views::EnvScale{}.toNorm(before)) * area.h;
    f.drag({xOfBeat(16), yLast}, {xOfBeat(16), yLast - 12});
    REQUIRE(points().size() == 3);
    CHECK(points()[2].value > before);
    CHECK(points()[2].pos == doctest::Approx(16));  // a vertical drag keeps the time
    // Alt+click deletes the first point.
    const float yFirst = area.bottom() - static_cast<float>(views::EnvScale{}.toNorm(0.0)) * area.h;
    f.click({xOfBeat(0) + 1, yFirst}, ui::kAlt);
    CHECK(points().size() == 2);
    // The header's mode button cycles read -> touch; "hide" hides the lane.
    f.root->click(f.view->tcpPanel().envControl(env, "mode")->id());
    CHECK(f.app->head().envelopes.find(env)->mode == model::EnvMode::Touch);
    f.sync();
    f.root->click(f.view->tcpPanel().envControl(env, "hide")->id());
    f.sync();
    CHECK_FALSE(f.app->head().envelopes.find(env)->visible);
    CHECK_FALSE(f.view->trackLayout().indexOfEnv(env));
}

TEST_CASE("automation lanes: items inserted, edited inside, moved, resized, pooled, deleted") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.view->trackLayout().rows()[0].track;
    auto added = f.dispatcher->execute(
        {"env.add",
         json::parse(R"({"track":")" + track.str() +
                     R"(","param":"volume","points":[{"beat":0,"value":0},{"beat":32,"value":0}]})")
             .value(),
         kUi});
    REQUIRE(added);
    const model::Id env = *model::Id::parse((*added)["env"].asString());
    f.sync();
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    const ui::Rect laneRect = f.view->rowRect(*lane);
    const ui::Rect area = views::Lanes::envArea(laneRect);
    auto xOfBeat = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    auto items = [&] {
        std::vector<std::pair<model::Id, model::AutomationItem>> out;
        for (const auto& e : f.app->head().autoItems) out.emplace_back(e.id, *e.value);
        return out;
    };
    auto rightClick = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Down, p, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 3);
        f.sync();
        REQUIRE(f.root->find("env.menu"));
    };
    const float body = area.y + area.h * 0.6f;
    // The lane's menu: points (rows 0-7), then automation items from row 9.
    constexpr int kItems = 9;
    // Insert: a 4-beat item at the pointer, capturing the flat envelope.
    rightClick({xOfBeat(4) + 1, body});
    f.menuRow("env.menu", kItems);
    REQUIRE(items().size() == 1);
    const model::Id ai = items()[0].first;
    CHECK(items()[0].second.position == doctest::Approx(4));
    CHECK(items()[0].second.length == doctest::Approx(4));
    // A click in its body adds a point to the item, not the envelope (above unity).
    auto steps = f.steps();
    const std::size_t pointsBefore = items()[0].second.points.size();
    const float above = area.bottom() - static_cast<float>(views::EnvScale{}.toNorm(1.0)) * area.h;
    REQUIRE(above > laneRect.y + 1 + views::Lanes::kAutoHeader);
    f.click({xOfBeat(6) + 1, above});
    CHECK(f.steps() == steps + 1);
    REQUIRE(items()[0].second.points.size() == pointsBefore + 1);
    CHECK(f.app->head().envelopes.find(env)->points.size() == 2);
    const auto played = model::effectivePoints(f.app->head(), env);
    CHECK(model::pointsValueAt(played, 6.0, 0.0) > 0);
    // Its header moves it (snapped), its right edge resizes it.
    const float header = laneRect.y + 1 + views::Lanes::kAutoHeader / 2;
    f.drag({xOfBeat(5), header}, {xOfBeat(9), header});
    CHECK(f.app->head().autoItems.find(ai)->position == doctest::Approx(8));
    // Dragging the right edge stretches its shape with it (its points and the
    // span they repeat over); Alt extends it by repeating the shape.
    const double sourceBefore = f.app->head().autoItems.find(ai)->sourceLength;
    const double lastPoint = f.app->head().autoItems.find(ai)->points.back().pos;
    f.drag({xOfBeat(12) - 2, body}, {xOfBeat(14) - 2, body});
    CHECK(f.app->head().autoItems.find(ai)->length == doctest::Approx(6));
    CHECK(f.app->head().autoItems.find(ai)->sourceLength == doctest::Approx(sourceBefore * 1.5));
    CHECK(f.app->head().autoItems.find(ai)->points.back().pos == doctest::Approx(lastPoint * 1.5));
    f.drag({xOfBeat(14) - 2, body}, {xOfBeat(16) - 2, body}, ui::kAlt);
    CHECK(f.app->head().autoItems.find(ai)->length == doctest::Approx(8));
    CHECK(f.app->head().autoItems.find(ai)->sourceLength == doctest::Approx(sourceBefore * 1.5));
    f.drag({xOfBeat(16) - 2, body}, {xOfBeat(14) - 2, body}, ui::kAlt);
    CHECK(f.app->head().autoItems.find(ai)->length == doctest::Approx(6));
    // Item rows: Duplicate (pooled), Duplicate (copy), Loop, Delete.
    rightClick({xOfBeat(9), body});
    f.menuRow("env.menu", kItems);
    REQUIRE(items().size() == 2);
    for (const auto& [id, it] : items()) CHECK(it.pool == 1);
    // Editing points in the copy edits the pool.
    model::Id copy = items()[0].first == ai ? items()[1].first : items()[0].first;
    CHECK(f.app->head().autoItems.find(copy)->position == doctest::Approx(14));
    f.click({xOfBeat(15) + 1, area.y + area.h * 0.9f});
    CHECK(f.app->head().autoItems.find(ai)->points.size() ==
          f.app->head().autoItems.find(copy)->points.size());
    CHECK(f.app->head().autoItems.find(ai)->points.size() == pointsBefore + 2);
    // Unpool appears for a pooled item: dup, dup, Unpool, Loop, Delete.
    rightClick({xOfBeat(15) + 1, body});
    f.menuRow("env.menu", kItems + 2);
    CHECK(f.app->head().autoItems.find(copy)->pool == 0);
    rightClick({xOfBeat(15) + 1, body});
    f.menuRow("env.menu", kItems + 2);
    CHECK_FALSE(f.app->head().autoItems.find(copy)->loop);
    rightClick({xOfBeat(15) + 1, body});
    f.menuRow("env.menu", kItems + 3);
    CHECK(items().size() == 1);
    // Deleting the block during a point gesture must not write its draft into
    // the underlying envelope instead.
    const auto remaining = items().front().first;
    const auto& remainingItem = *f.app->head().autoItems.find(remaining);
    const ui::Point point{xOfBeat(remainingItem.position + remainingItem.length / 2),
                          area.y + area.h * 0.85f};
    f.pointer(ui::PointerEvent::Type::Down, point);
    f.pointer(ui::PointerEvent::Type::Move, {point.x, point.y - 3});
    REQUIRE(f.view->gestures().state().autoItem == remaining);
    REQUIRE(f.dispatcher->execute(
        {"autoitem.remove", json::parse(R"({"item":")" + remaining.str() + R"("})").value(), kUi}));
    f.sync();
    const auto underlying = f.app->head().envelopes.find(env)->points;
    const auto version = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Up, {point.x, point.y - 3});
    const auto& after = f.app->head().envelopes.find(env)->points;
    REQUIRE(after.size() == underlying.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        CHECK(after[i].pos == underlying[i].pos);
        CHECK(after[i].value == underlying[i].value);
    }
    CHECK(f.app->head().version == version);
}

TEST_CASE("arrange gestures: the mouse map rebinds modifiers per context (gap log #37)") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0);
    const float step = static_cast<float>(g.gridBeats()) * f.beatPx();
    // Alt-drag copies instead of slipping; Ctrl-drag moves.
    REQUIRE(g.mouse.set("item.drag", ui::kAlt, "copy"));
    REQUIRE(g.mouse.set("item.drag", ui::kCtrl, "move"));
    const std::size_t count = f.app->head().items.size();
    auto offsetOf = [&] { return f.app->head().takes.find(*f.item(a).activeTake)->startOffset; };
    const double pos = f.item(a).position, offset = offsetOf();
    ui::Rect r = f.rectOf(a);
    f.drag({r.x + r.w / 2, r.y + r.h / 2}, {r.x + r.w / 2 + 2 * step, r.y + r.h / 2}, ui::kAlt);
    CHECK(f.app->head().items.size() == count + 1);
    CHECK(f.item(a).position == pos);
    CHECK(offsetOf() == offset);  // not slipped
    r = f.rectOf(a);
    f.drag({r.x + 14, r.y + r.h / 2}, {r.x + 14 + step, r.y + r.h / 2}, ui::kCtrl);
    CHECK(f.app->head().items.size() == count + 1);
    CHECK(f.item(a).position == doctest::Approx(pos + g.gridBeats()));
    // Ctrl+click on an item now extends the selection (it toggled by default).
    REQUIRE(g.mouse.set("item.click", ui::kCtrl, "extend"));
    const model::Id b = f.itemOnRow(1);
    f.click({f.rectOf(a).x + 14, f.rectOf(a).y + f.rectOf(a).h / 2});
    f.click({f.rectOf(b).x + 14, f.rectOf(b).y + f.rectOf(b).h / 2}, ui::kCtrl);
    f.click({f.rectOf(b).x + 14, f.rectOf(b).y + f.rectOf(b).h / 2}, ui::kCtrl);
    CHECK(g.selection().size() == 2);
    // Unknown behaviours are refused with the context's choices.
    const auto bad = g.mouse.set("item.drag", ui::kAlt, "fly");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().hint.find("slip") != std::string::npos);
    REQUIRE(g.mouse.set("item.drag", 0, "copy"));
    REQUIRE(g.mouse.set("item.drag", ui::kAlt, ""));
    CHECK(g.mouse.behaviour("item.drag", ui::kAlt) == "copy");
    CHECK(g.mouse.behaviour("item.drag", ui::kCtrl | ui::kAlt | ui::kShift) == "copy");
    auto restored = views::MouseMap::fromJson(g.mouse.toJson());
    REQUIRE(restored);
    CHECK(restored->behaviour("item.drag", ui::kAlt) == "copy");
}

TEST_CASE("arrange gestures: Alt on an item's right edge stretches it (length and play rate)") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0);
    const ui::Rect r = f.rectOf(a);
    const double len0 = f.item(a).length;
    const float step = static_cast<float>(g.gridBeats()) * f.beatPx();
    auto steps = f.steps();
    // Longer by two grid steps: the take plays slower so its material fills it.
    f.drag({r.right() - 2, r.y + r.h / 2}, {r.right() - 2 + 2 * step, r.y + r.h / 2}, ui::kAlt);
    const double len1 = f.item(a).length;
    CHECK(len1 == doctest::Approx(len0 + 2 * g.gridBeats()));
    const model::Take& take = *f.app->head().takes.find(*f.item(a).activeTake);
    CHECK(take.playrate == doctest::Approx(len0 / len1));  // one tempo: beats scale as seconds
    CHECK(f.steps() == steps + 1);
}

TEST_CASE("arrange gestures: a move that overlaps another item crossfades both over the overlap") {
    Fixture f;
    if (!f.ok) return;
    const model::Id audio = f.itemOnRow(0), midi = f.itemOnRow(0, true);
    const double audioEnd = f.item(audio).position + f.item(audio).length;
    REQUIRE(f.item(midi).position > audioEnd);
    // Drag the later item back so it starts 4 beats before the earlier one ends.
    const double target = audioEnd - 4;
    const ui::Rect r = f.rectOf(midi);
    const float dx = static_cast<float>(target - f.item(midi).position) * f.beatPx();
    auto steps = f.steps();
    f.drag({r.x + 20, r.y + r.h / 2}, {r.x + 20 + dx, r.y + r.h / 2});
    REQUIRE(f.item(midi).position == doctest::Approx(target));
    const model::Session& s = f.app->head();
    const double overlap = s.timeMap->secondsAt(audioEnd) - s.timeMap->secondsAt(target);
    CHECK(f.item(audio).fadeOut.length == doctest::Approx(overlap));
    CHECK(f.item(midi).fadeIn.length == doctest::Approx(overlap));
    CHECK(f.steps() == steps + 1);  // the move and both fades: one undo step
    // Off: a move makes no fades.
    f.view->gestures().autoCrossfade = false;
    const ui::Rect r2 = f.rectOf(midi);
    f.drag({r2.x + 20, r2.y + r2.h / 2},
           {r2.x + 20 - 2 * f.beatPx() * static_cast<float>(f.view->gestures().gridBeats()),
            r2.y + r2.h / 2});
    CHECK(f.item(audio).fadeOut.length == doctest::Approx(overlap));  // unchanged
}

TEST_CASE("automation lanes: a plugin parameter's lane uses the plugin's range and curve") {
    Fixture f;
    if (!f.ok) return;
    auto listed = f.dispatcher->execute({"plugin.list", json::Value::object(), kUi});
    REQUIRE(listed);
    // A stock effect with a continuous semantic parameter (partial builds have none).
    std::string fxId, key;
    std::shared_ptr<params::DescribedParam> spec;
    const model::Id track = f.view->trackLayout().rows()[0].track;
    for (const json::Value& p : (*listed)["plugins"].asArray()) {
        if (p.find("instrument") && p.find("instrument")->asBool()) continue;
        auto added =
            f.dispatcher->execute({"fx.add",
                                   json::parse(R"({"track":")" + track.str() + R"(","plugin":")" +
                                               p.find("id")->asString() + R"("})")
                                       .value(),
                                   kUi});
        if (!added) continue;
        const std::string id = (*added)["fx"].asString();
        auto described = f.dispatcher->execute(
            {"fx.describe", json::parse(R"({"fx":")" + id + R"(","full":true})").value(), kUi});
        if (described)
            if (const json::Value* ps = described->find("params"); ps && ps->isObject())
                for (const auto& m : ps->asObject()) {
                    auto d = params::fromDescription(m.key, m.value);
                    if (!d || (*d)->spec.type != params::Type::Float ||
                        ((*d)->spec.flags & (params::kReadonly | params::kHidden)))
                        continue;
                    fxId = id;
                    key = m.key;
                    spec = std::shared_ptr<params::DescribedParam>(std::move(*d));
                    break;
                }
        if (spec) break;
    }
    if (!spec) {
        MESSAGE("skipped: no stock effect with a continuous parameter (partial build)");
        return;
    }
    const double high = params::fromNormalized(spec->spec, 0.9);
    auto added = f.dispatcher->execute(
        {"env.add",
         json::parse(R"({"fx":")" + fxId + R"(","param":")" + key +
                     R"(","points":[{"beat":0,"value":)" + std::to_string(spec->spec.def) +
                     R"(},{"beat":16,"value":)" + std::to_string(high) + "}]}")
             .value(),
         kUi});
    REQUIRE_MESSAGE(added, (added ? std::string() : added.error().toString()));
    const model::Id env = *model::Id::parse((*added)["env"].asString());
    // Without the app's scale source the lane is not shown; with it, it is.
    f.sync();
    CHECK_FALSE(f.view->trackLayout().indexOfEnv(env));
    auto scales = std::make_shared<gui::PluginScales>(*f.dispatcher);
    f.view->trackLayout().pluginScales = [scales](const model::Envelope& e) {
        return (*scales)(e);
    };
    (void)f.dispatcher->execute(
        {"env.set", json::parse(R"({"env":")" + env.str() + R"(","visible":true})").value(), kUi});
    f.sync();
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    CHECK(*lane == *f.view->trackLayout().indexOf(track) + 1);
    // A click in the lane's upper quarter adds a value three quarters up the
    // parameter's own curve.
    const ui::Rect area = views::Lanes::envArea(f.view->rowRect(*lane));
    const float x = f.view->xAt(f.app->head().timeMap->secondsAt(8)) + 1;
    f.click({x, area.y + area.h * 0.25f});
    const auto& pts = f.app->head().envelopes.find(env)->points;
    REQUIRE(pts.size() == 3);
    CHECK(params::toNormalized(spec->spec, pts[1].value) == doctest::Approx(0.75).epsilon(0.05));
}

TEST_CASE(
    "arrange gestures: a double-click on empty channel space makes an empty MIDI item there") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id audio = f.itemOnRow(0), midi = f.itemOnRow(0, true);
    const ui::Rect ra = f.rectOf(audio), rm = f.rectOf(midi);
    const ui::Point empty{(ra.right() + rm.x) / 2, rm.y + rm.h / 2};
    REQUIRE_FALSE(g.hit(empty));
    const std::size_t items = f.app->head().items.size();
    auto steps = f.steps();
    f.pointer(ui::PointerEvent::Type::Down, empty, 1, 2);
    f.pointer(ui::PointerEvent::Type::Up, empty);
    f.sync();
    REQUIRE(f.app->head().items.size() == items + 1);
    REQUIRE(f.opened.size() == 1);
    const model::Item& made = f.item(f.opened[0]);
    const model::Session& s = f.app->head();
    const int bar = s.timeMap->barBeat(s.timeMap->beatAt(f.view->timeAt(empty.x))).bar;
    CHECK(made.position == doctest::Approx(s.timeMap->beatOfBar(bar)));  // from the bar's start
    CHECK(made.length ==
          doctest::Approx(s.timeMap->beatOfBar(bar + 1) - s.timeMap->beatOfBar(bar)));
    const model::Take* take = s.takes.find(*made.activeTake);
    REQUIRE(take);
    REQUIRE(take->source);
    CHECK(take->source->kind == model::Kind::Midi);
    CHECK(s.midi.find(*take->source)->sequence.notes.empty());
    CHECK(g.selection() == std::vector<model::Id>{f.opened[0]});
    CHECK(f.steps() == steps + 1);
    // Over a range: from the start of the bar holding the start to the end.
    auto ranged = g.newMidiItem(f.view->trackLayout().rows()[1].track, 5.0, 13.0);
    REQUIRE(ranged);
    CHECK(f.item(*ranged).position == doctest::Approx(4.0));
    CHECK(f.item(*ranged).length == doctest::Approx(9.0));
}

TEST_CASE("arrange clipboard: copy, cut, paste onto another channel, duplicate") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0);
    const double len = f.item(a).length;
    g.setSelection({a});
    // Copy, then paste at beat 200 onto row 1's channel: one undo step, selected.
    g.copySelection();
    REQUIRE(g.canPaste());
    const model::Id row1 = f.view->trackLayout().rows()[1].track;
    auto steps = f.steps();
    REQUIRE(g.paste(200, row1));
    f.sync();
    REQUIRE(g.selection().size() == 1);
    const model::Id pasted = g.selection()[0];
    CHECK(pasted != a);
    CHECK(f.item(pasted).track == row1);
    CHECK(f.item(pasted).position == doctest::Approx(200));
    CHECK(f.item(pasted).length == doctest::Approx(len));
    CHECK(f.steps() == steps + 1);
    // Cut removes it; a paste brings it back as it was (from the copy's version).
    g.setSelection({pasted});
    REQUIRE(g.cutSelection());
    f.sync();
    CHECK_FALSE(f.app->head().items.find(pasted));
    REQUIRE(g.paste(300));
    f.sync();
    REQUIRE(g.selection().size() == 1);
    CHECK(f.item(g.selection()[0]).track == row1);  // its own row (no target given)
    CHECK(f.item(g.selection()[0]).position == doctest::Approx(300));
    // Duplicate: right after itself, on the same track.
    const model::Id last = g.selection()[0];
    REQUIRE(g.duplicateSelection());
    f.sync();
    REQUIRE(g.selection().size() == 1);
    CHECK(f.item(g.selection()[0]).position == doctest::Approx(300 + len));
    CHECK(f.item(g.selection()[0]).track == f.item(last).track);
}

TEST_CASE("take comping: click a take lane to hear it, swipe a range to comp it in") {
    Fixture f;
    if (!f.ok) return;
    std::optional<model::Id> it;
    for (const auto& e : f.app->head().items)
        if (e.value->takes.size() == 2) it = e.id;
    REQUIRE(it);
    const ui::Rect r = f.rectOf(*it);
    REQUIRE(views::Lanes::showsTakeLanes(r, 2));
    const std::size_t heard = static_cast<std::size_t>(
        std::find(f.item(*it).takes.begin(), f.item(*it).takes.end(), *f.item(*it).activeTake) -
        f.item(*it).takes.begin());
    const std::size_t other = 1 - heard;
    auto laneCenter = [&](const ui::Rect& ir, std::size_t lane, float fraction) {
        const ui::Rect l = views::Lanes::takeLaneRect(ir, 2, lane);
        return ui::Point{ir.x + ir.w * fraction, l.y + l.h / 2};
    };
    // A click on the other take's lane: the whole item plays it (one step).
    auto steps = f.steps();
    f.click(laneCenter(r, other, 0.5f));
    CHECK(*f.item(*it).activeTake == f.item(*it).takes[other]);
    CHECK(f.steps() == steps + 1);
    // Swipe the first take's lane across the middle: split twice, the middle
    // piece plays the first take, the outer pieces keep the other.
    const std::size_t items = f.app->head().items.size();
    steps = f.steps();
    const ui::Rect r2 = f.rectOf(*it);
    f.drag(laneCenter(r2, heard, 0.3f), laneCenter(r2, heard, 0.6f));
    CHECK(f.app->head().items.size() == items + 2);
    CHECK(f.steps() == steps + 1);
    const model::Session& s = f.app->head();
    std::vector<const model::Item*> pieces;
    for (const auto& e : s.items)
        if (e.value->track == f.item(*it).track && e.value->takes.size() == 2)
            pieces.push_back(e.value.get());
    std::sort(pieces.begin(), pieces.end(),
              [](auto* a, auto* b) { return a->position < b->position; });
    REQUIRE(pieces.size() == 3);
    CHECK(*pieces[0]->activeTake == pieces[0]->takes[other]);
    CHECK(*pieces[1]->activeTake == pieces[1]->takes[heard]);
    CHECK(*pieces[2]->activeTake == pieces[2]->takes[other]);
    CHECK(pieces[1]->position == doctest::Approx(pieces[0]->position + pieces[0]->length));
}

namespace {
// The generated item with two takes, given two more: four in all.
model::Id fourTakeItem(Fixture& f) {
    std::optional<model::Id> it;
    for (const auto& e : f.app->head().items)
        if (e.value->takes.size() == 2) it = e.id;
    REQUIRE(it);
    for (int k = 0; k < 2; ++k) {
        json::Value add = json::Value::object();
        add["item"] = it->str();
        add["from"] = f.item(*it).takes[0].str();
        REQUIRE(f.dispatcher->execute({"take.add", std::move(add), kUi}));
    }
    f.sync();
    REQUIRE(f.item(*it).takes.size() == 4);
    return *it;
}
void rightClick(Fixture& f, ui::Point p) {
    f.pointer(ui::PointerEvent::Type::Down, p, 3);
    f.pointer(ui::PointerEvent::Type::Up, p, 3);
    f.sync();
}
// The menu bar's Item menu over `item` (selected first unless it is), opened
// in place of the item menu so pick and menuIndex read it.
void barMenu(Fixture& f, model::Id item) {
    auto& g = f.view->gestures();
    if (!g.isSelected(item)) g.setSelection({item});
    f.root->closePopups();
    f.root->openPopup(ui::makeMenu("item.menu", g.itemBarMenu(), [&f] { f.root->closePopup(); }),
                      {0, 0, 1, 1});
    f.sync();
}
// Runs the open item menu's row `index` (by activation: a tall menu can be
// clamped to the window and scroll).
void pick(Fixture& f, int index) {
    auto* menu = dynamic_cast<ui::List*>(f.root->find("item.menu"));
    REQUIRE(menu);
    REQUIRE(static_cast<std::size_t>(index) < menu->count());
    auto activate = menu->onActivate;  // closes (destroys) the menu
    activate(static_cast<std::size_t>(index));
    f.sync();
}
std::size_t menuIndex(Fixture& f, std::string_view label) {
    const auto* menu = f.root->find("item.menu");
    REQUIRE(menu);
    const auto items = ui::menuItems(*menu);
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].label.starts_with(label)) return i;
    FAIL("missing menu entry: " << label);
    return 0;
}
void pick(Fixture& f, std::string_view label) { pick(f, static_cast<int>(menuIndex(f, label))); }
// A point at height `y` inside `it` that no later item on its track covers.
ui::Point onItem(Fixture& f, model::Id it, float y) {
    const ui::Rect r = f.rectOf(it);
    for (float x = r.x + 8; x < r.right() - 8; x += 4)
        if (const auto h = f.view->gestures().hit({x, y}); h && h->item == it) return {x, y};
    FAIL("no free point on " << it.str());
    return {};
}
ui::Point laneAt(Fixture& f, model::Id it, std::size_t lane) {
    const ui::Rect r = f.rectOf(it);
    const ui::Rect l = views::Lanes::takeLaneRect(r, f.item(it).takes.size(), lane);
    return onItem(f, it, l.y + l.h / 2);
}
}  // namespace

TEST_CASE("arrange item menu splits at the edit cursor and disables zero crossing for MIDI") {
    Fixture f;
    if (!f.ok) return;
    const auto audio = f.itemOnRow(0);
    const auto span = model::itemSpan(f.app->head(), f.item(audio));
    const double cursor = span.startSec + .25;
    f.view->setPlayhead(cursor);
    rightClick(f, onItem(f, audio, f.rectOf(audio).y + f.rectOf(audio).h / 2));
    auto* menu = f.root->find("item.menu");
    REQUIRE(menu);
    auto rows = ui::menuItems(*menu);
    REQUIRE(rows.size() > 7);
    CHECK(rows[menuIndex(f, "Split item at cursor")].label == "Split item at cursor");
    CHECK(rows[menuIndex(f, "Split item at prior zero crossing")].label ==
          "Split item at prior zero crossing");
    CHECK(rows[menuIndex(f, "Split item at prior zero crossing")].enabled);
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        REQUIRE(
            gui::writePng(std::filesystem::path(out) / "item-split-menu.png", f.root->readBack()));
    // Export… is in the menu bar's Item menu.
    barMenu(f, audio);
    CHECK(ui::menuItems(*f.root->find("item.menu"))[menuIndex(f, "Export")].enabled);
    std::optional<model::Id> exported;
    f.view->gestures().onExport = [&](model::Id item) { exported = item; };
    pick(f, "Export");
    CHECK(exported == audio);
    rightClick(f, onItem(f, audio, f.rectOf(audio).y + f.rectOf(audio).h / 2));
    const auto count = f.app->head().items.size(), steps = f.steps();
    pick(f, "Split item at cursor");
    CHECK(f.app->head().items.size() == count + 1);
    CHECK(model::itemSpan(f.app->head(), f.item(audio)).endSec == doctest::Approx(cursor));
    CHECK(f.steps() == steps + 1);
    const auto midi = f.itemOnRow(1, true);
    rightClick(f, onItem(f, midi, f.rectOf(midi).y + f.rectOf(midi).h / 2));
    menu = f.root->find("item.menu");
    REQUIRE(menu);
    CHECK_FALSE(ui::menuItems(*menu)[menuIndex(f, "Split item at prior zero crossing")].enabled);
    const auto before = model::serializeProject(f.app->head());
    pick(f, "Split item at prior zero crossing");
    CHECK(f.root->find("item.menu"));  // a disabled entry does not activate
    CHECK(model::serializeProject(f.app->head()) == before);
    barMenu(f, midi);
    CHECK(
        ui::menuItems(*f.root->find("item.menu"))[menuIndex(f, "Export")].enabled);  // a MIDI file
}

TEST_CASE("the Item menu inserts a stretch marker at the cursor and skips MIDI") {
    Fixture f;
    if (!f.ok) return;
    const auto audio = f.itemOnRow(0), midi = f.itemOnRow(1, true);
    const auto take = *f.item(audio).activeTake;
    const auto span = model::itemSpan(f.app->head(), f.item(audio));
    f.view->setPlayhead(span.startSec + .25);
    auto& gestures = f.view->gestures();
    gestures.setSelection({audio, midi});
    barMenu(f, audio);
    const auto* menu = f.root->find("item.menu");
    REQUIRE(menu);
    const auto rows = ui::menuItems(*menu);
    REQUIRE(rows.size() > 14);
    CHECK(rows[menuIndex(f, "Add stretch marker at cursor")].label ==
          "Add stretch marker at cursor");
    CHECK(rows[menuIndex(f, "Add stretch marker at cursor")].enabled);
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        REQUIRE(gui::writePng(std::filesystem::path(out) / "stretch-marker-menu.png",
                              f.root->readBack()));
    const auto steps = f.steps();
    pick(f, "Add stretch marker at cursor");
    const auto& markers = f.app->head().takes.find(take)->stretchMarkers;
    REQUIRE(markers.size() == 3);
    CHECK(markers[1].itemPos == .25);
    CHECK(f.steps() == steps + 1);
    CHECK(f.app->head().takes.find(*f.item(midi).activeTake)->stretchMarkers.empty());
    gestures.setSelection({midi});
    barMenu(f, midi);
    menu = f.root->find("item.menu");
    REQUIRE(menu);
    CHECK_FALSE(ui::menuItems(*menu)[menuIndex(f, "Add stretch marker at cursor")].enabled);
    f.root->closePopups();
    gestures.setSelection({audio});
    for (const auto& [command, field, value] : {std::tuple{"item.set", "loop", true},
                                                {"item.set", "locked", true},
                                                {"take.set", "preserve_pitch", false}}) {
        auto args = json::Value::object();
        args[std::string(command) == "take.set" ? "take" : "item"] =
            std::string(command) == "take.set" ? take.str() : audio.str();
        args[field] = value;
        REQUIRE(f.dispatcher->execute({command, args, kUi}));
        f.sync();
        barMenu(f, audio);
        CHECK_FALSE(
            ui::menuItems(*f.root->find("item.menu"))[menuIndex(f, "Add stretch marker at cursor")]
                .enabled);
        f.root->closePopups();
        REQUIRE(f.app->undo());
        f.sync();
    }
    f.view->setPlayhead(span.endSec + 1);
    barMenu(f, audio);
    CHECK_FALSE(
        ui::menuItems(*f.root->find("item.menu"))[menuIndex(f, "Add stretch marker at cursor")]
            .enabled);
}

TEST_CASE("arrange menu consolidates audio and heals compatible splits with shared enablement") {
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    Fixture f([&] {
        std::lock_guard lock(mutex);
        done = true;
        finished.notify_all();
    });
    if (!f.ok) return;
    const auto audio = f.itemOnRow(0), midi = f.itemOnRow(1, true);
    auto args = json::Value::object();
    args["item"] = audio.str();
    const auto span = model::itemSpan(f.app->head(), f.item(audio));
    args["at"] = f.app->head().timeMap->beatAt((span.startSec + span.endSec) / 2);
    auto split = f.dispatcher->execute({"item.split", args, kUi});
    REQUIRE(split);
    const auto right = *model::Id::parse(split->find("right")->asString());
    f.sync();
    f.view->gestures().setSelection({audio, right});
    rightClick(f, onItem(f, audio, f.rectOf(audio).y + f.rectOf(audio).h / 2));
    auto* menu = f.root->find("item.menu");
    REQUIRE(menu);
    auto rows = ui::menuItems(*menu);
    const auto consolidate = menuIndex(f, "Consolidate items");
    const auto heal = menuIndex(f, "Heal splits");
    CHECK(heal == consolidate + 1);
    CHECK(rows[consolidate].enabled);
    CHECK(rows[heal].enabled);
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        REQUIRE(gui::writePng(std::filesystem::path(out) / "consolidate-heal-menu.png",
                              f.root->readBack()));
    const auto steps = f.steps();
    pick(f, "Heal splits");
    CHECK(f.steps() == steps + 1);
    CHECK_FALSE(f.app->head().items.contains(right));
    CHECK(f.view->gestures().selection() == std::vector<model::Id>{audio});
    gui::AudioEdit edit(*f.app, *f.dispatcher);
    std::string status;
    edit.onStatus = [&](const auto& text) { status = text; };
    f.view->gestures().onConsolidate = [&](const auto& selected) { edit.consolidate(selected); };
    rightClick(f, onItem(f, audio, f.rectOf(audio).y + f.rectOf(audio).h / 2));
    menu = f.root->find("item.menu");
    REQUIRE(menu);
    CHECK_FALSE(ui::menuItems(*menu)[menuIndex(f, "Heal splits")].enabled);
    pick(f, "Consolidate items");
    {
        std::unique_lock lock(mutex);
        finished.wait(lock, [&] { return done; });
    }
    f.dispatcher->flushJobEvents();
    edit.poll();
    f.sync();
    CHECK(status == "Items consolidated");
    CHECK(f.steps() == steps + 2);
    args = json::Value::object();
    args["item"] = midi.str();
    args["from"] = f.item(midi).takes.front().str();
    REQUIRE(f.dispatcher->execute({"take.add", args, kUi}));
    f.sync();
    f.view->gestures().setSelection({midi});
    rightClick(f, onItem(f, midi, f.rectOf(midi).y + f.rectOf(midi).h / 2));
    menu = f.root->find("item.menu");
    REQUIRE(menu);
    rows = ui::menuItems(*menu);
    CHECK(rows[menuIndex(f, "Consolidate items")].enabled);  // layers its takes
    CHECK_FALSE(rows[menuIndex(f, "Heal splits")].enabled);
}

TEST_CASE("takes: show all takes spreads them out; a lane is heard, deleted from the Item menu") {
    Fixture f;
    if (!f.ok) return;
    const model::Id it = fourTakeItem(f);
    const double bar = static_cast<double>(views::TrackLayout::kTakeBar),
                 base = static_cast<double>(views::TrackLayout::kDefaultHeight);
    const ui::Rect compact = f.rectOf(it);
    CHECK_FALSE(views::Lanes::showsTakeLanes(compact, 4));  // too many for the row
    barMenu(f, it);
    const auto steps = f.steps();
    pick(f, "Show all takes");
    CHECK(f.view->trackLayout().takesExpanded(it));
    CHECK(f.steps() == steps);  // a view, not an edit
    const auto row = *f.view->trackLayout().indexOf(f.item(it).track);
    views::Viewport vp = f.view->viewport();  // the tall row in sight
    vp.scrollY = f.view->trackLayout().yOf(row);
    f.view->setViewport(vp);
    f.sync();
    const ui::Rect r = f.rectOf(it);
    REQUIRE(views::Lanes::showsTakeLanes(r, 4));
    REQUIRE(f.view->lanesRect().contains(laneAt(f, it, 3)));
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        (void)gui::writePng(std::filesystem::path(out) / "takes-expanded.png", f.root->readBack());
    CHECK(f.view->trackLayout().heightOf(row) == doctest::Approx(bar + 4 * base));
    // A click on the third lane hears the third take.
    f.click(laneAt(f, it, 2));
    CHECK(*f.item(it).activeTake == f.item(it).takes[2]);
    // Heard, the second take is deleted from the Item menu.
    const model::Id second = f.item(it).takes[1];
    f.click(laneAt(f, it, 1));
    barMenu(f, it);
    pick(f, "Delete take ");
    REQUIRE(f.item(it).takes.size() == 3);
    CHECK(std::find(f.item(it).takes.begin(), f.item(it).takes.end(), second) ==
          f.item(it).takes.end());
    CHECK(f.view->trackLayout().heightOf(row) == doctest::Approx(bar + 3 * base));
    // Shown again: the menu entry is checked and hides them.
    barMenu(f, it);
    pick(f, "Show all takes");
    CHECK_FALSE(f.view->trackLayout().takesExpanded(it));
    CHECK(f.view->trackLayout().heightOf(row) == doctest::Approx(base));
    // Keep only the selected take: the heard one stays, in one undo step.
    const model::Id heard = *f.item(it).activeTake;
    const auto before = f.steps();
    barMenu(f, it);
    pick(f, "Keep only the selected take");
    CHECK(f.item(it).takes == std::vector<model::Id>{heard});
    CHECK(f.steps() == before + 1);
}

TEST_CASE("takes: Ctrl-drag takes one out to another track; explode spreads the rest") {
    Fixture f;
    if (!f.ok) return;
    const model::Id it = fourTakeItem(f);
    const model::Id heard = *f.item(it).activeTake;
    const model::Id home = f.item(it).track;
    const auto row = *f.view->trackLayout().indexOf(home);
    std::optional<std::size_t> other;
    for (std::size_t k = 0; k < f.view->trackLayout().rowCount(); ++k)
        if (k != row && f.view->trackLayout().rows()[k].type == model::StripType::Channel &&
            !f.view->trackLayout().rows()[k].env)
            other = k;
    REQUIRE(other);
    const ui::Rect r = f.rectOf(it);
    const ui::Rect target = f.view->rowRect(*other);
    const std::size_t items = f.app->head().items.size();
    const auto steps = f.steps();
    // Compact (no lanes): the heard take comes out.
    const ui::Point from = onItem(f, it, r.y + r.h - 8);
    f.drag(from, {from.x + 2 * f.beatPx(), target.y + 10}, ui::kCtrl);
    CHECK(f.steps() == steps + 1);
    REQUIRE(f.app->head().items.size() == items + 1);
    CHECK(f.item(it).takes.size() == 3);
    const auto sel = f.view->gestures().selection();
    REQUIRE(sel.size() == 1);
    CHECK(f.item(sel[0]).takes == std::vector<model::Id>{heard});
    CHECK(f.item(sel[0]).track == f.view->trackLayout().rows()[*other].track);
    // Explode the remaining three along the track, in recording order.
    const auto takes = f.item(it).takes;
    barMenu(f, it);
    pick(f, "Explode takes in sequence");
    CHECK(f.item(it).takes == std::vector<model::Id>{takes[0]});
    std::vector<const model::Item*> along;
    for (const auto& e : f.app->head().items)
        if (e.value->track == home && e.value->takes.size() == 1 &&
            std::find(takes.begin(), takes.end(), e.value->takes[0]) != takes.end())
            along.push_back(e.value.get());
    REQUIRE(along.size() == 3);
    std::sort(along.begin(), along.end(),
              [](auto* a, auto* b) { return a->position < b->position; });
    for (std::size_t k = 0; k < 3; ++k) CHECK(along[k]->takes[0] == takes[k]);
}

TEST_CASE("takes: explode onto new tracks; render an item with FX as a new take") {
    Fixture f;
    if (!f.ok) return;
    const model::Id it = fourTakeItem(f);
    const std::size_t tracks = f.app->head().tracks.size();
    barMenu(f, it);
    pick(f, "Explode takes onto new tracks");
    CHECK(f.app->head().tracks.size() == tracks + 3);
    CHECK(f.item(it).takes.size() == 1);
    // An audio item with one take: the renders are the last two rows.
    const model::Id audio = f.itemOnRow(0);
    REQUIRE(f.item(audio).takes.size() == 1);
    const ui::Rect ra = f.rectOf(audio);
    rightClick(f, onItem(f, audio, ra.y + ra.h / 2));
    pick(f, "Render with FX as a new take");
    REQUIRE(f.item(audio).takes.size() == 2);
    const model::Take& made = *f.app->head().takes.find(*f.item(audio).activeTake);
    CHECK(made.name.ends_with("(FX)"));
}

TEST_CASE("the renders act on every selected item; deleting FX empties their tracks' chains") {
    Fixture f;
    if (!f.ok) return;
    const model::Id audio = f.itemOnRow(0), midi = f.itemOnRow(1, true);
    const model::Id track = f.item(audio).track;
    auto listed = f.dispatcher->execute({"plugin.list", json::Value::object(), kUi});
    REQUIRE(listed);
    for (const json::Value& p : (*listed)["plugins"].asArray())  // an effect, where there is one
        if (!(p.find("instrument") && p.find("instrument")->asBool()) &&
            f.dispatcher->execute({"fx.add",
                                   json::parse(R"({"track":")" + track.str() + R"(","plugin":")" +
                                               p.find("id")->asString() + R"("})")
                                       .value(),
                                   kUi}))
            break;
    f.sync();
    f.view->gestures().setSelection({audio, midi});
    const auto steps = f.steps();
    const ui::Rect r = f.rectOf(audio);
    rightClick(f, onItem(f, audio, r.y + r.h / 2));
    pick(f, "Render with FX, deleting FX");
    CHECK(f.steps() == steps + 1);
    for (model::Id id : {audio, midi}) {
        const model::Take& tk = *f.app->head().takes.find(*f.item(id).activeTake);
        CHECK(tk.name.ends_with("(FX)"));
        CHECK(tk.source->kind != model::Kind::Midi);
    }
    CHECK(f.app->head().tracks.find(track)->chain.empty());
    CHECK(f.app->head().tracks.find(f.item(midi).track)->chain.empty());
}

TEST_CASE("arrange gestures: Ctrl+click on a fade handle cycles the fade's shape") {
    Fixture f;
    if (!f.ok) return;
    const model::Id a = f.itemOnRow(0);
    const ui::Rect r = f.rectOf(a);
    REQUIRE(f.item(a).fadeIn.shape == model::FadeShape::Linear);
    auto steps = f.steps();
    f.click({r.x + 3, r.y + 3}, ui::kCtrl);
    CHECK(f.item(a).fadeIn.shape == model::FadeShape::Slow);
    CHECK(f.steps() == steps + 1);
    f.click({r.x + 3, r.y + 3}, ui::kCtrl);
    CHECK(f.item(a).fadeIn.shape == model::FadeShape::Fast);
    CHECK(f.item(a).fadeOut.shape == model::FadeShape::Linear);  // the other end untouched
}

TEST_CASE(
    "arrange gestures: a right-click on an item opens its menu; Delete removes the selection") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id midi = f.itemOnRow(0, true), audio = f.itemOnRow(0);
    const ui::Rect rm = f.rectOf(midi);
    const ui::Point on{rm.x + rm.w / 2, rm.y + rm.h / 2};
    auto rightClick = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Down, p, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 3);
        f.sync();
    };
    rightClick(on);
    CHECK(g.selection() == std::vector<model::Id>{midi});  // the item under the pointer
    ui::Node* menu = f.root->find("item.menu");
    REQUIRE(menu);
    const auto before = f.steps();
    f.menuRow("item.menu", 4);  // Delete
    f.sync();
    CHECK_FALSE(f.app->head().items.find(midi));
    CHECK(f.app->head().items.find(audio));
    CHECK(f.steps() == before + 1);
    CHECK(g.selection().empty());
    // With several selected, Delete takes them all in one step.
    const model::Id a = f.itemOnRow(0), b = f.itemOnRow(1);
    g.setSelection({a, b});
    const ui::Rect ra = f.rectOf(a);
    rightClick({ra.x + ra.w / 2, ra.y + ra.h / 2});
    REQUIRE(f.root->find("item.menu"));
    f.menuRow("item.menu", 4);
    f.sync();
    CHECK_FALSE(f.app->head().items.find(a));
    CHECK_FALSE(f.app->head().items.find(b));
    CHECK(f.steps() == before + 2);
    // A right-drag is still a marquee, not a menu.
    const ui::Rect lanes = f.view->lanesRect();
    f.drag({lanes.x + 4, f.view->rowRect(0).y + 4},
           {lanes.x + 200, f.view->rowRect(1).bottom() - 4}, 0, 3);
    CHECK_FALSE(f.root->find("item.menu"));
}

TEST_CASE("arrange gestures: the menus paste where they open; the menu bar's has the rest") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    auto rightClick = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Down, p, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 3);
        f.sync();
    };
    // Two MIDI items four bars apart on a new channel.
    auto exec = [&](const char* command, const char* args) {
        auto r = f.dispatcher->execute({command, json::parse(args).value(), kUi});
        REQUIRE(r);
        return *r;
    };
    const std::string track = exec("track.add", R"({"name":"Keys"})")["track"].asString();
    const std::string add =
        R"({"track":")" + track + R"(","bar":1,"notation":"voice k; 1: C4:q r:*"})";
    const model::Id one = *model::Id::parse(exec("item.add", add.c_str())["item"].asString());
    const std::string later =
        R"({"track":")" + track + R"(","bar":5,"notation":"voice k; 1: E4:q r:*"})";
    const model::Id two = *model::Id::parse(exec("item.add", later.c_str())["item"].asString());
    f.sync();
    // Copy the first, then Paste from the menu on empty space: at that beat, that track.
    const ui::Rect r1 = f.rectOf(one);
    rightClick({r1.x + 4, r1.y + r1.h / 2});
    REQUIRE(f.root->find("item.menu"));
    f.menuRow("item.menu", 1);  // Copy
    const ui::Rect r2 = f.rectOf(two);
    const ui::Point gap{r1.right() + (r2.x - r1.right()) / 2, r1.y + r1.h / 2};
    rightClick(gap);
    REQUIRE(f.root->find("arrange.space.menu"));
    f.menuRow("arrange.space.menu", 0);  // Paste, its one row
    REQUIRE(g.selection().size() == 1);
    const model::Id pasted = g.selection().front();
    CHECK(pasted != one);
    CHECK(f.item(pasted).track.str() == track);
    CHECK(f.item(pasted).position > f.item(one).position);
    // Several MIDI items combine with Consolidate items. The menu bar's Item
    // menu has the other entries, none of these.
    g.setSelection({one, two});
    f.sync();
    rightClick({r1.x + 4, r1.y + r1.h / 2});
    const auto* menu = f.root->find("item.menu");
    REQUIRE(menu);
    std::vector<std::string> clicked;
    for (const auto& row : ui::menuItems(*menu)) clicked.push_back(row.label);
    CHECK(ui::menuItems(*menu)[menuIndex(f, "Consolidate items")].enabled);
    f.root->closePopups();
    const auto bar = g.itemBarMenu();
    CHECK(bar.front().label == "Export\u2026");
    for (const auto& row : bar)
        if (!row.separator)
            CHECK_MESSAGE(std::find(clicked.begin(), clicked.end(), row.label) == clicked.end(),
                          row.label);
}

TEST_CASE("arrange gestures: the Item menu sets what items follow; a time item moves by beats") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id audio = f.itemOnRow(0);
    barMenu(f, audio);
    REQUIRE(f.root->find("item.menu"));
    const double startSec = model::itemSpan(f.app->head(), f.item(audio)).startSec;
    pick(f, "Follow time");
    CHECK(f.item(audio).timebase == model::ItemTimebase::Time);
    CHECK(f.item(audio).position == doctest::Approx(startSec));  // seconds now, same place
    CHECK(g.selection() == std::vector<model::Id>{audio});
    // A drag still moves it by whole beats (snapped), now written in seconds.
    const ui::Rect moved = f.rectOf(audio);
    const float beat = f.beatPx();
    f.drag({moved.x + moved.w / 2, moved.y + moved.h / 2},
           {moved.x + moved.w / 2 + 2 * beat, moved.y + moved.h / 2});
    const model::Session& s = f.app->head();
    CHECK(model::itemSpan(s, f.item(audio)).startSec ==
          doctest::Approx(s.timeMap->secondsAt(s.timeMap->beatAt(startSec) + 2)).epsilon(1e-6));
    // The tempo halves: it stays at its seconds.
    const double before = model::itemSpan(s, f.item(audio)).startSec;
    REQUIRE(f.dispatcher->execute({"tempo.set",
                                   json::parse(R"({"points":[{"beat":0,"bpm":60}]})").value(),
                                   {cmd::Origin::Type::Ui, "gui", {}}}));
    f.sync();
    CHECK(model::itemSpan(f.app->head(), f.item(audio)).startSec == doctest::Approx(before));
}

TEST_CASE("arrange gestures: take FX and a take envelope drawn and edited on its item") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    std::vector<model::Id> takeFx;
    g.onTakeFx = [&](model::Id take) { takeFx.push_back(take); };
    const model::Id audio = f.itemOnRow(0);
    const model::Id take = *f.item(audio).activeTake;
    auto rightClick = [&] {  // the take entries are in the menu bar's Item menu
        barMenu(f, audio);
        REQUIRE(f.root->find("item.menu"));
    };
    rightClick();
    pick(f, "Take FX");
    CHECK(takeFx == std::vector<model::Id>{take});
    rightClick();
    pick(f, "Take volume envelope");
    auto envelope = [&]() -> const model::Envelope* {
        for (const auto& e : f.app->head().envelopes)
            if (e.value->target.object == take && e.value->target.param == "volume")
                return e.value.get();
        return nullptr;
    };
    REQUIRE(envelope());
    CHECK(envelope()->visible);
    REQUIRE(envelope()->points.size() == 1);
    // A click on its line in the item's middle adds a point there.
    const ui::Rect r = f.rectOf(audio);
    const ui::Rect area = views::Lanes::takeEnvArea(r);
    const auto scale = views::envScale(*envelope());
    REQUIRE(scale);
    const float lineY = area.bottom() - static_cast<float>(scale->toNorm(0.0)) * area.h;
    const ui::Point mid{r.x + r.w / 2, lineY};
    const auto before = f.steps();
    f.click(mid, ui::kShift);  // Shift: off the grid
    REQUIRE(envelope()->points.size() == 2);
    CHECK(f.steps() == before + 1);
    CHECK(f.item(audio).position == doctest::Approx(f.item(audio).position));  // the item stays
    const double pos = envelope()->points[1].pos;
    const double startBeat = model::itemSpan(f.app->head(), f.item(audio)).startBeat;
    CHECK(pos ==
          doctest::Approx(f.view->session().timeMap->beatAt(f.view->timeAt(mid.x)) - startBeat)
              .epsilon(1e-3));
    // Dragging it down lowers it; Alt+click deletes it.
    f.drag(mid, {mid.x, mid.y + 12}, ui::kShift);
    REQUIRE(envelope()->points.size() == 2);
    CHECK(envelope()->points[1].value < -1.0);
    const float y =
        area.bottom() - static_cast<float>(scale->toNorm(envelope()->points[1].value)) * area.h;
    f.click({mid.x, y}, ui::kAlt);
    CHECK(envelope()->points.size() == 1);
    // Hidden from the menu, its line no longer takes the press: the item moves instead.
    rightClick();
    pick(f, "Take volume envelope");
    CHECK_FALSE(envelope()->visible);
    CHECK_FALSE(g.takeEnvelopeAt(audio, {mid.x, lineY}));
}

TEST_CASE("left trim moves every take offset at its own play rate in one undo step") {
    Fixture f;
    REQUIRE(f.ok);
    std::optional<model::Id> id;
    for (const auto& entry : f.app->head().items)
        if (entry.value->takes.size() == 2) id = entry.id;
    REQUIRE(id);
    const auto takes = f.item(*id).takes;
    REQUIRE(f.dispatcher->execute(
        {"take.set", json::parse(R"({"take":")" + takes[1].str() + R"(","playrate":2})").value(),
         kUi}));
    f.sync();
    const auto before = f.app->head();
    const auto span = model::itemSpan(before, f.item(*id));
    const auto steps = f.steps();
    const auto rect = f.rectOf(*id);
    f.drag({rect.x + 2, rect.y + rect.h / 2}, {rect.x + 10, rect.y + rect.h / 2});
    REQUIRE(f.steps() == steps + 1);
    const double delta = model::itemSpan(f.app->head(), f.item(*id)).startSec - span.startSec;
    REQUIRE(delta > 0);
    for (auto take : takes)
        CHECK(f.app->head().takes.find(take)->startOffset ==
              doctest::Approx(before.takes.find(take)->startOffset +
                              delta * before.takes.find(take)->playrate));
    REQUIRE(f.dispatcher->execute({"history.undo", json::Value::object(), kUi}));
    for (auto take : takes)
        CHECK(f.app->head().takes.find(take)->startOffset == before.takes.find(take)->startOffset);
}

TEST_CASE("the left edge extends an item before its material; slip goes both ways") {
    Fixture f;
    REQUIRE(f.ok);
    for (bool midi : {false, true}) {
        CAPTURE(midi);
        model::Id id = f.itemOnRow(0, midi);
        // Somewhere with room before it: two bars in, the offset at zero.
        REQUIRE(f.dispatcher->execute(
            {"item.move", json::parse(R"({"item":")" + id.str() + R"(","position":8})").value(),
             kUi}));
        REQUIRE(f.dispatcher->execute(
            {"take.set",
             json::parse(R"({"take":")" + f.item(id).activeTake->str() + R"(","start_offset":0})")
                 .value(),
             kUi}));
        f.sync();
        const auto before = f.app->head();
        const double start = model::itemSpan(before, f.item(id)).startBeat;
        const double end = model::itemEndBeat(before, f.item(id));
        auto rect = f.rectOf(id);
        // The left edge two beats earlier: longer, starting earlier, the
        // material where it was (silence first).
        const auto steps = f.steps();
        f.drag({rect.x + 2, rect.y + rect.h / 2},
               {rect.x + 2 - 2 * f.beatPx(), rect.y + rect.h / 2});
        REQUIRE(f.steps() == steps + 1);
        const model::Session& s = f.app->head();
        CHECK(model::itemSpan(s, f.item(id)).startBeat == doctest::Approx(start - 2));
        CHECK(model::itemEndBeat(s, f.item(id)) == doctest::Approx(end));
        const double offset = s.takes.find(*f.item(id).activeTake)->startOffset;
        CHECK(offset < 0);
        CHECK(offset ==
              doctest::Approx(
                  midi ? -2.0 : -(s.timeMap->secondsAt(start) - s.timeMap->secondsAt(start - 2))));
        // Slip right: the material moves later under the item (silence first).
        rect = f.rectOf(id);
        const ui::Point mid{rect.x + rect.w / 2, rect.y + rect.h / 2};
        f.drag(mid, {mid.x + f.beatPx(), mid.y}, ui::kAlt);
        REQUIRE(f.steps() == steps + 2);
        CHECK(f.app->head().takes.find(*f.item(id).activeTake)->startOffset < offset);
    }
}

TEST_CASE("clipboard channel offsets skip envelope rows and report missing targets") {
    Fixture f;
    REQUIRE(f.ok);
    auto& gestures = f.view->gestures();
    const auto a = f.itemOnRow(0), b = f.itemOnRow(1);
    const auto target = f.view->trackLayout().rows()[2].track;
    const auto next = f.view->trackLayout().rows()[3].track;
    const auto last = f.view->trackLayout().rows()[5].track;
    gestures.setSelection({a, b});
    gestures.copySelection();
    REQUIRE(f.dispatcher->execute(
        {"env.add",
         json::parse(R"({"track":")" + target.str() + R"(","param":"volume","points":[]})").value(),
         kUi}));
    f.sync();
    REQUIRE(gestures.paste(200, target));
    f.sync();
    REQUIRE(gestures.selection().size() == 2);
    CHECK(f.item(gestures.selection()[0]).track == target);
    CHECK(f.item(gestures.selection()[1]).track == next);
    std::string diagnostic;
    gestures.onError = [&](const Error& error) { diagnostic = error.message; };
    REQUIRE(gestures.paste(300, last));
    f.sync();
    REQUIRE(gestures.selection().size() == 1);
    CHECK(f.item(gestures.selection().front()).track == last);
    CHECK(diagnostic.find("dropped 1") != std::string::npos);
}

TEST_CASE("clipboard keeps a discarded redo version available until replaced") {
    Fixture f;
    if (!f.ok) return;
    const auto original = f.itemOnRow(0);
    auto& g = f.view->gestures();
    g.setSelection({original});
    REQUIRE(g.duplicateSelection());
    f.sync();
    const auto copiedVersion = f.app->head().version;
    std::weak_ptr<const model::Session> weak = f.app->versions().pin(copiedVersion);
    g.copySelection();
    REQUIRE(f.dispatcher->execute({"history.undo", json::Value::object(), kUi}));
    REQUIRE(f.dispatcher->execute(
        {"track.set", json::parse(R"({"track":"tr_1","name":"new edit"})").value(), kUi}));
    f.sync();
    REQUIRE(f.app->versions().version(copiedVersion));
    REQUIRE(g.paste(200));
    f.sync();
    CHECK(f.item(g.selection()[0]).position == 200);
    g.copySelection();
    f.app->versions().collectUnused();
    CHECK_FALSE(f.app->versions().version(copiedVersion));
    CHECK(weak.expired());
}

TEST_CASE("an envelope drag over points another client changed is not applied") {
    Fixture f;
    REQUIRE(f.ok);
    const auto track = f.view->trackLayout().rows()[0].track;
    auto r = f.dispatcher->execute(
        {"env.add",
         json::parse(
             R"({"track":")" + track.str() +
             R"(","param":"volume","points":[{"beat":0,"value":0},{"beat":16,"value":-12}]})")
             .value(),
         kUi});
    REQUIRE(r);
    const auto env = *model::Id::parse((*r)["env"].asString());
    f.sync();
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    const auto area = views::Lanes::envArea(f.view->rowRect(*lane));
    const ui::Point p{f.view->xAt(f.app->head().timeMap->secondsAt(8)), area.y + area.h * .25f};
    f.pointer(ui::PointerEvent::Type::Down, p);
    REQUIRE(f.view->gestures().state().envCreated);
    REQUIRE(f.dispatcher->execute(
        {"env.points.set",
         json::parse(
             R"({"env":")" + env.str() +
             R"(","points":[{"beat":0,"value":0},{"beat":4,"value":-30},{"beat":16,"value":-12}]})")
             .value(),
         kUi}));
    f.sync();
    f.pointer(ui::PointerEvent::Type::Up, p);
    const auto& points = f.app->head().envelopes.find(env)->points;
    REQUIRE(points.size() == 3);
    CHECK(points[1].pos == doctest::Approx(4));  // the other client's point stays
    CHECK(points[1].value == doctest::Approx(-30));
}

TEST_CASE("automation points: select, move together, marquee, menu, clipboard, shapes") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.view->trackLayout().rows()[0].track;
    auto added = f.dispatcher->execute(
        {"env.add",
         json::parse(R"({"track":")" + track.str() +
                     R"(","param":"volume","points":[{"beat":0,"value":-12},{"beat":4,"value":-6},)"
                     R"({"beat":8,"value":-12},{"beat":12,"value":-6}]})")
             .value(),
         kUi});
    REQUIRE(added);
    const model::Id env = *model::Id::parse((*added)["env"].asString());
    f.sync();
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    const ui::Rect area = views::Lanes::envArea(f.view->rowRect(*lane));
    auto& g = f.view->gestures();
    auto points = [&] { return f.app->head().envelopes.find(env)->points; };
    auto at = [&](std::size_t i) {
        const model::EnvPoint p = points()[i];
        return ui::Point{
            f.view->xAt(f.app->head().timeMap->secondsAt(p.pos)),
            area.bottom() - static_cast<float>(views::EnvScale{}.toNorm(p.value)) * area.h};
    };
    auto rightClick = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Down, p, 3);
        f.pointer(ui::PointerEvent::Type::Up, p, 3);
        f.sync();
        REQUIRE(f.root->find("env.menu"));
    };
    // A click selects a point (no edit); Ctrl+click adds another.
    auto steps = f.steps();
    f.click(at(1));
    CHECK(f.steps() == steps);
    CHECK(g.pointEnvelope() == env);
    CHECK(g.selectedPoints().size() == 1);
    f.click(at(3), ui::kCtrl);
    REQUIRE(g.selectedPoints().size() == 2);
    // Dragging one of them moves both, in one step; the others stay.
    f.drag(at(1), {at(1).x, at(1).y - area.h * 0.1f});
    CHECK(f.steps() == steps + 1);
    REQUIRE(points().size() == 4);
    CHECK(points()[1].value > -6);
    CHECK(points()[3].value == doctest::Approx(points()[1].value));
    CHECK(points()[0].value == doctest::Approx(-12));
    CHECK(points()[2].value == doctest::Approx(-12));
    CHECK(g.selectedPoints().size() == 2);  // still selected, where they moved
    // A right-drag selects with a marquee (the first three points).
    f.drag({at(2).x + 6, area.y + 1}, {at(0).x - 6, area.bottom() - 1}, 0, 3);
    CHECK_FALSE(f.root->find("env.menu"));
    CHECK(g.selectedPoints().size() == 3);
    // Rows: Insert point, rule, Select all, Copy, Cut, Paste, Delete, Point shape.
    rightClick({at(3).x + 40, area.y + area.h / 2});
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        (void)gui::writePng(std::filesystem::path(out) / "automation-points-menu.png",
                            f.root->readBack());
    f.menuRow("env.menu", 6);
    CHECK(f.steps() == steps + 2);
    REQUIRE(points().size() == 1);
    CHECK(g.selectedPoints().empty());
    REQUIRE(f.dispatcher->execute({"history.undo", json::Value::object(), kUi}));
    f.sync();
    REQUIRE(points().size() == 4);
    // Select all, copy, paste at beat 16: four more points from there.
    rightClick({at(3).x + 40, area.y + area.h / 2});
    f.menuRow("env.menu", 2);
    CHECK(g.selectedPoints().size() == 4);
    g.copyPoints();
    REQUIRE(g.canPastePoints());
    REQUIRE(g.pastePoints(16));
    f.sync();
    REQUIRE(points().size() == 8);
    CHECK(points()[4].pos == doctest::Approx(16));
    CHECK(points()[7].pos == doctest::Approx(28));
    CHECK(points()[4].value == doctest::Approx(points()[0].value));
    CHECK(g.selectedPoints().size() == 4);  // the pasted ones
    // Point shape: the second menu sets the selected points' shape.
    rightClick({at(7).x + 40, area.y + area.h / 2});
    f.menuRow("env.menu", 7);
    REQUIRE(f.root->find("env.shape.menu"));
    f.menuRow("env.shape.menu", 1);  // Square
    for (std::size_t i = 4; i < 8; ++i) CHECK(points()[i].shape == model::PointShape::Square);
    CHECK(points()[0].shape == model::PointShape::Linear);
    // Pasting into a pan lane converts the values through the lanes.
    auto pan = f.dispatcher->execute(
        {"env.add", json::parse(R"({"track":")" + track.str() + R"(","param":"pan"})").value(),
         kUi});
    REQUIRE(pan);
    const model::Id panEnv = *model::Id::parse((*pan)["env"].asString());
    f.sync();
    REQUIRE(g.pastePoints(0, panEnv));
    f.sync();
    const auto& panPoints = f.app->head().envelopes.find(panEnv)->points;
    REQUIRE(panPoints.size() >= 4);
    for (const auto& p : panPoints) CHECK(std::abs(p.value) <= 100.0);
    // A click elsewhere in the lanes leaves the points.
    f.click({f.view->lanesRect().x + 5, f.view->lanesRect().bottom() - 5});
    CHECK_FALSE(g.pointEnvelope());
}

TEST_CASE("automation item point edits require an invertible amplitude") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* name, const std::string& text) {
        auto result = f.dispatcher->execute({name, json::parse(text).value(), kUi});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    const auto env = exec("env.add", R"({"track":"tr_1","param":"pan"})")["env"].asString();
    const auto id =
        exec(
            "autoitem.add",
            "{\"env\":\"" + env +
                R"(","position":4,"length":8,"points":[{"beat":2,"value":-50},{"beat":6,"value":50}]})")
            ["id"]
                .asString();
    std::string error;
    f.view->gestures().onError = [&](const Error& e) { error = e.toString(); };
    for (const char* amplitude :
         {"0", "0.0000000001", "0.000000001", "-0.0000000001", "-0.000000001"}) {
        exec("autoitem.set", "{\"item\":\"" + id + "\",\"amplitude\":" + amplitude + "}");
        f.sync();
        const auto lane = f.view->trackLayout().indexOfEnv(*model::Id::parse(env));
        REQUIRE(lane);
        const auto area = views::Lanes::envArea(f.view->rowRect(*lane));
        const auto version = f.app->head().version;
        const auto point = [&](double beat) {
            return ui::Point{f.view->xAt(f.app->head().timeMap->secondsAt(beat)),
                             area.y + area.h * 0.5f};
        };
        f.click(point(8));  // insertion in the body
        CHECK(error.find("restore the amplitude") != std::string::npos);
        error.clear();
        f.click(point(6), ui::kAlt);  // deletion of an existing played point
        CHECK(error.find("restore the amplitude") != std::string::npos);
        CHECK(f.app->head().version == version);
        const auto* item = f.app->head().autoItems.find(*model::Id::parse(id));
        REQUIRE(item->points.size() == 2);
        CHECK(item->points.front().value == -50);
        CHECK(item->points.back().value == 50);
    }
}

TEST_CASE("automation item drags reject concurrent transform and geometry edits") {
    Fixture f;
    if (!f.ok) return;
    auto exec = [&](const char* name, const std::string& text) {
        auto result = f.dispatcher->execute({name, json::parse(text).value(), kUi});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    const auto env = exec("env.add", R"({"track":"tr_1","param":"pan"})")["env"].asString();
    const auto id =
        exec(
            "autoitem.add",
            "{\"env\":\"" + env +
                R"(","position":4,"length":8,"points":[{"beat":0,"value":-50},{"beat":8,"value":50}]})")
            ["id"]
                .asString();
    std::string error;
    f.view->gestures().onError = [&](const Error& e) { error = e.code; };
    using Drag = views::ItemGestures::Drag;
    for (const auto drag : {Drag::EnvPoint, Drag::AutoItemMove, Drag::AutoItemLength}) {
        for (const char* edit : {"\"amplitude\":0.5", "\"baseline\":10", "\"position\":5",
                                 "\"length\":10", "\"source_length\":4", "\"loop\":false"}) {
            CAPTURE(static_cast<int>(drag));
            CAPTURE(std::string(edit));
            exec(
                "autoitem.set",
                "{\"item\":\"" + id +
                    R"(","position":4,"length":8,"source_length":8,"loop":true,"amplitude":1,"baseline":0,"points":[{"beat":0,"value":-50},{"beat":8,"value":50}]})");
            f.sync();
            const auto lane = f.view->trackLayout().indexOfEnv(*model::Id::parse(env));
            REQUIRE(lane);
            const auto rect = f.view->rowRect(*lane);
            const auto area = views::Lanes::envArea(rect);
            const ui::Point press{f.view->xAt(f.app->head().timeMap->secondsAt(
                                      drag == Drag::AutoItemLength ? 12 : 8)),
                                  drag == Drag::AutoItemMove ? rect.y + 3 : area.y + area.h * 0.6f};
            f.pointer(ui::PointerEvent::Type::Down, press);
            REQUIRE(f.view->gestures().state().autoItem == model::Id::parse(id));
            const ui::Point release{press.x + (drag != Drag::EnvPoint ? 16 : 0), press.y};
            if (drag != Drag::EnvPoint) f.pointer(ui::PointerEvent::Type::Move, release);
            exec("autoitem.set", "{\"item\":\"" + id + "\"," + edit + "}");
            f.sync();
            const auto version = f.app->head().version;
            error.clear();
            if (drag != Drag::EnvPoint) {
                f.pointer(ui::PointerEvent::Type::Move, release);
                CHECK(f.view->gestures().state().drag == Drag::None);
            }
            f.pointer(ui::PointerEvent::Type::Up, release);
            f.sync();
            CHECK(error == "env.changed");
            CHECK(f.app->head().version == version);
            const auto* item = f.app->head().autoItems.find(*model::Id::parse(id));
            CHECK(item->points.size() == 2);
            CHECK(item->points.front().value == -50);
            CHECK(item->points.back().value == 50);
        }
    }
}

TEST_CASE("audio warp handles drag once and render hits on the waveform") {
    Fixture f;
    if (!f.ok) return;
    const auto item = f.itemOnRow(0);
    const auto take = *f.item(item).activeTake;
    auto set = json::Value::object();
    set["item"] = item.str();
    set["loop"] = false;
    REQUIRE(f.dispatcher->execute({"item.set", set, kUi}));
    auto hits = json::Value::object();
    hits["take"] = take.str();
    hits["positions"] = *json::parse("[0.4,0.7,1.1]");
    REQUIRE(f.dispatcher->execute({"take.hits.set", hits, kUi}));
    auto anchor = json::Value::object();
    anchor["take"] = take.str();
    anchor["source_pos"] = .7;
    anchor["item_pos"] = .6;
    REQUIRE(f.dispatcher->execute({"take.stretch.add", anchor, kUi}));
    auto viewport = f.view->viewport();
    viewport.pixelsPerSecond = 150;
    f.view->setViewport(viewport);
    f.sync();
    const auto rect = f.rectOf(item);
    const double start = model::itemSpan(f.app->head(), f.item(item)).startSec;
    const auto steps = f.steps();
    f.drag({f.view->xAt(start + .6), rect.y + 25}, {f.view->xAt(start + .7), rect.y + 25},
           ui::kShift);
    CHECK(f.app->head().takes.find(take)->stretchMarkers[1].itemPos == doctest::Approx(.7));
    CHECK(f.steps() == steps + 1);
    if (const char* out = std::getenv("SUPPLE_SCREENSHOT_DIR")) {
        REQUIRE(gui::writePng(std::filesystem::path(out) / "audio-warp.png", f.root->readBack()));
        // Keep a portable one-item scene for a full-app render after this fixture
        // and its private media directory have been removed.
        model::EditTxn snapshot(model::Session::create(), 1);
        model::Track track;
        track.parent = model::kMaster;
        const auto trackId = snapshot.create(track);
        snapshot.modify<model::Track>(model::kMaster,
                                      [&](auto& master) { master.children.push_back(trackId); });
        auto tk = *f.app->head().takes.find(take);
        auto source = *f.app->head().media.find(*tk.source);
        const auto file = std::filesystem::path(out) / "warp-source.wav";
        std::filesystem::copy_file(f.app->mediaDir() / source.path, file,
                                   std::filesystem::copy_options::overwrite_existing);
        source.path = file.string();
        tk.source = snapshot.create(source);
        const auto takeId = snapshot.create(tk);
        auto it = f.item(item);
        it.track = trackId;
        it.takes = {takeId};
        it.activeTake = takeId;
        snapshot.create(it);
        auto project = snapshot.commit();
        REQUIRE(project);
        REQUIRE(writeFileAtomic(std::filesystem::path(out) / "warp-project.json",
                                model::serializeProject(*project)));
    }
}

TEST_CASE("audio slice menu workflow detects missing hits then commits its pure edit") {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    Fixture f([&] {
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        done.notify_one();
    });
    if (!f.ok) return;
    audio::AudioData audio;
    audio.sampleRate = 48000;
    audio.channels.assign(1, std::vector<float>(48000));
    for (int at : {4800, 19200, 33600})
        for (int i = 0; i < 2400; ++i)
            audio.channels[0][static_cast<std::size_t>(at + i)] =
                static_cast<float>(.8 * std::exp(-double(i) / 400) * std::cos(double(i) * .08));
    const auto path = f.dir / "attacks.wav";
    REQUIRE(audio::writeWav(path, audio));
    auto args = json::Value::object();
    args["track"] = f.item(f.itemOnRow(0)).track.str();
    args["audio_file"] = path.string();
    args["position"] = 0;
    auto added = f.dispatcher->execute({"item.add", args, kUi});
    REQUIRE(added);
    const auto item = *model::Id::parse(added->find("item")->asString());
    const auto count = f.app->head().items.size();
    const auto steps = f.steps();
    gui::AudioEdit edits(*f.app, *f.dispatcher);
    std::string message;
    edits.onStatus = [&](const auto& status) { message = status; };
    edits.run({item}, "slice", .25);
    {
        std::unique_lock lock(mutex);
        done.wait(lock, [&] { return finished; });
    }
    f.dispatcher->flushJobEvents();
    edits.poll();
    INFO(message);
    CHECK(f.app->head().items.size() == count + 3);
    CHECK(f.steps() == steps + 2);
}
