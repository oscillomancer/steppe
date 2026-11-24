// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launched audio across a trajectory change (S3 step D4, phase 1): a plan
// that changes the trajectory hands each launch reader's reading on as the
// new reader's previous, on the old trajectory, crossfaded at the switch
// into the same reading on the new one, primed on the audio thread within
// a warm-up's primes. Its own ctest entry.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
enum class Change { Envelope, ToTape, FromTape };
struct ChangeRun {
    std::vector<float> heard;
    std::uint32_t latency = 0;
};
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
// The loudest sample over [a, b) of the stream.
double loudest(const ChangeRun& r, std::int64_t a, std::int64_t b) {
    double m = 0;
    for (std::int64_t k = a; k < b; ++k)
        m = std::max(m, std::fabs(double(r.heard[static_cast<std::size_t>(k + r.latency)])));
    return m;
}
// The largest step between neighbouring samples over [a, b) of the stream.
double largestStep(const ChangeRun& r, std::int64_t a, std::int64_t b) {
    double m = 0;
    for (std::int64_t k = a; k < b; ++k) {
        const auto i = static_cast<std::size_t>(k + r.latency);
        m = std::max(m, std::fabs(double(r.heard[i + 1]) - double(r.heard[i])));
    }
    return m;
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

TEST_CASE("a trajectory change primes launched audio within a warm-up's primes per callback" *
          doctest::description("rt")) {
    // Phase 1 of S3 step D4: the new reading is primed on the audio thread,
    // at its first use, a warm-up's primes (45 at 48 kHz) at most; a warm-up
    // spreads its own over its frames.
    for (const bool change : {false, true}) {
        INFO("change " << change);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        constexpr std::uint32_t block = 128;
        AppLive live(p, kInCallback, block, 48000 * 6 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * block);
        p.run("launch.clip", R"({"clip": "cl_2"})");
        std::uint64_t most = 0;
        for (std::int64_t c = 21; c < 300000 / block; ++c) {
            if (change && c == 200000 / block)
                p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                                 R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
            const std::uint64_t before = detail::stretchPrimes();
            live.until(c * block);
            most = std::max(most, detail::stretchPrimes() - before);
        }
        CHECK(p.app->engine()->processErrors() == 0);
        live.stop();
        CHECK(most <= 45);
        if (change) CHECK(most == 45);  // the new reading, primed in one callback
    }
}

TEST_CASE("a launch queued across a trajectory change" * doctest::description("rt")) {
    // cl_2 plays; cl_3 is queued for bar 3 (running frame 256000), and a rate
    // envelope is added so that the change's switch S lands `d` frames
    // before bar 3 (the change reaches S about 9600 frames after the edit
    // here, a little later in the anticipative executor).
    // - d <= 0: cl_3 switches first, warmed on the old trajectory, then
    //   crossfades into the new one at S.
    // - d > 0 in the anticipative executor: its warm-up is under way when
    //   the change is adopted, so it starts again on the new trajectory and
    //   still switches at bar 3.
    // - d > 0 in the callback: the change voids the launch, not yet applied,
    //   and placement keeps it clear of the pending change: it lands a bar
    //   later (the app's service places it again).
    struct Case {
        bool anticipative;
        std::int64_t d;
        double beat;  // where it lands: bar 3, or bar 4
    };
    for (const Case c :
         {Case{false, -2000, 8}, Case{false, -300, 8}, Case{true, -2000, 8}, Case{true, -300, 8},
          Case{true, 200, 8}, Case{true, 2500, 8}, Case{false, 2500, 12}}) {
        INFO("anticipative " << c.anticipative << ", d " << c.d);
        Project p;
        twoTracks(p);
        auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
        args["audio_file"] = p.ramp("second.wav", 48000).string();
        p.runArgs("clip.add", args);  // cl_3
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        constexpr std::uint32_t block = 128;
        const EngineOptions options = c.anticipative ? EngineOptions{.workers = 0} : kInCallback;
        AppLive live(p, options, block, 48000 * 9 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * block);
        p.run("launch.clip", R"({"clip": "cl_2"})");
        live.until(200000);
        p.run("launch.clip", R"({"clip": "cl_3"})");  // bar 3
        const std::int64_t lag = c.anticipative ? 9793 : 9601;
        live.until(256000 - lag - c.d);
        p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                         R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
        for (std::int64_t f = 256000 - lag - c.d + 1024; f < 420000; f += 1024) {
            live.until(f);
            p.app->engine()->service();  // as the app's tick
        }
        const auto log = p.app->engine()->launchLog();
        CHECK(p.app->engine()->processErrors() == 0);
        ChangeRun run;
        run.heard = live.stop();
        run.latency = live.latency;
        REQUIRE(log.size() == 2);
        CHECK(log[1].clip == id("cl_3"));
        CHECK(log[1].anchorBeat == c.beat);
        if (c.beat == 8) CHECK(log[1].runningFrame == 256000);  // not moved
        const std::int64_t lands = log[1].runningFrame;
        CHECK(loudest(run, lands + 3000, lands + 30000) > 0.01);  // cl_3 plays
        if (c.d <= 0) {
            const std::int64_t S = 256000 - c.d;
            CHECK(largestStep(run, S - 300, S + 3000) <=
                  1.5 * largestStep(run, S + 20000, S + 40000));
        }
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
