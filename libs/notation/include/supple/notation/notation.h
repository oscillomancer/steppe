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

struct Compiled {
    midi::Sequence sequence;
    std::vector<midi::Meter> barMeters;
    std::optional<double> tempo;
    std::int64_t pickupTicks = 0;
    std::vector<Warning> warnings;
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

}  // namespace supple::notation
