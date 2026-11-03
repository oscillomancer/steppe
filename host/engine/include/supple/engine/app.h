// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The DAW application core without a GUI: the session history, plugins,
// rendering and (optionally) the real-time engine behind the command bus
// (session-model §6–7, engine.md §6). Model thread only.
//
// Every bus transaction (one edit command, or one batch) builds one EditTxn
// on the current branch head and commits as one version: one undo step, with
// its resolved invocations recorded for replay merges. Queries inside a batch
// read the transaction's preview, so they see the earlier steps.
//
// Plugin state lives in the model (FxInstance.patch / state blob). Patch
// edits are merged and validated on a scratch instance of the plugin; the
// instances that make sound follow the model on the next plan (InstancePool).
#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "supple/base/stop.h"

#include "supple/cmd/bus.h"
#include "supple/engine/midi_control.h"
#include "supple/engine/offline.h"
#include "supple/engine/portaudio.h"
#include "supple/engine/realtime.h"
#include "supple/engine/runtime_protection.h"
#include "supple/lua/runtime.h"
#include "supple/model/versions.h"

namespace supple::engine {
class SourcePreparation;

namespace detail {
class RuntimeProtection;
}

struct AppOptions {
    std::vector<std::filesystem::path> pluginPath;  // empty = host::defaultSearchPath()
    host::HostIdentity identity;
    std::filesystem::path chainDir;     // empty = the user config directory / <slug> / chains
    std::filesystem::path presetDir;    // empty = … / <slug> / presets (fx.preset.*)
    std::filesystem::path templateDir;  // empty = … / <slug> / track-templates
    std::filesystem::path projectTemplateDir;  // empty = … / <slug> / templates
    std::filesystem::path workdir;
    std::filesystem::path rendersDir;  // empty = <workdir>/renders; setRendersDir changes it
    // Render presets and the user's render default (ADR-0055). Empty =
    // <workdir>/render-presets; the app passes <data>/<slug>/render-presets.
    std::filesystem::path renderPresetDir;
    // DSP scripts (ADR-0032): <scriptDir>/<name>/<name>.cpp, each a plugin of
    // the script player. Empty = <workdir>/scripts.
    std::filesystem::path scriptDir;
    // An unsaved project's own folder (the GUI's, one per run): its media (the
    // recordings) live in <scratchDir>/<media_dir> until the first save moves
    // them beside the project file; clearScratch() deletes it when the
    // unsaved project is discarded. Empty: <workdir>/media, never deleted.
    std::filesystem::path scratchDir;
    std::string jackLibrary = "libjack.so.0";                    // tests use a stand-in
    std::shared_ptr<RuntimeProtectionOps> runtimeProtectionOps;  // null: Linux operations
    std::function<void()> jobStartedHook;   // worker-entry barrier for deterministic tests
    std::function<void()> jobFinishedHook;  // worker completion notification for tests
};

// Meter entries a full ring dropped (P5.3c): the renderer's ring and the audio
// callback's. A drop means a strip's peak/RMS missed a block, not that audio
// was lost.
struct MeterDrops {
    std::int64_t renderer = 0, callback = 0;
};

// One thread class's scheduling as engine.status reports it (ADR-0026).
struct ThreadClass {
    ThreadPolicy::Kind policy = ThreadPolicy::Kind::None;
    std::int64_t priority = 0;
};
struct EngineScheduling {
    ThreadClass callback, live, renderer, background;
};

// The live helpers' audio workgroup membership (ADR-0038); see WorkgroupStatus.
struct EngineWorkgroup {
    std::string state = "none";
    std::int64_t joined = 0, failed = 0, osError = 0;
    std::string message, hint;
};

// The real-time engine as engine.status reports it.
struct EngineStatus {
    bool preparingAudio = false;
    std::uint64_t preparedVersion = 0, preparationVersion = 0;
    std::string preparationError;
    bool running = false;
    std::string backend;
    double sampleRate = 0;
    std::int64_t block = 0, period = 0;
    std::int64_t planLatency = 0, deviceLatency = 0;  // frames
    std::int64_t xruns = 0, cycles = 0, processErrors = 0;
    std::int64_t midiDropped = 0;
    std::vector<MidiInputWarning> midiWarnings;
    bool midiDevicesChanged = false;
    std::string midiDevicesMessage;
    bool restartNeeded = false;
    bool anticipative = false;  // rendering ahead on the worker pool (ADR-0012)
    std::int64_t anticipationMisses = 0;
    std::int64_t liveHelpers = 0;
    EngineScheduling scheduling;  // recorded at start; "none" while stopped
    EngineWorkgroup workgroup;
    std::vector<std::string> warnings;
    RuntimeProtectionStatus memoryLock;
    RuntimeProtectionStatus cpuIdleLatency;
    MeterDrops meterDrops;                       // meter entries dropped by a full ring
    std::vector<std::string> failedFx;           // isolated instances whose bridge crashed or hung
    std::int64_t dropped = 0, voiceDropped = 0;  // plug-in output/voice transport
    json::Value failedErrors = json::Value::array();
};

struct RecordingWarning {
    std::string code, message, path, hint, track, file;
};

struct RecordedInfo {
    std::string track, item, take, file;  // take: the active one
    double seconds = 0;
    std::int64_t lostFrames = 0;     // input lost (the disk fell behind, playback dropped out)
    std::vector<std::string> takes;  // every take (one per loop pass)
    std::optional<std::vector<RecordingWarning>> warnings;
};

// A transport range (loop or punch), in seconds (ADR-0015).
struct TransportRange {
    bool enabled = false;
    double start = 0, end = 0;
};

// The metronome (transport state like the loop: not versioned, not saved).
struct MetronomeSettings {
    bool enabled = false;
    double volumeDb = -6.0;
    int countInBars = 0;           // clicks before a recording starts (0 = none)
    bool countInPlayback = false;  // and before plain playback
};

// A recording no version of the session uses (after a crash: a take that
// was never committed, or committed but never saved), and where its sidecar
// says it belongs.
struct RecoverableInfo {
    std::string file;  // relative to the project's media directory ("rec/Vocal-1.wav")
    std::string track;
    double startSeconds = 0, seconds = 0;
    std::int64_t channels = 0;
};

struct TransportInfo {
    bool playing = false, recording = false;
    bool countingIn = false;
    double seconds = 0, beat = 0;
    std::int64_t sample = 0;
    TransportRange loop, punch;
    std::optional<std::vector<RecordedInfo>> recorded;  // takes a stop/pause just committed
};

struct PlayrateState {
    double requestedRate = 1, effectiveRate = 1;
    bool preservePitch = false;
    std::optional<model::Id> envelope;
    // Latest model curve at the consumed timeline position, not the renderer's
    // lookahead position. Absent when no active nonempty envelope exists.
    std::optional<double> envelopeValue;
};

// One strip's meter reading (P5.3c): the peak and RMS of the audio of that
// strip's post signal (post-fader, post-pan) that has been *heard* since the
// last call. The engine does no decay or peak hold — the GUI does. A strip
// with no heard audio reports zeros. `track` is the strip's track id.
struct MeterReading {
    model::Id track;
    float peak[2] = {0.0f, 0.0f};
    float rms[2] = {0.0f, 0.0f};
};

struct EngineDeviceOptions {
    PortAudioOptions native;
    double rate = 0;  // zero: project rate; otherwise asked of the device or graph
    std::uint32_t inputs = 0, outputs = 2;
    bool forceGraph = false;  // JACK on PipeWire: force the block and rate while running
};

class EngineApp : public cmd::Host {
  public:
    static Result<std::unique_ptr<EngineApp>> create(AppOptions options,
                                                     model::Project project = {});
    ~EngineApp() override;
    EngineApp(const EngineApp&) = delete;
    EngineApp& operator=(const EngineApp&) = delete;

    // --- state ------------------------------------------------------------------
    const model::Session& head() const;
    model::Session view() const;  // the open transaction's preview, else the head
    model::EditTxn& txn();        // the open transaction (edit commands only)
    bool inTransaction() const { return txn_ != nullptr; }
    model::VersionStore& versions() { return *versions_; }
    const model::VersionStore& versions() const { return *versions_; }
    const AppOptions& options() const { return options_; }
    const std::filesystem::path& projectPath() const { return projectPath_; }
    // Where relative media paths resolve: the saved project's folder, or the
    // scratch folder of an unsaved one (empty without either).
    std::filesystem::path projectDir() const;
    // Deletes the scratch folder (an unsaved project's recordings): called when
    // the unsaved project is discarded (quit, new, open). Does nothing once the
    // project is saved, and never touches a folder holding the work directory.
    void clearScratch();

    // --- plugins ----------------------------------------------------------------
    const host::ScanReport& pluginScanReport() const { return scanReport_; }
    Result<std::span<const host::PluginInfo>> plugins(bool rescan = false);
    // A finished background scan (plugin.scan {wait: false}), on the model thread.
    void installPlugins(std::vector<host::PluginInfo> list, host::ScanReport report);
    std::string& pluginScanJob() { return pluginScanJob_; }  // the running one, if any
    // By id (exact, then case-insensitive) or display name; hint lists the closest ids.
    Result<host::PluginInfo> findPlugin(std::string_view idOrName);
    InstancePool& instances() { return *pool_; }
    MediaCache& media() { return media_; }
    // The plugin's semantic schema (null for plugins without the extension).
    Result<json::Value> fxSchema(const model::FxInstance& fx);
    Result<json::Value> fxFactoryPatches(model::Id id, const model::FxInstance& fx);
    // The plugin's CLAP parameters (from a scratch instance).
    Result<std::vector<host::ParamInfo>> fxParams(const model::FxInstance& fx);
    Result<std::uint32_t> fxSidechainChannels(const model::FxInstance& fx);  // 0: no key input
    // The plug-in's output ports past the main one (a multi-out instrument's),
    // ADR-0021.
    Result<std::vector<host::AudioPortInfo>> fxAuxOutputs(const model::FxInstance& fx);
    // The instance's reported latency in samples (the running one when there is
    // one, else a throwaway activation at the project rate).
    Result<json::Value> setPlainParams(model::Id id, const json::Value& values);
    Result<std::uint32_t> fxLatencySamples(model::Id id, const model::FxInstance& fx);
    // fx's patch after applying `patch` (a merge patch, or a complete patch
    // when `replace`), validated by the plugin; the complete canonical patch.
    Result<json::Value> mergedPatch(const model::FxInstance& fx, const json::Value& patch,
                                    bool replace);
    Result<std::string> describeFx(const model::FxInstance& fx, bool changedOnly);

    // --- project files ------------------------------------------------------------
    // Atomic; becomes the project path. `warnings` names each running plug-in
    // whose state could not be read: the file keeps the state it had before.
    Status save(const std::filesystem::path& path,
                std::vector<schema::Warning>* warnings = nullptr);
    Status open(const std::filesystem::path& path,
                std::vector<schema::Warning>* warnings = nullptr);  // replaces the history
    Status newProject(model::Project project);
    // A project template (project.template.*): `folder` receives project.json
    // (the head, without history) and media/ with a copy of every audio file
    // the head uses, named relative to it. openTemplate starts a new, unsaved
    // project from such a folder, its media copied into the new project's own
    // (or from an older single-file template); the template never becomes the
    // project path.
    Status writeTemplate(const std::filesystem::path& folder,
                         std::vector<schema::Warning>* warnings = nullptr);
    Status openTemplate(const std::filesystem::path& path,
                        std::vector<schema::Warning>* warnings = nullptr);
    // On (the GUI's preference), item.add copies an imported audio file into
    // the project's media folder and names the copy; off, the project names
    // the file where it is. A project without a folder (a headless engine's
    // unsaved one) always names the original.
    void setCopyImportedMedia(bool on) { copyImports_ = on; }
    bool copyImportedMedia() const { return copyImports_; }
    std::uint64_t projectGeneration() const {
        return projectGeneration_.load(std::memory_order_acquire);
    }

    // --- branches ------------------------------------------------------------------
    // Re-executes recorded invocations on `base` as `version` (the replayer
    // for merges): commands run against a temporary transaction on that base.
    Result<model::Replayed> replay(const cmd::Registry& registry, const model::Session& base,
                                   const std::vector<model::Invocation>& invocations,
                                   std::uint64_t version);
    Status undo();  // moves history, reloads the engine and emits session.changed
    Status redo();
    Status switchBranch(std::string_view name);  // reloads the engine
    model::MergeOutcome mergeBranch(const cmd::Registry& registry, std::string_view from,
                                    std::string_view into);

    // --- rendering ---------------------------------------------------------------
    Result<OfflineResult> renderMix(const OfflineSettings& settings);
    Result<OfflineResult> renderSession(const model::Session& session,
                                        const OfflineSettings& settings);
    // Where renders are written. Set on the command thread (the GUI's folder
    // choice); an empty path restores the default, <workdir>/renders.
    std::filesystem::path rendersDir() const {
        return options_.rendersDir.empty() ? options_.workdir / "renders" : options_.rendersDir;
    }
    void setRendersDir(std::filesystem::path dir) { options_.rendersDir = std::move(dir); }
    // The DSP script library (dsp.* commands). create() names it to the
    // script player, which lists a plugin for each script there, in
    // SUPPLE_SCRIPT_LIBRARY: first, before any folders the environment
    // already named.
    std::filesystem::path scriptDir() const {
        return options_.scriptDir.empty() ? options_.workdir / "scripts" : options_.scriptDir;
    }
    std::string rememberRender(const std::filesystem::path& path);
    std::string reserveRenderId();
    void publishRender(std::string id, const std::filesystem::path& path);
    // Each plugin's readouts as the last mix render ended (render.mix), by FX
    // id: what fx.meters {from: "render"} reads. Main thread.
    void rememberRenderMeters(std::map<std::string, json::Value> meters) {
        renderMeters_ = std::move(meters);
    }
    const json::Value* renderMeters(model::Id fx) const {
        auto found = renderMeters_.find(fx.str());
        return found == renderMeters_.end() ? nullptr : &found->second;
    }
    // The render queue (gap log #33): renders set up now, each pinned to the
    // version it was queued at, run later in one go (render.queue.*).
    // Transport-like state: not versioned, not saved with the project.
    struct QueuedRender {
        std::string id, command, label;
        json::Value args;  // the render command's arguments
        std::uint64_t version = 0;
        std::shared_ptr<const model::Session> pin;
        json::Value files;  // what a dry run said it would write
    };
    std::vector<QueuedRender>& renderQueue() { return renderQueue_; }
    std::string nextQueueId() { return "q" + std::to_string(++queueCounter_); }
    Result<std::filesystem::path> renderPath(std::string_view reference) const;
    void startJob(std::function<void(supple::StopToken)> work);
    void stopJobs() override;
    void dispatcherAttached(cmd::Dispatcher* dispatcher) override { commands_ = dispatcher; }

    // --- real-time engine (optional) ------------------------------------------------
    Status startEngine(Backend& backend, const BackendConfig& config);
    Status startEngine(Backend& backend, const BackendConfig& config, const EngineOptions& options);
    // On a backend the app owns: "jack" (the audio device, at the graph's
    // rate) or "dummy" (no device, paced like one, at the project's rate).
    Status startEngine(std::string_view backend, std::uint32_t block, bool anticipative = true,
                       const EngineDeviceOptions& device = {});
    // App-wide execution limit per background/live pool, including its caller.
    // 0: Automatic; 1..64: maximum threads. Applies on the next engine start
    // when EngineOptions::workers is -1; explicit helper counts take precedence.
    unsigned audioThreadLimit() const { return audioThreadLimit_; }
    Status setAudioThreadLimit(unsigned limit);
    // Saved intent; applies now if running, otherwise on successful start.
    Status setMemoryLock(bool requested);
    const RuntimeProtectionStatus& memoryLockStatus() const;
    Status setPreventDeepIdle(bool requested);
    const RuntimeProtectionStatus& cpuIdleLatencyStatus() const;
    void stopEngine();
    Engine* engine() { return engine_.get(); }
    const Backend* backend() const { return backend_; }  // null while stopped
    EngineStatus engineStatus();
    bool preparingAudio() const { return !pendingSourceKey_.empty(); }
    const std::string& preparationError() const { return preparationError_; }
    // The meters (P5.3c): one reading per strip of the newest plan, covering
    // the audio heard since the last call. Call it from this (the model)
    // thread, the way the GUI's tick does; empty when the engine is stopped.
    // Main-thread work only: it drains the engine's meter rings.
    std::vector<MeterReading> meters();
    // GUI activity subscription: observe before meters(), then arm only once
    // visible meters reach rest. Arm rechecks activity arriving during the read.
    std::uint64_t meterActivitySequence() const;
    void armMeterActivity(std::uint64_t observed, float minimumPeak = 0.001f);
    // fx.restart: the next plan gets a new instance of `fx` (the failed one is
    // freed once no plan uses it).
    void restartFx(model::Id fx);
    // Emits fx.failed once for each isolated instance that has failed.
    void checkFailures();
    // Commits recorded takes as items on their tracks (one version).
    Result<std::vector<RecordedInfo>> commitTakes(const std::vector<RecordedTake>& takes,
                                                  std::string_view label = "record");

    // --- recordings (crash recovery, ADR-0014 §3) -----------------------------------
    // Where media lives: next to the project file, or the work directory's
    // media/ before the project has one. Recordings go to its rec/.
    std::filesystem::path mediaDir() const;
    std::filesystem::path recordDir() const { return mediaDir() / "rec"; }
    // The monitoring mode track.add gives a new channel (a user preference the
    // GUI sets; on: it monitors once armed).
    model::Monitor defaultMonitor() const { return defaultMonitor_; }
    void setDefaultMonitor(model::Monitor m) { defaultMonitor_ = m; }
    // Recordings in recordDir() with a sidecar that no version uses and that
    // are not being recorded now, oldest name first.
    std::vector<RecoverableInfo> recoverable(
        std::vector<RecordingWarning>* warnings = nullptr) const;
    // Repairs their headers (the frames written after the last rewrite count)
    // and commits them as one version ("recover"), each on its sidecar's track
    // (or `track`) at its recorded position. `files` as recoverable() lists
    // them; empty: all.
    Result<std::vector<RecordedInfo>> recover(const std::vector<std::string>& files,
                                              std::optional<model::Id> track,
                                              std::vector<RecordingWarning>* warnings = nullptr);
    // The real-time engine's instance of `fx` (tests and tools), or null.
    host::Instance* instanceOf(model::Id fx) const { return pool_ ? pool_->find(fx) : nullptr; }
    // Transport requests. With a device, each waits (at most a second) until
    // the audio thread has acted on it, so the answer is the new state.
    Result<TransportInfo> transport(std::string_view action, std::optional<double> seconds = {});
    // Model/UI thread, allocation-free read; no command/log/history entry.
    // Effective rate is consumption telemetry; envelope points are cached when
    // the model head changes, never expanded during GUI synchronization.
    PlayrateState playrateState() const;
    // Automation writing (P5.14): while the transport plays, a change to a
    // strip's volume/pan/mute or tr_0/playrate (inside the edit's transaction) is recorded into
    // its envelope when that envelope is in touch, latch or write mode. At
    // pause/stop latch and write hold the last value to the stop position, and
    // a write envelope never touched is flattened over the pass (one
    // transaction, "Automation written").
    void recordAutomation(model::Id object, const std::string& param, double value);
    // Whether a change to this parameter would be recorded now.
    bool writesAutomation(model::Id object, const std::string& param) const;
    // transport.loop / transport.punch (ADR-0015): transport state, not
    // versioned; the loop reaches the engine (now, and whenever it starts),
    // the punch range trims takes when a recording is committed.
    void setLoop(const TransportRange& loop);
    void setPunch(const TransportRange& punch) { punch_ = punch; }
    const TransportRange& loop() const { return loop_; }
    const MetronomeSettings& metronome() const { return metronome_; }
    void setMetronome(const MetronomeSettings& m);  // reaches the engine now and on every start
    const TransportRange& punch() const { return punch_; }

    // --- main-thread service (event/deadline driven) --------------------------------
    // Frees retired plans, emits fx.failed (so subscribers see failures without
    // asking), restarts the engine after a device change, and autosaves the
    // project when its head changed and an autosave interval has passed.
    void tick();
    Waitable serviceWaitable() const { return mainWake_.waitable(); }
    int serviceFd() const { return serviceWaitable().fd(); }
    // All persistent local instances, including probes and retired plans.
    struct ServiceFd {
        int fd;
        short events;
        host::Instance* owner;
    };
    std::vector<ServiceFd> serviceFds() const;
    void serviceFdReady(const ServiceFd& watch, short ready);
    std::optional<std::uint64_t> nextServiceDelayMs() const;
    std::optional<std::chrono::steady_clock::time_point> nextServiceDeadline() const;
    std::uint64_t serviceClockMs() const;
    void workQueued() override { mainWake_.notify(); }

    // --- MIDI learn (ADR-0027) --------------------------------------------------------
    // A device's control message and the name of its input port.
    struct ControlEvent {
        ControlMidiEvent midi;
        std::string device;
    };
    // Takes the control messages the devices sent since the last call, hands
    // them to onControl, then moves the plug-in parameters learned for them
    // (fx `learn`): a semantic one through fx.param.set (one undo step per
    // movement, recorded by touch/latch envelopes), a plain one on the running
    // instance at once and into the project when the control rests. While
    // learning() it moves nothing. tick() calls it; device batches notify
    // the shared relay, so event-driven callers need no control poll timer.
    void pollControl();
    std::function<void(const std::vector<ControlEvent>&)> onControl;
    // Whether a binding with `when_*` conditions may act now: the window app
    // knows the selection and its windows. Unset (headless), such bindings wait.
    std::function<bool(model::Id fx, const model::ParamLearn& learn)> learnGate;
    void setLearning(bool on) { learning_ = on; }
    bool learning() const { return learning_; }
    bool hasLearn() const;  // any effect of the head has a learned parameter
    // Moves a parameter as a learned control does: `next` gets its current
    // normalized value and one encoder step, and returns the new value (none:
    // leave it). Returns the value set (rounded for a stepped parameter), or
    // none when it stayed, or when there is no such parameter.
    std::optional<double> moveParam(
        model::Id fx, const std::string& param,
        const std::function<std::optional<double>(double current, double step)>& next);

    // The parameter edited last in the window (the FX window's panel, a
    // plug-in's own editor), for "FX: last touched parameter (MIDI CC)". Not
    // agents' edits, nor what learned controls move. `serial` grows with
    // every touch.
    struct TouchedParam {
        model::Id fx;
        std::string param;  // as fx learn names it: a semantic key or a plain id
        std::uint64_t serial = 0;
    };
    std::optional<TouchedParam> lastTouched() const;
    void touchParam(model::Id fx, std::string param);
    // A parameter's name for people: the semantic name, or the plug-in's.
    std::string paramName(model::Id fx, const std::string& param);

    // Where autosaves go, next to the project file (<file>-data); empty when
    // the project has no file yet.
    std::filesystem::path autosaveDir() const;
    // The newest autosave of the project file `project` that is newer than the
    // file itself, or empty. project.open reports it instead of loading it.
    std::filesystem::path newerAutosave(const std::filesystem::path& project) const;

    // --- scripting ------------------------------------------------------------------
    // The Lua runtime scripts run in (ADR-0006). Attach the command dispatcher
    // once it exists: scripts then reach the bus and receive its events after
    // the command that emitted them. Null before that; script.run says so.
    void attachCommands(cmd::Dispatcher& dispatcher);
    lua::Runtime* scripting() { return scripts_.get(); }

    // --- cmd::Host ------------------------------------------------------------------
    cmd::Context makeContext(const cmd::Origin& origin) override;
    Result<std::uint64_t> beginTransaction(const cmd::Origin& origin,
                                           std::string_view label) override;
    Status commitTransaction(std::uint64_t id) override;
    Status rollbackTransaction(std::uint64_t id) override;
    Result<std::function<Status()>> invocationSavepoint() override;
    void log(const cmd::LogRecord& record) override;
    void emit(std::string_view topic, json::Value payload) override;
    void commandFinished() override;
    bool actionTransactional(std::string_view command) const override;
    std::uint64_t clockMs() const override;
    void invoked(std::string_view command, int version, const json::Value& args,
                 cmd::Kind kind) override;
    // Example fixtures: empty, saved, song (tr_1 "Bass"), song_with_item
    // (+ it_1: a MIDI bar), song_with_fx (+ fx_1 sine, fx_2 gain), song_with_env (+ en_1
    // on fx_2 gain), song_with_learn (song_with_fx + fx_2 gain on CC 21), song_undone,
    // engine_running (the dummy backend), engine_armed (+ tr_1 armed),
    // recording_interrupted (tr_1 + rec/Vocal-1.wav left by a crash), meter_change
    // (3/4 from bar 9), song_with_audio (tr_1 + it_1/tk_1 audio), song_with_autoitem (song_with_env
    // + ai_1), render_queued (song_with_fx + q1, a queued mix), and the bus's job_running. Built by
    // running commands.
    void setupFixture(cmd::Dispatcher& dispatcher, std::string_view fixture) override;
    // Relative project paths resolve against the work directory.
    std::filesystem::path resolve(const std::filesystem::path& path) const;

    // Tests and tools.
    // The command log (log.list): the most recent kMaxRecords invocations.
    const std::deque<cmd::LogRecord>& records() const { return records_; }
    static constexpr std::size_t kMaxRecords = 2000;
    std::vector<std::pair<std::string, json::Value>> takeEvents();
    void setFixedClock(std::uint64_t ms) { fixedClock_ = ms; }

  private:
    model::Monitor defaultMonitor_ = model::Monitor::On;
    unsigned audioThreadLimit_ = 0;
    std::unique_ptr<detail::RuntimeProtection> protections_;
    void finalizeRecordingHash(model::Id source, model::MediaSource expected, RecordedTake take,
                               std::string hash, std::string branch, std::uint64_t generation);
    void startPlaying(int countInBars);  // engine play(), after a count-in when > 0
    explicit EngineApp(AppOptions options);
    Result<host::Instance*> scratch(const model::FxInstance& fx);
    bool inRecordDir(const std::filesystem::path& file) const;
    void headChanged();  // reload the real-time engine
    void haltEngine();   // tears the engine down (stopEngine also cancels a restart)
    // The device changed under the running engine: stop it and remember to
    // start it again (same backend kind, block and options).
    void beginRestart(std::string reason, double seconds);
    // Tries a pending restart now (with backoff until 30 s have passed).
    void retryRestart();
    // Writes one autosave (rotating: the newest 5 stay) and marks the head
    // saved. Errors are emitted as engine.error.
    void autosave();
    void maybeAutosave();  // autosave() when the head changed and an interval passed

    // `prepare` runs once the new project is in place, before the engine
    // starts on it (a template's media copied into its folder).
    Status replaceProject(model::Session session, std::filesystem::path path,
                          const std::function<Status()>& prepare = {});
    bool copyImports_ = false;
    // Declared first: outlives instances, engine/backend, and worker joins.
    MainThreadWake mainWake_;
    AppOptions options_;
    std::atomic<std::uint64_t> projectGeneration_{0};
    cmd::Dispatcher* commands_ = nullptr;
    bool clearScripts_ = false;
    std::unique_ptr<model::VersionStore> versions_;
    std::unique_ptr<model::EditTxn> txn_;
    model::TxnRecord record_;
    std::uint64_t txnId_ = 0;
    std::optional<std::vector<host::PluginInfo>> catalogue_;
    host::ScanReport scanReport_;
    std::string pluginScanJob_;
    std::unique_ptr<InstancePool> pool_;
    MediaCache media_;
    std::map<std::string, std::filesystem::path> renders_;
    std::map<std::string, json::Value> renderMeters_;
    std::vector<QueuedRender> renderQueue_;
    std::uint64_t queueCounter_ = 0;
    mutable std::mutex rendersMutex_;
    std::uint64_t nextRender_ = 1;
    struct JobThread {
        supple::JoiningThread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<JobThread> jobs_;
    std::mutex recordingFinalizeMutex_;  // sidecar publication vs scratch relocation, never RT
    std::map<std::string, std::unique_ptr<host::Instance>> scratch_;
    Status moveScratchMedia(const std::filesystem::path& dest,
                            const std::function<Status()>& publish);
    std::unique_ptr<SourcePreparation> sourcePreparation_;
    std::string pendingSourceKey_, preparationError_;
    // The last source key and what it was computed from: most edits (a fader,
    // a plug-in parameter) share these collections, so the key is reused. An
    // unhashed file changed on disk is noticed at the next edit to them.
    struct SourceKeyMemo {
        model::IdMap<model::Item> items;
        model::IdMap<model::Take> takes;
        model::IdMap<model::MediaSource> media;
        std::shared_ptr<const model::Project> project;
        std::shared_ptr<const time::TimeMap> timeMap;
        std::filesystem::path dir;
        double rate = 0;
        std::string key;
    };
    std::optional<SourceKeyMemo> sourceKeyMemo_;
    std::shared_ptr<const model::Session> loadedHead_;
    std::unique_ptr<Engine> engine_;
    Backend* backend_ = nullptr;
    std::unique_ptr<Backend> ownedBackend_;
    double playStart_ = 0.0;  // transport.stop returns here
    std::optional<model::Id> playrateEnvelope_;
    model::Timebase playrateTimebase_ = model::Timebase::Beats;
    std::vector<model::EnvPoint> playratePoints_;
    // Automation written during this pass, per envelope (transient).
    struct AutoWrite {
        double startSeconds = 0, lastSeconds = 0, lastValue = 0;
        bool touched = false;
    };
    std::map<model::Id, AutoWrite> autoWrite_;
    std::optional<std::map<model::Id, AutoWrite>> autoWriteRollback_;
    struct OutputGesture {
        std::optional<PluginOutput> latest;
        std::vector<PluginOutput> recording;
        bool ended = false;
    };
    using OutputKey = std::pair<model::Id, std::uint32_t>;
    std::map<OutputKey, OutputGesture> outputGestures_;
    std::deque<PluginOutput> pendingOutputs_;
    std::uint64_t outputGeneration_ = 0, outputEpoch_ = 0;
    std::uint64_t outputDrops_ = 0, voiceDrops_ = 0, consumerDrops_ = 0;
    bool outputOverflow_ = false;
    static json::Value pluginErrorJson(const Error& error);
    std::vector<model::Id> pendingDirtyStates_;
    std::vector<host::Instance*> serviceInstances() const;
    void consumePluginOutputs();
    // A running plug-in's state into the model (its blob), when it differs:
    // one coalesced undo step per plug-in. For plug-ins that report a state
    // change their parameters do not show, and for every one before a save.
    // A plug-in that fails to report its state sets `failure` (when given).
    bool captureFxState(model::Id fx, Error* failure = nullptr);
    void captureFxStates(const std::vector<model::Id>& fx,
                         std::vector<schema::Warning>* failures = nullptr);
    Status commitOutputGesture(const OutputKey& key, const OutputGesture& gesture);
    // MIDI learn (learn.cpp).
    struct LearnState {
        Takeover takeover;
        std::uint64_t lastMs = 0, serial = 0;
    };
    std::map<std::pair<model::Id, std::string>, LearnState> learnState_;
    struct LearnCommit {  // a plain parameter set on the running instance
        double value = 0;
        std::uint64_t lastMs = 0;
        bool live = false;  // false: no running instance, commit through fx.param.set
    };
    std::map<OutputKey, LearnCommit> learnCommits_;
    std::vector<ControlMidiEvent> controlTaken_;
    bool learning_ = false;
    bool committingLearn_ = false;  // a learned value's commit is not a touch
    std::optional<TouchedParam> touched_;
    std::uint64_t touchSerial_ = 0;
    void applyLearn(const std::vector<ControlEvent>& events);
    // Commits the plain parameters that rested (all of them: `all`).
    void commitLearned(bool all);
    void recordAutomationAt(model::Id object, const std::string& param, double value,
                            double seconds);
    void beginAutomationPass(double seconds);
    Status finishAutomationPass(double stopSeconds);
    TransportRange loop_, punch_;  // kept across engine restarts (not in the session)
    MetronomeSettings metronome_;
    std::vector<std::filesystem::path> recordingFiles_;  // while recording
    std::set<model::Id> reportedFailures_;
    std::filesystem::path projectPath_;
    std::unique_ptr<lua::Runtime> scripts_;
    std::deque<cmd::LogRecord> records_;
    std::vector<std::pair<std::string, json::Value>> events_;
    std::uint64_t fixedClock_ = 0;

    // The engine's start recipe, kept to restart it after a device change.
    EngineDeviceOptions engineDevice_;
    std::string engineKind_;  // "jack"/"dummy" when the app owns the backend
    std::uint32_t engineBlock_ = 0;
    bool engineAnticipative_ = true;
    Backend* restartBackend_ = nullptr;  // a caller-supplied backend (not owned)
    BackendConfig restartConfig_{};
    EngineOptions restartOptions_{};
    bool restarting_ = false;
    std::string restartReason_, restartError_;
    double restartSeconds_ = 0.0;       // the transport position to restore
    std::uint64_t restartUntil_ = 0;    // give up (engine.error) at this clock
    std::uint64_t restartRetryAt_ = 0;  // the next attempt
    std::uint64_t restartBackoff_ = 0;  // ms, doubling to 4 s

    // Autosave: the head version last written and when.
    bool autosaveMarked_ = false;
    std::uint64_t autosaveMs_ = 0;
    std::uint64_t autosaveVersion_ = 0;
};

// Clones a take and its owned FX/envelopes/automation. `copy` may already
// have a new source/offset; `shift` clips its item-relative envelopes for a split.
Result<model::Id> copyTake(const model::Session& source, model::EditTxn& txn, model::Id original,
                           model::Take copy, double shift = 0);
// A copied take gets its own copy of an unpooled MIDI source; audio and
// pooled MIDI stay shared.
void unpoolCopiedSource(model::Take& take, const model::Session& s, model::EditTxn& txn);

// Registers the engine's command catalogue (session-model §7): project.*,
// track.*, item.*, midi.*, fx.*, render.mix, history.*, session.describe, …
void registerEngineCommands(cmd::Registry& registry);

// Compact text description of the session for LLMs (session-model §4).
std::string describeSession(const model::Session& s);

}  // namespace supple::engine
