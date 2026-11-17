// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// crash.*: the crash reports earlier runs left (ADR-0083, topic 7).
#include <chrono>
#include "command_helpers.h"
#include "supple/diagnostics/reports.h"

namespace supple::engine {
namespace {
namespace diag = supple::diagnostics;

constexpr const char* kIncident = "0123456789abcdef0123456789abcdef";

// The reader's errors as the command bus wants them: a code of this namespace, the argument's path.
Error failed(const Error& e) {
    if (e.code == "diagnostics.no_report")
        return Error("crash.no_report", e.message, "/incident",
                     "list the reports with crash.list and use an incident from it; a report "
                     "appears once its run has ended");
    return Error("crash.storage", e.message, {}, e.hint);
}

// Unix milliseconds of a file time, whatever clock the standard library's file clock is.
std::int64_t unixMs(std::filesystem::file_time_type t) {
    const auto offset = t - std::filesystem::file_time_type::clock::now();
    const auto at = std::chrono::system_clock::now() +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(offset);
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

json::Value reportJson(const diag::ReportSummary& r) {
    json::Value o = json::Value::object();
    o["incident"] = r.incident;
    o["role"] = r.role;
    o["kind"] = r.kind == diag::ReportKind::Crash            ? "crash"
                : r.kind == diag::ReportKind::UnexpectedExit ? "unexpected_exit"
                                                             : "other";
    o["platform"] = r.platform;
    o["build"] = json::Value::object();
    o["build"]["version"] = r.version;
    o["build"]["commit"] = r.commit;
    o["written_ms"] = unixMs(r.written);
    o["acknowledged"] = r.acknowledged;
    o["attribution"] = r.attribution.empty() ? json::Value() : json::Value(r.attribution);
    if (r.culprit) {
        json::Value p = json::Value::object();
        p["name"] = r.culprit->name;
        p["vendor"] = r.culprit->vendor;
        p["version"] = r.culprit->version;
        p["build_id"] = r.culprit->buildId;
        o["plugin"] = std::move(p);
    } else {
        o["plugin"] = json::Value();
    }
    o["plugin_count"] = static_cast<std::int64_t>(r.pluginCount);
    return o;
}

struct ListArgs {
    std::optional<std::int64_t> limit;
};
struct IncidentArgs {
    std::string incident;
};
auto incidentSchema(const char* name) {
    return schema::object<IncidentArgs>(name)
        .field("incident", &IncidentArgs::incident,
               schema::string()
                   .pattern("^[0-9a-f]{32}$", "incident id")
                   .doc("A report's incident id, from crash.list."))
        .build();
}
}  // namespace

void registerCrashReportCommands(cmd::Registry& registry) {
    [&] {
        Command<ListArgs, json::Value> c;
        c.name = "crash.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "The crash reports earlier runs left, newest first.";
        c.description =
            "Each report says when it was written, which build and role, how the run ended "
            "(kind: crash, unexpected_exit, or other for a kind this build does not know), the "
            "attribution level (fault_in_image, active_token, concurrent_activity, "
            "recent_command, unknown; a stated confidence, not a verdict) and the plug-in it "
            "names, if any, as the plug-in described itself. unreported lists runs that ended "
            "and left no report (the supervisor died or could not write). A report counts once "
            "its run has ended: reports_pending are still being written; live_runs are running "
            "now, this one included. ignored counts files that are not valid reports. limit caps "
            "each list (default 50); the _total fields count them all. Reports hold plug-in and "
            "command names, never project content.";
        c.tags = {"diagnostics"};
        c.args = schema::object<ListArgs>("crash_list_args")
                     .field("limit", &ListArgs::limit, schema::integer().range(1, 200))
                     .build();
        c.result = schema::any();
        c.examples = {{"The reports left behind", json::Value::object(),
                       j(R"({"reports":"<any>","reports_total":1,"unreported":[],
                            "unreported_total":0,"live_runs":0,"reports_pending":0,
                            "ignored":0})"),
                       "song_with_crash_report"}};
        c.run = [](const ListArgs& a, Context& ctx) -> Result<json::Value> {
            auto listing = diag::listReports(app(ctx).diagnosticsDir());
            if (!listing) return failed(listing.error());
            const auto cap = static_cast<std::size_t>(a.limit.value_or(50));
            json::Value out = json::Value::object();
            json::Value reports = json::Value::array(), unreported = json::Value::array();
            for (const auto& r : listing->reports) {
                if (reports.size() == cap) break;
                reports.push_back(reportJson(r));
            }
            for (const auto& r : listing->unreported) {
                if (unreported.size() == cap) break;
                json::Value o = json::Value::object();
                o["incident"] = r.incident;
                o["role"] = r.role;
                o["acknowledged"] = r.acknowledged;
                unreported.push_back(std::move(o));
            }
            out["reports"] = std::move(reports);
            out["reports_total"] = static_cast<std::int64_t>(listing->reports.size());
            out["unreported"] = std::move(unreported);
            out["unreported_total"] = static_cast<std::int64_t>(listing->unreported.size());
            out["live_runs"] = static_cast<std::int64_t>(listing->liveRuns);
            out["reports_pending"] = static_cast<std::int64_t>(listing->pending);
            out["ignored"] = static_cast<std::int64_t>(listing->ignored);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<IncidentArgs, json::Value> c;
        c.name = "crash.read";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "One crash report in full.";
        c.description =
            "The report as the supervisor wrote it (schema supple.crash-report/1.0): the run, the "
            "plug-ins it had announced, the threads with module-relative frames, the last "
            "commands, the attribution and everything it could not collect (unavailable, "
            "dropped). Frames are not symbolised: the offline resolver does that. Reports are "
            "tens of kilobytes and at most 4 MiB.";
        c.tags = {"diagnostics"};
        c.args = incidentSchema("crash_read_args");
        c.result = schema::any();
        c.errors = {{"crash.no_report", "no finished report has that incident id",
                     "list the reports with crash.list"}};
        c.examples = {{"Read a report", j(std::string(R"({"incident":")") + kIncident + "\"}"),
                       j(R"({"incident":"0123456789abcdef0123456789abcdef","report":"<any>"})"),
                       "song_with_crash_report"}};
        c.run = [](const IncidentArgs& a, Context& ctx) -> Result<json::Value> {
            auto text = diag::readReport(a.incident, app(ctx).diagnosticsDir());
            if (!text) return failed(text.error());
            auto parsed = json::parse(*text);
            if (!parsed) return Error("crash.storage", "the report could not be read", {}, {});
            json::Value out = json::Value::object();
            out["incident"] = a.incident;
            out["report"] = std::move(*parsed);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<IncidentArgs, json::Value> c;
        c.name = "crash.acknowledge";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Mark a crash report, or a run that left none, as seen.";
        c.description =
            "Writes a small marker beside the report; the report itself is never changed. "
            "An acknowledged report no longer counts for the restart notice. It does not end "
            "the suspicion of a plug-in: seen is not resolved, and crash.settle does that. "
            "Repeating it changes nothing.";
        c.tags = {"diagnostics"};
        c.args = incidentSchema("crash_acknowledge_args");
        c.result = schema::any();
        c.errors = {{"crash.no_report", "no finished report or ended run has that incident id",
                     "list the reports with crash.list"}};
        c.examples = {{"Acknowledge a report",
                       j(std::string(R"({"incident":")") + kIncident + "\"}"),
                       j(R"({"incident":"0123456789abcdef0123456789abcdef","acknowledged":true})"),
                       "song_with_crash_report"}};
        c.run = [](const IncidentArgs& a, Context& ctx) -> Result<json::Value> {
            auto st = diag::acknowledge(a.incident, app(ctx).diagnosticsDir());
            if (!st) return failed(st.error());
            json::Value out = json::Value::object();
            out["incident"] = a.incident;
            out["acknowledged"] = true;
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<IncidentArgs, json::Value> c;
        c.name = "crash.settle";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Mark the suspicion a crash report raised as resolved.";
        c.description =
            "Writes a small marker beside the report; the report itself is never changed. A "
            "settled report no longer makes its plug-in a suspect (project.plugins, "
            "project.open's project.suspect_plugin warning). Use it once the plug-in was "
            "loaded normally and ran well, or when the person chose to load it normally; "
            "crash.acknowledge only says the report was seen. A report whose run has not ended "
            "cannot be settled. Repeating it changes nothing.";
        c.tags = {"diagnostics"};
        c.args = incidentSchema("crash_settle_args");
        c.result = schema::any();
        c.errors = {{"crash.no_report", "no finished report has that incident id",
                     "list the reports with crash.list"}};
        c.examples = {{"Settle a report", j(std::string(R"({"incident":")") + kIncident + "\"}"),
                       j(R"({"incident":"0123456789abcdef0123456789abcdef","settled":true})"),
                       "song_with_crash_report"}};
        c.run = [](const IncidentArgs& a, Context& ctx) -> Result<json::Value> {
            auto st = diag::settle(a.incident, app(ctx).diagnosticsDir());
            if (!st) return failed(st.error());
            json::Value out = json::Value::object();
            out["incident"] = a.incident;
            out["settled"] = true;
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<IncidentArgs, json::Value> c;
        c.name = "crash.delete";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Delete one crash report, or the record of a run that left none.";
        c.description =
            "Removes the report file, its acknowledgement and an idle run marker. It cannot be "
            "undone. A run that is still live or a report still being written is refused.";
        c.tags = {"diagnostics"};
        c.args = incidentSchema("crash_delete_args");
        c.result = schema::any();
        c.errors = {{"crash.no_report", "no finished report or ended run has that incident id",
                     "list the reports with crash.list"}};
        c.examples = {{"Delete a report", j(std::string(R"({"incident":")") + kIncident + "\"}"),
                       j(R"({"incident":"0123456789abcdef0123456789abcdef","deleted":true})"),
                       "song_with_crash_report"}};
        c.run = [](const IncidentArgs& a, Context& ctx) -> Result<json::Value> {
            auto st = diag::deleteReport(a.incident, app(ctx).diagnosticsDir());
            if (!st) return failed(st.error());
            json::Value out = json::Value::object();
            out["incident"] = a.incident;
            out["deleted"] = true;
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, json::Value> c;
        c.name = "crash.clear";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Delete every finished crash report and record of an ended run.";
        c.description =
            "Removes what crash.list shows as over: reports and unreported runs, with their "
            "acknowledgements. It cannot be undone. Live runs, reports still being written and "
            "files that are not valid reports are left alone. Returns how many incidents went.";
        c.tags = {"diagnostics"};
        c.args = schema::object<NoArgs>("crash_clear_args").build();
        c.result = schema::any();
        c.examples = {{"Clear the reports", json::Value::object(), j(R"({"deleted":1})"),
                       "song_with_crash_report"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<json::Value> {
            auto n = diag::deleteAllReports(app(ctx).diagnosticsDir());
            if (!n) return failed(n.error());
            json::Value out = json::Value::object();
            out["deleted"] = static_cast<std::int64_t>(*n);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
