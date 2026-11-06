// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Object IDs (ADR-0008, session-model §1.1): `<kind>_<n>`, n from a per-kind
// counter that never goes backwards within a lineage. `tr_0` is the master.
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace supple::model {

enum class Kind : std::uint8_t {
    Track,          // tr
    Item,           // it
    Take,           // tk
    Media,          // md  audio media source
    Midi,           // mi  MIDI source
    Fx,             // fx  plugin instance
    Route,          // sd
    Envelope,       // en
    AutoItem,       // ai
    Marker,         // mk
    Region,         // rg
    TempoPoint,     // tp
    Meter,          // ts
    Group,          // gr
    Lane,           // ln
    RenderJob,      // rj
    Rack,           // rk  a modular rack (ADR-0062)
    Module,         // mo  a module in a rack
    Cable,          // cb  a cable between modules
    Project = 255,  // non-allocated singleton, used in changes/diffs
};
inline constexpr std::size_t kKindCount = 19;

std::string_view prefix(Kind kind);  // "tr"
std::optional<Kind> kindFromPrefix(std::string_view prefix);

struct Id {
    Kind kind = Kind::Track;
    std::uint32_t n = 0;

    std::string str() const;  // "tr_3"
    static std::optional<Id> parse(std::string_view text);
    friend auto operator<=>(const Id&, const Id&) = default;
    friend bool operator==(const Id&, const Id&) = default;
};

inline constexpr Id kMaster{Kind::Track, 0};
inline constexpr Id kProject{Kind::Project, 0};

// Per-kind next-ID counters (session-model: project.next_ids).
struct Counters {
    std::array<std::uint32_t, kKindCount> next{};
    std::uint32_t& operator[](Kind k) { return next[static_cast<std::size_t>(k)]; }
    std::uint32_t operator[](Kind k) const { return next[static_cast<std::size_t>(k)]; }
    friend bool operator==(const Counters&, const Counters&) = default;
};

}  // namespace supple::model
