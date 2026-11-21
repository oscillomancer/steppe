// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/context.h"

#include <algorithm>

#include "supple/model/session.h"

namespace supple::model {

ContextView buildContextView(const IdMap<ContextPoint>& points) {
    ContextView view;
    view.source = points;
    for (const auto& e : points) {
        auto& list = e.value->type == ContextType::Key ? view.keys : view.chords;
        list.push_back({e.value->beat, e.id});
    }
    for (auto* list : {&view.keys, &view.chords}) {
        // By beat, then id, so a duplicate beat keeps the lowest id.
        std::sort(list->begin(), list->end(), [](const auto& a, const auto& b) {
            return a.beat != b.beat ? a.beat < b.beat : a.id < b.id;
        });
        list->erase(std::unique(list->begin(), list->end(),
                                [](const auto& a, const auto& b) { return a.beat == b.beat; }),
                    list->end());
    }
    return view;
}

namespace {
std::optional<Id> holding(const std::vector<ContextView::Entry>& list, double beat) {
    auto it = std::upper_bound(list.begin(), list.end(), beat,
                               [](double b, const ContextView::Entry& e) { return b < e.beat; });
    if (it == list.begin()) return std::nullopt;
    return (it - 1)->id;
}
}  // namespace

std::optional<Id> keyPointAt(const Session& s, double beat) {
    return holding(s.contextView->keys, beat);
}

std::optional<Id> chordPointAt(const Session& s, double beat) {
    return holding(s.contextView->chords, beat);
}

std::optional<harmony::Key> parseKey(const ContextPoint& point) {
    const auto tonic = harmony::parsePitchClass(point.tonic);
    const auto scale = harmony::parseScale(point.scale);
    if (point.type != ContextType::Key || !tonic || !scale) return std::nullopt;
    return harmony::Key{*tonic, *scale};
}

std::optional<harmony::Chord> parseChordPoint(const ContextPoint& point) {
    if (point.type != ContextType::Chord || point.symbol.empty()) return std::nullopt;
    auto chord = harmony::parseChord(point.symbol);
    if (!chord) return std::nullopt;
    return *chord;
}

std::string keyWords(const ContextPoint& p) {
    std::string scale = p.scale;
    std::replace(scale.begin(), scale.end(), '_', ' ');
    return p.tonic + " " + scale + (parseKey(p) ? "" : "?");
}

std::optional<harmony::Key> keyAt(const Session& s, double beat) {
    const auto id = keyPointAt(s, beat);
    const ContextPoint* p = id ? s.context.find(*id) : nullptr;
    return p ? parseKey(*p) : std::nullopt;
}

std::optional<harmony::Chord> chordAt(const Session& s, double beat) {
    const auto id = chordPointAt(s, beat);
    const ContextPoint* p = id ? s.context.find(*id) : nullptr;
    return p ? parseChordPoint(*p) : std::nullopt;
}

namespace {
const ContextPoint* clipPointAt(const Clip& clip, ContextType type, double beat) {
    const ContextPoint* held = nullptr;
    for (const ContextPoint& p : clip.context)
        if (p.type == type && p.beat <= beat && (!held || p.beat >= held->beat)) held = &p;
    return held;
}
}  // namespace

const ContextPoint* keyPointAt(const Clip& clip, double beat) {
    return clipPointAt(clip, ContextType::Key, beat);
}
const ContextPoint* chordPointAt(const Clip& clip, double beat) {
    return clipPointAt(clip, ContextType::Chord, beat);
}
std::optional<harmony::Key> keyAt(const Clip& clip, double beat) {
    const ContextPoint* p = keyPointAt(clip, beat);
    return p ? parseKey(*p) : std::nullopt;
}
std::optional<harmony::Chord> chordAt(const Clip& clip, double beat) {
    const ContextPoint* p = chordPointAt(clip, beat);
    return p ? parseChordPoint(*p) : std::nullopt;
}

std::optional<Id> sectionAt(const Session& s, double beat) {
    std::optional<Id> best;
    double bestStart = 0, bestEnd = 0;
    for (const auto& e : s.regions) {
        const Region& r = *e.value;
        if (!r.section) continue;
        const double start = r.timebase == Timebase::Time ? s.timeMap->beatAt(r.start) : r.start;
        const double end = r.timebase == Timebase::Time ? s.timeMap->beatAt(r.end) : r.end;
        if (!(start <= beat && beat < end)) continue;
        // Latest start; then the shorter; then the lower id (the map is in id order,
        // so the first of equals stays).
        const bool better =
            !best || start > bestStart || (start == bestStart && end - start < bestEnd - bestStart);
        if (better) best = e.id, bestStart = start, bestEnd = end;
    }
    return best;
}

}  // namespace supple::model
