// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Text notation for MIDI items. Compilation and decompilation are non-real-time.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <supple/base/result.h>
#include <supple/harmony/harmony.h>
#include <supple/midi/midi.h>

namespace supple::notation {

struct Warning {
    std::string code;
    std::string message;
    std::string path;
};

struct DrumMap {
    std::vector<std::pair<std::string, std::uint8_t>> names;
};

struct CompileContext {
    std::optional<int> ppq;
    std::function<midi::Meter(int bar)> meterAt;
    std::optional<DrumMap> drums;
};

// A chord symbol event `{SYMBOL}:DUR` as compiled, at its start tick (ADR-0098).
struct ChordSymbol {
    std::int64_t tick = 0;
    harmony::Chord chord;
    std::string voice;  // the voice's name; a text without a voice line has "main"
};

struct Compiled {
    midi::Sequence sequence;
    std::vector<midi::Meter> barMeters;
    std::optional<double> tempo;
    std::int64_t pickupTicks = 0;
    std::vector<Warning> warnings;
    // One entry per chord symbol event, the bars a copy line or a bar range
    // repeats included, ordered by tick and then by voice declaration (a text
    // with no voice line has one implicit voice, declared first). A tied
    // continuation adds none, and a symbol that failed to compile adds none.
    std::vector<ChordSymbol> chords;
};

Result<Compiled> compile(std::string_view text, const CompileContext& context = {});

struct DecompileOptions {
    std::function<midi::Meter(int bar)> meterAt;
    bool drumGrids = true;
    std::optional<DrumMap> drums;
    // Spells pitch names by this key (harmony::spell). Unset: the default table,
    // which is what the output has always used.
    std::optional<harmony::Key> key;
};

Result<std::string> decompile(const midi::Sequence& sequence, const DecompileOptions& options = {});

// A duration token as the notation reads it (`q.`, `h+e`, `e3`, `700t`) in ticks at
// `ppq`, and the shortest token the decompiler would write for a tick count (a sum
// of two tokens, else `Nt`). Errors: `notation.bad_duration`, `notation.tick_fraction`;
// zero is not a duration here. The lead sheet shares them (ADR-0095).
Result<std::int64_t> parseDuration(std::string_view token, int ppq);
std::string durationText(std::int64_t ticks, int ppq);

}  // namespace supple::notation
