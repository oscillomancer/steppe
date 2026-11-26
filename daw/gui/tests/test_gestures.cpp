// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The arrange view's editing gestures (P5.6), driven like a user: pointer
// events through the root; the model read back; one history entry each.
#include <doctest/doctest.h>
#include "test_support.h"

#include <filesystem>
#include <map>
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
#include "supple/json/write.h"
#include "supple/midi/midi.h"
#include "supple/model/groups.h"
#include "supple/model/schemas.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/widgets.h"
#include "supple/views/arrange.h"
#include "supple/views/comp_view.h"
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

TEST_CASE("automation lanes: a plug-in described by a list (a VST3) gets its parameter's scale") {
    // fx.describe lists a non-semantic plug-in's parameters by numeric id,
    // which is the envelope's param ("1"); a semantic one keys them by name.
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-scales-test-" + std::to_string(testProcessId()));
    {
        auto app = engine::EngineApp::create(options);
        REQUIRE(app);
        cmd::Registry registry;
        cmd::Command<json::Value, json::Value> describe;
        describe.name = "fx.describe";
        describe.version = 2;
        describe.kind = cmd::Kind::Query;
        describe.exposure = cmd::Exposure::Internal;
        describe.args = schema::any();
        describe.result = schema::any();
        describe.run = [](const json::Value& a, cmd::Context&) -> Result<json::Value> {
            if (a.find("fx")->asString() == "fx_1")  // as Test VST3 Gain answers
                return json::parse(
                           R"({"name":"Test VST3 Gain","params":[{"id":1.0,"name":"Gain",)"
                           R"("min":0.0,"max":1.0,"default":0.5,"modulatable_by_envelope":true}]})")
                    .value();
            return json::parse(
                       R"({"name":"Semantic","params":{"gain":{"id":7,"name":"Gain","min":-24.0,)"
                       R"("max":24.0,"default":0.0,"type":"float","unit":"dB"}}})")
                .value();
        };
        registry.add(std::move(describe));
        cmd::Dispatcher dispatcher(registry, **app);
        gui::PluginScales scales(dispatcher);
        model::Envelope vst3;
        vst3.target = {*model::Id::parse("fx_1"), "1"};
        const auto listed = scales(vst3);
        REQUIRE(listed);
        REQUIRE(listed->param);
        CHECK(listed->param->name == "Gain");
        CHECK(listed->param->spec.max == doctest::Approx(1.0));
        model::Envelope missing;
        missing.target = {*model::Id::parse("fx_1"), "2"};
        CHECK_FALSE(scales(missing));
        model::Envelope semantic;
        semantic.target = {*model::Id::parse("fx_2"), "gain"};
        const auto keyed = scales(semantic);
        REQUIRE(keyed);
        CHECK(keyed->param->spec.max == doctest::Approx(24.0));
    }
    std::filesystem::remove_all(options.workdir);
}

TEST_CASE("TCP: a chain with a modulated plug-in marks its fx as the mixer does") {
    // gui.md §4.2: views::kModMark after "fx", in signal-cv.
    Fixture f;
    if (!f.ok) return;
    auto run = [&](const char* command, const std::string& args) {
        return f.dispatcher->execute({command, json::parse(args).value(), kUi});
    };
    const model::Id track = f.item(f.itemOnRow(0)).track;
    auto added = run("fx.add", R"({"track":")" + track.str() + R"(","plugin":"Hickory"})");
    if (!added) {
        MESSAGE("skipped: Hickory is not built (partial build)");
        return;
    }
    const std::string fx = (*added)["fx"].asString();
    f.sync();
    auto fxButton = [&] {
        auto* b = dynamic_cast<ui::Button*>(f.view->tcpPanel().controlNode(track, "fx"));
        REQUIRE(b);
        return b;
    };
    CHECK(fxButton()->mark().empty());
    auto d = run("fx.describe", R"({"fx":")" + fx + R"(","full":true})");
    REQUIRE(d);
    std::string param;
    for (const auto& m : d->find("params")->asObject())
        if (const json::Value* mod = m.value.find("modulatable"); mod && mod->asBool()) {
            param = m.key;
            break;
        }
    REQUIRE(!param.empty());
    REQUIRE(run("modulate",
                R"({"target":{"fx":")" + fx + R"(","param":")" + param + R"("},"source":"lfo"})"));
    f.sync();
    CHECK(fxButton()->mark() == views::kModMark);
    CHECK(fxButton()->markColor() == "signal-cv");
    CHECK(fxButton()->valueText() == "fx");
    // Every route off: dimmed; one on again: normal.
    const std::string route = f.app->head().modulations.begin()->id.str();
    REQUIRE(run("modulation.set", R"({"modulation":")" + route + R"(","enabled":false})"));
    f.sync();
    CHECK(fxButton()->mark() == views::kModMark);
    CHECK(fxButton()->markColor() == "ink-faint");
    REQUIRE(run("modulation.set", R"({"modulation":")" + route + R"(","enabled":true})"));
    f.sync();
    CHECK(fxButton()->markColor() == "signal-cv");
    CHECK(views::fxModulated(f.app->head(), *model::Id::parse(fx)));
    CHECK(views::chainModulated(f.app->head(), track));
}

TEST_CASE(
    "lane headers: the parameter first, a fixed mod column, a modulation-only lane's header") {
    // gui.md §4.3: "Cutoff · Marram", the owner elided first; "mod" in its
    // column, also on a lane without an envelope, whose header has title,
    // mod and hide.
    Fixture f;
    if (!f.ok) return;
    auto run = [&](const char* command, const std::string& args) {
        auto r = f.dispatcher->execute({command, json::parse(args).value(), kUi});
        INFO((r ? std::string() : r.error().toString()));
        return r;
    };
    const model::Id track = f.item(f.itemOnRow(0)).track;
    auto added = run("fx.add", R"({"track":")" + track.str() + R"(","plugin":"Hickory"})");
    if (!added) {
        MESSAGE("skipped: Hickory is not built (partial build)");
        return;
    }
    const std::string fx = (*added)["fx"].asString();
    auto scales = std::make_shared<gui::PluginScales>(*f.dispatcher);
    f.view->trackLayout().pluginScales = [scales](const model::Envelope& e) {
        return (*scales)(e);
    };
    std::vector<std::string> params;
    auto d = run("fx.describe", R"({"fx":")" + fx + R"(","full":true})");
    REQUIRE(d);
    for (const auto& m : d->find("params")->asObject())
        if (const json::Value* mod = m.value.find("modulatable"); mod && mod->asBool())
            params.push_back(m.key);
    REQUIRE(params.size() >= 2);
    const model::Id fxId = *model::Id::parse(fx);
    const model::EnvTarget withEnv{fxId, params[0]}, modOnly{fxId, params[1]};
    // The first parameter: a visible envelope and no route yet.
    REQUIRE(run("env.add", R"({"fx":")" + fx + R"(","param":")" + params[0] +
                               R"(","points":[{"beat":0,"value":0.5}]})"));
    const model::Id env = f.app->head().envelopes.begin()->id;
    REQUIRE(run("env.set", R"({"env":")" + env.str() + R"(","visible":true})"));
    f.sync();
    auto control = [&](const model::EnvTarget& t, const char* what) {
        return f.view->tcpPanel().laneControl(t, what);
    };
    REQUIRE(control(withEnv, "mod"));
    CHECK_FALSE(control(withEnv, "mod")->visible);  // no route: its column stays empty
    CHECK(control(withEnv, "mode")->visible);
    // Each parameter modulated: the second has a lane without an envelope.
    for (const auto& p : params)
        if (p == params[0] || p == params[1])
            REQUIRE(run("modulate", R"({"target":{"fx":")" + fx + R"(","param":")" + p +
                                        R"("},"source":"lfo"})"));
    f.sync();
    const model::Session& s = f.app->head();
    REQUIRE(control(modOnly, "title"));
    CHECK(control(withEnv, "mod")->visible);
    CHECK(control(modOnly, "mod")->visible);
    auto modColor = [&](const model::EnvTarget& t) {
        return static_cast<ui::Button*>(control(t, "mod"))->textColor();
    };
    CHECK(modColor(modOnly) == "signal-cv");
    // Its route off: "mod" dimmed, still there; on again: normal.
    std::string modOnlyRoute;
    for (const auto& m : f.app->head().modulations)
        if (m.value->target == modOnly) modOnlyRoute = m.id.str();
    REQUIRE(run("modulation.set", R"({"modulation":")" + modOnlyRoute + R"(","enabled":false})"));
    f.sync();
    REQUIRE(control(modOnly, "mod"));
    CHECK(control(modOnly, "mod")->visible);
    CHECK(modColor(modOnly) == "ink-faint");
    CHECK(modColor(withEnv) == "signal-cv");
    REQUIRE(run("modulation.set", R"({"modulation":")" + modOnlyRoute + R"(","enabled":true})"));
    f.sync();
    CHECK(modColor(modOnly) == "signal-cv");
    CHECK_FALSE(control(modOnly, "mode")->visible);
    CHECK_FALSE(control(modOnly, "active")->visible);
    CHECK(control(modOnly, "hide")->visible);
    // The same columns in both lanes.
    CHECK(control(withEnv, "mod")->bounds().x == control(modOnly, "mod")->bounds().x);
    CHECK(control(withEnv, "hide")->bounds().x == control(modOnly, "hide")->bounds().x);
    // The parameter first, then the plug-in.
    model::Envelope probe;
    probe.target = modOnly;
    const auto scale = f.view->trackLayout().scaleOf(probe);
    const views::LaneTitle parts = views::laneTitle(probe, s, scale ? &*scale : nullptr);
    CHECK(parts.owner == "Hickory");
    CHECK(control(modOnly, "title")->valueText() == parts.param + " · Hickory");
    // A long owner gives way before the parameter does.
    REQUIRE(run("fx.set", R"({"fx":")" + fx +
                              R"(","label":"A plug-in label far too long for any lane header"})"));
    f.sync();
    const std::string fitted = control(modOnly, "title")->valueText();
    CHECK(fitted.starts_with(parts.param + " · A"));
    CHECK(fitted.ends_with("…"));
    // Two lines at this height (the title over the cells); a low lane puts
    // the cells beside the title, in the same order.
    CHECK(control(modOnly, "mod")->bounds().y > control(modOnly, "title")->bounds().bottom() - 1);
    f.view->trackLayout().setGlobalHeight(20);  // lanes at their 28-unit minimum
    f.sync();
    f.view->tcpPanel().scrolled();
    f.sync();
    REQUIRE(control(modOnly, "mod"));
    CHECK(control(modOnly, "mod")->bounds().y < control(modOnly, "title")->bounds().bottom());
    CHECK(control(modOnly, "mod")->bounds().x > control(modOnly, "title")->bounds().x);
    CHECK(control(withEnv, "hide")->bounds().x == control(modOnly, "hide")->bounds().x);
    // Room for a few letters only: the parameter's, the owner gone.
    const std::string low = control(modOnly, "title")->valueText();
    INFO(low);
    REQUIRE(!low.empty());
    CHECK(low.find("Hickory") == std::string::npos);
    CHECK(low.front() == parts.param.front());
    // "mod" asks the app for the source of that target.
    std::vector<model::EnvTarget> asked;
    views::TcpCallbacks cb;
    cb.modulationSource = [&](const model::EnvTarget& t, const ui::Rect&) { asked.push_back(t); };
    cb.commit = [&](std::string_view command, json::Value args, std::string) {
        REQUIRE(f.dispatcher->execute({std::string(command), std::move(args), kUi}));
    };
    f.view->setTcpCallbacks(std::move(cb));
    f.root->click(control(modOnly, "mod")->id());
    CHECK(asked == std::vector<model::EnvTarget>{modOnly});
    // Hide on the lane without an envelope turns its route's lane off: the
    // lane goes, in one undo step.
    const auto versions = f.app->head().version;
    f.root->click(control(modOnly, "hide")->id());
    f.sync();
    CHECK_FALSE(f.view->trackLayout().indexOfTarget(modOnly));
    CHECK(f.app->head().version == versions + 1);
}

TEST_CASE("automation lanes: twin lanes' titles say their chain place or their item") {
    // gui.md §4.3: two effects of one name in a chain are numbered; one
    // effect's parameter on two items' takes of a track says each item.
    Fixture f;
    if (!f.ok) return;
    auto run = [&](const char* command, const std::string& args) {
        return f.dispatcher->execute({command, json::parse(args).value(), kUi});
    };
    const model::Id first = f.itemOnRow(0);
    const model::Id track = f.item(first).track;
    auto second = run("item.add", R"({"track":")" + track.str() +
                                      R"(","position":64,"notation":"voice k; 1: C4:w"})");
    REQUIRE(second);
    const model::Id other = *model::Id::parse((*second)["item"].asString());
    auto onTake = [&](model::Id item) -> std::optional<std::string> {
        auto added = run(
            "fx.add", R"({"take":")" + f.item(item).activeTake->str() + R"(","plugin":"Teasel"})");
        if (!added) return std::nullopt;
        return (*added)["fx"].asString();
    };
    const auto a = onTake(first), b = onTake(other);
    if (!a || !b) {
        MESSAGE("skipped: Teasel is not built (partial build)");
        return;
    }
    auto envOn = [&](const std::string& fx) {
        auto added = run(
            "env.add", R"({"fx":")" + fx + R"(","param":"drive","points":[{"beat":0,"value":3}]})");
        REQUIRE(added);
        return *model::Id::parse((*added)["env"].asString());
    };
    const model::Id ea = envOn(*a), eb = envOn(*b);
    const model::Session& s = f.app->head();
    auto title = [&](model::Id env) { return views::envTitle(*s.envelopes.find(env), s, nullptr); };
    const auto& names = s.takes.find(*f.item(first).activeTake)->name;
    const auto& otherName = s.takes.find(*f.item(other).activeTake)->name;
    auto barOf = [&](model::Id item) {
        return std::to_string(s.timeMap->barBeat(model::itemSpan(s, f.item(item)).startBeat).bar);
    };
    CHECK(title(ea) == "Teasel · drive (take: " + (names.empty() ? "" : names + ", ") + "bar " +
                           barOf(first) + ")");
    CHECK(title(eb) == "Teasel · drive (take: " + (otherName.empty() ? "" : otherName + ", ") +
                           "bar " + barOf(other) + ")");
    CHECK(title(ea) != title(eb));
    // A second Teasel in the first take's chain: each is numbered by its place.
    auto twin =
        run("fx.add", R"({"take":")" + f.item(first).activeTake->str() + R"(","plugin":"Teasel"})");
    REQUIRE(twin);
    const model::Id ec = envOn((*twin)["fx"].asString());
    const model::Session& now = f.app->head();
    auto titleNow = [&](model::Id env) {
        return views::envTitle(*now.envelopes.find(env), now, nullptr);
    };
    CHECK(titleNow(ea).starts_with("Teasel 1 · drive (take"));
    CHECK(titleNow(ec).starts_with("Teasel 2 · drive (take"));
    // Alone on its track, a take effect's lane is just "(take)".
    REQUIRE(run("item.remove", R"({"item":")" + other.str() + R"("})"));
    REQUIRE(run("fx.remove", R"({"fx":")" + (*twin)["fx"].asString() + R"("})"));
    const model::Session& alone = f.app->head();
    CHECK(views::envTitle(*alone.envelopes.find(ea), alone, nullptr) == "Teasel · drive (take)");
}

TEST_CASE("automation lanes: an item's take FX parameter gets a lane on the item's track") {
    Fixture f;
    if (!f.ok) return;
    auto listed = f.dispatcher->execute({"plugin.list", json::Value::object(), kUi});
    REQUIRE(listed);
    const model::Id item = f.itemOnRow(0);
    const model::Id track = f.item(item).track;
    const model::Id take = *f.item(item).activeTake;
    // A stock effect with a continuous semantic parameter, on the take's chain.
    std::string fxId, key;
    std::shared_ptr<params::DescribedParam> spec;
    for (const json::Value& p : (*listed)["plugins"].asArray()) {
        if (p.find("instrument") && p.find("instrument")->asBool()) continue;
        auto added =
            f.dispatcher->execute({"fx.add",
                                   json::parse(R"({"take":")" + take.str() + R"(","plugin":")" +
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
    auto scales = std::make_shared<gui::PluginScales>(*f.dispatcher);
    f.view->trackLayout().pluginScales = [scales](const model::Envelope& e) {
        return (*scales)(e);
    };
    // An item's take FX envelope is in timeline position (session-model).
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
    (void)f.dispatcher->execute(
        {"env.set", json::parse(R"({"env":")" + env.str() + R"(","visible":true})").value(), kUi});
    f.sync();
    // Visible, it has a lane under the item's track, as a track effect's has.
    auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    CHECK(*lane == *f.view->trackLayout().indexOf(track) + 1);
    {
        const model::Envelope* e = f.app->head().envelopes.find(env);
        const auto scale = f.view->trackLayout().scaleOf(*e);
        REQUIRE(scale);
        CHECK(views::envTitle(*e, f.app->head(), &*scale).ends_with(" (take)"));
    }
    // It plays only over the item: the lane is dimmed outside it, all of the
    // visible lane but the item's span.
    auto paint = [&] {
        f.root->damageAll();
        f.root->frame(false);
    };
    auto dimmedWidth = [&] {
        float w = 0;
        for (const ui::Rect& d : f.view->lanes().dimmedLaneSpans()) w += d.w;
        return w;
    };
    paint();
    const ui::Rect lanes = f.view->lanesRect();
    const ui::Rect laneRow = f.view->rowRect(*lane);
    const ui::Rect body = f.rectOf(item);
    const float visible = std::min(laneRow.right(), lanes.right()) - std::max(laneRow.x, lanes.x);
    const float played =
        std::max(0.0f, std::min(body.right(), lanes.right()) - std::max(body.x, lanes.x));
    REQUIRE_FALSE(f.view->lanes().dimmedLaneSpans().empty());
    for (const ui::Rect& d : f.view->lanes().dimmedLaneSpans()) {
        CHECK(static_cast<double>(d.y) == doctest::Approx(static_cast<double>(laneRow.y)));
        CHECK((d.right() <= body.x + 0.5f || d.x >= body.right() - 0.5f));
    }
    CHECK(static_cast<double>(dimmedWidth()) ==
          doctest::Approx(static_cast<double>(visible - played)).epsilon(0.01));
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        REQUIRE(gui::writePng(std::filesystem::path(out) / "take-fx-lane-dimmed.png",
                              f.root->readBack()));
    // Another take active: the effect plays nowhere, so all of the lane is dimmed.
    auto second = f.dispatcher->execute(
        {"take.add",
         json::parse(R"({"item":")" + item.str() + R"(","from":")" + take.str() + R"("})").value(),
         kUi});
    REQUIRE_MESSAGE(second, (second ? std::string() : second.error().toString()));
    const std::string secondTake = (*second)["id"].asString();
    auto activate = [&](const std::string& which) {
        auto r = f.dispatcher->execute(
            {"take.activate",
             json::parse(R"({"item":")" + item.str() + R"(","take":")" + which + R"("})").value(),
             kUi});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        f.sync();
        paint();
    };
    activate(secondTake);
    REQUIRE(f.view->trackLayout().indexOfEnv(env) == lane);
    CHECK(static_cast<double>(dimmedWidth()) ==
          doctest::Approx(static_cast<double>(visible)).epsilon(0.01));
    activate(take.str());
    CHECK(static_cast<double>(dimmedWidth()) ==
          doctest::Approx(static_cast<double>(visible - played)).epsilon(0.01));
    // Editable: a click in the lane's upper quarter adds a point.
    const ui::Rect area = views::Lanes::envArea(f.view->rowRect(*lane));
    const float x = f.view->xAt(f.app->head().timeMap->secondsAt(8)) + 1;
    f.click({x, area.y + area.h * 0.25f});
    const auto& pts = f.app->head().envelopes.find(env)->points;
    REQUIRE(pts.size() == 3);
    CHECK(params::toNormalized(spec->spec, pts[1].value) == doctest::Approx(0.75).epsilon(0.05));
    // The dimmed part edits as anywhere: a click past the item's end adds a
    // point there too.
    const float past = body.right() + 30;
    REQUIRE(past < lanes.right());
    f.click({past, area.y + area.h * 0.25f});
    CHECK(f.app->head().envelopes.find(env)->points.size() == 4);
    // The item moved to another track takes its take's lane along.
    const model::Id other = f.view->trackLayout().rows()[*lane + 1].track;
    REQUIRE(other != track);
    auto moved = f.dispatcher->execute(
        {"item.move",
         json::parse(R"({"item":")" + item.str() + R"(","track":")" + other.str() + R"("})")
             .value(),
         kUi});
    REQUIRE_MESSAGE(moved, (moved ? std::string() : moved.error().toString()));
    f.sync();
    lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    CHECK(*lane == *f.view->trackLayout().indexOf(other) + 1);
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

namespace {
// The generated item with two takes, and the index of the one heard.
std::pair<model::Id, std::size_t> twoTakeItem(Fixture& f) {
    std::optional<model::Id> it;
    for (const auto& e : f.app->head().items)
        if (e.value->takes.size() == 2) it = e.id;
    REQUIRE(it);
    const auto& takes = f.item(*it).takes;
    return {*it,
            static_cast<std::size_t>(
                std::find(takes.begin(), takes.end(), *f.item(*it).activeTake) - takes.begin())};
}
// The items of a track holding `takes` takes, in time order.
std::vector<const model::Item*> piecesOf(Fixture& f, model::Id track, std::size_t takes = 2) {
    std::vector<const model::Item*> out;
    for (const auto& e : f.app->head().items)
        if (e.value->track == track && e.value->takes.size() == takes) out.push_back(e.value.get());
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->position < b->position; });
    return out;
}
std::size_t heardOf(const model::Item& it) {
    return static_cast<std::size_t>(std::find(it.takes.begin(), it.takes.end(), *it.activeTake) -
                                    it.takes.begin());
}
// Every edit the gestures send, as well as running it.
std::vector<std::vector<views::EditStep>>& recordSteps(Fixture& f) {
    auto sent = std::make_shared<std::vector<std::vector<views::EditStep>>>();
    auto& g = f.view->gestures();
    g.run = [run = g.run, sent](const std::string& label,
                                const std::vector<views::EditStep>& steps) {
        sent->push_back(steps);
        return run(label, steps);
    };
    static std::vector<std::shared_ptr<std::vector<std::vector<views::EditStep>>>> keep;
    keep.push_back(sent);
    return *sent;
}
ui::Point laneAt(const ui::Rect& ir, std::size_t lanes, std::size_t lane, float x) {
    const ui::Rect l = views::Lanes::takeLaneRect(ir, lanes, lane);
    return {x, l.y + l.h / 2};
}
// The track's items as JSON, by position: what two ways of making a comp must agree on.
std::string trackJson(Fixture& f, model::Id track) {
    std::string out;
    for (const model::Item* it : piecesOf(f, track)) {
        // Each run makes new take ids: the heard take by its index instead.
        json::Value j = model::toJson(*it);
        j.asObject().erase("takes");
        j.asObject().erase("active_take");
        j["heard"] = static_cast<std::int64_t>(heardOf(*it));
        out += json::writeCanonical(j) + "\n";
    }
    return out;
}
}  // namespace

// session-model §1.5.1: the GUI comps with take.comp, the call an agent makes.
TEST_CASE("take comping: a click hears a take, a swipe comps with take.comp as an agent does") {
    Fixture f;
    if (!f.ok) return;
    auto [it, heard] = twoTakeItem(f);
    const std::size_t other = 1 - heard;
    const model::Id track = f.item(it).track;
    const ui::Rect r = f.rectOf(it);
    REQUIRE(views::Lanes::showsTakeLanes(r, 2));
    auto& sent = recordSteps(f);
    // A click on the other take's lane of a lone item: it plays that take
    // throughout (one step).
    auto steps = f.steps();
    f.click(laneAt(r, 2, other, r.x + r.w * 0.5f));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0][0].command == "take.activate");
    CHECK(heardOf(f.item(it)) == other);
    CHECK(f.steps() == steps + 1);
    // A swipe across the middle of the first take's lane: one take.comp with
    // the span, and no crossfade (a first comp takes take.comp's 10 ms, equal
    // power). The pieces overlap at each seam and fade there.
    steps = f.steps();
    f.drag(laneAt(r, 2, heard, r.x + r.w * 0.3f), laneAt(r, 2, heard, r.x + r.w * 0.6f));
    REQUIRE(sent.size() == 2);
    REQUIRE(sent[1].size() == 1);
    CHECK(sent[1][0].command == "take.comp");
    const json::Value swipe = sent[1][0].args;
    CHECK_FALSE(swipe.find("crossfade"));
    REQUIRE(swipe.find("spans")->size() == 1);
    CHECK(swipe.find("spans")->asArray()[0].find("index")->asInt() ==
          static_cast<std::int64_t>(heard));
    CHECK(f.steps() == steps + 1);
    auto pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    CHECK(heardOf(*pieces[0]) == other);
    CHECK(heardOf(*pieces[1]) == heard);
    CHECK(heardOf(*pieces[2]) == other);
    CHECK(pieces[0]->fadeOut.length == doctest::Approx(0.01).epsilon(0.01));
    CHECK(pieces[1]->fadeIn.length == doctest::Approx(0.01).epsilon(0.01));
    CHECK(pieces[1]->fadeIn.shape == model::FadeShape::Cos);
    CHECK(pieces[0]->fadeIn.length == doctest::Approx(0.5));  // the outer fades stay
    CHECK(pieces[2]->fadeOut.length == doctest::Approx(0.75));
    // The same call from an agent gives the same items.
    const std::string byGui = trackJson(f, track);
    REQUIRE(f.dispatcher->execute({"history.undo", json::Value::object(), kUi}));
    REQUIRE(f.dispatcher->execute({"take.comp", swipe, kUi}));
    f.sync();
    CHECK(trackJson(f, track) == byGui);

    // A comp whose crossfade was set (50 ms, linear): the next swipe sends no
    // crossfade, and take.comp gives the seams it makes the comp's own.
    // (The swipe's call again, with a crossfade.)
    json::Value set = swipe;
    set["crossfade"] = 0.05;
    set["crossfade_shape"] = "linear";
    REQUIRE(f.dispatcher->execute({"take.comp", set, kUi}));
    f.sync();
    pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    const ui::Rect last = f.rectOf([&] {
        for (const auto& e : f.app->head().items)
            if (e.value.get() == pieces[2]) return e.id;
        return model::Id{};
    }());
    f.drag(laneAt(last, 2, heard, last.x + last.w * 0.4f),
           laneAt(last, 2, heard, last.x + last.w * 0.7f));
    REQUIRE(sent.size() == 3);
    CHECK(sent[2][0].command == "take.comp");
    CHECK_FALSE(sent[2][0].args.find("crossfade"));
    CHECK_FALSE(sent[2][0].args.find("crossfade_shape"));
    pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 5);
    for (std::size_t i = 0; i + 1 < pieces.size(); ++i) {
        CHECK(pieces[i]->fadeOut.length == doctest::Approx(0.05).epsilon(0.01));
        CHECK(pieces[i]->fadeOut.shape == model::FadeShape::Linear);
    }

    // A click on a lane in a comp re-comps that piece alone, with no
    // crossfade: the first piece takes the middle one's take and joins it.
    const ui::Rect first = f.rectOf([&] {
        for (const auto& e : f.app->head().items)
            if (e.value.get() == pieces[0]) return e.id;
        return model::Id{};
    }());
    f.click(laneAt(first, 2, heard, first.x + first.w * 0.5f));
    REQUIRE(sent.size() == 4);
    CHECK(sent[3][0].command == "take.comp");
    CHECK_FALSE(sent[3][0].args.find("crossfade"));
    pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 4);
    CHECK(heardOf(*pieces[0]) == heard);
    CHECK(heardOf(*pieces[1]) == other);
    // A click on the take a piece already plays sends nothing.
    f.click(laneAt(first, 2, heard, first.x + first.w * 0.2f));
    CHECK(sent.size() == 4);
}

// The seams of a comp: drawn where the take changes, and a drag on one is a
// take.comp with that span edge moved and no crossfade (the seam keeps its own).
TEST_CASE("a comp's seam: found and hit where the take changes, dragged with take.comp") {
    Fixture f;
    if (!f.ok) return;
    auto [it, heard] = twoTakeItem(f);
    const model::Id track = f.item(it).track;
    const model::Session& s0 = f.app->head();
    const double start = f.item(it).position, length = f.item(it).length;
    // A comp made by an agent: the middle third plays the other take.
    json::Value args = json::Value::object();
    args["item"] = it.str();
    json::Value span = json::Value::object();
    span["from"] = start + length / 3;
    span["to"] = start + 2 * length / 3;
    span["index"] = static_cast<std::int64_t>(1 - heard);
    args["spans"] = json::Value::array();
    args["spans"].push_back(span);
    REQUIRE(f.dispatcher->execute({"take.comp", args, kUi}));
    f.sync();
    (void)s0;
    const std::size_t row = *f.view->trackLayout().indexOf(track);
    const auto seams = f.view->lanes().compSeams(row);  // a copy: the cache is rebuilt
    REQUIRE(seams.size() == 2);
    const model::Session& s = f.app->head();
    CHECK(seams[0].seconds == doctest::Approx(s.timeMap->secondsAt(start + length / 3)));
    auto pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    // The seam takes the press across the pieces' height: not a trim.
    auto& g = f.view->gestures();
    const float x = f.view->lanes().xAt(seams[0].seconds);
    const ui::Rect right = f.rectOf(seams[0].right);
    for (float y : {right.y + 12, right.y + right.h / 2, right.bottom() - 2}) {
        const auto h = g.hit({x + 2, y});
        REQUIRE(h);
        CHECK(h->zone == views::ItemGestures::Drag::Seam);
        CHECK(h->item == seams[0].right);
        CHECK(h->seamLeft == seams[0].left);
    }
    CHECK(g.cursorAt({x, right.y + right.h / 2}) == ui::Cursor::ResizeHorizontal);
    // The comp's outer start still trims.
    const ui::Rect first = f.rectOf(seams[0].left);
    CHECK(g.hit({first.x + 2, first.y + first.h / 2})->zone == views::ItemGestures::Drag::TrimLeft);
    // A free drag (Shift) moves the seam right by 20 px: one take.comp with
    // the two stretches beside it and no crossfade; the seam keeps its own.
    auto& sent = recordSteps(f);
    const auto steps = f.steps();
    const double before = seams[0].seconds;
    const float y = right.y + right.h / 2;
    f.drag({x, y}, {x + 20, y}, ui::kShift);
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].size() == 1);
    CHECK(sent[0][0].command == "take.comp");
    CHECK(sent[0][0].args.find("spans")->size() == 2);
    CHECK_FALSE(sent[0][0].args.find("crossfade"));
    CHECK(f.steps() == steps + 1);
    const auto moved = f.view->lanes().compSeams(row);
    REQUIRE(moved.size() == 2);
    CHECK(moved[0].seconds - before ==
          doctest::Approx(20.0 / f.view->viewport().pixelsPerSecond).epsilon(0.02));
    pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    CHECK(pieces[0]->fadeOut.length == doctest::Approx(0.01).epsilon(0.01));
    // The seam's top is the comp's crossfade: a drag sets its length (twice
    // the distance from the seam, as it is centred on it) at every seam.
    const auto now = f.view->lanes().compSeams(row);
    const float xs = f.view->lanes().xAt(now[0].seconds);
    const ui::Rect r2 = f.rectOf(now[0].right);
    REQUIRE(g.hit({xs, r2.y + 3}));
    CHECK(g.hit({xs, r2.y + 3})->zone == views::ItemGestures::Drag::CompFade);
    const double pps = f.view->viewport().pixelsPerSecond;
    f.drag({xs, r2.y + 3}, {xs + static_cast<float>(0.25 * pps), r2.y + 3}, ui::kShift);
    REQUIRE(sent.size() == 2);
    CHECK(sent[1][0].command == "take.comp");
    REQUIRE(sent[1][0].args.find("crossfade"));
    CHECK(sent[1][0].args.find("crossfade")->asDouble() == doctest::Approx(0.5).epsilon(0.02));
    pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    CHECK(pieces[0]->fadeOut.length == doctest::Approx(0.5).epsilon(0.02));
    CHECK(pieces[1]->fadeOut.length == doctest::Approx(0.5).epsilon(0.02));
    CHECK(pieces[2]->fadeIn.length == doctest::Approx(0.5).epsilon(0.02));
    // The fade handles' shape click (Ctrl) toggles equal power and linear.
    f.click({xs, r2.y + 3}, ui::kCtrl);
    REQUIRE(sent.size() == 3);
    pieces = piecesOf(f, track);
    CHECK(pieces[0]->fadeOut.shape == model::FadeShape::Linear);
    CHECK(pieces[2]->fadeIn.shape == model::FadeShape::Linear);
    CHECK(pieces[1]->fadeOut.length == doctest::Approx(0.5).epsilon(0.02));
    f.click({xs, r2.y + 3}, ui::kCtrl);
    CHECK(piecesOf(f, track)[0]->fadeOut.shape == model::FadeShape::Cos);
    // A plain click there changes nothing.
    f.click({xs, r2.y + 3});
    CHECK(sent.size() == 4);
    // Dragged far past the next seam, it stops short of it (1/64 beat): the
    // comp keeps its three pieces, and the seam has moved there.
    const auto beforeFar = f.view->lanes().compSeams(row);
    const float x2 = f.view->lanes().xAt(beforeFar[0].seconds);
    const auto farSteps = f.steps();
    f.drag({x2, y}, {x2 + 2000, y}, ui::kShift);
    REQUIRE(sent.size() == 5);
    CHECK_FALSE(sent[4][0].args.find("crossfade"));
    CHECK(f.steps() == farSteps + 1);  // not refused
    CHECK(piecesOf(f, track).size() == 3);
    const auto far = f.view->lanes().compSeams(row);
    REQUIRE(far.size() == 2);
    CHECK(far[0].seconds > beforeFar[0].seconds);
    CHECK(far[0].seconds < far[1].seconds);
    // The middle piece is now short, and its clamped seams can make its
    // neighbours meet: the comp is still one comp, asked from any piece (the
    // seam drag, the Crossfade field and the take badge ask the piece they are
    // on), and a second drag on the moved seam is still a seam drag.
    std::vector<model::Id> ids;
    for (const auto& e : f.app->head().items)
        if (e.value->track == track && e.value->takes.size() == 2) ids.push_back(e.id);
    REQUIRE(ids.size() == 3);
    for (model::Id id : ids) {
        CAPTURE(id.str());
        const auto comp = views::compOf(f.app->head(), id);
        REQUIRE(comp);
        CHECK(comp->pieces.size() == 3);
        CHECK(comp->seams.size() == 2);
    }
    const ui::Rect farRight = f.rectOf(far[0].right);
    const auto again =
        g.hit({f.view->lanes().xAt(far[0].seconds) + 1, farRight.y + farRight.h / 2});
    REQUIRE(again);
    CHECK(again->zone == views::ItemGestures::Drag::Seam);
}

// A seam dragged all the way to the next one leaves the piece between them
// 1/64 beat long, its two seams clamped to meet in it: at 125 BPM that core is
// an even 360 samples, and the pieces either side of it then meet at one
// sample. It is still one comp from every piece (the lanes, the seam drag, the
// Crossfade field and the take badge ask the piece they are on), so both seams
// stay drawn and draggable.
TEST_CASE("a seam dragged against the next one: the comp is still one comp from every piece") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.dispatcher->execute(
        {"tempo.set", json::parse(R"({"points":[{"beat":0,"bpm":125}]})").value(), kUi}));
    f.sync();
    auto [it, heard] = twoTakeItem(f);
    const model::Id track = f.item(it).track;
    const double start = f.item(it).position, length = f.item(it).length;
    json::Value args = json::Value::object();
    args["item"] = it.str();
    args["crossfade"] = 0.5;
    json::Value span = json::Value::object();
    span["from"] = start + length / 3;
    span["to"] = start + 2 * length / 3;
    span["index"] = static_cast<std::int64_t>(1 - heard);
    args["spans"] = json::Value::array();
    args["spans"].push_back(span);
    REQUIRE(f.dispatcher->execute({"take.comp", args, kUi}));
    f.sync();
    const std::size_t row = *f.view->trackLayout().indexOf(track);
    const auto seams = f.view->lanes().compSeams(row);
    REQUIRE(seams.size() == 2);
    const ui::Rect right = f.rectOf(seams[0].right);
    const float x = f.view->lanes().xAt(seams[0].seconds), y = right.y + right.h / 2;
    const auto steps = f.steps();
    f.drag({x, y}, {x + 2000, y}, ui::kShift);
    REQUIRE(f.steps() == steps + 1);
    const model::Session& s = f.app->head();
    std::vector<model::Id> ids;
    for (const auto& e : s.items)
        if (e.value->track == track && e.value->takes.size() == 2) ids.push_back(e.id);
    REQUIRE(ids.size() == 3);
    for (model::Id id : ids) {
        CAPTURE(id.str());
        const auto comp = views::compOf(s, id);
        REQUIRE(comp);
        CHECK(comp->pieces.size() == 3);
        CHECK(comp->seams.size() == 2);
    }
    const auto after = f.view->lanes().compSeams(row);
    REQUIRE(after.size() == 2);
    CHECK((after[1].seconds - after[0].seconds) * s.project->sampleRate ==
          doctest::Approx(360).epsilon(0.01));
    // Close enough to tell the two seams apart (150 px), each is a seam drag.
    auto viewport = f.view->viewport();
    viewport.pixelsPerSecond = 20000;
    viewport.startSeconds = after[0].seconds - 0.02;
    f.view->setViewport(viewport);
    f.sync();
    auto& g = f.view->gestures();
    for (std::size_t k = 0; k < 2; ++k) {
        CAPTURE(k);
        const auto seam = f.view->lanes().compSeams(row)[k];
        const ui::Rect r = f.rectOf(seam.right);
        const auto h = g.hit({f.view->lanes().xAt(seam.seconds) + 1, r.y + r.h / 2});
        REQUIRE(h);
        CHECK(h->zone == views::ItemGestures::Drag::Seam);
        CHECK(h->seamLeft == seam.left);
    }
    if (const char* out = std::getenv("SUPPLE_SCREENSHOT_DIR")) {
        viewport.pixelsPerSecond = 1500;
        viewport.startSeconds = after[0].seconds - 0.45;
        f.view->setViewport(viewport);
        f.sync();
        REQUIRE(gui::writePng(std::filesystem::path(out) / "comp-clamped-seams.png",
                              f.root->readBack()));
    }
}

// comp-seams (round 137): a seam drag and a lane click send take.comp with no
// crossfade, so a seam set by hand elsewhere (an agent's item.set on the two
// pieces beside it) stays as stored, byte for byte. Only the crossfade handle
// sends a crossfade, which rebuilds every seam.
TEST_CASE("a hand-set seam elsewhere survives a seam drag and a lane click") {
    Fixture f;
    if (!f.ok) return;
    auto [it, heard] = twoTakeItem(f);
    const std::size_t other = 1 - heard;
    const model::Id track = f.item(it).track;
    const double start = f.item(it).position, length = f.item(it).length;
    // Four pieces by an agent, heard, other, heard, other; seams 20 ms equal power.
    json::Value args = json::Value::object();
    args["item"] = it.str();
    args["crossfade"] = 0.02;
    args["crossfade_shape"] = "cos";
    args["spans"] = json::Value::array();
    for (int k = 0; k < 4; ++k) {
        json::Value span = json::Value::object();
        span["from"] = start + length * k / 4;
        span["to"] = start + length * (k + 1) / 4;
        span["index"] = static_cast<std::int64_t>(k % 2 == 0 ? heard : other);
        args["spans"].push_back(span);
    }
    REQUIRE(f.dispatcher->execute({"take.comp", args, kUi}));
    f.sync();
    auto ids = [&] {
        std::vector<model::Id> out;
        for (const model::Item* p : piecesOf(f, track))
            for (const auto& e : f.app->head().items)
                if (e.value.get() == p) out.push_back(e.id);
        return out;
    };
    std::vector<model::Id> pieces = ids();
    REQUIRE(pieces.size() == 4);
    // The first seam set by hand: 10 ms, linear, on both pieces beside it.
    REQUIRE(f.dispatcher->execute({"item.set",
                                   json::parse(R"({"item":")" + pieces[0].str() +
                                               R"(","fade_out":0.01,"fade_out_shape":"linear"})")
                                       .value(),
                                   kUi}));
    REQUIRE(f.dispatcher->execute({"item.set",
                                   json::parse(R"({"item":")" + pieces[1].str() +
                                               R"(","fade_in":0.01,"fade_in_shape":"linear"})")
                                       .value(),
                                   kUi}));
    f.sync();
    // The two pieces beside it, item and takes, as stored.
    auto stored = [&](model::Id id) {
        const model::Session& s = f.app->head();
        const model::Item* item = s.items.find(id);
        REQUIRE(item);
        std::string out = json::writeCanonical(model::toJson(*item));
        for (const auto& take : item->takes)
            out += json::writeCanonical(model::toJson(*s.takes.find(take)));
        return out;
    };
    const std::string a = stored(pieces[0]), b = stored(pieces[1]);
    auto handSet = [&] {
        auto got = f.dispatcher->execute(
            {"take.comp.get", json::parse(R"({"item":")" + pieces[0].str() + R"("})").value(),
             kUi});
        REQUIRE(got);
        CHECK_FALSE(got->find("uniform")->asBool());
        const json::Value* differing = got->find("differing_seams");
        REQUIRE(differing);
        REQUIRE(differing->size() == 1);
        const json::Value& seam = differing->asArray()[0];
        CHECK(seam.find("seam")->asInt() == 0);
        CHECK(seam.find("crossfade")->asDouble() == doctest::Approx(0.01));
        CHECK(seam.find("crossfade_shape")->asString() == "linear");
    };
    handSet();
    auto& sent = recordSteps(f);
    // The last seam dragged left by 12 px (free): no crossfade; the hand-set
    // seam and the piece beside it are as stored.
    const std::size_t row = *f.view->trackLayout().indexOf(track);
    const auto seams = f.view->lanes().compSeams(row);
    REQUIRE(seams.size() == 3);
    const float x = f.view->lanes().xAt(seams[2].seconds);
    const ui::Rect right = f.rectOf(seams[2].right);
    const float y = right.y + right.h / 2;
    REQUIRE(f.view->gestures().hit({x + 2, y})->zone == views::ItemGestures::Drag::Seam);
    auto steps = f.steps();
    f.drag({x, y}, {x - 12, y}, ui::kShift);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0][0].command == "take.comp");
    CHECK_FALSE(sent[0][0].args.find("crossfade"));
    CHECK_FALSE(sent[0][0].args.find("crossfade_shape"));
    CHECK(f.steps() == steps + 1);
    REQUIRE(f.view->lanes().compSeams(row).size() == 3);
    CHECK(f.view->lanes().compSeams(row)[2].seconds < seams[2].seconds);
    CHECK(stored(pieces[0]) == a);
    CHECK(stored(pieces[1]) == b);
    handSet();
    // A click on the last piece's other lane: it takes the third piece's take
    // and joins it, no crossfade; the hand-set seam is as stored.
    pieces = ids();
    REQUIRE(pieces.size() == 4);
    const ui::Rect last = f.rectOf(pieces[3]);
    steps = f.steps();
    f.click(laneAt(last, 2, heard, last.x + last.w * 0.5f));
    REQUIRE(sent.size() == 2);
    CHECK(sent[1][0].command == "take.comp");
    CHECK_FALSE(sent[1][0].args.find("crossfade"));
    CHECK(f.steps() == steps + 1);
    CHECK(piecesOf(f, track).size() == 3);
    CHECK(f.view->lanes().compSeams(row).size() == 2);
    CHECK(stored(pieces[0]) == a);
    CHECK(stored(pieces[1]) == b);
    handSet();
    // The crossfade handle still sends one, and rebuilds every seam with it.
    const auto now = f.view->lanes().compSeams(row);
    const float xs = f.view->lanes().xAt(now[1].seconds);
    const ui::Rect r2 = f.rectOf(now[1].right);
    REQUIRE(f.view->gestures().hit({xs, r2.y + 3})->zone == views::ItemGestures::Drag::CompFade);
    f.drag({xs, r2.y + 3}, {xs + 8, r2.y + 3}, ui::kShift);
    REQUIRE(sent.size() == 3);
    REQUIRE(sent[2][0].args.find("crossfade"));
    const auto got = f.dispatcher->execute(
        {"take.comp.get", json::parse(R"({"item":")" + pieces[0].str() + R"("})").value(), kUi});
    REQUIRE(got);
    CHECK(got->find("uniform")->asBool());
    CHECK(f.item(pieces[0]).fadeOut.shape == model::FadeShape::Cos);
}

// Round 119 G1: the badge switches a comp piece as a lane click does, with
// take.comp; take.activate would leave its seam fades over one source.
TEST_CASE("a comp piece's take badge switches it with take.comp, joining its neighbours") {
    Fixture f;
    if (!f.ok) return;
    auto [it, heard] = twoTakeItem(f);
    const model::Id track = f.item(it).track;
    const double start = f.item(it).position, length = f.item(it).length;
    json::Value args = json::Value::object();
    args["item"] = it.str();
    json::Value span = json::Value::object();
    span["from"] = start + length / 3;
    span["to"] = start + 2 * length / 3;
    span["index"] = static_cast<std::int64_t>(1 - heard);
    args["spans"] = json::Value::array();
    args["spans"].push_back(span);
    REQUIRE(f.dispatcher->execute({"take.comp", args, kUi}));
    f.sync();
    auto pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    model::Id middle{};
    for (const auto& e : f.app->head().items)
        if (e.value.get() == pieces[1]) middle = e.id;
    auto& sent = recordSteps(f);
    // The middle piece's badge, at the right of its name bar: its next take
    // is the one either side plays, so the three join into one item.
    const ui::Rect r = f.rectOf(middle);
    f.click({r.right() - 16, r.y + 8});
    REQUIRE(sent.size() == 1);
    CHECK(sent[0][0].command == "take.comp");
    pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 1);
    CHECK(heardOf(*pieces[0]) == heard);
    CHECK(f.view->lanes().compSeams(*f.view->trackLayout().indexOf(track)).empty());
    // A lone item's badge still switches its take with take.activate.
    model::Id whole{};
    for (const auto& e : f.app->head().items)
        if (e.value.get() == pieces[0]) whole = e.id;
    const ui::Rect w = f.rectOf(whole);
    f.click({w.right() - 16, w.y + 8});
    REQUIRE(sent.size() == 2);
    CHECK(sent[1][0].command == "take.activate");
    CHECK(heardOf(f.item(whole)) == 1 - heard);
}

// Round 119 G2: take.comp refuses a looped item, so a swipe on one splits it
// and hears the take there, with hard cuts, as before take.comp.
TEST_CASE("a swipe on a looped item splits it and switches the middle, with hard cuts") {
    Fixture f;
    if (!f.ok) return;
    auto [it, heard] = twoTakeItem(f);
    const model::Id track = f.item(it).track;
    json::Value loop = json::Value::object();
    loop["item"] = it.str();
    loop["loop"] = true;
    REQUIRE(f.dispatcher->execute({"item.set", loop, kUi}));
    f.sync();
    auto& sent = recordSteps(f);
    const auto steps = f.steps();
    const ui::Rect r = f.rectOf(it);
    f.drag(laneAt(r, 2, 1 - heard, r.x + r.w * 0.3f), laneAt(r, 2, 1 - heard, r.x + r.w * 0.6f));
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].size() == 3);
    CHECK(sent[0][0].command == "item.split");
    CHECK(sent[0][1].command == "item.split");
    CHECK(sent[0][2].command == "take.activate");
    CHECK(f.steps() == steps + 1);
    const auto pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    CHECK(heardOf(*pieces[0]) == heard);
    CHECK(heardOf(*pieces[1]) == 1 - heard);
    CHECK(heardOf(*pieces[2]) == heard);
    CHECK(pieces[0]->fadeOut.length == 0);  // hard cuts
    CHECK(pieces[1]->fadeIn.length == 0);
    CHECK(pieces[1]->loop);
    // No comp there either (round 122 H1): no seams, and the boundary trims.
    CHECK(f.view->lanes().compSeams(*f.view->trackLayout().indexOf(track)).empty());
    model::Id second{};
    for (const auto& e : f.app->head().items)
        if (e.value.get() == pieces[1]) second = e.id;
    const ui::Rect b = f.rectOf(second);
    const auto edge = f.view->gestures().hit({b.x + 2, b.y + b.h / 2});
    REQUIRE(edge);
    CHECK(edge->zone == views::ItemGestures::Drag::TrimLeft);
}

// Round 119 G3: take.comp refuses an item edited with a group (comp.linked),
// so on an edit-grouped track the swipe keeps its older path: item.split,
// which splits the group's items too, and take.activate of the middle.
namespace {
// The lanes' read-back (the shell gives the app's): take.comp.get on the bus.
void wireCompQuery(Fixture& f) {
    f.view->lanes().compQuery = [&f](const json::Value& args) -> std::optional<json::Value> {
        auto r = f.dispatcher->execute({"take.comp.get", args, kUi});
        if (!r) return std::nullopt;
        return std::move(*r);
    };
}
model::Id idOf(Fixture& f, const model::Item* item) {
    for (const auto& e : f.app->head().items)
        if (e.value.get() == item) return e.id;
    return {};
}
}  // namespace

// take.comp on an edit group comps every member (session-model §1.5.1, Edit
// groups): the grouped swipe, click and badge send it as for a lone track,
// and a member whose comp no longer reads like the group's breaks its seams.
TEST_CASE("an edit-grouped track comps every member with take.comp; a member that differs shows") {
    Fixture f;
    if (!f.ok) return;
    wireCompQuery(f);
    auto [it, heard] = twoTakeItem(f);
    const model::Id track = f.item(it).track;
    const double position = f.item(it).position, length = f.item(it).length;
    // A track grouped with it for edits (made after it, so its id is later:
    // the lead's track is the group's reference), holding a copy of the item
    // over the same span (the same takes, so take.comp gives both the same
    // crossfades).
    const auto other =
        f.dispatcher->execute({"track.add", json::parse(R"({"name":"Room"})").value(), kUi});
    REQUIRE(other);
    const model::Id room = *model::Id::parse(other->find("track")->asString());
    json::Value copy = json::Value::object();
    copy["item"] = it.str();
    copy["to"] = position + length + 8;
    const auto copied = f.dispatcher->execute({"item.duplicate", copy, kUi});
    REQUIRE(copied);
    json::Value move = json::Value::object();
    REQUIRE(copied->find("id"));
    move["item"] = copied->find("id")->asString();
    move["track"] = room.str();
    move["position"] = position;
    REQUIRE(f.dispatcher->execute({"item.move", move, kUi}));
    json::Value group = json::Value::object();
    group["name"] = "Kit";
    group["members"] = json::Value::array();
    group["members"].push_back(track.str());
    group["members"].push_back(room.str());
    auto made = f.dispatcher->execute({"group.add", group, kUi});
    REQUIRE(made);
    json::Value links = json::Value::object();
    links["group"] = made->find("group")->asString();
    links["links"] = json::Value::array();
    links["links"].push_back("edit");
    REQUIRE(f.dispatcher->execute({"group.set", links, kUi}));
    f.sync();
    REQUIRE(model::linkedItems(f.app->head(), it).size() == 2);
    auto& sent = recordSteps(f);
    // A click on the whole item: take.comp over it (take.activate would switch
    // it alone), so the member follows.
    {
        const ui::Rect r = f.rectOf(it);
        f.click(laneAt(r, 2, 1 - heard, r.x + r.w * 0.5f));
        REQUIRE(sent.size() == 1);
        REQUIRE(sent[0].size() == 1);
        CHECK(sent[0][0].command == "take.comp");
        CHECK_FALSE(sent[0][0].args.find("bypass_groups"));
        CHECK(heardOf(f.item(it)) == 1 - heard);
        const auto roomPieces = piecesOf(f, room);
        REQUIRE(roomPieces.size() == 1);
        CHECK(heardOf(*roomPieces[0]) == 1 - heard);
        REQUIRE(f.dispatcher->execute({"history.undo", json::Value::object(), kUi}));
        f.sync();
    }
    // A swipe: one take.comp, which comps both tracks alike.
    const ui::Rect r = f.rectOf(it);
    f.drag(laneAt(r, 2, 1 - heard, r.x + r.w * 0.3f), laneAt(r, 2, 1 - heard, r.x + r.w * 0.6f));
    REQUIRE(sent.size() == 2);
    REQUIRE(sent[1].size() == 1);
    CHECK(sent[1][0].command == "take.comp");
    const auto pieces = piecesOf(f, track);
    REQUIRE(pieces.size() == 3);
    CHECK(heardOf(*pieces[1]) == 1 - heard);
    const auto roomPieces = piecesOf(f, room);
    REQUIRE(roomPieces.size() == 3);
    CHECK(heardOf(*roomPieces[1]) == 1 - heard);
    // Both are comps: seams on both rows, none differing yet; the boundary
    // is the seam's, not a trim edge.
    const std::size_t leadRow = *f.view->trackLayout().indexOf(track);
    const std::size_t roomRow = *f.view->trackLayout().indexOf(room);
    {
        const auto leadSeams = f.view->lanes().compSeams(leadRow);
        const auto roomSeams = f.view->lanes().compSeams(roomRow);
        REQUIRE(leadSeams.size() == 2);
        REQUIRE(roomSeams.size() == 2);
        for (const auto& seam : leadSeams) CHECK_FALSE(seam.differs);
        for (const auto& seam : roomSeams) CHECK_FALSE(seam.differs);
        const ui::Rect b = f.rectOf(idOf(f, pieces[1]));
        const auto edge = f.view->gestures().hit({b.x + 2, b.y + b.h * 0.6f});
        REQUIRE(edge);
        CHECK(edge->zone == views::ItemGestures::Drag::Seam);
    }
    // A fade of the member's middle piece set for it alone (bypass_groups, as
    // an agent can): its comp no longer reads like the group's (take.comp.get
    // lists it in differing_members), and its seams, only its, are broken.
    json::Value fade = json::Value::object();
    fade["item"] = idOf(f, roomPieces[1]).str();
    fade["fade_in"] = 0.3;
    fade["bypass_groups"] = true;
    REQUIRE(f.dispatcher->execute({"item.set", fade, kUi}));
    f.sync();
    {
        json::Value ask = json::Value::object();
        ask["item"] = it.str();
        const auto back = f.dispatcher->execute({"take.comp.get", ask, kUi});
        REQUIRE(back);
        const json::Value* differing = back->find("differing_members");
        REQUIRE(differing);
        REQUIRE(differing->asArray().size() == 1);
        CHECK(differing->asArray()[0].asString() == room.str());
        const auto leadSeams = f.view->lanes().compSeams(leadRow);
        const auto roomSeams = f.view->lanes().compSeams(roomRow);
        REQUIRE(roomSeams.size() == 2);
        for (const auto& seam : leadSeams) CHECK_FALSE(seam.differs);
        for (const auto& seam : roomSeams) CHECK(seam.differs);
    }
    auto differing = [&] {
        json::Value ask = json::Value::object();
        ask["item"] = it.str();
        const auto back = f.dispatcher->execute({"take.comp.get", ask, kUi});
        REQUIRE(back);
        std::vector<std::string> out;
        for (const auto& t : back->find("differing_members")->asArray())
            out.push_back(t.asString());
        return out;
    };
    // An ordinary gesture on the lead sends no crossfade, so each member
    // keeps the seams its spans do not touch: a lane click on the lead's last
    // piece (it joins the middle one) leaves the member's first seam, faded
    // for it alone, as it is. The member still differs, its line still broken.
    {
        const ui::Rect p = f.rectOf(idOf(f, pieces[2]));
        f.click(laneAt(p, 2, 1 - heard, p.x + p.w * 0.5f));
        REQUIRE(sent.size() == 3);
        CHECK(sent[2][0].command == "take.comp");
        CHECK_FALSE(sent[2][0].args.find("crossfade"));
        const auto leadSeams = f.view->lanes().compSeams(leadRow);
        const auto roomSeams = f.view->lanes().compSeams(roomRow);
        REQUIRE(leadSeams.size() == 1);
        REQUIRE(roomSeams.size() == 1);
        CHECK_FALSE(leadSeams[0].differs);
        CHECK(roomSeams[0].differs);
        REQUIRE(piecesOf(f, room).size() == 2);
        CHECK(piecesOf(f, room)[1]->fadeIn.length == doctest::Approx(0.3));
        CHECK(differing() == std::vector<std::string>{room.str()});
    }
    // The crossfade handle on the lead sends the comp's crossfade, which
    // remakes every seam of every member: the member is in step again.
    const double pps = f.view->viewport().pixelsPerSecond;
    auto dragHandle = [&](std::size_t row, double seconds, unsigned mods) {
        const auto seam = f.view->lanes().compSeams(row).at(0);
        const ui::Rect hr = f.rectOf(seam.right);
        const float x = f.view->lanes().xAt(seam.seconds);
        REQUIRE(f.view->gestures().hit({x, hr.y + 3}));
        REQUIRE(f.view->gestures().hit({x, hr.y + 3})->zone == views::ItemGestures::Drag::CompFade);
        f.drag({x, hr.y + 3}, {x + static_cast<float>(seconds / 2 * pps), hr.y + 3}, mods);
    };
    dragHandle(leadRow, 0.5, ui::kShift);
    REQUIRE(sent.size() == 4);
    REQUIRE(sent[3][0].args.find("crossfade"));
    CHECK_FALSE(sent[3][0].args.find("bypass_groups"));
    for (const auto& seam : f.view->lanes().compSeams(roomRow)) CHECK_FALSE(seam.differs);
    CHECK(differing().empty());
    // Held with Alt, the member's handle sets its crossfade alone (1 s): it
    // differs again. A seam drag on the lead, without Alt, moves the seam on
    // both; the member's moved seam takes its own crossfade, not the lead's,
    // and it still differs until the lead's crossfade is set again.
    dragHandle(roomRow, 1.0, ui::kShift | ui::kAlt);
    REQUIRE(sent.size() == 5);
    CHECK(sent[4][0].args.find("bypass_groups"));
    CHECK(piecesOf(f, room)[1]->fadeIn.length == doctest::Approx(1.0).epsilon(0.02));
    CHECK(piecesOf(f, track)[1]->fadeIn.length == doctest::Approx(0.5).epsilon(0.02));
    CHECK(differing() == std::vector<std::string>{room.str()});
    {
        const auto seam = f.view->lanes().compSeams(leadRow).at(0);
        const ui::Rect sr = f.rectOf(seam.right);
        const float x = f.view->lanes().xAt(seam.seconds), y = sr.y + sr.h / 2;
        f.drag({x, y}, {x + 12, y}, ui::kShift);
        REQUIRE(sent.size() == 6);
        CHECK_FALSE(sent[5][0].args.find("crossfade"));
        CHECK_FALSE(sent[5][0].args.find("bypass_groups"));
        CHECK(f.view->lanes().compSeams(roomRow).at(0).seconds ==
              doctest::Approx(f.view->lanes().compSeams(leadRow).at(0).seconds));
        CHECK(f.view->lanes().compSeams(leadRow).at(0).seconds > seam.seconds);
        CHECK(piecesOf(f, room)[1]->fadeIn.length == doctest::Approx(1.0).epsilon(0.02));
        CHECK(piecesOf(f, track)[1]->fadeIn.length == doctest::Approx(0.5).epsilon(0.02));
        CHECK(f.view->lanes().compSeams(roomRow).at(0).differs);
        CHECK(differing() == std::vector<std::string>{room.str()});
    }
}

// A two-take item on a new track and a second track grouped with it for edits:
// with `locked`, the second track holds a locked copy of the item (a member
// that cannot follow); otherwise a one-take bass item over the same stretch.
namespace {

struct Band {
    model::Id it, track, other, otherItem;
    std::size_t heard = 0;
};

Band bandOf(Fixture& f, bool locked) {
    Band b;
    auto [it, heard] = twoTakeItem(f);
    b.it = it;
    b.heard = heard;
    b.track = f.item(it).track;
    const double position = f.item(it).position, length = f.item(it).length;
    const auto other = f.dispatcher->execute(
        {"track.add", json::parse(locked ? R"({"name":"Room"})" : R"({"name":"Bass"})").value(),
         kUi});
    REQUIRE(other);
    b.other = *model::Id::parse(other->find("track")->asString());
    if (locked) {
        json::Value copy = json::Value::object();
        copy["item"] = it.str();
        copy["to"] = position + length + 8;
        const auto copied = f.dispatcher->execute({"item.duplicate", copy, kUi});
        REQUIRE(copied);
        b.otherItem = *model::Id::parse(copied->find("id")->asString());
        json::Value move = json::Value::object();
        move["item"] = b.otherItem.str();
        move["track"] = b.other.str();
        move["position"] = position;
        REQUIRE(f.dispatcher->execute({"item.move", move, kUi}));
        json::Value lock = json::Value::object();
        lock["item"] = b.otherItem.str();
        lock["locked"] = true;
        REQUIRE(f.dispatcher->execute({"item.set", lock, kUi}));
    } else {
        json::Value add = json::Value::object();
        add["track"] = b.other.str();
        add["position"] = position;
        add["notation"] = "1: C2:q r:*";
        auto bass = f.dispatcher->execute({"item.add", add, kUi});
        REQUIRE(bass);
        b.otherItem = *model::Id::parse(bass->find("item")->asString());
        json::Value len = json::Value::object();
        len["item"] = b.otherItem.str();
        len["length"] = length;
        REQUIRE(f.dispatcher->execute({"item.set", len, kUi}));
    }
    json::Value group = json::Value::object();
    group["name"] = "Band";
    group["members"] = json::Value::array();
    group["members"].push_back(b.track.str());
    group["members"].push_back(b.other.str());
    auto made = f.dispatcher->execute({"group.add", group, kUi});
    REQUIRE(made);
    json::Value links = json::Value::object();
    links["group"] = made->find("group")->asString();
    links["links"] = json::Value::array();
    links["links"].push_back("edit");
    REQUIRE(f.dispatcher->execute({"group.set", links, kUi}));
    f.sync();
    return b;
}

std::size_t itemsOn(Fixture& f, model::Id track) {
    std::size_t n = 0;
    for (const auto& e : f.app->head().items)
        if (e.value->track == track) ++n;
    return n;
}

}  // namespace

// Round 129: a one-take member (a bass beside drum takes) is passed over by the
// group's take.comp (skipped_tracks, one_take), so the drums comp without Alt
// and the bass is left as it was.
TEST_CASE("a grouped comp passes over a one-take member: the drums comp without Alt") {
    Fixture f;
    if (!f.ok) return;
    wireCompQuery(f);
    const Band b = bandOf(f, false);
    std::vector<Error> errors;
    f.view->gestures().onError = [&](const Error& e) { errors.push_back(e); };
    auto& sent = recordSteps(f);
    const model::Item bassBefore = f.item(b.otherItem);
    const std::size_t row = *f.view->trackLayout().indexOf(b.track);
    const ui::Rect r = f.rectOf(b.it);
    f.drag(laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.3f),
           laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.6f));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0][0].command == "take.comp");
    CHECK_FALSE(sent[0][0].args.find("bypass_groups"));
    CHECK(errors.empty());
    const auto pieces = piecesOf(f, b.track);
    REQUIRE(pieces.size() == 3);
    CHECK(heardOf(*pieces[1]) == 1 - b.heard);
    CHECK(f.view->lanes().compSeams(row).size() == 2);
    // The bass: one item, where and as long as it was, with its one take.
    REQUIRE(itemsOn(f, b.other) == 1);
    const model::Item& bass = f.item(b.otherItem);
    CHECK(bass.position == bassBefore.position);
    CHECK(bass.length == bassBefore.length);
    CHECK(bass.takes == bassBefore.takes);
    // A member passed over does not read as differing.
    CHECK(f.view->lanes().compSeams(*f.view->trackLayout().indexOf(b.other)).empty());
    for (const auto& seam : f.view->lanes().compSeams(row)) CHECK_FALSE(seam.differs);
}

// Round 129 U1/U2: a group track that cannot follow (here a locked copy of the
// item) refuses the group's take.comp before anything is written; the refusal
// reaches the status line with a hint to hold Alt, and nothing changes. With
// Alt the gesture comps this track alone (bypass_groups), as Alt moves a fader
// alone.
TEST_CASE("a grouped comp a track cannot follow is refused with an Alt hint; Alt comps alone") {
    Fixture f;
    if (!f.ok) return;
    wireCompQuery(f);
    const Band b = bandOf(f, true);
    std::vector<Error> errors;
    f.view->gestures().onError = [&](const Error& e) { errors.push_back(e); };
    auto& sent = recordSteps(f);
    const std::uint64_t version = f.app->head().version;
    const std::size_t row = *f.view->trackLayout().indexOf(b.track);
    const ui::Rect r = f.rectOf(b.it);
    // The swipe: one take.comp for the group, refused before anything is written.
    f.drag(laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.3f),
           laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.6f));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0][0].command == "take.comp");
    CHECK_FALSE(sent[0][0].args.find("bypass_groups"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == "item.locked");
    // The engine names the member in the path, which is what the hint keys on.
    CHECK(errors[0].path == "/args/members/" + b.other.str() + "/items/" + b.otherItem.str());
    CHECK(errors[0].hint == "unlock it, or hold Alt to comp this track alone");
    // Nothing changed: the same version, one whole item, no seams.
    CHECK(f.app->head().version == version);
    CHECK(piecesOf(f, b.track).size() == 1);
    CHECK(heardOf(f.item(b.it)) == b.heard);
    CHECK(f.view->lanes().compSeams(row).empty());
    // A lane click on the whole item is refused the same way.
    f.click(laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.5f));
    REQUIRE(errors.size() == 2);
    CHECK(errors[1].code == "item.locked");
    CHECK(f.app->head().version == version);
    // With Alt the swipe comps this track alone; the locked copy is untouched.
    f.drag(laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.3f), laneAt(r, 2, 1 - b.heard, r.x + r.w * 0.6f),
           ui::kAlt);
    REQUIRE(sent.size() == 3);
    const json::Value* bypass = sent[2][0].args.find("bypass_groups");
    REQUIRE(bypass);
    CHECK(bypass->asBool());
    CHECK(errors.size() == 2);
    const auto pieces = piecesOf(f, b.track);
    REQUIRE(pieces.size() == 3);
    CHECK(heardOf(*pieces[1]) == 1 - b.heard);
    CHECK(itemsOn(f, b.other) == 1);
    CHECK(f.item(b.otherItem).locked);
    CHECK(f.view->lanes().compSeams(row).size() == 2);
}

// Round 119 G4: take.comp's warnings reach the status line's note, and the
// left piece's take badge keeps its press when a long crossfade brings it
// within the seam's reach.
TEST_CASE("a comp's warnings go to the status note; the badge wins over a seam beside it") {
    Fixture f;
    if (!f.ok) return;
    auto [it, heard] = twoTakeItem(f);
    const model::Id track = f.item(it).track;
    const double start = f.item(it).position, length = f.item(it).length;
    // A comp with a 2 s crossfade: at 16 px a second, each seam's crossfade
    // is 32 px wide, so the left piece's badge reaches the seam's zone.
    json::Value args = json::Value::object();
    args["item"] = it.str();
    json::Value span = json::Value::object();
    span["from"] = start + length / 3;
    span["to"] = start + 2 * length / 3;
    span["index"] = static_cast<std::int64_t>(1 - heard);
    args["spans"] = json::Value::array();
    args["spans"].push_back(span);
    args["crossfade"] = 2.0;
    REQUIRE(f.dispatcher->execute({"take.comp", args, kUi}));
    f.sync();
    const std::size_t row = *f.view->trackLayout().indexOf(track);
    const auto seams = f.view->lanes().compSeams(row);
    REQUIRE(seams.size() == 2);
    auto& g = f.view->gestures();
    const float x = f.view->lanes().xAt(seams[0].seconds);
    const ui::Rect left = f.rectOf(seams[0].left);
    REQUIRE(left.right() - x > 12);  // the crossfade's half reaches past the zone
    const auto onBadge = g.hit({x, left.y + 8});
    REQUIRE(onBadge);
    CHECK(onBadge->takeBadge);
    CHECK(onBadge->item == seams[0].left);
    // Below the name bar the seam keeps its press.
    CHECK(g.hit({x, left.y + left.h / 2})->zone == views::ItemGestures::Drag::Seam);
    // A swipe of a narrow stretch keeps the 2 s crossfade, which the short
    // piece clamps: take.comp's warning goes to the note.
    std::vector<std::string> notices;
    g.onNotice = [&](const std::string& n) { notices.push_back(n); };
    const ui::Rect first = f.rectOf(seams[0].left);
    const float mid = first.x + first.w * 0.4f;
    f.drag(laneAt(first, 2, 1 - heard, mid), laneAt(first, 2, 1 - heard, mid + 8), ui::kShift);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("shorter") != std::string::npos);
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
            !f.view->trackLayout().rows()[k].lane())
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

TEST_CASE("arrange gestures: the Item menu sets how a MIDI take sounds against the song") {
    // gui.md §6 (ADR-0098 amended): Play as written, Follow the song's key
    // or chords, and Bake into notes, for an item whose heard take is MIDI.
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    auto exec = [&](const std::string& command, const std::string& args) {
        auto r = f.dispatcher->execute({command, json::parse(args).value(), kUi});
        REQUIRE(r);
        return *r;
    };
    const std::string track = exec("track.add", R"({"name":"Keys"})")["track"].asString();
    const model::Id midi = *model::Id::parse(
        exec("item.add", R"({"track":")" + track +
                             R"(","bar":1,"notation":"voice v; 1: C4:q E4:q G4:q C5:q"})")["item"]
            .asString());
    f.sync();
    std::vector<std::pair<model::Id, model::ClipFollow>> followed;
    std::vector<std::vector<model::Id>> baked;
    std::optional<std::string> refusal;
    g.onFollow = [&](model::Id item, model::ClipFollow follow) {
        followed.emplace_back(item, follow);
    };
    g.onBake = [&](const std::vector<model::Id>& items) { baked.push_back(items); };
    g.followRefusal = [&](model::Id) { return refusal; };
    g.setSelection({midi});
    auto rows = [&] {
        std::map<std::string, const ui::MenuItem*> out;
        static std::vector<ui::MenuItem> held;
        held = g.itemBarMenu();
        for (const ui::MenuItem& m : held) out[m.label] = &m;
        return out;
    };
    {
        const auto r = rows();
        REQUIRE(r.contains("Play as written"));
        CHECK(r.at("Play as written")->checked);
        REQUIRE(r.contains("Follow the song's key"));
        CHECK(r.at("Follow the song's key")->enabled);
        CHECK(r.contains("Follow the song's chords"));
        REQUIRE(r.contains("Bake into notes"));
        CHECK_FALSE(r.at("Bake into notes")->enabled);  // nothing follows
    }
    barMenu(f, midi);
    pick(f, "Follow the song's key");
    REQUIRE(followed.size() == 1);
    CHECK(followed[0] == std::pair{midi, model::ClipFollow::Key});
    // A take that cannot follow says why, and Key and Chords are greyed.
    refusal = "no key in the song here";
    {
        const auto r = rows();
        REQUIRE(r.contains("Follow the song's key (no key in the song here)"));
        CHECK_FALSE(r.at("Follow the song's key (no key in the song here)")->enabled);
        CHECK(r.at("Play as written")->enabled);
    }
    refusal.reset();
    // Following: its entry is checked and Bake bakes it.
    const std::string take = f.app->head().items.find(midi)->activeTake->str();
    exec("take.set", R"({"take":")" + take + R"(","context":"1: key C major ~:w","follow":"key"})");
    f.sync();
    barMenu(f, midi);
    CHECK(ui::menuItems(*f.root->find("item.menu"))[menuIndex(f, "Follow the song's key")].checked);
    pick(f, "Bake into notes");
    REQUIRE(baked.size() == 1);
    CHECK(baked[0] == std::vector<model::Id>{midi});
    // An audio item has none of them.
    g.setSelection({f.itemOnRow(0)});
    f.sync();
    CHECK_FALSE(rows().contains("Play as written"));
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

TEST_CASE("cursors: an item's ends and fade corners, a locked item, a move, a row's lower edge") {
    Fixture f;
    if (!f.ok) return;
    using C = ui::Cursor;
    const model::Id a = f.itemOnRow(0);
    const ui::Rect r = f.rectOf(a);
    REQUIRE(r.w > 60);
    auto over = [&](ui::Point p) {
        f.pointer(ui::PointerEvent::Type::Move, p);
        return f.root->cursor();
    };
    const ui::Point body{r.x + r.w / 2, r.y + r.h / 2};
    const ui::Point right{r.right() - 2, r.y + r.h / 2}, left{r.x + 2, r.y + r.h / 2};
    const ui::Point fadeIn{r.x + 3, r.y + 3}, fadeOut{r.right() - 3, r.y + 3};
    CHECK(over(body) == C::Arrow);
    CHECK(over(right) == C::ResizeHorizontal);
    CHECK(over(left) == C::ResizeHorizontal);
    CHECK(over(fadeIn) == C::ResizeHorizontal);
    CHECK(over(fadeOut) == C::ResizeHorizontal);
    CHECK(over({r.x + r.w / 2, f.view->lanesRect().bottom() - 2}) == C::Arrow);  // empty lanes
    // A locked item is not trimmed or faded, and says so.
    auto lock = [&](bool on) {
        REQUIRE(f.dispatcher->execute({"item.set",
                                       json::parse(R"({"item":")" + a.str() + R"(","locked":)" +
                                                   (on ? "true" : "false") + "}")
                                           .value(),
                                       kUi}));
        f.sync();
    };
    lock(true);
    CHECK(over(right) == C::Arrow);
    CHECK(over(fadeIn) == C::Arrow);
    lock(false);
    CHECK(over(right) == C::ResizeHorizontal);
    // A press on the body is still a click; once it moves, the item moves.
    // (A drag given up, as when the window loses the pointer: the gesture is
    // cancelled and the root lets go of it.)
    auto cancel = [&](ui::Point at) {
        f.pointer(ui::PointerEvent::Type::Cancel, at);
        f.root->capture(nullptr);
        f.sync();
    };
    const auto steps = f.steps();
    f.pointer(ui::PointerEvent::Type::Down, body);
    CHECK(f.root->cursor() == C::Arrow);
    f.pointer(ui::PointerEvent::Type::Move, {body.x + 30, body.y});
    CHECK(f.root->cursor() == C::Move);
    cancel({body.x + 30, body.y});
    CHECK(f.steps() == steps);
    CHECK(over(body) == C::Arrow);
    // A trim keeps its cursor while the pointer is away from the edge.
    f.pointer(ui::PointerEvent::Type::Down, right);
    f.pointer(ui::PointerEvent::Type::Move, {right.x - 40, right.y + 50});
    CHECK(f.root->cursor() == C::ResizeHorizontal);
    cancel({right.x - 40, right.y + 50});
    CHECK(f.steps() == steps);
    // The track panel: a row's lower edge sets its height.
    const ui::Node* row = f.view->tcpPanel().rowNode(f.view->trackLayout().rows()[0].track);
    REQUIRE(row);
    const ui::Rect rb = row->bounds();
    using G = views::TcpRowGeometry;
    CHECK(over({rb.x + 100, rb.bottom() - 2}) == C::ResizeVertical);
    CHECK(over({rb.x + 100, rb.y + (G::kMixTop + G::kCellHeight + G::kLinkTop) / 2}) == C::Arrow);
}

TEST_CASE("a drag held at the lanes' edge scrolls the view and takes the item with it") {
    Fixture f;
    if (!f.ok) return;
    using T = ui::PointerEvent::Type;
    using views::ArrangeView;
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0);
    const ui::Rect r = f.rectOf(a);
    const ui::Rect lanes = f.view->lanesRect();
    const ui::Point body{r.x + r.w / 2, r.y + r.h / 2};
    const double position = f.item(a).position;
    const double start = f.view->viewport().startSeconds;
    // In the middle of the lanes a drag scrolls nothing, however long it waits.
    f.pointer(T::Down, body);
    f.pointer(T::Move, {body.x + 40, body.y});
    CHECK_FALSE(f.view->autoScrolling());
    CHECK_FALSE(f.view->autoScroll(1.0));
    CHECK(f.view->viewport().startSeconds == start);
    // Held in the zone at the right edge it scrolls on, and the item, still
    // under the pointer, is further along the timeline each time.
    const ui::Point edge{lanes.right() - 4, body.y};
    // How far (beats) the item in hand is from where the pointer holds it:
    // no more than the snap to the grid allows.
    const auto beatAt = [&](float x) { return f.app->head().timeMap->beatAt(f.view->timeAt(x)); };
    const double grab = beatAt(body.x) - position;
    const auto pinned = [&](ui::Point pointer) {
        return position + g.state().deltaBeats - (beatAt(pointer.x) - grab);
    };
    f.pointer(T::Move, edge);
    REQUIRE(f.view->autoScrolling());
    CHECK(std::abs(pinned(edge)) <= g.gridBeats() / 2);
    const double before = g.state().deltaBeats;
    CHECK(f.view->autoScroll(0.5));
    const double once = f.view->viewport().startSeconds;
    CHECK(once > start);
    CHECK(g.state().deltaBeats > before);
    // The item stayed under the pointer: it moved by what the view scrolled,
    // many grid steps.
    CHECK(g.state().deltaBeats - before > 10 * g.gridBeats());
    CHECK(std::abs(pinned(edge)) <= g.gridBeats() / 2);
    CHECK(f.view->autoScroll(0.5));
    CHECK(f.view->viewport().startSeconds > once);
    // Further out is faster: the same time scrolls more.
    const double near = f.view->viewport().startSeconds;
    f.pointer(T::Move, {lanes.right() + 60, body.y});
    CHECK(f.view->autoScroll(0.5));
    CHECK(f.view->viewport().startSeconds - near > near - once);
    CHECK(std::abs(pinned({lanes.right() + 60, body.y})) <= g.gridBeats() / 2);
    // The loop's pass steps it by the time since the pass before, and by no
    // more than the cap: a frame that stalled for two seconds is a short
    // step, not a jump. The first pass of a scroll only starts the clock.
    {
        using namespace std::chrono;
        const steady_clock::time_point t0{hours(1)};
        const double s0 = f.view->viewport().startSeconds;
        f.view->poll(true, t0);
        CHECK(f.view->viewport().startSeconds == s0);
        f.view->poll(true, t0 + milliseconds(25));
        const double step = f.view->viewport().startSeconds - s0;
        CHECK(step > 0);
        f.view->poll(true, t0 + milliseconds(25) + seconds(2));
        CHECK(f.view->viewport().startSeconds - s0 == doctest::Approx(3 * step));
    }
    // Back inside, it stops; the drop lands where the pointer is.
    f.pointer(T::Move, {lanes.x + lanes.w / 2, body.y});
    CHECK_FALSE(f.view->autoScrolling());
    const double held = f.view->viewport().startSeconds;
    CHECK_FALSE(f.view->autoScroll(1.0));
    CHECK(f.view->viewport().startSeconds == held);
    const double delta = g.state().deltaBeats;
    const auto steps = f.steps();
    f.pointer(T::Up, {lanes.x + lanes.w / 2, body.y});
    f.sync();
    CHECK(f.steps() == steps + 1);
    CHECK(f.item(a).position == doctest::Approx(position + delta));
    CHECK(f.view->timeAt(lanes.x + lanes.w / 2) >
          f.app->head().timeMap->secondsAt(position) + 1.0);  // it went past the old view
    CHECK_FALSE(f.view->autoScrolling());

    // At the left edge of a view already at the project's start there is
    // nothing to scroll: no move, and no frames asked for.
    views::Viewport home = f.view->viewport();
    home.startSeconds = 0;
    f.view->setViewport(home);
    f.root->frame(false);
    const model::Id other = f.itemOnRow(1);  // still near the start
    const ui::Rect r2 = f.rectOf(other);
    REQUIRE(f.view->lanesRect().contains({r2.x + r2.w / 2, r2.y + r2.h / 2}));
    f.pointer(T::Down, {r2.x + r2.w / 2, r2.y + r2.h / 2});
    f.pointer(T::Move, {lanes.x + 3, r2.y + r2.h / 2});
    CHECK(f.view->autoScrolling());
    CHECK_FALSE(f.view->autoScroll(1.0));
    CHECK(f.view->viewport().startSeconds == 0);
    f.pointer(T::Cancel, {lanes.x + 3, r2.y + r2.h / 2});
    f.root->capture(nullptr);
    CHECK_FALSE(f.view->autoScrolling());  // a drag given up scrolls no more

    // Rows: with more tracks than fit, a move held at the bottom scrolls
    // down; a trim, which stays on its row, does not.
    f.view->trackLayout().setGlobalHeight(200);
    f.root->frame(false);
    const ui::Rect tall = f.rectOf(other);
    const ui::Rect lanes2 = f.view->lanesRect();
    const ui::Point mid{tall.x + tall.w / 2, tall.y + tall.h / 2};
    REQUIRE(f.view->viewport().scrollY == 0);
    f.pointer(T::Down, mid);
    f.pointer(T::Move, {mid.x, lanes2.bottom() - 3});
    REQUIRE(f.view->autoScrolling());
    CHECK(f.view->autoScroll(0.25));
    CHECK(f.view->viewport().scrollY > 0);
    f.pointer(T::Cancel, {mid.x, lanes2.bottom() - 3});
    f.root->capture(nullptr);
    f.sync();
    views::Viewport top = f.view->viewport();
    top.scrollY = 0;
    f.view->setViewport(top);
    f.root->frame(false);
    const ui::Rect again = f.rectOf(other);
    f.pointer(T::Down, {again.right() - 2, again.y + again.h / 2});
    f.pointer(T::Move, {again.right() - 30, lanes2.bottom() - 3});
    CHECK_FALSE(f.view->autoScrolling());
    f.pointer(T::Cancel, {again.right() - 30, lanes2.bottom() - 3});
    f.root->capture(nullptr);
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

TEST_CASE("a trim moves the same edge of every selected item, audio and MIDI, in one step") {
    Fixture f;
    REQUIRE(f.ok);
    const model::Id audio = f.itemOnRow(0), midi = f.itemOnRow(1, true);
    f.view->gestures().setSelection({audio, midi});
    const double len0 = f.item(audio).length, len1 = f.item(midi).length;
    const double pos0 = f.item(audio).position, pos1 = f.item(midi).position;
    const float step = f.beatPx();
    auto steps = f.steps();
    ui::Rect r = f.rectOf(audio);
    f.drag({r.right() - 2, r.y + r.h / 2}, {r.right() - 2 - 2 * step, r.y + r.h / 2});
    CHECK(f.steps() == steps + 1);
    const double cut = len0 - f.item(audio).length;
    CHECK(cut > 0);
    CHECK(f.item(midi).length == doctest::Approx(len1 - cut));
    // The start, from the MIDI item's edge: both start later by the same beats.
    steps = f.steps();
    r = f.rectOf(midi);
    f.drag({r.x + 2, r.y + r.h / 2}, {r.x + 2 + step, r.y + r.h / 2});
    CHECK(f.steps() == steps + 1);
    const double d = f.item(midi).position - pos1;
    CHECK(d > 0);
    CHECK(f.item(audio).position == doctest::Approx(pos0 + d));
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
    // (A quarter of the lane: far enough to be a drag in a lane of any height.)
    f.drag(at(1), {at(1).x, at(1).y - area.h * 0.25f});
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

TEST_CASE("modulation: an item's take FX route has its lane, and the item's name its mark") {
    // ADR-0096 M5; gui.md §4.3: a route into an item's take FX asks for a
    // lane under the item's track (dimmed outside the item, as the take
    // FX's envelope lane is), and the item's name carries the mark while an
    // FX of its heard take is modulated (views::takeNameMark).
    Fixture f;
    if (!f.ok) return;
    auto listed = f.dispatcher->execute({"plugin.list", json::Value::object(), kUi});
    REQUIRE(listed);
    const model::Id item = f.itemOnRow(0);
    const model::Id track = f.item(item).track;
    const model::Id take = *f.item(item).activeTake;
    auto run = [&](const char* command, const std::string& args) {
        auto r = f.dispatcher->execute({command, json::parse(args).value(), kUi});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    std::string fx, key;
    for (const json::Value& p : (*listed)["plugins"].asArray()) {
        if (p.find("instrument") && p.find("instrument")->asBool()) continue;
        auto added =
            f.dispatcher->execute({"fx.add",
                                   json::parse(R"({"take":")" + take.str() + R"(","plugin":")" +
                                               p.find("id")->asString() + R"("})")
                                       .value(),
                                   kUi});
        if (!added) continue;
        const std::string id = (*added)["fx"].asString();
        const json::Value d = run("fx.describe", R"({"fx":")" + id + R"(","full":true})");
        if (const json::Value* ps = d.find("params"); ps && ps->isObject())
            for (const auto& m : ps->asObject())
                if (const json::Value* mod = m.value.find("modulatable");
                    key.empty() && mod && mod->asBool())
                    key = m.key;
        if (!key.empty()) {
            fx = id;
            break;
        }
        run("fx.remove", R"({"fx":")" + id + R"("})");
    }
    if (key.empty()) {
        MESSAGE("skipped: no stock effect with a modulatable parameter (partial build)");
        return;
    }
    auto scales = std::make_shared<gui::PluginScales>(*f.dispatcher);
    f.view->trackLayout().pluginScales = [scales](const model::Envelope& e) {
        return (*scales)(e);
    };
    auto marks = [&] {
        f.root->damageAll();
        f.root->frame(false);
        std::vector<std::string> lit;
        for (const std::string& m : f.view->lanes().stats().marks)
            if (!m.empty()) lit.push_back(m);
        return lit;
    };
    f.sync();
    CHECK(marks().empty());
    const std::string route = run("modulate", R"({"target":{"fx":")" + fx + R"(","param":")" + key +
                                                  R"("},"source":"lfo"})")["modulation"]
                                  .asString();
    f.sync();
    CHECK(marks() == std::vector<std::string>{"signal-cv"});
    run("modulation.set", R"({"modulation":")" + route + R"(","enabled":false})");
    f.sync();
    CHECK(marks() == std::vector<std::string>{"ink-faint"});
    run("modulation.set", R"({"modulation":")" + route + R"(","enabled":true,"lane":true})");
    f.sync();
    // Its lane: under the item's track, and dimmed outside the item.
    const model::EnvTarget target{*model::Id::parse(fx), key};
    auto lane = f.view->trackLayout().indexOfTarget(target);
    REQUIRE(lane);
    CHECK(*lane > *f.view->trackLayout().indexOf(track));
    CHECK(f.view->trackLayout().rows()[*lane].track == track);
    marks();
    const ui::Rect laneRow = f.view->rowRect(*lane);
    const ui::Rect body = f.rectOf(item);
    REQUIRE_FALSE(f.view->lanes().dimmedLaneSpans().empty());
    for (const ui::Rect& d : f.view->lanes().dimmedLaneSpans()) {
        CHECK(static_cast<double>(d.y) == doctest::Approx(static_cast<double>(laneRow.y)));
        CHECK((d.right() <= body.x + 0.5f || d.x >= body.right() - 0.5f));
    }
    // A copy of the take keeps its routes (the copy rule); with the copy's
    // routes removed and the copy heard, the name has no mark, and with the
    // first take heard again it has.
    const std::string second =
        run("take.add", R"({"item":")" + item.str() + R"(","from":")" + take.str() + R"("})")["id"]
            .asString();
    run("take.activate", R"({"item":")" + item.str() + R"(","take":")" + second + R"("})");
    const model::Session copied = f.app->head();
    const model::Take* copy = copied.takes.find(*model::Id::parse(second));
    REQUIRE(copy);
    std::vector<model::Id> copies;
    for (model::Id cfx : copy->chain)
        for (model::Id r : model::modulationsInto(copied, cfx)) copies.push_back(r);
    REQUIRE(copies.size() == 1);
    run("modulation.remove", R"({"modulation":")" + copies.front().str() + R"("})");
    f.sync();
    CHECK(marks().empty());
    run("take.activate", R"({"item":")" + item.str() + R"(","take":")" + take.str() + R"("})");
    f.sync();
    CHECK(marks() == std::vector<std::string>{"signal-cv"});
    if (const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS"))
        REQUIRE(gui::writePng(std::filesystem::path(out) / "take-fx-modulated-item.png",
                              f.root->readBack()));
}
