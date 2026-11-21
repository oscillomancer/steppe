// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "follow_tables.h"

#include <algorithm>

#include "supple/model/context.h"

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
// `placeAt` (the point holding at or before a beat), the first before them all.
template <class PlaceAt>
FollowRegions regionsOf(std::vector<double> beats, std::vector<harmony::Place>& places,
                        const PlaceAt& placeAt) {
    std::sort(beats.begin(), beats.end());
    beats.erase(std::unique(beats.begin(), beats.end()), beats.end());
    FollowRegions r;
    r.place.push_back(indexOf(places, harmony::Place{}));  // before the first point
    for (const double b : beats) {
        r.beats.push_back(b);
        r.place.push_back(indexOf(places, placeAt(b)));
    }
    return r;
}

harmony::Place keyOnly(harmony::Place p) {
    p.chord.reset();
    return p;
}

std::optional<ClipFollowTables> tablesFor(const model::Clip& clip, const ProjectFollow& project,
                                          bool chords) {
    std::vector<double> beats;
    for (const model::ContextPoint& point : clip.context)
        if (chords || point.type == model::ContextType::Key) beats.push_back(point.beat);
    std::vector<harmony::Place> from;
    ClipFollowTables t;
    t.keyOnly = !chords;
    t.clip = regionsOf(std::move(beats), from, [&](double b) {
        harmony::Place p{model::keyAt(clip, b), model::chordAt(clip, b)};
        return chords ? p : keyOnly(p);
    });
    const auto& to = chords ? project.fullPlaces : project.keyPlaces;
    if (from.size() * to.size() > kFollowPairs) return std::nullopt;
    t.projectPlaces = static_cast<std::uint32_t>(to.size());
    t.pitch.resize(from.size() * to.size() * 128);
    const auto mode = chords ? harmony::FollowMode::Chords : harmony::FollowMode::Key;
    for (std::size_t f = 0; f < from.size(); ++f)
        for (std::size_t k = 0; k < to.size(); ++k)
            for (int key = 0; key < 128; ++key)
                t.pitch[(f * to.size() + k) * 128 + static_cast<std::size_t>(key)] =
                    static_cast<std::uint8_t>(harmony::follow(key, from[f], to[k], mode).pitch);
    return t;
}

}  // namespace

std::uint16_t FollowRegions::at(double beat) const SUPPLE_RT {
    constexpr double kSlack = 1e-6;  // beats, as the players' event search
    const auto it = std::upper_bound(beats.begin(), beats.end(), beat + kSlack);
    return place[static_cast<std::size_t>(it - beats.begin())];
}

ProjectFollow projectFollow(const model::Session& s) {
    ProjectFollow out;
    std::vector<double> keyBeats, allBeats;
    for (const auto& e : s.contextView->keys) {
        keyBeats.push_back(e.beat);
        allBeats.push_back(e.beat);
    }
    for (const auto& e : s.contextView->chords) allBeats.push_back(e.beat);
    out.full = regionsOf(std::move(allBeats), out.fullPlaces, [&](double b) {
        return harmony::Place{model::keyAt(s, b), model::chordAt(s, b)};
    });
    out.keys = regionsOf(std::move(keyBeats), out.keyPlaces,
                         [&](double b) { return harmony::Place{model::keyAt(s, b), {}}; });
    return out;
}

ClipFollowBuild buildClipFollow(const model::Clip& clip, bool pitched,
                                const ProjectFollow& project) {
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
    return t.pitch[pair * 128 + static_cast<std::size_t>(key)];
}

}  // namespace supple::engine::detail
