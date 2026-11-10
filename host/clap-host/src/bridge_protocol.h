// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: the plugin bridge protocol (ADR-0013), shared by the host side
// (bridge_client.cpp) and the bridge executable (bridge/bridge_main.cpp).
//
// POSIX control: socketpair, [u32 size][u32 op/status][u32 instance][payload]
// with SCM_RIGHTS resources. Windows control: overlapped pipe; an additional
// u64 recipient-owned handle follows the prefix. See ADR-0037 for versioning.
// The pointer-free Header/event/audio layout is common to all platforms.
// Linux uses memfd/shared futexes; macOS uses unlinked shm/shared address wakes;
// Windows uses a pagefile mapping and duplicated request/done auto-reset events.
#pragma once

#include "bridge_platform.h"
#include "supple/base/rt.h"
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#elif defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#endif
#include <cerrno>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "supple/base/result.h"
#include "supple/host/clap_host.h"

namespace supple::host::bridge {

inline constexpr std::uint32_t kMagic = 0x31474253;  // "SBG1"
// 6 (unreleased): error details, restart failures, complete output event
// metadata in shared memory, an idle-output drain, process wake and ordered clears.
// 8: auxiliary output ports (ADR-0021) in Create and the data plane.
// 9: the Activate reply carries the widths the plug-in settled on.
// 10: the Header carries the audio thread's scheduling (ADR-0026).
// 11: a versioned outward notification subscription and process-level rearm.
// 12: a main-thread plugin-status query (no data-plane change).
// 13: read-only factory patches through semantic-params.
// 17/18: Floating carries a Cocoa owner window number (ADR-0041).
// 18/19: EditorScale renegotiates a live display-scale change (ADR-0040).
// 19/20: Cocoa editor window frames: EditorOpen takes one, EditorRequests and
// EditorClose report one (ADR-0040).
// 20/21: plug-in note and MIDI output (ADR-0068): the Create reply says whether
// the plug-in outputs notes; the Header carries the host sink's capacity and
// the unsupported-output count; OutputEvent gains its MIDI kinds and bytes.
#if defined(_WIN32)
inline constexpr std::uint32_t kVersion = 21;  // Windows runs one ahead: native control handles
#else
inline constexpr std::uint32_t kVersion = 20;
#endif
inline constexpr int kControlFd = 3;  // the bridge's end of the socketpair
inline constexpr std::uint32_t kMaxChannels = 16;
// Auxiliary output channels carried (ADR-0021); a plug-in's further ones go
// to scratch in the bridge.
inline constexpr std::uint32_t kMaxAuxChannels = 128;
inline constexpr std::uint32_t kMaxAuxPorts = 128;
inline constexpr std::uint32_t kMaxEvents = 2048;
inline constexpr std::uint32_t kMaxOutEvents = 1024;
inline constexpr std::uint32_t kMaxParamClears = 4096;
inline constexpr std::uint32_t kMaxMessage = 64u << 20;

// --- control plane -----------------------------------------------------------------

enum class Op : std::uint32_t {
    Create = 1,  // path, id, identity → info
    Activate,    // rate, min, max (+ memfd) → ok
    Deactivate,
    Params,  // → [ParamInfo]
    ParamValue,
    ParamText,
    ParamFromText,
    SetParam,
    Latency,  // → u32 latency, u32 tail, u8 reportsTail
    SemanticSchema,
    GetPatch,
    SetPatch,
    Describe,
    SaveState,
    LoadState,
    Service,  // serviceMainThread → didWork, notifications, log lines, needsAudioFlush
    Destroy,
    // The two parameter domains a VST3 has (identity for CLAP): main-thread
    // conversions for a host that holds a value in the other domain. Appended
    // so the numbers of the ops above do not change.
    NormalizedParam,  // id, plain → normalized
    PlainParam,       // id, normalized → plain
    // Native editor: X11/Win32 embedded; Cocoa floats in a bridge-owned window.
    EditorOpen,       // u8 parent kind, u64 parent handle, f64 scale, frame → u32 width, u32
                      // height, u8 resizable
    EditorResize,     // u32 width, u32 height → the accepted size (as EditorOpen)
    EditorClose,      // → frame (the window's final one)
    EditorRequests,   // → u8 resize, u8 closed, u32 width, u32 height, u8 show, u8 hide, frame
    TakeIdleOutputs,  // → u64 dropped, u32 count, [OutputEvent]
    SubscribeNotifications,  // process id 0: u32 version (+ eventfd) → u32 version
    AckNotifications,        // process id 0: no payload → ok
    PluginStatus,            // no payload → JSON text (null if unsupported)
    FactoryPatches,          // no payload → canonical patch array JSON text
    EditorScale,             // f64 scale → the size for the window (as EditorOpen)
};

enum class Reply : std::uint32_t { Ok = 0, Error = 1 };

// Byte-oriented encoding of the payloads.
class Writer {
  public:
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i16(std::int16_t v) { raw(&v, 2); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void f64(double v) { raw(&v, 8); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void blob(const std::vector<std::uint8_t>& b) {
        u32(static_cast<std::uint32_t>(b.size()));
        raw(b.data(), b.size());
    }
    void error(const Error& e) {
        str(e.code);
        str(e.message);
        str(e.path);
        str(e.hint);
        u32(static_cast<std::uint32_t>(e.details.size()));
        for (const auto& detail : e.details) error(detail);
    }

  private:
    void raw(const void* p, std::size_t n) {
        if (!n) return;
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
};

class Reader {
  public:
    Reader(const std::uint8_t* data, std::size_t size)
        : p_(size ? data : &empty_), end_(p_ + size) {}
    bool ok() const { return ok_; }
    void fail() { ok_ = false; }
    bool done() const { return ok_ && p_ == end_; }
    void skipString() {
        const auto n = u32();
        skip(n);
    }
    void skip(std::size_t n) {
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n)
            ok_ = false;
        else
            p_ += n;
    }
    std::uint8_t u8() {
        std::uint8_t v = 0;
        raw(&v, 1);
        return v;
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::int16_t i16() {
        std::int16_t v = 0;
        raw(&v, 2);
        return v;
    }
    std::int32_t i32() {
        std::int32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::int64_t i64() {
        std::int64_t v = 0;
        raw(&v, 8);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        raw(&v, 8);
        return v;
    }
    double f64() {
        double v = 0;
        raw(&v, 8);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    std::vector<std::uint8_t> blob() {
        const std::uint32_t n = u32();
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
            ok_ = false;
            return {};
        }
        std::vector<std::uint8_t> b(p_, p_ + n);
        p_ += n;
        return b;
    }
    Error error(unsigned depth = 0) {
        if (depth >= 32) {
            ok_ = false;
            return {};
        }
        std::string code = str(), message = str(), path = str(), hint = str();
        Error result(std::move(code), std::move(message), std::move(path), std::move(hint));
        const auto count = u32();
        if (count > static_cast<std::size_t>(end_ - p_) / 20) {
            ok_ = false;
            return result;
        }
        for (std::uint32_t i = 0; i < count && ok_; ++i) result.details.push_back(error(depth + 1));
        return result;
    }

  private:
    void raw(void* out, std::size_t n) {
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
            ok_ = false;
            return;
        }
        std::memcpy(out, p_, n);
        p_ += n;
    }
    inline static constexpr std::uint8_t empty_ = 0;
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

void writeOutputEvent(Writer& writer, const OutputEvent& event);
OutputEvent readOutputEvent(Reader& reader);

using Deadline = std::chrono::steady_clock::time_point;
inline Deadline deadlineAfter(int timeoutMs) {
    return timeoutMs < 0 ? Deadline::max()
                         : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
}
// An optional editor window frame: u8 present, i32 x, i32 y, u32 width, u32 height
// (17 bytes, fields zero when absent). Sizes beyond kMaxEditorFrame are invalid.
inline constexpr std::uint32_t kMaxEditorFrame = 1u << 20;
inline void writeFrame(Writer& w, const std::optional<Instance::EditorFrame>& f) {
    w.u8(f ? 1 : 0);
    w.i32(f ? f->x : 0);
    w.i32(f ? f->y : 0);
    w.u32(f ? f->width : 0);
    w.u32(f ? f->height : 0);
}
// The reader is left not ok when the flag or a size is out of range.
inline std::optional<Instance::EditorFrame> readFrame(Reader& r) {
    const auto present = r.u8();
    Instance::EditorFrame f;
    f.x = r.i32();
    f.y = r.i32();
    f.width = r.u32();
    f.height = r.u32();
    if (present > 1 || f.width > kMaxEditorFrame || f.height > kMaxEditorFrame ||
        (!present && (f.x || f.y || f.width || f.height)))
        r.fail();
    return present == 1 ? std::optional<Instance::EditorFrame>(f) : std::nullopt;
}
bool validRequest(Op op, Reader r);
bool validReply(Op op, Reader r);

// Writes one whole frame ([u32 size][u32 head][u32 instance][payload]) with an optional
// file descriptor; reads one (size-checked). False when the peer is gone.
bool sendFrame(Control socket, std::uint32_t head, std::uint32_t instance,
               const std::vector<std::uint8_t>& payload, Resource fd = noResource,
               Deadline deadline = Deadline::max());
bool recvFrame(Control socket, std::uint32_t& head, std::uint32_t& instance,
               std::vector<std::uint8_t>& payload, Resource* fd = nullptr, int timeoutMs = -1,
               int* descriptorCount = nullptr);
bool recvFrameUntil(Control socket, std::uint32_t& head, std::uint32_t& instance,
                    std::vector<std::uint8_t>& payload, Resource* fd, Deadline deadline,
                    int* descriptorCount = nullptr);

// --- data plane ---------------------------------------------------------------------

// Both endpoints use this native, pointer-free ABI; Layout accounts for its
// v6 size. Control-plane idle events encode each field without ABI padding.
static_assert(std::is_trivially_copyable_v<OutputEvent>);
static_assert(std::is_standard_layout_v<OutputEvent>);
static_assert(sizeof(OutputEvent) == 40);

// Commands the host runs on the bridge's audio thread, in order.
enum class Cmd : std::uint32_t { Process = 1, StartProcessing, StopProcessing, Reset, Flush, Quit };

struct Header {
    std::uint32_t magic, version;
    std::uint32_t maxFrames, inChannels, outChannels;
    std::uint32_t keyChannels, keyActive;            // the sidechain input (ADR-0018)
    std::uint32_t auxOutChannels;                    // auxiliary outputs (ADR-0021)
    alignas(64) std::atomic<std::uint32_t> request;  // bumped by the host (futex word)
    alignas(64) std::atomic<std::uint32_t> done;     // = request when finished (futex word)
    alignas(64) std::uint32_t cmd;
    std::uint32_t frames;
    std::int64_t steadyTime;
    std::uint32_t result;  // 1: ok
    std::uint32_t hasTransport;
    std::uint32_t hasKey;  // this block carries a key (else the plug-in gets silence)
    Transport transport;
    std::uint32_t eventCount;
    // Process: whether the host passed a sink, and the room it has left (at
    // most kMaxOutEvents), so the bridged plug-in sees the same pushes accepted
    // and refused as it would in process (ADR-0068).
    std::uint32_t outSink, outEventCapacity;
    std::uint32_t outEventCount, outEventsDropped, outEventsUnsupported;
    std::uint32_t processing;  // the plugin's processing state after the command
    // StartProcessing: the scheduling the audio thread takes (ThreadPolicy's
    // kind and priority; kind 0, None, keeps the current one).
    std::uint32_t audioPolicy, audioPriority;
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// Offsets of the arenas and buffers in the region.
struct Layout {
    std::size_t events, outEvents, inAudio, keyAudio, outAudio, auxOutAudio, size;
    std::uint32_t maxFrames, inChannels, outChannels, keyChannels, auxOutChannels;
    Layout(std::uint32_t frames, std::uint32_t in, std::uint32_t out, std::uint32_t key = 0,
           std::uint32_t auxOut = 0)
        : maxFrames(frames),
          inChannels(in),
          outChannels(out),
          keyChannels(key),
          auxOutChannels(auxOut) {
        auto align = [](std::size_t n) { return (n + 63) & ~std::size_t{63}; };
        events = align(sizeof(Header));
        outEvents = align(events + sizeof(Event) * kMaxEvents);
        inAudio = align(outEvents + sizeof(OutputEvent) * kMaxOutEvents);
        keyAudio = align(inAudio + sizeof(float) * in * frames);
        outAudio = align(keyAudio + sizeof(float) * key * frames);
        auxOutAudio = align(outAudio + sizeof(float) * out * frames);
        size = align(auxOutAudio + sizeof(float) * auxOut * frames);
    }
};

#if !defined(_WIN32)
inline long futexWait(std::atomic<std::uint32_t>& word, std::uint32_t expected, int timeoutMs) {
#if defined(__linux__)
    timespec ts{timeoutMs / 1000, static_cast<long>(timeoutMs % 1000) * 1000000L};
    return ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAIT, expected,
                     timeoutMs < 0 ? nullptr : &ts, nullptr, 0);
#elif defined(__APPLE__)
    if (timeoutMs < 0)
        return os_sync_wait_on_address(&word, expected, sizeof(expected),
                                       OS_SYNC_WAIT_ON_ADDRESS_SHARED);
    return os_sync_wait_on_address_with_timeout(
        &word, expected, sizeof(expected), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
        OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(timeoutMs) * 1'000'000);
#endif
}
inline void futexWake(std::atomic<std::uint32_t>& word) {
#if defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE, 1, nullptr, nullptr,
              0);
#elif defined(__APPLE__)
    os_sync_wake_by_address_any(&word, sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#endif
}
#endif
// Private per-instance auto-reset events on Windows; zero-resource wrapper on POSIX.
// One waiter per direction. Release/acquire on the words, never event state,
// publishes commands and outputs. Stale event signals only cause a recheck.
class SharedSignals {
  public:
    SharedSignals() = default;
    SharedSignals(const SharedSignals&) = delete;
    SharedSignals& operator=(const SharedSignals&) = delete;
    ~SharedSignals() { clear(); }
    bool create() {
#if defined(_WIN32)
        clear();
        request_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        done_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!request_ || !done_) {
            clear();
            return false;
        }
#endif
        return true;
    }
    void clear() {
#if defined(_WIN32)
        if (request_) CloseHandle(request_);
        if (done_ && done_ != request_) CloseHandle(done_);
        request_ = done_ = nullptr;
#endif
    }
#if defined(_WIN32)
    bool adopt(Resource request, Resource done) {
        clear();
        request_ = reinterpret_cast<HANDLE>(request);
        done_ = reinterpret_cast<HANDLE>(done);
        // Validate event handles before any audio thread can use them.
        if (!request_ || !done_ || request_ == INVALID_HANDLE_VALUE ||
            done_ == INVALID_HANDLE_VALUE || request_ == done_ || !SetEvent(request_) ||
            !SetEvent(done_)) {
            clear();
            return false;
        }
        ResetEvent(request_);
        ResetEvent(done_);
        return true;
    }
    Resource resource(bool completion) const {
        return reinterpret_cast<Resource>(completion ? done_ : request_);
    }
#endif
    long wait(std::atomic<std::uint32_t>& word, std::uint32_t seen, int timeoutMs,
              bool completion = false) {
#if defined(_WIN32)
        if (word.load(std::memory_order_acquire) != seen) {
            errno = EAGAIN;
            return -1;
        }
        const auto result =
            WaitForSingleObject(completion ? done_ : request_,
                                timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
        if (result == WAIT_OBJECT_0) return 0;
        errno = result == WAIT_TIMEOUT ? ETIMEDOUT : EIO;
        return -1;
#else
        (void)completion;
#if defined(__APPLE__)
        if (word.load(std::memory_order_acquire) != seen) {
            errno = EAGAIN;
            return -1;
        }
#endif
        return futexWait(word, seen, timeoutMs);
#endif
    }
    void wake(std::atomic<std::uint32_t>& word, bool completion = false) SUPPLE_RT {
        // All native wakes are one nonblocking syscall, including shutdown
        // invoked from main through this RT-annotated entry point.
        const rt::ScopedSanctionedSyscall sanctioned;
#if defined(_WIN32)
        (void)word;
        SetEvent(completion ? done_ : request_);
#else
        (void)completion;
        futexWake(word);
#endif
    }

  private:
#if defined(_WIN32)
    HANDLE request_ = nullptr, done_ = nullptr;
#endif
};

}  // namespace supple::host::bridge
