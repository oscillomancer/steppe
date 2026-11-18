// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: the host side of a bridged plugin instance (ADR-0013).
#pragma once

#include "bridge_child.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "supple/base/main_thread_wake.h"

#include "bridge_protocol.h"
#include "supple/host/clap_host.h"

namespace supple::host {

struct RemotePorts {
    std::uint32_t inChannels = 0, outChannels = 0, keyChannels = 0;
    std::vector<AudioPortInfo> auxOutputs;  // ADR-0021
    bool acceptsNotes = false, outputsNotes = false, semantic = false, editor = false;
    // The instance's place in the bridge's own execution table and an address in the plug-in's
    // binary there (ADR-0083, topic 6b); 0xffffffff, 0, 0 when the bridge's table had no room.
    std::uint32_t childCell = 0xffffffffu;
    std::uint16_t childGeneration = 0;
    std::uint64_t childImage = 0;
};

class RemoteInstance;

// Tests only: whether a new bridge is handed a capture slot as it starts (true), so that a test
// can send the operation itself. Process-wide.
void setBridgeCaptureSlotForTests(bool attach);

// How long after a bridge was failed as hung by late blocks (ADR-0081) the main thread leaves it
// alive before it kills it: the bridge's own handler budget (selfcapture::kHandlerBudget), so
// that a bridge that faulted as it was failed is not cut off mid-capsule. Tests only: a different
// grace in nanoseconds, or a negative value for the default. Process-wide.
void setBridgeKillGraceForTests(std::int64_t nanoseconds);

// Tests only: the next `checks` liveness checks of a bridge process (BridgeProcess::alive and the
// one ~BridgeProcess makes first) answer "alive" whatever the process says, then the real answer
// again. A bridge that is dying of its own fault is alive until its dump is done and a test cannot
// hold a real one there; this is how a test makes the host see one. Zero clears it. Process-wide.
void setBridgeLivenessForTests(int checks);
// Tests only: how many of those answers have not been given yet.
int bridgeLivenessLeftForTests();

class BridgeProcess {
  public:
    static constexpr int kHung = -2;
    static constexpr int kAbandoned = -4;  // abandon(): its user no longer wants an answer
    static Result<std::shared_ptr<BridgeProcess>> spawn();
    ~BridgeProcess();
    Result<std::vector<std::uint8_t>> call(bridge::Op op, std::uint32_t instance,
                                           const bridge::Writer& w,
                                           bridge::Resource fd = bridge::noResource,
                                           int timeoutMs = 10000);
    bool failed() const { return failed_.load(); }
    std::string failure() const;
    void fail(int why) SUPPLE_RT;
    void reap();  // main thread
    bool alive() const;
    void killBridge();
    // Any thread: fails this bridge as abandoned and kills it. A wait on it
    // off the callback returns within one wait step (10 ms); a control
    // request sees its socket close.
    void abandon();
    int pid() const { return pid_.load(); }
    // This bridge's ordinal in the run (ADR-0083, topic 6a): 1, 2, ...; 0 when it has none (a
    // process adopted by a test, or the 65536th of a run).
    std::uint16_t ordinal() const { return ordinal_; }
    // The id of the local crash report this bridge's death may have caused (ADR-0083, topic 6a):
    // set when the end was observed (reap(), main thread) and was a signal or an exception while a
    // diagnostics supervisor was there to write one; empty otherwise. The report may still not
    // exist (a repeat, the run's limit, a full folder): a reader of the id handles that.
    std::string incident() const;

  private:
    friend struct BridgeTestAccess;
    friend class RemoteInstance;
    static constexpr int kProtocolViolation = -3;
    BridgeProcess() = default;
    Status initializeNotifications();
    // Topic 6c: creates the bridge's capture slot and hands it over; any failure leaves the
    // bridge working with no capsule (the report is then the one of the host's account alone).
    void attachCaptureSlot();
    void watch();
    void notifySubscribers();
    void subscribe(RemoteInstance& instance, MainThreadWake& wake);
    void unsubscribe(RemoteInstance& instance);
    // Main only: ACK, then snapshot the members that need it (every member for
    // a pending whole-bridge refresh or an unnamed delivery). True when `self`
    // was among them, so its consumer need not pull it again (round 25).
    bool refresh(const RemoteInstance* self);
    MainThreadWake watcherWake_;
    WakeEvent notification_;
    bool notificationsReady_ = false;
    std::thread watcher_;
    std::atomic<bool> watcherQuit_{false}, ackPending_{false};
    std::atomic<std::uint64_t> delivery_{0};
    std::uint64_t servedDelivery_ = 0;
    bool refreshNeeded_ = true;  // every member: a delivery names none
    // Members whose own request changed them: only these are pulled (item D:
    // pulling every member made one FX's edit cost the bridge's size).
    std::vector<RemoteInstance*> refreshOnly_;
    void queueRefresh(RemoteInstance* member) {
        if (std::find(refreshOnly_.begin(), refreshOnly_.end(), member) == refreshOnly_.end())
            refreshOnly_.push_back(member);
    }
    std::vector<RemoteInstance*> members_;  // main only, includes retired instances
    // Ids an ACK may name beyond members_: destroys in flight. Longer lists
    // are unreadable (review round 26: they cost O(count x members) on main).
    static constexpr std::size_t kAckSlack = 16;
    std::mutex subscribersMutex_;
    std::map<MainThreadWake*, std::size_t> subscribers_;
    std::atomic<int> pid_{-1};
    bridge::Control sock_ = bridge::noControl;
    std::unique_ptr<bridge::NativeChild> child_;
    std::mutex requests_;  // control calls from instances on the main thread
    std::atomic<bool> failed_{false};
    std::atomic<int> failSignal_{0};
    std::atomic<std::int64_t> hungAt_{0};  // steady ns when it was failed as kHung
    // Topic 6a. The host sets `hostKilled_` before every kill, so that an end it caused is told
    // from one the operating system or the plug-in caused. The end is announced once, from
    // reap() on the main thread, or from the destructor when nobody reaped.
    std::uint16_t ordinal_ = 0;
    // Topic 6c: the capture slot's memory object while this process holds it. POSIX closes it as
    // soon as it is sent (the descriptor in flight keeps the object); Windows keeps the handle
    // until the process object goes, because the supervisor duplicates it from this process.
    bridge::Resource slot_ = bridge::noResource;
    std::atomic<bool> hostKilled_{false}, endAnnounced_{false};
    // Topic 6a: the id of the crash report a signal or exception end may have caused (written once
    // by announceEnd, then published by the release store of `incidentReady_`).
    std::array<char, 32> incident_{};
    std::atomic<bool> incidentReady_{false};
    void announceEnd(int why, bool hostEnded) noexcept;
};

class RemoteInstance {
  public:
    static Result<std::unique_ptr<RemoteInstance>> spawn(
        const PluginInfo& info, const HostIdentity& identity, RemotePorts& ports,
        std::shared_ptr<BridgeProcess> process = {});
    ~RemoteInstance();

    // main thread
    void setMainThreadWake(MainThreadWake& wake);
    // `settled` receives the widths the plug-in settled on at activation.
    Status activate(double rate, std::uint32_t minFrames, std::uint32_t maxFrames,
                    std::uint32_t inChannels, std::uint32_t outChannels,
                    std::uint32_t keyChannels = 0, bool keyActive = false,
                    std::uint32_t auxOutChannels = 0, RemotePorts* settled = nullptr);
    void deactivate();
    std::vector<ParamInfo> params();
    Result<double> paramValue(std::uint32_t id);
    Result<std::string> paramText(std::uint32_t id, double value);
    Result<double> paramFromText(std::uint32_t id, std::string_view text);
    Status setParam(std::uint32_t id, double value);
    // The two parameter domains a VST3 has (identity in the bridge for CLAP).
    Result<double> normalizedParam(std::uint32_t id, double plain);
    Result<double> plainParam(std::uint32_t id, double normalized);
    void refreshLatency();
    std::uint32_t latency() const { return latency_; }
    std::uint32_t tail() const { return tail_; }
    bool reportsTail() const { return reportsTail_; }
    // SemanticSchema/Describe/GetPatch: a string reply; `flag` = changedOnly.
    Result<std::string> text(bridge::Op op, std::optional<bool> flag = {});
    Status setPatch(std::string_view json, std::uint32_t mode);
    Result<std::vector<std::uint8_t>> saveState();
    Status loadState(std::span<const std::uint8_t> bytes);
    // Drain retained snapshots; pull on demand only without a wake subscription.
    bool service(Instance::Notifications& notes, std::vector<std::string>& log);
    bool needsAudioFlush() const SUPPLE_RT {
        return needsAudioFlush_.load(std::memory_order_acquire);
    }
    bool restartPending() const { return restartPending_; }  // as of the last service()
    // This instance's deadline off the callback; 0: bridgeDeadlineMs().
    // Atomic: any thread may set it at any time, and a wait already under
    // way keeps the deadline it started with.
    void setCallDeadlineMs(int ms) {
        callDeadlineMs_.store(std::max(ms, 0), std::memory_order_relaxed);
    }
    int callDeadlineMs() const { return callDeadlineMs_.load(std::memory_order_relaxed); }
    void abandon() { process_->abandon(); }  // any thread (BridgeProcess::abandon)
    bool processRequested() const SUPPLE_RT {
        return processRequested_.load(std::memory_order_acquire);
    }
    bool takeProcessRequest() SUPPLE_RT {
        return processRequested_.exchange(false, std::memory_order_acq_rel);
    }
    Instance::IdleOutputs takeIdleOutputs();
    // The plug-in's editor, embedded by the bridge into `x11Parent` (gap log #39).
    Result<Instance::EditorSize> openEditor(NativeWindow parent, double scale,
                                            std::optional<Instance::EditorFrame> frame);
    Result<Instance::EditorSize> resizeEditor(std::uint32_t width, std::uint32_t height);
    Result<Instance::EditorSize> setEditorScale(double scale);
    // Returns the frame the bridge's editor window had when it closed (macOS).
    std::optional<Instance::EditorFrame> closeEditor();
    Instance::EditorRequests editorRequests();

    // audio thread
    // False when the plug-in refused, or when the bridge has not answered yet
    // (startUnanswered(): a live deadline passed first; asking again later
    // gets the answer, ADR-0081).
    bool startProcessing() SUPPLE_RT;
    bool startUnanswered() const SUPPLE_RT { return startUnanswered_; }
    // While a late request is outstanding (ADR-0081 §4) these are kept, in
    // this order and once each, and sent before the next block: stop, reset,
    // flush. A reset drops the notes carried from skipped blocks.
    void stopProcessing() SUPPLE_RT;
    void reset() SUPPLE_RT;
    void flush() SUPPLE_RT;
    bool process(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT;
    // Events a skipped block carries to the next one the plug-in processes.
    static constexpr std::uint32_t kMaxCarried = 512;

    bool failed() const { return process_->failed(); }
    // The bridge's audio thread did not get the class its last command asked for.
    bool classRefused() const { return classRefused_.load(std::memory_order_relaxed); }
    std::string failure() const;
    // Main thread: the end of a failed bridge is observed here, so the id does not wait for some
    // other call to touch the bridge (the engine, which has a wake, does not pull a failed one).
    std::string incident() const {
        if (process_->failed()) process_->reap();
        return process_->incident();
    }
    int bridgePid() const { return process_->pid(); }

  private:
    friend struct BridgeTestAccess;
    friend class BridgeProcess;
    RemoteInstance() = default;
    void pullSnapshot();
    bool pullService();
    Instance::IdleOutputs pullIdleOutputs();
    Instance::EditorRequests pullEditorRequests();
    MainThreadWake* wake_ = nullptr;  // protected by process subscribersMutex_
    Instance::Notifications pendingNotes_;
    Instance::IdleOutputs pendingIdle_;
    Instance::EditorRequests pendingEditor_;
    std::vector<std::string> pendingLog_;
    bool pendingWork_ = false, editorOpen_ = false;
    Result<std::vector<std::uint8_t>> call(bridge::Op op, const bridge::Writer& w,
                                           bridge::Resource fd = bridge::noResource,
                                           int timeoutMs = 10000);
    enum class Sent : std::uint8_t { Answered, Late, Unsent };
    // Posts `cmd` and waits for its answer; Late leaves it outstanding.
    // Requires a settled instance.
    Sent post(bridge::Cmd cmd) SUPPLE_RT;
    // settle(), then post(): false unless it was answered.
    bool command(bridge::Cmd cmd) SUPPLE_RT;
    // While unsettled, keeps a lifecycle command for settle() to send.
    void lifecycle(bridge::Cmd cmd) SUPPLE_RT;
    // A skipped block's events, for the next block the plug-in processes.
    // Full of distinct parameters, it still takes a later value of one.
    void carry(std::span<const Event> events) SUPPLE_RT;
    void dropCarriedNotes() SUPPLE_RT;
    // Too much carried: keep each parameter's latest value, drop the notes
    // and, if any went, reset the plug-in first, so no note it never hears
    // the end of hangs.
    void overflow() SUPPLE_RT;
    // Drops the carried parameters `events` set again.
    void supersede(std::span<const Event> events) SUPPLE_RT;
    // Whether request `req` was answered: within the live deadline when the
    // calling thread has one (ADR-0081), else within the background one
    // (ADR-0013), which fails and kills a bridge that misses it.
    bool answered(std::uint32_t req, bridge::Cmd cmd) SUPPLE_RT;
    // The shared memory is the bridge's until it answers the request a late
    // live block left outstanding: waits for that answer first, then sends
    // the lifecycle commands kept meanwhile. False while one is outstanding.
    bool settle() SUPPLE_RT;
    void late() SUPPLE_RT;
    void unmap();

    std::shared_ptr<BridgeProcess> process_;
    std::uint32_t instanceId_ = 0;
    void* map_ = nullptr;
    bridge::Header* hdr_ = nullptr;
    bridge::SharedSignals signals_;
    bridge::Layout layout_{0, 0, 0};
    std::uint32_t seq_ = 0;
    std::uint32_t outstanding_ = 0;  // a late live request not yet answered (if pending_)
    bool pending_ = false;           // apart from its number, which wraps through 0
    bool startUnanswered_ = false;
    std::uint8_t lifecycle_ = 0;  // kept commands, a bit each (bridge_client.cpp)
    std::unique_ptr<Event[]> carried_ = std::make_unique<Event[]>(kMaxCarried);
    std::uint32_t carriedCount_ = 0;
    std::int64_t lateSince_ = 0;            // steady ns of the first of consecutive late calls
    std::int64_t lateFailNs_ = 50'000'000;  // consecutive late calls fail it after this
    bool spun_ = false;                     // the last answer came while spinning, before a wait
    std::uint32_t latency_ = 0, tail_ = 0;
    bool reportsTail_ = false, restartPending_ = false;
    // Control servicing publishes this flag while audio checks for idle work.
    std::atomic<bool> needsAudioFlush_{false};
    std::atomic<bool> processRequested_{false};
    std::atomic<bool> classRefused_{false};
    std::atomic<int> callDeadlineMs_{0};  // 0: the process-wide deadline
};

std::filesystem::path bridgeExecutable();

}  // namespace supple::host
