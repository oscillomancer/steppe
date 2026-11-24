// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launched audio under a pitch-keeping master playrate (S3 step D): the
// stretch converter's warm-up before a switch, cold starts, the readers'
// carry, placement around a warm-up, and the capture's end at an audio
// launch. Its own ctest entry, split from test_clip_player_rate.cpp, which
// ran past 600 s under ThreadSanitizer with them.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
// Service-only steps every 1024 frames over [from, to), merged in order. The
// app services its engine on every tick, and service() places a launch the
// renderer overtook while it was placed again from there (engine.md
// §1.4.2): an unquantized launch near the render horizon can be overtaken,
// under ThreadSanitizer's slowness above all, and without a service before
// the next step it would wait there unplaced.
std::vector<Step> serviced(std::vector<Step> steps, std::int64_t from, std::int64_t to,
                           std::uint32_t block) {
    for (std::int64_t f = from; f < to; f += 1024) steps.push_back({f / block, {}});
    std::stable_sort(steps.begin(), steps.end(),
                     [](const Step& a, const Step& b) { return a.cycle < b.cycle; });
    return steps;
}
}  // namespace

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

TEST_CASE("a press during an audio launch's warm-up lands after its switch, and wins" *
          doctest::description("rt")) {
    // Round 139 S1: cl_2 (audio) is applied from 2944 frames before bar 2.
    // A stop, a return or a MIDI clip pressed inside that warm-up would land
    // before its switch (unquantized) or on the same frame (on Bar), and the
    // earlier audio launch would then play over the later press. Each lands
    // after the switch instead (unquantized a frame after at least, on Bar
    // on bar 3), and the track ends on it; live is the replay.
    Project p;
    twoTracks(p);
    p.run("scene.add", "{}");  // sc_2
    const std::string midi = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                               R"("notation": "voice v; 1: C4:w"})")["clip"]
                                 .asString();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    Engine::LaunchRequest stop = backTo("tr_2");
    stop.action = LaunchLogEntry::Action::Stop;
    Engine::LaunchRequest notes = request("tr_2", "cl_2");
    notes.clip = id(midi.c_str());
    const std::pair<const char*, Engine::LaunchRequest> presses[] = {
        {"stop", stop}, {"return", backTo("tr_2")}, {"MIDI clip", notes}};
    Replays replays;
    for (const auto& [name, later] : presses)
        for (const bool quantized : {false, true})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                const std::string what = name;
                INFO(what << ", quantized " << quantized << ", anticipative "
                          << options.anticipative);
                constexpr std::uint32_t block = 128;
                const std::vector<Step> steps = serviced(
                    {{20, {request("tr_2", "cl_2")}},
                     {(128000 - 1000) / block,
                      {later},
                      quantized ? model::LaunchQuantize::Bar : model::LaunchQuantize::None}},
                    128000, 290000, block);
                const Live live =
                    playLive(s, p.app->projectDir(), options, block, 300000 + 16384, steps);
                REQUIRE_MESSAGE(live.log.size() == 2, describe(live.log) << live.report);
                CHECK(live.log[0].clip == id("cl_2"));
                CHECK(live.log[0].runningFrame == 128000);
                CHECK(live.log[1].action == later.action);
                if (later.action == LaunchLogEntry::Action::Clip)
                    CHECK(live.log[1].clip == notes.clip);
                if (quantized) {
                    CHECK(live.log[1].runningFrame == 256000);  // bar 3
                } else {
                    CHECK(live.log[1].runningFrame > 128000);
                }
                liveIsReplay(s, p.app->projectDir(), live, 7.0, 300000, replays);
            }
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
            const std::vector<Step> steps = serviced(
                {{2560 / block, {request("tr_2", "cl_2")}},
                 {(128000 - 1000) / block, {request("tr_2", "cl_3")}, model::LaunchQuantize::None},
                 {(256000 + 4000) / block, {request("tr_2", "cl_2")}}},
                128000, 250000, block);
            const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
            REQUIRE_MESSAGE(live.log.size() == 3, describe(live.log) << live.report);
            CHECK(live.log[0].runningFrame == 128000);
            CHECK(live.log[1].clip == id("cl_3"));
            CHECK(live.log[1].runningFrame >= 128000 + kWarm);
            CHECK(live.log[2].runningFrame == 384000);  // bar 4
            liveIsReplay(s, p.app->projectDir(), live, 10.0, 460000, replays);
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
