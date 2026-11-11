// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The bridged backing of host::Instance (ADR-0013): the plugin lives in a
// bridge process; main-thread calls are control-plane requests, audio-thread
// calls are commands in shared memory with a futex wake-up and a deadline.
#include "bridge_client.h"
#include "bridge_notifications.h"
#include "editor_platform.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

#include "supple/base/rt.h"

namespace supple::host {

using namespace bridge;

namespace {

std::mutex gBridgeMutex;
std::filesystem::path gBridgeExecutable;

constexpr int kCreateTimeoutMs = 20000;

// ThreadPolicy as kind << 8 | priority, read on audio threads.
std::atomic<std::uint32_t> gBridgeAudioPolicy{static_cast<std::uint32_t>(ThreadPolicy::Kind::Normal)
                                              << 8};

}  // namespace

void setBridgeAudioPolicy(ThreadPolicy policy) {
    gBridgeAudioPolicy.store(static_cast<std::uint32_t>(policy.kind) << 8 |
                                 static_cast<std::uint32_t>(std::clamp(policy.priority, 0, 99)),
                             std::memory_order_relaxed);
}

ThreadPolicy bridgeAudioPolicy() SUPPLE_RT {
    const std::uint32_t packed = gBridgeAudioPolicy.load(std::memory_order_relaxed);
    return {static_cast<ThreadPolicy::Kind>(packed >> 8), static_cast<int>(packed & 0xff)};
}

void setBridgeExecutable(std::filesystem::path path) {
    std::lock_guard lock(gBridgeMutex);
    gBridgeExecutable = std::move(path);
}

bool locateBridgeExecutable(std::string_view name) {
    std::error_code ec;
    const std::filesystem::path self = ownExecutable();
    if (self.empty()) return false;
    const std::filesystem::path dir = self.parent_path();
    auto file = platform::pathFromText(name);
#if defined(_WIN32)
    file += ".exe";
#endif
    // Installed: beside it. Build tree: host/engine/tools -> host/clap-host,
    // daw/gui -> host/clap-host.
    for (const std::filesystem::path& candidate :
         {dir / file, dir.parent_path() / "clap-host" / file,
          dir.parent_path().parent_path() / "host" / "clap-host" / file})
        if (std::filesystem::exists(candidate, ec)) {
            setBridgeExecutable(candidate);
            return true;
        }
#if defined(__APPLE__)
    // CMake's GUI executable is nested inside daw/gui/<slug>.app/Contents/MacOS.
    const auto build = dir.parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto candidate = build / "host" / "clap-host" / file;
    if (std::filesystem::is_regular_file(build / "CMakeCache.txt", ec) &&
        std::filesystem::is_regular_file(candidate, ec)) {
        setBridgeExecutable(candidate);
        return true;
    }
#endif
    return false;
}

std::filesystem::path bridgeExecutable() {
    std::lock_guard lock(gBridgeMutex);
    return gBridgeExecutable;
}

// --- lifetime -------------------------------------------------------------------------

Result<std::shared_ptr<BridgeProcess>> BridgeProcess::spawn() {
    const std::filesystem::path exe = bridgeExecutable();
    if (exe.empty())
        return Error("host.bridge", "no plugin bridge is configured", {},
                     "setBridgeExecutable() at startup, or use in-process isolation");
    Resource sv[2];
    if (!controlPair(sv)) return Error("host.bridge", "socketpair failed");
    std::shared_ptr<BridgeProcess> self(new BridgeProcess());
    auto child = NativeChild::launch(exe, sv[1]);
    closeResource(sv[1]);
    if (!child) {
        closeResource(sv[0]);
        Error error("host.bridge", "could not start plugin bridge: " + child.error().message,
                    platform::pathText(exe), "check the bridge executable and process resources");
        error.details.push_back(child.error());
        return error;
    }
    self->child_ = std::move(*child);
    self->pid_ = self->child_->pid();
#if defined(_WIN32)
    self->sock_ = new NativeControl(reinterpret_cast<HANDLE>(sv[0]));
#else
    self->sock_ = sv[0];
#endif
    if (auto status = self->initializeNotifications(); !status) return status.error();
    return self;
}

Status BridgeProcess::initializeNotifications() {
    if (notificationsReady_) return okStatus();
    if (!watcherWake_.waitable().valid() || !notification_.waitable().valid())
        return Error("host.bridge", "could not create bridge notification wake", "/notification",
                     "free event resources or use in-process isolation");
    Writer w;
    w.u32(kVersion);
    auto subscribed = call(Op::SubscribeNotifications, 0, w, notificationTarget(notification_));
    if (!subscribed) return subscribed.error();
    notificationsReady_ = true;
    try {
        watcher_ = std::thread([this] { watch(); });
    } catch (const std::system_error& error) {
        fail(-1);
        killBridge();
        notificationsReady_ = false;
        return Error("host.bridge",
                     "could not start bridge notification watcher: " + std::string(error.what()),
                     "/notification", "free thread resources or use in-process isolation");
    }
    return okStatus();
}

void BridgeProcess::notifySubscribers() {
    // Hold lifetime protection through notify; never call service inline.
    std::lock_guard lock(subscribersMutex_);
    for (const auto& [wake, count] : subscribers_) {
        (void)count;
        wake->notify();
    }
}

void BridgeProcess::subscribe(RemoteInstance& instance, MainThreadWake& wake) {
    std::lock_guard lock(subscribersMutex_);
    if (instance.wake_ == &wake) return;
    if (instance.wake_ && --subscribers_[instance.wake_] == 0) subscribers_.erase(instance.wake_);
    instance.wake_ = &wake;
    ++subscribers_[&wake];
    refreshNeeded_ = true;
    wake.notify();  // retained pre-subscription work and already-failed processes
}

void BridgeProcess::unsubscribe(RemoteInstance& instance) {
    std::lock_guard lock(subscribersMutex_);
    if (instance.wake_ && --subscribers_[instance.wake_] == 0) subscribers_.erase(instance.wake_);
    instance.wake_ = nullptr;
}

void BridgeProcess::watch() {
    nameThread("bridge-parent");
    bool watchingDeath = true, failureAnnounced = false;
    for (;;) {
        Waitable death;
        if (child_) death = child_->waitable();
        WaitItem descriptors[] = {
            {notification_.waitable(), kWaitRead, 0},
            {watcherWake_.waitable(), kWaitRead, 0},
            {watchingDeath ? death : Waitable{}, kWaitRead, 0},
#if defined(_WIN32)
            {Waitable{}, 0, 0}};  // process HANDLE reports death; no pipe reads here
#else
            {watchingDeath ? Waitable(sock_) : Waitable{}, 0, 0}};
#endif
        const int ready = waitFor(descriptors, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) {
            fail(-1);
            delivery_.fetch_add(1, std::memory_order_release);
            notifySubscribers();
            return;
        }
        bool changed = false;
        if (descriptors[1].ready) {
            (void)watcherWake_.drain();
            changed = failed() && !failureAnnounced;
        }
        if (watcherQuit_.load(std::memory_order_acquire)) return;
        if (descriptors[0].ready & kWaitRead) {
            if (notification_.drain()) {
                ackPending_.store(true, std::memory_order_release);
                changed = true;
            }
        }
        if (descriptors[2].ready || descriptors[3].ready) {
            fail(-1);
            watchingDeath = false;  // pidfd and HUP stay level-ready
            changed = true;
        }
        if (changed) {
            failureAnnounced |= failed();
            delivery_.fetch_add(1, std::memory_order_release);
            notifySubscribers();
        }
    }
}

bool BridgeProcess::refresh() {
    const auto delivery = delivery_.load(std::memory_order_acquire);
    if (!refreshNeeded_ && delivery == servedDelivery_) return false;
    refreshNeeded_ = false;
    servedDelivery_ = delivery;  // a delivery during pulls needs another pass
    if (ackPending_.exchange(false, std::memory_order_acq_rel) && !failed())
        (void)call(Op::AckNotifications, 0, Writer{});
    for (auto* member : members_) member->pullSnapshot();
    return true;
}

Result<std::shared_ptr<BridgeProcess>> createBridgeProcess() { return BridgeProcess::spawn(); }
bool bridgeProcessFailed(const std::shared_ptr<BridgeProcess>& process) {
    return process && (process->failed() || !process->alive());
}

Result<std::unique_ptr<RemoteInstance>> RemoteInstance::spawn(
    const PluginInfo& info, const HostIdentity& identity, RemotePorts& ports,
    std::shared_ptr<BridgeProcess> process) {
    if (!process) {
        auto made = createBridgeProcess();
        if (!made) return std::move(made).error();
        process = std::move(*made);
    }
    std::unique_ptr<RemoteInstance> self(new RemoteInstance());
    self->process_ = std::move(process);
    if (auto status = self->process_->initializeNotifications(); !status) return status.error();
    Writer w;
    w.str(platform::pathText(info.path));
    w.str(info.id);
    w.str(info.format);
    w.str(identity.name);
    w.str(identity.vendor);
    w.str(identity.url);
    w.str(identity.version);
    auto reply = self->call(Op::Create, w, -1, kCreateTimeoutMs);
    if (!reply) return std::move(reply).error();
    Reader r(reply->data(), reply->size());
    self->instanceId_ = r.u32();
    ports.inChannels = r.u32();
    ports.outChannels = r.u32();
    ports.acceptsNotes = r.u8() != 0;
    ports.semantic = r.u8() != 0;
    ports.keyChannels = r.u32();
    ports.editor = r.u8() != 0;
    ports.outputsNotes = r.u8() != 0;
    const std::uint32_t auxPorts = r.u32();
    for (std::uint32_t i = 0; i < auxPorts && i < kMaxAuxPorts && r.ok(); ++i) {
        AudioPortInfo port;
        port.name = r.str();
        port.channels = r.u32();
        ports.auxOutputs.push_back(std::move(port));
    }
    if (!r.ok() || !self->instanceId_ || auxPorts > kMaxAuxPorts)
        return Error("host.bridge", "malformed reply from the plugin bridge");
    if (ports.inChannels > kMaxChannels || ports.outChannels > kMaxChannels ||
        ports.keyChannels > kMaxChannels)
        return Error("host.layout", "bridge layout exceeds 16 channels", {},
                     "select a layout with at most 16 main and key channels");
    self->process_->members_.push_back(self.get());
    self->process_->refreshNeeded_ = true;
    return self;
}

RemoteInstance::~RemoteInstance() {
    if (process_) {
        process_->unsubscribe(*this);
        std::erase(process_->members_, this);
    }
    if (process_ && instanceId_ && !failed()) (void)call(Op::Destroy, Writer{}, -1, 2000);
    unmap();
}

BridgeProcess::~BridgeProcess() {
    watcherQuit_.store(true, std::memory_order_release);
    watcherWake_.notify();
    if (watcher_.joinable()) watcher_.join();
    closeControl(sock_);
    if (child_) {
        if (child_->waitable().valid()) {
            WaitItem exited{child_->waitable()};
            (void)waitFor(std::span(&exited, 1), 2000);
        } else {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (child_->alive() && std::chrono::steady_clock::now() < deadline)
                (void)waitFor({}, 10);
        }
    }
    child_.reset();
}

void RemoteInstance::unmap() {
    unmapMemory(map_, layout_.size);
    signals_.clear();
    map_ = nullptr;
    hdr_ = nullptr;
}

void BridgeProcess::killBridge() {
    // The helper retains the leader's identity until final tree cleanup.
    if (child_) child_->kill();
}

// Main thread: observes exit without reaping the helper-owned leader.
void BridgeProcess::reap() {
    if (pid_ <= 0) return;
    if (!child_ || child_->alive()) return;
    const int why = child_->exitStatus();
    pid_ = -1;
    fail(why);
    if (failSignal_.load() == -1) failSignal_.store(why);
}

bool BridgeProcess::alive() const {
    if (pid_ <= 0) return false;
    return child_ && child_->alive();
}

void BridgeProcess::fail(int why) SUPPLE_RT {
    if (!failed_.exchange(true)) {
        failSignal_.store(why, std::memory_order_release);
        watcherWake_.notify();
    }
}

std::string BridgeProcess::failure() const {
    if (!failed()) return {};
    const int s = failSignal_.load();
    if (s == kProtocolViolation) return "the plugin bridge violated the output-event protocol";
    if (s == kHung) return "the plugin stopped responding and its bridge was killed";
#if defined(_WIN32)
    if (s != 0 && s != -1)
        return "the plugin bridge exited (Windows status " +
               std::to_string(static_cast<std::uint32_t>(s)) + ")";
#else
    if (s > 0) return std::string("the plugin bridge died (") + ::strsignal(s) + ")";
#endif
    return "the plugin bridge exited";
}

std::string RemoteInstance::failure() const { return process_->failure(); }

// --- control plane --------------------------------------------------------------------

Result<std::vector<std::uint8_t>> BridgeProcess::call(Op op, std::uint32_t instance,
                                                      const Writer& w, Resource fd, int timeoutMs) {
    std::lock_guard lock(requests_);
    if (failed()) {
        reap();
        return Error("host.bridge_failed", failure(), {}, "fx.restart re-creates it");
    }
    const auto deadline = deadlineAfter(timeoutMs);
    std::uint32_t status = 0;
    std::vector<std::uint8_t> reply;
    std::uint32_t returnedId = 0;
    Resource replyFd = noResource;
#if defined(_WIN32)
    if (fd != noResource) {
        fd = child_ ? child_->duplicate(fd) : noResource;
        if (fd == noResource)
            return Error("bridge.transfer", "could not transfer bridge resource", "/handle",
                         "check process/handle resources");
    }
#endif
    if (!sendFrame(sock_, static_cast<std::uint32_t>(op), instance, w.bytes, fd, deadline) ||
        !recvFrameUntil(sock_, status, returnedId, reply, &replyFd, deadline)) {
#if defined(_WIN32)
        if (sock_->rejectedResource()) {
            fail(kProtocolViolation);
            killBridge();
            return Error("bridge.malformed", "unexpected handle in bridge reply", "/handle",
                         "use matching host and bridge builds");
        }
#endif
        // Gone, or not answering: either way the instance is lost.
        if (alive()) {
            fail(kHung);
            killBridge();
        } else {
            fail(-1);
            for (int i = 0; i < 100 && pid_ > 0; ++i) {
                reap();
                if (pid_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return Error("host.bridge_failed", failure(), {}, "fx.restart re-creates it");
    }
    if (replyFd >= 0) {
        closeResource(replyFd);
        killBridge();
        fail(kProtocolViolation);
        return Error("bridge.malformed", "unexpected descriptor in bridge reply", "/fd",
                     "use matching host and bridge builds");
    }
    Reader r(reply.data(), reply.size());
    if (returnedId == instance && status == static_cast<std::uint32_t>(Reply::Error)) {
        auto error = r.error();
        if (r.done()) return error;
    } else if (returnedId == instance && status == static_cast<std::uint32_t>(Reply::Ok) &&
               validReply(op, r)) {
        return reply;
    }
    killBridge();
    fail(-1);
    return Error("bridge.malformed", "malformed reply from the plugin bridge", {},
                 "fx.restart re-creates it");
}

Result<std::vector<std::uint8_t>> RemoteInstance::call(Op op, const Writer& w, Resource fd,
                                                       int timeoutMs) {
    switch (op) {
        case Op::Activate:
        case Op::Deactivate:
        case Op::SetParam:
        case Op::SetPatch:
        case Op::LoadState:
        case Op::EditorOpen:
        case Op::EditorResize:
        case Op::EditorScale:
        case Op::EditorClose:
            process_->refreshNeeded_ = true;
            break;
        default:
            break;
    }
    return process_->call(op, instanceId_, w, fd, timeoutMs);
}

Status RemoteInstance::activate(double rate, std::uint32_t minFrames, std::uint32_t maxFrames,
                                std::uint32_t in, std::uint32_t out, std::uint32_t key,
                                bool keyActive, std::uint32_t auxOut, RemotePorts* settled) {
    if (in > kMaxChannels || out > kMaxChannels || key > kMaxChannels)
        return Error("host.layout", "bridge layout exceeds 16 channels");
    unmap();
    if (failed()) return Error("host.bridge_failed", failure(), {}, "fx.restart re-creates it");
    layout_ = Layout(maxFrames, in, out, key, std::min(auxOut, kMaxAuxChannels));
    if (layout_.size > std::numeric_limits<std::uint32_t>::max())
        return Error("host.activate", "shared memory size exceeds the bridge protocol limit", {},
                     "use a smaller maximum block size");
    const Resource fd = createMemory(layout_.size);
    if (fd == noResource)
        return Error("host.bridge", "could not create shared memory", "/memory",
                     "free memory resources or use in-process isolation");
    map_ = mapMemory(fd, layout_.size);
    if (!map_ || !signals_.create()) {
        closeResource(fd);
        unmap();
        return Error("host.bridge", "could not map shared memory or create audio signals",
                     "/memory", "free memory/event resources or use in-process isolation");
    }
    hdr_ = new (map_) Header{};
    hdr_->magic = kMagic;
    hdr_->version = kVersion;
    hdr_->maxFrames = maxFrames;
    hdr_->inChannels = layout_.inChannels;
    hdr_->outChannels = layout_.outChannels;
    hdr_->keyChannels = layout_.keyChannels;
    hdr_->auxOutChannels = layout_.auxOutChannels;
    hdr_->keyActive = keyActive ? 1 : 0;
    seq_ = 0;
    Writer w;
    w.f64(rate);
    w.u32(minFrames);
    w.u32(maxFrames);
    w.u32(static_cast<std::uint32_t>(layout_.size));
#if defined(_WIN32)
    const auto requestEvent = process_->child_->duplicate(signals_.resource(false));
    const auto doneEvent = process_->child_->duplicate(signals_.resource(true));
    if (requestEvent == noResource || doneEvent == noResource) {
        process_->child_->revoke(requestEvent);
        process_->child_->revoke(doneEvent);
        closeResource(fd);
        unmap();
        return Error("host.bridge", "could not transfer audio signals", "/events",
                     "check process/handle resources");
    }
    w.u64(static_cast<std::uint64_t>(requestEvent));
    w.u64(static_cast<std::uint64_t>(doneEvent));
#endif
    auto reply = call(Op::Activate, w, fd);
    closeResource(fd);  // the mapping (and the bridge's copy) keep it
    if (!reply) {
#if defined(_WIN32)
        // The only early failure after event duplication precedes frame send.
        // Once sent, the child owns those handles; transport failure kills it.
        if (reply.error().code == "bridge.transfer") {
            process_->child_->revoke(requestEvent);
            process_->child_->revoke(doneEvent);
        }
#endif
        unmap();
        return std::move(reply).error();
    }
    Reader r(reply->data(), reply->size());
    RemotePorts ports;
    ports.inChannels = r.u32();
    ports.outChannels = r.u32();
    ports.keyChannels = r.u32();
    const std::uint32_t auxPorts = r.u32();
    for (std::uint32_t i = 0; i < auxPorts && i < kMaxAuxPorts && r.ok(); ++i) {
        AudioPortInfo port;
        port.name = r.str();
        port.channels = r.u32();
        ports.auxOutputs.push_back(std::move(port));
    }
    if (!r.ok() || auxPorts > kMaxAuxPorts) {
        deactivate();
        return Error("host.bridge", "malformed activation reply from the plugin bridge");
    }
    if (settled) *settled = std::move(ports);
    return okStatus();
}

void RemoteInstance::deactivate() {
    (void)call(Op::Deactivate, Writer{});
    unmap();
}

std::vector<ParamInfo> RemoteInstance::params() {
    std::vector<ParamInfo> out;
    auto reply = call(Op::Params, Writer{});
    if (!reply) return out;
    Reader r(reply->data(), reply->size());
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        ParamInfo p{};
        p.id = r.u32();
        p.name = r.str();
        p.module = r.str();
        p.min = r.f64();
        p.max = r.f64();
        p.def = r.f64();
        p.flags = r.u32();
        out.push_back(std::move(p));
    }
    return out;
}

Result<double> RemoteInstance::paramValue(std::uint32_t id) {
    Writer w;
    w.u32(id);
    auto reply = call(Op::ParamValue, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).f64();
}

Result<std::string> RemoteInstance::paramText(std::uint32_t id, double value) {
    Writer w;
    w.u32(id);
    w.f64(value);
    auto reply = call(Op::ParamText, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).str();
}

Result<double> RemoteInstance::paramFromText(std::uint32_t id, std::string_view text) {
    Writer w;
    w.u32(id);
    w.str(text);
    auto reply = call(Op::ParamFromText, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).f64();
}

Status RemoteInstance::setParam(std::uint32_t id, double value) {
    Writer w;
    w.u32(id);
    w.f64(value);
    auto reply = call(Op::SetParam, w);
    if (!reply) return std::move(reply).error();
    return okStatus();
}

Result<double> RemoteInstance::normalizedParam(std::uint32_t id, double plain) {
    Writer w;
    w.u32(id);
    w.f64(plain);
    auto reply = call(Op::NormalizedParam, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).f64();
}

Result<double> RemoteInstance::plainParam(std::uint32_t id, double normalized) {
    Writer w;
    w.u32(id);
    w.f64(normalized);
    auto reply = call(Op::PlainParam, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).f64();
}

void RemoteInstance::refreshLatency() {
    auto reply = call(Op::Latency, Writer{});
    if (!reply) return;
    Reader r(reply->data(), reply->size());
    latency_ = r.u32();
    tail_ = r.u32();
    reportsTail_ = r.u8() != 0;
}

Result<std::string> RemoteInstance::text(Op op, std::optional<bool> flag) {
    Writer w;
    if (flag) w.u8(*flag ? 1 : 0);
    auto reply = call(op, w);
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).str();
}

Status RemoteInstance::setPatch(std::string_view json, std::uint32_t mode) {
    Writer w;
    w.str(json);
    w.u32(mode);
    auto reply = call(Op::SetPatch, w);
    if (!reply) return std::move(reply).error();
    return okStatus();
}

Result<std::vector<std::uint8_t>> RemoteInstance::saveState() {
    auto reply = call(Op::SaveState, Writer{});
    if (!reply) return std::move(reply).error();
    return Reader(reply->data(), reply->size()).blob();
}

Status RemoteInstance::loadState(std::span<const std::uint8_t> bytes) {
    Writer w;
    w.blob(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    auto reply = call(Op::LoadState, w);
    if (!reply) return std::move(reply).error();
    return okStatus();
}

namespace {
Result<Instance::EditorSize> editorSize(Result<std::vector<std::uint8_t>> reply) {
    if (!reply) return std::move(reply).error();
    Reader r(reply->data(), reply->size());
    Instance::EditorSize size;
    size.width = r.u32();
    size.height = r.u32();
    size.resizable = r.u8() != 0;
    if (!r.ok()) return Error("host.bridge", "malformed editor reply from the plugin bridge");
    return size;
}
}  // namespace

Result<Instance::EditorSize> RemoteInstance::openEditor(
    NativeWindow parent, double scale, std::optional<Instance::EditorFrame> frame) {
    Writer w;
    // NSView pointers are process-local: only the bridge may create its parent.
    if (nativeWindowKind() == NativeWindow::Kind::Cocoa) parent = cocoaBridgeParent(parent);
    w.u8(static_cast<std::uint8_t>(parent.kind));
    w.u64(parent.handle);
    w.f64(scale);
    writeFrame(w, frame);
    auto result = editorSize(call(Op::EditorOpen, w));
    editorOpen_ = result.ok();
    pendingEditor_ = {};
    return result;
}

Result<Instance::EditorSize> RemoteInstance::resizeEditor(std::uint32_t width,
                                                          std::uint32_t height) {
    Writer w;
    w.u32(width);
    w.u32(height);
    return editorSize(call(Op::EditorResize, w));
}

Result<Instance::EditorSize> RemoteInstance::setEditorScale(double scale) {
    Writer w;
    w.f64(scale);
    return editorSize(call(Op::EditorScale, w));
}

std::optional<Instance::EditorFrame> RemoteInstance::closeEditor() {
    editorOpen_ = false;
    pendingEditor_ = {};
    if (failed()) return {};
    auto reply = call(Op::EditorClose, Writer{}, -1, 2000);
    if (!reply) return {};
    Reader r(reply->data(), reply->size());
    auto frame = readFrame(r);
    return r.ok() ? frame : std::nullopt;
}

Instance::EditorRequests RemoteInstance::pullEditorRequests() {
    Instance::EditorRequests q;
    if (failed()) {  // the bridge is gone, and the editor with it
        q.closed = true;
        return q;
    }
    auto reply = call(Op::EditorRequests, Writer{});
    if (!reply) {
        q.closed = failed();
        return q;
    }
    Reader r(reply->data(), reply->size());
    q.resize = r.u8() != 0;
    q.closed = r.u8() != 0;
    q.width = r.u32();
    q.height = r.u32();
    q.show = r.u8() != 0;
    q.hide = r.u8() != 0;
    q.frame = readFrame(r);
    return q;
}

bool RemoteInstance::pullService() {
    auto& notes = pendingNotes_;
    auto& log = pendingLog_;
    if (failed()) {
        process_->reap();
        return false;
    }
    auto reply = call(Op::Service, Writer{});
    if (!reply) {
        notes.restartError = reply.error();
        return false;
    }
    Reader r(reply->data(), reply->size());
    const bool didWork = r.u8() != 0;
    Instance::Notifications n;
    n.stateDirty = r.u8() != 0;
    n.paramsRescan = r.u8() != 0;
    n.latencyChanged = r.u8() != 0;
    n.semanticChanged = r.u8() != 0;
    n.tailChanged = r.u8() != 0;
    n.paramsRescanFlags = r.u32();
    if (r.u8()) processRequested_.store(true, std::memory_order_release);
    const auto clears = r.u32();  // validated by call()
    for (std::uint32_t i = 0; i < clears; ++i) n.paramsClear.push_back({r.u32(), r.u32()});
    n.droppedEvents = r.u32();
    needsAudioFlush_.store(r.u8() != 0, std::memory_order_release);
    const std::uint32_t lines = r.u32();
    for (std::uint32_t i = 0; i < lines && r.ok(); ++i) log.push_back(r.str());
    latency_ = r.u32();
    tail_ = r.u32();
    reportsTail_ = r.u8() != 0;
    restartPending_ = r.u8() != 0;
    if (r.u8()) notes.restartError = r.error();
    notes.stateDirty |= n.stateDirty;
    if (n.paramsRescan) notes.paramsClear.clear();
    if (notes.paramsClear.size() + n.paramsClear.size() > bridge::kMaxParamClears) {
        notes.restartError =
            Error("bridge.notifications_limit", "too many pending parameter clears");
        return false;
    }
    notes.paramsClear.insert(notes.paramsClear.end(), n.paramsClear.begin(), n.paramsClear.end());
    notes.paramsRescan |= n.paramsRescan;
    notes.latencyChanged |= n.latencyChanged;
    notes.semanticChanged |= n.semanticChanged;
    notes.tailChanged |= n.tailChanged;
    notes.paramsRescanFlags |= n.paramsRescanFlags;
    notes.droppedEvents += n.droppedEvents;
    return didWork;
}

Instance::IdleOutputs RemoteInstance::pullIdleOutputs() {
    Instance::IdleOutputs out;
    auto reply = call(Op::TakeIdleOutputs, Writer{});
    if (!reply) return out;
    Reader r(reply->data(), reply->size());
    out.dropped = r.u64();
    const auto count = r.u32();
    out.events.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) out.events.push_back(readOutputEvent(r));
    return out;
}

void RemoteInstance::setMainThreadWake(MainThreadWake& wake) { process_->subscribe(*this, wake); }

void RemoteInstance::pullSnapshot() {
    pendingWork_ |= pullService();
    if (failed()) return;
    mergeIdleOutputs(pendingIdle_, pullIdleOutputs());
    if (editorOpen_) mergeEditorRequests(pendingEditor_, pullEditorRequests());
}

bool RemoteInstance::service(Instance::Notifications& notes, std::vector<std::string>& log) {
    if (!process_->refresh() && !wake_) pendingWork_ |= pullService();
    auto pending = std::exchange(pendingNotes_, {});
    if (!mergeNotifications(notes, pending)) {
        notes.restartError =
            Error("bridge.notifications_limit", "too many pending parameter clears", "/paramsClear",
                  "drain notifications before accumulating further parameter clears");
        pendingWork_ = false;
    }
    log.insert(log.end(), pendingLog_.begin(), pendingLog_.end());
    pendingLog_.clear();
    return std::exchange(pendingWork_, false);
}

Instance::IdleOutputs RemoteInstance::takeIdleOutputs() {
    if (!process_->refresh() && !wake_) mergeIdleOutputs(pendingIdle_, pullIdleOutputs());
    return std::exchange(pendingIdle_, {});
}

Instance::EditorRequests RemoteInstance::editorRequests() {
    if (failed()) {
        Instance::EditorRequests q;
        q.closed = true;
        return q;
    }
    if (!process_->refresh() && !wake_) mergeEditorRequests(pendingEditor_, pullEditorRequests());
    return std::exchange(pendingEditor_, {});
}

// --- data plane (audio thread) ----------------------------------------------------------

bool RemoteInstance::command(Cmd cmd) SUPPLE_RT {
    if (!hdr_ || process_->failed()) return false;
    // The sanctioned exception to the audio thread's no-syscall rule
    // (ADR-0013): waking the bridge and waiting for it, with a deadline.
    rt::ScopedSanctionedSyscall syscalls;
    hdr_->cmd = static_cast<std::uint32_t>(cmd);
    const std::uint32_t req = ++seq_;
    hdr_->request.store(req, std::memory_order_release);
#if defined(__linux__)
    futexWake(hdr_->request);
#else
    signals_.wake(hdr_->request);
#endif
    for (int spin = 0; spin < 4000; ++spin)
        if (hdr_->done.load(std::memory_order_acquire) == req) return true;
    // Lifecycle commands may page in a plugin's code; a block must not stall
    // the renderer long, but a loaded machine must not get a healthy plugin
    // killed either.
    const int limitMs = cmd == Cmd::Process ? deadlineMs_ : 5 * deadlineMs_ / 2 + 2000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limitMs);
    for (;;) {
        const std::uint32_t seen = hdr_->done.load(std::memory_order_acquire);
        if (seen == req) return true;
#if defined(__linux__)
        futexWait(hdr_->done, seen, 10);
#else
        signals_.wait(hdr_->done, seen, 10, true);
#endif
        if (hdr_->done.load(std::memory_order_acquire) == req) return true;
        if (!process_->alive()) {
            process_->fail(-1);  // the signal is read when the main thread reaps it
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            process_->fail(-2);
            process_->killBridge();
            return false;
        }
    }
}

bool RemoteInstance::startProcessing() SUPPLE_RT {
    if (hdr_) {  // published with the command's request
        const ThreadPolicy policy = bridgeAudioPolicy();
        hdr_->audioPolicy = static_cast<std::uint32_t>(policy.kind);
        hdr_->audioPriority = static_cast<std::uint32_t>(policy.priority);
    }
    if (!command(Cmd::StartProcessing)) return false;
    return hdr_->processing != 0;
}

void RemoteInstance::stopProcessing() SUPPLE_RT { command(Cmd::StopProcessing); }
void RemoteInstance::reset() SUPPLE_RT { command(Cmd::Reset); }
void RemoteInstance::flush() SUPPLE_RT { command(Cmd::Flush); }

bool RemoteInstance::process(const ProcessBlock& b, OutputEvents* out) SUPPLE_RT {
    const std::uint32_t frames = std::min(b.frames, layout_.maxFrames);
    auto silence = [&] {
        for (std::uint32_t c = 0; c < b.outputChannels; ++c)
            std::fill_n(b.outputs[c], b.frames, 0.0f);
        for (std::uint32_t c = 0; b.auxOutputs && c < b.auxOutputChannels; ++c)
            std::fill_n(b.auxOutputs[c], b.frames, 0.0f);
    };
    if (!hdr_ || process_->failed()) {
        silence();  // a failed node is silent; the session plays on
        return true;
    }
    auto* base = static_cast<std::uint8_t*>(map_);
    auto* inAudio = reinterpret_cast<float*>(base + layout_.inAudio);
    auto* outAudio = reinterpret_cast<float*>(base + layout_.outAudio);
    for (std::uint32_t c = 0; c < layout_.inChannels; ++c) {
        float* dst = inAudio + static_cast<std::size_t>(c) * layout_.maxFrames;
        if (b.inputs && c < b.inputChannels)
            std::copy_n(b.inputs[c], frames, dst);
        else
            std::fill_n(dst, frames, 0.0f);
    }
    auto* keyAudio = reinterpret_cast<float*>(base + layout_.keyAudio);
    hdr_->hasKey = b.sidechain && b.sidechainChannels > 0 && layout_.keyChannels > 0 ? 1 : 0;
    if (hdr_->hasKey)
        for (std::uint32_t c = 0; c < layout_.keyChannels; ++c) {
            float* dst = keyAudio + static_cast<std::size_t>(c) * layout_.maxFrames;
            if (c < b.sidechainChannels)
                std::copy_n(b.sidechain[c], frames, dst);
            else
                std::fill_n(dst, frames, 0.0f);
        }
    auto* events = reinterpret_cast<Event*>(base + layout_.events);
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(b.events.size(), kMaxEvents));
    std::copy_n(b.events.begin(), n, events);
    hdr_->eventCount = n;
    hdr_->frames = frames;
    hdr_->steadyTime = b.steadyTime;
    hdr_->hasTransport = b.transport ? 1 : 0;
    if (b.transport) hdr_->transport = *b.transport;
    const std::uint32_t room = out ? static_cast<std::uint32_t>(std::min<std::size_t>(
                                         out->storage.size() - out->count, kMaxOutEvents))
                                   : 0;
    hdr_->outSink = out ? 1 : 0;
    hdr_->outEventCapacity = room;
    if (!command(Cmd::Process)) {
        silence();
        return true;
    }
    // The acknowledged arena is untrusted. Snapshot once, before copying any
    // output, and fail through the same atomic path as a dead bridge.
    const std::uint32_t outEventCount = hdr_->outEventCount;
    const auto* oe = reinterpret_cast<const OutputEvent*>(base + layout_.outEvents);
    bool kindsValid = true;
    for (std::uint32_t i = 0; i < outEventCount && i < room; ++i)
        kindsValid &= static_cast<std::uint8_t>(oe[i].kind) <=
                      static_cast<std::uint8_t>(OutputEvent::Kind::Midi);
    if (outEventCount > room || !kindsValid) {
        process_->fail(BridgeProcess::kProtocolViolation);
        silence();
        return true;
    }
    for (std::uint32_t c = 0; c < b.outputChannels; ++c) {
        if (c < layout_.outChannels)
            std::copy_n(outAudio + static_cast<std::size_t>(c) * layout_.maxFrames, frames,
                        b.outputs[c]);
        else
            std::fill_n(b.outputs[c], b.frames, 0.0f);
    }
    auto* auxOutAudio = reinterpret_cast<float*>(base + layout_.auxOutAudio);
    for (std::uint32_t c = 0; b.auxOutputs && c < b.auxOutputChannels; ++c) {
        if (c < layout_.auxOutChannels)
            std::copy_n(auxOutAudio + static_cast<std::size_t>(c) * layout_.maxFrames, frames,
                        b.auxOutputs[c]);
        else
            std::fill_n(b.auxOutputs[c], b.frames, 0.0f);
    }
    if (out) {
        for (std::uint32_t i = 0; i < outEventCount; ++i) {
            if (out->count < out->storage.size())
                out->storage[out->count++] = oe[i];
            else
                ++out->dropped;
        }
        out->dropped += hdr_->outEventsDropped;
        out->unsupported += hdr_->outEventsUnsupported;
    }
    return hdr_->result != 0;
}

}  // namespace supple::host
