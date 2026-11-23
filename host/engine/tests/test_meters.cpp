// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The meter bank and its rings (engine.md §2.3, P5.3c): the numbers the GUI
// meters read. The ring itself is the shared SpscRing; the bank adds the slot
// table, the heard-position rule and the drop counters. The threaded case runs
// under TSan in the tsan preset.
#include <doctest/doctest.h>

#include <atomic>
#include <limits>
#include <thread>
#include <vector>

#include "meters.h"

using namespace supple;
using namespace supple::engine;

namespace {

constexpr std::int64_t kHeardEverything = std::numeric_limits<std::int64_t>::max();

model::Id track(std::uint32_t n) { return model::Id{model::Kind::Track, n}; }

// A block of `frames` samples whose peak is `peak` (sumSq = peak²·frames, as a
// real measurement of a constant level would be).
MeterEntry block(std::uint16_t slot, std::uint64_t epoch, std::int64_t stream, std::uint32_t frames,
                 float peak) {
    MeterEntry e;
    e.slot = slot;
    e.generation = 1;
    e.epoch = epoch;
    e.stream = stream;
    e.frames = frames;
    e.peak[0] = e.peak[1] = peak;
    e.sumSq[0] = e.sumSq[1] = peak * peak * static_cast<float>(frames);
    return e;
}

const MeterReading* find(const std::vector<MeterReading>& readings, model::Id id) {
    for (const MeterReading& r : readings)
        if (r.track == id) return &r;
    return nullptr;
}

}  // namespace

TEST_CASE("a full meter ring drops entries and counts them") {
    auto owned = std::make_unique<MeterBank>();  // too large for Windows' 1 MB stack
    MeterBank& bank = *owned;
    bank.beginPlan();
    const std::uint16_t slot = bank.slotFor(track(1));
    REQUIRE(slot != MeterBank::kNoSlot);
    bank.endPlan();

    std::size_t accepted = 0;
    for (std::size_t i = 0; i < MeterBank::kRingCapacity + 8; ++i)
        accepted +=
            bank.pushRenderer(block(slot, 1, static_cast<std::int64_t>(i), 1, 0.5f)) ? 1 : 0;
    CHECK(accepted == MeterBank::kRingCapacity);
    CHECK(bank.droppedRenderer() == 8);
    CHECK(bank.droppedCallback() == 0);

    // The consumer reports what the ring kept.
    const auto readings = bank.read(1, kHeardEverything);
    const MeterReading* r = find(readings, track(1));
    REQUIRE(r != nullptr);
    CHECK(static_cast<double>(r->peak[0]) == doctest::Approx(0.5));
    CHECK(static_cast<double>(r->rms[0]) == doctest::Approx(0.5));
}

TEST_CASE("the meter ring wraps under a producer and a consumer thread") {
    // The ring the bank uses: push/pop across the wrap, from two threads, with
    // every item arriving exactly once and in order (TSan checks the orderings).
    SpscRing<MeterEntry, 4096> ring;
    constexpr std::uint64_t kTotal = 100000;
    std::atomic<std::uint64_t> pushed{0}, dropped{0};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            MeterEntry e;
            e.stream = static_cast<std::int64_t>(i);  // the payload: the push order
            while (!ring.push(e)) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            pushed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::uint64_t seen = 0;
    MeterEntry e;
    while (seen < kTotal) {
        if (ring.pop(e)) {
            REQUIRE(e.stream == static_cast<std::int64_t>(seen));
            ++seen;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(pushed.load() == kTotal);
    CHECK(seen == kTotal);  // every item arrived exactly once, in order
    // `dropped` counts the producer's retries while the ring was full; how many
    // there are is up to the scheduler (a sanitizer run makes the consumer much
    // slower), so only the single-threaded case asserts an exact count.
}

TEST_CASE("a slot a plan drops is reused only after another plan has been compiled") {
    auto owned = std::make_unique<MeterBank>();  // too large for Windows' 1 MB stack
    MeterBank& bank = *owned;
    bank.beginPlan();  // plan 1: tracks 1 and 2
    const std::uint16_t a = bank.slotFor(track(1));
    const std::uint16_t b = bank.slotFor(track(2));
    CHECK(a != MeterBank::kNoSlot);
    CHECK(a != b);
    CHECK(bank.slotFor(track(1)) == a);  // stable within one plan
    bank.endPlan();

    // Plan 2 drops track 1 (its slot retires) and brings track 3: the retired
    // slot must not be handed to a new track yet — the plan that dropped it is
    // still playing while this one is compiled.
    bank.beginPlan();
    CHECK(bank.slotFor(track(2)) == b);
    const std::uint16_t c = bank.slotFor(track(3));
    CHECK(c != a);
    CHECK(c != b);
    bank.endPlan();

    // Plan 3 still cannot use it: the plan that dropped it must have been
    // adopted first. Plan 4 can.
    bank.beginPlan();
    CHECK(bank.slotFor(track(2)) == b);
    const std::uint16_t d = bank.slotFor(track(4));
    CHECK(d != a);
    bank.endPlan();
    bank.beginPlan();
    CHECK(bank.slotFor(track(2)) == b);  // track 2 keeps its slot throughout
    CHECK(bank.slotFor(track(5)) == a);  // track 1's slot is reusable now
    bank.endPlan();
}

TEST_CASE("a meter reading covers only the audio heard since the last call") {
    auto owned = std::make_unique<MeterBank>();  // too large for Windows' 1 MB stack
    MeterBank& bank = *owned;
    bank.beginPlan();
    const std::uint16_t sine = bank.slotFor(track(1));
    const std::uint16_t quiet = bank.slotFor(track(2));
    bank.endPlan();
    REQUIRE(sine != MeterBank::kNoSlot);

    // Four 100-frame blocks of a 0.5 sine, four of silence on the other strip.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(bank.pushRenderer(block(sine, 7, i * 100, 100, 0.5f)));
        REQUIRE(bank.pushRenderer(block(quiet, 7, i * 100, 100, 0.0f)));
    }

    // Nothing has been played yet: nothing is reported, and nothing is lost.
    auto readings = bank.read(7, 0);
    REQUIRE(readings.size() == 2);
    CHECK(find(readings, track(1))->peak[0] == 0.0f);
    CHECK(find(readings, track(2))->peak[0] == 0.0f);

    // Two and a half blocks in: only the first two blocks count.
    readings = bank.read(7, 250);
    CHECK(static_cast<double>(find(readings, track(1))->peak[0]) == doctest::Approx(0.5));
    CHECK(static_cast<double>(find(readings, track(1))->rms[0]) == doctest::Approx(0.5));
    CHECK(find(readings, track(2))->peak[0] == 0.0f);
    CHECK(find(readings, track(2))->rms[0] == 0.0f);

    // The rest arrives later; this call reports only what came since.
    readings = bank.read(7, 400);
    CHECK(static_cast<double>(find(readings, track(1))->peak[0]) == doctest::Approx(0.5));
    CHECK(static_cast<double>(find(readings, track(1))->rms[0]) == doctest::Approx(0.5));

    // Nothing left: zeros, not a stale peak.
    readings = bank.read(7, 1000);
    REQUIRE(readings.size() == 2);
    CHECK(find(readings, track(1))->peak[0] == 0.0f);
    CHECK(find(readings, track(1))->rms[0] == 0.0f);
    CHECK(bank.pendingEntries() == 0);
}

TEST_CASE("meter entries of an older epoch are dropped, newer ones wait") {
    auto owned = std::make_unique<MeterBank>();  // too large for Windows' 1 MB stack
    MeterBank& bank = *owned;
    bank.beginPlan();
    const std::uint16_t slot = bank.slotFor(track(1));
    bank.endPlan();

    REQUIRE(bank.pushRenderer(block(slot, 1, 0, 100, 0.9f)));  // a seek flushed this
    REQUIRE(bank.pushRenderer(block(slot, 2, 0, 100, 0.4f)));  // heard now
    REQUIRE(bank.pushRenderer(block(slot, 3, 0, 100, 0.8f)));  // the callback is not there yet

    const auto readings = bank.read(2, 100);
    const MeterReading* r = find(readings, track(1));
    REQUIRE(r != nullptr);
    CHECK(static_cast<double>(r->peak[0]) ==
          doctest::Approx(0.4));  // not the 0.9 of the flushed epoch

    // The newer epoch's entry waits for the callback to reach it.
    CHECK(bank.pendingEntries() == 1);
    const auto later = bank.read(3, 100);
    CHECK(static_cast<double>(find(later, track(1))->peak[0]) == doctest::Approx(0.8));
    CHECK(bank.pendingEntries() == 0);
}

TEST_CASE("a strip the newest plan no longer has is not reported") {
    auto owned = std::make_unique<MeterBank>();  // too large for Windows' 1 MB stack
    MeterBank& bank = *owned;
    bank.beginPlan();
    const std::uint16_t slot = bank.slotFor(track(1));
    bank.endPlan();
    REQUIRE(bank.pushRenderer(block(slot, 1, 0, 100, 0.5f)));
    CHECK(bank.read(1, kHeardEverything).size() == 1);

    bank.beginPlan();  // a plan without track 1
    bank.endPlan();
    CHECK(bank.read(1, kHeardEverything).empty());
    CHECK(bank.pendingEntries() == 0);
}

TEST_CASE("rapid plans reject queued entries from a reused meter generation") {
    auto owned = std::make_unique<MeterBank>();  // too large for Windows' 1 MB stack
    MeterBank& bank = *owned;
    bank.beginPlan();
    const auto oldSlot = bank.slotFor(track(1));
    bank.endPlan();
    auto old = block(oldSlot, 1, 0, 128, 1);
    old.generation = bank.generationFor(oldSlot);
    REQUIRE(bank.pushRenderer(old));
    for (int i = 0; i < 2; ++i) {
        bank.beginPlan();
        bank.endPlan();
    }
    bank.beginPlan();
    const auto slot = bank.slotFor(track(2));
    bank.endPlan();
    REQUIRE(slot == oldSlot);
    REQUIRE(bank.generationFor(slot) != old.generation);
    REQUIRE(bank.pushCallback(old));  // an executor can still have the old plan
    auto reading = bank.read(1, kHeardEverything);
    REQUIRE(find(reading, track(2)));
    CHECK(find(reading, track(2))->peak[0] == 0);
    auto current = block(slot, 1, 0, 128, .25f);
    current.generation = bank.generationFor(slot);
    REQUIRE(bank.pushRenderer(current));
    reading = bank.read(1, kHeardEverything);
    CHECK(find(reading, track(2))->peak[0] == .25f);
}

TEST_CASE("meter activity arms once and rechecks publication without a periodic wake") {
    auto bank = std::make_unique<MeterBank>();
    const auto observed = bank->activitySequence();
    bank->armActivity(observed);
    REQUIRE(bank->pushCallback(block(0, 1, 0, 64, 0)));
    CHECK(bank->activityArmed());
    REQUIRE(bank->pushCallback(block(0, 1, 64, 64, 0.5f)));
    CHECK_FALSE(bank->activityArmed());
    const auto next = bank->activitySequence();
    REQUIRE(bank->pushCallback(block(0, 1, 128, 64, 0.5f)));
    CHECK_FALSE(bank->activityArmed());  // frame deadlines own ongoing motion
    bank->armActivity(next);             // activity arrived between the read and this arm
    CHECK_FALSE(bank->activityArmed());
    bank->armActivity(bank->activitySequence());
    CHECK(bank->activityArmed());
}

TEST_CASE("meter activity below a subscriber's display floor stays armed") {
    auto bank = std::make_unique<MeterBank>();
    const auto sequence = bank->activitySequence();
    bank->armActivity(sequence, 0.001f);  // the GUI's -60 dB floor
    REQUIRE(bank->pushCallback(block(0, 1, 0, 64, 0.0001f)));
    CHECK(bank->activityArmed());
    CHECK(bank->activitySequence() == sequence);
    REQUIRE(bank->pushRenderer(block(0, 1, 64, 64, 0.01f)));
    CHECK_FALSE(bank->activityArmed());
    CHECK(bank->activitySequence() == sequence + 1);
}
