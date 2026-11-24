// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launched audio under a pitch-keeping master playrate (S3 step D): the
// stretch converter's warm-up before a switch, cold starts, the readers'
// carry, placement around a warm-up, and the capture's end at an audio
// launch. Its own ctest entry, split from test_clip_player_rate.cpp, which
// ran past 600 s under ThreadSanitizer with them; the presses during a
// warm-up are in test_clip_player_rate_presses.cpp.
#include "clip_player_support.h"
#include "engine_test_access.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

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
    // it, and the launch still switches, at its acknowledged frame, and
    // plays, cold. The rate change also after a seek to 0.5 s (review round
    // 160: the device 24000 frames ahead of the stream; the next bar is bar
    // 2 again, at running frame 96000, so the material at the switch is the
    // twin's; the capture holds the eight cycles run stopped before play),
    // tr_1 muted in both so that the step at the switch is tr_2's alone,
    // compared with the twin without the seek.
    struct Case {
        bool rate;
        double seek;
    };
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        double twinStep = -1;
        for (const Case c : {Case{false, 0}, Case{true, 0}, Case{true, 0.5}}) {
            INFO("rate change " << c.rate << ", sought to " << c.seek << ", anticipative "
                                << options.anticipative);
            Project p;
            twoTracks(p);
            if (c.rate) p.run("track.set", R"({"track": "tr_1", "mute": true})");
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            AppLive live(p, options, 128, 48000 * 6 + 16384);
            const std::int64_t stopped = c.seek > 0 ? 8 * 128 : 0;
            if (c.seek > 0) {
                p.run("transport.seek", R"({"seconds": 0.5})");
                live.until(stopped);
            }
            REQUIRE(p.app->transport("play"));
            live.until(stopped + 20 * 128);
            p.run("launch.clip", R"({"clip": "cl_2"})");           // bar 2
            const std::int64_t bar = c.seek > 0 ? 96000 : 128000;  // its running frame
            live.until(stopped + bar - 1500);  // inside its warm-up (from bar - 2944)
            if (c.rate)
                p.run("transport.playrate.set", R"({"rate": 0.8, "preserve_pitch": true})");
            else
                p.run("track.add", R"({"name": "Empty"})");
            live.until(stopped + 250000);
            const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
            REQUIRE(log.size() == 1);
            CHECK(log[0].anchorSample == 96000);
            const std::int64_t switched = log[0].runningFrame;
            CHECK(switched == bar);  // applied (warming) before the edit: not moved
            ChangeRun run;
            run.heard = live.stop();
            run.latency = live.latency + static_cast<std::uint32_t>(stopped);
            CHECK(loudest(run, switched + 2000, switched + 40000) > 0.01);  // cl_2's ramp plays
            if (!c.rate) {
                const std::vector<float> offline = replay(p, 6.0, log, 128);
                const auto [worst, first] = compare(run.heard, run.latency, offline, 0, 240000);
                CHECK(worst == 0.0);
                CHECK(first == -1);
                continue;
            }
            // From the edit through the switch, against the twin's.
            const double step = largestStep(run, switched - 1500, switched + 3000);
            INFO("largest step from the edit through the switch " << step << ", the twin's "
                                                                  << twinStep);
            if (c.seek == 0)
                twinStep = step;
            else
                CHECK(step <= 1.5 * twinStep);
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

TEST_CASE("after a seek a launched clip's converter still warms up ahead of its switch" *
          doctest::description("rt")) {
    // S3 step D: a launch's warm-up runs at its switch's frame on the
    // trajectory, a device frame; the slot's target is a stream frame, which
    // a seek moves away from the device's by the seek's target (the plan's
    // rateOrigin). Taken as a device frame, the warm-up primed for frames
    // 144000 early here and went to waste, and the switch started cold, its
    // 45 primes in the callback holding it (90 in all).
    for (const double seek : {0.0, 3.0})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("sought to " << seek << " s, anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            AppLive live(p, options, 128, 48000 * 8);
            if (seek > 0) {
                auto a = j("{}");
                a["seconds"] = seek;
                p.runArgs("transport.seek", a);
                live.until(8 * 128);
            }
            REQUIRE(p.app->transport("play"));
            live.until(40 * 128);
            p.run("launch.clip", R"({"clip": "cl_2"})");  // the next bar
            std::uint64_t most = 0, total = 0;
            for (std::int64_t c = 41; c <= 200000 / 128; ++c) {
                const std::uint64_t before = detail::stretchPrimes();
                live.until(c * 128);
                const std::uint64_t d = detail::stretchPrimes() - before;
                total += d;
                most = std::max(most, d);
            }
            const auto log = p.app->engine()->launchLog();
            REQUIRE(log.size() == 1);
            CHECK(log[0].runningFrame == (seek > 0 ? 64000 : 128000));
            CHECK(p.app->engine()->processErrors() == 0);
            INFO("most primes in one callback " << most << ", " << total << " in all");
            CHECK(total == 45);  // one warm-up, no cold start
            // The renderer runs ahead in longer slices; the callback primes
            // once per 64 frames played.
            if (!options.anticipative) CHECK(most <= 2);
            ChangeRun run;
            run.heard = live.stop();
            run.latency = live.latency + (seek > 0 ? 8 * 128 : 0);  // the cycles before play
            CHECK(loudest(run, log[0].runningFrame + 3000, log[0].runningFrame + 30000) > 0.01);
        }
}

TEST_CASE("a plan that moves a track's shift during its launch's warm-up keeps the warm-up" *
          doctest::description("rt")) {
    // Review round 157 F1: tr_3 holds only an audio clip (no items, so a
    // change of its shift needs no fresh converter history), shifted by
    // tr_2's latent gain; a gain of the same latency added to tr_3 inside
    // cl_3's warm-up takes its shift to 0. The warm-up's switch was kept as
    // a node frame (the stream target plus the shift at the warm-up's
    // start), so the new plan read it that shift early, its converter
    // started again and the switch primed all at once. It is now the bare
    // stream target.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("track.add", R"({"name": "Clips"})");  // tr_3
        auto args = j(R"({"track": "tr_3", "scene": "sc_1"})");
        args["audio_file"] = p.ramp("clip3.wav", 48000).string();
        p.runArgs("clip.add", args);  // cl_3
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        AppLive live(p, options, 128, 48000 * 6);
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.clip", R"({"clip": "cl_3"})");  // bar 2: running frame 128000
        // The swap reaches the executors about this long after the edit, so
        // inside the warm-up (from 125056).
        const std::int64_t lag = options.anticipative ? 9793 : 9601;
        // All of them from the launch on (the renderer also primes while the
        // edits run), and the most in one callback.
        const std::uint64_t start = detail::stretchPrimes();
        std::uint64_t most = 0;
        const auto run = [&](std::int64_t to) {
            for (std::int64_t c = live.cycles + 1; c <= to / 128; ++c) {
                const std::uint64_t before = detail::stretchPrimes();
                live.until(c * 128);
                most = std::max(most, detail::stretchPrimes() - before);
            }
        };
        run(128000 - lag - 1500);
        const auto shiftOf = [&] {
            const detail::Plan& plan = EngineTestAccess::latestPlan(*p.app->engine());
            return std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                [](const auto& n) { return n.id == id("tr_3"); })
                ->timelineShift;
        };
        const std::int64_t before = shiftOf();
        p.run("fx.add", R"({"track": "tr_3", "plugin": "com.suppleaudio.test.gain"})");
        p.run("fx.patch.merge", R"({"fx": "fx_3", "patch": {"params": {"latency": "short"}}})");
        // The swap moves the shift and is handed over inside the warm-up
        // (review round 160 S1), else the case says nothing.
        const std::int64_t handAt = EngineTestAccess::latestPlan(*p.app->engine()).handAt;
        INFO("tr_3's shift " << before << " then " << shiftOf() << ", handed over at " << handAt);
        CHECK(before > 0);
        CHECK(shiftOf() == 0);
        CHECK(handAt > 128000 - 2944);
        CHECK(handAt < 128000);
        run(200000);
        const std::uint64_t total = detail::stretchPrimes() - start;
        const auto log = p.app->engine()->launchLog();
        REQUIRE(log.size() == 1);
        CHECK(log[0].runningFrame == 128000);
        CHECK(p.app->engine()->processErrors() == 0);
        INFO("most primes in one callback " << most << ", " << total << " in all");
        CHECK(total == 45);  // one warm-up, not started again
        if (!options.anticipative) CHECK(most <= 2);
        ChangeRun heard;
        heard.heard = live.stop();
        heard.latency = live.latency;
        CHECK(loudest(heard, 131000, 160000) > 0.01);  // cl_3 plays
    }
}

TEST_CASE("after a seek a launched audio clip under a pitch-keeping rate plays as the replay" *
          doctest::description("rt")) {
    // Review round 157: the seek of the case above (3 s, so the device runs
    // 144000 frames ahead of the stream), cl_2 launched on the next bar,
    // against the offline replay of the log from the seek's position. The
    // capture holds the eight cycles run stopped before play.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        AppLive live(p, options, 128, 48000 * 6);
        p.run("transport.seek", R"({"seconds": 3})");
        live.until(8 * 128);
        REQUIRE(p.app->transport("play"));
        live.until(40 * 128);
        p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 3: running frame 64000
        live.until(200000);
        const auto log = p.app->engine()->launchLog();
        REQUIRE(log.size() == 1);
        CHECK(log[0].runningFrame == 64000);
        CHECK(p.app->engine()->processErrors() == 0);
        const std::vector<float> heard = live.stop();
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.startSeconds = 3.0;
        settings.endSeconds = 3.0 + 200000 * 0.75 / 48000;
        settings.blockSize = 128;
        settings.projectDir = p.app->projectDir();
        const auto render = [&](std::vector<LaunchLogEntry> launches) {
            settings.launches = std::move(launches);
            auto r = renderOffline(p.app->head(), pool, settings);
            REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
            REQUIRE(r->audio.channels[0].size() >= 180000);
            return r->audio.channels[0];
        };
        const std::vector<float> offline = render(log);
        const std::uint32_t at = live.latency + 8 * 128;
        const auto [worst, first] = compare(heard, at, offline, 0, 180000);
        INFO("worst " << worst << " first at " << first);
        CHECK(worst == 0.0);
        CHECK(first == -1);
        // Without the launch it is not the same.
        const std::vector<float> plain = render({});
        const auto [apart, from] = compare(heard, at, plain, 0, 180000);
        CHECK(apart > 1e-3);
        CHECK(from >= 60000);  // the same until the switch
    }
}
