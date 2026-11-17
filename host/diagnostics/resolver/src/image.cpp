// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/crash-resolver/image.h>
#include <supple/imageid/parse.h>
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
using namespace detail;
namespace {

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
