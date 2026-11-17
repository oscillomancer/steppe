// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Portable tests of the trace model: frame walker, module identity, maps parsing,
// the deadline-bounded collection and the report shape. Platform collectors are
// exercised by the lifecycle fixture instead.
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>
#include "supple/diagcap/trace.h"
using namespace supple;
using namespace supple::diagnostics::detail;
namespace fs = std::filesystem;
namespace {
// A sparse fake target memory that records every read it serves.
struct Image {
    std::map<std::uint64_t, std::vector<unsigned char>> parts;
    mutable std::vector<std::pair<std::uint64_t, std::size_t>> reads;
    void put(std::uint64_t at, const std::vector<unsigned char>& bytes) { parts[at] = bytes; }
    template <class T>
    void word(std::uint64_t at, T value) {
        std::vector<unsigned char> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        put(at, bytes);
    }
    ReadMemory reader() const {
        return [this](std::uint64_t at, void* out, std::size_t n) {
            reads.emplace_back(at, n);
            auto it = parts.upper_bound(at);
            if (it == parts.begin()) return false;
            --it;
            if (at < it->first || at - it->first + n > it->second.size()) return false;
            std::memcpy(out, it->second.data() + (at - it->first), n);
            return true;
        };
    }
    void frame(std::uint64_t fp, std::uint64_t next, std::uint64_t ret) {
        std::vector<unsigned char> bytes(16);
        std::memcpy(bytes.data(), &next, 8);
        std::memcpy(bytes.data() + 8, &ret, 8);
        put(fp, bytes);
    }
};
template <class T>
void poke(std::vector<unsigned char>& bytes, std::size_t at, T value) {
    REQUIRE(at + sizeof(T) <= bytes.size());
    std::memcpy(&bytes[at], &value, sizeof(T));
}
// A minimal ELF64 image: header, one PT_LOAD at `vaddr`, one PT_NOTE with a GNU
// build ID note at file offset 0x200.
std::vector<unsigned char> elf(std::uint16_t type, std::uint64_t vaddr,
                               const std::vector<unsigned char>& id, std::uint32_t namesz = 4,
                               std::uint32_t descsz = 0xffffffffU) {
    std::vector<unsigned char> bytes(0x400, 0);
    std::memcpy(bytes.data(), "\177ELF", 4);
    bytes[4] = 2;
    bytes[5] = 1;
    poke<std::uint16_t>(bytes, 16, type);
    poke<std::uint64_t>(bytes, 32, 64);
    poke<std::uint16_t>(bytes, 54, 56);
    poke<std::uint16_t>(bytes, 56, 2);
    poke<std::uint32_t>(bytes, 64, 1);  // PT_LOAD
    poke<std::uint64_t>(bytes, 64 + 8, 0);
    poke<std::uint64_t>(bytes, 64 + 16, vaddr);
    poke<std::uint64_t>(bytes, 64 + 32, 0x400);
    poke<std::uint32_t>(bytes, 120, 4);  // PT_NOTE
    poke<std::uint64_t>(bytes, 120 + 8, 0x200);
    poke<std::uint64_t>(bytes, 120 + 16, vaddr + 0x200);
    const std::size_t noteLen = 12 + 4 + ((id.size() + 3) & ~std::size_t{3});
    poke<std::uint64_t>(bytes, 120 + 32, noteLen);
    poke<std::uint32_t>(bytes, 0x200, namesz);
    poke<std::uint32_t>(bytes, 0x204,
                        descsz == 0xffffffffU ? static_cast<std::uint32_t>(id.size()) : descsz);
    poke<std::uint32_t>(bytes, 0x208, 3);
    std::memcpy(&bytes[0x20c], "GNU\0", 4);
    std::memcpy(&bytes[0x210], id.data(), id.size());
    return bytes;
}
const std::vector<unsigned char> idA{0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04};
const std::vector<unsigned char> idB{0xca, 0xfe, 0xba, 0xbe};

Layout simpleLayout() {
    Layout layout;
    layout.modules.push_back({std::string("deadbeef01020304"), 0x400000});
    layout.regions.push_back({0x400000, 0x500000, 0});
    layout.regions.push_back({0x700000, 0x701000, -1});
    return layout;
}
}  // namespace

TEST_CASE("maps lines parse with paths ignored and malformed lines skipped") {
    const std::string text =
        "55d0c8a00000-55d0c8a01000 r--p 00000000 08:01 1234  /home/secret-project/steppe\n"
        "55d0c8a01000-55d0c8a05000 r-xp 00001000 08:01 1234  /home/secret-project/steppe\n"
        "7f0000000000-7f0000001000 rwxp 00000000 00:00 0\n"
        "7ffc00000000-7ffc00021000 rw-p 00000000 00:00 0   [stack]\n"
        "not a maps line\n"
        "1000-500 r-xp 00000000 08:01 5 /backwards\n"
        "zzzz-5000 r-xp 00000000 08:01 5 /bad\n"
        "7f1000000000-7f1000002000 r-xp 00000000 08:01 99999999999999999999 /overflow\n"
        "7f2000000000-7f2000002000 r-xp 00000000 08:01 99  /usr/lib/libc.so.6 (deleted)\n";
    const auto maps = parseMaps(text);
    REQUIRE(maps.size() == 5);
    CHECK(maps[0].named);
    CHECK_FALSE(maps[0].execute);
    CHECK(maps[1].execute);
    CHECK(maps[1].offset == 0x1000);
    CHECK(maps[1].inode == 1234);
    CHECK_FALSE(maps[2].named);
    CHECK(maps[2].write);
    CHECK_FALSE(maps[3].named);  // [stack]
    CHECK(maps[4].inode == 99);
    CHECK(maps[4].named);
    // The line limit bounds work on an oversized list.
    std::string many;
    for (std::size_t i = 0; i < kMaxMappings + 100; ++i)
        many += "1000-2000 r-xp 00000000 00:00 0\n";
    CHECK(parseMaps(many).size() == kMaxMappings);
}

TEST_CASE("ELF build IDs are read through the reader and every size is bounded") {
    Image image;
    image.put(0x7f0000000000ULL, elf(3, 0, idA));
    std::uint64_t bias = 1;
    auto id = elfBuildId(image.reader(), 0x7f0000000000ULL, &bias);
    REQUIRE(id);
    CHECK(*id == "deadbeef01020304");
    CHECK(bias == 0x7f0000000000ULL);
    // A prelinked-style image whose first segment is linked at a nonzero address.
    Image shifted;
    shifted.put(0x7f0000100000ULL, elf(3, 0x10000, idB));
    id = elfBuildId(shifted.reader(), 0x7f0000100000ULL, &bias);
    REQUIRE(id);
    CHECK(*id == "cafebabe");
    CHECK(bias == 0x7f0000100000ULL - 0x10000);
    // A fixed-address executable loads where it was linked.
    Image fixed;
    fixed.put(0x400000, elf(2, 0x400000, idB));
    id = elfBuildId(fixed.reader(), 0x400000, &bias);
    REQUIRE(id);
    CHECK(bias == 0);
    // Anything odd gives no ID and no crash.
    Image odd;
    odd.put(0x1000, elf(3, 0, idA, 0xffffffffU));  // note name size overflows the segment
    CHECK_FALSE(elfBuildId(odd.reader(), 0x1000));
    odd.put(0x2000, elf(3, 0, idA, 4, 65));  // build ID larger than the allowed 64 bytes
    CHECK_FALSE(elfBuildId(odd.reader(), 0x2000));
    odd.put(0x3000, elf(3, 0, idA, 4, 0x7fffffff));
    CHECK_FALSE(elfBuildId(odd.reader(), 0x3000));
    auto notElf = elf(3, 0, idA);
    notElf[1] = 'X';
    odd.put(0x4000, notElf);
    CHECK_FALSE(elfBuildId(odd.reader(), 0x4000));
    auto hugeCount = elf(3, 0, idA);
    poke<std::uint16_t>(hugeCount, 56, 0xffff);
    odd.put(0x5000, hugeCount);
    CHECK_FALSE(elfBuildId(odd.reader(), 0x5000));
    auto farHeaders = elf(3, 0, idA);
    poke<std::uint64_t>(farHeaders, 32, 0x7fffffffffffffffULL);
    odd.put(0x6000, farHeaders);
    CHECK_FALSE(elfBuildId(odd.reader(), 0x6000));
    CHECK_FALSE(elfBuildId(odd.reader(), 0x99999000));  // unreadable
    auto bigNote = elf(3, 0, idA);
    poke<std::uint64_t>(bigNote, 120 + 32, std::uint64_t{1} << 40);
    odd.put(0x7000, bigNote);
    CHECK_FALSE(elfBuildId(odd.reader(), 0x7000));
}

TEST_CASE("layout groups mappings into modules and keeps JIT memory anonymous") {
    Image image;
    image.put(0x55d0c8a00000ULL, elf(3, 0, idA));
    image.put(0x7f1000000000ULL, elf(3, 0, idB));
    const auto maps = parseMaps(
        "55d0c8a00000-55d0c8a01000 r--p 00000000 08:01 1234  /home/secret-project/steppe\n"
        "55d0c8a01000-55d0c8a05000 r-xp 00001000 08:01 1234  /home/secret-project/steppe\n"
        "7f0000000000-7f0000001000 rwxp 00000000 00:00 0\n"
        "7f1000000000-7f1000002000 r-xp 00000000 08:01 99    /usr/lib/libplugin.so\n");
    const auto layout = buildLayout(maps, image.reader());
    REQUIRE(layout.modules.size() == 2);
    CHECK(*layout.modules[0].id == "deadbeef01020304");
    CHECK(layout.modules[0].bias == 0x55d0c8a00000ULL);
    CHECK(*layout.modules[1].id == "cafebabe");
    REQUIRE(layout.regions.size() == 3);
    CHECK(layout.regions[0].module == 0);
    CHECK(layout.regions[1].module == -1);  // anonymous executable memory
    CHECK(layout.regions[2].module == 1);
}

TEST_CASE("walker follows a frame-pointer chain to its end") {
    Image image;
    image.frame(0x7000, 0x7100, 0x401010);
    image.frame(0x7100, 0x7200, 0x402020);
    image.frame(0x7200, 0, 0x403030);
    const auto walk =
        walkFrames(image.reader(), simpleLayout(), {0x400500, 0x6ff0, 0x7000}, Arch::X86_64);
    CHECK(walk.stop == Stop::End);
    REQUIRE(walk.frames.size() == 4);
    CHECK_FALSE(walk.frames[0].returnAddress);
    CHECK(walk.frames[0].offset == 0x500);
    CHECK(walk.frames[1].returnAddress);
    CHECK(walk.frames[1].offset == 0x1010);
    CHECK(walk.frames[3].offset == 0x3030);
    // Only the frame records themselves are read, never other stack words.
    for (const auto& [at, n] : image.reads) {
        CHECK(n == 16);
        CHECK((at == 0x7000 || at == 0x7100 || at == 0x7200));
    }
}

TEST_CASE("walker stops safely on every kind of corrupt chain") {
    const auto layout = simpleLayout();
    auto run = [&](Image& image, Regs regs, Arch arch = Arch::X86_64) {
        return walkFrames(image.reader(), layout, regs, arch);
    };
    {  // a frame pointer that points at itself
        Image image;
        image.frame(0x7000, 0x7000, 0x401010);
        const auto walk = run(image, {0x400500, 0x6ff0, 0x7000});
        CHECK(walk.stop == Stop::Cycle);
        CHECK(walk.frames.size() == 2);
    }
    {  // a chain that returns to an earlier frame
        Image image;
        image.frame(0x7000, 0x7100, 0x401010);
        image.frame(0x7100, 0x7000, 0x402020);
        CHECK(run(image, {0x400500, 0x6ff0, 0x7000}).stop == Stop::Cycle);
    }
    {  // misaligned
        Image image;
        CHECK(run(image, {0x400500, 0x6ff0, 0x7004}).stop == Stop::BadFramePointer);
        CHECK(image.reads.empty());
    }
    {  // below the stack pointer
        Image image;
        image.frame(0x7000, 0, 0x401010);
        CHECK(run(image, {0x400500, 0x8000, 0x7000}).stop == Stop::BadFramePointer);
    }
    {  // an enormous jump between frames
        Image image;
        image.frame(0x7000, 0x7000 + (std::uint64_t{1} << 31), 0x401010);
        CHECK(run(image, {0x400500, 0x6ff0, 0x7000}).stop == Stop::BadFramePointer);
    }
    {  // a return address that is not executable stops without recording it
        Image image;
        image.frame(0x7000, 0x7100, 0x401010);
        image.frame(0x7100, 0x7200, 0xdeadbeef);
        const auto walk = run(image, {0x400500, 0x6ff0, 0x7000});
        CHECK(walk.stop == Stop::NotExecutable);
        CHECK(walk.frames.size() == 2);
    }
    {  // unreadable memory
        Image image;
        image.frame(0x7000, 0x9000, 0x401010);
        const auto walk = run(image, {0x400500, 0x6ff0, 0x7000});
        CHECK(walk.stop == Stop::Unreadable);
        CHECK(walk.frames.size() == 2);
    }
    {  // pc itself outside executable memory: recorded as unavailable, chain continues
        Image image;
        image.frame(0x7000, 0, 0x401010);
        const auto walk = run(image, {0x1234, 0x6ff0, 0x7000});
        REQUIRE(walk.frames.size() == 2);
        CHECK(walk.frames[0].module == -1);
        CHECK(walk.frames[1].module == 0);
    }
    {  // JIT frames carry no module and no address
        Image image;
        image.frame(0x7000, 0, 0x700100);
        const auto walk = run(image, {0x400500, 0x6ff0, 0x7000});
        REQUIRE(walk.frames.size() == 2);
        CHECK(walk.frames[1].module == -1);
    }
    {  // pointer-authentication bits are stripped on arm64 only
        Image image;
        image.frame(0x7000, 0, 0xab00000000401010ULL);
        const auto arm = run(image, {0x400500, 0x6ff0, 0x7000}, Arch::Arm64);
        REQUIRE(arm.frames.size() == 2);
        CHECK(arm.frames[1].offset == 0x1010);
        CHECK(run(image, {0x400500, 0x6ff0, 0x7000}, Arch::X86_64).stop == Stop::NotExecutable);
    }
}

TEST_CASE("walker is capped at 128 frames and honours its deadline") {
    Image image;
    for (std::uint64_t i = 0; i < 300; ++i)
        image.frame(0x10000 + i * 0x20, 0x10000 + (i + 1) * 0x20, 0x401000 + i);
    const auto layout = simpleLayout();
    const auto walk =
        walkFrames(image.reader(), layout, {0x400500, 0x10000, 0x10000}, Arch::X86_64);
    CHECK(walk.stop == Stop::Depth);
    CHECK(walk.frames.size() == kMaxFrames);
    unsigned calls = 0;
    const auto expired = walkFrames(image.reader(), layout, {0x400500, 0x10000, 0x10000},
                                    Arch::X86_64, [&] { return ++calls > 2; });
    CHECK(expired.stop == Stop::Deadline);
    CHECK(expired.frames.size() < kMaxFrames);
    CHECK(expired.frames.size() >= 16);
}

namespace {
// A fake platform: threads with canned registers, a counter of stops and releases.
struct FakeProcess {
    Image image;
    std::string maps;
    std::vector<std::uint64_t> tids;
    std::map<std::uint64_t, Regs> regs;
    std::map<std::uint64_t, Missing> refuse;
    int stops = 0, releases = 0, stopped = 0, maxStopped = 0;
    unsigned expireAfter = ~0U, checks = 0;
    std::function<void(std::uint64_t)> onStop;  // runs as a thread is stopped, before it reads
    TraceOps ops() {
        TraceOps ops;
        ops.expired = [this] { return ++checks > expireAfter; };
        ops.maps = [this]() -> std::optional<std::string> {
            if (maps.empty()) return std::nullopt;
            return maps;
        };
        ops.read = image.reader();
        ops.threads = [this] { return tids; };
        ops.stop = [this](std::uint64_t tid, Regs& out) {
            if (auto it = refuse.find(tid); it != refuse.end()) return it->second;
            ++stops;
            maxStopped = std::max(maxStopped, ++stopped);
            if (onStop) onStop(tid);
            out = regs[tid];
            return Missing::None;
        };
        ops.release = [this](std::uint64_t) {
            ++releases;
            --stopped;
        };
        return ops;
    }
};
FakeProcess process() {
    FakeProcess p;
    p.image.put(0x400000, elf(3, 0, idA));
    p.maps = "400000-500000 r-xp 00000000 08:01 7 /secret/path/app\n";
    p.image.frame(0x7000, 0, 0x401000);
    p.regs[10] = p.regs[11] = p.regs[12] = {0x400100, 0x6ff0, 0x7000};
    p.tids = {12, 10, 11};
    return p;
}
}  // namespace

TEST_CASE("collection orders fault, main, then threads and stops one thread at a time") {
    auto p = process();
    const auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    REQUIRE(snapshot.threads.size() == 3);
    CHECK(snapshot.threads[0].role == Role::Fault);
    CHECK(snapshot.threads[1].role == Role::Main);
    CHECK(snapshot.threads[2].role == Role::Other);
    for (const auto& t : snapshot.threads) CHECK(t.missing == Missing::None);
    CHECK(snapshot.threads[0].walk.frames[0].offset == 0x200);  // from the capsule registers
    CHECK(snapshot.threads[1].walk.frames[0].offset == 0x100);
    CHECK(p.stops == 2);  // the fault thread is never stopped
    CHECK(p.releases == 2);
    CHECK(p.maxStopped == 1);
    REQUIRE(snapshot.modules.size() == 1);
    CHECK(*snapshot.modules[0].id == "deadbeef01020304");
}

TEST_CASE("collection reports why a thread is missing instead of an empty trace") {
    auto p = process();
    p.refuse[10] = Missing::PtraceDenied;
    p.refuse[12] = Missing::ThreadExited;
    auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    REQUIRE(snapshot.threads.size() == 3);
    CHECK(snapshot.threads[0].missing == Missing::None);
    CHECK(snapshot.threads[1].missing == Missing::PtraceDenied);
    CHECK(snapshot.threads[2].missing == Missing::ThreadExited);
    CHECK(snapshot.threads[1].walk.frames.empty());
    // Unreadable mappings leave every thread, the fault thread included, unavailable.
    auto q = process();
    q.maps.clear();
    snapshot = collectTrace(q.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    for (const auto& t : snapshot.threads) CHECK(t.missing == Missing::MapsUnreadable);
    CHECK(q.stops == 0);
}

TEST_CASE("a capsule without registers still names its thread, which is then read like any other") {
    // terminate and abort capture no registers, only the calling thread: it is parked in
    // its handler, so it can be stopped, and its real stack must not be filed elsewhere.
    auto p = process();
    auto snapshot = collectTrace(p.ops(), 11, Regs{}, 10, Arch::X86_64);
    REQUIRE(snapshot.threads.size() == 3);
    CHECK(snapshot.threads[0].role == Role::Fault);
    CHECK(snapshot.threads[0].missing == Missing::None);
    REQUIRE(!snapshot.threads[0].walk.frames.empty());
    CHECK(snapshot.threads[0].walk.frames[0].offset == 0x100);  // read from the stopped thread
    CHECK(snapshot.threads[1].role == Role::Main);
    CHECK(snapshot.threads[2].role == Role::Other);
    CHECK(p.stops == 3);
    CHECK(p.releases == 3);
    CHECK(p.maxStopped == 1);
    // With no thread id either, the fault entry is unavailable and says why; the other
    // threads are still read, and no thread is stopped on its behalf.
    auto q = process();
    snapshot = collectTrace(q.ops(), 0, Regs{}, 10, Arch::X86_64);
    REQUIRE(snapshot.threads.size() == 4);
    CHECK(snapshot.threads[0].role == Role::Fault);
    CHECK(snapshot.threads[0].missing == Missing::RegistersUnreadable);
    CHECK(snapshot.threads[0].walk.frames.empty());
    CHECK(q.stops == 3);
    // A refused stop is reported with its own reason, for the fault entry too.
    auto r = process();
    r.refuse[11] = Missing::PtraceDenied;
    snapshot = collectTrace(r.ops(), 11, Regs{}, 10, Arch::X86_64);
    CHECK(snapshot.threads[0].role == Role::Fault);
    CHECK(snapshot.threads[0].missing == Missing::PtraceDenied);
}

TEST_CASE("an expired hold budget marks the rest unavailable and releases every thread") {
    for (unsigned after = 0; after < 40; ++after) {
        auto p = process();
        p.expireAfter = after;
        const auto snapshot =
            collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
        REQUIRE(snapshot.threads.size() == 3);
        CHECK(p.stopped == 0);  // nothing is left stopped, whenever the budget ends
        CHECK(p.releases == p.stops);
        for (std::size_t i = 1; i < 3; ++i)
            CHECK((snapshot.threads[i].missing == Missing::None ||
                   snapshot.threads[i].missing == Missing::Deadline));
        if (after == 0) {
            CHECK(p.stops == 0);
            CHECK(snapshot.threads[1].missing == Missing::Deadline);
            CHECK(snapshot.threads[2].missing == Missing::Deadline);
        }
    }
}

TEST_CASE("collection caps threads at 128 and records what it dropped") {
    auto p = process();
    p.tids.clear();
    for (std::uint64_t t = 1; t <= 200; ++t) {
        p.tids.push_back(t);
        p.regs[t] = {0x400100, 0x6ff0, 0x7000};
    }
    const auto snapshot = collectTrace(p.ops(), 150, {0x400200, 0x6ff0, 0x7000}, 1, Arch::X86_64);
    REQUIRE(snapshot.threads.size() == kMaxThreads);
    CHECK(snapshot.dropped == 200 - kMaxThreads);
    CHECK(snapshot.threads[0].role == Role::Fault);  // reserved before truncating
    CHECK(snapshot.threads[1].role == Role::Main);
    const auto text = json::writeCanonical(threadsJson(snapshot));
    CHECK(text.size() < 4 * 1024 * 1024);
}

TEST_CASE("trace JSON carries identity and offsets only") {
    auto p = process();
    p.maps += "7f0000000000-7f0000001000 rwxp 00000000 00:00 0\n";
    p.image.frame(0x7000, 0x7100, 0x7f0000000100ULL);
    p.image.frame(0x7100, 0, 0x401234);
    const auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    const auto modules = modulesJson(snapshot);
    const auto threads = threadsJson(snapshot);
    CHECK(traceShape(&modules, threads));
    const auto text = json::writeCanonical(modules) + json::writeCanonical(threads);
    // No path, thread id, register value or absolute address survives.
    for (const char* secret : {"secret", "7f0000000100", "7f0000000", "6ff0", "7000", "7100",
                               "400200", "400100", "401234", "\"tid\"", "\"pid\""})
        CHECK(text.find(secret) == std::string::npos);
    CHECK(text.find("0x1234") != std::string::npos);  // module-relative offset
    CHECK(text.find("deadbeef01020304") != std::string::npos);
    // A JIT frame names no module and has no offset.
    const auto* frames = threads.at(0).find("frames");
    REQUIRE(frames);
    const auto& jit = frames->at(1);
    CHECK(jit.find("module")->isNull());
    CHECK(jit.find("offset") == nullptr);
    CHECK(json::parse(text.substr(0, json::writeCanonical(modules).size())));
}

TEST_CASE("trace shape validation is strict and nested") {
    auto p = process();
    const auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    const auto modules = modulesJson(snapshot);
    const auto good = threadsJson(snapshot);
    REQUIRE(traceShape(&modules, good));
    CHECK(traceShape(nullptr, json::Value::array()));  // older reports: no modules, no threads
    auto mutate = [&](auto change) {
        auto threads = good;
        change(threads);
        return traceShape(&modules, threads);
    };
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["extra"] = 1; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["ordinal"] = 5; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["role"] = "boss"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["state"] = "unavailable"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["stop"] = "melted"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["reason"] = "ptrace_denied"; }));
    CHECK_FALSE(mutate([](json::Value& t) {
        t.asArray()[0]["frames"].asArray()[0]["module"] = 9;  // out of range
    }));
    CHECK_FALSE(
        mutate([](json::Value& t) { t.asArray()[0]["frames"].asArray()[0]["offset"] = "0xZZ"; }));
    CHECK_FALSE(
        mutate([](json::Value& t) { t.asArray()[0]["frames"].asArray()[0]["offset"] = "0X10"; }));
    CHECK_FALSE(mutate([](json::Value& t) {
        t.asArray()[0]["frames"].asArray()[0]["offset"] = "0x11111111111111111";
    }));
    CHECK_FALSE(
        mutate([](json::Value& t) { t.asArray()[0]["frames"].asArray()[0]["return"] = 1; }));
    CHECK_FALSE(mutate([](json::Value& t) {
        auto& frames = t.asArray()[0]["frames"];
        for (int i = 0; i < 200; ++i) frames.push_back(frames.at(0));
    }));
    CHECK_FALSE(mutate([](json::Value& t) {
        for (int i = 0; i < 200; ++i) t.push_back(t.at(0));
    }));
    auto unavailable = json::Value::array();
    auto entry = json::Value::object();
    entry["ordinal"] = 0;
    entry["role"] = "fault";
    entry["state"] = "unavailable";
    entry["reason"] = "ptrace_denied";
    entry["frames"] = json::Value::array();
    unavailable.push_back(entry);
    CHECK(traceShape(&modules, unavailable));
    unavailable.asArray()[0]["stop"] = "end";
    CHECK_FALSE(traceShape(&modules, unavailable));
    auto badModules = modules;
    badModules.asArray()[0]["id"] = "DEADBEEF";  // upper case
    CHECK_FALSE(traceShape(&badModules, good));
    badModules.asArray()[0]["id"] = "abc";  // odd length
    CHECK_FALSE(traceShape(&badModules, good));
    badModules.asArray()[0]["format"] = "coff";
    CHECK_FALSE(traceShape(&badModules, good));
}

namespace {
using supple::diagctx::kClaiming;
using supple::diagctx::kMaxThreadSlots;
using supple::diagctx::kTableMagic;
using supple::diagctx::kTableVersion;
using supple::diagctx::ThreadRole;
constexpr std::uint64_t kTableAt = 0x900000;
// A table image as the worker's static one looks to a remote read.
supple::diagctx::TableImage tableImage() {
    supple::diagctx::TableImage image{};
    image.header.magic = kTableMagic;
    image.header.version = kTableVersion;
    return image;
}
void claim(supple::diagctx::TableImage& image, std::size_t slot, std::uint64_t id, ThreadRole role,
           std::uint16_t ordinal = 0) {
    image.threads[slot].id = id;
    image.threads[slot].info = static_cast<std::uint64_t>(role) | std::uint64_t{ordinal} << 8;
}
void put(FakeProcess& p, const supple::diagctx::TableImage& image) {
    std::vector<unsigned char> bytes(sizeof(image));
    std::memcpy(bytes.data(), &image, sizeof(image));
    p.image.put(kTableAt, bytes);
}
}  // namespace

TEST_CASE("the execution table labels threads by the role they registered") {
    auto p = process();
    auto image = tableImage();
    claim(image, 0, 10, ThreadRole::Model);
    claim(image, 1, 11, ThreadRole::AudioRender);
    claim(image, 5, 12, ThreadRole::AudioLive, 3);
    claim(image, 6, 99, ThreadRole::Recorder);  // a thread that is not in the process
    put(p, image);
    const auto snapshot =
        collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
    CHECK(snapshot.table == TableState::Read);
    REQUIRE(snapshot.threads.size() == 3);
    // The fault thread stays the fault entry and says what it was.
    CHECK(snapshot.threads[0].role == Role::Fault);
    CHECK(snapshot.threads[0].registered == ThreadRole::AudioRender);
    CHECK(snapshot.threads[1].registered == ThreadRole::Model);
    CHECK(snapshot.threads[2].registered == ThreadRole::AudioLive);
    CHECK(snapshot.threads[2].worker == 3);
    CHECK(roleString(snapshot.threads[0]) == "fault");
    CHECK(roleString(snapshot.threads[1]) == "model");
    CHECK(roleString(snapshot.threads[2]) == "audio_live");
    const auto threads = threadsJson(snapshot);
    const auto modules = modulesJson(snapshot);
    CHECK(traceShape(&modules, threads));
    CHECK(threads.at(0).find("registered")->asString() == "audio_render");
    CHECK(threads.at(0).find("worker") == nullptr);
    CHECK(threads.at(1).find("registered") == nullptr);  // role already says it
    CHECK(threads.at(2).find("worker")->asInt() == 3);
    CHECK(traceLimits(snapshot).size() == 1);  // best-effort frames only
}

TEST_CASE("a table that turned threads away says so, and only then") {
    auto limitCount = [](const TraceSnapshot& snapshot) {
        std::size_t found = 0;
        for (const auto& limit : traceLimits(snapshot))
            if (limit.find("execution table's capacity") != std::string::npos) ++found;
        return found;
    };
    auto p = process();
    auto image = tableImage();
    claim(image, 0, 10, ThreadRole::Model);
    put(p, image);
    auto snapshot =
        collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
    CHECK(snapshot.tableThreadsDropped == 0);
    CHECK(limitCount(snapshot) == 0);
    image.header.threadsDropped = 3;
    put(p, image);
    snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
    CHECK(snapshot.tableThreadsDropped == 3);
    CHECK(limitCount(snapshot) == 1);
}

TEST_CASE("without a table every thread keeps its position as its role") {
    auto p = process();
    const auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    CHECK(snapshot.table == TableState::NotOffered);
    CHECK(roleString(snapshot.threads[0]) == "fault");
    CHECK(roleString(snapshot.threads[1]) == "model");  // the main thread, by its position
    CHECK(roleString(snapshot.threads[2]) == "other");
    const auto limits = traceLimits(snapshot);
    for (const auto& limit : limits) CHECK(limit.find("execution table") == std::string::npos);
}

TEST_CASE("a table that is missing, unreadable or not ours gives roles nothing and says so") {
    auto limited = [](const TraceSnapshot& snapshot) {
        for (const auto& limit : traceLimits(snapshot))
            if (limit.find("execution table") != std::string::npos) return true;
        return false;
    };
    {  // the address reads nothing
        auto p = process();
        const auto snapshot =
            collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
        CHECK(snapshot.table == TableState::Unreadable);
        CHECK(limited(snapshot));
        for (const auto& t : snapshot.threads) CHECK(t.registered == ThreadRole::None);
        CHECK(snapshot.threads.size() == 3);  // traces are unaffected
        CHECK(snapshot.threads[1].missing == Missing::None);
    }
    for (int broken = 0; broken < 3; ++broken) {  // wrong magic, wrong version, zeroes
        auto p = process();
        auto image = tableImage();
        claim(image, 0, 10, ThreadRole::Model);
        if (broken == 0) image.header.magic ^= 1;
        if (broken == 1) image.header.version += 1;
        if (broken == 2) image = {};
        put(p, image);
        const auto snapshot =
            collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
        CHECK(snapshot.table == TableState::Invalid);
        CHECK(limited(snapshot));
        for (const auto& t : snapshot.threads) CHECK(t.registered == ThreadRole::None);
    }
    {  // slots that are free, mid-claim or carry an unknown role label nothing
        auto p = process();
        auto image = tableImage();
        claim(image, 0, 0, ThreadRole::Model);
        claim(image, 1, kClaiming, ThreadRole::Model);
        claim(image, 2, 10, ThreadRole::None);
        image.threads[3].id = 12;
        image.threads[3].info = 200;  // outside the closed set
        put(p, image);
        const auto snapshot =
            collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
        CHECK(snapshot.table == TableState::Read);
        for (const auto& t : snapshot.threads) CHECK(t.registered == ThreadRole::None);
    }
}

TEST_CASE("the table is read once, right after the fault thread and before any other is stopped") {
    auto p = process();
    auto image = tableImage();
    claim(image, 0, 10, ThreadRole::Model);
    put(p, image);
    std::vector<std::string> log;
    auto ops = p.ops();
    const auto read = ops.read;
    ops.read = [&](std::uint64_t at, void* out, std::size_t n) {
        if (at == kTableAt) log.push_back("table");
        return read(at, out, n);
    };
    const auto stop = ops.stop;
    ops.stop = [&](std::uint64_t tid, Regs& regs) {
        log.push_back("stop " + std::to_string(tid));
        return stop(tid, regs);
    };
    const auto snapshot =
        collectTrace(ops, 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
    REQUIRE(snapshot.threads.size() == 3);
    REQUIRE(log.size() == 3);
    CHECK(log[0] == "table");
    CHECK(log[1] == "stop 10");
    CHECK(log[2] == "stop 12");
}

TEST_CASE("a spent deadline cuts other threads but never the table") {
    auto p = process();
    auto image = tableImage();
    claim(image, 0, 11, ThreadRole::AudioRender);
    claim(image, 1, 10, ThreadRole::Model);
    put(p, image);
    p.expireAfter = 0;  // already expired when the first thread has been walked
    const auto snapshot =
        collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
    REQUIRE(snapshot.threads.size() == 3);
    CHECK(snapshot.table == TableState::Read);
    CHECK(snapshot.threads[1].missing == Missing::Deadline);
    CHECK(snapshot.threads[2].missing == Missing::Deadline);
    // The roles are still known, for the threads whose traces were cut as well.
    CHECK(snapshot.threads[0].registered == ThreadRole::AudioRender);
    CHECK(snapshot.threads[1].registered == ThreadRole::Model);
    CHECK(p.stops == 0);
}

TEST_CASE("role fields in a report are validated strictly") {
    auto p = process();
    auto image = tableImage();
    claim(image, 0, 11, ThreadRole::AudioRender);
    claim(image, 1, 12, ThreadRole::AudioBackground, 4);
    put(p, image);
    const auto snapshot =
        collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
    const auto modules = modulesJson(snapshot);
    const auto good = threadsJson(snapshot);
    REQUIRE(traceShape(&modules, good));
    auto mutate = [&](auto change) {
        auto threads = good;
        change(threads);
        return traceShape(&modules, threads);
    };
    // every registered role is a valid role string, and "main" stays valid for old reports
    for (const char* role : {"model", "audio_render", "audio_live", "audio_background",
                             "audio_offline", "recorder", "midi_output", "other", "main"})
        CHECK(mutate([&](json::Value& t) { t.asArray()[1]["role"] = role; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[1]["role"] = "audio"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[1]["role"] = "Model"; }));
    // "registered" belongs to the fault thread and is one of the registrable roles
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["registered"] = "fault"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["registered"] = "other"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[0]["registered"] = 5; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[1]["registered"] = "model"; }));
    // "worker" is a number from 1 to 65535 on a thread with a registered role
    CHECK(good.at(2).find("worker")->asInt() == 4);
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[2]["worker"] = 0; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[2]["worker"] = 65536; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[2]["worker"] = "4"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[2]["role"] = "other"; }));
    CHECK_FALSE(mutate([](json::Value& t) { t.asArray()[2]["role"] = "main"; }));
    CHECK(mutate([](json::Value& t) { t.asArray()[2]["role"] = "audio_live"; }));
    // a fault thread's worker number needs the role it was registered as
    auto plain = process();
    const auto unlabelled =
        threadsJson(collectTrace(plain.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64));
    REQUIRE(traceShape(&modules, unlabelled));
    auto numbered = unlabelled;
    numbered.asArray()[0]["worker"] = 1;
    CHECK_FALSE(traceShape(&modules, numbered));
    numbered.asArray()[0]["registered"] = "audio_live";
    CHECK(traceShape(&modules, numbered));
}

namespace {
// A minimal PE32+ image: DOS header, NT headers with a 240-byte optional header and a
// debug directory whose one CodeView entry points at an RSDS record.
struct PeOptions {
    std::uint16_t magic = 0x20b;
    std::uint32_t peOffset = 0x80;
    std::uint16_t optionalSize = 240;
    std::uint32_t directories = 16;
    std::uint32_t debugRva = 0x2000, debugSize = 28;
    std::uint32_t entryType = 2, dataSize = 0x40, dataRva = 0x2100;
    const char* signature = "RSDS";
    std::uint32_t imageSize = 0x3000;
    // The exception directory: `exceptionRva` of 0 leaves it out. The table at 0x2180 has
    // five sorted entries (see kPeFunctions); `exceptionSize` can be set to cover fewer.
    std::uint32_t exceptionRva = 0x2180, exceptionSize = 60;
    bool sortedTable = true;
};
struct PeFunction {
    std::uint32_t begin, end, unwind;
};
// Five functions with gaps between them; the unwind data lies inside the image.
constexpr PeFunction kPeFunctions[5] = {{0x1000, 0x1080, 0x2200},
                                        {0x1080, 0x10c0, 0x2210},
                                        {0x1200, 0x1210, 0x2220},
                                        {0x1400, 0x1500, 0x2230},
                                        {0x1800, 0x1801, 0x2240}};
std::vector<unsigned char> pe(const PeOptions& o = {}) {
    std::vector<unsigned char> bytes(0x2300, 0);
    poke<std::uint16_t>(bytes, 0, 0x5a4d);
    poke<std::uint32_t>(bytes, 0x3c, o.peOffset);
    if (o.peOffset + 24 + 240 <= bytes.size()) {
        std::memcpy(&bytes[o.peOffset], "PE\0\0", 4);
        poke<std::uint16_t>(bytes, o.peOffset + 4 + 16, o.optionalSize);
        const std::size_t opt = o.peOffset + 24;
        poke<std::uint16_t>(bytes, opt, o.magic);
        poke<std::uint64_t>(bytes, opt + 24, 0x140000000ULL);  // preferred base
        poke<std::uint32_t>(bytes, opt + 56, o.imageSize);
        poke<std::uint32_t>(bytes, opt + 108, o.directories);
        poke<std::uint32_t>(bytes, opt + 112 + 6 * 8, o.debugRva);
        poke<std::uint32_t>(bytes, opt + 112 + 6 * 8 + 4, o.debugSize);
        poke<std::uint32_t>(bytes, opt + 112 + 3 * 8, o.exceptionRva);
        poke<std::uint32_t>(bytes, opt + 112 + 3 * 8 + 4, o.exceptionSize);
    }
    if (o.exceptionRva == 0x2180) {
        for (std::size_t i = 0; i < 5; ++i) {
            const std::size_t at = 0x2180 + i * 12;
            const PeFunction& f = kPeFunctions[o.sortedTable ? i : 4 - i];
            poke<std::uint32_t>(bytes, at, f.begin);
            poke<std::uint32_t>(bytes, at + 4, f.end);
            poke<std::uint32_t>(bytes, at + 8, f.unwind);
        }
    }
    if (o.debugRva >= 0x200 && o.debugRva + 28 <= bytes.size()) {
        poke<std::uint32_t>(bytes, o.debugRva + 12, o.entryType);
        poke<std::uint32_t>(bytes, o.debugRva + 16, o.dataSize);
        poke<std::uint32_t>(bytes, o.debugRva + 20, o.dataRva);
    }
    if (o.dataRva >= 0x200 && o.dataRva + 24 <= bytes.size()) {
        std::memcpy(&bytes[o.dataRva], o.signature, 4);
        poke<std::uint32_t>(bytes, o.dataRva + 4, 0x12345678);  // Data1
        poke<std::uint16_t>(bytes, o.dataRva + 8, 0xabcd);      // Data2
        poke<std::uint16_t>(bytes, o.dataRva + 10, 0xef01);     // Data3
        const unsigned char tail[8] = {0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01};
        std::memcpy(&bytes[o.dataRva + 12], tail, 8);
        poke<std::uint32_t>(bytes, o.dataRva + 20, 3);  // age
        std::memcpy(&bytes[o.dataRva + 24], "C:\\secret\\project\\app.pdb", 25);
    }
    return bytes;
}
constexpr std::uint64_t kPeBase = 0x140100000ULL;
}  // namespace

TEST_CASE("a PE image yields its CodeView GUID and age, symbol-server ordered, and a bias") {
    Image image;
    image.put(kPeBase, pe());
    std::uint64_t bias = 0;
    const auto id = peDebugId(image.reader(), kPeBase, &bias);
    REQUIRE(id);
    CHECK(*id == "12345678abcdef0123456789abcdef0100000003");
    CHECK(bias == 0x100000);
    // The PDB path is never read, so it cannot reach a report.
    for (const auto& [at, size] : image.reads) {
        CHECK(size <= 1024);
        CHECK(at < kPeBase + 0x2118);
    }
}

TEST_CASE(
    "a PE image's function table is searched through the reader, for the entry that holds an "
    "address") {
    Image image;
    image.put(kPeBase, pe());
    const auto read = image.reader();
    const auto table = PeFunctionTable::open(read, kPeBase);
    REQUIRE(table);
    image.reads.clear();
    for (const auto& f : kPeFunctions) {
        // The first byte, the last byte and a byte in the middle all find the entry; the byte
        // after the end does not (unless it begins the next one).
        for (const std::uint32_t at : {f.begin, f.end - 1, f.begin + (f.end - f.begin) / 2}) {
            const auto found = table->find(read, kPeBase + at);
            REQUIRE(found);
            CHECK(found->begin == f.begin);
            CHECK(found->end == f.end);
            CHECK(found->unwind == f.unwind);
        }
    }
    // Gaps, before the first entry, after the last and outside the image find nothing.
    for (const std::uint32_t at : {0u, 0xfffu, 0x10c0u, 0x1100u, 0x11ffu, 0x1210u, 0x13ffu, 0x1500u,
                                   0x17ffu, 0x1801u, 0x2000u, 0x2fffu})
        CHECK_FALSE(table->find(read, kPeBase + at).has_value());
    CHECK_FALSE(table->find(read, kPeBase - 1).has_value());
    CHECK_FALSE(table->find(read, kPeBase + 0x3000).has_value());  // SizeOfImage
    CHECK_FALSE(table->find(read, ~std::uint64_t{0}).has_value());
    // Each search reads a few 12-byte entries and nothing else.
    for (const auto& [at, size] : image.reads) {
        CHECK(size == 12);
        CHECK(at >= kPeBase + 0x2180);
        CHECK(at < kPeBase + 0x2180 + 60);
    }
    CHECK(image.reads.size() <= (15 + 12 + 3) * 3);
}

TEST_CASE(
    "a function table that is missing, malformed, unreadable or unsorted finds nothing it should "
    "not") {
    const auto opens = [](const PeOptions& o) {
        Image image;
        image.put(kPeBase, pe(o));
        return PeFunctionTable::open(image.reader(), kPeBase).has_value();
    };
    CHECK(opens({}));
    PeOptions o;
    o = {};
    o.exceptionRva = 0;  // no exception directory
    CHECK_FALSE(opens(o));
    o = {};
    o.exceptionSize = 8;  // less than one entry
    CHECK_FALSE(opens(o));
    o = {};
    o.exceptionSize = 0xf00000;  // runs past SizeOfImage
    CHECK_FALSE(opens(o));
    o = {};
    o.directories = 3;  // the directory is not there
    CHECK_FALSE(opens(o));
    o = {};
    o.optionalSize = 100;
    CHECK_FALSE(opens(o));
    o = {};
    o.magic = 0x10b;
    CHECK_FALSE(opens(o));
    o = {};
    o.imageSize = 0;
    CHECK_FALSE(opens(o));
    // A table that stops being readable midway gives none, never a partial answer.
    {
        Image image;
        image.put(kPeBase, pe());
        const auto table = PeFunctionTable::open(image.reader(), kPeBase);
        REQUIRE(table);
        const ReadMemory failing = [](std::uint64_t, void*, std::size_t) { return false; };
        CHECK_FALSE(table->find(failing, kPeBase + 0x1000).has_value());
    }
    // An entry whose unwind data lies outside the image is refused.
    {
        Image image;
        auto bytes = pe();
        poke<std::uint32_t>(bytes, 0x2180 + 8, 0x5000);  // first entry's unwind
        image.put(kPeBase, bytes);
        const auto table = PeFunctionTable::open(image.reader(), kPeBase);
        REQUIRE(table);
        CHECK_FALSE(table->find(image.reader(), kPeBase + 0x1000).has_value());
        CHECK(table->find(image.reader(), kPeBase + 0x1080).has_value());  // others still found
    }
    // An entry that claims to reach past the image never answers for an address beyond it.
    {
        Image image;
        auto bytes = pe();
        poke<std::uint32_t>(bytes, 0x2180 + 4 * 12 + 4, 0x9000);  // last entry ends far past it
        image.put(kPeBase, bytes);
        const auto table = PeFunctionTable::open(image.reader(), kPeBase);
        REQUIRE(table);
        CHECK(table->find(image.reader(), kPeBase + 0x1800).has_value());
        CHECK_FALSE(table->find(image.reader(), kPeBase + 0x3500).has_value());
    }
    // A table in the wrong order may hide entries but terminates, in few reads, and never
    // returns an entry that does not hold the address.
    {
        PeOptions unsorted;
        unsorted.sortedTable = false;
        Image image;
        image.put(kPeBase, pe(unsorted));
        const auto table = PeFunctionTable::open(image.reader(), kPeBase);
        REQUIRE(table);
        image.reads.clear();
        for (std::uint32_t at = 0x1000; at < 0x1900; at += 0x40) {
            const auto found = table->find(image.reader(), kPeBase + at);
            if (found) {
                CHECK(found->begin <= at);
                CHECK(at < found->end);
            }
        }
        CHECK(image.reads.size() <= 0x900 / 0x40 * 4);
    }
}

TEST_CASE("malformed or truncated PE images yield no identity and bounded reads") {
    const auto none = [](const PeOptions& o) {
        Image image;
        image.put(kPeBase, pe(o));
        const auto id = peDebugId(image.reader(), kPeBase);
        for (const auto& [at, size] : image.reads) {
            CHECK(size <= 1024);
            (void)at;
        }
        return !id.has_value();
    };
    CHECK_FALSE(none({}));
    PeOptions o;
    o = {};
    o.magic = 0x10b;  // PE32
    CHECK(none(o));
    o = {};
    o.peOffset = 0x20000;  // far beyond the allowed header distance
    CHECK(none(o));
    o = {};
    o.peOffset = 8;  // inside the DOS header
    CHECK(none(o));
    o = {};
    o.optionalSize = 100;  // too small to hold the debug directory entry
    CHECK(none(o));
    o = {};
    o.optionalSize = 60000;
    CHECK(none(o));
    o = {};
    o.directories = 3;
    CHECK(none(o));
    o = {};
    o.debugRva = 0;
    CHECK(none(o));
    o = {};
    o.debugSize = 12;  // smaller than one entry
    CHECK(none(o));
    o = {};
    o.debugRva = 0x2f00;  // runs past SizeOfImage
    o.imageSize = 0x2f10;
    CHECK(none(o));
    o = {};
    o.entryType = 1;  // COFF, not CodeView
    CHECK(none(o));
    o = {};
    o.dataSize = 8;  // too small for a CodeView record
    CHECK(none(o));
    o = {};
    o.dataRva = 0;
    CHECK(none(o));
    o = {};
    o.dataRva = 0x2ff0;  // record past SizeOfImage
    CHECK(none(o));
    o = {};
    o.signature = "NB10";  // old PDB format
    CHECK(none(o));
    // Truncated memory: every prefix of the image fails cleanly.
    const auto full = pe();
    for (std::size_t keep :
         {0, 1, 63, 64, 0x80, 0x90, 0x150, 0x1ff0, 0x2000, 0x2010, 0x2100, 0x2110}) {
        Image image;
        image.put(kPeBase, std::vector<unsigned char>(
                               full.begin(), full.begin() + static_cast<std::ptrdiff_t>(keep)));
        CHECK_FALSE(peDebugId(image.reader(), kPeBase).has_value());
    }
    // A header that parsed still reports its bias when there is no CodeView record.
    Image image;
    PeOptions noDebug;
    noDebug.debugRva = 0;
    image.put(kPeBase, pe(noDebug));
    std::uint64_t bias = 0;
    CHECK_FALSE(peDebugId(image.reader(), kPeBase, &bias).has_value());
    CHECK(bias == 0x100000);
}

TEST_CASE("a platform layout and walker replace the maps file and the frame-pointer walk") {
    auto p = process();
    p.maps.clear();  // a maps file is not needed when a layout is supplied
    auto ops = p.ops();
    Layout supplied;
    supplied.modules.push_back({std::string("00112233445566778899aabbccddeeff00000001"), 0x400000});
    supplied.regions.push_back({0x400000, 0x500000, 0});
    ops.layout = [&] { return std::optional<Layout>(supplied); };
    std::vector<std::uint64_t> walked;
    ops.walk = [&](std::uint64_t tid, const Regs& regs, const Layout& layout) {
        walked.push_back(tid);
        Walk walk;
        auto frame = layoutFrame(layout, regs.pc, false);
        REQUIRE(frame);
        walk.frames.push_back(*frame);
        walk.stop = Stop::End;
        return walk;
    };
    const auto snapshot = collectTrace(ops, 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    REQUIRE(snapshot.threads.size() == 3);
    CHECK(walked == std::vector<std::uint64_t>{11, 10, 12});  // fault, main, the rest
    for (const auto& t : snapshot.threads) CHECK(t.missing == Missing::None);
    CHECK(snapshot.threads[0].walk.frames[0].offset == 0x200);
    REQUIRE(snapshot.modules.size() == 1);
    CHECK(*snapshot.modules[0].id == "00112233445566778899aabbccddeeff00000001");
    // A layout that cannot be read marks every thread, the faulting one included.
    ops.layout = [] { return std::optional<Layout>(); };
    walked.clear();
    const auto none = collectTrace(ops, 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    for (const auto& t : none.threads) CHECK(t.missing == Missing::MapsUnreadable);
    CHECK(walked.empty());
}

TEST_CASE("access_denied is a valid reason and validates strictly") {
    auto p = process();
    p.refuse[10] = Missing::AccessDenied;
    const auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    CHECK(snapshot.threads[1].missing == Missing::AccessDenied);
    const auto modules = modulesJson(snapshot);
    const auto threads = threadsJson(snapshot);
    CHECK(traceShape(&modules, threads));
    CHECK(json::writeCanonical(threads).find("\"access_denied\"") != std::string::npos);
    CHECK(p.releases == 1);  // only the thread that was stopped is released
}

namespace {
using supple::diagctx::Entry;
using supple::diagctx::packWord;
// A word written into the table image: `audio` or `main` of `cell`.
void setWord(supple::diagctx::TableImage& image, std::uint32_t cell, bool audio,
             std::uint64_t word) {
    (audio ? image.instances[cell].audio : image.instances[cell].main) = word;
}
// Where a word lives in the fake process, as the second read looks for it.
std::uint64_t wordAddress(std::uint32_t cell, bool audio) {
    return kTableAt + offsetof(supple::diagctx::TableImage, instances) +
           cell * sizeof(supple::diagctx::TableImage::InstanceCell) + (audio ? 0 : 8);
}
// Three threads on three stacks: 11 faults (stack 0x6000-0x7100, sp 0x6ff0), 10 is registered
// (stack 0xa000-0xc000 in its slot, sp 0xa800, no mapping), 12 is not (stack 0xe000-0x10000
// in the maps, sp 0xe800).
FakeProcess scoped(supple::diagctx::TableImage& image) {
    auto p = process();
    p.maps += "6000-7100 rw-p 00000000 00:00 0\ne000-10000 rw-p 00000000 00:00 0 [stack]\n";
    p.regs[11] = {0x400200, 0x6ff0, 0x7000};
    p.regs[10] = {0x400100, 0xa800, 0xa900};
    p.regs[12] = {0x400100, 0xe800, 0xe900};
    p.image.frame(0xa900, 0, 0x401000);
    p.image.frame(0xe900, 0, 0x401000);
    claim(image, 0, 10, ThreadRole::AudioLive, 1);
    image.threads[0].stackLow = 0xa000;
    image.threads[0].stackHigh = 0xc000;
    return p;
}
// Overwrites a word of the table in the fake process, as a running thread would.
void patchWord(FakeProcess& p, std::uint64_t address, std::uint64_t word) {
    auto& part = p.image.parts[kTableAt];
    REQUIRE(address >= kTableAt);
    REQUIRE(address - kTableAt + sizeof(word) <= part.size());
    std::memcpy(part.data() + (address - kTableAt), &word, sizeof(word));
}
TraceSnapshot collectScoped(FakeProcess& p, const supple::diagctx::TableImage& image) {
    put(p, image);
    return collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64, kTableAt);
}
const ThreadTrace& threadOf(const TraceSnapshot& snapshot, Role role, ThreadRole registered) {
    for (const auto& t : snapshot.threads)
        if (t.role == role && t.registered == registered) return t;
    FAIL("no such thread");
    return snapshot.threads[0];
}
}  // namespace

TEST_CASE("an open entry is tied to the thread whose stack holds its address") {
    auto image = tableImage();
    auto p = scoped(image);
    setWord(image, 5, true, packWord(Entry::Process, 3, 0x7000));  // fault thread, own stack
    setWord(image, 7, true, packWord(Entry::Reset, 9, 0xb000));    // thread 10, by its slot
    setWord(image, 8, false, packWord(Entry::State, 2, 0xf000));   // thread 12, by the maps
    const auto snapshot = collectScoped(p, image);
    REQUIRE(snapshot.table == TableState::Read);
    REQUIRE(snapshot.threads.size() == 3);
    const auto& fault = snapshot.threads[0];
    CHECK(fault.entry == Entry::Process);
    CHECK(fault.entryCell == 5);
    CHECK(fault.entryGeneration == 3);
    const auto& registered = threadOf(snapshot, Role::Main, ThreadRole::AudioLive);
    CHECK(registered.entry == Entry::Reset);
    CHECK(registered.entryCell == 7);
    CHECK(registered.entryGeneration == 9);
    const auto& other = snapshot.threads[2];
    CHECK(other.registered == ThreadRole::None);
    CHECK(other.entry == Entry::State);
    CHECK(other.entryCell == 8);
    CHECK(snapshot.unplacedEntries == 0);
    const auto limits = traceLimits(snapshot);
    for (const auto& limit : limits) CHECK(limit.find("plug-in entries") == std::string::npos);
}

TEST_CASE("of nested entries the thread is in the innermost, and the outer ones are still placed") {
    auto image = tableImage();
    auto p = scoped(image);
    setWord(image, 6, false, packWord(Entry::Params, 1, 0x7080));  // outer: higher address
    setWord(image, 5, true, packWord(Entry::Process, 3, 0x7000));  // inner: lower address
    setWord(image, 9, true, packWord(Entry::Flush, 4, 0x70a0));    // between the two
    const auto snapshot = collectScoped(p, image);
    CHECK(snapshot.threads[0].entry == Entry::Process);
    CHECK(snapshot.threads[0].entryCell == 5);
    CHECK(snapshot.unplacedEntries == 0);
}

TEST_CASE("an entry above the stack pointer's frame, or on no stack, is not claimed") {
    auto image = tableImage();
    auto p = scoped(image);
    // Below the stack pointer: a stale word from a frame that has returned.
    setWord(image, 1, true, packWord(Entry::Process, 1, 0x6f00));
    // A forged address beyond any real stack, and one the table could not hold at all.
    setWord(image, 2, true, packWord(Entry::Process, 1, supple::diagctx::kWordStackLimit - 16));
    setWord(image, 3, true, packWord(Entry::Process, 1, ~std::uint64_t{0}));  // recorded as none
    // Inside the thread's mapping but past its end.
    setWord(image, 4, false, packWord(Entry::Params, 1, 0x7100));
    const auto snapshot = collectScoped(p, image);
    for (const auto& t : snapshot.threads) CHECK(t.entry == Entry::None);
    CHECK(snapshot.unplacedEntries == 4);
    bool said = false;
    for (const auto& limit : traceLimits(snapshot))
        said = said || limit.find("no thread read could be tied to") != std::string::npos;
    CHECK(said);
}

TEST_CASE("a thread that is stopped is read again, and only an entry both reads agree on counts") {
    auto image = tableImage();
    auto p = scoped(image);
    setWord(image, 7, true, packWord(Entry::Reset, 9, 0xb000));    // thread 10
    setWord(image, 8, false, packWord(Entry::State, 2, 0xf000));   // thread 12
    setWord(image, 5, true, packWord(Entry::Process, 3, 0x7000));  // the fault thread
    // Thread 10 leaves its entry between the copy and its stop; thread 12 stays in its own.
    p.onStop = [&](std::uint64_t tid) {
        if (tid == 10) patchWord(p, wordAddress(7, true), packWord(Entry::None, 9, 0));
    };
    const auto snapshot = collectScoped(p, image);
    const auto& left = threadOf(snapshot, Role::Main, ThreadRole::AudioLive);
    CHECK(left.entry == Entry::None);
    CHECK(snapshot.threads[2].entry == Entry::State);
    CHECK(snapshot.threads[0].entry == Entry::Process);  // parked in its handler: no second read
    CHECK(snapshot.unplacedEntries == 1);                // the entry that was left, as copied
}

TEST_CASE("an entry changed to another by the second read is not trusted either") {
    auto image = tableImage();
    auto p = scoped(image);
    setWord(image, 7, true, packWord(Entry::Reset, 9, 0xb000));
    p.onStop = [&](std::uint64_t tid) {
        if (tid == 10)  // left and re-entered: a different entry at the same address
            patchWord(p, wordAddress(7, true), packWord(Entry::Process, 9, 0xb000));
    };
    const auto snapshot = collectScoped(p, image);
    CHECK(threadOf(snapshot, Role::Main, ThreadRole::AudioLive).entry == Entry::None);
    CHECK(snapshot.unplacedEntries == 1);
}

TEST_CASE("a thread whose stack is unknown gets no entry, and the entry is unplaced") {
    auto image = tableImage();
    auto p = scoped(image);
    p.maps = "400000-500000 r-xp 00000000 08:01 7 /secret/path/app\n";  // no stacks at all
    // Thread 10's slot still names its stack, thread 12 and the fault thread have nothing.
    setWord(image, 8, false, packWord(Entry::State, 2, 0xf000));
    setWord(image, 5, true, packWord(Entry::Process, 3, 0x7000));
    const auto snapshot = collectScoped(p, image);
    CHECK(snapshot.threads[0].entry == Entry::None);
    CHECK(snapshot.threads[2].entry == Entry::None);
    CHECK(snapshot.unplacedEntries == 2);
}

TEST_CASE("entries without a table, or from a table that is not ours, are not read") {
    auto image = tableImage();
    auto p = scoped(image);
    setWord(image, 5, true, packWord(Entry::Process, 3, 0x7000));
    image.header.magic ^= 1;
    const auto snapshot = collectScoped(p, image);
    CHECK(snapshot.table == TableState::Invalid);
    for (const auto& t : snapshot.threads) CHECK(t.entry == Entry::None);
    CHECK(snapshot.unplacedEntries == 0);
}

TEST_CASE("the table's counters become report limits") {
    auto image = tableImage();
    auto p = scoped(image);
    auto limitFor = [](const TraceSnapshot& snapshot, const char* text) {
        for (const auto& limit : traceLimits(snapshot))
            if (limit.find(text) != std::string::npos) return true;
        return false;
    };
    auto clean = collectScoped(p, image);
    CHECK_FALSE(limitFor(clean, "beyond the execution table's capacity are not attributed"));
    CHECK_FALSE(limitFor(clean, "stack address the table cannot hold"));
    image.header.untracked = 2;
    image.header.scopeOverflow = 1;
    const auto counted = collectScoped(p, image);
    CHECK(counted.tableUntracked == 2);
    CHECK(counted.tableScopeOverflow == 1);
    CHECK(limitFor(counted, "instances beyond the execution table's capacity are not attributed"));
    CHECK(limitFor(counted, "stack address the table cannot hold"));
}

TEST_CASE("the table's command ring is read with the table, validated and ordered") {
    namespace dc = supple::diagctx;
    auto image = tableImage();
    // Entries 1 to 70 were written: the ring holds 7 to 70, in slot order, not time order.
    image.header.commandHead = 70;
    for (std::uint64_t seq = 7; seq <= 70; ++seq)
        image.commands[seq % dc::kMaxCommandEntries] = {seq, static_cast<std::uint32_t>(seq) |
                                                                 std::uint64_t{1 + seq % 7} << 32 |
                                                                 std::uint64_t{2} << 40};
    image.header.commandsDropped = 3;
    auto p = scoped(image);
    const auto snapshot = collectScoped(p, image);
    REQUIRE(snapshot.commands.size() == 64);
    for (std::size_t i = 0; i < 64; ++i) {
        CHECK(snapshot.commands[i].seq == 7 + i);  // oldest first
        CHECK(snapshot.commands[i].nameId == 7 + i);
        CHECK(snapshot.commands[i].state == dc::CommandState::Ended);
    }
    CHECK(snapshot.commandsDamaged == 0);
    CHECK(snapshot.commandsRefused == 3);
    // A slot damaged after the fact is left out and counted; the rest still read.
    image.commands[10].second |= std::uint64_t{0xff} << 32;  // an origin outside the set
    const auto damaged = collectScoped(p, image);
    CHECK(damaged.commands.size() == 63);
    CHECK(damaged.commandsDamaged == 1);
    // Without a table that is ours there are no commands to show.
    image.header.magic = 0;
    const auto invalid = collectScoped(p, image);
    CHECK(invalid.table == TableState::Invalid);
    CHECK(invalid.commands.empty());
}

TEST_CASE("an entry reaches the report only as a name from the closed set") {
    auto image = tableImage();
    auto p = scoped(image);
    setWord(image, 5, true, packWord(Entry::StartProcessing, 3, 0x7000));
    const auto snapshot = collectScoped(p, image);
    const auto modules = modulesJson(snapshot);
    auto threads = threadsJson(snapshot);
    REQUIRE(traceShape(&modules, threads));
    CHECK(threads.at(0).find("entry")->asString() == "start_processing");
    CHECK(threads.at(1).find("entry") == nullptr);  // a thread in no entry says nothing
    // The cell and generation stay in the process: nothing numeric about them is reported.
    const auto text = json::writeCanonical(threads);
    CHECK(text.find("\"cell\"") == std::string::npos);
    CHECK(text.find("generation") == std::string::npos);
    for (const char* bad : {"none", "", "Process", "run", "process "}) {
        auto changed = threads;
        changed.asArray()[0]["entry"] = bad;
        CHECK_FALSE(traceShape(&modules, changed));
    }
    auto number = threads;
    number.asArray()[0]["entry"] = 7;
    CHECK_FALSE(traceShape(&modules, number));
}
