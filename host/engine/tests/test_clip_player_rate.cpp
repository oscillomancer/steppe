// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's players under a master playrate (ADR-0097, dated
// revision 2026-11-21; S3): split from test_clip_player.cpp so each runs as
// its own ctest entry (batch 3's Windows run timed the two together out).
// The capture-commit cases that run long live are in
// test_clip_player_rate_commit.cpp, _armed.cpp, _chain.cpp and _wrap.cpp
// (CI 45118821222).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE(
    "an audio clip launches while the master playrate keeps the pitch; one with take FX is "
    "refused") {
    // S3 step D: launched audio reads through a stretch converter warmed up
    // before its switch. A clip read through its take chain cannot yet
    // (launch.playrate); without the pitch kept it launches.
    Project p;
    twoTracks(p);
    p.run("scene.add", "{}");  // sc_3
    auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("chained.wav", 48000).string();
    const std::string chained = p.runArgs("clip.add", args)["clip"].asString();
    auto fx = j(R"({"plugin": "com.suppleaudio.test.gain"})");
    fx["take"] = takeOf(p, chained.c_str());
    p.runArgs("fx.add", fx);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    p.run("transport.playrate.set", R"({"rate": 0.5, "preserve_pitch": true})");
    REQUIRE(p.app->engine()->awaitPlan());
    CHECK(p.bus->execute({"launch.clip", j(R"({"clip": "cl_2"})"), kOrigin}));
    backend.run(40);  // (it started the transport: playing, the clock reads)
    CHECK(p.bus->execute({"launch.clip", j(R"({"clip": "cl_1"})"), kOrigin}));
    json::Value launch = json::Value::object();
    launch["clip"] = chained;
    auto r = p.bus->execute({"launch.clip", launch, kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.playrate");
    CHECK(r.error().message.find(chained) != std::string::npos);
    CHECK(r.error().message.find("take FX") != std::string::npos);
    // Without the pitch kept it launches.
    p.run("transport.playrate.set", R"({"rate": 0.5, "preserve_pitch": false})");
    REQUIRE(p.app->engine()->awaitPlan());
    backend.run(40);  // (playing: the new plan's clock reads)
    CHECK(p.bus->execute({"launch.clip", launch, kOrigin}));
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

TEST_CASE("launched audio under a pitch-keeping playrate plays as the offline replay" *
          doctest::description("rt")) {
    // S3 step D: twoTracks' scene on bar 2 (tr_2's audio clip cl_2), then
    // tr_1 back to the arrangement and tr_2 switched to another audio clip
    // (cl_3) on bar 3, and tr_2 stopped on bar 4, under a pitch-keeping 0.75
    // and envelope. Each launched clip's stretch converter warms up from the
    // quantum its switch falls in, the same live and offline, so live is the
    // replay bit for bit; and it is not the tape reading.
    for (const bool envelope : {false, true}) {
        Project p;
        twoTracks(p);
        auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
        args["audio_file"] = p.ramp("other.wav", 96000).string();
        p.runArgs("clip.add", args);  // cl_3
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        if (envelope)
            p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                             R"({"beat": 0, "value": 1}, {"beat": 8, "value": 0.6}]})");
        const model::Session s = p.app->head();
        const std::int64_t frames = 420000 + 16384;
        Replays replays;
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                if (envelope && block != 128) continue;
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(128000 + 4000) / block, {backTo("tr_1"), request("tr_2", "cl_3")}},
                    {(256000 + 4000) / block, {stop}}};
                const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
                REQUIRE(live.log.size() == 5);
                CHECK(live.log[1].anchorSample == 96000);
                CHECK(live.log[3].anchorSample == 192000);
                CHECK(live.log[4].anchorSample == 288000);
                liveIsReplay(s, p.app->projectDir(), live, 9.0, 420000, replays);
            }
        // Not the tape reading of the same log.
        if (!envelope) {
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 9.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = replays.rendered.front().first;
            auto kept = renderOffline(s, pool, settings);
            REQUIRE(kept);
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
            auto tape = renderOffline(p.app->head(), pool, settings);
            REQUIRE(tape);
            double differ = 0;
            for (std::size_t k = 130000; k < 250000; ++k)
                differ = std::max(differ, std::fabs(double(kept->audio.channels[0][k]) -
                                                    double(tape->audio.channels[0][k])));
            CHECK(differ > 1e-3);
        }
    }
}

TEST_CASE(
    "a launched audio clip's converter warms up a prime per 64 frames, and cold gives the same" *
    doctest::description("rt")) {
    // S3 step D, Q5 and round 139 R1/R4: from launchWarm before the switch
    // the next launch's stretch converter (reader 2) seeks and primes no
    // more than once per 64 frames played, at any block size (32 here),
    // is ready by the switch, and comes in on reader 0 with its history. A
    // launch it was not warmed for starts cold from the switch's quantum,
    // all at once, and plays the same bit for bit; also where a loop wrap
    // falls between that quantum's start and the switch, each taking the
    // switch frame's loop cycle: the song loops 96000..120010, at 0.75
    // wrapping at device frame 160014, and reaches an anchor at 96005 at
    // 160020, both in the quantum from 160000; read in the cycle before,
    // the five frames before the anchor would sound (round 139 R4).
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    // `lose`: the node frame where reader 0 loses its converter (as a plan
    // that did not carry it), restarting cold.
    const auto play = [&](std::uint32_t block, bool warm, bool looped,
                          std::int64_t lose = INT64_MAX) {
        InstancePool pool = makePool();
        MediaCache media;
        auto plan = detail::compile(s, pool, 0, block).value();
        plan.media = &media;
        plan.projectDir = p.app->projectDir();
        REQUIRE(detail::prepare(plan, s));
        const LoopRange loop{96000, 120010, looped};
        REQUIRE(detail::prepareRateSources(plan, 0, 0.0, &loop));  // as the engine does
        REQUIRE(detail::primeRateSources(plan, 0));
        REQUIRE(plan.rateStretch);
        for (auto* instance : plan.instances) REQUIRE(instance->startProcessing());
        auto& node = *std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                   [](const auto& n) { return n.id == id("tr_2"); });
        REQUIRE(node.timelineShift == 0);  // (tr_2's latent gain shifts tr_1)
        REQUIRE(node.launchReaders[2].state);
        const int lookahead = node.launchReaders[2].state->stretch->lookahead();
        const auto quanta = static_cast<std::uint32_t>((lookahead + 63) / 64);
        CHECK(node.launchWarm == (quanta + 1) * 64);  // the lookahead's quanta and one to spare
        detail::LaunchAction a;
        a.kind = detail::LaunchState::Kind::Clip;
        a.player = detail::playerOf(node, id("cl_2"));
        REQUIRE(a.player >= 0);
        a.anchorSample = looped ? 96005 : 96000;  // bar 2, heard at 128000 at 0.75
        a.anchorBeat = 4;
        const std::int64_t at = looped ? 160020 : 128000;
        if (looped) CHECK(detail::quantumAt(plan, at) == 160000);
        const std::int64_t warmAt = at - node.launchWarm;
        const std::int64_t end = at + 8192;
        const auto& master = plan.nodes[plan.mainMaster].post;
        std::vector<float> out;
        std::uint32_t primes = 0;
        const detail::RateClipState* warmed = nullptr;
        for (std::int64_t pos = 0; pos < end;) {
            if (warm && pos == warmAt) {
                CHECK(node.launchReaders[2].startQuantum == INT64_MIN);  // nothing before
                detail::prewarm(node, a, at);
            }
            if (pos == at) {
                if (warm) {
                    const detail::PlayerReader& r = node.launchReaders[2];
                    REQUIRE(r.state->primed);  // ready by the switch
                    CHECK(r.primes == quanta);
                    CHECK(r.startQuantum == detail::quantumAt(plan, at));
                    warmed = r.state.get();
                }
                detail::applyLaunch(node, a);
                if (warm) CHECK(node.launchReaders[0].state.get() == warmed);  // with its history
            }
            if (pos == lose) node.launchReaders[0].player = nullptr;
            std::int64_t next = std::min<std::int64_t>(pos + block, end);
            for (const std::int64_t stop : {warmAt, at, lose})
                if (pos < stop && stop < next) next = stop;
            auto n = static_cast<std::uint32_t>(next - pos);
            n = detail::rateUntil(plan, pos, n, loop, -1);  // no block crosses the wrap
            REQUIRE(detail::processBlock(plan, pos, n, true, &loop));
            out.insert(out.end(), master.l.begin(), master.l.begin() + n);
            pos += n;
            if (warm && pos > warmAt && pos <= at) {
                const detail::PlayerReader& r = node.launchReaders[2];
                CHECK(r.primes <= std::uint32_t(pos - warmAt) / 64);  // R1: a prime per 64 frames
                primes = r.primes;
            }
        }
        if (warm && lose == INT64_MAX) {
            CHECK(primes == quanta);
            CHECK(node.launchReaders[0].state.get() == warmed);
        }
        for (auto* instance : plan.instances) instance->stopProcessing();
        return out;
    };
    for (const bool looped : {false, true})
        for (const std::uint32_t block : {32u, 128u}) {
            INFO("looped " << looped << ", block " << block);
            const std::vector<float> warm = play(block, true, looped);
            const std::vector<float> cold = play(block, false, looped);
            REQUIRE(warm.size() == cold.size());
            double worst = 0;
            for (std::size_t k = 0; k < warm.size(); ++k)
                worst = std::max(worst, std::fabs(double(warm[k]) - double(cold[k])));
            CHECK(worst == 0.0);
        }
    // The catch-up: a converter lost 10 quanta after the switch restarts
    // from the switch's quantum and runs the quanta since at once, so it
    // plays on bit for bit; lost 40 quanta after (past the 32 caught up),
    // it starts at its own quantum, and differs from there.
    const std::vector<float> reference = play(128, true, false);
    const auto differs = [&](const std::vector<float>& v) {
        REQUIRE(v.size() == reference.size());
        double worst = 0;
        for (std::size_t k = 0; k < v.size(); ++k)
            worst = std::max(worst, std::fabs(double(v[k]) - double(reference[k])));
        return worst;
    };
    CHECK(differs(play(128, true, false, 128000 + 10 * 64)) == 0.0);
    CHECK(differs(play(128, true, false, 128000 + 40 * 64)) > 0.0);
}

TEST_CASE("a plan swap or a rate change during a launch's warm-up" * doctest::description("rt")) {
    // Round 139: a swap that keeps the rate (a track added) inside cl_2's
    // warm-up hands reader 2 over mid-warm-up, and the launch still plays
    // as the replay; a rate change there (0.75 to 0.8, the pitch kept) loses
    // it, and the launch still switches and plays, cold.
    for (const bool rate : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("rate change " << rate << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            AppLive live(p, options, 128, 48000 * 6 + 16384);
            REQUIRE(p.app->transport("play"));
            live.until(20 * 128);
            p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 2: running frame 128000
            live.until(128000 - 1500);                    // inside its warm-up (from 125056)
            if (rate)
                p.run("transport.playrate.set", R"({"rate": 0.8, "preserve_pitch": true})");
            else
                p.run("track.add", R"({"name": "Empty"})");
            live.until(250000);
            const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
            REQUIRE(log.size() == 1);
            CHECK(log[0].anchorSample == 96000);
            const std::int64_t switched = log[0].runningFrame;
            const std::vector<float> heard = live.stop();
            double loudest = 0;
            for (std::int64_t k = switched + 2000; k < switched + 40000; ++k)
                loudest =
                    std::max(loudest, std::fabs(double(heard[std::size_t(k + live.latency)])));
            CHECK(loudest > 0.01);  // cl_2's ramp plays
            if (!rate) {
                CHECK(switched == 128000);
                const std::vector<float> offline = replay(p, 6.0, log, 128);
                const auto [worst, first] = compare(heard, live.latency, offline, 0, 240000);
                CHECK(worst == 0.0);
                CHECK(first == -1);
            }
        }
}

TEST_CASE("under a pitch-keeping rate only an audio clip's launch waits for a warm-up" *
          doctest::description("rt")) {
    // Round 139 R3: on tr_2, which holds an audio clip, a MIDI clip and a
    // stop launched unquantized land where they would on the tape path; an
    // audio clip lands launchWarm (2944 frames at 48 kHz) later.
    Project p;
    twoTracks(p);
    p.run("scene.add", "{}");  // sc_2
    const std::string midi = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                               R"("notation": "voice v; 1: C4:w"})")["clip"]
                                 .asString();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    const model::Session tape = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session kept = p.app->head();
    const auto placed = [&](const model::Session& s, const Engine::LaunchRequest& r) {
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.play();
        backend.run(20);
        engine.service();
        auto at = engine.launch(std::span(&r, 1), model::LaunchQuantize::None);
        REQUIRE_MESSAGE(at, (at ? std::string() : at.error().toString()));
        engine.stop();
        return at->stream;
    };
    Engine::LaunchRequest stop = backTo("tr_2");
    stop.action = LaunchLogEntry::Action::Stop;
    Engine::LaunchRequest notes = request("tr_2", "cl_2");
    notes.clip = id(midi.c_str());
    CHECK(placed(kept, notes) == placed(tape, notes));
    CHECK(placed(kept, stop) == placed(tape, stop));
    CHECK(placed(kept, backTo("tr_2")) == placed(tape, backTo("tr_2")));
    CHECK(placed(kept, request("tr_2", "cl_2")) == placed(tape, request("tr_2", "cl_2")) + 2944);
}

TEST_CASE("a launched audio clip's converter carries over a plan swap under a pitch-keeping rate" *
          doctest::description("rt")) {
    // S3 step D: a plan swap that leaves the rate alone (a track added) hands
    // the launched clip's stretch converter to the new plan with its
    // history, so what plays is still the replay bit for bit; restarted cold
    // it would differ from the swap on.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        AppLive live(p, options, 128, 48000 * 7 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 2: running frame 128000
        live.until(128000 + 24000);
        p.run("track.add", R"({"name": "Empty"})");  // a new plan, the same rate
        live.until(300000);
        const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
        REQUIRE(log.size() == 1);
        const std::vector<float> heard = live.stop();
        const std::vector<float> offline = replay(p, 7.0, log, 128);
        const auto [worst, first] = compare(heard, live.latency, offline, 0, 290000);
        CHECK(worst == 0.0);
        CHECK(first == -1);
    }
}

TEST_CASE("a launch made during another's warm-up lands after it, and plays as the replay" *
          doctest::description("rt")) {
    // S3 step D: from launchWarm before its switch a launch is applied (its
    // converter warming on reader 2), so a later launch on the track does
    // not replace it. Reader 2 warms one launch at a time: a launch made
    // inside that warm-up is placed no earlier than launchWarm after the
    // first's switch, so the first still comes in warm. Unquantized it would
    // otherwise land within it (about 127000 + 128 + 2944). Then cl_2 again
    // on bar 4. Live is the replay throughout.
    Project p;
    twoTracks(p);
    auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("other.wav", 96000).string();
    p.runArgs("clip.add", args);  // cl_3
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 460000 + 16384;
    constexpr std::int64_t kWarm = 2944;  // at 48 kHz (the plan-level case above)
    Replays replays;
    for (const std::uint32_t block : {32u, 128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            // Blocks under a quantum (round 139) in the callback executor.
            if (block == 32 && options.anticipative) continue;
            INFO("block " << block << ", anticipative " << options.anticipative);
            const std::vector<Step> steps{
                {2560 / block, {request("tr_2", "cl_2")}},
                {(128000 - 1000) / block, {request("tr_2", "cl_3")}, model::LaunchQuantize::None},
                {(256000 + 4000) / block, {request("tr_2", "cl_2")}}};
            const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
            REQUIRE_MESSAGE(live.log.size() == 3, describe(live.log) << live.report);
            CHECK(live.log[0].runningFrame == 128000);
            CHECK(live.log[1].clip == id("cl_3"));
            CHECK(live.log[1].runningFrame >= 128000 + kWarm);
            CHECK(live.log[2].runningFrame == 384000);  // bar 4
            liveIsReplay(s, p.app->projectDir(), live, 10.0, 460000, replays);
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
    // 81 E3: its seek skipped the re-place). With the pitch kept, tr_2's
    // audio clip has no frames before the play start to warm its converter
    // in: it starts there cold, on time (S3 step D).
    struct Run {
        double rate;
        bool countIn, preserve;
    };
    for (const auto [rate, countIn, preserve] : {Run{1.25, false, false},
                                                 {0.8, false, false},
                                                 {1.25, true, false},
                                                 {0.8, false, true},
                                                 {1.25, true, true}})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("rate " << rate << ", count-in " << countIn << ", preserve " << preserve
                         << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            const model::Session unity = p.app->head();
            json::Value set = json::Value::object();
            set["rate"] = rate;
            set["preserve_pitch"] = preserve;
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
    "a playrate set while stopped is the rate the play starts at, and a capture may start while "
    "the rate glides" *
    doctest::description("rt")) {
    // The builder's second song journey, D2 (and F8): played at 1.25,
    // stopped, set to 1.0 and played again, the effective rate stayed 1.25
    // while stopped and glided for about 1.5 s after play, and a capture
    // armed at once was accepted and laid its sections off the bar lines.
    // Stopped, nothing is heard: the set rate is the one the play starts
    // at, and the effective rate says so; the edit keeps the plan, as any
    // scalar edit. Stopped means the transport was asked to stop, also
    // before a callback has heard it (review round 81 E4). Playing, a
    // capture was refused while the audible rate was not 1; it runs in song
    // time now, so arming while the rate glides is allowed (S3 B).
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
            // Playing: a change of rate glides, and a capture may start
            // while it does.
            REQUIRE(engine.load(fast, p.app->projectDir()));
            for (int i = 0; i < 2000 && engine.effectivePlayrate() != 1.25; ++i) run(1);
            REQUIRE(engine.effectivePlayrate() == 1.25);
            REQUIRE(engine.load(unity, p.app->projectDir()));
            run(2);
            REQUIRE(engine.effectivePlayrate() != 1.0);
            CHECK(engine.captureLaunches(true));
            CHECK(engine.launchCapturing());
            for (int i = 0; i < 2000 && engine.effectivePlayrate() != 1.0; ++i) run(1);
            CHECK(engine.effectivePlayrate() == 1.0);
            CHECK(engine.launchCapturing());
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

TEST_CASE("a once audio clip that has finished does not refuse or end a pitch-keeping capture" *
          doctest::description("rt")) {
    // Round 113 T4: cl_2 played once (0.75 s from bar 2, song 96000 to
    // 132000) is silent after its pass, so a plan that keeps the pitch
    // arriving then neither ends a running capture nor refuses arming.
    Project p;
    twoTracks(p);
    p.run("clip.set", R"({"clip": "cl_2", "mode": "once"})");
    const model::Session unity = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session kept = p.app->head();
    for (const bool running : {true, false}) {
        INFO("capturing " << running);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 6});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(unity, p.app->projectDir()));
        engine.play();
        backend.run(10);
        engine.service();
        if (running) REQUIRE(engine.captureLaunches(true));
        const Engine::LaunchRequest r = request("tr_2", "cl_2");
        REQUIRE(engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));  // bar 2
        backend.run(136000 / 128);                                             // past its one pass
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        REQUIRE(engine.load(kept, p.app->projectDir()));
        REQUIRE(engine.awaitPlan());
        if (running) {
            engine.service();
            const auto capture = engine.launchCapture();
            REQUIRE(capture);
            CHECK(capture->on);
            const auto& w = engine.warnings();
            CHECK_FALSE(std::any_of(w.begin(), w.end(), [](const std::string& s) {
                return s.find("capture ended") != std::string::npos;
            }));
        } else {
            // Before the service returns tr_2 to the arrangement: allowed.
            CHECK(engine.captureLaunches(true));
        }
        engine.stop();
    }
}

TEST_CASE("under a master playrate a switch crossfades over 240 song samples, by song position") {
    // S3 B (ADR-0097, note 2026-11-22): at 0.75 bar 2 (song 96000) is device
    // frame 128000. The crossfade runs over the next 240 song samples (320
    // device frames, 5 ms over the rate as heard), each frame's share by its
    // song position as a committed item's fade, the arrangement's ramp out
    // and the clip's in, each read through the tape converter.
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
    for (std::int64_t k = 60; k < 320; k += 30) {
        const double x = 0.75 * double(k) / double(kFade);
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

TEST_CASE("a capture under a master playrate runs through a rate edit and over a loop wrap" *
          doctest::description("rt")) {
    // S3 B, Q1: a scalar edit mid-capture keeps it running; the performance
    // holds each launch at its song position (song samples from the start),
    // laid out past the wrap by running song time. Under a loop over bars
    // 1-2 at 0.75: the scene on bar 2 (song 96000), then after the wrap and
    // a change to 1.2, tr_1 back to the arrangement on the loop's next bar.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
        p.run("transport.loop", R"({"start_seconds": 0, "end_seconds": 4, "enabled": true})");
        AppLive live(p, options, 128, 48000 * 9 + 16384);
        p.run("launch.capture", R"({"on": true})");
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2
        live.until(256000 + 20000);                     // the wrap: song 192000 at 256000
        p.run("transport.playrate.set", R"({"rate": 1.2, "preserve_pitch": false})");
        live.until(live.cycles * 128 + 4800);
        CHECK(p.run("launch.state", "{}")["capturing"].asBool());
        json::Value back = p.run("launch.arrangement", R"({"track": "tr_1"})");
        live.until(live.cycles * 128 + 96000);
        CHECK(p.run("launch.state", "{}")["capturing"].asBool());
        p.run("launch.commit", "{}");
        json::Value history = p.run("log.list", R"({"limit": 1})");
        const json::Value& last = history["entries"].at(history["entries"].size() - 1);
        REQUIRE(last.find("command")->asString() == "launch.commit");
        const json::Value& perf = *last.find("args")->find("performance");
        CHECK(perf.find("start")->asInt() == 0);
        const json::Value& launches = *perf.find("launches");
        REQUIRE(launches.size() == 3);
        CHECK(launches.at(0).find("frame")->asInt() == 96000);
        CHECK(launches.at(1).find("frame")->asInt() == 96000);
        // Past the wrap: the loop's length, then the anchor's place in it.
        const std::int64_t anchor = launches.at(2).find("anchor")->asInt();
        CHECK(anchor == std::int64_t(back["beat"].asDouble() * 24000));  // 120 BPM
        CHECK(launches.at(2).find("frame")->asInt() == 192000 + anchor);
        live.stop();
    }
}

TEST_CASE("a capture armed while the rate glides to 1 commits each launch on its song position" *
          doctest::description("rt")) {
    // S3 B, the builder's song journey 2 (D2): set back to 1 from 0.75, the
    // rate heard glides there. Arming during the glide is allowed now, and
    // the launches land on their bars: each one's frame is its anchor's
    // distance from the start, so the commit lays them on the bar lines (at
    // the device frames the journey found, 7.669 and 15.669 beats, before).
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
        AppLive live(p, options, 128, 48000 * 9 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20000);
        p.run("transport.playrate.set", R"({"rate": 1, "preserve_pitch": false})");
        live.until(20000 + 1280);
        REQUIRE(p.app->engine()->effectivePlayrate() != 1);  // gliding
        p.run("launch.capture", R"({"on": true})");
        const double scene = p.run("launch.scene", R"({"scene": "sc_1"})")["beat"].asDouble();
        live.until(live.cycles * 128 + 96000 + 4000);
        const double back = p.run("launch.arrangement", R"({"track": "tr_1"})")["beat"].asDouble();
        live.until(live.cycles * 128 + 96000 + 4000);
        json::Value done = p.run("launch.commit", "{}");
        json::Value history = p.run("log.list", R"({"limit": 1})");
        const json::Value& last = history["entries"].at(history["entries"].size() - 1);
        const json::Value& perf = *last.find("args")->find("performance");
        const std::int64_t start = perf.find("start")->asInt();
        const json::Value& launches = *perf.find("launches");
        REQUIRE(launches.size() == 3);
        for (std::size_t k = 0; k < 3; ++k) {
            const std::int64_t anchor = launches.at(k).find("anchor")->asInt();
            CHECK(launches.at(k).find("frame")->asInt() == anchor - start);
            CHECK(launches.at(k).find("anchor_beat")->asDouble() == (k < 2 ? scene : back));
        }
        CHECK(std::fmod(scene, 4.0) == 0.0);
        CHECK(std::fmod(back, 4.0) == 0.0);
        live.stop();
    }
}

TEST_CASE("under a pitch-keeping master playrate a MIDI capture commits as heard" *
          doctest::description("rt")) {
    // S3 B, Q2 (b): with the pitch kept, launched MIDI is captured as at
    // any rate; the audio arrangement item, which no launch touches, is read
    // by the same stretch converter live and in the commit's render.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        AppLive live(p, options, 128, 48000 * 7 + 16384);
        p.run("launch.capture", R"({"on": true})");
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 2: device 128000
        live.until(128000 + 8000);
        p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3: device 256000
        live.until(256000 + 40000);
        REQUIRE(p.app->engine()->launchLog().size() == 2);
        const std::vector<float> plain = arrangement(p, 7.0, 128);  // before the commit
        p.run("launch.commit", "{}");
        const std::int64_t ended = live.cycles * 128;
        const std::vector<float> heard = live.stop();
        const std::vector<float> committed = arrangement(p, 7.0, 128);
        const std::int64_t end = std::min<std::int64_t>(ended - std::int64_t(live.latency) - 128,
                                                        std::int64_t(committed.size()));
        REQUIRE(end > 256000 + 20000);
        const auto [worst, first] = compare(heard, live.latency, committed, 0, end);
        CHECK(worst == 0.0);
        CHECK(first == -1);
        // The launches changed what played: the uncommitted arrangement differs.
        CHECK(compare(heard, live.latency, plain, 0, end).first >= 0);
    }
}

TEST_CASE("under a pitch-keeping rate a switch that cuts arrangement audio ends the capture there" *
          doctest::description("rt")) {
    // Round 113 T2: tr_2's arrangement holds audio, read by the stretch
    // converter while the pitch is kept. A MIDI clip launched on it cuts
    // that item where the commit splits it, but the converter reads on live
    // (measured: the commit differed for 5664 frames before the switch,
    // and by up to 0.37 after a return). So a switch away from, or back to,
    // such an arrangement ends a capture on its boundary, with a warning
    // naming the track, and what was captured until then commits exactly.
    // A MIDI launch on tr_1, whose arrangement is MIDI, does not.
    for (const bool back : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("back " << back << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            // tr_2's item (5 s) from beat 4: it sounds at both switches.
            p.run("item.move", R"({"item": "it_2", "position": 4})");
            p.run("scene.add", "{}");  // sc_2
            const std::string clip = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                                       R"("notation": "voice v; 1: C4:w"})")["clip"]
                                         .asString();
            auto launchIt = j(R"({})");
            launchIt["clip"] = clip;
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            AppLive live(p, options, 128, 48000 * 9 + 16384);
            REQUIRE(p.app->transport("play"));
            live.until(20 * 128);
            if (back) {  // tr_2 on its MIDI clip from bar 2, before the capture
                p.runArgs("launch.clip", launchIt);
                live.until(128000 + 8000);
            }
            const double from = p.run("launch.capture", R"({"on": true})")["from"].asDouble();
            p.run("launch.clip", R"({"clip": "cl_1"})");  // tr_1, bar 2 or 3: no end
            live.until((back ? 256000 : 128000) + 8000);
            CHECK(p.run("launch.state", "{}")["capturing"].asBool());
            CHECK_FALSE(p.run("launch.state", "{}").find("capture_ends"));
            // The switch on tr_2, one bar later, ends it there.
            if (back)
                p.run("launch.arrangement", R"({"track": "tr_2"})");  // bar 4: song 288000
            else
                p.runArgs("launch.clip", launchIt);  // bar 3: song 192000
            const double bar = back ? 12.0 : 8.0;
            json::Value state = p.run("launch.state", "{}");
            CHECK(state["capturing"].asBool());
            REQUIRE(state.find("capture_ends"));
            CHECK(state["capture_ends"].asDouble() == bar);
            const auto& w = p.app->engine()->warnings();
            CHECK(std::any_of(w.begin(), w.end(), [](const std::string& m) {
                return m.find("the capture ends at tr_2's switch") != std::string::npos;
            }));
            live.until(std::llround(bar * double(kBeat) / 0.75) + 8000);
            CHECK_FALSE(p.run("launch.state", "{}")["capturing"].asBool());
            json::Value done = p.run("launch.commit", "{}");
            CHECK(done["from"].asDouble() == from);
            CHECK(done["to"].asDouble() == bar);
            const std::vector<float> heard = live.stop();
            const std::vector<float> committed = arrangement(p, 9.0, 128);
            const std::int64_t start = std::llround(from * double(kBeat) / 0.75);
            const std::int64_t end = std::llround(bar * double(kBeat) / 0.75);
            REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
            const auto [worst, first] = compare(heard, live.latency, committed, start, end);
            CHECK(worst == 0.0);
            CHECK(first == -1);
        }
}

TEST_CASE("under a pitch-keeping rate an audio clip's launch ends the capture at its switch" *
          doctest::description("rt")) {
    // S3 step D, Q2 (b): a launched audio clip reads through a stretch
    // converter whose history no committed item has, so a capture cannot
    // write it as heard. Its switch ends a running capture there, with a
    // warning saying why, though no arrangement audio is near (tr_2's item
    // is moved far off); what was captured until then commits exactly.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("item.move", R"({"item": "it_2", "position": 64})");  // far from both switches
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        AppLive live(p, options, 128, 48000 * 6 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        const double from = p.run("launch.capture", R"({"on": true})")["from"].asDouble();
        p.run("launch.clip", R"({"clip": "cl_1"})");  // tr_1, MIDI, bar 2: no end
        live.until(128000 + 8000);
        CHECK(p.run("launch.state", "{}")["capturing"].asBool());
        CHECK_FALSE(p.run("launch.state", "{}").find("capture_ends"));
        p.run("launch.clip", R"({"clip": "cl_2"})");  // tr_2, audio, bar 3: song 192000
        json::Value state = p.run("launch.state", "{}");
        CHECK(state["capturing"].asBool());
        REQUIRE(state.find("capture_ends"));
        CHECK(state["capture_ends"].asDouble() == 8.0);
        const auto& w = p.app->engine()->warnings();
        CHECK(std::any_of(w.begin(), w.end(), [](const std::string& m) {
            return m.find("the capture ends at tr_2's switch: it launches an audio clip") !=
                   std::string::npos;
        }));
        live.until(256000 + 8000);
        CHECK_FALSE(p.run("launch.state", "{}")["capturing"].asBool());
        json::Value done = p.run("launch.commit", "{}");
        CHECK(done["from"].asDouble() == from);
        CHECK(done["to"].asDouble() == 8.0);
        const std::vector<float> heard = live.stop();
        const std::vector<float> committed = arrangement(p, 6.0, 128);
        const std::int64_t start = std::llround(from * double(kBeat) / 0.75);
        const std::int64_t end = 256000;
        REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
        const auto [worst, first] = compare(heard, live.latency, committed, start, end);
        CHECK(worst == 0.0);
        CHECK(first == -1);
    }
}

TEST_CASE("under a pitch-keeping rate a capture's end at a switch goes with that switch" *
          doctest::description("rt")) {
    // Review round 117 W2: tr_2's switch to a MIDI clip on bar 3 (song
    // 192000), over its audio item, sets the capture's end there, with one
    // warning. Replaced before its boundary by a return to the arrangement,
    // or voided with its clip removed, the end goes with it: the capture runs
    // on past bar 3 and commits exactly.
    for (const bool removed : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("clip removed " << removed << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            p.run("scene.add", "{}");  // sc_2
            const std::string clip = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                                       R"("notation": "voice v; 1: C4:w"})")["clip"]
                                         .asString();
            auto launchIt = j(R"({})");
            launchIt["clip"] = clip;
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            AppLive live(p, options, 128, 48000 * 9 + 16384);
            REQUIRE(p.app->transport("play"));
            live.until(20 * 128);
            const double from = p.run("launch.capture", R"({"on": true})")["from"].asDouble();
            live.until(128000 + 8000);           // into bar 2
            p.runArgs("launch.clip", launchIt);  // bar 3
            json::Value state = p.run("launch.state", "{}");
            REQUIRE(state.find("capture_ends"));
            CHECK(state["capture_ends"].asDouble() == 8.0);
            const auto ends = [&] {
                const auto& w = p.app->engine()->warnings();
                return std::count_if(w.begin(), w.end(), [](const std::string& m) {
                    return m.find("the capture ends at tr_2's switch") != std::string::npos;
                });
            };
            CHECK(ends() == 1);
            if (removed) {
                auto args = j(R"({})");
                args["clip"] = clip;
                p.runArgs("clip.remove", args);
            } else {
                p.run("launch.arrangement", R"({"track": "tr_2"})");
            }
            live.until(live.cycles * 128 + 16 * 128);
            state = p.run("launch.state", "{}");
            CHECK(state["capturing"].asBool());
            CHECK_FALSE(state.find("capture_ends"));
            live.until(256000 + 40000);  // past bar 3
            CHECK(p.run("launch.state", "{}")["capturing"].asBool());
            CHECK(ends() <= 1);  // no other (a new plan starts the list again)
            json::Value done = p.run("launch.commit", "{}");
            CHECK(done["from"].asDouble() == from);
            const double to = done["to"].asDouble();
            CHECK(to > 8.5);
            live.until(live.cycles * 128 + 20 * 128);
            const std::vector<float> heard = live.stop();
            const std::vector<float> committed = arrangement(p, 9.0, 128);
            const std::int64_t start = std::llround(from * double(kBeat) / 0.75);
            const std::int64_t end = std::llround(to * double(kBeat) / 0.75);
            REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
            REQUIRE(end <= std::int64_t(committed.size()));
            const auto [worst, first] = compare(heard, live.latency, committed, start, end);
            CHECK_MESSAGE(worst == 0.0, "differs by " << worst << " from frame " << first);
        }
}

TEST_CASE(
    "under a pitch-keeping rate a switch clear of the track's audio does not end the capture" *
    doctest::description("rt")) {
    // Review round 117 W1: only a switch within the stretch converter's reach
    // of the track's arrangement audio ends a capture. tr_2's item (5 s of a
    // ramp) ends 0.1 s or 1 s before tr_2's switch to a MIDI clip on bar 4
    // (song 288000). Within the reach the capture ends there, as before;
    // clear of it the capture runs on, and its commit is exact to its last
    // sample: the converter reads nothing of the item there.
    for (const double gap : {0.1, 1.0})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("gap " << gap << " s, anticipative " << options.anticipative);
            const bool nearAudio = gap < 0.25;
            Project p;
            twoTracks(p);
            auto move = j(R"({"item": "it_2"})");  // 240000 samples: it ends at 288000 - gap
            move["position"] = (288000.0 - gap * 48000 - 240000) / double(kBeat);
            p.runArgs("item.move", move);
            p.run("scene.add", "{}");  // sc_2
            const std::string clip = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                                       R"("notation": "voice v; 1: C4:w"})")["clip"]
                                         .asString();
            auto launchIt = j(R"({})");
            launchIt["clip"] = clip;
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            AppLive live(p, options, 128, 48000 * 9 + 16384);
            REQUIRE(p.app->transport("play"));
            live.until(20 * 128);
            const double from = p.run("launch.capture", R"({"on": true})")["from"].asDouble();
            live.until(256000 + 8000);           // into bar 3
            p.runArgs("launch.clip", launchIt);  // bar 4: song 288000
            json::Value state = p.run("launch.state", "{}");
            CHECK(state["capturing"].asBool());
            CHECK(bool(state.find("capture_ends")) == nearAudio);
            live.until(std::llround(12.0 * double(kBeat) / 0.75) + 40000);
            CHECK(p.run("launch.state", "{}")["capturing"].asBool() == !nearAudio);
            json::Value done = p.run("launch.commit", "{}");
            CHECK(done["from"].asDouble() == from);
            const double to = done["to"].asDouble();
            if (nearAudio)
                CHECK(to == 12.0);
            else
                CHECK(to > 12.5);
            live.until(live.cycles * 128 + 20 * 128);
            const std::vector<float> heard = live.stop();
            const std::vector<float> committed = arrangement(p, 9.0, 128);
            const std::int64_t start = std::llround(from * double(kBeat) / 0.75);
            const std::int64_t end = std::llround(to * double(kBeat) / 0.75);
            REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
            REQUIRE(end <= std::int64_t(committed.size()));
            const auto [worst, first] = compare(heard, live.latency, committed, start, end);
            CHECK_MESSAGE(worst == 0.0, "differs by " << worst << " from frame " << first);
        }
}

TEST_CASE(
    "under a pitch-keeping rate a switch's reach is judged on the timeline, across a wrap and "
    "by the rate" *
    doctest::description("rt")) {
    // Review round 128 W1, by the decision launch.state reports (a capture
    // that ends at the switch says `capture_ends`):
    //  - a transport loop over 0-6 s, tr_2's switch to a MIDI clip on bar 3
    //    (4 s) on the second pass: its song frame is 10 s (the anchor plus a
    //    pass), and the audio is judged at the anchor (L1): tr_2's item over
    //    1.5-6.5 s ends the capture, and one at 9-14 s, never heard, does not;
    //  - a loop over 0-5.6 s and the switch on the beat at 5.5 s: the item at
    //    0-5 s lies just past the wrap, where the converter runs on, and ends it;
    //  - at rate 2 an item ending 0.4 s before the switch on bar 4 (6 s) is
    //    within the reach, 0.25 s of song at most rate 1, scaled by the rate
    //    above it: the scalar's, or an envelope's points over a scalar of
    //    0.75 (round 128 N2).
    struct Case {
        const char* name;
        double rate, loopEnd, itemSeconds, launchSong;
        bool quantizeBeat, ends, envelope = false;
    };
    for (const Case& c :
         {Case{"pass 2, audio at the anchor", 0.75, 6.0, 1.5, 8.2, false, true},
          Case{"pass 2, audio at the song frame", 0.75, 6.0, 9.0, 8.2, false, false},
          Case{"audio past the wrap", 0.75, 5.6, 0.0, 5.1, true, true},
          Case{"rate 2, 0.4 s clear", 2.0, 0.0, 0.6, 4.4, false, true},
          Case{"an envelope at 2, 0.4 s clear", 2.0, 0.0, 0.6, 4.4, false, true, true}}) {
        INFO(c.name);
        Project p;
        twoTracks(p);
        auto move = j(R"({"item": "it_2"})");  // 5 s of a ramp
        move["position"] = c.itemSeconds * 48000 / double(kBeat);
        p.runArgs("item.move", move);
        p.run("scene.add", "{}");  // sc_2
        const std::string clip = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                                   R"("notation": "voice v; 1: C4:w"})")["clip"]
                                     .asString();
        auto launchIt = j(R"({})");
        launchIt["clip"] = clip;
        if (c.quantizeBeat) {
            auto q = j(R"({"quantize": "beat"})");
            q["clip"] = clip;
            p.runArgs("clip.set", q);
        }
        auto rate = j(R"({"preserve_pitch": true})");
        rate["rate"] = c.envelope ? 0.75 : c.rate;
        p.runArgs("transport.playrate.set", rate);
        if (c.envelope)  // it replaces the scalar: rate 2 throughout
            p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                             R"({"beat": 0, "value": 2}, {"beat": 32, "value": 2}]})");
        if (c.loopEnd > 0) {
            auto loop = j(R"({"start_seconds": 0, "enabled": true})");
            loop["end_seconds"] = c.loopEnd;
            p.runArgs("transport.loop", loop);
        }
        AppLive live(p, kInCallback, 128, 48000 * 12);
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        REQUIRE(p.run("launch.capture", R"({"on": true})")["from"].isNumber());
        // To `launchSong` seconds of song heard (past the loop's end: the
        // second pass), then the launch.
        live.until(std::llround(c.launchSong * 48000 / c.rate));
        p.runArgs("launch.clip", launchIt);
        json::Value state = p.run("launch.state", "{}");
        CHECK(state["capturing"].asBool());
        CHECK(bool(state.find("capture_ends")) == c.ends);
        live.stop();
    }
}

TEST_CASE("a once audio clip is judged by where the song is after a relocation" *
          doctest::description("rt")) {
    // Review round 117 W3: cl_2 played once (0.75 s from bar 2, song 96000
    // to 132000) keeps its anchor through a seek, so it sounds again only if
    // the song comes back to its pass. Sought mid-pass to bar 3 (past it),
    // a plan that keeps the pitch allows arming; sought back to the start,
    // it will play again on bar 2, so arming is refused as before; and under
    // a transport loop over bars 1-2, past its pass, it plays again on the
    // next pass, so arming is refused too.
    Project p;
    twoTracks(p);
    p.run("clip.set", R"({"clip": "cl_2", "mode": "once"})");
    const model::Session unity = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session kept = p.app->head();
    for (const double seekTo : {4.0, 0.0, -1.0}) {  // -1: no seek, a loop over bars 1-2
        INFO("sought to " << seekTo << " s");
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 6});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(unity, p.app->projectDir()));
        if (seekTo < 0) engine.setLoop(0, 4, true);
        engine.play();
        backend.run(10);
        engine.service();
        const Engine::LaunchRequest r = request("tr_2", "cl_2");
        REQUIRE(engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));  // bar 2
        backend.run(100000 / 128);                                             // in its pass
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        if (seekTo >= 0) {
            engine.seek(seekTo);
            backend.run(20);
        } else {
            backend.run(60000 / 128);  // past its pass (song 132000), before the wrap
        }
        engine.service();
        REQUIRE(engine.load(kept, p.app->projectDir()));
        REQUIRE(engine.awaitPlan());
        // Before the service returns tr_2 to the arrangement.
        auto armed = engine.captureLaunches(true);
        if (seekTo > 0) {
            CHECK_MESSAGE(armed, (armed ? std::string() : armed.error().toString()));
        } else {
            REQUIRE_FALSE(armed);
            CHECK(armed.error().code == "launch.playrate");
        }
        engine.stop();
    }
}

TEST_CASE("a launched audio clip under a pitch-keeping rate: a capture ends, arming is refused" *
          doctest::description("rt")) {
    // S3 B, Q2 (b): a capture cannot write launched audio as heard while the
    // pitch is kept. A plan that keeps it arriving while cl_2 plays ends a
    // running capture there, with a warning naming the clip, and what was
    // performed stays committable; arming while it still plays is refused.
    Project p;
    twoTracks(p);
    const model::Session unity = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session kept = p.app->head();
    for (const bool running : {true, false}) {
        INFO("capturing " << running);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 6});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(unity, p.app->projectDir()));
        engine.play();
        backend.run(10);
        engine.service();
        if (running) REQUIRE(engine.captureLaunches(true));
        const Engine::LaunchRequest r = request("tr_2", "cl_2");
        REQUIRE(engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));  // bar 2
        backend.run(96000 / 128 + 40);
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        REQUIRE(engine.load(kept, p.app->projectDir()));
        REQUIRE(engine.awaitPlan());
        if (running) {
            engine.service();
            const auto capture = engine.launchCapture();
            REQUIRE(capture);
            CHECK_FALSE(capture->on);
            CHECK(capture->frames > 96000);
            REQUIRE(capture->launches.size() == 1);
            const auto& w = engine.warnings();
            CHECK(std::any_of(w.begin(), w.end(), [](const std::string& s) {
                return s.find("capture ended") != std::string::npos &&
                       s.find("cl_2") != std::string::npos;
            }));
        } else {
            // Before the service returns tr_2 to the arrangement: refused.
            auto refused = engine.captureLaunches(true);
            REQUIRE_FALSE(refused);
            CHECK(refused.error().code == "launch.playrate");
            CHECK(refused.error().message.find("cl_2") != std::string::npos);
        }
        engine.stop();
    }
}
