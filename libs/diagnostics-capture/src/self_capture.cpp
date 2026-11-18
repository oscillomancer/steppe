// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include "self_capture_internal.h"
#include "supple/diagcap/trace.h"
#include "supple/diagctx/scope.h"
#include "supple/diagctx/table.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// The bridge's self-capture, portable part (topic 6c): the cold module list, the slot and its
// protocol, and the capsule the handler writes. Everything a handler reaches in this file is
// static storage, atomic loads and stores and memcpy.
namespace supple::diagnostics::selfcapture {
namespace {
using detail::CapsuleHeader;
using detail::CapsuleThreadData;

// ---- cold module list -----------------------------------------------------------------
std::mutex listMutex;  // cold callers only; the handler never takes it
BridgeModule list[kMaxModules];
std::atomic<std::uint32_t> listCount{0};
std::atomic<std::uint32_t> listFlags{0};

// ---- the slot -------------------------------------------------------------------------
// Until the host's shared slot is attached the handler writes a private one of the same shape,
// so that the capture path is the same code with or without a host to read it.
alignas(64) unsigned char privateSlot[kSlotBytes];
std::atomic<unsigned char*> slotMemory{nullptr};
std::atomic<bool> attached{false};
std::atomic<bool> handlersInstalled{false};

unsigned char* activeSlot() noexcept { return slotMemory.load(std::memory_order_acquire); }

std::uint32_t loadState(SlotHeader* header) noexcept {
    return std::atomic_ref<std::uint32_t>(header->state).load(std::memory_order_acquire);
}
void storeState(SlotHeader* header, std::uint32_t state) noexcept {
    std::atomic_ref<std::uint32_t>(header->state).store(state, std::memory_order_release);
}

// The execution table as plain words, one relaxed atomic load for each: the table is live and
// other threads may be writing it, as when the worker's table is read from outside. Padding
// stays as the slot was (zero).
void copyTable(unsigned char* out) noexcept {
    using Image = diagctx::TableImage;
    const auto& table = diagctx::table();
    const auto put32 = [out](std::size_t at, const std::atomic<std::uint32_t>& word) noexcept {
        const std::uint32_t value = word.load(std::memory_order_relaxed);
        std::memcpy(out + at, &value, sizeof(value));
    };
    const auto put64 = [out](std::size_t at, const std::atomic<std::uint64_t>& word) noexcept {
        const std::uint64_t value = word.load(std::memory_order_relaxed);
        std::memcpy(out + at, &value, sizeof(value));
    };
    constexpr std::size_t header = offsetof(Image, header);
    put32(header + offsetof(Image::Header, magic), table.header.magic);
    put32(header + offsetof(Image::Header, version), table.header.version);
    put32(header + offsetof(Image::Header, threadsDropped), table.header.threadsDropped);
    put32(header + offsetof(Image::Header, untracked), table.header.untracked);
    put32(header + offsetof(Image::Header, commandsDropped), table.header.commandsDropped);
    put32(header + offsetof(Image::Header, scopeOverflow), table.header.scopeOverflow);
    put64(header + offsetof(Image::Header, commandHead), table.header.commandHead);
    for (std::size_t i = 0; i < diagctx::kMaxThreadSlots; ++i) {
        const std::size_t at = offsetof(Image, threads) + i * sizeof(Image::ThreadSlot);
        put64(at + offsetof(Image::ThreadSlot, id), table.threads[i].id);
        put64(at + offsetof(Image::ThreadSlot, info), table.threads[i].info);
        put64(at + offsetof(Image::ThreadSlot, stackLow), table.threads[i].stackLow);
        put64(at + offsetof(Image::ThreadSlot, stackHigh), table.threads[i].stackHigh);
    }
    for (std::size_t i = 0; i < diagctx::kMaxInstances; ++i) {
        const std::size_t at = offsetof(Image, instances) + i * sizeof(Image::InstanceCell);
        put64(at + offsetof(Image::InstanceCell, audio), table.instances[i].audio);
        put64(at + offsetof(Image::InstanceCell, main), table.instances[i].main);
        put64(at + offsetof(Image::InstanceCell, mainOwner), table.instances[i].mainOwner);
    }
    for (std::size_t i = 0; i < diagctx::kMaxCommandEntries; ++i) {
        const std::size_t at = offsetof(Image, commands) + i * sizeof(Image::CommandEntry);
        put64(at + offsetof(Image::CommandEntry, first), table.commands[i].first);
        put64(at + offsetof(Image::CommandEntry, second), table.commands[i].second);
    }
}
}  // namespace

std::uint64_t monotonic() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER frequency{}, counter{};
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0)
        return 0;
    const auto f = static_cast<std::uint64_t>(frequency.QuadPart);
    const auto c = static_cast<std::uint64_t>(counter.QuadPart);
    return c / f * 1'000'000'000ULL + c % f * 1'000'000'000ULL / f;
#else
    return detail::monotonicNanoseconds();
#endif
}

bool initializeSlot(void* memory, std::size_t bytes, std::uint32_t ordinal) noexcept {
    if (!memory || bytes != kSlotBytes ||
        (reinterpret_cast<std::uintptr_t>(memory) % alignof(SlotHeader)) != 0)
        return false;
    SlotHeader header{};
    header.magic = kSlotMagic;
    header.version = kSlotVersion;
    header.state = kSlotEmpty;
    header.capacity = static_cast<std::uint32_t>(bytes);
    header.ordinal = ordinal;
    std::memcpy(memory, &header, sizeof(header));
    return true;
}

bool slotUsable(const SlotHeader& header, std::size_t bytes) noexcept {
    return bytes == kSlotBytes && header.magic == kSlotMagic && header.version == kSlotVersion &&
           header.capacity == bytes && header.length <= bytes - sizeof(SlotHeader);
}

bool attachSlot(void* memory, std::size_t bytes) noexcept {
    if (attached.load(std::memory_order_acquire) || !memory || bytes != kSlotBytes ||
        (reinterpret_cast<std::uintptr_t>(memory) % alignof(SlotHeader)) != 0)
        return false;
    SlotHeader header;
    std::memcpy(&header, memory, sizeof(header));
    if (!slotUsable(header, bytes) ||
        std::atomic_ref<std::uint32_t>(static_cast<SlotHeader*>(memory)->state)
                .load(std::memory_order_acquire) != kSlotEmpty)
        return false;
    // A handler that already began (state writing in the private slot) keeps writing there:
    // the pointer is read once, at the handler's start.
    bool expected = false;
    if (!attached.compare_exchange_strong(expected, true)) return false;
    slotMemory.store(static_cast<unsigned char*>(memory), std::memory_order_release);
    return true;
}

bool recordImage(const void* address) {
    if (!address) return false;
    BridgeModule found{};
    if (!identifyImage(address, found) || found.start >= found.end) return false;
    const std::lock_guard lock(listMutex);
    const std::uint32_t count = listCount.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < count; ++i)
        if (list[i].start == found.start) return true;  // already there
    if (count >= kMaxModules) {
        listFlags.fetch_or(detail::kCapsuleModulesTruncated, std::memory_order_relaxed);
        return false;
    }
    list[count] = found;  // complete before the count says it is there
    listCount.store(count + 1, std::memory_order_release);
    return true;
}

std::uint32_t recordedImages() noexcept { return listCount.load(std::memory_order_acquire); }

void summarizeTable(CapsuleHeader& header) noexcept {
    const auto& table = diagctx::table();
    header.tableThreadsDropped = table.header.threadsDropped.load(std::memory_order_relaxed);
    header.tableUntracked = table.header.untracked.load(std::memory_order_relaxed);
    header.tableScopeOverflow = table.header.scopeOverflow.load(std::memory_order_relaxed);
    header.commandsRefused = table.header.commandsDropped.load(std::memory_order_relaxed);
}

// The entry the fault thread was inside: the same rule as the supervisor's collector and the
// worker's macOS capture. The innermost (lowest address) of the open entries whose stack
// address lies on the thread's recorded stack at or above its stack pointer. A thread that
// recorded no stack is attributed nothing.
void attributeFault(CapsuleThreadData& thread, std::uint64_t tid, std::uint64_t sp) noexcept {
    const diagctx::SlotInfo slot = diagctx::findThread(tid);
    thread.head.registered = static_cast<std::uint8_t>(slot.role);
    thread.head.worker = slot.ordinal;
    if (sp == 0 || slot.stackLow == 0 || slot.stackHigh <= slot.stackLow || sp < slot.stackLow ||
        sp >= slot.stackHigh)
        return;
    bool found = false;
    std::uint64_t best = 0;
    auto& cells = diagctx::table().instances;
    for (std::uint32_t cell = 0; cell < diagctx::kMaxInstances; ++cell) {
        for (const bool audio : {true, false}) {
            const auto fields = diagctx::unpackWord(
                (audio ? cells[cell].audio : cells[cell].main).load(std::memory_order_acquire));
            if (fields.kind == diagctx::Entry::None || fields.stack == 0) continue;
            if (fields.stack + 16 <= sp || fields.stack < slot.stackLow ||
                fields.stack >= slot.stackHigh)
                continue;
            if (!found || fields.stack < best) {
                found = true;
                best = fields.stack;
                thread.head.entry = static_cast<std::uint8_t>(fields.kind);
                thread.head.entryCell = static_cast<std::uint16_t>(cell);
                thread.head.entryGeneration = fields.generation;
            }
        }
    }
}

bool publishCapsule(const CapsuleHeader& header, const BridgeModule* modules,
                    std::uint32_t moduleCount, const CapsuleThreadData* threads,
                    std::uint32_t threadCount, std::uint64_t start) noexcept {
    unsigned char* memory = activeSlot();
    if (!memory) return false;
    auto* slot = reinterpret_cast<SlotHeader*>(memory);
    if (loadState(slot) != kSlotEmpty) return false;  // written once per bridge
    storeState(slot, kSlotWriting);
    std::size_t used = sizeof(CapsuleHeader) + std::size_t{moduleCount} * sizeof(BridgeModule);
    for (std::uint32_t i = 0; i < threadCount; ++i)
        used +=
            sizeof(CapsuleThread) + std::size_t{threads[i].head.frameCount} * sizeof(std::uint64_t);
    used += diagctx::kTableBytes;
    if (sizeof(SlotHeader) + used > kSlotBytes) return false;  // left `writing`: never complete
    unsigned char* out = memory + sizeof(SlotHeader);
    CapsuleHeader written = header;
    written.magic = kBridgeCapsuleMagic;
    written.moduleCount = moduleCount;
    written.threadCount = threadCount;
    written.commandCount = 0;
    written.flags |= kCapsuleHasTable;
    std::memcpy(out, &written, sizeof(written));
    out += sizeof(written);
    if (moduleCount > 0) {
        std::memcpy(out, modules, std::size_t{moduleCount} * sizeof(BridgeModule));
        out += std::size_t{moduleCount} * sizeof(BridgeModule);
    }
    for (std::uint32_t i = 0; i < threadCount; ++i) {
        std::memcpy(out, &threads[i].head, sizeof(CapsuleThread));
        out += sizeof(CapsuleThread);
        const std::size_t frames = std::size_t{threads[i].head.frameCount} * sizeof(std::uint64_t);
        if (frames > 0) std::memcpy(out, threads[i].frames, frames);
        out += frames;
    }
    copyTable(out);
    slot->length = static_cast<std::uint32_t>(used);
    // The bound is the handler's own: a slot that is not complete by then stays `writing`.
    if (monotonic() >= start + kHandlerBudget) return false;
    storeState(slot, kSlotComplete);
    return true;
}

#if !defined(__APPLE__)
bool captureAndPublish(const Fault& fault, std::uint64_t start) noexcept {
    // Static, zero-filled: one fault thread is all that is walked on these platforms.
    static CapsuleThreadData thread;
    static CapsuleHeader header;
    header.reserved = static_cast<std::uint32_t>(fault.tid);
    thread.head.role = static_cast<std::uint8_t>(detail::Role::Fault);
    thread.head.missing = static_cast<std::uint8_t>(detail::Missing::None);
    walkFault(fault, thread, start + kHandlerBudget);
    attributeFault(thread, fault.tid, fault.sp);
    summarizeTable(header);
    header.flags |= listFlags.load(std::memory_order_relaxed);
    // The list is copied into the slot straight from its static storage. Entries below the
    // count are complete (release/acquire), and the list only grows.
    const std::uint32_t count = listCount.load(std::memory_order_acquire);
    return publishCapsule(header, list, count, &thread, 1, start);
}
#else
bool captureAndPublish(const Fault& fault, std::uint64_t start) noexcept {
    return captureAndPublishMac(fault, start);
}
#endif

bool install() noexcept {
    if (handlersInstalled.load(std::memory_order_acquire)) return true;
    diagctx::initTable();
    if (!attached.load(std::memory_order_acquire)) {
        (void)initializeSlot(privateSlot, kSlotBytes, 0);
        slotMemory.store(privateSlot, std::memory_order_release);
    }
    const bool installed = installHandlers();
    if (installed) {
        handlersInstalled.store(true, std::memory_order_release);
        (void)recordImage(reinterpret_cast<const void*>(&install));  // the bridge's executable
    }
    return installed;
}

#if defined(__APPLE__)
// macOS fills the list from the cold calls and what dyld lists in the handler; the list itself
// is the same, so the macOS file reads it through this.
std::uint32_t copyRecorded(BridgeModule* out, std::uint32_t capacity,
                           std::uint32_t& flags) noexcept {
    const std::uint32_t count = listCount.load(std::memory_order_acquire);
    const std::uint32_t n = count < capacity ? count : capacity;
    for (std::uint32_t i = 0; i < n; ++i) out[i] = list[i];
    flags = listFlags.load(std::memory_order_relaxed);
    return n;
}
#endif
}  // namespace supple::diagnostics::selfcapture
