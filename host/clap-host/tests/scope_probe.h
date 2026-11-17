// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Test support: look at the execution table from inside a plug-in's own callbacks.
//
// The fixture plug-in module calls an observer at the start of each callback
// (supple_fixture_set_observer). The observer here records, for each call, which words of
// the execution table are not idle at that moment: the scopes the host holds open around
// the call. It is called from process() too, so it allocates nothing and takes no lock: a
// fixed array and one atomic counter.
#pragma once
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "supple/diagctx/scope.h"
#include "supple/plugin/test_module.h"

namespace supple::scopeprobe {

struct Open {
    std::uint32_t cell = 0;
    bool audio = false;  // the cell's audio word, else its main word
    diagctx::ScopeWord word;
};
struct Sighting {
    const char* phase = nullptr;      // the callback, as the fixture names it
    std::uint64_t observerFrame = 0;  // the observer's own frame address: below every scope
    std::uint8_t count = 0;           // how many of `open` are filled
    Open open[4];
    std::uint8_t more = 0;  // words past the fourth, not recorded
};
constexpr std::size_t kSightings = std::size_t{1} << 15;
inline Sighting gSightings[kSightings];
inline std::atomic<std::size_t> gCount{0};

[[gnu::noinline]] inline void observe(const char* phase) {
    const std::size_t at = gCount.fetch_add(1, std::memory_order_relaxed);
    if (at >= kSightings) return;
    Sighting& sighting = gSightings[at];
    sighting.phase = phase;
    sighting.observerFrame = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
    sighting.count = 0;
    sighting.more = 0;
    auto& cells = diagctx::table().instances;
    for (std::uint32_t cell = 0; cell < diagctx::kMaxInstances; ++cell) {
        for (const bool audio : {true, false}) {
            const auto word = diagctx::unpackWord(
                (audio ? cells[cell].audio : cells[cell].main).load(std::memory_order_acquire));
            if (word.kind == diagctx::Entry::None) continue;
            if (sighting.count < 4)
                sighting.open[sighting.count++] = {cell, audio, word};
            else
                ++sighting.more;
        }
    }
}

// Installs the observer in the fixture module for as long as it lives. Every sighting
// since the last take() is returned by take().
class Observer {
  public:
    explicit Observer(const char* module) : module_(plugin::test::open(module)) {
        REQUIRE(module_);
        const auto set = reinterpret_cast<void (*)(void (*)(const char*))>(
            plugin::test::symbol(module_, "supple_fixture_set_observer"));
        REQUIRE(set);
        gCount.store(0);
        set(&observe);
    }
    ~Observer() {
        const auto set = reinterpret_cast<void (*)(void (*)(const char*))>(
            plugin::test::symbol(module_, "supple_fixture_set_observer"));
        if (set) set(nullptr);
        plugin::test::close(module_);
    }
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;

    std::vector<Sighting> take() {
        const std::size_t n = std::min(gCount.exchange(0), kSightings);
        return std::vector<Sighting>(gSightings, gSightings + n);
    }

  private:
    void* module_;
};

// The sightings of one callback.
inline std::vector<Sighting> phase(const std::vector<Sighting>& all, const char* name) {
    std::vector<Sighting> found;
    for (const auto& s : all)
        if (s.phase && std::strcmp(s.phase, name) == 0) found.push_back(s);
    return found;
}

// Whether every sighting of `name` has exactly one word open, of this entry kind on this word
// of some cell. At least one sighting must exist.
inline bool onlyOpen(const std::vector<Sighting>& all, const char* name, diagctx::Entry kind,
                     bool audio) {
    const auto found = phase(all, name);
    if (found.empty()) return false;
    for (const auto& s : found)
        if (s.count != 1 || s.more != 0 || s.open[0].word.kind != kind || s.open[0].audio != audio)
            return false;
    return true;
}

}  // namespace supple::scopeprobe
