// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <string>
#include "native.h"
#include "supple/base/result.h"
#include "supple/json/json.h"
#include "trace.h"
namespace supple::diagnostics::detail {
class Inventory;
Status prepareDirectory(const std::filesystem::path& root);
Result<std::intptr_t> createMarker(const std::filesystem::path& root, const Packet& boot);
void releaseMarker(std::intptr_t handle);
Status publishReport(const std::filesystem::path& root, const Packet& boot, const Packet* terminal,
                     int code, bool fatal, const TraceSnapshot* trace = nullptr,
                     const Inventory* inventory = nullptr);
void removeMarker(const std::filesystem::path& root, const Packet& boot);
// The worker removes its own marker, which it still holds open and locked, when no supervisor
// is left to do it (ADR-0091). POSIX unlinks the file while the lock is still held, so there is
// no moment at which it is both present and unlocked, then closes it; Windows has to close its
// handle first (the file is not opened for deletion), then removes the file.
void removeOwnMarker(const std::filesystem::path& root, const Packet& boot, std::intptr_t file);
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
}  // namespace supple::diagnostics::detail
