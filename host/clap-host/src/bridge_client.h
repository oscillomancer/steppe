// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: the host side of a bridged plugin instance (ADR-0013).
#pragma once

#include "bridge_child.h"

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
};

class RemoteInstance;

class BridgeProcess {
  public:
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
    int pid() const { return pid_.load(); }

  private:
    friend struct BridgeTestAccess;
    friend class RemoteInstance;
    static constexpr int kHung = -2;
    static constexpr int kProtocolViolation = -3;
    BridgeProcess() = default;
    Status initializeNotifications();
    void watch();
    void notifySubscribers();
    void subscribe(RemoteInstance& instance, MainThreadWake& wake);
    void unsubscribe(RemoteInstance& instance);
    bool refresh();  // main only: ACK before snapshotting every registered member
    MainThreadWake watcherWake_;
    WakeEvent notification_;
    bool notificationsReady_ = false;
    std::thread watcher_;
    std::atomic<bool> watcherQuit_{false}, ackPending_{false};
    std::atomic<std::uint64_t> delivery_{0};
    std::uint64_t servedDelivery_ = 0;
    bool refreshNeeded_ = true;
    std::vector<RemoteInstance*> members_;  // main only, includes retired instances
    std::mutex subscribersMutex_;
    std::map<MainThreadWake*, std::size_t> subscribers_;
    std::atomic<int> pid_{-1};
    bridge::Control sock_ = bridge::noControl;
    std::unique_ptr<bridge::NativeChild> child_;
    std::mutex requests_;  // control calls from instances on the main thread
    std::atomic<bool> failed_{false};
    std::atomic<int> failSignal_{0};
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
    bool startProcessing() SUPPLE_RT;
    void stopProcessing() SUPPLE_RT;
    void reset() SUPPLE_RT;
    void flush() SUPPLE_RT;
    bool process(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT;

    bool failed() const { return process_->failed(); }
    std::string failure() const;
    void setDeadlineMs(int ms) { deadlineMs_ = ms; }
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
    bool command(bridge::Cmd cmd) SUPPLE_RT;
    void unmap();

    std::shared_ptr<BridgeProcess> process_;
    std::uint32_t instanceId_ = 0;
    void* map_ = nullptr;
    bridge::Header* hdr_ = nullptr;
    bridge::SharedSignals signals_;
    bridge::Layout layout_{0, 0, 0};
    std::uint32_t seq_ = 0;
    int deadlineMs_ = 2000;  // for process(); lifecycle commands get longer
    std::uint32_t latency_ = 0, tail_ = 0;
    bool reportsTail_ = false, restartPending_ = false;
    // Control servicing publishes this flag while audio checks for idle work.
    std::atomic<bool> needsAudioFlush_{false};
    std::atomic<bool> processRequested_{false};
};

std::filesystem::path bridgeExecutable();

}  // namespace supple::host
