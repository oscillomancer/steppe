// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagcap/trace.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <utility>
#include "supple/diagcap/capsule.h"
namespace supple::diagnostics::detail {
namespace {
constexpr std::uint64_t kMaxStep = std::uint64_t{1} << 30;  // largest frame-to-frame jump
constexpr std::size_t kMaxNoteBytes = 64 * 1024;
constexpr std::size_t kMaxProgramHeaders = 64;
constexpr std::size_t kMaxBuildIdBytes = 64;

template <class T>
T load(const unsigned char* at) {
    T value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
std::string hex(const unsigned char* bytes, std::size_t count) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += "0123456789abcdef"[bytes[i] >> 4];
        text += "0123456789abcdef"[bytes[i] & 15];
    }
    return text;
}
bool hexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool parseHex(std::string_view& text, std::uint64_t& value) {
    value = 0;
    std::size_t digits = 0;
    while (!text.empty()) {
        const char c = text.front();
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<unsigned>(c - 'a' + 10);
        else
            break;
        if (++digits > 16) return false;
        value = value << 4 | digit;
        text.remove_prefix(1);
    }
    return digits > 0;
}
bool skip(std::string_view& text, char c) {
    if (text.empty() || text.front() != c) return false;
    text.remove_prefix(1);
    return true;
}
void skipSpaces(std::string_view& text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
}
const char* missingName(Missing why) {
    switch (why) {
        case Missing::PtraceDenied:
            return "ptrace_denied";
        case Missing::AccessDenied:
            return "access_denied";
        case Missing::ThreadExited:
            return "thread_exited";
        case Missing::Deadline:
            return "deadline";
        case Missing::RegistersUnreadable:
            return "registers_unreadable";
        case Missing::MapsUnreadable:
            return "maps_unreadable";
        case Missing::Unsupported:
            return "unsupported";
        default:
            return "not_collected";
    }
}
const char* stopName(Stop stop) {
    switch (stop) {
        case Stop::End:
            return "end";
        case Stop::Depth:
            return "depth";
        case Stop::Unreadable:
            return "unreadable";
        case Stop::NotExecutable:
            return "not_executable";
        case Stop::BadFramePointer:
            return "bad_frame_pointer";
        case Stop::Cycle:
            return "cycle";
        default:
            return "deadline";
    }
}
const char* formatName() {
#if defined(_WIN32)
    return "pe";
#elif defined(__APPLE__)
    return "macho";
#else
    return "elf";
#endif
}
const Region* regionOf(const Layout& layout, std::uint64_t address) {
    auto it = std::upper_bound(layout.regions.begin(), layout.regions.end(), address,
                               [](std::uint64_t a, const Region& r) { return a < r.start; });
    if (it == layout.regions.begin()) return nullptr;
    --it;
    return address < it->end ? &*it : nullptr;
}
std::optional<Frame> frameAt(const Layout& layout, std::uint64_t address, bool returnAddress) {
    const Region* region = regionOf(layout, address);
    if (!region) return std::nullopt;
    Frame frame;
    frame.returnAddress = returnAddress;
    frame.module = region->module;
    if (region->module >= 0)
        frame.offset = address - layout.modules[static_cast<std::size_t>(region->module)].bias;
    return frame;
}
}  // namespace

std::vector<Mapping> parseMaps(std::string_view text) {
    std::vector<Mapping> result;
    while (!text.empty() && result.size() < kMaxMappings) {
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        Mapping m;
        std::uint64_t start, end, offset, major, minor;
        if (!parseHex(line, start) || !skip(line, '-') || !parseHex(line, end) ||
            !skip(line, ' ') || line.size() < 5)
            continue;
        const auto perms = line.substr(0, 4);
        line.remove_prefix(4);
        if (!skip(line, ' ') || !parseHex(line, offset) || !skip(line, ' ') ||
            !parseHex(line, major) || !skip(line, ':') || !parseHex(line, minor) ||
            !skip(line, ' '))
            continue;
        skipSpaces(line);
        std::uint64_t inode = 0;
        std::size_t digits = 0;
        while (!line.empty() && line.front() >= '0' && line.front() <= '9') {
            if (++digits > 19) break;
            inode = inode * 10 + static_cast<std::uint64_t>(line.front() - '0');
            line.remove_prefix(1);
        }
        if (digits == 0 || digits > 19 || end <= start) continue;
        skipSpaces(line);
        m.start = start;
        m.end = end;
        m.offset = offset;
        m.inode = inode;
        m.read = perms[0] == 'r';
        m.write = perms[1] == 'w';
        m.execute = perms[2] == 'x';
        m.named = !line.empty() && line.front() == '/';
        m.device = std::to_string(major) + ":" + std::to_string(minor);
        result.push_back(std::move(m));
    }
    return result;
}

std::optional<std::string> elfBuildId(const ReadMemory& read, std::uint64_t base,
                                      std::uint64_t* bias) {
    std::array<unsigned char, 64> header{};
    if (!read(base, header.data(), header.size())) return std::nullopt;
    if (std::memcmp(header.data(), "\177ELF", 4) != 0 || header[4] != 2 || header[5] != 1)
        return std::nullopt;  // 64-bit little-endian only
    const auto type = load<std::uint16_t>(&header[16]);
    const auto phoff = load<std::uint64_t>(&header[32]);
    const auto phentsize = load<std::uint16_t>(&header[54]);
    const auto phnum = load<std::uint16_t>(&header[56]);
    if ((type != 2 && type != 3) || phentsize != 56 || phnum == 0 || phnum > kMaxProgramHeaders ||
        phoff < 64 || phoff > 65536)
        return std::nullopt;
    std::vector<unsigned char> headers(std::size_t{phnum} * 56);
    if (!read(base + phoff, headers.data(), headers.size())) return std::nullopt;
    std::uint64_t loadBias = 0;
    bool haveBias = type == 2;  // fixed-address executables load where linked
    std::vector<std::pair<std::uint64_t, std::uint64_t>> notes;  // vaddr, size
    for (std::size_t i = 0; i < phnum; ++i) {
        const unsigned char* ph = &headers[i * 56];
        const auto kind = load<std::uint32_t>(ph);
        const auto fileOffset = load<std::uint64_t>(ph + 8);
        const auto vaddr = load<std::uint64_t>(ph + 16);
        const auto filesz = load<std::uint64_t>(ph + 32);
        if (kind == 1 && fileOffset == 0 && !haveBias) {
            loadBias = base - vaddr;
            haveBias = true;
        } else if (kind == 4 && filesz > 0 && filesz <= kMaxNoteBytes && notes.size() < 4) {
            notes.emplace_back(vaddr, filesz);
        }
    }
    if (!haveBias) return std::nullopt;
    if (bias) *bias = loadBias;
    for (const auto& [vaddr, size] : notes) {
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        if (!read(loadBias + vaddr, bytes.data(), bytes.size())) continue;
        std::uint64_t at = 0;
        while (at + 12 <= size) {
            const auto namesz = load<std::uint32_t>(&bytes[at]);
            const auto descsz = load<std::uint32_t>(&bytes[at + 4]);
            const auto kind = load<std::uint32_t>(&bytes[at + 8]);
            const std::uint64_t name = at + 12;
            const std::uint64_t desc = name + ((std::uint64_t{namesz} + 3) & ~std::uint64_t{3});
            const std::uint64_t next = desc + ((std::uint64_t{descsz} + 3) & ~std::uint64_t{3});
            if (next > size) break;
            if (kind == 3 && namesz == 4 && std::memcmp(&bytes[name], "GNU\0", 4) == 0 &&
                descsz >= 1 && descsz <= kMaxBuildIdBytes)
                return hex(&bytes[desc], descsz);
            at = next;
        }
    }
    return std::nullopt;
}

std::optional<std::string> peDebugId(const ReadMemory& read, std::uint64_t base,
                                     std::uint64_t* bias) {
    std::array<unsigned char, 64> dos{};
    if (!read(base, dos.data(), dos.size()) || load<std::uint16_t>(dos.data()) != 0x5a4d)
        return std::nullopt;  // "MZ"
    const auto peOffset = load<std::uint32_t>(&dos[0x3c]);
    if (peOffset < 64 || peOffset > 0x10000) return std::nullopt;
    // "PE\0\0", the 20-byte COFF header, then the optional header.
    std::array<unsigned char, 24> coff{};
    if (!read(base + peOffset, coff.data(), coff.size()) ||
        std::memcmp(coff.data(), "PE\0\0", 4) != 0)
        return std::nullopt;
    const auto optionalSize = load<std::uint16_t>(&coff[20]);
    constexpr std::size_t kDebugEntry = 112 + 6 * 8;  // data directory 6, past 7 entries
    if (optionalSize < kDebugEntry + 8 || optionalSize > 1024) return std::nullopt;
    std::vector<unsigned char> optional(optionalSize);
    if (!read(base + peOffset + 24, optional.data(), optional.size())) return std::nullopt;
    if (load<std::uint16_t>(optional.data()) != 0x20b) return std::nullopt;  // PE32+
    const auto preferred = load<std::uint64_t>(&optional[24]);
    const auto imageSize = load<std::uint32_t>(&optional[56]);
    const auto directories = load<std::uint32_t>(&optional[108]);
    if (bias) *bias = base - preferred;
    if (directories < 7 || imageSize == 0) return std::nullopt;
    const auto debugRva = load<std::uint32_t>(&optional[kDebugEntry]);
    const auto debugSize = load<std::uint32_t>(&optional[kDebugEntry + 4]);
    constexpr std::size_t kEntry = 28, kMaxEntries = 16;
    if (debugRva == 0 || debugSize < kEntry || std::uint64_t{debugRva} + debugSize > imageSize)
        return std::nullopt;
    const std::size_t count = std::min<std::size_t>(debugSize / kEntry, kMaxEntries);
    std::vector<unsigned char> entries(count * kEntry);
    if (!read(base + debugRva, entries.data(), entries.size())) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* entry = &entries[i * kEntry];
        const auto type = load<std::uint32_t>(entry + 12);
        const auto dataSize = load<std::uint32_t>(entry + 16);
        const auto dataRva = load<std::uint32_t>(entry + 20);
        if (type != 2 || dataSize < 24 || dataRva == 0 || std::uint64_t{dataRva} + 24 > imageSize)
            continue;
        std::array<unsigned char, 24> record{};  // "RSDS", GUID, age; the path is not read
        if (!read(base + dataRva, record.data(), record.size()) ||
            std::memcmp(record.data(), "RSDS", 4) != 0)
            continue;
        // GUID fields in symbol-server order: Data1, Data2, Data3 as numbers, Data4 as bytes.
        std::array<unsigned char, 16> guid{};
        const auto data1 = load<std::uint32_t>(&record[4]);
        const auto data2 = load<std::uint16_t>(&record[8]);
        const auto data3 = load<std::uint16_t>(&record[10]);
        for (std::size_t b = 0; b < 4; ++b)
            guid[b] = static_cast<unsigned char>((data1 >> (24 - 8 * b)) & 0xff);
        guid[4] = static_cast<unsigned char>(data2 >> 8);
        guid[5] = static_cast<unsigned char>(data2 & 0xff);
        guid[6] = static_cast<unsigned char>(data3 >> 8);
        guid[7] = static_cast<unsigned char>(data3 & 0xff);
        std::memcpy(&guid[8], &record[12], 8);
        const auto age = load<std::uint32_t>(&record[20]);
        std::array<unsigned char, 4> ageBytes{
            static_cast<unsigned char>(age >> 24), static_cast<unsigned char>((age >> 16) & 0xff),
            static_cast<unsigned char>((age >> 8) & 0xff), static_cast<unsigned char>(age & 0xff)};
        return hex(guid.data(), guid.size()) + hex(ageBytes.data(), ageBytes.size());
    }
    return std::nullopt;
}

std::optional<PeFunctionTable> PeFunctionTable::open(const ReadMemory& read, std::uint64_t base) {
    std::array<unsigned char, 64> dos{};
    if (!read(base, dos.data(), dos.size()) || load<std::uint16_t>(dos.data()) != 0x5a4d)
        return std::nullopt;
    const auto peOffset = load<std::uint32_t>(&dos[0x3c]);
    if (peOffset < 64 || peOffset > 0x10000) return std::nullopt;
    std::array<unsigned char, 24> coff{};
    if (!read(base + peOffset, coff.data(), coff.size()) ||
        std::memcmp(coff.data(), "PE\0\0", 4) != 0)
        return std::nullopt;
    const auto optionalSize = load<std::uint16_t>(&coff[20]);
    constexpr std::size_t kExceptionEntry = 112 + 3 * 8;  // data directory 3
    if (optionalSize < kExceptionEntry + 8 || optionalSize > 1024) return std::nullopt;
    std::vector<unsigned char> optional(optionalSize);
    if (!read(base + peOffset + 24, optional.data(), optional.size())) return std::nullopt;
    if (load<std::uint16_t>(optional.data()) != 0x20b) return std::nullopt;  // PE32+
    const auto imageSize = load<std::uint32_t>(&optional[56]);
    const auto directories = load<std::uint32_t>(&optional[108]);
    if (directories < 4 || imageSize == 0) return std::nullopt;
    const auto rva = load<std::uint32_t>(&optional[kExceptionEntry]);
    const auto size = load<std::uint32_t>(&optional[kExceptionEntry + 4]);
    constexpr std::uint32_t kEntry = 12;
    if (rva == 0 || size < kEntry || rva % 4 != 0 || std::uint64_t{rva} + size > imageSize)
        return std::nullopt;
    PeFunctionTable table;
    table.base_ = base;
    table.imageSize_ = imageSize;
    table.rva_ = rva;
    table.count_ = size / kEntry;
    return table;
}

std::optional<RuntimeFunction> PeFunctionTable::find(const ReadMemory& read,
                                                     std::uint64_t address) const {
    if (count_ == 0 || address < base_ || address - base_ >= imageSize_) return std::nullopt;
    const auto target = static_cast<std::uint32_t>(address - base_);
    std::uint32_t low = 0, high = count_;
    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2;
        std::array<unsigned char, 12> entry{};
        if (!read(base_ + rva_ + std::uint64_t{middle} * entry.size(), entry.data(), entry.size()))
            return std::nullopt;
        const auto begin = load<std::uint32_t>(&entry[0]);
        const auto end = load<std::uint32_t>(&entry[4]);
        const auto unwind = load<std::uint32_t>(&entry[8]);
        if (target < begin)
            high = middle;
        else if (target >= end)
            low = middle + 1;
        else
            return unwind != 0 && unwind < imageSize_
                       ? std::optional<RuntimeFunction>(RuntimeFunction{begin, end, unwind})
                       : std::nullopt;
    }
    return std::nullopt;
}

Layout buildLayout(const std::vector<Mapping>& mappings, const ReadMemory& read) {
    Layout layout;
    // Group file-backed mappings by (device, inode): one module per file.
    std::map<std::pair<std::string, std::uint64_t>, int> modules;
    std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> lowest;
    for (const auto& m : mappings) {
        if (!m.named || m.inode == 0) continue;
        const auto key = std::make_pair(m.device, m.inode);
        if (m.offset == 0) {
            auto it = lowest.find(key);
            if (it == lowest.end() || m.start < it->second) lowest[key] = m.start;
        }
    }
    for (const auto& m : mappings) {
        if (!m.execute || !m.read) continue;
        Region region{m.start, m.end, -1};
        if (m.named && m.inode != 0) {
            const auto key = std::make_pair(m.device, m.inode);
            auto found = modules.find(key);
            if (found == modules.end() && layout.modules.size() < kMaxModules) {
                Module module;
                const auto base = lowest.find(key);
                if (base != lowest.end()) {
                    std::uint64_t bias = base->second;
                    module.id = elfBuildId(read, base->second, &bias);
                    module.bias = bias;
                } else {
                    module.bias = m.start - m.offset;
                }
                layout.modules.push_back(std::move(module));
                found = modules.emplace(key, static_cast<int>(layout.modules.size() - 1)).first;
            }
            if (found != modules.end())
                region.module = found->second;
            else
                layout.truncatedModules = true;
        }
        layout.regions.push_back(region);
    }
    std::sort(layout.regions.begin(), layout.regions.end(),
              [](const Region& a, const Region& b) { return a.start < b.start; });
    // Mappings never overlap in a real process; a malformed list must not
    // produce ambiguous lookups, so later overlapping entries are dropped.
    std::vector<Region> clean;
    for (const auto& r : layout.regions)
        if (clean.empty() || r.start >= clean.back().end) clean.push_back(r);
    layout.regions = std::move(clean);
    return layout;
}

std::optional<Frame> layoutFrame(const Layout& layout, std::uint64_t address, bool returnAddress) {
    return frameAt(layout, address, returnAddress);
}

namespace {
std::atomic<EvidenceSink> evidenceSink{nullptr};
}
void setTraceEvidence(EvidenceSink sink) noexcept { evidenceSink.store(sink); }
bool traceEvidenceEnabled() noexcept { return evidenceSink.load() != nullptr; }
void traceEvidence(const char* line) {
    if (const auto sink = evidenceSink.load()) sink(line);
}

Arch hostArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return Arch::Arm64;
#else
    return Arch::X86_64;
#endif
}

Walk walkFrames(const ReadMemory& read, const Layout& layout, const Regs& regs, Arch arch,
                const std::function<bool()>& expired) {
    Walk walk;
    if (auto frame = frameAt(layout, regs.pc, false))
        walk.frames.push_back(*frame);
    else
        walk.frames.push_back(Frame{});  // pc outside executable memory: nothing to record
    std::uint64_t fp = regs.fp, previous = 0;
    bool first = true;
    for (;;) {
        if (expired && (walk.frames.size() & 15) == 0 && expired()) {
            walk.stop = Stop::Deadline;
            break;
        }
        if (fp == 0) {
            walk.stop = Stop::End;
            break;
        }
        if (walk.frames.size() >= kMaxFrames) {
            walk.stop = Stop::Depth;
            break;
        }
        if ((fp & 7) != 0 || (first && regs.sp != 0 && fp < regs.sp)) {
            walk.stop = Stop::BadFramePointer;
            break;
        }
        if (!first && fp <= previous) {
            walk.stop = Stop::Cycle;
            break;
        }
        if (!first && fp - previous > kMaxStep) {
            walk.stop = Stop::BadFramePointer;
            break;
        }
        std::array<std::uint64_t, 2> record{};
        if (!read(fp, record.data(), sizeof(record))) {
            walk.stop = Stop::Unreadable;
            break;
        }
        std::uint64_t ret = record[1];
        if (arch == Arch::Arm64) ret &= 0x0000ffffffffffffULL;  // strip pointer-auth bits
        if (ret == 0) {
            walk.stop = Stop::End;
            break;
        }
        auto frame = frameAt(layout, ret, true);
        if (!frame) {
            walk.stop = Stop::NotExecutable;
            break;
        }
        walk.frames.push_back(*frame);
        previous = fp;
        fp = record[0];
        first = false;
    }
    return walk;
}

std::string roleString(const ThreadTrace& thread) {
    if (thread.role == Role::Fault) return "fault";
    if (const char* name = diagctx::threadRoleName(thread.registered)) return name;
    return thread.role == Role::Main ? "model" : "other";
}

TraceSnapshot collectTrace(const TraceOps& ops, std::uint64_t faultTid, const Regs& faultRegs,
                           std::uint64_t mainTid, Arch arch, std::uint64_t tableAddress) {
    TraceSnapshot snapshot;
    const auto expired = [&] { return ops.expired && ops.expired(); };
    std::vector<std::uint64_t> tids = ops.threads ? ops.threads() : std::vector<std::uint64_t>{};
    std::sort(tids.begin(), tids.end());
    tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
    // Order: fault thread (always), main, then by thread id. The fault thread is read
    // from the capsule's registers when it has them; a capsule without (terminate,
    // abort) names only the thread, which is parked in its handler and read like any
    // other, so its real stack is never filed under another role.
    std::vector<std::pair<std::uint64_t, Role>> order;
    order.emplace_back(faultTid, Role::Fault);
    const bool mainListed =
        mainTid != faultTid && std::binary_search(tids.begin(), tids.end(), mainTid);
    if (mainListed) order.emplace_back(mainTid, Role::Main);
    for (auto tid : tids)
        if (tid != faultTid && !(mainListed && tid == mainTid))
            order.emplace_back(tid, Role::Other);
    if (order.size() > kMaxThreads) {
        snapshot.dropped = order.size() - kMaxThreads;
        order.resize(kMaxThreads);
    }
    Layout layout;
    std::vector<Mapping> mappings;
    bool haveMaps = false;
    if (ops.layout) {
        if (auto found = ops.layout()) {
            layout = std::move(*found);
            haveMaps = true;
        }
    } else {
        const auto text = ops.maps ? ops.maps() : std::nullopt;
        haveMaps = text && !text->empty();
        if (haveMaps) {
            mappings = parseMaps(*text);
            layout = buildLayout(mappings, ops.read);
        }
    }
    const auto walkThread = [&](std::uint64_t tid, const Regs& regs) {
        return ops.walk ? ops.walk(tid, regs, layout)
                        : walkFrames(ops.read, layout, regs, arch, expired);
    };
    // The table is read after the first thread of `order`, the fault thread.
    diagctx::TableImage table;
    const auto readTable = [&] {
        if (tableAddress == 0) return;
        if (!ops.read || !ops.read(tableAddress, &table, sizeof(table)))
            snapshot.table = TableState::Unreadable;
        else
            snapshot.table = diagctx::imageValid(table) ? TableState::Read : TableState::Invalid;
    };
    // Execution scopes: which entry each stopped thread is inside. A word is tied to a
    // thread when its recorded stack address lies on that thread's stack, at or above the
    // thread's stack pointer; of several (nested scopes) the lowest address is the
    // innermost, and that is the entry reported. The words were copied with the table, before
    // the thread was stopped, so for a thread other than the faulting one each candidate is
    // read again once the thread is stopped and accepted only if both reads agree: a
    // stopped thread cannot be in the middle of changing it. The faulting thread is parked in
    // its handler and needs no second read.
    constexpr std::size_t kWords = diagctx::kMaxInstances * 2;
    std::vector<std::uint8_t> placed(kWords, 0);
    const auto stackOf =
        [&](std::uint64_t sp) -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
        if (ops.stackRegion) return ops.stackRegion(sp);
        for (const auto& mapping : mappings)
            if (mapping.read && mapping.write && !mapping.execute && sp >= mapping.start &&
                sp < mapping.end)
                return std::make_pair(mapping.start, mapping.end);
        return std::nullopt;
    };
    const auto attribute = [&](ThreadTrace& trace, std::uint64_t tid, const Regs& regs,
                               bool reread) {
        if (snapshot.table != TableState::Read || regs.sp == 0) return;
        std::uint64_t low = 0, high = 0;
        const auto slot = diagctx::findThread(table, tid);
        if (slot.stackLow != 0 && slot.stackHigh > slot.stackLow && regs.sp >= slot.stackLow &&
            regs.sp < slot.stackHigh) {
            low = slot.stackLow;
            high = slot.stackHigh;
        } else if (const auto region = stackOf(regs.sp)) {
            low = region->first;
            high = region->second;
        } else {
            return;
        }
        bool found = false;
        std::uint64_t best = 0;
        for (std::uint32_t cell = 0; cell < diagctx::kMaxInstances; ++cell) {
            for (const bool audio : {true, false}) {
                const std::uint64_t word =
                    audio ? table.instances[cell].audio : table.instances[cell].main;
                const auto fields = diagctx::unpackWord(word);
                if (fields.kind == diagctx::Entry::None || fields.stack == 0) continue;
                // The address is kept to 16 bytes, rounding down.
                if (fields.stack + 16 <= regs.sp || fields.stack < low || fields.stack >= high)
                    continue;
                if (reread) {
                    std::uint64_t now = 0;
                    const std::uint64_t at = tableAddress +
                                             offsetof(diagctx::TableImage, instances) +
                                             cell * sizeof(diagctx::TableImage::InstanceCell) +
                                             (audio ? 0 : sizeof(std::uint64_t));
                    if (!ops.read || !ops.read(at, &now, sizeof(now)) || now != word) continue;
                }
                placed[cell * 2 + (audio ? 0 : 1)] = 1;
                if (!found || fields.stack < best) {
                    found = true;
                    best = fields.stack;
                    trace.entry = fields.kind;
                    trace.entryCell = cell;
                    trace.entryGeneration = fields.generation;
                }
            }
        }
    };
    std::vector<std::uint64_t> tidOf;
    std::optional<Regs> firstRegs;  // the fault thread's registers, for its attribution
    for (const auto& [tid, role] : order) {
        ThreadTrace trace;
        trace.role = role;
        if (!haveMaps) {
            trace.missing = Missing::MapsUnreadable;
        } else if (role == Role::Fault && faultRegs.pc != 0) {
            trace.walk = walkThread(tid, faultRegs);
            trace.missing = Missing::None;
            firstRegs = faultRegs;
        } else if (role == Role::Fault && tid == 0) {
            // terminate and abort capture no registers; without a thread id either
            // there is nothing to read the thread from.
            trace.missing = Missing::RegistersUnreadable;
        } else if (expired()) {
            trace.missing = Missing::Deadline;
        } else {
            Regs regs;
            const Missing why = ops.stop ? ops.stop(tid, regs) : Missing::Unsupported;
            if (why == Missing::None) {
                trace.walk = walkThread(tid, regs);
                trace.missing = Missing::None;
                if (role == Role::Fault)
                    firstRegs = regs;
                else
                    attribute(trace, tid, regs, true);  // still stopped: the second read
                if (ops.release) ops.release(tid);
            } else {
                trace.missing = why;
            }
        }
        snapshot.threads.push_back(std::move(trace));
        tidOf.push_back(tid);
        if (snapshot.threads.size() == 1) {
            readTable();
            if (firstRegs) attribute(snapshot.threads[0], tid, *firstRegs, false);
        }
    }
    if (snapshot.table == TableState::Read) {
        snapshot.tableThreadsDropped = table.header.threadsDropped;
        snapshot.tableUntracked = table.header.untracked;
        snapshot.tableScopeOverflow = table.header.scopeOverflow;
        {
            const auto ring = diagctx::readCommands(table);
            snapshot.commands.assign(ring.crumbs.begin(), ring.crumbs.begin() + ring.count);
            snapshot.commandsDamaged = ring.damaged;
            snapshot.commandsRefused = table.header.commandsDropped;
        }
        for (std::size_t w = 0; w < kWords; ++w) {
            const auto& cell = table.instances[w / 2];
            const auto word = diagctx::unpackWord(w % 2 == 0 ? cell.audio : cell.main);
            if (word.kind != diagctx::Entry::None && !placed[w]) ++snapshot.unplacedEntries;
        }
        for (std::size_t i = 0; i < snapshot.threads.size(); ++i) {
            const auto found = diagctx::findThread(table, tidOf[i]);
            snapshot.threads[i].registered = found.role;
            snapshot.threads[i].worker = found.ordinal;
        }
    }
    snapshot.modules = std::move(layout.modules);
    snapshot.regions = std::move(layout.regions);
    snapshot.truncatedModules = layout.truncatedModules;
    return snapshot;
}

json::Value modulesJson(const TraceSnapshot& trace) {
    auto modules = json::Value::array();
    for (const auto& module : trace.modules) {
        auto entry = json::Value::object();
        entry["format"] = formatName();
        entry["id"] = module.id ? json::Value(*module.id) : json::Value(nullptr);
        modules.push_back(std::move(entry));
    }
    return modules;
}

json::Value threadsJson(const TraceSnapshot& trace) {
    auto threads = json::Value::array();
    long long ordinal = 0;
    for (const auto& thread : trace.threads) {
        auto entry = json::Value::object();
        entry["ordinal"] = ordinal++;
        entry["role"] = roleString(thread);
        if (const char* registered = diagctx::threadRoleName(thread.registered)) {
            if (thread.role == Role::Fault) entry["registered"] = registered;
            if (thread.worker != 0) entry["worker"] = static_cast<long long>(thread.worker);
        }
        if (const char* name = diagctx::entryName(thread.entry)) entry["entry"] = name;
        const bool walked = thread.missing == Missing::None;
        entry["state"] = walked ? "walked" : "unavailable";
        if (walked)
            entry["stop"] = stopName(thread.walk.stop);
        else
            entry["reason"] = missingName(thread.missing);
        auto frames = json::Value::array();
        if (walked) {
            for (const auto& frame : thread.walk.frames) {
                auto item = json::Value::object();
                if (frame.module >= 0) {
                    item["module"] = frame.module;
                    char text[24];
                    std::snprintf(text, sizeof(text), "0x%llx",
                                  static_cast<unsigned long long>(frame.offset));
                    item["offset"] = std::string(text);
                } else {
                    item["module"] = nullptr;
                }
                item["return"] = frame.returnAddress;
                frames.push_back(std::move(item));
            }
        }
        entry["frames"] = std::move(frames);
        threads.push_back(std::move(entry));
    }
    return threads;
}

std::vector<std::string> traceLimits(const TraceSnapshot& trace) {
    std::vector<std::string> limits;
    bool walked = false, missing = false;
    for (const auto& thread : trace.threads) {
        (thread.missing == Missing::None ? walked : missing) = true;
    }
    if (walked)
        limits.push_back(
            "frames are best effort: optimized or foreign code can omit frame pointers, so "
            "a thread can lack frames or stop early");
    if (missing) limits.push_back("some threads could not be read; each lists its reason");
    if (trace.dropped) limits.push_back("threads beyond the report limit were omitted");
    if (trace.truncatedModules)
        limits.push_back("modules beyond the report limit have no identity");
    if (trace.table == TableState::Unreadable || trace.table == TableState::Invalid)
        limits.push_back("the execution table could not be read, so threads have no roles");
    if (trace.tableUntracked != 0)
        limits.push_back("instances beyond the execution table's capacity are not attributed");
    if (trace.tableScopeOverflow != 0)
        limits.push_back("entries whose stack address the table cannot hold are not attributed");
    if (trace.unplacedEntries != 0)
        limits.push_back(
            "some plug-in entries were open that no thread read could be tied to: their thread "
            "was not read, or the entry names no stack address");
    if (trace.tableThreadsDropped != 0)
        limits.push_back(
            "threads beyond the execution table's capacity have no role and may show as other");
    return limits;
}

namespace {
bool onlyKeys(const json::Value& value, std::initializer_list<std::string_view> keys) {
    if (!value.isObject()) return false;
    for (const auto& member : value.asObject())
        if (std::find(keys.begin(), keys.end(), member.key) == keys.end()) return false;
    return true;
}
bool hexString(const json::Value* v, std::size_t min, std::size_t max, bool prefix) {
    if (!v || !v->isString()) return false;
    std::string_view text = v->asString();
    if (prefix) {
        if (text.substr(0, 2) != "0x") return false;
        text.remove_prefix(2);
    }
    return text.size() >= min && text.size() <= max &&
           std::all_of(text.begin(), text.end(), hexDigit);
}
bool oneOf(const json::Value* v, std::initializer_list<std::string_view> choices) {
    return v && v->isString() &&
           std::find(choices.begin(), choices.end(), v->asString()) != choices.end();
}
}  // namespace

bool traceShape(const json::Value* modules, const json::Value& threads) {
    std::size_t moduleCount = 0;
    if (modules) {
        if (!modules->isArray() || modules->size() > kMaxModules) return false;
        moduleCount = modules->size();
        for (const auto& module : modules->asArray()) {
            if (!onlyKeys(module, {"format", "id"}) || module.size() != 2 ||
                !oneOf(module.find("format"), {"elf", "macho", "pe"}))
                return false;
            const auto* id = module.find("id");
            if (!id) return false;
            if (id->isNull()) continue;
            if (!hexString(id, 2, 128, false) || id->asString().size() % 2 != 0) return false;
        }
    }
    if (!threads.isArray() || threads.size() > kMaxThreads) return false;
    std::size_t index = 0;
    for (const auto& thread : threads.asArray()) {
        if (!onlyKeys(thread, {"ordinal", "role", "registered", "worker", "entry", "state",
                               "reason", "stop", "frames"}))
            return false;
        if (const auto* entry = thread.find("entry")) {
            // The closed set of entry kinds a scope records; never "none".
            if (!entry->isString()) return false;
            bool known = false;
            for (std::uint8_t kind = 1; kind < diagctx::kEntryCount; ++kind)
                known = known ||
                        entry->asString() == diagctx::entryName(static_cast<diagctx::Entry>(kind));
            if (!known) return false;
        }
        const auto* ordinal = thread.find("ordinal");
        if (!ordinal || !ordinal->isInt() || ordinal->asInt() != static_cast<std::int64_t>(index++))
            return false;
        // "main" is what reports written before the thread roles called the model thread.
        if (!oneOf(thread.find("role"),
                   {"fault", "main", "model", "audio_render", "audio_live", "audio_background",
                    "audio_offline", "recorder", "midi_output", "other"}))
            return false;
        {  // the fault thread's own role, and the worker number of a numbered thread
            const bool fault = thread.find("role")->asString() == "fault";
            const auto* registered = thread.find("registered");
            if (registered && (!fault || !oneOf(registered, {"model", "audio_render", "audio_live",
                                                             "audio_background", "audio_offline",
                                                             "recorder", "midi_output"})))
                return false;
            const auto* worker = thread.find("worker");
            if (worker) {
                const std::string_view effective =
                    fault ? (registered ? registered->asString() : std::string_view())
                          : thread.find("role")->asString();
                if (!worker->isInt() || worker->asInt() < 1 || worker->asInt() > 65535 ||
                    effective.empty() || effective == "other" || effective == "main")
                    return false;
            }
        }
        const auto* state = thread.find("state");
        const auto* frames = thread.find("frames");
        if (!state || !state->isString() || !frames || !frames->isArray() ||
            frames->size() > kMaxFrames)
            return false;
        const bool walked = state->asString() == "walked";
        if (!walked && state->asString() != "unavailable") return false;
        if (walked) {
            if (thread.find("reason") ||
                !oneOf(thread.find("stop"), {"end", "depth", "unreadable", "not_executable",
                                             "bad_frame_pointer", "cycle", "deadline"}))
                return false;
        } else {
            if (thread.find("stop") || frames->size() != 0 ||
                !oneOf(thread.find("reason"),
                       {"ptrace_denied", "access_denied", "thread_exited", "deadline",
                        "registers_unreadable", "maps_unreadable", "unsupported", "not_collected"}))
                return false;
        }
        for (const auto& frame : frames->asArray()) {
            if (!onlyKeys(frame, {"module", "offset", "return"})) return false;
            const auto* module = frame.find("module");
            const auto* ret = frame.find("return");
            if (!module || !ret || !ret->isBool()) return false;
            if (module->isNull()) {
                if (frame.find("offset")) return false;
            } else if (!module->isInt() || module->asInt() < 0 ||
                       static_cast<std::uint64_t>(module->asInt()) >= moduleCount ||
                       !hexString(frame.find("offset"), 1, 16, true)) {
                return false;
            }
        }
    }
    return true;
}

std::optional<TraceSnapshot> parseCapsule(const std::uint8_t* bytes, std::size_t size) {
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t n) {
        if (n > size - at) return false;  // at <= size always
        if (n > 0) std::memcpy(out, bytes + at, n);
        at += n;
        return true;
    };
    CapsuleHeader header;
    if (!bytes || !take(&header, sizeof(header)) || header.magic != kCapsuleMagic ||
        header.moduleCount > kCapsuleModules || header.threadCount > kCapsuleThreads ||
        (header.flags & ~(kCapsuleModulesTruncated | kCapsuleThreadsDropped)) != 0 ||
        header.reserved != 0)
        return std::nullopt;
    // The worker always records the fault entry first, so a capsule with no thread is not
    // one it wrote. Threads beyond the cap are counted together with their flag; the count
    // cannot exceed what a process could have (a damaged capsule must not report billions).
    constexpr std::uint32_t kMaxDropped = 1U << 20;
    if (header.threadCount == 0 ||
        (header.dropped != 0) != ((header.flags & kCapsuleThreadsDropped) != 0) ||
        header.dropped > kMaxDropped)
        return std::nullopt;
    // At most one word per cell side can be unplaced.
    if (header.unplaced > 2 * diagctx::kMaxInstances) return std::nullopt;
    if (header.commandCount > diagctx::kMaxCommandEntries ||
        header.commandsDamaged > diagctx::kMaxCommandEntries || header.reserved2 != 0)
        return std::nullopt;
    TraceSnapshot snapshot;
    // A capsule is written by the worker from its own execution table, which a process always
    // has: the counters and the command ring below are what it read, so the table counts as
    // read (the report's limits say what the commands are, not that the table was missing).
    snapshot.table = TableState::Read;
    snapshot.dropped = header.dropped;
    snapshot.truncatedModules = (header.flags & kCapsuleModulesTruncated) != 0;
    snapshot.tableThreadsDropped = header.tableThreadsDropped;
    snapshot.tableUntracked = header.tableUntracked;
    snapshot.tableScopeOverflow = header.tableScopeOverflow;
    snapshot.unplacedEntries = header.unplaced;
    snapshot.commandsDamaged = header.commandsDamaged;
    snapshot.commandsRefused = header.commandsRefused;
    Layout layout;
    for (std::uint32_t i = 0; i < header.moduleCount; ++i) {
        CapsuleModule module;
        if (!take(&module, sizeof(module)) || module.start >= module.end || module.hasUuid > 1 ||
            module.reserved != 0)
            return std::nullopt;
        Module entry;
        entry.bias = module.bias;
        if (module.hasUuid) {
            std::string id;
            for (auto byte : module.uuid) {
                id += "0123456789abcdef"[byte >> 4];
                id += "0123456789abcdef"[byte & 15];
            }
            entry.id = std::move(id);
        }
        layout.modules.push_back(std::move(entry));
        layout.regions.push_back(Region{module.start, module.end, static_cast<int>(i)});
    }
    std::sort(layout.regions.begin(), layout.regions.end(),
              [](const Region& a, const Region& b) { return a.start < b.start; });
    {  // overlapping ranges would make lookups ambiguous: later ones are dropped
        std::vector<Region> clean;
        for (const auto& r : layout.regions)
            if (clean.empty() || r.start >= clean.back().end) clean.push_back(r);
        layout.regions = std::move(clean);
    }
    std::vector<ThreadTrace> threads;
    for (std::uint32_t i = 0; i < header.threadCount; ++i) {
        CapsuleThread head;
        if (!take(&head, sizeof(head)) || head.role > static_cast<std::uint8_t>(Role::Other) ||
            head.missing > static_cast<std::uint8_t>(Missing::AccessDenied) ||
            head.stop > static_cast<std::uint8_t>(Stop::Deadline) || head.reserved != 0 ||
            head.registered >= diagctx::kThreadRoleCount ||
            (head.worker != 0 && head.registered == 0) || head.frameCount > kCapsuleFrames ||
            head.entry >= diagctx::kEntryCount)
            return std::nullopt;
        const bool walked = head.missing == static_cast<std::uint8_t>(Missing::None);
        if (walked != (head.frameCount > 0)) return std::nullopt;
        // An entry is only ever found for a thread that was read, with a cell the table has
        // and a generation a word holds; without one the cell and generation are zero.
        if (head.entry == 0 ? (head.entryCell != 0 || head.entryGeneration != 0)
                            : (!walked || head.entryCell >= diagctx::kMaxInstances ||
                               head.entryGeneration > diagctx::kGenerationMask))
            return std::nullopt;
        ThreadTrace trace;
        trace.role = static_cast<Role>(head.role);
        trace.registered = static_cast<diagctx::ThreadRole>(head.registered);
        trace.worker = head.worker;
        if (head.entry != 0) {
            trace.entry = static_cast<diagctx::Entry>(head.entry);
            trace.entryCell = head.entryCell;
            trace.entryGeneration = head.entryGeneration;
        }
        // The fault entry is the first thread and the only one.
        if ((i == 0) != (trace.role == Role::Fault)) return std::nullopt;
        trace.missing = static_cast<Missing>(head.missing);
        trace.walk.stop = static_cast<Stop>(head.stop);
        for (std::uint32_t f = 0; f < head.frameCount; ++f) {
            std::uint64_t address = 0;
            if (!take(&address, sizeof(address))) return std::nullopt;
            if (f == 0) {
                // The pc itself: outside every module it is recorded without identity.
                const auto frame = frameAt(layout, address, false);
                trace.walk.frames.push_back(frame ? *frame : Frame{});
            } else {
                // Later frames are return addresses. One outside every module is kept
                // without identity and the walk goes on: the system libraries of the dyld
                // shared cache are in no module, and the chain was validated in process.
                const auto frame = frameAt(layout, address, true);
                Frame item = frame ? *frame : Frame{};
                item.returnAddress = true;
                trace.walk.frames.push_back(item);
            }
        }
        threads.push_back(std::move(trace));
    }
    // The ring's entries, oldest first: each a whole entry of the 64 the ring holds, so the
    // sequences rise and span fewer than 64.
    for (std::uint32_t i = 0; i < header.commandCount; ++i) {
        CapsuleCommand command;
        if (!take(&command, sizeof(command)) || command.seq == 0 || command.reserved != 0 ||
            command.origin == 0 || command.origin >= diagctx::kCommandOriginCount ||
            command.state == 0 || command.state >= diagctx::kCommandStateCount)
            return std::nullopt;
        if (!snapshot.commands.empty() &&
            (command.seq <= snapshot.commands.back().seq ||
             command.seq - snapshot.commands.front().seq >= diagctx::kMaxCommandEntries))
            return std::nullopt;
        snapshot.commands.push_back(diagctx::CommandCrumb{
            command.seq, command.nameId, static_cast<diagctx::CommandOrigin>(command.origin),
            static_cast<diagctx::CommandState>(command.state)});
    }
    if (at != size) return std::nullopt;  // trailing bytes: not a capsule we wrote
    // Fault first, then main, then the rest in the order they came.
    for (const Role role : {Role::Fault, Role::Main, Role::Other})
        for (auto& thread : threads)
            if (thread.role == role) snapshot.threads.push_back(std::move(thread));
    snapshot.modules = std::move(layout.modules);
    snapshot.regions = std::move(layout.regions);
    return snapshot;
}
}  // namespace supple::diagnostics::detail
