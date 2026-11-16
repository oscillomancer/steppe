// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// <slug>-bridge: hosts isolated fx instances (ADR-0013).
// Started by the host with its end of the control socket on fd 3 (or
// --control-fd N). Main-thread requests arrive on the socket; audio-thread
// commands arrive in per-instance shared memory sent with Activate and run on
// that instance's audio thread. Exits when the host closes the control socket.
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
// After windows.h, whose types it uses (tools/ci/windows_includes.py).
#include <avrt.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../src/bridge_notifications.h"
#include "../src/bridge_protocol.h"
#include "../src/editor_platform.h"
#include "../src/main_thread_wake.h"
#include "../src/platform.h"
#include "../src/scan.h"
#include "supple/base/rt.h"
#include "supple/base/thread.h"
#include "supple/dsp/dsp.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"

using namespace supple;
using namespace supple::host;
using namespace supple::host::bridge;

namespace {

// The class an instance's audio thread runs a command in (ADR-0026,
// ADR-0081): what the host asked for, as the header carries it.
struct AudioClass {
    std::uint32_t kind = 0;  // 0: none yet, 1: background, 2: live
    ThreadPolicy background, live;
    std::uint32_t periodNs = 0;  // live: the device period (0: unknown)
    bool operator==(const AudioClass&) const = default;
};

ThreadPolicy policyFrom(std::uint32_t kind, std::uint32_t priority) {
    return {kind <= static_cast<std::uint32_t>(ThreadPolicy::Kind::High)
                ? static_cast<ThreadPolicy::Kind>(kind)
                : ThreadPolicy::Kind::None,
            static_cast<int>(std::min(priority, 99u))};
}

// The calling thread (an instance's audio thread) takes `want`; false when it
// was refused, the thread then keeping what it could. The live class is the
// callback's: on Linux its policy and priority; on macOS a time-constraint
// policy for the device period, or without one a block of `frames` at the
// activation's `rate` (the device helpers' rule, ADR-0038); on Windows MMCSS
// Pro Audio at critical priority. Setup calls, made only when the class
// changes.
bool takeAudioClass(const AudioClass& want, double rate, std::uint32_t frames) {
#if defined(_WIN32)
    static thread_local HANDLE mmcss = nullptr;
    const auto thread = GetCurrentThread();
    if (want.kind == 2) {
        (void)rate;
        (void)frames;
        DWORD index = 0;
        if (!mmcss) mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        return mmcss && AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    mmcss = nullptr;
#elif defined(__APPLE__)
    const auto thread = ::pthread_self();
    const auto mach = pthread_mach_thread_np(thread);
    if (want.kind == 2) {
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);
        const double ticks =
            liveClassPeriodNs(want.periodNs, frames, rate) * timebase.denom / timebase.numer;
        const auto period = static_cast<std::uint32_t>(std::clamp(ticks, 1.0, double(UINT32_MAX)));
        thread_time_constraint_policy_data_t policy{};
        policy.period = period;
        policy.computation = std::max<std::uint32_t>(1, period / 3);
        policy.constraint = std::max(policy.computation, period / 2);
        policy.preemptible = TRUE;
        if (thread_policy_set(mach, THREAD_TIME_CONSTRAINT_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
            return false;
        // Read back, as Linux does: the thread has the constraint asked for.
        thread_time_constraint_policy_data_t got{};
        mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        boolean_t isDefault = FALSE;
        return thread_policy_get(mach, THREAD_TIME_CONSTRAINT_POLICY,
                                 reinterpret_cast<thread_policy_t>(&got), &count,
                                 &isDefault) == KERN_SUCCESS &&
               !isDefault && got.period == policy.period;
    }
    (void)thread_policy_set(mach, THREAD_STANDARD_POLICY, nullptr, THREAD_STANDARD_POLICY_COUNT);
#else
    (void)rate;
    (void)frames;
    const auto thread = ::pthread_self();
    if (want.kind == 2) {
        if (want.live.kind == ThreadPolicy::Kind::None) return true;
        return setThreadPolicy(thread, want.live) && threadPolicy(thread) == want.live;
    }
#endif
    if (want.background.kind == ThreadPolicy::Kind::None) return true;
    return static_cast<bool>(setThreadPolicy(thread, want.background));
}

// The steady clock, as the host reads it (one clock for both processes).
std::int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The CPU the calling thread runs on, -1 where the platform does not say.
std::int32_t currentCpu() {
#if defined(_WIN32)
    return static_cast<std::int32_t>(GetCurrentProcessorNumber());
#elif defined(__linux__)
    return static_cast<std::int32_t>(sched_getcpu());
#else
    return -1;
#endif
}

// The calling thread's class as the platform reports it after taking one.
ThreadPolicy appliedClass() {
#if defined(_WIN32)
    return threadPolicy(GetCurrentThread());
#elif defined(__APPLE__)
    // The live class is a Mach time-constraint policy, which the QoS that
    // threadPolicy() reads does not show.
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t isDefault = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(::pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&policy), &count,
                          &isDefault) == KERN_SUCCESS &&
        !isDefault && policy.period > 0)
        return {ThreadPolicy::Kind::High, 0};
    return threadPolicy(::pthread_self());
#else
    return threadPolicy(::pthread_self());
#endif
}

struct BridgeInstance {
    std::unique_ptr<Instance> instance;
    MainThreadWake* wake = nullptr;
#if defined(__APPLE__)
    std::unique_ptr<FloatingEditorWindow> floating;
    Instance::EditorSize floatingSize;
    std::optional<Instance::EditorFrame> reportedFrame;  // the last one sent to the DAW
#endif
    Instance::Notifications notes;
    Instance::IdleOutputs idle;
    Instance::EditorRequests editor;
    std::vector<std::string> log;
    bool processRequested = false, clearOverflow = false;
    std::uint32_t latency = 0, tail = 0;
    bool reportsTail = false, restartPending = false, needsFlush = false;

    // Main only, under lifecycle: consuming the local queues is a new
    // publication, whereas draining these retained snapshots is not.
    bool collect() {
        auto n = instance->takeNotifications();
        auto outputs = instance->takeIdleOutputs();
        auto gui = instance->takeEditorRequests();
#if defined(__APPLE__)
        if (floating) {
            if (floating->closed() || gui.closed) {
                const auto last = floating->frame();
                instance->closeEditor();
                floating.reset();
                reportedFrame.reset();
                gui = {};
                gui.closed = true;
                gui.frame = last;  // where the user left it
            } else {
                unsigned width = 0, height = 0;
                if (floating->takeResize(width, height)) {
                    if (auto size = instance->resizeEditor(width, height)) {
                        floatingSize = *size;
                        floating->resize(size->width, size->height, size->resizable);
                    }
                }
                if (gui.resize) {
                    floatingSize.width = gui.width;
                    floatingSize.height = gui.height;
                    floating->resize(gui.width, gui.height, floatingSize.resizable);
                }
                if (gui.show) floating->show();
                if (gui.hide) floating->hide();
                // Geometry belongs to the child window, not a DAW placeholder.
                gui.resize = gui.show = gui.hide = false;
                // The DAW remembers where the window is: report moves and resizes.
                if (const auto now = floating->frame(); now != reportedFrame) {
                    reportedFrame = now;
                    gui.frame = now;
                }
            }
        }
#endif
        auto lines = instance->takeLog();
        const bool process = instance->takeProcessRequest();
        const auto newLatency = instance->latency(), newTail = instance->tail();
        const bool newReports = instance->reportsTail(), newRestart = instance->restartPending(),
                   newFlush = instance->needsAudioFlush();
        const bool changed = hasNotifications(n) || !outputs.events.empty() || outputs.dropped ||
                             hasEditorRequests(gui) || !lines.empty() || process ||
                             newLatency != latency || newTail != tail ||
                             newReports != reportsTail || newRestart != restartPending ||
                             newFlush != needsFlush;
        if (!mergeNotifications(notes, n)) clearOverflow = true;
        mergeIdleOutputs(idle, outputs);
        mergeEditorRequests(editor, gui);
        log.insert(log.end(), lines.begin(), lines.end());
        processRequested |= process;
        latency = newLatency;
        tail = newTail;
        reportsTail = newReports;
        restartPending = newRestart;
        needsFlush = newFlush;
        return changed;
    }

    void* map = nullptr;
    std::size_t mapSize = 0;
    Header* hdr = nullptr;
    SharedSignals signals;
    Layout layout{0, 0, 0};
    double rate = 0;  // of the current activation
    std::thread audio;
    std::atomic<bool> quit{false};
    // Serializes the plugin's lifecycle: the main thread's servicing (which
    // may restart the plugin: deactivate + activate) against the audio
    // thread's start/stop/reset/flush commands. process() takes no lock: it
    // only runs between start and stop, when no restart can happen.
    std::mutex lifecycle;

    // --- the audio thread ---------------------------------------------------------------

    // `last`: the request sequence when Activate was handled — taken on the
    // main thread before its reply, so a command the host sends right after
    // is never mistaken for one already served, however late this starts.
    void audioLoop(std::uint32_t last) {
        auto* base = static_cast<std::uint8_t*>(map);
        auto* events = reinterpret_cast<const Event*>(base + layout.events);
        auto* outEvents = reinterpret_cast<OutputEvent*>(base + layout.outEvents);
        auto* inAudio = reinterpret_cast<float*>(base + layout.inAudio);
        auto* outAudio = reinterpret_cast<float*>(base + layout.outAudio);
        auto* keyAudio = reinterpret_cast<float*>(base + layout.keyAudio);
        auto* auxOutAudio = reinterpret_cast<float*>(base + layout.auxOutAudio);
        const float* in[kMaxChannels];
        const float* key[kMaxChannels];
        float* out[kMaxChannels];
        float* auxOut[kMaxAuxChannels];
        for (std::uint32_t c = 0; c < layout.auxOutChannels; ++c)
            auxOut[c] = auxOutAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        for (std::uint32_t c = 0; c < layout.keyChannels; ++c)
            key[c] = keyAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        for (std::uint32_t c = 0; c < layout.inChannels; ++c)
            in[c] = inAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        for (std::uint32_t c = 0; c < layout.outChannels; ++c)
            out[c] = outAudio + static_cast<std::size_t>(c) * layout.maxFrames;
        // Created by the bridge's main thread: normal until told otherwise.
        AudioClass taken;
        bool refused = false;
        ThreadPolicy applied{ThreadPolicy::Kind::Normal, 0};
        std::int32_t lastCpu = -1;
        std::uint32_t migrations = 0;
        while (!quit.load()) {
            const std::uint32_t req = hdr->request.load(std::memory_order_acquire);
            if (req == last) {
#if defined(__linux__)
                futexWait(hdr->request, last, -1);
#else
                signals.wait(hdr->request, last, -1);
#endif
                continue;
            }
            if (quit.load(std::memory_order_acquire)) break;
            last = req;
            hdr->seenNs = steadyNs();
            const std::int32_t cpu = currentCpu();
            if (cpu >= 0 && lastCpu >= 0 && cpu != lastCpu) ++migrations;
            if (cpu >= 0) lastCpu = cpu;
            const auto command = static_cast<Cmd>(hdr->cmd);
            // The class the host asks for with this command, taken before it
            // runs, only when it changed (best effort: a refusal keeps the
            // thread as it was, and the answer says so).
            if (hdr->audioClass == 1 || hdr->audioClass == 2) {
                // The live class's period is the device's, not the
                // activation's capacity (render-ahead blocks are larger);
                // a change of it is a change of class.
                const AudioClass want{hdr->audioClass,
                                      policyFrom(hdr->audioPolicy, hdr->audioPriority),
                                      policyFrom(hdr->livePolicy, hdr->livePriority),
                                      hdr->audioClass == 2 ? hdr->livePeriodNs : 0};
                if (want != taken) {
                    refused = !takeAudioClass(
                        want, rate, command == Cmd::Process ? hdr->frames : layout.maxFrames);
                    taken = want;
                    applied = appliedClass();
                }
            }
            bool hadFlush = false;
            std::uint32_t result = 1;
            switch (command) {
                case Cmd::Process: {
                    // Lifecycle state is stable between start and stop. Other
                    // commands take lifecycle below and always wake main.
                    hadFlush = instance->needsAudioFlush();
                    ProcessBlock b;
                    b.inputs = layout.inChannels ? in : nullptr;
                    b.inputChannels = layout.inChannels;
                    b.outputs = out;
                    b.outputChannels = layout.outChannels;
                    if (layout.auxOutChannels) {
                        b.auxOutputs = auxOut;
                        b.auxOutputChannels = layout.auxOutChannels;
                    }
                    if (hdr->hasKey && layout.keyChannels) {
                        b.sidechain = key;
                        b.sidechainChannels = layout.keyChannels;
                    }
                    b.frames = std::min(hdr->frames, layout.maxFrames);
                    b.events =
                        std::span<const Event>(events, std::min(hdr->eventCount, kMaxEvents));
                    b.transport = hdr->hasTransport ? &hdr->transport : nullptr;
                    b.steadyTime = hdr->steadyTime;
                    // The host sink's room, so overflow happens where it would
                    // in process: the same events kept, dropped and counted.
                    OutputEvents sink{std::span<OutputEvent>(
                        outEvents, std::min(hdr->outEventCapacity, kMaxOutEvents))};
                    {
                        // The engine's environment for every node (ADR-0025).
                        dsp::DenormalGuard denormals;
                        result = instance->process(b, hdr->outSink ? &sink : nullptr) ? 1 : 0;
                    }
                    hdr->outEventCount = static_cast<std::uint32_t>(sink.count);
                    hdr->outEventsDropped = static_cast<std::uint32_t>(sink.dropped);
                    hdr->outEventsUnsupported = static_cast<std::uint32_t>(sink.unsupported);
                    break;
                }
                case Cmd::StartProcessing: {
                    std::lock_guard lock(lifecycle);
                    result = instance->startProcessing() ? 1 : 0;
                    break;
                }
                case Cmd::StopProcessing: {
                    std::lock_guard lock(lifecycle);
                    instance->stopProcessing();
                    break;
                }
                case Cmd::Reset: {
                    std::lock_guard lock(lifecycle);
                    instance->reset();
                    break;
                }
                case Cmd::Flush: {
                    std::lock_guard lock(lifecycle);
                    instance->flushParams();
                    break;
                }
                case Cmd::Quit:
                    quit = true;
                    break;
            }
            hdr->result = result;
            hdr->processing = instance->processing() ? 1 : 0;
            hdr->classRefused = refused ? 1 : 0;
            hdr->cpu = cpu;
            hdr->migrations = migrations;
            hdr->appliedPolicy = static_cast<std::uint32_t>(applied.kind);
            hdr->appliedPriority = static_cast<std::uint32_t>(std::max(applied.priority, 0));
            hdr->finishedNs = steadyNs();
            hdr->done.store(req, std::memory_order_release);
#if defined(__linux__)
            futexWake(hdr->done);
#else
            signals.wake(hdr->done, true);
#endif
            if (wake && (command != Cmd::Process || hadFlush)) wake->notify();
        }
    }

    void stopAudio() {
        if (!audio.joinable()) return;
        requestAudioQuit(quit, hdr->request, signals);
        audio.join();
        quit = false;
    }

    void unmap() {
        unmapMemory(map, mapSize);
        signals.clear();
        map = nullptr;
        hdr = nullptr;
    }
};

struct Bridge {
#if defined(_WIN32)
    std::unique_ptr<NativeControl> control;
    Control sock = noControl;
#else
    Control sock = kControlFd;
#endif
    MainThreadWake wake;  // outlives every instance and its callback threads
    Resource outward = noResource;
    NotificationLatch notifications;
    ~Bridge() {
        if (outward != noResource) closeResource(outward);
    }
    void collect(BridgeInstance& slot) {
        if (slot.collect()) notifications.publish();
    }
    bool announce() {
        if (outward < 0 || !notifications.announce()) return true;
        return signalNotification(outward);
    }
    std::map<std::uint32_t, std::unique_ptr<BridgeInstance>> instances;
    std::uint32_t nextId = 1;
    std::uint32_t replyId = 0;

    // --- the control plane ---------------------------------------------------------------

    bool reply(const Writer& w) {
        return sendFrame(sock, static_cast<std::uint32_t>(Reply::Ok), replyId, w.bytes);
    }
    bool fail(const Error& e) {
        Writer w;
        w.error(e);
        return sendFrame(sock, static_cast<std::uint32_t>(Reply::Error), replyId, w.bytes);
    }
    template <class T>
    bool replyOr(const Result<T>& r, auto&& write) {
        if (!r) return fail(r.error());
        Writer w;
        write(w, *r);
        return reply(w);
    }
    bool replyStatus(const supple::Status& st) { return st ? reply(Writer{}) : fail(st.error()); }
    static void writeSize(Writer& w, const Instance::EditorSize& size) {
        w.u32(size.width);
        w.u32(size.height);
        w.u8(size.resizable ? 1 : 0);
    }

    // Returns false to exit.
    bool handle(Op op, std::uint32_t instanceId, Reader& r, Resource fd, int descriptorCount = -1) {
        replyId = instanceId;
        if (!validRequest(op, r)) {
            if (fd >= 0) closeResource(fd);
            return fail(Error("bridge.malformed", "malformed request to the plugin bridge",
                              "/payload",
                              "send the exact payload defined for this bridge operation"));
        }
        if (descriptorCount < 0) descriptorCount = fd >= 0 ? 1 : 0;
        if (descriptorCount > 1) {
            if (fd >= 0) closeResource(fd);
            return fail(Error("bridge.fd", "exactly one descriptor is allowed", "/fd",
                              "send only the descriptor required by the operation"));
        }
        if (op == Op::SubscribeNotifications) {
            const auto version = r.u32();
            if (instanceId || outward >= 0 || version != kVersion || fd < 0) {
                if (fd >= 0) closeResource(fd);
                return fail(Error("bridge.subscription", "invalid notification subscription",
                                  "/version",
                                  "subscribe once with instance 0, matching protocol version and a "
                                  "native wake source"));
            }
            if (!validNotification(fd)) {
                closeResource(fd);
                return fail(Error("bridge.fd", "invalid native notification resource", "/fd",
                                  "send the signal target of a dedicated nonblocking WakeEvent"));
            }
            outward = fd;
            notifications.acknowledge();
            Writer w;
            w.u32(kVersion);
            return reply(w);
        }
        if (fd >= 0 && op != Op::Activate) {
            closeResource(fd);
            return fail(Error("bridge.fd", "unexpected descriptor for this operation", "/fd",
                              "send descriptors only with Activate or SubscribeNotifications"));
        }
        if (op == Op::AckNotifications) {
            if (instanceId || outward < 0)
                return fail(Error("bridge.subscription", "ACK requires a subscribed process",
                                  "/instance",
                                  "subscribe first, then acknowledge with instance 0"));
            notifications.acknowledge();
            return reply(Writer{});
        }
        if (outward < 0) {
            if (fd >= 0) closeResource(fd);
            return fail(
                Error("bridge.subscription", "notification subscription is required", "/version",
                      "subscribe with the matching bridge protocol before creating instances"));
        }
        if (op == Op::Create) {
            if (instanceId != 0)
                return fail(Error("bridge.instance", "Create requires instance id 0"));
            PluginInfo info;
            info.path = platform::pathFromText(r.str());
            info.id = r.str();
            info.format = r.str();
            HostIdentity identity;
            identity.name = r.str();
            identity.vendor = r.str();
            identity.url = r.str();
            identity.version = r.str();
            if (!r.ok()) return fail(Error("bridge.create", "malformed Create request"));
            auto made = Instance::create(info, identity);
            if (!made) return fail(made.error());
            auto slot = std::make_unique<BridgeInstance>();
            slot->instance = std::move(*made);
            slot->instance->setMainThreadWake(wake);
            slot->wake = &wake;
            const std::uint32_t newId = nextId++;
            Writer w;
            w.u32(newId);
            w.u32(slot->instance->audioInputChannels());
            w.u32(slot->instance->audioOutputChannels());
            w.u8(slot->instance->acceptsNotes() ? 1 : 0);
            w.u8(slot->instance->info().semantic ? 1 : 0);
            w.u32(slot->instance->sidechainChannels());
            w.u8(slot->instance->hasEditor() ? 1 : 0);
            w.u8(slot->instance->outputsNotes() ? 1 : 0);
            const auto& aux = slot->instance->auxOutputPorts();
            const auto ports = std::min<std::size_t>(aux.size(), kMaxAuxPorts);
            w.u32(static_cast<std::uint32_t>(ports));
            for (std::size_t i = 0; i < ports; ++i) {
                w.str(aux[i].name);
                w.u32(aux[i].channels);
            }
            instances.emplace(newId, std::move(slot));
            return reply(w);
        }
        auto it = instances.find(instanceId);
        if (it == instances.end()) {
            if (fd >= 0) closeResource(fd);
            return fail(Error("bridge.no_instance", "unknown instance id"));
        }
        BridgeInstance& slot = *it->second;
        auto& instance = slot.instance;
        auto& map = slot.map;
        auto& mapSize = slot.mapSize;
        auto& hdr = slot.hdr;
        auto& layout = slot.layout;
        auto& audio = slot.audio;
        auto& lifecycle = slot.lifecycle;
        auto stopAudio = [&] { slot.stopAudio(); };
        auto unmap = [&] { slot.unmap(); };
        switch (op) {
            case Op::Create:
            case Op::SubscribeNotifications:
            case Op::AckNotifications:
                break;
            case Op::Activate: {
                const double rate = r.f64();
                const std::uint32_t minFrames = r.u32(), maxFrames = r.u32(), size = r.u32();
                if (fd < 0) return fail(Error("bridge.activate", "no shared memory"));
#if defined(_WIN32)
                const auto requestEvent = static_cast<Resource>(r.u64());
                const auto doneEvent = static_cast<Resource>(r.u64());
#endif
                if (!r.ok() || size < sizeof(Header)) {
                    closeResource(fd);
#if defined(_WIN32)
                    closeResource(requestEvent);
                    closeResource(doneEvent);
#endif
                    return fail(Error("bridge.activate", "invalid shared memory size or request"));
                }
                stopAudio();
                unmap();
#if defined(_WIN32)
                if (!slot.signals.adopt(requestEvent, doneEvent)) {
                    closeResource(fd);
                    return fail(Error("bridge.activate", "invalid shared audio signal handles",
                                      "/events",
                                      "send a distinct request/done auto-reset event pair"));
                }
#endif
                map = mapMemory(fd, size);
                closeResource(fd);
                if (!map) {
                    unmap();
                    return fail(Error("bridge.activate", "could not map the shared memory"));
                }
                mapSize = size;
                hdr = static_cast<Header*>(map);
                layout = Layout(hdr->maxFrames, hdr->inChannels, hdr->outChannels, hdr->keyChannels,
                                hdr->auxOutChannels);
                if (hdr->magic != kMagic || hdr->version != kVersion || layout.size > size ||
                    hdr->maxFrames != maxFrames || maxFrames == 0 ||
                    hdr->inChannels > kMaxChannels || hdr->outChannels > kMaxChannels ||
                    hdr->keyChannels > kMaxChannels || hdr->auxOutChannels > kMaxAuxChannels) {
                    unmap();
                    return fail(Error("bridge.activate", "shared memory layout mismatch"));
                }
                (void)instance->setSidechainActive(hdr->keyActive != 0);
                auto st = instance->activate(rate, minFrames, maxFrames);
                if (!st) {
                    unmap();
                    return fail(st.error());
                }
                slot.rate = rate;
                const std::uint32_t first = hdr->request.load(std::memory_order_acquire);
                audio = std::thread([&slot, first] {
                    nameThread("audio-bridge");
                    rt::prefaultStack();  // ADR-0025
                    slot.audioLoop(first);
                });
                // The widths the plug-in settled on (a VST3 may adapt a layout
                // at activation): the host re-maps when they differ.
                Writer w;
                w.u32(instance->audioInputChannels());
                w.u32(instance->audioOutputChannels());
                w.u32(instance->sidechainChannels());
                const auto& aux = instance->auxOutputPorts();
                const auto ports = std::min<std::size_t>(aux.size(), kMaxAuxPorts);
                w.u32(static_cast<std::uint32_t>(ports));
                for (std::size_t i = 0; i < ports; ++i) {
                    w.str(aux[i].name);
                    w.u32(aux[i].channels);
                }
                return reply(w);
            }
            case Op::Deactivate:
                stopAudio();
                // A start answered after the host's live deadline passed may
                // have left it processing (ADR-0081); this thread is its audio
                // thread now that that one has ended.
                if (instance->processing()) instance->stopProcessing();
                instance->deactivate();
                unmap();
                return reply(Writer{});
            case Op::Params: {
                const auto params = instance->params();
                Writer w;
                w.u32(static_cast<std::uint32_t>(params.size()));
                for (const ParamInfo& p : params) {
                    w.u32(p.id);
                    w.str(p.name);
                    w.str(p.module);
                    w.f64(p.min);
                    w.f64(p.max);
                    w.f64(p.def);
                    w.u32(p.flags);
                }
                return reply(w);
            }
            case Op::ParamValue:
                return replyOr(instance->paramValue(r.u32()),
                               [](Writer& w, double v) { w.f64(v); });
            case Op::ParamText: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->paramText(id, r.f64()),
                               [](Writer& w, const std::string& s) { w.str(s); });
            }
            case Op::ParamFromText: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->paramFromText(id, r.str()),
                               [](Writer& w, double v) { w.f64(v); });
            }
            case Op::SetParam: {
                const std::uint32_t id = r.u32();
                return replyStatus(instance->setParam(id, r.f64()));
            }
            case Op::NormalizedParam: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->normalizedParam(id, r.f64()),
                               [](Writer& w, double v) { w.f64(v); });
            }
            case Op::PlainParam: {
                const std::uint32_t id = r.u32();
                return replyOr(instance->plainParam(id, r.f64()),
                               [](Writer& w, double v) { w.f64(v); });
            }
            case Op::Latency: {
                Writer w;
                w.u32(instance->latency());
                w.u32(instance->tail());
                w.u8(instance->reportsTail() ? 1 : 0);
                return reply(w);
            }
            case Op::SemanticSchema:
                return replyOr(instance->semanticSchema(), [](Writer& w, const json::Value& v) {
                    w.str(json::writeCompact(v));
                });
            case Op::PluginStatus:
                return replyOr(instance->pluginStatus(), [](Writer& w, const json::Value& v) {
                    w.str(json::writeCompact(v));
                });
            case Op::FactoryPatches:
                return replyOr(instance->factoryPatches(), [](Writer& w, const json::Value& v) {
                    w.str(json::writeCanonical(v));
                });
            case Op::GetPatch:
                return replyOr(instance->getPatch(r.u8() != 0),
                               [](Writer& w, const std::string& s) { w.str(s); });
            case Op::SetPatch: {
                const std::string text = r.str();
                return replyStatus(instance->setPatch(text, r.u32()));
            }
            case Op::Describe:
                return replyOr(instance->describe(r.u8() != 0),
                               [](Writer& w, const std::string& s) { w.str(s); });
            case Op::SaveState:
                return replyOr(instance->saveState(),
                               [](Writer& w, const std::vector<std::uint8_t>& b) { w.blob(b); });
            case Op::TakeIdleOutputs: {
                collect(slot);
                const auto out = std::exchange(slot.idle, {});
                Writer w;
                w.u64(out.dropped);
                w.u32(static_cast<std::uint32_t>(out.events.size()));
                for (const auto& event : out.events) writeOutputEvent(w, event);
                return reply(w);
            }
            case Op::LoadState: {
                const auto bytes = r.blob();
                return replyStatus(instance->loadState(bytes));
            }
            case Op::Service: {
                bool did = false;
                {
                    std::lock_guard lock(lifecycle);
                    did = instance->serviceMainThread();
                    collect(slot);
                }
                const auto n = std::exchange(slot.notes, {});
                const auto log = std::exchange(slot.log, {});
                Writer w;
                w.u8(did ? 1 : 0);
                w.u8(n.stateDirty ? 1 : 0);
                w.u8(n.paramsRescan ? 1 : 0);
                w.u8(n.latencyChanged ? 1 : 0);
                w.u8(n.semanticChanged ? 1 : 0);
                w.u8(n.tailChanged ? 1 : 0);
                w.u32(n.paramsRescanFlags);
                w.u8(std::exchange(slot.processRequested, false) ? 1 : 0);
                if (std::exchange(slot.clearOverflow, false) ||
                    n.paramsClear.size() > kMaxParamClears)
                    return fail(Error(
                        "bridge.notifications_limit", "too many parameter clears", "/paramsClear",
                        "drain Service snapshots before accumulating further clears"));
                w.u32(static_cast<std::uint32_t>(n.paramsClear.size()));
                for (const auto& clear : n.paramsClear) {
                    w.u32(clear.id);
                    w.u32(clear.flags);
                }
                w.u32(n.droppedEvents);
                w.u8(instance->needsAudioFlush() ? 1 : 0);
                w.u32(static_cast<std::uint32_t>(log.size()));
                for (const auto& line : log) w.str(line);
                w.u32(instance->latency());  // a restart may have changed them silently
                w.u32(instance->tail());
                w.u8(instance->reportsTail() ? 1 : 0);
                w.u8(instance->restartPending() ? 1 : 0);  // waits for processing to stop
                w.u8(n.restartError ? 1 : 0);
                if (n.restartError) w.error(*n.restartError);
                return reply(w);
            }
            case Op::EditorOpen: {
                const auto kind = static_cast<NativeWindow::Kind>(r.u8());
                const auto handle = r.u64();
                const double scale = r.f64();
                [[maybe_unused]] const auto frame = readFrame(r);  // Cocoa windows only
                if (!r.ok())
                    return fail(Error("bridge.editor", "malformed EditorOpen request", "/editor",
                                      "use a matching host and bridge build"));
                NativeWindow parent{kind, static_cast<std::uintptr_t>(handle)};
#if defined(__APPLE__)
                if (!validFloatingEditorParent(parent))
                    return fail(Error("host.editor.parent",
                                      "a bridged Cocoa editor requires its own window",
                                      "/editor/parent",
                                      "pass Floating with a 32-bit owner window number or zero"));
                if (slot.floating) {
                    slot.floating->showAbove(static_cast<std::uint32_t>(handle));
                    slot.editor = {};
                    return replyOr(Result<Instance::EditorSize>(slot.floatingSize), writeSize);
                }
                auto window = FloatingEditorWindow::create(instance->info().name);
                if (!window) return fail(window.error());
                parent = (*window)->parent();
#endif
                auto size = instance->openEditor(parent, scale);
                // A successful open starts a new view lifetime. Retained close
                // requests from its predecessor must not close the new window.
                if (size) slot.editor = {};
#if defined(__APPLE__)
                if (size) {
                    // Where the DAW remembered the window, and at its size when
                    // the editor is resizable (the plug-in may adjust it).
                    if (frame && size->resizable && frame->width > 0 && frame->height > 0)
                        if (auto accepted = instance->resizeEditor(frame->width, frame->height))
                            *size = *accepted;
                    (*window)->resize(size->width, size->height, size->resizable);
                    if (frame) (*window)->moveTo(frame->x, frame->y);
                    (*window)->showAbove(static_cast<std::uint32_t>(handle));
                    slot.floatingSize = *size;
                    slot.reportedFrame.reset();  // the first service reports where it is
                    slot.floating = std::move(*window);
                }
#endif
                return replyOr(std::move(size), writeSize);
            }
            case Op::EditorResize: {
                const std::uint32_t width = r.u32(), height = r.u32();
                auto size = instance->resizeEditor(width, height);
#if defined(__APPLE__)
                if (size && slot.floating) {
                    slot.floatingSize = *size;
                    slot.floating->resize(size->width, size->height, size->resizable);
                }
#endif
                return replyOr(std::move(size), writeSize);
            }
            case Op::EditorScale: {
                auto size = instance->setEditorScale(r.f64());
#if defined(__APPLE__)
                if (size && slot.floating) {
                    slot.floatingSize = *size;
                    slot.floating->resize(size->width, size->height, size->resizable);
                }
#endif
                return replyOr(std::move(size), writeSize);
            }
            case Op::EditorClose: {
                Writer w;
                std::optional<Instance::EditorFrame> last;
#if defined(__APPLE__)
                if (slot.floating) last = slot.floating->frame();
#endif
                instance->closeEditor();
#if defined(__APPLE__)
                slot.floating.reset();
                slot.reportedFrame.reset();
#endif
                writeFrame(w, last);
                return reply(w);
            }
            case Op::EditorRequests: {
                collect(slot);
                const auto q = std::exchange(slot.editor, {});
                Writer w;
                w.u8(q.resize ? 1 : 0);
                w.u8(q.closed ? 1 : 0);
                w.u32(q.width);
                w.u32(q.height);
                w.u8(q.show ? 1 : 0);
                w.u8(q.hide ? 1 : 0);
                writeFrame(w, q.frame);
                return reply(w);
            }
            case Op::Destroy:
                stopAudio();
                instance->closeEditor();
                if (instance->processing()) instance->stopProcessing();
                instance.reset();
                unmap();
                reply(Writer{});
                instances.erase(it);
                return true;  // an idle pool-owned bridge must accept the next Create
        }
        return fail(Error("bridge.op", "unknown request"));
    }
};

}  // namespace

#if defined(_WIN32)
int bridgeMain(int argc, char** argv) {
    if (auto initialized = initializeEditorPlatform(); !initialized) {
        std::fprintf(stderr, "%s\n", initialized.error().message.c_str());
        return 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--scan") {
        // Preserve binary protocol bytes while plugin printf/cout goes to NUL.
        const int saved = _dup(_fileno(stdout));
        if (saved < 0) return 1;
        if (!SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(saved)),
                                  HANDLE_FLAG_INHERIT, 0) ||
            _setmode(saved, _O_BINARY) < 0) {
            _close(saved);
            return 1;
        }
        FILE* output = _fdopen(saved, "wb");
        if (!output) {
            _close(saved);
            return 1;
        }
        FILE* ignored = nullptr;
        if (freopen_s(&ignored, "NUL", "w", stdout) != 0) {
            std::fclose(output);
            return 1;
        }
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
        std::error_code ec;
        const auto path =
            std::filesystem::absolute(platform::pathFromText(argv[2]), ec).lexically_normal();
        const Result<std::vector<PluginInfo>> result =
            ec ? Result<std::vector<PluginInfo>>(Error("host.scan.load",
                                                       "cannot resolve plugin path", "/path",
                                                       "check the module path"))
               : scanModuleInProcess(path);
        const auto text = json::writeCanonical(scanReply(result));
        const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
        const bool closed = std::fclose(output) == 0;
        return written && closed && result ? 0 : 1;
    }
#else
int main(int argc, char** argv) {
    if (auto initialized = initializeEditorPlatform(); !initialized) {
        std::fprintf(stderr, "%s\n", initialized.error().message.c_str());
        return 1;
    }
#endif
#if !defined(_WIN32)
    ::signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    // As the engine keeps the host (ADR-0084): out of transparent huge pages
    // but where a plug-in advised them (Linux 6.18), or out entirely. A bridge
    // started before the engine did inherits nothing; one that inherited a
    // setting (the host's, or its launcher's stricter one) keeps it. Best
    // effort.
    if (::prctl(PR_GET_THP_DISABLE, 0UL, 0UL, 0UL, 0UL) == 0 &&
        ::prctl(PR_SET_THP_DISABLE, 1UL, 1UL << 1, 0UL, 0UL) != 0)
        (void)::prctl(PR_SET_THP_DISABLE, 1UL, 0UL, 0UL, 0UL);
#endif
    if (argc == 3 && std::string_view(argv[1]) == "--scan") {
        // Keep plugin printf/cout (including background threads and unload
        // destructors) away from the protocol stream. Only our saved fd writes it.
        const int output = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (output < 0 || null < 0 || ::dup2(null, STDOUT_FILENO) < 0) {
            if (output >= 0) ::close(output);
            if (null >= 0) ::close(null);
            return 1;
        }
        ::close(null);
        std::error_code ec;
        const auto path = std::filesystem::absolute(argv[2], ec).lexically_normal();
        const Result<std::vector<PluginInfo>> result =
            ec ? Result<std::vector<PluginInfo>>(
                     Error("host.scan.load", "cannot resolve plugin path"))
               : scanModuleInProcess(path);
        const auto text = json::writeCanonical(scanReply(result));
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto n = ::write(output, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ::close(output);
                return 1;
            }
            offset += static_cast<std::size_t>(n);
        }
        ::close(output);
        return result ? 0 : 1;
    }
#endif
    Bridge b;
    for (int i = 1; i + 1 < argc; ++i)
#if defined(_WIN32)
        if (std::string_view(argv[i]) == "--control-handle") {
            const auto handle = reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(std::strtoull(argv[i + 1], nullptr, 10)));
            b.control = std::make_unique<NativeControl>(handle, true);
            b.sock = b.control.get();
        }
#else
        if (std::string_view(argv[i]) == "--control-fd") b.sock = std::atoi(argv[i + 1]);
#endif
#if defined(_WIN32)
    if (!b.sock) return 1;
#endif
    if (!b.wake.waitable().valid()) {
        std::perror("bridge main-thread eventfd");
        return 1;
    }
    std::vector<WaitItem> polled;
    std::vector<std::uint32_t> owners;  // the instance of each editor descriptor
    for (;;) {
#if defined(__APPLE__)
        @autoreleasepool {
#endif
            // Clear the coalesced wake before consuming work: a request made
            // during servicing must schedule another pass. Servicing before every
            // poll also prevents a busy control socket from starving callbacks.
            (void)b.wake.drain();
            for (auto& [id, slot] : b.instances) {
                (void)id;
                std::lock_guard lock(slot->lifecycle);
                (void)slot->instance->serviceMainThread();
                b.collect(*slot);
            }
            if (!b.announce()) break;
            // Timers and descriptors may be registered without an open editor.
            // Host death arrives as control-socket HUP; no periodic probe is needed.
            polled = {{controlWaitable(b.sock), kWaitRead, 0}, {b.wake.waitable(), kWaitRead, 0}};
            owners.assign(2, 0);
            int timeout = -1;
            for (auto& [id, slot] : b.instances) {
                for (const auto& [fd, flags] : slot->instance->watchedFds()) {
                    short events = 0;
                    if (flags & CLAP_POSIX_FD_READ) events |= kWaitRead;
                    if (flags & CLAP_POSIX_FD_WRITE) events |= kWaitWrite;
                    polled.push_back(WaitItem{fd, events, 0});
                    owners.push_back(id);
                }
                timeout = nearestTimeout(timeout, slot->instance->nextTimerMs());
            }
            const int ready = waitEditorEvents(polled, timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;  // rebuild deadlines and service work
                break;
            }
            for (std::size_t i = 2; ready > 0 && i < polled.size(); ++i) {
                if (!polled[i].ready) continue;
                const auto it = b.instances.find(owners[i]);
                if (it == b.instances.end()) continue;
                std::uint32_t flags = 0;
                if (polled[i].ready & kWaitRead) flags |= CLAP_POSIX_FD_READ;
                if (polled[i].ready & kWaitWrite) flags |= CLAP_POSIX_FD_WRITE;
                if (polled[i].ready & (kWaitError | kWaitHangup | kWaitInvalid))
                    flags |= CLAP_POSIX_FD_ERROR;
                std::lock_guard lock(it->second->lifecycle);
                it->second->instance->onFd(polled[i].source.fd(), flags);
            }
            if (polled[0].ready & kWaitInvalid) break;
            if (!(polled[0].ready & (kWaitRead | kWaitHangup | kWaitError))) continue;
            std::uint32_t op = 0;
            std::uint32_t id = 0;
            std::vector<std::uint8_t> payload;
            Resource fd = noResource;
            int descriptorCount = 0;
            if (!recvFrame(b.sock, op, id, payload, &fd, -1, &descriptorCount))
                break;  // the host is gone
            Reader r(payload.data(), payload.size());
            if (!b.handle(static_cast<Op>(op), id, r, fd, descriptorCount)) break;
#if defined(__APPLE__)
        }
#endif
    }
    for (auto& [id, slot] : b.instances) {
        (void)id;
        slot->stopAudio();
        if (slot->instance) slot->instance->closeEditor();
        if (slot->instance && slot->instance->processing()) slot->instance->stopProcessing();
        slot->instance.reset();
        slot->unmap();
    }
    return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::vector<std::string> text;
    text.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
        text.push_back(platform::pathText(std::filesystem::path(argv[i])));
    std::vector<char*> utf8;
    for (auto& arg : text) utf8.push_back(arg.data());
    utf8.push_back(nullptr);
    return bridgeMain(argc, utf8.data());
}
#endif
