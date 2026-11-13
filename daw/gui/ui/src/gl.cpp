// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/gl.h"

#include "supple/gui/gl_api.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace supple::ui {
namespace {

Result<unsigned> compile(GLenum type, std::string_view source) {
    const GLuint shader = glCreateShader(type);
    const char* text = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        glDeleteShader(shader);
        return Error("gui.gl.compile", std::string("shader did not compile: ") + log);
    }
    return shader;
}

// Grows (never shrinks) a stream buffer and uploads `bytes` into it.
void upload(GLuint vbo, std::size_t& capacity, const void* data, std::size_t bytes) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (bytes > capacity) {
        capacity = std::max(bytes, capacity * 2);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity), nullptr, GL_STREAM_DRAW);
    } else {
        // Orphan the old storage so the driver need not wait for the last draw.
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity), nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), data);
}

void blendState() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

constexpr std::string_view kStripVertex = R"(#version 330 core
uniform vec4 transform;
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
out vec4 vColor;
void main() {
    vColor = color;
    gl_Position = vec4(position * transform.xy + transform.zw, 0.0, 1.0);
}
)";

constexpr std::string_view kStripFragment = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

constexpr std::string_view kRectVertex = R"(#version 330 core
uniform vec4 transform;
uniform float ratio;
layout(location = 0) in vec4 rect;    // x, y, w, h (design units)
layout(location = 1) in vec4 color;
layout(location = 2) in vec4 border;
layout(location = 3) in vec4 area;    // the part of rect inside the clip
out vec4 vColor;
out vec4 vBorder;
flat out vec4 vPixelRect;  // top-left and size in framebuffer pixels
void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 p = area.xy + corner * area.zw;
    vColor = color;
    vBorder = border;
    vPixelRect = rect * ratio;
    gl_Position = vec4(p * transform.xy + transform.zw, 0.0, 1.0);
}
)";

constexpr std::string_view kRectFragment = R"(#version 330 core
in vec4 vColor;
in vec4 vBorder;
flat in vec4 vPixelRect;
layout(origin_upper_left) in vec4 gl_FragCoord;
out vec4 fragColor;
void main() {
    // Pixel coordinates avoid scissor-dependent interpolation rounding at a
    // border threshold (notably on Apple's software renderer).
    vec2 vLocal = gl_FragCoord.xy - vPixelRect.xy;
    vec2 vSize = vPixelRect.zw;
    // A one-device-pixel border where the rect is big enough to carry one.
    bool edge = vBorder.a > 0.0 && vSize.x >= 5.0 && vSize.y >= 5.0 &&
                (vLocal.x < 1.0 || vLocal.y < 1.0 || vLocal.x > vSize.x - 1.0 || vLocal.y > vSize.y - 1.0);
    fragColor = edge ? vBorder : vColor;
}
)";

}  // namespace

std::uint32_t packRgba(Color c) {
    auto byte = [](float v) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    // Little-endian bytes r, g, b, a for a GL_UNSIGNED_BYTE vec4 attribute.
    return byte(c.r) | (byte(c.g) << 8) | (byte(c.b) << 16) | (byte(c.a) << 24);
}

Result<GlProgram> GlProgram::build(std::string_view vertex, std::string_view fragment) {
    auto vsr = compile(GL_VERTEX_SHADER, vertex);
    if (!vsr) return std::move(vsr).error();
    const unsigned vs = *vsr;
    auto fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (!fs) {
        glDeleteShader(vs);
        return std::move(fs).error();
    }
    GlProgram p;
    p.id_ = glCreateProgram();
    glAttachShader(p.id_, vs);
    glAttachShader(p.id_, *fs);
    glLinkProgram(p.id_);
    glDeleteShader(vs);
    glDeleteShader(*fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(p.id_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(p.id_, sizeof log, nullptr, log);
        return Error("gui.gl.link", std::string("program did not link: ") + log);
    }
    return p;
}

GlProgram& GlProgram::operator=(GlProgram&& o) noexcept {
    std::swap(id_, o.id_);
    return *this;
}

GlProgram::~GlProgram() {
    if (id_) glDeleteProgram(id_);
}

int GlProgram::uniform(const char* name) const { return glGetUniformLocation(id_, name); }

// --- StripRenderer

Result<StripRenderer> StripRenderer::create() {
    auto program = GlProgram::build(kStripVertex, kStripFragment);
    if (!program) return std::move(program).error();
    StripRenderer r;
    r.program_ = std::move(*program);
    r.transform_ = r.program_.uniform("transform");
    glGenVertexArrays(1, &r.vao_);
    glGenBuffers(1, &r.vbo_);
    glBindVertexArray(r.vao_);
    glBindBuffer(GL_ARRAY_BUFFER, r.vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, rgba)));
    glBindVertexArray(0);
    return r;
}

StripRenderer::StripRenderer(StripRenderer&& o) noexcept { *this = std::move(o); }

StripRenderer& StripRenderer::operator=(StripRenderer&& o) noexcept {
    std::swap(program_, o.program_);
    std::swap(transform_, o.transform_);
    std::swap(vao_, o.vao_);
    std::swap(vbo_, o.vbo_);
    std::swap(capacity_, o.capacity_);
    std::swap(vertices_, o.vertices_);
    std::swap(firsts_, o.firsts_);
    std::swap(counts_, o.counts_);
    std::swap(runStart_, o.runStart_);
    return *this;
}

StripRenderer::~StripRenderer() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void StripRenderer::beginRun() { runStart_ = vertices_.size(); }

void StripRenderer::endRun() {
    const std::size_t n = vertices_.size() - runStart_;
    if (n >= 4) {
        firsts_.push_back(static_cast<int>(runStart_));
        counts_.push_back(static_cast<int>(n));
    } else {
        vertices_.resize(runStart_);  // fewer than two columns draw nothing
    }
}

void StripRenderer::flush(const GlFrame& frame) {
    if (!firsts_.empty()) {
        upload(vbo_, capacity_, vertices_.data(), vertices_.size() * sizeof(Vertex));
        blendState();
        glUseProgram(program_.id());
        const auto t = frame.transform();
        glUniform4f(transform_, t[0], t[1], t[2], t[3]);
        glBindVertexArray(vao_);
        glMultiDrawArrays(GL_TRIANGLE_STRIP, firsts_.data(), counts_.data(),
                          static_cast<GLsizei>(firsts_.size()));
        glBindVertexArray(0);
    }
    vertices_.clear();
    firsts_.clear();
    counts_.clear();
}

// --- RectRenderer

Result<RectRenderer> RectRenderer::create() {
    auto program = GlProgram::build(kRectVertex, kRectFragment);
    if (!program) return std::move(program).error();
    RectRenderer r;
    r.program_ = std::move(*program);
    r.transform_ = r.program_.uniform("transform");
    r.ratio_ = r.program_.uniform("ratio");
    glGenVertexArrays(1, &r.vao_);
    glGenBuffers(1, &r.vbo_);
    glBindVertexArray(r.vao_);
    glBindBuffer(GL_ARRAY_BUFFER, r.vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                          reinterpret_cast<void*>(offsetof(Instance, x)));
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Instance),
                          reinterpret_cast<void*>(offsetof(Instance, rgba)));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Instance),
                          reinterpret_cast<void*>(offsetof(Instance, border)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                          reinterpret_cast<void*>(offsetof(Instance, areaX)));
    glVertexAttribDivisor(3, 1);
    glBindVertexArray(0);
    return r;
}

RectRenderer::RectRenderer(RectRenderer&& o) noexcept { *this = std::move(o); }

RectRenderer& RectRenderer::operator=(RectRenderer&& o) noexcept {
    std::swap(program_, o.program_);
    std::swap(transform_, o.transform_);
    std::swap(ratio_, o.ratio_);
    std::swap(vao_, o.vao_);
    std::swap(vbo_, o.vbo_);
    std::swap(capacity_, o.capacity_);
    std::swap(instances_, o.instances_);
    return *this;
}

RectRenderer::~RectRenderer() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void RectRenderer::flush(const GlFrame& frame) {
    if (!instances_.empty()) {
#if defined(__APPLE__)
        // Apple's software rasterizer can cover a half-pixel triangle edge
        // differently after scissor clipping. Whole-pixel edges keep partial
        // and complete repaints identical (notably MIDI note thumbnails).
        for (auto& instance : instances_) {
            const float right = std::round((instance.x + instance.w) * frame.ratio) / frame.ratio;
            const float bottom = std::round((instance.y + instance.h) * frame.ratio) / frame.ratio;
            instance.x = std::round(instance.x * frame.ratio) / frame.ratio;
            instance.y = std::round(instance.y * frame.ratio) / frame.ratio;
            instance.w = right - instance.x;
            instance.h = bottom - instance.y;
        }
#endif
        // Draw only what lies in the clip, so no quad crosses the edge of
        // the viewport: Apple's software rasterizer drops some instances
        // that do (whole item bodies were missing from single frames while
        // the arrange view followed the playhead). The shader still borders
        // the whole rect, so the pixels are the same.
        std::size_t kept = 0;
        for (const auto& instance : instances_) {
            const Rect area = Rect{instance.x, instance.y, instance.w, instance.h}.intersect(frame.clip);
            if (area.empty()) continue;
            Instance& out = instances_[kept++];
            out = instance;
            out.areaX = area.x;
            out.areaY = area.y;
            out.areaW = area.w;
            out.areaH = area.h;
        }
        instances_.resize(kept);
    }
    if (!instances_.empty()) {
        upload(vbo_, capacity_, instances_.data(), instances_.size() * sizeof(Instance));
        blendState();
        glUseProgram(program_.id());
        const auto t = frame.transform();
        glUniform4f(transform_, t[0], t[1], t[2], t[3]);
        glUniform1f(ratio_, frame.ratio);
        glBindVertexArray(vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(instances_.size()));
        glBindVertexArray(0);
    }
    instances_.clear();
}

}  // namespace supple::ui
