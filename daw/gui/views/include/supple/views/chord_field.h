// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The chord field (docs/specs/gui.md §4.3 and §4.7, ADR-0095): one editor for
// a chord point, opened from the arrange ruler's chord row and from the piano
// roll's chord strip. It holds the symbol of the chord point at `beat` ("" when
// none starts there); Enter writes it with chord.set (an empty field writes "no
// chord"), and Tab writes it, when not empty, and opens the field again at the
// next bar.
#pragma once

#include <functional>
#include <string>

#include "supple/json/value.h"
#include "supple/model/session.h"
#include "supple/ui/geometry.h"

namespace supple::ui {
class Root;
}

namespace supple::views {

struct ChordFieldHost {
    // The session as it is now (read again after each write).
    std::function<const model::Session&()> session;
    // Runs a command; false when it failed (the host reports the error).
    std::function<bool(const std::string& command, json::Value args)> run;
    // The window x of a project beat, where the field opens for it.
    std::function<float(double beat)> xAt;
};

// The symbol of the chord point that starts at `beat`, "" when none does.
std::string chordSymbolAt(const model::Session& s, double beat);

// Opens the field as a popup of `root` at `at`, for the chord point at the
// project beat `beat`.
void openChordField(ui::Root& root, ChordFieldHost host, double beat, ui::Point at);

}  // namespace supple::views
