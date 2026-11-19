// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/gui/png.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include <zlib.h>

#include "supple/base/files.h"

namespace supple::gui {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}

void chunk(std::vector<std::uint8_t>& out, const char type[4],
           const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace

std::vector<std::uint8_t> encodePng(const Image& image) {
    if (image.width <= 0 || image.height <= 0) return {};
    const auto w = static_cast<std::size_t>(image.width),
               h = static_cast<std::size_t>(image.height);
    if (w > (std::numeric_limits<std::size_t>::max() - 1) / 4 ||
        h > std::numeric_limits<std::size_t>::max() / (w * 4 + 1) || image.rgba.size() != w * h * 4)
        return {};
    // Raw scanlines, each prefixed with filter type 0.
    std::vector<std::uint8_t> raw;
    raw.reserve(h * (w * 4 + 1));
    for (std::size_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const auto* row = image.rgba.data() + y * w * 4;
        raw.insert(raw.end(), row, row + w * 4);
    }
    // One zlib stream (the system zlib, Zlib license).
    uLongf zSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(zSize);
    if (compress2(z.data(), &zSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        return {};
    z.resize(zSize);

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(w));
    put32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, adaptive, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

Image opaque(Image image) {
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
    return image;
}

Status writePng(const std::filesystem::path& path, const Image& image) {
    const auto bytes = encodePng(image);
    if (bytes.empty())
        return Error("gui.png", "cannot encode the image", {},
                     "provide positive dimensions and exactly width * height * 4 RGBA bytes");
    return writeFileAtomic(path, std::span<const std::uint8_t>(bytes));
}

}  // namespace supple::gui
