// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// CLAP and VST3 hosting (docs/specs/sandbox.md §1, ADR-0017). Shared by the
// sandbox and engine.
//
// Threading follows CLAP: an Instance is created, configured, activated,
// saved/loaded and destroyed on the thread that created it (its main
// thread); process() and start/stopProcessing() run on one audio thread.
// The host callbacks a plugin makes are answered according to that split;
// audio-thread callbacks never allocate or lock.
#pragma once

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "supple/base/native_window.h"
#include "supple/base/result.h"
#include "supple/base/rt.h"
#include "supple/base/spsc.h"
#include "supple/base/thread.h"
#include "supple/clap/editor_picture.h"
#include "supple/clap/plugin_status.h"
#include "supple/clap/semantic_params.h"
#include "supple/clap/trace.h"
#include "supple/host/editor_frame.h"
#include "supple/json/value.h"

namespace supple {
class MainThreadWake;
}

namespace supple::host {

// Whether a plug-in is told the display scale (CLAP set_scale, VST3 content
// scale) for an editor parent of this kind. Cocoa editors use points and observe
// the backing scale themselves, so no factor is ever sent to them.
inline constexpr bool editorTakesScale(NativeWindow::Kind kind) {
    return kind == NativeWindow::Kind::X11 || kind == NativeWindow::Kind::Win32;
}

struct PluginInfo {
    std::string id, name, vendor, version, description;
    // CLAP by default; VST3 class ids use vst3:<32 hex digits>.
    std::string format = "clap";
    std::filesystem::path path;
    std::vector<std::string> features;
    bool instrument = false;  // declares the "instrument" feature
    bool semantic = false;    // implements com.suppleaudio.semantic-params/1
};

// Scans files or directories (recursively) for .clap binaries and .vst3 bundles.
// Third-party modules run in one supervised bridge child per module (ADR-0013).
// Successful and failed scans are cached until the module changes.
Result<std::vector<PluginInfo>> scan(std::span<const std::filesystem::path> dirsOrFiles);
struct ScanOptions {
    int timeoutMs = 5000;
    std::size_t maxOutputBytes = 1u << 20;
    // Empty uses XDG_CACHE_HOME (or HOME/.cache), with the product slug.
    std::filesystem::path cacheDirectory;
    // Empty discovers the configured/build/installed bridge.
    std::filesystem::path bridgeExecutable;
    // Test-only: bypass supervision and the cache, like trusted stock modules.
    bool forceInProcess = false;
    // Supervise every module, stock ones too, and refuse when the scanner is
    // missing: a scan off the main thread must not load a plug-in in process.
    bool superviseAll = false;
};
struct ScanFailure {
    std::filesystem::path module;
    Error reason;
    bool cached = false;
};
struct ScanReport {
    std::vector<ScanFailure> failures;
    std::vector<Error> warnings;
    std::size_t cachedModules = 0;
};
Result<std::vector<PluginInfo>> scan(std::span<const std::filesystem::path> dirsOrFiles,
                                     const ScanOptions& options, ScanReport* report = nullptr);
// Report for the last scan on this thread, including the compatible one-argument API.
// Catalogue owners should retain it alongside the returned available plugins.
const ScanReport& lastScanReport();
// CLAP and VST3 standard search paths, plus CLAP_PATH/VST3_PATH (existing only).
std::vector<std::filesystem::path> defaultSearchPath();
// The stock plugins that ship with the running binary, when the folder exists:
// installed, plugins/clap beside it (Windows), ../PlugIns/CLAP (macOS bundle) or
// ../lib/<slug>/plugins (Linux); in a build tree, build/<preset>/plugins/clap,
// two folders above daw/gui/ and host/engine/. The apps search it before
// defaultSearchPath(), so an older stock copy in ~/.clap never wins.
std::optional<std::filesystem::path> stockPluginDir();
// The executable image file of a scanned CLAP: its path, or for a macOS bundle the
// executable inside it. Empty for a VST3 (a folder bundle whose reports carry no build
// id) and when a bundle names none. Read-only; for comparing a build id with a crash
// report's (topic 7d).
std::filesystem::path pluginImageFile(const PluginInfo& info);
// The agent skill (the folder holding SKILL.md) that ships with the running
// binary, found as stockPluginDir() is: skill beside it (Windows),
// ../Resources/skill (macOS bundle), ../share/<slug>/skill (Linux), or
// build/<preset>/skill, the build's copy of the repository's skill/.
std::optional<std::filesystem::path> skillDir();
// Orders a scan result as a catalogue: by id, and within one id in search-path
// order, so a lookup by id takes the first location that has it (an explicit
// --plugin-path before ~/.clap). `shadowed` marks the later copies.
void orderCatalogue(std::vector<PluginInfo>& list);
bool shadowed(std::span<const PluginInfo> catalogue, std::size_t index);

struct HostIdentity {
    std::string name = "supple-host";  // callers pass the product name
    std::string vendor = "Supple Audio";
    std::string url;
    std::string version = "0.1.0";
};

struct ParamInfo {
    std::uint32_t id;
    std::string name, module;
    double min, max, def;
    std::uint32_t flags;  // CLAP_PARAM_IS_*; VST3 flags map to the same bits
};

// --- events ----------------------------------------------------------------------

// ParamMod: CLAP param_mod, `value` the modulation amount added to the
// parameter in its CLAP value units (a VST3 plug-in ignores it).
struct Event {
    enum class Kind : std::uint8_t { NoteOn, NoteOff, NoteChoke, Param, Midi, ParamMod };
    Kind kind = Kind::NoteOn;
    std::uint32_t time = 0;  // sample offset within the block
    // notes
    std::int16_t port = 0, channel = 0, key = 60;
    std::int32_t noteId = -1;
    double velocity = 1.0;  // 0..1
    // params
    std::uint32_t paramId = 0;
    double value = 0.0;  // CLAP value or VST3 normalized value
    // MIDI 1.0
    std::uint8_t midi[3] = {0, 0, 0};
};

struct Transport {
    bool playing = true;
    double tempo = 120.0;  // BPM
    double beat = 0.0;     // song position in quarter-note beats at block start
    double barStartBeat = 0.0;
    std::int32_t bar = 0;  // 0-based bar index
    std::uint16_t numerator = 4, denominator = 4;
    double seconds = 0.0;     // song position in seconds at block start
    bool loopActive = false;  // the transport loop (CLAP_TRANSPORT_IS_LOOP_ACTIVE)
    double loopStartBeat = 0.0, loopEndBeat = 0.0, loopStartSeconds = 0.0, loopEndSeconds = 0.0;
};

// An audio port past the main one: a multi-out instrument's "Kick" or
// "Out 3-4" (ADR-0021).
struct AudioPortInfo {
    std::string name;
    std::uint32_t channels = 0;
};

struct ProcessBlock {
    const float* const* inputs = nullptr;  // main input channels (may be null for instruments)
    std::uint32_t inputChannels = 0;
    float* const* outputs = nullptr;  // main output channels
    std::uint32_t outputChannels = 0;
    // The key (ADR-0018): the plug-in's sidechain input; null = silence.
    const float* const* sidechain = nullptr;
    std::uint32_t sidechainChannels = 0;
    // The outputs past the main port (ADR-0021): every auxiliary output
    // port's channels in declaration order. Null, or fewer channels than the
    // plug-in has: the rest go to scratch.
    float* const* auxOutputs = nullptr;
    std::uint32_t auxOutputChannels = 0;
    std::uint32_t frames = 0;
    std::span<const Event> events;         // sorted by time
    const Transport* transport = nullptr;  // null: no transport info
    std::int64_t steadyTime = -1;          // sample counter, or -1
};

// Events the plugin emits in process() or params.flush(). Flags retain CLAP's
// event flags; DontRecord forbids automation recording and IsLive identifies
// a live user event. NoteEnd uses the input-note (port, channel, key, noteId)
// address, including -1 wildcards. ParamValue retains its target address too.
// VST3 parameter outputs are normalized, have zero flags and no note address.
//
// Note and MIDI output (ADR-0068; process() only, never params.flush):
// NoteOn, NoteOff and NoteChoke carry the plug-in's output note address
// (port: its output note port or event bus; channel 0..15 and key 0..127, or
// -1 wildcards on an off or a choke) and noteId; on and off carry the velocity
// 0..1 in `value`. Midi carries one MIDI 1.0 short message in `midi` (status
// first, unused data bytes zero) on `port`: CC, pressure, pitch bend, program
// change, a note given as MIDI, system common and real-time. Their times are
// within the block (0..frames-1). midiOutput() tells these four kinds apart.
struct OutputEvent {
    enum class Kind : std::uint8_t {
        ParamValue,
        GestureBegin,
        GestureEnd,
        NoteEnd,
        NoteOn,
        NoteOff,
        NoteChoke,
        Midi
    };
    static constexpr std::uint32_t IsLive = CLAP_EVENT_IS_LIVE, DontRecord = CLAP_EVENT_DONT_RECORD;
    Kind kind = Kind::ParamValue;
    std::uint8_t midi[3] = {0, 0, 0};  // Kind::Midi
    std::uint32_t time = 0;
    std::uint32_t paramId = CLAP_INVALID_ID;
    double value = 0;  // a parameter value, or a note's velocity
    std::int32_t noteId = -1;
    std::uint32_t flags = 0;
    std::int16_t port = -1, channel = -1, key = -1;
    bool midiOutput() const { return kind >= Kind::NoteOn; }
};

// Caller-owned, preallocated sink for output events (the audio thread never
// allocates): events beyond storage.size() are dropped and counted. Each
// process() call sorts the events it appended by time, stably (same-time
// events keep the order the plug-in emitted them in). `unsupported` counts
// note/MIDI output with no OutputEvent form: SysEx, MIDI 2.0 messages and
// note expressions without a MIDI 1.0 equivalent, malformed notes and MIDI.
// `lost`: the block's output never arrived and never will (a bridged block
// that missed its live deadline or was skipped behind one, or a failed
// bridge): process() returned silence and no events, whatever the plug-in
// made of it (ADR-0088).
struct OutputEvents {
    std::span<OutputEvent> storage;
    std::size_t count = 0;
    std::size_t dropped = 0;
    std::size_t unsupported = 0;
    bool lost = false;
    void clear() {
        count = dropped = unsupported = 0;
        lost = false;
    }
};

class Library;         // a loaded .clap binary (shared by its instances)
class RemoteInstance;  // an instance in a bridge process
class BridgeProcess;   // owns a bridge process and its control socket
class Vst3Instance;    // local VST3 backing

// The bridge executable for isolated instances (process-wide, set at startup).
void setBridgeExecutable(std::filesystem::path path);
// The one set (empty when none is).
std::filesystem::path bridgeExecutable();
// Finds the bridge named `name` (<slug>-bridge) next to the running
// executable (installed) or in the build tree's host/clap-host directory, and
// sets it. False when there is none.
bool locateBridgeExecutable(std::string_view name);
Result<std::shared_ptr<BridgeProcess>> createBridgeProcess();
// The scheduling a bridged instance's audio thread takes when its processing
// starts (process-wide; Normal by default). The engine sets its background
// policy here while it runs, so a renderer that waits for a bridged plug-in
// does not wait behind a lower-priority thread (ADR-0026).
void setBridgeAudioPolicy(ThreadPolicy policy);
ThreadPolicy bridgeAudioPolicy() SUPPLE_RT;
// The failure deadline of a bridged call made off the device callback
// (ADR-0013): `process` waits this long, lifecycle commands 5/2 of it plus
// 2 s; a bridge that has not answered by then is killed and its instances
// fail (silent). Process-wide, for instances without their own
// (Instance::setCallDeadlineMs); 2000 ms by default, at least 1. A test that
// compares bridged renders bit for bit raises it, so a stalled machine
// fails no healthy plug-in (see bridge_patience.cpp in the engine tests).
void setBridgeDeadlineMs(int ms);
int bridgeDeadlineMs() SUPPLE_RT;
// The class a bridged instance's audio thread takes while it runs under a
// live deadline (ADR-0081): the device callback's (process-wide; Normal by
// default). On Linux this policy and priority; macOS and Windows apply their
// own audio class in the bridge (time-constraint, MMCSS Pro Audio) whatever
// it says. The engine sets it while it runs, with the device period (frames
// at the device's rate), which sizes the macOS time constraint (0: the
// command's block). Commands carry it as a duration, so an oversampled
// plug-in, running at another rate, asks for the device's period.
void setLiveBridgeAudioPolicy(ThreadPolicy policy, std::uint32_t periodFrames = 0,
                              double sampleRate = 0);
ThreadPolicy liveBridgeAudioPolicy() SUPPLE_RT;
std::uint32_t liveBridgePeriodNs() SUPPLE_RT;  // 0: none
bool bridgeProcessFailed(const std::shared_ptr<BridgeProcess>& process);

// ADR-0081: the device callback's bridge deadline, on the steady clock in
// nanoseconds (steadyNanoseconds()). While one is set on the calling thread,
// a bridged instance's audio-thread calls wait for its answer only until
// then, and never check, kill or reap its process: a stage that has not
// answered is silent for the block (process() returns true) and its request
// stays outstanding, answered before the shared memory is used again. After
// 50 ms of consecutive late blocks the instance fails, and the main thread
// kills its process when it next services it. Unset (0), the background
// deadline applies (ADR-0013). Each callback-owned thread sets it per cycle;
// 0 sets none (a live helper serving a cycle without one).
class LiveBridgeDeadline {
  public:
    explicit LiveBridgeDeadline(std::int64_t steadyNs) SUPPLE_RT;
    ~LiveBridgeDeadline() SUPPLE_RT;
    LiveBridgeDeadline(const LiveBridgeDeadline&) = delete;
    LiveBridgeDeadline& operator=(const LiveBridgeDeadline&) = delete;

  private:
    std::int64_t previous_;
};
// ADR-0085: a live pool's view of its threads' bridged waits. While one is
// set on the calling thread (LiveBridgeWaitScope), a bridged call under a
// live deadline that has not been answered within its spin, and so blocks,
// counts itself in `blocked` for as long as it blocks, then calls
// ended(context). The pool's idle helpers park only while `blocked` is
// nonzero, and ended() wakes them.
struct LiveBridgeWaiters {
    std::atomic<std::uint32_t> blocked{0};
    void (*ended)(void* context) = nullptr;  // real-time; may be null
    void* context = nullptr;
};
class LiveBridgeWaitScope {
  public:
    explicit LiveBridgeWaitScope(LiveBridgeWaiters* waiters) SUPPLE_RT;
    ~LiveBridgeWaitScope() SUPPLE_RT;
    LiveBridgeWaitScope(const LiveBridgeWaitScope&) = delete;
    LiveBridgeWaitScope& operator=(const LiveBridgeWaitScope&) = delete;

  private:
    LiveBridgeWaiters* previous_;
};
// One blocking wait, as a bridged live call counts it: in the calling
// thread's waiters (if any) from construction, out at destruction, which
// then calls their ended(). seq_cst, with a fence before ended().
class LiveBridgeBlocked {
  public:
    LiveBridgeBlocked() SUPPLE_RT;
    ~LiveBridgeBlocked() SUPPLE_RT;
    LiveBridgeBlocked(const LiveBridgeBlocked&) = delete;
    LiveBridgeBlocked& operator=(const LiveBridgeBlocked&) = delete;

  private:
    LiveBridgeWaiters* waiters_;
};
std::int64_t steadyNanoseconds() SUPPLE_RT;
// The calling thread's live deadline, 0 for none.
std::int64_t currentLiveBridgeDeadline() SUPPLE_RT;
// Bridged audio-thread calls that missed a live deadline, process-wide.
std::uint64_t bridgeLateCalls() SUPPLE_RT;

// How bridged calls spent their time (ADR-0081 diagnostics), process-wide
// since start or resetBridgeTiming(): calls under a live deadline and the
// rest apart. A call's wait runs from posting it to its answer (or giving
// up); its wake, from posting to the bridge's thread seeing it; its work,
// that thread's run of it; its return, from the bridge finishing to the
// host seeing the answer. Times in nanoseconds on the steady clock. `calls`
// and `late` count posted calls; wake, work and return are over those
// answered.
struct BridgeTiming {
    struct Calls {
        std::uint64_t calls = 0;
        std::uint64_t late = 0;          // live: not answered by the deadline
        std::uint64_t pastDeadline = 0;  // live: posted once the deadline had passed
        std::uint64_t spun = 0;          // answered while the host still spun, before a wait
        // Waits for the answer to an earlier late block before the next
        // call (its wait counts in the totals), and those that missed again.
        std::uint64_t settles = 0, lateSettles = 0;
        std::int64_t waitMaxNs = 0, waitSumNs = 0;
        std::int64_t wakeMaxNs = 0, wakeSumNs = 0;
        std::int64_t workMaxNs = 0, workSumNs = 0;
        std::int64_t returnMaxNs = 0, returnSumNs = 0;
        std::uint64_t wakesOver200us = 0, wakesOver1ms = 0;
        std::uint64_t bridgeCpus = 0;       // bit c: a bridge thread ran a call on CPU c
        std::uint64_t classMismatches = 0;  // Linux: answers from a thread not in the class asked
        std::uint32_t migrations = 0;       // the most CPU changes one bridge thread reported
        // How many answered calls' wake and return fell under each bound of
        // kTimingBucketsUs (the last: at or over 2 ms).
        std::array<std::uint64_t, 9> wakeBuckets{}, returnBuckets{};
    };
    static constexpr std::array<int, 8> kTimingBucketsUs{10, 20, 50, 100, 200, 500, 1000, 2000};
    Calls live, background;
};
BridgeTiming bridgeTiming();
void resetBridgeTiming();
// Main-thread requests to plug-in bridges (one round trip each) since start:
// a count, for tests that bound how many an operation makes.
std::uint64_t bridgeRequests();
// The live calls and settles so far and their summed waits, for a cycle to
// compare.
struct BridgeWaits {
    std::uint64_t calls = 0;
    std::int64_t waitNs = 0;
};
BridgeWaits bridgeLiveWaits() SUPPLE_RT;

class Instance {
  public:
    // `isolated`: the plugin runs in a bridge process. Pass `bridge` to share
    // one process with other isolated instances; otherwise one is spawned.
    static Result<std::unique_ptr<Instance>> create(const PluginInfo& info,
                                                    const HostIdentity& host, bool isolated = false,
                                                    std::shared_ptr<BridgeProcess> bridge = {});
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    const PluginInfo& info() const { return info_; }

    // --- lifecycle (main thread) ---
    // Deactivation (and destruction) require processing to have stopped on the
    // audio thread first; deactivate() while processing is ignored and logged.
    Status activate(double sampleRate, std::uint32_t minFrames, std::uint32_t maxFrames);
    void deactivate();
    bool active() const { return active_; }
    double sampleRate() const { return sampleRate_; }  // of the current/last activation
    std::uint32_t maxFrames() const { return maxFrames_; }
    std::uint32_t latency() const;
    std::uint32_t tail() const;  // UINT32_MAX = infinite; 0 when not reported
    bool reportsTail() const;
    // Isolated instances: the bridge crashed, hung or exited. The instance
    // then outputs silence and every call fails; re-create it to recover.
    bool failed() const;
    std::string failure() const;
    bool isolated() const { return remote_ != nullptr; }
    int bridgePid() const;  // -1 for in-process
    // Bridged: this instance's failure deadline off the callback, in place
    // of bridgeDeadlineMs() (0, the default, uses that); lifecycle commands
    // get 5/2 of it plus 2 s, and so do control requests (activation,
    // parameters, patches) at least, beyond their own timeouts. An offline
    // render sets it while it runs (ADR-0090). It is atomic: any thread may
    // set it at any time, and a wait already under way keeps the deadline
    // it started with. No effect in process; callDeadlineMs() is then 0.
    void setCallDeadlineMs(int ms);
    int callDeadlineMs() const;
    // Bridged: kills this instance's bridge now, from any thread, and fails
    // every instance in it; failure() says the host abandoned it. A call
    // waiting on it off the callback, on any thread, returns within one wait
    // step (10 ms), and a control request as its socket closes. For work
    // that no longer wants the answer and owns every instance in the bridge,
    // such as a stopped offline render with its own pool (ADR-0090). No
    // effect in process.
    void abandonBridge();
    // Bridged: its bridge's audio thread was refused the scheduling class the
    // last audio-thread call asked for (ADR-0026, ADR-0081); false in process.
    bool bridgeThreadRefused() const;

    // --- ports (valid after create; a stereo configuration is selected when offered) ---
    std::uint32_t audioInputChannels() const { return inChannels_; }
    std::uint32_t audioOutputChannels() const { return outChannels_; }
    bool acceptsNotes() const { return acceptsNotes_; }
    // The plug-in declares note output (ADR-0068): a CLAP output note port
    // (note-ports, is_input false) or a VST3 event output bus. Its process()
    // then may append NoteOn/NoteOff/NoteChoke/Midi output events.
    bool outputsNotes() const { return outputsNotes_; }
    // The sidechain (key) input: a CLAP plug-in's first non-main input port, a
    // VST3's first auxiliary input bus; 0 when it has none (ADR-0018).
    std::uint32_t sidechainChannels() const { return keyChannels_; }
    // The output ports past the main one (a multi-out instrument's), in
    // order; their channels are ProcessBlock::auxOutputs (ADR-0021).
    const std::vector<AudioPortInfo>& auxOutputPorts() const { return auxOutPorts_; }
    std::uint32_t auxOutputChannels() const;
    // Whether a key is connected; set before activate(). CLAP plug-ins see an
    // unconnected key port deactivated (audio-ports-activation) and VST3s an
    // inactive auxiliary bus, so they can key from their own input; either
    // way it hears silence. CLAP plug-ins without that extension follow the
    // ProcessBlock key buffers, including routing changes while processing.
    Status setSidechainActive(bool on);
    bool sidechainActive() const { return keyActive_; }
    // Whether a change to it needs a fresh activation: a VST3, a CLAP plug-in
    // with port activation, or an isolated one (whose extensions the host
    // cannot see).
    bool followsKeyActivation() const {
        return vst3_ != nullptr || remote_ != nullptr || extPortsActivation_ != nullptr;
    }

    // --- standard parameters (main thread) ---
    std::vector<ParamInfo> params() const;
    Result<double> paramValue(std::uint32_t id) const;
    Result<std::string> paramText(std::uint32_t id, double hostValue) const;
    Result<double> paramFromText(std::uint32_t id, std::string_view text) const;
    // Queues a value change for the next processed block; when not processing,
    // applies it at once through params.flush.
    Status setParam(std::uint32_t id, double hostValue);

    // A VST3 has two parameter domains: the plain value it declares (min..max,
    // what ParamInfo and every command carries) and the normalized 0..1 value
    // its processor is given with its events. paramsNormalized() says which
    // domain `Event::Param` values must be in for this instance; CLAP has no
    // such split (the host value is the event value). The two conversions are
    // main-thread calls — an edit controller is not the processor, and the host
    // never calls it from the audio thread.
    bool paramsNormalized() const;
    Result<double> normalizedParam(std::uint32_t id, double plain) const;
    Result<double> plainParam(std::uint32_t id, double normalized) const;

    // --- semantic view (only when info().semantic) ---
    Result<json::Value> semanticSchema();
    Result<std::string> getPatch(bool changedOnly);
    Status setPatch(std::string_view json, std::uint32_t mode);  // CLAP_SUPPLE_SET_*
    Result<std::string> describe(bool changedOnly);
    // Current problems on the main thread; null means the plugin offers no
    // status extension. Queries never consume or clear problems. Bridged too.
    Result<json::Value> pluginStatus();
    // Canonical read-only factory patches; empty when not offered. Main thread.
    Result<json::Value> factoryPatches();

    // --- the editor without a window (com.suppleaudio.editor-picture/1, in process) ---
    struct Picture {
        int width = 0, height = 0;
        std::vector<std::uint8_t> rgba;  // rows from the top
    };
    bool drawsEditor() const { return extPicture_ != nullptr; }
    // While on, what plays reaches the editor's meters and displays, window or not.
    Status watchEditor(bool on);
    // The editor as it would open now, after the pointer input (its edits
    // reach the plugin's parameters as a user's would), `scale` times its
    // design size.
    Result<Picture> drawEditor(double scale,
                               std::span<const clap_supple_pointer_input_t> input = {});

    // --- what the plugin's code traces (com.suppleaudio.trace/1, in process) ---
    bool traces() const { return extTrace_ != nullptr && !remote_; }
    // While inactive: from the first block processed next, each traced name
    // in `bins` bins of `interval` frames (bins 0 stops).
    Status startTrace(std::uint32_t interval, std::uint32_t bins);
    // While inactive: the trace as the plugin wrote it (supple.trace/1.0).
    Result<json::Value> readTrace();

    // --- state (main thread) ---
    // Inactive saves flush acknowledged main-thread edits into component state.
    // Active saves require those edits to have been delivered by the audio consumer.
    Result<std::vector<std::uint8_t>> saveState();
    // Restoring state invalidates edits queued before this call.
    Status loadState(std::span<const std::uint8_t> bytes);

    // --- processing (audio thread) ---
    // start_processing; false if inactive or the plugin refused (then
    // process() must not be called), or when a bridged plug-in has not
    // answered yet (startUnanswered()).
    bool startProcessing() SUPPLE_RT;
    // The last startProcessing() of a bridged instance passed its live
    // deadline unanswered (ADR-0081): not a refusal; ask again later.
    bool startUnanswered() const SUPPLE_RT;
    void stopProcessing() SUPPLE_RT;
    bool processing() const { return processing_; }
    // Where this instance is in this process's execution table (ADR-0083, topic 6b): its cell,
    // that cell's generation and an address in the plug-in's binary, 0 when unknown (a VST3, an
    // instance the table had no room for). A bridge says it in its Create reply, so that the
    // host can match what the bridge's table says to the instance it announced.
    struct DiagnosticsPlace {
        std::uint32_t cell = 0xffffffffu;
        std::uint16_t generation = 0;
        std::uint64_t image = 0;
    };
    DiagnosticsPlace diagnosticsPlace() const noexcept;
    // For a bridged instance whose bridge died of a signal or an exception once its end was
    // observed (any later call that touches the failed bridge does): the id of the local crash
    // report that death may have caused (ADR-0083, topic 6a). Empty for anything else, and while
    // no supervisor writes reports. The report may not exist (a repeat in the run, the run's
    // limit): whoever reads it by this id handles a missing one.
    std::string bridgeIncident() const;
    // The host's executor that currently processes this instance (an opaque
    // token, null for none): a host with several audio threads uses it so
    // that exactly one of them calls the audio-thread API at a time. An
    // executor takes a free instance with claimOwner(), never by testing
    // owner() and then calling setOwner(): two executors that both find it
    // free could otherwise both start it. Only the owner calls setOwner(),
    // to hand it on, mark it, or let it go (null, after stopProcessing()).
    void setOwner(const void* owner) SUPPLE_RT;
    const void* owner() const SUPPLE_RT { return owner_.load(std::memory_order_acquire); }
    // Takes the instance for `owner` if nobody owns it (owner() is null), in
    // one compare-exchange; false if another token holds it.
    bool claimOwner(const void* owner) SUPPLE_RT;
    // Returns false on CLAP_PROCESS_ERROR, or when not processing. Output
    // events are appended to `out` (may be null) and sorted by time. Queued setParam() changes
    // are delivered first, at time 0, as far as the event arena has room
    // after the block's own events (the rest wait for the next block); block
    // events beyond the arena are dropped and counted (Notifications).
    bool process(const ProcessBlock& block, OutputEvents* out = nullptr) SUPPLE_RT;
    // Delivers queued setParam() changes through params.flush while active but
    // not processing (CLAP requires the audio thread for that case).
    // Also serves a plugin's request_flush while active (see needsAudioFlush()).
    void flushParams() SUPPLE_RT;
    bool needsAudioFlush() const;  // active, a flush was requested, nothing processed since
    void reset() SUPPLE_RT;        // clap_plugin.reset: [audio-thread & active]
    // How many times reset() was called: a host that keeps per-instance state
    // (a rack's plugin module: which notes it sent) sees the plug-in forgot it.
    std::uint32_t resets() const { return resets_.load(std::memory_order_relaxed); }

    // --- main-thread servicing (call regularly: after renders, from the app loop) ---
    // Handles request_callback, request_restart (while active), pending flushes
    // and due timers. Returns true if anything was done.
    bool serviceMainThread();
    // Local/remote instance hook. Install on main; the
    // relay must outlive the instance and every plug-in callback thread.
    // Requests publish their flags before notifying, including from audio.
    void setMainThreadWake(MainThreadWake& wake);
    // The plug-in asked to be deactivated and reactivated (CLAP request_restart,
    // a VST3 latency change), and the restart waits for processing to stop:
    // the engine takes the instance off its executors, the next
    // serviceMainThread() restarts it, and its new latency applies.
    bool restartPending() const;
    // Flags the plugin raised since the last call (cleared on read).
    struct Notifications {
        bool stateDirty = false, paramsRescan = false, latencyChanged = false,
             semanticChanged = false, tailChanged = false;
        std::optional<Error> restartError;  // failed automatic reactivation, cleared on read
        struct ParamClear {
            std::uint32_t id = CLAP_INVALID_ID, flags = 0;
        };
        std::vector<ParamClear> paramsClear;  // CLAP params.clear is main-thread only
        std::uint32_t paramsRescanFlags = 0;
        std::uint32_t droppedEvents = 0;  // input events that did not fit a block's arena
    };
    Notifications takeNotifications();
    // CLAP request_process and queued setParam edits (local, VST3 or bridged):
    // main may inspect it to activate an inactive instance; its audio owner
    // consumes it to wake a sleeping stage.
    bool processRequested() const SUPPLE_RT;
    bool takeProcessRequest() SUPPLE_RT;
    bool sleeping() const SUPPLE_RT;
    // Main thread: subscribe an executor's futex sequence to process/flush
    // requests, queued parameter edits, and ownership release. Null detaches
    // and joins in-flight notifications before the word may be destroyed.
    void setAudioWake(std::atomic<std::uint32_t>* word);

    struct IdleOutputs {
        static constexpr std::size_t kCapacity = 1024;
        std::vector<OutputEvent> events;
        std::uint64_t dropped = 0;
    };
    // Main thread: drains CLAP flush outputs, including active-but-stopped
    // audio-thread flushes. FIFO, at most kCapacity events; overflow drops the
    // newest event and is counted since the previous drain. Flush times are 0.
    // Note and MIDI output in a flush has no block to belong to: it is dropped
    // and counted in `dropped`. Bridged instances drain over the control channel; bridge failure is
    // observable through failed(). VST3 has no CLAP flush outputs.
    IdleOutputs takeIdleOutputs();

    // --- editor (main/UI thread). X11/Win32 dimensions are pixels, Cocoa
    // dimensions are points. Bridged Cocoa editors float in the child process.
    struct EditorSize {
        std::uint32_t width = 0, height = 0;
        bool resizable = false;
    };
    using EditorFrame = host::EditorFrame;  // a bridge-owned window's content frame
    bool hasEditor() const;
    // `frame` asks a bridge-owned window (Cocoa) to open there, size included
    // when the editor is resizable; other editors ignore it.
    Result<EditorSize> openEditor(NativeWindow parent, double scale,
                                  std::optional<EditorFrame> frame = {});
    // Compatibility for existing X11 callers; native callers use the typed API.
    Result<EditorSize> openEditor(unsigned long x11Parent, double scale) {
        return openEditor({NativeWindow::Kind::X11, x11Parent}, scale);
    }
    // Host-driven resize; returns the size the plugin accepted.
    Result<EditorSize> resizeEditor(std::uint32_t width, std::uint32_t height);
    // The display scale of the open editor's window changed (X11/Win32 only;
    // Cocoa editors use points and are never told). The plug-in gets the new
    // factor (CLAP set_scale, VST3 content scale). A resizable editor keeps its
    // logical size unless the plug-in resized itself. Returns the size to give
    // the window, in pixels at the new scale.
    Result<EditorSize> setEditorScale(double scale);
    void closeEditor();
    // The frame a bridge-owned window had when closeEditor() closed it.
    std::optional<EditorFrame> takeClosedEditorFrame() { return std::exchange(closedFrame_, {}); }
    bool editorOpen() const { return editorOpen_; }
    // What the plugin asked for since the last call; the window owner applies show/hide.
    struct EditorRequests {
        bool resize = false, closed = false, show = false, hide = false;
        std::uint32_t width = 0, height = 0;
        // A bridge-owned window moved or resized, or closed (its final frame).
        std::optional<EditorFrame> frame;
    };
    EditorRequests takeEditorRequests();
    // clap.posix-fd-support: descriptors to watch (fd, CLAP_POSIX_FD_* flags),
    // and the callback when one is ready.
    std::vector<std::pair<int, std::uint32_t>> watchedFds() const;
    void onFd(int fd, std::uint32_t flags);
    // Milliseconds until the next plugin timer is due (-1: none): the UI
    // loop's deadline, including timers registered without an open editor.
    int nextTimerMs() const;

    // Log lines the plugin wrote (from any thread), drained on the main thread.
    std::vector<std::string> takeLog();

    const clap_plugin_t* raw() const { return plugin_; }

    struct HostState;  // opaque host-side state (callbacks, queues, event arena)

  private:
    // The instance's cell in the execution table (diagnostics-context, ADR-0083): claimed
    // in create(), given back when the instance's own members are gone. The first member,
    // so the last destroyed.
    class Cell {
      public:
        Cell() = default;
        Cell(const Cell&) = delete;
        Cell& operator=(const Cell&) = delete;
        ~Cell();
        void acquire() noexcept;
        std::uint32_t id() const noexcept SUPPLE_RT { return id_; }

      private:
        std::uint32_t id_ = 0xffffffffu;  // diagctx::kNoCell
    };
    Cell cell_;

    Instance() = default;
    friend struct HostCallbacks;
    bool processBackend(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT;

    PluginInfo info_;
    std::shared_ptr<Library> library_;
    std::unique_ptr<HostState> hostState_;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* extParams_ = nullptr;
    const clap_plugin_state_t* extState_ = nullptr;
    const clap_plugin_latency_t* extLatency_ = nullptr;
    const clap_plugin_tail_t* extTail_ = nullptr;
    const clap_plugin_timer_support_t* extTimer_ = nullptr;
    const clap_plugin_supple_semantic_params_t* extSemantic_ = nullptr;
    const clap_plugin_supple_editor_picture_t* extPicture_ = nullptr;
    const clap_plugin_audio_ports_activation_t* extPortsActivation_ = nullptr;
    const clap_plugin_supple_plugin_status_t* extStatus_ = nullptr;
    const clap_plugin_supple_trace_t* extTrace_ = nullptr;
    const clap_plugin_gui_t* extGui_ = nullptr;
    const clap_plugin_posix_fd_support_t* extFd_ = nullptr;
    bool editorOpen_ = false;
    double editorScale_ = 1;  // the factor the open editor last received
    std::optional<EditorFrame> closedFrame_;
    std::uint32_t inChannels_ = 0, outChannels_ = 0, keyChannels_ = 0;
    std::vector<AudioPortInfo> auxOutPorts_;
    bool acceptsNotes_ = false, outputsNotes_ = false, keyActive_ = false;
    bool active_ = false;
    std::atomic<bool> processing_{false};  // written on the audio thread
    std::uint32_t maxFrames_ = 0;
    double sampleRate_ = 0.0;
    std::unique_ptr<RemoteInstance> remote_;
    std::unique_ptr<Vst3Instance> vst3_;
    std::atomic<const void*> owner_{nullptr};
    std::atomic<std::uint32_t> resets_{0};
    std::atomic<bool> paramProcessRequested_{false};
    void notifyAudio() SUPPLE_RT;
    std::atomic<std::atomic<std::uint32_t>*> audioWake_{nullptr};
    std::atomic<std::uint32_t> audioWakeReaders_{0};
    Notifications remoteNotes_;
    std::optional<Error> vst3RestartError_;  // a failed VST3 reactivation, until read
    std::vector<std::string> remoteLog_;
    bool remoteEditor_ = false;  // the bridged plugin has an embeddable editor
};

// A failed pluginStatus() as a problems array of one host-scoped entry, for
// reports that carry on without the plugin's document (a crashed bridge).
json::Value statusFailure(const Error& error);

}  // namespace supple::host
