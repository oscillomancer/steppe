// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mix-value edits on the running plan (ADR-0060) and automation write holds
// (ADR-0061): the main-thread side. The executors' side is syncMix (plan.cpp).
#include <algorithm>

#include "plan.h"
#include "rack_runtime.h"
#include "supple/engine/realtime.h"

namespace supple::engine {
namespace {

bool contains(const std::vector<model::EnvTarget>& targets, const model::EnvTarget& t) {
    return std::find(targets.begin(), targets.end(), t) != targets.end();
}

bool writing(model::EnvMode m) {
    return m == model::EnvMode::Touch || m == model::EnvMode::Latch ||
           m == model::EnvMode::LatchPreview || m == model::EnvMode::Write;
}

// Compares two persistent collections entry by entry: the same IDs in the
// same order, and `same(old, new)` for every entry whose object changed.
template <class T, class F>
bool entriesMatch(const model::IdMap<T>& a, const model::IdMap<T>& b, F&& same) {
    if (a.sameStorage(b)) return true;
    if (a.size() != b.size()) return false;
    for (auto x = a.begin(), y = b.begin(); x != a.end(); ++x, ++y) {
        if (x->id != y->id) return false;
        if (x->value != y->value && !same(*x->value, *y->value)) return false;
    }
    return true;
}

// Whether `b` differs from `a` only in what a running plan takes without
// compiling: strip levels and pans, send levels and pans that keep the send's
// encoding, VCA levels, and the points of held volume/pan envelopes in a
// writing mode. Every other field is compared whole, so a field added to one
// of these objects can never pass as a mix edit. `changed`: something did
// change; `stale` gains the held targets whose curves the plan no longer
// matches. Keep the collections in step with model::Session.
bool mixOnly(const model::Session& a, const model::Session& b,
             const std::vector<model::EnvTarget>& holds, bool& changed,
             std::vector<model::EnvTarget>& stale) {
    if (!(a.project == b.project || *a.project == *b.project)) return false;
    if (!a.tempo.sameStorage(b.tempo) || !a.meters.sameStorage(b.meters) ||
        !a.items.sameStorage(b.items) || !a.takes.sameStorage(b.takes) ||
        !a.media.sameStorage(b.media) || !a.midi.sameStorage(b.midi) || !a.fx.sameStorage(b.fx) ||
        !a.autoItems.sameStorage(b.autoItems) || !a.markers.sameStorage(b.markers) ||
        !a.regions.sameStorage(b.regions) || !a.racks.sameStorage(b.racks) ||
        !a.cables.sameStorage(b.cables) || a.blobs != b.blobs || a.extraSections != b.extraSections)
        return false;
    // A module's knobs reach its instance without a compile (RackPool::
    // applyParams); where it sits and what it is called are the window's.
    const bool modules =
        entriesMatch(a.modules, b.modules, [&](model::RackModule x, const model::RackModule& y) {
            changed |= x.params != y.params || x.x != y.x || x.y != y.y || x.label != y.label;
            x.params = y.params;
            x.x = y.x;
            x.y = y.y;
            x.label = y.label;
            return x == y;
        });
    if (!modules) return false;
    const bool tracks =
        entriesMatch(a.tracks, b.tracks, [&](model::Track x, const model::Track& y) {
            changed |= x.volumeDb != y.volumeDb || x.pan != y.pan;
            x.volumeDb = y.volumeDb;
            x.pan = y.pan;
            return x == y;
        });
    if (!tracks) return false;
    const bool groups =
        entriesMatch(a.groups, b.groups, [&](model::TrackGroup x, const model::TrackGroup& y) {
            if (x.vca && y.vca) {
                changed |= x.vca->volumeDb != y.vca->volumeDb;
                x.vca->volumeDb = y.vca->volumeDb;
            }
            return x == y;
        });
    if (!groups) return false;
    const bool routes =
        entriesMatch(a.routes, b.routes, [&](model::Route x, const model::Route& y) {
            // Only sends between strips are plan edges; a level of exactly 0 dB
            // (with nothing else set) is summed without an encoder (plan.cpp).
            if (!x.from.track || !(x.to.track || x.to.fx)) return false;
            if ((x.volumeDb != 0) != (y.volumeDb != 0)) return false;
            changed |= x.volumeDb != y.volumeDb || x.pan.percent != y.pan.percent;
            x.volumeDb = y.volumeDb;
            if (x.pan.percent && y.pan.percent) x.pan = y.pan;
            return x == y;
        });
    if (!routes) return false;
    return entriesMatch(a.envelopes, b.envelopes, [&](model::Envelope x, const model::Envelope& y) {
        const model::EnvTarget& t = y.target;
        const auto kind = t.object.kind;
        const bool mix = (kind == model::Kind::Track || kind == model::Kind::Route)
                             ? t.param == "volume" || t.param == "pan"
                             : kind == model::Kind::Group && t.param == "volume";
        if (!mix || !writing(y.mode) || !contains(holds, t)) return false;
        // A route envelope's presence decides its send's encoding.
        if (kind == model::Kind::Route && x.points.empty() != y.points.empty()) return false;
        x.points = y.points;
        if (!(x == y)) return false;
        changed = true;
        if (!contains(stale, t)) stale.push_back(t);
        return true;
    });
}

}  // namespace

bool Engine::loadMixValues(const model::Session& session, const std::filesystem::path& dir) {
    auto* p = latestPlan_.load(std::memory_order_acquire);
    if (!backend_ || !latest_ || !p || rateSeekTarget_ || rateLoopTarget_ || notificationReload_ ||
        rateDeferred_ || scalarPending_ || dir != p->projectDir)
        return false;
    // A hold can end in headChanged alongside an unrelated mix edit. Its
    // stale curve must be compiled before that edit may take the fast path.
    if (releasesStaleCurve()) return false;
    bool changed = false;
    std::vector<model::EnvTarget> stale = staleCurves_;
    if (!mixOnly(*latest_, session, writeHolds_, changed, stale) || !changed) return false;
    detail::publishMix(*p, session, writeHolds_);
    plugins_.racks().applyParams(session);
    holdsPending_ = false;
    staleCurves_ = std::move(stale);
    latest_ = std::make_shared<const model::Session>(session);
    ++mixEdits_;
    return true;
}

bool Engine::releasesStaleCurve() const {
    return std::any_of(staleCurves_.begin(), staleCurves_.end(),
                       [&](const auto& t) { return !contains(writeHolds_, t); });
}

void Engine::applyPendingHolds() {
    auto* p = latestPlan_.load(std::memory_order_acquire);
    if (!holdsPending_ || !p || !latest_ || rateDeferred_) return;
    detail::publishMix(*p, *latest_, writeHolds_);
    holdsPending_ = false;
}

bool Engine::setWriteHolds(std::vector<model::EnvTarget> holds, bool apply) {
    if (holds == writeHolds_) return false;
    writeHolds_ = std::move(holds);
    holdsPending_ = true;                   // until a plan carries them
    if (releasesStaleCurve()) return true;  // the released curve is stale: compile it
    if (apply) applyPendingHolds();
    return false;
}

}  // namespace supple::engine
