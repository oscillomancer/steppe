// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Minimal PNG writer for snapshots and golden images: RGBA8, one zlib stream
// (system zlib), filter 0.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "supple/base/result.h"
#include "supple/gui/window.h"

namespace supple::gui {

std::vector<std::uint8_t> encodePng(const Image& image);
Status writePng(const std::filesystem::path& path, const Image& image);
// The image as a window shows it. A framebuffer read back keeps the alpha
// that blending left in it, which a display ignores and an image viewer does
// not: translucent layers would look paler in the file than on screen.
Image opaque(Image image);

}  // namespace supple::gui
