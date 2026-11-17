// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

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
    CHECK(kTableBytes == 18464);
    CHECK(sizeof(Table) == sizeof(TableImage));
    CHECK(offsetof(TableImage, threads) == 32);
    CHECK(offsetof(TableImage, instances) == 32 + 64 * 16);
    CHECK(offsetof(TableImage, commands) == 32 + (64 + 1024) * 16);
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
        while (!release.load()) std::this_thread::yield();
    });
    while (!ready.load()) std::this_thread::yield();
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

TEST_CASE("the table holds 64 threads and counts the ones it cannot hold") {
    const std::uint32_t before = table().header.threadsDropped.load();
    std::atomic<bool> release{false};
    std::atomic<int> started{0}, accepted{0};
    std::vector<std::thread> threads;
    constexpr int kThreads = 70;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&] {
            if (registerThread(ThreadRole::AudioBackground, 1)) accepted.fetch_add(1);
            started.fetch_add(1);
            while (!release.load()) std::this_thread::yield();
        });
    while (started.load() < kThreads) std::this_thread::yield();
    const auto image = copyOut();
    std::size_t used = 0;
    for (const auto& slot : image.threads) used += slot.id != 0 ? 1 : 0;
    CHECK(used == kMaxThreadSlots);
    CHECK(accepted.load() == static_cast<int>(kMaxThreadSlots));
    CHECK(table().header.threadsDropped.load() - before == kThreads - kMaxThreadSlots);
    release.store(true);
    for (auto& thread : threads) thread.join();
    std::size_t left = 0;
    for (const auto& slot : copyOut().threads) left += slot.id != 0 ? 1 : 0;
    CHECK(left == 0);
}

TEST_CASE("a hook runs once for each newly registered thread, after its slot is claimed") {
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
