// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "trace.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <utility>
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
const char* roleName(Role role) {
    return role == Role::Fault ? "fault" : role == Role::Main ? "main" : "other";
}
const char* missingName(Missing why) {
    switch (why) {
        case Missing::PtraceDenied:
            return "ptrace_denied";
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

TraceSnapshot collectTrace(const TraceOps& ops, std::uint64_t faultTid, const Regs& faultRegs,
                           std::uint64_t mainTid, Arch arch) {
    TraceSnapshot snapshot;
    const auto expired = [&] { return ops.expired && ops.expired(); };
    std::vector<std::uint64_t> tids = ops.threads ? ops.threads() : std::vector<std::uint64_t>{};
    std::sort(tids.begin(), tids.end());
    tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
    // Order: fault thread (always, from the capsule), main, then by thread id.
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
    const auto text = ops.maps ? ops.maps() : std::nullopt;
    Layout layout;
    const bool haveMaps = text && !text->empty();
    if (haveMaps) layout = buildLayout(parseMaps(*text), ops.read);
    for (const auto& [tid, role] : order) {
        ThreadTrace trace;
        trace.role = role;
        if (!haveMaps) {
            trace.missing = Missing::MapsUnreadable;
        } else if (role == Role::Fault) {
            trace.walk = walkFrames(ops.read, layout, faultRegs, arch, expired);
            trace.missing = Missing::None;
        } else if (expired()) {
            trace.missing = Missing::Deadline;
        } else {
            Regs regs;
            const Missing why = ops.stop ? ops.stop(tid, regs) : Missing::Unsupported;
            if (why == Missing::None) {
                trace.walk = walkFrames(ops.read, layout, regs, arch, expired);
                trace.missing = Missing::None;
                if (ops.release) ops.release(tid);
            } else {
                trace.missing = why;
            }
        }
        snapshot.threads.push_back(std::move(trace));
    }
    snapshot.modules = std::move(layout.modules);
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
        entry["role"] = roleName(thread.role);
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
        if (!onlyKeys(thread, {"ordinal", "role", "state", "reason", "stop", "frames"}))
            return false;
        const auto* ordinal = thread.find("ordinal");
        if (!ordinal || !ordinal->isInt() || ordinal->asInt() != static_cast<std::int64_t>(index++))
            return false;
        if (!oneOf(thread.find("role"), {"fault", "main", "other"})) return false;
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
                       {"ptrace_denied", "thread_exited", "deadline", "registers_unreadable",
                        "maps_unreadable", "unsupported", "not_collected"}))
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
}  // namespace supple::diagnostics::detail
