// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Thread traces for crash reports (ADR-0083, topic 4).
//
// Everything here is portable and runs in the healthy supervisor, never in the
// failing process: a frame-pointer walker over an injected fault-proof memory
// reader, module identity from ELF headers read through that same reader, the
// bounded orchestration with its deadline, and the report JSON. Each platform
// supplies only the TraceOps that stop a thread and read memory. A trace holds
// module identity plus module-relative executable addresses and nothing else: no
// register or stack bytes, paths, thread ids or arbitrary words.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "native.h"
#include "supple/diagctx/scope.h"
#include "supple/json/json.h"
namespace supple::diagnostics::detail {
constexpr std::size_t kMaxThreads = 128, kMaxFrames = 128, kMaxModules = 256;

struct Regs {
    std::uint64_t pc = 0, sp = 0, fp = 0;
};
// Reads `bytes` at a target address, all or nothing, never faulting the caller.
using ReadMemory = std::function<bool(std::uint64_t address, void* out, std::size_t bytes)>;

struct Mapping {  // one /proc/<pid>/maps line, without its path
    std::uint64_t start = 0, end = 0, offset = 0, inode = 0;
    bool read = false, write = false, execute = false, named = false;
    std::string device;
};
// Parses at most kMaxMappings lines; malformed lines are skipped, never trusted.
constexpr std::size_t kMaxMappings = 16384;
std::vector<Mapping> parseMaps(std::string_view text);

struct Module {
    std::optional<std::string> id;  // ELF build ID, lowercase hex; none when absent
    std::uint64_t bias = 0;         // load address minus link address
};
struct Region {  // an executable mapping
    std::uint64_t start = 0, end = 0;
    int module = -1;  // index into modules, -1: executable memory with no file (JIT)
};
struct Layout {
    std::vector<Module> modules;
    std::vector<Region> regions;  // sorted, disjoint
    bool truncatedModules = false;
};
// Build ID of the ELF image whose first page is at `base`, read through `read`.
// Every size read from the image is bounded; anything odd yields no ID.
std::optional<std::string> elfBuildId(const ReadMemory& read, std::uint64_t base,
                                      std::uint64_t* bias = nullptr);
Layout buildLayout(const std::vector<Mapping>& mappings, const ReadMemory& read);
// CodeView identity of the PE image loaded at `base` (64-bit images only): the PDB
// GUID in symbol-server order followed by the age as eight hex digits, 40 lowercase
// hex characters. The PDB path is never read. `bias` receives load address minus
// the image's preferred base once the headers parsed, even when there is no CodeView
// record. Every offset and count read from the image is bounded.
std::optional<std::string> peDebugId(const ReadMemory& read, std::uint64_t base,
                                     std::uint64_t* bias = nullptr);

// The x64 function table of a PE image (its exception directory, `.pdata`): sorted
// RUNTIME_FUNCTION entries of three RVAs. The Windows collector hands these to the OS
// unwinder instead of asking DbgHelp, whose lookup answered nothing for any module it had
// loaded without symbols; everything here is read through the fault-proof reader, with
// every size checked against the image and every search bounded.
struct RuntimeFunction {
    std::uint32_t begin = 0, end = 0, unwind = 0;  // RVAs; unwind is the UNWIND_INFO's
};
class PeFunctionTable {
  public:
    // The table of the 64-bit image whose headers are at `base`; none when the image has no
    // exception directory or its headers are not what the loader would accept.
    static std::optional<PeFunctionTable> open(const ReadMemory& read, std::uint64_t base);
    // The entry whose [begin, end) holds `address`, by binary search (at most 32 reads of
    // 12 bytes); none when no entry does or one cannot be read. An unsorted table can hide an
    // entry from the search; it cannot make it invent one.
    std::optional<RuntimeFunction> find(const ReadMemory& read, std::uint64_t address) const;

  private:
    std::uint64_t base_ = 0;
    std::uint32_t imageSize_ = 0, rva_ = 0, count_ = 0;
};

struct Frame {
    int module = -1;  // -1: unavailable, only its kind is recorded
    std::uint64_t offset = 0;
    bool returnAddress = false;  // false: the exact pc of the stopped instruction
};
enum class Stop : std::uint8_t {
    End,              // the chain ended at a null frame pointer or return address
    Depth,            // kMaxFrames reached with the chain still going
    Unreadable,       // memory at the frame pointer could not be read
    NotExecutable,    // a return address outside every executable mapping
    BadFramePointer,  // misaligned, below the stack pointer, or a huge jump
    Cycle,            // frame pointers did not strictly increase
    Deadline,         // the hold budget ran out
};
struct Walk {
    std::vector<Frame> frames;
    Stop stop = Stop::End;
};
// The frame for an address, validated against the executable regions: none when the
// address lies outside every one. For walkers that find frames some other way.
std::optional<Frame> layoutFrame(const Layout& layout, std::uint64_t address, bool returnAddress);
enum class Arch : std::uint8_t { X86_64, Arm64 };
Arch hostArch();
// Follows the frame-pointer chain from `regs`. Return addresses are validated
// against the executable regions; anything else ends the walk with a reason.
Walk walkFrames(const ReadMemory& read, const Layout& layout, const Regs& regs, Arch arch,
                const std::function<bool()>& expired = {});

enum class Role : std::uint8_t { Fault, Main, Other };
enum class Missing : std::uint8_t {
    None,
    PtraceDenied,
    ThreadExited,
    Deadline,
    RegistersUnreadable,
    MapsUnreadable,
    Unsupported,
    NotCollected,
    AccessDenied,  // the OS refused access to the thread (Windows)
};
struct ThreadTrace {
    Role role = Role::Other;
    Missing missing = Missing::NotCollected;  // None once frames were walked
    Walk walk;
    // What the process registered this thread as (execution table, topic 5); None for a
    // thread nobody registered: a driver's callback thread, a plug-in's own threads.
    diagctx::ThreadRole registered = diagctx::ThreadRole::None;
    std::uint16_t worker = 0;  // the N of audio-live-N and audio-bg-N, else 0
    // The innermost plug-in or module entry the thread was inside when it was stopped
    // (execution scopes, topic 5b); None when it was inside none, or none could be tied to
    // it. `entryCell` and `entryGeneration` say which instance's cell, for the inventory.
    diagctx::Entry entry = diagctx::Entry::None;
    std::uint32_t entryCell = diagctx::kNoCell;
    std::uint16_t entryGeneration = 0;
};
// Whether the execution table was found and usable.
enum class TableState : std::uint8_t {
    NotOffered,  // the worker published no address (it never reported ready)
    Unreadable,  // the read failed or ran into a limit
    Invalid,     // read, but not a table this build wrote
    Read,
};
struct TraceSnapshot {
    std::vector<Module> modules;
    // The executable regions the modules were found from (sorted, disjoint): what turns an
    // address inside a plug-in's binary into the module that holds it (the inventory's build).
    std::vector<Region> regions;
    std::vector<ThreadTrace> threads;  // fault first, then main, then by thread id
    std::size_t dropped = 0;           // threads omitted beyond kMaxThreads
    bool truncatedModules = false;
    TableState table = TableState::NotOffered;
    // Threads that tried to register and found the table full (its own count); their
    // roles are unknown.
    std::uint32_t tableThreadsDropped = 0;
    // The table's other counters, as read: instances that found no cell, and scopes whose
    // stack address did not fit a word. Entries open in the table that no stopped thread
    // claimed (its thread was not read, or the word names no address) are `unplacedEntries`.
    std::uint32_t tableUntracked = 0, tableScopeOverflow = 0, unplacedEntries = 0;
};

// What a platform provides. `stop` interrupts a thread and returns its registers,
// leaving it stopped until `release`; the collector releases each thread as soon as
// its stack is walked, and every stopped thread on any exit path.
struct TraceOps {
    std::function<bool()> expired;                     // the hold budget is spent
    std::function<std::optional<std::string>()> maps;  // mappings text, none if unreadable
    ReadMemory read;
    // Optional replacements for platforms whose memory map is not a maps file or whose
    // frames are not found by frame pointers (Windows). `layout` supersedes `maps`;
    // `walk` supersedes the frame-pointer walker for every thread, the faulting one
    // included, and receives the thread id so it can use that thread's own context.
    std::function<std::optional<Layout>()> layout;
    std::function<Walk(std::uint64_t tid, const Regs& regs, const Layout&)> walk;
    std::function<std::vector<std::uint64_t>()> threads;  // ascending thread ids
    std::function<Missing(std::uint64_t tid, Regs& regs)> stop;
    std::function<void(std::uint64_t tid)> release;
    // The stack that holds `sp`, [low, high), for a thread that registered none. Optional:
    // without it the mappings list is used (`maps`); with neither, an unregistered thread
    // is attributed nothing.
    std::function<std::optional<std::pair<std::uint64_t, std::uint64_t>>(std::uint64_t sp)>
        stackRegion;
};
// Collects the fault thread (registers supplied by the capsule), the main thread
// (`mainTid`) and the others. Stops at `expired` with the rest marked unavailable.
// `tableAddress` is the worker's execution table (0: none): it is read once, right after
// the fault thread's walk and before any other thread is stopped, and read even if the
// walk spent the deadline, so what the deadline cuts is other threads' traces and never
// the table. Its thread slots label the threads by role.
TraceSnapshot collectTrace(const TraceOps& ops, std::uint64_t faultTid, const Regs& faultRegs,
                           std::uint64_t mainTid, Arch arch, std::uint64_t tableAddress = 0);

// The role string a report shows for a thread. The fault thread is always "fault" (what
// it was registered as goes in "registered"); any other thread is its registered role,
// "model" for the main thread, else "other".
std::string roleString(const ThreadTrace& thread);

// Report JSON: top-level "modules" and "threads" values, and the limit strings
// to add to "unavailable". Offsets are hex strings; frames without module identity
// carry no address.
json::Value modulesJson(const TraceSnapshot& trace);
json::Value threadsJson(const TraceSnapshot& trace);
std::vector<std::string> traceLimits(const TraceSnapshot& trace);
// Strict nested validation of a report's "modules" and "threads" values.
bool traceShape(const json::Value* modules, const json::Value& threads);

// A macOS trace capsule (capsule.h) as a snapshot. The capsule comes from the failing
// process, so it is untrusted: every count, role, reason and range is validated, and
// the bytes must be consumed exactly. Addresses become module-relative offsets here, and
// an address outside every module is kept as a frame without identity. Fault first, then
// main, then the rest in capsule order. Nothing for a malformed capsule.
std::optional<TraceSnapshot> parseCapsule(const std::uint8_t* bytes, std::size_t size);

// Test evidence only. A collector that cannot be tried locally (Windows) writes what it
// saw, one line at a time, to the sink a test installed in the supervisor process; with
// no sink nothing is formatted or written. Never set in the product.
using EvidenceSink = void (*)(const char* line);
void setTraceEvidence(EvidenceSink sink) noexcept;
bool traceEvidenceEnabled() noexcept;
void traceEvidence(const char* line);

// The platform collector, run by the supervisor on the terminal packet inside the
// terminal hold; it must return well before the worker's one-second wait ends.
TraceSnapshot collectProcessTrace(Native& native, const Packet& terminal);
}  // namespace supple::diagnostics::detail
