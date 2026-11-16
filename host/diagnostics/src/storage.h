// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include "native.h"
#include "supple/base/result.h"
#include "supple/json/json.h"
#include "trace.h"
namespace supple::diagnostics::detail {
Status prepareDirectory(const std::filesystem::path& root);
Result<std::intptr_t> createMarker(const std::filesystem::path& root, const Packet& boot);
void releaseMarker(std::intptr_t handle);
Status publishReport(const std::filesystem::path& root, const Packet& boot, const Packet* terminal,
                     int code, bool fatal, const TraceSnapshot* trace = nullptr);
void removeMarker(const std::filesystem::path& root, const Packet& boot);
Status pruneReports(const std::filesystem::path& root);
}  // namespace supple::diagnostics::detail
