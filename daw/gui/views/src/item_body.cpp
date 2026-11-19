// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/item_body.h"

#include <algorithm>
#include <cmath>

#include "supple/model/audio_warp.h"
#include "supple/model/source_mapping.h"

namespace supple::views {

std::size_t drawAudioBody(ui::StripRenderer& strips, const ui::GlFrame& frame, const ui::Rect& r,
                          const peaks::Pyramid& pyramid, const model::Take& take,
                          const AudioWindow& window, float gain, std::uint32_t rgba) {
    const int channels = std::max(1, pyramid.channels());
    const ui::Rect vis = r.intersect(frame.clip);
    if (vis.empty() || r.h < 4) return 0;
    const float px = 1.0f / frame.ratio;  // one device pixel in design units
    const double rate = pyramid.sampleRate();
    const double sourceSeconds = static_cast<double>(pyramid.frames()) / rate;
    const model::AudioWarpMap timeMap(take);
    const double speed = model::audioSpeed(take);  // pitch unpreserved: faster or slower
    const double framesPerUnit = rate * speed / window.unitsPerSecond;
    const peaks::Level& level = pyramid.pick(framesPerUnit * static_cast<double>(px));
    const float laneH = r.h / static_cast<float>(channels);
    std::size_t columns = 0;
    for (int c = 0; c < channels; ++c) {
        const float mid = r.y + laneH * (static_cast<float>(c) + 0.5f), half = laneH * 0.5f - 1;
        strips.beginRun();
        // Columns by index: an accumulated fractional step would drift off
        // the device pixels.
        const auto first = static_cast<long>(std::floor(vis.x * frame.ratio));
        for (long i = first;; ++i) {
            const float x = static_cast<float>(i) / frame.ratio;
            if (x >= vis.right()) break;
            // The waveform and playback share original-source coordinates.
            double s0 = window.loop ? take.startOffset + window.secondsAt(x) * speed
                                    : timeMap.sourceAt(window.secondsAt(x));
            double s1 = window.loop ? take.startOffset + window.secondsAt(x + px) * speed
                                    : timeMap.sourceAt(window.secondsAt(x + px));
            if (window.loop && sourceSeconds > 0) {
                const double wrap = std::floor(s0 / sourceSeconds) * sourceSeconds;
                s0 -= wrap;
                s1 -= wrap;
            }
            const peaks::Span sp = pyramid.range(level, c, s0 * rate, s1 * rate);
            float top = mid - std::clamp(sp.max * gain, -1.0f, 1.0f) * half;
            float bottom = mid - std::clamp(sp.min * gain, -1.0f, 1.0f) * half;
            if (bottom - top < px) {  // silence still shows a hairline
                const float centre = (top + bottom) / 2;
                top = centre - px / 2;
                bottom = centre + px / 2;
            }
            strips.pair(x, top, bottom, rgba);
            ++columns;
        }
        strips.endRun();
    }
    return columns;
}

std::size_t drawMidiBody(ui::RectRenderer& notes, const ui::Rect& r, const model::Take& take,
                         const model::MidiSource& source, const MidiWindow& window,
                         std::uint32_t rgba, std::uint32_t border) {
    const midi::Sequence& seq = source.sequence;
    if (seq.notes.empty() || r.h < 4) return 0;
    const double ppq = seq.ppq > 0 ? seq.ppq : 960;
    const model::SourceBeatMapping mapping{take.startOffset,
                                           take.playrate > 0 ? take.playrate : 1.0,
                                           static_cast<double>(seq.length) / ppq};
    const bool looping = window.loop && mapping.sourceLength > 0;
    int lo = 127, hi = 0;
    double end = 0;
    bool hasNotes = false;
    for (const midi::Note& n : seq.notes) {
        if (looping && n.start >= seq.length) continue;
        hasNotes = true;
        lo = std::min(lo, static_cast<int>(n.pitch));
        hi = std::max(hi, static_cast<int>(n.pitch));
        end = std::max(end, (static_cast<double>(n.start) + static_cast<double>(n.length)) / ppq /
                                mapping.rate);
    }
    if (!hasNotes) return 0;
    const double tail = std::max(0.0, end - mapping.period());
    const int pitches = hi - lo + 3;  // a free row above and below
    const float noteH = std::max(1.0f, std::min(8.0f, r.h / static_cast<float>(pitches)));
    const float usedH = noteH * static_cast<float>(pitches);
    const float top = r.y + (r.h - usedH) / 2;
    std::size_t glyphs = 0;
    for (const double base :
         mapping.visibleOrigins(window.length, looping, window.from, window.to, tail))
        for (const midi::Note& n : seq.notes) {
            if (looping && n.start >= seq.length) continue;
            const double b0 = base + static_cast<double>(n.start) / ppq / mapping.rate;
            const double b1 = b0 + static_cast<double>(n.length) / ppq / mapping.rate;
            if (b1 <= window.from || b0 >= window.to) continue;
            if (glyphs >= 8192) return glyphs;
            ++glyphs;
            const float x0 = window.xAt(std::max(0.0, b0));
            const float x1 = std::min(window.xAt(b1), window.end);
            const float y = top + static_cast<float>(hi + 1 - n.pitch) * noteH;
            notes.add({x0, y, std::max(1.0f, x1 - x0), noteH}, rgba, border);
        }
    return glyphs;
}

}  // namespace supple::views
