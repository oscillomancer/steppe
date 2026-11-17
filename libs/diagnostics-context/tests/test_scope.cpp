// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "supple/diagctx/scope.h"

using namespace supple::diagctx;

namespace {
std::uint64_t audioWord(std::uint32_t cell) {
    return table().instances[cell].audio.load(std::memory_order_acquire);
}
std::uint64_t mainWord(std::uint32_t cell) {
    return table().instances[cell].main.load(std::memory_order_acquire);
}
// A stack address below the caller's frame.
[[gnu::noinline]] std::uint64_t deeper() {
    return reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
}
// Takes every cell and gives them back when it goes out of scope.
struct AllCells {
    std::vector<std::uint32_t> cells;
    AllCells() {
        for (;;) {
            const auto cell = acquireCell();
            if (cell == kNoCell) break;
            cells.push_back(cell);
        }
    }
    ~AllCells() {
        for (const auto cell : cells) releaseCell(cell);
    }
};
}  // namespace

TEST_CASE("a word packs a kind, a generation and a stack address, and reads back") {
    const std::uint64_t address = 0x7ffd'1234'5670;
    const auto word = packWord(Entry::Process, 0x2a5, address);
    const auto fields = unpackWord(word);
    CHECK(fields.kind == Entry::Process);
    CHECK(fields.generation == 0x2a5);
    CHECK(fields.stack == address);
    // The address is kept to 16 bytes, rounding down.
    CHECK(unpackWord(packWord(Entry::Params, 1, address + 15)).stack == address);
    // The generation keeps its 12 bits only.
    CHECK(unpackWord(packWord(Entry::Params, 0x1abc, address)).generation == 0xabc);
    // An address the word cannot hold is recorded as none, not truncated into a wrong one.
    CHECK(unpackWord(packWord(Entry::Process, 3, kWordStackLimit)).stack == 0);
    CHECK(unpackWord(packWord(Entry::Process, 3, ~std::uint64_t{0})).stack == 0);
    CHECK(unpackWord(packWord(Entry::Process, 3, kWordStackLimit - 16)).stack ==
          kWordStackLimit - 16);
    // An idle word, and a word whose kind is outside the closed set, are both idle.
    CHECK(unpackWord(0).kind == Entry::None);
    CHECK(unpackWord(std::uint64_t{kEntryCount} << kWordKindShift).kind == Entry::None);
    CHECK(unpackWord(~std::uint64_t{0}).kind == Entry::None);
}

TEST_CASE("entry names are a closed set of at most twenty kinds") {
    CHECK(kEntryCount <= 20);
    std::set<std::string> seen;
    for (std::uint8_t k = 1; k < kEntryCount; ++k) {
        const char* name = entryName(static_cast<Entry>(k));
        REQUIRE(name != nullptr);
        CHECK(seen.insert(name).second);  // distinct
    }
    CHECK(std::strcmp(entryName(Entry::Process), "process") == 0);
    CHECK(std::strcmp(entryName(Entry::StartProcessing), "start_processing") == 0);
    CHECK(std::strcmp(entryName(Entry::BridgeCall), "bridge_call") == 0);
    CHECK(entryName(Entry::None) == nullptr);
    CHECK(entryName(Entry::Count_) == nullptr);
    CHECK(entryName(static_cast<Entry>(200)) == nullptr);
}

TEST_CASE("cells are handed out once, come back idle in the next generation, and run out") {
    const auto a = acquireCell();
    const auto b = acquireCell();
    REQUIRE(a != kNoCell);
    REQUIRE(b != kNoCell);
    CHECK(a != b);
    CHECK(unpackWord(audioWord(a)).kind == Entry::None);
    CHECK(unpackWord(mainWord(a)).kind == Entry::None);
    const auto first = unpackWord(audioWord(a)).generation;
    CHECK(unpackWord(mainWord(a)).generation == first);
    releaseCell(a);
    // The lowest free cell is handed out first, so a gets its cell back, one generation on.
    const auto again = acquireCell();
    CHECK(again == a);
    CHECK(unpackWord(audioWord(again)).generation == ((first + 1) & kGenerationMask));
    CHECK(unpackWord(mainWord(again)).generation == ((first + 1) & kGenerationMask));
    releaseCell(again);
    releaseCell(b);
    releaseCell(kNoCell);  // ignored
    releaseCell(kMaxInstances + 5);

    const auto before = table().header.untracked.load();
    {
        AllCells all;
        CHECK(all.cells.size() == kMaxInstances);
        CHECK(table().header.untracked.load() - before == 1);  // the call that found none
        CHECK(std::set<std::uint32_t>(all.cells.begin(), all.cells.end()).size() == kMaxInstances);
        CHECK(acquireCell() == kNoCell);
        CHECK(table().header.untracked.load() - before == 2);
    }
    const auto after = acquireCell();  // everything was given back
    CHECK(after != kNoCell);
    releaseCell(after);
}

TEST_CASE("the generation wraps after 4096 reuses of a cell") {
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    const auto start = unpackWord(audioWord(cell)).generation;
    releaseCell(cell);
    std::uint32_t again = kNoCell;
    for (int i = 0; i < 4096; ++i) {
        again = acquireCell();
        REQUIRE(again == cell);
        releaseCell(again);
    }
    again = acquireCell();
    CHECK(again == cell);
    CHECK(unpackWord(audioWord(again)).generation == ((start + 4097) & kGenerationMask));
    releaseCell(again);
}

TEST_CASE("a scope marks its word for its length and puts back what it found") {
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    const auto generation = unpackWord(audioWord(cell)).generation;
    {
        AudioScope scope(cell, Entry::Process);
        const auto inside = unpackWord(audioWord(cell));
        CHECK(inside.kind == Entry::Process);
        CHECK(inside.generation == generation);
        CHECK(inside.stack != 0);
        // The recorded address is this frame's, above anything a call from here uses.
        CHECK(deeper() < inside.stack + 16);
        // The other word is its own.
        CHECK(unpackWord(mainWord(cell)).kind == Entry::None);
    }
    CHECK(unpackWord(audioWord(cell)).kind == Entry::None);
    CHECK(unpackWord(audioWord(cell)).generation == generation);
    {
        MainScope scope(cell, Entry::State);
        CHECK(unpackWord(mainWord(cell)).kind == Entry::State);
        CHECK(unpackWord(audioWord(cell)).kind == Entry::None);
    }
    CHECK(unpackWord(mainWord(cell)).kind == Entry::None);
    releaseCell(cell);
}

TEST_CASE("scopes nest and each exit restores the scope outside it") {
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    {
        MainScope outer(cell, Entry::Params);
        const auto outerWord = mainWord(cell);
        {
            MainScope inner(cell, Entry::MainThread);
            const auto innerWord = unpackWord(mainWord(cell));
            CHECK(innerWord.kind == Entry::MainThread);
            // The inner scope is deeper, so its address is the lower one.
            CHECK(innerWord.stack <= unpackWord(outerWord).stack);
            {
                MainScope innermost(cell, Entry::State);
                CHECK(unpackWord(mainWord(cell)).kind == Entry::State);
            }
            CHECK(unpackWord(mainWord(cell)).kind == Entry::MainThread);
        }
        CHECK(mainWord(cell) == outerWord);
        CHECK(unpackWord(mainWord(cell)).kind == Entry::Params);
    }
    CHECK(unpackWord(mainWord(cell)).kind == Entry::None);
    releaseCell(cell);
}

TEST_CASE("a scope on no cell does nothing, and its overflow counter stays still") {
    const auto before = table().header.scopeOverflow.load();
    {
        AudioScope none(kNoCell, Entry::Process);
        MainScope outOfRange(kMaxInstances, Entry::Process);
        MainScope wild(0xdeadbeef, Entry::Process);
    }
    CHECK(table().header.scopeOverflow.load() == before);
}

TEST_CASE("scopes on different cells in different threads do not disturb one another") {
    constexpr int kThreads = 8;
    std::vector<std::uint32_t> cells;
    for (int i = 0; i < kThreads; ++i) cells.push_back(acquireCell());
    std::atomic<int> ready{0}, wrong{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            const Entry kind = i % 2 ? Entry::Process : Entry::Reset;
            for (int round = 0; round < 2000; ++round) {
                AudioScope scope(cells[i], kind);
                if (unpackWord(audioWord(cells[i])).kind != kind) wrong.fetch_add(1);
                for (int other = 0; other < kThreads; ++other)
                    if (other != i && unpackWord(mainWord(cells[other])).kind != Entry::None)
                        wrong.fetch_add(1);
            }
        });
    while (ready.load() < kThreads) std::this_thread::yield();
    go.store(true);
    for (auto& thread : threads) thread.join();
    CHECK(wrong.load() == 0);
    for (const auto cell : cells) {
        CHECK(unpackWord(audioWord(cell)).kind == Entry::None);
        releaseCell(cell);
    }
}

TEST_CASE("a scope's address lies on the stack its thread registered") {
    std::uint64_t low = 0, high = 0, recorded = 0;
    std::thread worker([&] {
        REQUIRE(registerThread(ThreadRole::Model));
        const auto found = findThread(currentThreadId());
        low = found.stackLow;
        high = found.stackHigh;
        const auto cell = acquireCell();
        {
            AudioScope scope(cell, Entry::Process);
            recorded = unpackWord(audioWord(cell)).stack;
        }
        releaseCell(cell);
    });
    worker.join();
    CHECK(low != 0);
    CHECK(recorded >= low);
    CHECK(recorded < high);
}
