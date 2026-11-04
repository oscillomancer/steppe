// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/audio_warp.h"

#include <algorithm>
#include <cmath>

namespace supple::model {
double audioSpeed(const Take& t) {
    return t.playrate * (t.preservePitch ? 1.0 : std::pow(2.0, t.pitch / 12.0));
}
Status validateWarpMap(const Take& t, std::string path) {
    auto fail = [&](std::size_t i) -> Status {
        return Error("warp.map", "stretch anchors must increase in source and item time",
                     path + "/stretch_markers/" + std::to_string(i),
                     "use finite, ordered anchors with segment rates between 0.01 and 100");
    };
    if (t.stretchMarkers.size() > kMaxAudioMarkers) return fail(kMaxAudioMarkers);
    for (std::size_t i = 0; i < t.stretchMarkers.size(); ++i) {
        const auto& m = t.stretchMarkers[i];
        if (!std::isfinite(m.sourcePos) || !std::isfinite(m.itemPos) || !std::isfinite(m.slope) ||
            std::abs(m.sourcePos) > 1e9 || std::abs(m.itemPos) > 1e9)
            return fail(i);
        if (i) {
            const auto& before = t.stretchMarkers[i - 1];
            const double delta = m.itemPos - before.itemPos;
            const double rate = (m.sourcePos - before.sourcePos) / delta;
            if (!(delta > 0 && rate >= 0.01 && rate <= 100)) return fail(i);
        }
    }
    return {};
}
bool activeWarp(const Take& t) {
    return !t.stretchMarkers.empty() && t.preservePitch && t.pitchMode == "signalsmith" &&
           static_cast<bool>(validateWarpMap(t));
}
namespace {
double map(const Take& t, double x, bool inverse, bool active) {
    const double rate = inverse ? 1 / audioSpeed(t) : audioSpeed(t);
    if (!active) return inverse ? (x - t.startOffset) * rate : t.startOffset + x * rate;
    const auto input = [inverse](const StretchMarker& m) {
        return inverse ? m.sourcePos : m.itemPos;
    };
    const auto output = [inverse](const StretchMarker& m) {
        return inverse ? m.itemPos : m.sourcePos;
    };
    auto after = std::upper_bound(t.stretchMarkers.begin(), t.stretchMarkers.end(), x,
                                  [&](double value, const auto& m) { return value < input(m); });
    if (after == t.stretchMarkers.begin()) return output(*after) + (x - input(*after)) * rate;
    const auto& before = *std::prev(after);
    if (after == t.stretchMarkers.end()) return output(before) + (x - input(before)) * rate;
    const double f = (x - input(before)) / (input(*after) - input(before));
    return output(before) + f * (output(*after) - output(before));
}
}  // namespace
double AudioWarpMap::sourceAt(double seconds) const { return map(take_, seconds, false, active_); }
double AudioWarpMap::itemAt(double seconds) const { return map(take_, seconds, true, active_); }
double audioSourceAt(const Take& t, double seconds) { return AudioWarpMap(t).sourceAt(seconds); }
double audioItemAt(const Take& t, double seconds) { return AudioWarpMap(t).itemAt(seconds); }
void rebaseAudioTake(Take& t, double seconds) {
    t.startOffset = audioSourceAt(t, seconds);
    for (auto& m : t.stretchMarkers) m.itemPos -= seconds;
}
void slipAudioTake(Take& t, double delta) {
    t.startOffset += delta;
    for (auto& m : t.stretchMarkers) m.sourcePos += delta;
}
void rateAudioTake(Take& t, double rate) {
    for (auto& m : t.stretchMarkers) m.itemPos *= t.playrate / rate;
    t.playrate = rate;
}
Status validateAudioWarp(const Session& s, bool bundles) {
    for (const auto& e : s.takes) {
        const auto& t = *e.value;
        const std::string path = "/takes/" + e.id.str();
        if ((t.transients || !t.warpPeers.empty()) &&
            (!t.source || t.source->kind != Kind::Media || !s.media.contains(*t.source)))
            return Error("audio.source", "audio timing data needs an audio source",
                         path + "/source", "use an audio take or remove its audio timing data");
        if (t.transients) {
            const auto& h = *t.transients;
            if (!std::isfinite(h.sensitivity) || h.sensitivity < 0 || h.sensitivity > 1 ||
                h.positions.size() > kMaxAudioMarkers)
                return Error("audio.hits", "invalid transient analysis", path + "/transients",
                             "use sensitivity 0..1 and at most 65536 hits");
            double last = -1;
            for (double p : h.positions) {
                if (!std::isfinite(p) || p < 0 || p <= last)
                    return Error("audio.hits", "hit positions must be finite and increasing",
                                 path + "/transients/positions",
                                 "use unique source seconds in increasing order");
                last = p;
            }
        }
        if (!bundles || t.warpPeers.empty()) continue;
        if (t.warpPeers.size() > 128 || !std::is_sorted(t.warpPeers.begin(), t.warpPeers.end()) ||
            std::adjacent_find(t.warpPeers.begin(), t.warpPeers.end()) != t.warpPeers.end() ||
            !std::binary_search(t.warpPeers.begin(), t.warpPeers.end(), e.id))
            return Error("warp.peers", "invalid warp bundle", path + "/warp_peers",
                         "use audio.quantize to create a reciprocal bundle including this take");
        const Item* owner = nullptr;
        for (const auto& it : s.items)
            if (std::find(it.value->takes.begin(), it.value->takes.end(), e.id) !=
                it.value->takes.end())
                owner = it.value.get();
        for (auto id : t.warpPeers) {
            const auto* peer = s.takes.find(id);
            const Item* other = nullptr;
            for (const auto& it : s.items)
                if (std::find(it.value->takes.begin(), it.value->takes.end(), id) !=
                    it.value->takes.end())
                    other = it.value.get();
            bool match = peer && owner && other && !owner->loop && !other->loop && activeWarp(t) &&
                         activeWarp(*peer) && peer->warpPeers == t.warpPeers &&
                         peer->playrate == t.playrate && peer->pitch == t.pitch &&
                         peer->stretchMarkers.size() == t.stretchMarkers.size();
            if (match) {
                const auto a = itemSpan(s, *owner), b = itemSpan(s, *other);
                match = a.startSec == b.startSec && a.endSec == b.endSec;
                for (std::size_t i = 0; i < t.stretchMarkers.size(); ++i) {
                    const auto& x = t.stretchMarkers[i];
                    const auto& y = peer->stretchMarkers[i];
                    match = match && x.itemPos == y.itemPos &&
                            std::abs((x.sourcePos - t.startOffset) -
                                     (y.sourcePos - peer->startOffset)) < 1e-9;
                }
            }
            if (!match)
                return Error("warp.peers", "warp peers require aligned spans and a shared map",
                             path + "/warp_peers",
                             "edit all peers together or use audio.warp.detach first");
        }
    }
    return {};
}
}  // namespace supple::model
