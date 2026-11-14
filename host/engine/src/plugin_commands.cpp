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
}

}  // namespace supple::engine
