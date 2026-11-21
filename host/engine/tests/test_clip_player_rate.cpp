// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's players under a master playrate (ADR-0097, dated
// revision 2026-11-21; S3): split from test_clip_player.cpp so each runs as
// its own ctest entry (batch 3's Windows run timed the two together out).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("an audio clip's launch is refused while the master playrate keeps the pitch") {
    // S3 step 1: launched audio plays by tape only. A MIDI clip launches.
    Project p;
    twoTracks(p);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    p.run("transport.playrate.set", R"({"rate": 0.5, "preserve_pitch": true})");
    REQUIRE(p.app->engine()->awaitPlan());
    auto r = p.bus->execute({"launch.clip", j(R"({"clip": "cl_2"})"), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.playrate");
    CHECK(r.error().message.find("cl_2") != std::string::npos);
    CHECK(p.bus->execute({"launch.clip", j(R"({"clip": "cl_1"})"), kOrigin}));
    // Without the pitch kept it launches.
    p.run("transport.playrate.set", R"({"rate": 0.5, "preserve_pitch": false})");
    REQUIRE(p.app->engine()->awaitPlan());
    backend.run(40);  // (playing: the new plan's clock reads)
    CHECK(p.bus->execute({"launch.clip", j(R"({"clip": "cl_2"})"), kOrigin}));
    p.app->stopEngine();
}

TEST_CASE("a looped replay is refused under a master playrate") {
    // The replay wraps as the live executors do only on the unity path.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.5})");
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = 2.0;
    settings.projectDir = p.app->projectDir();
    settings.loop = std::pair{0.5, 1.5};
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "render.loop");
    settings.applyMasterPlayrate = false;  // the unity path loops
    CHECK(renderOffline(p.app->head(), pool, settings));
}

// --- Launches under a master playrate (ADR-0097, dated revision 2026-11-21) ---

TEST_CASE("launches under a master playrate play as the offline replay, in both executors" *
          doctest::description("rt")) {
    // twoTracks' scene launch, then tr_1 back to the arrangement and tr_2
    // stopped, under a constant 0.75 and under an envelope. At 0.75 bar 2
    // (song 96000) is heard at running frame 128000, bar 3 at 256000.
    for (const bool envelope : {false, true}) {
        Project p;
        twoTracks(p);
        underRate(p, envelope);
        const model::Session s = p.app->head();
        // What is compared (300000 frames, past the last switch at 256000)
        // and room for the output latency: no more is played.
        const std::int64_t frames = 300000 + 16384;
        Replays replays;
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                // Both block sizes under the constant rate; the envelope, whose
                // trajectory is what it adds, at one.
                if (envelope && block != 128) continue;
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(128000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
                REQUIRE(live.log.size() == 4);
                CHECK(live.log[0].anchorSample == 96000);  // bar 2
                CHECK(live.log[1].anchorSample == 96000);  // one target for the scene
                CHECK(live.log[2].anchorSample == 192000);
                if (!envelope) {
                    CHECK(live.log[0].runningFrame == 128000);
                    CHECK(live.log[2].runningFrame == 256000);
                }
                liveIsReplay(s, p.app->projectDir(), live, 5.0, 300000, replays);
            }
    }
}

TEST_CASE("a launch under a master playrate switches where the song reaches its boundary") {
    // The anchor is the first frame whose song position reaches the bar:
    // on the rate timeline the frame before is short of it. And it moves
    // with a scalar edit made while it waits.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.6, "preserve_pitch": false})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5;
    // 0.6: bar 2 (song 96000) at device 160000 exactly.
    const Live plain = playLive(s, p.app->projectDir(), kInCallback, 128, frames,
                                {{20, {request("tr_1", "cl_1")}}});
    REQUIRE(plain.log.size() == 1);
    CHECK(plain.log[0].anchorSample == 96000);
    CHECK(plain.log[0].runningFrame == 160000);
    // A scalar edit to 1.2 well before then: the launch is placed again on
    // the new trajectory, still on bar 2, much earlier.
    p.run("transport.playrate.set", R"({"rate": 1.2, "preserve_pitch": false})");
    const model::Session faster = p.app->head();
    const Live edited = playLive(s, p.app->projectDir(), kInCallback, 128, frames,
                                 {{20, {request("tr_1", "cl_1")}},
                                  {200, {}, {}, &faster},
                                  {210, {}}});  // (a service, which places it again)
    REQUIRE(edited.log.size() == 1);
    CHECK(edited.log[0].anchorSample == 96000);
    CHECK(edited.log[0].runningFrame < 140000);
    CHECK(edited.log[0].runningFrame > 200 * 128);
}

TEST_CASE("a launch made while stopped under a new playrate starts at the play start" *
          doctest::description("rt")) {
    // The builder's second song journey, D1: with the engine running and the
    // transport stopped, a playrate set and then a scene launched answered
    // beat 0 but played from bar 2. Stopped, a launch targets the play start
    // whatever the rate (as at 1.0), and after a count-in too (review round
    // 81 E3: its seek skipped the re-place).
    for (const auto [rate, countIn] : {std::pair{1.25, false}, {0.8, false}, {1.25, true}})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("rate " << rate << ", count-in " << countIn << ", anticipative "
                         << options.anticipative);
            Project p;
            twoTracks(p);
            const model::Session unity = p.app->head();
            json::Value set = j(R"({"preserve_pitch": false})");
            set["rate"] = rate;
            p.runArgs("transport.playrate.set", set);
            const model::Session rated = p.app->head();
            constexpr std::uint32_t block = 128;
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
            REQUIRE(engine.load(unity, p.app->projectDir()));
            const auto run = [&](std::int64_t cycles) {
                if (options.anticipative)
                    runAheadOnState(engine, backend, cycles, block);
                else
                    backend.run(cycles);
                engine.service();
            };
            run(10);
            REQUIRE(engine.load(rated, p.app->projectDir()));  // stopped
            run(10);
            const std::vector<Engine::LaunchRequest> scene{request("tr_1", "cl_1"),
                                                           request("tr_2", "cl_2")};
            auto placed = engine.launch(scene, model::LaunchQuantize::Bar);
            REQUIRE_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
            CHECK(placed->anchorSample == 0);
            run(10);  // still stopped: it stays queued for the play start
            if (countIn)
                engine.playCountIn(4800, 1200, 4);  // the transport is held for 0.1 s
            else
                engine.play();
            run(200);
            const auto log = engine.launchLog();
            REQUIRE(log.size() == 2);
            for (const LaunchLogEntry& e : log) {
                CHECK(e.anchorSample == 0);
                CHECK(e.runningFrame == 0);
            }
            engine.stop();
        }
}

TEST_CASE(
    "a playrate set while stopped is the rate the play starts at, and capture judges the "
    "audible rate" *
    doctest::description("rt")) {
    // The builder's second song journey, D2 (and F8): played at 1.25,
    // stopped, set to 1.0 and played again, the effective rate stayed 1.25
    // while stopped and glided for about 1.5 s after play, and a capture
    // armed at once was accepted and laid its sections off the bar lines.
    // Stopped, nothing is heard: the set rate is the one the play starts
    // at, and the effective rate says so; the edit keeps the plan, as any
    // scalar edit. Stopped means the transport was asked to stop, also
    // before a callback has heard it (review round 81 E4). Playing, a
    // capture is refused while the audible rate is not 1.
    for (const bool atOnce : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("edit at once " << atOnce << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            const auto rated = [&](double rate) {
                json::Value set = j(R"({"preserve_pitch": false})");
                set["rate"] = rate;
                p.runArgs("transport.playrate.set", set);
                return p.app->head();
            };
            const model::Session fast = rated(1.25);
            const model::Session unity = rated(1.0);
            constexpr std::uint32_t block = 128;
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
            REQUIRE(engine.load(fast, p.app->projectDir()));
            const auto run = [&](std::int64_t cycles) {
                if (options.anticipative)
                    runAheadOnState(engine, backend, cycles, block);
                else
                    backend.run(cycles);
                engine.service();
            };
            engine.play();
            run(400);
            CHECK(engine.effectivePlayrate() == 1.25);
            engine.pause();  // transport.stop: the transport stops and returns to its start
            REQUIRE(engine.prepareSeek(0.0));
            if (!atOnce) run(10);
            const auto generation = engine.planGeneration();
            REQUIRE(engine.load(unity, p.app->projectDir()));
            CHECK(engine.planGeneration() == generation);
            run(10);
            CHECK(engine.effectivePlayrate() == 1.0);  // stopped: what the play starts at
            engine.play();
            run(2);
            CHECK(engine.effectivePlayrate() == 1.0);
            CHECK(engine.captureLaunches(true));
            CHECK(engine.captureLaunches(false));
            // Playing: a change of rate glides, and while it is audible a
            // capture is refused.
            REQUIRE(engine.load(fast, p.app->projectDir()));
            for (int i = 0; i < 2000 && engine.effectivePlayrate() != 1.25; ++i) run(1);
            REQUIRE(engine.effectivePlayrate() == 1.25);
            REQUIRE(engine.load(unity, p.app->projectDir()));
            run(2);
            REQUIRE(engine.effectivePlayrate() != 1.0);
            const Status gliding = engine.captureLaunches(true);
            REQUIRE_FALSE(gliding);
            CHECK(gliding.error().code == "launch.playrate");
            for (int i = 0; i < 2000 && engine.effectivePlayrate() != 1.0; ++i) run(1);
            CHECK(engine.effectivePlayrate() == 1.0);
            CHECK(engine.captureLaunches(true));
            engine.stop();
        }
}

TEST_CASE("a launch while a playrate change is in flight says so, and lands once it is not") {
    // Review round 78 K1: right after a rate edit reaches the engine, and
    // before the executors take it, placement answers launch.later with the
    // rate's own message, not the relocation one.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.6, "preserve_pitch": false})");
    const model::Session slow = p.app->head();
    // A rate envelope changes the trajectory through a plan handover (a
    // scalar alone is published to the running plan at once).
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 1}, {"beat": 8, "value": 0.6}]})");
    const model::Session fast = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(slow, p.app->projectDir()));
    engine.play();
    backend.run(20);
    engine.service();
    REQUIRE(engine.load(fast, p.app->projectDir()));
    const std::vector<Engine::LaunchRequest> one{request("tr_1", "cl_1")};
    auto inFlight = engine.launch(one, model::LaunchQuantize::Bar);
    REQUIRE_FALSE(inFlight);
    CHECK(inFlight.error().code == "launch.later");
    CHECK(inFlight.error().message == "the master playrate is changing");
    CHECK(inFlight.error().hint == "launch again in a moment");
    // Once the executors take the new plan, it is placed.
    auto placed = engine.launch(one, model::LaunchQuantize::Bar);
    for (int cycle = 0; cycle < 2000 && !placed; ++cycle) {
        CHECK(placed.error().code == "launch.later");
        backend.run(1);
        engine.service();
        placed = engine.launch(one, model::LaunchQuantize::Bar);
    }
    REQUIRE_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
    engine.stop();
}

TEST_CASE("launch.capture is refused under a master playrate, and one becoming active ends it") {
    Project p;
    twoTracks(p);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    // Armed at unity, running: a playrate turned on ends it.
    p.run("launch.capture", R"({"on": true})");
    p.run("transport.play", "{}");
    backend.run(40);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    REQUIRE(p.app->engine()->awaitPlan());
    backend.run(40);
    p.app->engine()->service();
    auto capture = p.app->engine()->launchCapture();
    REQUIRE(capture);
    CHECK_FALSE(capture->on);
    CHECK(capture->frames > 0);
    // Now refused at arming.
    auto r = p.bus->execute({"launch.capture", j(R"({"on": true})"), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.playrate");
    CHECK(r.error().hint.find("then capture") != std::string::npos);
    p.app->stopEngine();
}

TEST_CASE("under a master playrate a switch crossfades over 5 ms as heard, in device frames") {
    // At 0.75 bar 2 (song 96000) is device frame 128000. The crossfade runs
    // over the next 240 device frames (180 song samples), the arrangement's
    // ramp out and the clip's in, each read through the tape converter.
    Project p;
    auto args = j(R"({"track": "tr_1", "bar": 1})");
    args["audio_file"] = p.ramp("item.wav", 240000).string();
    p.runArgs("item.add", args);
    args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("clip.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    LaunchLogEntry e = launch(128000, "cl_1");
    e.anchorSample = 96000;
    e.anchorBeat = 4;
    const auto out = p.render(2.5, {e});
    const auto plain = p.render(2.5, {});
    REQUIRE(out.size() > 130000);
    for (std::int64_t d = 120000; d < 128000; d += 97)
        REQUIRE(out[std::size_t(d)] == plain[std::size_t(d)]);
    for (std::int64_t k = 60; k < kFade; k += 30) {
        const double x = double(k) / double(kFade);
        const double clip = double(rampAt(static_cast<std::int64_t>(0.75 * double(k))));
        const double want = double(plain[std::size_t(128000 + k)]) * (1 - x) + clip * x;
        REQUIRE_MESSAGE(double(out[std::size_t(128000 + k)]) == doctest::Approx(want).epsilon(0.01),
                        "frame " << 128000 + k);
    }
    // Past it, the clip alone, its phase the song's from the anchor.
    CHECK(double(out[128000 + 400]) == doctest::Approx(double(rampAt(300))).epsilon(0.01));
}

TEST_CASE("under a slow master playrate a launch's first frame plays the note on its anchor") {
    // At 0.25 the frame before a switch can already be past the anchor (an
    // unquantized launch's anchor is the floor's song sample, rounded down):
    // the switch's first frame reaches back to it, so the clip's first note
    // sounds. Song 96000 + 0.5 at device frame 384002.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.25, "preserve_pitch": false})");
    LaunchLogEntry e = launch(384002, "cl_1");
    e.anchorSample = 96000;
    e.anchorBeat = 4;
    const auto out = p.render(2.5, {e});
    REQUIRE(out.size() > 384002 + 40000);
    // Half a second of C3 (131 Hz: about 131 zero crossings; the
    // arrangement's A3 there would give 220), before E3 an eighth (48000
    // frames) later.
    int crossings = 0;
    double peak = 0;
    for (std::size_t i = 384002 + 500; i < 384002 + 24500; ++i) {
        peak = std::max(peak, std::fabs(double(out[i])));
        if ((out[i] < 0) != (out[i + 1] < 0)) ++crossings;
    }
    CHECK(peak > 0.05);
    CHECK(crossings > 110);
    CHECK(crossings < 150);
}

TEST_CASE("under a master playrate a clip take's FX envelope plays in clip time (C2)") {
    // At 0.75 each 12000-sample pass takes 16000 device frames. The gain
    // envelope runs 0 to -24 dB over the pass, by the clip position heard,
    // never by the rate timeline.
    Project p;
    latentClip(p);
    p.run("env.add", R"({"fx": "fx_1", "param": "gain", "points": [{"seconds": 0, "value": 0},
                                                                  {"seconds": 0.25, "value": -24}]})");
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    LaunchLogEntry e = launch(128000, "cl_1");
    e.anchorSample = 96000;
    e.anchorBeat = 4;
    const auto x = p.render(3.0, {e});
    REQUIRE(x.size() > 128000 + 32000);
    const auto ratio = [&](std::int64_t k) {
        const auto c = static_cast<std::int64_t>(0.75 * double(k)) % 12000;
        return double(x[std::size_t(128000 + k)]) / double(rampAt(c));
    };
    CHECK(ratio(16000 + 800) > 0.7);    // clip position 600: about -1 dB
    CHECK(ratio(16000 + 15333) < 0.1);  // 11500: about -23 dB
    CHECK(ratio(16000 + 15333) > 0.05);
}
