// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launched audio across a trajectory change (S3 step D4, phase 1): a plan
// that changes the trajectory hands each launch reader's reading on as the
// new reader's previous, on the old trajectory, crossfaded at the switch
// into the same reading on the new one, primed on the audio thread within
// a warm-up's primes. Its own ctest entry.
#include <functional>
#include <limits>
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
enum class Change { Envelope, ToTape, FromTape };
ChangeRun changeRun(const EngineOptions& options, std::uint32_t block, Change kind, bool change,
                    std::int64_t at) {
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", kind == Change::FromTape
                                        ? R"({"rate": 0.75, "preserve_pitch": false})"
                                        : R"({"rate": 0.75, "preserve_pitch": true})");
    AppLive live(p, options, block, 48000 * 6 + 16384);
    REQUIRE(p.app->transport("play"));
    live.until(20 * 128);
    p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 2
    live.until(at);
    if (change) switch (kind) {
            case Change::Envelope:
                p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                                 R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
                break;
            case Change::ToTape:
                p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
                break;
            case Change::FromTape:
                p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
                break;
        }
    live.until(at + 80000);
    CHECK(p.app->engine()->processErrors() == 0);
    ChangeRun out;
    out.heard = live.stop();
    out.latency = live.latency;
    return out;
}
}  // namespace

TEST_CASE("launched audio stays continuous across a change of the trajectory" *
          doctest::description("rt")) {
    // cl_2 plays from bar 2; at running frame 200000 a compiled change
    // (a rate envelope; preserve pitch off; preserve pitch on) reaches it.
    // Before step D4 the readers restarted cold where they played: a click
    // of about ten times the material's own step, stretch to stretch and
    // stretch to tape. Now the old reading plays to the switch and
    // crossfades into the new one.
    struct Case {
        Change kind;
        std::uint32_t block;
    };
    for (const Case c :
         {Case{Change::Envelope, 128}, Case{Change::ToTape, 128}, Case{Change::FromTape, 128},
          Case{Change::Envelope, 32}, Case{Change::Envelope, 512}})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("change " << int(c.kind) << ", block " << c.block << ", anticipative "
                           << options.anticipative);
            const ChangeRun same = changeRun(options, c.block, c.kind, false, 200000);
            const ChangeRun moved = changeRun(options, c.block, c.kind, true, 200000);
            std::int64_t first = -1;
            for (std::int64_t k = 0; k < 280000; ++k)
                if (same.heard[std::size_t(k + same.latency)] !=
                    moved.heard[std::size_t(k + moved.latency)]) {
                    first = k;
                    break;
                }
            REQUIRE(first >= 200000);  // nothing changes before the change
            const double atSwitch = largestStep(moved, first - 200, first + 3000);
            const double own = std::max(largestStep(same, first - 200, first + 3000),
                                        largestStep(moved, first + 20000, first + 40000));
            INFO("first change at " << first << ", largest step there " << atSwitch
                                    << ", the material's own " << own);
            CHECK(atSwitch <= 1.5 * own);
            CHECK(loudest(moved, first + 3000, first + 40000) > 0.01);  // it plays on
        }
}

namespace {
// The primes of every callback over (from, to] that primed, by the frame it
// played to; `at(frame)` runs before each.
using Primes = std::vector<std::pair<std::int64_t, std::uint64_t>>;
Primes primesPerCallback(AppLive& live, std::int64_t from, std::int64_t to, std::uint32_t block,
                         const std::function<void(std::int64_t)>& at) {
    Primes out;
    for (std::int64_t c = from / block + 1; c <= to / block; ++c) {
        at(c * block);
        const std::uint64_t before = detail::stretchPrimes();
        live.until(c * block);
        if (const std::uint64_t d = detail::stretchPrimes() - before; d > 0)
            out.push_back({c * block, d});
    }
    return out;
}
void addEnvelope(Project& p) {
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
}
}  // namespace

TEST_CASE("a trajectory change primes launched audio within a warm-up's primes per track" *
          doctest::description("rt")) {
    // Phase 1 of S3 step D4: each track's new reading is primed on the audio
    // thread at its first use, in the callback holding the switch S (209600,
    // the callback to 209664): a warm-up's primes (45 at 48 kHz) per track
    // with a launched audio clip, so two such tracks take 90 there. The cold
    // start before D4 primed as many, in the same callback; what D4 buys
    // with them is a continuous switch, checked here too. Without a change
    // only cl_2's warm-up primes, two per callback at most.
    for (const int tracks : {0, 1, 2}) {  // 0: no change, one track
        INFO("tracks " << tracks);
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
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        constexpr std::uint32_t block = 128;
        AppLive live(p, kInCallback, block, 48000 * 6 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * block);
        p.run("launch.clip", R"({"clip": "cl_2"})");
        if (tracks == 2) p.run("launch.clip", R"({"clip": "cl_3"})");
        const Primes warm = primesPerCallback(live, 20 * block, 150016, block, [](std::int64_t) {});
        const Primes around = primesPerCallback(live, 150016, 280064, block, [&](std::int64_t f) {
            if (tracks > 0 && f == 200064) addEnvelope(p);
        });
        CHECK(p.app->engine()->processErrors() == 0);
        const std::vector<float> heard = live.stop();
        for (const auto& [frame, primes] : warm) CHECK(primes <= 2u * std::max(tracks, 1));
        if (tracks == 0) {
            CHECK(around.empty());
            continue;
        }
        REQUIRE(around.size() == 1);
        CHECK(around[0].first == 209664);
        CHECK(around[0].second == 45u * std::uint64_t(tracks));
        ChangeRun run;
        run.heard = heard;
        run.latency = live.latency;
        const std::int64_t S = 209600;
        // The material's own step just before S (earlier, the clips' own loop
        // restarts step further).
        CHECK(largestStep(run, S - 200, S + 3000) <= 1.5 * largestStep(run, S - 3000, S - 200));
    }
}

TEST_CASE("launches after a trajectory change still warm up" * doctest::description("rt")) {
    // Round 152 F1: the reader that held the clip playing at the change kept
    // its previous reading's flag, and two launches later, back on reader 2,
    // it warmed each later launch into that previous reading on the old
    // trajectory: those launches started cold, 45 primes in their switch's
    // callback. Three launches after an envelope edit now warm up, two primes
    // a callback at most, after the change's own 45 at S.
    Project p;
    twoTracks(p);
    auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("second.wav", 48000).string();
    p.runArgs("clip.add", args);  // cl_3
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    constexpr std::uint32_t block = 128;
    AppLive live(p, kInCallback, block, 48000 * 16 + 16384);
    REQUIRE(p.app->transport("play"));
    live.until(20 * block);
    p.run("launch.clip", R"({"clip": "cl_2"})");
    live.until(150016);
    const char* order[] = {"cl_3", "cl_2", "cl_3"};
    std::size_t pressed = 0;
    const Primes primes = primesPerCallback(live, 150016, 600064, block, [&](std::int64_t f) {
        if (f == 200064) addEnvelope(p);
        if (f % 1024 == 0) p.app->engine()->service();
        // Each pressed once the one before has switched (a bar each).
        if (f > 215000 && pressed < 3 && p.app->engine()->launchLog().size() == pressed + 1)
            p.run("launch.clip", std::string(R"({"clip": ")") + order[pressed++] + R"("})");
    });
    const auto log = p.app->engine()->launchLog();
    CHECK(p.app->engine()->processErrors() == 0);
    live.stop();
    REQUIRE(log.size() == 4);
    for (std::size_t k = 1; k < 4; ++k) CHECK(log[k].clip == id(order[k - 1]));
    for (const auto& [frame, n] : primes) {
        INFO("primes " << n << " in the callback to " << frame);
        if (frame == 209664)
            CHECK(n == 45);  // the change's own, at S
        else
            CHECK(n <= 2);
    }
}

TEST_CASE("a warming launch placed while a trajectory change is pending waits for its crossfade" *
          doctest::description("rt")) {
    // S3 step D4: from the change's adoption until the callback has played
    // its switch S and the crossfade, an audio launch placed then switches
    // at least the crossfade plus its warm-up after S, so it warms on the new
    // trajectory alone. cl_3 (unquantized) is pressed just before S.
    Project p;
    twoTracks(p);
    auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("second.wav", 48000).string();
    p.runArgs("clip.add", args);  // cl_3
    p.run("clip.set", R"({"clip": "cl_3", "quantize": "none"})");
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    constexpr std::uint32_t block = 128;
    AppLive live(p, kInCallback, block, 48000 * 6 + 16384);
    REQUIRE(p.app->transport("play"));
    live.until(20 * block);
    p.run("launch.clip", R"({"clip": "cl_2"})");
    live.until(200000);
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    // The change's switch, on the 64-frame grid (the continuity case sees
    // its first changed sample one frame later, where the crossfade starts).
    const std::int64_t S = 200000 + 9600;
    const std::int64_t warm = 2944, smooth = 480;  // launchWarm and smoothFrames at 48 kHz
    // Pressed as soon as placement takes it (launch.later until the
    // executors run the new controls), which must be inside the crossfade.
    std::int64_t pressed = -1;
    for (std::int64_t f = S - 2048; f < S + smooth && pressed < 0; f += block) {
        live.until(f);
        p.app->engine()->service();
        if (p.bus->execute({"launch.clip", j(R"({"clip": "cl_3"})"), kOrigin})) pressed = f;
    }
    INFO("pressed at " << pressed);
    REQUIRE(pressed >= 0);  // before S + smoothFrames
    live.until(260000);
    const auto log = p.app->engine()->launchLog();
    CHECK(p.app->engine()->processErrors() == 0);
    live.stop();
    REQUIRE(log.size() == 2);
    CHECK(log[1].clip == id("cl_3"));
    CHECK(log[1].runningFrame >= S + smooth + warm);
    CHECK(log[1].runningFrame < S + smooth + warm + 2 * block);
}

TEST_CASE("the old trajectory is kept through an outgoing launch's crossfade at the slowest rate") {
    // Round 145 A1: an outgoing reading may read the old trajectory through
    // its switch crossfade, 240 song samples, 960 frames at 0.25, past S;
    // rateSafeDevice keeps the next change from being adopted until then,
    // when either plan has launched audio under stretch (a switch to tape
    // included), and stays at the items' crossfade otherwise.
    Project p;
    twoTracks(p);
    const auto session = [&](const char* rate) {
        p.run("transport.playrate.set", rate);
        return p.app->head();
    };
    const model::Session kept = session(R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session tape = session(R"({"rate": 0.75, "preserve_pitch": false})");
    const model::Session tape2 = session(R"({"rate": 0.8, "preserve_pitch": false})");
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session enveloped = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    const auto planOf = [&](const model::Session& s) {
        auto plan = detail::compile(s, pool, 0, 128).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, s));
        return plan;
    };
    struct Case {
        const model::Session* from;
        const model::Session* to;
        bool stretched;  // launched audio under stretch on either side
    };
    for (const Case c : {Case{&kept, &tape, true}, Case{&kept, &enveloped, true},
                         Case{&tape, &kept, true}, Case{&tape, &tape2, false}}) {
        auto from = planOf(*c.from);
        auto to = planOf(*c.to);
        REQUIRE(detail::prepareRateSources(from, 0, 0.0, nullptr));
        from.timelines[0]->ensure(40000);
        REQUIRE(detail::configureRateTransition(to, &from, *from.timelines[0], 20000));
        REQUIRE(to.rateControlsChanged);
        const std::int64_t fade = 240;  // launchFadeFrames at 48 kHz
        const std::int64_t keep = c.stretched ? fade * 4 : to.smoothFrames;
        INFO("from preserve " << from.masterPlayrate.preservePitch << " to preserve "
                              << to.masterPlayrate.preservePitch);
        CHECK(to.rateSafeDevice == to.rateSwitchDevice + keep + to.rateSourceBehind + 64);
    }
}

TEST_CASE("a reading handed on from a tape reader takes its switch's loop cycle") {
    // Round 152 F2 and its re-check S1: a looped tape plan (song 0-20000,
    // 26667 frames a cycle at 0.75) whose tr_2 plays cl_2 from a switch at
    // frame 30000, in loop cycle 1, is carried to a stretched plan. The new
    // reading on reader 0 keeps cycle 1, from the switch's phase recorded in
    // its block (a tape reader keeps no cycle: 0 before). Also a switch made
    // on a plan without a playrate (N3: no phase recorded), the frame read
    // back while the timeline holds it.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    const model::Session tape = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session kept = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    const auto planOf = [&](const model::Session& s) {
        auto plan = detail::compile(s, pool, 0, 128).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, s));
        return plan;
    };
    for (const bool recorded : {true, false}) {
        INFO("phase recorded at the switch " << recorded);
        auto from = planOf(tape);
        auto to = planOf(kept);
        const LoopRange loop{0, 20000, true};
        REQUIRE(detail::prepareRateSources(from, 0, 0.0, &loop));
        from.timelines[0]->ensure(60000);
        REQUIRE(from.timelines[0]->cycle(from.timelines[0]->phase(30000)) == 1);
        REQUIRE(detail::configureRateTransition(to, &from, *from.timelines[0], 36000));
        REQUIRE(to.rateControlsChanged);
        const auto nodeOf = [](detail::Plan& plan) -> detail::Node& {
            return *std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                 [](const auto& n) { return n.id == id("tr_2"); });
        };
        detail::Node& old = nodeOf(from);
        detail::Node& node = nodeOf(to);
        REQUIRE_FALSE(old.live);
        const int player = detail::playerOf(old, id("cl_2"));
        REQUIRE(player >= 0);
        old.launch.kind = detail::LaunchState::Kind::Clip;
        old.launch.player = player;
        old.launch.anchorSample = 96000;
        old.launch.fadeFrom = -2;
        old.launchSwitchAt = 30000;
        old.launchSwitchPhase = recorded ? from.timelines[0]->absolutePhase(30000)
                                         : std::numeric_limits<double>::quiet_NaN();
        detail::PlayerReader& reading = old.launchReaders[0];
        REQUIRE(reading.state);
        REQUIRE_FALSE(reading.state->preserve);  // a tape reader
        reading.player = &old.players[static_cast<std::size_t>(player)];
        reading.anchor = 96000;
        REQUIRE(node.launchReaders[0].state);
        REQUIRE(node.launchReaders[0].state->preserve);
        detail::carryRateSources(from, to, -1);
        CHECK(node.launchReaders[0].previousLive);  // the tape reading, crossfaded from
        CHECK(node.launchReaders[0].launchCycle == 1);
    }
}

TEST_CASE("a launched clip restarting cold just before its first wrap keeps the switch's cycle" *
          doctest::description("rt")) {
    // S3 step D's cold start takes the loop cycle of the launch's switch,
    // which matters while the launch is still anchored: before its first
    // transport wrap (each wrap unanchors it), where the converter reads
    // ahead into the next cycle, before the anchor, and plays that only in
    // a later cycle than the switch's. The cycle was read back from the
    // timeline at the switch frame, a ring of about 11 s at 48 kHz: a cold
    // restart about one ring after the switch, with the frame that overwrote
    // the switch's slot already past the wrap, took cycle 1 for the switch,
    // and the reader (keyed, not reseeked when the launch unanchors) read
    // silence for the whole of cycle 1 before the anchor. The phase is now
    // recorded at the switch (Node::launchSwitchPhase).
    // cl_2 switches at running frame 128000 (bar 2 at 0.75); the loop is
    // chosen from the ring so that the first wrap lands 500 frames before
    // the switch plus one ring; cl_2 is muted (unread, so its converter falls
    // behind its catch-up) and unmuted so that it restarts cold about 1500
    // frames before the wrap (the unmute reaches the reader 9632 frames
    // later in the callback, 9824 in the anticipative executor). A guard
    // checks the restart came before the wrap, by the frame its burst of
    // primes (45) was for, recorded with each prime (stretchPrimeFrame), so
    // it does not depend on how far ahead the anticipative renderer runs
    // (review round 156 F1).
    std::size_t ring = 0;
    {
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        const model::Session session = p.app->head();
        InstancePool pool = makePool();
        MediaCache media;
        auto plan = detail::compile(session, pool, 0, 128).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, session));  // makes the timelines
        REQUIRE(plan.timelines[0]);
        ring = plan.timelines[0]->capacity();
    }
    constexpr std::int64_t anchor = 128000;  // the switch, and the anchor in each cycle
    const std::int64_t wrap = (anchor + static_cast<std::int64_t>(ring) - 500) / 4 * 4;
    const std::int64_t songFrames = wrap * 3 / 4;  // the loop, at 0.75
    INFO("ring " << ring << ", first wrap at " << wrap);
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const std::int64_t unmute = wrap - 1500 - (options.anticipative ? 9824 : 9632);
        Project p;
        twoTracks(p);
        p.run("track.set", R"({"track": "tr_1", "mute": true})");  // cl_2 alone is heard
        auto loop = j(R"({"start_seconds": 0, "enabled": true})");
        loop["end_seconds"] = double(songFrames) / 48000.0;
        p.runArgs("transport.loop", loop);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        AppLive live(p, options, 128, 48000 * 16 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 2
        live.until(unmute - 20000);
        p.run("clip.set", R"({"clip": "cl_2", "mute": true})");
        live.until(unmute);
        p.run("clip.set", R"({"clip": "cl_2", "mute": false})");
        // The cold restart's burst of primes: the first 45 after the unmute.
        const std::uint64_t start = detail::stretchPrimes();
        live.until(wrap + 70000);
        const std::uint64_t primes = detail::stretchPrimes() - start;
        const auto log = p.app->engine()->launchLog();
        CHECK(p.app->engine()->processErrors() == 0);
        ChangeRun run;
        run.heard = live.stop();
        run.latency = live.latency;
        REQUIRE(log.size() == 1);
        CHECK(log[0].runningFrame == anchor);
        // One reading's burst (its primes all for the frame it restarts at),
        // still in the ring, before the wrap: the launch still anchored.
        REQUIRE(primes >= 45);
        REQUIRE(primes < 256);
        const std::int64_t restart = detail::stretchPrimeFrame(start + 1);
        INFO("the restart's reading starts at " << restart << ", " << primes << " primes");
        CHECK(detail::stretchPrimeFrame(start + 45) == restart);
        CHECK(restart > unmute);
        CHECK(restart < wrap);
        CHECK(loudest(run, unmute - 40000, unmute - 25000) > 0.01);  // it played before the mute
        // After the wrap, in cycle 1 before the anchor, it plays.
        CHECK(loudest(run, wrap + 3000, wrap + 60000) > 0.01);
    }
}

TEST_CASE("after a seek a warm-up switching after a trajectory change warms on the new one") {
    // S3 step D4 and the warm-up's frame: a warm-up's switch (launchWarmAt)
    // is a stream frame, S a device frame, apart by rateOrigin after a seek
    // (72000 here). Compared unconverted, a warm-up switching 2000 frames
    // after S read as switching first: handed on as the previous plan's
    // reading and seeked for S (and warmLaunch, by the same comparison,
    // went on warming it on the old trajectory).
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session before = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.8, "preserve_pitch": true})");
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
    constexpr std::int64_t origin = 72000;  // device minus stream, after the seek
    auto from = planOf(before);
    auto to = planOf(after);
    from.rateOrigin = origin;
    to.rateOrigin = origin;
    REQUIRE(detail::prepareRateSources(from, origin, 54000.0, nullptr));
    from.timelines[0]->ensure(origin + 80000);
    REQUIRE(detail::configureRateTransition(to, &from, *from.timelines[0], origin + 36000));
    REQUIRE(to.rateControlsChanged);
    const std::int64_t S = to.rateSwitchDevice;
    REQUIRE(S > origin + 36000);
    to.timelines[0]->ensure(S + 20000);
    const auto nodeOf = [](detail::Plan& plan) -> detail::Node& {
        return *std::find_if(plan.nodes.begin(), plan.nodes.end(),
                             [](const auto& n) { return n.id == id("tr_2"); });
    };
    detail::Node& old = nodeOf(from);
    detail::Node& node = nodeOf(to);
    REQUIRE_FALSE(old.live);
    REQUIRE(old.timelineShift == 0);
    const int player = detail::playerOf(old, id("cl_2"));
    REQUIRE(player >= 0);
    const std::int64_t warmSwitch = S + 2000;  // a device frame, after S
    old.launchWarmValid = true;
    old.launchWarmNext.kind = detail::LaunchState::Kind::Clip;
    old.launchWarmNext.player = player;
    old.launchWarmNext.anchorSample = 96000;
    old.launchWarmAt = warmSwitch - origin;  // the slot's target, a stream frame
    detail::PlayerReader& warming = old.launchReaders[2];
    REQUIRE(warming.state);
    REQUIRE(warming.state->preserve);
    // Its warm-up under way: seeked for its switch, ten primes done.
    warming.player = &old.players[static_cast<std::size_t>(player)];
    warming.anchor = 96000;
    warming.startQuantum = detail::quantumAt(from, warmSwitch);
    warming.primes = 10;
    detail::carryRateSources(from, to, -1);
    detail::PlayerReader& r = node.launchReaders[2];
    CHECK_FALSE(r.previousLive);  // not the previous plan's reading: it switches after S
    CHECK(r.startQuantum == detail::quantumAt(to, warmSwitch));
    CHECK(r.primes == 10);  // as far as it had come
}
