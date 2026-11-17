// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Execution scopes (ADR-0083, topic 5b): which plug-in or module entry a thread is inside.
//
// Each plug-in instance (and each loaded plug-in library) owns one cell of the execution
// table, with two words: `audio` for the entries the audio thread makes and `main` for
// everything else. A scope writes its word on entry and puts the previous one back on exit,
// so scopes nest (a plug-in that calls back into the host, a rack hosting a plug-in) and a
// word is non-idle exactly while some thread is inside an entry of that instance.
//
// A word packs, from the top: the entry kind (8 bits), a generation (12 bits) and the
// scope's stack address divided by 16 (44 bits, so addresses below 2^48). The stack address
// is what ties the word to a thread from outside the process: the thread whose stack holds
// it, and whose stack pointer lies at or below it, is inside the entry; of several, the
// innermost (the lowest address) is the one it is in. The generation counts the cell's
// reuses and is a hint for a reader, never an identity: it repeats after 4,096 of them.
//
// What the stack address is (the same on every architecture). It is the frame address
// (`__builtin_frame_address(0)`) of the function that declares the scope, taken in that
// function and not in the constructor: a constructor that is not inlined (every Debug build,
// and any wrapper class that is not) has a frame of its own, which is gone when it returns
// and which the calls the entry makes then reuse, so its address would sit among the
// callees' and not above them. That is why the address is a defaulted argument of the
// constructor, evaluated at the declaration; a class that wraps a scope forwards it the same
// way. The frame address is the frame-pointer register of the function (x86-64: the slot
// that holds the saved rbp; arm64: the frame record, the saved x29 and x30), which stays at
// or above the function's stack pointer for as long as the function runs, and every frame
// of a function it calls lies wholly below the caller's stack pointer. So, for a scope that
// is a local of a function that has its own frame:
//   - the thread's stack pointer is at or below the recorded address while the scope lives;
//   - a scope in a called function (not inlined into the caller) has a strictly lower
//     address than the scope of the caller, which is what makes "the lowest" the innermost.
// Nothing else is promised. Where the frame address sits among the function's locals is the
// compiler's choice (the frame record is above them in one compiler's arm64 layout and
// below them in another's), so no code compares it with the address of a local. Two scopes
// declared in one function have the same address and cannot be told apart. A scope that is
// not a local (a member of a heap object) records an address on no stack, which a reader
// does not tie to a thread.
//
// Cost on the audio path: two release stores per entry, one on entry and one on exit, each
// a plain store on x86-64 and a store-release on arm64; no loop, no lock, no allocation, no
// call. A word has one writer at a time (the thread inside the entry), and the reader only
// looks after that thread has been stopped, so release ordering is all the writer owes: a
// reader that sees a word sees what the thread did before writing it, and a thread that is
// stopped cannot be halfway through writing one.
//
// The header is inline so the audio path calls nothing it cannot see; the cold parts
// (cells, naming) are in the library.
#include <atomic>
#include <cstdint>

#include "supple/base/rt.h"
#include "supple/diagctx/table.h"

namespace supple::diagctx {

// What a thread is doing inside an instance. The values are the report's contract (a closed
// set, parsed strictly); None is idle. BridgeCall stands in for any entry that an isolated
// instance forwards to its bridge child.
enum class Entry : std::uint8_t {
    None = 0,
    Create,   // the plug-in's init, create and extension lookups
    Destroy,  // its destroy
    Activate,
    Deactivate,
    StartProcessing,  // audio thread
    StopProcessing,   // audio thread
    Process,          // audio thread
    Flush,            // audio thread: a parameter flush while not processing
    Reset,            // audio thread
    Params,           // parameter reads, writes and conversions
    State,            // save and load
    Semantic,         // the semantic view: schema, patch, describe, status, factory patches
    EditorPicture,    // the editor without a window
    Trace,            // the telemetry extension
    Gui,              // a native editor window
    MainThread,       // on_main_thread, timers and file-descriptor callbacks
    LibraryOpen,      // dlopen, clap_entry.init and the factory lookup
    LibraryClose,     // clap_entry.deinit and dlclose
    BridgeCall,
    Count_  // one past the last kind
};
constexpr std::uint8_t kEntryCount = static_cast<std::uint8_t>(Entry::Count_);
static_assert(kEntryCount <= 20, "the table's contract is at most 20 entry kinds");
// The report string for an entry ("process", "start_processing", ...); nullptr for None or
// out of range.
const char* entryName(Entry entry) noexcept;

// A cell in the table, or none: an instance created while all 1,024 were taken has no cell
// (counted in `untracked`) and its scopes do nothing.
constexpr std::uint32_t kNoCell = ~std::uint32_t{0};
// Claims a cell; cold, any thread. kNoCell if none is free. Both words start idle, with the
// cell's next generation.
std::uint32_t acquireCell() noexcept;
// Gives the cell back, idle. kNoCell is ignored.
void releaseCell(std::uint32_t cell) noexcept;

// A word's fields. `stack` is the recorded address rounded down to 16, 0 when the scope had
// none to record (an address the word cannot hold).
struct ScopeWord {
    Entry kind = Entry::None;
    std::uint16_t generation = 0;
    std::uint64_t stack = 0;
};
constexpr unsigned kWordKindShift = 56, kWordGenerationShift = 44;
constexpr std::uint64_t kWordStackMask = (std::uint64_t{1} << kWordGenerationShift) - 1;
constexpr std::uint16_t kGenerationMask = 0xfff;
// The highest address a word can hold, plus one.
constexpr std::uint64_t kWordStackLimit = (kWordStackMask + 1) << 4;

constexpr std::uint64_t packWord(Entry kind, std::uint16_t generation,
                                 std::uint64_t stack) noexcept {
    const std::uint64_t field = stack < kWordStackLimit ? stack >> 4 : 0;
    return static_cast<std::uint64_t>(kind) << kWordKindShift |
           std::uint64_t{static_cast<std::uint16_t>(generation & kGenerationMask)}
               << kWordGenerationShift |
           field;
}
// A kind outside the closed set reads as None, so a damaged or hostile word is idle.
constexpr ScopeWord unpackWord(std::uint64_t word) noexcept {
    ScopeWord result;
    const auto kind = static_cast<std::uint8_t>(word >> kWordKindShift);
    result.kind = kind < kEntryCount ? static_cast<Entry>(kind) : Entry::None;
    result.generation =
        static_cast<std::uint16_t>((word >> kWordGenerationShift) & kGenerationMask);
    result.stack = (word & kWordStackMask) << 4;
    return result;
}

// Counted when a scope's stack address does not fit a word (see packWord); the report says
// those entries cannot be tied to a thread.
inline void countScopeOverflow() noexcept SUPPLE_RT {
    table().header.scopeOverflow.fetch_add(1, std::memory_order_relaxed);
}

enum class Word : std::uint8_t { Audio, Main };

// A token for the calling thread, unique among live threads: the address of a thread-local.
inline std::uint64_t threadToken() noexcept {
    static thread_local char marker;
    return reinterpret_cast<std::uintptr_t>(&marker);
}

// Marks the calling thread as inside `kind` of the instance that owns `cell` until the end of
// the scope. `Audio` for the entries the audio thread makes: the executors never run one
// instance's audio entries at the same time, so that word has one writer and a scope is two
// plain stores. `Main` for everything else: the host should never overlap two main-thread entries
// of one instance, but if it does (an editor opened on one thread while another reads a parameter)
// the thread that is not first does not record its entry, rather than interleave writes that could
// leave a stale word naming an entry nobody is in: the first thread to enter owns the word until
// its outermost scope ends, and its own nested scopes still record. A scope on kNoCell does
// nothing.
template <Word W>
class Scope {
  public:
    // `frame` is the frame address of the declaring function: leave it to the default, which
    // is evaluated there (see the top of this file). A wrapper takes the same default and
    // passes it on.
    Scope(std::uint32_t cell, Entry kind,
          const void* frame = __builtin_frame_address(0)) noexcept SUPPLE_RT {
        if (cell >= kMaxInstances) return;
        auto& cells = table().instances[cell];
        if constexpr (W == Word::Main) {
            const std::uint64_t me = threadToken();
            std::uint64_t expected = 0;
            if (cells.mainOwner.compare_exchange_strong(expected, me, std::memory_order_acquire,
                                                        std::memory_order_relaxed))
                owner_ = &cells.mainOwner;  // the outermost scope releases it
            else if (expected != me)
                return;  // another thread is inside an entry of this instance
        }
        word_ = W == Word::Audio ? &cells.audio : &cells.main;
        previous_ = word_->load(std::memory_order_relaxed);
        const auto here = reinterpret_cast<std::uintptr_t>(frame);
        if (here >= kWordStackLimit) countScopeOverflow();
        word_->store(packWord(kind, unpackWord(previous_).generation, here),
                     std::memory_order_release);
    }
    ~Scope() SUPPLE_RT {
        if (word_) word_->store(previous_, std::memory_order_release);
        if (owner_) owner_->store(0, std::memory_order_release);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    std::atomic<std::uint64_t>* word_ = nullptr;
    std::atomic<std::uint64_t>* owner_ = nullptr;  // set only by the outermost Main scope
    std::uint64_t previous_ = 0;
};
using AudioScope = Scope<Word::Audio>;
using MainScope = Scope<Word::Main>;

}  // namespace supple::diagctx
