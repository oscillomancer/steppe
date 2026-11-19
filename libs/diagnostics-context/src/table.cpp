// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/table.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

namespace supple::diagctx {
namespace {
// Zero-initialised and alignas a page: it lands in .bss and is never copied into the
// binary.
alignas(4096) Table gTable;
std::atomic<ThreadHook> gHook{nullptr};
std::atomic<detail::ReleaseHook> gReleaseHook{nullptr};

// The calling thread's stack as the system reports it, [low, high); 0, 0 when it will not
// say. Cold only: some systems allocate or read files to answer.
void stackBounds(std::uint64_t& low, std::uint64_t& high) noexcept {
    low = high = 0;
#if defined(_WIN32)
    ULONG_PTR l = 0, h = 0;
    ::GetCurrentThreadStackLimits(&l, &h);
    low = l;
    high = h;
#elif defined(__linux__)
    pthread_attr_t attributes;
    if (::pthread_getattr_np(::pthread_self(), &attributes) != 0) return;
    void* base = nullptr;
    std::size_t size = 0;
    if (::pthread_attr_getstack(&attributes, &base, &size) == 0 && base && size) {
        low = reinterpret_cast<std::uint64_t>(base);
        high = low + size;
    }
    ::pthread_attr_destroy(&attributes);
#elif defined(__APPLE__)
    const auto top = reinterpret_cast<std::uint64_t>(::pthread_get_stackaddr_np(::pthread_self()));
    const std::size_t size = ::pthread_get_stacksize_np(::pthread_self());
    if (top && size && top >= size) {
        high = top;
        low = top - size;
    }
#endif
}

// Releases the thread's slot when the thread ends, so a later thread that reuses the id
// is not labelled with this one's role.
struct Registration {
    int slot = -1;
    bool counted = false;  // a full table was counted for this thread
    bool hooked = false;   // the thread hook has run for this thread
    ~Registration() {
        if (slot < 0) return;
        auto& entry = gTable.threads[slot];
        // Contents first, id last: the slot is claimable only once its id is 0, and the
        // claimant's CAS acquires that release, so nothing written here can land after
        // the claimant's own stores. (The other order let a late wipe erase the next
        // thread's role.)
        entry.info.store(0, std::memory_order_relaxed);
        entry.stackLow.store(0, std::memory_order_relaxed);
        entry.stackHigh.store(0, std::memory_order_relaxed);
        if (const auto hook = gReleaseHook.load(std::memory_order_relaxed)) hook();
        entry.id.store(0, std::memory_order_release);
    }
};
}  // namespace

const char* threadRoleName(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::Model:
            return "model";
        case ThreadRole::AudioRender:
            return "audio_render";
        case ThreadRole::AudioLive:
            return "audio_live";
        case ThreadRole::AudioBackground:
            return "audio_background";
        case ThreadRole::AudioOffline:
            return "audio_offline";
        case ThreadRole::Recorder:
            return "recorder";
        case ThreadRole::MidiOutput:
            return "midi_output";
        case ThreadRole::BridgeControl:
            return "bridge_control";
        case ThreadRole::BridgeAudio:
            return "bridge_audio";
        case ThreadRole::None:
        case ThreadRole::Count_:
            break;
    }
    return nullptr;
}

Table& table() noexcept SUPPLE_RT { return gTable; }

void initTable() noexcept {
    // Version first, magic last: a reader that sees the magic sees a complete header.
    gTable.header.version.store(kTableVersion, std::memory_order_relaxed);
    gTable.header.magic.store(kTableMagic, std::memory_order_release);
}

bool imageValid(const TableImage& image) noexcept {
    return image.header.magic == kTableMagic && image.header.version == kTableVersion;
}

SlotInfo slotInfo(std::uint64_t info) noexcept {
    SlotInfo result;
    const auto role = static_cast<std::uint8_t>(info & 0xff);
    if (role == 0 || role >= kThreadRoleCount) return result;
    result.role = static_cast<ThreadRole>(role);
    result.ordinal = static_cast<std::uint16_t>((info >> 8) & 0xffff);
    return result;
}

SlotInfo findThread(const TableImage& image, std::uint64_t id) noexcept {
    if (id == 0 || id == kClaiming) return {};
    for (const auto& slot : image.threads) {
        if (slot.id != id) continue;
        SlotInfo found = slotInfo(slot.info);
        found.stackLow = slot.stackLow;
        found.stackHigh = slot.stackHigh;
        return found;
    }
    return {};
}

SlotInfo findThread(std::uint64_t id) noexcept {
    if (id == 0 || id == kClaiming) return {};
    for (const auto& slot : gTable.threads) {
        if (slot.id.load(std::memory_order_acquire) != id) continue;
        SlotInfo found = slotInfo(slot.info.load(std::memory_order_relaxed));
        found.stackLow = slot.stackLow.load(std::memory_order_relaxed);
        found.stackHigh = slot.stackHigh.load(std::memory_order_relaxed);
        return found;
    }
    return {};
}

std::uint64_t currentThreadId() noexcept {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    // The name task_threads lists this thread under. pthread_mach_thread_np adds no
    // reference to the port, so nothing is leaked and nothing has to be released.
    return static_cast<std::uint64_t>(::pthread_mach_thread_np(::pthread_self()));
#else
    return 0;
#endif
}

bool registerThread(ThreadRole role, std::uint16_t ordinal) noexcept {
    if (role == ThreadRole::None || role >= ThreadRole::Count_) return false;
    thread_local Registration mine;
    if (mine.slot >= 0) return true;
    initTable();
    const std::uint64_t id = currentThreadId();
    bool claimed = false;
    if (id != 0) {
        const std::uint64_t info = static_cast<std::uint64_t>(role) | std::uint64_t{ordinal} << 8;
        std::uint64_t low = 0, high = 0;
        stackBounds(low, high);
        for (std::size_t i = 0; i < kMaxThreadSlots && !claimed; ++i) {
            auto& slot = gTable.threads[i];
            // Claim a free slot (id 0). Everything else goes in before the id is
            // published, so a reader never pairs a new id with an old role.
            std::uint64_t expected = 0;
            if (slot.id.load(std::memory_order_relaxed) != 0) continue;
            // Reserve with a sentinel nobody uses as an id, then publish.
            if (!slot.id.compare_exchange_strong(expected, kClaiming, std::memory_order_acquire,
                                                 std::memory_order_relaxed))
                continue;
            slot.info.store(info, std::memory_order_relaxed);
            slot.stackLow.store(low, std::memory_order_relaxed);
            slot.stackHigh.store(high, std::memory_order_relaxed);
            slot.id.store(id, std::memory_order_release);
            mine.slot = static_cast<int>(i);
            claimed = true;
        }
        if (!claimed && !mine.counted) {
            mine.counted = true;
            gTable.header.threadsDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // The hook does not depend on the slot: a thread the table could not hold still gets
    // the alternate stack, so a stack overflow on it is still reported.
    if (!mine.hooked) {
        mine.hooked = true;
        if (const ThreadHook hook = gHook.load(std::memory_order_acquire)) hook();
    }
    return claimed;
}

void setThreadHook(ThreadHook hook) noexcept { gHook.store(hook, std::memory_order_release); }

namespace detail {
void setReleaseHook(ReleaseHook hook) noexcept {
    gReleaseHook.store(hook, std::memory_order_release);
}
}  // namespace detail

}  // namespace supple::diagctx
