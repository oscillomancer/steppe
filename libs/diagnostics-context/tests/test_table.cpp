// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "bounded.h"
#include "supple/diagctx/table.h"

using namespace supple::diagctx;

namespace {
TableImage copyOut() {
    TableImage image;
    // A plain read of the atomics, as the supervisor's remote read sees them.
    std::memcpy(&image, &table(), sizeof(image));
    return image;
}
}  // namespace

TEST_CASE("the table has the documented size and a stable layout") {
    CHECK(kTableBytes == 74816);
    CHECK(sizeof(Table) == sizeof(TableImage));
    CHECK(kMaxThreadSlots == 256);
    CHECK(offsetof(TableImage, threads) == 32);
    // Each instance cell fills a cache line of its own, so the first one starts on one.
    CHECK(offsetof(TableImage, instances) == 8256);
    CHECK(sizeof(TableImage::InstanceCell) == 64);
    CHECK(alignof(TableImage::InstanceCell) == 64);
    CHECK(offsetof(TableImage, commands) == 8256 + 1024 * 64);
}

TEST_CASE("an untouched or foreign image is not valid") {
    TableImage image{};
    CHECK_FALSE(imageValid(image));
    initTable();
    CHECK(imageValid(copyOut()));
    TableImage wrong = copyOut();
    wrong.header.version += 1;
    CHECK_FALSE(imageValid(wrong));
    wrong = copyOut();
    wrong.header.magic ^= 1;
    CHECK_FALSE(imageValid(wrong));
}

TEST_CASE("role names are a closed set") {
    CHECK(std::strcmp(threadRoleName(ThreadRole::Model), "model") == 0);
    CHECK(std::strcmp(threadRoleName(ThreadRole::AudioRender), "audio_render") == 0);
    CHECK(std::strcmp(threadRoleName(ThreadRole::AudioLive), "audio_live") == 0);
    CHECK(std::strcmp(threadRoleName(ThreadRole::AudioBackground), "audio_background") == 0);
    CHECK(std::strcmp(threadRoleName(ThreadRole::AudioOffline), "audio_offline") == 0);
    CHECK(std::strcmp(threadRoleName(ThreadRole::Recorder), "recorder") == 0);
    CHECK(std::strcmp(threadRoleName(ThreadRole::MidiOutput), "midi_output") == 0);
    CHECK(threadRoleName(ThreadRole::None) == nullptr);
    CHECK(threadRoleName(ThreadRole::Count_) == nullptr);
    CHECK(threadRoleName(static_cast<ThreadRole>(200)) == nullptr);
    CHECK(slotInfo(0).role == ThreadRole::None);
    CHECK(slotInfo(kThreadRoleCount).role == ThreadRole::None);  // out of range
    CHECK(slotInfo(0xff).role == ThreadRole::None);
}

TEST_CASE("a registered thread is found by its id until it exits") {
    std::uint64_t id = 0;
    bool registered = false, again = false;
    std::atomic<bool> release{false}, ready{false};
    std::thread worker([&] {
        id = currentThreadId();
        registered = registerThread(ThreadRole::AudioLive, 3);
        again = registerThread(ThreadRole::Recorder, 9);  // the first role stays
        ready.store(true);
        testing::waitFor([&] { return release.load(); }, "release");
    });
    testing::waitFor([&] { return ready.load(); }, "ready");
    CHECK(registered);
    CHECK(again);
    CHECK(id != 0);
    const auto live = copyOut();
    for (const auto& found : {findThread(live, id), findThread(id)}) {
        CHECK(found.role == ThreadRole::AudioLive);
        CHECK(found.ordinal == 3);
    }
    CHECK(findThread(live, 0).role == ThreadRole::None);
    CHECK(findThread(live, kClaiming).role == ThreadRole::None);
    release.store(true);
    worker.join();
    // The slot is free again, so a new thread that reuses the id is not mislabelled.
    CHECK(findThread(id).role == ThreadRole::None);
    CHECK(findThread(copyOut(), id).role == ThreadRole::None);
}

TEST_CASE("the table holds 256 threads and counts the ones it cannot hold") {
    const std::uint32_t before = table().header.threadsDropped.load();
    static std::atomic<int> hooked;
    hooked = 0;
    setThreadHook([]() noexcept { hooked.fetch_add(1); });
    std::atomic<bool> release{false};
    std::atomic<int> started{0}, accepted{0};
    std::vector<std::thread> threads;
    constexpr int kThreads = static_cast<int>(kMaxThreadSlots) + 6;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&] {
            if (registerThread(ThreadRole::AudioBackground, 1)) accepted.fetch_add(1);
            // A second call on a thread without a slot retries but is not counted again.
            (void)registerThread(ThreadRole::AudioBackground, 1);
            started.fetch_add(1);
            testing::waitFor([&] { return release.load(); }, "release");
        });
    testing::waitFor([&] { return started.load() >= kThreads; }, "started >= kThreads");
    setThreadHook(nullptr);
    const auto image = copyOut();
    std::size_t used = 0;
    for (const auto& slot : image.threads) used += slot.id != 0 ? 1 : 0;
    CHECK(used == kMaxThreadSlots);
    CHECK(accepted.load() == static_cast<int>(kMaxThreadSlots));
    CHECK(table().header.threadsDropped.load() - before == kThreads - kMaxThreadSlots);
    // The hook ran for every thread, the six without a slot included.
    CHECK(hooked.load() == kThreads);
    release.store(true);
    for (auto& thread : threads) thread.join();
    std::size_t left = 0;
    for (const auto& slot : copyOut().threads) left += slot.id != 0 ? 1 : 0;
    CHECK(left == 0);
}

TEST_CASE("a slot records the thread's stack, and an exited thread leaves none behind") {
    std::uint64_t id = 0, local = 0;
    std::atomic<bool> release{false}, ready{false};
    std::thread worker([&] {
        id = currentThreadId();
        // The frame's own address: under AddressSanitizer a local lives on a fake stack in
        // the heap, which is not what the thread recorded.
        local = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
        (void)registerThread(ThreadRole::Model);
        ready.store(true);
        testing::waitFor([&] { return release.load(); }, "release");
    });
    testing::waitFor([&] { return ready.load(); }, "ready");
    const auto found = findThread(copyOut(), id);
    CHECK(found.role == ThreadRole::Model);
    CHECK(found.stackLow != 0);
    CHECK(found.stackLow < found.stackHigh);
    CHECK(local >= found.stackLow);  // a local of the thread lies inside what it recorded
    CHECK(local < found.stackHigh);
    CHECK(findThread(id).stackLow == found.stackLow);
    release.store(true);
    worker.join();
    // The free slot is wiped whole.
    for (const auto& slot : copyOut().threads)
        if (slot.id == 0) CHECK((slot.info | slot.stackLow | slot.stackHigh) == 0);
}

TEST_CASE("a slot being released cannot be claimed over, so the next thread keeps its role") {
    // Thread A leaves a slot and pauses between the two steps of the release; thread B
    // registers meanwhile. B must not end up in A's slot half-wiped: whatever B claims,
    // it must still carry its own role once A has finished.
    static std::atomic<int> state{0};  // 0 idle, 1 A is releasing, 2 B has registered
    state = 0;
    detail::setReleaseHook([]() noexcept {
        state.store(1);
        testing::waitFor([&] { return state.load() == 2; }, "state == 2");
    });
    std::thread a([] { (void)registerThread(ThreadRole::Recorder, 1); });
    std::uint64_t idB = 0;
    std::atomic<bool> release{false}, bRegistered{false};
    std::thread b([&] {
        testing::waitFor([&] { return state.load() == 1; }, "state == 1");
        idB = currentThreadId();
        (void)registerThread(ThreadRole::AudioLive, 2);
        bRegistered.store(true);
        state.store(2);
        testing::waitFor([&] { return release.load(); }, "release");
    });
    a.join();  // A returns once B has registered and its release has finished
    detail::setReleaseHook(nullptr);
    CHECK(bRegistered.load());
    const auto found = findThread(copyOut(), idB);
    CHECK(found.role == ThreadRole::AudioLive);
    CHECK(found.ordinal == 2);
    CHECK(found.stackLow != 0);  // its stack bounds were not wiped either
    release.store(true);
    b.join();
}

TEST_CASE("a hook runs once for each registering thread, after its slot is claimed") {
    static std::atomic<int> calls{0};
    static std::atomic<std::uint64_t> seen{0};
    setThreadHook([]() noexcept {
        calls.fetch_add(1);
        seen.store(findThread(currentThreadId()).role == ThreadRole::Model ? 1 : 2);
    });
    std::thread worker([] {
        (void)registerThread(ThreadRole::Model);
        (void)registerThread(ThreadRole::Model);
    });
    worker.join();
    setThreadHook(nullptr);
    CHECK(calls.load() == 1);
    CHECK(seen.load() == 1);
}

TEST_CASE("an invalid role registers nothing") {
    std::thread worker([] {
        CHECK_FALSE(registerThread(ThreadRole::None));
        CHECK_FALSE(registerThread(ThreadRole::Count_));
        CHECK(findThread(currentThreadId()).role == ThreadRole::None);
    });
    worker.join();
}

// A wait that is never satisfied must end in a failed check naming the wait, not block the run.
// The case is expected to fail, and it ends in about a second.
TEST_CASE("a wait that never comes gives up and says what it waited for" * doctest::should_fail()) {
    std::atomic<bool> never{false};
    const auto start = std::chrono::steady_clock::now();
    const bool done = testing::waitFor([&] { return never.load(); }, "a flag nobody sets",
                                       std::chrono::seconds(1));
    CHECK_FALSE(done);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
    CHECK(testing::waitFor([] { return true; }, "something already true"));
}
