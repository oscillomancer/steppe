// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The execution table (ADR-0083, topic 5).
//
// One static, zero-initialised block per process that says which owned threads exist
// and, from step 5b, which plug-in or module entry each instance is inside. A crash
// report reads it from outside the failing process: the Linux and Windows supervisors
// copy it with a bounded remote read, the macOS worker copies it into its capsule.
//
// It holds integers and enums only. It never holds a string, a pointer to model or
// plug-in memory, or anything a user typed, and everything a reader takes from it is
// untrusted: `imageValid` and the enum range checks come before any use.
//
// The layout is a template over the word types so one definition serves both sides:
// `Table` (atomic words) is what the process writes, `TableImage` (plain words) is what
// a reader copies bytes into. The two have the same size and layout (static_assert).
//
// Library placement: GPL, under libs/ so that command-bus (also under libs/) can link it
// without a library depending on host products; core-base and modules/rack are MIT and
// plug-in-linked and never include this.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "supple/base/rt.h"

namespace supple::diagctx {

constexpr std::uint32_t kTableMagic = 0x58435453;  // "STCX"
constexpr std::uint32_t kTableVersion = 1;
// Slots are sized for the real maximum of owned threads: the fixed ones (model, render,
// MIDI output, recorder, one offline render), a live pool and a background pool of up to
// 64 helpers each, and an export's own pool of up to 63. A machine with 32 CPUs runs 92 of
// them while it exports; 256 leaves room for a second concurrent render.
constexpr std::size_t kMaxThreadSlots = 256, kMaxInstances = 1024, kMaxCommandEntries = 64;

// What an owned thread does. The values are the report's contract (closed set, parsed
// strictly); None is an unclaimed slot. Threads nobody registered (a driver's device
// callback, a plug-in's own threads) have no slot and are reported as "other".
enum class ThreadRole : std::uint8_t {
    None = 0,
    Model,
    AudioRender,
    AudioLive,
    AudioBackground,
    AudioOffline,
    Recorder,
    MidiOutput,
    Count_  // one past the last role
};
constexpr std::uint8_t kThreadRoleCount = static_cast<std::uint8_t>(ThreadRole::Count_);
// The report string for a role ("model", "audio_render", ...); nullptr for None or out of
// range.
const char* threadRoleName(ThreadRole role) noexcept;

template <class W32, class W64>
struct TableT {
    struct Header {
        W32 magic, version;
        W32 threadsDropped, untracked, commandsDropped, scopeOverflow;
        W64 commandHead;
    };
    // A registered thread. `id` is the operating system's id for the thread (Linux and
    // Windows thread id, macOS Mach thread port name) and is written last when a slot is
    // claimed and cleared last when it is released, so a slot with id 0 is free or still
    // being claimed and everything else in it belongs to `id`. `info` packs role (bits
    // 0-7) and an ordinal (bits 8-23) for numbered workers such as audio-live-2.
    // `stackLow` and `stackHigh` are the thread's stack, recorded while cold, so that a
    // stack address finds the thread without the mapping list; 0 when the system would
    // not say.
    struct ThreadSlot {
        W64 id, info, stackLow, stackHigh;
    };
    // Two words per instance, each with one writer: `audio` for the audio-thread entries
    // and `main` for everything else (step 5b).
    struct InstanceCell {
        W64 audio, main;
    };
    // The last registered commands (step 5d), model thread only.
    struct CommandEntry {
        W64 first, second;
    };
    Header header;
    ThreadSlot threads[kMaxThreadSlots];
    InstanceCell instances[kMaxInstances];
    CommandEntry commands[kMaxCommandEntries];
};
using Table = TableT<std::atomic<std::uint32_t>, std::atomic<std::uint64_t>>;
using TableImage = TableT<std::uint32_t, std::uint64_t>;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free &&
              std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(sizeof(Table) == sizeof(TableImage) && alignof(Table) == alignof(TableImage));
static_assert(sizeof(Table::Header) == 32 && sizeof(Table::ThreadSlot) == 32 &&
              sizeof(Table::InstanceCell) == 16 && sizeof(Table::CommandEntry) == 16);
static_assert(sizeof(TableImage) ==
              32 + 32 * kMaxThreadSlots + 16 * (kMaxInstances + kMaxCommandEntries));
static_assert(std::is_trivially_copyable_v<TableImage> && std::is_standard_layout_v<TableImage>);
constexpr std::size_t kTableBytes = sizeof(TableImage);

// The process's table: static storage, zero-filled, so it costs the binary nothing.
Table& table() noexcept SUPPLE_RT;
// Writes the magic and version (release); idempotent. Cold. Nothing reads the table
// before the supervisor has been told its address, which happens after this.
void initTable() noexcept;
// Whether a copy of a table is one this build wrote.
bool imageValid(const TableImage& image) noexcept;

// A slot's role and ordinal, None for a free slot or an out-of-range role. `stackLow` and
// `stackHigh` are the stack the thread recorded, 0 and 0 when it recorded none.
struct SlotInfo {
    ThreadRole role = ThreadRole::None;
    std::uint16_t ordinal = 0;
    std::uint64_t stackLow = 0, stackHigh = 0;
};
SlotInfo slotInfo(std::uint64_t info) noexcept;
// The slot of the thread with operating-system id `id`, in a copy of the table or in the
// live one (atomic loads only, async-signal-safe: the macOS terminal handler uses it).
// Role None if no registered thread has that id. A slot still being claimed (id
// kClaiming) matches nothing.
constexpr std::uint64_t kClaiming = ~std::uint64_t{0};
SlotInfo findThread(const TableImage& image, std::uint64_t id) noexcept;
SlotInfo findThread(std::uint64_t id) noexcept;

// Registers the calling thread; cold, owned threads only, never from an audio path.
// Idempotent per thread (a second call keeps the first role). The slot is released when
// the thread exits. False if the table is full (counted once per thread in
// `threadsDropped`) or the thread id is unavailable. The thread hook runs once for the
// thread either way, so a thread without a slot still gets its alternate stack.
bool registerThread(ThreadRole role, std::uint16_t ordinal = 0) noexcept;
// The id registerThread records for the calling thread, 0 if unavailable.
std::uint64_t currentThreadId() noexcept;
// Called once on each thread that registers, after its slot is claimed (or found
// unavailable); the diagnostics library installs the alternate signal stack through it.
// Set while cold.
using ThreadHook = void (*)() noexcept;
void setThreadHook(ThreadHook hook) noexcept;

namespace detail {
// A test seam: called on a thread that is releasing its slot, after the first of the two
// release steps and before the second. Null in production; costs one relaxed load at
// thread exit.
using ReleaseHook = void (*)() noexcept;
void setReleaseHook(ReleaseHook hook) noexcept;
}  // namespace detail

}  // namespace supple::diagctx
