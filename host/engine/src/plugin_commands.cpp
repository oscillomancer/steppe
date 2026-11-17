// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Plug-in catalogue commands: plugin.list and plugin.scan.
#include "command_helpers.h"

namespace supple::engine {

namespace {

struct PluginArgs {
    std::optional<std::string> filter;
};

struct PluginEntry {
    std::string id, name, vendor, format;
    bool instrument = false, semantic = false;
    std::string path;
    std::optional<bool> shadowed;  // an earlier location on the plugin path has this id
    std::vector<std::string> features;
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

struct PluginList {
    std::vector<PluginEntry> plugins;
    ScanReport scan;
    std::optional<std::string> job;  // plugin.scan {wait: false}: the background scan
};

struct ScanArgs {
    std::optional<bool> wait;
};

struct OverrideArgs {
    std::string plugin;
    LoadMode mode = LoadMode::Normal;
};

struct OverrideResult {
    std::string plugin, name, format;
    LoadMode mode = LoadMode::Normal;
    bool changed = false;
    std::vector<std::string> fx;  // the fx of this plug-in in the head session
};

struct OverrideEntry {
    std::string plugin, name, format;
    LoadMode mode = LoadMode::Bridge;
    std::vector<std::string> fx;
};

struct OverrideList {
    std::vector<OverrideEntry> overrides;
    bool bridgeAvailable = false;
};

auto loadModeSchema() {
    return schema::enumeration<LoadMode>({{LoadMode::Normal, "normal"},
                                          {LoadMode::Bridge, "bridge"},
                                          {LoadMode::Disable, "disable"}});
}

// The fx of the head session that load `info`.
std::vector<std::string> fxOf(EngineApp& app, const host::PluginInfo& info) {
    std::vector<std::string> out;
    auto plugins = app.plugins();
    for (const auto& entry : app.head().fx) {
        const auto& ref = entry.value->plugin;
        bool same = ref.id == info.id;
        if (!same && plugins)
            if (auto found = EngineApp::findPlugin(*plugins, ref.id))
                same = found->id == info.id && found->format == info.format;
        if (same) out.push_back(entry.id.str());
    }
    return out;
}

}  // namespace

void registerPluginCommands(cmd::Registry& registry) {
    [&] {
        Command<PluginArgs, PluginList> c;
        c.name = "plugin.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List scanned plugins; filter matches id, name, or vendor.";
        c.tags = {"plugin"};
        c.args = schema::object<PluginArgs>("plugin_list_args")
                     .field("filter", &PluginArgs::filter, schema::string())
                     .build();
        auto entry =
            schema::object<PluginEntry>("plugin_entry")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("format", &PluginEntry::format, schema::string().doc("clap or vst3"))
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .field("features", &PluginEntry::features,
                       schema::arrayOf(schema::string())
                           .doc("CLAP features (a VST3's subcategories): the main category "
                                "first, then the kind of plugin, such as reverb"))
                .field("path", &PluginEntry::path, schema::string())
                .field("shadowed", &PluginEntry::shadowed,
                       schema::boolean().doc("Another location earlier on the plugin path has the "
                                             "same id; fx.add by id uses that one"))
                .build();
        c.result = schema::object<PluginList>("plugin_list")
                       .field("plugins", &PluginList::plugins, schema::arrayOf(entry))
                       .field("scan", &PluginList::scan, scanReportSchema())
                       .build();
        c.examples = {{"Find a test synth", j(R"({"filter":"test.sine"})"),
                       j(R"({"plugins":"<any>","scan":"<any>"})"), "empty"}};
        c.run = [](const PluginArgs& a, Context& ctx) -> Result<PluginList> {
            auto found = app(ctx).plugins();
            if (!found) return std::move(found).error();
            PluginList out;
            out.scan = scanReport(app(ctx).pluginScanReport());
            for (std::size_t i = 0; i < found->size(); ++i) {
                const auto& p = (*found)[i];
                if (a.filter && p.id.find(*a.filter) == std::string::npos &&
                    p.name.find(*a.filter) == std::string::npos &&
                    p.vendor.find(*a.filter) == std::string::npos)
                    continue;
                PluginEntry e{p.id,       p.name,          p.vendor, p.format,  p.instrument,
                              p.semantic, p.path.string(), {},       p.features};
                if (host::shadowed(*found, i)) e.shadowed = true;
                out.plugins.push_back(std::move(e));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ScanArgs, PluginList> c;
        c.name = "plugin.scan";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Rescan configured plugin paths and return the available plugins.";
        c.description =
            "wait: false scans in the background (every module through the supervised "
            "scanner) and returns at once with the current list and the scan's job; the "
            "catalogue changes when the job is done. A background scan already running is "
            "returned instead of a second one.";
        c.tags = {"plugin"};
        c.args = schema::object<ScanArgs>("plugin_scan_args")
                     .field("wait", &ScanArgs::wait,
                            schema::boolean().doc("false: scan in the background (default true)"))
                     .build();
        auto entry =
            schema::object<PluginEntry>("plugin_entry")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("format", &PluginEntry::format, schema::string().doc("clap or vst3"))
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .field("features", &PluginEntry::features,
                       schema::arrayOf(schema::string())
                           .doc("CLAP features (a VST3's subcategories): the main category "
                                "first, then the kind of plugin, such as reverb"))
                .field("path", &PluginEntry::path, schema::string())
                .field("shadowed", &PluginEntry::shadowed, schema::boolean())
                .build();
        c.result = schema::object<PluginList>("plugin_list")
                       .field("plugins", &PluginList::plugins, schema::arrayOf(entry))
                       .field("scan", &PluginList::scan, scanReportSchema())
                       .field("job", &PluginList::job,
                              schema::string().doc("the background scan (wait: false)"))
                       .build();
        c.examples = {{"Scan", j("{}"), j(R"({"plugins":"<any>","scan":"<any>"})"), "empty"}};
        c.run = [](const ScanArgs& a, Context& ctx) -> Result<PluginList> {
            EngineApp& engine = app(ctx);
            const bool wait = a.wait.value_or(true);
            std::optional<std::string> job;
            if (!wait) {
                auto& jobs = ctx.service<cmd::JobManager>();
                std::string& running = engine.pluginScanJob();
                const auto state =
                    running.empty() ? Result<json::Value>(json::Value()) : jobs.status(running);
                if (!state || !state->isObject() || !state->find("status") ||
                    state->find("status")->asString() != "running") {
                    running = jobs.create();
                    auto& dispatcher = ctx.service<cmd::Dispatcher>();
                    const auto& configured = engine.options().pluginPath;
                    auto paths = configured.empty() ? host::defaultSearchPath() : configured;
                    engine.startJob([&engine, &jobs, &dispatcher, id = running,
                                     paths = std::move(paths)](supple::StopToken) {
                        host::ScanReport report;
                        host::ScanOptions options;
                        options.superviseAll = true;  // nothing loads in this thread
                        auto found = host::scan(paths, options, &report);
                        if (!found) {
                            (void)jobs.fail(id, std::move(found).error());
                            return;
                        }
                        dispatcher.post(
                            "plugin.scan.done",
                            [&engine, &jobs, id, list = std::move(*found),
                             report = std::move(report)]() mutable -> Status {
                                json::Value result = json::Value::object();
                                result["plugins"] = static_cast<std::int64_t>(list.size());
                                engine.installPlugins(std::move(list), std::move(report));
                                return jobs.complete(id, std::move(result));
                            });
                    });
                }
                job = running;
            }
            auto found = engine.plugins(wait);
            if (!found) return std::move(found).error();
            PluginList out;
            out.scan = scanReport(engine.pluginScanReport());
            for (std::size_t i = 0; i < found->size(); ++i) {
                const auto& p = (*found)[i];
                PluginEntry e{p.id,       p.name,          p.vendor, p.format,  p.instrument,
                              p.semantic, p.path.string(), {},       p.features};
                if (host::shadowed(*found, i)) e.shadowed = true;
                out.plugins.push_back(std::move(e));
            }
            out.job = std::move(job);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<OverrideArgs, OverrideResult> c;
        c.name = "plugin.override";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Load a plug-in normally, on a bridge, or not at all for this run.";
        c.description =
            "A safety choice for a plug-in suspected of crashing: mode bridge runs it in a "
            "dedicated bridge process (a crash then stays there; a live bridge can drop blocks "
            "under load), disable does not load it (its strips pass audio through and a rack "
            "module of it is silent), normal loads it as the project says. It holds until the "
            "app quits, is not in the project or its history, and is not saved: a project saved "
            "while it holds is byte-for-byte the one saved without. Instances of the plug-in "
            "restart, so state that was never saved is lost and held notes end. Stock plug-ins "
            "cannot be bridged, and bridge needs the bridge executable; both are refused with "
            "plugin.override_unavailable.";
        c.tags = {"plugin"};
        c.args = schema::object<OverrideArgs>("plugin_override_args")
                     .field("plugin", &OverrideArgs::plugin,
                            schema::string().doc("id or name, as plugin.list shows it"))
                     .field("mode", &OverrideArgs::mode, loadModeSchema())
                     .build();
        c.result = schema::object<OverrideResult>("plugin_override_result")
                       .field("plugin", &OverrideResult::plugin, schema::string())
                       .field("name", &OverrideResult::name, schema::string())
                       .field("format", &OverrideResult::format, schema::string())
                       .field("mode", &OverrideResult::mode, loadModeSchema())
                       .field("changed", &OverrideResult::changed,
                              schema::boolean().doc("false: it was set that way already"))
                       .field("fx", &OverrideResult::fx,
                              schema::arrayOf(schema::string())
                                  .doc("the fx of this plug-in in the project now"))
                       .build();
        c.examples = {
            {"Load normally", j(R"({"plugin":"com.suppleaudio.test.gain","mode":"normal"})"),
             j(R"({"plugin":"com.suppleaudio.test.gain","name":"<any>","format":"clap","mode":"normal","changed":false,"fx":[]})"),
             "empty"}};
        c.run = [](const OverrideArgs& a, Context& ctx) -> Result<OverrideResult> {
            EngineApp& engine = app(ctx);
            auto info = engine.lookupPlugin(a.plugin);
            if (!info) return std::move(info).error().at("/plugin");
            auto changed = engine.setLoadOverride(*info, a.mode);
            if (!changed) return std::move(changed).error();
            OverrideResult r;
            r.plugin = info->id;
            r.name = info->name;
            r.format = info->format;
            r.mode = a.mode;
            r.changed = *changed;
            r.fx = fxOf(engine, *info);
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, OverrideList> c;
        c.name = "plugin.overrides";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List the plug-ins this run bridges or has disabled.";
        c.description =
            "What plugin.override set, with the fx of each plug-in in the project now. "
            "bridge_available: a bridge executable was found, so mode bridge can be set.";
        c.tags = {"plugin"};
        c.args = schema::object<NoArgs>("plugin_overrides_args").build();
        auto entry = schema::object<OverrideEntry>("plugin_override_entry")
                         .field("plugin", &OverrideEntry::plugin, schema::string())
                         .field("name", &OverrideEntry::name, schema::string())
                         .field("format", &OverrideEntry::format, schema::string())
                         .field("mode", &OverrideEntry::mode, loadModeSchema())
                         .field("fx", &OverrideEntry::fx, schema::arrayOf(schema::string()))
                         .build();
        c.result = schema::object<OverrideList>("plugin_override_list")
                       .field("overrides", &OverrideList::overrides, schema::arrayOf(entry))
                       .field("bridge_available", &OverrideList::bridgeAvailable, schema::boolean())
                       .build();
        c.examples = {
            {"None set", j("{}"), j(R"({"overrides":[],"bridge_available":"<any>"})"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<OverrideList> {
            EngineApp& engine = app(ctx);
            OverrideList out;
            out.bridgeAvailable = !host::bridgeExecutable().empty();
            auto plugins = engine.plugins();
            for (const auto& [key, mode] : engine.loadOverrides().all()) {
                OverrideEntry e;
                e.plugin = key.second;
                e.format = key.first;
                e.mode = mode;
                if (plugins)
                    for (const auto& p : *plugins)
                        if (p.id == key.second && p.format == key.first) {
                            e.name = p.name;
                            e.fx = fxOf(engine, p);
                            break;
                        }
                out.overrides.push_back(std::move(e));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
