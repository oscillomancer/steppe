// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace supple::paths {
// Platform directory roots, without the product slug. No directory creation.
// Empty config/data/cache means no user directory is available. Runtime always
// has a temporary-directory fallback. data/cache allowTemporary preserve the
// CLI fallback when no user directory exists. Call only off audio, before workers start.
std::filesystem::path home();
std::filesystem::path config();
std::filesystem::path data(bool allowTemporary = false);
// Non-roaming data, no temporary fallback (ADR-0083).
std::filesystem::path localData();
std::filesystem::path cache(bool allowTemporary = false);
std::filesystem::path runtime();

// A folder name for an identifier (a plugin ID) that is safe on every
// platform and distinct for distinct IDs, also where names ignore case: the
// ID with every character but letters, digits, '.', '-' and '_' made '_'
// (at most 100 of them, after a '_' when it would start with '.' or be
// empty), then '-' and the first 8 hex digits of the whole ID's SHA-256.
std::string idFolder(std::string_view id);
// The name earlier versions gave it: the first part alone, untruncated.
// Distinct IDs can share it, so a reader checks whose files it finds there.
// Read, never written.
std::string legacyIdFolder(std::string_view id);
}  // namespace supple::paths
