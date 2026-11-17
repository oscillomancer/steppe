// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The header parsers behind `identify`, shared with the crash resolver's symbol readers.
// Internal: nothing else includes this. Each one reads through a `Source` and checks every
// count, length and offset against the file size before it allocates or reads.
#pragma once
#include <supple/imageid/image.h>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace supple::diagnostics::resolver::detail {

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
std::string hex(const unsigned char* bytes, std::size_t count);
Error fail(const char* code, const char* message, const char* hint = "");
Error malformed(const char* what);

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

bool within(const Source& source, std::uint64_t offset, std::uint64_t bytes);
// `count` records of `entry` bytes at `offset`, checked before anything is allocated.
bool readRecords(const Source& source, std::uint64_t offset, std::uint64_t count,
                 std::uint64_t entry, std::vector<unsigned char>& out);

struct ElfHeader {
    std::uint16_t type = 0;
    std::uint64_t phoff = 0, shoff = 0;
    std::uint16_t phentsize = 0, phnum = 0, shentsize = 0, shnum = 0;
};
struct ElfSection {
    std::uint32_t type = 0, link = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0;
};

std::optional<ElfHeader> elfHeader(const Source& source);
bool elfSections(const Source& source, const ElfHeader& h, const Limits& limits,
                 std::vector<ElfSection>& out);
std::string elfId(const Source& source, const ElfHeader& h, const Limits& limits);

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

// Nullopt: not a thin 64-bit Mach-O. Error: a Mach-O whose load commands are damaged.
std::optional<Result<MachInfo>> machInfo(const Source& source, const Limits& limits);
// Slices of a file: itself when thin, each architecture when universal.
std::vector<std::pair<std::uint64_t, std::uint64_t>> machSlices(const Source& source,
                                                                const Limits& limits);

struct PeSection {
    std::uint32_t va = 0, virtualSize = 0, rawSize = 0, rawPointer = 0, characteristics = 0;
    std::uint64_t extent() const { return virtualSize ? virtualSize : rawSize; }
};
struct PeInfo {
    std::uint64_t imageBase = 0;
    std::uint32_t exportRva = 0, exportSize = 0, debugRva = 0, debugSize = 0;
    std::vector<PeSection> sections;
};
struct Located {
    std::uint64_t offset = 0;
    std::uint64_t available = 0;  // bytes from here to the end of the section's file data
};

// Nullopt: not a PE32+ image.
std::optional<PeInfo> peInfo(const Source& source);
std::optional<Located> locate(const Source& source, const PeInfo& info, std::uint64_t rva);
std::optional<std::uint64_t> locateRange(const Source& source, const PeInfo& info,
                                         std::uint64_t rva, std::uint64_t bytes);
std::string peId(const Source& source, const PeInfo& info);

}  // namespace supple::diagnostics::resolver::detail
