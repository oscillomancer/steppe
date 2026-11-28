// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The GUI's own words for what ADR-0096 M5 adds to the commands that copy,
// move or join takes (gui.md §2, "Modulation words"): the refusals
// modulation.limit, modulation.cycle and item.heal_fx, and the warnings
// modulation.not_copied and modulation.removed. Each is keyed on the
// command and the code; a cycle refusal also on the argument it is refused
// at (session-model.md's table of the edits that can close one: track.set's
// parent send is not its rack's Out); a warning also on its reason, the
// cause the bus names (command-bus.md), its path naming only its route.
// None carries the engine's message, its hint, a command's name or a raw
// id.
// modulation.stacked is a route command's warning (modulate,
// modulation.add), which keep their own words, so it has none here.
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

// One warning's words: `code` at `path` (as the result gives it,
// /modulations/<id>) from `command`, its cause from `reason` (not_copied:
// source, cycle or limit; removed: source_removed, target_removed,
// source_unfit or target_unfit; none or another: the plain sentence).
// nullopt for a code that is not modulation.not_copied or
// modulation.removed. `s` is the session to name the route from: before
// the command for a delete (its route is gone after it), before or after
// for a copy (the original is in both). A route `s` does not hold is "A
// modulation (route)".
std::optional<std::string> modulationWarningWords(const model::Session& s, std::string_view command,
                                                  std::string_view code, std::string_view path,
                                                  std::string_view reason = {});
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
