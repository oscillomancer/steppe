// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The real-time engine (spec: docs/specs/engine.md §1.6, §2.3): plays a
// session through an audio backend. The main thread compiles a session
// version into a plan and hands it to the audio thread (RtHandoff); the audio
// thread adopts it at the next cycle, starts processing new plugin instances
// and stops removed ones; retired plans and unused instances are freed on the
// main thread (service()). Transport commands travel on a wait-free queue.
//
// Anticipative nodes render ahead; callback-owned nodes use a separate live
// fork-join pool (ADR-0024). Output is bit-identical to the offline renderer for
// block-size-invariant plugins; it lags the render position by the plan's
// output latency (the transport reports the render position).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/handoff.h"
#include "supple/base/main_thread_wake.h"
#include "supple/base/spsc.h"
#include "supple/base/thread.h"
#include "supple/engine/midi_ports.h"
#include "supple/engine/offline.h"
#include "supple/engine/sync.h"

namespace supple::engine {

namespace detail {
struct Plan;
class BufferPool;
struct ChunkRing;
class SyncRuntime;
class Workers;
class LiveWorkers;
class Recorder;
class HardwareMidiRuntime;
}  // namespace detail

class MeterBank;  // meters.h: per-strip peak/RMS telemetry (P5.3c)

// Recording (ADR-0014): one armed track's input.
struct RecordTarget {
    model::Id track;
    std::vector<int> inputs;  // device input channels, 0-based (one per file channel)
    std::filesystem::path file;
    // MIDI instead of audio (ADR-0016): which device events it records.
    struct Midi {
        std::uint64_t ports = 0;
        int fromChannel = 0, toChannel = 0;
        // Include Note Out routes, on the destination's musical input clock.
        // startRecording resolves its shift from the loaded plan.
        bool routed = false;
        std::int64_t shift = 0;
    };
    std::optional<Midi> midi;
    // A master's output instead of device input (ADR-0057): two channels,
    // the master's mix before its own items. startRecording sets
    // `outputShift`, the master's timeline shift where that mix is produced.
    bool output = false;
    std::int64_t outputShift = 0;
    // A rack's Out instead of device channels: stereo input, before items
    // and inserts, after input FX. outputShift is its input clock + input FX.
    bool rackInput = false;
};

// A master recording its output (ADR-0057): its own items do not play
// within [startSeconds, endSeconds) (the punch range), or anywhere when
// `everywhere`, so what is heard there is what is recorded.
struct RecordingMaster {
    model::Id master;
    bool everywhere = true;
    double startSeconds = 0, endSeconds = 0;
};

// A recorded MIDI message, where it was heard, and in which run (loop pass).
struct RecordedMidiEvent {
    std::int64_t timeline = 0;
    std::uint32_t run = 0;
    std::uint8_t bytes[3] = {0, 0, 0};
    // Note Out's stable route/voice address; device messages retain -1.
    model::Id route = {};
    int voice = -1;
};

// A contiguous run of a recording on the timeline (ADR-0015): a loop wrap
// starts a new one. Frames from `fileFrame` on were heard at `timeline` on.
struct RecordedSegment {
    std::int64_t fileFrame = 0;
    std::int64_t timeline = 0;
};

// A writer-thread failure, consumed by the model before recording stops.
struct RecordingError {
    model::Id track;
    std::filesystem::path file;
    Error error;
};

// Punch placement in recording sample-rate frames, independent of later transport.
struct RecordedPunch {
    bool enabled = false;
    std::int64_t start = 0, end = 0;
    bool operator==(const RecordedPunch&) const = default;
};

struct RecordedTake {
    model::Id track;
    std::filesystem::path file;
    // Optional known digest (e.g. a recovered take). stopRecording leaves it
    // empty: EngineApp hashes closed audio files in a background job.
    std::string hash;
    std::filesystem::path journal;  // owned audio placement journal, retired after save
    std::int64_t startSample = 0;   // timeline sample of the file's first frame (may be < 0)
    std::int64_t frames = 0;
    // The runs, in file order; the first is {0, startSample}. More than one:
    // loop passes (`loop` is the loop they were recorded in), or runs split
    // by seeks while no loop was active (`loop` is then [0, 0]).
    std::vector<RecordedSegment> segments;
    struct {
        std::int64_t start = 0, end = 0;
    } loop;
    std::uint64_t overflowFrames = 0;  // input lost because the disk fell behind
    std::string error;                 // first file/sidecar error of this target, if any
    std::optional<Error> failure;      // structured first error, including stop/close failures
    std::vector<Error> warnings;       // e.g. recording.loop_runs_full; retained passes are usable
    double sampleRate = 0;             // of the file and of startSample (the engine's rate)
    // MIDI (ADR-0016): no file; `frames` is the length recorded, `segments`
    // the runs, the events in time order.
    std::optional<RecordedPunch> punch = {};  // authoritative when present
    bool isMidi = false;
    std::vector<RecordedMidiEvent> midi;
};

struct BackendConfig {
    double sampleRate = 48000.0;
    std::uint32_t blockSize = 256;
    std::uint32_t inputs = 0, outputs = 2;
    bool midiInputs = false;   // open the device's MIDI inputs too (ADR-0016)
    bool midiOutputs = false;  // open hardware MIDI destinations (ADR-0069)
};

// A file audition (ADR-0094): decoded audio at the device's rate, played on
// outputs 1/2 after the project's output and outside it. Immutable once
// published. `right` empty: mono, played on both.
struct AuditionVoice {
    std::vector<float> left, right;
    std::size_t frames() const { return left.size(); }
};
// What the callback plays (Engine::auditionState): the voice's serial (0:
// none) and its frame, and the serial of the last request it took (a voice
// published and not yet taken is newer).
struct AuditionState {
    std::uint64_t serial = 0;
    bool playing = false;
    std::int64_t position = 0;
    std::uint64_t taken = 0;
};

// A short MIDI message from a device input during one callback (ADR-0016).
struct MidiInputEvent {
    std::uint32_t frame = 0;  // within the callback's frames
    std::uint16_t port = 0;   // index into Backend::midiPorts()
    std::uint8_t size = 0;    // 1..3
    std::uint8_t bytes[3] = {0, 0, 0};
};
inline constexpr std::size_t kMaxMidiInputEvents = 1024;  // per callback

// Hardware output, relative to the current engine callback (ADR-0069).
// Complete MIDI 1.0 messages, including system common/realtime and bounded SysEx.
struct MidiOutputEvent {
    std::uint32_t frame = 0;
    std::uint16_t port = 0;  // index into midiOutputPorts()
    std::uint8_t size = 0;   // 1..10, no running status
    std::uint8_t bytes[10]{};
};
inline constexpr std::size_t kMaxMidiOutputEvents = 1024;  // per device period
// Of a period's output events, what only releases and resets may use: note
// streams and clock or MTC stop short of it, so a stop's whole-port
// all-notes-off always fits (16 channels x CC64 + CC123 x 16 ports).
inline constexpr std::size_t kMidiReleaseReserve = 512;
namespace detail {
// Fixed callback staging shared by all backends. Stable frame order at flush.
class MidiOutputCycle {
  public:
    void begin(std::uint32_t frames, std::size_t ports) SUPPLE_RT;
    void chunk(std::uint32_t offset, std::uint32_t frames) SUPPLE_RT;
    bool write(const MidiOutputEvent&) SUPPLE_RT;
    std::size_t size() const SUPPLE_RT { return count_; }  // written this period
    std::span<const MidiOutputEvent> sorted() SUPPLE_RT;
    void drop(std::uint64_t count = 1) SUPPLE_RT {
        dropped_.fetch_add(count, std::memory_order_relaxed);
    }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void reset() {
        dropped_.store(0);
        begin(0, 0);
    }

  private:
    std::array<MidiOutputEvent, kMaxMidiOutputEvents> events_{};
    std::size_t count_ = 0, ports_ = 0;
    std::uint32_t offset_ = 0, frames_ = 0;
    std::atomic<std::uint64_t> dropped_{0};
};
}  // namespace detail

// Whether a device event passes a MIDI route: its port is in `ports` (bit
// per Backend::midiPorts() index) and its channel matches `fromChannel`
// (1-based, 0: any). `out` gets the message with its channel set to
// `toChannel` (0: kept). Only channel messages pass.
inline bool routeMidi(const MidiInputEvent& e, std::uint64_t ports, int fromChannel, int toChannel,
                      std::uint8_t out[3]) {
    if (e.size == 0 || e.port >= 64 || ((ports >> e.port) & 1u) == 0) return false;
    const std::uint8_t status = e.bytes[0];
    if (status < 0x80 || status >= 0xf0) return false;
    const int ch = status & 0x0f;
    if (fromChannel > 0 && ch != fromChannel - 1) return false;
    out[0] =
        static_cast<std::uint8_t>((status & 0xf0) | (toChannel > 0 ? (toChannel - 1) & 0x0f : ch));
    out[1] = e.size > 1 ? e.bytes[1] : 0;
    out[2] = e.size > 2 ? e.bytes[2] : 0;
    return true;
}
// A sync message from a device input (ADR-0071): Clock (F8), Start (FA),
// Continue (FB), Stop (FC), Song Position (F2 lsb msb), an MTC quarter
// frame (F1 dd) or an MTC full frame (F0 7F <id> 01 01 hh mm ss ff F7).
// Backends deliver them beside the channel messages of midiInput(), which
// keeps its contract.
struct SyncMidiEvent {
    std::uint32_t frame = 0;  // within the callback's frames
    std::uint16_t port = 0;   // index into Backend::midiPorts()
    std::uint8_t size = 0;    // 1..10
    std::array<std::uint8_t, 10> bytes{};
};
inline constexpr std::size_t kMaxSyncMidiEvents = 512;  // per callback
// The size of the sync message `bytes` starts with, or 0 when it is not one
// (a full frame needs all ten bytes in place). `size` bytes are readable.
inline std::size_t syncMidiSize(const std::uint8_t* bytes, std::size_t size) SUPPLE_RT {
    if (size == 0) return 0;
    switch (bytes[0]) {
        case 0xF8:
        case 0xFA:
        case 0xFB:
        case 0xFC:
            return 1;
        case 0xF1:
            return size >= 2 && bytes[1] < 0x80 ? 2 : 0;
        case 0xF2:
            return size >= 3 && bytes[1] < 0x80 && bytes[2] < 0x80 ? 3 : 0;
        case 0xF0:
            return size >= 10 && bytes[1] == 0x7F && bytes[3] == 0x01 && bytes[4] == 0x01 &&
                           bytes[9] == 0xF7
                       ? 10
                       : 0;
        default:
            return 0;
    }
}
// A device's control message for MIDI learn (ADR-0027): a note, a CC, a
// program change or a pitch bend, as heard, in arrival order.
struct ControlMidiEvent {
    std::uint16_t port = 0;  // index into Backend::midiPorts()
    std::uint8_t bytes[3] = {0, 0, 0};
};
// Note off/on, CC, program change and pitch bend; not aftertouch or system.
inline bool isControlMidi(std::uint8_t status) {
    const std::uint8_t kind = status & 0xf0;
    return kind == 0x80 || kind == 0x90 || kind == 0xb0 || kind == 0xc0 || kind == 0xe0;
}

// The ports a route's device name selects: a case-insensitive substring of
// the port name; "" or "*" selects every port.
std::uint64_t midiPortMask(const std::vector<std::string>& ports, std::string_view device);

// A native MIDI input that could not be opened. Owner thread only.
struct MidiInputWarning {
    std::string device;
    std::int64_t osError = 0;
    std::string message;
    std::string hint = "close the other application and restart the engine";
};

// What a running backend reports (for engine.status).
struct BackendStatus {
    std::uint32_t period = 0;         // the device's period, in frames
    std::uint64_t xruns = 0;          // reported by the device
    std::uint32_t outputLatency = 0;  // frames from the backend's outputs to the device
    std::uint32_t inputLatency = 0;   // frames from the device to the backend's inputs
    bool restartNeeded = false;       // the device's rate changed or it went away
    std::string_view restartReason;   // why, for engine.restarting (may be empty)
    bool midiDevicesChanged = false;  // advisory only; audio keeps running
};

// Whether the live helpers joined the device's audio workgroup (macOS
// CoreAudio; ADR-0038). `state`: "none" (no workgroup applies: other platforms,
// JACK, dummy), "ready" (found, no helper joined), "joined" (every helper that
// tried), "partial", "failed" (lookup refused or every join refused).
struct WorkgroupStatus {
    std::string state = "none";
    std::uint32_t joined = 0;  // helper threads currently in the workgroup
    std::uint32_t failed = 0;  // helper joins refused since start
    std::int64_t osError = 0;  // the last refusal's OS error, 0 if none
    std::string message, hint;
};

// An audio device driver. The callback runs on the backend's audio thread.
// Contract for stop(): after the last audio cycle and before stop() returns,
// the backend calls the callback once more with frames == 0 on its audio
// thread (a Manual driver: on the caller, which is its audio thread), so the
// engine can stop every plugin where CLAP requires it.
class Backend {
  public:
    using Callback =
        std::function<void(const float* const* in, float* const* out, std::uint32_t frames)>;
    virtual ~Backend() = default;
    // Install before start; relay outlives stop/close. Default backends have no
    // asynchronous status producers. Null detaches after stop/close.
    virtual void setMainThreadWake(MainThreadWake*) {}
    virtual Status start(const BackendConfig& config, Callback callback) = 0;
    virtual void stop() = 0;
    virtual BackendConfig config() const = 0;
    virtual std::string name() const = 0;
    // True when callbacks run on a thread of the backend's own (a device);
    // false for a backend driven by its caller.
    virtual bool ownsThread() const = 0;
    // Native device helpers apply scheduling on their own thread before work.
    // Setup/cleanup are non-RT; the backend outlives all owned thread joins.
    virtual bool nativeThreadPolicy() const { return false; }
    virtual Status prepareCurrentThread(bool) { return okStatus(); }
    virtual void releaseCurrentThread() {}
    // Audio workgroup membership of the live helpers; reads atomics.
    virtual WorkgroupStatus workgroup() const { return {}; }
    // Main thread after start(), before live jobs: configure an idle helper.
    // Normal/dummy backends keep normal scheduling. RT backends MUST override
    // this and refuse unsafe scheduling/capacity (engine then runs serially).
    virtual unsigned liveThreadCapacity() const { return 64; }
    virtual Status prepareLiveThread(std::thread::native_handle_type) {
        return Error("engine.live_threads", "backend does not support live helper scheduling", {},
                     "use Audio processing threads Limit 1 or a backend supporting live helpers");
    }
    // Main thread after start(): the device callback thread's scheduling, from
    // which the engine derives its background threads' (engine.md §2.2). None
    // when the backend cannot tell (a manual driver's callback is its caller).
    virtual ThreadPolicy callbackPolicy() { return {}; }
    virtual BackendStatus status() const { return {config().blockSize, 0, 0, 0, false, {}}; }
    // MIDI (ADR-0016). Inside the callback only: this cycle's device events,
    // sorted by frame (at most kMaxMidiInputEvents; more are dropped).
    virtual std::span<const MidiInputEvent> midiInput() const { return {}; }
    // Inside the callback only: this cycle's sync messages (ADR-0071), sorted
    // by frame, from every port (at most kMaxSyncMidiEvents; more are dropped,
    // and counted in midiDropped() where the backend reports drops).
    virtual std::span<const SyncMidiEvent> syncMidi() const { return {}; }
    virtual std::uint64_t midiDropped() const { return 0; }
    // The MIDI input ports, named after their devices (fixed while running).
    virtual std::vector<std::string> midiPorts() const { return {}; }
    virtual std::vector<MidiInputWarning> midiWarnings() const { return {}; }
    // Output writes are callback-only, bounded and allocation-free. False
    // counts a rejected event; accepted events may still be lost downstream.
    virtual bool writeMidi(const MidiOutputEvent&) SUPPLE_RT { return false; }
    // Events accepted so far this device period, by every writer (routed
    // MIDI, clock and MTC), for the shared release reserve.
    virtual std::size_t midiOutputUsed() const SUPPLE_RT { return 0; }
    virtual std::vector<std::string> midiOutputPorts() const { return {}; }
    virtual std::uint64_t midiOutDropped() const { return 0; }
    // Loss after callback acceptance, attributed to an output port.
    virtual std::uint64_t midiOutAsyncDropped(std::size_t) const { return 0; }
    virtual std::vector<MidiInputWarning> midiOutputWarnings() const { return {}; }
    // Audio output latency minus MIDI port latency, in frames (signed).
    // At equal cycle frames, positive means MIDI leaves earlier than audio.
    // Routing must DELAY MIDI by this plus the user's offset to align it.
    // Backends do not apply this compensation implicitly. Owner thread only.
    virtual std::vector<std::int64_t> midiOutputLatency() const { return {}; }
    // What each audio input channel is connected to (index 0 = channel 1;
    // "" when unconnected), fixed while running. Main thread.
    virtual std::vector<std::string> inputNames() const { return {}; }
    // What each audio output channel is connected to, likewise (ADR-0056).
    virtual std::vector<std::string> outputNames() const { return {}; }
};

// No device. `Manual` runs cycles on the caller's thread (deterministic
// tests); `Thread` runs them on its own thread, paced like a device or free.
// Either can capture the output.
class DummyBackend : public Backend {
  public:
    enum class Mode { Manual, Thread };
    struct Options {
        Mode mode = Mode::Manual;
        bool paced = false;              // Thread mode: sleep one block period per cycle
        std::int64_t captureFrames = 0;  // frames of output to keep
        // Tests: the rate start() grants, as a device may, instead of the
        // one requested (0: the requested rate).
        double grantedRate = 0;
        // Tests: fills the input channels before each cycle (on the audio
        // thread; `frame` counts input frames since start). Null: silence.
        std::function<void(float* const* in, std::uint32_t channels, std::uint32_t frames,
                           std::int64_t frame)>
            input;
        // Tests: MIDI input ports and the events of each cycle (appended to
        // `events`, frames within the cycle; `frame` as for `input`).
        std::vector<std::string> midiOutputPorts;
        std::vector<std::int64_t> midiOutputLatency;
        // Captured events use absolute frames since start. Fixed capacity
        // chosen before start; overflow counts as output drops.
        std::size_t midiCaptureEvents = 65536;
        std::vector<std::string> midiPorts;
        std::function<void(std::vector<MidiInputEvent>& events, std::uint32_t frames,
                           std::int64_t frame)>
            midi;
        // Tests: the sync messages of each cycle (ADR-0071), likewise.
        std::function<void(std::vector<SyncMidiEvent>& events, std::uint32_t frames,
                           std::int64_t frame)>
            sync;
    };
    explicit DummyBackend(Options options);
    ~DummyBackend() override;

    Status start(const BackendConfig& config, Callback callback) override;
    void stop() override;
    BackendConfig config() const override { return config_; }
    std::string name() const override { return "dummy"; }
    bool ownsThread() const override { return options_.mode == Mode::Thread; }
    Status prepareLiveThread(std::thread::native_handle_type) override { return okStatus(); }
    ThreadPolicy callbackPolicy() override;  // Thread mode: its thread's
    std::span<const MidiInputEvent> midiInput() const override { return midi_; }
    std::span<const SyncMidiEvent> syncMidi() const override { return sync_; }
    std::vector<std::string> midiPorts() const override { return options_.midiPorts; }

    bool writeMidi(const MidiOutputEvent& event) SUPPLE_RT override {
        return midiOutput_.write(event);
    }
    std::size_t midiOutputUsed() const SUPPLE_RT override { return midiOutput_.size(); }
    std::vector<std::string> midiOutputPorts() const override;
    std::vector<std::int64_t> midiOutputLatency() const override;
    std::uint64_t midiOutDropped() const override { return midiOutput_.dropped(); }
    std::uint64_t midiOutAsyncDropped(std::size_t port) const override {
        return port < 16 ? midiAsyncDrops_[port].load() : 0;
    }
    struct CapturedMidi {
        std::int64_t frame = 0;
        MidiOutputEvent event;
    };
    std::span<const CapturedMidi> midiCapture() const { return midiCapture_; }  // after run/stop

    void run(std::int64_t cycles);  // Manual mode: run cycles now, on this thread
    std::int64_t cycles() const { return cycles_.load(); }
    // Every output channel; read after stop() / run().
    const audio::AudioData& capture() const { return capture_; }

  private:
    detail::MidiOutputCycle midiOutput_;
    std::vector<CapturedMidi> midiCapture_;
    void captureMidi();  // this callback's MIDI output, into midiCapture_
    std::array<std::atomic<std::uint64_t>, 16> midiAsyncDrops_{};
    std::vector<MidiInputEvent> midi_;  // this cycle's (capacity reserved)
    std::vector<SyncMidiEvent> sync_;   // likewise
    void cycle();
    void finishMidi();
    Options options_;
    BackendConfig config_;
    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> cycles_{0};
    std::vector<std::vector<float>> outputs_;  // config.outputs channels
    std::vector<float*> outPtrs_;
    std::vector<std::vector<float>> inputs_;
    std::vector<float*> inPtrs_;
    std::int64_t inputFrame_ = 0;
    audio::AudioData capture_;
    std::int64_t captured_ = 0;
};

// The transport's loop (ADR-0015), in plan samples.
struct LoopRange {
    std::int64_t start = 0, end = 0;
    bool on = false;
    bool active() const { return on && end > start; }
    // Frames of [pos, pos + n) before the loop end (n when it is not crossed;
    // a block starting at or past the end never wraps).
    std::uint32_t until(std::int64_t pos, std::uint32_t n) const {
        return active() && pos < end && pos + n > end ? static_cast<std::uint32_t>(end - pos) : n;
    }
    // After frames [from, to): where playback continues.
    std::int64_t next(std::int64_t from, std::int64_t to) const {
        return active() && from < end && to == end ? start : to;
    }
};

struct TransportState {
    bool playing = false;
    bool countingIn = false;  // playing, but the count-in still holds the position
    std::int64_t sample = 0;  // render position
    double seconds = 0.0;
    LoopRange loop;
};

struct BenchProbe;

struct EngineOptions {
    // Render ahead of the playhead on the anticipation thread and the worker
    // pool (ADR-0012). Off: every node runs in the audio callback.
    bool anticipative = true;
    std::uint32_t renderBlock = 1024;  // frames per anticipative block (64 .. 8192)
    double renderAheadMs = 200.0;      // how far ahead of the playhead
    int workers = -1;                  // helpers per pool; -1: every allowed CPU but three
    BenchProbe* benchProbe = nullptr;  // benchmark only; preallocated, outlives stop()
    // The floor of each callback cycle's bridge deadline (ADR-0081): bridged
    // calls on live nodes wait until the cycle's start plus its period or
    // this, whichever is larger. Tests raise it so that a loaded machine
    // never makes a healthy bridged plug-in late.
    double liveBridgeDeadlineMs = 2.0;
    // The renderer's and background pool's scheduling (engine.md §2.2,
    // ADR-0026). Auto: real-time below a real-time callback, else normal. The
    // others force it for benchmarks and tests (still below a real-time
    // callback, within the same checks).
    enum class Background { Auto, Normal, Fifo };
    Background background = Background::Auto;
    int backgroundPriority = 0;  // Fifo; 0: Engine::kBackgroundPriority
    // Tests (route E stage 5): compile each plan a second time and compare
    // the two (engine.md "Edits are being made cheaper"); a difference fails
    // the load with engine.plan_differs. It doubles each compile's time.
    bool verifyPlans = false;
    // A compile reuses the buffers of plans no executor can reach any more
    // (engine.md, the compile profile's step 3). Off: each allocates anew.
    bool recycleBuffers = true;
    // Tests: the pool fills every buffer it takes back with loud garbage, so
    // a reuse that fails to clear one is heard.
    bool poisonRecycledBuffers = false;
};

// Output values remain in the host event domain (VST3 normalized). Main-thread
// consumers convert them, honor DontRecord and never echo them back to DSP.
struct PluginOutput {
    model::Id fx;
    // The plan that produced it, and the patch sequence when its block began
    // (ADR-0092): odd while patches are being applied.
    std::uint64_t planGeneration = 0, epoch = 0, patchSequence = 0;
    std::int64_t timelineSample = 0, streamSample = 0;
    host::OutputEvent event;
    bool idle = false, anticipative = false;
};
struct PluginOutputs {
    std::vector<PluginOutput> events;
    std::uint64_t dropped = 0;       // cumulative since this Engine was constructed
    std::uint64_t voiceDropped = 0;  // voice tracking capacity, same lifetime
    // The FX whose outputs were dropped since the last take (each once), or
    // droppedAll when more did than are tracked: only these need recovery.
    std::vector<model::Id> droppedFx;
    bool droppedAll = false;
};

// Prepared main-thread rack timing, without instantiating plug-ins for queries.
struct RackLatencyInfo {
    std::int64_t latency = 0;
    std::map<model::Id, std::int64_t> outputs;
};

class Engine {
  public:
    Engine(InstancePool& plugins, MediaCache& media, EngineOptions options = {});
    ~Engine();
    // Main thread before start; wake outlives stop().
    void setMainThreadWake(MainThreadWake& wake) { mainWake_ = &wake; }
    // Main thread: one-shot callback watermark for queued anticipative outputs.
    // INT64_MAX disarms it. Rechecks after arming so consumption cannot be lost.
    void armOutputWake(std::uint64_t epoch, std::int64_t stream);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the backend with the engine's callback.
    Status start(Backend& backend, const BackendConfig& config);
    // Stops processing on every instance (on the audio thread), then the backend.
    void stop();
    bool running() const { return backend_ != nullptr; }

    // Compiles `session` and hands the plan to the audio thread. A session
    // that differs from the last one only in strip, send and VCA levels and
    // pans (and in the points of envelopes a write pass holds) updates the
    // running plan instead, without compiling (ADR-0060).
    Status load(const model::Session& session, const std::filesystem::path& projectDir = {});
    // A load() that playrate scheduling deferred returned success before
    // compiling. If the edit then fails to compile (in whichever load() takes
    // it up), it never played: the engine goes back to the session it plays,
    // and this returns that failure, once. Main thread.
    std::optional<Error> takeUnplayedEdit() { return std::exchange(unplayedEdit_, std::nullopt); }
    // The session the engine plays or, while an edit is deferred, will play.
    const std::shared_ptr<const model::Session>& latestSession() const { return latest_; }
    // Automation writing (ADR-0061): the volume/pan targets a write pass
    // holds at their static value instead of their envelope. Returns true
    // when releasing a hold needs a load() of the current session to restore
    // the envelope's curve; otherwise `apply` publishes the holds to the
    // running plan (false: a load() follows, and takes them).
    bool setWriteHolds(std::vector<model::EnvTarget> holds, bool apply = true);
    const std::vector<model::EnvTarget>& writeHolds() const { return writeHolds_; }
    // Edits load() applied to the running plan without compiling (ADR-0060).
    std::uint64_t mixEdits() const { return mixEdits_; }  // main thread
    // Main thread: the plans verifyPlans compiled twice and found equal.
    std::uint64_t verifiedPlans() const { return verifiedPlans_; }
    // Buffers compiles took from retired plans, and allocated anew (with
    // recycleBuffers; main thread).
    std::uint64_t buffersReused() const;
    std::uint64_t buffersMade() const;
    // Patch-only FX edits load() applied to the instances without compiling
    // (ADR-0092).
    std::uint64_t patchEdits() const { return patchEdits_; }  // main thread
    void play();
    void pause();  // stops the transport; plugin state is reset (no hanging notes)
    void seek(double seconds);
    // Control thread: source preparation completes off-thread before publishing
    // the audible seek epoch. Structured failures leave the old epoch intact.
    Status prepareSeek(double seconds);
    // The transport loop (ADR-0015): playback reaching `end` continues at
    // `start` (a play position at or past the end plays on). Changing it
    // re-renders what was rendered ahead, like a seek to where the listener is.
    void setLoop(double startSeconds, double endSeconds, bool on);
    LoopRange loop() const { return readLoop(); }
    // The metronome: a click on every beat of the meter while the transport
    // plays, the bar's first accented. Live output only (renders and
    // recordings never hear it); any thread.
    void setMetronome(bool on, double gainDb);
    // play() after a count-in: `frames` of clicks (a beat every `beatFrames`,
    // a bar every `beatsPerBar` beats, the first of each accented) while the
    // transport holds its position — nothing plays or records, monitored
    // inputs are heard as when stopped — then playback starts at the frame the
    // count ends. The clicks sound with the metronome off too (at its level).
    // pause() cancels it. Live output only.
    void playCountIn(std::int64_t frames, std::int64_t beatFrames, int beatsPerBar);
    // File audition (ADR-0094). Main thread: `voice` plays from its first
    // sample at the next cycle, on outputs 1/2 after the project's output,
    // in either executor, stopped or playing; what played fades out over
    // 5 ms. Null or empty stops. Returns the voice's serial (0 for a stop).
    // A stop of the engine drops it; renders, recordings and meters never
    // contain it.
    std::uint64_t audition(std::shared_ptr<const AuditionVoice> voice);
    // Any thread: the voice playing and its frame.
    AuditionState auditionState() const;
    // Main thread (one consumer): the control messages the devices sent since
    // the last call (ADR-0027), oldest first, appended to `out`; at most
    // kControlRing are held between calls (more are counted as dropped).
    void takeControlMidi(std::vector<ControlMidiEvent>& out);
    std::uint64_t controlMidiDrops() const { return controlDrops_.load(); }
    static constexpr std::size_t kControlRing = 1024;
    // The backend's MIDI input ports (ADR-0016); empty when stopped.
    std::vector<std::string> midiPorts() const {
        return backend_ ? backend_->midiPorts() : std::vector<std::string>{};
    }
    TransportState transport() const;
    // Main thread: the master playrate as heard. Stopped, nothing is heard,
    // and a steady scalar is the rate the play starts at (the builder's song
    // journey 2, F8); an envelope or a sync chase reads as last heard.
    double effectivePlayrate() const;

    // --- external sync (ADR-0071) ----------------------------------------------------
    // Main thread. The configuration the callback runs (now if running, else
    // from start()); a change of listens() or chases() recompiles the plan
    // (a sync plan has every node live, and the rate path on with pitch
    // preserved when it chases).
    Status setSync(const SyncConfig& config);
    const SyncConfig& syncConfig() const { return syncConfig_; }
    // What the callback last reported; the default while stopped or off.
    SyncStatus syncStatus() const;
    // The chase scale: the live playrate is the saved rate times it, through
    // the scalar control path (ADR-0046) on a sync plan. Never saved.
    void setSyncRate(double scale);
    double syncRate() const { return syncScale_; }
    // MIDI clock and MTC output (ADR-0073): messages written to, and refused
    // by, each Backend::midiOutputPorts() index since start.
    std::uint64_t syncOutputSent(std::size_t port) const {
        return port < kMaxSyncOutputs ? syncOutSent_[port].load(std::memory_order_relaxed) : 0;
    }
    std::uint64_t syncOutputDropped(std::size_t port) const {
        return port < kMaxSyncOutputs ? syncOutDropped_[port].load(std::memory_order_relaxed) : 0;
    }
    std::vector<std::string> midiOutputPorts() const {
        return backend_ ? backend_->midiOutputPorts() : std::vector<std::string>{};
    }
    std::vector<std::int64_t> midiOutputLatency() const {
        return backend_ ? backend_->midiOutputLatency() : std::vector<std::int64_t>{};
    }
    // Main thread: waits (bounded) until the renderer has acted on every
    // transport request, so transport() reports the new state. With the
    // renderer in the callback: two audio cycles (none for a manual backend).
    void awaitTransport(double timeoutSeconds = 1.0);
    // Anticipative, main thread: waits until `frames` of the current
    // transport epoch are rendered ahead of the playhead (at most the
    // render-ahead amount), or the transport is paused. False on timeout.
    bool awaitRendered(std::int64_t frames, double timeoutSeconds = 10.0);
    // Main thread (tests, tools): waits until the renderer has adopted the
    // newest plan and queued it for the callback, whose next cycle adopts it
    // (anticipative; immediately true otherwise, where the next cycle does).
    bool awaitPlan(double timeoutSeconds = 10.0);
    // Audio cycles that found no rendered audio while playing (after the
    // pre-roll of a start or seek).
    std::uint64_t anticipationMisses() const { return misses_.load(); }
    // Callback cycles in which a bridged plug-in did not answer by the
    // cycle's bridge deadline (ADR-0081): each ran past its period.
    std::uint64_t bridgeLateCycles() const { return bridgeLate_.load(); }
    // Callback cycles with bridged calls that ran past their period, by
    // cause (ADR-0081 diagnostics): a call's wait reached the deadline; the
    // bridged waits took the overrun without reaching it; or the cycle was
    // not waiting on the bridge (its own work, or its thread descheduled).
    struct BridgeOverruns {
        std::uint64_t lateWait = 0, slowBridge = 0, notWaiting = 0;
        std::int64_t longestCycleNs = 0;  // of the cycles with bridged calls
    };
    BridgeOverruns bridgeOverruns() const {
        return {overrunLateWait_.load(), overrunSlowBridge_.load(), overrunNotWaiting_.load(),
                longestBridgedCycle_.load()};
    }
    // Recording (main thread): capture starts with the first played cycle,
    // placed where the performer heard it (device and plan latency).
    // Main thread: masters recording their output (ADR-0057), for the plans
    // the next load() builds. Their record taps stay while they are listed.
    void setRecordingMasters(std::vector<RecordingMaster> masters) {
        recordingMasters_ = std::move(masters);
    }
    Status startRecording(std::vector<RecordTarget> targets, RecordedPunch punch = {});
    Status setRecordingPunch(RecordedPunch punch);  // model thread only, never the callback
    // Also retrieves takes finalized by stop(), once, after the device stopped.
    Result<std::vector<RecordedTake>> stopRecording();
    std::vector<RecordingError> takeRecordingErrors();
    bool recording() const { return recorderOwned_ != nullptr; }
    // Tests: while held, the renderer renders nothing. Wait for its
    // acknowledgement before driving a manual backend into an underrun.
    void holdRenderer(bool hold) {
        holdSeen_.store(false, std::memory_order_release);
        hold_.store(hold, std::memory_order_release);
        wakeRenderer();
    }
    bool awaitRendererHeld(double timeoutSeconds = 10.0);
    const EngineOptions& options() const { return options_; }
    unsigned liveHelpers() const;  // main thread: enabled capacity, 0 when serial
    // Main thread: the threads that work to the device's deadline, the
    // callback's (once it has run, Linux only) and the live helpers'. For
    // counting their page faults (engine.md §2.2).
    std::vector<std::thread::native_handle_type> deadlineThreads();
    // The background threads' real-time priority (ADR-0026): below the
    // kernel's threaded interrupts (50) and the audio threads rtkit grants
    // other applications (up to 20). Any real-time level outruns normal
    // desktop load, and 200 ms of render-ahead needs no more.
    static constexpr int kBackgroundPriority = 10;
    // What each thread class runs with, recorded (and verified) at start; None
    // for a class without threads, and for all while stopped. Main thread.
    struct Scheduling {
        ThreadPolicy callback, live, renderer, background;
    };
    const Scheduling& scheduling() const { return scheduling_; }

    // Main thread, regularly: frees retired plans; once the newest plan is
    // live, destroys instances whose fx left the session.
    void service();
    // Main thread: FIFO per executor; no ordering is implied between executor
    // queues and idle flushes. Metadata identifies the plan and transport epoch.
    PluginOutputs takePluginOutputs();
    // Plug-ins that said their state changed (clap.state mark_dirty) beyond
    // what their parameters hold — a sampler's loaded file — since the last
    // call; serviced on the main thread.
    std::vector<model::Id> takeDirtyStates();
    // Main-thread watermark for deferring anticipative output recording.
    std::int64_t playedStream(std::uint64_t epoch) const;
    std::optional<RackLatencyInfo> rackLatencies(model::Id rack, std::uint64_t version) const;
    std::uint64_t planGeneration() const { return publishedGen_; }  // main thread
    // Moves by two with each patch-only edit (ADR-0092): plug-in outputs that
    // carry another value were produced before it. Main thread.
    std::uint64_t patchSequence() const { return patchSeq_.load(std::memory_order_relaxed); }
    // Main thread (ADR-0086): the last plan load() published has the device
    // callback process a bridged plug-in (one on a live node, or any with
    // the in-callback executor). False when stopped.
    bool bridgedLive() const { return bridgedLive_; }
    std::uint64_t pluginOutputDrops() const { return outputDrops_.load() + idleOutputDrops_; }
    // Modulation events that waited past their grid point for room (a full
    // block or a bridged block's cut, ADR-0096): cumulative, any thread.
    std::uint64_t modulationDeferred() const { return modDeferred_.load(); }
    // Telemetry (ADR-0096, M2), main thread: each bound modulation route's
    // target's last offset in the newest plan (a plug-in's as a fraction of
    // its CLAP range, a mixer target's in dB or percent). An inert route,
    // or one no plan binds yet, is absent.
    std::map<model::Id, double> modulationOffsets() const;

    // --- The clip launcher (ADR-0097, engine.md §1.4.2; launch_runtime.cpp) ---
    // What one track is to do: start a clip, return to the arrangement, stop.
    struct LaunchRequest {
        model::Id track;
        LaunchLogEntry::Action action = LaunchLogEntry::Action::Clip;
        model::Id clip;
        std::optional<model::Id> scene;  // a scene launch's, logged with it
    };
    // Where a launch was placed: one target for all its tracks (a scene's),
    // in the epoch's stream, and the song position there.
    struct LaunchPlaced {
        std::uint64_t epoch = 0;
        std::int64_t stream = 0, anchorSample = 0;
        double anchorBeat = 0;
    };
    // Main thread: queues the requests at the next `quantize` boundary at or
    // after the latest of their tracks' floors (none: the floor; stopped: the
    // play position, applied at the play start). A newer request for a track
    // replaces its pending one. launch.queue when the queue is full,
    // launch.clip when a track's node has no player for the clip.
    Result<LaunchPlaced> launch(std::span<const LaunchRequest> requests,
                                model::LaunchQuantize quantize);
    // Main thread: the launches applied since the engine started, in order
    // of epoch and running frame (service() collects them), and the pending
    // ones.
    const std::vector<LaunchLogEntry>& launchLog() const { return launchLog_; }
    struct LaunchPending {
        LaunchRequest request;
        LaunchPlaced placed;
    };
    std::vector<LaunchPending> launchesPending() const;
    // Main thread: what each track with clips in the running plan plays as
    // heard (an applied launch not yet heard is still queued), and its
    // queued launch. The GUI's telemetry and launch.state read it.
    struct TrackLaunch {
        enum class Plays : std::uint8_t { Arrangement, Clip, Held, Stopped };
        model::Id track;
        Plays plays = Plays::Arrangement;
        model::Id clip;                  // Clip and Held
        std::optional<double> position;  // Clip: in its period, in the clip's unit
        std::optional<LaunchPending> queued;
        // What the plan's player for the clip it plays (else the one it has
        // queued) follows by (ADR-0098 X4), and whether the follow budget
        // forced that; unset when it plays no clip and has none queued.
        enum class Followed : std::uint8_t { None, Key, Chords };
        std::optional<Followed> followed;
        bool followBudget = false;
    };
    std::vector<TrackLaunch> launchState() const;
    // Main thread: what the running plan's player for `clip` on `track`
    // follows by, and whether the follow budget forced that (ADR-0098 X4c);
    // unset when the plan has no such player.
    struct ClipFollowed {
        TrackLaunch::Followed followed = TrackLaunch::Followed::None;
        bool budget = false;
    };
    std::optional<ClipFollowed> clipFollowed(model::Id track, model::Id clip) const;
    // Main thread: the clips the running plan left out of the memory
    // budget (they cannot be launched; launch.memory).
    std::vector<model::Id> clipsSkipped() const;
    static constexpr std::size_t kLaunchSlots = 256;
    // Capturing a performance (ADR-0097 L4, engine.md §1.4.2). A capture
    // runs from the song sample where it began for `frames` of running
    // time, under the transport loop it ran with. `launches` holds first,
    // at running frame 0, what each track that was not following the
    // arrangement played as it began (its own anchor kept), then each
    // launch applied within it, its runningFrame counted from the start.
    struct LaunchCapture {
        bool on = false;  // still capturing: frames grows
        std::int64_t startSample = 0, frames = 0;
        LoopRange loop;
        std::vector<LaunchLogEntry> launches;
    };
    // Main thread: starts a capture at the heard position (stopped: the
    // play position, where playing starts), or ends the running one at the
    // heard position. A relocation (a seek, a stop, a loop change) ends a
    // capture that has run any frames; one that has not starts again where
    // the transport goes. Starting is refused while a master playrate is
    // active (`launch.playrate`; ADR-0097, dated revision 2026-11-21): its
    // running time is not song time. One that becomes active ends a running
    // capture (service).
    Status captureLaunches(bool on);
    // Main thread: the running capture, else the last one; none before the
    // first. Collects the executors' acknowledgements first.
    std::optional<LaunchCapture> launchCapture();
    bool launchCapturing() const { return capture_.on; }  // main thread

    std::uint32_t outputLatency() const;
    void setMidiPreferences(MidiPortPreferences inputs, MidiPortPreferences outputs);
    std::uint64_t midiPortDropped(bool output, std::size_t port) const;
    std::uint32_t midiLeadFrames() const;
    // Owner thread: the shared audio lead (routed notes, clock, SPP and MTC)
    // with one port's offset at a candidate value.
    std::uint32_t midiLeadForOffset(std::size_t port, double offsetMs) const;
    const std::vector<std::string>& warnings() const { return warnings_; }

    // --- meters (P5.3c, engine.md §2.3) --------------------------------------------
    // The bank the GUI's meters read. Created with the backend and kept across
    // plan swaps; null while the engine is stopped (and for offline renders,
    // which do not meter). The audio threads write it; read it from the main
    // thread — see EngineApp::meters().
    std::shared_ptr<MeterBank> meterBank() const { return meters_; }
    // What the audio callback has played, as a meter may report it: the
    // transport epoch, and the frames of that epoch the listener has heard —
    // the frames the callback played, minus the device's output latency. While
    // the transport is stopped nothing is rendered ahead of the listener, so
    // everything already processed counts as heard.
    struct MeterClock {
        std::uint64_t epoch = 0;
        std::int64_t heardStream = 0;
    };
    MeterClock meterClock() const;
    std::uint64_t processErrors() const { return processErrors_.load(); }
    // Audio cycles run since start (the main thread's way to see a request acted on).
    std::uint64_t cycles() const { return cycles_.load(std::memory_order_acquire); }
    const BackendConfig& config() const { return config_; }
    // True when the last stop() stopped the plugins in the backend's final
    // (audio-thread) callback, as the Backend contract promises.
    bool lastStopOnAudioThread() const { return stoppedOnAudioThread_.load(); }

  private:
    friend struct EngineTestAccess;  // defined by the tests alone
    friend struct OutputTestAccess;  // likewise (test_app.cpp)
    friend struct RecordingErrorTestAccess;
    friend struct RateTestAccess;  // likewise (test_module_library.cpp)
    void cycle(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT;
    // cycle() after its bookkeeping: count-in, then the transport's work.
    void cycleTransport(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT;
    // External sync (ADR-0071): the runtime the main thread published, the
    // one the callback adopted, every one the main thread still owns.
    void runSync(const float* const* in, float* const* out, std::uint32_t frames,
                 double timelineBefore, std::uint64_t epochBefore, bool playingBefore) SUPPLE_RT;
    void publishSyncStatus(const SyncStatus& status) SUPPLE_RT;
    Status startSync();  // main thread: a runtime for the running device
    SyncConfig syncConfig_;
    double syncScale_ = 1.0;
    std::vector<std::unique_ptr<detail::SyncRuntime>> syncRuntimes_;
    std::atomic<detail::SyncRuntime*> syncNext_{nullptr}, syncAdopted_{nullptr};
    detail::SyncRuntime* syncRt_ = nullptr;  // audio thread
    std::int64_t syncDevice_ = 0;            // audio thread: device frames since start
    bool syncPlayed_ = false;                // audio thread: the previous cycle played
    std::atomic<std::int64_t> syncOutputLatency_{0}, syncInputLatency_{0}, syncPlanLatency_{0},
        syncAudioLead_{0};
    std::array<std::atomic<std::uint64_t>, kMaxSyncOutputs> syncOutSent_{}, syncOutDropped_{};
    // Engine stop: the main thread asks the callback for the runtime's
    // farewell (Stop to running slaves) and waits for it (ADR-0073 §9).
    std::atomic<bool> syncFarewell_{false}, syncTransportRunning_{false};
    std::atomic<std::uint32_t> syncFarewells_{0};
    void sayFarewell() SUPPLE_RT;  // audio thread: the current runtime's
    static constexpr std::size_t kSyncWords = (sizeof(SyncStatus) + 7) / 8;
    std::array<std::atomic<std::uint64_t>, kSyncWords> syncWords_{};
    std::atomic<std::uint64_t> syncSeq_{0};
    std::uint32_t syncRuntimeIds_ = 0;  // main thread
    // One part of a device cycle (a count-in splits a cycle where it ends):
    // `midiBase` is where the part starts in the cycle's device MIDI.
    void cycleBody(const float* const* in, float* const* out, std::uint32_t frames,
                   std::uint32_t midiBase) SUPPLE_RT;
    // Anticipative: the callback's work — the master ring, or the live nodes.
    void anticipativeCycle(const float* const* in, float* const* out, std::uint32_t frames,
                           std::uint32_t midiBase) SUPPLE_RT;
    // Returns the frames rendered (fewer than asked at a loop wrap).
    // `offset`: where this part starts in the cycle (for the device MIDI).
    // Mixes the metronome into device output: `k` frames at `out[c] + offset`
    // that play timeline frames from `pos`. Audio thread.
    void addClicks(const detail::Plan& p, float* const* out, std::uint32_t offset, std::int64_t pos,
                   std::uint32_t k) SUPPLE_RT;
    // File audition (ADR-0094): requests the main thread publishes, each an
    // immutable voice (or a stop) with a serial; it frees those below the
    // smallest serial the callback still reads (auditionKeep_).
    struct AuditionRequest {
        std::shared_ptr<const AuditionVoice> voice;  // null: a stop
        std::uint64_t serial = 0;
    };
    struct AuditionPlay {
        const AuditionVoice* voice = nullptr;
        std::size_t position = 0;
        std::uint64_t serial = 0;
    };
    static constexpr double kAuditionFadeSeconds = 0.005;
    void mixAudition(float* const* out, std::uint32_t frames) SUPPLE_RT;
    void reclaimAuditions();  // main thread
    void resetAuditions();    // main thread, no callback running
    std::deque<std::unique_ptr<AuditionRequest>> auditionOwned_;  // main thread
    std::uint64_t auditionSerial_ = 0;                            // main thread
    std::atomic<const AuditionRequest*> auditionNext_{nullptr};
    std::atomic<std::uint64_t> auditionKeep_{0}, auditionPlaying_{0}, auditionTaken_{0};
    std::atomic<std::int64_t> auditionPosition_{0};
    // Audio thread: the request last taken, the voice playing and the one
    // fading out (a newer request cuts an older fade).
    const AuditionRequest* auditionSeen_ = nullptr;
    AuditionPlay auditionPlay_, auditionFade_;
    std::uint32_t auditionFadeLeft_ = 0, auditionFadeFrames_ = 0;
    std::atomic<bool> metroOn_{false};
    std::atomic<float> metroGain_{0.5f};
    // The click sounding now (audio thread only).
    std::int64_t clickAge_ = -1, clickLength_ = 0;
    double clickPhase_ = 0, clickStep_ = 0;
    float clickAmp_ = 0, clickEnv_ = 0, clickDecay_ = 0;
    void startClick(bool accent, double rate) SUPPLE_RT;
    float clickSample() SUPPLE_RT;  // the sounding click's next sample (0 when none)
    // Count-in (playCountIn): requested by the main thread under countSeq_,
    // taken by the audio thread at the start of a cycle.
    std::atomic<std::int64_t> countFrames_{0}, countBeat_{1};
    std::atomic<int> countBar_{4};
    std::atomic<std::uint64_t> countSeq_{0};
    std::atomic<bool> counting_{false};
    void countClicks(float* const* out, std::uint32_t k) SUPPLE_RT;
    // The device outputs from `offset` (ADR-0056): silence, or the plan's
    // output masters.
    void silence(float* const* out, std::uint32_t offset, std::uint32_t k) const SUPPLE_RT;
    void writeDevice(const detail::Plan& p, float* const* out, std::uint32_t offset,
                     std::uint32_t k) const SUPPLE_RT;
    // `out` advanced by `by` frames, every device channel.
    std::array<float*, 64> advanced(float* const* out, std::uint32_t by) const SUPPLE_RT;
    // Audio thread: the count in progress, and the play request this cycle
    // acts on (read once, before the count request, so a count-in is never
    // missed by a cycle that already sees its play()).
    std::uint64_t countSeen_ = 0;
    std::int64_t countLeft_ = 0, countPos_ = 0, countBeatA_ = 1;
    int countBarA_ = 4;
    bool want_ = false, countHold_ = false;
    bool wantNow() const SUPPLE_RT { return want_ && !countHold_; }
    std::uint32_t liveRender(const float* const* in, float* const* out, std::uint32_t frames,
                             std::uint32_t offset) SUPPLE_RT;
    // `continuous`: adopted exactly at its handoff while playing, in its
    // epoch, after the renderer carried the nodes it moves into the live
    // region (Plan::handContinuable). Otherwise those nodes start afresh.
    void adoptLive(detail::Plan* next, bool continuous = false) SUPPLE_RT;
    void adoptRendererSources(detail::Plan* next) SUPPLE_RT;
    void publishRendererPlan(detail::Plan& next, std::uint64_t epoch, bool queuedRate) SUPPLE_RT;
    // What a pull played: the timeline sample of its first frame (or
    // kNothingPlayed) and how many frames, from the start of the buffer, came
    // from rendered chunks (the rest is silence: the renderer fell behind).
    struct Pulled {
        std::int64_t first;
        std::uint32_t frames;
        std::int64_t stream;  // the stream position of the first frame played
        bool wrapped;         // stopped at a loop wrap (the rest is for the next pull)
    };
    Pulled pullAhead(float* const* out, std::uint32_t frames) SUPPLE_RT;
    static constexpr std::int64_t kNothingPlayed = INT64_MIN;
    // Bounded transport reads. The audio thread gives up quickly (the writer's
    // window is a handful of atomic stores) and keeps what it has; the
    // renderer — its plugins' audio thread — uses the same bound rather than
    // waiting on the main thread, and relocates on a later pass.
    static constexpr int kAudioTransportTries = 4;
    static constexpr int kRendererTransportTries = 64;
    void rendererLoop(std::uint64_t firstEpoch);
    void scheduleBackground();  // main thread, start(): renderer and pool policy
    std::uint64_t relocate(std::uint64_t previous);  // returns the epoch it relocated for
    std::int64_t playedPosition(std::uint64_t epoch, std::int64_t epochStart) const;
    // The anticipative executors' position in `epoch`, also before the
    // renderer has relocated for it: then what relocate() will take.
    std::int64_t anticipativePosition(std::uint64_t epoch) const;
    // Frames the callback has played in `epoch` (0 before its first).
    // The epoch with its seek target and loop, read under xportSeq_: the
    // audio thread gives up after `tries` (a write in progress) and keeps
    // what it has for another cycle; the renderer relocates with the same
    // bound (relocate()); only the main thread's transportSnapshot() waits
    // the writer out, and only there is that safe.
    struct TransportSnapshot {
        std::uint64_t epoch = 0;
        std::uint64_t seekSeq = 0;
        std::int64_t seekSample = 0;
        LoopRange loop;
    };
    bool readTransport(TransportSnapshot& out, int tries,
                       std::uint64_t* seq = nullptr) const SUPPLE_RT;
    TransportSnapshot transportSnapshot() const;
    LoopRange readLoop() const { return transportSnapshot().loop; }
    template <class F>
    void writeTransport(F&& change);
    // One executor's share of a plan: stops what it owns that `next` lacks,
    // then starts what `next` has once the other executor has let go.
    // `handed`: instances moving to the other executor with their state (no reset).
    void adoptShare(std::vector<host::Instance*>& owned, const std::vector<host::Instance*>* next,
                    const void* self,
                    const std::vector<host::Instance*>* handed = nullptr) SUPPLE_RT;
    void startFree(std::vector<host::Instance*>& owned, const std::vector<host::Instance*>* next,
                   const void* self) SUPPLE_RT;
    static void resetOwned(const std::vector<host::Instance*>& owned, detail::Plan* plan) SUPPLE_RT;
    // Frees the plans no executor can still use; returns the oldest
    // generation an executor may use, as read for that decision.
    std::uint64_t collectPlans();
    Status loadNow(const model::Session& session, const std::filesystem::path& projectDir);
    // The oldest plan generation an executor may still be using.
    std::uint64_t oldestInUse() const;
    // Route E stage 1: the rings of plans older than `oldest` (one reading
    // of oldestInUse()) go to the pool, from which load() takes a ring of
    // the shape it needs.
    void recycleRings(std::uint64_t oldest);
    std::unique_ptr<detail::ChunkRing> takeRing(std::size_t capacity, std::uint32_t frames);
    // `patchSequence`: read before the block was processed.
    void publishPluginOutputs(detail::Plan& plan, bool renderer, std::uint64_t epoch,
                              std::int64_t pos, std::int64_t stream,
                              std::uint64_t patchSequence) SUPPLE_RT;

    void pushControlMidi() SUPPLE_RT;
    std::unique_ptr<SpscRing<ControlMidiEvent, kControlRing>> controlMidi_ =
        std::make_unique<SpscRing<ControlMidiEvent, kControlRing>>();
    std::atomic<std::uint64_t> controlDrops_{0};
    using OutputQueue = SpscRing<PluginOutput, 4096>;
    std::unique_ptr<OutputQueue> rendererOutputs_, callbackOutputs_;
    std::vector<PluginOutput> idleOutputs_;  // bounded main-thread queue
    std::atomic<std::uint64_t> outputDrops_{0}, voiceDrops_{0};
    std::atomic<std::uint64_t> modDeferred_{0};
    std::uint64_t modDeferredWarned_ = 0;  // main thread: what service() has reported
    // FX whose plug-in outputs were dropped, written on the audio thread:
    // slots hold (kind + 1) << 32 | n; more than fit set droppedFxAll_.
    static constexpr std::size_t kDroppedFx = 512;
    std::array<std::atomic<std::uint64_t>, kDroppedFx> droppedFx_{};
    std::atomic<std::uint32_t> droppedFxCount_{0};
    std::atomic<bool> droppedFxAll_{false};
    void noteDroppedFx(model::Id fx) SUPPLE_RT;
    std::uint64_t idleOutputDrops_ = 0;
    bool notificationReload_ = false;
    MidiPortPreferences midiInputsPreferences_, midiOutputsPreferences_;
    bool midiSettingsDirty_ = false;
    std::unique_ptr<detail::HardwareMidiRuntime> hardwareMidi_;
    std::span<const MidiInputEvent> deviceMidi_;
    InstancePool& plugins_;
    MediaCache& media_;
    MainThreadWake* mainWake_ = nullptr;
    void notifyMain() SUPPLE_RT {
        if (mainWake_) mainWake_->notify();
    }
    void wakeOutputConsumed() SUPPLE_RT;
    std::atomic<std::uint64_t> outputWakeSerial_{0}, outputWakeEpoch_{0};
    std::atomic<std::int64_t> outputWakeAt_{INT64_MAX};
    std::uint64_t outputWakeSeen_ = 0;  // callback only
    EngineOptions options_;
    Backend* backend_ = nullptr;
    // What the device granted, set once start() has returned: the main
    // thread's. The audio path reads deviceRate_ instead.
    BackendConfig config_;
    // Plans: published by the main thread, freed once no executor can use
    // them (both acknowledged a newer generation).
    std::vector<std::unique_ptr<detail::Plan>> plans_;
    std::vector<std::unique_ptr<detail::ChunkRing>> ringPool_;  // main thread (recycleRings)
    std::unique_ptr<detail::BufferPool> bufferPool_;  // main thread (collectPlans, prepare)
    std::atomic<detail::Plan*> latestPlan_{nullptr};
    std::atomic<detail::Plan*> handPlan_{nullptr};  // renderer -> callback (ADR-0014)
    std::atomic<std::uint64_t> callbackGen_{0};     // the callback's plan generation
    // Transport requests: coalescing atomics (the latest request wins and none
    // is ever dropped), read by the audio thread at the start of each cycle.
    // The seek target and the loop change only with the epoch, under the
    // sequence lock below.
    std::atomic<bool> wantPlaying_{false};
    std::atomic<std::int64_t> seekSample_{0};
    std::atomic<std::uint64_t> seekSeq_{0};
    std::atomic<std::uint64_t> seekEpoch_{0};  // main thread: the epoch the last seek opened
    std::atomic<bool> shutdownRequested_{false};
    std::uint64_t seekSeen_ = 0;
    // seekSeen_/xportSeen_ are plain, and for that reason the in-callback
    // transport path below runs in one mode only: the audio thread's cycle()
    // when the engine is not anticipative, the renderer via relocate() when it
    // is (cycle() returns before that path whenever ring_ is set). If a future
    // change lets both run in one engine, these two become atomics.
    // Plans carry a generation; the audio thread stores it here after adopt()
    // has stopped the instances the plan dropped. service() prunes only then.
    std::uint64_t publishedGen_ = 0;
    // Tests only (EngineTestAccess): runs inside service() right after the
    // old plans are collected, where an executor may adopt a newer plan.
    void (*afterCollectForTests_)(void*) = nullptr;
    void* afterCollectContext_ = nullptr;
    // Tests only (EngineTestAccess): changes the verifier's second compile
    // (EngineOptions::verifyPlans) before the two are compared.
    void (*secondCompileForTests_)(detail::Plan&) = nullptr;
    // Tests only (EngineTestAccess): runs between the verifier's two
    // compiles, where a plug-in may ask for a restart.
    void (*betweenCompilesForTests_)(void*) = nullptr;
    void* betweenCompilesContext_ = nullptr;
    // Tests only (EngineTestAccess): runs in launch() between publishing
    // the slots and deciding them, as a main thread preempted there.
    void (*launchCommitForTests_)(void*) = nullptr;
    void* launchCommitContext_ = nullptr;
    bool bridgedLive_ = false;  // main thread (bridgedLive())
    std::atomic<std::uint64_t> stoppedGen_{0};
    // The session the engine plays: the newest published plan's, or a mix or
    // scalar edit of it. A playrate-deferred edit sets it before any plan
    // exists; rateDeferredFrom_ then holds the one it replaced, which load()
    // puts back if the deferred edit fails to compile (review 37 F1).
    std::shared_ptr<const model::Session> latest_;
    // The clip launcher's queue (launch_runtime.cpp): one slot per track.
    // `state` is generation << 3 | status, so an executor's compare-and-swap
    // fails if the main thread rewrote the slot meanwhile (a seqlock); the
    // fields are relaxed atomics for the same reason.
    //
    // A launch has several targets (ADR-0097, dated revision 2026-11-20):
    // its track's channel and every rack Notes feed whose source is that
    // track, each switched at the target stream plus its own node's timeline
    // shift, by whichever executor runs that node. Whether it is taken is
    // decided once for all of them:
    // - kPending: published, undecided. An executor that reaches one of its
    //   frames (or is past one) marks it kMissed; the main thread places it
    //   again.
    // - kCommitted: the main thread found, after publishing, every executor's
    //   horizon at or before the target (launchHorizon_), so each will read
    //   the slot before it passes its frames.
    // - kApplied: the first target switched (compare-and-swap from
    //   kCommitted, against kVoided). Every other target follows at its
    //   frame, or, if its executor relocated first, at its next block.
    //   `done` gathers the executor classes finished with it (bit 0 the
    //   renderer, bit 1 the callback); the slot is free once `done` covers
    //   `required`.
    // - kVoided: committed, but its epoch ended before any target switched;
    //   placed again, as kMissed.
    struct LaunchSlot {
        enum : std::uint64_t {
            kFree = 0,
            kPending = 1,
            kApplied = 2,
            kMissed = 3,
            kCommitted = 4,
            kVoided = 5
        };
        std::atomic<std::uint64_t> state{0};
        std::atomic<std::uint64_t> track{0}, clip{0}, epoch{0};
        std::atomic<std::uint64_t> scene{0};  // a scene launch's scene id number; 0: none
        std::atomic<std::uint8_t> action{0}, done{0}, required{0};
        std::atomic<std::int64_t> target{0}, anchorSample{0};
        std::atomic<double> anchorBeat{0};
        // Main thread only: what to re-place after a miss or a new epoch.
        LaunchRequest request;
        model::LaunchQuantize quantize = model::LaunchQuantize::Bar;
    };
    struct LaunchAck {
        std::uint64_t track = 0, clip = 0, epoch = 0, scene = 0;
        std::uint8_t action = 0;
        std::int64_t stream = 0, anchorSample = 0;
        double anchorBeat = 0;
    };
    std::unique_ptr<std::array<LaunchSlot, kLaunchSlots>> launchSlots_ =
        std::make_unique<std::array<LaunchSlot, kLaunchSlots>>();
    // Slots the executors still act on (pending, committed, or applied and
    // not yet done): they skip the queue at 0.
    std::atomic<std::uint32_t> launchesQueued_{0};
    // Each executor class's horizon (0 the renderer, 1 the callback): the
    // epoch and the stream its current block may reach, stored before it
    // reads the queue (sequentially consistent, against the main thread's
    // publish then read). Written by that executor only.
    struct LaunchHorizon {
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::int64_t> end{0};
    };
    std::array<LaunchHorizon, 2> launchHorizon_{};
    // Executor-private, by class: the epoch and stream of its last
    // launchesAt, so that a relocated executor knows which of its targets
    // of an applied launch it switched before leaving the epoch.
    std::array<std::uint64_t, 2> launchLastEpoch_{};
    std::array<std::int64_t, 2> launchLastStream_{};
    std::array<bool, 2> launchLastValid_{};
    std::unique_ptr<SpscRing<LaunchAck, 1024>> rendererLaunchAcks_ =
        std::make_unique<SpscRing<LaunchAck, 1024>>();
    std::unique_ptr<SpscRing<LaunchAck, 1024>> callbackLaunchAcks_ =
        std::make_unique<SpscRing<LaunchAck, 1024>>();
    std::vector<LaunchLogEntry> launchLog_;
    // Main thread: by track, the log's length when a plan without its clip
    // returned it to the arrangement (carryLaunch); older entries no longer
    // say what it plays.
    std::map<model::Id, std::size_t> launchReset_;
    std::uint64_t launchCheckedGen_ = UINT64_MAX;  // the plan generation last checked
    // The rate controls of the plan last checked (serviceLaunches): a
    // change voids the launches placed on the old trajectory.
    struct LaunchRateControls {
        bool enabled = false, sync = false;
        double scalar = 1;
        std::vector<double> points;  // sample, value pairs
        bool operator==(const LaunchRateControls&) const = default;
    };
    LaunchRateControls launchRateControls_;
    // Executors: applies the launches due at `stream` to this class's nodes
    // of `p`, marks those already past as missed, and returns the frames
    // until the next one (at most `k`).
    std::uint32_t launchesAt(detail::Plan& p, int liveClass, std::uint64_t epoch,
                             std::int64_t stream, std::uint32_t k, bool renderer) SUPPLE_RT;
    // Executors: `mine` (classes) are finished with an applied slot.
    void launchDone(LaunchSlot& slot, std::uint8_t mine) SUPPLE_RT;
    // Main thread: whether every executor running one of `track`'s launch
    // targets in `plan` has a horizon at or before `target` in `epoch`
    // (read after the slot is published).
    bool launchHorizonsAllow(const detail::Plan& plan, model::Id track, std::uint64_t epoch,
                             std::int64_t target) const;
    // Main thread (service): collects acks, re-places missed launches and
    // those of an old epoch.
    void serviceLaunches();
    // Main thread: places again the missed launches and those of an ended
    // epoch (from serviceLaunches, and from play()).
    void placeLaunchesAgain();
    // Main thread, inside play() only: the epoch its seek to the play
    // position begins, and that position. While set, a stopped launch
    // clock reads the start of that epoch, so a launch placed while
    // stopped still starts at the play start (the builder's song journey
    // 2, D1).
    std::optional<std::pair<std::uint64_t, std::int64_t>> playStart_;
    // Main thread, from play() and playCountIn(): under rate scheduling,
    // the seek to the play position, unless transport.stop prepared it and
    // it is not stale, then the stopped launches placed again (D1). False:
    // the seek failed (a warning says why).
    bool seekToPlayStart();
    // Main thread: a scalar playrate edit while stopped made the seek
    // transport.stop prepared stale (it holds the old rate: D2).
    bool preparedSeekStale_ = false;
    struct LaunchClock;
    bool launchClock(LaunchClock& out) const;
    // Main thread: the capture (captureLaunches). `pending`: its start is
    // not placed yet (relocating, or a relocation before it ran).
    struct CaptureState {
        bool on = false, any = false, pending = false;
        std::uint64_t epoch = 0;
        std::int64_t startFrame = 0, endFrame = 0, startSample = 0;
        LoopRange loop;
    };
    CaptureState capture_;
    void placeCapture();                // a pending start, once the clock reads
    void captureRelocating();           // writeTransport: a relocation is coming
    std::int64_t captureHeard() const;  // the capture epoch's heard stream
    Result<LaunchPlaced> placeLaunch(std::span<const LaunchRequest> requests,
                                     model::LaunchQuantize quantize) const;
    // launch()'s one attempt: placed, published and decided; `missed` when
    // every slot it published was missed.
    Result<LaunchPlaced> publishLaunch(std::span<const LaunchRequest> requests,
                                       model::LaunchQuantize quantize, bool& missed);
    // placeLaunch on a playrate plan while playing (S3): the first stream
    // frame at or after `floor` whose song position, on a copy of the
    // running rate timeline, reaches the quantize boundary.
    Result<LaunchPlaced> placeRateLaunch(const detail::Plan& plan, std::uint64_t epoch,
                                         std::int64_t floor, model::LaunchQuantize quantize) const;
    // Main thread: voids the pending launches of the current epoch whose
    // target lies at or past device frame `device` of the running rate
    // timeline (a control edit from there), for service to place again.
    void voidRateLaunches(std::int64_t device);
    // Main thread: whether the newest plan plays at a master playrate other
    // than a steady 1 (a rate, an envelope or a sync chase).
    bool rateActive() const;
    std::optional<std::shared_ptr<const model::Session>> rateDeferredFrom_;
    // Whether a refused playrate move was pending when the deferral began:
    // the restore puts it back with rateDeferredFrom_ (review 37c F1).
    bool rateDeferredScalarPending_ = false;
    std::optional<Error> unplayedEdit_;  // takeUnplayedEdit()
    // The session the newest published plan was compiled from: what the
    // prune keeps, whatever latest_ names.
    std::shared_ptr<const model::Session> compiled_;
    bool prunePending_ = false;
    std::uint32_t outputLatency_ = 0;
    std::vector<std::string> warnings_;
    std::vector<model::Id> dirtyStates_;
    // The meter bank (P5.3c): created by start(), kept across plan swaps, and
    // written by the renderer and the callback.
    std::shared_ptr<MeterBank> meters_;

    // Audio-thread state. The previous plan is handed back to the main thread
    // the moment a new one is acquired, so the audio thread keeps its own list
    // of the instances it is processing (preallocated; never grows there).
    static constexpr std::size_t kMaxInstances = 4096;
    detail::Plan* plan_ = nullptr;
    std::vector<host::Instance*> processing_;
    bool playing_ = false;
    std::int64_t pos_ = 0;
    // Shared with the main thread.
    std::atomic<std::int64_t> reportedPos_{0};
    std::atomic<bool> reportedPlaying_{false};
    std::atomic<bool> shutDown_{false};
    std::atomic<bool> stoppedOnAudioThread_{false};
    std::atomic<std::uint64_t> processErrors_{0};
    std::atomic<std::uint64_t> cycles_{0};

    // Anticipation (ADR-0012). The renderer thread owns plan_, processing_,
    // playing_ and pos_ (the render position) instead of the audio thread.
    // Seek and pause bump epoch_; the callback drops chunks of older epochs.
    std::unique_ptr<detail::ChunkRing> ring_;
    // Rendered plans cannot be coalesced: callback-owned sources must consume
    // every clock trajectory already represented in frontier/master audio.
    SpscRing<detail::Plan*, 1024> planHandoffs_;
    // Control producer -> renderer consumer. Converter preparation is complete
    // before publication; the renderer preserves these scheduled trajectories.
    SpscRing<detail::Plan*, 1024> ratePlanRequests_;
    std::unique_ptr<detail::Workers> workers_;
    std::unique_ptr<detail::LiveWorkers> liveWorkers_;
    std::atomic<detail::LiveWorkers*> liveWorkersReady_{nullptr};
    std::vector<std::string> startWarnings_;  // scheduling fallbacks, kept with plan warnings
    Scheduling scheduling_;
    std::thread renderer_;
    void wakeRenderer() SUPPLE_RT;
    void wakeRendererConsumed() SUPPLE_RT;
    std::int64_t rendererSpaceTarget(std::uint64_t epoch) const;
    bool rendererHasRoom(std::uint64_t epoch) const;
    std::chrono::steady_clock::duration rendererSafetyTimeout() const;
    static std::chrono::steady_clock::time_point stoppedRenderDeadline(
        std::chrono::steady_clock::time_point next, std::chrono::steady_clock::time_point last,
        bool requested);
    static std::chrono::steady_clock::time_point rendererDeadline(
        std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point idleNext,
        bool idleWork, std::chrono::steady_clock::duration safetyTimeout);
    std::atomic<std::uint32_t> rendererEvent_{0};
    // One-shot stream threshold, or INT64_MAX while not waiting for space.
    std::atomic<std::int64_t> rendererWakeAt_{INT64_MAX};
    std::atomic<bool> rendererQuit_{false}, hold_{false}, holdSeen_{false};
    std::int64_t aheadFrames_ = 0;                 // render-ahead target
    std::atomic<std::uint64_t> epoch_{0};          // main thread: seek, pause, setLoop
    std::atomic<std::uint64_t> epochDone_{0};      // renderer: the epoch it has relocated for
    std::atomic<std::int64_t> epochStart_{0};      // ... and where that epoch starts
    std::atomic<std::int64_t> renderedStream_{0};  // renderer: frames rendered in the epoch
    std::int64_t epochStartLocal_ = 0;             // renderer
    std::int64_t streamLocal_ = 0;                 // renderer: frames rendered in the epoch
    LoopRange loopR_;                              // renderer: the loop of its epoch
    std::uint64_t rtEpoch_ = 0;                    // audio thread
    std::uint32_t rtOffset_ = 0;                   // audio thread: into the front chunk
    bool rtPrimed_ = false;                        // audio thread: played a chunk of rtEpoch_
    std::atomic<std::uint64_t> rtEpochSeen_{0};
    std::atomic<std::int64_t> rtPos_{0};        // after the last frame played...
    std::atomic<double> effectivePlayrate_{1};  // consumed output, never render lookahead
    std::atomic<double> rtTimeline_{0};
    std::atomic<std::uint64_t> timelineEpoch_{0};
    std::atomic<bool> rateScheduling_{false};
    std::optional<double> rateSeekTarget_;
    std::optional<LoopRange> rateLoopTarget_;
    detail::Plan* ratePreparedTail_ = nullptr;  // control thread only
    std::array<std::atomic<std::uint64_t>, 13> rateClockWords_{};
    std::atomic<std::uint64_t> rateClockSequence_{0};
    std::atomic<std::int64_t> rateSafeStream_{0};
    std::atomic<std::uint64_t> rateSafeEpoch_{0};
    // Renderer asks the control thread to prepare a fresh seek for an edit it
    // cannot adopt continuously. Keep playing the old plan; never spin/retry.
    std::atomic<std::uint64_t> rateReseekGeneration_{0};
    std::uint64_t rateReseekServiced_ = 0;  // control thread only
    bool rateDeferred_ = false;
    bool scalarPending_ = false;
    // Control thread: audio played at the latest scalar playrate move.
    std::int64_t scalarHeard_ = 0;
    std::uint64_t scalarHeardEpoch_ = UINT64_MAX;
    std::atomic<bool> rateDeferredWake_{false};
    std::filesystem::path rateDeferredDir_;
    std::uint64_t clickSeekSeen_ = 0;
    double rateEpochStart_ = 0;  // callback-only, unity trajectory before activation
    void publishRateClock(detail::Plan& plan, std::int64_t device, std::int64_t stream,
                          std::uint64_t epoch, bool live) SUPPLE_RT;
    std::array<std::uint64_t, 13> readRateClock() const;
    Status prepareRatePlan(detail::Plan& plan);
    bool loadScalarRate(const model::Session& session, const std::filesystem::path& projectDir);
    // ADR-0095 (main thread): an edit of only the context points compiles nothing.
    bool loadContextOnly(const model::Session& session, const std::filesystem::path& projectDir);
    // ADR-0060/0061 (main thread): the mix-only edit path of load(), the
    // write holds, and the held targets whose compiled curves are stale.
    bool loadMixValues(const model::Session& session, const std::filesystem::path& projectDir);
    std::vector<model::EnvTarget> writeHolds_, staleCurves_;
    std::uint64_t mixEdits_ = 0;
    std::uint64_t verifiedPlans_ = 0;
    // ADR-0093: the stream position, in this epoch, by which the renderer has
    // taken the last published edit it reads (loadMixValues).
    std::uint64_t renderedAheadEpoch_ = 0;
    std::int64_t renderedAheadUntil_ = 0;
    // ADR-0092 (main thread): the patch-only edit path of load().
    bool loadPatches(const model::Session& session, const std::filesystem::path& projectDir);
    std::uint64_t patchEdits_ = 0;
    // Moved before and after loadPatches applies patches (odd in between);
    // executors read it before each block they process and stamp that
    // block's plug-in outputs with it. The app drops any other stamp.
    std::atomic<std::uint64_t> patchSeq_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);  // read on the audio path
    bool holdsPending_ = false;       // writeHolds_ not yet in the newest plan
    bool releasesStaleCurve() const;  // a load must compile
    void applyPendingHolds();         // after a shortcut that publishes no plan
    double ratePreparationSeconds_ = 0;

    std::atomic<std::int64_t> rtStream_{0};     // (frames played in the epoch)
    std::atomic<std::uint64_t> rtPosEpoch_{0};  // ...in this epoch
    LoopRange loopC_;                           // audio thread: the loop it plays
    std::uint64_t xportSeen_ = ~0ull;           // as seekSeen_: one executor at a time
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> bridgeLate_{0};
    std::atomic<std::uint64_t> overrunLateWait_{0}, overrunSlowBridge_{0}, overrunNotWaiting_{0};
    std::atomic<std::int64_t> longestBridgedCycle_{0};

    // The callback's share when anticipative: its plan and the live instances
    // it owns, and where it plays live nodes.
    std::uint32_t deviceInputs_ = 0;   // written before the backend starts
    std::uint32_t deviceOutputs_ = 2;  // the callback's out channels (at most 64), likewise
    // The device's rate for the audio path. Atomic, unlike the copies above:
    // the requested rate is stored before the backend starts, and the granted
    // one only once start() returns, while callbacks may already run. Nothing
    // that depends on the rate (a plan, a sync runtime, a count-in) runs
    // before the granted rate is stored.
    std::atomic<double> deviceRate_{48000.0};
    Backend* midiSource_ = nullptr;  // the backend, for the callback: set before it starts
    detail::Plan* livePlan_ = nullptr;
    std::vector<host::Instance*> liveProcessing_;
    std::int64_t livePos_ = 0;
    std::int64_t liveStream_ = 0;  // frames the live nodes played in rtEpoch_
    bool liveReady_ = false;       // livePos_ is valid for rtEpoch_
    bool liveEntered_ = false;     // the next placement continues the renderer's nodes

    // Epoch bumps (seek, pause, setLoop) with the seek target and the loop,
    // written by the main thread alone under one sequence lock (odd while
    // writing), so an executor starting an epoch gets exactly its seek and
    // loop (ADR-0015).
    std::atomic<std::uint64_t> xportSeq_{0};
    std::atomic<std::int64_t> loopStart_{0}, loopEnd_{0};
    std::atomic<bool> loopOn_{false};

    // Recording: the callback captures while recorder_ is set. cycleSeq_ is
    // odd while a callback runs (seq_cst with recorder_, so stopRecording
    // knows when no callback can still hold the recorder).
    std::unique_ptr<detail::Recorder> recorderOwned_;
    std::vector<RecordedTake> stoppedTakes_;         // main thread; retained until stopRecording()
    std::vector<RecordingMaster> recordingMasters_;  // main thread (ADR-0057)
    std::atomic<detail::Recorder*> recorder_{nullptr};
    std::atomic<std::uint64_t> cycleSeq_{0};
    // The thread the last cycle ran on (Linux), for deadlineThreads().
    std::atomic<std::thread::native_handle_type> callbackThread_{};
};

}  // namespace supple::engine
