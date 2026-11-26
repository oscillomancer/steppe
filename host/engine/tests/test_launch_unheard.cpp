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
    // a launch never heard.
    for (const bool latent : {false, true}) {
        Project p;
        if (latent)
            latentClip(p);
        else
            twoAudioClips(p);
        const model::Session s = p.app->head();
        for (const std::int64_t stop : {2990, 2998, 2999, 3000, 3001, 3010})
            for (const Interrupt how :
                 {Interrupt::Pause, Interrupt::PauseAndReturn, Interrupt::Locate}) {
                INFO("latent " << latent << ", stopped after cycle " << stop << ", interrupt "
                               << int(how));
                Run r;
                r.launches = {{20, {request("tr_1", "cl_1")}, model::LaunchQuantize::Bar}};
                r.stopCycle = stop;
                r.how = how;
                const Interrupted cb = executorsAgree(s, p.app->projectDir(), r);
                onlyHeard(cb, stop * 32, 0);
                // While stopped, launch.state has an unheard launch queued.
                const auto tr1 = std::find_if(cb.stopped.begin(), cb.stopped.end(),
                                              [](const auto& t) { return t.track == id("tr_1"); });
                REQUIRE(tr1 != cb.stopped.end());
                const bool heard = stop * 32 > 96000;
                CHECK((tr1->plays == Engine::TrackLaunch::Plays::Clip) == heard);
                if (!heard && how != Interrupt::Locate) CHECK(tr1->queued.has_value());
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
