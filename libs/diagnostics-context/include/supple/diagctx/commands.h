// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Command breadcrumbs (ADR-0083, topic 5d): the last commands the model thread began.
//
// The command dispatcher writes one entry into the execution table's ring of 64 when a
// registered command begins and marks it when the command ends, so a crash report can say
// which commands were in flight or had just finished. An entry holds three integers and no
// string: the command's id (an index into the name table the process announced once, when
// its registry was built), the category of its origin, and whether it began or ended. Not
// recorded, ever: arguments, results, error text, labels, paths, script text, client or agent
// names, durations. A name the dispatcher never registered never enters, so a caller cannot
// put text of its own into a report through here.
//
// One writer: the model thread. The first thread to begin a command owns the ring; a
// command begun on another thread is not recorded and is counted in the table's
// `commandsDropped`. Process-wide, one dispatcher owns the ring (`claimCommands`): a second
// registry's ids would mean other names than the table the supervisor holds.
//
// Writing is a few relaxed and release stores on the model thread: no lock, no call, no
// allocation, no message. Reading is for the supervisor after the stop phase, from a copy of
// the table; everything read is untrusted and `readCommands` keeps only entries that agree
// with the ring's own sequence.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "supple/diagctx/table.h"

namespace supple::diagctx {

enum class CommandOrigin : std::uint8_t {
    None = 0,
    Ui,
    Script,
    Mcp,
    Rpc,
    Test,
    Internal,
    Agent,
    Count_
};
enum class CommandState : std::uint8_t { None = 0, Begun, Ended, Count_ };
constexpr std::uint8_t kCommandOriginCount = static_cast<std::uint8_t>(CommandOrigin::Count_);
constexpr std::uint8_t kCommandStateCount = static_cast<std::uint8_t>(CommandState::Count_);
// The report strings ("ui", "script", "begun", ...); nullptr for None or out of range.
const char* commandOriginName(CommandOrigin origin) noexcept;
const char* commandStateName(CommandState state) noexcept;

// The name table: ids are 0 .. kMaxCommandNames - 1, each name at most kMaxCommandName bytes
// of [a-z0-9_.-]. A longer name or a later id is not announced (its entries read "unknown").
constexpr std::size_t kMaxCommandNames = 4096, kMaxCommandName = 64;
constexpr std::size_t kNamesPerChunk = 4;  // names in one announcement
// Whether `name` may be in the table: 1 to kMaxCommandName bytes of [a-z0-9_.-].
bool validCommandName(std::string_view name) noexcept;

// ---- writer (model thread) ----------------------------------------------------------------

// The process's one dispatcher claims the ring, once, while cold; later claims are refused.
bool claimCommands() noexcept;
// Sends the process's command names to the supervisor (through the inventory sink, so a
// process without one sends nothing), four to a chunk. `names[id]` is the name of id `id`;
// an empty or invalid name is skipped. Cold; may wait briefly for a slow supervisor, once.
void announceCommandNames(std::span<const std::string_view> names) noexcept;

// Records that command `nameId` begins; returns its sequence number, or 0 when the entry was
// not recorded (another thread owns the ring).
std::uint64_t beginCommand(std::uint32_t nameId, CommandOrigin origin) noexcept;
// Marks the entry of `seq` ended, if the ring still holds it (a command that nests more than
// 64 deep or wide overwrites its own entry, which is then simply left as it was).
void endCommand(std::uint64_t seq) noexcept;

// begin on construction, end on destruction; does nothing when `active` is false.
class CommandScope {
  public:
    CommandScope(bool active, std::uint32_t nameId, CommandOrigin origin) noexcept
        : seq_(active ? beginCommand(nameId, origin) : 0) {}
    ~CommandScope() {
        if (seq_ != 0) endCommand(seq_);
    }
    CommandScope(const CommandScope&) = delete;
    CommandScope& operator=(const CommandScope&) = delete;

  private:
    std::uint64_t seq_;
};

// ---- reader (supervisor, and the macOS worker filling its capsule) ------------------------

struct CommandCrumb {
    std::uint64_t seq = 0;
    std::uint32_t nameId = 0;
    CommandOrigin origin = CommandOrigin::None;
    CommandState state = CommandState::None;
};
struct CommandsRead {
    std::array<CommandCrumb, kMaxCommandEntries> crumbs;  // by ascending seq
    std::uint32_t count = 0;
    std::uint32_t damaged = 0;  // used slots that disagree with the ring's sequence or enums
};
// The entries of a copy of the table's ring. `head` is the sequence of the last entry the
// writer finished; an entry one past it (the writer was stopped between its two stores) is
// accepted. An entry is kept only if its slot is where its sequence belongs, it is within the
// last 64, and its enums are in range; anything else used is counted in `damaged`.
CommandsRead readCommands(
    std::uint64_t head,
    const std::array<TableImage::CommandEntry, kMaxCommandEntries>& entries) noexcept;
CommandsRead readCommands(const TableImage& image) noexcept;
// The live table (async-signal-safe: atomic loads only).
CommandsRead readCommands() noexcept;

namespace detail {
// Tests only: forgets the claim and the owner and clears the ring.
void resetCommandsForTests() noexcept;
}  // namespace detail

}  // namespace supple::diagctx
