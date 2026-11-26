// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "follow_tables.h"

#include <algorithm>
#include <limits>

#include "supple/model/context.h"
#include "supple/model/follow.h"

namespace supple::engine::detail {
namespace {

// The index of `place` in `places`, added when new.
std::uint16_t indexOf(std::vector<harmony::Place>& places, const harmony::Place& place) {
    for (std::size_t i = 0; i < places.size(); ++i)
        if (places[i].key == place.key && places[i].chord == place.chord)
            return static_cast<std::uint16_t>(i);
    places.push_back(place);
    return static_cast<std::uint16_t>(places.size() - 1);
}

// Regions over the change `beats` (ascending, distinct), each place read by
// `placeAt` (the points holding at or before a beat), `before` before them all.
template <class PlaceAt>
FollowRegions regionsOf(std::vector<double> beats, std::vector<harmony::Place>& places,
                        const harmony::Place& before, const PlaceAt& placeAt) {
    std::sort(beats.begin(), beats.end());
    beats.erase(std::unique(beats.begin(), beats.end()), beats.end());
    FollowRegions r;
    r.place.push_back(indexOf(places, before));
    for (const double b : beats) {
        r.beats.push_back(b);
        r.place.push_back(indexOf(places, placeAt(b)));
    }
    return r;
}

std::optional<ClipFollowTables> tablesFor(const model::Clip& clip, ProjectFollow& project,
                                          bool chords) {
    std::vector<double> beats;
    for (const model::ContextPoint& point : clip.context)
        if (chords || point.type == model::ContextType::Key) beats.push_back(point.beat);
    std::vector<harmony::Place> from;
    ClipFollowTables t;
    t.keyOnly = !chords;
    // As soundingKey reads a context: before the first points, the first key and
    // chord hold.
    const harmony::Place before =
        model::takePlaceAt(clip.context, std::numeric_limits<double>::lowest(), chords);
    t.clip = regionsOf(std::move(beats), from, before,
                       [&](double b) { return model::takePlaceAt(clip.context, b, chords); });
    const auto& to = chords ? project.fullPlaces : project.keyPlaces;
    if (from.size() * to.size() > kFollowPairs) return std::nullopt;
    t.projectPlaces = static_cast<std::uint32_t>(to.size());
    t.rows.resize(from.size() * to.size());
    const auto mode = chords ? harmony::FollowMode::Chords : harmony::FollowMode::Key;
    std::array<std::uint8_t, 128> row{};
    for (std::size_t f = 0; f < from.size(); ++f)
        for (std::size_t k = 0; k < to.size(); ++k) {
            for (int key = 0; key < 128; ++key)
                row[static_cast<std::size_t>(key)] =
                    static_cast<std::uint8_t>(harmony::follow(key, from[f], to[k], mode).pitch);
            const auto next = static_cast<std::uint32_t>(project.pool->size() / 128);
            const auto [at, added] = project.rowIndex.try_emplace(row, next);
            if (added) project.pool->insert(project.pool->end(), row.begin(), row.end());
            t.rows[f * to.size() + k] = at->second;
        }
    t.pool = project.pool;
    return t;
}

}  // namespace

std::uint16_t FollowRegions::at(double beat) const SUPPLE_RT {
    constexpr double kSlack = 1e-6;  // beats: a region starting within rounding of `beat` has begun
    const auto it = std::upper_bound(beats.begin(), beats.end(), beat + kSlack);
    return place[static_cast<std::size_t>(it - beats.begin())];
}

ProjectFollow projectFollow(const model::Session& s) {
    ProjectFollow out;
    const model::ContextView& view = *s.contextView;
    std::vector<double> keyBeats, allBeats;
    for (const auto& e : view.keys) {
        keyBeats.push_back(e.beat);
        allBeats.push_back(e.beat);
    }
    for (const auto& e : view.chords) allBeats.push_back(e.beat);
    // As soundingKey reads the project: nothing holds before its first point.
    constexpr double kBefore = std::numeric_limits<double>::lowest();
    out.full =
        regionsOf(std::move(allBeats), out.fullPlaces, model::projectPlaceAt(view, kBefore, true),
                  [&](double b) { return model::projectPlaceAt(view, b, true); });
    out.keys =
        regionsOf(std::move(keyBeats), out.keyPlaces, model::projectPlaceAt(view, kBefore, false),
                  [&](double b) { return model::projectPlaceAt(view, b, false); });
    return out;
}

ClipFollowBuild buildClipFollow(const model::Clip& clip, bool pitched, ProjectFollow& project) {
    ClipFollowBuild out;
    if (clip.follow == model::ClipFollow::None || clip.kind != model::ClipKind::Midi || !pitched ||
        !model::keyAt(clip, 0))
        return out;
    if (clip.follow == model::ClipFollow::Chords) {
        if (auto t = tablesFor(clip, project, true)) {
            out.outcome = FollowOutcome::Chords;
            out.tables = std::move(t);
            return out;
        }
        out.overBudget = true;
    }
    if (auto t = tablesFor(clip, project, false)) {
        out.outcome = FollowOutcome::Key;
        out.tables = std::move(t);
        return out;
    }
    out.overBudget = true;
    return out;
}

int followedKey(const ClipFollowTables& t, const ProjectFollow& project, double clipBeat,
                double songBeat, int channel, int key) SUPPLE_RT {
    if (channel == 9 || key < 0 || key > 127) return key;
    const FollowRegions& to = t.keyOnly ? project.keys : project.full;
    const std::size_t pair = std::size_t{t.clip.at(clipBeat)} * t.projectPlaces + to.at(songBeat);
    return (*t.pool)[std::size_t{t.rows[pair]} * 128 + static_cast<std::size_t>(key)];
}

}  // namespace supple::engine::detail
