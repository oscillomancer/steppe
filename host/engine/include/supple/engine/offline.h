// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Offline rendering of a session version (spec: docs/specs/engine.md §1, §3).
//
// The session is compiled into a plan: tracks in topological order (sources
// before the tracks they feed), each track summing its
// inputs (routes and children's parent sends, in a fixed order), playing its
// MIDI items into its FX chain, then its typed Console 9 strip stages and
// fader/pan/mute (ADR-0019). Routes tap pre-FX, pre-fader or post-fader. Plugin delay
// compensation schedules sources by their downstream latency, with edge
// padding at fan-outs (ADR-0051). Items, controls and plugin transport share
// that schedule. Trimming the output tap's timeline shift aligns the render
// with the timeline to the sample.
//
// Deterministic: fixed summation order, independent of worker scheduling.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "supple/base/stop.h"

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/host/clap_host.h"
#include "supple/model/session.h"

namespace supple::engine {

namespace detail {
class RackPool;  // the module instances of racks (ADR-0062)
}

// Owns the plugin instances for a session's fx objects (main thread). An
// instance is created on first use and reused while its plugin stays the
// same; its patch or state is (re)applied when the model's changes.
class InstancePool {
  public:
    using Resolver = std::function<Result<host::PluginInfo>(const model::PluginRef&)>;
    InstancePool(Resolver resolve, host::HostIdentity identity = {});
    ~InstancePool();
    // Main thread, before use. Also applies to replaced/retired instances.
    void setMainThreadWake(MainThreadWake& wake);
    std::vector<host::Instance*> instances() const;

    // The instance for `id`, configured from `fx` (patch or state blob from
    // `session`). A plugin that cannot be found is an error the caller may
    // turn into a pass-through placeholder.
    Result<host::Instance*> get(model::Id id, const model::FxInstance& fx,
                                const model::Session& session);
    // Destroys instances whose fx no longer exists in `session` (call it once
    // no plan uses them), and replaced instances retired at a generation up
    // to `liveGeneration` (the newest plan the audio side has adopted).
    void prune(const model::Session& session, std::uint64_t liveGeneration = UINT64_MAX);
    // Instances replaced from now on may still be used by plans before
    // `generation` (the real-time engine sets the next plan's generation).
    void setGeneration(std::uint64_t generation);
    // The racks' module instances, which follow the same generations and
    // pruning (ADR-0062).
    detail::RackPool& racks() { return *racks_; }
    // The next get() for `id` creates a new instance (fx.restart); the old
    // one lives until prune().
    void restart(model::Id id);
    // Isolated instances whose bridge crashed or hung, with the reason.
    std::vector<std::pair<model::Id, std::string>> failed() const;
    host::Instance* find(model::Id id) const;
    // Main thread: service and retain restart failures until fx.restart/replacement.
    // A latency that moved since the last service (a plug-in restarted, e.g. a
    // low-latency mode) is reported as latencyChanged.
    host::Instance::Notifications service(model::Id id);
    // The plug-in asked for a restart that waits for its processing to stop:
    // plans leave it off their executors (its stage passes through,
    // compensated) until service() has restarted it.
    bool restarting(model::Id id) const;
    bool restarting(const host::Instance* instance) const;
    std::vector<std::pair<model::Id, Error>> failedErrors() const;
    const Error* failure(model::Id id) const;
    std::uint32_t rememberedLatency(model::Id id) const;
    void rememberLatency(model::Id id, std::uint32_t latency);  // what a plan compensated
    bool automationCleared(model::Id id, std::uint32_t param) const;
    void resetAutomation(model::Id id);  // explicit model reload
    // Main thread, only after applying plug-in-originated values to its model
    // patch: acknowledge that patch without sending it back on the next get().
    void acknowledgeOutputPatch(model::Id id, const json::Value& patch);
    // The equivalent acknowledgement for captured opaque state.
    void acknowledgeOutputState(model::Id id, const std::string& hash);

    std::size_t retired() const { return retired_.size(); }
    std::size_t size() const { return instances_.size(); }

  private:
    std::unique_ptr<detail::RackPool> racks_;
    struct Entry {
        std::unique_ptr<host::Instance> instance;
        std::string pluginId;
        model::Isolation isolation = model::Isolation::InProcess;
        std::uint32_t latency = 0;
        bool restarting = false;  // a restart the plug-in asked for waits on processing
        std::optional<Error> failure;
        std::vector<std::uint32_t> clearedAutomation;
        std::string appliedState;  // canonical patch text or blob hash last applied
    };
    void retire(std::unique_ptr<host::Instance> instance);
    MainThreadWake* mainWake_ = nullptr;
    Resolver resolve_;
    host::HostIdentity identity_;
    std::shared_ptr<host::BridgeProcess> sharedBridge_;
    std::map<model::Id, Entry> instances_;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<host::Instance>>> retired_;
    std::uint64_t generation_ = 0;
};

// How a take's audio is played back (session-model §1.5). `rate` is the rate
// the clip reads the variant at, one frame per timeline sample:
//  * varispeed — `pitch_mode: "resample"` or `preserve_pitch: false` — the
//    source is resampled to `rate / speed()`, so reading it at `rate` plays
//    it `speed()` times faster and the pitch follows the rate;
//  * stretch — `preserve_pitch` with the `"signalsmith"` mode — the source is
//    stretched (duration / playrate) and transposed by `pitch` semitones with
//    the pitch preserved, deterministically.
// An item with nothing to apply (playrate 1, no pitch) is the plain
// resample, and a `pitch` offset that a resample cannot honour is ignored
// (the compiler warns).
struct MediaVariant {
    double playrate = 1.0;      // 0.01 .. 100
    double pitch = 0.0;         // semitones
    bool preservePitch = true;  // ask for a stretch (needs a stretch mode)
    std::string pitchMode = "signalsmith";

    bool stretch() const {
        return preservePitch && pitchMode == "signalsmith" && (playrate != 1.0 || pitch != 0.0);
    }
    // Source seconds played per timeline second.
    double speed() const { return playrate * (preservePitch ? 1.0 : std::pow(2.0, pitch / 12.0)); }
};

// Immutable decoded audio and playback variants (model/decode thread only).
// Sources with a known hash share their content; otherwise every get checks
// dev/inode/size/nanosecond-mtime before using cached data. Decoded originals
// are weakly indexed; only a plan or passthrough variant keeps them alive.
struct PreparedSource {
    std::shared_ptr<const audio::AudioData> data;
    bool itemLocal = false;
    std::optional<Error> error;
};
// Complete immutable source snapshot. Preparation never owns plug-in instances.
struct PreparedSources {
    std::string key;
    double sampleRate = 0;
    model::Session session;  // immutable provenance for source-dependent edits
    std::map<model::Id, PreparedSource> takes;
};
Result<std::string> sourcePreparationKey(const model::Session& session,
                                         const std::filesystem::path& directory, double rate);
class MediaCache {
  public:
    // Decoded audio at `rate` (resampled once, then kept; 0: the file's own
    // rate), with `variant` applied (a variant that asks for nothing is the
    // plain resample). The resampler and the stretch are deterministic, so
    // every cache gives the same samples (offline ≡ real time).
    Result<std::shared_ptr<const audio::AudioData>> get(const std::filesystem::path& path,
                                                        double rate = 0,
                                                        const MediaVariant& variant = {},
                                                        std::string_view contentHash = {});
    std::size_t size() const { return entries_.size(); }
    // Bound worker-owned reusable variants; outstanding plan leases remain valid.
    void trimVariants(std::size_t samples = std::size_t{1} << 26);
    Result<std::shared_ptr<const PreparedSources>> prepareSources(
        const model::Session& session, const std::filesystem::path& directory, double rate,
        const std::function<bool()>& cancelled = {});
    void adoptSources(std::shared_ptr<const PreparedSources> sources) {
        prepared_ = std::move(sources);
    }
    const std::shared_ptr<const PreparedSources>& sources() const { return prepared_; }

  private:
    friend struct MediaCacheTestAccess;
    std::map<std::string, std::weak_ptr<const audio::AudioData>> decoded_;
    std::map<std::string, std::shared_ptr<const audio::AudioData>> entries_;
    std::shared_ptr<const PreparedSources> prepared_;
};

struct OfflineSettings {
    // Playback/stems apply master rate; analysis and item-FX renders opt out.
    bool applyMasterPlayrate = true;
    model::Id master = model::kMaster;  // mix output root, ignored for isolated track taps
    double startSeconds = 0.0;
    double endSeconds = 0.0;   // exclusive; the tail follows
    double tailSeconds = 0.0;  // rendered after endSeconds
    std::uint32_t blockSize = 512;
    // Relative media paths resolve against <projectDir>/<project.media_dir>
    // (or <projectDir> itself when the file is found there).
    std::filesystem::path projectDir;
    MediaCache* media = nullptr;  // null: a cache for this render only
    // Threads rendering the graph; 0: every allowed CPU but two. The output is
    // identical for any count.
    unsigned threads = 0;
    // Select one track's output without its sends or master processing.
    std::optional<model::Id> track;
    // Internal freeze capture: full graph schedule, pre-fader main + auxiliaries.
    std::optional<model::Id> freezeTrack;
    bool freezeProbe = false;            // prepare and return scheduling metadata only
    double freezeTailLimitSeconds = 2.;  // bounded attempt; caller retries up to 600 s
    bool freezeTail = false;             // reported tail plus a sample-counted silence window
    // Render through the master with this track as the exclusive solo source.
    std::optional<model::Id> throughMaster;
    model::Tap tap = model::Tap::PostFader;
    std::shared_ptr<const std::atomic<bool>> cancelFlag;  // checked between offline blocks
    std::shared_ptr<std::atomic<double>> progressFlag;
    // Optional worker-side publication, at most once per 500 ms plus final.
    std::function<void(double)> progress;
    double progressBase = 0.0, progressScale = 1.0;
    supple::StopToken stopToken;
};

struct OfflineResult {
    std::int64_t freezeLatency = 0, freezeShift = 0, freezeTailFrames = 0;
    audio::AudioData audio;             // master output, stereo, at the project rate
    std::uint32_t outputLatency = 0;    // the compensated master latency (samples)
    std::vector<std::string> warnings;  // e.g. missing plugins rendered as pass-through
    std::vector<std::string> log;       // plugin log lines
    // The compiled graph, one line per track in processing order:
    // "tr_3 in=[tr_2 pre_fader d0 g0.707/0.707] lat_in=0 lat_chain=0 timeline_shift=0".
    // lat_in is the forward arrival diagnostic; timeline_shift schedules the
    // node input at processing position - timeline_shift (ADR-0051).
    std::vector<std::string> graph;
    // Each plugin's read-only values (meters, readouts) as the render ended,
    // by FX id, for the plugins that have any (instanceReadouts).
    std::map<std::string, json::Value> meters;
};

// An instance's read-only parameters as they stand: a semantic plugin's by
// patch key in physical units (a choice by its key), any other's by name as
// CLAP values. Main thread.
Result<json::Value> instanceReadouts(host::Instance& in);

Result<OfflineResult> renderOffline(const model::Session& session, InstancePool& pool,
                                    const OfflineSettings& settings);

// Track pan (the master pans as a balance control instead).
// Constant-power pan with the project's centre attenuation (pan law, dB):
// -3 dB is classic constant power (centre 0.707, hard side 1.0); 0 dB keeps
// the centre at unity. `pan` in [-1, 1]. Returns {left gain, right gain}.
std::pair<double, double> panGains(double pan, double panLawDb);

}  // namespace supple::engine
