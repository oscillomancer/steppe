// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/offline.h"

#include "plan.h"
#include "stretch.h"
#include "supple/model/freeze.h"

#include "file_platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <optional>
#include <set>
#include <thread>

#include "supple/base/thread.h"
#include "supple/clap/semantic_params.h"
#include "supple/dsp/dsp.h"
#include "supple/model/numeric.h"
#include "supple/params/params.h"
#include "supple/time/timeline.h"

namespace supple::engine {

using model::Id;
using model::Kind;
using model::Session;

namespace {
// Keep the target and precisely the tracks whose enabled sends feed it.
Session upstreamSession(const Session& source, Id target) {
    if (target == model::kMaster) return source;
    Session s = source;
    std::set<Id> keep{target};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& entry : source.tracks) {
            const auto& track = *entry.value;
            if (track.parentSend.enabled && track.parent && keep.count(*track.parent))
                changed |= keep.insert(entry.id).second;
        }
        for (const auto& entry : source.routes) {
            const auto& route = *entry.value;
            const auto to = route.to.fx ? model::trackOfFx(source, *route.to.fx) : route.to.track;
            if (route.from.track && to && keep.count(*to))
                changed |= keep.insert(*route.from.track).second;
        }
    }
    const bool masterFeedsTarget = keep.count(model::kMaster) != 0;
    keep.insert(model::kMaster);
    // A target's sends toward the master are irrelevant to its tap. Retain a
    // harmless root connection so the compiler still orders the master last.
    model::IdMap<model::Track>::Builder tracks(source.tracks);
    for (const auto& entry : source.tracks) {
        if (!keep.count(entry.id)) {
            tracks.erase(entry.id);
            continue;
        }
        if (entry.id == model::kMaster && !masterFeedsTarget) {
            auto master = std::make_shared<model::Track>(*entry.value);
            master->chain.clear();
            master->mute = false;
            tracks.put(entry.id, entry.rev, std::move(master));
        } else if (entry.value->parent && !keep.count(*entry.value->parent)) {
            auto track = std::make_shared<model::Track>(*entry.value);
            track->parent = model::kMaster;
            track->parentSend.enabled = true;
            tracks.put(entry.id, entry.rev, std::move(track));
        }
    }
    s.tracks = tracks.build();
    model::IdMap<model::Item>::Builder items(source.items);
    for (const auto& entry : source.items)
        if (!keep.count(entry.value->track) || entry.value->track == model::kMaster)
            items.erase(entry.id);
    s.items = items.build();
    model::IdMap<model::Route>::Builder routes(source.routes);
    for (const auto& entry : source.routes) {
        const auto to = entry.value->to.fx ? model::trackOfFx(source, *entry.value->to.fx)
                                           : entry.value->to.track;
        if (!entry.value->from.track || !to || !keep.count(*entry.value->from.track) ||
            !keep.count(*to))
            routes.erase(entry.id);
    }
    s.routes = routes.build();
    return s;
}
}  // namespace

// --- instance pool ------------------------------------------------------------------

InstancePool::InstancePool(Resolver resolve, host::HostIdentity identity)
    : resolve_(std::move(resolve)), identity_(std::move(identity)) {}

InstancePool::~InstancePool() = default;

void InstancePool::setMainThreadWake(MainThreadWake& wake) {
    mainWake_ = &wake;
    for (auto* instance : instances()) instance->setMainThreadWake(wake);
}
std::vector<host::Instance*> InstancePool::instances() const {
    std::vector<host::Instance*> out;
    for (const auto& [id, entry] : instances_)
        if (entry.instance) out.push_back(entry.instance.get());
    for (const auto& [generation, instance] : retired_) out.push_back(instance.get());
    return out;
}

Result<host::Instance*> InstancePool::get(Id id, const model::FxInstance& fx,
                                          const Session& session) {
    const bool isolated = fx.isolation == model::Isolation::SharedBridge ||
                          fx.isolation == model::Isolation::Dedicated;
    std::string desired;
    if (fx.patch)
        desired = "patch:" + json::writeCompact(*fx.patch);
    else if (fx.state)
        desired = "blob:" + *fx.state;
    auto& e = instances_[id];
    // A new state blob is loaded into a fresh instance when the current one
    // may be processing: CLAP's state.load runs on the main thread and a
    // plugin need not guard its DSP against a concurrent process() (semantic
    // patches are designed for live changes and stay in place).
    const bool freshForState = e.instance && !fx.patch && fx.state && desired != e.appliedState &&
                               e.instance->processing();
    // Undo can return to the just-added FX, before it had any saved settings.
    // No state document exists to load in that case; recreate its defaults,
    // keeping any processing instance alive until its old plan is retired.
    const bool freshForDefaults = e.instance && desired.empty() && !e.appliedState.empty();
    if (!e.instance || e.pluginId != fx.plugin.id || e.isolation != fx.isolation || freshForState ||
        freshForDefaults) {
        // The plan playing now may still use the old instance: keep it alive
        // until prune() says every plan that could use it is gone.
        if (e.instance) retire(std::move(e.instance));
        auto info = resolve_(fx.plugin);
        if (!info) {
            instances_.erase(id);
            return std::move(info).error();
        }
        std::shared_ptr<host::BridgeProcess> bridge;
        if (fx.isolation == model::Isolation::SharedBridge) {
            if (!sharedBridge_ || host::bridgeProcessFailed(sharedBridge_)) {
                auto spawned = host::createBridgeProcess();
                if (!spawned) {
                    instances_.erase(id);
                    return std::move(spawned).error();
                }
                sharedBridge_ = std::move(*spawned);
            }
            bridge = sharedBridge_;
        }
        auto made = host::Instance::create(*info, identity_, isolated, std::move(bridge));
        if (!made) {
            instances_.erase(id);
            return std::move(made).error();
        }
        e.instance = std::move(*made);
        if (mainWake_) e.instance->setMainThreadWake(*mainWake_);
        e.pluginId = fx.plugin.id;
        e.isolation = fx.isolation;
        e.appliedState.clear();
        e.failure.reset();
        e.clearedAutomation.clear();
    }
    if (desired != e.appliedState && !desired.empty()) {
        if (fx.patch) {
            SUPPLE_TRY(
                e.instance->setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
        } else {
            auto it = session.blobs.find(*fx.state);
            if (it == session.blobs.end())
                return Error("engine.blob_missing", id.str() + " refers to a missing state blob",
                             "/fx/" + id.str() + "/state",
                             "load a project/preset containing the missing FX state blob or "
                             "capture its state again");
            SUPPLE_TRY(e.instance->loadState(it->second->data));
        }
        e.appliedState = desired;
    }
    service(id);
    return e.instance.get();
}

void InstancePool::prune(const Session& session, std::uint64_t liveGeneration) {
    for (auto it = instances_.begin(); it != instances_.end();)
        it = session.fx.contains(it->first) && !model::frozenOwner(session, it->first)
                 ? std::next(it)
                 : instances_.erase(it);
    std::erase_if(retired_, [&](const auto& r) { return r.first <= liveGeneration; });
}

void InstancePool::retire(std::unique_ptr<host::Instance> instance) {
    // Its editor goes now, while the window it is embedded in still exists
    // (the UI reopens it on the replacement); the instance may play on.
    instance->closeEditor();
    retired_.emplace_back(generation_, std::move(instance));
}

void InstancePool::restart(Id id) {
    if (auto it = instances_.find(id); it != instances_.end()) {
        if (it->second.instance) retire(std::move(it->second.instance));
        instances_.erase(it);
    }
}

std::vector<std::pair<Id, std::string>> InstancePool::failed() const {
    std::vector<std::pair<Id, std::string>> out;
    for (const auto& [id, e] : instances_)
        if (e.failure)
            out.emplace_back(id, e.failure->message);
        else if (e.instance && e.instance->failed())
            out.emplace_back(id, e.instance->failure());
    return out;
}

host::Instance* InstancePool::find(Id id) const {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.instance.get();
}

host::Instance::Notifications InstancePool::service(Id id) {
    auto it = instances_.find(id);
    if (it == instances_.end() || !it->second.instance) return {};
    auto& entry = it->second;
    // A restart requested while processing happens here once it has stopped.
    entry.instance->serviceMainThread();
    auto notes = entry.instance->takeNotifications();
    entry.restarting = entry.instance->restartPending();
    if (entry.instance->active()) {
        const std::uint32_t latency = entry.instance->latency();
        notes.latencyChanged |= latency != entry.latency;
        entry.latency = latency;
    }
    if (!entry.instance->active() && !entry.failure && !notes.restartError &&
        entry.instance->processRequested() && entry.instance->sampleRate() > 0 &&
        entry.instance->maxFrames() > 0) {
        auto activated =
            entry.instance->activate(entry.instance->sampleRate(), 1, entry.instance->maxFrames());
        if (!activated) notes.restartError = activated.error();
    }
    if (notes.restartError) {
        entry.failure = notes.restartError;
        entry.failure->path = "/fx/" + id.str();
        entry.failure->hint = "Use fx.restart to recreate this instance from its saved state";
    }
    if (notes.paramsRescan) entry.clearedAutomation.clear();
    for (const auto& clear : notes.paramsClear)
        if (clear.flags & (CLAP_PARAM_CLEAR_ALL | CLAP_PARAM_CLEAR_AUTOMATIONS)) {
            if (std::find(entry.clearedAutomation.begin(), entry.clearedAutomation.end(),
                          clear.id) == entry.clearedAutomation.end())
                entry.clearedAutomation.push_back(clear.id);
        }
    return notes;
}

void InstancePool::rememberLatency(Id id, std::uint32_t latency) {
    if (auto it = instances_.find(id); it != instances_.end()) it->second.latency = latency;
}

bool InstancePool::restarting(Id id) const {
    auto it = instances_.find(id);
    return it != instances_.end() && it->second.restarting;
}

bool InstancePool::restarting(const host::Instance* instance) const {
    for (const auto& [id, e] : instances_)
        if (e.instance.get() == instance) return e.restarting;
    return false;
}

std::uint32_t InstancePool::rememberedLatency(Id id) const {
    auto it = instances_.find(id);
    return it == instances_.end() ? 0 : it->second.latency;
}

const Error* InstancePool::failure(Id id) const {
    auto it = instances_.find(id);
    return it != instances_.end() && it->second.failure ? &*it->second.failure : nullptr;
}

std::vector<std::pair<Id, Error>> InstancePool::failedErrors() const {
    std::vector<std::pair<Id, Error>> out;
    for (const auto& [id, entry] : instances_) {
        if (entry.failure)
            out.emplace_back(id, *entry.failure);
        else if (entry.instance && entry.instance->failed())
            out.emplace_back(
                id, Error("engine.bridge_failed", entry.instance->failure(), "/fx/" + id.str(),
                          "Use fx.restart to recreate this instance"));
    }
    return out;
}

bool InstancePool::automationCleared(Id id, std::uint32_t param) const {
    auto it = instances_.find(id);
    return it != instances_.end() &&
           std::find(it->second.clearedAutomation.begin(), it->second.clearedAutomation.end(),
                     param) != it->second.clearedAutomation.end();
}

void InstancePool::acknowledgeOutputPatch(Id id, const json::Value& patch) {
    if (auto it = instances_.find(id); it != instances_.end() && it->second.instance)
        it->second.appliedState = "patch:" + json::writeCompact(patch);
}

void InstancePool::acknowledgeOutputState(Id id, const std::string& hash) {
    if (auto it = instances_.find(id); it != instances_.end() && it->second.instance)
        it->second.appliedState = "blob:" + hash;
}

void InstancePool::resetAutomation(Id id) {
    if (auto it = instances_.find(id); it != instances_.end()) it->second.clearedAutomation.clear();
}

// --- media -----------------------------------------------------------------------------

namespace {
// One variant's samples can never be heard past the render limit, so refuse
// to build (and keep) a variant larger than that.
constexpr std::size_t kMaxVariantFrames = std::size_t{1} << 28;

Result<std::string> mediaIdentity(const std::filesystem::path& path, std::string_view hash) {
    if (!hash.empty()) return "hash:" + std::string(hash);
    const auto identity = detail::files::identity(path);
    if (!identity)
        return Error("engine.media_missing", "cannot inspect media " + path.string(), {},
                     "restore the source file or import an existing audio_file");
    return *identity;
}
}  // namespace

Result<std::shared_ptr<const audio::AudioData>> MediaCache::get(const std::filesystem::path& path,
                                                                double rate,
                                                                const MediaVariant& variant,
                                                                std::string_view contentHash) {
    // Never called by either executor: un-hashed media is stat'ed at plan
    // preparation, and immutable buffers are leased by each prepared plan.
    auto identity = mediaIdentity(path, contentHash);
    if (!identity) return identity.error();
    const bool stretch = variant.stretch();
    const bool pitched = stretch || !variant.preservePitch;
    const std::string key = *identity + "@" + json::formatDouble(rate) + (stretch ? "|s" : "|r") +
                            "|" + json::formatDouble(variant.playrate) + "|" +
                            json::formatDouble(pitched ? variant.pitch : 0.0);
    if (auto it = entries_.find(key); it != entries_.end()) return it->second;
    // Reuse originals only while something actually leases them. Prune on
    // variant misses so cache hits do not scan all live passthrough entries.
    std::erase_if(decoded_, [](const auto& entry) { return entry.second.expired(); });
    std::shared_ptr<const audio::AudioData> data;
    if (auto decoded = decoded_.find(*identity); decoded != decoded_.end())
        data = decoded->second.lock();
    if (!data) {
        auto read = audio::readAudio(path);
        if (!read) return std::move(read).error();
        // An external replacement during decode must not label new samples
        // with the old file identity. Leave cached/leased buffers untouched.
        if (contentHash.empty()) {
            auto after = mediaIdentity(path, {});
            if (!after) return after.error();
            if (*after != *identity)
                return Error("engine.media_changed",
                             "media changed while decoding " + path.string(), {},
                             "finish editing the source file, then reload the project or reimport "
                             "audio_file");
        }
        data = std::make_shared<const audio::AudioData>(std::move(*read));
        decoded_.insert_or_assign(*identity, data);
    }
    const double planRate = rate > 0 ? rate : data->sampleRate;
    const double targetRate = stretch ? planRate : planRate / variant.speed();
    const double frames = static_cast<double>(data->frames()) * (targetRate / data->sampleRate) /
                          (stretch ? variant.playrate : 1.0);
    if (!(frames <= static_cast<double>(kMaxVariantFrames)))
        return Error("engine.media_too_long",
                     "playing this media this slowly would need an unreasonable amount of memory",
                     {}, "use a playrate closer to 1");
    if (targetRate > 0 && data->sampleRate != targetRate) {
        auto resampled = audio::resample(*data, targetRate);
        if (!resampled) return std::move(resampled).error();
        data = std::make_shared<const audio::AudioData>(std::move(*resampled));
    }
    if (stretch) {
        auto stretched = stretchAudio(*data, variant.playrate, variant.pitch);
        if (!stretched) return std::move(stretched).error();
        data = std::make_shared<const audio::AudioData>(std::move(*stretched));
    }
    entries_.emplace(key, data);
    return data;
}

void MediaCache::trimVariants(std::size_t samples) {
    std::size_t retained = 0;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const auto count = entry->second->frames() * entry->second->channelCount();
        if (count > samples - std::min(samples, retained))
            entry = entries_.erase(entry);
        else {
            retained += count;
            ++entry;
        }
    }
    std::erase_if(decoded_, [](const auto& entry) { return entry.second.expired(); });
}

// --- mixing helpers ---------------------------------------------------------------------

std::pair<double, double> panGains(double pan, double panLawDb) {
    const double p = std::clamp(pan, -1.0, 1.0);
    const double angle = (p + 1.0) * std::numbers::pi / 4.0;
    // The sin/cos curve is constant power: 1/sqrt(2) at the centre, which is
    // what "-3 dB" means. Other laws scale it to their centre gain.
    const double centre =
        panLawDb == -3.0 ? std::numbers::sqrt2 / 2.0 : std::pow(10.0, panLawDb / 20.0);
    const double scale = centre * std::numbers::sqrt2;
    return {std::cos(angle) * scale, std::sin(angle) * scale};
}

using namespace detail;

Result<OfflineResult> renderOffline(const Session& s, InstancePool& pool,
                                    const OfflineSettings& opt) {
    static_assert(std::atomic<double>::is_always_lock_free);
    if (!(opt.endSeconds > opt.startSeconds))
        return Error("engine.range", "the render range is empty", {}, "end must be after start");
    if (opt.blockSize < 1 || opt.blockSize > 8192)
        return Error("engine.block_size", "block size out of range", {}, "1 .. 8192");
    if (!(opt.tailSeconds >= 0 && opt.tailSeconds <= 600))
        return Error("engine.tail", "tail out of range", {}, "0 .. 600 s");
    SUPPLE_TRY(model::checkedSeconds(s, opt.startSeconds, "/start_seconds"));
    SUPPLE_TRY(model::checkedSeconds(s, opt.endSeconds, "/end_seconds"));
    // Reject obvious expanded-output excess before constructing media/DSP state
    // or iterating the inverse clock. Envelopes get an optimistic 4x bound;
    // their exact duration is checked by the bounded inverse-clock prepass.
    const double fastest =
        opt.applyMasterPlayrate
            ? (model::activePlayrateEnvelope(s) ? model::kMaxPlayrate : s.project->playrate.rate)
            : 1.;
    const long double minimumFrames =
        ((static_cast<long double>(opt.endSeconds) - static_cast<long double>(opt.startSeconds)) /
             static_cast<long double>(fastest) +
         static_cast<long double>(opt.tailSeconds)) *
        static_cast<long double>(s.project->sampleRate);
    if (minimumFrames > (std::int64_t(1) << 28))
        return Error("engine.too_long", "playrate-expanded render exceeds the frame budget",
                     "/end_seconds", "shorten the timeline range or increase the master playrate");
    Session selectedSession = opt.track ? upstreamSession(s, *opt.track) : s;
    if (opt.throughMaster) {
        if (!s.tracks.contains(*opt.throughMaster) || *opt.throughMaster == model::kMaster)
            return Error("track.unknown", "through-master stem track does not exist", "/track",
                         "use an existing track id from track.list");
        // One exclusive solo uses precisely the playback compiler's signal-path
        // semantics, including nested parents and ordinary sends. Existing
        // solos/solo-defeat must not add other contributors to this stem.
        model::IdMap<model::Track>::Builder tracks(s.tracks);
        for (const auto& entry : s.tracks) {
            auto soloed = std::make_shared<model::Track>(*entry.value);
            soloed->solo = entry.id == *opt.throughMaster ? model::Solo::On : model::Solo::Off;
            tracks.put(entry.id, entry.rev, std::move(soloed));
        }
        selectedSession.tracks = tracks.build();
        model::IdMap<model::TrackGroup>::Builder groups(s.groups);
        for (const auto& entry : s.groups) {
            if (!entry.value->vca || !entry.value->vca->solo) continue;
            auto control = std::make_shared<model::TrackGroup>(*entry.value);
            control->vca->solo = false;
            groups.put(entry.id, entry.rev, std::move(control));
        }
        selectedSession.groups = groups.build();
    }
    // A stem of a channel or buss (isolated or through the master) and a
    // freeze capture leave the masters' own items out: they belong to no
    // channel (ADR-0057). A master's mix keeps them.
    if (opt.track || opt.throughMaster || opt.freezeTrack) {
        model::IdMap<model::Item>::Builder items(selectedSession.items);
        for (const auto& entry : selectedSession.items)
            if (const model::Track* owner = selectedSession.tracks.find(entry.value->track);
                owner && owner->type == model::StripType::Master)
                items.erase(entry.id);
        selectedSession.items = items.build();
    }
    auto compiled = detail::compile(selectedSession, pool, opt.startSeconds, opt.blockSize);
    if (!compiled) return std::move(compiled).error();
    Plan& p = *compiled;
    if (!opt.applyMasterPlayrate) p.rateEnabled = false;
    MediaCache localMedia;
    p.projectDir = opt.projectDir;
    p.media = opt.media ? opt.media : &localMedia;
    // Retain the full ownership context for every selected take's phase peers,
    // without decoding unrelated tracks (which may legitimately have missing media).
    Session sourceSession = s;
    std::set<Id> sourceTakes;
    for (const auto& entry : selectedSession.items)
        if (entry.value->activeTake) {
            sourceTakes.insert(*entry.value->activeTake);
            if (const auto* take = s.takes.find(*entry.value->activeTake))
                sourceTakes.insert(take->warpPeers.begin(), take->warpPeers.end());
        }
    model::IdMap<model::Item>::Builder sourceItems(s.items);
    for (const auto& entry : s.items) {
        if (selectedSession.items.contains(entry.id)) continue;
        const auto peer = std::find_if(entry.value->takes.begin(), entry.value->takes.end(),
                                       [&](Id take) { return sourceTakes.contains(take); });
        if (peer == entry.value->takes.end())
            sourceItems.erase(entry.id);
        else {
            auto item = std::make_shared<model::Item>(*entry.value);
            item->activeTake = *peer;
            sourceItems.put(entry.id, entry.rev, std::move(item));
        }
    }
    sourceSession.items = sourceItems.build();
    auto sources = p.media->prepareSources(sourceSession, p.projectDir, p.rate);
    if (!sources) return sources.error();
    p.preparedSources = *sources;
    SUPPLE_TRY(prepare(p, selectedSession));
    auto master = std::find_if(p.nodes.begin(), p.nodes.end(),
                               [&](const Node& node) { return node.id == opt.master; });
    if (master == p.nodes.end() || !master->master)
        return Error("track.strip_type", "mix output must be a master", "/master",
                     "choose a master id from track.list");
    const Node* selected = &*master;
    const auto selectedTrack = opt.freezeTrack ? opt.freezeTrack : opt.track;
    if (selectedTrack) {
        auto it = std::find_if(p.nodes.begin(), p.nodes.end(),
                               [&](const Node& node) { return node.id == *selectedTrack; });
        if (it == p.nodes.end())
            return Error("track.unknown", "track is absent from the render plan", "/track",
                         "use an existing track id from track.list");
        selected = &*it;
        // An analysis tap is isolated from the session's current solo state.
        for (Node& node : p.nodes) node.soloSilenced = false;
    }
    const auto outputLatency =
        selectedTrack
            ? selected->timelineShift + (opt.tap == model::Tap::PreFx ? 0 : selected->latChain)
            : selected->timelineShift + selected->latChain;
    const std::int64_t timelineLength =
        time::samplesAt(opt.endSeconds, p.rate) - time::samplesAt(opt.startSeconds, p.rate);
    if (opt.freezeProbe) {
        OfflineResult result;
        result.warnings = p.warnings;
        result.freezeLatency = selected->latChain;
        result.freezeShift = selected->timelineShift;
        result.outputLatency = static_cast<std::uint32_t>(outputLatency);
        return result;
    }
    auto reportedTail = [&] {
        auto chainTail = [](const std::vector<Stage>& chain) {
            std::int64_t sum = 0;
            for (const auto& stage : chain) {
                if (!stage.instance || stage.bypass) continue;
                const auto tail = stage.instance->tail();
                if (tail == UINT32_MAX) return std::int64_t(-1);
                sum += (std::int64_t(tail) + stage.os - 1) / stage.os;
            }
            return sum;
        };
        const auto trackTail = chainTail(selected->chain);
        if (trackTail < 0) return std::int64_t(-1);
        std::int64_t takeTail = 0;
        for (const auto& take : selected->takeChains) {
            const auto tail = chainTail(take.chain);
            if (tail < 0) return std::int64_t(-1);
            takeTail = std::max(takeTail, tail);
        }
        return trackTail + takeTail;
    };
    std::int64_t minimumTail = opt.freezeTail ? reportedTail() : 0;
    if (minimumTail < 0 || minimumTail > time::samplesAt(600., p.rate))
        return Error("freeze.tail_limit", "the track reports an infinite or excessive tail",
                     "/track", "disable infinite hold/feedback before freezing");
    const auto tailFrames =
        time::samplesAt(opt.freezeTail ? std::min(600., std::max(opt.freezeTailLimitSeconds,
                                                                 double(minimumTail) / p.rate + 1.))
                                       : opt.tailSeconds,
                        p.rate);
    std::int64_t contentFrames = timelineLength;
    if (p.rateEnabled) {
        // Count inverse-clock boundaries using bounded scratch/history. This
        // prepass never renders audio and never allocates an expanded timeline.
        auto& timeline = *p.timelines[0];
        std::int64_t device = 0;
        while (timeline.phase(device) < double(timelineLength)) {
            if (device + tailFrames + outputLatency > (std::int64_t(1) << 28))
                return Error("engine.too_long", "playrate-expanded render exceeds the frame budget",
                             {}, "shorten the timeline range or increase the master playrate");
            const auto end = device + 4096;
            timeline.ensure(end);
            if (timeline.phase(end) >= double(timelineLength)) {
                auto lo = device, hi = end;
                while (lo < hi) {
                    const auto mid = lo + (hi - lo) / 2;
                    if (timeline.phase(mid) < double(timelineLength))
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                device = lo;
                break;
            }
            device = end;
        }
        contentFrames = device;
    }
    const std::int64_t length = contentFrames + tailFrames;
    const std::int64_t total = length + outputLatency;
    if (total > (std::int64_t(1) << 28))
        return Error("engine.too_long", "render too long", {},
                     "shorten the render range or use a lower sample rate");
    const auto channels = opt.freezeTrack ? std::size_t(2 + selected->auxCount) : 2u;
    if (opt.freezeTrack && std::uint64_t(total) * channels > (std::uint64_t(1) << 28))
        return Error("render.budget", "multichannel capture exceeds the memory budget", "/track",
                     "reduce track channels or shorten the arrangement before freezing");
    std::vector<std::vector<float>> output(channels,
                                           std::vector<float>(static_cast<std::size_t>(total)));
    auto& outL = output[0];
    auto& outR = output[1];
    std::int64_t renderedFrames = total, quietFrames = 0;
    bool settled = !opt.freezeTail, invalidTail = false;
    const bool decodeStem = opt.track && opt.tap == model::Tap::PostFader && !selected->master;
    ConsoleStage<console9::Buss> stemDecoder;
    stemDecoder.init(console9::seedsFor(s.project->seed, (selected->id.n << 3) | 4),
                     p.linearConsole);
    bool failed = false, startFailed = false, cancelled = false;
    std::optional<Error> preparationError;

    // Every instance the plan processes (track and take chains), once each.
    const std::vector<host::Instance*> instances = p.instances;
    detail::Workers workers(opt.threads == 0 ? detail::Workers::defaultHelpers()
                                             : std::min(opt.threads, 64u) - 1);
    std::thread audio([&] {
        auto progressAt = std::chrono::steady_clock::time_point::min();
        nameThread("audio-offline");
        const auto primed = detail::prepareRateSources(p, 0, 0);
        if (!primed) {
            preparationError = primed.error();
            return;
        }
        for (host::Instance* in : instances) {
            in->reset();
            in->flushParams();
            if (!in->startProcessing()) startFailed = true;
        }
        if (!startFailed) {
            for (std::int64_t pos = 0; pos < total;) {
                if (opt.stopToken.stop_requested() ||
                    (opt.cancelFlag && opt.cancelFlag->load(std::memory_order_relaxed))) {
                    cancelled = true;
                    break;
                }
                auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(p.block, total - pos));
                if (p.rateEnabled) n = std::min(n, static_cast<std::uint32_t>(64 - (pos & 63)));
                if (!(workers.helpers() ? processBlockOn(workers, p, pos, n)
                                        : processBlock(p, pos, n))) {
                    failed = true;
                    break;
                }
                const Stereo& tap = opt.tap == model::Tap::PreFx      ? selected->preFx
                                    : opt.tap == model::Tap::PreFader ? selected->preFader
                                                                      : selected->post;
                std::copy_n(tap.l.begin(), n, outL.begin() + pos);
                std::copy_n(tap.r.begin(), n, outR.begin() + pos);
                if (decodeStem) {
                    dsp::DenormalGuard denormals;  // as the graph's own decode (ADR-0025)
                    for (std::uint32_t i = 0; i < n; ++i)
                        stemDecoder.sample(tap.l[i], tap.r[i],
                                           outL[static_cast<std::size_t>(pos) + i],
                                           outR[static_cast<std::size_t>(pos) + i]);
                }
                if (opt.freezeTrack)
                    for (std::size_t c = 2; c < channels; ++c)
                        std::copy_n(selected->aux[c - 2].begin(), n, output[c].begin() + pos);
                if (opt.freezeTail) {
                    const auto currentTail = reportedTail();
                    if (currentTail < 0 || currentTail > time::samplesAt(600., p.rate)) {
                        invalidTail = true;
                        break;
                    }
                    minimumTail = std::max(minimumTail, currentTail);
                    for (std::uint32_t i = 0; i < n; ++i) {
                        const auto at = pos + i;
                        if (at < contentFrames + outputLatency) continue;
                        bool quiet = true;
                        for (const auto& channel : output) {
                            const float sample = channel[static_cast<std::size_t>(at)];
                            quiet &= std::isfinite(sample) && std::abs(sample) < 1.e-6f;
                        }
                        quietFrames = quiet ? quietFrames + 1 : 0;
                        if (quietFrames >= time::samplesAt(1., p.rate) &&
                            at + 1 >= contentFrames + outputLatency + minimumTail) {
                            settled = true;
                            renderedFrames = at + 1;
                            break;
                        }
                    }
                }
                const double fraction = opt.progressBase + opt.progressScale *
                                                               static_cast<double>(pos + n) /
                                                               static_cast<double>(total);
                if (opt.progressFlag) opt.progressFlag->store(fraction, std::memory_order_relaxed);
                if (opt.progress &&
                    (pos + n == total || std::chrono::steady_clock::now() >= progressAt)) {
                    opt.progress(fraction);
                    progressAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
                pos += n;
                if (opt.freezeTail && settled) break;
            }
        }
        for (host::Instance* in : instances) in->stopProcessing();
    });
    audio.join();

    OfflineResult r;
    for (const auto& fx : s.fx) pool.service(fx.id);
    for (host::Instance* in : instances) {
        for (auto& line : in->takeLog()) r.log.push_back(in->info().name + ": " + line);
    }
    for (const auto& fx : s.fx)
        if (host::Instance* in = pool.find(fx.id); in && !in->failed())
            if (auto readouts = instanceReadouts(*in); readouts && readouts->size() > 0)
                r.meters[fx.id.str()] = std::move(*readouts);
    if (preparationError) return *preparationError;
    if (startFailed)
        return Error("engine.start_failed", "a plugin refused to start processing", {},
                     "restart or replace the FX that refused processing with a plugin supporting "
                     "the render sample rate and block size");
    if (cancelled)
        return Error("job.cancelled", "offline render cancelled", {},
                     "submit a new render job if its output is still wanted; the cancelled job "
                     "cannot resume");
    if (failed)
        return Error("engine.process_error", "a plugin reported a processing error", {},
                     "bypass or replace the failing FX and render the range again");
    for (const Node& n : p.nodes) {
        std::string line = n.id.str() + " in=[";
        for (std::size_t k = 0; k < n.in.size(); ++k) {
            const Edge& e = p.edges[n.in[k]];
            static constexpr const char* kTaps[] = {"post_fader", "pre_fader", "pre_fx"};
            char g[64];
            std::snprintf(g, sizeof g, " d%lld g%.3f/%.3f", static_cast<long long>(e.delay.length),
                          e.gainL, e.gainR);
            line += (k ? ", " : "") + p.nodes[e.from].id.str() + " " +
                    kTaps[static_cast<int>(e.tap)] + g;
        }
        line += "] lat_in=" + std::to_string(n.latIn) + " lat_chain=" + std::to_string(n.latChain);
        line += " timeline_shift=" + std::to_string(n.timelineShift);
        r.graph.push_back(std::move(line));
    }
    if (opt.freezeTail && (invalidTail || reportedTail() < 0))
        return Error("freeze.tail_limit", "the tail became infinite while rendering", "/track",
                     "disable infinite hold/feedback before freezing");
    if (opt.freezeTail && settled &&
        reportedTail() > renderedFrames - outputLatency - contentFrames)
        return Error("freeze.tail_changed", "a bridge reported a longer tail after rendering",
                     "/track", "use in-process isolation to capture a dynamically changing tail");
    if (!settled)
        return Error(tailFrames < time::samplesAt(600., p.rate) ? "freeze.tail_pending"
                                                                : "freeze.tail_limit",
                     "the tail did not settle within the render budget", "/track",
                     "reduce feedback or disable continuous sound before freezing");
    r.freezeLatency = selected->latChain;
    r.freezeShift = selected->timelineShift;
    r.freezeTailFrames = renderedFrames - outputLatency - contentFrames;
    r.outputLatency = static_cast<std::uint32_t>(outputLatency);
    r.warnings = std::move(p.warnings);
    for (const auto& node : p.nodes)
        forEachStage(node, [&](const Stage& st) {
            if (st.voiceDropped)
                r.warnings.push_back(st.fx.str() + ": voice tracking capacity exceeded: " +
                                     std::to_string(st.voiceDropped));
        });
    r.audio.sampleRate = p.rate;
    const auto skip = static_cast<std::ptrdiff_t>(outputLatency);
    for (auto& channel : output) {
        channel.resize(static_cast<std::size_t>(renderedFrames));
        channel.erase(channel.begin(), channel.begin() + skip);
    }
    r.audio.channels = std::move(output);
    return r;
}

Result<json::Value> instanceReadouts(host::Instance& in) {
    json::Value out = json::Value::object();
    if (in.info().semantic && !in.paramsNormalized()) {
        auto schema = in.semanticSchema();
        if (!schema) return std::move(schema).error();
        const auto* entries = schema->find("params");
        if (!entries || !entries->isObject()) return out;
        for (const auto& m : entries->asObject()) {
            auto described = params::fromDescription(m.key, m.value);
            if (!described || !((*described)->spec.flags & params::kReadonly)) continue;
            auto value = in.paramValue((*described)->spec.id);
            if (!value) return std::move(value).error();
            const auto& spec = (*described)->spec;
            const double physical = params::fromClap(spec, *value);
            out[m.key] = spec.type == params::Type::Float
                             ? json::Value(std::round(physical * 1e4) / 1e4)
                             : params::toJson(spec, physical);
        }
        return out;
    }
    for (const host::ParamInfo& p : in.params()) {
        if (!(p.flags & CLAP_PARAM_IS_READONLY) || (p.flags & CLAP_PARAM_IS_HIDDEN)) continue;
        auto value = in.paramValue(p.id);
        if (!value) return std::move(value).error();
        out[p.name] = *value;
    }
    return out;
}

}  // namespace supple::engine
