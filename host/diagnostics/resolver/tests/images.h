// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Builders for tiny ELF, Mach-O and PE images, the same shapes the resolver reads.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace images {

struct Func {
    std::string name;
    std::uint64_t address = 0;
    std::uint64_t size = 0;  // 0: unknown
    unsigned type = 2;       // ELF symbol type (2 = function)
    unsigned section = 1;    // ELF section index / Mach-O section ordinal
};

class Bytes {
  public:
    std::vector<std::uint8_t> data;
    void at(std::size_t offset, std::size_t size) {
        if (data.size() < offset + size) data.resize(offset + size, 0);
    }
    void u8(std::size_t o, unsigned v) {
        at(o, 1);
        data[o] = static_cast<std::uint8_t>(v);
    }
    void u16(std::size_t o, unsigned v) { put(o, v, 2, false); }
    void u32(std::size_t o, std::uint64_t v) { put(o, v, 4, false); }
    void u64(std::size_t o, std::uint64_t v) { put(o, v, 8, false); }
    void be32(std::size_t o, std::uint64_t v) { put(o, v, 4, true); }
    void be64(std::size_t o, std::uint64_t v) { put(o, v, 8, true); }
    void text(std::size_t o, const std::string& s, bool nul = true) {
        at(o, s.size() + 1);
        std::memcpy(&data[o], s.data(), s.size());
        if (nul) data[o + s.size()] = 0;
    }
    void bytes(std::size_t o, const std::vector<std::uint8_t>& b) {
        at(o, b.size());
        if (!b.empty()) std::memcpy(&data[o], b.data(), b.size());
    }
    std::size_t size() const { return data.size(); }

  private:
    void put(std::size_t o, std::uint64_t v, std::size_t n, bool big) {
        at(o, n);
        for (std::size_t i = 0; i < n; ++i)
            data[o + (big ? n - 1 - i : i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
    }
};

// --- ELF -------------------------------------------------------------------------------
struct ElfSpec {
    std::vector<Func> functions;
    std::vector<std::uint8_t> buildId{0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04};
    bool symtab = true;   // else the symbols go in .dynsym
    bool symbols = true;  // false: neither table
    bool programNote = true;
};
// Sections: 1 .text (exec, 0x1000..0x2000), 2 .data (0x3000..0x3100), 3 .symtab, 4 .strtab,
// 5 .note.gnu.build-id.
inline std::vector<std::uint8_t> elf(const ElfSpec& spec) {
    Bytes b;
    const std::size_t phoff = 64, noteOff = 64 + 56,
                      noteSize = (12 + 4 + spec.buildId.size() + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> note;
    {
        Bytes n;
        n.u32(0, 4);
        n.u32(4, spec.buildId.size());
        n.u32(8, 3);
        n.text(12, "GNU");
        n.bytes(16, spec.buildId);
        n.at(0, noteSize);
        note = n.data;
    }
    b.bytes(noteOff, note);
    std::size_t symOff = noteOff + note.size();
    std::string strtab(1, '\0');
    std::vector<std::size_t> nameOffsets;
    for (const auto& f : spec.functions) {
        nameOffsets.push_back(strtab.size());
        strtab += f.name;
        strtab += '\0';
    }
    const std::size_t count = 1 + spec.functions.size();
    const std::size_t strOff = symOff + count * 24;
    const std::size_t shOff = (strOff + strtab.size() + 7) & ~std::size_t{7};
    b.text(0, std::string("\177ELF"), false);
    b.u8(4, 2);
    b.u8(5, 1);
    b.u8(6, 1);
    b.u16(16, 3);
    b.u16(18, 62);
    b.u32(20, 1);
    b.u64(32, spec.programNote ? phoff : 0);
    b.u64(40, shOff);
    b.u16(52, 64);
    b.u16(54, spec.programNote ? 56 : 0);
    b.u16(56, spec.programNote ? 1 : 0);
    b.u16(58, 64);
    b.u16(60, 6);
    b.u16(62, 0);
    if (spec.programNote) {
        b.u32(phoff, 4);
        b.u64(phoff + 8, noteOff);
        b.u64(phoff + 16, noteOff);
        b.u64(phoff + 32, note.size());
        b.u64(phoff + 40, note.size());
    }
    for (std::size_t i = 0; i < spec.functions.size(); ++i) {
        const auto& f = spec.functions[i];
        const std::size_t e = symOff + (i + 1) * 24;
        b.u32(e, nameOffsets[i]);
        b.u8(e + 4, 0x10 | f.type);
        b.u16(e + 6, f.section);
        b.u64(e + 8, f.address);
        b.u64(e + 16, f.size);
    }
    b.text(strOff, strtab, false);
    const auto section = [&](std::size_t i, unsigned type, std::uint64_t flags, std::uint64_t addr,
                             std::uint64_t offset, std::uint64_t size, unsigned link) {
        const std::size_t s = shOff + i * 64;
        b.u32(s + 4, type);
        b.u64(s + 8, flags);
        b.u64(s + 16, addr);
        b.u64(s + 24, offset);
        b.u64(s + 32, size);
        b.u32(s + 40, link);
        b.u64(s + 56, 24);
    };
    section(1, 1, 6, 0x1000, 0, 0x1000, 0);
    section(2, 1, 3, 0x3000, 0, 0x100, 0);
    if (spec.symbols)
        section(3, spec.symtab ? 2 : 11, 0, 0, symOff, count * 24, 4);
    else
        section(3, 1, 0, 0, 0, 0, 0);
    section(4, 3, 0, 0, strOff, strtab.size(), 0);
    section(5, 7, 2, 0, noteOff, note.size(), 0);
    b.at(shOff, 6 * 64);
    return b.data;
}

// --- Mach-O ----------------------------------------------------------------------------
struct MachSpec {
    std::vector<Func> functions;  // address is the link-time vmaddr; `type` is the nlist n_type
    std::vector<std::uint8_t> uuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    bool symtab = true;
    bool text = true;  // section 1 is __TEXT,__text (pure instructions); else __DATA,__data
};
// Section 1 spans 0x100001000..0x100002000.
inline std::vector<std::uint8_t> macho(const MachSpec& spec) {
    Bytes b;
    const std::size_t cmds = 32;
    const std::size_t segSize = 72 + 80, uuidSize = 24, symtabSize = 24;
    const std::size_t total = segSize + uuidSize + symtabSize;
    const std::size_t symOff = (cmds + total + 15) & ~std::size_t{15};
    std::string strtab(" \0", 2);
    std::vector<std::size_t> offsets;
    for (const auto& f : spec.functions) {
        offsets.push_back(strtab.size());
        strtab += f.name;
        strtab += '\0';
    }
    const std::size_t strOff = symOff + spec.functions.size() * 16;
    b.u32(0, 0xfeedfacf);
    b.u32(4, 0x0100000c);
    b.u32(12, 2);
    b.u32(16, 3);
    b.u32(20, total);
    std::size_t at = cmds;
    b.u32(at, 0x19);
    b.u32(at + 4, segSize);
    b.text(at + 8, spec.text ? "__TEXT" : "__DATA");
    b.u64(at + 24, 0x100000000ull);
    b.u64(at + 32, 0x4000);
    b.u32(at + 64, 1);  // nsects
    b.text(at + 72, spec.text ? "__text" : "__data");
    b.text(at + 88, spec.text ? "__TEXT" : "__DATA");
    b.u64(at + 104, 0x100001000ull);
    b.u64(at + 112, 0x1000);
    b.u32(at + 136, spec.text ? 0x80000400u : 0);
    at += segSize;
    b.u32(at, 0x1b);
    b.u32(at + 4, uuidSize);
    b.bytes(at + 8, spec.uuid);
    at += uuidSize;
    b.u32(at, 2);
    b.u32(at + 4, symtabSize);
    b.u32(at + 8, spec.symtab ? symOff : 0);
    b.u32(at + 12, spec.symtab ? spec.functions.size() : 0);
    b.u32(at + 16, strOff);
    b.u32(at + 20, strtab.size());
    for (std::size_t i = 0; i < spec.functions.size(); ++i) {
        const auto& f = spec.functions[i];
        const std::size_t e = symOff + i * 16;
        b.u32(e, offsets[i]);
        b.u8(e + 4, f.type == 2 ? 0x0f : f.type);  // 0x0f: external, defined in a section
        b.u8(e + 5, f.section);
        b.u64(e + 8, f.address);
    }
    b.text(strOff, strtab, false);
    return b.data;
}
inline std::vector<std::uint8_t> fat(const std::vector<std::vector<std::uint8_t>>& slices,
                                     bool wide = false) {
    Bytes b;
    b.be32(0, wide ? 0xcafebabfu : 0xcafebabeu);
    b.be32(4, slices.size());
    const std::size_t entry = wide ? 32 : 20;
    std::size_t offset = (8 + entry * slices.size() + 0x3fff) & ~std::size_t{0x3fff};
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const std::size_t e = 8 + i * entry;
        b.be32(e, 0x0100000c);
        if (wide) {
            b.be64(e + 8, offset);
            b.be64(e + 16, slices[i].size());
        } else {
            b.be32(e + 8, offset);
            b.be32(e + 12, slices[i].size());
        }
        b.bytes(offset, slices[i]);
        offset = (offset + slices[i].size() + 0x3fff) & ~std::size_t{0x3fff};
    }
    return b.data;
}

// --- PE --------------------------------------------------------------------------------
struct PeSpec {
    std::vector<Func> exports;           // address is the RVA, in .text (0x1000..0x2000)
    std::vector<std::size_t> forwarded;  // export indices that forward to another DLL
    std::array<std::uint8_t, 16> guid{0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
                                      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    std::uint32_t age = 0x2a;
    bool debug = true;
    bool exportTable = true;
};
inline constexpr std::uint64_t kPeBase = 0x140000000ull;
inline std::vector<std::uint8_t> pe(const PeSpec& spec) {
    Bytes b;
    const std::size_t peOff = 0x80, optSize = 240, sections = peOff + 24 + optSize;
    const std::size_t rdata = 0x600;  // file offset of .rdata, loaded at RVA 0x2000
    const auto rva = [](std::size_t fileOffset) { return fileOffset - 0x600 + 0x2000; };
    b.u16(0, 0x5a4d);
    b.u32(0x3c, peOff);
    b.text(peOff, std::string("PE\0\0", 4), false);
    b.u16(peOff + 4, 0x8664);
    b.u16(peOff + 6, 2);
    b.u16(peOff + 20, optSize);
    const std::size_t opt = peOff + 24;
    b.u16(opt, 0x20b);
    b.u64(opt + 24, kPeBase);
    b.u32(opt + 56, 0x4000);
    b.u32(opt + 108, 16);
    const auto section = [&](std::size_t i, const char* name, unsigned va, unsigned vsize,
                             unsigned raw, unsigned ptr, std::uint32_t chars) {
        const std::size_t s = sections + i * 40;
        b.text(s, name);
        b.u32(s + 8, vsize);
        b.u32(s + 12, va);
        b.u32(s + 16, raw);
        b.u32(s + 20, ptr);
        b.u32(s + 36, chars);
    };
    section(0, ".text", 0x1000, 0x1000, 0x200, 0x400, 0x60000020u);
    section(1, ".rdata", 0x2000, 0x1000, 0x400, 0x600, 0x40000040u);
    b.at(0x400, 0x200);
    std::size_t at = rdata;
    const std::size_t n = spec.exports.size();
    const std::size_t exportDir = at;
    const std::size_t functions = exportDir + 40;
    const std::size_t names = functions + n * 4;
    const std::size_t ordinals = names + n * 4;
    std::size_t cursor = ordinals + n * 2;
    std::vector<std::size_t> nameAt;
    for (const auto& e : spec.exports) {
        nameAt.push_back(cursor);
        b.text(cursor, e.name);
        cursor += e.name.size() + 1;
    }
    const std::string forward = "OTHER.target";
    const std::size_t forwardAt = cursor;
    b.text(forwardAt, forward);
    cursor += forward.size() + 1;
    const std::size_t exportEnd = cursor;
    b.u32(exportDir + 20, n);
    b.u32(exportDir + 24, n);
    b.u32(exportDir + 28, rva(functions));
    b.u32(exportDir + 32, rva(names));
    b.u32(exportDir + 36, rva(ordinals));
    for (std::size_t i = 0; i < n; ++i) {
        const bool fwd =
            std::find(spec.forwarded.begin(), spec.forwarded.end(), i) != spec.forwarded.end();
        b.u32(functions + i * 4, fwd ? rva(forwardAt) : spec.exports[i].address);
        b.u32(names + i * 4, rva(nameAt[i]));
        b.u16(ordinals + i * 2, i);
    }
    cursor = (cursor + 7) & ~std::size_t{7};
    const std::size_t debugDir = cursor;
    const std::size_t record = debugDir + 28;
    if (spec.debug) {
        b.u32(debugDir + 12, 2);
        b.u32(debugDir + 16, 24 + 4);
        b.u32(debugDir + 20, rva(record));
        b.u32(debugDir + 24, record);
        b.text(record, "RSDS", false);
        b.u32(record + 4, std::uint32_t{spec.guid[0]} | spec.guid[1] << 8 | spec.guid[2] << 16 |
                              std::uint32_t{spec.guid[3]} << 24);
        b.u16(record + 8, spec.guid[4] | spec.guid[5] << 8);
        b.u16(record + 10, spec.guid[6] | spec.guid[7] << 8);
        for (int i = 0; i < 8; ++i) b.u8(record + 12 + i, spec.guid[8 + i]);
        b.u32(record + 20, spec.age);
        b.text(record + 24, "x.pdb");
        cursor = record + 24 + 6;
    }
    if (spec.exportTable) {
        b.u32(opt + 112, rva(exportDir));
        b.u32(opt + 116, exportEnd - exportDir);
    }
    if (spec.debug) {
        b.u32(opt + 112 + 6 * 8, rva(debugDir));
        b.u32(opt + 112 + 6 * 8 + 4, 28);
    }
    b.at(0, 0x600 + 0x400);
    return b.data;
}

}  // namespace images
