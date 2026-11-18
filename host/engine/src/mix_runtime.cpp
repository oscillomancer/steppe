// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mix-value edits on the running plan (ADR-0060), automation write holds
// (ADR-0061) and patch-only FX edits (ADR-0092): the main-thread side. The
// executors' side of mix values is syncMix (plan.cpp).
#include <algorithm>
#include <tuple>

#include "plan.h"
#include "rack_runtime.h"
#include "supple/engine/realtime.h"
#include "supple/model/freeze.h"

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
// compiling: strip levels, pans, widths and dual pans, send levels and pans that keep the send's
// encoding, VCA levels, FX wet and bypass, track and VCA mute and solo, route mutes that
// keep the send's encoding, and the points of held volume/pan envelopes in a
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
        !a.media.sameStorage(b.media) || !a.midi.sameStorage(b.midi) ||
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
    // An FX's wet and bypass (ADR-0093): every stage that runs it has a slot.
    // `offline` decides whether an instance exists, so it compiles.
    const bool fx = entriesMatch(a.fx, b.fx, [&](model::FxInstance x, const model::FxInstance& y) {
        changed |= x.wet != y.wet || x.bypass != y.bypass;
        x.wet = y.wet;
        x.bypass = y.bypass;
        return x == y;
    });
    if (!fx) return false;
    // Mute and solo (ADR-0093): mute is a slot; solo is published as each
    // node's silenced flag from model::soloSilencedTracks.
    const bool tracks =
        entriesMatch(a.tracks, b.tracks, [&](model::Track x, const model::Track& y) {
            changed |= x.volumeDb != y.volumeDb || x.pan != y.pan || x.width != y.width ||
                       x.panLeft != y.panLeft || x.panRight != y.panRight || x.mute != y.mute ||
                       x.solo != y.solo;
            x.mute = y.mute;
            x.solo = y.solo;
            x.volumeDb = y.volumeDb;
            x.pan = y.pan;
            x.width = y.width;  // pan positions (ADR-0066); mode and law compile
            x.panLeft = y.panLeft;
            x.panRight = y.panRight;
            return x == y;
        });
    if (!tracks) return false;
    const bool groups =
        entriesMatch(a.groups, b.groups, [&](model::TrackGroup x, const model::TrackGroup& y) {
            if (x.vca && y.vca) {
                changed |= x.vca->volumeDb != y.vca->volumeDb || x.vca->mute != y.vca->mute ||
                           x.vca->solo != y.vca->solo;
                x.vca->volumeDb = y.vca->volumeDb;
                x.vca->mute = y.vca->mute;
                x.vca->solo = y.vca->solo;
            }
            return x == y;
        });
    if (!groups) return false;
    const bool routes =
        entriesMatch(a.routes, b.routes, [&](model::Route x, const model::Route& y) {
            // A MIDI route is never encoded: its mute is a value.
            if (x.midi && !x.from.midiDevice) {
                changed |= x.mute != y.mute;
                x.mute = y.mute;
                return x == y;
            }
            // A rack send's level and mute (ADR-0075): it is never encoded,
            // so any level is a mix value (publishMix, syncMix).
            if (!x.midi && x.from.module && model::rackSendHome(a, x) &&
                model::rackSendHome(b, y)) {
                changed |= x.volumeDb != y.volumeDb || x.mute != y.mute;
                x.volumeDb = y.volumeDb;
                x.mute = y.mute;
                return x == y;
            }
            // Only sends between strips are plan edges; a level of exactly 0 dB
            // (with nothing else set) is summed without an encoder (plan.cpp).
            if (!x.from.track || !(x.to.track || x.to.fx)) return false;
            if ((x.volumeDb != 0) != (y.volumeDb != 0)) return false;
            // A mute decides a send's encoding unless something else encodes
            // it anyway (plan.cpp, Edge::encode): only then is it a value.
            const auto encoded = [](const model::Route& r) {
                return r.tap != model::Tap::PostFader || r.volumeDb != 0 || r.pan.percent ||
                       r.mono || r.phaseInvert;
            };
            if (x.mute != y.mute && !(x.to.track && encoded(x) && encoded(y))) return false;
            changed |=
                x.volumeDb != y.volumeDb || x.pan.percent != y.pan.percent || x.mute != y.mute;
            x.mute = y.mute;
            x.volumeDb = y.volumeDb;
            if (x.pan.percent && y.pan.percent) x.pan = y.pan;
            return x == y;
        });
    if (!routes) return false;
    return entriesMatch(a.envelopes, b.envelopes, [&](model::Envelope x, const model::Envelope& y) {
        const model::EnvTarget& t = y.target;
        const auto kind = t.object.kind;
        const bool mix = (kind == model::Kind::Track || kind == model::Kind::Route)
                             ? (t.param == "volume" || t.param == "pan" ||
                                (kind == model::Kind::Track &&
                                 (t.param == "width" || t.param == "pan_left" ||
                                  t.param == "pan_right" || t.param == "pan_boost")))
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

// Whether `b` differs from `a` only in FX patches, each FX keeping a patch:
// every other collection has the same storage, and each changed FX is
// compared whole with its patch taken from `b`, so a field added later can
// never pass as a patch edit. `changed` gains the FX whose patch differs.
// Keep the collections in step with model::Session.
bool patchOnly(const model::Session& a, const model::Session& b, std::vector<model::Id>& changed) {
    if (!(a.project == b.project || *a.project == *b.project)) return false;
    if (!a.tempo.sameStorage(b.tempo) || !a.meters.sameStorage(b.meters) ||
        !a.tracks.sameStorage(b.tracks) || !a.groups.sameStorage(b.groups) ||
        !a.items.sameStorage(b.items) || !a.takes.sameStorage(b.takes) ||
        !a.media.sameStorage(b.media) || !a.midi.sameStorage(b.midi) ||
        !a.routes.sameStorage(b.routes) || !a.envelopes.sameStorage(b.envelopes) ||
        !a.autoItems.sameStorage(b.autoItems) || !a.markers.sameStorage(b.markers) ||
        !a.regions.sameStorage(b.regions) || !a.racks.sameStorage(b.racks) ||
        !a.modules.sameStorage(b.modules) || !a.cables.sameStorage(b.cables) ||
        a.blobs != b.blobs || a.extraSections != b.extraSections)
        return false;
    if (a.fx.sameStorage(b.fx) || a.fx.size() != b.fx.size()) return false;
    for (auto x = a.fx.begin(), y = b.fx.begin(); x != a.fx.end(); ++x, ++y) {
        if (x->id != y->id) return false;
        if (x->value == y->value) continue;
        const model::FxInstance& to = *y->value;
        if (!x->value->patch || !to.patch) return false;
        model::FxInstance from = *x->value;
        const bool differs = *from.patch != *to.patch;
        from.patch = to.patch;
        if (!(from == to)) return false;
        if (differs) changed.push_back(y->id);
    }
    return true;
}

}  // namespace

bool Engine::loadPatches(const model::Session& session, const std::filesystem::path& dir) {
    auto* p = latestPlan_.load(std::memory_order_acquire);
    // The guards of loadMixValues: never while an edit is deferred, for the
    // reload that ends the deferral, or for a plug-in's notification reload.
    if (!backend_ || !latest_ || !p || rateSeekTarget_ || rateLoopTarget_ || notificationReload_ ||
        rateDeferred_ || rateDeferredFrom_ || scalarPending_ || dir != p->projectDir)
        return false;
    std::vector<model::Id> changed;
    if (!patchOnly(*latest_, session, changed) || changed.empty()) return false;
    // A compile resends every automated value at the new plan's first block
    // and lets automation the plug-in cleared play again (resetAutomation):
    // an FX with automation, or any cleared automation, compiles as before.
    if (plugins_.anyAutomationCleared()) return false;
    for (const model::Id id : changed) {
        // A frozen track's FX is in no plan; its instance waits to be pruned.
        if (model::frozenOwner(session, id) || !plugins_.patchable(id, *session.fx.find(id)))
            return false;
        if (detail::automated(session, id)) return false;
    }
    // From here the patches are applied. Whatever a plug-in reports that a
    // plan holds (latency, parameters, ports, activation, a restart) makes
    // this a compile, which finds those patches already applied. The patch
    // sequence is odd meanwhile: plug-in outputs of blocks begun before the
    // last patch reached its instance carry an older value, and the app
    // drops them as it drops a replaced plan's.
    patchSeq_.fetch_add(1);
    struct Even {
        std::atomic<std::uint64_t>& seq;
        ~Even() { seq.fetch_add(1); }
    } even{patchSeq_};
    for (const model::Id id : changed) {
        host::Instance& instance = *plugins_.find(id);
        const auto ports = [&] {
            return std::tuple{instance.audioInputChannels(), instance.audioOutputChannels(),
                              instance.acceptsNotes(),       instance.outputsNotes(),
                              instance.sidechainChannels(),  instance.auxOutputChannels()};
        };
        const auto before = ports();
        const bool active = instance.active();
        auto notes = plugins_.applyPatch(id, *session.fx.find(id), session);
        if (!notes) return false;  // the compile reports it
        if (notes->stateDirty &&
            std::find(dirtyStates_.begin(), dirtyStates_.end(), id) == dirtyStates_.end())
            dirtyStates_.push_back(id);
        // A rescan of values or their text, reported after a patch, is not one.
        if (detail::rescansParamList(*notes) || !notes->paramsClear.empty() ||
            notes->restartError || notes->latencyChanged || plugins_.restarting(id) ||
            active != instance.active() || before != ports())
            return false;
    }
    latest_ = std::make_shared<const model::Session>(session);
    ++patchEdits_;
    return true;
}

bool Engine::loadMixValues(const model::Session& session, const std::filesystem::path& dir) {
    auto* p = latestPlan_.load(std::memory_order_acquire);
    // As loadScalarRate: never while an edit is deferred or for the reload
    // that ends the deferral (review 37b F1).
    if (!backend_ || !latest_ || !p || rateSeekTarget_ || rateLoopTarget_ || notificationReload_ ||
        rateDeferred_ || rateDeferredFrom_ || scalarPending_ || dir != p->projectDir)
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
