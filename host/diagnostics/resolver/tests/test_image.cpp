// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/crash-resolver/image.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "images.h"
#include "temp_dir.h"
#if __has_include(<cxxabi.h>)
#define HAVE_DEMANGLER 1
#endif

using namespace supple::diagnostics::resolver;
namespace fs = std::filesystem;

namespace {

std::vector<images::Func> elfFunctions() {
    return {{"main", 0x1100, 0x40},
            {"alias", 0x1100, 0x10},
            {"_ZN3foo3barEv", 0x1200, 0x20},
            {"asm_label", 0x1300, 0},
            {"after", 0x1400, 0x10},
            {"data_func", 0x3010, 0x10, 2, 2},  // in a section that is not code
            {"object", 0x1500, 8, 1},           // not a function
            {"undefined", 0, 0, 2, 0}};
}
std::vector<std::uint8_t> sampleElf() {
    images::ElfSpec spec;
    spec.functions = elfFunctions();
    return images::elf(spec);
}
std::vector<std::uint8_t> sampleMacho() {
    images::MachSpec spec;
    spec.functions = {{"_main", 0x100001100ull},
                      {"__ZN3foo3barEv", 0x100001200ull},
                      {"_debug", 0x100001300ull, 0, 0x24},  // a stab entry
                      {"_extern", 0, 0, 0x01},              // undefined
                      {"_outside", 0x100003000ull},
                      {"_nosection", 0x100001400ull, 0, 2, 2}};
    return images::macho(spec);
}
std::vector<std::uint8_t> samplePe() {
    images::PeSpec spec;
    spec.exports = {{"Alpha", 0x1100}, {"Beta", 0x1200}, {"Forwarded", 0}, {"Data", 0x2100}};
    spec.forwarded = {2};
    return images::pe(spec);
}

Identity only(const Source& source) {
    const auto found = identify(source);
    REQUIRE(found.size() == 1);
    return found.front();
}
SymbolTable read(const std::vector<std::uint8_t>& bytes, const Limits& limits = {}) {
    const MemorySource source(bytes);
    auto table = readSymbols(source, only(source), limits);
    REQUIRE(table);
    return std::move(table.value());
}
bool sane(const SymbolTable& table) {
    for (std::size_t i = 0; i < table.symbols.size(); ++i) {
        const auto& s = table.symbols[i];
        if (s.end <= s.address) return false;
        if (std::uint64_t{s.name} + s.length > table.names.size() || s.length == 0) return false;
        if (i > 0 && table.symbols[i - 1].address >= s.address) return false;
    }
    return true;
}
std::string nameOf(const SymbolTable& table, std::uint64_t address) {
    const auto match = lookup(table, address);
    return match ? std::string(match->name) : std::string("<none>");
}

// A source that stops answering after a number of reads.
class Failing final : public Source {
  public:
    Failing(const Source& base, int budget) : base_(base), budget_(budget) {}
    std::uint64_t size() const override { return base_.size(); }
    bool read(std::uint64_t offset, void* out, std::size_t bytes) const override {
        return budget_-- > 0 && base_.read(offset, out, bytes);
    }

  private:
    const Source& base_;
    mutable int budget_;
};

// Runs the reader over damaged copies of `image`: every prefix, then each byte replaced by
// extreme values, then reads that fail part way. The reader may refuse; it may not crash
// (the sanitizer presets watch for that) or return a table that breaks its own rules.
void damage(const std::vector<std::uint8_t>& image) {
    const auto check = [](const std::vector<std::uint8_t>& bytes, bool& ok) {
        const MemorySource source(bytes);
        for (const auto& found : identify(source)) {
            const auto table = readSymbols(source, found);
            if (table && !sane(table.value())) ok = false;
        }
    };
    bool ok = true;
    for (std::size_t length = 0; length <= image.size(); ++length) {
        const std::vector<std::uint8_t> prefix(image.begin(),
                                               image.begin() + static_cast<std::ptrdiff_t>(length));
        check(prefix, ok);
    }
    CHECK(ok);
    ok = true;
    for (std::size_t at = 0; at < image.size(); ++at)
        for (const std::uint8_t value : {0x00, 0x01, 0x7f, 0x80, 0xff}) {
            if (image[at] == value) continue;
            auto copy = image;
            copy[at] = value;
            check(copy, ok);
        }
    CHECK(ok);
    const MemorySource whole(image);
    for (int budget = 0; budget < 40; ++budget) {
        const Failing source(whole, budget);
        for (const auto& found : identify(source)) {
            const auto table = readSymbols(source, found);
            CHECK((!table || sane(table.value())));
        }
    }
}

}  // namespace

TEST_CASE("an ELF image gives its build id and its functions with extents") {
    const auto bytes = sampleElf();
    const MemorySource source(bytes);
    const auto id = only(source);
    CHECK(id.format == Format::Elf);
    CHECK(id.id == "deadbeef01020304");
    const auto table = read(bytes);
    CHECK(sane(table));
    CHECK(table.id == "deadbeef01020304");
    // The larger of two symbols at one address names it.
    CHECK(nameOf(table, 0x1100) == "main");
    CHECK(nameOf(table, 0x113f) == "main");
    CHECK(nameOf(table, 0x1140) == "<none>");
    CHECK(lookup(table, 0x1120)->displacement == 0x20);
    CHECK(nameOf(table, 0x1210) == "_ZN3foo3barEv");
    // A symbol with no size runs to the next symbol, a sized one stops at its size.
    CHECK(nameOf(table, 0x13ff) == "asm_label");
    CHECK(nameOf(table, 0x1400) == "after");
    CHECK(nameOf(table, 0x1410) == "<none>");
    CHECK(nameOf(table, 0x0fff) == "<none>");
    CHECK(nameOf(table, 0x1500) == "<none>");  // an object, not a function
    CHECK(nameOf(table, 0x3010) == "<none>");  // data section
    CHECK(table.skipped == 2);                 // the data-section and the undefined function
    CHECK_FALSE(table.truncated);
}

TEST_CASE("an ELF image reads .dynsym when there is no .symtab, and fails plainly without either") {
    images::ElfSpec spec;
    spec.functions = elfFunctions();
    spec.symtab = false;
    CHECK(nameOf(read(images::elf(spec)), 0x1100) == "main");
    spec.symbols = false;
    const auto bytes = images::elf(spec);
    const MemorySource source(bytes);
    const auto table = readSymbols(source, only(source));
    REQUIRE_FALSE(table);
    CHECK(table.error().code == "symbols.no_symbols");
    CHECK_FALSE(table.error().hint.empty());
}

TEST_CASE("a Mach-O image gives its UUID and the functions of its code sections") {
    const auto bytes = sampleMacho();
    const MemorySource source(bytes);
    const auto id = only(source);
    CHECK(id.format == Format::MachO);
    CHECK(id.id == "0102030405060708090a0b0c0d0e0f10");
    const auto table = read(bytes);
    CHECK(sane(table));
    CHECK(nameOf(table, 0x100001100ull) == "main");  // the C underscore is dropped
    CHECK(nameOf(table, 0x1000011ffull) == "main");  // no size: runs to the next symbol
    CHECK(nameOf(table, 0x100001200ull) == "_ZN3foo3barEv");
    CHECK(nameOf(table, 0x100001fffull) == "_ZN3foo3barEv");  // and the last to the section end
    CHECK(nameOf(table, 0x100002000ull) == "<none>");
    CHECK(table.skipped == 2);  // outside its section, and in a section that does not exist
}

TEST_CASE("a Mach-O file with no code section or no symbols says so") {
    images::MachSpec spec;
    spec.functions = {{"_data", 0x100001100ull}};
    spec.text = false;
    CHECK(read(images::macho(spec)).symbols.empty());
    spec.symtab = false;
    const auto bytes = images::macho(spec);
    const MemorySource source(bytes);
    const auto table = readSymbols(source, only(source));
    REQUIRE_FALSE(table);
    CHECK(table.error().code == "symbols.no_symbols");
}

TEST_CASE("a universal Mach-O file offers each slice by its own UUID") {
    images::MachSpec a, b;
    a.functions = {{"_one", 0x100001100ull}};
    b.functions = {{"_two", 0x100001100ull}};
    b.uuid.assign(16, 0xab);
    for (const bool wide : {false, true}) {
        const auto bytes = images::fat({images::macho(a), images::macho(b)}, wide);
        const MemorySource source(bytes);
        const auto found = identify(source);
        REQUIRE(found.size() == 2);
        CHECK(found[0].id == "0102030405060708090a0b0c0d0e0f10");
        CHECK(found[1].id == "abababababababababababababababab");
        CHECK(found[0].offset > 0);
        auto first = readSymbols(source, found[0]);
        auto second = readSymbols(source, found[1]);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(nameOf(*first, 0x100001100ull) == "one");
        CHECK(nameOf(*second, 0x100001100ull) == "two");
    }
}

TEST_CASE("a PE image gives the id the collector reads and its exported functions") {
    const auto bytes = samplePe();
    const MemorySource source(bytes);
    const auto id = only(source);
    CHECK(id.format == Format::Pe);
    // GUID in symbol-server order, then the age as eight hex digits.
    CHECK(id.id == "123456789abcdef001020304050607080000002a");
    const auto table = read(bytes);
    CHECK(sane(table));
    CHECK(nameOf(table, images::kPeBase + 0x1100) == "Alpha");
    CHECK(nameOf(table, images::kPeBase + 0x11ff) == "Alpha");
    CHECK(nameOf(table, images::kPeBase + 0x1200) == "Beta");
    CHECK(nameOf(table, images::kPeBase + 0x1fff) == "Beta");
    CHECK(nameOf(table, images::kPeBase + 0x2100) == "<none>");  // not in a code section
    CHECK(table.skipped == 2);  // the forwarder and the data export
}

TEST_CASE("a PE image with no export table is matched but has no symbols") {
    images::PeSpec spec;
    spec.exportTable = false;
    const auto bytes = images::pe(spec);
    const MemorySource source(bytes);
    CHECK(only(source).id.size() == 40);
    const auto table = readSymbols(source, only(source));
    REQUIRE_FALSE(table);
    CHECK(table.error().code == "symbols.no_symbols");
    spec.debug = false;
    const auto none = images::pe(spec);
    const MemorySource anonymous(none);
    CHECK(only(anonymous).id.empty());
}

TEST_CASE("damaged and cut-short ELF images never crash the reader") { damage(sampleElf()); }
TEST_CASE("damaged and cut-short Mach-O images never crash the reader") {
    damage(sampleMacho());
    images::MachSpec a, b;
    a.functions = {{"_one", 0x100001100ull}};
    b.functions = {{"_two", 0x100001100ull}};
    damage(images::fat({images::macho(a), images::macho(b)}));
}
TEST_CASE("damaged and cut-short PE images never crash the reader") { damage(samplePe()); }

TEST_CASE("an image read as the wrong format fails with a structured error") {
    const auto bytes = sampleElf();
    const MemorySource source(bytes);
    auto wrong = only(source);
    wrong.format = Format::MachO;
    const auto table = readSymbols(source, wrong);
    REQUIRE_FALSE(table);
    CHECK(table.error().code == "symbols.unsupported");
    wrong.format = Format::Elf;
    wrong.length = bytes.size() + 1;  // outside the file
    CHECK(readSymbols(source, wrong).error().code == "symbols.malformed");
}

TEST_CASE("limits bound what a file can make the reader hold") {
    const auto bytes = sampleElf();
    {
        Limits limits;
        limits.maxSymbols = 3;
        const auto table = read(bytes, limits);
        CHECK(table.truncated);
        CHECK(table.symbols.size() == 2);
        CHECK(sane(table));
    }
    {
        Limits limits;
        limits.maxNameBytes = 4;  // "main" fits, longer names are skipped
        const auto table = read(bytes, limits);
        CHECK(nameOf(table, 0x1100) == "main");
        CHECK(nameOf(table, 0x1200) == "<none>");
        CHECK(table.skipped >= 2);
    }
    const MemorySource source(bytes);
    const auto id = only(source);
    Limits tiny;
    tiny.maxStringBytes = 4;
    CHECK(readSymbols(source, id, tiny).error().code == "symbols.too_large");
    Limits entries;
    entries.maxEntries = 2;
    CHECK(readSymbols(source, id, entries).error().code == "symbols.too_large");
    Limits small;
    small.maxFileBytes = 100;
    CHECK(identify(source, small).empty());
    CHECK(readSymbols(source, id, small).error().code == "symbols.too_large");
    Limits sections;
    sections.maxSections = 3;
    CHECK(readSymbols(source, id, sections).error().code == "symbols.no_symbols");
    const auto macho = sampleMacho();
    Limits commands;
    commands.maxLoadCommands = 2;
    CHECK(identify(MemorySource(macho), commands).empty());
    const auto pe = samplePe();
    const MemorySource peSource(pe);
    Limits names;
    names.maxNames = 1;
    CHECK(readSymbols(peSource, only(peSource), names).error().code == "symbols.too_large");
}

TEST_CASE("a name with no terminator inside the string table is skipped, not read past") {
    images::ElfSpec spec;
    spec.functions = {{"last", 0x1100, 0x10}};
    auto bytes = images::elf(spec);
    // Shorten the string table by one byte: its last name loses its terminator.
    std::uint64_t sectionTable = 0;
    std::memcpy(&sectionTable, &bytes[40], 8);
    std::uint64_t size = 0;
    std::memcpy(&size, &bytes[sectionTable + 4 * 64 + 32], 8);
    REQUIRE(size > 1);
    --size;
    std::memcpy(&bytes[sectionTable + 4 * 64 + 32], &size, 8);
    const auto table = read(bytes);
    CHECK(table.symbols.empty());
    CHECK(table.skipped == 1);
}

TEST_CASE("names are demangled where the platform can") {
    CHECK(demangle("plain") == "plain");
    CHECK(demangle("_Zbroken") == "_Zbroken");
#if defined(HAVE_DEMANGLER)
    CHECK(demangle("_ZN3foo3barEv") == "foo::bar()");
#endif
}

TEST_CASE("a file source reads ranges and refuses everything else") {
    const tempdir::Dir scratch("crash-resolver-source");
    const auto& dir = scratch.path;
    const auto path = dir / "bytes.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "0123456789";
    }
    const FileSource file(path);
    REQUIRE(file.ok());
    CHECK(file.size() == 10);
    char buffer[4] = {};
    CHECK(file.read(6, buffer, 4));
    CHECK(std::string(buffer, 4) == "6789");
    CHECK_FALSE(file.read(7, buffer, 4));
    CHECK_FALSE(file.read(11, buffer, 0));
    CHECK(file.read(10, buffer, 0));
    CHECK_FALSE(FileSource(dir / "missing").ok());
    CHECK_FALSE(FileSource(dir).ok());
    const auto bytes = sampleElf();
    {
        std::ofstream out(dir / "image", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    const FileSource image(dir / "image");
    const auto found = identify(image);
    REQUIRE(found.size() == 1);
    const auto table = readSymbols(image, found.front());
    REQUIRE(table);
    CHECK(nameOf(*table, 0x1100) == "main");
}
