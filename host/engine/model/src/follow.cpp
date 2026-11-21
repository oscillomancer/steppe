// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/follow.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "supple/json/write.h"
#include "supple/model/source_mapping.h"

namespace supple::model {
namespace {

const ContextPoint* held(std::span<const ContextPoint> points, ContextType type, double beat) {
    const double limit = beat + kPlaceSlack;
    const ContextPoint* last = nullptr;
    const ContextPoint* first = nullptr;
    for (const ContextPoint& p : points) {
        if (p.type != type) continue;
        if (!first) first = &p;
        if (p.beat > limit) break;  // sorted: nothing later holds
        if (!last || p.beat >= last->beat) last = &p;
    }
    return last ? last : first;
}

std::string words(const ContextPoint& p) {
    return p.type == ContextType::Key ? "key " + keyWords(p)
                                      : "chord " + (p.symbol.empty() ? "none" : p.symbol);
}

bool holdsMembers(const Take& t) {
    return !t.context.empty() || t.contextPeriod || t.contextPhase != 0 ||
           t.follow != ClipFollow::None;
}

bool validPeriod(const std::optional<double>& period) {
    return !period || (std::isfinite(*period) && *period > 0);
}

// `beat` in [0, period), a beat within the slack below the period reading as 0.
double wrapped(double beat, double period) {
    double u = beat - period * std::floor(beat / period);
    if (period - u <= kPlaceSlack || u < 0) u = 0;
    return u;
}

}  // namespace

const ContextPoint* contextKeyPointAt(std::span<const ContextPoint> points, double contextBeat) {
    return held(points, ContextType::Key, contextBeat);
}
const ContextPoint* contextChordPointAt(std::span<const ContextPoint> points, double contextBeat) {
    return held(points, ContextType::Chord, contextBeat);
}

harmony::Place takePlaceAt(std::span<const ContextPoint> points, double contextBeat, bool chords) {
    harmony::Place place;
    if (const ContextPoint* k = contextKeyPointAt(points, contextBeat)) place.key = parseKey(*k);
    if (chords)
        if (const ContextPoint* c = contextChordPointAt(points, contextBeat))
            place.chord = parseChordPoint(*c);
    return place;
}

namespace {
const ContextPoint* projectHeld(const ContextView& view,
                                const std::vector<ContextView::Entry>& list, double songBeat) {
    const double limit = songBeat + kPlaceSlack;
    const auto it =
        std::upper_bound(list.begin(), list.end(), limit,
                         [](double b, const ContextView::Entry& e) { return b < e.beat; });
    if (it == list.begin()) return nullptr;
    return view.source.find((it - 1)->id);
}
}  // namespace

harmony::Place projectPlaceAt(const ContextView& project, double songBeat, bool chords) {
    harmony::Place place;
    if (const ContextPoint* k = projectHeld(project, project.keys, songBeat))
        place.key = parseKey(*k);
    if (chords)
        if (const ContextPoint* c = projectHeld(project, project.chords, songBeat))
            place.chord = parseChordPoint(*c);
    return place;
}

// --- the fields and their rules -----------------------------------------------------------

bool takeIsMidi(const Take& take) { return take.source && take.source->kind == Kind::Midi; }

bool takeCanFollow(const Take& take) {
    if (!takeIsMidi(take)) return false;
    const ContextPoint* first =
        contextKeyPointAt(take.context, std::numeric_limits<double>::lowest());
    return first && parseKey(*first).has_value();
}

std::vector<ClipContextNote> fitTakeContext(Take& take, TakeHome home) {
    std::vector<ClipContextNote> notes;
    if (home == TakeHome::Clip || !takeIsMidi(take)) {
        const bool hadContext =
            !take.context.empty() || take.contextPeriod || take.contextPhase != 0;
        const std::string why =
            home == TakeHome::Clip ? "a clip's take holds none of it: the context is the clip's own"
                                   : "only a MIDI take holds a context";
        if (hadContext)
            notes.push_back({"take.context_dropped", "dropped the take's context: " + why});
        if (take.follow != ClipFollow::None)
            notes.push_back({"take.follow_reset", "follow is reset to none: " + why});
        take.context.clear();
        take.contextPeriod.reset();
        take.contextPhase = 0;
        take.follow = ClipFollow::None;
        return notes;
    }
    if (!validPeriod(take.contextPeriod)) {
        notes.push_back({"take.context_dropped",
                         "dropped context_period: it must be finite and above 0, so the context "
                         "does not repeat"});
        take.contextPeriod.reset();
    }
    if (!std::isfinite(take.contextPhase)) {
        notes.push_back({"take.context_dropped", "context_phase is not finite: read as 0"});
        take.contextPhase = 0;
    }
    const auto drop = [&](const ContextPoint& p, const std::string& why) {
        notes.push_back({"take.context_dropped", "dropped " + words(p) + " at beat " +
                                                     json::formatDouble(p.beat) + ": " + why});
    };
    std::vector<ContextPoint> points = take.context;
    for (ContextPoint& p : points)
        if (std::isfinite(p.beat)) p.beat = std::round(p.beat * 960.0) / 960.0;
    std::stable_sort(points.begin(), points.end(),
                     [](const ContextPoint& a, const ContextPoint& b) {
                         if (a.beat != b.beat) return a.beat < b.beat;
                         return a.type < b.type;
                     });
    std::vector<ContextPoint> kept;
    std::size_t keys = 0, chords = 0;
    for (const ContextPoint& p : points) {
        if (!std::isfinite(p.beat) || p.beat < 0) {
            drop(p, "before the context's start");
        } else if (take.contextPeriod && p.beat >= *take.contextPeriod) {
            drop(p, "at or past the context's period");
        } else if (!kept.empty() && kept.back().beat == p.beat && kept.back().type == p.type) {
            drop(p, "another point of its type is at that beat");
        } else if ((p.type == ContextType::Key ? keys : chords) >= kMaxContextPoints) {
            drop(p, "a take holds at most " + std::to_string(kMaxContextPoints) + " of a type");
        } else {
            (p.type == ContextType::Key ? keys : chords)++;
            kept.push_back(p);
        }
    }
    take.context = std::move(kept);
    if (take.follow != ClipFollow::None && !takeCanFollow(take)) {
        take.follow = ClipFollow::None;
        notes.push_back({"take.follow_reset",
                         "follow is reset to none: the take's context holds no readable key"});
    }
    return notes;
}

void checkTakeContext(const Session& s, std::vector<Error>& errors) {
    auto fail = [&](std::string path, std::string message) {
        if (errors.size() < 50)
            errors.emplace_back("model.invariant", std::move(message), std::move(path));
    };
    std::set<Id> inClip;
    for (const auto& e : s.clips)
        for (Id t : e.value->takes) inClip.insert(t);
    for (const auto& e : s.takes) {
        const Take& t = *e.value;
        const std::string path = "/takes/" + e.id.str();
        if (!holdsMembers(t)) continue;
        if (!takeIsMidi(t)) {
            fail(path,
                 "an audio or empty take holds no context, context_period, context_phase "
                 "or follow");
            continue;
        }
        if (inClip.contains(e.id)) {
            fail(path,
                 "a clip's take holds none of context, context_period, context_phase or "
                 "follow: they are a clip's own");
            continue;
        }
        if (!validPeriod(t.contextPeriod))
            fail(path + "/context_period", "the context period must be finite and above 0");
        if (!std::isfinite(t.contextPhase))
            fail(path + "/context_phase", "the context phase must be finite");
        std::size_t keys = 0, chords = 0;
        for (std::size_t i = 0; i < t.context.size(); ++i) {
            const ContextPoint& p = t.context[i];
            const std::string pt = path + "/context/" + std::to_string(i);
            (p.type == ContextType::Key ? keys : chords)++;
            if (!std::isfinite(p.beat) || p.beat < 0)
                fail(pt + "/beat", "a take's context point lies at beat 0 or later");
            else if (t.contextPeriod && validPeriod(t.contextPeriod) && p.beat >= *t.contextPeriod)
                fail(pt + "/beat", "the point lies at or past the context's period");
            if (i > 0) {
                const ContextPoint& q = t.context[i - 1];
                if (p.beat < q.beat || (p.beat == q.beat && p.type < q.type))
                    fail(pt, "the points are not sorted by beat, then type (key first)");
                else if (p.beat == q.beat && p.type == q.type)
                    fail(pt, "two points of one type at one beat");
            }
        }
        if (keys > kMaxContextPoints || chords > kMaxContextPoints)
            fail(path + "/context",
                 "more than " + std::to_string(kMaxContextPoints) + " context points of one type");
        if (t.follow != ClipFollow::None && !takeCanFollow(t))
            fail(path + "/follow", "follow needs a readable key in the take's context");
    }
}

void takeContextFromClip(Take& take, const Clip& clip, double clipBeatAtStart, ClipFollow follow) {
    if (clip.context.empty() || !takeIsMidi(take)) return;
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    take.context = clip.context;
    for (ContextPoint& p : take.context) p.beat *= rate;
    take.contextPeriod = clip.length * rate;
    take.contextPhase = clipBeatAtStart * rate - take.startOffset;
    take.follow = follow;
    fitTakeContext(take, TakeHome::Item);
}

// --- a note's place -----------------------------------------------------------------------

double contextBeatOf(const Take& take, double unwrappedSourceBeat) {
    const double u = take.contextPhase + unwrappedSourceBeat;
    if (take.contextPeriod && validPeriod(take.contextPeriod))
        return wrapped(u, *take.contextPeriod);
    return u;
}

NotePlace notePlace(const Take& take, double itemStartBeat, double unwrappedSourceBeat) {
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    return {contextBeatOf(take, unwrappedSourceBeat),
            itemStartBeat + (unwrappedSourceBeat - take.startOffset) / rate};
}

void wrapStartOffset(Take& take, double sourceBeats) {
    if (!(sourceBeats > 0) || !std::isfinite(sourceBeats) || !std::isfinite(take.startOffset))
        return;
    if (take.startOffset >= 0 && take.startOffset < sourceBeats) return;
    const double inside =
        take.startOffset - sourceBeats * std::floor(take.startOffset / sourceBeats);
    take.contextPhase += take.startOffset - inside;
    take.startOffset = inside;
}

PlaceReading placeAt(const Session& s, const Item& item, const Take& take, double songBeat) {
    const model::ItemSpan span = itemSpan(s, item);
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    PlaceReading r;
    r.songBeat = songBeat;
    r.sourceBeat = take.startOffset + (songBeat - span.startBeat) * rate;
    r.contextBeat = contextBeatOf(take, r.sourceBeat);
    r.take = takePlaceAt(take.context, r.contextBeat, true);
    r.project = projectPlaceAt(*s.contextView, songBeat, true);
    r.follow = take.follow;
    return r;
}

std::optional<PlaceReading> placeAt(const Session& s, Id item, Id take, double songBeat) {
    const Item* it = s.items.find(item);
    const Take* tk = s.takes.find(take);
    if (!it || !tk || !takeIsMidi(*tk)) return std::nullopt;
    if (std::find(it->takes.begin(), it->takes.end(), take) == it->takes.end()) return std::nullopt;
    return placeAt(s, *it, *tk, songBeat);
}

std::optional<PlaceReading> placeAt(const Session& s, Id item, double songBeat) {
    const Item* it = s.items.find(item);
    if (!it || !it->activeTake) return std::nullopt;
    return placeAt(s, item, *it->activeTake, songBeat);
}

harmony::Followed soundsAt(const PlaceReading& place, int channel, int key) {
    const int written = std::clamp(key, 0, 127);
    if (place.follow == ClipFollow::None || channel == 9 || key < 0 || key > 127)
        return harmony::Followed{written, harmony::FollowedBy::Copy, false, false};
    const bool chords = place.follow == ClipFollow::Chords;
    harmony::Place from = place.take, to = place.project;
    if (!chords) from.chord.reset(), to.chord.reset();
    return harmony::follow(key, from, to,
                           chords ? harmony::FollowMode::Chords : harmony::FollowMode::Key);
}

// --- what a note sounds as ----------------------------------------------------------------

harmony::Followed soundingKey(std::span<const ContextPoint> context, ClipFollow follow,
                              const ContextView& project, double contextBeat, double songBeat,
                              int channel, int key) {
    if (follow == ClipFollow::None || channel == 9 || key < 0 || key > 127)
        return harmony::Followed{std::clamp(key, 0, 127), harmony::FollowedBy::Copy, false, false};
    const bool chords = follow == ClipFollow::Chords;
    return harmony::follow(key, takePlaceAt(context, contextBeat, chords),
                           projectPlaceAt(project, songBeat, chords),
                           chords ? harmony::FollowMode::Chords : harmony::FollowMode::Key);
}

std::vector<SoundingNote> soundingNotes(const Session& s, Id itemId, double fromBeat,
                                        double toBeat) {
    std::vector<SoundingNote> out;
    const Item* item = s.items.find(itemId);
    if (!item || !item->activeTake) return out;
    const Take* take = s.takes.find(*item->activeTake);
    if (!take || !takeIsMidi(*take)) return out;
    const MidiSource* src = s.midi.find(*take->source);
    if (!src) return out;
    const midi::Sequence& seq = src->sequence;
    if (!(seq.ppq > 0)) return out;
    // The same walk as the engine's compile (plan.cpp, trackEvents): the item's passes, the
    // note a take starts inside (chased at the item's start) and the preceding cycle's crossing
    // notes.
    const ItemSpan span = itemSpan(s, *item);
    const double startBeat = span.startBeat;
    const double endBeat = s.timeMap->beatAt(span.endSec);
    const double rate = take->playrate > 0 ? take->playrate : 1.0;
    const double cycle = static_cast<double>(seq.length) / seq.ppq;
    const bool looping = item->loop && cycle > 0;
    const SourceBeatMapping mapping{take->startOffset, rate, looping ? cycle : 0.0};
    const double sourceOffset = mapping.sourceOffset(looping);
    // The part of start_offset that is whole cycles: a looped item's notes count them in
    // their unwrapped source beat.
    const double wholeCycles = take->startOffset - sourceOffset;
    const bool follows = take->follow != ClipFollow::None;
    const long reps = looping ? 1 + static_cast<long>(std::floor(
                                        ((endBeat - startBeat) * rate + sourceOffset) / cycle))
                              : 1;
    // Passes a range can reach (the chased passes, -1 and 0, are always walked).
    long firstPass = std::numeric_limits<long>::min(), lastPass = std::numeric_limits<long>::max();
    if (looping && std::isfinite(fromBeat) && std::isfinite(toBeat)) {
        const double lo = std::floor(((fromBeat - startBeat) * rate + sourceOffset) / cycle) - 1;
        const double hi = std::floor(((toBeat - startBeat) * rate + sourceOffset) / cycle) + 1;
        if (std::abs(lo) < 1e15) firstPass = static_cast<long>(lo);
        if (std::abs(hi) < 1e15) lastPass = static_cast<long>(hi);
    }
    constexpr double kEdge = 1e-9;
    for (long k = looping ? -1 : 0; k < reps; ++k) {
        if (k > 0 && k < firstPass) {
            k = firstPass - 1;
            continue;
        }
        if (k > 0 && k > lastPass) break;
        const double offset = static_cast<double>(k) * cycle;
        for (std::size_t i = 0; i < seq.notes.size(); ++i) {
            const midi::Note& n = seq.notes[i];
            if (looping && n.start >= seq.length) continue;
            const double b0 = static_cast<double>(n.start) / seq.ppq;
            const double itemBeat = mapping.itemBeat(b0, static_cast<double>(k), looping);
            const bool chased = itemBeat < 0;
            if (chased &&
                static_cast<double>(n.start + n.length) / seq.ppq + offset <= sourceOffset)
                continue;  // it ended before the item's edge
            const double heard = chased ? startBeat : startBeat + itemBeat;
            if (heard + kEdge >= endBeat) continue;
            if (heard < fromBeat || heard >= toBeat) continue;
            const NotePlace place =
                notePlace(*take, startBeat, b0 + static_cast<double>(k) * cycle + wholeCycles);
            SoundingNote note{static_cast<std::uint32_t>(i),
                              static_cast<std::int32_t>(k),
                              place.songBeat,
                              place.contextBeat,
                              n.channel,
                              n.pitch,
                              harmony::Followed{n.pitch, harmony::FollowedBy::Copy, false, false}};
            if (follows)
                note.sounds = soundingKey(take->context, take->follow, *s.contextView,
                                          place.contextBeat, place.songBeat, n.channel, n.pitch);
            note.endBeat = std::min(
                endBeat,
                startBeat + mapping.itemBeat(static_cast<double>(n.start + n.length) / seq.ppq,
                                             static_cast<double>(k), looping));
            out.push_back(note);
        }
    }
    return out;
}

LeadPitches leadPitches(const std::vector<SoundingNote>& heard,
                        const std::vector<std::int64_t>& sounded, double itemStartBeat,
                        std::size_t noteCount) {
    constexpr double kEdge = 1e-9;
    LeadPitches out;
    out.keep.assign(noteCount, std::nullopt);
    std::vector<bool> whole(noteCount, false);
    for (std::size_t k = 0; k < heard.size() && k < sounded.size(); ++k) {
        const SoundingNote& h = heard[k];
        if (h.note >= noteCount) continue;
        const bool chased = h.songBeat < itemStartBeat - kEdge;
        if (!out.keep[h.note]) {
            out.keep[h.note] = sounded[k];
            whole[h.note] = !chased;
        } else if (!chased) {
            if (!whole[h.note]) {  // the first whole pass beats a held-in fragment
                out.keep[h.note] = sounded[k];
                whole[h.note] = true;
            } else if (out.keep[h.note] != sounded[k]) {
                out.alike = false;
            }
        }
    }
    return out;
}

// --- contextOver ----------------------------------------------------------------------------

std::vector<ContextPoint> contextOver(const Take& take, double u0, double u1, bool* truncated) {
    if (truncated) *truncated = false;
    std::vector<ContextPoint> out;
    if (take.context.empty() || !std::isfinite(u0) || !std::isfinite(u1)) return out;
    const bool repeats = take.contextPeriod && validPeriod(take.contextPeriod);
    const double period = repeats ? *take.contextPeriod : 0.0;
    std::size_t keys = 0, chords = 0;
    // The last point kept of each type, to drop one that lands on the same grid beat.
    const auto round960 = [](double v) { return std::round(v * 960.0) / 960.0; };
    const auto add = [&](ContextPoint p, double beat) {
        p.beat = round960(beat);
        std::size_t& count = p.type == ContextType::Key ? keys : chords;
        for (auto it = out.rbegin(); it != out.rend() && it->beat == p.beat; ++it)
            if (it->type == p.type) return true;  // a point of its type is already there
        if (count >= kMaxContextPoints) {
            if (truncated) *truncated = true;
            return false;
        }
        ++count;
        out.push_back(std::move(p));
        return true;
    };
    // In force at u0, carried in at 0.
    const double at0 = repeats ? wrapped(u0, period) : u0;
    if (const ContextPoint* k = contextKeyPointAt(take.context, at0)) add(*k, 0.0);
    if (const ContextPoint* c = contextChordPointAt(take.context, at0)) add(*c, 0.0);
    const double firstRepeat = repeats ? std::floor(u0 / period) : 0.0;
    for (double n = firstRepeat;; n += 1.0) {
        const double base = repeats ? n * period : 0.0;
        if (base >= u1) break;
        bool full = false;
        for (const ContextPoint& p : take.context) {
            const double q = base + p.beat;
            if (q <= u0 || q >= u1) continue;
            if (!add(p, q - u0)) {
                full = true;
                break;
            }
        }
        if (full || !repeats) break;
    }
    std::stable_sort(out.begin(), out.end(), [](const ContextPoint& a, const ContextPoint& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        return a.type < b.type;
    });
    return out;
}

}  // namespace supple::model
