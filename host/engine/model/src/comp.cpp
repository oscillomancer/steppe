// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/comp.h"

#include <algorithm>

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

bool continues(const Session& s, const Entry& l, const Entry& r) {
    if (!(r.start > l.start && r.start <= l.end && r.end > l.end)) return false;
    if (!compatible(s, *l.item, *r.item)) return false;
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

}  // namespace supple::model
