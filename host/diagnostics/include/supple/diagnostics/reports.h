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
}  // namespace supple::diagnostics
