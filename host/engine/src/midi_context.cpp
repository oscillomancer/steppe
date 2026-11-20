// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "midi_context.h"

#include <algorithm>
#include <cmath>

#include "supple/model/context.h"
#include "supple/model/source_mapping.h"

namespace supple::engine::detail {

std::vector<double> soundingBeats(const model::Session& s, model::Id source, std::int64_t tick) {
    std::vector<double> beats;
    const model::MidiSource* midi = s.midi.find(source);
    if (!midi || midi->sequence.ppq <= 0) return beats;
    const midi::Sequence& seq = midi->sequence;
    const double sourceBeat = static_cast<double>(tick) / seq.ppq;
    const double loopBeats = static_cast<double>(seq.length) / seq.ppq;
    for (const auto& entry : s.items) {
        const model::Item& item = *entry.value;
        const model::Take* take = item.activeTake ? s.takes.find(*item.activeTake) : nullptr;
        if (!take || !take->source || *take->source != source) continue;
        const model::ItemSpan span = model::itemSpan(s, item);
        const double itemBeats = s.timeMap->beatAt(span.endSec) - span.startBeat;
        const double rate = take->playrate > 0 ? take->playrate : 1.0;
        const bool looping = item.loop && loopBeats > 0;
        const model::SourceBeatMapping mapping{take->startOffset, rate, loopBeats};
        if (looping && tick >= seq.length) continue;  // not in the loop: never heard
        const int reps = looping
                             ? 1 + static_cast<int>(std::floor(
                                       (itemBeats * rate + mapping.sourceOffset(true)) / loopBeats))
                             : 1;
        for (int pass = looping ? -1 : 0; pass < reps; ++pass) {
            const double on = mapping.itemBeat(sourceBeat, pass, looping);
            if (on < 0 || on >= itemBeats) continue;
            beats.push_back(span.startBeat + on);
        }
    }
    std::sort(beats.begin(), beats.end());
    beats.erase(std::unique(beats.begin(), beats.end()), beats.end());
    return beats;
}

Result<harmony::Key> parseKeyText(std::string_view text, const std::string& path) {
    const std::size_t space = text.find(' ');
    const auto tonic = harmony::parsePitchClass(text.substr(0, space));
    if (!tonic || space == std::string_view::npos)
        return Error("key.tonic", "'" + std::string(text) + "' is not a key", path,
                     "write a tonic and a scale, for example \"Eb major\" or \"F# minor\"");
    std::string scaleText(text.substr(space + 1));
    std::replace(scaleText.begin(), scaleText.end(), ' ', '_');
    const auto scale = harmony::parseScale(scaleText);
    if (!scale) {
        std::string list;
        for (auto sc : harmony::allScales())
            list += (list.empty() ? "" : ", ") + std::string(harmony::scaleName(sc));
        return Error("scale.unknown", "'" + scaleText + "' is not a scale", path,
                     "scales: " + list);
    }
    const harmony::Key key{*tonic, *scale};
    if (!harmony::keySupported(key))
        return Error("key.unsupported", std::string(text) + " needs a double accidental", path,
                     "use the enharmonic key (Ab major for G# major)");
    return key;
}

namespace {

std::string beatText(const model::Session& s, double beat) {
    const auto bb = s.timeMap->barBeat(beat);
    return "bar " + std::to_string(bb.bar) + " beat " + std::to_string(bb.beat + 1);
}

}  // namespace

Result<NoteContext> noteContext(const model::Session& s, model::Id source, std::int64_t tick,
                                double fallbackBeat, bool needKey, bool needChord,
                                const std::string& path) {
    std::vector<double> beats = soundingBeats(s, source, tick);
    if (beats.empty()) beats.push_back(fallbackBeat);
    NoteContext out;
    bool first = true;
    double firstBeat = beats.front();
    for (const double beat : beats) {
        std::optional<harmony::Key> key;
        std::optional<harmony::Chord> chord;
        if (needKey) {
            const auto id = model::keyPointAt(s, beat);
            const model::ContextPoint* point = id ? s.context.find(*id) : nullptr;
            key = point ? model::parseKey(*point) : std::nullopt;
            if (!key)
                return Error("midi.no_key",
                             point ? "the key at " + beatText(s, beat) + " is '" +
                                         model::keyWords(*point) + "', which this build cannot read"
                                   : "no key holds at " + beatText(s, beat),
                             path, "pass key (for example \"C major\"), or set one with key.set");
        }
        if (needChord) {
            const auto id = model::chordPointAt(s, beat);
            const model::ContextPoint* point = id ? s.context.find(*id) : nullptr;
            chord = point ? model::parseChordPoint(*point) : std::nullopt;
            if (!chord)
                return Error("midi.no_chord",
                             point && !point->symbol.empty()
                                 ? "the chord at " + beatText(s, beat) + " is '" + point->symbol +
                                       "', which this build cannot read"
                                 : "no chord holds at " + beatText(s, beat),
                             path,
                             "pass chord (for example \"Cm7\"), set one with chord.set, or leave "
                             "this note out of notes");
        }
        if (first) {
            out.key = key;
            out.chord = chord;
            first = false;
            continue;
        }
        if (needKey && key != out.key)
            return Error("midi.context_varies",
                         "this note sounds in different keys: " + beatText(s, firstBeat) + " and " +
                             beatText(s, beat) + " (a loop pass or another item sharing this MIDI)",
                         path,
                         "pass key to say which one, or make the item not loop or unshare it so "
                         "every pass sees one key");
        if (needChord && chord != out.chord)
            return Error("midi.context_varies",
                         "this note sounds over different chords: " + beatText(s, firstBeat) +
                             " and " + beatText(s, beat) +
                             " (a loop pass or another item sharing this MIDI)",
                         path,
                         "pass chord to say which one, or make the item not loop or unshare it so "
                         "every pass sees one chord");
    }
    return out;
}

}  // namespace supple::engine::detail
