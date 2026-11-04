// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/session.h"
#include "supple/model/audio_warp.h"
#include "supple/model/groups.h"
#include "supple/model/numeric.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

#include "supple/model/env_shape.h"
#include "supple/model/freeze.h"
#include "supple/model/schemas.h"

namespace supple::model {

// --- ids -------------------------------------------------------------------------------

namespace {
constexpr std::string_view kPrefixes[kKindCount] = {"tr", "it", "tk", "md", "mi", "fx", "sd", "en",
                                                    "ai", "mk", "rg", "tp", "ts", "gr", "ln", "rj"};
}

std::string_view prefix(Kind kind) {
    return kind == Kind::Project ? "project" : kPrefixes[static_cast<std::size_t>(kind)];
}

std::optional<Kind> kindFromPrefix(std::string_view p) {
    for (std::size_t i = 0; i < kKindCount; ++i)
        if (kPrefixes[i] == p) return static_cast<Kind>(i);
    return std::nullopt;
}

std::string Id::str() const {
    return *this == kProject ? "project" : std::string(prefix(kind)) + "_" + std::to_string(n);
}

std::optional<Id> Id::parse(std::string_view text) {
    if (text == "project") return kProject;
    const auto us = text.find('_');
    if (us == std::string_view::npos || us + 1 >= text.size()) return std::nullopt;
    auto k = kindFromPrefix(text.substr(0, us));
    if (!k) return std::nullopt;
    const std::string_view digits = text.substr(us + 1);
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;  // canonical only
    std::uint32_t n = 0;
    auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
    return Id{*k, n};
}

// --- session ------------------------------------------------------------------------------

Session Session::create(Project project) {
    Session s;
    s.project = std::make_shared<const Project>(std::move(project));
    for (auto& n : s.next.next) n = 1;
    EditTxn txn(s, 0);
    Track master;
    master.name = "Master";
    master.type = StripType::Master;
    txn.insert(kMaster, std::move(master));  // the master is tr_0
    txn.create(TempoPointObj{0.0, 120.0, false});
    txn.create(MeterObj{1, 4, 4});
    auto out = txn.commit();
    return std::move(out).value();
}

// --- transactions --------------------------------------------------------------------------

EditTxn::EditTxn(const Session& base, std::uint64_t version)
    : base_(base),
      version_(version),
      next_(base.next),
      project_(base.project),
      blobs_(base.blobs) {}

void EditTxn::setProject(Project p) {
    project_ = std::make_shared<const Project>(std::move(p));
    projectChanged_ = true;
}

void EditTxn::putBlob(const std::string& hash, std::shared_ptr<const Blob> blob) {
    blobs_[hash] = std::move(blob);
}

void EditTxn::record(Change::Op op, Id id) {
    auto it = changes_.find(id);
    if (it == changes_.end()) {
        changes_.emplace(id, op);
        return;
    }
    Change::Op& cur = it->second;
    if (cur == Change::Op::Add && op == Change::Op::Remove)
        changes_.erase(it);  // created and removed in one transaction: nothing happened
    else if (cur == Change::Op::Add)
        ;  // add + update = add
    else
        cur = op;  // update + update = update; update + remove = remove
}

std::vector<Change> EditTxn::changes() const {
    std::vector<Change> out;
    out.reserve(changes_.size());
    for (const auto& [id, op] : changes_) out.push_back({op, id});
    if (projectChanged_) out.push_back({Change::Op::Update, kProject});
    return out;
}

namespace {
template <class T>
void apply(Session& s, TxnBuilders& b) {
    auto& slot = Traits<T>::slot(b);
    if (slot && slot->touched()) Traits<T>::map(s) = slot->build();
}
}  // namespace

void EditTxn::removeItemsOn(Id envelope) {
    std::vector<Id> on;
    for (const auto& e : base_.autoItems)
        if (const AutomationItem* ai = get<AutomationItem>(e.id); ai && ai->envelope == envelope)
            on.push_back(e.id);
    for (Id id : created_)
        if (id.kind == Kind::AutoItem)
            if (const AutomationItem* ai = get<AutomationItem>(id); ai && ai->envelope == envelope)
                on.push_back(id);
    for (Id id : on) remove<AutomationItem>(id);
}

Session EditTxn::preview() const {
    Session s = base_;
    apply<TempoPointObj>(s, builders_);
    apply<MeterObj>(s, builders_);
    apply<Track>(s, builders_);
    apply<TrackGroup>(s, builders_);
    apply<Item>(s, builders_);
    apply<Take>(s, builders_);
    apply<MediaSource>(s, builders_);
    apply<MidiSource>(s, builders_);
    apply<FxInstance>(s, builders_);
    apply<Route>(s, builders_);
    apply<Envelope>(s, builders_);
    apply<AutomationItem>(s, builders_);
    apply<Marker>(s, builders_);
    apply<Region>(s, builders_);
    s.project = project_;
    s.blobs = blobs_;
    s.next = next_;
    s.version = version_;
    if ((builders_.tempo && builders_.tempo->touched()) ||
        (builders_.meters && builders_.meters->touched())) {
        std::vector<time::TempoPoint> tempo;
        for (const auto& e : s.tempo)
            tempo.push_back({e.value->beat, e.value->bpm,
                             e.value->ramp ? time::TempoShape::Ramp : time::TempoShape::Step});
        std::sort(tempo.begin(), tempo.end(),
                  [](const auto& a, const auto& b) { return a.beat < b.beat; });
        std::vector<time::MeterChange> meter;
        for (const auto& e : s.meters)
            meter.push_back({e.value->bar, e.value->numerator, e.value->denominator});
        std::sort(meter.begin(), meter.end(),
                  [](const auto& a, const auto& b) { return a.bar < b.bar; });
        if (auto map = time::TimeMap::create(std::move(tempo), std::move(meter)))
            s.timeMap = std::make_shared<const time::TimeMap>(std::move(*map));
    }
    return s;
}

Result<Session> EditTxn::commit() {
    if (committed_) return Error("model.txn", "transaction already committed");
    committed_ = true;
    Session s = base_;
    apply<TempoPointObj>(s, builders_);
    apply<MeterObj>(s, builders_);
    apply<Track>(s, builders_);
    apply<TrackGroup>(s, builders_);
    apply<Item>(s, builders_);
    apply<Take>(s, builders_);
    apply<MediaSource>(s, builders_);
    apply<MidiSource>(s, builders_);
    apply<FxInstance>(s, builders_);
    apply<Route>(s, builders_);
    apply<Envelope>(s, builders_);
    apply<AutomationItem>(s, builders_);
    apply<Marker>(s, builders_);
    apply<Region>(s, builders_);
    s.project = project_;
    s.blobs = blobs_;
    s.next = next_;
    s.version = version_;
    // Derive the time map from the tempo points and meter changes.
    std::vector<time::TempoPoint> tempo;
    for (const auto& e : s.tempo)
        tempo.push_back({e.value->beat, e.value->bpm,
                         e.value->ramp ? time::TempoShape::Ramp : time::TempoShape::Step});
    std::sort(tempo.begin(), tempo.end(),
              [](const auto& a, const auto& b) { return a.beat < b.beat; });
    std::vector<time::MeterChange> meter;
    for (const auto& e : s.meters)
        meter.push_back({e.value->bar, e.value->numerator, e.value->denominator});
    std::sort(meter.begin(), meter.end(),
              [](const auto& a, const auto& b) { return a.bar < b.bar; });
    auto map = time::TimeMap::create(std::move(tempo), std::move(meter));
    if (!map) {
        Error e = std::move(map).error();
        e.code = "model.invariant";
        return e;
    }
    s.timeMap = std::make_shared<const time::TimeMap>(std::move(*map));
    SUPPLE_TRY(checkInvariants(s));
    SUPPLE_TRY(validateFrozenTransition(base_, s));
    return s;
}

// --- invariants ----------------------------------------------------------------------------

namespace {

struct Checker {
    const Session& s;
    std::vector<Error> errors;

    void fail(std::string path, std::string message) {
        if (errors.size() < 50)
            errors.emplace_back("model.invariant", std::move(message), std::move(path));
    }
    static std::string at(const char* coll, Id id, const char* field = nullptr) {
        std::string p = std::string("/") + coll + "/" + id.str();
        if (field) p += std::string("/") + field;
        return p;
    }

    void counters() {
        auto check = [&](auto& map, Kind k) {
            for (const auto& e : map)
                if (e.id.n >= s.next[k])
                    fail("/project/next_ids/" + std::string(prefix(k)),
                         "counter " + std::to_string(s.next[k]) + " is not past " + e.id.str());
        };
        check(s.tempo, Kind::TempoPoint);
        check(s.meters, Kind::Meter);
        check(s.tracks, Kind::Track);
        check(s.groups, Kind::Group);
        check(s.items, Kind::Item);
        check(s.takes, Kind::Take);
        check(s.media, Kind::Media);
        check(s.midi, Kind::Midi);
        check(s.fx, Kind::Fx);
        check(s.routes, Kind::Route);
        check(s.envelopes, Kind::Envelope);
        check(s.autoItems, Kind::AutoItem);
        check(s.markers, Kind::Marker);
        check(s.regions, Kind::Region);
    }

    void tracks() {
        const Track* master = s.tracks.find(kMaster);
        if (!master) {
            fail("/tracks/tr_0", "the master track tr_0 is missing");
            return;
        }
        if (master->parent) fail(at("tracks", kMaster, "parent"), "the master has no parent");
        for (const auto& e : s.tracks) {
            const Track& t = *e.value;
            if (t.type != StripType::Master) {
                if (!t.parent) {
                    fail(at("tracks", e.id, "parent"), e.id.str() + " has no parent");
                } else if (const Track* p = s.tracks.find(*t.parent); !p) {
                    fail(at("tracks", e.id, "parent"),
                         "parent " + t.parent->str() + " does not exist");
                } else if (std::count(p->children.begin(), p->children.end(), e.id) != 1) {
                    fail(at("tracks", e.id, "parent"),
                         t.parent->str() + " does not list " + e.id.str() + " exactly once");
                }
            }
            std::set<Id> seen;
            for (Id c : t.children) {
                if (!seen.insert(c).second)
                    fail(at("tracks", e.id, "children"), "duplicate child " + c.str());
                const Track* child = s.tracks.find(c);
                if (!child)
                    fail(at("tracks", e.id, "children"), "child " + c.str() + " does not exist");
                else if (child->parent != e.id)
                    fail(at("tracks", e.id, "children"), c.str() + " names another parent");
            }
            // Walk to a master root, with a bound to reject parent cycles.
            std::size_t steps = 0;
            std::optional<Id> cur = e.id;
            while (cur && steps <= s.tracks.size()) {
                const Track* x = s.tracks.find(*cur);
                if (x && x->type == StripType::Master) break;
                cur = x ? x->parent : std::nullopt;
                ++steps;
            }
            if (!cur || steps > s.tracks.size())
                fail(at("tracks", e.id, "parent"), e.id.str() + " is not under a master");
            if (t.record.input && !s.routes.contains(*t.record.input))
                fail(at("tracks", e.id, "record"),
                     "record input " + t.record.input->str() + " does not exist");
        }
    }

    void itemsAndTakes() {
        std::map<Id, Id> takeOwner;
        for (const auto& e : s.items) {
            const Item& it = *e.value;
            if (!s.tracks.contains(it.track))
                fail(at("items", e.id, "track"), "track " + it.track.str() + " does not exist");
            for (Id t : it.takes) {
                if (!s.takes.contains(t))
                    fail(at("items", e.id, "takes"), "take " + t.str() + " does not exist");
                auto [pos, fresh] = takeOwner.emplace(t, e.id);
                if (!fresh)
                    fail(at("items", e.id, "takes"),
                         t.str() + " also belongs to " + pos->second.str());
            }
            if (it.activeTake &&
                std::find(it.takes.begin(), it.takes.end(), *it.activeTake) == it.takes.end())
                fail(at("items", e.id, "active_take"),
                     it.activeTake->str() + " is not one of the item's takes");
            if (!it.takes.empty() && !it.activeTake)
                fail(at("items", e.id, "active_take"), "an item with takes needs an active take");
            if (!(it.length >= 0)) fail(at("items", e.id, "length"), "negative length");
        }
        for (const auto& e : s.takes) {
            if (!takeOwner.count(e.id)) fail(at("takes", e.id), e.id.str() + " belongs to no item");
            const Take& t = *e.value;
            if (t.source) {
                const bool ok = (t.source->kind == Kind::Media && s.media.contains(*t.source)) ||
                                (t.source->kind == Kind::Midi && s.midi.contains(*t.source));
                if (!ok)
                    fail(at("takes", e.id, "source"),
                         "source " + t.source->str() + " does not exist");
            }
        }
    }

    void fx() {
        std::map<Id, std::string> owner;
        auto claim = [&](const std::vector<Id>& chain, const std::string& where) {
            for (Id f : chain) {
                if (!s.fx.contains(f)) {
                    fail(where, "fx " + f.str() + " does not exist");
                    continue;
                }
                auto [pos, fresh] = owner.emplace(f, where);
                if (!fresh) fail(where, f.str() + " is also in " + pos->second);
            }
        };
        for (const auto& e : s.tracks) {
            claim(e.value->chain, at("tracks", e.id, "chain"));
            claim(e.value->inputChain, at("tracks", e.id, "input_chain"));
        }
        for (const auto& e : s.takes) claim(e.value->chain, at("takes", e.id, "chain"));
        for (const auto& e : s.fx)
            if (!owner.count(e.id)) fail(at("fx", e.id), e.id.str() + " is in no chain");
    }

    void routes() {
        std::map<Id, std::vector<Id>> edges;  // track -> tracks it feeds
        for (const auto& e : s.tracks)
            if (e.value->parent && e.value->parentSend.enabled)
                edges[e.id].push_back(*e.value->parent);
        for (const auto& e : s.routes) {
            const Route& r = *e.value;
            auto endOk = [&](const RouteEnd& x, const char* field, bool from) {
                const int n = (x.track ? 1 : 0) + (x.hwChannels ? 1 : 0) + (x.midiDevice ? 1 : 0) +
                              (x.fx ? 1 : 0);
                if (n != 1)
                    fail(at("routes", e.id, field),
                         "exactly one of track, fx, hw_channels, midi_device");
                if (!from && x.midiDevice)
                    fail(at("routes", e.id, field), "a route cannot send to a MIDI input");
                if (from && x.fx)
                    fail(at("routes", e.id, field), "a route cannot come from a plug-in's key");
                if (x.track && !s.tracks.contains(*x.track))
                    fail(at("routes", e.id, field), "track " + x.track->str() + " does not exist");
                if (x.fx && !trackOfFx(s, *x.fx))
                    fail(at("routes", e.id, field),
                         x.fx->str() + " is not in a track's FX chain (a key route needs one)");
            };
            endOk(r.from, "from", true);
            endOk(r.to, "to", false);
            const std::optional<Id> target = r.to.fx ? trackOfFx(s, *r.to.fx) : r.to.track;
            if (r.from.track && target) edges[*r.from.track].push_back(*target);
        }
        // Cycle detection (iterative DFS with colours).
        std::map<Id, int> colour;
        for (const auto& [start, unused] : edges) {
            (void)unused;
            if (colour[start]) continue;
            std::vector<std::pair<Id, std::size_t>> stack{{start, 0}};
            colour[start] = 1;
            while (!stack.empty()) {
                auto& [node, i] = stack.back();
                auto it = edges.find(node);
                if (it == edges.end() || i >= it->second.size()) {
                    colour[node] = 2;
                    stack.pop_back();
                    continue;
                }
                const Id next = it->second[i++];
                if (colour[next] == 1) {
                    fail("/routes", "routing cycle through " + next.str());
                    return;
                }
                if (!colour[next]) {
                    colour[next] = 1;
                    stack.push_back({next, 0});
                }
            }
        }
    }

    void envelopes() {
        for (const auto& e : s.envelopes) {
            const Envelope& env = *e.value;
            const Id o = env.target.object;
            const bool ok =
                (o.kind == Kind::Track && s.tracks.contains(o)) ||
                (o.kind == Kind::Group && s.groups.contains(o) && s.groups.find(o)->vca) ||
                (o.kind == Kind::Route && s.routes.contains(o)) ||
                (o.kind == Kind::Fx && s.fx.contains(o)) ||
                (o.kind == Kind::Take && s.takes.contains(o));
            if (!ok) fail(at("envelopes", e.id, "target"), "target " + o.str() + " does not exist");
            for (std::size_t i = 1; i < env.points.size(); ++i)
                if (env.points[i].pos < env.points[i - 1].pos) {
                    fail(at("envelopes", e.id, "points"), "points are not sorted by position");
                    break;
                }
        }
        for (const auto& e : s.regions)
            if (e.value->end < e.value->start)
                fail(at("rulers/regions", e.id, "end"), "region ends before it starts");
        for (const auto& e : s.autoItems) {
            const AutomationItem& ai = *e.value;
            const Envelope* env = s.envelopes.find(ai.envelope);
            if (!env)
                fail(at("automation_items", e.id, "envelope"),
                     "envelope " + ai.envelope.str() + " does not exist");
            else if (env->target.object.kind == Kind::Take)
                fail(at("automation_items", e.id, "envelope"),
                     "automation items sit on track, route and plugin envelopes");
            if (!(ai.length > 0) || !(ai.sourceLength > 0))
                fail(at("automation_items", e.id, "length"), "lengths must be positive");
            for (std::size_t i = 1; i < ai.points.size(); ++i)
                if (ai.points[i].pos < ai.points[i - 1].pos) {
                    fail(at("automation_items", e.id, "points"), "points are not sorted");
                    break;
                }
        }
    }
};

}  // namespace

Status automationExpansionBudget(const Session& s) {
    constexpr std::size_t limit = 100000;
    std::map<Id, std::size_t> totals;
    for (const auto& e : s.envelopes) {
        if (e.value->points.size() > limit)
            return Error("automation.budget", "envelope exceeds 100000 expanded points",
                         "/envelopes/" + e.id.str() + "/points", "reduce the envelope points");
        totals[e.id] = e.value->points.size();
    }
    for (const auto& e : s.autoItems) {
        const auto& ai = *e.value;
        const double reps = ai.loop ? std::ceil(ai.length / ai.sourceLength) : 1;
        if (!std::isfinite(reps) || reps < 1 || reps > limit || ai.points.size() > limit)
            return Error("automation.budget",
                         "automation expansion exceeds 100000 repetitions/points",
                         "/automation_items/" + e.id.str(),
                         "increase source_length or reduce length/points");
        const auto cost = static_cast<std::size_t>(reps) * (ai.points.size() + 2) + 2;
        auto& total = totals[ai.envelope];
        if (cost > limit || total > limit - cost)
            return Error("automation.budget", "envelope exceeds 100000 expanded points",
                         "/automation_items/" + e.id.str(),
                         "reduce length/points or increase source_length");
        total += cost;
    }
    return {};
}

Status validateNumeric(const Session& s) {
    SUPPLE_TRY(validateAudioWarp(s, false));
    const auto validPlayrate = [](double rate, const std::string& path) -> Status {
        if (!std::isfinite(rate) || rate < kMinPlayrate || rate > kMaxPlayrate)
            return Error("playrate.range", "master playrate is outside 0.25..4.0", path,
                         "provide a finite playback ratio from 0.25 to 4.0");
        return {};
    };
    SUPPLE_TRY(validPlayrate(s.project->playrate.rate, "/project/playrate/rate"));
    if (s.project->historyBudgetBytes == 0)
        return Error("history.budget", "history byte budget must be positive",
                     "/project/history_budget_bytes", "use a positive estimated byte budget");
    auto position = [&](double value, Timebase tb, const std::string& path) {
        return tb == Timebase::Time ? checkedSeconds(s, value, path) : checkedBeat(s, value, path);
    };
    for (const auto& e : s.tempo)
        SUPPLE_TRY(checkedBeat(s, e.value->beat, "/tempo/points/" + e.id.str() + "/beat"));
    for (const auto& e : s.meters)
        SUPPLE_TRY(checkedBeat(s, s.timeMap->beatOfBar(e.value->bar),
                               "/tempo/meter/" + e.id.str() + "/bar"));
    const auto& render = s.project->render;
    if (render.isObject() && render.size())
        if (auto valid = schemaOf<Project>().validate(toJson(*s.project)); !valid) {
            Error error = valid.error();
            auto prefix = [](auto&& self, Error& e) -> void {
                e.path = "/project" + e.path;
                for (auto& child : e.details) self(self, child);
            };
            prefix(prefix, error);
            return error;
        }
    if (!render.isObject())
        return Error("schema.type", "render must be an object", "/project/render");
    for (const auto& e : s.media) {
        const auto& md = *e.value;
        const std::string p = "/sources/media/" + e.id.str();
        if (!std::isfinite(md.sampleRate) || md.sampleRate <= 0 || md.frames < 0)
            return numericError(p);
        SUPPLE_TRY(checkedSamples(
            static_cast<long double>(md.frames) / static_cast<long double>(md.sampleRate),
            s.project->sampleRate, p + "/frames"));
    }
    for (const auto& e : s.midi) {
        const auto& seq = e.value->sequence;
        const std::string p = "/sources/midi/" + e.id.str() + "/sequence";
        constexpr auto limit = std::numeric_limits<std::int64_t>::max() / 4;
        if (seq.ppq < 1 || seq.ppq > 32767 || seq.length < 0 || seq.length > limit)
            return numericError(p + "/length");
        for (std::size_t i = 0; i < seq.notes.size(); ++i) {
            const auto& n = seq.notes[i];
            if (n.start < 0 || n.length < 0 || n.start > limit || n.length > limit - n.start)
                return numericError(p + "/notes/" + std::to_string(i));
        }
        for (const auto& c : seq.controls)
            if (c.tick < 0 || c.tick > limit) return numericError(p + "/controls");
        for (const auto& x : seq.sysex)
            if (x.tick < 0 || x.tick > limit) return numericError(p + "/sysex");
    }
    std::map<Id, long double> eventCounts;
    for (const auto& e : s.items) {
        const auto& it = *e.value;
        const std::string p = "/items/" + e.id.str();
        if (!std::isfinite(it.position) || it.position < 0) return numericError(p + "/position");
        if (!std::isfinite(it.length) || it.length < 0 || !std::isfinite(it.position + it.length))
            return numericError(p + "/length");
        const auto span = itemSpan(s, it);
        SUPPLE_TRY(checkedBeat(s, span.startBeat, p + "/position"));
        SUPPLE_TRY(checkedBeat(s, s.timeMap->beatAt(span.endSec), p + "/length"));
        SUPPLE_TRY(checkedSeconds(s, span.startSec, p + "/position"));
        SUPPLE_TRY(checkedSeconds(s, span.endSec, p + "/length"));
        for (auto [name, fade] :
             {std::pair{"fade_in", it.fadeIn}, std::pair{"fade_out", it.fadeOut}}) {
            if (!std::isfinite(fade.length) || fade.length < 0 || !std::isfinite(fade.curve))
                return numericError(p + "/" + name);
            SUPPLE_TRY(checkedSeconds(s, span.startSec + fade.length, p + "/" + name));
        }
        for (Id tk : it.takes) {
            const auto* take = s.takes.find(tk);
            if (!take) continue;  // reference checker supplies the missing-object error
            const std::string tp = "/takes/" + tk.str();
            if (!std::isfinite(take->startOffset)) return numericError(tp + "/start_offset");
            if (!std::isfinite(take->playrate) || !(take->playrate > 0))
                return numericError(tp + "/playrate");
            const double pitchRate = std::exp2(take->pitch / 12);
            if (!std::isfinite(take->pitch) || !std::isfinite(pitchRate) || !(pitchRate > 0))
                return numericError(tp + "/pitch");
            if (!take->source) continue;
            if (take->source->kind == Kind::Media) {
                const auto* md = s.media.find(*take->source);
                if (!md) continue;
                const double speed = take->playrate * (take->preservePitch ? 1 : pitchRate);
                if (!std::isfinite(speed) || !(speed > 0)) return numericError(tp + "/playrate");
                SUPPLE_TRY(checkedSamples(take->startOffset / speed, s.project->sampleRate,
                                          tp + "/start_offset"));
                SUPPLE_TRY(checkedSamples(take->startOffset + (span.endSec - span.startSec) * speed,
                                          md->sampleRate, tp + "/playrate"));
                const long double variantFrames = static_cast<long double>(md->frames) *
                                                  static_cast<long double>(s.project->sampleRate) /
                                                  static_cast<long double>(md->sampleRate) /
                                                  static_cast<long double>(speed);
                if (!std::isfinite(variantFrames) || variantFrames > (std::int64_t{1} << 28))
                    return Error("media.budget", "take variant exceeds the in-memory frame budget",
                                 tp + "/playrate", "increase playrate or use a shorter source");
            } else if (const auto* src = s.midi.find(*take->source)) {
                const auto& seq = src->sequence;
                const double loop = static_cast<double>(seq.length) / seq.ppq;
                const double offset =
                    it.loop && loop > 0 ? std::fmod(take->startOffset, loop) : take->startOffset;
                const double content =
                    (s.timeMap->beatAt(span.endSec) - span.startBeat) * take->playrate + offset;
                if (!std::isfinite(content) || !std::isfinite(take->startOffset * seq.ppq) ||
                    std::abs(take->startOffset * seq.ppq) > static_cast<double>(INT64_MAX / 4))
                    return numericError(tp + "/start_offset");
                const double reps = it.loop && loop > 0 ? 1 + std::floor(content / loop) : 1;
                if (!std::isfinite(reps) || reps < 0 || reps >= std::numeric_limits<int>::max())
                    return numericError(p + "/length");
                // Planning materializes events: bound work, including the prior loop pass.
                auto& count = eventCounts[it.track];
                count += (static_cast<long double>(reps) + 1) *
                         (2.L * seq.notes.size() + seq.controls.size() + seq.sysex.size());
                if (count > 1000000 || reps > 1000000)
                    return Error("midi.budget", "MIDI expansion exceeds 1000000 events/passes",
                                 p + "/length",
                                 "shorten the item or increase the source loop length");
            }
        }
    }
    auto points = [&](const auto& pts, Timebase tb, const std::string& p) -> Status {
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto at = p + "/" + std::to_string(i);
            SUPPLE_TRY(position(pts[i].pos, tb, at + "/beat"));
            if (!std::isfinite(pts[i].value) || !std::isfinite(pts[i].tension) ||
                (i && !std::isfinite(pts[i].value - pts[i - 1].value)))
                return numericError(at + "/value");
        }
        return {};
    };
    auto curveOutput = [&](const Envelope* env, double value, const std::string& path) -> Status {
        if (!env || env->target.object.kind == Kind::Fx) return {};
        if (env->target.param == "playrate") return validPlayrate(value, path);
        if (env->target.param == "volume") {
            const double gain = value <= kSilenceDb ? 0 : std::pow(10, value / 20);
            if (!std::isfinite(gain) ||
                gain > static_cast<double>(std::numeric_limits<float>::max()))
                return numericError(path);
        } else if (env->target.param == "pitch") {
            const double speed = std::exp2(value / 12);
            if (!std::isfinite(speed) || !(speed > 0)) return numericError(path);
        }
        return {};
    };
    std::map<Id, std::pair<double, double>> curveBounds;
    for (const auto& e : s.envelopes) {
        SUPPLE_TRY(
            points(e.value->points, e.value->timebase, "/envelopes/" + e.id.str() + "/points"));
        auto& bounds = curveBounds[e.id];
        for (const auto& pt : e.value->points) {
            SUPPLE_TRY(
                curveOutput(e.value.get(), pt.value, "/envelopes/" + e.id.str() + "/points"));
            bounds.first = std::min(bounds.first, pt.value);
            bounds.second = std::max(bounds.second, pt.value);
        }
        if (!std::isfinite(bounds.second - bounds.first))
            return numericError("/envelopes/" + e.id.str() + "/points");
    }
    for (const auto& e : s.autoItems) {
        const auto& ai = *e.value;
        const std::string p = "/automation_items/" + e.id.str();
        SUPPLE_TRY(checkedBeat(s, ai.position, p + "/position"));
        if (!(ai.length > 0) || !(ai.sourceLength > 0)) return numericError(p + "/length");
        SUPPLE_TRY(checkedBeat(s, ai.position + ai.length, p + "/length"));
        SUPPLE_TRY(checkedBeat(s, ai.sourceLength, p + "/source_length"));
        SUPPLE_TRY(points(ai.points, Timebase::Beats, p + "/points"));
        const auto* envelope = s.envelopes.find(ai.envelope);
        if (envelope && envelope->target.object == kMaster && envelope->target.param == "playrate")
            for (const auto& pt : ai.points) SUPPLE_TRY(validPlayrate(pt.value, p + "/points"));
        if (!std::isfinite(ai.baseline) || !std::isfinite(ai.amplitude))
            return numericError(p + "/amplitude");
        auto& bounds = curveBounds[ai.envelope];
        auto include = [&](double value) -> Status {
            if (!std::isfinite(value)) return numericError(p + "/points");
            SUPPLE_TRY(curveOutput(s.envelopes.find(ai.envelope), value, p + "/points"));
            bounds.first = std::min(bounds.first, value);
            bounds.second = std::max(bounds.second, value);
            if (!std::isfinite(bounds.second - bounds.first)) return numericError(p + "/points");
            return {};
        };
        if (ai.points.empty()) SUPPLE_TRY(include(ai.baseline));
        for (const auto& pt : ai.points)
            SUPPLE_TRY(include(ai.baseline + ai.amplitude * (pt.value - ai.baseline)));
    }
    for (const auto& e : s.markers)
        SUPPLE_TRY(
            position(e.value->pos, e.value->timebase, "/rulers/markers/" + e.id.str() + "/pos"));
    for (const auto& e : s.regions) {
        const auto& r = *e.value;
        SUPPLE_TRY(position(r.start, r.timebase, "/rulers/regions/" + e.id.str() + "/start"));
        SUPPLE_TRY(position(r.end, r.timebase, "/rulers/regions/" + e.id.str() + "/end"));
    }
    return automationExpansionBudget(s);
}

Status checkInvariants(const Session& s) {
    SUPPLE_TRY(validateAudioWarp(s));
    SUPPLE_TRY(validateNumeric(s));
    Checker c{s, {}};
    if (!(s.project->sampleRate >= 8000 && s.project->sampleRate <= 384000))
        c.fail("/project/sample_rate", "sample rate out of range");
    if (auto strips = validateStripRules(s); !strips) {
        Error error = std::move(strips).error();
        error.code = "model.invariant";
        return error;
    }
    SUPPLE_TRY(validateGroups(s));
    c.counters();
    c.tracks();
    c.itemsAndTakes();
    c.fx();
    c.routes();
    c.envelopes();
    if (c.errors.empty()) return validateFrozenState(s);
    if (c.errors.size() == 1) return c.errors.front();
    Error e("model.invariant", std::to_string(c.errors.size()) + " invariant violations");
    e.details = std::move(c.errors);
    return e;
}

// --- diff ------------------------------------------------------------------------------------

namespace {

template <class T>
void diffMap(const IdMap<T>& a, const IdMap<T>& b, std::vector<ObjectDiff>& out) {
    if (a.sameStorage(b)) return;
    auto ia = a.begin(), ib = b.begin();
    while (ia != a.end() || ib != b.end()) {
        if (ib == b.end() || (ia != a.end() && ia->id < ib->id)) {
            out.push_back({Change::Op::Remove, ia->id, {}});
            ++ia;
        } else if (ia == a.end() || ib->id < ia->id) {
            out.push_back({Change::Op::Add, ib->id, {}});
            ++ib;
        } else {
            if (ia->value != ib->value) {
                const json::Value ja = toJson(*ia->value), jb = toJson(*ib->value);
                if (!(ja == jb)) {
                    std::vector<std::string> fields;
                    std::set<std::string> keys;
                    for (const auto& m : ja.asObject()) keys.insert(m.key);
                    for (const auto& m : jb.asObject()) keys.insert(m.key);
                    for (const auto& k : keys) {
                        const json::Value* x = ja.find(k);
                        const json::Value* y = jb.find(k);
                        if (!x || !y || !(*x == *y)) fields.push_back(k);
                    }
                    out.push_back({Change::Op::Update, ia->id, std::move(fields)});
                }
            }
            ++ia;
            ++ib;
        }
    }
}

}  // namespace

std::vector<ObjectDiff> diff(const Session& a, const Session& b) {
    std::vector<ObjectDiff> out;
    const auto from = toJson(*a.project), to = toJson(*b.project);
    std::vector<std::string> fields;
    for (const auto& member : from.asObject()) {
        const auto* after = to.find(member.key);
        if (!after || member.value != *after) fields.push_back(member.key);
    }
    for (const auto& member : to.asObject())
        if (!from.find(member.key)) fields.push_back(member.key);
    if (!fields.empty()) out.push_back({Change::Op::Update, kProject, std::move(fields)});
    // Kind order as in session-model §1 (tempo, rulers, tracks, items, …).
    diffMap(a.tempo, b.tempo, out);
    diffMap(a.meters, b.meters, out);
    diffMap(a.markers, b.markers, out);
    diffMap(a.regions, b.regions, out);
    diffMap(a.tracks, b.tracks, out);
    diffMap(a.groups, b.groups, out);
    diffMap(a.items, b.items, out);
    diffMap(a.takes, b.takes, out);
    diffMap(a.fx, b.fx, out);
    diffMap(a.media, b.media, out);
    diffMap(a.midi, b.midi, out);
    diffMap(a.routes, b.routes, out);
    diffMap(a.envelopes, b.envelopes, out);
    diffMap(a.autoItems, b.autoItems, out);
    return out;
}

std::optional<Id> trackOfFx(const Session& s, Id fx) {
    for (const auto& e : s.tracks)
        if (std::find(e.value->chain.begin(), e.value->chain.end(), fx) != e.value->chain.end())
            return e.id;
    return std::nullopt;
}

bool sendsTo(const Session& s, Id from, Id to) {
    for (const auto& e : s.routes) {
        const Route& r = *e.value;
        if (r.from.track != from || r.to.track != to || r.to.fx) continue;
        if (std::all_of(r.channels.from.begin(), r.channels.from.end(),
                        [](int c) { return c <= 2; }))
            return true;
    }
    return false;
}

namespace {
std::string_view labelPrefix(StripType type) {
    switch (type) {
        case StripType::Channel:
            return "ch_";
        case StripType::Buss:
            return "buss_";
        case StripType::Master:
            return "mstr_";
    }
    return "ch_";
}
}  // namespace

std::string trackLabel(const Session& s, Id track) {
    const Track* t = s.tracks.find(track);
    if (!t) return track.str();
    if (!t->name.empty()) return t->name;
    if (track == kMaster) return "Master";
    std::size_t place = 0;
    for (const auto& e : s.tracks) {
        if (e.value->type != t->type || e.id == kMaster) continue;
        ++place;
        if (e.id == track) break;
    }
    return std::string(labelPrefix(t->type)) + std::to_string(place);
}

std::string nextTrackName(const Session& s, StripType type) {
    std::set<std::string> taken;
    for (const auto& e : s.tracks) taken.insert(trackLabel(s, e.id));
    for (std::size_t n = 1;; ++n) {
        std::string name = std::string(labelPrefix(type)) + std::to_string(n);
        if (!taken.contains(name)) return name;
    }
}

std::optional<Id> mediaSourceWithHash(const Session& s, std::string_view hash) {
    if (hash.empty()) return std::nullopt;  // an unhashed source is not an identity
    for (const auto& e : s.media)
        if (e.value->hash == hash) return e.id;
    return std::nullopt;
}

}  // namespace supple::model

namespace supple::model {

Timebase itemTimebase(const Session& s, const Item& it) {
    if (it.timebase == ItemTimebase::Time) return Timebase::Time;
    if (it.timebase == ItemTimebase::Beats) return Timebase::Beats;
    if (it.timebase == ItemTimebase::BeatsPosition) return Timebase::BeatsPosition;
    if (const Track* tr = s.tracks.find(it.track)) {
        if (tr->timebase == TrackTimebase::Time) return Timebase::Time;
        if (tr->timebase == TrackTimebase::Beats) return Timebase::Beats;
        if (tr->timebase == TrackTimebase::BeatsPosition) return Timebase::BeatsPosition;
    }
    return s.project->timebase;
}

namespace {
ItemSpan spanAs(const time::TimeMap& tm, const Item& it, Timebase tb) {
    if (tb == Timebase::Time) return {tm.beatAt(it.position), it.position, it.position + it.length};
    const double startSec = tm.secondsAt(it.position);
    if (tb == Timebase::Beats)
        return {it.position, startSec, tm.secondsAt(it.position + it.length)};
    return {it.position, startSec, startSec + it.length};  // beats position, seconds length
}
void writeSpan(const time::TimeMap& tm, Item& it, Timebase tb, double startBeat, double endBeat) {
    if (tb == Timebase::Time) {
        it.position = tm.secondsAt(startBeat);
        it.length = tm.secondsAt(endBeat) - it.position;
    } else if (tb == Timebase::Beats) {
        it.position = startBeat;
        it.length = endBeat - startBeat;
    } else {
        it.position = startBeat;
        it.length = tm.secondsAt(endBeat) - tm.secondsAt(startBeat);
    }
}
}  // namespace

double itemLengthBeats(const Session& s, const Item& it) {
    const ItemSpan span = itemSpan(s, it);
    return s.timeMap->beatAt(span.endSec) - span.startBeat;
}

double itemEndBeat(const Session& s, const Item& it) {
    return s.timeMap->beatAt(itemSpan(s, it).endSec);
}

double lastItemBeat(const Session& s) {
    double end = 0;
    for (const auto& e : s.items) end = std::max(end, itemEndBeat(s, *e.value));
    return end;
}

void placeItem(const Session& s, Item& it, double startBeat, std::optional<double> lengthBeats) {
    const time::TimeMap& tm = *s.timeMap;
    const Timebase tb = itemTimebase(s, it);
    startBeat = std::max(0.0, startBeat);
    if (lengthBeats) {
        writeSpan(tm, it, tb, startBeat, startBeat + *lengthBeats);
        return;
    }
    // Keep the length in the item's own domain.
    if (tb == Timebase::Time)
        it.position = tm.secondsAt(startBeat);
    else
        it.position = startBeat;
}

void moveItem(const Session& s, Item& it, Id track, double startBeat) {
    const time::TimeMap& tm = *s.timeMap;
    const Timebase before = itemTimebase(s, it);
    const ItemSpan span = itemSpan(s, it);
    it.track = track;
    startBeat = std::max(0.0, startBeat);
    const double endBeat = before == Timebase::Beats
                               ? startBeat + (tm.beatAt(span.endSec) - span.startBeat)
                               : tm.beatAt(tm.secondsAt(startBeat) + (span.endSec - span.startSec));
    writeSpan(tm, it, itemTimebase(s, it), startBeat, endBeat);
}

void placeItemSeconds(const Session& s, Item& it, double startSec, double endSec) {
    const time::TimeMap& tm = *s.timeMap;
    writeSpan(tm, it, itemTimebase(s, it), tm.beatAt(startSec), tm.beatAt(endSec));
}

void retimeItem(const Session& s, Item& it, Timebase from, Timebase to) {
    const time::TimeMap& tm = *s.timeMap;
    const ItemSpan span = spanAs(tm, it, from);
    writeSpan(tm, it, to, span.startBeat, tm.beatAt(span.endSec));
}

ItemSpan itemSpan(const Session& s, const Item& it) {
    return spanAs(*s.timeMap, it, itemTimebase(s, it));
}

std::optional<Id> activePlayrateEnvelope(const Session& s) {
    for (const auto& e : s.envelopes) {
        const auto& env = *e.value;
        if (!env.active || env.target.object != kMaster || env.target.param != "playrate") continue;
        if (!env.points.empty()) return e.id;
        for (const auto& item : s.autoItems)
            if (item.value->envelope == e.id) return e.id;
    }
    return {};
}

Status validateRecordingPlayrate(const Session& s) {
    auto refused = [](std::string path) -> Status {
        return Error("record.playrate", "recording requires unity master playrate", std::move(path),
                     "set transport.playrate.set {rate: 1} and disable the master playrate "
                     "envelope with env.set {active: false} before recording");
    };
    if (s.project->playrate.rate != 1.0) return refused("/project/playrate/rate");
    if (const auto envelope = activePlayrateEnvelope(s)) {
        SUPPLE_TRY(automationExpansionBudget(s));
        for (const auto& pt : effectivePoints(s, *envelope))
            if (pt.value != 1.0) return refused("/envelopes/" + envelope->str() + "/points");
    }
    return {};
}

double pointsValueAt(const std::vector<EnvPoint>& pts, double pos, double fallback) {
    if (pts.empty()) return fallback;
    if (pos < pts.front().pos) return pts.front().value;
    if (pos >= pts.back().pos) return pts.back().value;
    const auto next = std::upper_bound(pts.begin(), pts.end(), pos,
                                       [](double b, const EnvPoint& p) { return b < p.pos; });
    const EnvPoint& b = *next;
    const EnvPoint& a = *(next - 1);
    if (b.pos <= a.pos) return b.value;
    return a.value +
           (b.value - a.value) * shapeCurve(a.shape, a.tension, (pos - a.pos) / (b.pos - a.pos));
}

std::vector<EnvPoint> effectivePoints(const Session& s, Id envelope) {
    if (!automationExpansionBudget(s)) return {};
    const Envelope* env = s.envelopes.find(envelope);
    if (!env) return {};
    std::vector<const AutomationItem*> items;
    for (const auto& e : s.autoItems)
        if (e.value->envelope == envelope) items.push_back(e.value.get());
    if (items.empty()) return env->points;
    std::stable_sort(
        items.begin(), items.end(),
        [](const AutomationItem* a, const AutomationItem* b) { return a->position < b->position; });
    const std::vector<EnvPoint>& under = env->points;
    std::vector<EnvPoint> out;
    // Automation items use beats regardless of the parent envelope's domain.
    // Expand/clip their repetitions in beats, then place their points in the
    // envelope domain. Underlying points and interpolation stay in that domain.
    auto position = [&](double beat) {
        return env->timebase == Timebase::Time ? s.timeMap->secondsAt(beat) : beat;
    };
    auto add = [&](double pos, double value, PointShape shape = PointShape::Linear,
                   double tension = 0.0) { out.push_back({position(pos), value, shape, tension}); };
    double prevEnd = -1e300;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const AutomationItem& ai = *items[k];
        const double start = std::max(ai.position, prevEnd);
        double end = ai.position + ai.length;
        if (k + 1 < items.size()) end = std::min(end, items[k + 1]->position);  // cut by the next
        if (!(end > start)) continue;
        const double fromUnder =
            prevEnd == -1e300 ? -std::numeric_limits<double>::infinity() : position(prevEnd);
        const double nativeStart = position(start), nativeEnd = position(end);
        for (const EnvPoint& p : under)  // the envelope between items
            if (p.pos > fromUnder && p.pos < nativeStart) out.push_back(p);
        if (!under.empty()) add(start, pointsValueAt(under, nativeStart, 0.0));
        const double src = ai.sourceLength;
        auto mapped = [&](double v) { return ai.baseline + ai.amplitude * (v - ai.baseline); };
        auto local = [&](double x) {
            return mapped(pointsValueAt(ai.points, std::min(x, src), ai.baseline));
        };
        const double span = end - ai.position;
        const int reps = ai.loop ? std::max(1, static_cast<int>(std::ceil(span / src - 1e-9))) : 1;
        for (int r = 0; r < reps; ++r) {
            const double base = ai.position + r * src;
            const double segEnd = ai.loop ? std::min(base + src, end) : end;
            if (segEnd <= start) continue;
            const double from = std::max(base, start);
            add(from, local(from - base));
            for (const EnvPoint& p : ai.points) {
                const double at = base + p.pos;
                if (at > from && at < segEnd && p.pos < src)
                    add(at, mapped(p.value), p.shape, p.tension);
                else if (at == from && p.pos < src) {  // the segment's first point keeps its shape
                    out.back().shape = p.shape;
                    out.back().tension = p.tension;
                }
            }
            add(segEnd, local(segEnd - base));
        }
        if (!under.empty()) add(end, pointsValueAt(under, nativeEnd, 0.0));  // back to the envelope
        prevEnd = end;
    }
    for (const EnvPoint& p : under)
        if (p.pos > position(prevEnd)) out.push_back(p);
    return out;
}

}  // namespace supple::model
