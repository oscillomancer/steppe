// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The immutable session (session-model §1–4): every object in ID-keyed maps,
// edited only through EditTxn, validated by invariants, diffable.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/idmap.h"
#include "supple/model/objects.h"
#include "supple/time/timeline.h"

namespace supple::model {

struct Blob {
    std::vector<std::uint8_t> data;
};

struct Session {
    std::shared_ptr<const Project> project = std::make_shared<const Project>();
    Counters next;  // next ID per kind (project.next_ids in the file)
    IdMap<TempoPointObj> tempo;
    IdMap<MeterObj> meters;
    std::shared_ptr<const time::TimeMap> timeMap = std::make_shared<const time::TimeMap>();
    IdMap<Track> tracks;
    IdMap<TrackGroup> groups;
    IdMap<Item> items;
    IdMap<Take> takes;
    IdMap<MediaSource> media;
    IdMap<MidiSource> midi;
    IdMap<FxInstance> fx;
    IdMap<Route> routes;
    IdMap<Envelope> envelopes;
    IdMap<AutomationItem> autoItems;
    IdMap<Marker> markers;
    IdMap<Region> regions;
    IdMap<Rack> racks;
    IdMap<RackModule> modules;
    IdMap<Cable> cables;
    std::map<std::string, std::shared_ptr<const Blob>> blobs;  // "sha256:<hex>"
    // Unknown top-level project sections (including extensions) survive
    // load/save; UI state belongs to the separate .ui.json sidecar.
    json::Value extraSections = json::Value::object();
    std::uint64_t version = 0;  // the version that produced this state

    // A new project: master track tr_0, 120 BPM at beat 0 (tp_1), 4/4 at
    // bar 1 (ts_1), counters past them.
    static Session create(Project project = {});
};

// An EditTxn's per-collection builders (created on first touch).
struct TxnBuilders {
    std::optional<IdMap<TempoPointObj>::Builder> tempo;
    std::optional<IdMap<MeterObj>::Builder> meters;
    std::optional<IdMap<Track>::Builder> tracks;
    std::optional<IdMap<TrackGroup>::Builder> groups;
    std::optional<IdMap<Item>::Builder> items;
    std::optional<IdMap<Take>::Builder> takes;
    std::optional<IdMap<MediaSource>::Builder> media;
    std::optional<IdMap<MidiSource>::Builder> midi;
    std::optional<IdMap<FxInstance>::Builder> fx;
    std::optional<IdMap<Route>::Builder> routes;
    std::optional<IdMap<Envelope>::Builder> envelopes;
    std::optional<IdMap<AutomationItem>::Builder> autoItems;
    std::optional<IdMap<Marker>::Builder> markers;
    std::optional<IdMap<Region>::Builder> regions;
    std::optional<IdMap<Rack>::Builder> racks;
    std::optional<IdMap<RackModule>::Builder> modules;
    std::optional<IdMap<Cable>::Builder> cables;
};

// Which map of the Session holds objects of type T, and their ID kind.
template <class T>
struct Traits;
#define SUPPLE_MODEL_TRAITS(T, K, member)                                                  \
    template <>                                                                            \
    struct Traits<T> {                                                                     \
        static constexpr Kind kind = Kind::K;                                              \
        static IdMap<T>& map(Session& s) { return s.member; }                              \
        static const IdMap<T>& map(const Session& s) { return s.member; }                  \
        static std::optional<IdMap<T>::Builder>& slot(TxnBuilders& b) { return b.member; } \
    };
SUPPLE_MODEL_TRAITS(TempoPointObj, TempoPoint, tempo)
SUPPLE_MODEL_TRAITS(MeterObj, Meter, meters)
SUPPLE_MODEL_TRAITS(Track, Track, tracks)
SUPPLE_MODEL_TRAITS(TrackGroup, Group, groups)
SUPPLE_MODEL_TRAITS(Item, Item, items)
SUPPLE_MODEL_TRAITS(Take, Take, takes)
SUPPLE_MODEL_TRAITS(MediaSource, Media, media)
SUPPLE_MODEL_TRAITS(MidiSource, Midi, midi)
SUPPLE_MODEL_TRAITS(FxInstance, Fx, fx)
SUPPLE_MODEL_TRAITS(Route, Route, routes)
SUPPLE_MODEL_TRAITS(Envelope, Envelope, envelopes)
SUPPLE_MODEL_TRAITS(AutomationItem, AutoItem, autoItems)
SUPPLE_MODEL_TRAITS(Marker, Marker, markers)
SUPPLE_MODEL_TRAITS(Region, Region, regions)
SUPPLE_MODEL_TRAITS(Rack, Rack, racks)
SUPPLE_MODEL_TRAITS(RackModule, Module, modules)
SUPPLE_MODEL_TRAITS(Cable, Cable, cables)
#undef SUPPLE_MODEL_TRAITS

// One object-level change made by a transaction.
struct Change {
    enum class Op : std::uint8_t { Add, Remove, Update };
    Op op;
    Id id;
    friend bool operator==(const Change&, const Change&) = default;
};

// The only way to build a new session version. Reads see the transaction's
// own edits. IDs come from the session's counters; every touched object is
// stamped with the new version as its revision. commit() validates the
// invariants (a violation means a command let a bad edit through: a bug).
class EditTxn {
  public:
    EditTxn(const Session& base, std::uint64_t version);

    const Session& base() const { return base_; }
    std::uint64_t version() const { return version_; }

    template <class T>
    const T* get(Id id) const {
        return builder<T>().find(id);
    }
    template <class T>
    bool exists(Id id) const {
        return get<T>(id) != nullptr;
    }
    // Creates an object with the next ID of its kind.
    template <class T>
    Id create(T value) {
        const Id id{Traits<T>::kind, next_[Traits<T>::kind]++};
        builder<T>().put(id, version_, std::make_shared<const T>(std::move(value)));
        record(Change::Op::Add, id);
        created_.push_back(id);
        return id;
    }
    // Adds an object under an explicit ID (the master, loaders, merge replay);
    // false if the ID is taken. Counters move past it.
    template <class T>
    bool insert(Id id, T value) {
        auto& b = builder<T>();
        if (id.kind != Traits<T>::kind || b.find(id)) return false;
        b.put(id, version_, std::make_shared<const T>(std::move(value)));
        if (next_[id.kind] <= id.n) next_[id.kind] = id.n + 1;
        record(Change::Op::Add, id);
        created_.push_back(id);
        return true;
    }
    // Replaces an existing object (false if it does not exist).
    template <class T>
    bool put(Id id, T value) {
        auto& b = builder<T>();
        if (!b.find(id)) return false;
        b.put(id, version_, std::make_shared<const T>(std::move(value)));
        record(Change::Op::Update, id);
        return true;
    }
    // Copy, modify, store: txn.modify<Track>(id, [](Track& t) { t.mute = true; }).
    template <class T, class F>
    bool modify(Id id, F&& f) {
        const T* cur = get<T>(id);
        if (!cur) return false;
        T copy = *cur;
        f(copy);
        return put<T>(id, std::move(copy));
    }
    template <class T>
    bool remove(Id id) {
        if (!builder<T>().erase(id)) return false;
        record(Change::Op::Remove, id);
        if constexpr (std::is_same_v<T, Envelope>) removeItemsOn(id);  // its automation items
        return true;
    }

    const Project& project() const { return *project_; }
    void setProject(Project p);
    bool projectChanged() const { return projectChanged_; }
    void putBlob(const std::string& hash, std::shared_ptr<const Blob> blob);

    // Changes so far, coalesced per object (add+update = add, add+remove = none).
    std::vector<Change> changes() const;
    // IDs created so far, in creation order (removed ones included: replay
    // merges match creations by order).
    const std::vector<Id>& created() const { return created_; }
    // The session as it would be committed now, without validation (queries
    // inside a batch read their earlier steps' effects through this).
    Session preview() const;

    // Validates and returns the new session. The transaction is spent.
    Result<Session> commit();

  private:
    void removeItemsOn(Id envelope);
    template <class T>
    typename IdMap<T>::Builder& builder() const {
        auto& slot = Traits<T>::slot(builders_);
        if (!slot) slot.emplace(Traits<T>::map(base_));
        return *slot;
    }
    void record(Change::Op op, Id id);

    Session base_;
    std::uint64_t version_;
    Counters next_;
    std::shared_ptr<const Project> project_;
    std::map<std::string, std::shared_ptr<const Blob>> blobs_;
    mutable TxnBuilders builders_;
    std::map<Id, Change::Op> changes_;
    std::vector<Id> created_;
    bool committed_ = false;
    bool projectChanged_ = false;
};

// --- invariants and diffs --------------------------------------------------------

// Checks every structural invariant (session-model §3): the track tree is
// rooted at project.masters, every reference resolves, active takes belong to their
// items, no route/parent-send cycles, each fx in exactly one chain, envelope
// points sorted, counters past every used ID, tempo point at beat 0 and meter
// at bar 1. Errors: code "model.invariant" with a path like "/tracks/tr_3".
Status checkInvariants(const Session& s);

// Edit-time strip/routing rules, with actionable track.strip_type and
// route.strip_type errors. Also checked by the invariant checker.
Status validateStripRules(const Session& s);

// The track whose FX chain holds `fx` (not input chains or take FX), if any.
std::optional<Id> trackOfFx(const Session& s, Id fx);
// The plugin module that holds `fx` (ADR-0062), if any.
std::optional<Id> moduleOfFx(const Session& s, Id fx);
// The rack a cable end is in: its module's, or for a plug-in's parameter its
// plugin module's.
std::optional<Id> rackOfEnd(const Session& s, const CableEnd& end);
// What the input menu calls an Out module (rack.md §8): its label, else
// "<rack name> Out N" numbered among its rack's Out modules in id order.
std::string moduleLabel(const Session& s, Id module);
// What a rack's tab says: its name, else its home track's label.
std::string rackLabel(const Session& s, Id rack);
// Whether `from` already sends its main pair to the strip `to` (a route
// between the two tracks from channels 1/2, not to a plug-in's key): the
// GUI's send drags add at most one. Sends from auxiliary pairs do not count.
bool sendsTo(const Session& s, Id from, Id to);

// What every view calls a track: its name, or for an unnamed one a numbered
// label by type ("ch_2", "buss_1", "mstr_1"; the first master, tr_0, is
// "Master"), numbered by its place among that type's tracks in id order.
std::string trackLabel(const Session& s, Id track);
// The name track.add gives a new strip of `type`: the first "ch_N", "buss_N" or
// "mstr_N" (N from 1) that no track is labelled with.
std::string nextTrackName(const Session& s, StripType type);

// The media source holding the bytes `hash` identifies ("sha256:<hex>"), if the
// session has one: a file added twice is one source, not two.
std::optional<Id> mediaSourceWithHash(const Session& s, std::string_view hash);

// The points an envelope plays (gap log #28): its own, with each automation
// item on it replacing the span it covers (looped, scaled; a later item cuts
// an earlier one it overlaps), a step at each edge. Absolute beats.
// Refuses expansions beyond 100k repetitions/points per envelope.
Status automationExpansionBudget(const Session& s);
std::vector<EnvPoint> effectivePoints(const Session& s, Id envelope);
// An envelope's value at `pos` from sorted points with their segment shapes
// (`fallback` without points).
double pointsValueAt(const std::vector<EnvPoint>& points, double pos, double fallback);

// Active master-rate envelope with effective points (including automation items),
// or none. Model thread only; the audio engine uses a compiled curve instead.
std::optional<Id> activePlayrateEnvelope(const Session& s);
// Recording currently requires unity scalar and effective master-rate curve.
// Used before creating files and before publishing any edit/history transition.
Status validateRecordingPlayrate(const Session& s);

// Where an item sits after resolving its timebase (item, then track, then
// project): its start in beats and seconds and its end in seconds. The
// engine plays and the GUI draws items through this one mapping.
struct ItemSpan {
    double startBeat = 0, startSec = 0, endSec = 0;
};
ItemSpan itemSpan(const Session& s, const Item& item);
// The timebase an item follows: its own, else its track's, else the project's.
Timebase itemTimebase(const Session& s, const Item& item);
// Its length in beats at the current tempo map (end beat - start beat).
double itemLengthBeats(const Session& s, const Item& item);
double itemEndBeat(const Session& s, const Item& item);
// The last item end, in beats (0 without items).
double lastItemBeat(const Session& s);
// Writes an item's position (and length) in its own domain so that it starts
// at `startBeat` — and lasts `lengthBeats`, when given; otherwise it keeps
// its length in that domain (beats for `beats`, seconds for `time` and
// `beats_position`). `item.track` and `item.timebase` must be set first.
void placeItem(const Session& s, Item& item, double startBeat,
               std::optional<double> lengthBeats = std::nullopt);
// Moves an item to `track` (may be its own) to start at `startBeat`, keeping
// its length as its timebase defines it before the move (beats for `beats`,
// seconds otherwise), even when the new track's timebase differs.
void moveItem(const Session& s, Item& item, Id track, double startBeat);
// Places it from seconds (a recording): start and end on the timeline.
void placeItemSeconds(const Session& s, Item& item, double startSec, double endSec);
// Changes what an item follows, keeping it where it is now (start and end).
// `track`/`project`: the timebase the item would inherit after the change
// (callers changing a track's or the project's timebase pass the new one).
void retimeItem(const Session& s, Item& item, Timebase from, Timebase to);

// Object-level differences, deterministic order (kind, then ID). `fields`
// lists the changed top-level fields of updated objects (JSON member names).
struct ObjectDiff {
    Change::Op op;
    Id id;
    std::vector<std::string> fields;
};
std::vector<ObjectDiff> diff(const Session& a, const Session& b);

}  // namespace supple::model
