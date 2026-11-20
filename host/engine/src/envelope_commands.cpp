// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Automation commands: env.* and autoitem.* (session-model §7).
#include "clip_launcher.h"
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- automation envelopes ---------------------------------------------------------------

struct EnvPointArg {
    std::optional<double> beat, seconds;
    std::optional<std::int64_t> bar;
    json::Value value;
    std::optional<model::PointShape> shape;
    std::optional<double> tension;
};

struct EnvAddArgs {
    std::optional<std::string> track, fx, route, take, group, module;
    std::string param;
    std::optional<std::vector<EnvPointArg>> points;
};

struct EnvAddResult {
    std::string env;
    std::int64_t points = 0;
};

struct EnvPointsSetArgs {
    std::string env;
    std::vector<EnvPointArg> points;
    std::optional<double> fromBeat, toBeat;
};

struct EnvCount {
    std::int64_t points = 0;
};

struct EnvRef {
    std::string env;
};

struct EnvSetArgs {
    std::string env;
    std::optional<bool> active, visible;
    std::optional<model::EnvMode> mode;
};

struct EnvListArgs {
    std::optional<std::string> object;
};

struct EnvEntry {
    std::string env, target, param, mode;
    std::int64_t points = 0;
    bool active = true, visible = true;
};

struct EnvList {
    std::vector<EnvEntry> envelopes;
};

schema::Schema<model::PointShape> pointShapeSchema() {
    return schema::enumeration<model::PointShape>({{model::PointShape::Linear, "linear"},
                                                   {model::PointShape::Square, "square"},
                                                   {model::PointShape::Slow, "slow"},
                                                   {model::PointShape::FastStart, "fast_start"},
                                                   {model::PointShape::FastEnd, "fast_end"},
                                                   {model::PointShape::Bezier, "bezier"}});
}

schema::Schema<model::EnvMode> envModeSchema() {
    return schema::enumeration<model::EnvMode>({{model::EnvMode::Trim, "trim"},
                                                {model::EnvMode::Read, "read"},
                                                {model::EnvMode::Touch, "touch"},
                                                {model::EnvMode::Latch, "latch"},
                                                {model::EnvMode::LatchPreview, "latch_preview"},
                                                {model::EnvMode::Write, "write"}});
}

schema::Schema<EnvPointArg> envPointSchema() {
    return schema::object<EnvPointArg>("env_point_arg")
        .field("beat", &EnvPointArg::beat, schema::number().min(0))
        .field("bar", &EnvPointArg::bar,
               schema::integer().range(1, std::numeric_limits<int>::max()))
        .field("seconds", &EnvPointArg::seconds, schema::number().min(0))
        .field("value", &EnvPointArg::value, schema::any())
        .field("shape", &EnvPointArg::shape, pointShapeSchema())
        .field("tension", &EnvPointArg::tension, schema::number().range(-1, 1))
        .build();
}

// `itemRelative`: a take's envelope, positions from its owner's start: beats
// from an item's start, or in a clip's unit from the clip's start (`clip`:
// beats for MIDI, seconds for audio; ADR-0097).
Result<std::vector<model::EnvPoint>> envPoints(const model::Session& s, const EnvDomain& d,
                                               const std::vector<EnvPointArg>& in,
                                               const std::string& path, bool itemRelative = false,
                                               model::Timebase timebase = model::Timebase::Beats,
                                               std::optional<model::ClipKind> clip = std::nullopt) {
    std::vector<model::EnvPoint> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const EnvPointArg& a = in[i];
        const std::string at = path + "/" + std::to_string(i);
        if (itemRelative && clip) {
            const bool audio = *clip == model::ClipKind::Audio;
            if (audio ? !a.seconds || a.beat || a.bar : !a.beat || a.seconds || a.bar)
                return Error("clip.unsupported",
                             audio ? "an audio clip's take envelope takes seconds from its start"
                                   : "a MIDI clip's take envelope takes beats from its start",
                             at,
                             audio ? "e.g. {\"seconds\": 0.5, \"value\": -6}; a clip has no bars"
                                   : "e.g. {\"beat\": 2, \"value\": -6}; a clip has no bars");
            const double pos = audio ? *a.seconds : *a.beat;
            if (!std::isfinite(pos) || pos < 0 || pos > 1e9)
                return Error("env.position", "a point's position must be finite and not negative",
                             at, "give a position within the clip");
            model::EnvPoint pt;
            pt.pos = pos;
            auto v = envValue(d, a.value, at + "/value");
            if (!v) return std::move(v).error();
            pt.value = *v;
            pt.shape = a.shape.value_or(model::PointShape::Linear);
            pt.tension = a.tension.value_or(0.0);
            out.push_back(pt);
            continue;
        }
        if (itemRelative && !a.beat)
            return Error("env.position", "a take's envelope takes beats from the item's start", at,
                         "e.g. {\"beat\": 0, \"value\": -6} at the item's start");
        const int given = a.beat.has_value() + a.bar.has_value() + a.seconds.has_value();
        if (given != 1)
            return Error("env.position", "give one of beat, bar or seconds", at,
                         "provide exactly one of beat, bar or seconds for each point");
        model::EnvPoint pt;
        pt.pos = a.seconds
                     ? s.timeMap->beatAt(*a.seconds)
                     : beatsOf(s, a.beat,
                               a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
        SUPPLE_TRY(model::checkedBeat(s, pt.pos,
                                      at + (a.seconds ? "/seconds"
                                            : a.bar   ? "/bar"
                                                      : "/beat")));
        if (timebase == model::Timebase::Time)
            pt.pos = a.seconds.value_or(s.timeMap->secondsAt(pt.pos));
        auto v = envValue(d, a.value, at + "/value");
        if (!v) return std::move(v).error();
        pt.value = *v;
        pt.shape = a.shape.value_or(model::PointShape::Linear);
        pt.tension = a.tension.value_or(0.0);
        out.push_back(pt);
    }
    std::stable_sort(
        out.begin(), out.end(),
        [](const model::EnvPoint& x, const model::EnvPoint& y) { return x.pos < y.pos; });
    return out;
}

}  // namespace

void registerEnvelopeCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- env.*
    [&] {
        Command<EnvAddArgs, EnvAddResult> c;
        c.name = "env.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Automate a parameter: a track's or route's mixer control, or a plugin's.";
        c.description =
            "Target one of track (param volume|pan|mute; width|pan_left|pan_right|pan_boost on "
            "channels/busses; playrate only on tr_0), route "
            "(volume|pan|mute), group (VCA volume|mute), or "
            "fx (a semantic key such as \"gain\" or \"filter1.cutoff\", or for other plugins a "
            "CLAP id or parameter name), or module (a rack module's knob by key, values in its "
            "unit, a choice by its key; module.describe). points: [{beat|bar|seconds, value, "
            "shape?, tension?}]; "
            "values in the parameter's unit (dB, %, Hz… — text like \"800 Hz\" works for "
            "plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values "
            "move along the parameter's curve (a log cutoff sweeps evenly in pitch). take (param "
            "volume|pan|mute): a take envelope, its points in beats from the item's start (it "
            "moves with the item), or on a clip's take from the clip's start in beats (MIDI) or "
            "seconds (audio); it shapes the take after its take FX (a MIDI take needs an "
            "instrument in its take FX). Master playrate points are finite numeric ratios "
            "0.25..4.0; an active nonempty master curve replaces the saved scalar during "
            "playback and rendering. Audio sources convert before take FX/live mixing; MIDI "
            "and automation follow the same timeline clock. Width and dual pans use -100..100 "
            "percent; pan_boost uses 0..1 (on at 0.5), overriding the effective law's "
            "boost while the envelope is active and nonempty.";
        c.tags = {"automation"};
        c.args = schema::object<EnvAddArgs>("env_add_args")
                     .field("track", &EnvAddArgs::track, idArg("tr", "Track"))
                     .field("module", &EnvAddArgs::module, idArg("mo", "Rack module"))
                     .field("take", &EnvAddArgs::take, idArg("tk", "Take"))
                     .field("group", &EnvAddArgs::group, idArg("gr", "VCA group"))
                     .field("fx", &EnvAddArgs::fx, idArg("fx", "Plugin instance"))
                     .field("route", &EnvAddArgs::route, idArg("sd", "Route"))
                     .field("param", &EnvAddArgs::param, schema::string())
                     .field("points", &EnvAddArgs::points, schema::arrayOf(envPointSchema()))
                     .build();
        c.result = schema::object<EnvAddResult>("env_add")
                       .field("env", &EnvAddResult::env, schema::string())
                       .field("points", &EnvAddResult::points, schema::integer())
                       .build();
        c.examples = {
            {"Fade a track out over bar 2",
             j(R"({"track": "tr_1", "param": "volume", "points": [{"bar": 2, "value": 0}, {"bar": 3, "value": "-60 dB"}]})"),
             j(R"({"env": "en_1", "points": 2})"), "song"},
            {"Sweep a plugin parameter",
             j(R"({"fx": "fx_2", "param": "gain", "points": [{"beat": 0, "value": "-12 dB", "shape": "slow"}, {"beat": 4, "value": 0}]})"),
             j(R"({"env": "en_1", "points": 2})"), "song_with_fx"},
            {"Duck a take halfway through its item",
             j(R"({"take": "tk_1", "param": "volume", "points": [{"beat": 0, "value": 0}, {"beat": 2, "value": "-12 dB"}]})"),
             j(R"({"env": "en_1", "points": 2})"), "song_with_audio"},
            {"A rack module's knob: a VCA opening over a bar",
             j(R"({"module": "mo_3", "param": "level", "points": [{"beat": 0, "value": -40}, {"beat": 4, "value": 0}]})"),
             j(R"({"env": "en_1", "points": 2})"), "song_with_rack"},
            {"Master rate envelope",
             j(R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1},{"beat":4,"value":2}]})"),
             j(R"({"env":"en_1","points":2})"), "empty"}};
        c.run = [](const EnvAddArgs& a, Context& ctx) -> Result<EnvAddResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            if (a.track.has_value() + a.fx.has_value() + a.route.has_value() + a.take.has_value() +
                    a.group.has_value() + a.module.has_value() !=
                1)
                return Error("env.target", "give one of track, fx, route, take, group or module",
                             "", "e.g. {\"fx\": \"fx_1\", \"param\": \"filter1.cutoff\"}");
            model::Envelope env;
            if (a.take) {
                auto id = existing<model::Take>(s, *a.take, "/take");
                if (!id) return std::move(id).error();
                const model::Take& take = *s.takes.find(*id);
                if (take.source && take.source->kind == model::Kind::Midi) {
                    bool instrument = false;
                    for (Id f : take.chain)
                        if (auto info = app.findPlugin(s.fx.find(f)->plugin.id))
                            instrument = instrument || info->instrument;
                    if (!instrument)
                        return Error("env.take_midi",
                                     "a MIDI take's envelope needs an instrument in its take FX",
                                     "/take",
                                     "fx.add {take, plugin: an instrument} first, or automate "
                                     "the track");
                }
                env.target.object = *id;
            } else if (a.group) {
                auto id = existing<model::TrackGroup>(s, *a.group, "/group");
                if (!id) return id.error();
                if (!s.groups.find(*id)->vca || (a.param != "volume" && a.param != "mute"))
                    return Error("env.target", "VCA target must be volume or mute", "/group",
                                 "add a VCA with vca.add and choose volume or mute");
                env.target.object = *id;
            } else if (a.track) {
                auto id = existing<model::Track>(s, *a.track, "/track");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else if (a.fx) {
                auto id = existing<model::FxInstance>(s, *a.fx, "/fx");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else if (a.module) {
                auto id = existing<model::RackModule>(s, *a.module, "/module");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else {
                auto id = existing<model::Route>(s, *a.route, "/route");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            }
            env.target.param = a.param;
            for (const auto& e : s.envelopes)
                if (e.value->target.object == env.target.object && e.value->target.param == a.param)
                    return Error("env.exists", "an envelope already automates this parameter",
                                 "/param", "env.points.set " + e.id.str());
            auto domain = envDomain(app, s, env.target, "/param");
            if (!domain) return std::move(domain).error();
            if (a.points) {
                auto pts = envPoints(
                    s, *domain, *a.points, "/points", a.take.has_value(), model::Timebase::Beats,
                    a.take ? clipKindOfTake(s, *model::Id::parse(*a.take)) : std::nullopt);
                if (!pts) return std::move(pts).error();
                env.points = std::move(*pts);
            }
            const Id id = app.txn().create(env);
            return EnvAddResult{id.str(), static_cast<std::int64_t>(env.points.size())};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        struct WritePoint {
            double pos = 0, value = 0;
        };
        struct WriteArgs {
            std::string env;
            double after = 0, through = 0;
            std::vector<WritePoint> points;
        };
        Command<WriteArgs, Empty> c;
        c.name = "env.write.commit";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::Internal;
        c.summary = "Record what a write pass wrote into an envelope as a replayable edit.";
        c.description =
            "Points with after < pos <= through go and the given points come in, in the "
            "envelope's own units (beats or seconds by its timebase, stored values).";
        c.args =
            schema::object<WriteArgs>("env_write_commit_args")
                .field("env", &WriteArgs::env, idArg("en", "Envelope"))
                .field("after", &WriteArgs::after, schema::number())
                .field("through", &WriteArgs::through, schema::number())
                .field("points", &WriteArgs::points,
                       schema::arrayOf(schema::object<WritePoint>("env_write_point")
                                           .field("pos", &WritePoint::pos, schema::number())
                                           .field("value", &WritePoint::value, schema::number())
                                           .build()))
                .build();
        c.result = emptySchema();
        c.run = [](const WriteArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& engine = app(ctx);
            auto id = existing<model::Envelope>(engine.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            std::vector<model::EnvPoint> points;
            for (const auto& p : a.points) points.push_back({p.pos, p.value});
            engine.writeEnvelope(*id, a.after, a.through, points, false);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvPointsSetArgs, EnvCount> c;
        c.name = "env.points.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace an envelope's points (all of them, or those in a beat range).";
        c.description = "from_beat/to_beat: replace only points with from_beat <= beat < to_beat.";
        c.tags = {"automation"};
        c.args = schema::object<EnvPointsSetArgs>("env_points_set_args")
                     .field("env", &EnvPointsSetArgs::env, idArg("en", "Envelope"))
                     .field("points", &EnvPointsSetArgs::points, schema::arrayOf(envPointSchema()))
                     .field("from_beat", &EnvPointsSetArgs::fromBeat, schema::number().min(0))
                     .field("to_beat", &EnvPointsSetArgs::toBeat, schema::number().min(0))
                     .build();
        c.result = schema::object<EnvCount>("env_count")
                       .field("points", &EnvCount::points, schema::integer())
                       .build();
        c.examples = {
            {"Three points",
             j(R"({"env": "en_1", "points": [{"beat": 0, "value": -6}, {"beat": 2, "value": 0, "shape": "square"}, {"beat": 3, "value": "-3 dB"}]})"),
             j(R"({"points": 3})"), "song_with_env"}};
        c.run = [](const EnvPointsSetArgs& a, Context& ctx) -> Result<EnvCount> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Envelope>(s, a.env, "/env");
            if (!id) return std::move(id).error();
            const model::Envelope& env = *s.envelopes.find(*id);
            auto domain = envDomain(app, s, env.target, "/env");
            if (!domain) return std::move(domain).error();
            auto pts = envPoints(s, *domain, a.points, "/points",
                                 env.target.object.kind == model::Kind::Take, env.timebase,
                                 env.target.object.kind == model::Kind::Take
                                     ? clipKindOfTake(s, env.target.object)
                                     : std::nullopt);
            if (!pts) return std::move(pts).error();
            std::vector<model::EnvPoint> next;
            if (a.fromBeat || a.toBeat) {
                const auto position = [&](double beat) {
                    return env.timebase == model::Timebase::Time ? s.timeMap->secondsAt(beat)
                                                                 : beat;
                };
                const double lo = a.fromBeat ? position(*a.fromBeat) : 0.0;
                const double hi =
                    a.toBeat ? position(*a.toBeat) : std::numeric_limits<double>::infinity();
                for (const auto& pt : env.points)
                    if (pt.pos < lo || pt.pos >= hi) next.push_back(pt);
            }
            next.insert(next.end(), pts->begin(), pts->end());
            std::stable_sort(next.begin(), next.end(),
                             [](const auto& x, const auto& y) { return x.pos < y.pos; });
            const auto count = static_cast<std::int64_t>(next.size());
            app.txn().modify<model::Envelope>(
                *id, [&](model::Envelope& e) { e.points = std::move(next); });
            return EnvCount{count};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvSetArgs, Empty> c;
        c.name = "env.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Set an envelope's options: active (heard), mode (read, touch, latch, write).";
        c.description =
            "visible shows or hides the envelope's lane in the arrange view (a hidden envelope "
            "still plays while active).";
        c.tags = {"automation"};
        c.args = schema::object<EnvSetArgs>("env_set_args")
                     .field("env", &EnvSetArgs::env, idArg("en", "Envelope"))
                     .field("active", &EnvSetArgs::active, schema::boolean())
                     .field("visible", &EnvSetArgs::visible, schema::boolean())
                     .field("mode", &EnvSetArgs::mode, envModeSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Mute the automation", j(R"({"env": "en_1", "active": false})"), j("{}"),
                       "song_with_env"}};
        c.run = [](const EnvSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            auto id = existing<model::Envelope>(app.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            app.txn().modify<model::Envelope>(*id, [&](model::Envelope& e) {
                if (a.active) e.active = *a.active;
                if (a.visible) e.visible = *a.visible;
                if (a.mode) e.mode = *a.mode;
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvRef, Empty> c;
        c.name = "env.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove an envelope (the parameter returns to its set value).";
        c.tags = {"automation"};
        c.args = schema::object<EnvRef>("env_ref")
                     .field("env", &EnvRef::env, idArg("en", "Envelope"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"env": "en_1"})"), j("{}"), "song_with_env"}};
        c.run = [](const EnvRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            auto id = existing<model::Envelope>(app.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            app.txn().remove<model::Envelope>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvListArgs, EnvList> c;
        c.name = "env.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List envelopes (optionally of one track, route or plugin).";
        c.tags = {"automation"};
        c.args = schema::object<EnvListArgs>("env_list_args")
                     .field("object", &EnvListArgs::object, schema::string())
                     .build();
        auto entry = schema::object<EnvEntry>("env_entry")
                         .field("env", &EnvEntry::env, schema::string())
                         .field("target", &EnvEntry::target, schema::string())
                         .field("param", &EnvEntry::param, schema::string())
                         .field("points", &EnvEntry::points, schema::integer())
                         .field("active", &EnvEntry::active, schema::boolean())
                         .field("visible", &EnvEntry::visible, schema::boolean())
                         .field("mode", &EnvEntry::mode, schema::string())
                         .build();
        c.result = schema::object<EnvList>("env_list")
                       .field("envelopes", &EnvList::envelopes, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"All", j("{}"),
             j(R"({"envelopes": [{"env": "en_1", "target": "fx_2", "param": "gain", "points": 2, "active": true, "visible": true, "mode": "read"}]})"),
             "song_with_env"}};
        c.run = [](const EnvListArgs& a, Context& ctx) -> Result<EnvList> {
            const model::Session s = app(ctx).view();
            static constexpr const char* kModes[] = {"trim",  "read",          "touch",
                                                     "latch", "latch_preview", "write"};
            EnvList out;
            for (const auto& e : s.envelopes) {
                const model::Envelope& env = *e.value;
                if (a.object && env.target.object.str() != *a.object) continue;
                out.envelopes.push_back({e.id.str(), env.target.object.str(), env.target.param,
                                         kModes[static_cast<int>(env.mode)],
                                         static_cast<std::int64_t>(env.points.size()), env.active,
                                         env.visible});
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- autoitem.* (gap log #28)
    [&] {
        struct ItemPointArg {
            double beat = 0;  // from the item's start
            json::Value value;
            std::optional<model::PointShape> shape;
            std::optional<double> tension;
        };
        struct AutoItemAddArgs {
            std::string env;
            std::optional<double> position;
            std::optional<std::int64_t> bar;
            double length = 4;
            std::optional<std::vector<ItemPointArg>> points;
            std::optional<double> sourceLength;
            std::optional<bool> loop;
            std::optional<std::string> name;
        };
        struct AutoItemSetArgs {
            std::string item;
            std::optional<double> position, length, sourceLength, baseline, amplitude;
            std::optional<bool> loop;
            std::optional<std::vector<ItemPointArg>> points;
            std::optional<std::string> name;
        };
        struct AutoItemRef {
            std::string item;
        };
        struct AutoItemDupArgs {
            std::string item;
            std::optional<double> position;
            bool pooled = true;
        };
        auto pointArgSchema = [] {
            return schema::object<ItemPointArg>("item_point")
                .field("beat", &ItemPointArg::beat, schema::number().min(0))
                .field("value", &ItemPointArg::value, schema::any())
                .field("shape", &ItemPointArg::shape,
                       schema::enumeration<model::PointShape>(
                           {{model::PointShape::Linear, "linear"},
                            {model::PointShape::Square, "square"},
                            {model::PointShape::Slow, "slow"},
                            {model::PointShape::FastStart, "fast_start"},
                            {model::PointShape::FastEnd, "fast_end"},
                            {model::PointShape::Bezier, "bezier"}}))
                .field("tension", &ItemPointArg::tension, schema::number().range(-1, 1))
                .build();
        };
        // An item's points in its envelope's units, sorted.
        auto itemPoints = [](EngineApp& app, const model::Session& s, const model::Envelope& env,
                             const std::vector<ItemPointArg>& in,
                             const std::string& path) -> Result<std::vector<model::EnvPoint>> {
            auto domain = envDomain(app, s, env.target, path.c_str());
            if (!domain) return std::move(domain).error();
            std::vector<model::EnvPoint> out;
            for (std::size_t i = 0; i < in.size(); ++i) {
                auto v = envValue(*domain, in[i].value, path + "/" + std::to_string(i) + "/value");
                if (!v) return std::move(v).error();
                out.push_back({in[i].beat, *v, in[i].shape.value_or(model::PointShape::Linear),
                               in[i].tension.value_or(0.0)});
            }
            std::stable_sort(out.begin(), out.end(),
                             [](const auto& a, const auto& b) { return a.pos < b.pos; });
            return out;
        };
        // An edit to a pooled item's points or source length reaches its pool.
        auto propagate = [](model::EditTxn& t, const model::Session& s, Id from,
                            const model::AutomationItem& edited) {
            if (edited.pool <= 0) return;
            for (const auto& e : s.autoItems)
                if (e.id != from && e.value->pool == edited.pool)
                    t.modify<model::AutomationItem>(e.id, [&](model::AutomationItem& x) {
                        x.points = edited.points;
                        x.sourceLength = edited.sourceLength;
                    });
        };
        auto entryOf = [](Id id, const model::AutomationItem& ai) {
            json::Value e = json::Value::object();
            e["id"] = id.str();
            e["env"] = ai.envelope.str();
            e["position"] = ai.position;
            e["length"] = ai.length;
            e["source_length"] = ai.sourceLength;
            e["loop"] = ai.loop;
            e["pool"] = static_cast<std::int64_t>(ai.pool);
            e["baseline"] = ai.baseline;
            e["amplitude"] = ai.amplitude;
            e["name"] = ai.name;
            json::Value pts = json::Value::array();
            for (const auto& p : ai.points) {
                json::Value pt = json::Value::object();
                pt["beat"] = p.pos;
                pt["value"] = p.value;
                pts.push_back(std::move(pt));
            }
            e["points"] = std::move(pts);
            return e;
        };
        {
            Command<AutoItemAddArgs, json::Value> c;
            c.name = "autoitem.add";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "Put an automation item on an envelope: a block that replaces it there.";
            c.description =
                "position (beat) or bar, length in beats. points (beats from the item's start, "
                "values in the envelope's units) over source_length beats (default: length), "
                "repeated while loop (default true) and the item is longer; without points it "
                "captures what the envelope plays over that range. Duplicate it pooled "
                "(autoitem.duplicate) to edit several at once. Track, route and plugin "
                "envelopes.";
            c.tags = {"automation"};
            c.args =
                schema::object<AutoItemAddArgs>("autoitem_add_args")
                    .field("env", &AutoItemAddArgs::env, idArg("en", "Envelope"))
                    .field("position", &AutoItemAddArgs::position, schema::number().min(0))
                    .field("bar", &AutoItemAddArgs::bar,
                           schema::integer().range(1, std::numeric_limits<int>::max()))
                    .field("length", &AutoItemAddArgs::length, schema::number().exclusiveMin(0))
                    .defaultValue(4.0)
                    .field("points", &AutoItemAddArgs::points, schema::arrayOf(pointArgSchema()))
                    .field("source_length", &AutoItemAddArgs::sourceLength,
                           schema::number().exclusiveMin(0))
                    .field("loop", &AutoItemAddArgs::loop, schema::boolean())
                    .field("name", &AutoItemAddArgs::name, schema::string())
                    .build();
            c.result = schema::any();
            c.examples = {
                {"A one-bar swell, looped over two bars",
                 j(R"({"env": "en_1", "bar": 1, "length": 8, "source_length": 4, "points": [{"beat": 0, "value": -12}, {"beat": 4, "value": 0}]})"),
                 j(R"({"id": "ai_1", "env": "en_1", "position": 0.0, "length": 8.0,
                                   "source_length": 4.0, "loop": true, "pool": 0,
                                   "baseline": 0.0, "amplitude": 1.0, "name": "",
                                   "points": [{"beat": 0.0, "value": -12.0},
                                              {"beat": 4.0, "value": 0.0}]})"),
                 "song_with_env"}};
            c.run = [itemPoints, entryOf](const AutoItemAddArgs& a,
                                          Context& ctx) -> Result<json::Value> {
                EngineApp& app = engine::app(ctx);
                const model::Session s = app.view();
                auto en = existing<model::Envelope>(s, a.env, "/env");
                if (!en) return std::move(en).error();
                const model::Envelope& env = *s.envelopes.find(*en);
                if (env.target.object.kind == model::Kind::Take)
                    return Error("autoitem.take",
                                 "automation items sit on track, route and "
                                 "plugin envelopes",
                                 "/env", "edit the take envelope's points");
                if (a.position && a.bar)
                    return Error("autoitem.position", "give position or bar", "/position",
                                 "provide either position in beats or bar, not both");
                model::AutomationItem ai;
                ai.envelope = *en;
                ai.position = a.bar ? s.timeMap->beatOfBar(static_cast<int>(*a.bar))
                                    : a.position.value_or(0.0);
                ai.length = a.length;
                ai.sourceLength = a.sourceLength.value_or(a.length);
                ai.loop = a.loop.value_or(true);
                ai.name = a.name.value_or("");
                if (a.points) {
                    auto pts = itemPoints(app, s, env, *a.points, "/points");
                    if (!pts) return std::move(pts).error();
                    ai.points = std::move(*pts);
                } else {  // capture what plays there now
                    const auto played = model::effectivePoints(s, *en);
                    const double end = ai.position + ai.sourceLength;
                    ai.points.push_back({0.0, model::pointsValueAt(played, ai.position, 0.0)});
                    for (const auto& p : played)
                        if (p.pos > ai.position && p.pos < end)
                            ai.points.push_back({p.pos - ai.position, p.value, p.shape, p.tension});
                    ai.points.push_back({ai.sourceLength, model::pointsValueAt(played, end, 0.0)});
                }
                const Id id = app.txn().create(ai);
                return entryOf(id, ai);
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<AutoItemSetArgs, json::Value> c;
            c.name = "autoitem.set";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "Move, resize, loop, scale or rewrite an automation item.";
            c.description =
                "points and source_length reach every item of its pool; position, length, loop, "
                "baseline, amplitude (values play as baseline + amplitude x (value - baseline)) "
                "and name are the item's own.";
            c.tags = {"automation"};
            c.args =
                schema::object<AutoItemSetArgs>("autoitem_set_args")
                    .field("item", &AutoItemSetArgs::item, idArg("ai", "Automation item"))
                    .field("position", &AutoItemSetArgs::position, schema::number().min(0))
                    .field("length", &AutoItemSetArgs::length, schema::number().exclusiveMin(0))
                    .field("source_length", &AutoItemSetArgs::sourceLength,
                           schema::number().exclusiveMin(0))
                    .field("loop", &AutoItemSetArgs::loop, schema::boolean())
                    .field("baseline", &AutoItemSetArgs::baseline, schema::number())
                    .field("amplitude", &AutoItemSetArgs::amplitude, schema::number().range(-2, 2))
                    .field("points", &AutoItemSetArgs::points, schema::arrayOf(pointArgSchema()))
                    .field("name", &AutoItemSetArgs::name, schema::string())
                    .build();
            c.result = schema::any();
            c.examples = {{"Two beats later", j(R"({"item": "ai_1", "position": 2})"),
                           j(R"("<any>")"), "song_with_autoitem"}};
            c.run = [itemPoints, propagate, entryOf](const AutoItemSetArgs& a,
                                                     Context& ctx) -> Result<json::Value> {
                EngineApp& app = engine::app(ctx);
                const model::Session s = app.view();
                auto id = existing<model::AutomationItem>(s, a.item, "/item");
                if (!id) return std::move(id).error();
                model::AutomationItem ai = *s.autoItems.find(*id);
                if (a.position) ai.position = *a.position;
                if (a.length) ai.length = *a.length;
                if (a.sourceLength) ai.sourceLength = *a.sourceLength;
                if (a.loop) ai.loop = *a.loop;
                if (a.baseline) ai.baseline = *a.baseline;
                if (a.amplitude) ai.amplitude = *a.amplitude;
                if (a.name) ai.name = *a.name;
                if (a.points) {
                    auto pts =
                        itemPoints(app, s, *s.envelopes.find(ai.envelope), *a.points, "/points");
                    if (!pts) return std::move(pts).error();
                    ai.points = std::move(*pts);
                }
                auto& t = app.txn();
                t.put<model::AutomationItem>(*id, ai);
                if (a.points || a.sourceLength) propagate(t, s, *id, ai);
                return entryOf(*id, ai);
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<AutoItemDupArgs, json::Value> c;
            c.name = "autoitem.duplicate";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "Copy an automation item (pooled by default: edits to either reach both).";
            c.description = "position: where the copy starts (default: right after the item).";
            c.tags = {"automation"};
            c.args = schema::object<AutoItemDupArgs>("autoitem_duplicate_args")
                         .field("item", &AutoItemDupArgs::item, idArg("ai", "Automation item"))
                         .field("position", &AutoItemDupArgs::position, schema::number().min(0))
                         .field("pooled", &AutoItemDupArgs::pooled, schema::boolean())
                         .defaultValue(true)
                         .build();
            c.result = schema::any();
            c.examples = {{"A pooled copy right after it", j(R"({"item": "ai_1"})"),
                           j(R"("<any>")"), "song_with_autoitem"}};
            c.run = [entryOf](const AutoItemDupArgs& a, Context& ctx) -> Result<json::Value> {
                EngineApp& app = engine::app(ctx);
                const model::Session s = app.view();
                auto id = existing<model::AutomationItem>(s, a.item, "/item");
                if (!id) return std::move(id).error();
                model::AutomationItem copy = *s.autoItems.find(*id);
                auto& t = app.txn();
                if (a.pooled && copy.pool == 0) {  // a new pool for the two
                    int top = 0;
                    for (const auto& e : s.autoItems) top = std::max(top, e.value->pool);
                    if (top == std::numeric_limits<int>::max())
                        return Error("autoitem.pool", "pool id space is exhausted", "/pooled",
                                     "duplicate unpooled or remove the highest pool");
                    copy.pool = top + 1;
                    t.modify<model::AutomationItem>(
                        *id, [&](model::AutomationItem& x) { x.pool = copy.pool; });
                } else if (!a.pooled) {
                    copy.pool = 0;
                }
                copy.position = a.position.value_or(copy.position + copy.length);
                const Id fresh = t.create(copy);
                return entryOf(fresh, copy);
            };
            addChecked(registry, std::move(c));
        }
        for (const bool unpool : {false, true}) {
            Command<AutoItemRef, Empty> c;
            c.name = unpool ? "autoitem.unpool" : "autoitem.remove";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = unpool ? "Take an automation item out of its pool (its points stay)."
                               : "Remove an automation item (the envelope under it plays again).";
            c.tags = {"automation"};
            c.args = schema::object<AutoItemRef>(unpool ? "autoitem_unpool" : "autoitem_remove")
                         .field("item", &AutoItemRef::item, idArg("ai", "Automation item"))
                         .build();
            c.result = emptySchema();
            c.examples = {{unpool ? "Unpool" : "Remove", j(R"({"item": "ai_1"})"), j("{}"),
                           "song_with_autoitem"}};
            c.run = [unpool](const AutoItemRef& a, Context& ctx) -> Result<Empty> {
                EngineApp& app = engine::app(ctx);
                auto id = existing<model::AutomationItem>(app.view(), a.item, "/item");
                if (!id) return std::move(id).error();
                if (unpool)
                    app.txn().modify<model::AutomationItem>(
                        *id, [](model::AutomationItem& x) { x.pool = 0; });
                else
                    app.txn().remove<model::AutomationItem>(*id);
                return Empty{};
            };
            addChecked(registry, std::move(c));
        }
        {
            struct ListArgs {
                std::optional<std::string> env;
            };
            Command<ListArgs, json::Value> c;
            c.name = "autoitem.list";
            c.kind = Kind::Query;
            c.exposure = cmd::Exposure::McpExec;
            c.summary = "List automation items (all, or one envelope's), with their points.";
            c.tags = {"automation"};
            c.args = schema::object<ListArgs>("autoitem_list_args")
                         .field("env", &ListArgs::env, idArg("en", "Envelope"))
                         .build();
            c.result = schema::any();
            c.examples = {{"None yet", j("{}"), j(R"({"items": []})"), "empty"}};
            c.run = [entryOf](const ListArgs& a, Context& ctx) -> Result<json::Value> {
                const model::Session s = app(ctx).view();
                json::Value items = json::Value::array();
                for (const auto& e : s.autoItems)
                    if (!a.env || e.value->envelope.str() == *a.env)
                        items.push_back(entryOf(e.id, *e.value));
                json::Value out = json::Value::object();
                out["items"] = std::move(items);
                return out;
            };
            addChecked(registry, std::move(c));
        }
    }();
}

}  // namespace supple::engine
