// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "clip_follow.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "supple/harmony/harmony.h"
#include "supple/model/context.h"
#include "supple/model/follow.h"

namespace supple::engine::detail {

schema::Schema<FollowedInfo> followedSchema() {
    auto region = schema::object<FollowRegion>("followed_region")
                      .field("from_bar", &FollowRegion::fromBar,
                             schema::integer().doc("First project bar of the run."))
                      .field("to_bar", &FollowRegion::toBar,
                             schema::integer().doc("Last project bar of the run (inclusive)."))
                      .field("from_key", &FollowRegion::fromKey, schema::string())
                      .field("to_key", &FollowRegion::toKey, schema::string())
                      .field("by", &FollowRegion::by,
                             schema::enumeration<std::string>({{"transpose", "transpose"},
                                                               {"degree", "degree"},
                                                               {"chords", "chords"},
                                                               {"copy", "copy"}}))
                      .field("interval", &FollowRegion::interval,
                             schema::integer().doc("Semitones, for transpose."))
                      .build();
    return schema::object<FollowedInfo>("followed")
        .field("mode", &FollowedInfo::mode,
               schema::enumeration<std::string>(
                   {{"none", "none"}, {"key", "key"}, {"chords", "chords"}}))
        .field("regions", &FollowedInfo::regions, schema::arrayOf(region))
        .field("folded", &FollowedInfo::folded, schema::integer())
        .field("copied", &FollowedInfo::copied,
               schema::enumeration<std::string>({{"drums", "drums"},
                                                 {"audio", "audio"},
                                                 {"no_key", "no_key"},
                                                 {"no_notes", "no_notes"}}))
        .build();
}

schema::Schema<model::ClipFollow> followArgSchema() {
    return schema::enumeration<model::ClipFollow>({{model::ClipFollow::None, "none"},
                                                   {model::ClipFollow::Key, "key"},
                                                   {model::ClipFollow::Chords, "chords"}})
        .doc(
            "How the clip's notes are mapped to the key (or chords) under the item (ADR-0098): "
            "none, key or chords. Default: the clip's own follow.");
}

namespace {

std::string keyText(const harmony::Key& key) {
    std::string scale(harmony::scaleName(key.scale));
    std::replace(scale.begin(), scale.end(), '_', ' ');
    return harmony::printPitchClass(key.tonic) + " " + scale;
}

const char* byName(harmony::FollowedBy by) {
    switch (by) {
        case harmony::FollowedBy::Transpose:
            return "transpose";
        case harmony::FollowedBy::Degree:
            return "degree";
        case harmony::FollowedBy::Chords:
            return "chords";
        case harmony::FollowedBy::Copy:
            break;
    }
    return "copy";
}

// One heard note occurrence, with the places it was mapped between.
struct Occurrence {
    std::uint32_t note = 0;
    int bar = 1;  // the project bar where it is heard from (a chased note: the item's start)
    harmony::Followed followed;
    std::optional<harmony::Key> fromKey, toKey;
};

}  // namespace

Result<FollowedInfo> followPlacedItem(const model::Session& s, model::EditTxn& t, model::Id clipId,
                                      const model::Clip& clip, model::Id itemId,
                                      std::optional<model::ClipFollow> requested,
                                      const std::string& path, std::vector<ClipNote>& notes) {
    model::Session now = t.preview();
    const model::Item* itemPtr = now.items.find(itemId);
    FollowedInfo info;
    if (!itemPtr) return info;
    const model::Item item = *itemPtr;
    const model::ItemSpan span = model::itemSpan(now, item);
    const double itemStart = span.startBeat;
    const double itemBeats = model::itemLengthBeats(now, item);
    const model::ClipFollow wanted = requested.value_or(clip.follow);

    // What the laid takes follow, and when a clip that cannot follow is copied, why.
    const model::Take* active = item.activeTake ? now.takes.find(*item.activeTake) : nullptr;
    const model::MidiSource* source =
        active && active->source && active->source->kind == model::Kind::Midi
            ? now.midi.find(*active->source)
            : nullptr;
    const char* copiedBecause = nullptr;
    model::ClipFollow outcome = wanted;
    if (wanted == model::ClipFollow::None) {
        outcome = model::ClipFollow::None;
    } else if (clip.kind != model::ClipKind::Midi) {
        copiedBecause = "audio";
    } else if (!model::keyAt(clip, 0)) {
        copiedBecause = "no_key";
    } else if (!source || source->sequence.notes.empty()) {
        copiedBecause = "no_notes";
    } else if (std::all_of(source->sequence.notes.begin(), source->sequence.notes.end(),
                           [](const midi::Note& n) { return n.channel == 9; })) {
        copiedBecause = "drums";
    }
    if (copiedBecause) outcome = model::ClipFollow::None;

    // Every laid MIDI take carries the clip's context and the outcome; the notes stay as
    // written (ADR-0098 "following is playback"). A clip with no context leaves them be.
    for (const model::Id takeId : item.takes) {
        const model::Take* take = now.takes.find(takeId);
        if (!take || !model::takeIsMidi(*take)) continue;
        t.modify<model::Take>(takeId, [&](model::Take& k) {
            model::takeContextFromClip(k, clip, 0.0, outcome);
        });
    }

    // Placed as it is: the mismatch warning says when the clip's key is not the project's.
    if (outcome == model::ClipFollow::None) {
        info.mode = "none";
        if (copiedBecause) info.copied = copiedBecause;
        if (auto note = mismatchNote(s, clipId, clip, itemStart, itemBeats, 0.0, path))
            notes.push_back(std::move(*note));
        return info;
    }
    info.mode = outcome == model::ClipFollow::Chords ? "chords" : "key";

    // What it sounds as, from the one helper every reader uses.
    now = t.preview();
    const model::Take& laid = *now.takes.find(*item.activeTake);
    const auto heard = model::soundingNotes(now, itemId, -1e18, 1e18);
    std::vector<Occurrence> occurrences;
    std::map<std::uint32_t, bool> foldedNotes;
    std::int64_t shaped = 0, copiedNoKey = 0;
    std::optional<int> firstNoKeyBar, firstShapeBar;
    std::optional<std::pair<harmony::Key, harmony::Key>> shapeKeys;
    for (const model::SoundingNote& n : heard) {
        if (n.channel == 9) continue;
        Occurrence o;
        o.note = n.note;
        o.followed = n.sounds;
        o.fromKey = model::takePlaceAt(laid.context, n.contextBeat, false).key;
        o.toKey = model::projectPlaceAt(*now.contextView, n.songBeat, false).key;
        o.bar = now.timeMap->barBeat(std::max(n.songBeat, itemStart)).bar;
        if (n.sounds.by == harmony::FollowedBy::Copy) {
            if (!firstNoKeyBar || o.bar < *firstNoKeyBar) firstNoKeyBar = o.bar;
            ++copiedNoKey;
        }
        if (n.sounds.scaleShape) {
            if (!firstShapeBar || o.bar < *firstShapeBar) {
                firstShapeBar = o.bar;
                if (o.fromKey && o.toKey) shapeKeys = std::make_pair(*o.fromKey, *o.toKey);
            }
            ++shaped;
        }
        if (n.sounds.folded) foldedNotes[n.note] = true;
        occurrences.push_back(std::move(o));
    }

    // Regions: maximal runs of bars over which the keys and the way notes were mapped agree.
    using Way = std::tuple<std::string, std::string, std::string, std::int64_t, bool>;
    std::map<Way, std::set<int>> bars;
    for (const Occurrence& o : occurrences) {
        const bool transpose =
            o.followed.by == harmony::FollowedBy::Transpose && o.fromKey && o.toKey;
        const std::int64_t interval =
            transpose ? harmony::tonicInterval(o.fromKey->tonic, o.toKey->tonic) : 0;
        const Way way{byName(o.followed.by), o.fromKey ? keyText(*o.fromKey) : std::string(),
                      o.toKey ? keyText(*o.toKey) : std::string(), interval, transpose};
        bars[way].insert(o.bar);
    }
    // A bar where no note starts (a rest, or a note held over it) does not end a run.
    std::set<int> onsets;
    for (const auto& entry : bars) onsets.insert(entry.second.begin(), entry.second.end());
    std::vector<FollowRegion> regions;
    for (const auto& [way, set] : bars) {
        std::vector<int> run;
        const auto flush = [&] {
            if (run.empty()) return;
            FollowRegion r;
            r.fromBar = run.front();
            r.toBar = run.back();
            r.by = std::get<0>(way);
            if (!std::get<1>(way).empty()) r.fromKey = std::get<1>(way);
            if (!std::get<2>(way).empty()) r.toKey = std::get<2>(way);
            if (std::get<4>(way)) r.interval = std::get<3>(way);
            regions.push_back(std::move(r));
            run.clear();
        };
        for (const int bar : set) {
            if (!run.empty()) {
                bool gap = false;  // a bar between the run and this one holds a note's start
                for (int b = run.back() + 1; b < bar && !gap; ++b) gap = onsets.count(b) > 0;
                if (gap) flush();
            }
            run.push_back(bar);
        }
        flush();
    }
    std::sort(regions.begin(), regions.end(), [](const FollowRegion& a, const FollowRegion& b) {
        return std::tie(a.fromBar, a.by) < std::tie(b.fromBar, b.by);
    });
    if (!regions.empty()) info.regions = std::move(regions);

    if (copiedNoKey > 0)
        notes.push_back({"context.no_key",
                         "no key holds under " + std::to_string(copiedNoKey) + " note" +
                             (copiedNoKey == 1 ? "" : "s") + " from bar " +
                             std::to_string(*firstNoKeyBar) +
                             ": they sound as written (write a key with context.write or "
                             "key.set)",
                         path});
    if (shaped > 0)
        notes.push_back(
            {"context.scale_shape",
             "the clip's " + (shapeKeys ? keyText(shapeKeys->first) : std::string("key")) +
                 " and the project's " +
                 (shapeKeys ? keyText(shapeKeys->second) : std::string("key")) + " (from bar " +
                 std::to_string(*firstShapeBar) +
                 ") are scales of different sizes: the notes there are transposed by the tonic "
                 "interval, not mapped by degree",
             path});
    if (!foldedNotes.empty()) info.folded = static_cast<std::int64_t>(foldedNotes.size());
    return info;
}

}  // namespace supple::engine::detail
