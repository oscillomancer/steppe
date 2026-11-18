// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/offline.h"

#include "rack_runtime.h"

#include "plan.h"
#include "stretch.h"
#include "supple/model/freeze.h"

#include "file_platform.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
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
// The node a route leaves and the one it reaches: a track's, or for a
// module end its rack's (ADR-0062).
std::pair<std::optional<Id>, std::optional<Id>> routeNodes(const Session& s,
                                                           const model::Route& r) {
    auto rackOf = [&](Id module) -> std::optional<Id> {
        const model::RackModule* m = s.modules.find(module);
        return m ? std::optional<Id>(m->rack) : std::nullopt;
    };
    std::optional<Id> from = r.from.module ? rackOf(*r.from.module) : r.from.track;
    std::optional<Id> to = r.to.module ? rackOf(*r.to.module)
                           : r.to.fx   ? model::trackOfFx(s, *r.to.fx)
                                       : r.to.track;
    return {from, to};
}

// Keep the target and precisely the tracks (and racks) whose enabled sends
// feed it.
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
            const auto [from, to] = routeNodes(source, *entry.value);
            if (from && to && keep.count(*to)) changed |= keep.insert(*from).second;
            // A rack send follows its home channel's mute (ADR-0075).
            if (const auto home = model::rackSendHome(source, *entry.value);
                home && to && keep.count(*to))
                changed |= keep.insert(*home).second;
        }
        // A Notes source includes incoming sends, whose channel node must
        // remain available so its implicit reader edges can be compiled.
        for (const auto& entry : source.modules) {
            const auto& module = *entry.value;
            if (module.type != model::kNotesModule || module.sourceOff || !module.sourceSends ||
                !keep.count(module.rack))
                continue;
            const auto* rack = source.racks.find(module.rack);
            const auto track = module.source.value_or(rack ? rack->track : model::kMaster);
            changed |= keep.insert(track).second;
        }
    }
    // A kept rack's Notes modules still play their tracks' MIDI items.
    std::set<Id> midiSources;
    for (const auto& entry : source.modules) {
        const model::RackModule& m = *entry.value;
        if (m.type != model::kNotesModule || m.sourceOff || !keep.count(m.rack)) continue;
        if (m.source)
            midiSources.insert(*m.source);
        else if (const model::Rack* rack = source.racks.find(m.rack))
            midiSources.insert(rack->track);
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
        if ((!keep.count(entry.value->track) && !midiSources.count(entry.value->track)) ||
            entry.value->track == model::kMaster)
            items.erase(entry.id);
    s.items = items.build();
    model::IdMap<model::Route>::Builder routes(source.routes);
    for (const auto& entry : source.routes) {
        const auto [from, to] = routeNodes(source, *entry.value);
        if (!from || !to || !keep.count(*from) || !keep.count(*to)) routes.erase(entry.id);
    }
    s.routes = routes.build();
    model::IdMap<model::Rack>::Builder racks(source.racks);
    for (const auto& entry : source.racks)
        if (!keep.count(entry.id)) racks.erase(entry.id);
    s.racks = racks.build();
    return s;
}
}  // namespace

// --- instance pool ------------------------------------------------------------------

InstancePool::InstancePool(Resolver resolve, host::HostIdentity identity)
    : racks_(std::make_unique<detail::RackPool>()),
      resolve_(std::move(resolve)),
      identity_(std::move(identity)) {}

void InstancePool::setGeneration(std::uint64_t generation) {
    generation_ = generation;
    racks_->setGeneration(generation);
}

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

void InstancePool::setDefaultIsolation(model::Isolation isolation) {
    defaultIsolation_ =
        isolation == model::Isolation::Inherit ? model::Isolation::InProcess : isolation;
}

void InstancePool::setStockDir(std::filesystem::path dir) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(dir, ec);
    stockDir_ = std::optional<std::filesystem::path>(ec ? std::move(dir) : std::move(canonical));
}

model::Isolation InstancePool::resolveIsolation(const model::FxInstance& fx,
                                                const host::PluginInfo& info,
                                                bool* fallback) const {
    if (fallback) *fallback = false;
    if (fx.isolation != model::Isolation::Inherit) return fx.isolation;
    if (defaultIsolation_ == model::Isolation::InProcess) return model::Isolation::InProcess;
    if (!stockDir_) {
        std::error_code ec;
        auto dir = host::stockPluginDir();
        if (dir) {
            auto canonical = std::filesystem::weakly_canonical(*dir, ec);
            if (!ec) dir = std::move(canonical);
        }
        stockDir_ = std::move(dir);
    }
    if (*stockDir_ && !info.path.empty()) {
        std::error_code ec;
        const auto path = std::filesystem::weakly_canonical(info.path, ec);
        const auto inside = (ec ? info.path : path).lexically_relative(**stockDir_);
        if (!inside.empty() && *inside.begin() != "..") return model::Isolation::InProcess;
    }
    if (host::bridgeExecutable().empty()) {
        if (fallback) *fallback = true;
        return model::Isolation::InProcess;
    }
    return defaultIsolation_;
}

model::Isolation InstancePool::effectiveIsolation(Id id, const model::FxInstance& fx) const {
    if (auto it = instances_.find(id); it != instances_.end() && it->second.instance)
        return it->second.effective;
    if (fx.isolation != model::Isolation::Inherit) return fx.isolation;
    if (defaultIsolation_ == model::Isolation::InProcess) return model::Isolation::InProcess;
    auto info = resolve_(fx.plugin);
    if (!info) return model::Isolation::InProcess;  // nothing to make: nothing bridged
    return resolveIsolation(fx, *info, nullptr);
}

std::vector<model::Id> InstancePool::isolationFallbacks() const {
    std::vector<model::Id> out;
    for (const auto& [id, e] : instances_)
        if (e.instance && e.fallback) out.push_back(id);
    return out;
}

Result<host::Instance*> InstancePool::get(Id id, const model::FxInstance& fx,
                                          const Session& session) {
    auto& e = instances_[id];
    // Route E stage 2: the session's object for this FX, if `fx` is it. One
    // the instance was last configured from (the pool keeps it, so its
    // address is not reused) carries the same patch and state: nothing to
    // serialize. Any other, or a copy, is compared by its text.
    const auto* stored = session.fx.entry(id);
    const std::shared_ptr<const model::FxInstance> own =
        stored && stored->value.get() == &fx ? stored->value : nullptr;
    const bool unchanged = own && e.instance && e.appliedFx == own;
    std::string desired;
    if (unchanged) {
        desired = e.appliedState;
    } else if (fx.patch) {
        desired = "patch:" + json::writeCompact(*fx.patch);
        ++patchSerializations_;
    } else if (fx.state) {
        desired = "blob:" + *fx.state;
    }
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
        bool fallback = false;
        const model::Isolation effective = resolveIsolation(fx, *info, &fallback);
        const bool isolated =
            effective == model::Isolation::SharedBridge || effective == model::Isolation::Dedicated;
        std::shared_ptr<host::BridgeProcess> bridge;
        if (effective == model::Isolation::SharedBridge) {
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
        e.effective = effective;
        e.fallback = fallback;
        e.appliedState.clear();
        e.appliedFx.reset();
        e.failure.reset();
        e.clearedAutomation.clear();
    }
    if (desired != e.appliedState && !desired.empty()) {
        if (fx.patch) {
            SUPPLE_TRY(e.instance->setPatch(std::string_view(desired).substr(6),  // "patch:"
                                            CLAP_SUPPLE_SET_REPLACE));
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
    e.appliedFx = own;
    service(id);
    return e.instance.get();
}

bool InstancePool::patchable(Id id, const model::FxInstance& fx) const {
    const auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    const Entry& e = it->second;
    return e.instance && !e.instance->failed() && !e.failure && !e.restarting && fx.patch &&
           e.pluginId == fx.plugin.id && e.isolation == fx.isolation &&
           e.appliedState.starts_with("patch:");
}

Result<host::Instance::Notifications> InstancePool::applyPatch(Id id, const model::FxInstance& fx,
                                                               const Session& session) {
    if (!patchable(id, fx))
        return Error("engine.patch_unpatchable", id.str() + " needs a compile for its patch",
                     "/fx/" + id.str(), "reload the session");
    Entry& e = instances_[id];
    const auto* stored = session.fx.entry(id);
    const std::shared_ptr<const model::FxInstance> own =
        stored && stored->value.get() == &fx ? stored->value : nullptr;
    if (!own || e.appliedFx != own) {
        auto desired = "patch:" + json::writeCompact(*fx.patch);
        ++patchSerializations_;
        if (desired != e.appliedState) {
            SUPPLE_TRY(e.instance->setPatch(std::string_view(desired).substr(6),  // "patch:"
                                            CLAP_SUPPLE_SET_REPLACE));
            e.appliedState = std::move(desired);
        }
    }
    e.appliedFx = own;
    return service(id);
}

void InstancePool::prune(const Session& session, std::uint64_t liveGeneration) {
    for (auto it = instances_.begin(); it != instances_.end();)
        it = session.fx.contains(it->first) && !model::frozenOwner(session, it->first)
                 ? std::next(it)
                 : instances_.erase(it);
    std::erase_if(retired_, [&](const auto& r) { return r.first <= liveGeneration; });
    racks_->prune(session, liveGeneration);
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
        notes.latencyChanged |= latency != entry.rawLatency;
        entry.rawLatency = latency;
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
    if (auto it = instances_.find(id); it != instances_.end()) {
        it->second.latency = latency;
        it->second.rawLatency = it->second.instance->latency();
    }
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

std::vector<Id> InstancePool::bridgeThreadsRefused() const {
    std::vector<Id> out;
    for (const auto& [id, entry] : instances_)
        if (entry.instance && !entry.instance->failed() && entry.instance->bridgeThreadRefused())
            out.push_back(id);
    return out;
}

bool InstancePool::automationCleared(Id id, std::uint32_t param) const {
    auto it = instances_.find(id);
    return it != instances_.end() &&
           std::find(it->second.clearedAutomation.begin(), it->second.clearedAutomation.end(),
                     param) != it->second.clearedAutomation.end();
}

// Both forget the session object the instance was configured from: it no
// longer describes the instance, so the next get() compares by text.
void InstancePool::acknowledgeOutputPatch(Id id, const json::Value& patch) {
    if (auto it = instances_.find(id); it != instances_.end() && it->second.instance) {
        it->second.appliedState = "patch:" + json::writeCompact(patch);
        it->second.appliedFx.reset();
    }
}

void InstancePool::acknowledgeOutputState(Id id, const std::string& hash) {
    if (auto it = instances_.find(id); it != instances_.end() && it->second.instance) {
        it->second.appliedState = "blob:" + hash;
        it->second.appliedFx.reset();
    }
}

bool InstancePool::anyAutomationCleared() const {
    return std::any_of(instances_.begin(), instances_.end(),
                       [](const auto& entry) { return !entry.second.clearedAutomation.empty(); });
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

using namespace detail;

namespace {
// ADR-0090: a render's bridged calls wait with its own deadline, from its
// first activation to its last stopProcessing, and the process-wide one
// returns after it.
class RenderDeadlines {
  public:
    RenderDeadlines(std::vector<host::Instance*> list, int ms) : instances_(std::move(list)) {
        for (host::Instance* in : instances_) in->setCallDeadlineMs(std::max(ms, 1));
    }
    ~RenderDeadlines() {
        for (host::Instance* in : instances_) in->setCallDeadlineMs(0);
    }
    RenderDeadlines(const RenderDeadlines&) = delete;
    RenderDeadlines& operator=(const RenderDeadlines&) = delete;

  private:
    std::vector<host::Instance*> instances_;
};

// ADR-0090: a stop or a cancel kills the render's own bridges, so a render
// blocked in a bridged call (activation, reset, flush, start, a block or
// stopProcessing) returns within a wait step instead of at its deadline.
// Every caller gives a render a pool of its own, so no one else uses them.
// The watch looks every 10 ms; what it does is decided by the request, not
// the clock. It runs only while the render has bridged instances.
RenderWatchTestHooks gWatchHooks;  // tests only, set while no render runs

class RenderWatch {
  public:
    RenderWatch(std::vector<host::Instance*> list, const OfflineSettings& opt)
        : instances_(std::move(list)), opt_(opt) {
        std::erase_if(instances_, [](host::Instance* in) { return !in->isolated(); });
        if (instances_.empty()) return;
        thread_ = std::thread([this] {
            nameThread("render-watch");
            std::unique_lock lock(mutex_);
            while (!finished_) {
                if (requested(opt_)) {
                    for (host::Instance* in : instances_) in->abandonBridge();
                    if (gWatchHooks.killed) gWatchHooks.killed(gWatchHooks.context);
                    abandoned_.store(true, std::memory_order_release);
                    return;
                }
                wake_.wait_for(lock, std::chrono::milliseconds(10));
            }
        });
    }
    ~RenderWatch() { finish(); }
    RenderWatch(const RenderWatch&) = delete;
    RenderWatch& operator=(const RenderWatch&) = delete;
    // A stop or a cancel was asked for.
    static bool requested(const OfflineSettings& opt) {
        return opt.stopToken.stop_requested() ||
               (opt.cancelFlag && opt.cancelFlag->load(std::memory_order_relaxed));
    }
    // Ends the watch; after it, abandoned() no longer changes.
    void finish() {
        if (gWatchHooks.finishing) gWatchHooks.finishing(gWatchHooks.context);
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    // The watch killed the render's bridges.
    bool abandoned() const { return abandoned_.load(std::memory_order_acquire); }

  private:
    std::vector<host::Instance*> instances_;
    const OfflineSettings& opt_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool finished_ = false;
    std::atomic<bool> abandoned_{false};
    std::thread thread_;
};

Error renderCancelled() {
    return Error("job.cancelled", "offline render cancelled", {},
                 "submit a new render job if its output is still wanted; the cancelled job "
                 "cannot resume");
}
}  // namespace

void setRenderWatchTestHooks(const RenderWatchTestHooks& hooks) { gWatchHooks = hooks; }

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
    if (opt.modules) pool.racks().setCatalog(opt.modules);
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
    // From here every bridged call of the render has its deadline and its
    // stop (ADR-0090): activation in prepare() included.
    const RenderDeadlines deadlines(p.instances, opt.bridgeDeadlineMs);
    RenderWatch watch(p.instances, opt);
    if (RenderWatch::requested(opt)) return renderCancelled();
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
    if (auto prepared = prepare(p, selectedSession); !prepared) {
        watch.finish();
        // A stop while it activated: the watch killed what it waited for.
        if (watch.abandoned()) return renderCancelled();
        return std::move(prepared).error();
    }
    // ADR-0090: a plug-in that fails while the render runs fails the
    // render; one that had failed before keeps its behaviour.
    struct Watched {
        host::Instance* instance;
        Id fx;
        bool failedBefore;
    };
    std::vector<Watched> watched;
    for (const Node& node : p.nodes)
        forEachStage(node, [&](const Stage& st) {
            if (st.instance) watched.push_back({st.instance, st.fx, st.instance->failed()});
        });
    const auto failing = [&] {
        return std::any_of(watched.begin(), watched.end(), [](const Watched& w) {
            return !w.failedBefore && w.instance->failed();
        });
    };
    const auto failedNow = [&] {
        std::vector<const Watched*> out;
        for (const Watched& w : watched)
            if (!w.failedBefore && w.instance->failed() &&
                std::none_of(out.begin(), out.end(),
                             [&](const Watched* seen) { return seen->instance == w.instance; }))
                out.push_back(&w);
        return out;
    };
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
        for (Edge& edge : p.edges) edge.homeSilenced = false;  // rack sends (ADR-0075)
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
                if (RenderWatch::requested(opt)) {
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
                // A failed plug-in's output went silent (ADR-0013 §6): the
                // render fails below.
                if (failing()) break;
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
    if (preparationError) return *preparationError;
    // A call the watch's kill ended can return before the watch has said it
    // killed: a failed render waits for the watch to end first (the join
    // orders its kills before abandoned()), and a stop or cancel asked for
    // by then is the outcome too.
    const auto lost = failedNow();
    if (startFailed || failed || !lost.empty()) watch.finish();
    // Cancelled between blocks, or stopped while a bridged call waited: the
    // job's outcome, whatever the abandoned bridges' instances say.
    if (cancelled || watch.abandoned() || RenderWatch::requested(opt)) return renderCancelled();
    // Failed starting, processing or stopping: the render's audio is not
    // what the session plays (ADR-0090).
    if (!lost.empty()) {
        const Watched& first = *lost.front();
        std::string message = first.instance->info().name + " (" + first.fx.str() +
                              ") failed during the render: " + first.instance->failure();
        for (std::size_t i = 1; i < lost.size(); ++i)
            message += (i == 1 ? "; so did " : ", ") + lost[i]->instance->info().name + " (" +
                       lost[i]->fx.str() + ")";
        return Error("engine.fx_failed", message, "/fx/" + first.fx.str(),
                     "render again: an offline render starts its own plug-in instances; if the "
                     "FX fails again, bypass or replace it");
    }
    if (startFailed)
        return Error("engine.start_failed", "a plugin refused to start processing", {},
                     "restart or replace the FX that refused processing with a plugin supporting "
                     "the render sample rate and block size");
    if (failed)
        return Error("engine.process_error", "a plugin reported a processing error", {},
                     "bypass or replace the failing FX and render the range again");
    // Read once the render is known good: a readout is a bridged request too,
    // and a failed instance has none (ADR-0090).
    for (const auto& fx : s.fx)
        if (host::Instance* in = pool.find(fx.id); in && !in->failed())
            if (auto readouts = instanceReadouts(*in); readouts && readouts->size() > 0)
                r.meters[fx.id.str()] = std::move(*readouts);
    // The watch covered the requests above too; a stop during them still
    // cancels the render.
    watch.finish();
    if (watch.abandoned()) return renderCancelled();
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
    for (const auto& node : p.nodes) {
        if (node.midiDropped)
            r.warnings.push_back(node.id.str() + ": MIDI send capacity exceeded: " +
                                 std::to_string(node.midiDropped));
        forEachStage(node, [&](const Stage& st) {
            if (st.voiceDropped)
                r.warnings.push_back(st.fx.str() + ": voice tracking capacity exceeded: " +
                                     std::to_string(st.voiceDropped));
        });
    }
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
