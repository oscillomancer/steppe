// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "clip_follow.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "heard_midi.h"
#include "supple/harmony/harmony.h"
#include "supple/model/context.h"
#include "supple/model/source_mapping.h"

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
        .field("unrolled", &FollowedInfo::unrolled, schema::boolean())
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

// One heard pass of a note: where it sounds (item beats) and what it maps to.
struct Occurrence {
    int pass = 0;
    double on = 0, off = 0;  // item beats; `on` is 0 for a note held into the item's start
    double writtenOn = 0;    // where the note began, before that clamp (negative when held in)
    bool heldIn = false;
    harmony::Followed followed;
    std::optional<harmony::Key> fromKey, toKey;
    int bar = 1;
};

}  // namespace

Result<FollowedInfo> followPlacedItem(const model::Session& s, model::EditTxn& t, model::Id clipId,
                                      const model::Clip& clip, model::Id itemId,
                                      std::optional<model::ClipFollow> requested, bool pooled,
                                      const std::string& path, std::vector<ClipNote>& notes) {
    const model::Session now = t.preview();
    const model::Item* itemPtr = now.items.find(itemId);
    FollowedInfo info;
    if (!itemPtr) return info;
    const model::Item item = *itemPtr;
    const model::ItemSpan span = model::itemSpan(now, item);
    const double itemStart = span.startBeat;
    const double itemBeats = model::itemLengthBeats(now, item);
    const model::ClipFollow wanted = requested.value_or(clip.follow);

    // Placed as it is: the mismatch warning says when the clip's key is not the project's.
    const auto placeAsIs = [&](const char* copiedBecause) {
        info = FollowedInfo{};
        info.mode = "none";
        if (copiedBecause) info.copied = copiedBecause;
        if (auto note = mismatchNote(s, clipId, clip, itemStart, itemBeats, 0.0, path))
            notes.push_back(std::move(*note));
        return info;
    };
    if (wanted == model::ClipFollow::None) return placeAsIs(nullptr);
    if (clip.kind != model::ClipKind::Midi) return placeAsIs("audio");
    if (!model::keyAt(clip, 0)) return placeAsIs("no_key");
    const model::Take* take = item.activeTake ? now.takes.find(*item.activeTake) : nullptr;
    const model::MidiSource* source =
        take && take->source && take->source->kind == model::Kind::Midi
            ? now.midi.find(*take->source)
            : nullptr;
    if (!source || source->sequence.notes.empty()) return placeAsIs("no_notes");
    const midi::Sequence& seq = source->sequence;
    if (std::all_of(seq.notes.begin(), seq.notes.end(),
                    [](const midi::Note& n) { return n.channel == 9; }))
        return placeAsIs("drums");

    const harmony::FollowMode mode = wanted == model::ClipFollow::Chords
                                         ? harmony::FollowMode::Chords
                                         : harmony::FollowMode::Key;
    info.mode = mode == harmony::FollowMode::Chords ? "chords" : "key";

    // The passes in which each note is heard, as heardMidi schedules them.
    const double rate = take->playrate > 0 ? take->playrate : 1.0;
    const double loopBeats = seq.ppq > 0 ? static_cast<double>(seq.length) / seq.ppq : 0.0;
    const bool looping = item.loop && loopBeats > 0;
    const model::SourceBeatMapping mapping{take->startOffset, rate, loopBeats};
    const int reps = looping
                         ? 1 + static_cast<int>(std::floor(
                                   (itemBeats * rate + mapping.sourceOffset(looping)) / loopBeats))
                         : 1;
    const auto beatOf = [&](std::int64_t tick, int pass) {
        return mapping.itemBeat(static_cast<double>(tick) / seq.ppq, pass, looping);
    };
    const auto occurrencesOf = [&](const midi::Note& n) {
        std::vector<Occurrence> out;
        if (looping && n.start >= seq.length) return out;
        for (int k = looping ? -1 : 0; k < reps; ++k) {
            double on = beatOf(n.start, k);
            const double off = std::min(itemBeats, beatOf(n.start + n.length, k));
            if (on >= itemBeats) continue;
            const double written = on;
            const bool heldIn = on < 0;
            if (heldIn) {
                if (off <= 0) continue;
                on = 0;  // held into the item's start
            }
            Occurrence o;
            o.pass = k;
            o.on = on;
            o.off = off;
            o.writtenOn = written;
            o.heldIn = heldIn;
            out.push_back(o);
        }
        return out;
    };
    const auto mapOccurrence = [&](const midi::Note& n, Occurrence& o) {
        const double place = itemStart + o.on;
        // The clip beat the note was written at: for a note held into the item's start,
        // where it began (in the clip's previous pass), not where it sounds from.
        double inClip = clip.length > 0 ? std::fmod(o.writtenOn, clip.length) : 0.0;
        if (inClip < 0) inClip += clip.length;
        harmony::Place from{model::keyAt(clip, inClip), model::chordAt(clip, inClip)};
        harmony::Place to{model::keyAt(s, place), model::chordAt(s, place)};
        o.fromKey = from.key;
        o.toKey = to.key;
        o.bar = s.timeMap->barBeat(place).bar;
        o.followed = harmony::follow(n.pitch, from, to, mode);
    };

    // The occurrence whose pitch the note keeps in a looped item: the first whole pass, else
    // the held-in fragment when that is all there is.
    const auto lead = [](const std::vector<Occurrence>& h) -> const Occurrence& {
        for (const Occurrence& o : h)
            if (!o.heldIn) return o;
        return h.front();
    };
    // Map every heard pass of every pitched note.
    std::vector<std::vector<Occurrence>> heard(seq.notes.size());
    bool moved = false, alike = true;
    std::int64_t shaped = 0, copiedNoKey = 0;
    std::optional<int> firstNoKeyBar, firstShapeBar;
    std::optional<std::pair<harmony::Key, harmony::Key>> shapeKeys;
    for (std::size_t i = 0; i < seq.notes.size(); ++i) {
        const midi::Note& n = seq.notes[i];
        if (n.channel == 9) continue;
        heard[i] = occurrencesOf(n);
        for (Occurrence& o : heard[i]) mapOccurrence(n, o);
        for (const Occurrence& o : heard[i]) {
            moved |= o.followed.pitch != n.pitch;
            // A fragment held into the item's start does not decide whether every pass maps
            // alike (it plays the note's own pitch); a note heard only so is its own case.
            alike &= o.heldIn || o.followed.pitch == lead(heard[i]).followed.pitch;
            if (o.followed.by == harmony::FollowedBy::Copy) {
                if (!firstNoKeyBar) firstNoKeyBar = o.bar;
                ++copiedNoKey;
            }
            if (o.followed.scaleShape) {
                if (!firstShapeBar) {
                    firstShapeBar = o.bar;
                    if (o.fromKey && o.toKey) shapeKeys = std::make_pair(*o.fromKey, *o.toKey);
                }
                ++shaped;
            }
        }
    }
    if (pooled && moved)
        return Error("clip.follow_pooled",
                     "the clip's notes would move to " +
                         std::string(info.mode == "chords" ? "the chords" : "the key") +
                         " here, and a pooled item shares the clip's MIDI: one source cannot be "
                         "two keys",
                     "/pooled",
                     "leave pooled off (the item gets its own MIDI), or place the clip with "
                     "follow: \"none\"");

    // Regions: maximal runs of bars over which the keys and the way notes were mapped agree.
    using Way = std::tuple<std::string, std::string, std::string, std::int64_t, bool>;
    std::map<Way, std::set<int>> bars;
    std::map<Way, harmony::Followed> sample;
    for (std::size_t i = 0; i < seq.notes.size(); ++i)
        for (const Occurrence& o : heard[i]) {
            const bool transpose =
                o.followed.by == harmony::FollowedBy::Transpose && o.fromKey && o.toKey;
            const std::int64_t interval =
                transpose ? harmony::tonicInterval(o.fromKey->tonic, o.toKey->tonic) : 0;
            const Way way{byName(o.followed.by), o.fromKey ? keyText(*o.fromKey) : std::string(),
                          o.toKey ? keyText(*o.toKey) : std::string(), interval, transpose};
            bars[way].insert(o.bar);
        }
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
            if (!run.empty() && bar != run.back() + 1) flush();
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
                             ": they are copied, not mapped (write a key with context.write or "
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

    std::int64_t folded = 0;
    if (alike) {
        // The item stays as it is, over the mapped source.
        if (moved) {
            t.modify<model::MidiSource>(*take->source, [&](model::MidiSource& m) {
                for (std::size_t i = 0; i < m.sequence.notes.size() && i < heard.size(); ++i)
                    if (!heard[i].empty())
                        m.sequence.notes[i].pitch =
                            static_cast<std::uint8_t>(lead(heard[i]).followed.pitch);
                m.sequence.canonicalize();
            });
        }
        for (const auto& h : heard)
            if (!h.empty() && lead(h).followed.folded) ++folded;
    } else {
        // A pass maps differently from another: the item is laid out pass by pass, each
        // note at its place, with the loop off.
        midi::Sequence out;
        out.ppq = seq.ppq;
        out.length = std::llround(itemBeats * seq.ppq);
        const auto tick = [&](double beat) { return std::llround(beat * seq.ppq); };
        std::vector<std::pair<std::size_t, Occurrence>> order;
        for (std::size_t i = 0; i < seq.notes.size(); ++i) {
            const midi::Note& n = seq.notes[i];
            std::vector<Occurrence> list = n.channel == 9 ? occurrencesOf(n) : heard[i];
            for (Occurrence& o : list) order.emplace_back(i, o);
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.second.pass < b.second.pass;
        });
        for (const auto& [i, o] : order) {
            midi::Note note = seq.notes[i];
            if (note.channel != 9) {
                note.pitch = static_cast<std::uint8_t>(o.followed.pitch);
                if (o.followed.folded) ++folded;
            }
            note.start = tick(o.on);
            note.length = std::max<std::int64_t>(0, tick(o.off) - note.start);
            out.notes.push_back(note);
        }
        for (int k = looping ? -1 : 0; k < reps; ++k) {
            for (const midi::Control& c : seq.controls) {
                const double at = beatOf(c.tick, k);
                if (at < 0 || at >= itemBeats) continue;
                midi::Control control = c;
                control.tick = tick(at);
                out.controls.push_back(control);
            }
            for (const midi::Sysex& x : seq.sysex) {
                const double at = beatOf(x.tick, k);
                if (at < 0 || at >= itemBeats) continue;
                midi::Sysex sysex = x;
                sysex.tick = tick(at);
                out.sysex.push_back(std::move(sysex));
            }
        }
        out.canonicalize();
        t.modify<model::MidiSource>(*take->source,
                                    [&](model::MidiSource& m) { m.sequence = std::move(out); });
        t.modify<model::Take>(*item.activeTake, [](model::Take& k) {
            k.startOffset = 0;
            k.playrate = 1.0;
        });
        // The item's other takes are laid out alike, by plain repetition, so that every take
        // keeps the item's length and loop state.
        for (const model::Id other : item.takes) {
            if (other == *item.activeTake) continue;
            const model::Take* ot = now.takes.find(other);
            const model::MidiSource* os = ot && ot->source && ot->source->kind == model::Kind::Midi
                                              ? now.midi.find(*ot->source)
                                              : nullptr;
            if (!os) continue;
            midi::Sequence plain;
            plain.ppq = os->sequence.ppq;
            heardMidi(now, item, *ot, os->sequence, 0, plain);
            plain.length = std::llround(itemBeats * plain.ppq);
            plain.canonicalize();
            t.modify<model::MidiSource>(
                *ot->source, [&](model::MidiSource& m) { m.sequence = std::move(plain); });
            t.modify<model::Take>(other, [](model::Take& k) {
                k.startOffset = 0;
                k.playrate = 1.0;
            });
        }
        t.modify<model::Item>(itemId, [](model::Item& it) { it.loop = false; });
        info.unrolled = true;
    }
    if (folded > 0) info.folded = folded;
    return info;
}

}  // namespace supple::engine::detail
