// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launches (clips, returns, stops, scenes; audio and MIDI, a Notes feed
// following the MIDI track), stops, seeks, the transport loop, plan swaps,
// plays, services or none, late renderer leaves and captures (ended by
// turning them off, a seek, a stop or a loop change) in seeded sequences,
// run on both executors in lockstep, with the launch invariants checked
// after every step (ADR-0097, revision 2026-11-26; review rounds 205 and
// 207): launch.state agrees with what the node plays, and on both
// executors; what a track plays, settled, is the newest launch made on it;
// no launch is refused while few are pending; a slot is held only while
// its launch waits or its switch is ahead; each epoch sounds as its log
// replays, and as the other executor's where their logs agree; a capture
// that is as heard sounds as its own replay. The steps come from a fixed
// seed (no clock decides them), and the anticipative renderer is let run
// until it can hold no more before every step's actions, so a run repeats
// from its seed on both executors (checked); a failure prints the seed and
// the step.
#include <cstdint>
#include <cstdlib>
#include <random>

#include "clip_player_support.h"
#include "engine_test_access.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {

constexpr std::uint32_t kBlock = 32;
constexpr std::int64_t kFrames = 48000 * 60;
constexpr std::int64_t kFade = 240 + 64;  // a switch's crossfade and the channel shift
constexpr const char* kTracks[] = {"tr_1", "tr_2", "tr_3"};
constexpr const char* kClips[3][2] = {{"cl_1", "cl_2"}, {"cl_3", "cl_4"}, {"cl_5", "cl_6"}};
constexpr const char* kScenes[] = {"sc_1", "sc_2"};

// tr_1 (cl_1, cl_2) shifted 64 frames by tr_2's latent gain; tr_2 (cl_3,
// cl_4) unshifted: loop clips of 36000-frame ramps (each frame tells where
// it is). tr_3 plays the sine from MIDI clips cl_5 and cl_6; tr_4's rack
// turns tr_3's notes into its gate through a Notes module following tr_3
// (`liveFeed`: with a device, so the rack is live and the anticipative
// engine runs it in the callback).
void project(Project& p, bool liveFeed) {
    p.run("track.add", R"({"name": "Latent"})");
    for (const char* track : {"tr_1", "tr_2"})
        for (const char* scene : kScenes) {
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
    p.run("track.add", R"({"name": "Keys"})");  // tr_3
    p.run("fx.add", R"({"track": "tr_3", "plugin": "com.suppleaudio.test.sine"})");
    p.run(
        "clip.add",
        R"({"track": "tr_3", "scene": "sc_1", "notation": "voice bass; 1: C3:e r:e C3:e r:e r:h"})");
    p.run("clip.add",
          R"({"track": "tr_3", "scene": "sc_2", "notation": "voice bass; 1: G3:q r:q A3:q r:q"})");
    p.run("track.add", R"({"name": "Gates"})");  // tr_4
    p.run("rack.add", R"({"track": "tr_4"})");   // rk_1, its Out mo_1
    p.run("module.add",
          liveFeed
              ? R"({"rack": "rk_1", "type": "notes", "source": "tr_3", "device": "controller"})"
              : R"({"rack": "rk_1", "type": "notes", "source": "tr_3"})");  // mo_2
    p.run("cable.add", R"({"from": "mo_2.gate", "to": "mo_1.l"})");
}

// One playing stretch of one epoch: where its first frame is in the
// output, its song sample there, the loop it played under, and where it
// ended.
struct Segment {
    std::uint64_t epoch = 0;
    std::int64_t out = 0, song = 0, end = -1;
    LoopRange loop;
    bool edited = false;  // a plan swap during it
    // The sound goes on into it: begun by a loop change while playing, or by
    // a play with no block since the pause (the in-callback executor's
    // voices sound on); `held`: the song sample it went on from.
    bool continuous = false;
    std::int64_t held = 0;
    bool playStart = false;  // begun by a play
    std::string steps;       // the steps that led to it (a failure's message)
};

// The song sample `frames` after `from`, under `loop`.
std::int64_t wrapped(LoopRange loop, std::int64_t from, std::int64_t frames) {
    std::int64_t pos = from + frames;
    if (loop.active() && from < loop.end && pos >= loop.end)
        pos = loop.start + (pos - loop.end) % (loop.end - loop.start);
    return pos;
}

// The first frame of segment `g`, at or after `from`, that plays song
// sample `start`.
std::optional<std::int64_t> locate(const Segment& g, std::int64_t start, std::int64_t from) {
    const std::int64_t length = g.end - g.out;
    std::vector<std::int64_t> at;
    if (start >= g.song) at.push_back(start - g.song);
    const LoopRange& l = g.loop;
    if (l.active() && g.song < l.end && start >= l.start && start < l.end)
        for (std::int64_t k = 0, first = l.end - g.song + (start - l.start);
             first + k * (l.end - l.start) < length; ++k)
            at.push_back(first + k * (l.end - l.start));
    std::optional<std::int64_t> best;
    for (const std::int64_t a : at)
        if (a >= from && a < length && wrapped(l, g.song, a) == start && (!best || a < *best))
            best = a;
    return best;
}

struct Read {
    int segment;
    std::int64_t offset;  // its first frame, from the segment's
    bool stopped;         // switched on while stopped
    Engine::LaunchCapture capture;
    std::string where;
};

std::string label(const Engine::TrackLaunch& t) {
    switch (t.plays) {
        case Engine::TrackLaunch::Plays::Clip:
        case Engine::TrackLaunch::Plays::Held:
            return "clip " + t.clip.str();
        case Engine::TrackLaunch::Plays::Arrangement:
            return "arrangement";
        case Engine::TrackLaunch::Plays::Stopped:
            return "stopped";
    }
    return "?";
}

struct Sequence {
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend;
    Engine engine;
    bool anticipative;
    std::uint64_t seed;
    std::int64_t done = 0;  // cycles
    bool playing = false;
    std::int64_t song = 0;        // the song sample the epoch began at (or stands at)
    std::int64_t epochCycle = 0;  // the cycle it began at
    std::int64_t quietSince = 0;  // the cycle of the last launch or transport change
    std::int64_t stoppedAt = 0;
    std::int64_t pausedAt = -1, pausedSong = -1;
    // A plan swapped while stopped, which the renderer adopts once it
    // renders: its nodes are not yet what plays.
    bool unadopted = false;
    bool liveFeed = false;
    LoopRange loop;
    std::vector<Segment> segments;
    // A capture switched on: the segment it went into (-1: not yet) and its
    // first frame in the output (-1: the segment's first).
    bool capturing = false;
    int captureSegment = -1;
    std::int64_t captureOut = -1;
    bool endedUnread = false;  // a capture a relocation ended, read at the next service
    bool relocatedCapture = false;
    std::vector<Read> captures;
    // Each epoch a late leave went on in, and where the renderer settled.
    std::map<std::uint64_t, std::int64_t> lateEnds;
    // The newest launch made on each track, as launch.state names it.
    std::map<std::string, std::string> newest;
    int step = 0;
    std::string trail;  // the steps so far, for a failure's message

    Sequence(const model::Session& s, const fs::path& dir, const EngineOptions& o, std::uint64_t sd)
        : backend({.mode = DummyBackend::Mode::Manual, .captureFrames = kFrames + 48000}),
          engine(pool, media, o),
          anticipative(o.anticipative),
          seed(sd) {
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        REQUIRE(engine.load(s, dir));
        for (const char* track : kTracks) newest[track] = "arrangement";
    }
    void note(const std::string& what) {
        trail += " " + std::to_string(step) + ":" + what;
        if (trail.size() > 1600) trail.erase(0, trail.size() - 1600);
    }
    std::string where() const {
        return "seed " + std::to_string(seed) +
               std::string(anticipative ? " anticipative" : " in-callback") + ", step " +
               std::to_string(step) + ", cycle " + std::to_string(done) + "; steps:" + trail;
    }
    // The renderer relocated for the transport as it stands and, playing, as
    // far ahead as it goes, where it stays until more is played (by
    // condition, no clock): what a main-thread action finds is then the same
    // on every run, under load too.
    void settle() {
        if (!anticipative) return;
        while (!engine.awaitRendered(std::numeric_limits<std::int64_t>::max()))
            MESSAGE("the renderer has not settled yet");
    }
    void run(std::int64_t cycles) {
        if (cycles <= 0) return;
        if (anticipative)
            runAheadOnState(engine, backend, cycles, kBlock);
        else
            backend.run(cycles);
        done += cycles;
    }
    std::int64_t position() const {
        return playing ? wrapped(loop, song, (done - epochCycle) * kBlock) : song;
    }
    void begin() {
        epochCycle = done;
        quietSince = done;
        segments.push_back(
            {EngineTestAccess::epoch(engine), done * kBlock, song, -1, loop, false, false, 0, false,
             std::to_string(step) +
                 " after:" + trail.substr(trail.size() > 700 ? trail.size() - 700 : 0)});
        if (capturing && captureSegment < 0) captureSegment = int(segments.size()) - 1;
    }
    void end() {
        if (!segments.empty() && segments.back().end < 0) segments.back().end = done * kBlock;
        relocatedCapture = capturing;
    }
    // After a relocation: one that ran ends there (read at the next
    // service); one that heard nothing yet begins where the transport goes.
    void relocated() {
        if (!relocatedCapture) return;
        relocatedCapture = false;
        if (const auto c = engine.launchCapture(); c && c->on) {
            captureSegment = -1;
            captureOut = -1;
        } else {
            capturing = false;
            endedUnread = captureSegment >= 0;
        }
    }
    void play() {
        engine.play();
        playing = true;
        begin();
        segments.back().playStart = true;
        if (done == pausedAt && pausedSong >= 0) {
            segments.back().continuous = true;
            segments.back().held = pausedSong;
        }
        unadopted = false;
    }
    void pause() {
        song = position();
        pausedAt = done;
        pausedSong = song;
        end();
        engine.pause();
        relocated();
        playing = false;
        stoppedAt = done;
        quietSince = done;
    }
    void seek(std::int64_t sample) {
        if (playing)
            song = position(), end();
        else
            relocatedCapture = capturing;
        engine.seek(double(sample) / 48000);
        relocated();
        song = sample;
        if (playing) begin();
        stoppedAt = done;
    }
    void setLoop(LoopRange to) {
        if (playing)
            song = position(), end();
        else
            relocatedCapture = capturing;
        engine.setLoop(double(to.start) / 48000, double(to.end) / 48000, to.on);
        relocated();
        loop = to;
        if (playing) {
            begin();
            segments.back().continuous = true;
            segments.back().held = song;
        }
        stoppedAt = done;
    }
    void edit(const model::Session& s, const fs::path& dir) {
        REQUIRE(engine.load(s, dir));
        if (playing && !segments.empty()) segments.back().edited = true;
        if (!playing && anticipative) unadopted = true;
        // Settled again once an executor has adopted the plan (its nodes
        // carry the launch state then).
        quietSince = stoppedAt = done;
    }
    void readCapture() {
        // One that heard no frame replaces nothing: the kept one is an
        // older capture, read already.
        const auto c = engine.launchCapture();
        bool heard = false;
        if (captureSegment >= 0) {
            const Segment& g = segments[std::size_t(captureSegment)];
            const std::int64_t from = captureOut < 0 ? g.out : captureOut;
            heard = (g.end < 0 ? done * kBlock : g.end) > from;
        }
        if (c && heard) {
            const Segment& g = segments[std::size_t(captureSegment)];
            captures.push_back({captureSegment, captureOut < 0 ? 0 : captureOut - g.out,
                                captureOut < 0, *c, where()});
        }
        endedUnread = false;
    }
    void captureOn() {
        if (endedUnread) {
            engine.service();
            readCapture();
        }
        INFO(where());
        CHECK(engine.captureLaunches(true));
        capturing = true;
        captureSegment = playing ? int(segments.size()) - 1 : -1;
        captureOut = playing ? done * kBlock : -1;
    }
    void captureOff() {
        (void)engine.captureLaunches(false);
        capturing = false;
        engine.service();
        readCapture();
    }
    void service() {
        engine.service();
        if (endedUnread) readCapture();
    }
    void noteLate() {
        if (const auto e = EngineTestAccess::launchLateEpoch(engine); e != UINT64_MAX)
            lateEnds[e] = EngineTestAccess::launchLateEnd(engine);
    }
    std::optional<Engine::TrackLaunch> state(const char* track) const {
        for (const auto& t : engine.launchState())
            if (t.track == id(track)) return t;
        return std::nullopt;
    }
    std::string plays(const char* track) const {
        const auto t = state(track);
        return t ? label(*t) : "?";
    }
    std::string node(const char* track) const {
        for (const detail::Node& n : EngineTestAccess::latestPlan(engine).nodes)
            if (n.id == id(track) && !n.players.empty()) {
                const auto& L = n.launch;
                if ((L.kind == detail::LaunchState::Kind::Clip ||
                     L.kind == detail::LaunchState::Kind::Held) &&
                    L.player >= 0)
                    return "clip " + n.players[std::size_t(L.player)].clip.str();
                return L.kind == detail::LaunchState::Kind::Arrangement ? "arrangement" : "stopped";
            }
        return "?";
    }
    bool anyQueued() const {
        for (const auto& t : engine.launchState())
            if (t.queued) return true;
        return !engine.launchesPending().empty();
    }
    bool queued(const char* track) const {
        const auto t = state(track);
        return t && t->queued.has_value();
    }
    std::size_t queuedTracks() const {
        std::size_t n = 0;
        for (const auto& t : engine.launchState()) n += t.queued.has_value();
        return n;
    }
    // Stopped (two cycles on, so the callback left the epoch) every unheard
    // launch is taken back; playing, once nothing is queued and the
    // render-ahead span has passed since the last change, with no slot's
    // switch ahead (one applied ahead may have its ack unread).
    bool settledStopped() const { return !playing && done - stoppedAt >= 2; }
    bool settledPlaying() const {
        return playing && !anyQueued() && done - quietSince >= 200 &&
               EngineTestAccess::slotsAhead(engine) == 0;
    }
    bool settled() const { return settledStopped() || settledPlaying(); }
    // After a service: launch.state against the nodes and against the newest
    // launch made on each track, and the slots held.
    void checkSettled() {
        if (!settled()) return;
        INFO(where());
        INFO("log:" << describe(engine.launchLog()));
        for (const char* track : kTracks) {
            // (Stopped, a launch queued for the play start may already be
            // in what the renderer prepared there.)
            if (queued(track)) continue;
            INFO(std::string(track));
            if (!unadopted) CHECK(node(track) == plays(track));
            CHECK(plays(track) == newest[track]);
        }
        // Nothing heard keeps a slot; a waiting launch keeps its own.
        const std::size_t held = EngineTestAccess::slotsHeld(engine);
        INFO("held: " << EngineTestAccess::heldSlots(engine));
        if (settledStopped())
            CHECK(held <= queuedTracks() + engine.launchesPending().size());
        else
            CHECK(held == 0);
    }
    Result<Engine::LaunchPlaced> tryLaunch(const std::vector<Engine::LaunchRequest>& requests,
                                           model::LaunchQuantize q) {
        return engine.launch(requests, q);
    }
    void launched(const std::vector<Engine::LaunchRequest>& requests,
                  const Result<Engine::LaunchPlaced>& placed) {
        INFO(where());
        CHECK_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
        if (!placed) return;
        for (const auto& r : requests)
            newest[r.track.str()] = r.action == LaunchLogEntry::Action::Clip
                                        ? "clip " + r.clip.str()
                                    : r.action == LaunchLogEntry::Action::Stop ? "stopped"
                                                                               : "arrangement";
        quietSince = done;
    }
};

// The offline replay of `log` from song sample `start` for `frames`, under
// `loop`.
std::vector<float> replay(const model::Session& s, const fs::path& dir,
                          const std::vector<LaunchLogEntry>& log, std::int64_t start,
                          std::size_t frames, LoopRange loop) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.startSeconds = double(start) / 48000;
    settings.endSeconds = settings.startSeconds + double(frames) / 48000 + 0.05;
    settings.blockSize = kBlock;
    settings.projectDir = dir;
    settings.launches = log;
    if (loop.active()) settings.loop = {double(loop.start) / 48000, double(loop.end) / 48000};
    auto r = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// Two known faults the sequences found (round 205; narrowed in round 207
// F3; capture-start-state removes them): what each track plays where a
// replay starts is listed as a launch at frame 0, so the replay cannot
// hold (A) a loop clip carried from an earlier epoch to before its anchor
// (a seek back), which live plays by its phase (engine.md §1.4.2, "Phase")
// and the replay leaves silent until the anchor, or (B) a crossfade
// running where playing begins: carried on from an earlier epoch (a pause
// or seek inside it), or, for a capture beginning at a play start, a
// switch there from a clip the stop kept (the replay fades from silence).
// A return anchored after the start replays exactly, and so do a switch
// at an epoch's start from silence and a capture begun inside a crossfade
// while playing. Found in round 207 by the MIDI track: (D) a capture
// begun while playing inside a sounding note, which live holds and the
// replay, starting inside the note, never starts (to be marked not as
// heard); (G) an epoch a loop change began while playing inside a note:
// the in-callback executor holds the note on, the anticipative one and
// the replay start silent. `offset`: the start's frame from the epoch's
// first, `start` its song sample, `continuous`: begun by a loop change
// while playing, `inCallback`: the executor. Returns "A", "B", "D", "G" or
// nothing.
// Whether tr_3's clip `clip` anchored at `anchor` has a note sounding just
// before song sample `at` (120 BPM, 24000 samples a beat; four-beat clips).
bool noteSounding(model::Id clip, std::int64_t anchor, std::int64_t at) {
    const std::int64_t beat = 24000, phase = ((at - anchor) % (4 * beat) + 4 * beat) % (4 * beat);
    const std::vector<std::pair<double, double>> notes =
        clip == id("cl_5") ? std::vector<std::pair<double, double>>{{0, 0.5}, {1, 1.5}}
                           : std::vector<std::pair<double, double>>{{0, 1}, {2, 3}};
    for (const auto& [on, off] : notes)
        if (phase > std::int64_t(on * beat) && phase < std::int64_t(off * beat) + kFade)
            return true;
    return false;
}
const char* faultAt(const std::vector<LaunchLogEntry>& log, std::uint64_t epoch,
                    std::int64_t offset, std::int64_t start, bool capture, bool continuous,
                    bool inCallback, std::int64_t held = 0) {
    std::map<model::Id, const LaunchLogEntry*> carried, newest;
    for (const LaunchLogEntry& e : log)
        if (e.epoch < epoch) carried[e.track] = &e;
    newest = carried;
    for (const LaunchLogEntry& e : log)
        if (e.epoch == epoch && e.runningFrame <= offset) newest[e.track] = &e;
    // G: by the epoch's own start (what the in-callback executor held there).
    if (continuous && inCallback)
        for (const auto& [track, e] : carried)
            if (track == id("tr_3") && e->action == LaunchLogEntry::Action::Clip &&
                e->anchorSample <= held && noteSounding(e->clip, e->anchorSample, held))
                return "G";
    for (const auto& [track, e] : newest) {
        if (track == id("tr_3") && e->action == LaunchLogEntry::Action::Clip &&
            e->anchorSample <= start) {
            if (capture && offset > 0 && noteSounding(e->clip, e->anchorSample, start)) return "D";
        }
        const bool fading = e->anchorSample <= start && start < e->anchorSample + kFade;
        if (e->epoch < epoch) {
            if (e->action == LaunchLogEntry::Action::Clip && e->anchorSample > start) return "A";
            if (fading) return "B";
        } else if (e->action == LaunchLogEntry::Action::Clip && e->anchorSample > start) {
            return "A";  // (a launch placed before a seek back, switched after it)
        } else if (capture && offset == 0 && fading) {
            const auto it = carried.find(track);
            if (it != carried.end() && it->second->action == LaunchLogEntry::Action::Clip)
                return "B";
        }
    }
    return nullptr;
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

// The frames from an epoch's start up to which two executors' logs place
// the same switches (each unquantized launch lands at its own executor's
// horizon): before the first entry in which they differ, less a margin.
std::int64_t sameUpTo(std::vector<LaunchLogEntry> a, std::vector<LaunchLogEntry> b,
                      std::int64_t length) {
    const auto order = [](const LaunchLogEntry& x, const LaunchLogEntry& y) {
        return std::tie(x.runningFrame, x.track.n) < std::tie(y.runningFrame, y.track.n);
    };
    std::sort(a.begin(), a.end(), order);
    std::sort(b.begin(), b.end(), order);
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const bool same = i < a.size() && i < b.size() && a[i].track == b[i].track &&
                          a[i].action == b[i].action && a[i].clip == b[i].clip &&
                          a[i].anchorSample == b[i].anchorSample &&
                          a[i].runningFrame == b[i].runningFrame;
        if (same) continue;
        std::int64_t at = length;
        if (i < a.size()) at = std::min(at, a[i].runningFrame);
        if (i < b.size()) at = std::min(at, b[i].runningFrame);
        return std::max<std::int64_t>(0, at - 128);
    }
    return length;
}

struct Counts {
    int epochs = 0, comparedEpochs = 0, captures = 0, comparedCaptures = 0, markedCaptures = 0,
        faultE = 0;
    std::map<std::string, int> faults;
};

// Each epoch against its log's replay, and each capture read against its
// own (`q`'s run of the session `s`).
Counts judge(Sequence& q, const model::Session& s, const fs::path& dir) {
    Counts n;
    const auto log = q.engine.launchLog();
    const std::uint32_t latency = q.engine.outputLatency();
    const auto& out = q.backend.capture().channels[0];
    INFO("seed " << q.seed << std::string(q.anticipative ? " anticipative" : " in-callback")
                 << "; log:" << describe(log));
    // Each epoch as its log replays (but its last 2048 frames before a stop
    // or seek, and an epoch a late leave went on in, which a capture marks).
    for (const Segment& g : q.segments) {
        ++n.epochs;
        const std::int64_t length = g.end - g.out - 2048;
        if (length <= 0 || q.lateEnds.count(g.epoch)) continue;
        if (const char* f =
                faultAt(log, g.epoch, 0, g.song, false, g.continuous, !q.anticipative, g.held)) {
            ++n.faults[f];
            continue;
        }
        auto offline = replay(s, dir, epochLog(log, g.epoch), g.song, std::size_t(length), g.loop);
        // (H, found in round 207: in-callback, the live Notes feed's gate
        // holds its value from before a pause for the first block after the
        // play; compared from the second block.)
        std::size_t skip = 0;
        if (!q.anticipative && q.liveFeed && g.playStart) {
            skip = kBlock;
            ++n.faults["H"];
        }
        offline.erase(offline.begin(), offline.begin() + std::ptrdiff_t(skip));
        const std::size_t frames = std::size_t(length) - skip;
        INFO("began at step " << g.steps);
        INFO("epoch " << g.epoch << " from song " << g.song << " at frame " << g.out << ", "
                      << length << " frames (from " << skip << "): "
                      << differences(out, latency + std::size_t(g.out) + skip, offline, frames));
        CHECK(maxDiff(out, latency + std::size_t(g.out) + skip, offline, frames) == 0.0);
        ++n.comparedEpochs;
    }
    // A capture as heard sounds as its own replay.
    for (const Read& c : q.captures) {
        ++n.captures;
        const Segment& g = q.segments[std::size_t(c.segment)];
        const bool asHeard =
            c.capture.facts == Engine::CaptureFacts{.fromPlayStart = c.capture.facts.fromPlayStart};
        if (!c.capture.placed || c.capture.frames <= 4096) continue;
        INFO("capture read at " << c.where);
        INFO("capture from song " << c.capture.startSample << ", " << c.capture.frames
                                  << " frames, in epoch " << g.epoch << " (from song " << g.song
                                  << ", its frame " << c.offset
                                  << "); its launches:" << describe(c.capture.launches));
        // Begun over frames a late leave's renderer rendered before it
        // settled: never as heard (round 203 F2, round 205 F3).
        if (const auto it = q.lateEnds.find(g.epoch);
            it != q.lateEnds.end() && c.offset < it->second) {
            CHECK_FALSE(asHeard);
            ++n.markedCaptures;
            continue;
        }
        if (!asHeard) {
            ++n.markedCaptures;
            continue;
        }
        // Located by its start, at or after where it was switched on (one
        // placed at a later service begins there). (E, found in round 207:
        // in-callback, one switched on while stopped keeps where it was
        // switched on across a seek while stopped, and spans the jump;
        // counted, not compared, until capture-start-state.)
        const auto located = locate(g, c.capture.startSample, c.offset);
        if (!located && c.stopped && !q.anticipative) {
            ++n.faultE;
            continue;
        }
        CHECK(located.has_value());
        if (!located) continue;
        const std::int64_t offset = *located;
        if (const char* f = faultAt(log, g.epoch, offset, c.capture.startSample, true, g.continuous,
                                    !q.anticipative, g.held)) {
            ++n.faults[f];
            continue;
        }
        const std::int64_t at = g.out + offset;
        const std::size_t frames = std::size_t(c.capture.frames - 2048);
        const auto offline =
            replay(s, dir, c.capture.launches, c.capture.startSample, frames, c.capture.loop);
        INFO(differences(out, latency + std::size_t(at), offline, frames));
        CHECK(maxDiff(out, latency + std::size_t(at), offline, frames) == 0.0);
        ++n.comparedCaptures;
    }
    return n;
}

// The two executors' epochs, frame for frame, where their logs agree.
int compareExecutors(Sequence& a, Sequence& b) {
    int compared = 0;
    const auto logA = a.engine.launchLog(), logB = b.engine.launchLog();
    const std::uint32_t latA = a.engine.outputLatency(), latB = b.engine.outputLatency();
    const auto& outA = a.backend.capture().channels[0];
    const auto& outB = b.backend.capture().channels[0];
    REQUIRE(a.segments.size() == b.segments.size());
    INFO("seed " << a.seed << "; in-callback log:" << describe(logA)
                 << "\nanticipative log:" << describe(logB));
    for (std::size_t i = 0; i < a.segments.size(); ++i) {
        const Segment& g = a.segments[i];
        REQUIRE(g.epoch == b.segments[i].epoch);
        REQUIRE(g.out == b.segments[i].out);
        if (b.lateEnds.count(g.epoch)) continue;
        // (Not its last frames before a stop or seek, a crossfade and the
        // shift: the executors' tails there differed by 64 frames once, open;
        // nor a loop change's epoch over a held note, G.)
        const std::int64_t length =
            sameUpTo(epochLog(logA, g.epoch), epochLog(logB, g.epoch), g.end - g.out - kFade);
        if (length <= 0) continue;
        if (const char* f = faultAt(logA, g.epoch, 0, g.song, false, g.continuous, true, g.held);
            f && f[0] == 'G')
            continue;
        const std::int64_t skip = a.liveFeed && g.playStart ? kBlock : 0;  // H
        if (length <= skip) continue;
        const std::vector<float> slice(
            outB.begin() + std::ptrdiff_t(latB + std::size_t(g.out + skip)),
            outB.begin() + std::ptrdiff_t(latB + std::size_t(g.out + length)));
        const std::size_t frames = std::size_t(length - skip);
        INFO("began at step " << g.steps);
        INFO("epoch " << g.epoch << " at frame " << g.out << ", " << length << " frames (from "
                      << skip << "): "
                      << differences(outA, latA + std::size_t(g.out + skip), slice, frames));
        CHECK(maxDiff(outA, latA + std::size_t(g.out + skip), slice, frames) == 0.0);
        ++compared;
    }
    return compared;
}

// One seeded sequence on both executors in lockstep. `longPlay`: one play
// with no stop or seek, unquantized or sixteenth launches and runs only
// (more launches heard in one epoch than there are slots). Returns the
// anticipative run's log and output, to check it repeats.
std::pair<std::string, std::vector<float>> runSequence(const model::Session& s,
                                                       const model::Session& edited,
                                                       const fs::path& dir, std::uint64_t seed,
                                                       int steps, bool longPlay, bool liveFeed) {
    std::mt19937_64 rng(seed);
    // Uniform in [0, n): the generator's output is fixed by the standard,
    // the distribution classes are not.
    const auto pick = [&](std::uint64_t n) { return rng() % n; };
    Sequence a(s, dir, kInCallback, seed), b(s, dir, EngineOptions{.workers = 0}, seed);
    Sequence* both[] = {&a, &b};
    a.liveFeed = b.liveFeed = liveFeed;
    const auto each = [&](auto&& f) {
        for (Sequence* q : both) f(*q);
    };
    const auto note = [&](const std::string& what) { each([&](Sequence& q) { q.note(what); }); };
    const auto run = [&](std::int64_t n) { each([&](Sequence& q) { q.run(n); }); };
    bool editedPlan = false;
    const auto launch = [&] {
        std::vector<Engine::LaunchRequest> requests;
        const auto shape = pick(6);  // a track (0-2), two tracks (3, 4), or a scene (5)
        if (shape == 5) {
            const auto scene = pick(2);
            for (int t = 0; t < 3; ++t) {
                Engine::LaunchRequest r = request(kTracks[t], kClips[t][scene]);
                r.scene = id(kScenes[scene]);
                requests.push_back(r);
            }
        } else {
            for (int t = 0; t < 3; ++t) {
                const bool in = shape < 3 ? std::uint64_t(t) == shape : t != int(shape - 2);
                if (!in) continue;
                const auto which = pick(6);
                if (which == 4)
                    requests.push_back(backTo(kTracks[t]));
                else if (which == 5)
                    requests.push_back(
                        {id(kTracks[t]), LaunchLogEntry::Action::Stop, {}, std::nullopt});
                else
                    requests.push_back(request(kTracks[t], kClips[t][which % 2]));
            }
        }
        static const model::LaunchQuantize quantize[] = {
            model::LaunchQuantize::None, model::LaunchQuantize::Sixteenth,
            model::LaunchQuantize::Beat, model::LaunchQuantize::Bar};
        const auto q = quantize[longPlay ? pick(2) : pick(4)];
        std::string what = "launch";
        for (const auto& r : requests)
            what += "," + r.track.str() + "=" +
                    (r.action == LaunchLogEntry::Action::Clip   ? r.clip.str()
                     : r.action == LaunchLogEntry::Action::Stop ? std::string("stop")
                                                                : std::string("arr"));
        if (shape == 5) what += "(scene)";
        note(what + "q" + std::to_string(int(q)));
        auto pa = a.tryLaunch(requests, q);
        auto pb = b.tryLaunch(requests, q);
        // While the transport relocates, a launch is refused for a moment
        // (launch.later): again a cycle on (on both), as a caller would.
        const auto later = [](const Result<Engine::LaunchPlaced>& p) {
            return !p && p.error().code == "launch.later";
        };
        for (int again = 0; (later(pa) || later(pb)) && again < 8; ++again) {
            run(1);
            if (later(pa)) pa = a.tryLaunch(requests, q);
            if (later(pb)) pb = b.tryLaunch(requests, q);
        }
        a.launched(requests, pa);
        b.launched(requests, pb);
    };
    each([](Sequence& q) {
        q.play();
        q.run(20);
        q.engine.service();
    });
    bool late = false;
    int step = 0;
    for (; step < steps && a.done * kBlock < kFrames - 48000; ++step) {
        each([&](Sequence& q) {
            q.step = step;
            q.settle();
        });
        const auto r = pick(100);
        if (longPlay) {
            if (r < 50) {
                launch();
            } else {
                const std::int64_t n = 1 + std::int64_t(pick(250));
                note("run" + std::to_string(n));
                run(n);
            }
        } else if (r < 26) {
            launch();
        } else if (r < 52) {
            const std::int64_t n =
                pick(3) == 0 ? 1 + std::int64_t(pick(8)) : 1 + std::int64_t(pick(400));
            note("run" + std::to_string(n));
            run(n);
        } else if (r < 60) {
            note(a.playing ? "pause" : "play");
            each([](Sequence& q) { q.playing ? q.pause() : q.play(); });
        } else if (r < 68) {
            const std::int64_t sample = std::int64_t(pick(12 * 128)) * 375;  // k/128 s
            note("seek" + std::to_string(sample));
            each([&](Sequence& q) { q.seek(sample); });
        } else if (r < 71) {
            // The transport loop on (1 to 3 s from somewhere in the first 4 s) or off.
            LoopRange to = a.loop;
            if (to.on) {
                to.on = false;
            } else {
                to.start = std::int64_t(pick(4 * 128)) * 375;
                to.end = to.start + std::int64_t(1 + pick(3)) * 48000;
                to.on = true;
            }
            note("loop" + (to.on ? std::to_string(to.start) + "-" + std::to_string(to.end)
                                 : std::string("off")));
            each([&](Sequence& q) { q.setLoop(to); });
        } else if (r < 73) {
            // A plan swap that sounds the same (a spare track comes or goes).
            editedPlan = !editedPlan;
            note("edit");
            each([&](Sequence& q) { q.edit(editedPlan ? edited : s, dir); });
        } else if (r < 77) {
            if (!a.playing) {
                note("play");
                each([](Sequence& q) { q.play(); });
            }
        } else if (r < 84) {
            note(a.capturing ? "capture off" : "capture on");
            each([](Sequence& q) { q.capturing ? q.captureOff() : q.captureOn(); });
        } else if (r < 86) {
            note("late");
            late = true;
            EngineTestAccess::rendererLeaveLate(b.engine, true);
        } else if (r < 89) {
            // A storm: more one-cycle seeks than the ring of epoch ends holds,
            // no service, then a launch, then long enough to be compared.
            const auto seeks = 64 + std::int64_t(pick(12));
            const std::int64_t sample = std::int64_t(pick(12 * 128)) * 375;
            note("storm" + std::to_string(seeks));
            for (std::int64_t i = 0; i < seeks; ++i)
                each([&](Sequence& q) {
                    q.settle();
                    q.seek(sample);
                    q.run(1);
                });
            each([](Sequence& q) { q.settle(); });
            launch();
            const std::int64_t n = 200 + std::int64_t(pick(200));
            note("run" + std::to_string(n));
            run(n);
        } else {
            note("run1");
            run(1);
        }
        if (late && !longPlay && r >= 26 && r < 52) {  // the late leave settles from here
            late = false;
            EngineTestAccess::rendererLeaveLate(b.engine, false);
        }
        if (pick(100) < 85) {
            each([](Sequence& q) {
                q.settle();
                q.service();
                q.checkSettled();
            });
            // Both executors hold the same launch.state, settled (stopped,
            // what a launch queued for the play start shows can differ).
            if (a.settled() && b.settled()) {
                INFO(b.where());
                INFO("in-callback log:" << describe(a.engine.launchLog()));
                INFO("anticipative log:" << describe(b.engine.launchLog()));
                for (const char* track : kTracks) {
                    if (!a.playing && (a.queued(track) || b.queued(track))) continue;
                    INFO(std::string(track)
                         << ": in-callback " << a.plays(track)
                         << (a.queued(track) ? " (queued)" : "") << ", anticipative "
                         << b.plays(track) << (b.queued(track) ? " (queued)" : ""));
                    CHECK(a.plays(track) == b.plays(track));
                    CHECK(a.queued(track) == b.queued(track));
                }
            }
        }
        each([](Sequence& q) { q.noteLate(); });
    }
    EngineTestAccess::rendererLeaveLate(b.engine, false);
    each([](Sequence& q) {
        q.settle();
        if (q.playing) q.pause();
        q.run(10);
        q.service();
        q.checkSettled();
        q.noteLate();
        q.engine.stop();
        INFO(q.where());
        CHECK(q.engine.processErrors() == 0);
        if (q.anticipative) CHECK(q.engine.anticipationMisses() == 0);
    });
    const int executors = compareExecutors(a, b);
    for (Sequence* q : both) {
        const Counts n = judge(*q, s, dir);
        MESSAGE("seed " << seed << std::string(q->anticipative ? " anticipative" : " in-callback")
                        << ": " << step << " steps, " << n.epochs << " epochs (" << n.comparedEpochs
                        << " against the replay, " << executors << " against the other executor), "
                        << n.captures << " captures read (" << n.comparedCaptures << " compared, "
                        << n.markedCaptures << " not as heard), " << q->engine.launchLog().size()
                        << " launches logged; not compared (faults): " << [&] {
                               std::string f;
                               for (const auto& [k, v] : n.faults)
                                   f += k + " " + std::to_string(v) + ", ";
                               return f + "E " + std::to_string(n.faultE);
                           }());
    }
    return {describe(b.engine.launchLog()), b.backend.capture().channels[0]};
}

}  // namespace

TEST_CASE("seeded sequences of launches, stops, seeks and captures keep the launch invariants" *
          doctest::description("rt")) {
    // 6 seeds of 400 steps on the project with a Notes feed, 3 with a live
    // one, a long play of 1200, and one seed run twice for 200, each on both
    // executors in lockstep (seed 12, the live feed, found the late-leave
    // fault; SUPPLE_SEQUENCE_SEED=12 runs it); fewer under ThreadSanitizer,
    // where the same steps run several times slower.
#if defined(__SANITIZE_THREAD__)
    constexpr bool kTsan = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    constexpr bool kTsan = true;
#else
    constexpr bool kTsan = false;
#endif
#else
    constexpr bool kTsan = false;
#endif
    // SUPPLE_SEQUENCE_SEED=n runs that seed alone (to reproduce a failure).
    const char* only = std::getenv("SUPPLE_SEQUENCE_SEED");
    if (only) {
        const std::uint64_t seed = std::strtoull(only, nullptr, 10);
        Project p;
        project(p, seed >= 9 && seed <= 12);
        const model::Session s = p.app->head();
        p.run("track.add", R"({"name": "Spare"})");
        (void)runSequence(s, p.app->head(), p.app->projectDir(), seed, seed > 100 ? 1200 : 400,
                          seed > 100, seed >= 9 && seed <= 12);
        return;
    }
    for (const bool liveFeed : {false, true}) {
        Project p;
        project(p, liveFeed);
        const model::Session s = p.app->head();
        p.run("track.add", R"({"name": "Spare"})");
        const model::Session edited = p.app->head();
        const fs::path dir = p.app->projectDir();
        INFO(std::string(liveFeed ? "live Notes feed" : "Notes feed"));
        if (!liveFeed) {
            for (std::uint64_t seed = 1; seed <= (kTsan ? 2u : 6u); ++seed)
                (void)runSequence(s, edited, dir, seed, 400, false, false);
            (void)runSequence(s, edited, dir, 101, kTsan ? 300 : 1200, true, false);
            // A run repeats from its seed, the anticipative one included.
            const auto first = runSequence(s, edited, dir, 3, kTsan ? 100 : 200, false, false);
            const auto again = runSequence(s, edited, dir, 3, kTsan ? 100 : 200, false, false);
            CHECK(first.first == again.first);
            CHECK((first.second == again.second));
        } else {
            for (std::uint64_t seed = 9; seed <= (kTsan ? 9u : 11u); ++seed)
                (void)runSequence(s, edited, dir, seed, 400, false, true);
        }
    }
}

TEST_CASE(
    "a launch the callback switched while the renderer's leave was held late reaches the "
    "renderer's targets (round 207)" *
    doctest::description("rt")) {
    // Anticipative, tr_3's launches switching both tr_3 (the renderer) and
    // tr_4's live Notes feed (the callback). The renderer's leave held late
    // across a pause; cl_5 launched while stopped, for the play start; play:
    // the callback switches the feed at once while the renderer, its leave
    // pending, applies nothing. Released mid-epoch (`midEpoch`), the renderer
    // switches tr_3 at its next block; released only after a later pause
    // and play, at its leave. Meanwhile launch.state says cl_5 is queued;
    // after, tr_3 plays cl_5 and holds no slot, and a capture from the play
    // start is not as heard.
    for (const bool midEpoch : {true, false}) {
        INFO(std::string(midEpoch ? "released mid-epoch" : "released after a pause and play"));
        Project p;
        project(p, true);
        const model::Session s = p.app->head();
        Sequence q(s, p.app->projectDir(), EngineOptions{.workers = 0}, 1);
        q.play();
        q.run(20);
        q.engine.service();
        REQUIRE(q.engine.launch(std::vector{request("tr_3", "cl_6")}, model::LaunchQuantize::None));
        q.run(400);
        q.engine.service();
        q.settle();
        EngineTestAccess::rendererLeaveLate(q.engine, true);
        q.pause();
        q.run(2);
        q.engine.service();
        REQUIRE(q.engine.launch(std::vector{request("tr_3", "cl_5")}, model::LaunchQuantize::Bar));
        q.engine.service();
        REQUIRE(q.engine.captureLaunches(true));
        q.play();
        q.run(20);
        q.engine.service();
        CHECK(q.queued("tr_3"));
        if (!midEpoch) {
            q.pause();
            q.run(2);
            q.engine.service();
            CHECK(q.queued("tr_3"));
            CHECK(q.plays("tr_3") == "clip cl_6");
            q.play();
            q.run(5);
        }
        EngineTestAccess::rendererLeaveLate(q.engine, false);
        q.run(q.done + 400 - q.done);
        q.engine.service();
        q.settle();
        q.run(300);
        q.engine.service();
        CHECK_FALSE(q.queued("tr_3"));
        CHECK(q.plays("tr_3") == "clip cl_5");
        // The capture from the play start lacks the switch on tr_3 there: not as heard.
        (void)q.engine.captureLaunches(false);
        q.engine.service();
        const auto capture = q.engine.launchCapture();
        REQUIRE(capture);
        CHECK(capture->facts.launchUnsettled);
        CHECK(q.node("tr_3") == "clip cl_5");
        CHECK(EngineTestAccess::slotsHeld(q.engine) == 0);
        q.engine.stop();
    }
}
