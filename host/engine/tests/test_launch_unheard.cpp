// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A launch applied ahead of the listener and not heard before a stop or a
// locate is taken back and placed again (ADR-0097, dated 2026-11-26): the
// anticipative executor, which applies launches in the frames it renders
// ahead, and the in-callback one, which applies a launch's early half
// (prelaunch, prewarm) before its switch, then sound as the in-callback
// executor does when nothing was applied, and the log lists only what was
// heard (the frames handed to the device callback).
#include "clip_player_support.h"
#include "engine_test_access.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {

enum class Interrupt { Pause, PauseAndReturn, Locate };

// What one interrupted run did: its capture, the log as it ended, the
// launch state while it stood stopped, and where it went on.
struct Interrupted {
    std::vector<float> out;
    std::uint32_t latency = 0;
    std::vector<LaunchLogEntry> log;
    std::vector<LaunchLogEntry> logStopped;    // the log while stopped (after a service)
    std::vector<Engine::TrackLaunch> stopped;  // launch.state then
    std::uint64_t epochBefore = 0;
    std::int64_t resumed = 0;  // the cycle it went on at
    bool unlogged = false;     // launchesUnlogged() as it ended
    std::uint64_t misses = 0, errors = 0;
    std::vector<std::int64_t> targets;  // each Step's placed stream frame
};

struct Run {
    std::uint32_t block = 32;
    // Each: the cycle it is made at (before that cycle runs), the requests
    // and their quantize.
    std::vector<Step> launches;
    std::int64_t stopCycle = 0;
    Interrupt how = Interrupt::Pause;
    std::int64_t frames = 48000 * 6;
    // After the resume: cl `relaunch` again, on the next bar (an unquantized
    // one lands at each executor's own horizon), this many cycles on.
    const char* relaunch = "cl_1";
    std::int64_t relaunchAfter = 1500;  // a bar's half before the next bar
    bool dropRendererAcks = false;
};

Interrupted interrupted(const model::Session& s, const fs::path& dir, const EngineOptions& options,
                        const Run& r) {
    Interrupted out;
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = r.frames});
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = r.block}));
    REQUIRE(engine.load(s, dir));
    EngineTestAccess::dropRendererAcks(engine, r.dropRendererAcks);
    engine.play();
    std::int64_t done = 0;
    const auto run = [&](std::int64_t until) {
        const std::int64_t n = until - done;
        if (n <= 0) return;
        if (options.anticipative)
            runAheadOnState(engine, backend, n, r.block);
        else
            backend.run(n);
        done = until;
    };
    for (const Step& step : r.launches) {
        run(step.cycle);
        engine.service();
        const auto placed = engine.launch(step.requests, step.quantize);
        REQUIRE(placed);
        out.targets.push_back(placed->stream);
    }
    run(r.stopCycle);
    engine.service();
    out.epochBefore = EngineTestAccess::epoch(engine);
    // Acks dropped only up to the interrupt: what is placed again after it
    // is heard in the new epoch and must be logged.
    EngineTestAccess::dropRendererAcks(engine, false);
    if (r.how == Interrupt::Locate) {
        engine.seek(0.5);
        run(done + 2);  // the callback leaves the epoch
        engine.service();
        out.logStopped = engine.launchLog();
        out.stopped = engine.launchState();
    } else {
        engine.pause();
        run(done + 200);  // stopped for 200 cycles
        engine.service();
        out.logStopped = engine.launchLog();
        out.stopped = engine.launchState();
        if (r.how == Interrupt::PauseAndReturn) {
            const Engine::LaunchRequest back = backTo("tr_1");
            REQUIRE(engine.launch(std::span(&back, 1), model::LaunchQuantize::None));
        }
        engine.play();
    }
    out.resumed = done;
    run(out.resumed + r.relaunchAfter);
    engine.service();
    if (r.relaunch) {
        const Engine::LaunchRequest again = request("tr_1", r.relaunch);
        REQUIRE(engine.launch(std::span(&again, 1), model::LaunchQuantize::Bar));
    }
    run(r.frames / r.block);
    engine.service();
    out.misses = engine.anticipationMisses();
    out.latency = engine.outputLatency();
    out.log = engine.launchLog();
    out.unlogged = engine.launchesUnlogged();
    engine.stop();
    out.errors = engine.processErrors();
    out.out = backend.capture().channels[0];
    return out;
}

// The two executors' runs of `r` sound alike over the whole capture (but
// its last render-ahead span) and log alike; returns the in-callback run.
Interrupted executorsAgree(const model::Session& s, const fs::path& dir, const Run& r) {
    const Interrupted cb = interrupted(s, dir, kInCallback, r);
    const Interrupted ah = interrupted(s, dir, EngineOptions{.workers = 0}, r);
    CHECK(cb.errors == 0);
    CHECK(ah.errors == 0);
    CHECK(ah.misses == 0);
    const std::size_t whole = std::size_t(r.frames) - 4096 - std::max(cb.latency, ah.latency);
    const std::vector<float> reference(cb.out.begin() + cb.latency,
                                       cb.out.begin() + cb.latency + whole);
    INFO("in-callback log:" << describe(cb.log));
    INFO("anticipative log:" << describe(ah.log));
    INFO("anticipative against in-callback: " << differences(ah.out, ah.latency, reference, whole));
    CHECK(maxDiff(ah.out, ah.latency, reference, whole) == 0.0);
    CHECK(describe(ah.log) == describe(cb.log));
    CHECK(describe(ah.logStopped) == describe(cb.logStopped));
    // Something sounds, so the comparison is not of silence.
    CHECK(std::any_of(reference.begin(), reference.end(), [](float v) { return v != 0.0f; }));
    return cb;
}

// No entry of the stopped epoch at or past where its hearing ended.
void onlyHeard(const Interrupted& run, std::int64_t heardEnd, std::int64_t shift) {
    for (const LaunchLogEntry& e : run.log)
        if (e.epoch == run.epochBefore) {
            INFO(describe(run.log));
            CHECK(e.runningFrame + shift < heardEnd);
        }
}

// tr_1's audio clips cl_1 (on sc_1) and cl_2 (on sc_2), ramps of `frames`.
void twoAudioClips(Project& p, std::size_t frames = 36000) {
    for (const char* scene : {"sc_1", "sc_2"}) {
        auto args = j(R"({"track": "tr_1"})");
        args["scene"] = scene;
        args["audio_file"] =
            p.ramp((std::string("clip-") + scene + ".wav").c_str(), frames).string();
        p.runArgs("clip.add", args);
    }
}

// tr_2, with no audio, gets a latent track FX: tr_1's channel is shifted
// by 64 frames (its launches switch 64 frames behind their target).
void shiftedChannel(Project& p) {
    p.run("track.add", R"({"name": "Latent"})");
    p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.gain"})");
    const std::string fx = p.app->head().tracks.find(id("tr_2"))->chain.front().str();
    p.run("fx.patch.merge",
          R"({"fx": ")" + fx + R"(", "patch": {"params": {"latency": "short"}}})");
}

// The offline replay of `log` from `start` seconds (a capture's render).
std::vector<float> replay(const model::Session& s, const fs::path& dir,
                          const std::vector<LaunchLogEntry>& log, std::size_t frames,
                          double start = 0.0) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.startSeconds = start;
    settings.endSeconds = start + double(frames) / 48000 + 0.05;
    settings.blockSize = 32;
    settings.projectDir = dir;
    settings.launches = log;
    auto r = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// The run sounds as its log replays (review round 201 F3): up to 2048
// frames before the stop, as the stopped epoch's log; and, when the launch
// was not heard, the resumed epoch for a second as its own log (from where
// it began: the resume, or the locate's frame two cycles before `resumed`).
void asReplayed(const model::Session& s, const fs::path& dir, const Interrupted& run,
                std::int64_t stopCycle, Interrupt how, bool heard) {
    std::vector<LaunchLogEntry> stopped, resumed;
    for (const LaunchLogEntry& e : run.log)
        (e.epoch <= run.epochBefore ? stopped : resumed).push_back(e);
    const std::size_t head = std::size_t(stopCycle * 32) - 2048;
    const auto before = replay(s, dir, stopped, head);
    INFO("before the stop against its log's replay: " << differences(run.out, run.latency, before,
                                                                     head));
    CHECK(maxDiff(run.out, run.latency, before, head) == 0.0);
    if (heard) return;  // carried over from the stopped epoch: its log cannot say so
    const std::size_t n = 48000;
    const double from = how == Interrupt::Locate ? 0.5 : double(stopCycle * 32) / 48000;
    const auto after = replay(s, dir, resumed, n, from);
    const std::size_t at =
        run.latency + std::size_t((how == Interrupt::Locate ? run.resumed - 2 : run.resumed) * 32);
    INFO("the resumed epoch against its log's replay: " << differences(run.out, at, after, n));
    CHECK(maxDiff(run.out, at, after, n) == 0.0);
}

}  // namespace

TEST_CASE("a launch the listener did not hear before a stop or locate is placed again (F6)" *
          doctest::description("rt")) {
    // Block 32, cl_1 launched on bar 2 (stream 96000; plain, then through a
    // 64-sample latent take chain whose input switches 64 frames early, the
    // in-callback executor's early half). Stopped after 2990 cycles (both
    // undecided in the in-callback run, applied ahead by the renderer),
    // 2998 and 2999 (the latent chain's input switched in callback), 3000
    // (the switch at the very frame hearing ended: not heard), 3001 and
    // 3010 (heard). Each by a pause and play, a pause, a return and play,
    // and a locate. The renderer applies the launch up to 4096 frames ahead:
    // the review's F6 (round 192) had it play straight on, its log claiming
    // a launch never heard. Round 201 F3: each run sounds as its log
    // replays, and a channel shifted 64 frames by another track's latent FX
    // (its switch heard at 96064: stops after 3001 and 3002 do not hear it).
    for (const int setup : {0, 1, 2}) {
        Project p;
        if (setup == 1)
            latentClip(p);
        else
            twoAudioClips(p);
        if (setup == 2) shiftedChannel(p);
        const std::int64_t shift = setup == 2 ? 64 : 0;
        const model::Session s = p.app->head();
        const std::vector<std::int64_t> stops =
            setup == 2 ? std::vector<std::int64_t>{2990, 3001, 3002, 3003, 3010}
                       : std::vector<std::int64_t>{2990, 2998, 2999, 3000, 3001, 3010};
        for (const std::int64_t stop : stops)
            for (const Interrupt how :
                 {Interrupt::Pause, Interrupt::PauseAndReturn, Interrupt::Locate}) {
                INFO("setup " << setup << ", stopped after cycle " << stop << ", interrupt "
                              << int(how));
                Run r;
                r.launches = {{20, {request("tr_1", "cl_1")}, model::LaunchQuantize::Bar}};
                r.stopCycle = stop;
                r.how = how;
                const Interrupted cb = executorsAgree(s, p.app->projectDir(), r);
                onlyHeard(cb, stop * 32, shift);
                // While stopped, launch.state has an unheard launch queued.
                const auto tr1 = std::find_if(cb.stopped.begin(), cb.stopped.end(),
                                              [](const auto& t) { return t.track == id("tr_1"); });
                REQUIRE(tr1 != cb.stopped.end());
                const bool heard = stop * 32 > 96000 + shift;
                CHECK((tr1->plays == Engine::TrackLaunch::Plays::Clip) == heard);
                if (!heard && how != Interrupt::Locate) CHECK(tr1->queued.has_value());
                asReplayed(s, p.app->projectDir(), cb, stop, how, heard);
            }
    }
}

TEST_CASE("the frame where hearing ended is not heard; the one before it is (F6 boundary)" *
          doctest::description("rt")) {
    // Hearing ends at stream 96000 (block 32, stopped after 3000 cycles).
    // At 120 BPM cl_1's bar-2 switch is at 96000, the frame where hearing
    // ended: not heard, so the stopped epoch logs nothing and the launch is
    // placed again. At 120.001 BPM it is at 95999, one frame inside: heard,
    // so it keeps its entry and is not placed again. Both executors, alike.
    for (const auto& [bpm, target] :
         {std::pair<const char*, std::int64_t>{"120", 96000}, {"120.001", 95999}}) {
        INFO(std::string(bpm) << " BPM, the switch at " << target);
        Project p;
        twoAudioClips(p);
        p.run("tempo.set", std::string(R"({"points": [{"beat": 0, "bpm": )") + bpm + "}]}");
        const model::Session s = p.app->head();
        Run r;
        r.launches = {{20, {request("tr_1", "cl_1")}, model::LaunchQuantize::Bar}};
        r.stopCycle = 3000;
        r.relaunch = nullptr;
        r.relaunchAfter = 1500;
        const Interrupted cb = executorsAgree(s, p.app->projectDir(), r);
        REQUIRE(cb.targets == std::vector<std::int64_t>{target});
        asReplayed(s, p.app->projectDir(), cb, 3000, Interrupt::Pause, target != 96000);
        std::size_t inStopped = 0, after = 0;
        for (const LaunchLogEntry& e : cb.log)
            (e.epoch == cb.epochBefore ? inStopped : after) += e.clip == id("cl_1");
        if (target == 96000) {
            CHECK(inStopped == 0);
            CHECK(after == 1);  // placed again, logged once, in the new epoch
        } else {
            CHECK(inStopped == 1);
            CHECK(after == 0);  // heard: not placed again
        }
    }
}

TEST_CASE("two unheard switches on one channel go back to its state before both (F6)" *
          doctest::description("rt")) {
    // cl_1 on the next 32nd note (3000 frames at 120 BPM), and once the
    // renderer has applied it, cl_2 on the next one after its horizon: both
    // switch inside the render-ahead span, unheard, when the transport stops
    // a cycle later. The channel goes back to what it played at the stop
    // (the arrangement), and cl_2, placed again after cl_1 as it was made,
    // replaces it in the queue, as it did in the in-callback run (where
    // cl_1 was still waiting when cl_2 came).
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    Run r;
    r.launches = {{1000, {request("tr_1", "cl_1")}, model::LaunchQuantize::ThirtySecond},
                  {1002, {request("tr_1", "cl_2")}, model::LaunchQuantize::ThirtySecond}};
    r.stopCycle = 1003;
    const Interrupted cb = executorsAgree(s, p.app->projectDir(), r);
    onlyHeard(cb, 1003 * 32, 0);
    std::size_t first = 0, second = 0;
    for (const LaunchLogEntry& e : cb.log)
        if (e.epoch > cb.epochBefore) (e.clip == id("cl_1") ? first : second) += 1;
    INFO("after the play: cl_1 " << first << ", cl_2 " << second);
    CHECK(second >= 1);  // cl_2 placed again
}

TEST_CASE("a stop, a return and a scene launched unheard are taken back too (F6)" *
          doctest::description("rt")) {
    // cl_1 plays from bar 2 (heard). Then on bar 3 (192000): a stop of the
    // track, a return to the arrangement, or scene sc_2 (cl_2 on tr_1), the
    // transport stopped 160 frames before it: applied ahead by the renderer,
    // taken back, placed again.
    Project p;
    twoAudioClips(p, 240000);  // 5 s: cl_1 still plays at bar 3
    const model::Session s = p.app->head();
    Engine::LaunchRequest stop = backTo("tr_1");
    stop.action = LaunchLogEntry::Action::Stop;
    Engine::LaunchRequest scene = request("tr_1", "cl_2");
    scene.scene = id("sc_2");
    const std::pair<const char*, Engine::LaunchRequest> later[] = {
        {"stop", stop}, {"return", backTo("tr_1")}, {"scene", scene}};
    for (const auto& [name, second] : later) {
        INFO(name);
        Run r;
        r.launches = {{20, {request("tr_1", "cl_1")}, model::LaunchQuantize::Bar},
                      {4000, {second}, model::LaunchQuantize::Bar}};
        r.stopCycle = (192000 - 160) / 32;
        r.relaunch = nullptr;
        const Interrupted cb = executorsAgree(s, p.app->projectDir(), r);
        onlyHeard(cb, r.stopCycle * 32, 0);
    }
}

TEST_CASE("an audio launch's warm-up taken back under a pitch-keeping playrate (F6)" *
          doctest::description("rt")) {
    // Rate 0.75, pitch kept: cl_2 on tr_2 switches at stream 128000 (bar 2)
    // and is applied from its warm-up, 2944 frames before (prewarm, in both
    // executors). Stopped between the warm-up's start and the switch: the
    // warm-up is taken back with the launch; at the next play both
    // executors warm and switch it again alike.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    for (const Interrupt how : {Interrupt::Pause, Interrupt::Locate}) {
        INFO("interrupt " << int(how));
        Run r;
        r.block = 128;
        r.launches = {{20, {request("tr_2", "cl_2")}, model::LaunchQuantize::Bar}};
        r.stopCycle = (128000 - 1000) / 128;
        r.how = how;
        r.relaunch = nullptr;
        r.relaunchAfter = 2000;
        r.frames = 300000;
        const Interrupted cb = executorsAgree(s, p.app->projectDir(), r);
        onlyHeard(cb, r.stopCycle * 128, 0);
    }
}

TEST_CASE("a capture across a stop holds no unheard launch; it is logged once later (F6)" *
          doctest::description("rt")) {
    // A capture runs when the transport stops 160 frames before cl_1's
    // switch on bar 2, which the renderer had applied: the capture (ended by
    // the stop, ADR-0097 L4) holds no cl_1, launch.state has it queued
    // meanwhile, and after the play it is logged once, in the new epoch.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        constexpr std::uint32_t block = 32;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 5});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.play();
        std::int64_t done = 0;
        const auto run = [&](std::int64_t until) {
            if (until <= done) return;
            if (options.anticipative)
                runAheadOnState(engine, backend, until - done, block);
            else
                backend.run(until - done);
            done = until;
        };
        run(20);
        engine.service();
        REQUIRE(engine.captureLaunches(true));
        const Engine::LaunchRequest r = request("tr_1", "cl_1");
        REQUIRE(engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));
        run((96000 - 160) / block);
        engine.service();
        const std::uint64_t stopped = EngineTestAccess::epoch(engine);
        engine.pause();
        run(done + 10);  // the callback leaves the epoch
        engine.service();
        const auto capture = engine.launchCapture();
        REQUIRE(capture);
        INFO(describe(capture->launches));
        CHECK(std::none_of(capture->launches.begin(), capture->launches.end(),
                           [](const LaunchLogEntry& e) { return e.clip == id("cl_1"); }));
        const auto state = engine.launchState();
        const auto tr1 = std::find_if(state.begin(), state.end(),
                                      [](const auto& t) { return t.track == id("tr_1"); });
        REQUIRE(tr1 != state.end());
        CHECK(tr1->plays == Engine::TrackLaunch::Plays::Arrangement);
        REQUIRE(tr1->queued);
        CHECK(tr1->queued->request.clip == id("cl_1"));
        engine.play();
        run(done + 48000 * 2 / block);
        engine.service();
        const auto log = engine.launchLog();
        INFO(describe(log));
        CHECK(std::count_if(log.begin(), log.end(),
                            [](const LaunchLogEntry& e) { return e.clip == id("cl_1"); }) == 1);
        CHECK(std::none_of(log.begin(), log.end(), [&](const LaunchLogEntry& e) {
            return e.clip == id("cl_1") && e.epoch <= stopped;
        }));
        engine.stop();
    }
}

TEST_CASE("a dropped ack for an unheard launch is not a launch unlogged (F6)" *
          doctest::description("rt")) {
    // The renderer's acks dropped (a full ring): stopped before the switch,
    // the dropped ack was for a launch never heard, which is taken back and
    // placed again: nothing is missing from the log. Stopped after it, the
    // launch was heard and the log misses it: unlogged.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const std::int64_t stop : {(96000 - 160) / 32, 3100}) {
        INFO("stopped after cycle " << stop);
        Run r;
        r.launches = {{20, {request("tr_1", "cl_1")}, model::LaunchQuantize::Bar}};
        r.stopCycle = stop;
        r.relaunch = nullptr;
        r.dropRendererAcks = true;
        const Interrupted ah = interrupted(s, p.app->projectDir(), EngineOptions{.workers = 0}, r);
        CHECK(ah.unlogged == (stop * 32 > 96000));
    }
}

namespace {

// An engine on a manual backend, driven cycle by cycle (review round 201's
// probes).
struct Manual {
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend;
    Engine engine;
    bool anticipative;
    std::uint32_t block;
    std::int64_t done = 0;
    Manual(const model::Session& s, const fs::path& dir, const EngineOptions& o, std::uint32_t blk,
           std::int64_t frames)
        : backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames}),
          engine(pool, media, o),
          anticipative(o.anticipative),
          block(blk) {
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = blk}));
        REQUIRE(engine.load(s, dir));
    }
    void run(std::int64_t until) {
        if (until <= done) return;
        if (anticipative)
            runAheadOnState(engine, backend, until - done, block);
        else
            backend.run(until - done);
        done = until;
    }
    std::int64_t place(const Engine::LaunchRequest& r, model::LaunchQuantize q) {
        engine.service();
        const auto placed = engine.launch(std::span(&r, 1), q);
        REQUIRE_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
        return placed->stream;
    }
    // What launch.state says tr_1 plays, and what its node holds.
    std::string plays() const {
        for (const auto& t : engine.launchState())
            if (t.track == id("tr_1"))
                return t.plays == Engine::TrackLaunch::Plays::Clip          ? "clip " + t.clip.str()
                       : t.plays == Engine::TrackLaunch::Plays::Arrangement ? "arrangement"
                                                                            : "other";
        return "?";
    }
    std::string node() const {
        for (const detail::Node& n : EngineTestAccess::latestPlan(engine).nodes)
            if (n.id == id("tr_1") && !n.players.empty()) {
                const auto& L = n.launch;
                if (L.kind == detail::LaunchState::Kind::Clip && L.player >= 0)
                    return "clip " + n.players[std::size_t(L.player)].clip.str();
                return L.kind == detail::LaunchState::Kind::Arrangement ? "arrangement" : "other";
            }
        return "?";
    }
};

}  // namespace

TEST_CASE(
    "two launches closer than the channel's lead, stopped between their switches: the heard "
    "one plays on (round 201 F1)" *
    doctest::description("rt")) {
    // cl_2's take has 40 latent gains: tr_1's lead is 2560 frames, at 300
    // BPM more than a 1/16. A (cl_1 on a 1/32) has its early half applied,
    // then B (cl_2 on the next 1/16) is made, its early half recorded
    // before A's switch; the stop falls 64 frames after A's switch. Taking
    // back B's early half must leave A playing: after the stop the node
    // plays what launch.state says, and from the resume the channel sounds
    // as the run without B until B's switch placed again, or from A where
    // B's fade starts at the play start (the in-callback executor; the
    // renderer's horizon places B past A's lead).
    Project p;
    twoAudioClips(p);
    for (int i = 0; i < 40; ++i) {
        auto fx = j(R"({"plugin": "com.suppleaudio.test.gain"})");
        fx["take"] = takeOf(p, "cl_2");
        const std::string fxId = p.runArgs("fx.add", fx)["fx"].asString();
        auto patch = j(R"({"patch": {"params": {"latency": "short"}}})");
        patch["fx"] = fxId;
        p.runArgs("fx.patch.merge", patch);
    }
    p.run("tempo.set", R"({"points": [{"beat": 0, "bpm": 300}]})");
    const model::Session s = p.app->head();
    for (const Interrupt how : {Interrupt::Locate, Interrupt::Pause}) {
        INFO(std::string(how == Interrupt::Locate ? "locate" : "pause"));
        struct Out {
            std::vector<float> out;
            std::uint32_t latency = 0;
            std::vector<LaunchLogEntry> log;
            std::int64_t resumed = 0;
            std::string plays, node;
        };
        const auto once = [&](bool withB) {
            Out o;
            Manual e(s, p.app->projectDir(), kInCallback, 32, 48000 * 4);
            e.engine.play();
            e.run(200);
            std::int64_t kA = e.place(request("tr_1", "cl_1"), model::LaunchQuantize::ThirtySecond);
            for (int i = 0; i < 3 && (kA / 1200) % 2 == 0; ++i) {  // an odd 32nd
                e.run(e.done + 38);
                kA = e.place(request("tr_1", "cl_1"), model::LaunchQuantize::ThirtySecond);
            }
            REQUIRE((kA / 1200) % 2 == 1);
            e.run((kA - 2560) / 32 + 2);  // A's early half applied
            if (withB) (void)e.place(request("tr_1", "cl_2"), model::LaunchQuantize::Sixteenth);
            e.run((kA + 64 + 31) / 32);  // 64 frames past A's switch
            e.engine.service();
            if (how == Interrupt::Locate) {
                e.engine.seek(0.25);
                e.run(e.done + 2);
            } else {
                e.engine.pause();
                e.run(e.done + 200);
            }
            e.engine.service();
            if (how != Interrupt::Locate) e.engine.play();
            o.resumed = e.done;
            e.run(e.done + 2);
            e.engine.service();
            o.plays = e.plays();
            o.node = e.node();
            e.run(48000 * 4 / 32 - 200);
            e.engine.service();
            o.latency = e.engine.outputLatency();
            o.log = e.engine.launchLog();
            e.engine.stop();
            o.out = e.backend.capture().channels[0];
            return o;
        };
        const Out b = once(true), a = once(false);
        INFO("with B log:" << describe(b.log));
        // After a locate A plays until B's switch placed again; after a
        // pause B is placed again at the play start, its fade from A.
        CHECK(b.plays == (how == Interrupt::Locate ? "clip cl_1" : "clip cl_2"));
        CHECK(b.node == b.plays);
        // From the resume to B's switch placed again (or the resume's fade).
        std::int64_t bAgain = -1;
        for (const LaunchLogEntry& e : b.log)
            if (e.clip == id("cl_2") && e.epoch > b.log.front().epoch) bAgain = e.runningFrame;
        const std::size_t from = std::size_t(b.resumed * 32);
        // (B's own switch fades out A over 64 frames before its frame.)
        const std::size_t n =
            how == Interrupt::Locate && bAgain > 64 ? std::size_t(bAgain) - 64 : 1;
        const std::vector<float> control(a.out.begin() + a.latency + from,
                                         a.out.begin() + a.latency + from + n);
        INFO("with B against A alone from the resume: " << differences(b.out, b.latency + from,
                                                                       control, n));
        CHECK(maxDiff(b.out, b.latency + from, control, n) == 0.0);
        CHECK(std::any_of(control.begin(), control.end(), [](float v) { return v != 0.0f; }));
    }
}

TEST_CASE(
    "an unheard launch is placed again after more than eight epochs between services (round "
    "201 F2)" *
    doctest::description("rt")) {
    // The renderer applies cl_1 (bar 2) ahead; the stop after 2990 cycles
    // leaves it unheard. Then 9 seeks, 2 cycles, 8 seeks and 2 cycles
    // before any service: the stopped epoch's end must still be known. Both
    // executors take it back, the log keeps no entry of the stopped epoch,
    // and they sound alike after.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    std::vector<float> outs[2];
    std::uint32_t latency[2] = {0, 0};
    std::string logs[2];
    for (const bool ahead : {false, true}) {
        INFO(std::string(ahead ? "anticipative" : "in-callback"));
        Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                 48000 * 6);
        e.engine.play();
        e.run(20);
        (void)e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
        e.run(2990);
        e.engine.service();
        const std::uint64_t e0 = EngineTestAccess::epoch(e.engine);
        for (int i = 0; i < 9; ++i) e.engine.seek(0.5);
        e.run(e.done + 2);
        for (int i = 0; i < 8; ++i) e.engine.seek(0.5);
        e.run(e.done + 2);
        e.engine.service();
        CHECK(e.node() == e.plays());
        e.run(e.done + 4000);
        e.engine.service();
        const auto log = e.engine.launchLog();
        INFO(describe(log));
        std::size_t later = 0;
        for (const LaunchLogEntry& le : log) {
            CHECK(le.epoch != e0);
            later += le.clip == id("cl_1");
        }
        CHECK(later == 1);
        logs[ahead] = describe(log);
        latency[ahead] = e.engine.outputLatency();
        e.engine.stop();
        outs[ahead] = e.backend.capture().channels[0];
    }
    const std::size_t from = 2994 * 32, n = 4000 * 32 - 4096;
    const std::vector<float> reference(outs[0].begin() + latency[0] + from,
                                       outs[0].begin() + latency[0] + from + n);
    INFO("anticipative against in-callback: " << differences(outs[1], latency[1] + from, reference,
                                                             n));
    CHECK(maxDiff(outs[1], latency[1] + from, reference, n) == 0.0);
    CHECK(logs[1] == logs[0]);
}

TEST_CASE(
    "a capture read before the stopped epoch is judged, on a shifted channel, is not as heard "
    "(round 201, launch_unsettled)" *
    doctest::description("rt")) {
    // tr_2's latent FX shifts tr_1 by 64 frames. cl_1 on bar 2 (key 96064);
    // paused after 3001 cycles (heard to 96032): unheard. Read at once, before
    // the callback leaves the epoch, a capture that lists cl_1 must not be as
    // heard; read after, it is as heard and lists nothing.
    Project p;
    twoAudioClips(p);
    shiftedChannel(p);
    const model::Session s = p.app->head();
    for (const bool ahead : {false, true}) {
        INFO(std::string(ahead ? "anticipative" : "in-callback"));
        Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                 48000 * 4);
        REQUIRE(EngineTestAccess::launchShift(e.engine, id("tr_1")) == 64);
        e.engine.play();
        e.run(20);
        e.engine.service();
        REQUIRE(e.engine.captureLaunches(true));
        const std::int64_t target = e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
        REQUIRE(target == 96000);
        e.run(3001);
        e.engine.service();
        e.engine.pause();
        const auto window = e.engine.launchCapture();  // no cycle yet
        REQUIRE(window);
        INFO("window:" << describe(window->launches));
        if (!window->launches.empty()) CHECK(window->facts.launchUnsettled);
        e.run(e.done + 10);
        e.engine.service();
        const auto later = e.engine.launchCapture();
        REQUIRE(later);
        CHECK(later->launches.empty());
        CHECK(later->facts == Engine::CaptureFacts{.fromPlayStart = later->facts.fromPlayStart});
        e.engine.stop();
    }
}

TEST_CASE(
    "a renderer that leaves an epoch before the callback waits for its verdict: logged once, "
    "the capture not as heard (round 201 F2)" *
    doctest::description("rt")) {
    // The renderer applies cl_1 (bar 2) ahead; the transport is located
    // after 2990 cycles (unheard) or 3010 (heard). With the test hook the
    // renderer's leave acts as a timed-out wait for the callback: it applies
    // nothing until the callback's end is known, then takes back by it. The
    // callback's verdict is the only one: the log equals the in-callback
    // run's (cl_1 once: placed again, or kept), the node plays what
    // launch.state says, and the capture running across is not as heard.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const std::int64_t stop : {2990, 3010}) {
        INFO("stopped after cycle " << stop);
        std::string logs[2];
        for (const bool ahead : {false, true}) {
            INFO(std::string(ahead ? "anticipative, its leave late" : "in-callback"));
            Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                     48000 * 6);
            e.engine.play();
            e.run(20);
            e.engine.service();
            REQUIRE(e.engine.captureLaunches(true));
            (void)e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
            e.run(stop);
            e.engine.service();
            EngineTestAccess::rendererLeaveLate(e.engine, ahead);
            e.engine.seek(0.5);
            e.run(e.done + 2);
            e.engine.service();
            EngineTestAccess::rendererLeaveLate(e.engine, false);
            e.run(e.done + 8);
            e.engine.service();
            CHECK(e.node() == e.plays());
            const auto capture = e.engine.launchCapture();
            REQUIRE(capture);
            if (ahead) CHECK(capture->facts.launchUnsettled);
            e.run(e.done + 3000);
            e.engine.service();
            const auto log = e.engine.launchLog();
            std::size_t cl1 = 0;
            for (const LaunchLogEntry& le : log) cl1 += le.clip == id("cl_1");
            CHECK(cl1 == 1);
            logs[ahead] = describe(log);
            e.engine.stop();
        }
        CHECK(logs[1] == logs[0]);
    }
}

TEST_CASE("a heard launch stays heard however many epochs pass, playing or stopped (round 203 F1)" *
          doctest::description("rt")) {
    // cl_1 heard on bar 2 (played past it), then:
    //  - 70 seeks, each with 2 cycles and a service (H2): the log keeps cl_1,
    //    launch.state and the node agree after every seek, and a capture
    //    started after them lists cl_1 and is as heard;
    //  - 65 seeks of one cycle each before a service (H; epoch 0's ring
    //    entry reused before any service read it): as 62;
    //  - paused, 70 seeks while stopped with a service each, then play (H3):
    //    as 10.
    // Both executors.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    const auto heardThen = [&](Manual& e) {
        e.engine.play();
        e.run(20);
        (void)e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
        e.run(3100);
        e.engine.service();
    };
    const auto cl1 = [](const std::vector<LaunchLogEntry>& log) {
        std::size_t n = 0;
        for (const LaunchLogEntry& le : log) n += le.clip == id("cl_1");
        return n;
    };
    for (const bool ahead : {false, true}) {
        INFO(std::string(ahead ? "anticipative" : "in-callback"));
        const EngineOptions o = ahead ? EngineOptions{.workers = 0} : kInCallback;
        {  // H2
            Manual e(s, p.app->projectDir(), o, 32, 48000 * 6);
            heardThen(e);
            for (int i = 1; i <= 70; ++i) {
                INFO("after seek " << i);
                e.engine.seek(2.5);
                e.run(e.done + 2);
                e.engine.service();
                CHECK(cl1(e.engine.launchLog()) == 1);
                CHECK(e.plays() == "clip cl_1");
                CHECK(e.node() == e.plays());
            }
            REQUIRE(e.engine.captureLaunches(true));
            e.run(e.done + 100);
            e.engine.service();
            const auto capture = e.engine.launchCapture();
            REQUIRE(capture);
            CHECK(cl1(capture->launches) == 1);
            CHECK(capture->facts == Engine::CaptureFacts{});
            e.engine.stop();
        }
        // H, H3: the many runs against their controls, from the last seek or play.
        const auto seeksThen = [&](int seeks, bool stopped) {
            Manual e(s, p.app->projectDir(), o, 32, 48000 * 8);
            heardThen(e);
            if (stopped) {
                e.engine.pause();
                e.run(e.done + 10);
                e.engine.service();
            }
            for (int i = 0; i < seeks; ++i) {
                e.engine.seek(2.5);
                e.run(e.done + (stopped ? 2 : 1));
                if (stopped) e.engine.service();
            }
            e.engine.service();
            if (stopped) e.engine.play();
            const std::int64_t from = e.done;
            e.run(e.done + 4);
            e.engine.service();
            CHECK(e.node() == e.plays());
            CHECK(e.plays() == "clip cl_1");
            e.run(e.done + 3000);
            e.engine.service();
            CHECK(cl1(e.engine.launchLog()) == 1);
            const std::uint32_t latency = e.engine.outputLatency();
            e.engine.stop();
            const auto& out = e.backend.capture().channels[0];
            return std::vector<float>(out.begin() + latency + std::size_t(from) * 32,
                                      out.begin() + latency + std::size_t(from + 2900) * 32);
        };
        for (const bool stopped : {false, true}) {
            INFO((stopped ? "H3: scrubbing while stopped" : "H: seeks before a service"));
            const auto many = seeksThen(stopped ? 70 : 65, stopped);
            const auto few = seeksThen(stopped ? 10 : 62, stopped);
            INFO(differences(many, 0, few, few.size()));
            CHECK(many == few);
            CHECK(std::any_of(few.begin(), few.end(), [](float v) { return v != 0.0f; }));
        }
    }
}

TEST_CASE(
    "a capture over the frames a late renderer leave rendered is not as heard (round 203 F2)" *
    doctest::description("rt")) {
    // As the late-leave test: cl_1 applied ahead and unheard (2990 cycles),
    // the renderer's leave late at a seek to 2.5 s. A capture armed just
    // before the seek (pending across it) or turned on just after starts in
    // the new epoch, over frames the renderer rendered with cl_1 before it
    // settled: not as heard. The in-callback run's same captures are.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const bool before : {true, false}) {
        INFO(std::string(before ? "armed before the seek" : "turned on after it"));
        for (const bool ahead : {false, true}) {
            INFO(std::string(ahead ? "anticipative, its leave late" : "in-callback"));
            Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                     48000 * 6);
            e.engine.play();
            e.run(20);
            (void)e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
            e.run(2990);
            e.engine.service();
            EngineTestAccess::rendererLeaveLate(e.engine, ahead);
            if (before) REQUIRE(e.engine.captureLaunches(true));
            e.engine.seek(2.5);
            if (!before) REQUIRE(e.engine.captureLaunches(true));
            e.run(e.done + 1);
            e.engine.service();
            EngineTestAccess::rendererLeaveLate(e.engine, false);
            e.run(e.done + 400);
            e.engine.service();
            const auto capture = e.engine.launchCapture();
            REQUIRE(capture);
            REQUIRE(capture->placed);
            CHECK(capture->facts.launchUnsettled == ahead);
            e.engine.stop();
        }
    }
}

TEST_CASE("a long play without a stop never runs out of launch slots (round 205 F1)" *
          doctest::description("rt")) {
    // cl_1 and cl_2 launched on tr_1 in turn on sixteenths, 400 cycles (more
    // than a sixteenth and the render-ahead span) and a service after each, 300 times in one play:
    // a heard launch's slot is free again while its epoch runs on (the 257th was refused with
    // launch.queue at e9aabe53). Both executors.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const bool ahead : {false, true}) {
        INFO(std::string(ahead ? "anticipative" : "in-callback"));
        Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                 48000 * 82);
        e.engine.play();
        e.run(20);
        int refused = 0;
        for (int i = 0; i < 300; ++i) {
            e.engine.service();
            const Engine::LaunchRequest r = request("tr_1", i % 2 ? "cl_2" : "cl_1");
            const auto placed = e.engine.launch(std::span(&r, 1), model::LaunchQuantize::Sixteenth);
            if (!placed) {
                INFO("launch " << i + 1 << ": " << placed.error().toString());
                CHECK(placed);
                if (++refused > 3) break;
            }
            e.run(e.done + 400);
        }
        e.engine.service();
        CHECK(refused == 0);
        CHECK(e.engine.launchLog().size() == 300);
        CHECK(EngineTestAccess::slotsHeld(e.engine) == 0);
        e.engine.stop();
    }
}

TEST_CASE("a launch made after many unserviced seeks keeps a heard launch (round 205 F2)" *
          doctest::description("rt")) {
    // cl_1 heard on bar 2, no service from there (its ack unread), then 70
    // seeks of one cycle each (epoch 0's ring entry reused), then cl_2
    // launched on the next bar before any service: the launch reads the
    // acks first, and the heard
    // launch's slot, reused, has its entry settled as heard. The log keeps
    // cl_1, launch.state and the node agree, and a capture started then
    // lists cl_1. Controls: 62 seeks, and 70 with a service before the
    // launch. Both executors.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const bool ahead : {false, true})
        for (const auto& [seeks, serviced] :
             {std::pair{70, false}, std::pair{62, false}, std::pair{70, true}}) {
            INFO((ahead ? "anticipative, " : "in-callback, ")
                 << seeks << " seeks" << (serviced ? ", serviced" : ""));
            Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                     48000 * 8);
            e.engine.play();
            e.run(20);
            (void)e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
            e.run(3100);  // heard; no service from here, so its ack is not read
            for (int i = 0; i < seeks; ++i) {
                e.engine.seek(2.5);
                e.run(e.done + 1);
            }
            if (serviced) e.engine.service();
            const Engine::LaunchRequest r = request("tr_1", "cl_2");
            REQUIRE(e.engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));
            e.engine.service();
            REQUIRE(e.engine.captureLaunches(true));
            e.run(e.done + 50);
            e.engine.service();
            std::size_t cl1 = 0;
            for (const LaunchLogEntry& le : e.engine.launchLog()) cl1 += le.clip == id("cl_1");
            CHECK(cl1 == 1);
            CHECK(e.plays() == "clip cl_1");
            CHECK(e.node() == e.plays());
            const auto capture = e.engine.launchCapture();
            REQUIRE(capture);
            REQUIRE(capture->launches.size() == 1);
            CHECK(capture->launches[0].clip == id("cl_1"));
            CHECK(capture->facts == Engine::CaptureFacts{});
            e.engine.stop();
        }
}

TEST_CASE("a seek while a late renderer leave is pending moves the late span on (round 205 F3)" *
          doctest::description("rt")) {
    // cl_1 applied ahead and unheard (2990 cycles); the renderer's leave
    // late at a seek to 2.5 s, then a second seek to 3 s while it is still
    // pending. A capture started in the second epoch, over frames the
    // renderer rendered before it settled, is not as heard; the launch is
    // placed again and logged once. In-callback, the same capture is as
    // heard.
    Project p;
    twoAudioClips(p);
    const model::Session s = p.app->head();
    for (const bool ahead : {false, true}) {
        INFO(std::string(ahead ? "anticipative, its leave late" : "in-callback"));
        Manual e(s, p.app->projectDir(), ahead ? EngineOptions{.workers = 0} : kInCallback, 32,
                 48000 * 8);
        e.engine.play();
        e.run(20);
        (void)e.place(request("tr_1", "cl_1"), model::LaunchQuantize::Bar);
        e.run(2990);
        e.engine.service();
        EngineTestAccess::rendererLeaveLate(e.engine, ahead);
        e.engine.seek(2.5);
        e.run(e.done + 1);
        e.engine.seek(3.0);
        REQUIRE(e.engine.captureLaunches(true));
        e.run(e.done + 1);
        e.engine.service();
        EngineTestAccess::rendererLeaveLate(e.engine, false);
        e.run(e.done + 400);
        e.engine.service();
        const auto capture = e.engine.launchCapture();
        REQUIRE(capture);
        REQUIRE(capture->placed);
        CHECK(capture->facts.launchUnsettled == ahead);
        e.run(e.done + 3000);
        e.engine.service();
        std::size_t cl1 = 0;
        for (const LaunchLogEntry& le : e.engine.launchLog()) cl1 += le.clip == id("cl_1");
        CHECK(cl1 == 1);
        CHECK(e.node() == e.plays());
        CHECK(EngineTestAccess::slotsHeld(e.engine) == 0);
        e.engine.stop();
    }
}
