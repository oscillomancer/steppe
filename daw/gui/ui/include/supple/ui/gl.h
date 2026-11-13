// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Bulk GL layers (docs/specs/gui.md §3, ADR-0005 §3): data-dense content
// (waveforms, notes, clip bodies) bypasses NanoVG. A node that sets
// `drawsGl` gets paintGl() after its NanoVG paint, with NanoVG flushed and
// the GL scissor set to the region being painted. Renderers here batch a
// whole view into one or two draw calls.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/ui/geometry.h"
#include "supple/ui/theme.h"

namespace supple::ui {

// The frame a GL layer draws into, in design units (window coordinates).
struct GlFrame {
    float width = 0, height = 0, ratio = 1;
    Rect clip;  // the region being painted (already set as the GL scissor)
    // Design units → clip space: x' = x·sx + tx, y' = y·sy + ty.
    std::array<float, 4> transform() const { return {2.0f / width, -2.0f / height, -1.0f, 1.0f}; }
};

std::uint32_t packRgba(Color c);  // RGBA8 for vertex attributes

class GlProgram {
  public:
    static Result<GlProgram> build(std::string_view vertex, std::string_view fragment);
    GlProgram() = default;
    GlProgram(GlProgram&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    GlProgram& operator=(GlProgram&& o) noexcept;
    ~GlProgram();
    unsigned id() const { return id_; }
    int uniform(const char* name) const;

  private:
    unsigned id_ = 0;
};

// Filled bands as triangle strips: each run is a sequence of (top, bottom)
// vertex pairs, left to right (a waveform channel between its max and min
// curves). One upload and one multi-draw per flush.
class StripRenderer {
  public:
    struct Vertex {
        float x, y;
        std::uint32_t rgba;
    };
    static Result<StripRenderer> create();
    StripRenderer(StripRenderer&&) noexcept;
    StripRenderer& operator=(StripRenderer&&) noexcept;
    ~StripRenderer();

    void beginRun();
    void pair(float x, float top, float bottom, std::uint32_t rgba) {
        vertices_.push_back({x, top, rgba});
        vertices_.push_back({x, bottom, rgba});
    }
    void endRun();
    std::size_t vertexCount() const { return vertices_.size(); }
    // Draws and clears what was added.
    void flush(const GlFrame& frame);

  private:
    StripRenderer() = default;
    GlProgram program_;
    int transform_ = -1;
    unsigned vao_ = 0, vbo_ = 0;
    std::size_t capacity_ = 0;
    std::vector<Vertex> vertices_;
    std::vector<int> firsts_, counts_;
    std::size_t runStart_ = 0;
};

// Instanced rectangles with an optional 1-pixel darker border (MIDI notes,
// clip bodies, meter segments).
class RectRenderer {
  public:
    struct Instance {
        float x, y, w, h;
        std::uint32_t rgba;
        std::uint32_t border;  // RGBA8; alpha 0 = none
        // The part of the rect inside the frame's clip, which is what is
        // drawn; the whole rect still places the border (set by flush).
        float areaX = 0, areaY = 0, areaW = 0, areaH = 0;
    };
    static Result<RectRenderer> create();
    RectRenderer(RectRenderer&&) noexcept;
    RectRenderer& operator=(RectRenderer&&) noexcept;
    ~RectRenderer();

    void add(const Rect& r, std::uint32_t rgba, std::uint32_t border = 0) {
        instances_.push_back({r.x, r.y, r.w, r.h, rgba, border});
    }
    std::size_t count() const { return instances_.size(); }
    void flush(const GlFrame& frame);

  private:
    RectRenderer() = default;
    GlProgram program_;
    int transform_ = -1, ratio_ = -1;
    unsigned vao_ = 0, vbo_ = 0;
    std::size_t capacity_ = 0;
    std::vector<Instance> instances_;
};

}  // namespace supple::ui
