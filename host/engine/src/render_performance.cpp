// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The launcher performance a render plays (engine.md §3.1).
#include "render_performance.h"

#include <algorithm>

#include "clip_launcher.h"
#include "plan.h"
#include "supple/engine/app.h"
#include "supple/model/session.h"
#include "supple/time/timeline.h"

namespace supple::engine {

namespace {

constexpr std::size_t kMaxLaunches = 100000;  // the render's own; launch.commit has none

struct LauncherSelector {
    std::string from;
    double startBeat = 0, endBeat = 0;
};

schema::Schema<LauncherSelector> launcherSchema() {
    return schema::object<LauncherSelector>("performance_from_launcher")
        .field("from", &LauncherSelector::from,
               schema::enumeration<std::string>({{"launcher", "launcher"}}))
        .field("start_beat", &LauncherSelector::startBeat, schema::number().min(0))
        .field("end_beat", &LauncherSelector::endBeat, schema::number().exclusiveMin(0))
        .build();
}

// The master playrate as an offline render plays it (plan.cpp's rule, with
// no sync chase offline).
bool rateEnabled(const model::Session& s) {
    return s.project->playrate.rate != 1 || model::activePlayrateEnvelope(s).has_value();
}
bool rateStretch(const model::Session& s) {
    return rateEnabled(s) && s.project->playrate.preservePitch;
}

Error launchesError(std::string message, std::string path) {
    return Error("render.performance.launches", std::move(message), std::move(path),
                 "give the launches in frame order, each within frames");
}

// Each launch against the version rendered, without a plan: what the
// offline replay could not play exactly is refused.
Status checkPerformance(const CapturedPerformance& p, const model::Session& s, double rate) {
    if (p.sampleRate != rate)
        return Error("render.performance.sample_rate",
                     "the performance runs at " + json::formatDouble(p.sampleRate) +
                         " Hz and the render at " + json::formatDouble(rate) + " Hz",
                     "/performance/sample_rate",
                     "launch.commit it (commit converts the rate) and render the timeline");
    if (p.frames <= 0)
        return Error("render.empty", "the performance has no running time", "/performance/frames",
                     "give frames above 0, or capture a performance that ran");
    if (p.launches.size() > kMaxLaunches)
        return Error("render.performance.too_many",
                     std::to_string(p.launches.size()) + " launches, more than the render's " +
                         std::to_string(kMaxLaunches),
                     "/performance/launches", "launch.commit it and render the timeline");
    if (p.loop && rateEnabled(s))
        return Error("render.loop", "a looped performance cannot follow a master playrate",
                     "/performance/loop",
                     "launch.commit it and render the timeline, or capture without the loop");
    const bool stretch = rateStretch(s);
    std::int64_t previous = 0;
    for (std::size_t i = 0; i < p.launches.size(); ++i) {
        const CapturedLaunch& e = p.launches[i];
        const std::string path = "/performance/launches/" + std::to_string(i);
        if (e.frame < previous)
            return launchesError("launch " + std::to_string(i) + " at frame " +
                                     std::to_string(e.frame) + " comes before the one ahead of it",
                                 path + "/frame");
        if (e.frame >= p.frames)
            return launchesError("launch " + std::to_string(i) + " at frame " +
                                     std::to_string(e.frame) + " is past the performance's " +
                                     std::to_string(p.frames) + " frames",
                                 path + "/frame");
        previous = e.frame;
        const auto track = model::Id::parse(e.track);
        const model::Track* tr = track ? s.tracks.find(*track) : nullptr;
        if (!tr || tr->type != model::StripType::Channel)
            return Error("track.unknown",
                         tr ? e.track + " is not a channel" : "no track " + e.track,
                         path + "/track",
                         "render a version that has the track (version), or give the "
                         "performance without its launches");
        if (e.action != "clip") continue;
        if (!e.clip)
            return launchesError("launch " + std::to_string(i) + " names no clip", path + "/clip");
        auto clip = existing<model::Clip>(s, *e.clip, path + "/clip");
        if (!clip) return std::move(clip).error();
        const model::Clip& c = *s.clips.find(*clip);
        if (c.track != *track)
            return launchesError(*e.clip + " is not " + e.track + "'s clip", path + "/clip");
        if (tr->freeze) return trackFrozen(*track, path + "/clip");
        if (stretch && c.kind != model::ClipKind::Midi && detail::clipHasTakeChain(s, c))
            return Error("launch.playrate",
                         *e.clip +
                             " is an audio clip with take FX or envelopes, which cannot play "
                             "while the master playrate keeps the pitch",
                         path + "/clip",
                         "turn off the master playrate's preserve pitch, or set it to 1 without "
                         "an envelope, or launch a copy of the clip without take FX");
    }
    return {};
}

// The launcher as it plays now, from `startBeat` to `endBeat` on `s`: each
// track playing a clip (or held) launches it at frame 0, anchored there; a
// stopped track stops; a track on its arrangement gets nothing.
CapturedPerformance launcherPerformance(EngineApp& app, const PerformanceRequest& r,
                                        const model::Session& s, double rate) {
    CapturedPerformance p;
    p.sampleRate = rate;
    const time::TimeMap& tm = *s.timeMap;
    p.start = time::samplesAt(tm.secondsAt(r.startBeat), rate);
    p.frames = time::samplesAt(tm.secondsAt(r.endBeat), rate) - p.start;
    const Engine* engine = app.engine();
    if (!engine) return p;  // no launcher state: every track follows its arrangement
    using Plays = Engine::TrackLaunch::Plays;
    for (const Engine::TrackLaunch& t : engine->launchState()) {
        if (t.plays == Plays::Arrangement) continue;
        CapturedLaunch l;
        l.track = t.track.str();
        l.action = t.plays == Plays::Stopped ? "stop" : "clip";
        if (t.plays != Plays::Stopped) l.clip = t.clip.str();
        l.anchor = p.start;
        l.anchorBeat = r.startBeat;
        p.launches.push_back(std::move(l));
    }
    return p;
}

}  // namespace

Result<PerformanceRequest> decodePerformance(const json::Value& v) {
    PerformanceRequest out;
    if (v.isString()) {
        if (v.asString() != "captured")
            return Error("schema.enum", "performance is \"captured\" or an object", "/performance",
                         "give \"captured\", a captured_performance (launch.commit's), or "
                         "{\"from\": \"launcher\", \"start_beat\": b, \"end_beat\": e}");
        out.kind = PerformanceRequest::Kind::Captured;
        return out;
    }
    schema::DecodeCtx ctx;
    if (v.isObject() && v.find("from")) {
        LauncherSelector sel;
        if (!launcherSchema().decodeInto(v, sel, ctx, "/performance")) return ctx.errors.front();
        if (!(sel.endBeat > sel.startBeat))
            return Error("render.empty", "end_beat is not after start_beat",
                         "/performance/end_beat", "give an end_beat after start_beat");
        out.kind = PerformanceRequest::Kind::Launcher;
        out.startBeat = sel.startBeat;
        out.endBeat = sel.endBeat;
        return out;
    }
    if (!capturedPerformanceSchema().decodeInto(v, out.given, ctx, "/performance"))
        return ctx.errors.front();
    out.kind = PerformanceRequest::Kind::Given;
    return out;
}

std::optional<std::uint64_t> capturedVersion(EngineApp& app) {
    const auto c = app.launchCapture();
    if (!c || !c->capture.placed) return std::nullopt;
    return c->capture.version;
}

Result<RenderPerformance> resolvePerformance(EngineApp& app, const PerformanceRequest& request,
                                             const model::Session& s, double rate) {
    RenderPerformance out;
    switch (request.kind) {
        case PerformanceRequest::Kind::Captured: {
            const auto c = app.launchCapture();
            if (!c || !c->capture.placed || c->capture.frames <= 0)
                return Error("launch.capture", "no performance has been captured", "/performance",
                             "launch.capture {on: true}, play and launch clips, then render");
            out.perf = performanceOf(c->capture, c->sampleRate);
            const Engine::CaptureFacts& f = c->capture.facts;
            auto& why = out.notHeardBecause;
            if (!f.fromPlayStart) why.push_back("began_mid_run");
            if (f.edited) why.push_back("edited_during");
            if (f.rateChanged) why.push_back("playrate_changed");
            if (f.liveInput) why.push_back("live_input");
            if (f.dropout) why.push_back("dropout");
            if (f.pluginFailed) why.push_back("plugin_failed");
            if (c->endedBy != EngineApp::CaptureEnd::None) why.push_back("ended_by_engine");
            if (s.version != c->capture.version) why.push_back("other_version");
            out.capturedOn = c->capture.version;
            break;
        }
        case PerformanceRequest::Kind::Given:
            out.perf = request.given;
            out.notHeardBecause = {"given"};
            break;
        case PerformanceRequest::Kind::Launcher:
            out.perf = launcherPerformance(app, request, s, rate);
            out.notHeardBecause = {"from_launcher"};
            break;
    }
    SUPPLE_TRY(checkPerformance(out.perf, s, rate));
    out.asHeard = out.notHeardBecause.empty();
    // Canonical: as the schema writes it, so the object rendered round-trips.
    auto canonical =
        capturedPerformanceSchema().decode(capturedPerformanceSchema().encode(out.perf));
    if (canonical) out.perf = std::move(*canonical);
    const time::TimeMap& tm = *s.timeMap;
    out.startBeat = tm.beatAt(double(out.perf.start) / rate);
    out.endBeat = tm.beatAt(double(out.perf.start + out.perf.frames) / rate);
    return out;
}

void applyPerformance(const RenderPerformance& p, OfflineSettings& settings) {
    const double rate = p.perf.sampleRate;
    settings.startSeconds = double(p.perf.start) / rate;
    // Running time: with a loop the render wraps at it and runs on.
    settings.endSeconds = double(p.perf.start + p.perf.frames) / rate;
    settings.loop.reset();
    if (p.perf.loop)
        settings.loop =
            std::pair{double(p.perf.loop->start) / rate, double(p.perf.loop->end) / rate};
    settings.launches.clear();
    settings.launchesInSong = true;
    for (const CapturedLaunch& l : p.perf.launches) {
        LaunchLogEntry e;
        e.runningFrame = l.frame;
        e.track = *model::Id::parse(l.track);
        e.action = l.action == "clip"   ? LaunchLogEntry::Action::Clip
                   : l.action == "stop" ? LaunchLogEntry::Action::Stop
                                        : LaunchLogEntry::Action::Arrangement;
        if (l.clip) e.clip = *model::Id::parse(*l.clip);
        if (l.scene) e.scene = model::Id::parse(*l.scene);
        e.anchorSample = l.anchor;
        e.anchorBeat = l.anchorBeat;
        e.songFrame = p.perf.start + l.frame;
        settings.launches.push_back(e);
    }
}

void addPerformanceResult(json::Value& out, const RenderPerformance& p) {
    out["performance"] = capturedPerformanceSchema().encode(p.perf);
    json::Value range = json::Value::object();
    range["start_beat"] = p.startBeat;
    range["end_beat"] = p.endBeat;
    out["range"] = std::move(range);
    out["as_heard"] = p.asHeard;
    if (!p.asHeard) {
        json::Value why = json::Value::array();
        for (const std::string& reason : p.notHeardBecause) why.push_back(reason);
        out["not_heard_because"] = std::move(why);
    }
}

Error notAsHeard(const RenderPerformance& p) {
    const std::string first = p.notHeardBecause.empty() ? "given" : p.notHeardBecause.front();
    const std::string hint =
        first == "other_version"   ? "render version " + std::to_string(p.capturedOn.value_or(0)) +
                                         ", the version it was captured on"
        : first == "began_mid_run" ? "capture from stop: launch.capture {on: true} before play"
        : first == "edited_during" ? "capture again without editing during it"
        : first == "playrate_changed" ? "capture again without changing the playrate during it"
        : first == "live_input"       ? "capture again with no armed or monitored input"
        : first == "dropout"          ? "capture again: the output dropped out during it"
        : first == "plugin_failed"    ? "capture again: a plug-in failed during it"
        : first == "ended_by_engine"  ? "the engine stopped during it: capture again"
        : first == "from_launcher"
            ? "render a capture (performance: \"captured\") to require as_heard"
            : "render a capture (performance: \"captured\") to require as_heard";
    std::string because;
    for (const std::string& reason : p.notHeardBecause)
        because += (because.empty() ? "" : ", ") + reason;
    return Error("render.not_as_heard", "the performance would not render as heard: " + because,
                 "/performance", hint);
}

}  // namespace supple::engine
