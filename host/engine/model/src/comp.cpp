// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/comp.h"

#include <algorithm>

#include <cmath>

#include "supple/model/audio_warp.h"
#include "supple/model/schemas.h"
#include "supple/time/timeline.h"

namespace supple::model {
namespace {

struct Entry {
    Id id;
    const Item* item;
    std::int64_t start, end;
};

std::optional<Kind> sourceKind(const Take& take) {
    return take.source ? std::optional<Kind>(take.source->kind) : std::nullopt;
}

bool allAudio(const Session& s, const Item& item) {
    return std::all_of(item.takes.begin(), item.takes.end(), [&](Id id) {
        const Take* take = s.takes.find(id);
        return take && take->source && take->source->kind == Kind::Media;
    });
}

bool compatible(const Session& s, const Item& a, const Item& b) {
    if (a.track != b.track || a.lane != b.lane || a.timebase != b.timebase || a.loop || b.loop ||
        a.takes.size() != b.takes.size() || a.takes.size() < 2)
        return false;
    for (std::size_t i = 0; i < a.takes.size(); ++i) {
        const Take* x = s.takes.find(a.takes[i]);
        const Take* y = s.takes.find(b.takes[i]);
        if (!x || !y || sourceKind(*x) != sourceKind(*y) || x->name != y->name) return false;
        if (x->source && x->source->kind == Kind::Media && x->source != y->source) return false;
    }
    return true;
}

// Whether every take plays the same source at the seam on both sides, to a sample: a split
// piece does, a copy placed after the original (same takes, other source position) does not, and
// is another item, not the next piece.
bool sourceContinues(const Session& s, const Entry& l, const Entry& r) {
    const double rate = s.project->sampleRate;
    const std::int64_t at = r.start + (l.end - r.start) / 2;
    const double seamSec = static_cast<double>(at) / rate;
    const ItemSpan ls = itemSpan(s, *l.item), rs = itemSpan(s, *r.item);
    const double seamBeat = s.timeMap->beatAt(seamSec);
    const double beatSample = s.timeMap->beatAt(seamSec + 1.0 / rate) - seamBeat;
    for (std::size_t i = 0; i < l.item->takes.size(); ++i) {
        const Take& x = *s.takes.find(l.item->takes[i]);
        const Take& y = *s.takes.find(r.item->takes[i]);
        if (!x.source) continue;
        if (x.source->kind == Kind::Media) {
            const double a = audioSourceAt(x, seamSec - ls.startSec);
            const double b = audioSourceAt(y, seamSec - rs.startSec);
            const double speed = std::max({1.0, audioSpeed(x), audioSpeed(y)});
            if (std::fabs(a - b) * rate > speed + 1e-6) return false;
        } else {
            const double a = x.startOffset + (seamBeat - ls.startBeat) * x.playrate;
            const double b = y.startOffset + (seamBeat - rs.startBeat) * y.playrate;
            if (std::fabs(a - b) > std::max(1.0, x.playrate) * beatSample + 1e-9) return false;
        }
    }
    return true;
}

bool continues(const Session& s, const Entry& l, const Entry& r) {
    if (!(r.start > l.start && r.start <= l.end && r.end > l.end)) return false;
    if (!compatible(s, *l.item, *r.item)) return false;
    if (!sourceContinues(s, l, r)) return false;
    return r.start == l.end || (allAudio(s, *l.item) && allAudio(s, *r.item));
}

Error mismatch(std::string path, const std::string& what) {
    return Error("comp.mismatch", what, std::move(path),
                 "name the pieces of one comp in items (take.comp.get lists a comp's pieces), or "
                 "use take.add and item.split to make takes and pieces that match");
}

}  // namespace

Result<std::vector<CompPiece>> compRun(const Session& s, std::optional<Id> item,
                                       std::span<const Id> items) {
    const double rate = s.project->sampleRate;
    auto entry = [&](Id id) {
        const Item& it = *s.items.find(id);
        const ItemSpan span = itemSpan(s, it);
        return Entry{id, &it, time::samplesAt(span.startSec, rate),
                     time::samplesAt(span.endSec, rate)};
    };
    auto check = [&](Id id, const std::string& path) -> Status {
        const Item& it = *s.items.find(id);
        if (it.takes.size() < 2)
            return Error("comp.takes", id.str() + " has fewer than two takes", path,
                         "a comp chooses between takes: record several passes or add takes "
                         "(take.add), or switch the one take with take.activate");
        if (it.loop)
            return Error("comp.loop", id.str() + " loops", path,
                         "comp an item that does not loop, or item.set {loop: false} first");
        return {};
    };
    std::vector<Entry> run;
    if (item) {
        SUPPLE_TRY(check(*item, "/item"));
        const Entry first = entry(*item);
        std::vector<Entry> others;
        for (const auto& e : s.items)
            if (e.id != *item && e.value->track == first.item->track) others.push_back(entry(e.id));
        run.push_back(first);
        // To the right, then to the left; more than one continuation is not guessed.
        for (bool right : {true, false})
            for (;;) {
                const Entry at = right ? run.back() : run.front();
                std::vector<Entry> found;
                for (const Entry& o : others)
                    if (right ? continues(s, at, o) : continues(s, o, at)) found.push_back(o);
                if (found.empty()) break;
                if (found.size() > 1)
                    return mismatch(
                        "/item", "more than one item could continue the comp after " + at.id.str());
                if (right)
                    run.push_back(found.front());
                else
                    run.insert(run.begin(), found.front());
            }
    } else {
        std::vector<std::pair<Entry, std::size_t>> listed;
        for (std::size_t i = 0; i < items.size(); ++i) {
            SUPPLE_TRY(check(items[i], "/items/" + std::to_string(i)));
            listed.emplace_back(entry(items[i]), i);
        }
        std::sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) {
            return a.first.start == b.first.start ? a.first.id < b.first.id
                                                  : a.first.start < b.first.start;
        });
        for (std::size_t i = 0; i < listed.size(); ++i) {
            if (i > 0 && !continues(s, listed[i - 1].first, listed[i].first))
                return mismatch("/items/" + std::to_string(listed[i].second),
                                listed[i].first.id.str() + " does not continue " +
                                    listed[i - 1].first.id.str());
            run.push_back(listed[i].first);
        }
    }
    std::vector<CompPiece> out;
    for (std::size_t i = 0; i < run.size(); ++i) {
        CompPiece p;
        p.item = run[i].id;
        p.start = p.coreStart = run[i].start;
        p.end = p.coreEnd = run[i].end;
        out.push_back(p);
    }
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        const std::int64_t overlap = out[i].end - out[i + 1].start;
        const std::int64_t at = out[i + 1].start + overlap / 2;
        out[i].coreEnd = at;
        out[i + 1].coreStart = at;
    }
    return out;
}

bool joinablePieces(const Session& s, Id left, Id right) {
    const Item* l = s.items.find(left);
    const Item* r = s.items.find(right);
    if (!l || !r || l->takes.size() != r->takes.size()) return false;
    auto plain = [&](const Item& it) {
        auto j = toJson(it);
        for (const char* key :
             {"position", "length", "fade_in", "fade_out", "takes", "active_take"})
            j.asObject().erase(key);
        return j;
    };
    if (plain(*l) != plain(*r)) return false;
    for (std::size_t i = 0; i < l->takes.size(); ++i) {
        const Take* x = s.takes.find(l->takes[i]);
        const Take* y = s.takes.find(r->takes[i]);
        if (!x || !y || !x->source || !y->source || x->source->kind != y->source->kind)
            return false;
        const bool audio = x->source->kind == Kind::Media;
        if (!x->stretchMarkers.empty() || !y->stretchMarkers.empty() || !x->warpPeers.empty() ||
            !y->warpPeers.empty() || !x->takeMarkers.empty() || !y->takeMarkers.empty() ||
            x->chain.size() != y->chain.size())
            return false;
        auto a = toJson(*x), b = toJson(*y);
        for (const char* key : {"start_offset", "chain", "source"}) {
            a.asObject().erase(key);
            b.asObject().erase(key);
        }
        if (a != b) return false;
        // The audio source is one file (compRun checked the media); a MIDI piece cut by
        // item.split holds its own copy of the notes, which are the same notes while neither
        // half has been edited (a pooled source is one object, and is the same by its id).
        if (audio) {
            if (x->source != y->source) return false;
        } else if (x->source != y->source) {
            const MidiSource* p = s.midi.find(*x->source);
            const MidiSource* q = s.midi.find(*y->source);
            if (!p || !q || p->pooled || q->pooled || toJson(*p) != toJson(*q)) return false;
        }
        for (std::size_t f = 0; f < x->chain.size(); ++f) {
            const auto* p = s.fx.find(x->chain[f]);
            const auto* q = s.fx.find(y->chain[f]);
            if (!p || !q || toJson(*p) != toJson(*q)) return false;
        }
        for (const auto& e : s.envelopes) {
            const Id object = e.value->target.object;
            if (object == l->takes[i] || object == r->takes[i]) return false;
            for (std::size_t f = 0; f < x->chain.size(); ++f)
                if (object == x->chain[f] || object == y->chain[f]) return false;
        }
    }
    return true;
}

CompCrossfade compCrossfade(const Session& s, std::span<const CompPiece> run) {
    CompCrossfade out;
    if (run.size() < 2) return out;
    out.seconds = 0;
    for (std::size_t i = 0; i + 1 < run.size(); ++i) {
        const Fade& left = s.items.find(run[i].item)->fadeOut;
        const Fade& right = s.items.find(run[i + 1].item)->fadeIn;
        for (const Fade* fade : {&left, &right})
            if (fade->length > out.seconds) {
                out.seconds = fade->length;
                out.shape = fade->shape == FadeShape::Linear ? FadeShape::Linear : FadeShape::Cos;
            }
    }
    return out;
}

namespace {
// The seams (see compDifferingSeams) that are not what take.comp writes for `read`.
std::vector<std::size_t> seamsOffFrom(const Session& s, std::span<const CompPiece> run,
                                      const CompCrossfade& read) {
    std::vector<std::size_t> out;
    const double rate = s.project->sampleRate;
    const auto wanted = static_cast<std::int64_t>(std::llround(read.seconds * rate / 2.0));
    auto indexOf = [&](const Item& item) {
        const auto at = item.activeTake
                            ? std::find(item.takes.begin(), item.takes.end(), *item.activeTake)
                            : item.takes.end();
        return at - item.takes.begin();
    };
    for (std::size_t k = 0; k + 1 < run.size(); ++k) {
        const Item& left = *s.items.find(run[k].item);
        const Item& right = *s.items.find(run[k + 1].item);
        std::int64_t h = 0;
        if (indexOf(left) != indexOf(right) && wanted > 0)
            h = std::min({wanted, (run[k].coreEnd - run[k].coreStart) / 2,
                          (run[k + 1].coreEnd - run[k + 1].coreStart) / 2});
        Fade expected;
        if (h > 0) {
            expected.length = static_cast<double>(2 * h) / rate;
            expected.shape = read.shape;
        }
        auto same = [&](const Fade& fade) {
            return fade.length == expected.length && (h == 0 || fade.shape == expected.shape);
        };
        if (!same(left.fadeOut) || !same(right.fadeIn)) out.push_back(k);
    }
    return out;
}
}  // namespace

std::vector<std::size_t> compDifferingSeams(const Session& s, std::span<const CompPiece> run) {
    if (run.size() < 2) return {};
    // The crossfade that fits most seams: each fade found on a seam is tried, and the one that
    // leaves the fewest seams off wins (the first seam's, on a tie). A comp as take.comp made it
    // has one that fits all.
    std::vector<std::size_t> best;
    bool first = true;
    auto consider = [&](const Fade& fade) {
        CompCrossfade candidate;
        candidate.seconds = fade.length;
        candidate.shape = fade.shape == FadeShape::Linear ? FadeShape::Linear : FadeShape::Cos;
        auto off = seamsOffFrom(s, run, candidate);
        if (first || off.size() < best.size()) best = std::move(off);
        first = false;
    };
    consider(Fade{});  // hard cuts, tried first on no seam fade at all
    for (std::size_t k = 0; k + 1 < run.size(); ++k) {
        consider(s.items.find(run[k].item)->fadeOut);
        consider(s.items.find(run[k + 1].item)->fadeIn);
    }
    return best;
}

void setItemStartSeconds(const Session& s, Item& it, double startSec) {
    const time::TimeMap& tm = *s.timeMap;
    const ItemSpan span = itemSpan(s, it);
    switch (itemTimebase(s, it)) {
        case Timebase::Time:
            it.length = span.endSec - startSec;
            it.position = startSec;
            break;
        case Timebase::Beats: {
            const double endBeat = it.position + it.length;
            it.position = tm.beatAt(startSec);
            it.length = endBeat - it.position;
            break;
        }
        case Timebase::BeatsPosition:
            it.position = tm.beatAt(startSec);
            it.length = span.endSec - startSec;
            break;
    }
}

void setItemEndSeconds(const Session& s, Item& it, double endSec) {
    const time::TimeMap& tm = *s.timeMap;
    const ItemSpan span = itemSpan(s, it);
    switch (itemTimebase(s, it)) {
        case Timebase::Time:
            it.length = endSec - it.position;
            break;
        case Timebase::Beats:
            it.length = tm.beatAt(endSec) - it.position;
            break;
        case Timebase::BeatsPosition:
            it.length = endSec - span.startSec;
            break;
    }
}

void setItemEndToItemEnd(const Session& s, Item& it, const Item& other) {
    switch (itemTimebase(s, it)) {
        case Timebase::Time:
        case Timebase::Beats:
            it.length = other.position + other.length - it.position;
            break;
        case Timebase::BeatsPosition:
            it.length = itemSpan(s, other).endSec - itemSpan(s, it).startSec;
            break;
    }
}

}  // namespace supple::model
