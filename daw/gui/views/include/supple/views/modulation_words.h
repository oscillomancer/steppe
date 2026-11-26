// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The GUI's own words for what ADR-0096 M5 adds to the commands that copy,
// move or join takes (gui.md §2, "Modulation words"): the refusals
// modulation.limit, modulation.cycle and item.heal_fx, and the warnings
// modulation.not_copied, modulation.stacked and modulation.removed. Each is
// keyed on the command, the code and the path; none carries the engine's
// message, its hint, a command's name or a raw id.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/model/session.h"

namespace supple::views {

// The words for `e`, refused by `command`, when its code is one of M5's
// copy, move or join refusals; nullopt for any other code (its caller keeps
// its own words). `s` is the session the command ran on.
std::optional<std::string> modulationRefusalWords(const model::Session& s, std::string_view command,
                                                  const Error& e);
// `e` with modulationRefusalWords' words as its message and no hint, or `e`
// as it is.
Error modulationRefusal(const model::Session& s, std::string_view command, Error e);

// One warning's words: `code` at `path` (as the result gives it, under
// /modulations/<id> or /target) from `command`. nullopt for a code that is
// not a modulation warning. A removed route is named when `s` still holds
// it (a delete's caller passes the session from before it), else "A
// modulation route".
std::optional<std::string> modulationWarningWords(const model::Session& s, std::string_view command,
                                                  std::string_view code, std::string_view path);
// The status line's note for the modulation warnings in `result` (a
// command's answer, `warnings`), "" when it has none: the first in full,
// then how many more ("and 2 more modulations were not copied").
std::string modulationNote(const model::Session& s, std::string_view command,
                           const json::Value& result);
// modulationNote's words for one of a gesture's commands added to `note`
// (a space between), for a gesture that runs several.
void appendModulationNote(std::string& note, const model::Session& s, std::string_view command,
                          const json::Value& result);
// Adds `result`'s warnings to `into`'s ({"warnings": [...]}; `into` is made
// an object when it is not one), so a gesture of several commands is
// noted once.
void collectWarnings(json::Value& into, const json::Value& result);

}  // namespace supple::views
