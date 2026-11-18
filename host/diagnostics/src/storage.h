// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <string>
#include "supple/base/result.h"
#include "supple/diagcap/native.h"
#include "supple/diagcap/trace.h"
#include "supple/json/json.h"
namespace supple::diagnostics::detail {
class Inventory;
Status prepareDirectory(const std::filesystem::path& root);
Result<std::intptr_t> createMarker(const std::filesystem::path& root, const Packet& boot);
void releaseMarker(std::intptr_t handle);
// The supervisor's own lock on its incident, `<incident>.pub`, taken before the worker starts and
// held until the report is published and the worker's marker removed. The worker's marker is
// released when the worker dies, which is before the supervisor has formatted and published; in
// that window the run is still the supervisor's (F1 of round 38b), and a reader that counts only
// the worker's marker would call it a run that left no report.
Result<std::intptr_t> createPublishHold(const std::filesystem::path& root, const Packet& boot);
// Lets go of the hold and removes its file: on POSIX the file first, while it is still locked.
void removePublishHold(const std::filesystem::path& root, const Packet& boot, std::intptr_t file);
Status publishReport(const std::filesystem::path& root, const Packet& boot, const Packet* terminal,
                     int code, bool fatal, const TraceSnapshot* trace = nullptr,
                     const Inventory* inventory = nullptr);
// A report of a bridge process that died while the worker lives (topic 6a): kind `bridge_exit`,
// under an incident of its own with no run marker, written by the supervisor from the inventory
// alone. `bridge` must be an ended one in `inventory`.
Status publishBridgeReport(const std::filesystem::path& root, const Packet& boot,
                           const Inventory& inventory, std::uint16_t bridge);
// The same in two steps, so that the supervisor can do the second off its receive loop. The first
// formats the report from the inventory as it is now (in memory, no file); it fails when `bridge`
// is not an ended one or the report would exceed its size. The second writes it and applies
// retention, which reads every report in the folder: that is the part that takes time.
struct PreparedBridgeReport {
    std::string id;    // the incident, 32 lowercase hex characters
    std::string text;  // the canonical report
};
std::optional<PreparedBridgeReport> prepareBridgeReport(const Packet& boot,
                                                        const Inventory& inventory,
                                                        std::uint16_t bridge);
Status writeBridgeReport(const std::filesystem::path& root, const PreparedBridgeReport& report);
void removeMarker(const std::filesystem::path& root, const Packet& boot);
// The worker removes its own marker, which it still holds open and locked, when no supervisor
// is left to do it (ADR-0091). POSIX unlinks the file while the lock is still held, so there is
// no moment at which it is both present and unlocked, then closes it; Windows has to close its
// handle first (the file is not opened for deletion), then removes the file.
void removeOwnMarker(const std::filesystem::path& root, const Packet& boot, std::intptr_t file);
// Retention keeps the newest 20 crash and unexpected-exit reports and, on a budget of their own,
// the newest `kKeepBridgeReports` bridge_exit reports; 50 MiB bounds both together.
inline constexpr std::size_t kKeepBridgeReports = 5;
Status pruneReports(const std::filesystem::path& root);
// Runs that ended and left no report keep their marker so the next start can say so; the newest
// `kKeepUnreportedRuns` are kept and older ones removed with their acknowledgements. Held markers
// (live runs) and markers beside a report are never touched. Runs after a report is published
// and whenever a run creates its own marker.
inline constexpr std::size_t kKeepUnreportedRuns = 20;
Status pruneMarkers(const std::filesystem::path& root);
// Reading what earlier runs left (reports.h). A report-named file is `<32 hex>.json`; its stem is
// the incident, empty for any other name.
std::string reportStem(const std::filesystem::path& path);
// The shape check retention and the reader share. Tolerant (the reader's) accepts a kind and
// top-level fields this build does not know, and an attribution level it does not know; the
// strict one is the authority to delete, so a report it cannot fully read is kept.
bool reportShape(const json::Value& value, bool tolerant = false);
// The folder is the user's own, or does not exist (then the caller finds nothing). Creates nothing.
Status checkDirectoryForReading(const std::filesystem::path& root);
// What a run marker says. Held: some process holds its lock, or it is still being created (the
// creator locks before it writes, so an empty marker is one being created). Free: nobody holds
// it, so the run is over. Gone and Unreadable: no marker, or not a file this user owns.
enum class MarkerState { Gone, Free, Held, Unreadable };
MarkerState probeMarker(const std::filesystem::path& path);
// The state of an incident's run: its marker and the supervisor's hold together. Held while either
// is held, unless the marker is already gone (the report is then complete). The hold is probed
// first: if it is free the supervisor is finished (or dead), and by then it has removed the marker
// it was waiting to remove, so the two probes cannot disagree about a run that just ended.
MarkerState probeRun(const std::filesystem::path& root, const std::string& incident);
}  // namespace supple::diagnostics::detail
