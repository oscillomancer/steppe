// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "clip_context_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

#include "supple/model/context.h"
#include "supple/notation/notation.h"

namespace supple::gui {

namespace {

constexpr int kPpq = 960;

std::int64_t tickOf(double beat) { return static_cast<std::int64_t>(std::llround(beat * kPpq)); }

}  // namespace

std::string clipLeadSheet(const std::vector<model::ContextPoint>& points, const time::TimeMap& map,
                          double lengthBeats) {
    if (points.empty()) return {};
    struct At {
        const model::ContextPoint* key = nullptr;
        const model::ContextPoint* chord = nullptr;
    };
    std::map<std::int64_t, At> byTick;
    int last = map.barBeat(std::max(0.0, lengthBeats - 1.0 / kPpq)).bar;
    for (const model::ContextPoint& p : points) {
        At& at = byTick[tickOf(p.beat)];
        (p.type == model::ContextType::Key ? at.key : at.chord) = &p;
        last = std::max(last, map.barBeat(p.beat).bar);
    }
    std::string out;
    for (int bar = 1; bar <= last; ++bar) {
        const std::int64_t t0 = tickOf(map.beatOfBar(bar)), t1 = tickOf(map.beatOfBar(bar + 1));
        std::vector<std::int64_t> cuts{t0};
        for (auto it = byTick.upper_bound(t0); it != byTick.end() && it->first < t1; ++it)
            cuts.push_back(it->first);
        cuts.push_back(t1);
        out += std::to_string(bar) + ":";
        for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
            const auto found = byTick.find(cuts[i]);
            const At at = found == byTick.end() ? At{} : found->second;
            if (at.key) out += " key " + at.key->tonic + " " + at.key->scale;
            out += ' ';
            if (!at.chord)
                out += "~";
            else if (at.chord->symbol.empty())
                out += "r";
            else
                out += "{" + at.chord->symbol + "}";
            out += ":" + notation::durationText(cuts[i + 1] - cuts[i], kPpq);
        }
        out += '\n';
    }
    return out;
}

std::vector<model::ContextPoint> withContextPoint(std::vector<model::ContextPoint> points,
                                                  const model::ContextPoint& point, bool remove) {
    const std::int64_t tick = tickOf(point.beat);
    std::erase_if(points, [&](const model::ContextPoint& p) {
        return p.type == point.type && tickOf(p.beat) == tick;
    });
    if (!remove) {
        model::ContextPoint p = point;
        p.beat = static_cast<double>(tick) / kPpq;
        points.push_back(std::move(p));
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const model::ContextPoint& a, const model::ContextPoint& b) {
                         if (tickOf(a.beat) != tickOf(b.beat)) return a.beat < b.beat;
                         return a.type == model::ContextType::Key &&
                                b.type != model::ContextType::Key;
                     });
    return points;
}

ClipContextAxis clipContextAxis(const model::Clip& clip) {
    ClipContextAxis a;
    a.length = clip.length > 0 ? clip.length : 0.0;
    a.looping = clip.loop.on && clip.loop.length > 0;
    a.from = a.looping ? clip.loop.start : 0.0;
    a.window = a.looping ? clip.loop.length : a.length;
    return a;
}

std::vector<double> ClipContextAxis::soundingBeats(double content) const {
    // As the engine's clipSoundingBeats, step for step.
    std::vector<double> beats;
    if (!(length > 0)) return beats;
    const int passes = looping ? static_cast<int>(std::ceil(length / window - 1e-9)) : 1;
    if (content < from || content >= from + window) return beats;
    for (int pass = 0; pass < passes; ++pass) {
        const double at = pass * window + content - from;
        if (at < length) beats.push_back(at);
    }
    return beats;
}

double ClipContextAxis::contextBeat(double content) const {
    const std::vector<double> beats = soundingBeats(content);
    if (!beats.empty()) return beats.front();
    return std::clamp(content, 0.0, std::max(0.0, length - 1.0 / 960.0));
}

std::vector<std::pair<double, std::optional<harmony::Key>>> clipKeySpans(const model::Clip& clip) {
    const ClipContextAxis axis = clipContextAxis(clip);
    // contextBeat is piecewise: the key it reads can change only at a key
    // point's beat seen directly (outside the window) or through the window
    // (inside it), and where the mapping jumps: the window's edges and the
    // clip's end.
    std::vector<double> cuts{0.0, axis.from, axis.from + axis.window, axis.length};
    for (const model::ContextPoint& p : clip.context)
        if (p.type == model::ContextType::Key) {
            cuts.push_back(p.beat);
            cuts.push_back(p.beat + axis.from);
        }
    std::sort(cuts.begin(), cuts.end());
    std::vector<std::pair<double, std::optional<harmony::Key>>> spans{
        {-1e9, model::keyAt(clip, axis.contextBeat(-1.0))}};
    for (const double at : cuts) {
        const auto key = model::keyAt(clip, axis.contextBeat(at));
        if (key != spans.back().second) spans.emplace_back(at, key);
    }
    return spans;
}

}  // namespace supple::gui
