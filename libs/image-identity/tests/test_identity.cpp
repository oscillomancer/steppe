// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Tests of the image-identity library alone: which executable image a file is, its build id, and
// the bounds of the reader. Symbols and their resolution are the crash resolver's tests.
#include <doctest/doctest.h>
#include <supple/imageid/image.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "images.h"
#include "temp_dir.h"

using namespace supple::diagnostics::resolver;

namespace {

std::vector<images::Func> someFunctions() { return {{"main", 0x1100, 0x40}}; }
std::vector<std::uint8_t> sampleElf() {
    images::ElfSpec spec;
    spec.functions = someFunctions();
    return images::elf(spec);
}
Identity only(const Source& source) {
    const auto found = identify(source);
    REQUIRE(found.size() == 1);
    return found.front();
}

}  // namespace

TEST_CASE("an ELF image gives its build id and its format") {
    const auto bytes = sampleElf();
    const MemorySource source(bytes);
    const auto id = only(source);
    CHECK(id.format == Format::Elf);
    CHECK(id.id == "deadbeef01020304");
}

TEST_CASE("an ELF build id comes from a section when there are no program headers") {
    images::ElfSpec spec;
    spec.functions = someFunctions();
    spec.programNote = false;
    const auto bytes = images::elf(spec);
    const MemorySource source(bytes);
    CHECK(only(source).id == "deadbeef01020304");
}

TEST_CASE("files that are not supported images are not identified") {
    const auto empty = std::vector<std::uint8_t>{};
    CHECK(identify(MemorySource(empty)).empty());
    const std::vector<std::uint8_t> text{'h', 'e', 'l', 'l', 'o'};
    CHECK(identify(MemorySource(text)).empty());
    // A Java class file starts like a universal Mach-O file but counts far too many images.
    std::vector<std::uint8_t> java{0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 52};
    java.resize(256, 0);
    CHECK(identify(MemorySource(java)).empty());
    // 32-bit and big-endian ELF are not read.
    auto elf32 = sampleElf();
    elf32[4] = 1;
    CHECK(identify(MemorySource(elf32)).empty());
    auto big = sampleElf();
    big[5] = 2;
    CHECK(identify(MemorySource(big)).empty());
    // An object file is neither an executable nor a library.
    auto object = sampleElf();
    object[16] = 1;
    CHECK(identify(MemorySource(object)).empty());
}

TEST_CASE("a build-id note whose size runs past its segment gives no id, not the bytes beyond") {
    // descsz 40 is a plausible build-id length (the limit is 64), but the note holds 8 bytes
    // and ends there: reading 40 would take the heap's bytes for the id. A check that the
    // note's end is inside its segment is what refuses it; without that check this id is
    // nonempty.
    for (const std::uint32_t descsz : {9u, 21u, 40u, 64u}) {
        images::ElfSpec spec;
        spec.functions = {{"f", 0x1100, 0x10}};
        spec.noteDescsz = descsz;
        const auto bytes = images::elf(spec);
        const MemorySource source(bytes);
        const auto found = identify(source);
        REQUIRE(found.size() == 1);
        CHECK(found[0].id.empty());
    }
    // The same image with the true size has its id.
    images::ElfSpec spec;
    spec.functions = {{"f", 0x1100, 0x10}};
    const auto bytes = images::elf(spec);
    const MemorySource source(bytes);
    CHECK(identify(source)[0].id == "deadbeef01020304");
    // Sizes the limit refuses outright, and zero, give none either.
    for (const std::uint32_t descsz : {65u, 1000000u, 0xffffffffu}) {
        images::ElfSpec odd;
        odd.noteDescsz = descsz;
        const auto oddBytes = images::elf(odd);
        const MemorySource oddSource(oddBytes);
        CHECK(identify(oddSource)[0].id.empty());
    }
}

TEST_CASE("a Mach-O image gives its UUID") {
    images::MachSpec spec;
    spec.functions = {{"_main", 0x100001100ull}};
    const auto bytes = images::macho(spec);
    const MemorySource source(bytes);
    const auto id = only(source);
    CHECK(id.format == Format::MachO);
    CHECK(id.id == "0102030405060708090a0b0c0d0e0f10");
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
        CHECK(found[1].offset > found[0].offset);
    }
}

TEST_CASE("a PE image gives the id the collector reads, and none without a debug entry") {
    images::PeSpec spec;
    spec.exports = {{"Alpha", 0x1100}};
    const auto bytes = images::pe(spec);
    const MemorySource source(bytes);
    const auto id = only(source);
    CHECK(id.format == Format::Pe);
    // GUID in symbol-server order, then the age as eight hex digits.
    CHECK(id.id == "123456789abcdef001020304050607080000002a");
    spec.debug = false;
    const auto none = images::pe(spec);
    const MemorySource anonymous(none);
    CHECK(only(anonymous).id.empty());
}

TEST_CASE("a file source reads ranges, refuses everything else, and identifies a file") {
    const tempdir::Dir scratch("image-identity-source");
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
    CHECK(found.front().id == "deadbeef01020304");
}
