// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The project file with its branches (ADR-0076, session-model §5.4). The
// document is main's head, as projectToJson writes it; a `branches` section
// beside it keeps the other branches, the versions merges and undo need, and
// which branch is current. Each version's state is stored as the objects in
// which it differs from main's head.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/json/json.h"
#include "supple/model/versions.h"
#include "supple/schema/schema.h"

namespace supple::model {

// How deep a top-level section's objects sit in a project document, which decides what a
// saved branch's state names: 1 for a section whose members are objects by ID (or blobs
// by hash), 2 for one a level further down (`rulers`, `sources`, `context`), 0 for
// anything else, saved whole. A hand-kept list: a test checks it against every section
// projectToJson writes.
int objectDepth(std::string_view section);

// The largest autosave number, and so the largest `autosaved_through`: a JSON
// integer is signed 64-bit. autosave-<n> with n above it is not an autosave.
inline constexpr std::uint64_t kAutosaveNumberMax = 9'223'372'036'854'775'807ULL;

// `history`: keep every retained version, base and redo step (undo survives
// reopening); otherwise what VersionStore::saved(false) keeps. A store with
// main alone and nothing to undo writes no `branches` section.
// `autosavedThrough`: the highest numbered autosave (<file>-data/autosave-<n>)
// this file holds the work of, written as `autosaved_through` (schema 2.15):
// an autosave numbered above it is newer than the file (ADR-0078). At most
// kAutosaveNumberMax (a larger value is written as it).
json::Value projectFileToJson(const VersionStore& store, bool history = false,
                              std::optional<std::uint64_t> autosavedThrough = std::nullopt);
std::string serializeProjectFile(const VersionStore& store, bool history = false,
                                 std::optional<std::uint64_t> autosavedThrough = std::nullopt);

struct OpenedProject {
    VersionStore store;
    // The file's `autosaved_through`; absent in a file written before 2.15,
    // or by a program that does not autosave, and when it does not read.
    std::optional<std::uint64_t> autosavedThrough;
    // Set when the file had a branches section that could not be restored:
    // main opened alone, and saving over the file would drop the section.
    std::optional<Error> branchesDropped;
};
// Which logged steps a saved branch may hold: a merge replays them, so a
// file from someone else must not be able to name a step that does more than
// edit the session (ADR-0076, "Replayable steps"). The engine passes its rule
// (EngineApp::replayableFromFile); without one only the structure is checked.
using InvocationCheck = std::function<Status(const Invocation&)>;
// Main's document must load (its errors are the project's); a damaged or
// stale branches section, or one with a step `check` refuses, only sets
// branchesDropped.
Result<OpenedProject> projectFileFromJson(const json::Value& doc,
                                          std::vector<schema::Warning>* warnings = nullptr,
                                          const InvocationCheck& check = {});
Result<OpenedProject> parseProjectFile(std::string_view text,
                                       std::vector<schema::Warning>* warnings = nullptr,
                                       const InvocationCheck& check = {});

}  // namespace supple::model
