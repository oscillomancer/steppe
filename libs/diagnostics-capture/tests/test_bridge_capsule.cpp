// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The supervisor's reader of a bridge's own capsule (CTC5, topic 6c.4): `parseBridgeCapsule`. The
// bytes come from a process that runs third-party code, so every case here is a capsule a
// handler would never write, and each must yield nothing; the good ones prove what the parser
// makes of a real shape, including the threads it lists from the table copy.
#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include "supple/diagcap/selfcapture.h"
#include "supple/diagcap/trace.h"
#include "supple/diagctx/scope.h"
#include "supple/diagctx/table.h"
using namespace supple::diagnostics::detail;
namespace sc = supple::diagnostics::selfcapture;
namespace diagctx = supple::diagctx;
namespace {
constexpr std::uint64_t kBase = 0x7f0000000000ULL;
// A capsule as a bridge's handler writes it, in parts a case can change before it is serialized.
struct Build {
    CapsuleHeader header{};
    std::vector<sc::BridgeModule> modules;
    struct Thread {
        CapsuleThread head{};
        std::vector<std::uint64_t> frames;
    };
    std::vector<Thread> threads;
    std::unique_ptr<diagctx::TableImage> table;
    std::vector<std::uint8_t> extra;  // bytes after the table
    bool omitTable = false;
};
sc::BridgeModule image(std::uint64_t start, std::uint64_t end, std::uint32_t idLength,
                       std::uint8_t first = 0xa0) {
    sc::BridgeModule m{};
    m.bias = 0x1000;
    m.start = start;
    m.end = end;
    m.idLength = idLength;
    for (std::uint32_t i = 0; i < idLength; ++i) m.id[i] = static_cast<std::uint8_t>(first + i);
    return m;
}
void slot(diagctx::TableImage& table, std::size_t at, std::uint64_t id, diagctx::ThreadRole role,
          std::uint16_t ordinal, std::uint64_t low, std::uint64_t high) {
    table.threads[at].id = id;
    table.threads[at].info = static_cast<std::uint64_t>(role) | std::uint64_t{ordinal} << 8;
    table.threads[at].stackLow = low;
    table.threads[at].stackHigh = high;
}
void open(diagctx::TableImage& table, std::uint32_t cell, bool audio, diagctx::Entry kind,
          std::uint16_t generation, std::uint64_t stack) {
    (audio ? table.instances[cell].audio : table.instances[cell].main) =
        diagctx::packWord(kind, generation, stack);
}
// The fault thread (id 100) is in the plug-in's image, inside a Process entry of cell 3 on its
// own stack; two other registered threads exist, one of them inside a Create entry of cell 5.
Build good() {
    Build b;
    b.table = std::make_unique<diagctx::TableImage>();
    std::memset(b.table.get(), 0, sizeof(diagctx::TableImage));
    b.table->header.magic = diagctx::kTableMagic;
    b.table->header.version = diagctx::kTableVersion;
    b.header.magic = sc::kBridgeCapsuleMagic;
    b.header.flags = sc::kCapsuleHasTable;
    b.header.reserved = 100;                                          // the fault thread's id
    b.modules.push_back(image(kBase, kBase + 0x1000, 20));            // the bridge's executable
    b.modules.push_back(image(kBase + 0x10000, kBase + 0x20000, 0));  // an image with no identity
    b.modules.push_back(image(kBase + 0x30000, kBase + 0x40000, 32, 0x10));  // the plug-in
    b.header.moduleCount = 3;
    Build::Thread fault;
    fault.head.role = static_cast<std::uint8_t>(Role::Fault);
    fault.head.missing = static_cast<std::uint8_t>(Missing::None);
    fault.head.registered = static_cast<std::uint8_t>(diagctx::ThreadRole::BridgeAudio);
    fault.head.worker = 1;
    fault.head.entry = static_cast<std::uint8_t>(diagctx::Entry::Process);
    fault.head.entryCell = 3;
    fault.head.entryGeneration = 7;
    fault.frames = {kBase + 0x30100, kBase + 0x30200, kBase + 0x500};
    fault.head.frameCount = 3;
    b.threads.push_back(fault);
    b.header.threadCount = 1;
    slot(*b.table, 0, 99, diagctx::ThreadRole::BridgeControl, 0, 0x7000, 0x8000);
    slot(*b.table, 1, 100, diagctx::ThreadRole::BridgeAudio, 1, 0x9000, 0xa000);
    slot(*b.table, 2, 101, diagctx::ThreadRole::BridgeAudio, 2, 0xb000, 0xc000);
    open(*b.table, 3, true, diagctx::Entry::Process, 7, 0x9800);  // the fault thread's
    open(*b.table, 5, false, diagctx::Entry::Create, 2, 0x7400);  // the control thread's
    return b;
}
std::vector<std::uint8_t> encode(const Build& b) {
    std::vector<std::uint8_t> bytes;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* at = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), at, at + n);
    };
    put(&b.header, sizeof(b.header));
    for (const auto& m : b.modules) put(&m, sizeof(m));
    for (const auto& t : b.threads) {
        put(&t.head, sizeof(t.head));
        put(t.frames.data(), t.frames.size() * sizeof(std::uint64_t));
    }
    if (!b.omitTable) put(b.table.get(), sizeof(diagctx::TableImage));
    bytes.insert(bytes.end(), b.extra.begin(), b.extra.end());
    return bytes;
}
std::optional<TraceSnapshot> parsed(const Build& b) {
    const auto bytes = encode(b);
    return parseBridgeCapsule(bytes.data(), bytes.size());
}
}  // namespace

TEST_CASE("a bridge capsule becomes modules, the fault thread and the table's other threads") {
    const auto trace = parsed(good());
    REQUIRE(trace);
    CHECK(trace->table == TableState::Read);
    // The identities are hex of exactly the bytes the record's length says; none for length 0.
    REQUIRE(trace->modules.size() == 3);
    CHECK(trace->modules[0].id == std::string("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3"));
    CHECK_FALSE(trace->modules[1].id);
    REQUIRE(trace->modules[2].id);
    CHECK(trace->modules[2].id->size() == 64);
    // Fault first (its own slot is not listed again), then the others in the table's order.
    REQUIRE(trace->threads.size() == 3);
    const auto& fault = trace->threads[0];
    CHECK(fault.role == Role::Fault);
    CHECK(fault.missing == Missing::None);
    CHECK(fault.registered == diagctx::ThreadRole::BridgeAudio);
    CHECK(fault.worker == 1);
    CHECK(fault.entry == diagctx::Entry::Process);
    CHECK(fault.entryCell == 3);
    CHECK(fault.entryGeneration == 7);
    // Frames are a module and an offset, never an address; the third is in the first module.
    REQUIRE(fault.walk.frames.size() == 3);
    CHECK(fault.walk.frames[0].module == 2);
    CHECK_FALSE(fault.walk.frames[0].returnAddress);
    CHECK(fault.walk.frames[1].returnAddress);
    CHECK(fault.walk.frames[2].module == 0);
    const auto& control = trace->threads[1];
    CHECK(control.role == Role::Other);
    CHECK(control.missing == Missing::NotCollected);
    CHECK(control.registered == diagctx::ThreadRole::BridgeControl);
    CHECK(control.entry == diagctx::Entry::Create);  // the word on its recorded stack
    CHECK(control.entryCell == 5);
    CHECK(control.entryGeneration == 2);
    const auto& other = trace->threads[2];
    CHECK(other.registered == diagctx::ThreadRole::BridgeAudio);
    CHECK(other.worker == 2);
    CHECK(other.entry == diagctx::Entry::None);
    // Both open words were placed on a thread: nothing is unplaced.
    CHECK(trace->unplacedEntries == 0);
    CHECK(trace->commands.empty());
}

TEST_CASE("an open word no thread's stack holds is counted as unplaced") {
    auto b = good();
    open(*b.table, 9, true, diagctx::Entry::Params, 1, 0x123000);  // on nobody's stack
    open(*b.table, 10, false, diagctx::Entry::Gui, 1, 0);          // no address to place it by
    const auto trace = parsed(b);
    REQUIRE(trace);
    CHECK(trace->unplacedEntries == 2);
}

TEST_CASE("the table's words are the table's: a changed fault word is unplaced, not invented") {
    auto b = good();
    open(*b.table, 3, true, diagctx::Entry::Params, 7, 0x9800);  // no longer the one it read
    const auto trace = parsed(b);
    REQUIRE(trace);
    CHECK(trace->threads[0].entry == diagctx::Entry::Process);  // the capsule's own word
    CHECK(trace->unplacedEntries == 1);
}

TEST_CASE("a table's free, claiming, repeated and unroled slots are not threads") {
    auto b = good();
    slot(*b.table, 3, diagctx::kClaiming, diagctx::ThreadRole::Model, 0, 0, 0);
    slot(*b.table, 4, 101, diagctx::ThreadRole::BridgeAudio, 2, 0xb000, 0xc000);  // a repeat
    slot(*b.table, 5, 102, diagctx::ThreadRole::None, 0, 0, 0);
    b.table->threads[6].id = 103;
    b.table->threads[6].info = 200;  // a role outside the closed set
    const auto trace = parsed(b);
    REQUIRE(trace);
    CHECK(trace->threads.size() == 3);
}

TEST_CASE("the thread count is capped, and what is left out is counted") {
    auto b = good();
    for (std::size_t i = 3; i < diagctx::kMaxThreadSlots; ++i)
        slot(*b.table, i, 1000 + i, diagctx::ThreadRole::BridgeAudio, static_cast<std::uint16_t>(i),
             0, 0);
    const auto trace = parsed(b);
    REQUIRE(trace);
    CHECK(trace->threads.size() == 128);
    CHECK(trace->dropped == 128);  // 255 other registered threads, 127 of them fit
}

TEST_CASE("a capsule of several threads is taken as it is, and lists nothing from the table") {
    auto b = good();
    Build::Thread main;
    main.head.role = static_cast<std::uint8_t>(Role::Main);
    main.head.missing = static_cast<std::uint8_t>(Missing::None);
    main.head.registered = static_cast<std::uint8_t>(diagctx::ThreadRole::BridgeControl);
    main.frames = {kBase + 0x30300};
    main.head.frameCount = 1;
    b.threads.push_back(main);
    b.header.threadCount = 2;
    const auto trace = parsed(b);
    REQUIRE(trace);
    REQUIRE(trace->threads.size() == 2);
    CHECK(trace->threads[0].role == Role::Fault);
    CHECK(trace->threads[1].role == Role::Main);
}

TEST_CASE("a bridge capsule that no handler would write yields nothing") {
    struct Case {
        const char* what;
        void (*change)(Build&);
    };
    const Case cases[] = {
        {"the worker's magic", [](Build& b) { b.header.magic = kCapsuleMagic; }},
        {"no magic", [](Build& b) { b.header.magic = 0; }},
        {"no table flag", [](Build& b) { b.header.flags = 0; }},
        {"an unknown flag", [](Build& b) { b.header.flags |= 8; }},
        {"more modules than the cap", [](Build& b) { b.header.moduleCount = 257; }},
        {"no thread", [](Build& b) { b.header.threadCount = 0; }},
        {"more threads than the cap", [](Build& b) { b.header.threadCount = 129; }},
        {"dropped without its flag", [](Build& b) { b.header.dropped = 1; }},
        {"its flag without dropped", [](Build& b) { b.header.flags |= kCapsuleThreadsDropped; }},
        {"a billion dropped",
         [](Build& b) {
             b.header.dropped = 1U << 21;
             b.header.flags |= kCapsuleThreadsDropped;
         }},
        {"an unplaced count (the parser works it out)", [](Build& b) { b.header.unplaced = 1; }},
        {"a command", [](Build& b) { b.header.commandCount = 1; }},
        {"a damaged command count", [](Build& b) { b.header.commandsDamaged = 1; }},
        {"a refused command count", [](Build& b) { b.header.commandsRefused = 1; }},
        {"a reserved word", [](Build& b) { b.header.reserved2 = 1; }},
        {"a module that ends where it starts",
         [](Build& b) { b.modules[0].end = b.modules[0].start; }},
        {"a module that ends before it starts",
         [](Build& b) { b.modules[0].end = b.modules[0].start - 1; }},
        {"an identity of 33 bytes", [](Build& b) { b.modules[0].idLength = 33; }},
        {"bytes after the identity", [](Build& b) { b.modules[1].id[31] = 1; }},
        {"a module's reserved word", [](Build& b) { b.modules[0].reserved = 1; }},
        {"a first thread that is not the fault thread",
         [](Build& b) { b.threads[0].head.role = static_cast<std::uint8_t>(Role::Other); }},
        {"a role past the set", [](Build& b) { b.threads[0].head.role = 9; }},
        {"a registered role past the set", [](Build& b) { b.threads[0].head.registered = 200; }},
        {"a worker number without a role",
         [](Build& b) {
             b.threads[0].head.registered = 0;
             b.threads[0].head.worker = 4;
         }},
        {"a walked thread without frames",
         [](Build& b) {
             b.threads[0].frames.clear();
             b.threads[0].head.frameCount = 0;
         }},
        {"frames the thread did not walk",
         [](Build& b) {
             b.threads[0].head.missing = static_cast<std::uint8_t>(Missing::Deadline);
         }},
        {"a frame count past the cap", [](Build& b) { b.threads[0].head.frameCount = 129; }},
        {"a stop reason past the set", [](Build& b) { b.threads[0].head.stop = 99; }},
        {"an entry past the set", [](Build& b) { b.threads[0].head.entry = 40; }},
        {"a cell the table does not have", [](Build& b) { b.threads[0].head.entryCell = 1024; }},
        {"a generation a word cannot hold",
         [](Build& b) { b.threads[0].head.entryGeneration = 0x1000; }},
        {"a cell without an entry", [](Build& b) { b.threads[0].head.entry = 0; }},
        {"no table", [](Build& b) { b.omitTable = true; }},
        {"a table that is not one this build wrote", [](Build& b) { b.table->header.magic = 0; }},
        {"a table of another version", [](Build& b) { b.table->header.version = 99; }},
        {"a byte too many", [](Build& b) { b.extra.push_back(0); }},
    };
    for (const auto& c : cases) {
        auto b = good();
        c.change(b);
        INFO(c.what);
        CHECK_FALSE(parsed(b));
    }
}

TEST_CASE("every truncation of a bridge capsule, and a corrupt byte anywhere, is safe") {
    const auto bytes = encode(good());
    REQUIRE(parseBridgeCapsule(bytes.data(), bytes.size()));
    // Any prefix is refused: the table is last and must be whole, and no length but the full one
    // is a capsule. Sampled at the structure's seams and every 997 bytes between them.
    std::vector<std::size_t> cuts;
    for (std::size_t n = 0; n < 600; ++n) cuts.push_back(n);
    for (std::size_t n = 600; n < bytes.size(); n += 997) cuts.push_back(n);
    cuts.push_back(bytes.size() - 1);
    for (const auto n : cuts) CHECK_FALSE(parseBridgeCapsule(bytes.data(), n));
    CHECK_FALSE(parseBridgeCapsule(nullptr, 0));
    CHECK_FALSE(parseBridgeCapsule(bytes.data(), 0));
    // A byte flipped anywhere in the header, the records and the table's head yields a capsule or
    // nothing, and never a count or a name that was not checked (the loop is the evidence that
    // nothing reads out of bounds, which the sanitizer builds make loud).
    for (std::size_t at = 0; at < 800; ++at) {
        auto copy = bytes;
        copy[at] ^= 0xff;
        const auto trace = parseBridgeCapsule(copy.data(), copy.size());
        if (trace) {
            CHECK(trace->threads.size() <= 128);
            CHECK(trace->modules.size() <= 256);
        }
    }
}
