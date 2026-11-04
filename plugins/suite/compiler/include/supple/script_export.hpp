// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A player export (Steppe's ADR-0032 §5): a copy of the script player with one
// script embedded. Linux and Windows append it to the module file. macOS
// stores the same JSON in Contents/Resources/supple-script.json and signs
// the bundle ad hoc; its Mach-O never receives a trailer. An export offers
// that one plugin and no library. The appended file ends:
//
//   <payload> <payload length: 8 bytes, little-endian> "SPLSCRPT"
//
// The payload is a JSON object in UTF-8:
//
//   {"schema": "supple.script-export/1.0", "name": "<the script's name>",
//    "source": "<the script's text>"}
//
// The dynamic loader maps only what the module's headers name, so the bytes
// after it are never loaded; the player reads them as a file. Header-only and
// free of dependencies: the player and the hosts that export include it.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace supple::scriptc {

// Text the player keeps in its file, so a host can tell the player from
// another plugin before it copies one (the player defines it as an object
// the compiler and linker must keep: a string it only compares against may
// be folded into code).
#define SUPPLE_SCRIPT_PLAYER_MARK "supple.script-player: plays DSP scripts"
inline constexpr std::string_view kPlayerMark = SUPPLE_SCRIPT_PLAYER_MARK;

inline constexpr char kExportMagic[8] = {'S', 'P', 'L', 'S', 'C', 'R', 'P', 'T'};
inline constexpr std::string_view kExportSchema = "supple.script-export/1.0";
inline constexpr char kExportResource[] = "supple-script.json";
// The length and the magic, after the payload.
inline constexpr std::size_t kExportFooterBytes = 16;
// The longest payload a player reads: a script is at most 1 MiB, and JSON's
// escapes can make it longer.
inline constexpr std::uint64_t kMaxExportPayload = std::uint64_t(8) << 20;

// What to append to a copy of the player.
inline std::string exportTrailer(std::string_view payload) {
    std::string out(payload);
    const std::uint64_t n = payload.size();
    for (int i = 0; i < 8; ++i) out += static_cast<char>((n >> (8 * i)) & 0xff);
    out.append(kExportMagic, sizeof kExportMagic);
    return out;
}

// The length of the payload before `footer`, a file's last kExportFooterBytes
// bytes, in a file of `fileSize` bytes; nothing when the file has none.
inline std::optional<std::uint64_t> exportPayloadLength(std::string_view footer,
                                                        std::uint64_t fileSize) {
    if (footer.size() != kExportFooterBytes || fileSize < kExportFooterBytes ||
        std::memcmp(footer.data() + 8, kExportMagic, sizeof kExportMagic) != 0)
        return std::nullopt;
    std::uint64_t n = 0;
    for (int i = 0; i < 8; ++i)
        n |= std::uint64_t(static_cast<unsigned char>(footer[i])) << (8 * i);
    if (n > kMaxExportPayload || n > fileSize - kExportFooterBytes) return std::nullopt;
    return n;
}

// The payload a whole file ends with, or nothing.
inline std::optional<std::string_view> exportPayload(std::string_view file) {
    if (file.size() < kExportFooterBytes) return std::nullopt;
    const auto n = exportPayloadLength(file.substr(file.size() - kExportFooterBytes), file.size());
    if (!n) return std::nullopt;
    return file.substr(file.size() - kExportFooterBytes - *n, *n);
}

// A whole file without its payload: the player an export was made from (the
// file itself when it is not an export).
inline std::string_view exportPlayer(std::string_view file) {
    const auto payload = exportPayload(file);
    return payload ? file.substr(0, file.size() - kExportFooterBytes - payload->size()) : file;
}

}  // namespace supple::scriptc
