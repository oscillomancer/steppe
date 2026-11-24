// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launched audio across a trajectory change (S3 step D4, phase 2, the
// mirror): the main thread primes, at prepare, what each launch reader reads
// at the change's switch S, and an executor takes that reading only when it
// is the one it would seek and prime itself. So the run holding S primes
// nothing for it, and what plays is the miss path's bit for bit: each case
// also runs with the mirror off. Its own ctest entry.
#include <functional>
#include <optional>
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
enum class Change { Envelope, FromTape };

struct MirrorRun {
    std::vector<float> heard;
    std::uint32_t latency = 0;
    std::vector<std::pair<std::int64_t, std::uint64_t>> around;  // primes per run about S
    std::int64_t switched = 0;
    std::int64_t S = INT64_MIN;  // the change's switch as prepared, a stream frame
};

// twoTracks' cl_2 (with two tracks, cl_3 on a third track like tr_2 as
// well) launched on bar 2 at 0.75; `afterLaunch` runs right after the
// launch; the change is made at running frame 200064 (S at 209600, the
// callback to 209664).
MirrorRun changeRun(const EngineOptions& options, int tracks, Change kind, bool mirror,
                    const std::function<void(Project&, AppLive&)>& afterLaunch = {}) {
    std::optional<MirrorOff> off;
    if (!mirror) off.emplace();
    Project p;
    twoTracks(p);
    if (tracks == 2) {
        p.run("track.add", R"({"name": "Loop 2"})");  // tr_3, as tr_2
        auto args = j(R"({"track": "tr_3", "scene": "sc_1"})");
        args["audio_file"] = p.ramp("clip3.wav", 48000).string();
        p.runArgs("clip.add", args);  // cl_3
        p.run("fx.add", R"({"track": "tr_3", "plugin": "com.suppleaudio.test.gain"})");
        p.run("fx.patch.merge", R"({"fx": "fx_3", "patch": {"params": {"latency": "short"}}})");
    }
    p.run("transport.playrate.set", kind == Change::FromTape
                                        ? R"({"rate": 0.75, "preserve_pitch": false})"
                                        : R"({"rate": 0.75, "preserve_pitch": true})");
    constexpr std::uint32_t block = 128;
    AppLive live(p, options, block, 48000 * 6 + 16384);
    REQUIRE(p.app->transport("play"));
    live.until(20 * block);
    p.run("launch.clip", R"({"clip": "cl_2"})");
    if (tracks == 2) p.run("launch.clip", R"({"clip": "cl_3"})");
    if (afterLaunch) afterLaunch(p, live);
    live.until(150016);
    MirrorRun out;
    out.around = primesPerRun(live, 150016, 280064, block, [&](std::int64_t f) {
        if (f != 200064) return;
        live.settle();  // no attempt late: the S an unloaded run gets (round 166 T1)
        if (kind == Change::Envelope)
            p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                             R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
        else
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        out.S = preparedSwitchForTests(*p.app->engine());
    });
    CHECK(p.app->engine()->processErrors() == 0);
    const auto log = p.app->engine()->launchLog();
    REQUIRE(log.size() == std::size_t(tracks));
    out.switched = log[0].runningFrame;
    out.heard = live.stop();
    out.latency = live.latency;
    return out;
}

std::uint64_t primes(const MirrorRun& r) {
    std::uint64_t n = 0;
    for (const auto& [frame, count] : r.around) n += count;
    return n;
}

// The first frame where two runs differ, or -1. Two runs whose change
// switched at different frames differ for that alone (review round 166),
// so that is checked first, and said.
std::int64_t firstDifference(const MirrorRun& a, const MirrorRun& b) {
    INFO("switches at " << a.S << " and " << b.S);
    REQUIRE(a.S == b.S);
    if (a.latency != b.latency || a.heard.size() != b.heard.size()) return 0;
    for (std::size_t k = 0; k < a.heard.size(); ++k)
        if (a.heard[k] != b.heard[k]) return std::int64_t(k);
    return -1;
}
}  // namespace

TEST_CASE("the mirror primes a trajectory change's launched audio before its switch" *
          doctest::description("rt")) {
    // A rate envelope added (stretch to stretch), with one launched audio
    // clip and with two, and preserve pitch turned on (tape to stretch): the
    // miss path primes a warm-up's primes (45) per track in the run holding
    // S; with the mirror none there, and the same output.
    struct Case {
        Change kind;
        int tracks;
    };
    for (const Case c :
         {Case{Change::Envelope, 1}, Case{Change::Envelope, 2}, Case{Change::FromTape, 1}})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("from tape " << (c.kind == Change::FromTape) << ", tracks " << c.tracks
                              << ", anticipative " << options.anticipative);
            const MirrorBudget budget(openForRenderer(options));
            const MirrorRun with = changeRun(options, c.tracks, c.kind, true);
            const MirrorRun without = changeRun(options, c.tracks, c.kind, false);
            CHECK(with.around.empty());
            // All of them about S (the renderer's can fall into two of its
            // slices as they are counted here, with the machine's load).
            CHECK(primes(without) == 45u * std::uint64_t(c.tracks));
            if (!options.anticipative) {  // all in the callback holding S
                REQUIRE(without.around.size() == 1);
                CHECK(without.around[0].first == 209664);
            }
            CHECK(firstDifference(with, without) == -1);
        }
}

TEST_CASE("two launches that differ only in their switch's quantum both find the mirror's reading" *
          doctest::description("rt")) {
    // Reader 0's key holds no switch quantum (S3 step D4, phase 2): where a
    // reading began does not change what the reading primed for S gives. A
    // scalar edit (0.8, then back to 0.75) after the launch brings bar 2
    // sooner, into another quantum; both readings are found at S, and each
    // plays what the miss path plays.
    const auto wobble = [](Project& p, AppLive& live) {
        live.settle();
        p.run("transport.playrate.set", R"({"rate": 0.8, "preserve_pitch": true})");
        live.until(60032);
        live.settle();
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    };
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const MirrorBudget budget(openForRenderer(options));
        const MirrorRun plain = changeRun(options, 1, Change::Envelope, true);
        const MirrorRun moved = changeRun(options, 1, Change::Envelope, true, wobble);
        const MirrorRun movedOff = changeRun(options, 1, Change::Envelope, false, wobble);
        INFO("switched at " << plain.switched << " and " << moved.switched);
        CHECK(plain.switched == 128000);
        CHECK(plain.switched / detail::kRateQuantum != moved.switched / detail::kRateQuantum);
        CHECK(plain.around.empty());
        CHECK(moved.around.empty());
        CHECK(firstDifference(moved, movedOff) == -1);
    }
}

TEST_CASE("a warm-up switching at or before a change's switch takes the mirror's reading") {
    // Reader 2 for S (review round 163 S2): a warm-up applied when the
    // change is prepared, switching in [handAt, S], is what reads at S, so
    // the mirror primes reader 2 for S and the carry takes it when its key
    // is the one the carry would seek (the hit), else seeks it unprimed for
    // its first use (the miss). Live, the window is narrow (under 100 frames
    // here, the renderer's: "launches queued across a trajectory change play
    // the same with the mirror", d -30), so the plan-level case pins it.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session before = p.app->head();
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    const model::Session after = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    const auto planOf = [&](const model::Session& s) {
        auto plan = detail::compile(s, pool, 0, 128).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, s));
        return plan;
    };
    for (const bool hit : {true, false}) {
        INFO("hit " << hit);
        auto from = planOf(before);
        auto to = planOf(after);
        REQUIRE(detail::prepareRateSources(from, 0, 0.0, nullptr));
        from.timelines[0]->ensure(160000);
        REQUIRE(detail::configureRateTransition(to, &from, *from.timelines[0], 100000));
        REQUIRE(to.rateControlsChanged);
        const std::int64_t S = to.rateSwitchDevice;
        REQUIRE(S > 100000);
        to.timelines[0]->ensure(S + 20000);
        const auto nodeOf = [](detail::Plan& plan) -> detail::Node& {
            return *std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                 [](const auto& n) { return n.id == id("tr_2"); });
        };
        detail::Node& old = nodeOf(from);
        detail::Node& node = nodeOf(to);
        REQUIRE_FALSE(old.live);
        const int player = detail::playerOf(old, id("cl_2"));
        REQUIRE(player >= 0);
        const std::int64_t warmSwitch = S - 128;  // at or before S: reader 2 reads at S
        constexpr std::int64_t anchor = 96000;
        old.launchWarmValid = true;
        old.launchWarmNext.kind = detail::LaunchState::Kind::Clip;
        old.launchWarmNext.player = player;
        old.launchWarmNext.anchorSample = anchor;
        old.launchWarmAt = warmSwitch;  // no seek: stream and device agree
        detail::PlayerReader& warming = old.launchReaders[2];
        REQUIRE(warming.state);
        warming.player = &old.players[static_cast<std::size_t>(player)];
        warming.anchor = anchor;
        warming.startQuantum = detail::quantumAt(from, warmSwitch);
        warming.primes = 10;
        // The mirror's reading for that warm-up: a hit, or one 64 frames off.
        detail::LaunchMirror m;
        m.track = id("tr_2");
        m.clip = id("cl_2");
        m.anchor = hit ? anchor : anchor + 64;
        m.warmSwitch = warmSwitch;
        detail::primeLaunchMirror(to, {m});
        detail::PlayerReader& r = node.launchReaders[2];
        REQUIRE(r.mirror);
        REQUIRE(r.primes == 45u);
        detail::carryRateSources(from, to, -1);
        CHECK_FALSE(r.mirror);
        CHECK(r.previousLive);  // the warm-up's reading crossfades into it
        CHECK(r.anchor == anchor);
        CHECK(r.startQuantum == detail::quantumAt(to, S));
        // Taken, primed on the main thread; missed, seeked for its first use.
        CHECK(r.primes == (hit ? 45u : 0u));
    }
}

TEST_CASE("a change primed again leaves no earlier attempt's mirror reading to take") {
    // Review round 166: prepareRatePlan primes the mirror at every attempt
    // and as it begins (a late attempt prepares again, a failed one again as
    // a seek); each call's readings replace the last's. A warm-up switching
    // after S is keyed by its own switch, not by S: a reading an earlier
    // call left marked would be taken (all 45 primes) although this call
    // did not prime it. Unmarked, the carry seeks it as far as the warm-up
    // had come (10).
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session before = p.app->head();
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    const model::Session after = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    const auto planOf = [&](const model::Session& s) {
        auto plan = detail::compile(s, pool, 0, 128).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, s));
        return plan;
    };
    auto from = planOf(before);
    auto to = planOf(after);
    REQUIRE(detail::prepareRateSources(from, 0, 0.0, nullptr));
    from.timelines[0]->ensure(160000);
    REQUIRE(detail::configureRateTransition(to, &from, *from.timelines[0], 100000));
    REQUIRE(to.rateControlsChanged);
    const std::int64_t S = to.rateSwitchDevice;
    to.timelines[0]->ensure(S + 20000);
    const auto nodeOf = [](detail::Plan& plan) -> detail::Node& {
        return *std::find_if(plan.nodes.begin(), plan.nodes.end(),
                             [](const auto& n) { return n.id == id("tr_2"); });
    };
    detail::Node& old = nodeOf(from);
    detail::Node& node = nodeOf(to);
    REQUIRE_FALSE(old.live);
    const int player = detail::playerOf(old, id("cl_2"));
    REQUIRE(player >= 0);
    const std::int64_t warmSwitch = S + 2000;  // after S: keyed by its own switch
    constexpr std::int64_t anchor = 96000;
    old.launchWarmValid = true;
    old.launchWarmNext.kind = detail::LaunchState::Kind::Clip;
    old.launchWarmNext.player = player;
    old.launchWarmNext.anchorSample = anchor;
    old.launchWarmAt = warmSwitch;  // no seek: stream and device agree
    detail::PlayerReader& warming = old.launchReaders[2];
    REQUIRE(warming.state);
    warming.player = &old.players[static_cast<std::size_t>(player)];
    warming.anchor = anchor;
    warming.startQuantum = detail::quantumAt(from, warmSwitch);
    warming.primes = 10;
    detail::LaunchMirror m;
    m.track = id("tr_2");
    m.clip = id("cl_2");
    m.anchor = anchor;
    m.warmSwitch = warmSwitch;
    detail::primeLaunchMirror(to, {m});  // an earlier attempt
    detail::PlayerReader& r = node.launchReaders[2];
    REQUIRE(r.mirror);
    REQUIRE(r.primes == 45u);
    detail::primeLaunchMirror(to, {});  // the next, which primes nothing
    CHECK_FALSE(r.mirror);
    detail::carryRateSources(from, to, -1);
    CHECK_FALSE(r.mirror);
    CHECK(r.anchor == anchor);
    CHECK(r.startQuantum == detail::quantumAt(to, warmSwitch));
    CHECK(r.primes == 10u);  // as far as the warm-up had come, not the earlier 45
}

TEST_CASE("the mirror primes within the hand's slack, and what it leaves plays the same" *
          doctest::description("rt")) {
    // Review round 166 F: the first attempt primes reader by reader while
    // the hand stays more than one of the frontier's steps past it (frames,
    // no clock); a reader left is a miss, which its executor primes. Two
    // launched tracks, both readers 0 for S: stopped (the hook) after 0, 1
    // and all of them, the executors prime the rest, 45 each, and every
    // sample is the miss path's at the same S. With the engine's own slack:
    // the callback's hand is its render-ahead past what has played, room
    // for both; a renderer caught up leaves it under a render block past
    // its frontier, so none (the executors prime both, as with it off).
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const MirrorRun without = changeRun(options, 2, Change::Envelope, false);
        REQUIRE(primes(without) == 90u);
        for (const int k : {0, 1, kAllReaders}) {
            INFO("budget " << k);
            const MirrorBudget budget(k);
            const MirrorRun with = changeRun(options, 2, Change::Envelope, true);
            CHECK(primes(with) == 45u * std::uint64_t(2 - std::min(k, 2)));
            CHECK(firstDifference(with, without) == -1);
        }
        const MirrorRun own = changeRun(options, 2, Change::Envelope, true);
        CHECK(primes(own) == (options.anticipative ? 90u : 0u));
        CHECK(firstDifference(own, without) == -1);
    }
}

TEST_CASE("the mirror primes the reading heard first first") {
    // Review round 166 F: what a tight budget leaves to the executors is
    // predictable: readings by the device frame they are first read (S,
    // or a warm-up's own switch after S), then as given. Given a warm-up
    // switching after S before reader 0's reading for S, a budget of one
    // primes reader 0.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session before = p.app->head();
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    const model::Session after = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    const auto planOf = [&](const model::Session& s) {
        auto plan = detail::compile(s, pool, 0, 128).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, s));
        return plan;
    };
    auto from = planOf(before);
    auto to = planOf(after);
    REQUIRE(detail::prepareRateSources(from, 0, 0.0, nullptr));
    from.timelines[0]->ensure(160000);
    REQUIRE(detail::configureRateTransition(to, &from, *from.timelines[0], 100000));
    REQUIRE(to.rateControlsChanged);
    const std::int64_t S = to.rateSwitchDevice;
    to.timelines[0]->ensure(S + 20000);
    detail::Node& node = *std::find_if(to.nodes.begin(), to.nodes.end(),
                                       [](const auto& n) { return n.id == id("tr_2"); });
    detail::LaunchMirror later;  // a warm-up switching after S: reader 2
    later.track = id("tr_2");
    later.clip = id("cl_2");
    later.anchor = 96000;
    later.warmSwitch = S + 2000 - to.rateOrigin;  // a stream frame
    detail::LaunchMirror atS = later;             // what reads at S: reader 0
    atS.warmSwitch = INT64_MIN;
    int asked = 0;
    detail::primeLaunchMirror(to, {later, atS}, [&] { return asked++ < 1; });
    CHECK(asked == 2);  // asked again before the second, refused
    CHECK(node.launchReaders[0].mirror);
    CHECK_FALSE(node.launchReaders[2].mirror);
    // With room for both, both, in the same order.
    asked = 0;
    detail::primeLaunchMirror(to, {later, atS}, [&] { return ++asked > 0; });
    CHECK(asked == 2);
    CHECK(node.launchReaders[0].mirror);
    CHECK(node.launchReaders[2].mirror);
}
