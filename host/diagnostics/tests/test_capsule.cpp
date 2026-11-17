// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The macOS trace capsule (capsule.h): the stack walker the failing process runs in its
// terminal hold, the framing over the private channel, and the supervisor's strict
// parser. The Mach calls that gather threads and modules are not here; they only run
// on macOS, and native CI covers them.
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "../src/capsule.h"
#include "../src/trace.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
using namespace supple::diagnostics::detail;
namespace {
// Serializes the used part of a capsule exactly as sendCapsule writes it.
std::unique_ptr<CapsuleData> fresh() {
    auto data = std::make_unique<CapsuleData>();
    std::memset(data.get(), 0, sizeof(CapsuleData));
    data->header.magic = kCapsuleMagic;
    return data;
}
std::vector<std::uint8_t> encode(const CapsuleData& data) {
    std::vector<std::uint8_t> bytes;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    };
    // A mutated header can claim more than the arrays hold; the claim is written as it
    // stands and only what exists is copied. Cases that must be refused by a count guard
    // alone use `claimed` below instead, which holds every entry the header claims.
    put(&data.header, sizeof(CapsuleHeader));
    put(data.modules,
        std::min<std::size_t>(data.header.moduleCount, kCapsuleModules) * sizeof(CapsuleModule));
    for (std::uint32_t i = 0; i < std::min<std::size_t>(data.header.threadCount, kCapsuleThreads);
         ++i) {
        put(&data.threads[i].head, sizeof(CapsuleThread));
        put(data.threads[i].frames,
            std::min<std::size_t>(data.threads[i].head.frameCount, kCapsuleFrames) *
                sizeof(std::uint64_t));
    }
    return bytes;
}
// A capsule whose header claims `modules` modules and `threads` threads, followed by exactly
// that many valid entries: non-overlapping modules, a walked fault entry with `faultFrames`
// frames first, then exited threads. Nothing in it is wrong except, when a count is above its
// limit, that count, so only the parser's count guard can refuse it.
std::vector<std::uint8_t> claimed(std::uint32_t modules, std::uint32_t threads,
                                  std::uint32_t faultFrames) {
    std::vector<std::uint8_t> bytes;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    };
    CapsuleHeader header{};
    header.magic = kCapsuleMagic;
    header.moduleCount = modules;
    header.threadCount = threads;
    put(&header, sizeof header);
    for (std::uint32_t i = 0; i < modules; ++i) {
        CapsuleModule m{};
        m.start = 0x10000 + std::uint64_t{i} * 0x1000;
        m.end = m.start + 0x800;
        put(&m, sizeof m);
    }
    for (std::uint32_t t = 0; t < threads; ++t) {
        CapsuleThread head{};
        head.role = static_cast<std::uint8_t>(t == 0 ? Role::Fault : Role::Other);
        head.missing = static_cast<std::uint8_t>(t == 0 ? Missing::None : Missing::ThreadExited);
        head.stop = static_cast<std::uint8_t>(Stop::End);
        head.frameCount = t == 0 ? faultFrames : 0;
        put(&head, sizeof head);
        for (std::uint32_t f = 0; f < head.frameCount; ++f) {
            const std::uint64_t frame = 0x10100 + f;
            put(&frame, sizeof frame);
        }
    }
    return bytes;
}
void addModule(CapsuleData& data, std::uint64_t bias, std::uint64_t start, std::uint64_t end,
               int uuid = -1) {
    auto& m = data.modules[data.header.moduleCount++];
    m.bias = bias;
    m.start = start;
    m.end = end;
    if (uuid >= 0) {
        m.hasUuid = 1;
        for (int i = 0; i < 16; ++i) m.uuid[i] = static_cast<std::uint8_t>(uuid + i);
    }
}
void addThread(CapsuleData& data, Role role, Missing missing, Stop stop,
               std::vector<std::uint64_t> frames) {
    auto& t = data.threads[data.header.threadCount++];
    t.head.role = static_cast<std::uint8_t>(role);
    t.head.missing = static_cast<std::uint8_t>(missing);
    t.head.stop = static_cast<std::uint8_t>(stop);
    t.head.frameCount = static_cast<std::uint32_t>(frames.size());
    std::copy(frames.begin(), frames.end(), t.frames);
}
#if !defined(_WIN32)
// A sparse fake stack of frame records {previous fp, return address}. The in-process walker
// that reads it exists only where the capsule layer does (not on Windows).
struct Stack {
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> records;
    static bool read(void* context, std::uint64_t at, void* out, std::size_t n) {
        const auto& map = static_cast<Stack*>(context)->records;
        const auto it = map.find(at);
        if (it == map.end() || n != 16) return false;
        std::memcpy(out, &it->second, 16);
        return true;
    }
    StackReader reader() { return StackReader{&Stack::read, this}; }
};
#endif
}  // namespace

#if !defined(_WIN32)
TEST_CASE("the in-process walker follows a frame-pointer chain and records why it stopped") {
    Stack stack;
    stack.records[0x7000] = {0x7100, 0x401000};
    stack.records[0x7100] = {0x7200, 0x402000};
    stack.records[0x7200] = {0, 0x403000};
    auto thread = std::make_unique<CapsuleThreadData>();
    std::memset(thread.get(), 0, sizeof(CapsuleThreadData));
    walkStack(stack.reader(), {0x400100, 0x6ff0, 0x7000}, false, *thread, 0);
    REQUIRE(thread->head.frameCount == 4);
    CHECK(thread->frames[0] == 0x400100);  // the pc itself
    CHECK(thread->frames[1] == 0x401000);
    CHECK(thread->frames[3] == 0x403000);
    CHECK(thread->head.stop == static_cast<std::uint8_t>(Stop::End));
}

TEST_CASE("the in-process walker stops on every kind of corrupt chain") {
    const auto run = [](Stack& stack, CapsuleRegs regs, bool arm64 = false) {
        auto thread = std::make_unique<CapsuleThreadData>();
        std::memset(thread.get(), 0, sizeof(CapsuleThreadData));
        walkStack(stack.reader(), regs, arm64, *thread, 0);
        return std::make_pair(thread->head.frameCount, static_cast<Stop>(thread->head.stop));
    };
    Stack stack;
    CHECK(run(stack, {0x400100, 0x6ff0, 0}) == std::make_pair(1u, Stop::End));  // no chain
    CHECK(run(stack, {0x400100, 0x6ff0, 0x7001}) ==
          std::make_pair(1u, Stop::BadFramePointer));  // misaligned
    CHECK(run(stack, {0x400100, 0x7100, 0x7000}) ==
          std::make_pair(1u, Stop::BadFramePointer));  // below the stack pointer
    CHECK(run(stack, {0x400100, 0x6ff0, 0x7000}) ==
          std::make_pair(1u, Stop::Unreadable));  // nothing mapped there
    stack.records[0x7000] = {0x7000, 0x401000};   // points at itself
    CHECK(run(stack, {0x400100, 0x6ff0, 0x7000}) == std::make_pair(2u, Stop::Cycle));
    stack.records[0x7000] = {0x7000 + (std::uint64_t{1} << 31), 0x401000};
    stack.records[0x7000 + (std::uint64_t{1} << 31)] = {0, 0x402000};
    CHECK(run(stack, {0x400100, 0x6ff0, 0x7000}) ==
          std::make_pair(2u, Stop::BadFramePointer));  // a huge jump
    stack.records.clear();
    stack.records[0x7000] = {0x7100, 0};  // a null return address ends it
    CHECK(run(stack, {0x400100, 0x6ff0, 0x7000}) == std::make_pair(1u, Stop::End));
    // Pointer-authentication bits are stripped on arm64 and kept on x86-64.
    stack.records[0x7000] = {0, 0xabcd000000401000ULL};
    auto thread = std::make_unique<CapsuleThreadData>();
    std::memset(thread.get(), 0, sizeof(CapsuleThreadData));
    walkStack(stack.reader(), {0x400100, 0x6ff0, 0x7000}, true, *thread, 0);
    CHECK(thread->frames[1] == 0x401000);
    walkStack(stack.reader(), {0x400100, 0x6ff0, 0x7000}, false, *thread, 0);
    CHECK(thread->frames[1] == 0xabcd000000401000ULL);
}

TEST_CASE("the in-process walker is capped at 128 frames and honours its deadline") {
    Stack stack;
    for (std::uint64_t i = 0; i < 400; ++i)
        stack.records[0x10000 + 16 * i] = {0x10000 + 16 * (i + 1), 0x401000 + i};
    auto thread = std::make_unique<CapsuleThreadData>();
    std::memset(thread.get(), 0, sizeof(CapsuleThreadData));
    walkStack(stack.reader(), {0x400100, 0, 0x10000}, false, *thread, 0);
    CHECK(thread->head.frameCount == kCapsuleFrames);
    CHECK(thread->head.stop == static_cast<std::uint8_t>(Stop::Depth));
    // A deadline already behind us ends the walk at its first check, with what it has.
    walkStack(stack.reader(), {0x400100, 0, 0x10000}, false, *thread, 1);
    CHECK(thread->head.stop == static_cast<std::uint8_t>(Stop::Deadline));
    CHECK(thread->head.frameCount >= 1);
    CHECK(thread->head.frameCount < kCapsuleFrames);
}
#endif

TEST_CASE("a capsule becomes module identity and offsets, fault first") {
    auto data = fresh();
    addModule(*data, 0x100000, 0x100000, 0x180000, 0x10);
    addModule(*data, 0x7ff000000000, 0x7ff000000000, 0x7ff000100000);  // no UUID
    addThread(*data, Role::Fault, Missing::None, Stop::Cycle, {0x100300, 0xdead0000, 0x100310});
    addThread(*data, Role::Other, Missing::None, Stop::End, {0x100200, 0x7ff000000040, 0x100210});
    addThread(*data, Role::Main, Missing::Deadline, Stop::End, {});
    const auto bytes = encode(*data);
    const auto snapshot = parseCapsule(bytes.data(), bytes.size());
    REQUIRE(snapshot);
    REQUIRE(snapshot->modules.size() == 2);
    CHECK(*snapshot->modules[0].id == "101112131415161718191a1b1c1d1e1f");
    CHECK_FALSE(snapshot->modules[1].id);
    REQUIRE(snapshot->threads.size() == 3);
    CHECK(snapshot->threads[0].role == Role::Fault);
    CHECK(snapshot->threads[1].role == Role::Main);
    CHECK(snapshot->threads[2].role == Role::Other);
    const auto& fault = snapshot->threads[0];
    CHECK(fault.missing == Missing::None);
    REQUIRE(fault.walk.frames.size() == 3);
    CHECK(fault.walk.stop == Stop::Cycle);  // what the worker said, untouched
    CHECK(fault.walk.frames[0].module == 0);
    CHECK(fault.walk.frames[0].offset == 0x300);
    CHECK(fault.walk.frames[1].module == -1);  // 0xdead0000 is in no module: no identity
    CHECK(fault.walk.frames[1].returnAddress);
    CHECK(fault.walk.frames[2].offset == 0x310);
    CHECK_FALSE(fault.walk.frames[0].returnAddress);
    CHECK(snapshot->threads[1].missing == Missing::Deadline);
    CHECK(snapshot->threads[1].walk.frames.empty());
    const auto& other = snapshot->threads[2];
    REQUIRE(other.walk.frames.size() == 3);
    CHECK(other.walk.frames[1].module == 1);
    CHECK(other.walk.frames[1].offset == 0x40);
    CHECK(other.walk.frames[1].returnAddress);
    CHECK(other.walk.stop == Stop::End);
    // A pc outside every module is recorded without identity, and the walk goes on.
    auto odd = fresh();
    addModule(*odd, 0, 0x1000, 0x2000);
    addThread(*odd, Role::Fault, Missing::None, Stop::End, {0x9000, 0x1100});
    const auto oddBytes = encode(*odd);
    const auto oddSnapshot = parseCapsule(oddBytes.data(), oddBytes.size());
    REQUIRE(oddSnapshot);
    REQUIRE(oddSnapshot->threads[0].walk.frames.size() == 2);
    CHECK(oddSnapshot->threads[0].walk.frames[0].module == -1);
    CHECK(oddSnapshot->threads[0].walk.frames[1].module == 0);
    // Threads beyond the cap are counted with their flag, and the count survives.
    auto counted = fresh();
    addThread(*counted, Role::Fault, Missing::None, Stop::End, {0x1000});
    counted->header.flags = kCapsuleThreadsDropped;
    counted->header.dropped = 3;
    const auto countedBytes = encode(*counted);
    const auto countedSnapshot = parseCapsule(countedBytes.data(), countedBytes.size());
    REQUIRE(countedSnapshot);
    CHECK(countedSnapshot->dropped == 3);
    // A capsule with no thread records nothing, not even the fault entry every capsule
    // starts with: it is refused (review round 29, F3).
    CapsuleHeader header{};
    header.magic = kCapsuleMagic;
    CHECK_FALSE(parseCapsule(reinterpret_cast<const std::uint8_t*>(&header), sizeof(header)));
}

TEST_CASE("a capsule carries the role each thread registered as") {
    using supple::diagctx::ThreadRole;
    auto data = fresh();
    addModule(*data, 0x100000, 0x100000, 0x180000, 0x10);
    addThread(*data, Role::Fault, Missing::None, Stop::End, {0x100300});
    addThread(*data, Role::Main, Missing::None, Stop::End, {0x100200});
    addThread(*data, Role::Other, Missing::None, Stop::End, {0x100100});
    addThread(*data, Role::Other, Missing::Deadline, Stop::End, {});
    data->threads[0].head.registered = static_cast<std::uint8_t>(ThreadRole::AudioRender);
    data->threads[1].head.registered = static_cast<std::uint8_t>(ThreadRole::Model);
    data->threads[2].head.registered = static_cast<std::uint8_t>(ThreadRole::AudioLive);
    data->threads[2].head.worker = 2;
    const auto bytes = encode(*data);
    const auto snapshot = parseCapsule(bytes.data(), bytes.size());
    REQUIRE(snapshot);
    REQUIRE(snapshot->threads.size() == 4);
    CHECK(snapshot->threads[0].registered == ThreadRole::AudioRender);
    CHECK(snapshot->threads[1].registered == ThreadRole::Model);
    CHECK(snapshot->threads[2].registered == ThreadRole::AudioLive);
    CHECK(snapshot->threads[2].worker == 2);
    CHECK(snapshot->threads[3].registered == ThreadRole::None);  // unregistered: "other"
    CHECK(roleString(snapshot->threads[2]) == "audio_live");
    CHECK(roleString(snapshot->threads[3]) == "other");
    // The registered role of an unreadable thread is still reported.
    data->threads[3].head.registered = static_cast<std::uint8_t>(ThreadRole::Recorder);
    const auto cut = encode(*data);
    CHECK(parseCapsule(cut.data(), cut.size())->threads[3].registered == ThreadRole::Recorder);
}

TEST_CASE("overlapping module ranges cannot make an address ambiguous") {
    auto data = fresh();
    addModule(*data, 0, 0x1000, 0x3000);
    addModule(*data, 0, 0x2000, 0x4000);  // overlaps the first: dropped
    addThread(*data, Role::Fault, Missing::None, Stop::End, {0x2800, 0x3800});
    const auto bytes = encode(*data);
    const auto snapshot = parseCapsule(bytes.data(), bytes.size());
    REQUIRE(snapshot);
    REQUIRE(snapshot->threads[0].walk.frames.size() == 2);
    CHECK(snapshot->threads[0].walk.frames[0].module == 0);
    CHECK(snapshot->threads[0].walk.frames[1].module == -1);  // 0x3800 only in the dropped one
}

TEST_CASE("a capsule at each count limit parses and one entry more is refused by the guard") {
    // Each capsule below holds every entry its header claims, all of them valid, so the only
    // thing that can refuse the "one more" cases is the guard on that count (review round 31, F4).
    const auto parses = [](const std::vector<std::uint8_t>& bytes) {
        return parseCapsule(bytes.data(), bytes.size()).has_value();
    };
    CHECK(parses(claimed(kCapsuleModules, 1, 1)));
    CHECK(parses(claimed(1, kCapsuleThreads, 1)));
    CHECK(parses(claimed(1, 1, kCapsuleFrames)));
    CHECK_FALSE(parses(claimed(kCapsuleModules + 1, 1, 1)));  // module count
    CHECK_FALSE(parses(claimed(1, kCapsuleThreads + 1, 1)));  // thread count
    CHECK_FALSE(parses(claimed(1, 1, kCapsuleFrames + 1)));   // too many frames
}

TEST_CASE("a malformed or truncated capsule yields nothing") {
    auto good = fresh();
    addModule(*good, 0x1000, 0x1000, 0x2000, 1);
    addThread(*good, Role::Fault, Missing::None, Stop::End, {0x1100, 0x1200});
    addThread(*good, Role::Other, Missing::ThreadExited, Stop::End, {});
    const auto bytes = encode(*good);
    REQUIRE(parseCapsule(bytes.data(), bytes.size()));
    CHECK_FALSE(parseCapsule(nullptr, 0));
    // Every proper prefix is refused, and so is any trailing byte.
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK_FALSE(parseCapsule(bytes.data(), n));
    auto longer = bytes;
    longer.push_back(0);
    CHECK_FALSE(parseCapsule(longer.data(), longer.size()));
    const auto mutate = [&](const char* what, auto change) {
        CAPTURE(what);
        auto copy = std::make_unique<CapsuleData>(*good);
        change(*copy);
        const auto mutated = encode(*copy);
        CHECK_FALSE(parseCapsule(mutated.data(), mutated.size()));
    };
    mutate("magic", [](CapsuleData& d) { d.header.magic = 0; });
    mutate("flags", [](CapsuleData& d) { d.header.flags = 4; });
    mutate("reserved", [](CapsuleData& d) { d.header.reserved = 1; });
    mutate("empty range", [](CapsuleData& d) { d.modules[0].end = d.modules[0].start; });
    mutate("inverted range", [](CapsuleData& d) { d.modules[0].end = d.modules[0].start - 1; });
    mutate("uuid flag", [](CapsuleData& d) { d.modules[0].hasUuid = 2; });
    mutate("module reserved", [](CapsuleData& d) { d.modules[0].reserved = 1; });
    mutate("role", [](CapsuleData& d) { d.threads[0].head.role = 3; });
    mutate("reason", [](CapsuleData& d) { d.threads[1].head.missing = 9; });
    mutate("stop", [](CapsuleData& d) { d.threads[0].head.stop = 7; });
    mutate("thread reserved", [](CapsuleData& d) { d.threads[0].head.reserved = 1; });
    mutate("unknown registered role", [](CapsuleData& d) {
        d.threads[0].head.registered = supple::diagctx::kThreadRoleCount;
    });
    mutate("registered role out of range",
           [](CapsuleData& d) { d.threads[1].head.registered = 255; });
    mutate("worker number without a role", [](CapsuleData& d) { d.threads[0].head.worker = 1; });
    mutate("walked without frames", [](CapsuleData& d) { d.threads[0].head.frameCount = 0; });
    mutate("unavailable with frames", [](CapsuleData& d) { d.threads[1].head.frameCount = 1; });
    // The fault entry comes first, exactly once (review round 29, F3).
    mutate("no threads", [](CapsuleData& d) { d.header.threadCount = 0; });
    mutate("no fault entry",
           [](CapsuleData& d) { d.threads[0].head.role = static_cast<std::uint8_t>(Role::Main); });
    mutate("fault entry second", [](CapsuleData& d) { std::swap(d.threads[0], d.threads[1]); });
    mutate("two fault entries",
           [](CapsuleData& d) { d.threads[1].head.role = static_cast<std::uint8_t>(Role::Fault); });
    // The dropped count and its flag agree, and the count is plausible.
    mutate("dropped without its flag", [](CapsuleData& d) { d.header.dropped = 1; });
    mutate("flag without dropped", [](CapsuleData& d) { d.header.flags = kCapsuleThreadsDropped; });
    mutate("absurd dropped", [](CapsuleData& d) {
        d.header.flags = kCapsuleThreadsDropped;
        d.header.dropped = 4294967295U;
    });
}

#if !defined(_WIN32)
namespace {
Packet terminalPacket() {
    Packet p;
    p.kind = Message::Terminal;
    p.role = 3;
    std::fill(p.incident.begin(), p.incident.begin() + 32, 'a');
    return p;
}
struct Pair {
    int fds[2] = {-1, -1};
    Pair() { REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }
    ~Pair() {
        for (int fd : fds)
            if (fd >= 0) ::close(fd);
    }
    void closeSender() {
        ::close(fds[0]);
        fds[0] = -1;
    }
};
std::uint64_t soon() { return monotonicNanoseconds() + 5'000'000'000ULL; }
}  // namespace

TEST_CASE("a capsule crosses the channel intact, however large") {
    auto data = fresh();
    addModule(*data, 0x1000, 0x1000, 0x2000, 7);
    // The largest legal capsule is far beyond a socket buffer: the sender must wait for
    // the supervisor to drain it, and every byte must arrive in order.
    for (std::size_t i = 0; i < kCapsuleThreads; ++i) {
        std::vector<std::uint64_t> frames;
        for (std::size_t f = 0; f < kCapsuleFrames; ++f) frames.push_back(0x1000 + i * 16 + f % 16);
        addThread(*data, i == 0 ? Role::Fault : Role::Other, Missing::None, Stop::Depth, frames);
    }
    const auto expected = encode(*data);
    CHECK(expected.size() <= kCapsuleMaxBytes);
    Pair pair;
    const Packet base = terminalPacket();
    bool sent = false;
    std::thread sender([&] { sent = sendCapsule(pair.fds[0], base, data.get(), soon()); });
    const auto received = receiveCapsule(pair.fds[1], base, soon());
    sender.join();
    CHECK(sent);
    REQUIRE(received);
    CHECK(*received == expected);
    const auto snapshot = parseCapsule(received->data(), received->size());
    REQUIRE(snapshot);
    CHECK(snapshot->threads.size() == kCapsuleThreads);
}

TEST_CASE("an empty marker, a stranger and a short capsule are all refused") {
    const Packet base = terminalPacket();
    {  // The worker had nothing to send: a packet with no bytes.
        // One line per block, so a run that hangs shows which block it was in.
        std::fprintf(stderr, "capsule refusal block 1: an empty packet\n");
        Pair pair;
        REQUIRE(sendCapsule(pair.fds[0], base, nullptr, soon()));
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, soon()));
    }
    {  // Another incident's capsule is not ours.
        // One line per block, so a run that hangs shows which block it was in.
        std::fprintf(stderr, "capsule refusal block 2: another incident's capsule\n");
        Pair pair;
        auto data = fresh();
        addThread(*data, Role::Fault, Missing::None, Stop::End, {0x1000});
        Packet other = base;
        other.incident[0] = 'b';
        REQUIRE(sendCapsule(pair.fds[0], other, data.get(), soon()));
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, soon()));
    }
    {  // An oversized byte count is refused before anything is allocated.
        // One line per block, so a run that hangs shows which block it was in.
        std::fprintf(stderr, "capsule refusal block 3: an oversized byte count\n");
        Pair pair;
        Packet huge = base;
        huge.kind = Message::Trace;
        huge.code = static_cast<std::uint32_t>(kCapsuleMaxBytes + 1);
        REQUIRE(::send(pair.fds[0], &huge, sizeof(huge), 0) == static_cast<ssize_t>(sizeof(huge)));
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, soon()));
    }
    {  // The worker dies mid-capsule: the supervisor gets end of file, not a hang.
        // One line per block, so a run that hangs shows which block it was in.
        std::fprintf(stderr, "capsule refusal block 4: a worker that dies mid-capsule\n");
        Pair pair;
        Packet cut = base;
        cut.kind = Message::Trace;
        cut.code = 64;
        REQUIRE(::send(pair.fds[0], &cut, sizeof(cut), 0) == static_cast<ssize_t>(sizeof(cut)));
        char some[16] = {};
        REQUIRE(::send(pair.fds[0], some, sizeof(some), 0) == 16);
        pair.closeSender();
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, soon()));
    }
    {  // A silent worker: the receiver gives up at its deadline instead of waiting.
        // One line per block, so a run that hangs shows which block it was in.
        std::fprintf(stderr, "capsule refusal block 5: a silent worker\n");
        Pair pair;
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, monotonicNanoseconds() + 20'000'000ULL));
    }
    {  // A deadline already spent: nothing is read, and a full buffer is not waited on.
        // One line per block, so a run that hangs shows which block it was in.
        std::fprintf(stderr, "capsule refusal block 6: a full buffer and a spent deadline\n");
        Pair pair;
        auto data = fresh();
        addThread(*data, Role::Fault, Missing::None, Stop::End, {0x1000});
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, 1));
        std::vector<char> filler(4096, 'x');
        // Non-blocking for real, not only per call: where the kernel reads the socket's own
        // flag to decide whether to wait for buffer space, MSG_DONTWAIT alone would block
        // on the first send that finds the socket full.
        REQUIRE(::fcntl(pair.fds[0], F_SETFL, ::fcntl(pair.fds[0], F_GETFL) | O_NONBLOCK) == 0);
        // Bounded by construction: a socket buffer is never 256 MiB, so if a platform's
        // sends never report "full" this stops anyway and the check below fails, rather
        // than the test running on until the harness gives up.
        for (int sends = 0;
             sends < 65536 && ::send(pair.fds[0], filler.data(), filler.size(), MSG_DONTWAIT) > 0;
             ++sends) {
        }
        CHECK_FALSE(sendCapsule(pair.fds[0], base, data.get(), 1));
    }
}

TEST_CASE("a send or receive leaves the descriptor's blocking mode as it found it") {
    const Packet base = terminalPacket();
    auto data = fresh();
    addThread(*data, Role::Fault, Missing::None, Stop::End, {0x1000});
    for (const bool startNonBlocking : {false, true}) {
        Pair pair;
        for (int fd : pair.fds) {
            const int flags = ::fcntl(fd, F_GETFL);
            REQUIRE(::fcntl(fd, F_SETFL,
                            startNonBlocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0);
        }
        const int before = ::fcntl(pair.fds[0], F_GETFL) & O_NONBLOCK;
        REQUIRE(sendCapsule(pair.fds[0], base, data.get(), soon()));
        CHECK((::fcntl(pair.fds[0], F_GETFL) & O_NONBLOCK) == before);
        CHECK(receiveCapsule(pair.fds[1], base, soon()));
        CHECK((::fcntl(pair.fds[1], F_GETFL) & O_NONBLOCK) == before);
        // And while it runs, a blocking descriptor does not make a full socket wait: the
        // refusal comes from the deadline. Checked on the receive side, which is empty.
        CHECK_FALSE(receiveCapsule(pair.fds[1], base, monotonicNanoseconds() + 10'000'000ULL));
        CHECK((::fcntl(pair.fds[1], F_GETFL) & O_NONBLOCK) == before);
    }
}
#endif
