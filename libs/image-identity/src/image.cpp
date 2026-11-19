// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/imageid/image.h>
#include <supple/imageid/parse.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <system_error>

namespace supple::diagnostics::resolver::detail {

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
Error fail(const char* code, const char* message, const char* hint) {
    return Error(code, message, {}, hint);
}
Error malformed(const char* what) {
    return Error("symbols.malformed", std::string("the file is damaged: ") + what, {},
                 "Use the file the crashing build produced; this one was changed or cut short.");
}

bool within(const Source& source, std::uint64_t offset, std::uint64_t bytes) {
    return offset <= source.size() && bytes <= source.size() - offset;
}
bool readRecords(const Source& source, std::uint64_t offset, std::uint64_t count,
                 std::uint64_t entry, std::vector<unsigned char>& out) {
    if (entry == 0 || count > source.size() / entry) return false;
    const std::uint64_t bytes = count * entry;
    if (!within(source, offset, bytes)) return false;
    out.resize(static_cast<std::size_t>(bytes));
    return bytes == 0 || source.read(offset, out.data(), out.size());
}

// --- ELF -------------------------------------------------------------------------------

namespace {
constexpr std::size_t kMaxProgramHeaders = 4096;
constexpr std::size_t kMaxNoteBytes = 64 * 1024;
constexpr std::size_t kMaxBuildIdBytes = 64;
}  // namespace

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
namespace {
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
}  // namespace
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

// --- Mach-O ----------------------------------------------------------------------------

namespace {
constexpr std::size_t kMaxMachSections = 1024;
}  // namespace

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

// --- PE --------------------------------------------------------------------------------

namespace {
constexpr std::size_t kMaxPeSections = 96;
constexpr std::size_t kMaxDebugEntries = 16;
}  // namespace

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

}  // namespace supple::diagnostics::resolver::detail

namespace supple::diagnostics::resolver {
using namespace detail;

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

}  // namespace supple::diagnostics::resolver
