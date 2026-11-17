// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/scope.h"

namespace supple::diagctx {
namespace {
// One bit per cell: set while an instance or library holds it. Zero-initialised static
// storage, never freed; only cold code touches it.
constexpr std::size_t kCellWords = kMaxInstances / 64;
static_assert(kMaxInstances % 64 == 0);
std::atomic<std::uint64_t> gTaken[kCellWords];

std::uint16_t generationOf(std::uint64_t word) noexcept { return unpackWord(word).generation; }
}  // namespace

const char* entryName(Entry entry) noexcept {
    switch (entry) {
        case Entry::Create:
            return "create";
        case Entry::Destroy:
            return "destroy";
        case Entry::Activate:
            return "activate";
        case Entry::Deactivate:
            return "deactivate";
        case Entry::StartProcessing:
            return "start_processing";
        case Entry::StopProcessing:
            return "stop_processing";
        case Entry::Process:
            return "process";
        case Entry::Flush:
            return "flush";
        case Entry::Reset:
            return "reset";
        case Entry::Params:
            return "params";
        case Entry::State:
            return "state";
        case Entry::Semantic:
            return "semantic";
        case Entry::EditorPicture:
            return "editor_picture";
        case Entry::Trace:
            return "trace";
        case Entry::Gui:
            return "gui";
        case Entry::MainThread:
            return "main_thread";
        case Entry::LibraryOpen:
            return "library_open";
        case Entry::LibraryClose:
            return "library_close";
        case Entry::BridgeCall:
            return "bridge_call";
        case Entry::None:
        case Entry::Count_:
            break;
    }
    return nullptr;
}

std::uint32_t acquireCell() noexcept {
    initTable();
    for (std::size_t w = 0; w < kCellWords; ++w) {
        std::uint64_t taken = gTaken[w].load(std::memory_order_relaxed);
        while (taken != ~std::uint64_t{0}) {
            const auto bit = static_cast<unsigned>(__builtin_ctzll(~taken));
            if (!gTaken[w].compare_exchange_weak(taken, taken | std::uint64_t{1} << bit,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed))
                continue;  // `taken` was reloaded
            const auto cell = static_cast<std::uint32_t>(w * 64 + bit);
            auto& entry = table().instances[cell];
            // Both words start idle in the next generation. The words are idle (or being
            // given back) here: the previous holder released them before clearing the bit.
            const std::uint16_t next = static_cast<std::uint16_t>(
                (generationOf(entry.audio.load(std::memory_order_relaxed)) + 1) & kGenerationMask);
            const std::uint64_t idle = packWord(Entry::None, next, 0);
            entry.main.store(idle, std::memory_order_relaxed);
            entry.mainOwner.store(0, std::memory_order_relaxed);
            entry.audio.store(idle, std::memory_order_release);
            return cell;
        }
    }
    table().header.untracked.fetch_add(1, std::memory_order_relaxed);
    return kNoCell;
}

void releaseCell(std::uint32_t cell) noexcept {
    if (cell >= kMaxInstances) return;
    auto& entry = table().instances[cell];
    // Idle, in the same generation, before the cell can be handed out again.
    const std::uint64_t idle =
        packWord(Entry::None, generationOf(entry.audio.load(std::memory_order_relaxed)), 0);
    entry.main.store(idle, std::memory_order_relaxed);
    entry.mainOwner.store(0, std::memory_order_relaxed);
    entry.audio.store(idle, std::memory_order_relaxed);
    gTaken[cell / 64].fetch_and(~(std::uint64_t{1} << (cell % 64)), std::memory_order_release);
}

}  // namespace supple::diagctx
