// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's transport commands (ADR-0097, step L2): launch.clip,
// launch.scene, launch.stop, launch.arrangement and launch.state. What a
// track plays is transport state, not an edit: these are actions and a
// query, outside history.
#include "clip_launcher.h"
#include "command_helpers.h"
#include "supple/model/launcher.h"

namespace supple::engine {

namespace {

struct LaunchTarget {
    std::string track;
    std::optional<std::string> clip;
};
struct LaunchDone {
    std::string action;
    std::vector<LaunchTarget> tracks;
    std::optional<double> beat, seconds;
    bool started = false;
};
struct ClipArg {
    std::string clip;
};
struct SceneArg {
    std::string scene;
};
struct TrackArg {
    std::optional<std::string> track;
};
struct LaunchQueued {
    std::string action;
    std::optional<std::string> clip;
    double beat = 0, seconds = 0;
};
struct LaunchTrackState {
    std::string track, plays;
    std::optional<std::string> clip;
    std::optional<double> position;
    std::optional<LaunchQueued> queued;
};
struct LaunchStateResult {
    bool playing = false;
    std::vector<LaunchTrackState> tracks;
};

using Action = LaunchLogEntry::Action;

std::string actionName(Action a) {
    return a == Action::Clip ? "clip" : a == Action::Stop ? "stop" : "arrangement";
}

Result<Engine*> runningEngine(EngineApp& app) {
    if (!app.engine())
        return Error("engine.stopped", "the real-time engine is not running", {},
                     "engine.start first");
    return app.engine();
}

model::LaunchQuantize quantizeOf(const model::Session& s, const model::Clip* clip) {
    if (clip && clip->quantize) return *clip->quantize;
    return s.project->launchQuantize;
}

// Places the requests on one target and, for a clip launched while
// stopped, starts the transport (ADR-0097: it applies at the play start).
Result<LaunchDone> launchNow(EngineApp& app, Action action,
                             const std::vector<Engine::LaunchRequest>& requests,
                             model::LaunchQuantize quantize, const std::string& path) {
    auto engine = runningEngine(app);
    if (!engine) return std::move(engine).error();
    LaunchDone out;
    out.action = actionName(action);
    if (requests.empty()) return out;
    auto placed = (*engine)->launch(requests, quantize);
    if (!placed) {
        Error e = std::move(placed).error();
        e.path = path;
        return e;
    }
    for (const auto& r : requests)
        out.tracks.push_back({r.track.str(), r.action == Action::Clip
                                                 ? std::optional<std::string>(r.clip.str())
                                                 : std::nullopt});
    out.beat = placed->anchorBeat;
    out.seconds = double(placed->anchorSample) / (*engine)->config().sampleRate;
    if (action == Action::Clip && !(*engine)->transport().playing) {
        auto played = app.transport("play");
        if (!played) return std::move(played).error();
        out.started = true;
    }
    return out;
}

Status refuseFrozen(const model::Session& s, Id track, const std::string& path) {
    if (s.tracks.find(track)->freeze)
        return Error("track.frozen", track.str() + " is frozen; it plays its print", path,
                     "track.unfreeze {track: \"" + track.str() + "\"} to launch its clips");
    return {};
}

// launch.stop and launch.arrangement: one track, or every track the
// running plan gives clips that `wanted` selects.
Result<LaunchDone> toAll(EngineApp& app, Context&, Action action, const TrackArg& a,
                         const std::function<bool(const Engine::TrackLaunch&)>& wanted) {
    auto engine = runningEngine(app);
    if (!engine) return std::move(engine).error();
    const model::Session s = app.view();
    std::vector<Engine::LaunchRequest> requests;
    if (a.track) {
        auto id = existing<model::Track>(s, *a.track, "/track");
        if (!id) return std::move(id).error();
        const model::Track& tr = *s.tracks.find(*id);
        // A frozen track already follows the arrangement (ADR-0097).
        if (tr.freeze) return LaunchDone{actionName(action), {}, {}, {}, false};
        const auto state = (*engine)->launchState();
        const bool hasClips =
            std::any_of(state.begin(), state.end(), [&](const auto& t) { return t.track == *id; });
        if (!hasClips) {
            if (action == Action::Arrangement)  // it follows it already
                return LaunchDone{actionName(action), {}, {}, {}, false};
            return Error("launch.track", id->str() + " has no clips in the running plan", "/track",
                         "a track without clips follows the arrangement; mute it to silence it");
        }
        requests.push_back({*id, action, {}});
    } else {
        for (const Engine::TrackLaunch& t : (*engine)->launchState())
            if (wanted(t)) requests.push_back({t.track, action, {}});
    }
    const model::LaunchQuantize q = quantizeOf(s, nullptr);
    return launchNow(app, action, requests, q, a.track ? "/track" : "");
}

std::string playsName(Engine::TrackLaunch::Plays p) {
    using P = Engine::TrackLaunch::Plays;
    return p == P::Clip      ? "clip"
           : p == P::Held    ? "held"
           : p == P::Stopped ? "stopped"
                             : "arrangement";
}

schema::Schema<LaunchDone> launchDoneSchema() {
    auto target = schema::object<LaunchTarget>("launch_target")
                      .field("track", &LaunchTarget::track, schema::string())
                      .field("clip", &LaunchTarget::clip, schema::string())
                      .build();
    return schema::object<LaunchDone>("launch_done")
        .field("action", &LaunchDone::action, schema::string())
        .field("tracks", &LaunchDone::tracks, schema::arrayOf(target))
        .field("beat", &LaunchDone::beat, schema::number())
        .field("seconds", &LaunchDone::seconds, schema::number())
        .field("started", &LaunchDone::started, schema::boolean())
        .build();
}

const char* kWhen =
    "It applies at the next quantize boundary (the clip's quantize, else the project's "
    "launch_quantize) at or after the earliest moment every track can still take it: about "
    "one device block ahead for a track the audio callback runs, about the render-ahead "
    "(0.2 s) for one the renderer runs. beat and seconds give that song position. While the "
    "transport is stopped it applies at the play position when playing starts.";

std::vector<cmd::ErrorSpec> commonErrors() {
    return {{"engine.stopped", "the engine is not running", "engine.start first"},
            {"launch.playrate", "a master playrate is active",
             "set the master playrate to 1 and turn off its envelope"},
            {"launch.clip", "the running plan has no player for the clip",
             "wait for the edit to reach the engine, then launch again"},
            {"launch.queue", "too many launches are waiting", "wait for them to play"},
            {"launch.later", "the transport is relocating", "launch again in a moment"}};
}

}  // namespace

void registerLaunchCommands(cmd::Registry& registry) {
    [&] {
        Command<ClipArg, LaunchDone> c;
        c.name = "launch.clip";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Play a clip on its track from the next quantize boundary.";
        c.description = std::string(kWhen) +
                        " Launching while stopped starts the transport. The track leaves the "
                        "arrangement (or the clip it played) there: its sounding notes end and "
                        "audio crossfades over 5 ms.";
        c.tags = {"clip launcher", "transport"};
        c.args = schema::object<ClipArg>("launch_clip_args")
                     .field("clip", &ClipArg::clip, idArg("cl", "Clip"))
                     .build();
        c.result = launchDoneSchema();
        c.errors = commonErrors();
        c.errors.push_back({"track.frozen", "the clip's track is frozen", "track.unfreeze first"});
        c.errors.push_back({"launch.memory", "the clip's audio did not fit the budget",
                            "use shorter audio or fewer files in clips"});
        c.examples = {{"While stopped", j(R"({"clip": "cl_1"})"),
                       j(R"({"action": "clip", "tracks": [{"track": "tr_1", "clip": "cl_1"}],
                            "beat": 0, "seconds": 0, "started": true})"),
                       "song_with_clip_running"}};
        c.run = [](const ClipArg& a, Context& ctx) -> Result<LaunchDone> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Clip>(s, a.clip, "/clip");
            if (!id) return std::move(id).error();
            const model::Clip& clip = *s.clips.find(*id);
            SUPPLE_TRY(refuseFrozen(s, clip.track, "/clip"));
            return launchNow(app, Action::Clip, {{clip.track, Action::Clip, *id}},
                             quantizeOf(s, &clip), "/clip");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<SceneArg, LaunchDone> c;
        c.name = "launch.scene";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Play every clip of a scene together, one target for all tracks.";
        c.description =
            std::string(kWhen) +
            " All the scene's tracks switch at the same boundary, the project's launch_quantize. "
            "Tracks with no clip in the scene keep playing what they play. A frozen track is "
            "skipped with a track.frozen warning, a clip past the memory budget with a "
            "launch.memory warning.";
        c.tags = {"clip launcher", "transport", "scene"};
        c.args = schema::object<SceneArg>("launch_scene_args")
                     .field("scene", &SceneArg::scene, idArg("sc", "Scene"))
                     .build();
        c.result = launchDoneSchema();
        c.errors = commonErrors();
        c.errors.push_back({"launch.scene", "the scene has no clip that can play",
                            "add clips to the scene, or unfreeze their tracks"});
        c.examples = {{"While stopped", j(R"({"scene": "sc_1"})"),
                       j(R"({"action": "clip", "tracks": [{"track": "tr_1", "clip": "cl_1"}],
                            "beat": 0, "seconds": 0, "started": true})"),
                       "song_with_clip_running"}};
        c.run = [](const SceneArg& a, Context& ctx) -> Result<LaunchDone> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto scene = existing<model::Scene>(s, a.scene, "/scene");
            if (!scene) return std::move(scene).error();
            auto engine = runningEngine(app);
            if (!engine) return std::move(engine).error();
            const auto skipped = (*engine)->clipsSkipped();
            std::vector<Engine::LaunchRequest> requests;
            for (Id track : tracksInDisplayOrder(s))
                for (const auto& e : s.clips) {
                    if (e.value->scene != *scene || e.value->track != track) continue;
                    if (s.tracks.find(track)->freeze) {
                        ctx.warn("track.frozen",
                                 track.str() + " is frozen; its clip " + e.id.str() +
                                     " was not launched",
                                 "/scene");
                    } else if (std::find(skipped.begin(), skipped.end(), e.id) != skipped.end()) {
                        ctx.warn("launch.memory",
                                 e.id.str() + "'s audio did not fit the prepared-audio budget",
                                 "/scene");
                    } else {
                        requests.push_back({track, Action::Clip, e.id});
                    }
                }
            if (requests.empty())
                return Error("launch.scene", a.scene + " has no clip that can play", "/scene",
                             "add clips to the scene with clip.add, or unfreeze their tracks");
            return launchNow(app, Action::Clip, requests, quantizeOf(s, nullptr), "/scene");
        };
        addChecked(registry, std::move(c));
    }();
    using TL = Engine::TrackLaunch;
    for (const Action action : {Action::Stop, Action::Arrangement}) {
        const bool stop = action == Action::Stop;
        Command<TrackArg, LaunchDone> c;
        c.name = stop ? "launch.stop" : "launch.arrangement";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = stop ? "Silence a track's clip, or every playing clip, at the next boundary."
                         : "Return a track, or every track, to the arrangement at the boundary.";
        c.description =
            std::string(kWhen) +
            (stop ? " A stopped track is silent until a clip is launched or it returns to the "
                    "arrangement. Without track: every track a clip holds or is queued to hold."
                  : " Returning does not chase notes: an arrangement note already under way "
                    "stays silent until its next start. Without track: every track not "
                    "following the arrangement, or queued to leave it.") +
            " A frozen track already follows the arrangement: nothing changes.";
        c.tags = {"clip launcher", "transport"};
        c.args = schema::object<TrackArg>(stop ? "launch_stop_args" : "launch_arrangement_args")
                     .field("track", &TrackArg::track, idArg("tr", "Track"))
                     .build();
        c.result = launchDoneSchema();
        c.errors = commonErrors();
        if (stop)
            c.errors.push_back({"launch.track", "the track has no clips in the running plan",
                                "mute the track to silence it"});
        c.examples = {
            stop ? cmd::Example{"One track, while stopped", j(R"({"track": "tr_1"})"),
                                j(R"({"action": "stop", "tracks": [{"track": "tr_1"}], "beat": 0,
                                     "seconds": 0, "started": false})"),
                                "song_with_clip_running"}
                 : cmd::Example{"Nothing left the arrangement", j("{}"),
                                j(R"({"action": "arrangement", "tracks": [], "started": false})"),
                                "song_with_clip_running"}};
        c.run = [action, stop](const TrackArg& a, Context& ctx) -> Result<LaunchDone> {
            return toAll(engine::app(ctx), ctx, action, a, [stop](const TL& t) {
                if (stop)
                    return t.plays == TL::Plays::Clip || t.plays == TL::Plays::Held ||
                           (t.queued && t.queued->request.action == Action::Clip);
                return t.plays != TL::Plays::Arrangement ||
                       (t.queued && t.queued->request.action != Action::Arrangement);
            });
        };
        addChecked(registry, std::move(c));
    }
    [&] {
        Command<NoArgs, LaunchStateResult> c;
        c.name = "launch.state";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "What each track with clips plays, and what it has queued.";
        c.description =
            "plays is arrangement, clip, held (a once clip past its length, silent) or stopped, "
            "as heard. position is the playing clip's place in its period, in beats for a MIDI "
            "clip and seconds for an audio clip. queued is a launch not heard yet, with the "
            "song position it applies at. Tracks without clips always follow the arrangement "
            "and are not listed.";
        c.tags = {"clip launcher", "transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        auto queued = schema::object<LaunchQueued>("launch_queued")
                          .field("action", &LaunchQueued::action, schema::string())
                          .field("clip", &LaunchQueued::clip, schema::string())
                          .field("beat", &LaunchQueued::beat, schema::number())
                          .field("seconds", &LaunchQueued::seconds, schema::number())
                          .build();
        auto track = schema::object<LaunchTrackState>("launch_track_state")
                         .field("track", &LaunchTrackState::track, schema::string())
                         .field("plays", &LaunchTrackState::plays, schema::string())
                         .field("clip", &LaunchTrackState::clip, schema::string())
                         .field("position", &LaunchTrackState::position, schema::number())
                         .field("queued", &LaunchTrackState::queued, queued)
                         .build();
        c.result = schema::object<LaunchStateResult>("launch_state")
                       .field("playing", &LaunchStateResult::playing, schema::boolean())
                       .field("tracks", &LaunchStateResult::tracks, schema::arrayOf(track))
                       .build();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"}};
        c.examples = {
            {"Before any launch", j("{}"),
             j(R"({"playing": false, "tracks": [{"track": "tr_1", "plays": "arrangement"}]})"),
             "song_with_clip_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<LaunchStateResult> {
            EngineApp& app = engine::app(ctx);
            auto engine = runningEngine(app);
            if (!engine) return std::move(engine).error();
            const double rate = (*engine)->config().sampleRate;
            LaunchStateResult out;
            out.playing = (*engine)->transport().playing;
            for (const TL& t : (*engine)->launchState()) {
                LaunchTrackState e;
                e.track = t.track.str();
                e.plays = playsName(t.plays);
                if (t.plays == TL::Plays::Clip || t.plays == TL::Plays::Held) e.clip = t.clip.str();
                e.position = t.position;
                if (t.queued) {
                    LaunchQueued q;
                    q.action = actionName(t.queued->request.action);
                    if (t.queued->request.action == Action::Clip)
                        q.clip = t.queued->request.clip.str();
                    q.beat = t.queued->placed.anchorBeat;
                    q.seconds = double(t.queued->placed.anchorSample) / rate;
                    e.queued = q;
                }
                out.tracks.push_back(std::move(e));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
