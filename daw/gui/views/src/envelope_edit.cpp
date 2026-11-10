// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/envelope_edit.h"

#include <algorithm>
#include <cmath>

namespace supple::views {
namespace {

bool near(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(a)); }

bool isSelected(const model::EnvPoint& p, const std::vector<model::EnvPoint>& selected) {
    return std::any_of(selected.begin(), selected.end(),
                       [&](const model::EnvPoint& s) { return samePoint(p, s); });
}

void sortByTime(std::vector<model::EnvPoint>& points) {
    std::stable_sort(
        points.begin(), points.end(),
        [](const model::EnvPoint& a, const model::EnvPoint& b) { return a.pos < b.pos; });
}

}  // namespace

bool samePoint(const model::EnvPoint& a, const model::EnvPoint& b) {
    return near(a.pos, b.pos) && near(a.value, b.value);
}

std::vector<std::size_t> selectedIndices(const std::vector<model::EnvPoint>& points,
                                         const std::vector<model::EnvPoint>& selected) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < points.size(); ++i)
        if (isSelected(points[i], selected)) out.push_back(i);
    return out;
}

std::string envUnit(const model::Envelope& env, const EnvScale& scale) {
    switch (scale.kind) {
        case EnvScale::Kind::Volume:
            return "volume";
        case EnvScale::Kind::Percent:
        case EnvScale::Kind::Pan:
            return "pan";
        case EnvScale::Kind::Boost:
        case EnvScale::Kind::Mute:
            return "mute";
        case EnvScale::Kind::Playrate:
            return "playrate";
        case EnvScale::Kind::Plugin:
            break;
    }
    return "param:" + env.target.param;
}

EnvClip copyPoints(const std::vector<model::EnvPoint>& points,
                   const std::vector<model::EnvPoint>& selected, const EnvScale& scale,
                   std::string unit) {
    EnvClip clip;
    clip.unit = std::move(unit);
    for (std::size_t i : selectedIndices(points, selected)) {
        clip.points.push_back(points[i]);
        clip.norms.push_back(scale.toNorm(points[i].value));
    }
    if (!clip.points.empty()) {
        const double first = clip.points.front().pos;
        for (model::EnvPoint& p : clip.points) p.pos -= first;
    }
    return clip;
}

EnvEdit deletePoints(const std::vector<model::EnvPoint>& points,
                     const std::vector<model::EnvPoint>& selected) {
    EnvEdit out;
    for (const model::EnvPoint& p : points)
        if (!isSelected(p, selected)) out.points.push_back(p);
    return out;
}

EnvEdit movePoints(const std::vector<model::EnvPoint>& points,
                   const std::vector<model::EnvPoint>& selected, double beats, double norm,
                   const EnvScale& scale) {
    EnvEdit out;
    const auto picked = selectedIndices(points, selected);
    // None of them before the start: the whole group stops there.
    for (std::size_t i : picked) beats = std::max(beats, -points[i].pos);
    out.points = points;
    for (std::size_t i : picked) {
        model::EnvPoint& p = out.points[i];
        p.pos += beats;
        if (norm != 0) p.value = scale.fromNorm(std::clamp(scale.toNorm(p.value) + norm, 0.0, 1.0));
        out.selected.push_back(p);
    }
    sortByTime(out.points);
    return out;
}

EnvEdit shapePoints(const std::vector<model::EnvPoint>& points,
                    const std::vector<model::EnvPoint>& selected, model::PointShape shape) {
    EnvEdit out;
    out.points = points;
    for (std::size_t i : selectedIndices(points, selected)) {
        out.points[i].shape = shape;
        if (shape != model::PointShape::Bezier) out.points[i].tension = 0;
        out.selected.push_back(out.points[i]);
    }
    return out;
}

EnvEdit pastePoints(const std::vector<model::EnvPoint>& points, const EnvClip& clip, double atBeat,
                    const EnvScale& scale, const std::string& unit) {
    EnvEdit out;
    if (clip.empty()) {
        out.points = points;
        return out;
    }
    atBeat = std::max(0.0, atBeat);
    const double end = atBeat + clip.points.back().pos;
    for (const model::EnvPoint& p : points)
        if (p.pos < atBeat - 1e-9 || p.pos > end + 1e-9) out.points.push_back(p);
    for (std::size_t i = 0; i < clip.points.size(); ++i) {
        model::EnvPoint p = clip.points[i];
        p.pos += atBeat;
        if (unit != clip.unit) p.value = scale.fromNorm(clip.norms[i]);
        out.points.push_back(p);
        out.selected.push_back(p);
    }
    sortByTime(out.points);
    return out;
}

}  // namespace supple::views
