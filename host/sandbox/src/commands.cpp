// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sandbox/commands.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>

#include "../../dsp-commands/dsp_shared.h"
#include "analysis_commands.h"
#include "dsp_commands.h"
#include "optimize_commands.h"
#include "supple/base/files.h"
#include "supple/notation/notation.h"

namespace supple::sandbox {

namespace fs = std::filesystem;
using cmd::Command;
using cmd::Context;
using cmd::ErrorSpec;
using cmd::Example;
using cmd::Exposure;
using cmd::Kind;
using cmd::NoArgs;

namespace {

json::Value obj(std::string_view text) { return json::parse(text).value(); }

Session& S(Context& ctx) { return ctx.service<Session>(); }

// --- argument / result types -----------------------------------------------------

struct Empty {};

struct PluginListArgs {
    std::optional<std::string> filter;
    bool rescan = false;
};
struct PluginEntry {
    std::string id, name, vendor;
    bool instrument = false, semantic = false;
    std::vector<std::string> features;
    std::string path, version;
    std::optional<bool> shadowed;  // another location earlier on the path has this id
};
struct ScanReason {
    std::string code, message, path, hint;
    std::optional<std::vector<json::Value>> details;
};
ScanReason scanReason(const Error& e) {
    ScanReason r{e.code, e.message, e.path, e.hint, {}};
    if (!e.details.empty()) {
        auto encode = [](auto&& self, const Error& error) -> json::Value {
            json::Value v = json::Value::object();
            v["code"] = error.code;
            v["message"] = error.message;
            v["path"] = error.path;
            v["hint"] = error.hint;
            if (!error.details.empty()) {
                v["details"] = json::Value::array();
                for (const auto& d : error.details) v["details"].push_back(self(self, d));
            }
            return v;
        };
        r.details = std::vector<json::Value>{};
        for (const auto& d : e.details) r.details->push_back(encode(encode, d));
    }
    return r;
}
struct ScanFailure {
    std::string module;
    ScanReason reason;
    bool cached = false;
};
struct ScanReport {
    std::vector<ScanFailure> failures;
    std::vector<ScanReason> warnings;
    std::uint64_t cachedModules = 0;
};
ScanReport scanReport(const host::ScanReport& source) {
    ScanReport r;
    for (const auto& f : source.failures)
        r.failures.push_back({f.module.string(), scanReason(f.reason), f.cached});
    for (const auto& w : source.warnings) r.warnings.push_back(scanReason(w));
    r.cachedModules = source.cachedModules;
    return r;
}
auto scanReportSchema() {
    auto reason = schema::object<ScanReason>("scan_reason")
                      .field("code", &ScanReason::code, schema::string())
                      .field("message", &ScanReason::message, schema::string())
                      .field("path", &ScanReason::path, schema::string())
                      .field("hint", &ScanReason::hint, schema::string())
                      .field("details", &ScanReason::details, schema::arrayOf(schema::any()))
                      .build();
    auto failure = schema::object<ScanFailure>("scan_failure")
                       .field("module", &ScanFailure::module, schema::string())
                       .field("reason", &ScanFailure::reason, reason)
                       .field("cached", &ScanFailure::cached, schema::boolean())
                       .build();
    return schema::object<ScanReport>("scan_report")
        .field("failures", &ScanReport::failures, schema::arrayOf(failure))
        .field("warnings", &ScanReport::warnings, schema::arrayOf(reason))
        .field("cached_modules", &ScanReport::cachedModules, schema::integerOf<std::uint64_t>())
        .build();
}
struct PluginListResult {
    std::vector<PluginEntry> plugins;
    ScanReport scan;
};

struct LoadArgs {
    std::string plugin;
    std::optional<std::int64_t> position;
};
struct LoadResult {
    std::string instance, name;
    bool semantic = false;
    std::vector<std::string> chain;
};

struct InstanceArgs {
    std::string instance;
};
struct ChainResult {
    std::vector<std::string> chain;
};

enum class Detail { Summary, Full };
struct DescribeArgs {
    std::string instance;
    Detail detail = Detail::Summary;
};
struct DescribeResult {
    std::string plugin, name;
    json::Value params;
    std::optional<std::string> patchType;
    std::optional<json::Value> patchSchema;
    std::string description;
    std::optional<std::string> docs;
    bool statusAvailable = false;
    std::optional<json::Value> problems;
    std::optional<std::vector<std::string>> inactive;
};

struct ParamGetArgs {
    std::string instance;
    std::optional<std::vector<std::string>> keys;
};
struct ParamSetArgs {
    std::string instance;
    json::Value values;
};
struct ValuesResult {
    json::Value values;
};

enum class PatchDetail { Changed, Full };
struct PatchGetArgs {
    std::string instance;
    PatchDetail detail = PatchDetail::Changed;
};
struct PatchResult {
    json::Value patch;
};
struct PatchArgs {
    std::string instance;
    json::Value patch;
};
struct DescriptionResult {
    std::string description;
};
struct MergeResult {
    json::Value changed;
    std::string description;
    std::optional<std::vector<std::string>> inactive;
};

struct PresetSaveArgs {
    std::string instance, name;
    std::optional<std::vector<std::string>> tags;
    std::optional<std::string> notes;
};
struct PathResult {
    std::string path;
};
enum class PresetSource { User, Factory };
auto presetSourceSchema() {
    return schema::enumeration<PresetSource>(
        {{PresetSource::User, "user"}, {PresetSource::Factory, "factory"}});
}
struct PresetLoadArgs {
    std::string instance, name;
    PresetSource source = PresetSource::User;
};
struct PresetListArgs {
    std::optional<std::string> plugin;
    PresetSource source = PresetSource::User;
};
struct PresetEntry {
    std::string plugin, name;
    std::vector<std::string> tags;
    std::optional<bool> readOnly;
};
struct PresetListResult {
    std::vector<PresetEntry> presets;
};

enum class Analyze { None, Summary, Standard, Full };
struct RenderArgs {
    std::optional<std::string> notation, midiFile, audioFile;
    std::optional<double> bars, seconds, tempo, tailS;
    std::optional<std::string> name;
    Analyze analyze = Analyze::None;
};
struct RenderResultArgs {
    std::string render, path;
    double seconds = 0, peakDbfs = 0, tailS = 0;
    bool tailCapped = false;
    std::int64_t latencySamples = 0;
    std::optional<std::vector<std::string>> warnings, log;
    std::optional<json::Value> analysis;
};

struct SessionSetArgs {
    std::optional<double> sampleRate;
    std::optional<std::int64_t> blockSize;
    std::optional<double> tempo;
    std::optional<std::string> meter;
    std::optional<double> tailLimitS;
};
struct SettingsResult {
    json::Value settings;
};
struct SessionGetResult {
    json::Value settings, chain, instances, renders;
};

// --- schemas ----------------------------------------------------------------------

auto instanceField() {
    return schema::string()
        .pattern("^in_[0-9]+$", "instance id")
        .doc("Instance id from plugin.load, e.g. \"in_1\".");
}
auto stringList() { return schema::arrayOf(schema::string()); }

schema::Schema<Empty> emptySchema() { return schema::object<Empty>("empty").build(); }

// --- helpers ----------------------------------------------------------------------

std::string rangeText(const json::Value& e) {
    const std::string type = e.find("type") ? e.find("type")->asString() : "";
    if (type == "enum") {
        std::string s;
        for (const auto& c : e.find("choices")->asArray())
            s += (s.empty() ? "" : "|") + c.asString();
        return s;
    }
    if (type == "bool") return "on|off";
    std::string s = json::writeCompact(*e.find("min")) + ".." + json::writeCompact(*e.find("max"));
    if (const auto* u = e.find("unit")) s += " " + u->asString();
    if (const auto* c = e.find("curve"); c && c->asString() != "linear") s += ", " + c->asString();
    return s;
}

Result<DescribeResult> describePlugin(Session& s, const DescribeArgs& a) {
    auto inst = s.instance(a.instance);
    if (!inst) return std::move(inst).error();
    host::Instance& in = **inst;
    DescribeResult r;
    r.plugin = in.info().id;
    r.name = in.info().name;
    // A failed query is reported, not fatal: the description still helps.
    auto status = in.pluginStatus();
    if (!status)
        r.problems = host::statusFailure(status.error());
    else if (!status->isNull()) {
        r.statusAvailable = true;
        r.problems = *status->find("problems");
    }
    const bool full = a.detail == Detail::Full;
    if (in.info().semantic) {
        // The values first: reading them may refresh the schema cache, so take
        // the (current) schema reference after it.
        auto values = s.paramGet(a.instance);
        if (!values) return std::move(values).error();
        const json::Value& schema = s.schema(a.instance);
        json::Value params = json::Value::object();
        std::vector<std::string> inactive;
        if (const json::Value* entries = schema.find("params"); entries && entries->isObject()) {
            for (const auto& m : entries->asObject()) {
                const json::Value* active = m.value.find("active");
                const bool applies = !(active && active->isBool() && !active->asBool());
                if (!applies) inactive.push_back(m.key);
                const json::Value* cur = values->find(m.key);
                if (!cur || !cur->find("value"))
                    return Error(
                        "sandbox.plugin_contract",
                        r.name + " did not provide a current value for '" + m.key + "'",
                        "/params/" + m.key + "/value",
                        "Restart or replace the plugin; its parameter query is incomplete.");
                if (full) {
                    json::Value e = m.value;
                    if (cur) {
                        e["value"] = *cur->find("value");
                        if (const auto* t = cur->find("text")) e["text"] = *t;
                    }
                    params[m.key] = std::move(e);
                } else if (applies) {
                    const std::string group =
                        m.value.find("group") ? m.value.find("group")->asString() : "main";
                    std::string line = m.key + " = " +
                                       (cur && cur->find("text") ? cur->find("text")->asString()
                                                                 : std::string("?")) +
                                       "  (" + rangeText(m.value) + ")";
                    if (!params.find(group)) params[group] = json::Value::array();
                    params[group].push_back(std::move(line));
                }
            }
        }
        r.params = std::move(params);
        if (!inactive.empty()) r.inactive = std::move(inactive);
        if (const auto* t = schema.find("patch_type"); t && t->isString())
            r.patchType = t->asString();
        if (full && schema.find("patch")) r.patchSchema = *schema.find("patch");
        if (const auto* d = schema.find("docs"); d && d->isString() && !d->asString().empty())
            r.docs = d->asString();
        auto d = s.describe(a.instance, !full);
        if (d) r.description = *d;
        return r;
    }
    json::Value params = json::Value::object();
    for (const auto& p : in.params()) {
        const auto v = in.paramValue(p.id);
        if (!v)
            return Error("sandbox.plugin_contract",
                         r.name + " did not provide a current value for '" + p.name + "'",
                         "/params/" + std::to_string(p.id) + "/value",
                         "Restart or replace the plugin; clap.params.get_value failed.");
        const std::string text = v ? in.paramText(p.id, *v).valueOr("") : "";
        if (full) {
            json::Value e = json::Value::object();
            e["name"] = p.name;
            if (!p.module.empty()) e["module"] = p.module;
            e["min"] = p.min;
            e["max"] = p.max;
            e["default"] = p.def;
            if (v) e["value"] = *v;
            e["text"] = text;
            params[std::to_string(p.id)] = std::move(e);
        } else {
            const std::string group = p.module.empty() ? "main" : p.module;
            if (!params.find(group)) params[group] = json::Value::array();
            params[group].push_back(p.name + " = " + text + "  (id " + std::to_string(p.id) + ")");
        }
    }
    r.params = std::move(params);
    r.description = in.info().description;
    return r;
}

// Builds a Session RenderInput from the render command's arguments.
Result<RenderInput> renderInput(Session& s, const RenderArgs& a,
                                std::vector<std::string>& warnings) {
    const int sources = (a.notation ? 1 : 0) + (a.midiFile ? 1 : 0) + (a.audioFile ? 1 : 0);
    if (sources > 1)
        return Error("render.input", "give one of notation, midi_file or audio_file", {},
                     "render one input at a time");
    RenderInput in;
    in.name = a.name.value_or("");
    in.tempo = a.tempo;
    in.tailSeconds = a.tailS;
    midi::Meter meter{s.settings().numerator, s.settings().denominator};
    if (a.notation) {
        auto compiled = notation::compile(*a.notation);
        if (!compiled) {
            Error e = std::move(compiled).error();
            if (e.path.empty() || e.path.front() != '/') e.path = "/notation";
            return e;
        }
        for (const auto& w : compiled->warnings)
            warnings.push_back(w.code + " at " + w.path + ": " + w.message);
        if (!in.tempo && compiled->tempo) in.tempo = compiled->tempo;
        if (!compiled->barMeters.empty()) {
            meter = compiled->barMeters.front();
            in.meter = meter;
        }
        in.barMeters = std::move(compiled->barMeters);
        in.notes = std::move(compiled->sequence);
    }
    if (a.midiFile) {
        auto bytes = readFile(*a.midiFile, 64u << 20);
        if (!bytes) return std::move(bytes).error().at("/midi_file");
        midi::SmfInfo info;
        auto seq = midi::readSmf(*bytes, &info);
        if (!seq) return std::move(seq).error().at("/midi_file");
        if (!in.tempo && !info.tempos.empty()) in.tempo = info.tempos.front().second;
        if (!info.meters.empty()) {
            meter = info.meters.front().second;
            in.meter = meter;
        }
        if (info.tempos.size() > 1)
            warnings.push_back("midi_file has tempo changes; the sandbox renders at one tempo");
        in.notes = std::move(*seq);
    }
    if (a.audioFile) {
        auto audio = audio::readAudio(*a.audioFile);
        if (!audio) return std::move(audio).error().at("/audio_file");
        if (audio->truncated)
            warnings.push_back("audio_file is truncated; rendering what is there");
        if (audio->sampleRate != s.settings().sampleRate)
            return Error("render.input_rate",
                         "audio_file is at " + json::formatDouble(audio->sampleRate) +
                             " Hz, the session at " + json::formatDouble(s.settings().sampleRate),
                         "/audio_file", "set the session rate to match (session.set sample_rate)");
        in.audio = std::move(*audio);
    }
    if (a.bars) {
        if (!(*a.bars > 0 && *a.bars <= 1000))
            return Error("render.length", "bars out of range", "/bars", "(0, 1000]");
        const double tempo = in.tempo.value_or(s.settings().tempo);
        const double beatsPerBar = meter.numerator * 4.0 / meter.denominator;
        in.seconds = *a.bars * beatsPerBar * 60.0 / tempo;
    }
    if (a.seconds) in.seconds = a.seconds;
    return in;
}

}  // namespace

// --- registration ----------------------------------------------------------------------

void registerCommands(cmd::Registry& registry, const ExamplePlugins& ex) {
    // One line per command group (the name before the first '.'), as the agent index shows it.
    registry.describeGroup("analyze", "Analyze a render or audio file: loudness, spectrum, pitch.");
    registry.describeGroup("compare", "Compare two renders or files by their descriptors.");
    registry.describeGroup("dsp", "DSP scripts: write, check, test, preview and export.");
    registry.describeGroup("masking", "Estimate where one sound masks another.");
    registry.describeGroup("optimize", "Tune parameters toward a reference sound or a target.");
    registry.describeGroup("param", "Read and set plug-in parameters in physical units.");
    registry.describeGroup("patch", "An instance's patch document: get, set, merge.");
    registry.describeGroup("plugin",
                           "The plug-ins the sandbox can load: list, load, remove, describe.");
    registry.describeGroup("preset", "User presets and factory patches: list, load, save.");
    registry.describeGroup("profile", "Score a render against a target descriptor profile.");
    registry.describeGroup("render", "Render notes, a MIDI file or audio through the chain.");
    registry.describeGroup("script", "Run a Lua script on the command bus.");
    registry.describeGroup("session", "The sandbox session: settings, chain, instances; reset.");
    const std::string I = ex.instrument, F = ex.effect;
    lua::registerScriptCommands(registry);

    // plugin.list
    {
        Command<PluginListArgs, PluginListResult> c;
        c.name = "plugin.list";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "List the CLAP plugins the sandbox can load.";
        c.description =
            "Scans the plugin path once (rescan: true to scan again). filter matches the id or "
            "name, case-insensitively. Load one with plugin.load.";
        c.tags = {"plugin"};
        c.args = schema::object<PluginListArgs>("plugin_list_args")
                     .field("filter", &PluginListArgs::filter,
                            schema::string().doc("Text to match in the id or name."))
                     .field("rescan", &PluginListArgs::rescan, schema::boolean())
                     .defaultValue(false)
                     .build();
        auto entry =
            schema::object<PluginEntry>("plugin")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .field("features", &PluginEntry::features, stringList())
                .field("path", &PluginEntry::path, schema::string())
                .field("version", &PluginEntry::version, schema::string())
                .field("shadowed", &PluginEntry::shadowed,
                       schema::boolean().doc("Not loadable by id: an earlier location on the "
                                             "plugin path has the same id (load it by path)"))
                .build();
        c.result = schema::object<PluginListResult>("plugin_list")
                       .field("plugins", &PluginListResult::plugins, schema::arrayOf(entry))
                       .field("scan", &PluginListResult::scan, scanReportSchema())
                       .build();
        c.examples = {{"List everything", obj("{}"),
                       obj(R"({"plugins": "<any>", "scan": "<any>"})"), "empty"}};
        c.run = [](const PluginListArgs& a, Context& ctx) -> Result<PluginListResult> {
            auto list = S(ctx).plugins(a.rescan);
            if (!list) return std::move(list).error();
            auto lower = [](std::string x) {
                std::transform(x.begin(), x.end(), x.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return x;
            };
            const std::string f = lower(a.filter.value_or(""));
            PluginListResult r;
            r.scan = scanReport(S(ctx).pluginScanReport());
            for (std::size_t i = 0; i < list->size(); ++i) {
                const auto& p = (*list)[i];
                if (!f.empty() && lower(p.id).find(f) == std::string::npos &&
                    lower(p.name).find(f) == std::string::npos)
                    continue;
                PluginEntry e{p.id,       p.name,     p.vendor,        p.instrument,
                              p.semantic, p.features, p.path.string(), p.version,
                              {}};
                if (host::shadowed(*list, i)) e.shadowed = true;
                r.plugins.push_back(std::move(e));
            }
            return r;
        };
        registry.add(std::move(c));
    }

    // plugin.load
    {
        Command<LoadArgs, LoadResult> c;
        c.name = "plugin.load";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Load a plugin into the chain and return its instance id.";
        c.description =
            "plugin is an id from plugin.list or a .clap path. An instrument goes first in the "
            "chain, anything else last, unless position says otherwise (instruments must stay "
            "first). Then describe it with plugin.describe.";
        c.tags = {"plugin", "chain"};
        c.args = schema::object<LoadArgs>("plugin_load_args")
                     .field("plugin", &LoadArgs::plugin,
                            schema::string().minLength(1).doc("Plugin id or .clap path."))
                     .field("position", &LoadArgs::position,
                            schema::integer().min(0).doc("Chain index (0 = first)."))
                     .build();
        c.result = schema::object<LoadResult>("plugin_load")
                       .field("instance", &LoadResult::instance, schema::string())
                       .field("name", &LoadResult::name, schema::string())
                       .field("semantic", &LoadResult::semantic, schema::boolean())
                       .field("chain", &LoadResult::chain, stringList())
                       .build();
        c.errors = {
            {"sandbox.unknown_plugin", "no plugin with that id or path",
             "closest ids in the hint; see plugin.list"},
            {"sandbox.chain_order", "an instrument would not be first, or a second instrument",
             "remove the existing instrument or load an effect"},
            {"sandbox.position", "position past the end of the chain", "0 .. chain length"}};
        c.examples = {
            {"Load an instrument", obj(R"({"plugin": ")" + I + R"("})"),
             obj(R"({"instance": "in_1", "name": "<any>", "semantic": "<any>", "chain": ["in_1"]})"),
             "empty"},
            {"Add an effect after the instrument", obj(R"({"plugin": ")" + F + R"("})"),
             obj(R"({"instance": "in_3", "name": "<any>", "semantic": "<any>", "chain": ["in_1", "in_2", "in_3"]})"),
             "synth_loaded"}};
        c.run = [](const LoadArgs& a, Context& ctx) -> Result<LoadResult> {
            Session& s = S(ctx);
            std::optional<std::size_t> pos;
            if (a.position) pos = static_cast<std::size_t>(*a.position);
            auto id = s.load(a.plugin, pos);
            if (!id) return std::move(id).error();
            host::Instance* in = s.instance(*id).value();
            return LoadResult{*id, in->info().name, in->info().semantic, s.chain()};
        };
        registry.add(std::move(c));
    }

    // plugin.remove
    {
        Command<InstanceArgs, ChainResult> c;
        c.name = "plugin.remove";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Remove a plugin instance from the chain.";
        c.description = "Unloads the instance; its id is not reused.";
        c.tags = {"plugin", "chain"};
        c.args = schema::object<InstanceArgs>("instance_args")
                     .field("instance", &InstanceArgs::instance, instanceField())
                     .build();
        c.result = schema::object<ChainResult>("chain")
                       .field("chain", &ChainResult::chain, stringList())
                       .build();
        c.errors = {
            {"sandbox.unknown_instance", "no such instance", "the hint lists loaded instances"}};
        c.examples = {{"Remove the effect", obj(R"({"instance": "in_2"})"),
                       obj(R"({"chain": ["in_1"]})"), "synth_loaded"}};
        c.run = [](const InstanceArgs& a, Context& ctx) -> Result<ChainResult> {
            SUPPLE_TRY(S(ctx).remove(a.instance));
            return ChainResult{S(ctx).chain()};
        };
        registry.add(std::move(c));
    }

    // plugin.describe
    {
        Command<DescribeArgs, DescribeResult> c;
        c.name = "plugin.describe";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Describe an instance: parameters, patch format, and current problems.";
        c.description =
            "detail summary: parameters grouped as compact 'key = value (range)' lines plus a "
            "prose description of the current sound. detail full: every parameter's schema entry "
            "with current value and text, and the patch JSON Schema. inactive lists the "
            "parameters that do not apply with the current values (summary leaves them out): "
            "their entries carry \"active\": false, and active_when says what makes them apply "
            "(another parameter's values), or their module is not in the patch (add it with "
            "patch.merge). Semantic plugins are programmed with param.set (physical units) or "
            "patch.merge.";
        c.tags = {"plugin", "params"};
        c.args = schema::object<DescribeArgs>("plugin_describe_args")
                     .field("instance", &DescribeArgs::instance, instanceField())
                     .field("detail", &DescribeArgs::detail,
                            schema::enumeration<Detail>(
                                {{Detail::Summary, "summary"}, {Detail::Full, "full"}}))
                     .defaultValue(Detail::Summary)
                     .build();
        c.result = schema::object<DescribeResult>("plugin_description")
                       .field("plugin", &DescribeResult::plugin, schema::string())
                       .field("name", &DescribeResult::name, schema::string())
                       .field("params", &DescribeResult::params, schema::any())
                       .field("patch_type", &DescribeResult::patchType, schema::string())
                       .field("patch_schema", &DescribeResult::patchSchema, schema::any())
                       .field("description", &DescribeResult::description, schema::string())
                       .field("docs", &DescribeResult::docs, schema::string())
                       .field("status_available", &DescribeResult::statusAvailable,
                              schema::boolean().doc("The instance supplied its current problems"))
                       .field("problems", &DescribeResult::problems,
                              schema::any().doc("Current plugin problems: code, message, scope, "
                                                "line when known, hint, silent; scope host when "
                                                "unreadable"))
                       .field("inactive", &DescribeResult::inactive,
                              stringList().doc("Parameters that do not apply now"))
                       .build();
        c.examples = {
            {"Summarize the effect", obj(R"({"instance": "in_2"})"),
             obj(R"({"plugin": ")" + F +
                 R"(", "name": "<any>", "params": "<any>", "patch_type": "<any>", "description": "<any>", "status_available": false})"),
             "synth_loaded"}};
        c.run = [](const DescribeArgs& a, Context& ctx) { return describePlugin(S(ctx), a); };
        registry.add(std::move(c));
    }

    // param.get
    {
        Command<ParamGetArgs, ValuesResult> c;
        c.name = "param.get";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Read parameter values (physical value and display text).";
        c.description =
            "keys: semantic keys (e.g. \"filter.cutoff\"); omit for all. Third-party plugins: CLAP "
            "ids or names; values are CLAP values.";
        c.tags = {"params"};
        c.args = schema::object<ParamGetArgs>("param_get_args")
                     .field("instance", &ParamGetArgs::instance, instanceField())
                     .field("keys", &ParamGetArgs::keys, stringList())
                     .build();
        c.result = schema::object<ValuesResult>("param_values")
                       .field("values", &ValuesResult::values, schema::any())
                       .build();
        c.errors = {{"sandbox.unknown_param", "no parameter with that key",
                     "closest key in the hint; see plugin.describe"}};
        c.examples = {{"Read one value", obj(R"({"instance": "in_2", "keys": ["gain"]})"),
                       obj(R"({"values": {"gain": {"value": 0.0, "text": "<any>"}}})"),
                       "synth_loaded"}};
        c.run = [](const ParamGetArgs& a, Context& ctx) -> Result<ValuesResult> {
            std::vector<std::string> keys = a.keys.value_or(std::vector<std::string>{});
            auto v = S(ctx).paramGet(a.instance, keys);
            if (!v) return std::move(v).error();
            return ValuesResult{std::move(*v)};
        };
        registry.add(std::move(c));
    }

    // param.set
    {
        Command<ParamSetArgs, ValuesResult> c;
        c.name = "param.set";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Set parameters in physical units; returns the values as applied.";
        c.description =
            "values: {key: number | \"unit string\" | enum key | bool}, e.g. {\"filter.cutoff\": "
            "\"1.2 kHz\", \"filter.mode\": \"lowpass\"}. All values are validated first; nothing "
            "changes if any is invalid. Third-party plugins: CLAP id or name keys with CLAP values "
            "or {\"text\": \"...\"}. A value that does not apply with the new values says "
            "\"active\": false (plugin.describe detail full says what makes it apply). Use "
            "patch.merge to change several sections at once.";
        c.tags = {"params"};
        c.args = schema::object<ParamSetArgs>("param_set_args")
                     .field("instance", &ParamSetArgs::instance, instanceField())
                     .field("values", &ParamSetArgs::values, schema::any().doc("Key -> value."))
                     .build();
        c.result = schema::object<ValuesResult>("param_values")
                       .field("values", &ValuesResult::values, schema::any())
                       .build();
        c.errors = {
            {"sandbox.unknown_param", "no parameter with that key", "closest key in the hint"},
            {"params.out_of_range", "a value is outside its range",
             "the hint gives the valid range"},
            {"params.parse", "a unit string could not be read",
             "the hint shows the expected form"}};
        c.examples = {{"Set a level with a unit string",
                       obj(R"({"instance": "in_2", "values": {"gain": "-6 dB"}})"),
                       obj(R"({"values": {"gain": {"value": -6.0, "text": "<any>"}}})"),
                       "synth_loaded"}};
        c.run = [](const ParamSetArgs& a, Context& ctx) -> Result<ValuesResult> {
            auto v = S(ctx).paramSet(a.instance, a.values);
            if (!v) return std::move(v).error();
            return ValuesResult{std::move(*v)};
        };
        registry.add(std::move(c));
    }

    // patch.get
    {
        Command<PatchGetArgs, PatchResult> c;
        c.name = "patch.get";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Get an instance's patch document (changed values or all).";
        c.description =
            "detail changed (default): only values that differ from the defaults, still a "
            "complete patch. detail full: every value. Semantic plugins only.";
        c.tags = {"patch"};
        c.args = schema::object<PatchGetArgs>("patch_get_args")
                     .field("instance", &PatchGetArgs::instance, instanceField())
                     .field("detail", &PatchGetArgs::detail,
                            schema::enumeration<PatchDetail>(
                                {{PatchDetail::Changed, "changed"}, {PatchDetail::Full, "full"}}))
                     .defaultValue(PatchDetail::Changed)
                     .build();
        c.result = schema::object<PatchResult>("patch")
                       .field("patch", &PatchResult::patch, schema::any())
                       .build();
        c.errors = {{"sandbox.not_semantic", "the plugin has no semantic patch",
                     "use param.get / presets"}};
        c.examples = {{"Get the changed values", obj(R"({"instance": "in_2"})"),
                       obj(R"({"patch": "<any>"})"), "synth_loaded"}};
        c.run = [](const PatchGetArgs& a, Context& ctx) -> Result<PatchResult> {
            auto p = S(ctx).patchGet(a.instance, a.detail == PatchDetail::Changed);
            if (!p) return std::move(p).error();
            return PatchResult{std::move(*p)};
        };
        registry.add(std::move(c));
    }

    // patch.set
    {
        Command<PatchArgs, DescriptionResult> c;
        c.name = "patch.set";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Replace an instance's whole patch; returns a description of the result.";
        c.description =
            "patch: a complete patch document (see plugin.describe detail full for its schema); "
            "missing values take their defaults. Validated fully before anything changes. For "
            "partial edits use patch.merge.";
        c.tags = {"patch"};
        c.args = schema::object<PatchArgs>("patch_set_args")
                     .field("instance", &PatchArgs::instance, instanceField())
                     .field("patch", &PatchArgs::patch, schema::any())
                     .build();
        c.result = schema::object<DescriptionResult>("description")
                       .field("description", &DescriptionResult::description, schema::string())
                       .build();
        c.errors = {
            {"sandbox.not_semantic", "the plugin has no semantic patch", "use presets"},
            {"schema.invalid", "the patch has problems", "details list each with its path"}};
        c.examples = {
            {"Reset to defaults",
             obj(R"({"instance": "in_2", "patch": {"schema": "supple.test-gain-patch/1.0"}})"),
             obj(R"({"description": "<any>"})"), "synth_loaded"}};
        c.run = [](const PatchArgs& a, Context& ctx) -> Result<DescriptionResult> {
            auto d = S(ctx).patchSet(a.instance, a.patch);
            if (!d) return std::move(d).error();
            return DescriptionResult{std::move(*d)};
        };
        registry.add(std::move(c));
    }

    // patch.merge
    {
        Command<PatchArgs, MergeResult> c;
        c.name = "patch.merge";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Merge a partial patch (RFC 7396) into an instance.";
        c.description =
            "patch: only the values to change, e.g. {\"params\": {\"filter.cutoff\": \"800 Hz\"}}; "
            "null removes a value (back to its default). Returns the effective change (a minimal "
            "merge patch; {} if nothing changed) and a description of the new sound. inactive "
            "names the params it set that do not apply with the new values: they are kept, and "
            "heard once what their active_when names allows it (plugin.describe detail full).";
        c.tags = {"patch"};
        c.args = schema::object<PatchArgs>("patch_merge_args")
                     .field("instance", &PatchArgs::instance, instanceField())
                     .field("patch", &PatchArgs::patch, schema::any())
                     .build();
        c.result = schema::object<MergeResult>("patch_merge")
                       .field("changed", &MergeResult::changed, schema::any())
                       .field("description", &MergeResult::description, schema::string())
                       .field("inactive", &MergeResult::inactive,
                              stringList().doc("Params set that do not apply now"))
                       .build();
        c.errors = {{"sandbox.not_semantic", "the plugin has no semantic patch", "use param.set"},
                    {"params.out_of_range", "a value is outside its range",
                     "the hint gives the valid range"}};
        c.examples = {{"Turn it down",
                       obj(R"({"instance": "in_2", "patch": {"params": {"gain": "-12 dB"}}})"),
                       obj(R"({"changed": {"params": {"gain": -12.0}}, "description": "<any>"})"),
                       "synth_loaded"}};
        c.run = [](const PatchArgs& a, Context& ctx) -> Result<MergeResult> {
            auto m = S(ctx).patchMerge(a.instance, a.patch);
            if (!m) return std::move(m).error();
            MergeResult r{std::move(m->changed), std::move(m->description), std::nullopt};
            if (!m->inactive.empty()) r.inactive = std::move(m->inactive);
            return r;
        };
        registry.add(std::move(c));
    }

    // preset.save / preset.load / preset.list
    {
        Command<PresetSaveArgs, PathResult> c;
        c.name = "preset.save";
        c.kind = Kind::Action;
        c.exposure = Exposure::McpTool;
        c.summary = "Save an instance's current settings as a named preset.";
        c.description =
            "Stores the patch (semantic plugins) or the plugin state. Overwrites a preset of the "
            "same name for the same plugin. Names: 1-64 of A-Z a-z 0-9 space _ . + -.";
        c.tags = {"preset"};
        c.args = schema::object<PresetSaveArgs>("preset_save_args")
                     .field("instance", &PresetSaveArgs::instance, instanceField())
                     .field("name", &PresetSaveArgs::name, schema::string().minLength(1))
                     .field("tags", &PresetSaveArgs::tags, stringList())
                     .field("notes", &PresetSaveArgs::notes, schema::string())
                     .build();
        c.result = schema::object<PathResult>("path")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"sandbox.preset_name", "the name has characters outside the allowed set",
                     "letters, digits, space _ . + -"}};
        c.examples = {{"Save", obj(R"({"instance": "in_2", "name": "Quiet", "tags": ["soft"]})"),
                       obj(R"({"path": "<any>"})"), "synth_loaded"}};
        c.run = [](const PresetSaveArgs& a, Context& ctx) -> Result<PathResult> {
            const auto tags = a.tags.value_or(std::vector<std::string>{});
            auto p = S(ctx).presetSave(a.instance, a.name, tags, a.notes.value_or(""));
            if (!p) return std::move(p).error();
            return PathResult{p->string()};
        };
        registry.add(std::move(c));
    }
    {
        Command<PresetLoadArgs, DescriptionResult> c;
        c.name = "preset.load";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Load a named preset into an instance.";
        c.description =
            "The preset must belong to the instance's plugin (see preset.list). source: factory "
            "applies a read-only factory patch; user (default) loads a saved preset.";
        c.tags = {"preset"};
        c.args = schema::object<PresetLoadArgs>("preset_load_args")
                     .field("instance", &PresetLoadArgs::instance, instanceField())
                     .field("name", &PresetLoadArgs::name, schema::string().minLength(1))
                     .field("source", &PresetLoadArgs::source, presetSourceSchema())
                     .defaultValue(PresetSource::User)
                     .build();
        c.result = schema::object<DescriptionResult>("description")
                       .field("description", &DescriptionResult::description, schema::string())
                       .build();
        c.errors = {{"sandbox.unknown_preset", "no preset with that name for this plugin",
                     "closest names in the hint"}};
        c.examples = {{"Load", obj(R"({"instance": "in_2", "name": "Quiet"})"),
                       obj(R"({"description": "<any>"})"), "synth_with_preset"}};
        c.run = [](const PresetLoadArgs& a, Context& ctx) -> Result<DescriptionResult> {
            auto d = S(ctx).presetLoad(a.instance, a.name, a.source == PresetSource::Factory);
            if (!d) return std::move(d).error();
            return DescriptionResult{std::move(*d)};
        };
        registry.add(std::move(c));
    }
    {
        Command<PresetListArgs, PresetListResult> c;
        c.name = "preset.list";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "List user presets, or a plugin's read-only factory patches.";
        c.description =
            "source: user (default) lists saved presets. source: factory requires plugin and lists "
            "its immutable factory patches in declaration order.";
        c.tags = {"preset"};
        c.args = schema::object<PresetListArgs>("preset_list_args")
                     .field("plugin", &PresetListArgs::plugin, schema::string())
                     .field("source", &PresetListArgs::source, presetSourceSchema())
                     .defaultValue(PresetSource::User)
                     .build();
        auto entry = schema::object<PresetEntry>("preset")
                         .field("plugin", &PresetEntry::plugin, schema::string())
                         .field("name", &PresetEntry::name, schema::string())
                         .field("tags", &PresetEntry::tags, stringList())
                         .field("read_only", &PresetEntry::readOnly, schema::boolean())
                         .build();
        c.result = schema::object<PresetListResult>("preset_list")
                       .field("presets", &PresetListResult::presets, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"List the effect's presets", obj(R"({"plugin": ")" + F + R"("})"),
             obj(R"({"presets": [{"plugin": ")" + F + R"(", "name": "Quiet", "tags": ["soft"]}]})"),
             "synth_with_preset"}};
        c.run = [](const PresetListArgs& a, Context& ctx) -> Result<PresetListResult> {
            auto l = S(ctx).presetList(a.plugin.value_or(""), a.source == PresetSource::Factory);
            if (!l) return std::move(l).error();
            PresetListResult r;
            for (auto& p : *l)
                r.presets.push_back({p.plugin, p.name, p.tags,
                                     p.readOnly ? std::optional<bool>(true) : std::nullopt});
            return r;
        };
        registry.add(std::move(c));
    }

    // render
    {
        Command<RenderArgs, RenderResultArgs> c;
        c.name = "render";
        c.kind = Kind::Action;
        c.exposure = Exposure::McpTool;
        c.summary = "Render notes, a MIDI file or an audio file through the chain to a WAV.";
        c.description =
            "notation: a phrase in the notation (see the notation guide), e.g. \"voice lead; 1: "
            "C4:q E4:q G4:h | C5:w\". "
            "midi_file / audio_file: paths (audio at the session rate). bars/seconds override the "
            "length; the tail after it is rendered until silence (tail_s caps it). Returns the "
            "render id for analyze/compare and the WAV path.";
        c.tags = {"render"};
        c.args =
            schema::object<RenderArgs>("render_args")
                .field("notation", &RenderArgs::notation, schema::string().doc("Notation text."))
                .field("midi_file", &RenderArgs::midiFile, schema::string())
                .field("audio_file", &RenderArgs::audioFile, schema::string())
                .field("bars", &RenderArgs::bars, schema::number().exclusiveMin(0).max(1000))
                .field("seconds", &RenderArgs::seconds, schema::number().exclusiveMin(0).max(600))
                .field("tempo", &RenderArgs::tempo,
                       schema::number().range(1, 999).unit(units::Unit::bpm))
                .field("tail_s", &RenderArgs::tailS, schema::number().range(0, 60))
                .field("name", &RenderArgs::name, schema::string().maxLength(200))
                .field("analyze", &RenderArgs::analyze,
                       schema::enumeration<Analyze>({{Analyze::None, "none"},
                                                     {Analyze::Summary, "summary"},
                                                     {Analyze::Standard, "standard"},
                                                     {Analyze::Full, "full"}})
                           .doc("Also analyze the render (see the analyze command); summary is "
                                "compact (≤ 400 tokens)."))
                .defaultValue(Analyze::None)
                .build();
        c.result =
            schema::object<RenderResultArgs>("render_result")
                .field("render", &RenderResultArgs::render, schema::string())
                .field("path", &RenderResultArgs::path, schema::string())
                .field("seconds", &RenderResultArgs::seconds, schema::number())
                .field("peak_dbfs", &RenderResultArgs::peakDbfs, schema::number())
                .field("tail_s", &RenderResultArgs::tailS, schema::number())
                .field("tail_capped", &RenderResultArgs::tailCapped, schema::boolean())
                .field("latency_samples", &RenderResultArgs::latencySamples, schema::integer())
                .field("warnings", &RenderResultArgs::warnings, stringList())
                .field("log", &RenderResultArgs::log, stringList())
                .field("analysis", &RenderResultArgs::analysis, schema::any())
                .build();
        c.errors = {
            {"notation.*", "the notation does not compile", "the error has line:col and a hint"},
            {"render.empty_chain", "nothing is loaded", "plugin.load first"},
            {"render.not_instrument", "notes given but the first plugin is an effect",
             "load an instrument first"},
            {"render.input_rate", "audio_file at another sample rate", "session.set sample_rate"}};
        c.examples = {
            {"Render a phrase", obj(R"({"notation": "voice lead; 1: A4:q E5:q r:h"})"),
             obj(R"({"render": "rd_1", "path": "<any>", "seconds": "<any>", "peak_dbfs": "<any>", "tail_s": "<any>", "tail_capped": false, "latency_samples": 0})"),
             "synth_loaded"}};
        c.run = [](const RenderArgs& a, Context& ctx) -> Result<RenderResultArgs> {
            Session& s = S(ctx);
            std::vector<std::string> warnings;
            auto in = renderInput(s, a, warnings);
            if (!in) return std::move(in).error();
            auto rec = s.render(*in);
            if (!rec) return std::move(rec).error();
            const RenderRecord& r = **rec;
            RenderResultArgs out;
            out.render = r.id;
            out.path = r.path.string();
            // Rounded for readability: 0.1 ms and 0.01 dB are below anything audible.
            auto round = [](double v, double step) { return std::round(v / step) * step; };
            out.seconds = round(r.seconds, 1e-4);
            out.peakDbfs = round(r.peakDbfs, 0.01);
            out.tailS = round(r.tailSeconds, 1e-4);
            out.tailCapped = r.tailCapped;
            out.latencySamples = r.latency;
            if (!warnings.empty()) out.warnings = std::move(warnings);
            if (!r.log.empty()) out.log = r.log;
            if (a.analyze != Analyze::None) {
                const auto detail = a.analyze == Analyze::Full       ? analysis::Detail::Full
                                    : a.analyze == Analyze::Standard ? analysis::Detail::Standard
                                                                     : analysis::Detail::Summary;
                auto report = analyzeRender(s, r.id, detail);
                if (!report) return std::move(report).error();
                if (RenderRecord* rec2 = s.mutableRender(r.id)) rec2->analysis = *report;
                out.analysis = std::move(*report);
            }
            return out;
        };
        registry.add(std::move(c));
    }

    // session.get / session.set / session.reset
    {
        Command<NoArgs, SessionGetResult> c;
        c.name = "session.get";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Show the session: settings, chain, instances and renders.";
        c.tags = {"session"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<SessionGetResult>("session")
                       .field("settings", &SessionGetResult::settings, schema::any())
                       .field("chain", &SessionGetResult::chain, schema::any())
                       .field("instances", &SessionGetResult::instances, schema::any())
                       .field("renders", &SessionGetResult::renders, schema::any())
                       .build();
        c.examples = {
            {"An empty session", obj("{}"),
             obj(R"({"settings": {"sample_rate": 48000.0, "block_size": 256, "tempo": 120.0, "meter": "4/4", "tail_limit_s": 5.0}, "chain": [], "instances": {}, "renders": {}})"),
             "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<SessionGetResult> {
            json::Value v = S(ctx).summary();
            return SessionGetResult{*v.find("settings"), *v.find("chain"), *v.find("instances"),
                                    *v.find("renders")};
        };
        registry.add(std::move(c));
    }
    {
        Command<SessionSetArgs, SettingsResult> c;
        c.name = "session.set";
        c.kind = Kind::Edit;
        c.exposure = Exposure::McpTool;
        c.summary = "Change session settings (rate, block size, tempo, meter, tail limit).";
        c.description =
            "Only the given fields change. A rate or block size change reactivates plugins at the "
            "next render.";
        c.tags = {"session"};
        c.args =
            schema::object<SessionSetArgs>("session_set_args")
                .field("sample_rate", &SessionSetArgs::sampleRate,
                       schema::number().range(8000, 384000).unit(units::Unit::Hz))
                .field("block_size", &SessionSetArgs::blockSize, schema::integer().range(1, 8192))
                .field("tempo", &SessionSetArgs::tempo,
                       schema::number().range(1, 999).unit(units::Unit::bpm))
                .field("meter", &SessionSetArgs::meter,
                       schema::string().pattern("^[0-9]+/[0-9]+$", "meter like 3/4"))
                .field("tail_limit_s", &SessionSetArgs::tailLimitS, schema::number().range(0, 60))
                .build();
        c.result = schema::object<SettingsResult>("settings")
                       .field("settings", &SettingsResult::settings, schema::any())
                       .build();
        c.examples = {
            {"Slow down, 3/4", obj(R"({"tempo": "90 bpm", "meter": "3/4"})"),
             obj(R"({"settings": {"sample_rate": 48000.0, "block_size": 256, "tempo": 90.0, "meter": "3/4", "tail_limit_s": 5.0}})"),
             "empty"}};
        c.run = [](const SessionSetArgs& a, Context& ctx) -> Result<SettingsResult> {
            Session& s = S(ctx);
            SessionSettings st = s.settings();
            if (a.sampleRate) st.sampleRate = *a.sampleRate;
            if (a.blockSize) st.blockSize = static_cast<std::uint32_t>(*a.blockSize);
            if (a.tempo) st.tempo = *a.tempo;
            if (a.tailLimitS) st.tailLimitSeconds = *a.tailLimitS;
            if (a.meter) {
                const auto slash = a.meter->find('/');
                int n = 0, d = 0;
                const char* b = a.meter->data();
                const bool okN = std::from_chars(b, b + slash, n).ec == std::errc{};
                const bool okD =
                    std::from_chars(b + slash + 1, b + a.meter->size(), d).ec == std::errc{};
                if (!okN || !okD || n < 1 || n > 64 || d < 1 || d > 64)
                    return Error("sandbox.settings", "invalid meter", "/meter",
                                 "N/D with N 1..64 and D one of 1, 2, 4, 8, 16, 32, 64");
                st.numerator = static_cast<std::uint16_t>(n);
                st.denominator = static_cast<std::uint16_t>(d);
            }
            SUPPLE_TRY(s.setSettings(st));
            return SettingsResult{toJson(s.settings())};
        };
        registry.add(std::move(c));
    }
    {
        Command<NoArgs, Empty> c;
        c.name = "session.reset";
        c.kind = Kind::Action;
        c.exposure = Exposure::McpTool;
        c.summary = "Unload every plugin, delete renders and restore default settings.";
        c.description = "Not undoable. Saved presets are kept.";
        c.tags = {"session"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = emptySchema();
        c.examples = {{"Start over", obj("{}"), obj("{}"), "synth_loaded"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<Empty> {
            S(ctx).reset();
            return Empty{};
        };
        registry.add(std::move(c));
    }

    registerAnalysisCommands(registry);
    registerOptimizeCommands(registry);
    registerDspCommands(registry, ex);
}

// --- host ------------------------------------------------------------------------------

SandboxHost::SandboxHost(std::unique_ptr<Session> session, fs::path commandLog,
                         ExamplePlugins examples)
    : session_(std::move(session)),
      commandLog_(std::move(commandLog)),
      examples_(std::move(examples)) {
    scripts_ = std::make_unique<lua::Runtime>();  // commands attach later
}

std::vector<std::pair<std::string, json::Value>> SandboxHost::takeEvents() {
    return std::exchange(events_, {});
}

void SandboxHost::attachCommands(cmd::Dispatcher& dispatcher) {
    if (!scripts_) scripts_ = std::make_unique<lua::Runtime>();
    scripts_->setCommands(dispatcher.registry(), dispatcher);
}

cmd::Context SandboxHost::makeContext(const cmd::Origin& origin) {
    session_->adoptBackgroundRenders();  // e.g. an optimizer's best candidate
    cmd::Context ctx(origin);
    ctx.provide<Session>(*session_);
    if (scripts_) ctx.provide<lua::Runtime>(*scripts_);
    return ctx;
}

void SandboxHost::stopJobs() { session_->stopJobs(); }

Result<std::uint64_t> SandboxHost::beginTransaction(const cmd::Origin&, std::string_view) {
    SUPPLE_TRY(session_->begin());
    openTxn_ = nextTxn_++;
    return openTxn_;
}

Status SandboxHost::commitTransaction(std::uint64_t id) {
    if (id != openTxn_) return Error("sandbox.transaction", "no such open transaction");
    session_->commit();
    openTxn_ = 0;
    return okStatus();
}

Status SandboxHost::rollbackTransaction(std::uint64_t id) {
    if (id != openTxn_) return Error("sandbox.transaction", "no such open transaction");
    auto result = session_->rollback();
    openTxn_ = 0;
    return result;
}

Result<std::function<Status()>> SandboxHost::invocationSavepoint() {
    if (!session_->inTransaction()) return std::function<Status()>{};
    auto report = [this](const Error& error) {
        auto encode = [](auto&& self, const Error& e) -> json::Value {
            json::Value value = json::Value::object();
            value["code"] = e.code;
            value["message"] = e.message;
            value["path"] = e.path;
            value["hint"] = e.hint;
            if (!e.details.empty()) {
                value["details"] = json::Value::array();
                for (const auto& detail : e.details) value["details"].push_back(self(self, detail));
            }
            return value;
        };
        emit("sandbox.savepoint_failed", encode(encode, error));
    };
    auto saved = session_->invocationSavepoint();
    if (!saved) {
        report(saved.error());
        return saved.error();
    }
    const auto eventCount = events_.size();
    auto restoreEvents = scripts_ ? scripts_->eventSavepoint() : std::function<void()>{};
    return std::function<Status()>([this, restore = std::move(*saved), eventCount,
                                    restoreEvents = std::move(restoreEvents), report] {
        auto result = restore();
        if (events_.size() > eventCount) events_.resize(eventCount);
        if (restoreEvents) restoreEvents();
        if (!result) report(result.error());
        return result;
    });
}

json::Value toJson(const cmd::LogRecord& r) {
    static constexpr const char* kOrigins[] = {"ui",   "lua",      "mcp",  "rpc",
                                               "test", "internal", "agent"};
    json::Value v = json::Value::object();
    v["seq"] = r.seq;
    v["time_ms"] = r.time_ms;
    json::Value o = json::Value::object();
    o["type"] = kOrigins[static_cast<int>(r.origin.type)];
    if (!r.origin.client.empty()) o["client"] = r.origin.client;
    if (!r.origin.agent.empty()) o["agent"] = r.origin.agent;
    v["origin"] = std::move(o);
    v["command"] = r.command;
    v["version"] = r.version;
    v["args"] = r.args;
    v["ok"] = r.ok;
    if (!r.ok) v["error"] = r.errorCode;
    if (r.ok) v["result"] = r.resultDigest;
    v["duration_us"] = r.duration_us;
    return v;
}

void SandboxHost::log(const cmd::LogRecord& record) {
    records_.push_back(record);
    if (commandLog_.empty()) return;
    std::ofstream out(commandLog_, std::ios::app);
    out << json::writeCompact(toJson(record)) << '\n';
}

void SandboxHost::emit(std::string_view topic, json::Value payload) {
    // Scripts see the event once the emitting command has finished.
    if (scripts_) scripts_->event(topic, payload);
    events_.emplace_back(std::string(topic), std::move(payload));
}

void SandboxHost::commandFinished() {
    if (scripts_) scripts_->deliver();
}

bool SandboxHost::actionTransactional(std::string_view command) const {
    return command == "render";  // a rolled-back render is deleted
}

std::uint64_t SandboxHost::clockMs() const {
    if (fixedClock_) return fixedClock_;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void SandboxHost::setupFixture(cmd::Dispatcher& dispatcher, std::string_view fixture) {
    if (fixture == "empty") return;
    if (fixture == "job_running") {  // the bus's own job.* examples
        (void)dispatcher.jobs().create();
        return;
    }
    // gain/gain.cpp in the DSP script library; dsp_player: and the script
    // player on the plugin path (the harness gives it); dsp_player_editor:
    // a player built with its editor.
    if (fixture == "dsp_script" || fixture.starts_with("dsp_player")) {
        (void)writeFileAtomic(session_->scriptDir() / "gain" / "gain.cpp",
                              dsp_commands::detail::kGain);
        return;
    }
    Session& s = *session_;
    (void)s.load(examples_.instrument);
    (void)s.load(examples_.effect);
    if (fixture == "synth_with_preset") {
        (void)s.patchMerge("in_2", obj(R"({"params": {"gain": -3}})"));
        const std::string tags[] = {"soft"};
        (void)s.presetSave("in_2", "Quiet", tags);
        (void)s.patchMerge("in_2", obj(R"({"params": {"gain": 0}})"));
    }
    if (fixture == "synth_and_render") {
        auto seq = notation::compile("voice lead; 1: A4:q r:q r:h");
        if (seq) {
            RenderInput in;
            in.notes = std::move(seq->sequence);
            (void)s.render(in);
        }
    }
}

}  // namespace supple::sandbox
