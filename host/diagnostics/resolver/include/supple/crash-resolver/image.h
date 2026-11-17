// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Offline symbol reader for the crash resolver (ADR-0083, topic 4). It reads one
// executable image or symbol file (ELF, Mach-O, PE) and answers "which function holds this
// link-time address". Function names only: no DWARF, no line tables, no PDB.
//
// Every file is hostile input. All reads go through `Source`, which refuses ranges outside
// the file; every count, length and offset taken from a header is checked against the file
// size and a fixed limit before it is used to allocate or to read.
#pragma once
#include <supple/base/result.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace supple::diagnostics::resolver {

enum class Format { Elf, MachO, Pe };
std::string_view formatName(Format format);

// Read-only random access to bytes. `read` fills exactly `size` bytes or fails; a range
// that is not entirely inside the source fails without touching `out`.
class Source {
  public:
    virtual ~Source() = default;
    virtual std::uint64_t size() const = 0;
    virtual bool read(std::uint64_t offset, void* out, std::size_t bytes) const = 0;
};

class MemorySource final : public Source {
  public:
    MemorySource(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit MemorySource(const std::vector<std::uint8_t>& bytes)
        : data_(bytes.data()), size_(bytes.size()) {}
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, void* out, std::size_t bytes) const override;

  private:
    const std::uint8_t* data_;
    std::size_t size_;
};

class FileSource final : public Source {
  public:
    explicit FileSource(const std::filesystem::path& path);
    bool ok() const { return ok_; }
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, void* out, std::size_t bytes) const override;

  private:
    mutable std::ifstream file_;
    std::uint64_t size_ = 0;
    bool ok_ = false;
};

// Fixed bounds. A file that exceeds one is refused (`symbols.too_large`) or, for the symbol
// count alone, read up to the bound and marked truncated. Tests lower them.
struct Limits {
    std::uint64_t maxFileBytes = std::uint64_t{16} << 30;
    std::uint64_t maxSymbols = 4u << 20;        // 4M functions, at most ~128 MiB in memory
    std::uint64_t maxEntries = 8u << 20;        // symbol-table entries examined
    std::uint64_t maxStringBytes = 256u << 20;  // one string table
    std::size_t maxNameBytes = 4096;            // a longer name is skipped
    std::size_t maxSections = 65535;            // ELF section headers
    std::size_t maxLoadCommands = 4096;         // Mach-O
    std::size_t maxCommandBytes = 1u << 20;     // Mach-O load commands, together
    std::size_t maxSlices = 8;                  // Mach-O universal binary
    std::size_t maxNames = 1u << 17;            // PE exports
};

// One image inside a file: a whole ELF or PE file, or one slice of a Mach-O universal one.
struct Identity {
    Format format = Format::Elf;
    std::string id;  // lowercase hex as in the crash report; empty when the image has none
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// Which images `source` holds and their ids. Empty when it is not a supported image
// (anything but 64-bit little-endian ELF, 64-bit Mach-O or PE32+). Reads only headers.
std::vector<Identity> identify(const Source& source, const Limits& limits = {});

struct Symbol {
    std::uint64_t address = 0;  // link-time address, as in a report's frame offset
    std::uint64_t end = 0;      // one past the last address the function covers
    std::uint32_t name = 0;     // offset into SymbolTable::names
    std::uint32_t length = 0;
};

struct SymbolTable {
    Format format = Format::Elf;
    std::string id;
    std::vector<Symbol> symbols;  // sorted by address, one per address
    std::string names;
    std::uint64_t skipped = 0;  // entries not usable as functions (bad name, bad section)
    bool truncated = false;     // more functions than Limits::maxSymbols
};

// Reads the function symbols of `image`. Errors carry `symbols.*` codes: `unsupported`,
// `malformed`, `too_large`, `read_failed`, `no_symbols`.
Result<SymbolTable> readSymbols(const Source& source, const Identity& image,
                                const Limits& limits = {});

struct Match {
    std::string_view name;  // as stored (Mach-O's leading underscore removed), not demangled
    std::uint64_t displacement = 0;
};
// The function whose extent contains `address`, if any.
std::optional<Match> lookup(const SymbolTable& table, std::uint64_t address);

// Itanium-mangled names (`_Z...`) become readable where the platform has a demangler;
// everything else is returned as is.
std::string demangle(std::string_view name);

}  // namespace supple::diagnostics::resolver
