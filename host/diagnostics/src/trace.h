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
};
struct ThreadTrace {
    Role role = Role::Other;
    Missing missing = Missing::NotCollected;  // None once frames were walked
    Walk walk;
};
struct TraceSnapshot {
    std::vector<Module> modules;
    std::vector<ThreadTrace> threads;  // fault first, then main, then by thread id
    std::size_t dropped = 0;           // threads omitted beyond kMaxThreads
    bool truncatedModules = false;
};

// What a platform provides. `stop` interrupts a thread and returns its registers,
// leaving it stopped until `release`; the collector releases each thread as soon as
// its stack is walked, and every stopped thread on any exit path.
struct TraceOps {
    std::function<bool()> expired;                     // the hold budget is spent
    std::function<std::optional<std::string>()> maps;  // mappings text, none if unreadable
    ReadMemory read;
    std::function<std::vector<std::uint64_t>()> threads;  // ascending thread ids
    std::function<Missing(std::uint64_t tid, Regs& regs)> stop;
    std::function<void(std::uint64_t tid)> release;
};
// Collects the fault thread (registers supplied by the capsule), the main thread
// (`mainTid`) and the others. Stops at `expired` with the rest marked unavailable.
TraceSnapshot collectTrace(const TraceOps& ops, std::uint64_t faultTid, const Regs& faultRegs,
                           std::uint64_t mainTid, Arch arch);

// Report JSON: top-level "modules" and "threads" values, and the limit strings
// to add to "unavailable". Offsets are hex strings; frames without module identity
// carry no address.
json::Value modulesJson(const TraceSnapshot& trace);
json::Value threadsJson(const TraceSnapshot& trace);
std::vector<std::string> traceLimits(const TraceSnapshot& trace);
// Strict nested validation of a report's "modules" and "threads" values.
bool traceShape(const json::Value* modules, const json::Value& threads);

// The platform collector, run by the supervisor on the terminal packet inside the
// terminal hold; it must return well before the worker's one-second wait ends.
TraceSnapshot collectProcessTrace(const Native& native, const Packet& terminal);
}  // namespace supple::diagnostics::detail
