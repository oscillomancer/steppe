// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/commands.h"

#include <algorithm>
#include <atomic>

namespace supple::diagctx {
namespace {
std::atomic<bool> gClaimed{false};
std::atomic<std::uintptr_t> gOwner{0};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);

// A value that differs between live threads: the address of a thread-local.
std::uintptr_t threadToken() noexcept {
    thread_local char token;
    return reinterpret_cast<std::uintptr_t>(&token);
}

constexpr std::uint64_t pack(std::uint32_t nameId, CommandOrigin origin,
                             CommandState state) noexcept {
    return static_cast<std::uint64_t>(nameId) | static_cast<std::uint64_t>(origin) << 32 |
           static_cast<std::uint64_t>(state) << 40;
}
}  // namespace

const char* commandOriginName(CommandOrigin origin) noexcept {
    switch (origin) {
        case CommandOrigin::Ui:
            return "ui";
        case CommandOrigin::Script:
            return "script";
        case CommandOrigin::Mcp:
            return "mcp";
        case CommandOrigin::Rpc:
            return "rpc";
        case CommandOrigin::Test:
            return "test";
        case CommandOrigin::Internal:
            return "internal";
        case CommandOrigin::Agent:
            return "agent";
        case CommandOrigin::None:
        case CommandOrigin::Count_:
            break;
    }
    return nullptr;
}
const char* commandStateName(CommandState state) noexcept {
    switch (state) {
        case CommandState::Begun:
            return "begun";
        case CommandState::Ended:
            return "ended";
        case CommandState::None:
        case CommandState::Count_:
            break;
    }
    return nullptr;
}

bool validCommandName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxCommandName) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

bool claimCommands() noexcept { return !gClaimed.exchange(true); }

namespace {
// Whether some thread has registered as the model thread (cold: only a thread that is not the
// ring's owner asks).
bool modelThreadRegistered() noexcept {
    for (const auto& slot : table().threads) {
        const auto id = slot.id.load(std::memory_order_acquire);
        if (id != 0 && id != kClaiming &&
            slotInfo(slot.info.load(std::memory_order_relaxed)).role == ThreadRole::Model)
            return true;
    }
    return false;
}
}  // namespace

std::uint64_t beginCommand(std::uint32_t nameId, CommandOrigin origin) noexcept {
    auto& t = table();
    const auto token = threadToken();
    if (gOwner.load(std::memory_order_relaxed) != token) {
        // The ring belongs to the thread registered as Model. Until one has registered the first
        // thread to begin a command stands in (a program or a test that dispatches off the model
        // thread, as the registration may come later); the Model thread takes the ring from a
        // stand-in, and while one is registered nobody else is given it.
        const bool model = findThread(currentThreadId()).role == ThreadRole::Model;
        if (model) {
            gOwner.store(token, std::memory_order_relaxed);
        } else {
            std::uintptr_t none = 0;
            if (modelThreadRegistered() ||
                !gOwner.compare_exchange_strong(none, token, std::memory_order_relaxed)) {
                t.header.commandsDropped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    // One writer: the sequence is read and written by this thread alone. The slot is emptied,
    // filled and then labelled with its sequence, and only then counted, so a thread stopped
    // between two stores leaves a slot a reader skips or accepts whole.
    const std::uint64_t seq = t.header.commandHead.load(std::memory_order_relaxed) + 1;
    auto& entry = t.commands[seq % kMaxCommandEntries];
    entry.first.store(0, std::memory_order_release);
    entry.second.store(pack(nameId, origin, CommandState::Begun), std::memory_order_release);
    entry.first.store(seq, std::memory_order_release);
    t.header.commandHead.store(seq, std::memory_order_release);
    return seq;
}

void endCommand(std::uint64_t seq) noexcept {
    if (seq == 0) return;
    auto& entry = table().commands[seq % kMaxCommandEntries];
    if (entry.first.load(std::memory_order_relaxed) != seq) return;  // overwritten by nesting
    const std::uint64_t word = entry.second.load(std::memory_order_relaxed);
    entry.second.store((word & ~(std::uint64_t{0xff} << 40)) |
                           static_cast<std::uint64_t>(CommandState::Ended) << 40,
                       std::memory_order_release);
}

CommandsRead readCommands(
    std::uint64_t head,
    const std::array<TableImage::CommandEntry, kMaxCommandEntries>& entries) noexcept {
    CommandsRead out;
    for (std::size_t slot = 0; slot < kMaxCommandEntries; ++slot) {
        const auto& e = entries[slot];
        if (e.first == 0) continue;  // never written, or being rewritten
        const std::uint64_t seq = e.first;
        const auto origin = static_cast<std::uint8_t>((e.second >> 32) & 0xff);
        const auto state = static_cast<std::uint8_t>((e.second >> 40) & 0xff);
        const bool ok = seq % kMaxCommandEntries == slot && seq <= head + 1 &&
                        seq + kMaxCommandEntries > head && (e.second >> 48) == 0 && origin >= 1 &&
                        origin < kCommandOriginCount && state >= 1 && state < kCommandStateCount;
        if (!ok) {
            ++out.damaged;
            continue;
        }
        out.crumbs[out.count++] =
            CommandCrumb{seq, static_cast<std::uint32_t>(e.second),
                         static_cast<CommandOrigin>(origin), static_cast<CommandState>(state)};
    }
    std::sort(out.crumbs.begin(), out.crumbs.begin() + out.count,
              [](const CommandCrumb& a, const CommandCrumb& b) { return a.seq < b.seq; });
    return out;
}

CommandsRead readCommands(const TableImage& image) noexcept {
    std::array<TableImage::CommandEntry, kMaxCommandEntries> copy;
    for (std::size_t i = 0; i < kMaxCommandEntries; ++i) copy[i] = image.commands[i];
    return readCommands(image.header.commandHead, copy);
}

CommandsRead readCommands() noexcept {
    auto& t = table();
    std::array<TableImage::CommandEntry, kMaxCommandEntries> copy;
    const std::uint64_t head = t.header.commandHead.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < kMaxCommandEntries; ++i) {
        copy[i].first = t.commands[i].first.load(std::memory_order_acquire);
        copy[i].second = t.commands[i].second.load(std::memory_order_acquire);
    }
    return readCommands(head, copy);
}

namespace detail {
void resetCommandsForTests() noexcept {
    auto& t = table();
    gClaimed.store(false);
    gOwner.store(0);
    t.header.commandHead.store(0);
    t.header.commandsDropped.store(0);
    for (auto& e : t.commands) {
        e.first.store(0);
        e.second.store(0);
    }
}
}  // namespace detail

}  // namespace supple::diagctx
