// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/realtime.h"
#include "supple/engine/bench_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <system_error>
#include <thread>
#include <type_traits>

#include "supple/base/environment.h"
#include "supple/base/sleep.h"
#include "supple/base/thread.h"
#include "supple/diagctx/table.h"
#include "supple/host/clap_host.h"

#include <cstring>
#include "hardware_midi.h"
#include "live_workers.h"
#include "meters.h"
#include "midi_output.h"

#include "plan.h"
#include "plan_dump.h"
#include "rack_runtime.h"
#include "recording.h"
#include "sync_runtime.h"
#include "workers.h"

namespace supple::engine {

namespace {

// Metering (P5.3c): the executor pushes what it just measured. `renderer`
// selects the ring — the renderer thread's or the audio callback's — and this
// is called from the executor thread only, never from a worker task.
void pushMeters(MeterBank& bank, const detail::Plan& plan, const std::vector<std::size_t>& nodes,
                std::uint64_t epoch, std::int64_t stream, std::uint32_t frames,
                bool renderer) SUPPLE_RT {
    for (std::size_t i : nodes) {
        const detail::Node& node = plan.nodes[i];
        if (node.meterSlot == MeterBank::kNoSlot) continue;
        MeterEntry entry;
        entry.epoch = epoch;
        entry.stream = stream;
        entry.frames = frames;
        entry.slot = node.meterSlot;
        entry.generation = node.meterGeneration;
        for (int c = 0; c < 2; ++c) {
            entry.peak[c] = node.meterPeak[c];
            entry.sumSq[c] = node.meterSumSq[c];
        }
        if (renderer)
            bank.pushRenderer(entry);
        else
            bank.pushCallback(entry);
    }
}

// Recorded masters the renderer runs (ADR-0057): the frames [stream, stream +
// frames) of epoch `e` the callback is playing, from their record rings.
// Called whenever the callback plays, recording or not, so the rings drain.
void pullRecordTaps(detail::Plan& p, std::uint64_t e, std::int64_t stream,
                    std::uint32_t frames) SUPPLE_RT {
    for (detail::RecordTap& tap : p.recordTaps)
        if (tap.ring >= 0)
            p.recordRings[static_cast<std::size_t>(tap.ring)]->pullAt(
                e, stream, tap.pulled.l.data(), tap.pulled.r.data(), frames);
}

void captureRoutedMidi(detail::Recorder& recorder, const detail::Plan& plan,
                       std::uint32_t frames) SUPPLE_RT {
    for (const auto& node : plan.nodes)
        if ((!plan.split || node.live) && !node.routedMidi.empty())
            recorder.captureRoutedMidi(node.id, node.routedMidi, frames);
}

}  // namespace

std::uint64_t midiPortMask(const std::vector<std::string>& ports, std::string_view device) {
    auto lower = [](std::string_view v) {
        std::string out(v);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string want = lower(device);
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < ports.size() && i < 64; ++i)
        if (want.empty() || want == "*" || lower(ports[i]).find(want) != std::string::npos)
            mask |= std::uint64_t{1} << i;
    return mask;
}

// --- dummy backend -----------------------------------------------------------------

DummyBackend::DummyBackend(Options options) : options_(options) {}

DummyBackend::~DummyBackend() { stop(); }

void DummyBackend::requestRestart(std::string reason) {
    restartReason_ = std::move(reason);
    restartNeeded_.store(true);
}

BackendStatus DummyBackend::status() const {
    BackendStatus s{config_.blockSize, 0, 0, 0, false, {}};
    s.restartNeeded = restartNeeded_.load();
    if (s.restartNeeded) s.restartReason = restartReason_;
    return s;
}

Status DummyBackend::start(const BackendConfig& config, Callback callback) {
    if (running_)
        return Error("backend.running", "the backend is already running", {},
                     "stop this backend before starting it again or changing its configuration");
    if (onStart_) onStart_();
    if (failStarts_ > 0) {
        --failStarts_;
        return Error("backend.open", "the dummy device did not open (a test's failStarts)", {},
                     "a test asked for it; start again");
    }
    restartNeeded_.store(false);
    config_ = config;
    if (options_.grantedRate > 0) config_.sampleRate = options_.grantedRate;
    callback_ = std::move(callback);
    outputs_.assign(config.outputs, std::vector<float>(config.blockSize, 0.0f));
    outPtrs_.clear();
    for (auto& c : outputs_) outPtrs_.push_back(c.data());
    inputs_.assign(config.inputs, std::vector<float>(config.blockSize, 0.0f));
    inPtrs_.clear();
    for (auto& c : inputs_) inPtrs_.push_back(c.data());
    inputFrame_ = 0;
    midiOutput_.reset();
    midiCapture_.clear();
    for (auto& drop : midiAsyncDrops_) drop.store(0);
    if (config.midiOutputs && !options_.midiOutputPorts.empty())
        midiCapture_.reserve(options_.midiCaptureEvents);
    midi_.clear();
    midi_.reserve(kMaxMidiInputEvents);
    sync_.clear();
    sync_.reserve(kMaxSyncMidiEvents);
    capture_.sampleRate = config.sampleRate;
    capture_.channels.assign(
        config.outputs, std::vector<float>(static_cast<std::size_t>(options_.captureFrames), 0.0f));
    captured_ = 0;
    cycles_ = 0;
    running_ = true;
    if (options_.mode == Mode::Thread) {
        thread_ = std::thread([this] {
            nameThread("audio-dummy");
            const auto period =
                std::chrono::duration<double>(config_.blockSize / config_.sampleRate);
            auto next = std::chrono::steady_clock::now();
            while (running_.load()) {
                cycle();
                if (options_.paced) {
                    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                    supple::sleepUntil(next);
                }
            }
            finishMidi();  // the final callback (Backend::stop contract)
        });
    }
    return okStatus();
}

void DummyBackend::stop() {
    const bool wasRunning = running_.exchange(false);
    if (thread_.joinable()) {
        thread_.join();
    } else if (wasRunning && callback_) {
        finishMidi();  // Manual: the caller is the audio thread
    }
}

// The final callback may still write MIDI (note releases, a sync runtime's
// farewell): one frame, captured.
void DummyBackend::finishMidi() {
    midiOutput_.begin(1, config_.midiOutputs ? options_.midiOutputPorts.size() : 0);
    callback_(nullptr, outPtrs_.data(), 0);
    captureMidi();
}

ThreadPolicy DummyBackend::callbackPolicy() {
    return thread_.joinable() ? threadPolicy(thread_.native_handle()) : ThreadPolicy{};
}

void DummyBackend::run(std::int64_t n) {
    for (std::int64_t i = 0; i < n && running_; ++i) cycle();
}

void DummyBackend::cycle() {
    float* const* out = outPtrs_.data();
    if (options_.input && !inPtrs_.empty())
        options_.input(inPtrs_.data(), config_.inputs, config_.blockSize, inputFrame_);
    midi_.clear();
    if (options_.midi && config_.midiInputs) {
        options_.midi(midi_, config_.blockSize, inputFrame_);
        if (midi_.size() > kMaxMidiInputEvents) midi_.resize(kMaxMidiInputEvents);
        for (std::size_t i = 1; i < midi_.size(); ++i)  // stable, allocation-free
            for (std::size_t k = i; k > 0 && midi_[k].frame < midi_[k - 1].frame; --k)
                std::swap(midi_[k], midi_[k - 1]);
    }
    sync_.clear();
    if (options_.sync && config_.midiInputs) {
        options_.sync(sync_, config_.blockSize, inputFrame_);
        if (sync_.size() > kMaxSyncMidiEvents) sync_.resize(kMaxSyncMidiEvents);
        for (std::size_t i = 1; i < sync_.size(); ++i)
            for (std::size_t k = i; k > 0 && sync_[k].frame < sync_[k - 1].frame; --k)
                std::swap(sync_[k], sync_[k - 1]);
    }
    midiOutput_.begin(config_.blockSize, config_.midiOutputs ? options_.midiOutputPorts.size() : 0);
    callback_(inPtrs_.empty() ? nullptr : inPtrs_.data(), out, config_.blockSize);
    captureMidi();
    inputFrame_ += config_.blockSize;
    const auto room = std::min<std::int64_t>(config_.blockSize, options_.captureFrames - captured_);
    if (room > 0) {
        for (std::size_t c = 0; c < outputs_.size(); ++c)
            std::copy_n(outputs_[c].begin(), room, capture_.channels[c].begin() + captured_);
        captured_ += room;
    }
    cycles_.fetch_add(1);
}

void DummyBackend::captureMidi() {
    for (const auto& event : midiOutput_.sorted()) {
        if (midiCapture_.size() == midiCapture_.capacity()) {
            midiOutput_.drop();
            midiAsyncDrops_[event.port].fetch_add(1);
        } else
            midiCapture_.push_back({inputFrame_ + event.frame, event});
    }
}

std::vector<std::string> DummyBackend::midiOutputPorts() const {
    return config_.midiOutputs ? options_.midiOutputPorts : std::vector<std::string>{};
}
std::vector<std::int64_t> DummyBackend::midiOutputLatency() const {
    auto result = options_.midiOutputLatency;
    result.resize(midiOutputPorts().size());
    return result;
}

// --- engine --------------------------------------------------------------------------

Engine::Engine(InstancePool& plugins, MediaCache& media, EngineOptions options)
    : rendererOutputs_(std::make_unique<OutputQueue>()),
      callbackOutputs_(std::make_unique<OutputQueue>()),
      plugins_(plugins),
      media_(media),
      options_(options) {
    idleOutputs_.reserve(4096);
    options_.renderBlock = std::clamp<std::uint32_t>(options_.renderBlock, 64, 8192);
    options_.renderAheadMs = std::clamp(options_.renderAheadMs, 10.0, 2000.0);
    processing_.reserve(kMaxInstances);
    liveProcessing_.reserve(kMaxInstances);
    if (options_.recycleBuffers) {
        bufferPool_ = std::make_unique<detail::BufferPool>();
        bufferPool_->poisonForTests = options_.poisonRecycledBuffers;
    }
}

Engine::~Engine() { stop(); }

std::uint64_t Engine::buffersReused() const { return bufferPool_ ? bufferPool_->reused() : 0; }
std::uint64_t Engine::buffersMade() const { return bufferPool_ ? bufferPool_->made() : 0; }

void Engine::setMidiPreferences(MidiPortPreferences inputs, MidiPortPreferences outputs) {
    midiInputsPreferences_ = std::move(inputs);
    midiOutputsPreferences_ = std::move(outputs);
    midiSettingsDirty_ = true;
}
std::uint32_t Engine::midiLeadFrames() const { return hardwareMidi_ ? hardwareMidi_->lead() : 0; }
std::uint32_t Engine::midiLeadForOffset(std::size_t port, double offsetMs) const {
    const auto* plan = latestPlan_.load(std::memory_order_acquire);
    return plan ? static_cast<std::uint32_t>(detail::hardwareMidiLead(*plan, port, offsetMs)) : 0;
}
std::uint32_t Engine::outputLatency() const { return outputLatency_ + midiLeadFrames(); }
std::uint64_t Engine::midiPortDropped(bool output, std::size_t port) const {
    return hardwareMidi_ ? hardwareMidi_->dropped(output, port) : 0;
}

unsigned Engine::liveHelpers() const { return liveWorkers_ ? liveWorkers_->helpers() : 0; }

std::vector<std::thread::native_handle_type> Engine::deadlineThreads() {
    std::vector<std::thread::native_handle_type> out;
    if (liveWorkers_) out = liveWorkers_->handles();
    const auto callback = callbackThread_.load(std::memory_order_relaxed);
    if (callback != std::thread::native_handle_type{}) out.insert(out.begin(), callback);
    return out;
}

Status Engine::start(Backend& backend, const BackendConfig& config) {
    if (backend_)
        return Error("engine.running", "the engine is already running", {},
                     "use engine.stop before changing the backend or starting another engine");
    if (config.outputs < 2 || config.blockSize == 0)
        return Error("engine.config", "the engine needs a block size and two outputs", {},
                     "provide a positive block_size and at least two output channels");
    hardwareMidi_ = std::make_unique<detail::HardwareMidiRuntime>();
    resetAuditions();  // the callback starts with nothing to play (ADR-0094)
    shutDown_ = false;
    shutdownRequested_ = false;
    // Before the backend starts: its callbacks push meter entries at once.
    // Kept across plan swaps; slots for a plan's strips are assigned in load().
    meters_ = std::make_shared<MeterBank>();
    if (mainWake_) meters_->setMainThreadWake(*mainWake_);
    if (options_.anticipative) {
        // Allocated before the backend starts: its callbacks read the ring at
        // once. Room for the render-ahead amount at up to 192 kHz.
        const auto chunks = static_cast<std::size_t>(
            std::ceil(options_.renderAheadMs / 1000.0 * 192000.0 / options_.renderBlock));
        deviceOutputs_ = std::min<std::uint32_t>(config.outputs, 64);
        ring_ = std::make_unique<detail::ChunkRing>(chunks + 2, options_.renderBlock, false,
                                                    deviceOutputs_);
        const TransportSnapshot t = transportSnapshot();  // the main thread writes it
        rtEpoch_ = t.epoch;
        loopC_ = t.loop;
        rtOffset_ = 0;
        rtPrimed_ = false;
        primeAhead_.store(INT64_MAX, std::memory_order_relaxed);
        livePlan_ = nullptr;
        classBase_ = nullptr;
        liveReady_ = liveEntered_ = false;
        callbackGen_.store(0);
    }
    deviceInputs_ =
        std::min<std::uint32_t>(config.inputs, 64);  // the callback's copy, set before it runs
    deviceOutputs_ = std::min<std::uint32_t>(config.outputs, 64);
    outputWakeAt_.store(INT64_MAX);
    outputWakeSerial_.fetch_add(1);
    outputWakeSeen_ = 0;
    midiSource_ = &backend;  // before any callback (backend_ is set after start returns)
    deviceRate_.store(config.sampleRate, std::memory_order_relaxed);
    syncDevice_ = 0;
    syncRt_ = nullptr;
    for (auto& n : syncOutSent_) n.store(0, std::memory_order_relaxed);
    for (auto& n : syncOutDropped_) n.store(0, std::memory_order_relaxed);
    syncSeq_.store(0, std::memory_order_relaxed);
    auto started = backend.start(config, [this](const float* const* in, float* const* out,
                                                std::uint32_t n) { cycle(in, out, n); });
    if (!started) {
        ring_.reset();
        return started;
    }
    config_ = backend.config();  // what the device granted (e.g. its rate)
    // Before any plan or sync runtime is published, so their release orders
    // it for every reader that loads it after acquiring one. BridgeCycle
    // loads it at the top of a cycle, before the plan: see there.
    deviceRate_.store(config_.sampleRate, std::memory_order_relaxed);
    hardwareMidi_->prepare(config_.sampleRate, config_.outputs);
    backend_ = &backend;
    startWarnings_.clear();
    syncInputLatency_.store(backend.status().inputLatency, std::memory_order_relaxed);
    if (auto synced = startSync(); !synced)
        startWarnings_.push_back("sync: " + synced.error().toString());
    scheduling_ = {};
    const unsigned requested = options_.workers < 0
                                   ? detail::Workers::defaultHelpers()
                                   : static_cast<unsigned>(std::min(options_.workers, 64));
    const unsigned helpers =
        std::min({requested, detail::LiveWorkers::maxHelpers(), backend.liveThreadCapacity()});
    if (helpers > 0) {
        try {
            liveWorkers_ = std::make_unique<detail::LiveWorkers>(
                helpers, detail::LiveWorkers::Hooks{},
                backend.nativeThreadPolicy() ? &backend : nullptr);
        } catch (const std::system_error& error) {
            startWarnings_.push_back(
                "live helper creation failed: " + std::string(error.what()) +
                "; using serial processing; lower Audio processing threads and restart the engine");
        }
        if (liveWorkers_)
            if (auto configured = liveWorkers_->configure(backend); !configured) {
                startWarnings_.push_back(configured.error().message + "; " +
                                         configured.error().hint);
                liveWorkers_.reset();
            }
    } else if (requested > 0) {
        startWarnings_.push_back(
            "live multiprocessing unavailable: fewer than two usable CPUs or "
            "backend scheduling information unavailable; using serial processing; "
            "check the backend and process CPU allowance, then restart the engine");
    }
    // Callbacks already exist, but cannot see a partially configured pool.
    liveWorkersReady_.store(liveWorkers_.get(), std::memory_order_release);
    scheduling_.callback = backend.callbackPolicy();
    if (liveWorkers_) scheduling_.live = liveWorkers_->policy();
    // A bridged plug-in on a live node runs in the callback's class (ADR-0081).
    host::setLiveBridgeAudioPolicy(scheduling_.callback.kind == ThreadPolicy::Kind::None
                                       ? ThreadPolicy{ThreadPolicy::Kind::Normal, 0}
                                       : scheduling_.callback,
                                   config_.blockSize, config_.sampleRate);
    if (options_.anticipative) {
        aheadFrames_ = std::max<std::int64_t>(
            2 * options_.renderBlock,
            static_cast<std::int64_t>(options_.renderAheadMs / 1000.0 * config_.sampleRate));
        aheadFrames_ = std::min<std::int64_t>(
            aheadFrames_, static_cast<std::int64_t>(ring_->mask - 1) * options_.renderBlock);
        primeAhead_.store(aheadFrames_, std::memory_order_release);
        const auto backgroundHelpers = options_.workers < 0
                                           ? detail::Workers::defaultHelpers()
                                           : static_cast<unsigned>(std::min(options_.workers, 64));
        Backend* native =
            backend.nativeThreadPolicy() && options_.background != EngineOptions::Background::Normal
                ? &backend
                : nullptr;
        if (native && backgroundHelpers + 3 > allowedCpuCount()) {
            native = nullptr;
            startWarnings_.push_back(
                "background audio threads run at normal priority: the pool "
                "must leave two usable CPUs free; lower Audio processing threads and restart");
        }
        // macOS takes a thread's QoS only from the thread itself, so without a
        // device policy the pool and the renderer set the background policy on
        // themselves as they start; elsewhere scheduleBackground() sets it.
        std::optional<ThreadPolicy> self;
#if defined(__APPLE__)
        selfApplied_ = okStatus();
        if (!backend.nativeThreadPolicy())
            if (const auto request = backgroundRequest(backgroundHelpers);
                request.want.kind == ThreadPolicy::Kind::High)
                self = request.want;
#endif
        // The renderer joins the pool's barrier (`extra`), so one refusal leaves
        // every background thread normal (ADR-0026 rule 3).
        workers_ = std::make_unique<detail::Workers>(backgroundHelpers, detail::Workers::Hooks{},
                                                     native, self, self ? 1u : 0u);
        if (auto policy = workers_->startupStatus(); !policy && native)
            startWarnings_.push_back(policy.error().toString());
        if (!workers_->startupStatus()) native = nullptr;
        std::shared_ptr<detail::ThreadStartup> rendererStartup;
        if (native) rendererStartup = std::make_shared<detail::ThreadStartup>(1, native, false);
        // The renderer's starting epoch is taken here, not on its thread: a
        // seek or pause issued before the thread runs must still be seen.
        const TransportSnapshot t = transportSnapshot();
        const std::uint64_t first = t.epoch;
        epochStartLocal_ = pos_;
        epochStart_.store(pos_);
        streamLocal_ = 0;
        renderedStream_.store(0);
        loopR_ = t.loop;
        epochDone_.store(first, std::memory_order_release);
        rendererQuit_ = false;
        rendererWakeAt_.store(INT64_MAX);
        const bool rendererSelf = self.has_value();
        try {
            renderer_ = std::thread([this, first, rendererStartup, rendererSelf, native] {
                nameThread("audio-render");
                (void)diagctx::registerThread(diagctx::ThreadRole::AudioRender);
                rt::prefaultStack();  // ADR-0025
                if (rendererSelf) (void)workers_->enterStartup(workers_->helpers());
                const bool elevated = rendererStartup && rendererStartup->enter(0);
                detail::ThreadStartup::Cleanup cleanup{elevated ? native : nullptr};
                rendererLoop(first);
            });
        } catch (...) {
            if (rendererSelf) workers_->cancelStartup();  // the helpers wait for it
            throw;
        }
        auto rendererPolicy = rendererStartup ? rendererStartup->finish() : okStatus();
#if defined(__APPLE__)
        if (self) selfApplied_ = workers_->finishStartup();
#endif
        scheduling_.renderer = {
            native && rendererPolicy ? ThreadPolicy::Kind::High : ThreadPolicy::Kind::Normal, 0};
        if (!rendererPolicy) startWarnings_.push_back(rendererPolicy.error().toString());
        // No plan exists before start() returns, so no bridged instance has
        // started processing with an older policy.
        scheduleBackground();
    }
    // Every helper has joined or refused by now (the startup barriers returned).
    if (const WorkgroupStatus group = backend.workgroup();
        group.state == "failed" || group.state == "partial")
        startWarnings_.push_back("audio workgroup " + group.state + ": " + group.message + "; " +
                                 group.hint);
    warnings_ = startWarnings_;
    return okStatus();
}

Engine::BackgroundRequest Engine::backgroundRequest(unsigned helpers) const {
    using Kind = ThreadPolicy::Kind;
    using Background = EngineOptions::Background;
    const ThreadPolicy callback = scheduling_.callback;
    BackgroundRequest request;
    ThreadPolicy& want = request.want;
    std::string& refused = request.refused;
    if (options_.background == Background::Auto ? callback.realtime()
                                                : options_.background != Background::Normal) {
        int priority = options_.background != Background::Auto && options_.backgroundPriority > 0
                           ? options_.backgroundPriority
                           : kBackgroundPriority;
        if (callback.realtime()) priority = std::min(priority, callback.priority - 1);
        priority = realtimePriorityLimit(priority);
        const int cpus = static_cast<int>(allowedCpuCount());
        const int threads = static_cast<int>(helpers) + 1;
        if (priority < 1)
            refused =
                "no real-time priority is available below the callback's and within "
                "RLIMIT_RTPRIO; grant real-time scheduling (the audio or realtime "
                "group's limits) and restart the engine";
        else if (threads + 2 > cpus)
            refused = "the renderer and its " + std::to_string(threads - 1) +
                      " helpers would leave fewer than two of the " + std::to_string(cpus) +
                      " allowed CPUs to the rest of the system; lower Audio processing threads "
                      "and restart the engine";
        else
#if defined(__linux__)
            want = {Kind::Fifo, priority};
#else
            want = {Kind::High, 0};
#endif
    }
    return request;
}

void Engine::scheduleBackground() {
    using Kind = ThreadPolicy::Kind;
    if (backend_->nativeThreadPolicy()) {
        scheduling_.background = workers_->policy();
        host::setBridgeAudioPolicy(scheduling_.renderer);
        return;
    }
    const ThreadPolicy normal{Kind::Normal, 0};
    auto [want, refused] = backgroundRequest(workers_->helpers());  // why none, if so
    if (want != normal) {
#if defined(__APPLE__)
        // Set by each thread on itself as it started (start()), all or none
        // across the pool, the renderer only after it.
        const Status applied = selfApplied_;
        if (!applied) refused = applied.error().message + "; " + applied.error().hint;
#else
        Status applied = workers_->setPolicy(want);
        if (applied) applied = setThreadPolicy(renderer_.native_handle(), want);
        if (!applied) {  // all or nothing: a slower straggler would delay every job
            (void)workers_->setPolicy(normal);
            (void)setThreadPolicy(renderer_.native_handle(), normal);
            refused = applied.error().message + "; " + applied.error().hint;
        }
#endif
    }
    if (!refused.empty())
        startWarnings_.push_back("background audio threads run at normal priority: " + refused);
    scheduling_.renderer = threadPolicy(renderer_.native_handle());
    scheduling_.background = workers_->policy();
    host::setBridgeAudioPolicy(scheduling_.renderer);
}

void Engine::sayFarewell() SUPPLE_RT {
    if (!syncRt_) return;
    syncRt_->farewell(midiSource_, syncOutSent_.data(), syncOutDropped_.data());
    syncTransportRunning_.store(false, std::memory_order_relaxed);
}

void Engine::stop() {
    if (!backend_) return;
    // A MIDI output's slave is playing: Stop goes out in a running cycle,
    // then has the device's latency to leave (bounded like the native
    // sender's close). The next, silenced cycle releases routed notes, so
    // each port gets its Stop before the note-offs; the final callback is
    // the fallback for a device that runs no more cycles (ADR-0073 §9).
    if (syncTransportRunning_.load(std::memory_order_acquire)) {
        const auto acked = syncFarewells_.load(std::memory_order_acquire);
        syncFarewell_.store(true, std::memory_order_release);
        const double period = config_.sampleRate > 0 ? config_.blockSize / config_.sampleRate : 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(std::max(0.05, 4 * period)));
        while (syncFarewells_.load(std::memory_order_acquire) == acked &&
               std::chrono::steady_clock::now() < deadline)
            supple::sleepFor(std::chrono::milliseconds(1));
        if (syncFarewells_.load(std::memory_order_acquire) != acked && config_.sampleRate > 0) {
            const double leave =
                static_cast<double>(syncOutputLatency_.load(std::memory_order_relaxed)) /
                    config_.sampleRate +
                0.005;
            std::this_thread::sleep_for(std::chrono::duration<double>(
                std::min(leave, detail::kMidiOutputCloseWaitSeconds)));
        }
    }
    // Silence from the next cycle on, then stop the backend: its final
    // zero-frame callback stops every plugin on the audio thread.
    shutdownRequested_.store(true, std::memory_order_release);
    stoppedOnAudioThread_ = false;
    if (renderer_.joinable()) {  // the renderer stops every plugin as it ends
        rendererQuit_.store(true, std::memory_order_release);
        wakeRenderer();
        notifyMain();
        renderer_.join();
    }
    recorder_.store(nullptr, std::memory_order_release);
    backend_->stop();
    liveWorkersReady_.store(nullptr, std::memory_order_release);
    liveWorkers_.reset();
    if (recorderOwned_) {  // no callback now: retain MIDI as well as the closed audio files
        auto takes = recorderOwned_->finish(readLoop());
        for (auto& take : takes) stoppedTakes_.push_back(std::move(take));
    }
    recorderOwned_.reset();
    // Every audio thread has joined, so nothing else touches the plugins: stop
    // any the audio thread left processing (a backend that broke its stop()
    // contract, or that ran a cycle after the request) here.
    for (host::Instance* in : processing_)
        if (in->processing()) {
            stoppedOnAudioThread_ = false;
            in->stopProcessing();
        }
    if (!liveProcessing_.empty()) stoppedOnAudioThread_ = false;
    for (host::Instance* in : liveProcessing_) in->stopProcessing();
    for (auto* list : {&processing_, &liveProcessing_}) {
        for (host::Instance* in : *list) in->setOwner(nullptr);
        list->clear();
    }
    // Both executors and their helpers have joined, including a backend
    // that omitted its final callback. Recover any retained module owners.
    for (const auto& plan : plans_)
        for (auto* in : plan->modules)
            if (in->owner() == &processing_ || in->owner() == &liveProcessing_) in->releaseOwner();
    backend_ = nullptr;
    plan_ = nullptr;
    livePlan_ = nullptr;
    classBase_ = nullptr;  // freed with plans_ below
    syncRt_ = nullptr;     // no callback runs now
    resetAuditions();      // a voice playing is dropped (ADR-0094)
    syncNext_.store(nullptr);
    syncAdopted_.store(nullptr);
    syncRuntimes_.clear();
    handPlan_.store(nullptr);
    detail::Plan* discarded = nullptr;
    while (planHandoffs_.pop(discarded)) {
    }
    while (ratePlanRequests_.pop(discarded)) {
    }
    latestPlan_.store(nullptr);
    bridgedLive_ = false;
    for (const auto& plan : plans_)
        for (auto* instance : plan->instances) instance->setAudioWake(nullptr);
    ratePreparedTail_ = nullptr;
    rateReseekGeneration_.store(0);
    rateReseekServiced_ = 0;
    rateDeferred_ = false;
    // A deferred edit dropped here never played either; the engine is
    // stopped, so the app's next start loads its head anyway.
    if (rateDeferredFrom_) latest_ = *std::exchange(rateDeferredFrom_, std::nullopt);
    scalarPending_ = false;
    scalarHeardEpoch_ = UINT64_MAX;
    ratePreparationSeconds_ = 0;
    rateDeferredWake_.store(false);
    rateSafeStream_.store(0);
    rateSafeEpoch_.store(0);
    rateScheduling_.store(false);
    staleCurves_.clear();  // no plan left to be stale
    effectivePlayrate_.store(1);
    rtTimeline_.store(0);
    timelineEpoch_.store(0);
    rateClockSequence_.store(0);
    for (auto& word : rateClockWords_) word.store(0);
    plans_.clear();     // nothing runs a plan now
    ringPool_.clear();  // nor holds a ring for the next edit
    hardwareMidi_->releaseHistory(true);
    callbackGen_.store(0);
    workers_.reset();
    ring_.reset();  // no callbacks after backend_->stop()
    scheduling_ = {};
    host::setBridgeAudioPolicy({ThreadPolicy::Kind::Normal, 0});
    host::setLiveBridgeAudioPolicy({ThreadPolicy::Kind::Normal, 0});
}

Status Engine::load(const model::Session& session, const std::filesystem::path& projectDir) {
    const std::shared_ptr<const model::Session> before = latest_;
    auto loaded = loadNow(session, projectDir);
    if (loaded && before) captureEdit(*before, session);  // a render's as_heard
    // A deferred edit compiles in the first load() past its deferral, whoever
    // calls it. If that failed, the edit never played: latest_ goes back to
    // the session that did, so the prune, the mix and scalar shortcuts and
    // later reloads do not act on it, and the app learns it (review 37 F1).
    // A failure before the deferral is decided leaves the edit deferred.
    if (!loaded && rateDeferredFrom_ && !rateDeferred_) {
        latest_ = *std::exchange(rateDeferredFrom_, std::nullopt);
        // A refused playrate move in that session is pending again: the
        // compile that cleared it never published it (review 37c F1).
        scalarPending_ = rateDeferredScalarPending_;
        unplayedEdit_ = loaded.error();
    }
    return loaded;
}

Status Engine::loadNow(const model::Session& session, const std::filesystem::path& projectDir) {
    if (recording()) SUPPLE_TRY(model::validateRecordingPlayrate(session));
    // Neither shortcut may release a hold whose compiled curve is stale; the
    // scalar one publishes no mix values, so it carries pending holds after.
    const bool compile = midiSettingsDirty_ || releasesStaleCurve();
    if (!compile && loadContextOnly(session, projectDir)) return {};
    if (!compile && loadScalarRate(session, projectDir)) {
        applyPendingHolds();
        return {};
    }
    if (!compile && loadMixValues(session, projectDir)) return {};
    if (!compile && loadPatches(session, projectDir)) {
        applyPendingHolds();
        return {};
    }
    const auto controlEpoch = epoch_.load(std::memory_order_acquire);
    const auto lead =
        options_.anticipative
            ? aheadFrames_
            : static_cast<std::int64_t>(config_.sampleRate * options_.renderAheadMs / 1000.);
    if (!rateSeekTarget_ && wantPlaying_.load(std::memory_order_acquire) &&
        rateSafeEpoch_.load(std::memory_order_acquire) == controlEpoch &&
        playedStream(controlEpoch) + lead < rateSafeStream_.load(std::memory_order_acquire)) {
        if (!rateDeferredFrom_) {
            rateDeferredFrom_ = latest_;  // what keeps playing meanwhile
            rateDeferredScalarPending_ = scalarPending_;
        }
        latest_ = std::make_shared<const model::Session>(session);
        rateDeferredDir_ = projectDir;
        rateDeferred_ = true;
        rateDeferredWake_.store(true);
        if (playedStream(controlEpoch) + lead >= rateSafeStream_.load()) notifyMain();
        return {};
    }
    rateDeferred_ = false;
    scalarPending_ = false;
    rateDeferredWake_.store(false);
    if (!notificationReload_)
        for (const auto& fx : session.fx) plugins_.resetAutomation(fx.id);
    if (!backend_)
        return Error("engine.stopped", "start the engine first", {},
                     "call engine.start with a valid backend before loading or recording");
    // Instances the compile replaces may still play in plans up to this one.
    plugins_.setGeneration(publishedGen_ + 1);
    // ADR-0014: a strip that leaves the live region (monitoring off, or auto
    // and disarmed) while the transport plays stays live until it stops. The
    // renderer could take its instance only at the render horizon, while the
    // callback still plays it up to there: the track would pass through for
    // the render-ahead time, or need a fresh instance without the plugin's
    // state and editor. The callback keeps it, unmonitored, and service()
    // reloads once the transport stands still.
    std::vector<model::Id> keepLive;
    if (options_.anticipative && !plans_.empty() &&
        (wantPlaying_.load(std::memory_order_acquire) ||
         reportedPlaying_.load(std::memory_order_acquire)))
        for (const detail::Node& n : plans_.back()->nodes)
            if (n.live) keepLive.push_back(n.id);
    std::sort(keepLive.begin(), keepLive.end());
    auto build = [&]() -> Result<std::unique_ptr<detail::Plan>> {
        // External sync input compiles the sync plan; anticipative engines
        // run it all live (a callback-only engine already does), and the rate
        // path is on when a chase can change the rate.
        const detail::SyncOverrides overrides{syncScale_, options_.anticipative,
                                              syncConfig_.chases()};
        auto compiled = detail::compile(
            session, plugins_, 0.0,
            options_.anticipative ? std::max(options_.renderBlock, config_.blockSize)
                                  : config_.blockSize,
            config_.sampleRate,  // the device's rate, whatever the project's
            &keepLive, recordingMasters_, syncConfig_.listens() ? &overrides : nullptr);
        if (!compiled) return std::move(compiled).error();
        auto built = std::make_unique<detail::Plan>(std::move(*compiled));
        built->projectDir = projectDir;
        built->media = &media_;
        built->rateBackendLatency = backend_->status().outputLatency;
        built->rateEnabled |= rateScheduling_.load(std::memory_order_acquire);
        built->buffers = bufferPool_.get();
        built->gestureHolds = true;  // ADR-0061, amended: the hand holds its lanes
        auto sources = media_.prepareSources(session, projectDir, config_.sampleRate);
        if (!sources) return sources.error();
        built->preparedSources = *sources;
        SUPPLE_TRY(detail::prepare(*built, session));
        return built;
    };
    auto first = build();
    if (!first) return std::move(first).error();
    std::unique_ptr<detail::Plan> plan = std::move(*first);
    {
        // A key connected or disconnected is part of a VST3's activation (its
        // auxiliary bus) and of a CLAP plug-in's with port activation, which a
        // processing instance cannot change: it gets a fresh one (ADR-0018).
        // Other CLAP plug-ins hear the key or silence as is.
        bool keyChanged = false;
        for (const detail::Node& n : plan->nodes)
            detail::forEachStage(n, [&](const detail::Stage& st) {
                if (st.instance && st.instance->processing() && st.instance->sidechainChannels() &&
                    st.instance->followsKeyActivation() &&
                    st.instance->sidechainActive() != st.keyed) {
                    plugins_.restart(st.fx);
                    keyChanged = true;
                }
            });
        if (keyChanged) {
            auto again = build();
            if (!again) return std::move(again).error();
            plan = std::move(*again);
        }
    }
    if (options_.verifyPlans) {
        // Route E stage 5: the same session compiled again gives the same
        // plan. A compile services each instance it takes, so a plug-in
        // that asked for a restart, or one the executors have since stopped,
        // can change the second (round 46 F1): then the later compile is
        // the plan, checked against another, a few times at most.
        for (int tries = 0;; ++tries) {
            const auto changes = plugins_.availabilityChanges();
            if (tries == 0 && betweenCompilesForTests_)
                betweenCompilesForTests_(betweenCompilesContext_);
            auto again = build();
            if (!again) return std::move(again).error();
            if (plugins_.availabilityChanges() != changes && tries < 3) {
                plan = std::move(*again);
                continue;
            }
            if (secondCompileForTests_) secondCompileForTests_(**again);
            const auto difference =
                detail::firstDifference(detail::dumpPlan(*plan), detail::dumpPlan(**again));
            if (!difference.empty())
                return Error(
                    "engine.plan_differs",
                    "a second compile of the same session gave a different plan: " + difference, {},
                    "an engine fault, not the session's: report it with the project");
            break;
        }
        ++verifiedPlans_;
    }
    // Bridged FX on live nodes run in the callback under its bridge deadline
    // (ADR-0081): no stage is passed through for being isolated.
    if (std::any_of(plan->nodes.begin(), plan->nodes.end(),
                    [](const detail::Node& n) { return n.midiIn.has_value() || n.rack; })) {
        const std::vector<std::string> ports = backend_->midiPorts();  // ADR-0016
        for (detail::Node& n : plan->nodes) {
            if (n.midiIn) n.midiIn->ports = midiPortMask(ports, n.midiIn->device);
            if (n.rack)  // Notes modules playing a device (ADR-0062)
                for (detail::NotesFeed& feed : n.rack->notes) {
                    if (feed.device) feed.ports = midiPortMask(ports, *feed.device);
                    if (feed.sourceDevice)
                        feed.sourcePorts = midiPortMask(ports, *feed.sourceDevice);
                }
        }
    }
    if (plan->instances.size() > kMaxInstances)
        return Error(
            "engine.too_many_plugins",
            "more than " + std::to_string(kMaxInstances) + " plugin instances", {},
            "remove unused FX or render tracks and remove their FX to reduce the number of "
            "live plugin instances");
    if (options_.anticipative) {
        const auto quantum = plan->rateEnabled ? 64u : options_.renderBlock;
        // Every ring keeps room for the render-ahead time in whole chunks at
        // up to 192 kHz. At lower rates that is the slack for short chunks:
        // no chunk crosses a loop wrap, so a loop shorter than a chunk fills
        // a ring with chunks that short, and a ring sized to the lead at the
        // device's rate would fill first (round 36 F1). Frontier and record
        // rings come from retired plans when their shape fits (route E
        // stage 1), so their size costs only when one is first built.
        const auto chunks =
            static_cast<std::size_t>(std::ceil(options_.renderAheadMs / 1000. * 192000. / quantum));
        recycleRings(oldestInUse());
        if (plan->anyLive) {
            plan->split = true;
            for (std::size_t i = 0; i < plan->frontier.size(); ++i)
                plan->frontierRings.push_back(takeRing(chunks + 2, quantum));
        } else if (plan->rateEnabled) {
            plan->rateOutput =
                std::make_unique<detail::ChunkRing>(chunks + 2, quantum, true, deviceOutputs_);
        }
        // A recorded master the renderer runs reaches the callback through a
        // record ring (ADR-0057).
        for (detail::RecordTap& tap : plan->recordTaps)
            if (!plan->split || !plan->nodes[tap.node].live) {
                tap.ring = static_cast<int>(plan->recordRings.size());
                plan->recordRings.push_back(takeRing(chunks + 2, quantum));
            }
    }
    // What the recorder takes for each recorded master: the node the callback
    // runs, or what it pulled from the master's record ring.
    for (detail::RecordTap& tap : plan->recordTaps) {
        const detail::Stereo& from =
            tap.ring >= 0 ? tap.pulled : detail::recordSignal(plan->nodes[tap.node]);
        plan->processedInputs.push_back(
            {plan->nodes[tap.node].id, {from.l.data(), from.r.data()}, 0});
    }
    SUPPLE_TRY(detail::prepareHardwareMidi(*plan, *backend_, midiInputsPreferences_,
                                           midiOutputsPreferences_));
    if (plan->hardwareLead) hardwareMidi_->ensureHistory();
    plan->rateBackendLatency += plan->hardwareLead;
    plan->rateDeviceMargin += plan->hardwareLead;
    midiSettingsDirty_ = false;
    outputLatency_ = static_cast<std::uint32_t>(plan->outputLatency);
    // Audio is heard after the plan's latency and the hardware MIDI lead,
    // which every sending port's clock and MTC follow too (ADR-0073 §10).
    syncOutputLatency_.store(
        plan->outputLatency + plan->hardwareLead + backend_->status().outputLatency,
        std::memory_order_relaxed);
    syncPlanLatency_.store(plan->outputLatency + plan->hardwareLead, std::memory_order_relaxed);
    syncAudioLead_.store(plan->hardwareLead, std::memory_order_relaxed);
    warnings_ = plan->warnings;
    for (const detail::OutputTap& o : plan->outputs)  // ADR-0056
        if (std::max(o.left, o.right) >= static_cast<int>(deviceOutputs_))
            warnings_.push_back(
                plan->nodes[o.node].id.str() + " plays to output " +
                std::to_string(std::max(o.left, o.right) + 1) + ", but the device has " +
                std::to_string(deviceOutputs_) +
                " outputs: set its hw_output, or start the engine with more outputs");
    warnings_.insert(warnings_.end(), startWarnings_.begin(), startWarnings_.end());
    // Metering (P5.3c): every strip of this plan gets its bank slot here, on
    // the main thread — never while an audio side reads it — and the lists the
    // executors iterate are built from the slots.
    if (meters_) {
        meters_->beginPlan();
        for (detail::Node& node : plan->nodes) {
            node.meterSlot = meters_->slotFor(node.id);
            node.meterGeneration = meters_->generationFor(node.meterSlot);
        }
        meters_->endPlan();
        detail::Plan& p = *plan;
        p.meterNodes.clear();
        p.rendererMeterNodes.clear();
        p.liveMeterNodes.clear();
        for (std::size_t i = 0; i < p.nodes.size(); ++i)
            if (p.nodes[i].meterSlot != MeterBank::kNoSlot) p.meterNodes.push_back(i);
        for (std::size_t i : p.rendererNodes)
            if (p.nodes[i].meterSlot != MeterBank::kNoSlot) p.rendererMeterNodes.push_back(i);
        for (std::size_t i : p.liveNodes)
            if (p.nodes[i].meterSlot != MeterBank::kNoSlot) p.liveMeterNodes.push_back(i);
    }
    std::optional<double> recoverySeek;
    plan->generation = ++publishedGen_;
    plan->rateEpochSeekSequence =
        seekSeq_.load(std::memory_order_relaxed) + (rateSeekTarget_ ? 1 : 0);
    if (plan->rateEnabled) {
        {
            if ((options_.anticipative ? ratePlanRequests_.free() : planHandoffs_.free()) == 0)
                return Error("playrate.busy", "the prepared-plan queue is full", {},
                             "let audio consume pending edits before retrying");
            auto prepared = prepareRatePlan(*plan);
            if (!prepared && !rateSeekTarget_ &&
                (prepared.error().code == "playrate.history" ||
                 prepared.error().code == "playrate.prepare_late")) {
                // The model edit is valid. Reuse locate's off-callback priming
                // and publish its epoch only once the new source state is ready.
                recoverySeek = transport().seconds;
                rateSeekTarget_ = *recoverySeek * config_.sampleRate;
                plan->rateEpochSeekSequence = seekSeq_.load(std::memory_order_relaxed) + 1;
                prepared = prepareRatePlan(*plan);
                rateSeekTarget_.reset();
            }
            SUPPLE_TRY(prepared);
        }
        if (plan->rateSafeDevice != INT64_MIN) {
            rateSafeStream_.store(plan->handAt + plan->rateSafeDevice - plan->rateHandDevice,
                                  std::memory_order_release);
            rateSafeEpoch_.store(plan->handEpoch, std::memory_order_release);
        }
        rateScheduling_.store(true, std::memory_order_release);
    }
    // The playing clips this plan lacks fade out from copies (ADR-0097).
    if (!plans_.empty()) detail::addRetiring(*plan, *plans_.back());
    latest_ = std::make_shared<const model::Session>(session);
    compiled_ = latest_;
    rateDeferredFrom_.reset();  // the deferred edit compiled with this one
    prunePending_ = true;
    if (options_.anticipative)
        for (auto* instance : plan->instances) instance->setAudioWake(&rendererEvent_);
    if (plan->rateEnabled) {
        // The input can alias the session snapshot replaced above.
        plan->requestedScalar = plan->masterPlayrate.rate;
        plan->scalarHistory.reserve(1024);
        for (auto& timeline : plan->timelines)
            timeline->bindScalarPublication(plan->scalarPublication.get());
        if (options_.anticipative)
            ratePlanRequests_.push(plan.get());
        else
            planHandoffs_.push(plan.get());
        ratePreparedTail_ = plan.get();
    }
    // Compiled from the session: no curve is stale; holds carry over (ADR-0061).
    if (!writeHolds_.empty()) detail::publishMix(*plan, session, writeHolds_);
    holdsPending_ = false;
    staleCurves_.clear();
    latestPlan_.store(plan.get(), std::memory_order_release);
    // The callback's instances: the live nodes', or all of them without the
    // renderer (ADR-0086).
    {
        const auto& callback = options_.anticipative ? plan->liveInstances : plan->instances;
        bridgedLive_ = std::any_of(callback.begin(), callback.end(),
                                   [](const host::Instance* in) { return in->isolated(); });
    }
    if (recoverySeek)
        writeTransport([&] {
            seekSample_.store(time::samplesAt(*recoverySeek, config_.sampleRate),
                              std::memory_order_relaxed);
            seekSeq_.fetch_add(1, std::memory_order_relaxed);
            seekEpoch_.store(epoch_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        });
    wakeRenderer();
    notifyMain();
    plans_.push_back(std::move(plan));
    collectPlans();
    plugins_.setGeneration(publishedGen_ + 1);  // later replacements: after this plan
    return okStatus();
}

template <class F>
void Engine::writeTransport(F&& change) {
    captureRelocating();  // ADR-0097 L4: a capture ends at a relocation
    const std::uint64_t seq = xportSeq_.load(std::memory_order_relaxed);
    xportSeq_.store(seq + 1, std::memory_order_relaxed);  // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    change();
    epoch_.fetch_add(1, std::memory_order_release);  // anticipative: drop what is rendered
    xportSeq_.store(seq + 2, std::memory_order_release);
    wakeRenderer();
    notifyMain();
}

bool Engine::seekToPlayStart() {
    const auto* prepared = latestPlan_.load(std::memory_order_acquire);
    const bool preparedSeek =
        !preparedSeekStale_ && prepared && prepared->rateSeekSequence != 0 &&
        prepared->preparedRateBoundary.epoch == epoch_.load(std::memory_order_acquire) &&
        prepared->rateSeekSequence == seekSeq_.load(std::memory_order_acquire);
    if (wantPlaying_.load(std::memory_order_acquire) ||
        !rateScheduling_.load(std::memory_order_acquire) || preparedSeek)
        return true;
    auto ready = prepareSeek(transport().seconds);
    if (!ready) {
        warnings_.push_back(ready.error().toString());
        notifyMain();
        return false;
    }
    // The seek begins a new epoch at the play position: the launches placed
    // while stopped, for the play start, are placed there again before the
    // transport runs (else service would place them once it runs, on the
    // next boundary).
    playStart_.emplace(seekEpoch_.load(std::memory_order_acquire),
                       seekSample_.load(std::memory_order_acquire));
    takeBackUnheard();  // what the stop left unheard is placed there too
    placeLaunchesAgain();
    playStart_.reset();
    return true;
}

void Engine::play() {
    if (!seekToPlayStart()) return;
    placeCapture();  // a capture armed while stopped begins at the play position
    wantPlaying_.store(true, std::memory_order_release);
    wakeRenderer();
    notifyMain();
}
void Engine::pause() {
    wantPlaying_.store(false, std::memory_order_release);
    writeTransport([] {});
}
void Engine::seek(double seconds) {
    auto ready = prepareSeek(seconds);
    if (!ready) {
        warnings_.push_back(ready.error().toString());
        notifyMain();
    }
}

Status Engine::prepareSeek(double seconds) {
    if (rateScheduling_.load(std::memory_order_acquire) && latest_) {
        rateSeekTarget_ = std::max(0., seconds) * config_.sampleRate;
        const auto dir = plans_.empty() ? std::filesystem::path{} : plans_.back()->projectDir;
        const auto status = load(*latest_, dir);
        rateSeekTarget_.reset();
        if (!status) return status;
        preparedSeekStale_ = false;
    }
    writeTransport([&] {
        seekSample_.store(time::samplesAt(std::max(0., seconds), config_.sampleRate),
                          std::memory_order_relaxed);
        seekSeq_.fetch_add(1, std::memory_order_relaxed);
        seekEpoch_.store(epoch_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (rateLoopTarget_) {
            loopStart_.store(rateLoopTarget_->start, std::memory_order_relaxed);
            loopEnd_.store(rateLoopTarget_->end, std::memory_order_relaxed);
            loopOn_.store(rateLoopTarget_->on, std::memory_order_relaxed);
        }
    });
    return {};
}

void Engine::setLoop(double startSeconds, double endSeconds, bool on) {
    if (rateScheduling_.load(std::memory_order_acquire) && latest_) {
        rateLoopTarget_ =
            LoopRange{time::samplesAt(std::max(0., startSeconds), config_.sampleRate),
                      time::samplesAt(std::max(0., endSeconds), config_.sampleRate), on};
        const auto ready = prepareSeek(transport().seconds);
        rateLoopTarget_.reset();
        if (!ready) {
            warnings_.push_back(ready.error().toString());
            notifyMain();
        }
        return;
    }
    writeTransport([&] {
        loopStart_.store(time::samplesAt(std::max(0.0, startSeconds), config_.sampleRate),
                         std::memory_order_relaxed);
        loopEnd_.store(time::samplesAt(std::max(0.0, endSeconds), config_.sampleRate),
                       std::memory_order_relaxed);
        loopOn_.store(on, std::memory_order_relaxed);
    });
}

bool Engine::readTransport(TransportSnapshot& t, int tries, std::uint64_t* seqOut) const SUPPLE_RT {
    for (int i = 0; i < tries; ++i) {
        const std::uint64_t seq = xportSeq_.load(std::memory_order_acquire);
        if (seq & 1) continue;
        t.epoch = epoch_.load(std::memory_order_relaxed);
        t.seekSeq = seekSeq_.load(std::memory_order_relaxed);
        t.seekSample = seekSample_.load(std::memory_order_relaxed);
        t.loop.start = loopStart_.load(std::memory_order_relaxed);
        t.loop.end = loopEnd_.load(std::memory_order_relaxed);
        t.loop.on = loopOn_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (xportSeq_.load(std::memory_order_relaxed) == seq) {
            if (seqOut) *seqOut = seq;
            return true;
        }
    }
    return false;
}

Engine::TransportSnapshot Engine::transportSnapshot() const {
    TransportSnapshot t;
    while (!readTransport(t, 64)) std::this_thread::yield();
    return t;
}

std::int64_t Engine::playedPosition(std::uint64_t epoch, std::int64_t epochStart) const {
    return rtPosEpoch_.load(std::memory_order_acquire) == epoch ? rtPos_.load() : epochStart;
}

std::int64_t Engine::anticipativePosition(std::uint64_t epoch) const {
    // Until the renderer has relocated for `epoch` (review round 70 P3: a
    // pause and a launch or capture in one main-thread turn), the position
    // relocate() will take: a seek's sample when a seek opened an epoch it
    // has not relocated for, else where the epoch it last relocated for was
    // heard (a pause or a loop change resumes there). Not that epoch's start.
    const auto done = epochDone_.load(std::memory_order_acquire);
    if (done < epoch) {
        if (seekEpoch_.load(std::memory_order_acquire) > done) return seekSample_.load();
        return playedPosition(done, epochStart_.load());
    }
    return playedPosition(epoch, epochStart_.load());
}

std::int64_t Engine::playedStream(std::uint64_t epoch) const {
    const auto stream = rtStream_.load();  // consumption/renderer-arm handshake
    return rtPosEpoch_.load(std::memory_order_acquire) == epoch ? stream : 0;
}

TransportState Engine::transport() const {
    TransportState t;
    t.playing = reportedPlaying_.load();
    t.countingIn = counting_.load(std::memory_order_relaxed);
    const auto epoch = epoch_.load(std::memory_order_acquire);
    if (rateScheduling_.load(std::memory_order_acquire) &&
        timelineEpoch_.load(std::memory_order_acquire) == epoch) {
        const double sample = rtTimeline_.load(std::memory_order_relaxed);
        t.sample = static_cast<std::int64_t>(std::llround(sample));
        t.seconds = sample / config_.sampleRate;
    } else {
        t.sample = options_.anticipative ? anticipativePosition(epoch) : reportedPos_.load();
        const auto* prepared = latestPlan_.load(std::memory_order_acquire);
        if (prepared && prepared->rateSeekSequence != 0 &&
            prepared->preparedRateBoundary.epoch == epoch)
            t.sample =
                static_cast<std::int64_t>(std::llround(prepared->rateControl[0].clock.timeline));
        t.seconds = double(t.sample) / config_.sampleRate;
    }
    t.loop = readLoop();
    return t;
}

Engine::MeterClock Engine::meterClock() const {
    MeterClock clock;
    clock.epoch = epoch_.load(std::memory_order_acquire);
    if (!wantPlaying_.load(std::memory_order_acquire) ||
        counting_.load(std::memory_order_relaxed)) {
        // Stopped (or counting in): the renderer renders nothing ahead, and what the callback
        // plays is monitored input, heard as it is produced.
        clock.heardStream = std::numeric_limits<std::int64_t>::max();
        return clock;
    }
    const std::int64_t played = playedStream(clock.epoch);
    const std::int64_t latency =
        backend_ ? static_cast<std::int64_t>(backend_->status().outputLatency + midiLeadFrames())
                 : 0;
    clock.heardStream = std::max<std::int64_t>(0, played - latency);
    return clock;
}

namespace {
std::atomic<double> gTransportWaitSeconds{0};  // 0: the default or the environment's
}  // namespace

double transportWaitSeconds() {
    if (const double set = gTransportWaitSeconds.load(std::memory_order_relaxed); set > 0)
        return set;
    static const double fromEnvironment = environmentFlagEnabled("SUPPLE_TEST_TRANSPORT_PATIENT")
                                              ? kPatientTransportSeconds
                                              : kTransportWaitSeconds;
    return fromEnvironment;
}

void setTransportWaitSeconds(double seconds) {
    gTransportWaitSeconds.store(std::isfinite(seconds) && seconds > 0 ? seconds : 0,
                                std::memory_order_relaxed);
}

namespace {
std::atomic<double> gAwaitSeconds{0};  // 0: the default or the environment's
}  // namespace

double awaitSeconds() {
    if (const double set = gAwaitSeconds.load(std::memory_order_relaxed); set > 0) return set;
    static const double fromEnvironment = environmentFlagEnabled("SUPPLE_TEST_TRANSPORT_PATIENT")
                                              ? kPatientTransportSeconds
                                              : kAwaitSeconds;
    return fromEnvironment;
}

void setAwaitSeconds(double seconds) {
    gAwaitSeconds.store(std::isfinite(seconds) && seconds > 0 ? seconds : 0,
                        std::memory_order_relaxed);
}

Status Engine::awaitTransport(double timeoutSeconds) {
    if (!backend_) return okStatus();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    // Whether it was done by the deadline: checked once more after it, so a
    // wait that slept past it still counts what happened meanwhile.
    auto waitUntil = [&](auto done) {
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return done();
            supple::sleepFor(std::chrono::microseconds(200));
        }
        return true;
    };
    bool done = true;
    if (options_.anticipative) {
        const std::uint64_t e = epoch_.load();
        const bool want = wantPlaying_.load();
        done = waitUntil([&] { return epochDone_.load() >= e && reportedPlaying_.load() == want; });
    } else if (backend_->ownsThread()) {
        const std::uint64_t before = cycles_.load();  // the first may have begun before
        done = waitUntil([&] { return cycles_.load() >= before + 2; });
    }
    if (done) return okStatus();
    return Error("engine.transport",
                 "the audio engine did not act on the transport request within " +
                     std::to_string(timeoutSeconds) +
                     " s; it stays queued and takes effect when the device runs again",
                 {},
                 "the audio device may have stalled: check engine.status (callbacks, xruns); do "
                 "not repeat the request, which is still queued, and restart the engine if the "
                 "device does not recover");
}

bool Engine::awaitPlan(double timeoutSeconds) {
    if (!ring_) return true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (stoppedGen_.load(std::memory_order_acquire) < publishedGen_)
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        else
            supple::sleepFor(std::chrono::microseconds(200));
    return true;
}

bool Engine::awaitRendered(std::int64_t frames, double timeoutSeconds) {
    if (!ring_) return true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    const std::uint64_t e = epoch_.load();
    const bool want = wantPlaying_.load();
    for (;;) {
        if (epochDone_.load() >= e && reportedPlaying_.load() == want) {
            if (!want) return true;
            const std::int64_t played = playedStream(e);
            // Before the epoch is heard the callback waits for a whole render
            // block (pre-roll, ADR-0012), unless a test holds the renderer short.
            std::int64_t goal = std::min(frames, aheadFrames_);
            if (played == 0)
                goal = std::max(goal, std::min<std::int64_t>(options_.renderBlock, aheadFrames_));
            goal = std::min(goal, holdFrom_.load(std::memory_order_acquire) - played);
            // A renderer waiting for room has rendered all it can hold.
            if (renderedStream_.load(std::memory_order_acquire) - played >= goal ||
                rendererWakeAt_.load() != INT64_MAX ||
                rendererLaunchHeld_.load(std::memory_order_acquire))  // held (round 209)
                return true;
        }
        if (std::chrono::steady_clock::now() > deadline) return false;
        // A renderer without work (every node live) waits for no consumption:
        // wake it, so it reports the stream position it keeps ahead.
        wakeRenderer();
        supple::sleepFor(std::chrono::microseconds(200));
    }
}

bool Engine::awaitRendererHeld(double timeoutSeconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (!holdSeen_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        supple::sleepFor(std::chrono::microseconds(200));
    }
    return true;
}

std::uint64_t Engine::oldestInUse() const {
    const std::uint64_t rendered = stoppedGen_.load(std::memory_order_acquire);
    return options_.anticipative ? std::min(rendered, callbackGen_.load(std::memory_order_acquire))
                                 : rendered;
}

void Engine::recycleRings(std::uint64_t oldest) {
    if (plans_.empty()) return;
    for (const auto& p : plans_) {
        // As collectPlans retires plans: no executor reaches these rings again.
        if (p->generation >= oldest || p.get() == plans_.back().get()) continue;
        for (auto* rings : {&p->frontierRings, &p->recordRings})
            for (auto& ring : *rings)
                if (ring) ringPool_.push_back(std::move(ring));
    }
    // Keep as many as the plans still in use hold: what edits at this pace
    // have needed at once.
    std::size_t keep = 0;
    for (const auto& p : plans_)
        if (p->generation >= oldest || p.get() == plans_.back().get())
            keep += p->frontierRings.size() + p->recordRings.size();
    if (ringPool_.size() > keep)
        ringPool_.erase(ringPool_.begin(),
                        ringPool_.begin() + static_cast<std::ptrdiff_t>(ringPool_.size() - keep));
}

std::unique_ptr<detail::ChunkRing> Engine::takeRing(std::size_t capacity, std::uint32_t frames) {
    for (auto it = ringPool_.rbegin(); it != ringPool_.rend(); ++it)
        if ((*it)->fits(capacity, frames)) {
            auto ring = std::move(*it);
            ringPool_.erase(std::next(it).base());
            ring->restart();
            return ring;
        }
    return std::make_unique<detail::ChunkRing>(capacity, frames);
}

std::uint64_t Engine::collectPlans() {
    // One reading decides both the rings recycled and the plans freed.
    const std::uint64_t oldest = oldestInUse();
    recycleRings(oldest);
    // The newest plan always stays (an executor may be about to adopt it).
    const auto retired = [&](const auto& p) {
        return p->generation < oldest && p.get() != plans_.back().get();
    };
    for (const auto& p : plans_) {
        if (retired(p) && options_.anticipative)
            for (auto* instance : p->instances) {
                const bool retained = std::any_of(plans_.begin(), plans_.end(), [&](const auto& q) {
                    return (q->generation >= oldest || q.get() == plans_.back().get()) &&
                           std::binary_search(q->instances.begin(), q->instances.end(), instance);
                });
                if (!retained) instance->setAudioWake(nullptr);
            }
    }
    // Their buffers serve the next compiles (as their rings, above).
    if (bufferPool_) {
        for (const auto& p : plans_)
            if (retired(p)) bufferPool_->harvest(*p);
        bufferPool_->trim();
    }
    std::erase_if(plans_, retired);
    if (std::none_of(plans_.begin(), plans_.end(), [](const auto& p) { return p->hardwareLead; }))
        (void)hardwareMidi_->releaseHistory();
    return oldest;
}

std::vector<model::Id> Engine::takeDirtyStates() { return std::exchange(dirtyStates_, {}); }

void Engine::armOutputWake(std::uint64_t epoch, std::int64_t stream) {
    outputWakeEpoch_.store(epoch, std::memory_order_relaxed);
    outputWakeAt_.store(stream, std::memory_order_relaxed);
    // Paired SC operations with the callback's stream store and serial load:
    // either it sees this arm, or our following recheck sees its consumption.
    outputWakeSerial_.fetch_add(1);
    if (stream != INT64_MAX && playedStream(epoch) >= stream) notifyMain();
}
void Engine::wakeOutputConsumed() SUPPLE_RT {
    const auto serial = outputWakeSerial_.load();
    if (serial == outputWakeSeen_) return;
    const auto epoch = outputWakeEpoch_.load(std::memory_order_relaxed);
    const auto target = outputWakeAt_.load(std::memory_order_relaxed);
    if (outputWakeSerial_.load() != serial) return;  // next cycle retries
    if (target == INT64_MAX) {
        outputWakeSeen_ = serial;
        return;
    }
    if (epoch == rtEpoch_ && rtStream_.load() >= target) {
        outputWakeSeen_ = serial;
        notifyMain();
    }
}

std::optional<RackLatencyInfo> Engine::rackLatencies(model::Id id, std::uint64_t version) const {
    const auto* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || plan->sessionVersion != version) return {};
    for (const auto& node : plan->nodes)
        if (node.rack && node.rack->rack == id) {
            RackLatencyInfo result;
            result.latency = node.latChain;
            for (std::size_t i = 0; i < node.rack->outIds.size(); ++i)
                result.outputs[node.rack->outIds[i]] = node.rack->outLatencies[i];
            for (const auto& out : node.rack->notesOut) result.outputs[out.id] = out.latency;
            return result;
        }
    return {};
}

void Engine::service() {
    reclaimAuditions();  // voices the callback has moved past (ADR-0094)
    // Sync runtimes the callback has moved past (ADR-0071).
    if (const auto* adopted = syncAdopted_.load(std::memory_order_acquire)) {
        const auto at = std::find_if(syncRuntimes_.begin(), syncRuntimes_.end(),
                                     [&](const auto& rt) { return rt.get() == adopted; });
        if (at != syncRuntimes_.end()) syncRuntimes_.erase(syncRuntimes_.begin(), at);
    }
    bool reload = false;
    bool scalarLate = false;
    for (const auto& p : plans_)
        if (p->rateEnabled)
            for (const auto& timeline : p->timelines)
                scalarLate |= timeline->scalarLate.exchange(false, std::memory_order_acq_rel);
    if (scalarLate && latest_) {
        const auto ready = prepareSeek(transport().seconds);
        if (!ready) warnings_.push_back("playrate recovery: " + ready.error().toString());
    }
    if (scalarPending_ && latest_) {
        const auto session = latest_;
        const auto dir = rateDeferredDir_;
        loadScalarRate(*session, dir);
    }
    serviceLaunches();  // the clip launcher's acks and misses (ADR-0097)
    placeCapture();
    // ADR-0096: modulation events that had to wait are reported, not lost.
    if (const auto deferred = modDeferred_.load(); deferred != modDeferredWarned_) {
        warnings_.push_back("modulation: " + std::to_string(deferred - modDeferredWarned_) +
                            " events waited past their grid point for room in a block "
                            "(modulation.events)");
        modDeferredWarned_ = deferred;
    }
    const auto reseek = rateReseekGeneration_.load(std::memory_order_acquire);
    if (reseek != 0 && reseek != rateReseekServiced_) {
        rateReseekServiced_ = reseek;
        const auto* requested = latestPlan_.load(std::memory_order_acquire);
        // A newer prepared locate supersedes the failed queue entry. Ordinary
        // edits remain ordered behind it, so recover with the latest model.
        if (latest_ && requested &&
            (requested->generation == reseek || requested->rateSeekSequence == 0)) {
            const auto ready = prepareSeek(transport().seconds);
            if (!ready) warnings_.push_back("playrate recovery: " + ready.error().toString());
        }
    }
    if (rateDeferred_ && latest_) {
        const auto e = epoch_.load(std::memory_order_acquire);
        const auto lead =
            options_.anticipative
                ? aheadFrames_
                : static_cast<std::int64_t>(config_.sampleRate * options_.renderAheadMs / 1000.);
        if (!wantPlaying_.load(std::memory_order_acquire) ||
            e != rateSafeEpoch_.load(std::memory_order_acquire) ||
            playedStream(e) + lead >= rateSafeStream_.load(std::memory_order_acquire)) {
            const auto session = *latest_;
            const auto dir = rateDeferredDir_;
            rateDeferred_ = false;
            const auto loaded = load(session, dir);
            if (!loaded)
                warnings_.push_back("deferred playrate edit: " + loaded.error().toString() +
                                    "; it never played: the engine plays the session before it");
        }
    }
    if (latest_) {
        const auto clock = transport();
        std::set<host::Instance*> serviced;
        auto drainIdle = [&](host::Instance& instance, model::Id fx, std::uint64_t generation) {
            auto idle = instance.takeIdleOutputs();
            idleOutputDrops_ += idle.dropped;
            if (idle.dropped != 0) noteDroppedFx(fx);
            for (const auto& event : idle.events) {
                if (idleOutputs_.size() == 4096) {
                    ++idleOutputDrops_;
                    noteDroppedFx(fx);
                    continue;
                }
                PluginOutput output;
                output.fx = fx;
                output.planGeneration = generation;
                output.epoch = epoch_.load(std::memory_order_acquire);
                output.patchSequence = patchSeq_.load(std::memory_order_relaxed);
                output.timelineSample = clock.sample;
                output.event = event;
                output.idle = true;
                idleOutputs_.push_back(output);
            }
        };
        for (const auto& fx : latest_->fx) {
            auto* instance = plugins_.find(fx.id);
            if (!instance) continue;
            serviced.insert(instance);
            const bool active = instance->active();
            const bool restarting = plugins_.restarting(fx.id);
            const bool cleared = plugins_.automationCleared(fx.id);
            auto notes = plugins_.service(fx.id);
            if (notes.stateDirty &&
                std::find(dirtyStates_.begin(), dirtyStates_.end(), fx.id) == dirtyStates_.end())
                dirtyStates_.push_back(fx.id);
            // A restart waiting on processing takes the plug-in off the
            // executors; once restarted, its (new) latency is compensated.
            // A values rescan matters only to automation (ADR-0092): one the
            // plug-in had cleared plays again, and an envelope's values are
            // sent again over what the plug-in changed.
            const bool rescan =
                detail::rescansParamList(notes) ||
                (notes.paramsRescan && (cleared || detail::automated(*latest_, fx.id)));
            reload |= rescan || !notes.paramsClear.empty() || notes.restartError.has_value() ||
                      active != instance->active() || notes.latencyChanged ||
                      restarting != plugins_.restarting(fx.id);
            drainIdle(*instance, fx.id, publishedGen_);
        }
        // Replaced instances still belong to older executor plans. Keep their
        // callbacks/timers and idle sinks serviced until both executors retire them.
        for (auto it = plans_.rbegin(); it != plans_.rend(); ++it)
            for (const auto& node : (*it)->nodes)
                detail::forEachStage(node, [&](const detail::Stage& st) {
                    if (!st.instance || !serviced.insert(st.instance).second) return;
                    st.instance->serviceMainThread();
                    st.instance->takeNotifications();
                    drainIdle(*st.instance, st.fx, (*it)->generation);
                });
        // A failed compilation may retire an instance before it appears in a
        // plan. Its subscriptions remain live until pruning is safe.
        for (auto* instance : plugins_.instances())
            if (serviced.insert(instance).second) {
                instance->serviceMainThread();
                instance->takeNotifications();
                instance->takeIdleOutputs();
            }
    }
    // A strip kept live while playing (load()) leaves once the transport stops.
    if (!plans_.empty() && plans_.back()->heldLive &&
        !wantPlaying_.load(std::memory_order_acquire) &&
        !reportedPlaying_.load(std::memory_order_acquire))
        reload = true;
    if (reload && backend_ && latest_) {
        const auto session = *latest_;
        const auto dir = plans_.empty() ? std::filesystem::path{} : plans_.back()->projectDir;
        notificationReload_ = true;
        const auto loaded = load(session, dir);
        notificationReload_ = false;
        if (!loaded)
            warnings_.push_back("plugin notification reload: " + loaded.error().toString());
    }
    // The pool is pruned at the generation the plans were collected at: a
    // newer reading may let an executor's adoption since then free an
    // instance that a plan kept above still names (the next service()
    // walks those plans).
    const std::uint64_t live = collectPlans();
    if (afterCollectForTests_) afterCollectForTests_(afterCollectContext_);
    if (prunePending_ && !rateDeferred_ && compiled_ && live >= publishedGen_) {
        // Every executor has adopted the newest plan and stopped what it
        // dropped. Pruned against that plan's own session: latest_ may name
        // one that never compiled.
        plugins_.prune(*compiled_, live);
        prunePending_ = false;
    }
}

PluginOutputs Engine::takePluginOutputs() {
    PluginOutputs out;
    out.events = std::move(idleOutputs_);
    idleOutputs_.clear();
    idleOutputs_.reserve(4096);
    for (auto* queue : {rendererOutputs_.get(), callbackOutputs_.get()}) {
        const auto count = queue->size();
        PluginOutput event;
        for (std::size_t i = 0; i < count && queue->pop(event); ++i) out.events.push_back(event);
    }
    out.dropped = pluginOutputDrops();
    out.voiceDropped = voiceDrops_.load();
    // A slot not yet written (a drop racing this take) or a full table: all.
    const auto count = droppedFxCount_.exchange(0, std::memory_order_acq_rel);
    out.droppedAll = droppedFxAll_.exchange(false, std::memory_order_acq_rel) || count > kDroppedFx;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, kDroppedFx); ++i) {
        const auto v = droppedFx_[i].exchange(0, std::memory_order_acq_rel);
        if (v == 0) {
            out.droppedAll = true;
            continue;
        }
        const model::Id id{static_cast<model::Kind>((v >> 32) - 1),
                           static_cast<std::uint32_t>(v & 0xffffffffu)};
        if (std::find(out.droppedFx.begin(), out.droppedFx.end(), id) == out.droppedFx.end())
            out.droppedFx.push_back(id);
    }
    return out;
}

void Engine::noteDroppedFx(model::Id fx) SUPPLE_RT {
    const std::uint64_t v = (static_cast<std::uint64_t>(fx.kind) + 1) << 32 | fx.n;
    const auto known = std::min<std::uint32_t>(droppedFxCount_.load(std::memory_order_acquire),
                                               static_cast<std::uint32_t>(kDroppedFx));
    for (std::uint32_t i = 0; i < known; ++i)
        if (droppedFx_[i].load(std::memory_order_relaxed) == v) return;
    const auto slot = droppedFxCount_.fetch_add(1, std::memory_order_acq_rel);
    if (slot < kDroppedFx)
        droppedFx_[slot].store(v, std::memory_order_release);
    else
        droppedFxAll_.store(true, std::memory_order_release);
}

void Engine::publishPluginOutputs(detail::Plan& p, bool renderer, std::uint64_t epoch,
                                  std::int64_t pos, std::int64_t stream,
                                  std::uint64_t patchSequence) SUPPLE_RT {
    auto& queue = renderer ? *rendererOutputs_ : *callbackOutputs_;
    bool changed = false;
    for (auto& node : p.nodes) {
        if (options_.anticipative && node.live == renderer) continue;
        // A MIDI send's notes or events past its prepared capacity (ADR-0067)
        // count with the voices an instrument could not track.
        changed |= node.midiDropped != 0;
        voiceDrops_.fetch_add(node.midiDropped, std::memory_order_relaxed);
        node.midiDropped = 0;
        detail::forEachStage(node, [&](detail::Stage& st) {
            changed |= st.voiceDropped != 0 || st.outEvents.dropped != 0;
            voiceDrops_.fetch_add(st.voiceDropped, std::memory_order_relaxed);
            st.voiceDropped = 0;
            modDeferred_.fetch_add(st.modDeferred, std::memory_order_relaxed);
            st.modDeferred = 0;
            // The FX before its count, so a drain that sees the count can name it.
            if (st.outEvents.dropped != 0) noteDroppedFx(st.fx);
            outputDrops_.fetch_add(st.outEvents.dropped, std::memory_order_release);
            for (std::size_t i = 0; i < st.outEvents.count; ++i) {
                const auto& event = st.outEvents.storage[i];
                // Voice ends and note/MIDI output stay on the audio path;
                // the main thread takes parameter outputs only.
                if (event.kind == host::OutputEvent::Kind::NoteEnd || event.midiOutput()) continue;
                changed = true;
                PluginOutput output;
                output.fx = st.fx;
                output.planGeneration = p.generation;
                output.epoch = epoch;
                output.patchSequence = patchSequence;
                output.timelineSample = time::samplesAt(p.startSeconds, p.rate) +
                                        static_cast<std::int64_t>(std::llround(p.rateSample(
                                            pos + event.time - st.inputLatency, node.live)));
                output.streamSample = stream + event.time;
                output.event = event;
                output.anticipative = renderer;
                if (!queue.push(output)) {
                    noteDroppedFx(st.fx);
                    outputDrops_.fetch_add(1, std::memory_order_release);
                }
            }
        });
    }
    if (changed) notifyMain();
}

namespace {
// An instance whose start_processing() failed: nobody retries it until a plan
// that uses it is adopted again (its stage passes audio through).
const char kRefused = 0;
}  // namespace

void Engine::adoptShare(std::vector<host::Instance*>& owned,
                        const std::vector<host::Instance*>* next, const void* self,
                        const std::vector<host::Instance*>* handed) SUPPLE_RT {
    // Stop what the new plan no longer gives this executor, keep the rest
    // (lists are sorted; capacity was reserved and checked in load()).
    std::size_t kept = 0;
    for (host::Instance* in : owned) {
        if (next && std::binary_search(next->begin(), next->end(), in)) {
            owned[kept++] = in;
        } else {
            // Leaving this executor (removed, or moving to the other one):
            // no voice may hang on, e.g. live MIDI notes when monitoring stops.
            // One `handed` over continues where this executor left it (the
            // renderer's, into the live region: it holds no device notes).
            if (!handed || !std::binary_search(handed->begin(), handed->end(), in)) in->reset();
            in->stopProcessing();
            in->setOwner(nullptr);
        }
    }
    owned.resize(kept);  // shrinking never allocates
    if (next)
        for (host::Instance* in : *next)
            if (in->owner() == &kRefused) in->setOwner(nullptr);  // retry with a new plan
    startFree(owned, next, self);
}

void Engine::startFree(std::vector<host::Instance*>& owned,
                       const std::vector<host::Instance*>* next, const void* self) SUPPLE_RT {
    if (!next) return;
    for (host::Instance* in : *next) {
        // Ours already, refused, or still owned by the other executor. The
        // other executor may find the same instance free at the same moment
        // (one handed into the live region while a newer plan, stopped or
        // after an fx.move, gives it back to the renderer): the claim is one
        // compare-exchange, so only one of them starts it.
        if (in->owner() || in->processing() || !in->claimOwner(self)) continue;
        if (!in->startProcessing()) {
            // Refused: its stage passes audio through until a plan retries
            // it. It forgets what it holds here, where this executor has just
            // tried to start it: an instance handed over unreset (the
            // renderer's held note) that the callback then restarts must not
            // resume that note when a retry succeeds, untracked, so never ended.
            in->reset();
            if (in->startUnanswered()) {
                // Not refused: a bridge that has not answered by the live
                // deadline (ADR-0081). Looked at again next cycle, without a
                // wait; one that never answers fails at the lifecycle
                // deadline, as a start off the callback (ADR-0101).
                in->setOwner(nullptr);
                continue;
            }
            in->setOwner(&kRefused);
            processErrors_.fetch_add(1);
            notifyMain();
            continue;
        }
        owned.push_back(in);  // within the reserved capacity
    }
}

void Engine::resetOwned(const std::vector<host::Instance*>& owned, detail::Plan* plan) SUPPLE_RT {
    for (host::Instance* in : owned) in->reset();
    if (!plan) return;
    auto owns = [&](host::Instance* in) {
        return in &&
               (in->owner() == &owned ||
                (!in->processing() && std::find(owned.begin(), owned.end(), in) != owned.end()));
    };
    for (auto& node : plan->nodes) {
        detail::forEachStage(node, [&](detail::Stage& st) {
            if (!owns(st.instance)) return;
            st.voices.clear();
            st.pendingVoices.clear();
            st.sleeping = false;
        });
        if (node.noteTarget >= 0 &&
            owns(node.chain[static_cast<std::size_t>(node.noteTarget)].instance)) {
            node.held.fill(0);
            node.liveHeld.fill(0);
        }
        for (auto& take : node.takeChains)
            if (take.noteTarget >= 0 &&
                owns(take.chain[static_cast<std::size_t>(take.noteTarget)].instance))
                take.held.fill(0);
    }
}

void Engine::cycle(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT {
    struct InCycle {
        Engine& engine;
        std::uint64_t errors, misses, generation, stopped, epoch;
        bool playing;
        float* const* out;
        std::uint32_t frames;
        explicit InCycle(Engine& e, float* const* output, std::uint32_t count)
            : engine(e),
              errors(e.processErrors_.load()),
              misses(e.misses_.load()),
              generation(e.callbackGen_.load()),
              stopped(e.stoppedGen_.load()),
              epoch(e.rtPosEpoch_.load()),
              playing(e.reportedPlaying_.load()),
              out(output),
              frames(count) {
            e.cycleSeq_.fetch_add(1);
        }
        ~InCycle() {
            auto& e = engine;
            if (e.hardwareMidi_)
                e.hardwareMidi_->finish(
                    out, e.deviceOutputs_, frames,
                    frames == 0 || e.shutdownRequested_.load(std::memory_order_relaxed));
            e.cycleSeq_.fetch_add(1, std::memory_order_release);
            e.wakeOutputConsumed();
            if (errors != e.processErrors_.load() || misses != e.misses_.load() ||
                generation != e.callbackGen_.load() || stopped != e.stoppedGen_.load() ||
                epoch != e.rtPosEpoch_.load() || playing != e.reportedPlaying_.load())
                e.notifyMain();
        }
    } inCycle(*this, out, frames);
#if defined(__linux__)
    // The thread the device calls on, so its page faults can be counted
    // (deadlineThreads()). pthread_self() only reads thread-local storage.
    if (const auto self = currentThreadHandle();
        callbackThread_.load(std::memory_order_relaxed) != self)
        callbackThread_.store(self, std::memory_order_relaxed);
#endif
    // ADR-0081: bridged calls on this thread, and on the live helpers serving
    // this cycle, wait no later than the cycle's start plus its period or
    // 2 ms, whichever is larger. A cycle with a late call ran past its period.
    // The period is fixed here, before any bridged call (the final callback
    // below stops plug-ins), so before the cycle acquires its plan: in the
    // one cycle that first adopts a plan, a callback preempted between here
    // and that acquire for longer than the rest of start() and a whole load()
    // may still read the requested rate, and that cycle's deadline is off by
    // the requested-to-granted ratio. Later cycles read the granted rate.
    struct BridgeCycle {
        Engine& engine;
        std::uint64_t late = host::bridgeLateCalls();
        host::BridgeWaits waits = host::bridgeLiveWaits();
        std::int64_t start = host::steadyNanoseconds();
        std::int64_t period;
        host::LiveBridgeDeadline deadline;
        BridgeCycle(Engine& e, std::uint32_t count)
            : engine(e),
              period(static_cast<std::int64_t>(count * 1e9 /
                                               e.deviceRate_.load(std::memory_order_relaxed))),
              deadline(start + std::max<std::int64_t>(
                                   static_cast<std::int64_t>(e.options_.liveBridgeDeadlineMs * 1e6),
                                   period)) {}
        ~BridgeCycle() {
            const bool lateCall = host::bridgeLateCalls() != late;
            if (lateCall) {
                engine.bridgeLate_.fetch_add(1, std::memory_order_relaxed);
                engine.notifyMain();
            }
            // Why a cycle with bridged calls ran past its period (ADR-0081
            // diagnostics). The waits are summed over this cycle's threads.
            const host::BridgeWaits now = host::bridgeLiveWaits();
            if (now.calls == waits.calls) return;
            const std::int64_t took = host::steadyNanoseconds() - start;
            std::int64_t longest = engine.longestBridgedCycle_.load(std::memory_order_relaxed);
            while (took > longest && !engine.longestBridgedCycle_.compare_exchange_weak(
                                         longest, took, std::memory_order_relaxed)) {
            }
            if (took <= period) return;
            // The bridge is to blame only when its waits cover the overrun:
            // a call posted after the deadline had passed is late, but the
            // time went before it.
            if (now.waitNs - waits.waitNs < took - period)
                engine.overrunNotWaiting_.fetch_add(1, std::memory_order_relaxed);
            else if (lateCall)
                engine.overrunLateWait_.fetch_add(1, std::memory_order_relaxed);
            else
                engine.overrunSlowBridge_.fetch_add(1, std::memory_order_relaxed);
        }
    } bridgeCycle(*this, frames);
    hardwareMidi_->begin(*midiSource_, frames);
    if (frames == 0) {  // the backend's final callback: stop every plugin here
        detail::adoptRackModules(livePlan_ ? &livePlan_->liveModules : nullptr, nullptr,
                                 &liveProcessing_);
        if (!options_.anticipative)
            detail::adoptRackModules(plan_ ? &plan_->modules : nullptr, nullptr, &processing_);
        sayFarewell();  // if no running cycle sent it
        syncFarewell_.store(false, std::memory_order_relaxed);
        // Anticipative: the renderer stopped its share as it ended; the live
        // share is this thread's.
        for (host::Instance* inst : liveProcessing_) {
            inst->stopProcessing();
            inst->setOwner(nullptr);
        }
        liveProcessing_.clear();
        // In the callback's executor, whatever processing_ still holds, shut
        // down or not. Anticipative, it is the renderer's, which stopped it as
        // it ended (shutDown_), so only a renderer that never ran leaves any.
        if (!options_.anticipative || !shutDown_.load()) {
            for (host::Instance* inst : processing_) {
                inst->stopProcessing();
                inst->setOwner(nullptr);
            }
            processing_.clear();
        }
        if (!shutDown_.load()) {
            playing_ = false;
            stoppedOnAudioThread_ = true;
            shutDown_.store(true);
        }
        return;
    }
    deviceMidi_ =
        hardwareMidi_->input(midiSource_->midiInput(), latestPlan_.load(std::memory_order_acquire));
    pushControlMidi();
    // External sync (ADR-0071): the runtime the main thread published, and
    // the transport as the previous cycle left it.
    if (auto* next = syncNext_.load(std::memory_order_acquire); next != syncRt_) {
        sayFarewell();  // the old runtime stops its slaves before it is dropped
        syncRt_ = next;
        syncAdopted_.store(next, std::memory_order_release);
    }
    if (syncFarewell_.load(std::memory_order_acquire)) {  // the engine stops
        sayFarewell();
        syncFarewell_.store(false, std::memory_order_relaxed);
        syncFarewells_.fetch_add(1, std::memory_order_release);
    }
    const double timelineBefore = rtTimeline_.load(std::memory_order_relaxed);
    const auto epochBefore = timelineEpoch_.load(std::memory_order_acquire);
    cycleTransport(in, out, frames);
    // After the project's output, before LTC (runSync) and the hardware
    // MIDI lead's delay (~InCycle), which apply to every channel.
    mixAudition(out, frames);
    runSync(in, out, frames, timelineBefore, epochBefore, syncPlayed_);
    syncDevice_ += frames;
}

void Engine::cycleTransport(const float* const* in, float* const* out,
                            std::uint32_t frames) SUPPLE_RT {
    // The play request first, then a count-in request (playCountIn stores them
    // in the other order), so this cycle cannot play past a count it missed.
    want_ = wantPlaying_.load(std::memory_order_acquire);
    if (const std::uint64_t seq = countSeq_.load(std::memory_order_acquire); seq != countSeen_) {
        countSeen_ = seq;
        countLeft_ = countFrames_.load(std::memory_order_relaxed);
        countBeatA_ = countBeat_.load(std::memory_order_relaxed);
        countBarA_ = countBar_.load(std::memory_order_relaxed);
        countPos_ = 0;
    }
    if (!want_) countLeft_ = 0;  // a stop cancels the count
    if (countLeft_ > 0) {
        // The count holds the transport for its part of this cycle (heard as
        // stopped, plus the clicks); the rest of the cycle plays.
        counting_.store(true, std::memory_order_relaxed);
        const auto k = static_cast<std::uint32_t>(std::min<std::int64_t>(countLeft_, frames));
        countHold_ = true;
        cycleBody(in, out, k, 0);
        countHold_ = false;
        countClicks(out, k);
        countLeft_ -= k;
        if (k < frames) {
            std::array<const float*, 64> inAt{};
            for (std::uint32_t c = 0; c < deviceInputs_ && c < inAt.size(); ++c)
                inAt[c] = in && in[c] ? in[c] + k : nullptr;
            const auto outAt = advanced(out, k);
            cycleBody(in ? inAt.data() : nullptr, outAt.data(), frames - k, k);
        }
        counting_.store(countLeft_ > 0, std::memory_order_relaxed);
        return;
    }
    cycleBody(in, out, frames, 0);
}

// External sync (ADR-0071): after the transport's work, so the LTC output
// replaces its channel after the masters are written, and the tracker sees
// the timeline this cycle played.
void Engine::runSync(const float* const* in, float* const* out, std::uint32_t frames,
                     double timelineBefore, std::uint64_t epochBefore,
                     bool playingBefore) SUPPLE_RT {
    detail::SyncRuntime* rt = syncRt_;
    if (!rt) return;
    const double rate = deviceRate_.load(std::memory_order_relaxed);
    const double after = rtTimeline_.load(std::memory_order_relaxed);
    const auto epochAfter = timelineEpoch_.load(std::memory_order_acquire);
    // The timeline is published only for a cycle that played.
    const bool playing = want_ && !counting_.load(std::memory_order_relaxed) &&
                         epochAfter == rtEpoch_ &&
                         (after != timelineBefore || epochAfter != epochBefore);
    double before = timelineBefore;
    // A loop wrap inside the cycle: the timeline ran from `before` to the
    // loop's end, then from its start to `after`.
    const bool wrapped = playing && playingBefore && epochAfter == epochBefore && after < before &&
                         loopC_.active() && before <= static_cast<double>(loopC_.end) &&
                         after >= static_cast<double>(loopC_.start);
    if (!wrapped && (!playingBefore || epochAfter != epochBefore || !(after >= before)))
        before = after - static_cast<double>(frames);  // a start or seek
    detail::SyncCycle c;
    c.in = in;
    c.inputs = deviceInputs_;
    c.out = out;
    c.outputs = deviceOutputs_;
    c.frames = frames;
    if (midiSource_) c.midi = midiSource_->syncMidi();
    c.device = syncDevice_;
    c.playing = playing;
    c.timelineStart = playing ? before / rate : after / rate;
    c.timelineEnd = after / rate;
    c.wrapped = wrapped;
    c.loopActive = loopC_.active();
    c.loopStart = static_cast<double>(loopC_.start) / rate;
    c.loopEnd = static_cast<double>(loopC_.end) / rate;
    c.planLatency = syncPlanLatency_.load(std::memory_order_relaxed);
    c.audioLead = syncAudioLead_.load(std::memory_order_relaxed);
    c.outputLatency = syncOutputLatency_.load(std::memory_order_relaxed);
    c.inputLatency = syncInputLatency_.load(std::memory_order_relaxed);
    const detail::Plan* p = ring_ ? livePlan_ : plan_;
    c.timeMap = p ? p->timeMap.get() : nullptr;
    c.midiOut = midiSource_;
    c.outSent = syncOutSent_.data();
    c.outDropped = syncOutDropped_.data();
    syncPlayed_ = playing;
    if (rt->process(c)) notifyMain();
    syncTransportRunning_.store(rt->transportRunning(), std::memory_order_release);
    publishSyncStatus(rt->status());
}

// The status travels as words; copying it bytewise is sound.
static_assert(std::is_trivially_copyable_v<SyncStatus>);
static_assert(std::atomic<double>::is_always_lock_free);  // deviceRate_, read by the callback

void Engine::publishSyncStatus(const SyncStatus& status) SUPPLE_RT {
    std::array<std::uint64_t, kSyncWords> words{};
    std::memcpy(words.data(), &status, sizeof status);
    const auto seq = syncSeq_.load(std::memory_order_relaxed);
    syncSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < words.size(); ++i)
        syncWords_[i].store(words[i], std::memory_order_relaxed);
    syncSeq_.store(seq + 2, std::memory_order_release);
}

SyncStatus Engine::syncStatus() const {
    SyncStatus status;
    if (!backend_ || !syncAdopted_.load(std::memory_order_acquire)) return status;
    std::array<std::uint64_t, kSyncWords> words{};
    for (;;) {
        const auto seq = syncSeq_.load(std::memory_order_acquire);
        if (seq & 1) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = syncWords_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq == syncSeq_.load(std::memory_order_relaxed)) break;
    }
    if (syncSeq_.load(std::memory_order_acquire) == 0) return status;
    std::memcpy(static_cast<void*>(&status), words.data(), sizeof status);
    return status;
}

Status Engine::startSync() {
    // Always a runtime, "off" included: the callback adopts in publication
    // order, so service() frees those before the adopted one.
    auto rt = detail::SyncRuntime::create(syncConfig_, config_.sampleRate, ++syncRuntimeIds_);
    if (!rt) return std::move(rt).error();
    syncNext_.store(rt->get(), std::memory_order_release);
    syncRuntimes_.push_back(std::move(*rt));
    return {};
}

Status Engine::setSync(const SyncConfig& config) {
    if (config == syncConfig_) return {};
    // The plan changes when listening starts or stops, or the rate path does.
    const bool planChanged =
        config.listens() != syncConfig_.listens() || config.chases() != syncConfig_.chases();
    const bool sameRuntime = config.sameRuntime(syncConfig_);
    syncConfig_ = config;
    if (!config.chases()) syncScale_ = 1.0;
    if (!backend_) return {};
    if (sameRuntime && !syncRuntimes_.empty() &&
        syncRuntimes_.back().get() == syncNext_.load(std::memory_order_acquire))
        syncRuntimes_.back()->adjust(config);  // the receivers keep what they know
    else
        SUPPLE_TRY(startSync());
    if (planChanged && latest_) {
        const auto dir = plans_.empty() ? std::filesystem::path{} : plans_.back()->projectDir;
        const auto session = latest_;
        SUPPLE_TRY(load(*session, dir));
    }
    return {};
}

// Each callback's device control messages, once, for MIDI learn (ADR-0027).
void Engine::pushControlMidi() SUPPLE_RT {
    if (!midiSource_) return;
    bool changed = false;
    for (const MidiInputEvent& e : deviceMidi_) {
        if (e.size == 0 || !isControlMidi(e.bytes[0])) continue;
        changed = true;
        ControlMidiEvent c;
        c.port = e.port;
        c.bytes[0] = e.bytes[0];
        c.bytes[1] = e.size > 1 ? e.bytes[1] : 0;
        c.bytes[2] = e.size > 2 ? e.bytes[2] : 0;
        if (!controlMidi_->push(c)) controlDrops_.fetch_add(1, std::memory_order_relaxed);
    }
    if (changed) notifyMain();
}

void Engine::takeControlMidi(std::vector<ControlMidiEvent>& out) {
    ControlMidiEvent e;
    while (controlMidi_->pop(e)) out.push_back(e);
}

void Engine::cycleBody(const float* const* in, float* const* out, std::uint32_t frames,
                       std::uint32_t midiBase) SUPPLE_RT {
    if (ring_) {  // anticipative: the renderer has done most of the work
        anticipativeCycle(in, out, frames, midiBase);
        wakeRendererConsumed();
        cycles_.fetch_add(1, std::memory_order_release);
        return;
    }
    if (shutdownRequested_.load(std::memory_order_acquire)) {
        // Stopping: this executor starts no plug-in again, neither a refused
        // one's retry nor a newer plan's, which the final callback and stop()
        // would then leave processing with no owner, so that an engine
        // restarted on this pool could never claim it.
        if (!shutDown_.load()) {
            detail::adoptRackModules(plan_ ? &plan_->modules : nullptr, nullptr, &processing_);
            for (host::Instance* inst : processing_) {
                inst->stopProcessing();
                inst->setOwner(nullptr);
            }
            processing_.clear();
            playing_ = false;
            stoppedOnAudioThread_ = true;
            shutDown_.store(true);
        }
        silence(out, 0, frames);
        reportedPlaying_.store(false);
        cycles_.fetch_add(1, std::memory_order_release);
        return;
    }
    TransportSnapshot xport;
    std::uint64_t xportSeq = 0;
    const bool xportNew = xportSeq_.load(std::memory_order_acquire) != xportSeen_ &&
                          readTransport(xport, kAudioTransportTries, &xportSeq);
    const auto handEpoch = xportNew ? xport.epoch : rtEpoch_;
    detail::Plan* plan = plan_;
    if (rateScheduling_.load(std::memory_order_acquire)) {
        for (detail::Plan* candidate = nullptr; planHandoffs_.peek(candidate);) {
            if (candidate->handEpoch < handEpoch) {
                planHandoffs_.consume();
                continue;
            }
            if (candidate->handEpoch > handEpoch) break;
            const auto stream = rtStream_.load(std::memory_order_relaxed);
            const bool immediate = !wantNow() || candidate->rateSeekSequence != 0;
            if (!immediate && candidate->handAt > stream) {
                if (candidate->handAt - stream < frames) {
                    const auto first = static_cast<std::uint32_t>(candidate->handAt - stream);
                    cycleBody(in, out, first, midiBase);
                    std::array<const float*, 64> input{};
                    for (std::uint32_t c = 0; c < deviceInputs_ && c < input.size(); ++c)
                        input[c] = in && in[c] ? in[c] + first : nullptr;
                    const auto rest = advanced(out, first);
                    cycleBody(in ? input.data() : nullptr, rest.data(), frames - first,
                              midiBase + first);
                    return;
                }
                break;
            }
            plan = candidate;
            planHandoffs_.consume();
            // Each trajectory is prepared against the preceding queued plan;
            // never skip a ready plan and its source transition.
            break;
        }
    } else
        plan = latestPlan_.load(std::memory_order_acquire);
    if (plan != plan_) {
        if (plan_ && plan) {
            detail::carryLiveNotes(*plan_, *plan, false);
            detail::carryRateSources(*plan_, *plan, -1);
            detail::carryMix(*plan_, *plan, -1);
            ackReturns(*plan, -1, false, rtEpoch_, rtStream_.load(std::memory_order_relaxed));
        }
        detail::adoptRackModules(plan_ ? &plan_->modules : nullptr, plan ? &plan->modules : nullptr,
                                 &processing_);
        adoptShare(processing_, plan ? &plan->instances : nullptr, &processing_);
        plan_ = plan;
        if (plan) {
            if (plan->rateEnabled) pos_ = plan->rateHandDevice;
            detail::locate(*plan, pos_, false);
            stoppedGen_.store(plan->generation, std::memory_order_release);
        }
    }
    if (xportNew && xport.seekSeq != seekSeen_) {
        seekSeen_ = xport.seekSeq;
        clickAge_ = -1;
        pos_ = xport.seekSample;
        if (plan) {
            detail::locate(*plan, pos_);
            resetOwned(processing_, plan);
        }
    }
    if (const bool want = wantNow(); want != playing_) {
        playing_ = want;
        if (!want) resetOwned(processing_, plan);  // no hanging notes
        // Starting: what ran while stopped (plugins, the Console 9 noise)
        // starts over, as in the renderer; monitored strips play on.
        if (want && plan) {
            detail::locateClass(*plan, pos_, false);
            for (detail::Node& node : plan->nodes)
                if (!node.live)
                    detail::forEachStage(node, [](detail::Stage& st) {
                        if (st.instance && st.instance->processing()) st.instance->reset();
                        st.voices.clear();
                        st.pendingVoices.clear();
                        st.sleeping = false;
                    });
        }
    }
    if (xportNew) {
        xportSeen_ = xportSeq;
        loopC_ = xport.loop;
    }
    if (xportNew && xport.epoch != rtEpoch_) {
        // Where the epoch it leaves stopped being heard (ADR-0097, dated
        // 2026-11-26): everything it processed there was handed to the device.
        const std::int64_t heard =
            rtPosEpoch_.load(std::memory_order_relaxed) == rtEpoch_ ? rtStream_.load() : 0;
        // Both classes (it runs every node): the slots' verdicts, written
        // before the end is published (a renderer settling by it reads
        // them), then what it applied there and was not heard goes back,
        // here at the leave (round 203 F1).
        leaveLaunchEpoch(3, plan, rtEpoch_, heard, true, xport.epoch);
        noteHeardEnd(rtEpoch_, xport.epoch, heard);
        launchLeftEpoch_[1] = rtEpoch_;
        launchLeftEnd_[1] = heard;
        if (plan) takeBackNodes(*plan, -1, rtEpoch_, heard);
        markLeft(3, rtEpoch_, xport.epoch);
        // The callback's own meter coordinates (P5.3c). With no anticipation it
        // is both renderer and player, so what it processes is what is heard;
        // the anticipative path keeps these in pullAhead/liveRender.
        rtEpoch_ = xport.epoch;
        rateEpochStart_ = double(pos_);
        rtStream_.store(0, std::memory_order_relaxed);
        rtPosEpoch_.store(rtEpoch_, std::memory_order_release);
    }
    if (plan) {
        startFree(processing_, &plan->instances, &processing_);  // any refused, retried
        // For monitored inputs (audio and MIDI).
        plan->deviceIn = in;
        plan->deviceInChannels = deviceInputs_;
        plan->deviceRecording = recorder_.load() != nullptr;
        plan->deviceMidi = deviceMidi_;
        plan->deviceMidiOffset = midiBase;
    }
    if (plan && !playing_ && !shutDown_.load() && frames <= plan->block) {
        // Stopped: plugins keep running (monitored input is heard, editors
        // answer, a sampler loads what its editor picked: it does so only in
        // process()), nothing plays.
        const auto patchSequence = patchSeq_.load(std::memory_order_acquire);
        if (detail::processCallbackBlock(liveWorkersReady_.load(std::memory_order_acquire), *plan,
                                         pos_, frames, false, false, &processing_)) {
            hardwareMidi_->collect(*plan, pos_, frames, midiBase, false, rtEpoch_);
            publishPluginOutputs(*plan, false, rtEpoch_, pos_, rtStream_.load(), patchSequence);
            writeDevice(*plan, out, 0, frames);
            // Metering (P5.3c): monitored input is heard as it is produced —
            // there is no play position ahead of it to wait for.
            if (meters_)
                pushMeters(*meters_, *plan, plan->meterNodes, rtEpoch_,
                           rtStream_.load(std::memory_order_relaxed), frames, false);
        } else {
            processErrors_.fetch_add(1);
            silence(out, 0, frames);
        }
        reportedPos_.store(pos_);
        reportedPlaying_.store(false);
        cycles_.fetch_add(1, std::memory_order_release);
        return;
    }
    const bool run = plan && playing_ && !shutDown_.load() && frames <= plan->block;
    bool ok = run;
    std::array<const float*, 64> inAt{};
    // The callback's meter stream (P5.3c): the frames it has played in
    // rtEpoch_. It is the only writer here, so a plain local then one store.
    std::int64_t meterStream = rtStream_.load(std::memory_order_relaxed);
    for (std::uint32_t done = 0; ok && done < frames;) {
        std::uint32_t k =
            detail::rateUntil(*plan, pos_, frames - done, loopC_, -1);  // split at a loop wrap
        k = launchesAt(*plan, -1, rtEpoch_, meterStream, k, false);     // and at a launch
        for (std::uint32_t c = 0; c < deviceInputs_ && c < inAt.size(); ++c)
            inAt[c] = in && in[c] ? in[c] + done : nullptr;
        const float* const* ins = in ? inAt.data() : nullptr;
        plan->deviceIn = ins;  // monitored input for this part of the cycle
        plan->deviceMidiOffset = midiBase + done;
        const auto patchSequence = patchSeq_.load(std::memory_order_acquire);
        if (!detail::processCallbackBlock(liveWorkersReady_.load(std::memory_order_acquire), *plan,
                                          pos_, k, true, false, &processing_, &loopC_)) {
            ok = false;
            break;
        }
        hardwareMidi_->collect(*plan, pos_, k, midiBase + done, true, rtEpoch_);
        publishPluginOutputs(*plan, false, rtEpoch_, pos_, meterStream, patchSequence);
        writeDevice(*plan, out, done, k);
        addClicks(*plan, out, done, pos_, k);
        if (detail::Recorder* r = recorder_.load()) {
            const auto timeline = static_cast<std::int64_t>(std::llround(plan->rateSample(pos_)));
            r->capture(ins, deviceInputs_, k, timeline, plan->processedInputs);
            r->captureMidi(plan->deviceMidi, midiBase + done, k, timeline);
            captureRoutedMidi(*r, *plan, k);
        }
        // Metering (P5.3c): no anticipation here, so the callback renders and
        // plays in step; its own stream is the position a reader aligns to.
        if (meters_) pushMeters(*meters_, *plan, plan->meterNodes, rtEpoch_, meterStream, k, false);
        meterStream += k;
        const auto next = plan->rateEnabled ? pos_ + k : loopC_.next(pos_, pos_ + k);
        if (detail::rateWrapped(*plan, pos_, pos_ + k, loopC_, false))
            detail::wrapTo(*plan, next, -1);
        pos_ = next;
        done += k;
    }
    if (!ok) {
        if (run) processErrors_.fetch_add(1);
        silence(out, 0, frames);
    }
    if (ok && run) {
        rtPos_.store(pos_, std::memory_order_relaxed);
        rtStream_.store(meterStream);
        rtPosEpoch_.store(rtEpoch_, std::memory_order_release);
    }
    if (plan && ok && run) {
        rtTimeline_.store(plan->rateSample(pos_), std::memory_order_relaxed);
        effectivePlayrate_.store(plan->rateEnabled
                                     ? plan->timelines[0]->rate(pos_ - 1 - plan->outputLatency -
                                                                plan->rateBackendLatency)
                                     : 1.,
                                 std::memory_order_relaxed);
        timelineEpoch_.store(rtEpoch_, std::memory_order_release);
        publishRateClock(*plan, pos_, meterStream, rtEpoch_, false);
    }
    reportedPos_.store(pos_);
    reportedPlaying_.store(playing_);
    cycles_.fetch_add(1, std::memory_order_release);
}

// --- anticipation (ADR-0012) -----------------------------------------------------------------

// --- metronome --------------------------------------------------------------------------------

void Engine::setMetronome(bool on, double gainDb) {
    metroGain_.store(static_cast<float>(std::pow(10.0, gainDb / 20.0)), std::memory_order_relaxed);
    metroOn_.store(on, std::memory_order_release);
}

void Engine::playCountIn(std::int64_t frames, std::int64_t beatFrames, int beatsPerBar) {
    if (!seekToPlayStart()) return;  // as play(), with its re-place (review round 81 E3)
    countFrames_.store(std::max<std::int64_t>(0, frames), std::memory_order_relaxed);
    countBeat_.store(std::max<std::int64_t>(1, beatFrames), std::memory_order_relaxed);
    countBar_.store(std::max(1, beatsPerBar), std::memory_order_relaxed);
    countSeq_.fetch_add(1, std::memory_order_release);
    placeCapture();  // a capture armed while stopped begins at the play position
    wantPlaying_.store(true, std::memory_order_release);
    wakeRenderer();
    notifyMain();
}

void Engine::startClick(bool accent, double rate) SUPPLE_RT {  // a 40 ms decaying sine
    clickAge_ = 0;
    clickLength_ = static_cast<std::int64_t>(0.04 * rate);
    clickPhase_ = 0;
    clickStep_ = 2.0 * std::numbers::pi * (accent ? 1600.0 : 1100.0) / rate;
    clickAmp_ = accent ? 1.0f : 0.6f;
    clickEnv_ = 1.0f;
    clickDecay_ = static_cast<float>(std::exp(-1.0 / (0.008 * rate)));
}

float Engine::clickSample() SUPPLE_RT {
    if (clickAge_ < 0) return 0.0f;
    const float v = clickAmp_ * clickEnv_ * static_cast<float>(std::sin(clickPhase_));
    clickPhase_ += clickStep_;
    clickEnv_ *= clickDecay_;
    if (++clickAge_ >= clickLength_) clickAge_ = -1;
    return v;
}

void Engine::silence(float* const* out, std::uint32_t offset, std::uint32_t k) const SUPPLE_RT {
    for (std::uint32_t c = 0; c < deviceOutputs_; ++c) std::fill_n(out[c] + offset, k, 0.0f);
}

void Engine::writeDevice(const detail::Plan& p, float* const* out, std::uint32_t offset,
                         std::uint32_t k) const SUPPLE_RT {
    detail::writeOutputs(p, out, deviceOutputs_, offset, k);
}

std::array<float*, 64> Engine::advanced(float* const* out, std::uint32_t by) const SUPPLE_RT {
    std::array<float*, 64> at{};
    for (std::uint32_t c = 0; c < deviceOutputs_; ++c) at[c] = out[c] + by;
    return at;
}

void Engine::countClicks(float* const* out, std::uint32_t k) SUPPLE_RT {
    const float gain = metroGain_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < k; ++i, ++countPos_) {
        if (countPos_ % countBeatA_ == 0)
            startClick((countPos_ / countBeatA_) % countBarA_ == 0,
                       deviceRate_.load(std::memory_order_relaxed));
        const float v = gain * clickSample();
        out[0][i] += v;
        out[1][i] += v;
    }
}

void Engine::addClicks(const detail::Plan& p, float* const* out, std::uint32_t offset,
                       std::int64_t pos, std::uint32_t k) SUPPLE_RT {
    const bool on = metroOn_.load(std::memory_order_acquire);
    if ((!on && clickAge_ < 0) || k == 0 || !p.timeMap) return;
    const time::TimeMap& tm = *p.timeMap;
    if (p.rateEnabled) {
        const auto& clock = *p.timelines[options_.anticipative ? 1 : 0];
        const float gain = metroGain_.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < k; ++i) {
            const auto device = pos + i - p.outputLatency;
            const double here = clock.position(device);
            double before = clock.position(device - 1);
            // Wrapped: the window starts at the loop start and includes it, so
            // a beat there clicks after every wrap (a bound just below it fell
            // within the tolerance below and skipped that click).
            const bool wrapped =
                clock.cycle(clock.phase(device)) != clock.cycle(clock.phase(device - 1));
            if (wrapped) before = clock.loopStart();
            const double beat = tm.beatAt(p.startSeconds + here / p.rate);
            const double previousBeat = tm.beatAt(p.startSeconds + before / p.rate);
            const auto bb = tm.barBeat(beat);
            const auto meter = tm.meterAt(bb.bar);
            const double unit = 4. / meter.denominator;
            const double bar = tm.beatOfBar(bb.bar);
            const double index = std::floor((beat - bar) / unit + 1e-9);
            const double tick = bar + index * unit;
            const bool sinceBefore =
                wrapped ? tick >= previousBeat - 1e-9 : tick > previousBeat + 1e-9;
            if (on && sinceBefore && tick <= beat + 1e-9) startClick(index == 0, p.rate);
            const float value = gain * clickSample();
            out[0][offset + i] += value;
            out[1][offset + i] += value;
        }
        return;
    }
    const double s0 = p.startSeconds + static_cast<double>(pos) / p.rate;
    const double s1 = p.startSeconds + static_cast<double>(pos + k) / p.rate;
    const double b0 = tm.beatAt(s0), b1 = tm.beatAt(s1);
    // The meter's beats in [b0, b1): frame offsets, accented on a bar's first.
    std::array<std::uint32_t, 16> starts{};
    std::array<bool, 16> accents{};
    std::size_t n = 0;
    for (double cursor = b0; on && n < starts.size();) {
        const time::BarBeat bb = tm.barBeat(cursor);
        const time::MeterChange m = tm.meterAt(bb.bar);
        const double barStart = tm.beatOfBar(bb.bar);
        const double unit = 4.0 / static_cast<double>(m.denominator);
        const double index = std::ceil((cursor - barStart) / unit - 1e-9);
        const bool nextBar = index >= m.numerator;
        const double beat = nextBar ? tm.beatOfBar(bb.bar + 1) : barStart + index * unit;
        if (beat >= b1) break;
        const double at = (tm.secondsAt(beat) - s0) * p.rate;
        starts[n] = static_cast<std::uint32_t>(
            std::clamp(std::llround(at), 0LL, static_cast<long long>(k) - 1));
        accents[n] = nextBar || index == 0;
        ++n;
        cursor = beat + 1e-7;  // past this beat: the next ceil finds the next one
    }
    const float gain = metroGain_.load(std::memory_order_relaxed);
    std::size_t next = 0;
    for (std::uint32_t i = 0; i < k; ++i) {
        for (; next < n && starts[next] == i; ++next) startClick(accents[next], p.rate);
        if (clickAge_ < 0) continue;
        const float v = gain * clickSample();
        out[0][offset + i] += v;
        out[1][offset + i] += v;
    }
}

Engine::Pulled Engine::pullAhead(float* const* out, std::uint32_t frames) SUPPLE_RT {
    std::int64_t first = kNothingPlayed, firstStream = 0;
    const std::uint64_t e = rtEpoch_;  // anticipativeCycle took it with its loop
    if (options_.benchProbe && rtPrimed_ && wantNow() &&
        epochDone_.load(std::memory_order_acquire) == e)
        options_.benchProbe->observeAhead(renderedStream_.load(std::memory_order_acquire) -
                                          playedStream(e));
    detail::ChunkRing& ring = livePlan_ && livePlan_->rateOutput ? *livePlan_->rateOutput : *ring_;
    while (const detail::Chunk* c = ring.front()) {  // flushed by a seek or pause
        if (c->epoch >= e) break;
        ring.pop();
        rtOffset_ = 0;
    }
    if (livePlan_)  // a recorded master's ring flushes with it (ADR-0057)
        for (const auto& record : livePlan_->recordRings) record->dropBefore(e);
    if (!rtPrimed_ && !primeReady(e, frames)) {  // pre-roll: time holds still
        silence(out, 0, frames);
        return {kNothingPlayed, 0, 0, false};
    }
    std::uint32_t done = 0;
    while (done < frames) {
        const detail::Chunk* c = ring.front();
        if (!c || c->epoch != e) break;
        bool wrapped = c->start + rtOffset_ != first + done;
        if (done > 0 && livePlan_ && livePlan_->rateEnabled) {
            const auto& clock = *livePlan_->timelines[1];
            wrapped |=
                clock.cycle(clock.phase(c->start + rtOffset_)) != clock.cycle(clock.phase(first));
        }
        if (done > 0 && wrapped) {
            // Primed: the cycle's rest after the wrap is not pre-roll again
            // (review round 96, S1).
            rtPrimed_ = true;
            return {first, done, firstStream, true};
        }
        const std::uint32_t take = std::min(frames - done, c->frames - rtOffset_);
        if (first == kNothingPlayed) {
            first = c->start + rtOffset_;
            firstStream = c->stream + rtOffset_;
        }
        for (std::uint32_t ch = 0; ch < deviceOutputs_; ++ch) {
            if (ch < c->width())
                std::copy_n(c->channel(ch) + rtOffset_, take, out[ch] + done);
            else
                std::fill_n(out[ch] + done, take, 0.0f);
        }
        done += take;
        rtOffset_ += take;
        const auto pos = c->start + rtOffset_;
        const auto stream = c->stream + rtOffset_;
        const double timeline = c->timeline.empty() ? double(pos) : c->timeline[rtOffset_];
        double rate = c->rates.empty() ? 1. : c->rates[rtOffset_ - 1];
        if (c->stream == 0) rateEpochStart_ = double(c->start);
        if (livePlan_ && livePlan_->rateEnabled) {
            detail::prepareRateWindow(*livePlan_, pos, 1, 1, &loopC_);
            rate = livePlan_->timelines[1]->rate(pos - 1 - livePlan_->outputLatency -
                                                 livePlan_->rateBackendLatency);
        }
        if (livePlan_) publishRateClock(*livePlan_, pos, stream, e, true);
        if (rtOffset_ == c->frames) {
            ring.pop();
            rtOffset_ = 0;
        }
        // Publish freed slots before consumption, for the renderer arm/recheck
        // handshake. The chunk may be reused as soon as pop returns.
        rtPos_.store(pos, std::memory_order_relaxed);
        rtTimeline_.store(timeline, std::memory_order_relaxed);
        effectivePlayrate_.store(rate, std::memory_order_relaxed);
        timelineEpoch_.store(e, std::memory_order_release);
        rtStream_.store(stream);
        rtPosEpoch_.store(e);
    }
    // Primed first: a cycle that played audio and then ran short is a gap,
    // also the cycle that started the epoch.
    if (done > 0) rtPrimed_ = true;
    if (done < frames) {
        silence(out, done, frames - done);
        if (rtPrimed_ && wantPlaying_.load(std::memory_order_relaxed))
            misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return {first, done, firstStream, false};
}

bool Engine::primeReady(std::uint64_t epoch, std::uint32_t frames) const SUPPLE_RT {
    if (epochDone_.load(std::memory_order_acquire) != epoch) return false;
    const std::int64_t need =
        std::min<std::int64_t>(std::max<std::int64_t>(options_.renderBlock, frames),
                               primeAhead_.load(std::memory_order_acquire));
    if (renderedStream_.load(std::memory_order_acquire) - playedStream(epoch) >= need) return true;
    // Or a ring the renderer writes is full: a loop shorter than the rings'
    // chunks fills them before a whole block is rendered (engine.md, rings),
    // and what they hold is all there is until this callback plays it. The
    // rings' state, not the renderer's wake flag, which each of its passes
    // clears before it looks again: read then, it held a start one more
    // cycle. This callback is their only consumer, so full stays full.
    const auto full = [](const detail::ChunkRing& ring) {
        return ring.write.load(std::memory_order_acquire) -
                   ring.read.load(std::memory_order_relaxed) >
               ring.mask;
    };
    const detail::Plan* p = livePlan_;
    if (p)
        for (const auto& ring : p->recordRings)
            if (full(*ring)) return true;
    if (p && p->split) {
        for (const auto& ring : p->frontierRings)
            if (full(*ring)) return true;
        return false;
    }
    return full(p && p->rateOutput ? *p->rateOutput : *ring_);
}

// --- recording (ADR-0014) --------------------------------------------------------------------

Status Engine::setRecordingPunch(RecordedPunch punch) {
    SUPPLE_TRY(detail::validateRecordingPunch(punch));
    return recorderOwned_ ? recorderOwned_->setPunch(punch) : okStatus();
}

Status Engine::startRecording(std::vector<RecordTarget> targets, RecordedPunch punch) {
    if (!backend_)
        return Error("engine.stopped", "start the engine first", {},
                     "call engine.start with a valid backend before loading or recording");
    if (recorderOwned_)
        return Error("record.running", "already recording", {},
                     "stop the current recording with transport.stop before starting another");
    if (latest_) SUPPLE_TRY(model::validateRecordingPlayrate(*latest_));
    SUPPLE_TRY(detail::validateRecordingPunch(punch));
    // While slaved the chase rate stays close to 1 and is not part of the
    // project: recording goes ahead (ADR-0071). Only the saved rate counts.
    const auto* slaved = latestPlan_.load(std::memory_order_acquire);
    if (rateScheduling_.load(std::memory_order_acquire) && !(slaved && slaved->syncRate)) {
        if (!wantPlaying_.load(std::memory_order_acquire)) {
            SUPPLE_TRY(prepareSeek(transport().seconds));
            // Recording starts from where the renderer was placed: never from
            // a position it has not taken.
            if (backend_->ownsThread()) SUPPLE_TRY(awaitTransport());
        } else {
            const auto heard = readRateClock();
            const auto state =
                std::bit_cast<detail::PlayrateClock::State>(std::array<std::uint64_t, 6>{
                    heard[0], heard[1], heard[2], heard[3], heard[4], heard[5]});
            bool unsafe = state.scalar != 1 || state.target != 1 || effectivePlayrate() != 1;
            const auto heardStream = std::bit_cast<std::int64_t>(heard[7]);
            for (const auto& pending : plans_) {
                if (pending->generation < heard[9]) continue;
                unsafe |= pending->masterPlayrate.rate != 1 ||
                          std::any_of(pending->rateEnvelope.points.begin(),
                                      pending->rateEnvelope.points.end(),
                                      [](const auto& point) { return point.value != 1; });
                if (pending->rateEnabled && pending->rateSafeDevice != INT64_MIN) {
                    const auto drained = pending->preparedRateBoundary.stream +
                                         pending->rateSafeDevice - pending->rateHandDevice +
                                         pending->outputLatency + pending->rateBackendLatency;
                    if (heardStream < drained)
                        unsafe |= pending->rateControl[0].clock.from != 1 ||
                                  pending->timelines[0]->hasNonUnityCurveHistory();
                }
            }
            if (unsafe)
                return Error(
                    "record.playrate", "a non-unity source trajectory is still audible or queued",
                    "/project/playrate/rate", "stop playback and restore unity before recording");
        }
    }
    if (targets.empty())
        return Error("record.nothing_armed", "no track is armed with an input", {},
                     "use track.list for a channel id, then track.set {track, armed: true, "
                     "record_input: [1, 2]}, or arm a master to record its output");
    // Rendered inputs use their musical clock; device inputs use round trip.
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    for (RecordTarget& target : targets) {
        if (!target.rackInput) continue;
        const auto node = plan ? std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                              [&](const auto& n) { return n.id == target.track; })
                               : std::vector<detail::Node>::const_iterator{};
        if (!plan || node == plan->nodes.end() || !node->recordRack)
            return Error("record.rack_input",
                         "the loaded plan has no armed rack input for " + target.track.str(),
                         "/track",
                         "assign a rack Out with track.set {record_rack, armed: true} and load it "
                         "before recording");
        target.outputShift = node->timelineShift + node->inputLatency;
    }
    for (RecordTarget& target : targets) {
        if (!target.midi || !target.midi->routed) continue;
        const auto node = plan ? std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                              [&](const auto& n) { return n.id == target.track; })
                               : std::vector<detail::Node>::const_iterator{};
        if (!plan || node == plan->nodes.end() || !node->live)
            return Error("record.midi_route",
                         "the Note Out destination is not armed in the loaded plan", "/track",
                         "arm the destination in a MIDI record mode, load it, and record again");
        target.midi->shift = node->timelineShift;
    }
    for (RecordTarget& t : targets) {
        if (!t.output) continue;
        const auto node = plan
                              ? std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                             [&](const detail::Node& n) { return n.id == t.track; })
                              : std::vector<detail::Node>::const_iterator{};
        if (!plan || node == plan->nodes.end() || !node->master)
            return Error("record.output", t.track.str() + " is not a master of the loaded plan",
                         "/track", "arm a master (track.list, type master) and record again");
        t.outputShift = node->itemShift;
    }
    const BackendStatus b = backend_->status();
    const std::int64_t latency =
        static_cast<std::int64_t>(outputLatency()) + b.outputLatency + b.inputLatency;
    auto rec =
        std::make_unique<detail::Recorder>(std::move(targets), config_.sampleRate, latency, punch);
    rec->setMainThreadWake(mainWake_);
    rec->setDriftTolerant(slaved && slaved->syncRate);
    rec->setLoop(readLoop());  // for the MIDI journals (ADR-0080)
    SUPPLE_TRY(rec->open());
    recorderOwned_ = std::move(rec);
    recorder_.store(recorderOwned_.get(), std::memory_order_release);
    return okStatus();
}

Result<std::vector<RecordedTake>> Engine::stopRecording() {
    if (!recorderOwned_) {
        if (!stoppedTakes_.empty()) return std::exchange(stoppedTakes_, {});
        return Error(
            "record.stopped", "not recording", {},
            "start recording with transport.record after arming a track and assigning its input");
    }
    recorder_.store(nullptr);
    // A callback that entered before the store may still hold the recorder;
    // one that enters after it sees null. (seq_cst on both sides.)
    if (const std::uint64_t seq = cycleSeq_.load(); seq & 1) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cycleSeq_.load(std::memory_order_acquire) == seq &&
               std::chrono::steady_clock::now() < deadline)
            supple::sleepFor(std::chrono::microseconds(200));
        if (cycleSeq_.load(std::memory_order_acquire) == seq)  // never free what it may use
            return Error("record.busy",
                         "the audio callback has not returned for 2 s; the recording is kept", {},
                         "stop again, or stop the engine (the files stay recoverable)");
    }
    auto takes = recorderOwned_->finish(readLoop());
    recorderOwned_.reset();
    for (auto& take : takes) stoppedTakes_.push_back(std::move(take));
    return std::exchange(stoppedTakes_, {});
}

std::vector<RecordingError> Engine::takeRecordingErrors() {
    return recorderOwned_ ? recorderOwned_->takeErrors() : std::vector<RecordingError>{};
}

std::uint64_t Engine::relocate(std::uint64_t previous) {
    // The newest epoch with its own seek and loop (never a loop or seek of a
    // later epoch paired with this one). Bounded like the callback's read: the
    // renderer is its plugins' audio thread, so it never waits for the main
    // thread to finish a write. A write in progress leaves this pass without
    // relocating (the epoch still differs from `seen`, so the loop tries again
    // after publication wakes it).
    TransportSnapshot t;
    if (!readTransport(t, kRendererTransportTries)) return previous;
    const std::uint64_t epoch = t.epoch;
    // What it applied ahead of the listener in `previous` (ADR-0097, dated
    // 2026-11-26): the frames heard there are final once the callback has
    // entered the new epoch, so it waits for that here on a seek as well
    // (bounded, as for a pause below); a manual backend's callback runs only
    // when its caller says, so its count is final already.
    const bool undos =
        plan_ && std::any_of(plan_->nodes.begin(), plan_->nodes.end(),
                             [](const detail::Node& n) { return n.launchUndoCount > 0; });
    if (t.seekSeq != seekSeen_) {
        seekSeen_ = t.seekSeq;
        pos_ = t.seekSample;
        if (undos && backend_->ownsThread() &&
            !rendererLeaveLateForTests_.load(std::memory_order_relaxed)) {
            const auto deadline = std::chrono::steady_clock::now() + rendererSafetyTimeout();
            for (;;) {
                const auto event = rendererEvent_.load(std::memory_order_acquire);
                if (rtEpochSeen_.load(std::memory_order_acquire) >= epoch || rendererQuit_.load())
                    break;
                if (!rt::waitUntil(rendererEvent_, event, deadline)) break;
            }
        }
    } else {
        // Paused: resume where the listener is. Once the callback has seen
        // the new epoch it plays nothing older, so its position is final. (A
        // manual backend's callback runs only when its caller says.)
        //
        // This takes rtPos_ only while rtPosEpoch_ is still `previous`, which
        // holds because rtPosEpoch_ is written where the callback *plays*
        // frames (pullAhead, liveRender) and the callback cannot hold a chunk
        // of the new epoch before this relocation produced one: there is one
        // chunk producer and the epoch flush in pullAhead. If that ever
        // changes, the fallback (epochStartLocal_, the renderer's epoch start)
        // would jump playback back by up to the render-ahead span.
        if (backend_->ownsThread()) {
            const auto safety = wantPlaying_.load(std::memory_order_acquire)
                                    ? rendererSafetyTimeout()
                                    : std::chrono::seconds(1);
            const auto deadline = std::chrono::steady_clock::now() + safety;
            for (;;) {
                const auto event = rendererEvent_.load(std::memory_order_acquire);
                if (rtEpochSeen_.load(std::memory_order_acquire) >= epoch || rendererQuit_.load())
                    break;
                if (!rt::waitUntil(rendererEvent_, event, deadline)) break;
            }
        }
        pos_ = playedPosition(previous, epochStartLocal_);
    }
    // Where hearing ended in the epoch left: the callback's end once it has
    // left it; with a manual backend, whose callback runs only when told,
    // what it consumed is final already. Unknown (the wait timed out, or the
    // test hook): settled once the callback's end is known, nothing applied
    // or taken back until then (settleLeave; round 201 F2).
    std::optional<std::int64_t> leftEnd;
    if (rendererLeaveLateForTests_.load(std::memory_order_relaxed))
        leftEnd = std::nullopt;
    else if (!backend_->ownsThread())
        leftEnd = playedStream(previous);
    else
        leftEnd = heardEnd(previous);
    if (launchLeavePending_ != UINT64_MAX) {
        // Still waiting for an earlier leave's verdicts: it applied nothing
        // since, so its records are that epoch's (settled first). It left
        // this one too, having applied nothing there (round 205 F3); the
        // late span moves on to the epoch it goes on in, once it renders
        // there before the leave settles (launchesAt; round 209).
        markLeft(1, previous, epoch);
        launchLeaveUndos_ = true;  // marked as before, whatever it left behind
    } else if (leftEnd) {
        launchLeftEpoch_[0] = previous;
        launchLeftEnd_[0] = *leftEnd;
        leaveLaunchEpoch(1, plan_, previous, *leftEnd);
        if (plan_) takeBackNodes(*plan_, 0, previous, *leftEnd);
        markLeft(1, previous, epoch);
    } else {
        launchLeavePending_ = previous;
        // What it renders before the leave settles may still hold a switch
        // that was not heard: a capture over those frames is not as heard
        // (marked as it renders them, launchesAt: a leave that settles first,
        // as at a play start the renderer holds for, renders none; round 209).
        launchLeaveUndos_ = undos;
    }
    if (plan_) {
        detail::locateClass(*plan_, pos_, false);
        resetOwned(processing_, plan_);
    }
    epochStartLocal_ = pos_;
    epochStart_.store(pos_);
    streamLocal_ = 0;
    renderedStream_.store(0, std::memory_order_release);
    loopR_ = t.loop;
    epochDone_.store(epoch, std::memory_order_release);
    return epoch;
}

void Engine::wakeRenderer() SUPPLE_RT {
    if (!options_.anticipative) return;
    rendererEvent_.fetch_add(1, std::memory_order_release);
    rt::wake(rendererEvent_);
}

void Engine::wakeRendererConsumed() SUPPLE_RT {
    // Publish even unchanged consumption: this callback may have freed only
    // obsolete chunks while stopped or prerolling. Those slots must participate
    // in the same seq-cst store/load handshake as played frames.
    rtStream_.store(rtStream_.load(std::memory_order_relaxed));
    const auto target = rendererWakeAt_.load();
    if (target == INT64_MAX || playedStream(rtEpoch_) < target) return;
    auto expected = target;
    if (rendererWakeAt_.compare_exchange_strong(expected, INT64_MAX)) wakeRenderer();
}

std::chrono::steady_clock::time_point Engine::rendererDeadline(
    std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point idleNext,
    bool idleWork, std::chrono::steady_clock::duration safetyTimeout) {
    const auto safety = now + safetyTimeout;
    return idleWork ? std::min(idleNext, safety) : safety;
}

std::chrono::steady_clock::time_point Engine::stoppedRenderDeadline(
    std::chrono::steady_clock::time_point next, std::chrono::steady_clock::time_point last,
    bool requested) {
    return requested ? std::min(next, last + std::chrono::milliseconds(1)) : next;
}

std::chrono::steady_clock::duration Engine::rendererSafetyTimeout() const {
    if (playing_ && plan_ && !hold_.load(std::memory_order_acquire) &&
        (!plan_->split || plan_->feedsCallback()))
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(0.5 * static_cast<double>(aheadFrames_) /
                                          config_.sampleRate));
    return std::chrono::seconds(1);
}

std::int64_t Engine::rendererSpaceTarget(std::uint64_t epoch) const {
    if (!playing_ || !plan_ || hold_.load(std::memory_order_acquire) ||
        streamLocal_ >= holdFrom_.load(std::memory_order_acquire) ||
        (plan_->split && !plan_->feedsCallback()))
        return INT64_MAX;
    // The horizon allows another whole render block once ahead < aheadFrames_.
    auto target = streamLocal_ - aheadFrames_ + 1;
    const auto requireSlot = [&](const detail::ChunkRing& ring) {
        const auto w = ring.write.load(std::memory_order_relaxed);
        const auto r = ring.read.load(std::memory_order_acquire);
        if (w - r <= ring.mask) return;
        const auto& front = ring.chunks[r & ring.mask];
        // Old epochs are dropped by the next callback, even without playback.
        target = std::max(target, front.epoch == epoch ? front.stream + front.frames : 0);
    };
    if (plan_->split) {
        for (const auto& ring : plan_->frontierRings) requireSlot(*ring);
    } else {
        requireSlot(plan_->rateOutput ? *plan_->rateOutput : *ring_);
    }
    for (const auto& ring : plan_->recordRings) requireSlot(*ring);  // ADR-0057
    return target;
}

bool Engine::rendererHasRoom(std::uint64_t epoch) const {
    // Read consumption first, including after arming a callback wake.
    const auto played = playedStream(epoch);
    if (!playing_ || !plan_ || hold_.load(std::memory_order_acquire) ||
        streamLocal_ >= holdFrom_.load(std::memory_order_acquire) ||
        streamLocal_ - played >= aheadFrames_)
        return false;
    for (const auto& ring : plan_->recordRings)  // recorded masters (ADR-0057)
        if (!ring->slot()) return false;
    if (plan_->split) {
        if (!plan_->feedsCallback()) return false;
        for (const auto& ring : plan_->frontierRings)
            if (!ring->slot()) return false;
        return true;
    }
    return (plan_->rateOutput ? plan_->rateOutput->slot() : ring_->slot()) != nullptr;
}

// Both halves of adoption follow the callback's RT rules. Transport relocation
// between them may wait for the callback's epoch acknowledgement; source/graph
// preparation never belongs to either half.
void Engine::adoptRendererSources(detail::Plan* next) SUPPLE_RT {
    // Continuable only while playing, from a plan of the same kind: two unity
    // plans, or two rate plans adopted at their prepared boundary without a
    // seek (rateAdoption is Ready only at stream == handAt, where
    // rateHandDevice is this renderer's own device position, so pos_ below
    // stays put). Between unity and rate the device coordinate changes.
    const bool sameKind = plan_ && next && plan_->rateEnabled == next->rateEnabled;
    const bool continuable =
        sameKind && playing_ &&
        (!next->rateEnabled || (next->rateSeekSequence == 0 && streamLocal_ == next->handAt &&
                                pos_ == next->rateHandDevice));
    if (plan_ && next) {
        detail::carryRateSources(*plan_, *next, 0);
        detail::carryMix(*plan_, *next, 0);
        // A node moving into the live region: the callback may continue it
        // from the handoff, where this renderer stopped, with its voices,
        // delay lines, console state and plug-in state (not reset). Its share
        // of `next` is untouched until the handoff is published below.
        if (continuable) detail::carryMix(*plan_, *next, 1, 0);
    }
    if (next) next->handContinuable = next->liveCarried = continuable;
    detail::adoptRackModules(plan_ ? &plan_->rendererModules : nullptr,
                             next ? &next->rendererModules : nullptr, &processing_);
    adoptShare(processing_, next ? &next->rendererInstances : nullptr, &processing_,
               continuable ? &next->liveInstances : nullptr);
    plan_ = next;
    if (next && next->rateEnabled) pos_ = next->rateHandDevice;
}

void Engine::publishRendererPlan(detail::Plan& next, std::uint64_t epoch,
                                 bool queuedRate) SUPPLE_RT {
    detail::locateClass(next, pos_, false, false);
    if (next.rateEnabled) {
        if (!playing_) streamLocal_ = next.handAt;
    } else {
        next.handAt = streamLocal_;  // unity handoff, renderer -> callback only
        next.handEpoch = epoch;
    }
    if (queuedRate) ratePlanRequests_.consume();
    // Its returns, before the callback can see the plan (the live nodes
    // carried from this renderer's included).
    ackReturns(next, -1, true, epoch, streamLocal_);
    // Queued for the callback before the adoption is reported, so a caller
    // of awaitPlan() knows the callback's next cycle takes it.
    planHandoffs_.push(&next);  // space checked before adoption
    stoppedGen_.store(next.generation, std::memory_order_release);
    handPlan_.store(&next, std::memory_order_release);
    notifyMain();
}

void Engine::rendererLoop(std::uint64_t seen) {
    const std::uint32_t block = options_.renderBlock;
    const auto blockTime = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(static_cast<double>(block) / config_.sampleRate));
    auto idleNext = std::chrono::steady_clock::now();  // the next stopped block
    auto idleLast = idleNext - std::chrono::milliseconds(1);
    const void* self = &processing_;
    for (;;) {
        // Observe the notification before every predicate. A publication during
        // this pass changes the word, so FUTEX_WAIT cannot miss its wake.
        const auto event = rendererEvent_.load(std::memory_order_acquire);
        if (rendererQuit_.load(std::memory_order_acquire)) break;
        rendererWakeAt_.store(INT64_MAX);
        // Held only while in the hold's wait below: a pass leaving it by
        // another way (a relocation, a stop, no room) is not held (round 211).
        rendererLaunchHeld_.store(false, std::memory_order_release);
        if (playing_ && plan_ && plan_->split && !plan_->feedsCallback()) {
            // Everything is live: nothing to render. Stay one render-ahead
            // span past what the callback played, so that a plan giving the
            // renderer work takes over where it can still render ahead (the
            // usual edit horizon), not back at the epoch's start. Advanced
            // like rendering would (stream and timeline together, through the
            // loop), before the adoption below: the callback may have run far
            // since the last pass.
            std::int64_t target = (playedStream(seen) + aheadFrames_ + 63) / 64 * 64;
            detail::Plan* pending = nullptr;
            if (ratePlanRequests_.peek(pending) && pending->handEpoch == seen &&
                pending->rateSeekSequence == 0 && pending->handAt >= streamLocal_)
                target = std::min(target, pending->handAt);
            for (std::int64_t left = target - streamLocal_; left > 0;) {
                auto k = detail::rateUntil(
                    *plan_, pos_,
                    static_cast<std::uint32_t>(std::min<std::int64_t>(left, plan_->block)), loopR_,
                    0);
                // The launch queue still hears from this executor: its
                // horizon, and that it has nothing of an applied launch.
                k = launchesAt(*plan_, 0, seen, streamLocal_, k, true);
                pos_ = plan_->rateEnabled ? pos_ + k : loopR_.next(pos_, pos_ + k);
                streamLocal_ += k;
                left -= k;
            }
            renderedStream_.store(streamLocal_, std::memory_order_release);
        }
        TransportSnapshot control;
        if (!readTransport(control, kRendererTransportTries)) continue;
        detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
        bool queuedRate = false;
        if (rateScheduling_.load(std::memory_order_acquire)) {
            while (ratePlanRequests_.peek(plan)) {
                if (plan->handEpoch >= control.epoch) {
                    queuedRate = true;
                    break;
                }
                ratePlanRequests_.consume();
            }
            if (!queuedRate) plan = plan_;
        }
        if (rateScheduling_.load(std::memory_order_acquire) && control.seekSeq != seekSeen_ &&
            (!plan || plan->rateSeekSequence != control.seekSeq))
            continue;
        const bool seekReady =
            !plan || plan->rateSeekSequence == 0 ||
            (control.seekSeq >= plan->rateSeekSequence && control.epoch >= plan->handEpoch);
        const bool canonical = !playing_ || !plan || !plan->rateEnabled ||
                               plan->rateSeekSequence != 0 || streamLocal_ % 64 == 0;
        const bool sourceSafe = !playing_ || !plan_ || !plan || plan->rateSeekSequence != 0 ||
                                !plan_->rateEnabled || pos_ >= plan_->rateSafeDevice;
        bool adopt =
            plan != plan_ && planHandoffs_.free() > 0 && seekReady && canonical && sourceSafe &&
            (!plan || plan->generation != rateReseekGeneration_.load(std::memory_order_acquire));
        if (adopt && plan && plan->rateEnabled) {
            const auto ready = detail::rateAdoption(*plan, plan_, streamLocal_, control.epoch,
                                                    wantPlaying_.load(std::memory_order_acquire));
            if (ready == detail::RateAdoption::Reseek) {
                rateReseekGeneration_.store(plan->generation, std::memory_order_release);
                notifyMain();
            }
            adopt = ready == detail::RateAdoption::Ready;
        }
        if (adopt) {
            adoptRendererSources(plan);
            if (plan) {
                if (epoch_.load(std::memory_order_acquire) != seen) {
                    seen = relocate(seen);
                    plan->handContinuable = false;  // what was carried belongs to the old epoch
                }
                publishRendererPlan(*plan, seen, queuedRate);
            }
        } else if (plan_) {
            detail::claimRackModules(plan_->rendererModules, self);
            startFree(processing_, &plan_->rendererInstances, self);  // let go by the callback
        }
        if (epoch_.load(std::memory_order_acquire) != seen) seen = relocate(seen);
        if (plan_ && plan_->rateEnabled && plan_->rateEpochSeekSequence != seekSeen_) continue;
        if (const bool want = wantPlaying_.load(std::memory_order_acquire); want != playing_) {
            playing_ = want;
            if (!want) resetOwned(processing_, plan_);  // no hanging notes
            // Starting: what ran while stopped (plugins, the Console 9 noise)
            // starts over, so playback is the offline render's.
            if (want && plan_) {
                detail::locateClass(*plan_, pos_, false);
                resetOwned(processing_, plan_);
            }
            reportedPlaying_.store(playing_);
            notifyMain();
        }
        const bool held = hold_.load(std::memory_order_acquire);
        if (held) holdSeen_.store(true, std::memory_order_release);
        // Stopped: the renderer's plugins keep running at the device's pace
        // (nothing plays): they load files, answer their editors and ring out
        // as hosts run them. Early requested blocks are at least 1 ms apart,
        // even when a plug-in requests the next block from inside process().
        const bool idleWork = !playing_ && plan_ && !held && !plan_->rendererNodes.empty();
        const auto requested = [&] {
            if (idleWork)
                for (auto* instance : plan_->rendererInstances)
                    if (instance->processRequested() || instance->needsAudioFlush()) return true;
            return false;
        };
        if (idleWork) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= stoppedRenderDeadline(idleNext, idleLast, requested())) {
                const auto patchSequence = patchSeq_.load(std::memory_order_acquire);
                const bool ok = detail::processRendererBlock(workers_.get(), *plan_, pos_, block,
                                                             self, &loopR_, false);
                if (!ok) {
                    processErrors_.fetch_add(1);
                    notifyMain();
                }
                publishPluginOutputs(*plan_, true, seen, pos_, streamLocal_, patchSequence);
                // An early block replaces the next regular block; repeated
                // requests must not push its deadline ever farther ahead.
                idleNext = now < idleNext ? now + blockTime
                                          : std::max(idleNext, now - 4 * blockTime) + blockTime;
                idleLast = std::chrono::steady_clock::now();
            }
        }
        if (!rendererHasRoom(seen)) {
            const bool pendingBoundary = queuedRate && plan && plan->handEpoch == seen &&
                                         plan->rateSeekSequence == 0 && plan->handAt > streamLocal_;
            auto target = rendererSpaceTarget(seen);
            if (pendingBoundary)
                target = std::min(target, std::max<std::int64_t>(0, plan->handAt - aheadFrames_));
            rendererWakeAt_.store(target);
            // Seq-cst arm/stream handshake: either this observes consumption
            // before arming, or the callback observes the arm after publishing
            // consumption (and ring slots). No callback wakes below threshold.
            if (rendererHasRoom(seen) ||
                (pendingBoundary && playedStream(seen) + aheadFrames_ >= plan->handAt))
                continue;
            const auto idleDue = stoppedRenderDeadline(idleNext, idleLast, requested());
            rt::waitUntil(rendererEvent_, event,
                          rendererDeadline(std::chrono::steady_clock::now(), idleDue, idleWork,
                                           rendererSafetyTimeout()));
            continue;
        }
        // Its leave of the epoch before still waits for the callback's end
        // and a launch of this epoch it has not switched falls in the next
        // block: it waits for the callback's next cycle, which records that
        // end and wakes it (round 209). Should the wait outlast what it has
        // rendered ahead while playing, the callback plays silence for the
        // frames it lacks and counts an anticipation miss (the capture is
        // not as heard); the launch still switches at its frame.
        if (launchHeldByLeave(*plan_, seen, streamLocal_, streamLocal_ + block)) {
            rendererLaunchHeld_.store(true, std::memory_order_release);
            rt::waitUntil(rendererEvent_, event,
                          std::chrono::steady_clock::now() + rendererSafetyTimeout());
            continue;
        }
        auto renderFrames = block;
        if (queuedRate && plan != plan_ && plan->handEpoch == seen && plan->rateSeekSequence == 0 &&
            plan->handAt > streamLocal_)
            renderFrames =
                std::min(renderFrames, static_cast<std::uint32_t>(plan->handAt - streamLocal_));
        if (plan != plan_ && plan && plan->rateEnabled && streamLocal_ % 64 != 0)
            renderFrames =
                std::min(renderFrames, static_cast<std::uint32_t>(64 - streamLocal_ % 64));
        std::uint32_t n = detail::rateUntil(*plan_, pos_, renderFrames, loopR_,
                                            0);                  // no chunk crosses a loop wrap
        n = launchesAt(*plan_, 0, seen, streamLocal_, n, true);  // or a launch
        const auto patchSequence = patchSeq_.load(std::memory_order_acquire);
        const bool ok = [&] {
            BenchProbe::Timer timer(options_.benchProbe, false, n);
            return detail::processRendererBlock(workers_.get(), *plan_, pos_, n, self, &loopR_);
        }();
        if (!ok) {
            processErrors_.fetch_add(1);
            notifyMain();
        }
        publishPluginOutputs(*plan_, true, seen, pos_, streamLocal_, patchSequence);
        auto fill = [&](detail::Chunk* c, const detail::Stereo& from) {
            if (ok) {
                std::copy_n(from.l.begin(), n, c->l.begin());
                std::copy_n(from.r.begin(), n, c->r.begin());
            } else {
                std::fill(c->l.begin(), c->l.end(), 0.0f);
                std::fill(c->r.begin(), c->r.end(), 0.0f);
            }
            c->epoch = seen;
            c->start = pos_;
            c->stream = streamLocal_;
            c->frames = n;
            if (!c->timeline.empty()) {
                for (std::uint32_t i = 0; i <= n; ++i) {
                    c->timeline[i] = plan_->rateSample(pos_ + i);
                    c->rates[i] = plan_->rateEnabled
                                      ? plan_->timelines[0]->rate(pos_ + i - plan_->outputLatency)
                                      : 1.;
                }
            }
        };
        // Recorded masters first (ADR-0057): the callback that sees this
        // block's frontier or root chunk then finds their chunk too.
        for (const detail::RecordTap& tap : plan_->recordTaps)
            if (tap.ring >= 0) {
                detail::ChunkRing& ring = *plan_->recordRings[static_cast<std::size_t>(tap.ring)];
                fill(ring.slot(), detail::recordSignal(plan_->nodes[tap.node]));
                ring.publish();
            }
        if (plan_->split) {  // the frontier taps, for the callback's live nodes
            for (std::size_t j = 0; j < plan_->frontier.size(); ++j) {
                const detail::Edge& e = plan_->edges[plan_->frontier[j]];
                fill(plan_->frontierRings[j]->slot(), detail::edgeSignal(*plan_, e));
                plan_->frontierRings[j]->publish();
            }
        } else {  // the device's channels (ADR-0056)
            auto& output = plan_->rateOutput ? *plan_->rateOutput : *ring_;
            detail::Chunk* c = output.slot();
            fill(c, plan_->nodes[plan_->mainMaster].post);
            std::array<float*, 64> channels{};
            const std::uint32_t width = std::min<std::uint32_t>(c->width(), 64);
            for (std::uint32_t ch = 0; ch < width; ++ch) channels[ch] = c->channel(ch);
            if (ok)
                detail::writeOutputs(*plan_, channels.data(), width, 0, n);
            else
                for (std::uint32_t ch = 2; ch < width; ++ch) std::fill_n(channels[ch], n, 0.0f);
            output.publish();
        }
        // Metering (P5.3c): the renderer publishes what it just rendered, at
        // the same coordinates as the chunk it just handed over. The reader
        // holds these back until the callback has played them.
        if (ok && meters_)
            pushMeters(*meters_, *plan_, plan_->rendererMeterNodes, seen, streamLocal_, n, true);
        const auto next = plan_->rateEnabled ? pos_ + n : loopR_.next(pos_, pos_ + n);
        if (detail::rateWrapped(*plan_, pos_, pos_ + n, loopR_, false))
            detail::wrapTo(*plan_, next, 0);
        pos_ = next;
        streamLocal_ += n;
        renderedStream_.store(streamLocal_, std::memory_order_release);
    }
    // The renderer is its plugins' audio thread: stop them here.
    detail::adoptRackModules(plan_ ? &plan_->rendererModules : nullptr, nullptr, self);
    for (host::Instance* in : processing_) {
        in->stopProcessing();
        in->setOwner(nullptr);
    }
    processing_.clear();
    playing_ = false;
    reportedPlaying_.store(false);
    stoppedOnAudioThread_ = true;
    shutDown_.store(true);
    notifyMain();
}

// --- the live region (ADR-0014) -----------------------------------------------------------

namespace {
// A node that joined the running live region without a continuous handoff:
// its voices, held notes, delays, console and rack restart at `pos`, and the
// plug-ins `owner` (the callback) runs are reset.
void restartJoined(detail::Plan& plan, detail::Node& n, std::int64_t pos,
                   const void* owner) SUPPLE_RT {
    detail::forEachStage(n, [&](detail::Stage& st) {
        st.preserveDryOnLocate = false;
        if (st.instance && st.instance->owner() == owner) st.instance->reset();
        st.voices.clear();
        st.pendingVoices.clear();
        st.sleeping = false;
    });
    n.releaseHeld = false;
    n.releaseLive = false;
    n.held.fill(0);
    n.liveHeld.fill(0);
    detail::locateNode(plan, n, pos, true);
}
}  // namespace

void Engine::adoptLive(detail::Plan* next, bool continuous) SUPPLE_RT {
    if (livePlan_ && next && next->split) {
        detail::carryRateSources(*livePlan_, *next, 1);
        detail::carryLiveNotes(*livePlan_, *next, true);
        detail::carryMix(*livePlan_, *next, 1);
    }
    detail::adoptRackModules(livePlan_ ? &livePlan_->liveModules : nullptr,
                             next ? &next->liveModules : nullptr, &liveProcessing_);
    adoptShare(liveProcessing_, next ? &next->liveInstances : nullptr, &liveProcessing_);
    const bool wasLive = livePlan_ && livePlan_->split;
    detail::Plan* const previous = livePlan_;
    livePlan_ = next;
    if (!next) return;
    hardwareMidi_->collect(*next, livePos_, 0, 0, wantNow(), rtEpoch_);
    if (next->split) {
        if (wasLive && liveReady_) {
            detail::locateClass(*next, livePos_, true, false);  // continue at the switch position
            // Nodes joining the live region continue only from an exact
            // handoff; otherwise what the renderer carried (or left in the
            // instance) belongs elsewhere in time: they start afresh.
            if (!continuous)
                for (detail::Node& n : next->nodes) {
                    if (!n.live) continue;
                    const bool joined = std::none_of(
                        previous->nodes.begin(), previous->nodes.end(),
                        [&](const detail::Node& old) { return old.id == n.id && old.live; });
                    if (joined) restartJoined(*next, n, livePos_, &liveProcessing_);
                }
        } else {
            liveReady_ = false;  // entering the live mode: liveRender takes the position
            // Taken over at the handoff while playing, the renderer's nodes
            // continue with the state it carried (adoptRendererSources): held
            // notes play on. Otherwise (the first plan, stopped, a seek) the
            // live nodes start afresh.
            liveEntered_ = continuous;
            if (!continuous)
                for (detail::Node& n : next->nodes)  // (not continuous: carry no dry signal)
                    if (n.live)
                        detail::forEachStage(
                            n, [](detail::Stage& st) { st.preserveDryOnLocate = false; });
        }
    }
    callbackGen_.store(next->generation, std::memory_order_release);
}

void Engine::anticipativeCycle(const float* const* in, float* const* out, std::uint32_t frames,
                               std::uint32_t midiBase) SUPPLE_RT {
    TransportSnapshot xport;
    if (epoch_.load(std::memory_order_acquire) != rtEpoch_ &&
        readTransport(xport,
                      kAudioTransportTries)) {  // else: the main thread is writing, next cycle
        if (clickSeekSeen_ != xport.seekSeq) {
            clickAge_ = -1;
            clickSeekSeen_ = xport.seekSeq;
        }
        // Where the epoch it leaves stopped being heard: what it consumed of
        // it (ADR-0097, dated 2026-11-26), before it plays anything newer.
        const std::int64_t heard = playedStream(rtEpoch_);
        leaveLaunchEpoch(2, livePlan_, rtEpoch_, heard, true, xport.epoch);  // first (above)
        noteHeardEnd(rtEpoch_, xport.epoch, heard);
        launchLeftEpoch_[1] = rtEpoch_;
        launchLeftEnd_[1] = heard;
        if (livePlan_) takeBackNodes(*livePlan_, 1, rtEpoch_, heard);
        markLeft(2, rtEpoch_, xport.epoch);
        rtEpoch_ = xport.epoch;
        rtOffset_ = 0;
        rtPrimed_ = false;
        liveReady_ = liveEntered_ = false;  // a new epoch: the live region starts afresh
        loopC_ = xport.loop;                // the renderer takes the loop with the epoch too
        rtEpochSeen_.store(rtEpoch_, std::memory_order_release);
        wakeRenderer();
        notifyMain();
    }
    const std::uint64_t e = rtEpoch_;
    const bool playing = wantNow();
    std::array<const float*, 64> inAt{};
    const std::uint32_t inputs = deviceInputs_;
    for (std::uint32_t done = 0; done < frames;) {
        std::uint32_t k = frames - done;
        // Adopt every rendered trajectory at its stream boundary. Replacing
        // this queue with a latest pointer would skip an intermediate rate on
        // live sources while its frontier audio was already audible.
        for (detail::Plan* hand = nullptr; planHandoffs_.peek(hand);) {
            const bool oldEpoch = hand->handEpoch < e;
            // Unity plans have no epoch-specific prepared source state. A
            // seek/loop between renderer adoption and the first callback must
            // not discard the only graph for live monitoring. Rate plans keep
            // the strict epoch check: their converter state belongs to a seek.
            if (oldEpoch && hand->rateEnabled) {
                // Skipped, never adopted: a node it took into the live region
                // is lost there (round 178 F2, round 180 F1).
                ackSkipped(*hand);
                classBase_ = hand;
                planHandoffs_.consume();
                continue;
            }
            if (hand->handEpoch > e) break;
            const auto here =
                livePlan_ && livePlan_->split && liveReady_ ? liveStream_ : playedStream(e);
            if (oldEpoch || !playing || here >= hand->handAt) {
                // Exactly at the handoff, playing, in its epoch: the renderer's
                // carried state is the callback's here (adoptLive).
                const bool continuous =
                    !oldEpoch && playing && hand->handContinuable && here == hand->handAt;
                if (livePlan_)
                    ackClassMoves(*livePlan_, classBase_, *hand, hand->liveCarried, e, here);
                classBase_ = nullptr;
                adoptLive(hand, continuous);
                ackReturns(*hand, 1, false, e, here);  // the live region's own
                planHandoffs_.consume();
                wakeRenderer();
                continue;
            }
            if (hand->handAt - here < k) k = static_cast<std::uint32_t>(hand->handAt - here);
            break;
        }
        for (std::uint32_t c = 0; c < inputs; ++c) inAt[c] = in && in[c] ? in[c] + done : nullptr;
        const auto outAt = advanced(out, done);
        const float* const* ins = in ? inAt.data() : nullptr;
        if (livePlan_ && livePlan_->split) {
            k = liveRender(ins, outAt.data(), k, midiBase + done);
        } else if (countHold_) {  // counting in: the rendered audio waits
            silence(outAt.data(), 0, k);
        } else {
            // Nothing runs here, but the launch queue still hears from the
            // callback: its horizon, and that it has nothing of an applied
            // launch (else the slot would never be free again).
            if (playing && livePlan_) (void)launchesAt(*livePlan_, 1, e, playedStream(e), k, false);
            const Pulled played = pullAhead(outAt.data(), k);
            if (played.wrapped) k = played.frames;  // the rest continues at the loop start
            if (played.first != kNothingPlayed && livePlan_)
                addClicks(*livePlan_, outAt.data(), 0, played.first, played.frames);
            if (livePlan_ && played.first != kNothingPlayed)
                pullRecordTaps(*livePlan_, e, played.stream, played.frames);
            // Only input that went with played audio is recorded; what came
            // during a dropout had no timeline position and counts as lost.
            if (detail::Recorder* r = recorder_.load()) {
                if (played.first != kNothingPlayed) {
                    const auto timeline =
                        livePlan_ ? static_cast<std::int64_t>(
                                        std::llround(livePlan_->rateSample(played.first, true)))
                                  : played.first;
                    r->capture(ins, inputs, played.frames, timeline,
                               livePlan_ ? std::span<const detail::ProcessedInput>(
                                               livePlan_->processedInputs)
                                         : std::span<const detail::ProcessedInput>());
                    r->captureMidi(deviceMidi_, midiBase + done, played.frames, timeline);
                    r->lose(k - played.frames);
                } else if (playing) {
                    r->lose(k);
                }
            }
        }
        done += k;
    }
}

std::uint32_t Engine::liveRender(const float* const* in, float* const* out, std::uint32_t k,
                                 std::uint32_t offset) SUPPLE_RT {
    detail::Plan& p = *livePlan_;
    const void* self = &liveProcessing_;
    if (!liveReady_) {
        if (epochDone_.load(std::memory_order_acquire) < rtEpoch_) {  // not placed yet
            silence(out, 0, k);
            return k;
        }
        livePos_ = playedPosition(rtEpoch_, epochStart_.load());
        liveStream_ = playedStream(rtEpoch_);
        // The first plan, or a new epoch, starts afresh; a plan taking over
        // from the renderer mid-epoch continues (adoptLive).
        detail::locateClass(p, livePos_, true, !liveEntered_);
        if (!liveEntered_) resetOwned(liveProcessing_, &p);
        liveReady_ = true;
        liveEntered_ = false;
    }
    detail::claimRackModules(p.liveModules, self);
    startFree(liveProcessing_, &p.liveInstances, self);  // let go by the renderer
    // The frontier: what the renderer made for this position. Before its
    // first chunk (pre-roll) time holds still; the monitored input is heard.
    bool playing = wantNow();
    if (options_.benchProbe && playing && rtPrimed_ && p.feedsCallback() &&
        epochDone_.load(std::memory_order_acquire) == rtEpoch_)
        options_.benchProbe->observeAhead(renderedStream_.load(std::memory_order_acquire) -
                                          liveStream_);
    if (playing) k = detail::rateUntil(p, livePos_, k, loopC_, 1);       // stop at a loop wrap
    if (playing) k = launchesAt(p, 1, rtEpoch_, liveStream_, k, false);  // or a launch
    // Pre-roll until a whole render block of the epoch is rendered (ADR-0012).
    // Its rings let go of older epochs' chunks meanwhile: the renderer may
    // be waiting for that room.
    if (playing && p.feedsCallback() && !rtPrimed_) {
        for (const auto& ring : p.frontierRings) ring->dropBefore(rtEpoch_);
        for (const auto& ring : p.recordRings) ring->dropBefore(rtEpoch_);
        if (!primeReady(rtEpoch_, k)) playing = false;
    }
    std::uint32_t found = k;
    for (std::size_t j = 0; j < p.frontier.size(); ++j) {
        detail::Edge& e = p.edges[p.frontier[j]];
        const std::uint32_t got =
            playing ? p.frontierRings[j]->pullAt(rtEpoch_, liveStream_, e.frontierBuf.l.data(),
                                                 e.frontierBuf.r.data(), k)
                    : 0;
        if (!playing) e.frontierBuf.clear(k);
        found = std::min(found, got);
    }
    for (detail::RecordTap& tap : p.recordTaps) {  // recorded masters (ADR-0057)
        if (tap.ring < 0) continue;
        const std::uint32_t got =
            playing ? p.recordRings[static_cast<std::size_t>(tap.ring)]->pullAt(
                          rtEpoch_, liveStream_, tap.pulled.l.data(), tap.pulled.r.data(), k)
                    : 0;
        if (!playing) tap.pulled.clear(k);
        found = std::min(found, got);
    }
    if (playing && p.feedsCallback()) {
        if (found > 0) {
            rtPrimed_ = true;
        } else if (!rtPrimed_) {
            playing = false;  // pre-roll
        }
        if (rtPrimed_ && found < k) misses_.fetch_add(1, std::memory_order_relaxed);
    }
    p.deviceIn = in;
    p.deviceInChannels = deviceInputs_;
    p.deviceRecording = recorder_.load() != nullptr;
    p.deviceMidi = deviceMidi_;
    p.deviceMidiOffset = offset;
    const auto patchSequence = patchSeq_.load(std::memory_order_acquire);
    const bool ok = detail::processCallbackBlock(liveWorkersReady_.load(std::memory_order_acquire),
                                                 p, livePos_, k, playing, true, self, &loopC_);
    if (!ok) {
        processErrors_.fetch_add(1);
        notifyMain();
    }
    if (ok) hardwareMidi_->collect(p, livePos_, k, offset, playing, rtEpoch_);
    publishPluginOutputs(p, false, rtEpoch_, livePos_, liveStream_, patchSequence);
    // Metering (P5.3c): the callback measures the live nodes it just played, at
    // the position it played them from. While the transport is stopped nothing
    // is rendered ahead, so a monitored input is heard as it is produced.
    if (ok && meters_ && (playing || !wantNow()))
        pushMeters(*meters_, p, p.liveMeterNodes, rtEpoch_, liveStream_, k, false);
    if (ok)
        writeDevice(p, out, 0, k);
    else
        silence(out, 0, k);
    if (playing) addClicks(p, out, 0, livePos_, k);
    if (playing) {
        if (detail::Recorder* r = recorder_.load()) {
            const auto timeline =
                static_cast<std::int64_t>(std::llround(p.rateSample(livePos_, true)));
            r->capture(in, deviceInputs_, k, timeline, p.processedInputs);
            r->captureMidi(p.deviceMidi, offset, k, timeline);
            captureRoutedMidi(*r, p, k);
        }
        const auto next = p.rateEnabled ? livePos_ + k : loopC_.next(livePos_, livePos_ + k);
        if (detail::rateWrapped(p, livePos_, livePos_ + k, loopC_, true))
            detail::wrapTo(p, next, 1);
        livePos_ = next;
        liveStream_ += k;
        rtPos_.store(livePos_, std::memory_order_relaxed);
        rtTimeline_.store(p.rateSample(livePos_, true), std::memory_order_relaxed);
        effectivePlayrate_.store(
            p.rateEnabled
                ? p.timelines[1]->rate(livePos_ - 1 - p.outputLatency - p.rateBackendLatency)
                : 1.,
            std::memory_order_relaxed);
        timelineEpoch_.store(rtEpoch_, std::memory_order_release);
        publishRateClock(p, livePos_, liveStream_, rtEpoch_, true);
        rtStream_.store(liveStream_);
        rtPosEpoch_.store(rtEpoch_);
    }
    return k;
}

}  // namespace supple::engine
