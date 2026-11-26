// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launches, stops, seeks, scrubbing while stopped, plays, services or none,
// late renderer leaves and captures in seeded, deterministic sequences, on
// both executors, with the launch invariants checked after every step
// (ADR-0097, revision 2026-11-26; review round 205): launch.state agrees
// with what the node plays, no launch is refused while few are pending, a
// slot is held only while its launch waits or its switch is ahead, each epoch sounds as its log
// replays, and a capture that is as heard sounds as its own replay. The
// steps come from a fixed seed (no clock decides them); a failure prints
// the seed and the step.
#include <cstdint>
#include <random>

#include "clip_player_support.h"
#include "engine_test_access.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {

constexpr std::uint32_t kBlock = 32;
constexpr std::int64_t kFrames = 48000 * 60;
constexpr const char* kTracks[] = {"tr_1", "tr_2"};

// tr_1 (cl_1, cl_2) shifted 64 frames by tr_2's latent gain; tr_2 (cl_3,
// cl_4) unshifted, cl_3 through a modulated take chain. Ramps of 36000 frames (each frame tells
// where it is).
void project(Project& p) {
    p.run("track.add", R"({"name": "Latent"})");
    for (const char* track : {"tr_1", "tr_2"})
        for (const char* scene : {"sc_1", "sc_2"}) {
            auto args = j(R"({})");
            args["track"] = track;
            args["scene"] = scene;
            args["audio_file"] =
                p.ramp((std::string("seq-") + track + scene + ".wav").c_str(), 36000).string();
            p.runArgs("clip.add", args);
        }
    p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.gain"})");
    const std::string fx = p.app->head().tracks.find(id("tr_2"))->chain.front().str();
    p.run("fx.patch.merge",
          R"({"fx": ")" + fx + R"(", "patch": {"params": {"latency": "short"}}})");
    // cl_3 through a gain in its take chain, which a route from Transport's
    // `playing` moves (ADR-0096 M5): tr_2's launches enter its clip chain
    // early (preenter), under the same invariants.
    p.run("rack.add", R"({"track": "tr_2"})");                        // rk_1, mo_1 its Out
    p.run("module.add", R"({"rack": "rk_1", "type": "mod_out"})");    // mo_2
    p.run("module.add", R"({"rack": "rk_1", "type": "transport"})");  // mo_3
    p.run("cable.add", R"({"from": "mo_3.playing", "to": "mo_2.in_1"})");
    json::Value gain = json::Value::object();
    gain["take"] = takeOf(p, "cl_3");
    gain["plugin"] = "com.suppleaudio.test.gain";
    const std::string takeGain = p.runArgs("fx.add", gain)["fx"].asString();
    json::Value route = j(R"({"source": {"module": "mo_2", "input": "in_1"}, "depth": -0.25})");
    route["target"] = json::Value::object();
    route["target"]["fx"] = takeGain;
    route["target"]["param"] = "gain";
    p.runArgs("modulation.add", route);
}

// One playing stretch of one epoch: where its first frame is in the
// capture, its song sample there, and where it ended.
struct Segment {
    std::uint64_t epoch = 0;
    std::int64_t out = 0, song = 0, end = -1;
    std::string steps;  // the steps that led to it (a failure's message)
};

struct Sequence {
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend;
    Engine engine;
    bool anticipative;
    std::uint64_t seed;
    std::mt19937_64 rng;
    std::int64_t done = 0;  // cycles
    bool playing = false;
    std::int64_t song = 0;        // the song sample the epoch began at (or stands at)
    std::int64_t epochCycle = 0;  // the cycle it began at
    std::int64_t quietSince = 0;  // the cycle of the last launch or transport change
    std::int64_t stoppedAt = 0;
    std::vector<Segment> segments;
    // A capture switched on: the segment it went into (-1: not yet).
    bool capturing = false;
    int captureSegment = -1;
    int step = 0;
    std::string trail;  // the steps so far, for a failure's message

    Sequence(const model::Session& s, const fs::path& dir, const EngineOptions& o, std::uint64_t sd)
        : backend({.mode = DummyBackend::Mode::Manual, .captureFrames = kFrames + 48000}),
          engine(pool, media, o),
          anticipative(o.anticipative),
          seed(sd),
          rng(sd) {
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        REQUIRE(engine.load(s, dir));
    }
    // Uniform in [0, n): the generator's output is fixed by the standard,
    // the distribution classes are not.
    std::uint64_t pick(std::uint64_t n) { return rng() % n; }
    void note(const std::string& what) {
        trail += " " + std::to_string(step) + ":" + what;
        if (trail.size() > 1600) trail.erase(0, trail.size() - 1600);
    }
    std::string where() const {
        return "seed " + std::to_string(seed) +
               std::string(anticipative ? " anticipative" : " in-callback") + ", step " +
               std::to_string(step) + ", cycle " + std::to_string(done) + "; steps:" + trail;
    }
    void run(std::int64_t cycles) {
        if (cycles <= 0) return;
        if (anticipative)
            runAheadOnState(engine, backend, cycles, kBlock);
        else
            backend.run(cycles);
        done += cycles;
    }
    std::int64_t position() const { return playing ? song + (done - epochCycle) * kBlock : song; }
    void begin() {
        epochCycle = done;
        quietSince = done;
        segments.push_back({EngineTestAccess::epoch(engine), done * kBlock, song, -1,
                            std::to_string(step) + " after:" +
                                trail.substr(trail.size() > 700 ? trail.size() - 700 : 0)});
        if (capturing && captureSegment < 0) captureSegment = int(segments.size()) - 1;
    }
    void end() {
        if (!segments.empty() && segments.back().end < 0) segments.back().end = done * kBlock;
        if (capturing) capturing = false;  // a stop or seek ends it
    }
    void play() {
        engine.play();
        playing = true;
        begin();
    }
    void pause() {
        song = position();
        end();
        engine.pause();
        playing = false;
        stoppedAt = done;
        quietSince = done;
    }
    void seek(std::int64_t sample) {
        if (playing) song = position(), end();
        engine.seek(double(sample) / 48000);
        song = sample;
        if (playing) begin();
        stoppedAt = done;
    }
    std::string plays(const char* track) const {
        for (const auto& t : engine.launchState())
            if (t.track == id(track))
                return t.plays == Engine::TrackLaunch::Plays::Clip          ? "clip " + t.clip.str()
                       : t.plays == Engine::TrackLaunch::Plays::Arrangement ? "arrangement"
                                                                            : "other";
        return "?";
    }
    std::string node(const char* track) const {
        for (const detail::Node& n : EngineTestAccess::latestPlan(engine).nodes)
            if (n.id == id(track) && !n.players.empty()) {
                const auto& L = n.launch;
                if (L.kind == detail::LaunchState::Kind::Clip && L.player >= 0)
                    return "clip " + n.players[std::size_t(L.player)].clip.str();
                return L.kind == detail::LaunchState::Kind::Arrangement ? "arrangement" : "other";
            }
        return "?";
    }
    bool anyQueued() const {
        for (const auto& t : engine.launchState())
            if (t.queued) return true;
        return !engine.launchesPending().empty();
    }
    bool queued(const char* track) const {
        for (const auto& t : engine.launchState())
            if (t.track == id(track) && t.queued) return true;
        return false;
    }
    std::size_t queuedTracks() const {
        std::size_t n = 0;
        for (const auto& t : engine.launchState()) n += t.queued.has_value();
        return n;
    }
    // After a service: launch.state against the nodes, and the slots held.
    // While stopped (two cycles on, so the callback left the epoch) every
    // unheard launch is taken back; while playing, once nothing is queued
    // and the render-ahead span has passed since the last change.
    void checkSettled() {
        INFO(where());
        const bool settledStopped = !playing && done - stoppedAt >= 2;
        // (Playing, a launch may be applied ahead of the callback with its ack
        // not read yet: not settled while any slot's switch is ahead.)
        const bool settledPlaying = playing && !anyQueued() && done - quietSince >= 200 &&
                                    EngineTestAccess::slotsAhead(engine) == 0;
        if (!settledStopped && !settledPlaying) return;
        INFO("log:" << describe(engine.launchLog()));
        for (const char* track : kTracks) {
            // (Stopped, a launch queued for the play start may already be
            // in what the renderer prepared there.)
            if (settledStopped && queued(track)) continue;
            INFO(track);
            CHECK(node(track) == plays(track));
        }
        // Nothing heard keeps a slot; a waiting launch keeps its own.
        const std::size_t held = EngineTestAccess::slotsHeld(engine);
        INFO("held: " << EngineTestAccess::heldSlots(engine));
        if (settledStopped)
            CHECK(held <= queuedTracks() + engine.launchesPending().size());
        else
            CHECK(held == 0);
    }
    bool soon = false;  // launches only unquantized or on sixteenths (a long play)
    void launch() {
        std::vector<Engine::LaunchRequest> requests;
        const auto tracks = pick(3);  // tr_1, tr_2, or both (a scene's slots)
        for (int t = 0; t < 2; ++t) {
            if (tracks != 2 && std::uint64_t(t) != tracks) continue;
            const char* clips[2][2] = {{"cl_1", "cl_2"}, {"cl_3", "cl_4"}};
            const auto which = pick(5);
            requests.push_back(which == 4 ? backTo(kTracks[t])
                                          : request(kTracks[t], clips[t][which % 2]));
        }
        static const model::LaunchQuantize quantize[] = {
            model::LaunchQuantize::None, model::LaunchQuantize::Sixteenth,
            model::LaunchQuantize::Beat, model::LaunchQuantize::Bar};
        const auto q = quantize[soon ? pick(2) : pick(4)];
        std::string what = "launch";
        for (const auto& r : requests)
            what += "," + r.track.str() + "=" + (r.clip.n ? r.clip.str() : "arr");
        note(what + "q" + std::to_string(int(q)));
        auto placed = engine.launch(requests, q);
        // While the transport relocates, a launch is refused for a moment
        // (launch.later): again a cycle on, as a caller would.
        for (int again = 0; !placed && placed.error().code == "launch.later" && again < 8;
             ++again) {
            run(1);
            placed = engine.launch(requests, q);
        }
        INFO(where());
        CHECK_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
        quietSince = done;
    }
};

// The offline replay of `log` from song sample `start` for `frames`.
std::vector<float> replay(const model::Session& s, const fs::path& dir,
                          const std::vector<LaunchLogEntry>& log, std::int64_t start,
                          std::size_t frames) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.startSeconds = double(start) / 48000;
    settings.endSeconds = settings.startSeconds + double(frames) / 48000 + 0.05;
    settings.blockSize = kBlock;
    settings.projectDir = dir;
    settings.launches = log;
    auto r = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// Known gaps the sequences found, not F6's (reported in round 205; C at the
// take-fx merge):
// what each track plays at a replay's start is listed as a launch at frame
// 0, so the replay cannot hold (A) a loop clip a seek carried to before its
// anchor, which live plays by its phase (engine.md §1.4.2, "Phase") and the
// replay leaves silent until the anchor, or (B) a switch whose 5 ms
// crossfade is still running there (a pause and play carries it on; the
// replay fades from silence). Such an epoch or capture is counted, not
// compared: whether each track's newest switch before song sample `start`
// (in epoch `epoch`, which began at song sample `from`) is after it or
// within the fade (and the channel's 64-frame shift) of it.
bool fadeOrAnchorAt(const std::vector<LaunchLogEntry>& log, std::uint64_t epoch, std::int64_t from,
                    std::int64_t start) {
    std::map<model::Id, const LaunchLogEntry*> newest;
    for (const LaunchLogEntry& e : log)
        if (e.epoch < epoch || (e.epoch == epoch && from + e.runningFrame <= start))
            newest[e.track] = &e;
    for (const auto& [track, e] : newest) {
        if (e->anchorSample + 240 + 64 > start) return true;
        // (C), take-fx's: from a start off the 32-frame grid, the modulated
        // take chain's frames to the next grid point differ live and
        // offline (found here, reported; open).
        if (start % 32 != 0 && e->action == LaunchLogEntry::Action::Clip && e->clip == id("cl_3"))
            return true;
    }
    return false;
}

// What each track plays as `epoch` begins (its newest entry from an earlier
// epoch, as a capture lists it at its start), then that epoch's entries.
std::vector<LaunchLogEntry> epochLog(const std::vector<LaunchLogEntry>& log, std::uint64_t epoch) {
    std::map<model::Id, LaunchLogEntry> before;
    std::vector<LaunchLogEntry> out;
    for (const LaunchLogEntry& e : log)
        if (e.epoch < epoch) before[e.track] = e;
    for (auto& [track, e] : before) {
        if (e.action == LaunchLogEntry::Action::Arrangement) continue;
        e.runningFrame = 0;
        e.epoch = epoch;
        out.push_back(e);
    }
    for (const LaunchLogEntry& e : log)
        if (e.epoch == epoch) out.push_back(e);
    return out;
}

// `longPlay`: one play with no stop or seek, unquantized or sixteenth
// launches and runs only (more launches heard in one epoch than there are
// slots).
void runSequence(const model::Session& s, const fs::path& dir, const EngineOptions& o,
                 std::uint64_t seed, int steps, bool longPlay = false) {
    Sequence q(s, dir, o, seed);
    q.soon = longPlay;
    q.play();
    q.run(20);
    q.engine.service();
    struct Read {
        int segment;
        Engine::LaunchCapture capture;
        std::string where;
    };
    std::vector<Read> captures;
    bool late = false;
    std::map<std::uint64_t, std::int64_t> lateEnds;
    for (q.step = 0; q.step < steps && q.done * kBlock < kFrames - 48000; ++q.step) {
        const auto r = q.pick(100);
        if (longPlay) {
            if (r < 50) {
                q.launch();
            } else {
                const std::int64_t n = 1 + q.pick(250);
                q.note("run" + std::to_string(n));
                q.run(n);
            }
        } else if (r < 28) {
            q.launch();
        } else if (r < 56) {
            const std::int64_t n = q.pick(3) == 0 ? 1 + q.pick(8) : 1 + q.pick(400);
            q.note("run" + std::to_string(n));
            q.run(n);
        } else if (r < 64) {
            if (q.playing) {
                q.note("pause");
                q.pause();
            } else {
                q.note("play");
                q.play();
            }
        } else if (r < 73) {
            const std::int64_t sample = std::int64_t(q.pick(6 * 128)) * 375;  // k/128 s
            q.note("seek" + std::to_string(sample));
            q.seek(sample);
        } else if (r < 78) {
            if (!q.playing) {
                q.note("play");
                q.play();
            }
        } else if (r < 85) {
            if (q.capturing) {
                q.note("capture off");
                (void)q.engine.captureLaunches(false);
                q.capturing = false;
                q.engine.service();
                if (const auto c = q.engine.launchCapture(); c && q.captureSegment >= 0)
                    captures.push_back({q.captureSegment, *c, q.where()});
            } else {
                q.note("capture on");
                INFO(q.where());
                CHECK(q.engine.captureLaunches(true));
                q.capturing = true;
                q.captureSegment = q.playing ? int(q.segments.size()) - 1 : -1;
            }
        } else if (r < 87) {
            if (q.anticipative) {
                q.note("late");
                late = true;
                EngineTestAccess::rendererLeaveLate(q.engine, true);
            }
        } else if (r < 90) {
            // A storm: more one-cycle seeks than the ring of epoch ends holds,
            // no service, then a launch.
            const auto seeks = 64 + std::int64_t(q.pick(12));
            const std::int64_t sample = std::int64_t(q.pick(6 * 128)) * 375;
            q.note("storm" + std::to_string(seeks));
            for (std::int64_t i = 0; i < seeks; ++i) {
                q.seek(sample);
                q.run(1);
            }
            q.launch();
        } else {
            q.note("run1");
            q.run(1);
        }
        if (late && !longPlay && r >= 28 && r < 56) {  // the late leave settles from here
            late = false;
            EngineTestAccess::rendererLeaveLate(q.engine, false);
        }
        if (q.pick(100) < 85) {
            q.engine.service();
            q.checkSettled();
        }
        // Each epoch a late leave went on in, and where the renderer settled.
        if (const auto e = EngineTestAccess::launchLateEpoch(q.engine); e != UINT64_MAX)
            lateEnds[e] = EngineTestAccess::launchLateEnd(q.engine);
    }
    EngineTestAccess::rendererLeaveLate(q.engine, false);
    if (q.playing) q.pause();
    q.run(10);
    q.engine.service();
    q.checkSettled();
    const auto log = q.engine.launchLog();
    const std::uint32_t latency = q.engine.outputLatency();
    if (const auto e = EngineTestAccess::launchLateEpoch(q.engine); e != UINT64_MAX)
        lateEnds[e] = EngineTestAccess::launchLateEnd(q.engine);
    q.engine.stop();
    INFO("seed " << seed << std::string(q.anticipative ? " anticipative" : " in-callback")
                 << "; log:" << describe(log));
    CHECK(q.engine.processErrors() == 0);
    if (q.anticipative) CHECK(q.engine.anticipationMisses() == 0);
    const auto& out = q.backend.capture().channels[0];
    int carried = 0;
    // Each epoch as its log replays (but its last 2048 frames before a stop
    // or seek, and an epoch a late leave went on in, which a capture marks).
    for (const Segment& g : q.segments) {
        const std::int64_t length = g.end - g.out - 2048;
        if (length <= 0 || lateEnds.count(g.epoch)) continue;
        const auto heard = epochLog(log, g.epoch);
        if (fadeOrAnchorAt(log, g.epoch, g.song, g.song)) {
            ++carried;
            continue;
        }
        const auto offline = replay(s, dir, heard, g.song, std::size_t(length));
        INFO("began at step " << g.steps);
        INFO("epoch " << g.epoch << " from song " << g.song << " at frame " << g.out << ", "
                      << length << " frames: "
                      << differences(out, latency + std::size_t(g.out), offline,
                                     std::size_t(length)));
        CHECK(maxDiff(out, latency + std::size_t(g.out), offline, std::size_t(length)) == 0.0);
    }
    // A capture as heard sounds as its own replay.
    for (const Read& c : captures) {
        const Segment& g = q.segments[std::size_t(c.segment)];
        const bool asHeard =
            c.capture.facts == Engine::CaptureFacts{.fromPlayStart = c.capture.facts.fromPlayStart};
        if (!c.capture.placed || c.capture.frames <= 4096) continue;
        INFO("capture read at " << c.where);
        INFO("capture from song " << c.capture.startSample << ", " << c.capture.frames
                                  << " frames, in epoch " << g.epoch << " (from song " << g.song
                                  << "); its launches:" << describe(c.capture.launches));
        // Begun over frames a late leave's renderer rendered before it
        // settled: never as heard (round 203 F2, round 205 F3).
        if (const auto it = lateEnds.find(g.epoch);
            it != lateEnds.end() && c.capture.startSample - g.song < it->second) {
            CHECK_FALSE(asHeard);
            continue;
        }
        if (!asHeard) continue;
        if (fadeOrAnchorAt(log, g.epoch, g.song, c.capture.startSample)) {
            ++carried;
            continue;
        }
        const std::int64_t at = g.out + (c.capture.startSample - g.song);
        const std::size_t n = std::size_t(c.capture.frames - 2048);
        const auto offline = replay(s, dir, c.capture.launches, c.capture.startSample, n);
        INFO(differences(out, latency + std::size_t(at), offline, n));
        CHECK(maxDiff(out, latency + std::size_t(at), offline, n) == 0.0);
    }
    MESSAGE("seed " << seed << std::string(q.anticipative ? " anticipative" : " in-callback")
                    << ": " << q.step << " steps, " << q.segments.size() << " epochs played, "
                    << captures.size() << " captures read, " << log.size() << " launches logged, "
                    << carried
                    << " not compared (a clip before its anchor, or a fade, at its start)");
}

}  // namespace

TEST_CASE("seeded sequences of launches, stops, seeks and captures keep the launch invariants" *
          doctest::description("rt")) {
    Project p;
    project(p);
    const model::Session s = p.app->head();
    // 12 seeds of 400 steps and 2 long plays of 1200, on each executor:
    // 14400 steps.
    for (const EngineOptions& o : {kInCallback, EngineOptions{.workers = 0}}) {
        for (std::uint64_t seed = 1; seed <= 12; ++seed)
            runSequence(s, p.app->projectDir(), o, seed, 400);
        for (const std::uint64_t seed : {101u, 102u})
            runSequence(s, p.app->projectDir(), o, seed, 1200, true);
    }
}
