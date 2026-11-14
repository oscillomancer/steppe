// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The project file with its branches (ADR-0076, session-model §5.4). The
// document is main's head, as projectToJson writes it; a `branches` section
// beside it keeps the other branches, the versions merges and undo need, and
// which branch is current. Each version's state is stored as the objects in
// which it differs from main's head.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/json/json.h"
#include "supple/model/versions.h"
#include "supple/schema/schema.h"

namespace supple::model {

// `history`: keep every retained version, base and redo step (undo survives
// reopening); otherwise what VersionStore::saved(false) keeps. A store with
// main alone and nothing to undo writes no `branches` section.
json::Value projectFileToJson(const VersionStore& store, bool history = false);
std::string serializeProjectFile(const VersionStore& store, bool history = false);

struct OpenedProject {
    VersionStore store;
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
