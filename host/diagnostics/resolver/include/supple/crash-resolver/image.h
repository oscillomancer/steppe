// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Offline symbol reader for the crash resolver (ADR-0083, topic 4). It reads one
// executable image or symbol file (ELF, Mach-O, PE) and answers "which function holds this
// link-time address". Function names only: no DWARF, no line tables, no PDB.
//
// What an image is (`Source`, `Limits`, `Identity`, `identify`) lives in the library
// image-identity (`supple/imageid/image.h`), which the application also links; it is
// included here so that existing users of this header are unchanged.
#pragma once
#include <supple/base/result.h>
#include <supple/imageid/image.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace supple::diagnostics::resolver {

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
