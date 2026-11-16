// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "analysis_commands.h"

#include <algorithm>

#include "../../analysis-commands/analysis_shared.h"
#include "supple/engine/app.h"

namespace supple::engine {
namespace {
using analysis_commands::DetailArg;
using analysis_commands::Loaded;
using cmd::Command;
using cmd::Context;
using cmd::Kind;

json::Value obj(std::string_view text) { return json::parse(text).value(); }
EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }

struct RenderArgs {
    std::string render;
    DetailArg detail = DetailArg::Summary;
};
struct TrackArgs {
    std::string track;
    std::optional<double> startBeat, endBeat;
    model::Tap tap = model::Tap::PostFader;
    DetailArg detail = DetailArg::Summary;
};
struct CompareArgs {
    std::string reference, test;
};
struct MaskingArgs {
    std::string maskerTrack, maskeeTrack;
    std::optional<double> startBeat, endBeat;
};

schema::Schema<model::Tap> tapSchema() {
    return schema::enumeration<model::Tap>({{model::Tap::PostFader, "post_fader"},
                                            {model::Tap::PreFader, "pre_fader"},
                                            {model::Tap::PreFx, "pre_fx"}});
}

double lastBeat(const model::Session& s) { return model::lastItemBeat(s); }

Result<Loaded> trackAudio(EngineApp& a, std::string_view track, std::optional<double> start,
                          std::optional<double> end, model::Tap tap) {
    const auto s = a.view();
    const auto id = model::Id::parse(track);
    if (!id || !s.tracks.contains(*id))
        return Error("track.unknown", "no track " + std::string(track), "/track",
                     "use track.list for available track ids");
    const double first = start.value_or(0.0);
    const double last = end.value_or(lastBeat(s));
    if (!(last > first))
        return Error("engine.range", "analysis range is empty", "/end_beat",
                     "add an item or pass end_beat greater than start_beat");
    OfflineSettings settings;
    settings.applyMasterPlayrate = false;
    settings.startSeconds = s.timeMap->secondsAt(first);
    settings.endSeconds = s.timeMap->secondsAt(last);
    settings.track = *id;
    settings.tap = tap;
    auto render = a.renderMix(settings);
    if (!render) return std::move(render).error();
    Loaded loaded;
    loaded.data = std::move(render->audio);
    return loaded;
}

Result<Loaded> source(EngineApp& a, const std::string& name, std::string_view path) {
    if (name.starts_with("tr_")) {
        auto result = trackAudio(a, name, {}, {}, model::Tap::PostFader);
        if (!result) {
            auto error = std::move(result).error();
            error.path = path;
            return error;
        }
        return result;
    }
    auto file = a.renderPath(name);
    if (!file) {
        auto error = std::move(file).error();
        error.path = path;
        return error;
    }
    auto audio = audio::readAudio(*file);
    if (!audio) {
        auto error = std::move(audio).error();
        error.path = path;
        if (error.hint.empty()) error.hint = "pass a render id or an audio file path";
        return error;
    }
    Loaded loaded;
    loaded.data = std::move(*audio);
    return loaded;
}

Result<analysis::Report> measure(Loaded& audio) { return analysis_commands::analyze(audio); }
}  // namespace

Result<json::Value> analyzeAudio(const audio::AudioData& data, analysis::Detail detail) {
    std::vector<const float*> channels;
    auto signal = analysis_commands::signalOf(data, channels);
    if (!signal) return std::move(signal).error();
    auto result = analysis::analyze(*signal);
    if (!result) return std::move(result).error();
    return analysis_commands::reportJson(*result, detail);
}

void registerEngineAnalysisCommands(cmd::Registry& registry) {
    {
        Command<RenderArgs, json::Value> c;
        c.name = "analyze.render";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Analyze a render id or audio path: loudness, spectrum, envelope and stereo.";
        c.tags = {"analysis"};
        c.args = schema::object<RenderArgs>("analyze_render_args")
                     .field("render", &RenderArgs::render, schema::string().minLength(1))
                     .field("detail", &RenderArgs::detail, analysis_commands::detailSchema())
                     .defaultValue(DetailArg::Summary)
                     .build();
        c.result = schema::any();
        c.examples = {
            {"Analyze a mix", obj(R"({"render":"rd_1"})"), obj(R"("<any>")"), "song_with_render"}};
        c.run = [](const RenderArgs& args, Context& ctx) -> Result<json::Value> {
            auto loaded = source(app(ctx), args.render, "/render");
            if (!loaded) return std::move(loaded).error();
            auto report = measure(*loaded);
            if (!report) return std::move(report).error();
            return analysis_commands::reportJson(*report, analysis_commands::toDetail(args.detail));
        };
        registry.add(std::move(c));
    }
    {
        Command<TrackArgs, json::Value> c;
        c.name = "analyze.track";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Render one track's output offline and analyze it.";
        c.tags = {"analysis"};
        c.args = schema::object<TrackArgs>("analyze_track_args")
                     .field("track", &TrackArgs::track, schema::string().minLength(1))
                     .field("start_beat", &TrackArgs::startBeat, schema::number().min(0))
                     .field("end_beat", &TrackArgs::endBeat, schema::number().exclusiveMin(0))
                     .field("tap", &TrackArgs::tap, tapSchema())
                     .defaultValue(model::Tap::PostFader)
                     .field("detail", &TrackArgs::detail, analysis_commands::detailSchema())
                     .defaultValue(DetailArg::Summary)
                     .build();
        c.result = schema::any();
        c.examples = {
            {"Analyze a track", obj(R"({"track":"tr_1"})"), obj(R"("<any>")"), "song_with_item"}};
        c.run = [](const TrackArgs& args, Context& ctx) -> Result<json::Value> {
            auto loaded = trackAudio(app(ctx), args.track, args.startBeat, args.endBeat, args.tap);
            if (!loaded) return std::move(loaded).error();
            auto report = measure(*loaded);
            if (!report) return std::move(report).error();
            return analysis_commands::reportJson(*report, analysis_commands::toDetail(args.detail));
        };
        registry.add(std::move(c));
    }
    {
        Command<CompareArgs, analysis_commands::CompareResult> c;
        c.name = "analyze.compare";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Compare two renders or track outputs by perceptual descriptors.";
        c.tags = {"analysis"};
        c.args = schema::object<CompareArgs>("analyze_compare_args")
                     .field("reference", &CompareArgs::reference, schema::string().minLength(1))
                     .field("test", &CompareArgs::test, schema::string().minLength(1))
                     .build();
        c.result = analysis_commands::compareSchema();
        c.examples = {{"Compare a track with itself", obj(R"({"reference":"tr_1","test":"tr_1"})"),
                       obj(R"({"notable":[],"ltas_rms_db":0.0,"summary":"<any>"})"),
                       "song_with_item"}};
        c.run = [](const CompareArgs& args,
                   Context& ctx) -> Result<analysis_commands::CompareResult> {
            auto ref = source(app(ctx), args.reference, "/reference");
            if (!ref) return std::move(ref).error();
            auto test = source(app(ctx), args.test, "/test");
            if (!test) return std::move(test).error();
            auto a = measure(*ref);
            if (!a) return std::move(a).error();
            auto b = measure(*test);
            if (!b) return std::move(b).error();
            return analysis_commands::compare(*a, *b);
        };
        registry.add(std::move(c));
    }
    {
        Command<MaskingArgs, json::Value> c;
        c.name = "analyze.masking";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Estimate masking between two tracks over the same range.";
        c.tags = {"analysis"};
        c.args =
            schema::object<MaskingArgs>("analyze_masking_args")
                .field("masker_track", &MaskingArgs::maskerTrack, schema::string().minLength(1))
                .field("maskee_track", &MaskingArgs::maskeeTrack, schema::string().minLength(1))
                .field("start_beat", &MaskingArgs::startBeat, schema::number().min(0))
                .field("end_beat", &MaskingArgs::endBeat, schema::number().exclusiveMin(0))
                .build();
        c.result = schema::any();
        c.examples = {{"One track against itself",
                       obj(R"({"masker_track":"tr_1","maskee_track":"tr_1"})"), obj(R"("<any>")"),
                       "song_with_fx"}};
        c.run = [](const MaskingArgs& args, Context& ctx) -> Result<json::Value> {
            auto masker = trackAudio(app(ctx), args.maskerTrack, args.startBeat, args.endBeat,
                                     model::Tap::PostFader);
            if (!masker) {
                auto error = std::move(masker).error();
                if (error.path == "/track") error.path = "/masker_track";
                return error;
            }
            auto maskee = trackAudio(app(ctx), args.maskeeTrack, args.startBeat, args.endBeat,
                                     model::Tap::PostFader);
            if (!maskee) {
                auto error = std::move(maskee).error();
                if (error.path == "/track") error.path = "/maskee_track";
                return error;
            }
            auto maskerSignal = masker->signal();
            if (!maskerSignal) return std::move(maskerSignal).error();
            auto maskeeSignal = maskee->signal();
            if (!maskeeSignal) return std::move(maskeeSignal).error();
            auto report = analysis::masking(*maskerSignal, *maskeeSignal);
            if (!report) return std::move(report).error();
            return analysis::toJson(*report);
        };
        registry.add(std::move(c));
    }
}
}  // namespace supple::engine
