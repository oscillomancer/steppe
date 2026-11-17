// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/crash-resolver/image.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <system_error>
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define SUPPLE_HAVE_CXXABI 1
#endif

namespace supple::diagnostics::resolver {
namespace {

template <typename T>
T le(const unsigned char* p) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value = static_cast<T>(value | (T{p[i]} << (8 * i)));
    return value;
}
template <typename T>
T be(const unsigned char* p) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value = static_cast<T>((value << 8) | T{p[i]});
    return value;
}
std::string hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}
Error fail(const char* code, const char* message, const char* hint = "") {
    return Error(code, message, {}, hint);
}
Error malformed(const char* what) {
    return Error("symbols.malformed", std::string("the file is damaged: ") + what, {},
                 "Use the file the crashing build produced; this one was changed or cut short.");
}

// A byte range of another source: one slice of a universal Mach-O file.
class Window final : public Source {
  public:
    Window(const Source& base, std::uint64_t offset, std::uint64_t length)
        : base_(base), offset_(offset), length_(length) {}
    std::uint64_t size() const override { return length_; }
    bool read(std::uint64_t offset, void* out, std::size_t bytes) const override {
        if (offset > length_ || bytes > length_ - offset) return false;
        return base_.read(offset_ + offset, out, bytes);
    }

  private:
    const Source& base_;
    std::uint64_t offset_, length_;
};

bool within(const Source& source, std::uint64_t offset, std::uint64_t bytes) {
    return offset <= source.size() && bytes <= source.size() - offset;
}
// `count` records of `entry` bytes at `offset`, checked before anything is allocated.
bool readRecords(const Source& source, std::uint64_t offset, std::uint64_t count,
                 std::uint64_t entry, std::vector<unsigned char>& out) {
    if (entry == 0 || count > source.size() / entry) return false;
    const std::uint64_t bytes = count * entry;
    if (!within(source, offset, bytes)) return false;
    out.resize(static_cast<std::size_t>(bytes));
    return bytes == 0 || source.read(offset, out.data(), out.size());
}
// The NUL-terminated name at `start` inside `pool`, if it ends within `limit` bytes.
std::optional<std::string_view> nameAt(const char* pool, std::uint64_t poolSize,
                                       std::uint64_t start, std::size_t limit) {
    if (start >= poolSize) return std::nullopt;
    const std::uint64_t room = std::min<std::uint64_t>(poolSize - start, limit + 1);
    const char* begin = pool + start;
    const void* end = std::memchr(begin, 0, static_cast<std::size_t>(room));
    if (!end) return std::nullopt;
    return std::string_view(begin, static_cast<std::size_t>(static_cast<const char*>(end) - begin));
}

// Collects function symbols, then sorts and gives each an extent.
class Builder {
  public:
    Builder(const Limits& limits, Format format, std::string id) : limits_(limits) {
        table_.format = format;
        table_.id = std::move(id);
    }
    // `size` zero means unknown; `limit` is the end of the section holding the function.
    // False once the table is full and nothing more can be added.
    bool add(std::uint64_t address, std::uint64_t size, std::uint64_t limit,
             std::string_view name) {
        if (name.empty() || name.size() > limits_.maxNameBytes || address >= limit) {
            ++table_.skipped;
            return true;
        }
        if (pending_.size() >= limits_.maxSymbols ||
            table_.names.size() + name.size() > limits_.maxStringBytes) {
            table_.truncated = true;
            return false;
        }
        Pending p;
        p.symbol.address = address;
        p.symbol.name = static_cast<std::uint32_t>(table_.names.size());
        p.symbol.length = static_cast<std::uint32_t>(name.size());
        p.limit = limit;
        p.sized = size != 0;
        p.symbol.end = p.sized ? (size > limit - address ? limit : address + size) : 0;
        table_.names.append(name);
        pending_.push_back(p);
        return true;
    }
    void skip() { ++table_.skipped; }
    SymbolTable finish() {
        const std::string& names = table_.names;
        std::stable_sort(pending_.begin(), pending_.end(), [&](const Pending& a, const Pending& b) {
            if (a.symbol.address != b.symbol.address) return a.symbol.address < b.symbol.address;
            if (a.sized != b.sized) return a.sized;
            if (a.symbol.end != b.symbol.end) return a.symbol.end > b.symbol.end;
            return std::string_view(names).substr(a.symbol.name, a.symbol.length) <
                   std::string_view(names).substr(b.symbol.name, b.symbol.length);
        });
        std::vector<Pending> unique;
        unique.reserve(pending_.size());
        for (const auto& p : pending_)
            if (unique.empty() || unique.back().symbol.address != p.symbol.address)
                unique.push_back(p);
        table_.symbols.reserve(unique.size());
        for (std::size_t i = 0; i < unique.size(); ++i) {
            Symbol s = unique[i].symbol;
            if (!unique[i].sized) {
                s.end = unique[i].limit;
                if (i + 1 < unique.size() && unique[i + 1].symbol.address < s.end)
                    s.end = unique[i + 1].symbol.address;
            }
            table_.symbols.push_back(s);
        }
        return std::move(table_);
    }

  private:
    struct Pending {
        Symbol symbol;
        std::uint64_t limit = 0;
        bool sized = false;
    };
    const Limits& limits_;
    SymbolTable table_;
    std::vector<Pending> pending_;
};

// --- ELF -------------------------------------------------------------------------------

struct ElfHeader {
    std::uint16_t type = 0;
    std::uint64_t phoff = 0, shoff = 0;
    std::uint16_t phentsize = 0, phnum = 0, shentsize = 0, shnum = 0;
};
struct ElfSection {
    std::uint32_t type = 0, link = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0;
};
constexpr std::size_t kMaxProgramHeaders = 4096;
constexpr std::size_t kMaxNoteBytes = 64 * 1024;
constexpr std::size_t kMaxBuildIdBytes = 64;

std::optional<ElfHeader> elfHeader(const Source& source) {
    std::array<unsigned char, 64> h{};
    if (!source.read(0, h.data(), h.size())) return std::nullopt;
    if (std::memcmp(h.data(), "\177ELF", 4) != 0 || h[4] != 2 || h[5] != 1) return std::nullopt;
    ElfHeader out;
    out.type = le<std::uint16_t>(&h[16]);
    out.phoff = le<std::uint64_t>(&h[32]);
    out.shoff = le<std::uint64_t>(&h[40]);
    out.phentsize = le<std::uint16_t>(&h[54]);
    out.phnum = le<std::uint16_t>(&h[56]);
    out.shentsize = le<std::uint16_t>(&h[58]);
    out.shnum = le<std::uint16_t>(&h[60]);
    if (out.type != 2 && out.type != 3) return std::nullopt;  // as the collector reads it
    return out;
}
std::optional<std::string> noteBuildId(const std::vector<unsigned char>& bytes) {
    std::uint64_t at = 0;
    const std::uint64_t size = bytes.size();
    while (at + 12 <= size) {
        const auto namesz = le<std::uint32_t>(&bytes[at]);
        const auto descsz = le<std::uint32_t>(&bytes[at + 4]);
        const auto kind = le<std::uint32_t>(&bytes[at + 8]);
        const std::uint64_t name = at + 12;
        const std::uint64_t desc = name + ((std::uint64_t{namesz} + 3) & ~std::uint64_t{3});
        const std::uint64_t next = desc + ((std::uint64_t{descsz} + 3) & ~std::uint64_t{3});
        if (next > size) break;
        if (kind == 3 && namesz == 4 && std::memcmp(&bytes[name], "GNU\0", 4) == 0 && descsz >= 1 &&
            descsz <= kMaxBuildIdBytes)
            return hex(&bytes[desc], descsz);
        at = next;
    }
    return std::nullopt;
}
bool elfSections(const Source& source, const ElfHeader& h, const Limits& limits,
                 std::vector<ElfSection>& out) {
    if (h.shoff == 0 || h.shnum == 0 || h.shentsize != 64 || h.shnum > limits.maxSections)
        return false;
    std::vector<unsigned char> raw;
    if (!readRecords(source, h.shoff, h.shnum, 64, raw)) return false;
    out.resize(h.shnum);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const unsigned char* s = &raw[i * 64];
        out[i].type = le<std::uint32_t>(s + 4);
        out[i].flags = le<std::uint64_t>(s + 8);
        out[i].addr = le<std::uint64_t>(s + 16);
        out[i].offset = le<std::uint64_t>(s + 24);
        out[i].size = le<std::uint64_t>(s + 32);
        out[i].link = le<std::uint32_t>(s + 40);
    }
    return true;
}
std::string elfId(const Source& source, const ElfHeader& h, const Limits& limits) {
    if (h.phnum > 0 && h.phentsize == 56 && h.phnum <= kMaxProgramHeaders) {
        std::vector<unsigned char> headers;
        if (readRecords(source, h.phoff, h.phnum, 56, headers)) {
            std::size_t notes = 0;
            for (std::size_t i = 0; i < h.phnum && notes < 8; ++i) {
                const unsigned char* ph = &headers[i * 56];
                const auto offset = le<std::uint64_t>(ph + 8);
                const auto filesz = le<std::uint64_t>(ph + 32);
                if (le<std::uint32_t>(ph) != 4 || filesz == 0 || filesz > kMaxNoteBytes ||
                    !within(source, offset, filesz))
                    continue;
                ++notes;
                std::vector<unsigned char> bytes(static_cast<std::size_t>(filesz));
                if (!source.read(offset, bytes.data(), bytes.size())) continue;
                if (auto id = noteBuildId(bytes)) return *id;
            }
        }
    }
    // A section-only file (an object kept for its symbols) has no program headers.
    std::vector<ElfSection> sections;
    if (!elfSections(source, h, limits, sections)) return {};
    std::size_t notes = 0;
    for (const auto& s : sections) {
        if (s.type != 7 || s.size == 0 || s.size > kMaxNoteBytes || notes >= 16 ||
            !within(source, s.offset, s.size))
            continue;
        ++notes;
        std::vector<unsigned char> bytes(static_cast<std::size_t>(s.size));
        if (!source.read(s.offset, bytes.data(), bytes.size())) continue;
        if (auto id = noteBuildId(bytes)) return *id;
    }
    return {};
}
Result<SymbolTable> readElf(const Source& source, const Identity& image, const Limits& limits) {
    const auto header = elfHeader(source);
    if (!header) return fail("symbols.unsupported", "not a 64-bit little-endian ELF image");
    std::vector<ElfSection> sections;
    if (!elfSections(source, *header, limits, sections))
        return fail("symbols.no_symbols", "the image has no readable section table",
                    "Use the build's unstripped file or its separate debug file.");
    const ElfSection* symtab = nullptr;
    for (int want : {2, 11}) {  // .symtab, else .dynsym
        for (const auto& s : sections)
            if (static_cast<int>(s.type) == want) {
                symtab = &s;
                break;
            }
        if (symtab) break;
    }
    if (!symtab)
        return fail("symbols.no_symbols", "the image has no symbol table",
                    "Use the build's unstripped file or its separate debug file.");
    if (symtab->size % 24 != 0 || symtab->size / 24 > limits.maxEntries)
        return symtab->size / 24 > limits.maxEntries
                   ? Result<SymbolTable>(fail("symbols.too_large", "the symbol table is too large"))
                   : Result<SymbolTable>(malformed("symbol table size"));
    if (symtab->link >= sections.size() || sections[symtab->link].type != 3)
        return malformed("symbol table without a string table");
    const ElfSection& strtab = sections[symtab->link];
    if (strtab.size > limits.maxStringBytes)
        return fail("symbols.too_large", "the string table is too large");
    if (!within(source, symtab->offset, symtab->size) ||
        !within(source, strtab.offset, strtab.size))
        return malformed("symbol table outside the file");
    std::vector<char> strings(static_cast<std::size_t>(strtab.size));
    if (!strings.empty() && !source.read(strtab.offset, strings.data(), strings.size()))
        return fail("symbols.read_failed", "reading the string table failed");

    Builder builder(limits, Format::Elf, image.id);
    const std::uint64_t count = symtab->size / 24;
    constexpr std::uint64_t kChunk = 4096;
    std::vector<unsigned char> chunk;
    for (std::uint64_t first = 0; first < count; first += kChunk) {
        const std::uint64_t n = std::min(kChunk, count - first);
        if (!readRecords(source, symtab->offset + first * 24, n, 24, chunk))
            return fail("symbols.read_failed", "reading the symbol table failed");
        for (std::uint64_t i = 0; i < n; ++i) {
            const unsigned char* e = &chunk[i * 24];
            const auto info = e[4];
            const auto shndx = le<std::uint16_t>(e + 6);
            const auto value = le<std::uint64_t>(e + 8);
            const auto size = le<std::uint64_t>(e + 16);
            const unsigned type = info & 15u;
            if (type != 2 && type != 10) continue;  // functions and indirect functions
            if (shndx == 0 || shndx >= sections.size() || (sections[shndx].flags & 4) == 0 ||
                value < sections[shndx].addr ||
                value - sections[shndx].addr >= sections[shndx].size) {
                builder.skip();
                continue;
            }
            const auto name =
                nameAt(strings.data(), strings.size(), le<std::uint32_t>(e), limits.maxNameBytes);
            if (!name) {
                builder.skip();
                continue;
            }
            if (!builder.add(value, size, sections[shndx].addr + sections[shndx].size, *name))
                return builder.finish();
        }
    }
    return builder.finish();
}

// --- Mach-O ----------------------------------------------------------------------------

struct MachSection {
    std::uint64_t addr = 0, size = 0;
    bool text = false;
};
struct MachInfo {
    bool hasUuid = false;
    std::array<unsigned char, 16> uuid{};
    std::vector<MachSection> sections;
    bool hasSymtab = false;
    std::uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
};
constexpr std::size_t kMaxMachSections = 1024;

// Nullopt: not a thin 64-bit Mach-O. Error: a Mach-O whose load commands are damaged.
std::optional<Result<MachInfo>> machInfo(const Source& source, const Limits& limits) {
    std::array<unsigned char, 32> h{};
    if (!source.read(0, h.data(), h.size()) || le<std::uint32_t>(h.data()) != 0xfeedfacfu)
        return std::nullopt;
    const auto ncmds = le<std::uint32_t>(&h[16]);
    const auto sizeofcmds = le<std::uint32_t>(&h[20]);
    if (ncmds > limits.maxLoadCommands || sizeofcmds > limits.maxCommandBytes ||
        !within(source, 32, sizeofcmds))
        return Result<MachInfo>(malformed("load commands"));
    std::vector<unsigned char> cmds(sizeofcmds);
    if (sizeofcmds > 0 && !source.read(32, cmds.data(), cmds.size()))
        return Result<MachInfo>(fail("symbols.read_failed", "reading the load commands failed"));
    MachInfo info;
    std::uint64_t at = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (at + 8 > sizeofcmds) return Result<MachInfo>(malformed("load command list"));
        const auto cmd = le<std::uint32_t>(&cmds[at]);
        const auto cmdsize = le<std::uint32_t>(&cmds[at + 4]);
        if (cmdsize < 8 || cmdsize % 8 != 0 || cmdsize > sizeofcmds - at)
            return Result<MachInfo>(malformed("load command size"));
        const unsigned char* c = &cmds[at];
        if (cmd == 0x1b && cmdsize >= 24) {  // LC_UUID
            std::memcpy(info.uuid.data(), c + 8, 16);
            info.hasUuid = true;
        } else if (cmd == 0x19) {  // LC_SEGMENT_64
            if (cmdsize < 72) return Result<MachInfo>(malformed("segment command"));
            const auto nsects = le<std::uint32_t>(c + 64);
            if (nsects > (cmdsize - 72) / 80 || info.sections.size() + nsects > kMaxMachSections)
                return Result<MachInfo>(malformed("section count"));
            const bool textSegment =
                std::strncmp(reinterpret_cast<const char*>(c + 8), "__TEXT", 16) == 0;
            for (std::uint32_t s = 0; s < nsects; ++s) {
                const unsigned char* sec = c + 72 + std::size_t{s} * 80;
                MachSection m;
                m.addr = le<std::uint64_t>(sec + 32);
                m.size = le<std::uint64_t>(sec + 40);
                const auto flags = le<std::uint32_t>(sec + 64);
                m.text = textSegment && (flags & 0x80000400u) != 0;
                info.sections.push_back(m);
            }
        } else if (cmd == 0x2 && cmdsize >= 24) {  // LC_SYMTAB
            info.hasSymtab = true;
            info.symoff = le<std::uint32_t>(c + 8);
            info.nsyms = le<std::uint32_t>(c + 12);
            info.stroff = le<std::uint32_t>(c + 16);
            info.strsize = le<std::uint32_t>(c + 20);
        }
        at += cmdsize;
    }
    return Result<MachInfo>(std::move(info));
}

// Slices of a file: itself when thin, each architecture when universal.
std::vector<std::pair<std::uint64_t, std::uint64_t>> machSlices(const Source& source,
                                                                const Limits& limits) {
    std::array<unsigned char, 8> h{};
    if (!source.read(0, h.data(), h.size())) return {};
    const auto magic = be<std::uint32_t>(h.data());
    if (le<std::uint32_t>(h.data()) == 0xfeedfacfu) return {{0, source.size()}};
    if (magic != 0xcafebabeu && magic != 0xcafebabfu) return {};
    const bool wide = magic == 0xcafebabfu;
    const auto count = be<std::uint32_t>(&h[4]);
    if (count == 0 || count > limits.maxSlices) return {};  // also a Java class file
    const std::size_t entry = wide ? 32 : 20;
    std::vector<unsigned char> raw;
    if (!readRecords(source, 8, count, entry, raw)) return {};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* a = &raw[i * entry];
        const std::uint64_t offset = wide ? be<std::uint64_t>(a + 8) : be<std::uint32_t>(a + 8);
        const std::uint64_t length = wide ? be<std::uint64_t>(a + 16) : be<std::uint32_t>(a + 12);
        if (within(source, offset, length) && length >= 32) out.emplace_back(offset, length);
    }
    return out;
}

Result<SymbolTable> readMachO(const Source& whole, const Identity& image, const Limits& limits) {
    const Window slice(whole, image.offset, image.length);
    const auto parsed = machInfo(slice, limits);
    if (!parsed) return fail("symbols.unsupported", "not a 64-bit Mach-O image");
    if (!parsed->ok()) return parsed->error();
    const MachInfo& info = parsed->value();
    if (!info.hasSymtab || info.nsyms == 0)
        return fail("symbols.no_symbols", "the image has no symbol table",
                    "Use the build's unstripped file or its dSYM's DWARF file.");
    if (info.nsyms > limits.maxEntries || info.strsize > limits.maxStringBytes)
        return fail("symbols.too_large", "the symbol table is too large");
    if (!within(slice, info.symoff, std::uint64_t{info.nsyms} * 16) ||
        !within(slice, info.stroff, info.strsize))
        return malformed("symbol table outside the file");
    std::vector<char> strings(info.strsize);
    if (!strings.empty() && !slice.read(info.stroff, strings.data(), strings.size()))
        return fail("symbols.read_failed", "reading the string table failed");

    Builder builder(limits, Format::MachO, image.id);
    constexpr std::uint64_t kChunk = 4096;
    std::vector<unsigned char> chunk;
    for (std::uint64_t first = 0; first < info.nsyms; first += kChunk) {
        const std::uint64_t n = std::min<std::uint64_t>(kChunk, info.nsyms - first);
        if (!readRecords(slice, info.symoff + first * 16, n, 16, chunk))
            return fail("symbols.read_failed", "reading the symbol table failed");
        for (std::uint64_t i = 0; i < n; ++i) {
            const unsigned char* e = &chunk[i * 16];
            const unsigned type = e[4], sect = e[5];
            const auto value = le<std::uint64_t>(e + 8);
            if ((type & 0xe0u) != 0 || (type & 0x0eu) != 0x0eu) continue;  // debug and non-section
            if (sect == 0 || sect > info.sections.size() || !info.sections[sect - 1].text ||
                value < info.sections[sect - 1].addr ||
                value - info.sections[sect - 1].addr >= info.sections[sect - 1].size) {
                builder.skip();
                continue;
            }
            auto name =
                nameAt(strings.data(), strings.size(), le<std::uint32_t>(e), limits.maxNameBytes);
            if (!name) {
                builder.skip();
                continue;
            }
            if (name->size() > 1 && name->front() == '_') name->remove_prefix(1);  // C prefix
            if (!builder.add(value, 0, info.sections[sect - 1].addr + info.sections[sect - 1].size,
                             *name))
                return builder.finish();
        }
    }
    return builder.finish();
}

// --- PE --------------------------------------------------------------------------------

struct PeSection {
    std::uint32_t va = 0, virtualSize = 0, rawSize = 0, rawPointer = 0, characteristics = 0;
    std::uint64_t extent() const { return virtualSize ? virtualSize : rawSize; }
};
struct PeInfo {
    std::uint64_t imageBase = 0;
    std::uint32_t exportRva = 0, exportSize = 0, debugRva = 0, debugSize = 0;
    std::vector<PeSection> sections;
};
constexpr std::size_t kMaxPeSections = 96;
constexpr std::size_t kMaxDebugEntries = 16;

// Nullopt: not a PE32+ image.
std::optional<PeInfo> peInfo(const Source& source) {
    std::array<unsigned char, 64> dos{};
    if (!source.read(0, dos.data(), dos.size()) || le<std::uint16_t>(dos.data()) != 0x5a4d)
        return std::nullopt;
    const auto pe = le<std::uint32_t>(&dos[0x3c]);
    if (pe < 64 || pe > 0x10000) return std::nullopt;
    std::array<unsigned char, 24> coff{};
    if (!source.read(pe, coff.data(), coff.size()) || std::memcmp(coff.data(), "PE\0\0", 4) != 0)
        return std::nullopt;
    const auto sectionCount = le<std::uint16_t>(&coff[6]);
    const auto optionalSize = le<std::uint16_t>(&coff[20]);
    if (sectionCount > kMaxPeSections || optionalSize < 112 + 7 * 8 || optionalSize > 1024)
        return std::nullopt;
    std::vector<unsigned char> optional(optionalSize);
    if (!source.read(pe + 24, optional.data(), optional.size()) ||
        le<std::uint16_t>(optional.data()) != 0x20b)
        return std::nullopt;
    PeInfo info;
    info.imageBase = le<std::uint64_t>(&optional[24]);
    const auto directories = le<std::uint32_t>(&optional[108]);
    if (directories > 0) {
        info.exportRva = le<std::uint32_t>(&optional[112]);
        info.exportSize = le<std::uint32_t>(&optional[116]);
    }
    if (directories > 6) {
        info.debugRva = le<std::uint32_t>(&optional[112 + 6 * 8]);
        info.debugSize = le<std::uint32_t>(&optional[112 + 6 * 8 + 4]);
    }
    std::vector<unsigned char> table;
    if (!readRecords(source, std::uint64_t{pe} + 24 + optionalSize, sectionCount, 40, table))
        return std::nullopt;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const unsigned char* s = &table[i * 40];
        PeSection section;
        section.virtualSize = le<std::uint32_t>(s + 8);
        section.va = le<std::uint32_t>(s + 12);
        section.rawSize = le<std::uint32_t>(s + 16);
        section.rawPointer = le<std::uint32_t>(s + 20);
        section.characteristics = le<std::uint32_t>(s + 36);
        info.sections.push_back(section);
    }
    return info;
}
struct Located {
    std::uint64_t offset = 0;
    std::uint64_t available = 0;  // bytes from here to the end of the section's file data
};
std::optional<Located> locate(const Source& source, const PeInfo& info, std::uint64_t rva) {
    for (const auto& s : info.sections) {
        if (rva < s.va || rva - s.va >= s.rawSize) continue;
        const std::uint64_t inside = rva - s.va;
        if (!within(source, s.rawPointer, s.rawSize)) return std::nullopt;
        return Located{std::uint64_t{s.rawPointer} + inside, s.rawSize - inside};
    }
    return std::nullopt;
}
std::optional<std::uint64_t> locateRange(const Source& source, const PeInfo& info,
                                         std::uint64_t rva, std::uint64_t bytes) {
    const auto at = locate(source, info, rva);
    if (!at || bytes > at->available) return std::nullopt;
    return at->offset;
}
std::string peId(const Source& source, const PeInfo& info) {
    constexpr std::size_t kEntry = 28;
    if (info.debugRva == 0 || info.debugSize < kEntry) return {};
    const std::size_t count = std::min<std::size_t>(info.debugSize / kEntry, kMaxDebugEntries);
    const auto at = locateRange(source, info, info.debugRva, count * kEntry);
    if (!at) return {};
    std::vector<unsigned char> entries(count * kEntry);
    if (!source.read(*at, entries.data(), entries.size())) return {};
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* entry = &entries[i * kEntry];
        const auto type = le<std::uint32_t>(entry + 12);
        const auto dataSize = le<std::uint32_t>(entry + 16);
        const auto dataPointer = le<std::uint32_t>(entry + 24);
        if (type != 2 || dataSize < 24 || dataPointer == 0) continue;
        std::array<unsigned char, 24> record{};  // "RSDS", GUID, age; the path is not read
        if (!source.read(dataPointer, record.data(), record.size()) ||
            std::memcmp(record.data(), "RSDS", 4) != 0)
            continue;
        // Symbol-server order: Data1, Data2, Data3 as numbers, Data4 as bytes, then the age.
        std::array<unsigned char, 20> id{};
        const auto data1 = le<std::uint32_t>(&record[4]);
        const auto data2 = le<std::uint16_t>(&record[8]);
        const auto data3 = le<std::uint16_t>(&record[10]);
        for (std::size_t b = 0; b < 4; ++b)
            id[b] = static_cast<unsigned char>((data1 >> (24 - 8 * b)) & 0xff);
        id[4] = static_cast<unsigned char>(data2 >> 8);
        id[5] = static_cast<unsigned char>(data2 & 0xff);
        id[6] = static_cast<unsigned char>(data3 >> 8);
        id[7] = static_cast<unsigned char>(data3 & 0xff);
        std::memcpy(&id[8], &record[12], 8);
        const auto age = le<std::uint32_t>(&record[20]);  // stored little-endian, shown big-endian
        for (std::size_t b = 0; b < 4; ++b)
            id[16 + b] = static_cast<unsigned char>((age >> (24 - 8 * b)) & 0xff);
        return hex(id.data(), id.size());
    }
    return {};
}
Result<SymbolTable> readPe(const Source& source, const Identity& image, const Limits& limits) {
    const auto info = peInfo(source);
    if (!info) return fail("symbols.unsupported", "not a PE32+ image");
    if (info->exportRva == 0 || info->exportSize < 40)
        return fail("symbols.no_symbols", "the image exports no functions",
                    "Only exported function names are read from Windows images: the PDB is not.");
    const auto at = locateRange(source, *info, info->exportRva, 40);
    std::array<unsigned char, 40> directory{};
    if (!at || !source.read(*at, directory.data(), directory.size()))
        return malformed("export directory");
    const auto functionCount = le<std::uint32_t>(&directory[20]);
    const auto nameCount = le<std::uint32_t>(&directory[24]);
    const auto functionsRva = le<std::uint32_t>(&directory[28]);
    const auto namesRva = le<std::uint32_t>(&directory[32]);
    const auto ordinalsRva = le<std::uint32_t>(&directory[36]);
    if (functionCount > limits.maxNames || nameCount > limits.maxNames)
        return fail("symbols.too_large", "the export table is too large");
    std::vector<unsigned char> functions, names, ordinals;
    const auto functionsAt =
        locateRange(source, *info, functionsRva, std::uint64_t{functionCount} * 4);
    const auto namesAt = locateRange(source, *info, namesRva, std::uint64_t{nameCount} * 4);
    const auto ordinalsAt = locateRange(source, *info, ordinalsRva, std::uint64_t{nameCount} * 2);
    if (!functionsAt || !namesAt || !ordinalsAt ||
        !readRecords(source, *functionsAt, functionCount, 4, functions) ||
        !readRecords(source, *namesAt, nameCount, 4, names) ||
        !readRecords(source, *ordinalsAt, nameCount, 2, ordinals))
        return malformed("export tables");

    Builder builder(limits, Format::Pe, image.id);
    std::vector<char> text;
    for (std::uint32_t i = 0; i < nameCount; ++i) {
        const auto index = le<std::uint16_t>(&ordinals[std::size_t{i} * 2]);
        if (index >= functionCount) {
            builder.skip();
            continue;
        }
        const auto rva = le<std::uint32_t>(&functions[std::size_t{index} * 4]);
        // A forwarded export names another DLL's function; its "address" is a string.
        const bool forwarded = rva >= info->exportRva && rva - info->exportRva < info->exportSize;
        const PeSection* section = nullptr;
        for (const auto& s : info->sections)
            if (rva >= s.va && rva - s.va < s.extent()) section = &s;
        if (forwarded || !section || (section->characteristics & (0x20000000u | 0x20u)) == 0) {
            builder.skip();
            continue;
        }
        const auto nameAtFile =
            locate(source, *info, le<std::uint32_t>(&names[std::size_t{i} * 4]));
        if (!nameAtFile) {
            builder.skip();
            continue;
        }
        const std::size_t room = static_cast<std::size_t>(
            std::min<std::uint64_t>(nameAtFile->available, limits.maxNameBytes + 1));
        text.resize(room);
        if (!source.read(nameAtFile->offset, text.data(), room)) {
            builder.skip();
            continue;
        }
        const auto name = nameAt(text.data(), text.size(), 0, limits.maxNameBytes);
        if (!name) {
            builder.skip();
            continue;
        }
        const std::uint64_t base = info->imageBase;
        if (!builder.add(base + rva, 0, base + section->va + section->extent(), *name))
            return builder.finish();
    }
    return builder.finish();
}

}  // namespace

std::string_view formatName(Format format) {
    switch (format) {
        case Format::Elf:
            return "elf";
        case Format::MachO:
            return "macho";
        case Format::Pe:
            return "pe";
    }
    return "elf";
}

bool MemorySource::read(std::uint64_t offset, void* out, std::size_t bytes) const {
    if (offset > size_ || bytes > size_ - offset) return false;
    if (bytes > 0) std::memcpy(out, data_ + offset, bytes);
    return true;
}

FileSource::FileSource(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return;
    const auto length = std::filesystem::file_size(path, ec);
    if (ec) return;
    file_.open(path, std::ios::binary);
    if (!file_) return;
    size_ = length;
    ok_ = true;
}
bool FileSource::read(std::uint64_t offset, void* out, std::size_t bytes) const {
    if (!ok_ || offset > size_ || bytes > size_ - offset) return false;
    if (bytes == 0) return true;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes));
    return file_.good() && static_cast<std::size_t>(file_.gcount()) == bytes;
}

std::vector<Identity> identify(const Source& source, const Limits& limits) {
    std::vector<Identity> found;
    if (source.size() > limits.maxFileBytes) return found;
    if (const auto elf = elfHeader(source)) {
        found.push_back({Format::Elf, elfId(source, *elf, limits), 0, source.size()});
        return found;
    }
    if (const auto info = peInfo(source)) {
        found.push_back({Format::Pe, peId(source, *info), 0, source.size()});
        return found;
    }
    for (const auto& [offset, length] : machSlices(source, limits)) {
        const Window slice(source, offset, length);
        const auto parsed = machInfo(slice, limits);
        if (!parsed || !parsed->ok()) continue;
        const MachInfo& info = parsed->value();
        found.push_back({Format::MachO, info.hasUuid ? hex(info.uuid.data(), 16) : std::string{},
                         offset, length});
    }
    return found;
}

Result<SymbolTable> readSymbols(const Source& source, const Identity& image, const Limits& limits) {
    if (source.size() > limits.maxFileBytes)
        return fail("symbols.too_large", "the file is larger than the reader accepts");
    if (!within(source, image.offset, image.length))
        return fail("symbols.malformed", "the image lies outside the file");
    switch (image.format) {
        case Format::Elf:
            return readElf(source, image, limits);
        case Format::MachO:
            return readMachO(source, image, limits);
        case Format::Pe:
            return readPe(source, image, limits);
    }
    return fail("symbols.unsupported", "unknown image format");
}

std::optional<Match> lookup(const SymbolTable& table, std::uint64_t address) {
    const auto after =
        std::upper_bound(table.symbols.begin(), table.symbols.end(), address,
                         [](std::uint64_t value, const Symbol& s) { return value < s.address; });
    if (after == table.symbols.begin()) return std::nullopt;
    const Symbol& s = *(after - 1);
    if (address >= s.end) return std::nullopt;
    return Match{std::string_view(table.names).substr(s.name, s.length), address - s.address};
}

std::string demangle(std::string_view name) {
#if defined(SUPPLE_HAVE_CXXABI)
    if (name.size() > 2 && name[0] == '_' && name[1] == 'Z') {
        const std::string mangled(name);
        int status = 0;
        char* readable = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
        if (status == 0 && readable) {
            std::string out(readable);
            std::free(readable);
            return out;
        }
        std::free(readable);
    }
#endif
    return std::string(name);
}

}  // namespace supple::diagnostics::resolver
