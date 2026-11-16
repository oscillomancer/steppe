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
#include <map>
#include <vector>
#include "../src/private_files.h"
#include "../src/storage.h"
#include "../src/trace.h"
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
fs::path rootFor(const char* name) { return fs::temp_directory_path() / name; }
Packet bootFor(unsigned id) {
    Packet p;
    p.role = 3;
    std::fill(p.incident.begin(), p.incident.begin() + 32, '0');
    p.incident[30] = "0123456789abcdef"[(id >> 4) & 15];
    p.incident[31] = "0123456789abcdef"[id & 15];
    return p;
}
}  // namespace

TEST_CASE("a report with a trace is published, validated and retained like any other") {
    auto id = incidentId();
    REQUIRE(id);
    const auto root = rootFor(id->c_str()) / "app" / "diagnostics";
    REQUIRE(prepareDirectory(root));
    auto p = process();
    const auto snapshot = collectTrace(p.ops(), 11, {0x400200, 0x6ff0, 0x7000}, 10, Arch::X86_64);
    Packet terminal = bootFor(1);
    terminal.kind = Message::Terminal;
    terminal.pc = 0x400200;
    REQUIRE(publishReport(root, bootFor(1), &terminal, 11, true, &snapshot));
    const auto file = root / (std::string(bootFor(1).incident.data()) + ".json");
    auto text = privateRead(file);
    REQUIRE(text);
    auto value = json::parse(*text);
    REQUIRE(value);
    CHECK(value->find("threads")->size() == 3);
    CHECK(value->find("modules")->size() == 1);
    CHECK_FALSE(value->find("complete")->asBool());
    CHECK(text->find("secret") == std::string::npos);
    // Validated reports take part in retention: the oldest of 22 goes. Each report is
    // aged into the past right after it is published, so the one being published is
    // always the newest and report 1 the oldest.
    const auto age = [&](unsigned i) {
        fs::last_write_time(root / (std::string(bootFor(i).incident.data()) + ".json"),
                            fs::file_time_type::clock::now() - std::chrono::seconds(1000 - i));
    };
    age(1);
    for (unsigned i = 2; i < 23; ++i) {
        REQUIRE(publishReport(root, bootFor(i), &terminal, 11, true, &snapshot));
        age(i);
    }
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.path().extension() == ".json") ++count;
    CHECK(count == 20);
    // A report whose nested trace is malformed is never authority to delete itself.
    auto bad = *value;
    bad["incident"] = std::string(bootFor(200).incident.data());
    bad["threads"].asArray()[0]["frames"].asArray()[0]["module"] = 77;
    const auto badFile = root / (std::string(bootFor(200).incident.data()) + ".json");
    REQUIRE(privatePublish(badFile, json::writeCanonical(bad)));
    fs::last_write_time(badFile, fs::file_time_type::clock::now() - std::chrono::hours(48));
    // Over the limit again, with the malformed report the oldest entry of all.
    for (unsigned i = 23; i < 25; ++i)
        REQUIRE(publishReport(root, bootFor(i), &terminal, 11, true, &snapshot));
    REQUIRE(pruneReports(root));
    CHECK(fs::exists(badFile));
    count = 0;
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.path().extension() == ".json") ++count;
    CHECK(count == 21);  // 20 validated reports plus the preserved malformed one
    std::error_code ec;
    fs::remove_all(rootFor(id->c_str()), ec);
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
};
std::vector<unsigned char> pe(const PeOptions& o = {}) {
    std::vector<unsigned char> bytes(0x2200, 0);
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
