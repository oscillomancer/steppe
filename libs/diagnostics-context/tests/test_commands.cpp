// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "supple/diagctx/commands.h"
#include "supple/diagctx/inventory.h"
#include "supple/diagctx/scope.h"

using namespace supple::diagctx;

namespace {
struct Fresh {
    Fresh() { detail::resetCommandsForTests(); }
    ~Fresh() { detail::resetCommandsForTests(); }
};
}  // namespace

TEST_CASE("command names are a closed alphabet and length") {
    CHECK(validCommandName("track.set_volume"));
    CHECK(validCommandName("a"));
    CHECK(validCommandName("item.apply-fx"));
    CHECK(validCommandName(std::string(kMaxCommandName, 'a')));
    CHECK_FALSE(validCommandName(""));
    CHECK_FALSE(validCommandName(std::string(kMaxCommandName + 1, 'a')));
    CHECK_FALSE(validCommandName("Track.Set"));
    CHECK_FALSE(validCommandName("a b"));
    CHECK_FALSE(validCommandName("a/b"));
    CHECK_FALSE(validCommandName(std::string("a\0b", 3)));
    CHECK_FALSE(validCommandName("\xc3\xa9"));
    CHECK(commandOriginName(CommandOrigin::Script) == std::string("script"));
    CHECK(commandOriginName(CommandOrigin::None) == nullptr);
    CHECK(commandStateName(CommandState::Ended) == std::string("ended"));
    CHECK(commandStateName(CommandState::Count_) == nullptr);
}

TEST_CASE("a command is recorded when it begins and marked when it ends") {
    Fresh fresh;
    CHECK(claimCommands());
    CHECK_FALSE(claimCommands());  // one dispatcher per process
    {
        CommandScope outer(true, 7, CommandOrigin::Mcp);
        auto during = readCommands();
        REQUIRE(during.count == 1);
        CHECK(during.crumbs[0].seq == 1);
        CHECK(during.crumbs[0].nameId == 7);
        CHECK(during.crumbs[0].origin == CommandOrigin::Mcp);
        CHECK(during.crumbs[0].state == CommandState::Begun);
        {
            CommandScope inner(true, 9, CommandOrigin::Script);  // a step of a batch or script
            CHECK(readCommands().count == 2);
        }
        auto nested = readCommands();
        REQUIRE(nested.count == 2);  // in order, the inner one ended and the outer still open
        CHECK(nested.crumbs[0].nameId == 7);
        CHECK(nested.crumbs[0].state == CommandState::Begun);
        CHECK(nested.crumbs[1].nameId == 9);
        CHECK(nested.crumbs[1].state == CommandState::Ended);
    }
    auto after = readCommands();
    REQUIRE(after.count == 2);
    CHECK(after.crumbs[0].state == CommandState::Ended);
    CHECK(after.damaged == 0);
}

TEST_CASE("an inactive scope records nothing") {
    Fresh fresh;
    {
        CommandScope off(false, 1, CommandOrigin::Ui);
    }
    CHECK(readCommands().count == 0);
    CHECK(table().header.commandHead.load() == 0);
}

TEST_CASE("the ring keeps the last 64 commands in order") {
    Fresh fresh;
    for (std::uint32_t i = 1; i <= 300; ++i) CommandScope(true, i, CommandOrigin::Script);
    const auto read = readCommands();
    REQUIRE(read.count == kMaxCommandEntries);
    CHECK(read.damaged == 0);
    for (std::uint32_t i = 0; i < read.count; ++i) {
        CHECK(read.crumbs[i].seq == 300 - 63 + i);
        CHECK(read.crumbs[i].nameId == 300 - 63 + i);
        CHECK(read.crumbs[i].state == CommandState::Ended);
    }
}

TEST_CASE("a command that outlives its entry leaves the overwriting entries alone") {
    Fresh fresh;
    const auto first = beginCommand(1, CommandOrigin::Rpc);
    for (std::uint32_t i = 0; i < kMaxCommandEntries; ++i)
        CommandScope(true, 100 + i, CommandOrigin::Script);
    endCommand(first);  // its slot now holds another command: not touched
    const auto read = readCommands();
    REQUIRE(read.count == kMaxCommandEntries);
    for (std::uint32_t i = 0; i < read.count; ++i) CHECK(read.crumbs[i].nameId >= 100);
    CHECK(read.crumbs[0].state == CommandState::Ended);
    endCommand(0);  // no entry: nothing happens
}

TEST_CASE("only the thread that began the first command may write the ring") {
    Fresh fresh;
    CHECK(beginCommand(1, CommandOrigin::Ui) == 1);
    std::uint64_t other = 99;
    std::thread([&] { other = beginCommand(2, CommandOrigin::Ui); }).join();
    CHECK(other == 0);  // refused
    CHECK(table().header.commandsDropped.load() == 1);
    std::thread([&] {
        endCommand(1);
    }).join();  // ending needs no ownership and changes only its own entry
    const auto read = readCommands();
    REQUIRE(read.count == 1);
    CHECK(read.crumbs[0].nameId == 1);
    CHECK(beginCommand(3, CommandOrigin::Ui) == 2);  // the owner goes on
}

TEST_CASE("the reader keeps only entries that agree with the ring") {
    std::array<TableImage::CommandEntry, kMaxCommandEntries> ring{};
    const auto set = [&](std::uint64_t seq, std::uint32_t id, std::uint8_t origin,
                         std::uint8_t state, std::uint64_t extra = 0) {
        ring[seq % kMaxCommandEntries] = {
            seq, id | std::uint64_t{origin} << 32 | std::uint64_t{state} << 40 | extra};
    };
    for (std::uint64_t seq = 1; seq <= 10; ++seq) set(seq, 5, 1, 2);
    CHECK(readCommands(10, ring).count == 10);
    CHECK(readCommands(10, ring).damaged == 0);
    // The writer stopped after labelling entry 11 and before counting it: accepted.
    set(11, 6, 3, 1);
    CHECK(readCommands(10, ring).count == 11);
    // An entry further ahead than that is not one the writer could have made.
    set(12, 6, 3, 1);
    auto ahead = readCommands(10, ring);
    CHECK(ahead.count == 11);
    CHECK(ahead.damaged == 1);
    ring[12 % kMaxCommandEntries] = {};
    // Wrong slot, enum out of range, stale, extra bits.
    auto damaged = ring;
    damaged[20] = {13,
                   1 | std::uint64_t{1} << 32 | std::uint64_t{1} << 40};  // 13 belongs in slot 13
    damaged[21] = {21, 1 | std::uint64_t{99} << 32 | std::uint64_t{1} << 40};
    damaged[22] = {22, 1 | std::uint64_t{1} << 32 | std::uint64_t{0} << 40};
    damaged[23] = {23,
                   1 | std::uint64_t{1} << 32 | std::uint64_t{1} << 40 | std::uint64_t{1} << 48};
    damaged[24] = {24 - 64 + 64, 0};  // seq 24 > head + 1
    const auto read = readCommands(10, damaged);
    CHECK(read.count == 11);
    CHECK(read.damaged == 5);
    for (std::uint32_t i = 1; i < read.count; ++i)
        CHECK(read.crumbs[i].seq > read.crumbs[i - 1].seq);
    // Stale: older than the ring can hold.
    std::array<TableImage::CommandEntry, kMaxCommandEntries> old{};
    old[5] = {5, 1 | std::uint64_t{1} << 32 | std::uint64_t{1} << 40};
    CHECK(readCommands(200, old).count == 0);
    CHECK(readCommands(200, old).damaged == 1);
    // Every byte of every field replaced: the reader never reports an entry it cannot vouch for.
    for (std::size_t slot = 0; slot < kMaxCommandEntries; ++slot)
        for (int byte = 0; byte < 16; ++byte)
            for (const std::uint8_t value : {0x00, 0x01, 0x7f, 0xff}) {
                auto bad = ring;
                auto* word = byte < 8 ? &bad[slot].first : &bad[slot].second;
                const int shift = (byte % 8) * 8;
                *word = (*word & ~(std::uint64_t{0xff} << shift)) | std::uint64_t{value} << shift;
                const auto r = readCommands(10, bad);
                CHECK(r.count <= 12);
                for (std::uint32_t i = 0; i < r.count; ++i) {
                    CHECK(r.crumbs[i].seq >= 1);
                    CHECK(r.crumbs[i].seq <= 11);
                    CHECK(r.crumbs[i].seq % kMaxCommandEntries ==
                          r.crumbs[i].seq % kMaxCommandEntries);
                    CHECK(commandOriginName(r.crumbs[i].origin) != nullptr);
                    CHECK(commandStateName(r.crumbs[i].state) != nullptr);
                }
            }
}

namespace {
std::vector<std::string> gNames;
std::vector<std::uint32_t> gFirst;
void collect(const Event& event) noexcept {
    if (event.event != EventKind::Names) return;
    gFirst.push_back(event.cell);
    for (const auto field :
         {event.identity.name, event.identity.vendor, event.identity.version, event.identity.id})
        gNames.emplace_back(field);
}
}  // namespace

TEST_CASE("the name table goes out four to a chunk and skips invalid names") {
    gNames.clear();
    gFirst.clear();
    setInventorySink(&collect);
    const std::vector<std::string_view> names{"a.one", "a.two", "Bad Name", "a.four", "",
                                              "b.x",   "b.y",   "b.z",      "c.1"};
    announceCommandNames(names);
    setInventorySink(nullptr);
    REQUIRE(gFirst.size() == 3);
    CHECK(gFirst == std::vector<std::uint32_t>{0, 4, 8});
    CHECK(gNames[0] == "a.one");
    CHECK(gNames[2].empty());  // not a valid name: left out
    CHECK(gNames[3] == "a.four");
    CHECK(gNames[4].empty());
    CHECK(gNames[5] == "b.x");
    CHECK(gNames[8] == "c.1");
    // With no sink nothing is sent and nothing breaks.
    announceCommandNames(names);
}

TEST_CASE("the image announced for a cell is kept for a handler to read, sink or none") {
    CHECK(announcedImage(kMaxInstances) == 0);  // out of range: none, never a read past the array
    CHECK(announcedImage(kNoCell) == 0);
    setInventorySink(nullptr);
    CHECK(announcedImage(7) == 0);
    announce(7, InstanceKind::Clap, InstanceOrigin::Foreign, InstanceMode::InProcess,
             InstanceState::Active, 0x1234, Identity{});
    CHECK(announcedImage(7) == 0x1234);  // recorded although nobody was listening
    announceRemove(7);
    CHECK(announcedImage(7) == 0x1234);  // a retired instance's module is still named
    announce(7, InstanceKind::Clap, InstanceOrigin::Foreign, InstanceMode::InProcess,
             InstanceState::Active, 0x5678, Identity{});
    CHECK(announcedImage(7) == 0x5678);  // the cell's next occupant replaces it
    announce(kMaxInstances, InstanceKind::Clap, InstanceOrigin::Foreign, InstanceMode::InProcess,
             InstanceState::Active, 0x9, Identity{});  // ignored, and harmless
    announce(7, InstanceKind::Clap, InstanceOrigin::Foreign, InstanceMode::InProcess,
             InstanceState::Active, 0, Identity{});
    CHECK(announcedImage(7) == 0);
}
