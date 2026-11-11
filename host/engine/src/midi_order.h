// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>
#include "supple/base/rt.h"

namespace supple::engine::detail {
// Prepared scratch keeps sorting allocation-free. Releases normally precede
// onsets, except when that would reverse a voice's own chronology at a frame
// (late events and playrate compression can put an entire note at one frame).
struct MidiOrder {
    std::vector<std::size_t> indices;
    std::vector<std::uint8_t> chronological;
    void prepare(std::size_t events, std::uint32_t frames) {
        indices.reserve(events);
        chronological.resize(frames);
    }
    template <class T, class Time, class Address, class Rank>
    void sort(std::vector<T>& events, std::vector<T>& scratch, Time time, Address address,
              Rank rank) SUPPLE_RT {
        indices.resize(events.size());
        std::iota(indices.begin(), indices.end(), 0);
        std::fill(chronological.begin(), chronological.end(), 0);
        std::sort(indices.begin(), indices.end(), [&](auto a, auto b) {
            if (address(events[a]) != address(events[b]))
                return address(events[a]) < address(events[b]);
            if (time(events[a]) != time(events[b])) return time(events[a]) < time(events[b]);
            return a < b;
        });
        auto previous = events.size();
        bool onset = false;
        for (auto i : indices) {
            const auto frame = time(events[i]);
            if (previous == events.size() || time(events[previous]) != frame ||
                address(events[previous]) != address(events[i]))
                onset = false;
            if (rank(events[i]) == 2)
                onset = true;
            else if (rank(events[i]) == 0 && onset && frame < chronological.size())
                chronological[frame] = 1;
            previous = i;
        }
        std::sort(indices.begin(), indices.end(), [&](auto a, auto b) {
            const auto frame = time(events[a]);
            if (frame != time(events[b])) return frame < time(events[b]);
            if (frame < chronological.size() && chronological[frame]) return a < b;
            if (rank(events[a]) != rank(events[b])) return rank(events[a]) < rank(events[b]);
            if (address(events[a]) != address(events[b]))
                return address(events[a]) < address(events[b]);
            return a < b;
        });
        scratch.resize(events.size());
        for (std::size_t i = 0; i < indices.size(); ++i) scratch[i] = events[indices[i]];
        events.swap(scratch);
    }
};
}  // namespace supple::engine::detail
