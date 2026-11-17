// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "supple/base/result.h"
namespace supple::diagnostics {
// The crash reports an application's earlier runs left in its diagnostics folder, for a restart
// notice or a viewer (ADR-0083, topic 7). Reading changes nothing except what `acknowledge`
// writes; a report is never altered, and one this build cannot fully read is still listed
// (kind `other`, an attribution level it does not know shown as `other`), never hidden.

// The folder the frontend writes to (`<local data>/<slug>/diagnostics`); empty when the
// platform has no local data folder. Every function below takes `{}` to mean this folder.
std::filesystem::path defaultDirectory();

enum class ReportKind { Crash, UnexpectedExit, Other };

// The plug-in the report's attribution names, as the plug-in described itself (its own strings,
// possibly absent) plus the identity of its binary when the report recorded one.
struct ReportPlugin {
    std::string name, vendor, version;
    std::string buildId;  // module identity (ELF build id, Mach-O UUID, PE debug id); may be empty
    // What lets a project's plug-in be matched to it (topic 7d). `pluginId` is the id the plug-in
    // gave itself (the report's `plugin_id`; the report's own `id` is only an ordinal), empty
    // when it was not sent. `kind` is the entry's kind as written (`clap`, `vst3`,
    // `clap_library`, `vst3_library`, or one this build does not know) and `format` the plug-in
    // format it stands for (`clap`, `vst3`), empty for a kind this build does not know.
    // `origin` is as written (`foreign`, `host`, `stock`, `library`, `project`).
    std::string pluginId, kind, format, origin;
};

struct ReportSummary {
    std::string incident;  // 32 lowercase hex digits
    std::string role;      // "gui", "engine" or "fixture"
    ReportKind kind = ReportKind::Other;
    std::string platform;
    std::string version, commit;  // the build that wrote it
    std::filesystem::file_time_type written;
    bool acknowledged = false;
    // The report's attribution level as the report spells it ("fault_in_image", "active_token",
    // "concurrent_activity", "recent_command", "unknown"), "other" for one this build does not
    // know, empty when the report has no attribution.
    std::string attribution;
    std::optional<ReportPlugin> culprit;  // the plug-in the attribution names, when it names one
    std::size_t pluginCount = 0;          // plug-ins and modules the run had announced
    std::filesystem::path path;
};

// A run that ended and left no report: its run marker is there with no holder and no report
// carries its incident. The supervisor died, or could not write.
struct UnreportedRun {
    std::string incident;
    std::string role;  // from the marker; empty when it cannot be read
    bool acknowledged = false;
    // When the marker was written, which is when that run started (file time).
    std::filesystem::file_time_type started;
    std::filesystem::path marker;
};

struct ReportListing {
    // Newest first. Only reports whose run is over: a report whose run marker is still held is
    // being published, and counts in `pending` until the holder lets go.
    std::vector<ReportSummary> reports;
    std::vector<UnreportedRun> unreported;  // newest first
    std::size_t liveRuns = 0;               // markers still held, this process's own among them
    std::size_t pending = 0;                // reports of runs not yet over
    std::size_t ignored = 0;                // report-named files that are not valid reports
};

// Lists the folder. A missing folder is an empty listing; one that is not private to the user
// (a link, another owner) is an error. Bounded: nothing larger than 4 MiB is read.
Result<ReportListing> listReports(const std::filesystem::path& directory = {});

// The canonical JSON text of one valid report whose run is over. Errors carry
// `diagnostics.no_report` (none, or not valid, or its run is not over).
Result<std::string> readReport(const std::string& incident,
                               const std::filesystem::path& directory = {});

// Records that the user has seen an incident (a listed report or unreported run): a small file
// beside the report, `<incident>.ack`, that is removed with the report. Idempotent. Nothing
// else is touched; deleting a report is not part of this.
Status acknowledge(const std::string& incident, const std::filesystem::path& directory = {});

// Writes the files of a run that ended without a report (a run marker nobody holds), for tests.
Status writeSampleUnreportedRun(const std::string& incident,
                                const std::filesystem::path& directory);

// Writes a crash report that names `plugin` (and its `vendor`) as the plug-in the faulting
// instruction was in (attribution fault_in_image), for tests and examples.
Status writeSampleCrash(const std::string& incident, const std::string& plugin,
                        const std::string& vendor, const std::filesystem::path& directory);

// Writes one small, valid report (an unexpected exit of a fixture run, no capture) under
// `incident`: for command examples and tests of whatever reads reports. Not for use by a run that
// has crashed.
Status writeSampleReport(const std::string& incident, const std::filesystem::path& directory);

// Deletes one incident whose run is over: its report, its acknowledgement, its settlement and, when
// nobody holds it, its run marker (an unreported run is deleted by this too). Errors carry
// `diagnostics.no_report` when there is no such finished incident. Idempotent per file.
Status deleteReport(const std::string& incident, const std::filesystem::path& directory = {});

// Deletes every incident `listReports` lists as over (reports and unreported runs) and returns
// how many. A run still live, a report still being published and a file that is not a valid
// report are left alone.
Result<std::size_t> deleteAllReports(const std::filesystem::path& directory = {});

// Plug-ins the last reports point at (topic 7d, P4-P8). The *window* is the newest
// `kSuspectReports` valid reports whose run is over, by file time, whether or not they were
// acknowledged: acknowledging means the user has seen a report, not that the suspicion is
// resolved. A report in the window yields a suspect when its attribution is `fault_in_image` or
// `active_token`, it names a plug-in, that plug-in has a format this build knows and an id
// (otherwise a project's plug-in cannot be matched to it), and it is not *settled*. Everything
// is derived from the files; nothing is kept in memory.
inline constexpr std::size_t kSuspectReports = 3;
// How many report files `suspects` opens at most while it looks for `kSuspectReports` valid
// ones; files behind that are not read.
inline constexpr std::size_t kSuspectAttempts = 8;
struct Suspect {
    // The key. `buildId` is empty for a report that recorded none (an `active_token` attribution,
    // every VST3); such a suspect is matched on the version too and is never an exact build.
    std::string format, pluginId, buildId;
    // From the newest report that yields it.
    std::string name, vendor, version, origin;
    std::string attribution;  // the strongest level among its reports
    // The reports in the window that yield it, newest first: what `settle` marks.
    std::vector<std::string> incidents;
    std::filesystem::file_time_type newest;
};
// The suspects, the one with the newest report first. A missing folder gives none; one that is not
// private to the user is an error. A report that is still being published (T8), one that is not
// valid and one a newer build wrote with a kind or level this build does not know never yield.
Result<std::vector<Suspect>> suspects(const std::filesystem::path& directory = {});
// Marks that the suspicion a report raised is resolved (the user chose to load the plug-in
// normally, or a run loaded it normally and ended well): a small file beside the report,
// `<incident>.settled`, that is removed with the report. A report is never rewritten and the
// acknowledgement is untouched. Idempotent. Only a valid report whose run is over can be settled
// (`diagnostics.no_report` otherwise), so a settlement never outlives its report except by a race
// that retention cleans up.
Status settle(const std::string& incident, const std::filesystem::path& directory = {});
// Settles every report of the suspect's `incidents`; keeps going past a failure and returns the
// first.
Status settle(const Suspect& suspect, const std::filesystem::path& directory = {});

// What a restart shows the user (topic 7b, P1): one plain statement that the last run ended
// unexpectedly, for the newest incident nobody has acknowledged, and how many earlier ones the
// same dismissal covers. Wording is provisional (P12). Only incidents whose run is over count, so
// a report still being written is never announced half done.
struct RestartNotice {
    enum class Kind { Report, NoReport } kind = Kind::Report;
    std::string headline;                // the sentence the user reads first
    std::string detail;                  // what to know next
    std::vector<std::string> incidents;  // newest first: the ones dismissing this acknowledges
    std::size_t earlier = 0;             // incidents.size() - 1
    std::string plugin;                  // the plug-in the report points at, when it points at one
};
// How many reports `lastRunNotice` parses; the unacknowledged ones behind them are counted by
// file name only (P4: the notice looks at the last three).
inline constexpr std::size_t kNoticeReportsRead = 3;
// Decides the notice from the folder, cheaply enough for a start: it lists names, probes the run
// markers, reads each acknowledgement, and parses only the newest `kNoticeReportsRead` reports,
// newest first by file time. The reports behind them are counted without being read, so a file
// that turns out not to be a valid report still counts as an earlier close. Empty when every
// ended run is acknowledged (or there were none); a folder that is not private to the user is
// an error.
Result<std::optional<RestartNotice>> lastRunNotice(const std::filesystem::path& directory = {});
// The headline and detail as one text, for a place with no layout (a terminal).
std::string noticeText(const RestartNotice& notice);
// Acknowledges every incident the notice covers (the latest and the earlier ones); keeps going
// past a failure and returns the first. Parses nothing.
Status acknowledgeNotice(const RestartNotice& notice, const std::filesystem::path& directory = {});
}  // namespace supple::diagnostics
