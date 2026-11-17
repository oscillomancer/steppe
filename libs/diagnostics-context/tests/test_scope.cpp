// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bounded.h"
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
        // The recorded address is this function's frame, above every frame of a call from here.
        CHECK(deeper() < inside.stack);
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

// A function that holds a scope and then calls something, as a host entry does. Noinline, so
// it has a frame of its own: the scope's recorded address must be that frame's, whether or not
// the compiler inlines the scope's constructor into it (a Debug build does not).
[[gnu::noinline]] std::uint64_t holdsScope(std::uint32_t cell, std::uint64_t& callee) {
    AudioScope scope(cell, Entry::Process);
    callee = deeper();
    return unpackWord(audioWord(cell)).stack;
}

TEST_CASE("the recorded address is the frame of the function that holds the scope") {
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    // The rule, on every architecture: the address is the function's frame address, which is
    // at or above the function's stack pointer for as long as the scope lives, so every frame
    // of a function it calls lies strictly below it. Where it sits among the function's own
    // locals is the compiler's choice (the arm64 frame record may be above or below them), so
    // nothing here compares it with a local.
    std::uint64_t callee = 0;
    const auto recorded = holdsScope(cell, callee);
    CHECK(recorded != 0);
    CHECK(callee < recorded);
    {
        AudioScope scope(cell, Entry::Process);
        const auto frame = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
        CHECK(unpackWord(audioWord(cell)).stack == (frame & ~std::uint64_t{15}));
    }
    releaseCell(cell);
}

TEST_CASE("a scope in a called function has a lower address than the scope that calls it") {
    const auto cell = acquireCell();
    const auto other = acquireCell();
    REQUIRE(cell != kNoCell);
    REQUIRE(other != kNoCell);
    {
        MainScope outer(cell, Entry::Params);
        std::uint64_t callee = 0;
        const auto inner = holdsScope(other, callee);
        CHECK(inner < unpackWord(mainWord(cell)).stack);
    }
    releaseCell(cell);
    releaseCell(other);
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

TEST_CASE("two threads in main-thread entries of one instance cannot corrupt its word") {
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    const auto& owner = table().instances[cell].mainOwner;
    std::atomic<int> state{0};  // 1 A is inside, 2 B has been and gone, 3 A may leave
    std::uint64_t whileInside = 0, whileBoth = 0;
    std::thread a([&] {
        MainScope scope(cell, Entry::Gui);
        whileInside = mainWord(cell);
        state.store(1);
        testing::waitFor([&] { return state.load() == 3; }, "state == 3");
    });
    testing::waitFor([&] { return state.load() == 1; }, "state == 1");
    CHECK(owner.load() != 0);
    std::thread b([&] {
        {
            MainScope scope(cell, Entry::Params);  // not first: records nothing
            whileBoth = mainWord(cell);
            {
                MainScope nested(cell, Entry::State);  // nor does anything nested in it
                CHECK(mainWord(cell) == whileInside);
            }
        }
        // B has left; A is still inside, and its word is exactly as it was.
        CHECK(mainWord(cell) == whileInside);
        CHECK(owner.load() != 0);
        state.store(2);
    });
    b.join();
    CHECK(whileBoth == whileInside);
    CHECK(unpackWord(mainWord(cell)).kind == Entry::Gui);
    state.store(3);
    a.join();
    // Both are gone: the word is idle and the cell is free of an owner, whichever left first.
    CHECK(unpackWord(mainWord(cell)).kind == Entry::None);
    CHECK(owner.load() == 0);
    // And the next entrant, from either thread, records as usual.
    {
        MainScope scope(cell, Entry::Params);
        CHECK(unpackWord(mainWord(cell)).kind == Entry::Params);
        CHECK(owner.load() != 0);
    }
    CHECK(owner.load() == 0);
    releaseCell(cell);
}

TEST_CASE("a thread's own nested main scopes record, and the audio word has no owner") {
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    {
        MainScope outer(cell, Entry::Params);
        {
            MainScope inner(cell, Entry::MainThread);
            CHECK(unpackWord(mainWord(cell)).kind == Entry::MainThread);
        }
        CHECK(unpackWord(mainWord(cell)).kind == Entry::Params);
    }
    CHECK(table().instances[cell].mainOwner.load() == 0);
    {
        AudioScope audio(cell, Entry::Process);
        CHECK(table().instances[cell].mainOwner.load() == 0);  // audio scopes take no ownership
        CHECK(unpackWord(audioWord(cell)).kind == Entry::Process);
    }
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
            testing::waitFor([&] { return go.load(); }, "go");
            const Entry kind = i % 2 ? Entry::Process : Entry::Reset;
            for (int round = 0; round < 2000; ++round) {
                AudioScope scope(cells[i], kind);
                if (unpackWord(audioWord(cells[i])).kind != kind) wrong.fetch_add(1);
                for (int other = 0; other < kThreads; ++other)
                    if (other != i && unpackWord(mainWord(cells[other])).kind != Entry::None)
                        wrong.fetch_add(1);
            }
        });
    testing::waitFor([&] { return ready.load() >= kThreads; }, "ready >= kThreads");
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

// ---- Overhead, informational (ADR-0083, topic 5b) ------------------------------------------
// What a scope costs on the audio path, measured in an optimized build: alone, and with
// several workers each in a scope on an instance of its own at the same moment. The cells
// each fill a cache line for that reason: measured on the 16-byte cells that came first (four
// to a line), the concurrent figure was 6.0, 5.7 and 9.1 ns for 2, 4 and 8 workers against
// 1.75, 1.74 and 1.84 ns with each cell on its own line. It prints numbers and asserts only
// that the work was done: no wall-clock time decides anything. Run it with SUPPLE_SCOPE_BENCH=1
// in the release preset:
//   SUPPLE_SCOPE_BENCH=1 diagnostics-context.tests -tc="scope overhead*"
namespace {
using Clock = std::chrono::steady_clock;
double nanosecondsPer(Clock::duration time, std::uint64_t count) {
    return std::chrono::duration<double, std::nano>(time).count() / static_cast<double>(count);
}
// Enters and leaves one scope on `cell`, `count` times; returns a sum that depends on every
// pass, so the loop cannot be dropped.
[[gnu::noinline]] std::uint64_t scopes(std::uint32_t cell, std::uint64_t count) {
    std::uint64_t seen = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        AudioScope scope(cell, Entry::Process);
        asm volatile("" ::: "memory");
        seen += static_cast<std::uint64_t>(unpackWord(audioWord(cell)).kind);
    }
    return seen;
}
// The same loop without the scope.
[[gnu::noinline]] std::uint64_t noScopes(std::uint32_t cell, std::uint64_t count) {
    std::uint64_t seen = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        asm volatile("" ::: "memory");
        seen += static_cast<std::uint64_t>(unpackWord(audioWord(cell)).kind);
    }
    return seen;
}
template <class Work>
double concurrently(unsigned workers, std::uint64_t count, Work work) {
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> sums(workers);
    for (unsigned i = 0; i < workers; ++i)
        threads.emplace_back([&, i] {
            ready.fetch_add(1);
            testing::waitFor([&] { return go.load(); }, "go");
            sums[i] = work(i, count);
        });
    testing::waitFor([&] { return ready.load() >= workers; }, "ready >= workers");
    const auto start = Clock::now();
    go.store(true);
    for (auto& thread : threads) thread.join();
    const auto elapsed = Clock::now() - start;
    for (const auto sum : sums) CHECK(sum == count);
    return nanosecondsPer(elapsed, count);  // per scope, one worker's share
}
}  // namespace

TEST_CASE("scope overhead (informational)") {
    if (const char* on = std::getenv("SUPPLE_SCOPE_BENCH"); !on || std::string(on) != "1") return;
    constexpr std::uint64_t kCount = 50'000'000;
    const auto cell = acquireCell();
    REQUIRE(cell != kNoCell);
    (void)scopes(cell, 1'000'000);  // warm up
    auto start = Clock::now();
    const auto without = noScopes(cell, kCount);
    const double base = nanosecondsPer(Clock::now() - start, kCount);
    start = Clock::now();
    const auto with = scopes(cell, kCount);
    const double scoped = nanosecondsPer(Clock::now() - start, kCount);
    // The loop with a scope sees it open on every pass; the loop without sees an idle word.
    CHECK(with == static_cast<std::uint64_t>(Entry::Process) * kCount);
    CHECK(without == 0);
    std::fprintf(stderr,
                 "scope overhead: one thread, %.2f ns per loop with a scope, %.2f ns without; "
                 "the scope adds %.2f ns (one enter and one exit)\n",
                 scoped, base, scoped - base);
    releaseCell(cell);

    // Several workers, each on an instance of its own, processed at the same moment.
    const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
    for (const unsigned workers : {2u, 4u, std::min(8u, hardware)}) {
        std::vector<std::uint32_t> cells;
        for (unsigned i = 0; i < workers; ++i) cells.push_back(acquireCell());
        const double real = concurrently(workers, kCount / 4, [&](unsigned i, std::uint64_t n) {
            return scopes(cells[i], n) / static_cast<std::uint64_t>(Entry::Process);
        });
        std::fprintf(stderr,
                     "scope overhead: %u workers, %.2f ns per scope, one cell to a cache line\n",
                     workers, real);
        for (const auto c : cells) releaseCell(c);
    }
}
