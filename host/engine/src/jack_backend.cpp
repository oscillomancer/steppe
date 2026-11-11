// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/base/main_thread_wake.h"
#include "supple/engine/bench_probe.h"
#include "supple/engine/jack.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <cerrno>
#include <cstring>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace supple::engine {

namespace {

// PipeWire requests read when the client opens (pipewire-jack's variables),
// set for that call only. A variable the user set is left as it is.
class ClientEnvironment {
  public:
    ClientEnvironment() = default;
    ClientEnvironment(const ClientEnvironment&) = delete;
    ClientEnvironment& operator=(const ClientEnvironment&) = delete;
    ~ClientEnvironment() {
        for (const char* name : set_) ::unsetenv(name);
    }
    void request(const char* name, const std::string& value) {
        if (std::getenv(name)) return;
        ::setenv(name, value.c_str(), 0);
        set_.push_back(name);
    }

  private:
    std::vector<const char*> set_;
};

// The part of the JACK API we use, declared from its documentation: opaque
// handles, frame counts, flags and callback signatures.
using Frames = std::uint32_t;
struct JackClient;
struct JackPort;
struct LatencyRange {
    Frames min, max;
};
constexpr int kNoStartServer = 0x01;
constexpr unsigned long kPortIsInput = 0x1, kPortIsOutput = 0x2, kPortIsPhysical = 0x4;
constexpr int kPlaybackLatency = 1;
constexpr const char* kAudioType = "32 bit float mono audio";
constexpr const char* kMidiType = "8 bit raw midi";
struct MidiEvent {  // jack_midi_event_t
    Frames time;
    std::size_t size;
    unsigned char* buffer;
};

struct Api {
    pthread_t (*clientThreadId)(JackClient*) = nullptr;
    int (*acquireRealtime)(pthread_t, int) = nullptr;
    JackClient* (*clientOpen)(const char*, int, int*, ...) = nullptr;
    int (*clientClose)(JackClient*) = nullptr;
    int (*activate)(JackClient*) = nullptr;
    int (*deactivate)(JackClient*) = nullptr;
    int (*setProcessCallback)(JackClient*, int (*)(Frames, void*), void*) = nullptr;
    int (*setXrunCallback)(JackClient*, int (*)(void*), void*) = nullptr;
    int (*setBufferSizeCallback)(JackClient*, int (*)(Frames, void*), void*) = nullptr;
    int (*setSampleRateCallback)(JackClient*, int (*)(Frames, void*), void*) = nullptr;
    void (*onShutdown)(JackClient*, void (*)(void*), void*) = nullptr;
    int (*setPortRegistrationCallback)(JackClient*, void (*)(std::uint32_t, int, void*),
                                       void*) = nullptr;
    JackPort* (*portRegister)(JackClient*, const char*, const char*, unsigned long,
                              unsigned long) = nullptr;
    void* (*portGetBuffer)(JackPort*, Frames) = nullptr;
    const char* (*portName)(const JackPort*) = nullptr;
    void (*portGetLatencyRange)(JackPort*, int, LatencyRange*) = nullptr;
    Frames (*getSampleRate)(JackClient*) = nullptr;
    Frames (*getBufferSize)(JackClient*) = nullptr;
    int (*setBufferSize)(JackClient*, Frames) = nullptr;
    const char** (*getPorts)(JackClient*, const char*, const char*, unsigned long) = nullptr;
    int (*connect)(JackClient*, const char*, const char*) = nullptr;
    void (*free)(void*) = nullptr;
    std::uint32_t (*midiGetEventCount)(void*) = nullptr;  // optional: MIDI (ADR-0016)
    int (*midiEventGet)(MidiEvent*, void*, std::uint32_t) = nullptr;
    void (*midiClearBuffer)(void*) = nullptr;
    int (*midiEventWrite)(void*, Frames, const unsigned char*, std::size_t) = nullptr;
};

template <class F>
bool bind(void* handle, const char* name, F& out, std::string& missing) {
    out = reinterpret_cast<F>(::dlsym(handle, name));
    if (!out) missing += (missing.empty() ? "" : ", ") + std::string(name);
    return out != nullptr;
}

// Libraries are loaded once per path and never unloaded: libjack
// implementations keep threads and state that do not survive dlclose.
Result<const Api*> loadApi(const std::string& library) {
    static std::mutex mutex;
    static std::map<std::string, Api> loaded;
    std::lock_guard lock(mutex);
    if (auto it = loaded.find(library); it != loaded.end()) return &it->second;
    void* h = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* why = ::dlerror();
        return Error("backend.unavailable",
                     "JACK is not available: " + std::string(why ? why : library), {},
                     "install JACK or PipeWire's JACK support (pipewire-jack)");
    }
    Api api;
    std::string missing;
    bind(h, "jack_client_open", api.clientOpen, missing);
    bind(h, "jack_client_close", api.clientClose, missing);
    bind(h, "jack_activate", api.activate, missing);
    bind(h, "jack_deactivate", api.deactivate, missing);
    bind(h, "jack_set_process_callback", api.setProcessCallback, missing);
    bind(h, "jack_set_xrun_callback", api.setXrunCallback, missing);
    bind(h, "jack_set_buffer_size_callback", api.setBufferSizeCallback, missing);
    bind(h, "jack_set_sample_rate_callback", api.setSampleRateCallback, missing);
    bind(h, "jack_on_shutdown", api.onShutdown, missing);
    bind(h, "jack_port_register", api.portRegister, missing);
    bind(h, "jack_port_get_buffer", api.portGetBuffer, missing);
    bind(h, "jack_port_name", api.portName, missing);
    bind(h, "jack_get_sample_rate", api.getSampleRate, missing);
    bind(h, "jack_get_buffer_size", api.getBufferSize, missing);
    bind(h, "jack_set_buffer_size", api.setBufferSize, missing);
    bind(h, "jack_get_ports", api.getPorts, missing);
    bind(h, "jack_connect", api.connect, missing);
    bind(h, "jack_free", api.free, missing);
    std::string optional;  // latency reporting is informational
    bind(h, "jack_client_thread_id", api.clientThreadId, optional);
    bind(h, "jack_acquire_real_time_scheduling", api.acquireRealtime, optional);
    bind(h, "jack_port_get_latency_range", api.portGetLatencyRange, optional);
    bind(h, "jack_midi_get_event_count", api.midiGetEventCount, optional);
    bind(h, "jack_midi_event_get", api.midiEventGet, optional);
    bind(h, "jack_set_port_registration_callback", api.setPortRegistrationCallback, optional);
    bind(h, "jack_midi_clear_buffer", api.midiClearBuffer, optional);
    bind(h, "jack_midi_event_write", api.midiEventWrite, optional);
    if (!missing.empty()) {
        ::dlclose(h);
        return Error("backend.unavailable", library + " lacks JACK symbols: " + missing, {},
                     "provide a JACK library exporting the required JACK API, or choose backend "
                     "dummy for engine.start");
    }
    return &loaded.emplace(library, api).first->second;
}

std::string statusText(int status) {
    static constexpr std::pair<int, const char*> kBits[] = {
        {0x01, "failure"},           {0x02, "invalid option"}, {0x04, "name not unique"},
        {0x10, "server failed"},     {0x20, "server error"},   {0x40, "no such client"},
        {0x80, "load failure"},      {0x100, "init failure"},  {0x200, "shared memory failure"},
        {0x400, "version mismatch"}, {0x800, "backend error"}, {0x1000, "client zombie"}};
    std::string out;
    for (const auto& [bit, text] : kBits)
        if (status & bit) out += (out.empty() ? "" : ", ") + std::string(text);
    return out.empty() ? "unknown error" : out;
}

}  // namespace

struct JackBackend::Impl {
    static constexpr std::uint32_t kMaxPorts = 64;
    enum State : int { kRunning, kStopping, kStopped };

    const Api* api = nullptr;
    JackClient* client = nullptr;
    std::array<JackPort*, kMaxPorts> inPorts{}, outPorts{};
    std::uint32_t inputs = 0, outputs = 0, block = 0;
    // MIDI (ADR-0016): one input port per physical MIDI capture port; this
    // period's events, and the current block's (frames from its start).
    static constexpr std::uint32_t kMaxMidiPorts = 16;
    std::array<JackPort*, kMaxMidiPorts> midiPorts{};
    std::uint32_t midiInputs = 0;
    std::vector<std::string> midiNames;
    std::vector<MidiInputEvent> periodMidi, blockMidi;  // capacity reserved
    std::atomic<std::uint64_t> midiDropped{0};
    std::array<JackPort*, kMaxMidiPorts> midiOutPorts{};
    std::vector<std::string> midiOutNames;
    std::vector<MidiInputWarning> midiOutWarnings;
    std::array<std::atomic<std::uint64_t>, 16> midiAsyncDrops{};
    detail::MidiOutputCycle midiOut;
    std::atomic<bool> midiOutReady{false};
    std::atomic<bool> midiTopologyChanged{false};
    // The rate the client runs at (read once it processes) and the latest one
    // the server reported. PipeWire reports a stale rate until the client's
    // first cycle (its default, or the latency request's denominator, before
    // activation, and flips during it), so only a later difference counts.
    std::atomic<Frames> rate{0}, reportedRate{0};
    Callback callback;
    MainThreadWake* mainWake = nullptr;
    BenchProbe* benchProbe = nullptr;

    std::atomic<int> state{kRunning};
    std::atomic<std::uint32_t> period{0};
    std::atomic<std::uint64_t> xruns{0}, cycles{0};
    std::atomic<bool> serverGone{false};

    int process(Frames n) SUPPLE_RT {
        BenchProbe::Timer timer(benchProbe, true, n);
        std::array<float*, kMaxPorts> out;
        std::array<const float*, kMaxPorts> in;
        for (std::uint32_t c = 0; c < outputs; ++c)
            out[c] = static_cast<float*>(api->portGetBuffer(outPorts[c], n));
        for (std::uint32_t c = 0; c < inputs; ++c)
            in[c] = static_cast<const float*>(api->portGetBuffer(inPorts[c], n));
        std::array<void*, kMaxMidiPorts> midiBuffers{};
        const auto midiOutputCount =
            midiOutReady.load(std::memory_order_acquire) ? midiOutNames.size() : 0;
        for (std::size_t p = 0; p < midiOutputCount; ++p) {
            midiBuffers[p] = api->portGetBuffer(midiOutPorts[p], n);
            if (midiBuffers[p]) api->midiClearBuffer(midiBuffers[p]);
        }
        midiOut.begin(n, midiOutputCount);
        const int s = state.load(std::memory_order_acquire);
        if (s == kRunning) {
            // The engine writes stereo; other opened device ports must stay silent.
            for (std::uint32_t c = 2; c < outputs; ++c) std::fill_n(out[c], n, 0.0f);
            collectMidi(n);
            std::array<float*, kMaxPorts> o;
            std::array<const float*, kMaxPorts> i;
            std::size_t next = 0;  // into periodMidi
            for (Frames at = 0; at < n;) {
                const Frames len = std::min<Frames>(block, n - at);
                for (std::uint32_t c = 0; c < outputs; ++c) o[c] = out[c] + at;
                for (std::uint32_t c = 0; c < inputs; ++c) i[c] = in[c] + at;
                blockMidi.clear();
                for (; next < periodMidi.size() && periodMidi[next].frame < at + len; ++next) {
                    MidiInputEvent e = periodMidi[next];
                    e.frame -= at;
                    blockMidi.push_back(e);
                }
                midiOut.chunk(at, len);
                callback(inputs ? i.data() : nullptr, o.data(), len);
                at += len;
            }
            blockMidi.clear();
        } else {
            midiOut.chunk(0, 1);
            for (std::uint32_t c = 0; c < outputs; ++c) std::fill_n(out[c], n, 0.0f);
            if (s == kStopping) {
                callback(nullptr, out.data(), 0);  // the final callback (Backend::stop contract)
                state.store(kStopped, std::memory_order_release);
            }
        }
        for (const auto& event : midiOut.sorted())
            if (!midiBuffers[event.port] ||
                api->midiEventWrite(midiBuffers[event.port], event.frame, event.bytes,
                                    event.size)) {
                midiOut.drop();
                midiAsyncDrops[event.port].fetch_add(1, std::memory_order_relaxed);
            }
        cycles.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // This period's short MIDI messages from every port, sorted by frame
    // (longer ones, e.g. system exclusive, and overflow are counted).
    void collectMidi(Frames n) SUPPLE_RT {
        periodMidi.clear();
        for (std::uint32_t p = 0; p < midiInputs; ++p) {
            void* buffer = api->portGetBuffer(midiPorts[p], n);
            const std::uint32_t count = buffer ? api->midiGetEventCount(buffer) : 0;
            for (std::uint32_t k = 0; k < count; ++k) {
                MidiEvent ev{};
                if (api->midiEventGet(&ev, buffer, k) != 0) continue;
                if (ev.size == 0 || ev.size > 3 || ev.buffer[0] >= 0xf0 ||
                    periodMidi.size() == periodMidi.capacity()) {
                    midiDropped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                MidiInputEvent e;
                e.frame = std::min<Frames>(ev.time, n - 1);
                e.port = static_cast<std::uint16_t>(p);
                e.size = static_cast<std::uint8_t>(ev.size);
                std::copy_n(ev.buffer, ev.size, e.bytes);
                periodMidi.push_back(e);
            }
        }
        for (std::size_t i = 1; i < periodMidi.size(); ++i)  // stable, allocation-free
            for (std::size_t k = i; k > 0 && periodMidi[k].frame < periodMidi[k - 1].frame; --k)
                std::swap(periodMidi[k], periodMidi[k - 1]);
    }

    static void topologyThunk(std::uint32_t, int, void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (!self.midiOutReady.load(std::memory_order_acquire)) return;
        if (!self.midiTopologyChanged.exchange(true, std::memory_order_acq_rel) && self.mainWake)
            self.mainWake->notify();
    }

    static int processThunk(Frames n, void* self) SUPPLE_RT {
        return static_cast<Impl*>(self)->process(n);
    }
    static int xrunThunk(void* self) {
        auto* s = static_cast<Impl*>(self);
        s->xruns.fetch_add(1, std::memory_order_relaxed);
        if (s->mainWake) s->mainWake->notify();
        return 0;
    }
    static int bufferSizeThunk(Frames n, void* self) {
        auto* s = static_cast<Impl*>(self);
        if (s->period.exchange(n, std::memory_order_acq_rel) != n && s->mainWake)
            s->mainWake->notify();
        return 0;
    }
    static int sampleRateThunk(Frames n, void* self) {
        auto* s = static_cast<Impl*>(self);
        if (s->reportedRate.exchange(n, std::memory_order_acq_rel) != n && s->mainWake)
            s->mainWake->notify();
        return 0;
    }
    static void shutdownThunk(void* self) {
        auto* s = static_cast<Impl*>(self);
        s->serverGone.store(true, std::memory_order_release);
        if (s->mainWake) s->mainWake->notify();
    }

    void close() {
        if (client) api->clientClose(client);
        client = nullptr;
    }
    std::vector<std::string> inputNames;   // what each input is connected to
    std::vector<std::string> outputNames;  // what each output is connected to
};

JackBackend::JackBackend(Options options) : options_(std::move(options)) {}

std::vector<std::string> JackBackend::outputNames() const {
    std::vector<std::string> out = impl_ ? impl_->outputNames : std::vector<std::string>{};
    out.resize(config_.outputs);  // unconnected channels: ""
    return out;
}

Result<std::vector<std::string>> JackBackend::playbackNames(const Options& options) {
    return physicalPorts(options, false);
}

std::vector<std::string> JackBackend::inputNames() const {
    std::vector<std::string> out = impl_ ? impl_->inputNames : std::vector<std::string>{};
    out.resize(config_.inputs);  // unconnected channels: ""
    return out;
}

Result<std::vector<std::string>> JackBackend::captureNames(const Options& options) {
    return physicalPorts(options, true);
}

Result<std::vector<std::string>> JackBackend::physicalPorts(const Options& options, bool capture) {
    auto loadedApi = loadApi(options.library);
    if (!loadedApi) return std::move(loadedApi).error();
    const Api* api = *loadedApi;
    int status = 0;
    JackClient* client =
        api->clientOpen((options.clientName + "-probe").c_str(), kNoStartServer, &status);
    if (!client)
        return Error("backend.open",
                     "could not connect to the JACK server (" + statusText(status) + ")", {},
                     "use backend dummy if JACK is unavailable; for JACK, use a running server and "
                     "a client/port configuration it can accept");
    std::vector<std::string> names;
    if (const char** ports =
            api->getPorts(client, nullptr, kAudioType,
                          kPortIsPhysical | (capture ? kPortIsOutput : kPortIsInput))) {
        for (std::size_t c = 0; ports[c]; ++c) names.emplace_back(ports[c]);
        api->free(static_cast<void*>(ports));
    }
    api->clientClose(client);
    return names;
}

JackBackend::~JackBackend() { stop(); }

Status JackBackend::available(const std::string& library) {
    SUPPLE_TRY(loadApi(library));
    return okStatus();
}

Status JackBackend::start(const BackendConfig& config, Callback callback) {
    if (impl_)
        return Error("backend.running", "the backend is already running", {},
                     "stop this backend before starting it again or changing its configuration");
    if (!std::isfinite(config.sampleRate) || config.sampleRate < 1 || config.sampleRate > 768000)
        return Error("backend.config", "invalid requested sample rate", "/sample_rate",
                     "use a finite rate from 1 to 768000 Hz");
    if (!std::isfinite(options_.requestRate) || options_.requestRate < 0 ||
        options_.requestRate > 768000)
        return Error("backend.config", "invalid requested graph rate", "/rate",
                     "use a finite rate from 1 to 768000 Hz, or 0 for the graph's own rate");
    if (config.blockSize == 0 || config.outputs == 0 || config.outputs > Impl::kMaxPorts ||
        config.inputs > Impl::kMaxPorts)
        return Error("backend.config",
                     "the JACK backend needs a block size, 1 to " +
                         std::to_string(Impl::kMaxPorts) + " outputs and at most as many inputs",
                     {},
                     "provide a positive block_size and an output/input channel count within the "
                     "backend port limit");
    auto loadedApi = loadApi(options_.library);
    if (!loadedApi) return std::move(loadedApi).error();
    const Api* api = *loadedApi;
    stoppedMidiOutDropped_ = 0;
    auto impl = std::make_unique<Impl>();
    impl->api = api;
    impl->mainWake = mainWake_;
    impl->callback = std::move(callback);
    impl->benchProbe = options_.benchProbe;
    impl->inputs = config.inputs;
    impl->outputs = config.outputs;
    impl->block = config.blockSize;

    // The block as the client's latency, and any rate, are requests to the
    // graph read when the client opens; forcing makes both hold while we run.
    int status = 0;
    {
        ClientEnvironment environment;
        const auto block = std::to_string(config.blockSize);
        const auto rate = std::to_string(
            static_cast<long>(options_.requestRate > 0 ? options_.requestRate : config.sampleRate));
        if (options_.requestLatency) environment.request("PIPEWIRE_LATENCY", block + "/" + rate);
        if (options_.requestRate > 0) environment.request("PIPEWIRE_RATE", "1/" + rate);
        if (options_.forceGraph)
            environment.request("PIPEWIRE_PROPS",
                                "{ node.force-quantum=" + block +
                                    (options_.requestRate > 0 ? " node.force-rate=" + rate : "") +
                                    " }");
        impl->client = api->clientOpen(options_.clientName.c_str(), kNoStartServer, &status);
    }
    if (!impl->client)
        return Error("backend.open",
                     "could not connect to the JACK server (" + statusText(status) + ")", {},
                     "is PipeWire (with pipewire-jack) or a JACK server running?");

    auto fail = [&](std::string what) -> Status {
        impl->close();
        return Error("backend.open", "JACK: " + what, {},
                     "use backend dummy if JACK is unavailable; for JACK, use a running server and "
                     "a client/port configuration it can accept");
    };
    for (std::uint32_t c = 0; c < config.outputs; ++c) {
        const std::string port = "out_" + std::to_string(c + 1);
        impl->outPorts[c] =
            api->portRegister(impl->client, port.c_str(), kAudioType, kPortIsOutput, 0);
        if (!impl->outPorts[c]) return fail("could not register port " + port);
    }
    for (std::uint32_t c = 0; c < config.inputs; ++c) {
        const std::string port = "in_" + std::to_string(c + 1);
        impl->inPorts[c] =
            api->portRegister(impl->client, port.c_str(), kAudioType, kPortIsInput, 0);
        if (!impl->inPorts[c]) return fail("could not register port " + port);
    }
    impl->periodMidi.reserve(kMaxMidiInputEvents);
    impl->blockMidi.reserve(kMaxMidiInputEvents);
    std::vector<std::string> midiSources;  // the physical MIDI capture ports we listen to
    if (config.midiInputs && api->midiGetEventCount && api->midiEventGet) {
        if (const char** ports =
                api->getPorts(impl->client, nullptr, kMidiType, kPortIsPhysical | kPortIsOutput)) {
            for (std::uint32_t k = 0; ports[k] && k < Impl::kMaxMidiPorts; ++k) {
                const std::string port = "midi_in_" + std::to_string(k + 1);
                JackPort* p =
                    api->portRegister(impl->client, port.c_str(), kMidiType, kPortIsInput, 0);
                if (!p) continue;  // a port we cannot open is left out
                impl->midiPorts[impl->midiInputs++] = p;
                impl->midiNames.emplace_back(ports[k]);
                midiSources.emplace_back(ports[k]);
            }
            api->free(static_cast<void*>(ports));
        }
    }
    if (config.midiOutputs) {
        if (!api->midiClearBuffer || !api->midiEventWrite) {
            impl->midiOutWarnings.push_back(
                {"JACK MIDI outputs", 0, "JACK MIDI output symbols unavailable"});
        } else if (const char** ports = api->getPorts(impl->client, nullptr, kMidiType,
                                                      kPortIsPhysical | kPortIsInput)) {
            for (std::uint32_t k = 0; ports[k] && k < Impl::kMaxMidiPorts; ++k) {
                const std::string name = "midi_out_" + std::to_string(k + 1);
                auto* port =
                    api->portRegister(impl->client, name.c_str(), kMidiType, kPortIsOutput, 0);
                if (!port) {
                    impl->midiOutWarnings.push_back(
                        {ports[k], 0, "could not register JACK MIDI output"});
                    continue;
                }
                impl->midiOutPorts[impl->midiOutNames.size()] = port;
                impl->midiOutNames.emplace_back(ports[k]);
            }
            api->free(static_cast<void*>(ports));
        }
    }
    if (options_.forceBufferSize && api->getBufferSize(impl->client) != config.blockSize)
        api->setBufferSize(impl->client, config.blockSize);  // granted or not: period is reported
    impl->period = api->getBufferSize(impl->client);
    Impl* self = impl.get();
    if (api->setProcessCallback(impl->client, &Impl::processThunk, self) != 0)
        return fail("could not set the process callback");
    api->setXrunCallback(impl->client, &Impl::xrunThunk, self);
    api->setBufferSizeCallback(impl->client, &Impl::bufferSizeThunk, self);
    api->setSampleRateCallback(impl->client, &Impl::sampleRateThunk, self);
    api->onShutdown(impl->client, &Impl::shutdownThunk, self);
    if ((config.midiInputs || config.midiOutputs) && api->setPortRegistrationCallback)
        api->setPortRegistrationCallback(impl->client, &Impl::topologyThunk, self);
    active_.store(self, std::memory_order_release);  // midiInput() from the first callback
    if (api->activate(impl->client) != 0) {
        active_.store(nullptr);
        return fail("could not activate the client");
    }
    // The rate is settled once the client has run a cycle (a few ms); a server
    // that does not run us within a second is asked anyway.
    const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (impl->cycles.load(std::memory_order_relaxed) == 0 &&
           !impl->serverGone.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < settle)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    impl->rate.store(api->getSampleRate(impl->client), std::memory_order_relaxed);
    impl->reportedRate.store(impl->rate.load(std::memory_order_relaxed), std::memory_order_relaxed);

    if (options_.connectOutputs) {
        // Our outputs to the physical playback ports, in order; a device with
        // fewer ports leaves the rest unconnected.
        impl->outputNames.resize(config.outputs);
        if (const char** ports =
                api->getPorts(impl->client, nullptr, kAudioType, kPortIsPhysical | kPortIsInput)) {
            for (std::uint32_t c = 0; c < config.outputs && ports[c]; ++c)
                if (api->connect(impl->client, api->portName(impl->outPorts[c]), ports[c]) == 0)
                    impl->outputNames[c] = ports[c];
            api->free(static_cast<void*>(ports));
        }
        // The physical capture ports into our inputs, in order.
        impl->inputNames.resize(config.inputs);
        if (const char** ports =
                api->getPorts(impl->client, nullptr, kAudioType, kPortIsPhysical | kPortIsOutput)) {
            for (std::uint32_t c = 0; c < config.inputs && ports[c]; ++c)
                if (api->connect(impl->client, ports[c], api->portName(impl->inPorts[c])) == 0)
                    impl->inputNames[c] = ports[c];
            api->free(static_cast<void*>(ports));
        }
    }
    for (std::uint32_t k = 0; k < impl->midiInputs; ++k)  // each device into its port
        api->connect(impl->client, midiSources[k].c_str(), api->portName(impl->midiPorts[k]));
    std::size_t opened = 0;
    for (std::size_t p = 0; p < impl->midiOutNames.size(); ++p) {
        const int error = api->connect(impl->client, api->portName(impl->midiOutPorts[p]),
                                       impl->midiOutNames[p].c_str());
        if (error && error != EEXIST) {
            impl->midiOutWarnings.push_back(
                {impl->midiOutNames[p], error, "could not connect JACK MIDI output"});
        } else {
            impl->midiOutPorts[opened] = impl->midiOutPorts[p];
            impl->midiOutNames[opened++] = impl->midiOutNames[p];
        }
    }
    impl->midiOutNames.resize(opened);
    impl->midiOutReady.store(true, std::memory_order_release);
    config_ = config;
    config_.sampleRate = static_cast<double>(impl->rate.load(std::memory_order_relaxed));
    impl_ = std::move(impl);
    return okStatus();
}

void JackBackend::stop() {
    if (!impl_) return;
    Impl& m = *impl_;
    m.state.store(Impl::kStopping, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(options_.stopTimeoutSeconds);
    while (m.state.load(std::memory_order_acquire) != Impl::kStopped &&
           !m.serverGone.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // After deactivate() the process callback never runs again; without the
    // final callback, the engine stops its plugins on this thread.
    if (!m.serverGone.load()) m.api->deactivate(m.client);
    m.close();
    active_.store(nullptr, std::memory_order_release);
    stoppedMidiOutDropped_ = m.midiOut.dropped();
    impl_.reset();
}

JackBackend::Stats JackBackend::stats() const {
    Stats s;
    if (!impl_) return s;
    const Impl& m = *impl_;
    s.period = m.period.load(std::memory_order_relaxed);
    s.xruns = m.xruns.load(std::memory_order_relaxed);
    s.cycles = m.cycles.load(std::memory_order_relaxed);
    s.rateChanged =
        m.reportedRate.load(std::memory_order_relaxed) != m.rate.load(std::memory_order_relaxed);
    s.serverGone = m.serverGone.load(std::memory_order_relaxed);
    if (m.api->portGetLatencyRange && m.outputs > 0 && !s.serverGone) {
        LatencyRange range{0, 0};
        m.api->portGetLatencyRange(m.outPorts[0], kPlaybackLatency, &range);
        s.outputLatency = range.max;
    }
    return s;
}

BackendStatus JackBackend::status() const {
    const Stats s = stats();
    std::uint32_t inputLatency = 0;
    if (impl_ && impl_->api->portGetLatencyRange && impl_->inputs > 0 && !s.serverGone) {
        LatencyRange range{0, 0};
        impl_->api->portGetLatencyRange(impl_->inPorts[0], 0 /* capture */, &range);
        inputLatency = range.max;
    }
    const std::string_view reason = s.serverGone    ? "the audio server went away"
                                    : s.rateChanged ? "the device's rate changed"
                                                    : std::string_view{};
    return {s.period,
            s.xruns,
            s.outputLatency,
            inputLatency,
            s.rateChanged || s.serverGone,
            reason,
            impl_ && impl_->midiTopologyChanged.load(std::memory_order_acquire)};
}

std::span<const MidiInputEvent> JackBackend::midiInput() const {
    const Impl* m = active_.load(std::memory_order_acquire);  // the audio thread's view
    return m ? std::span<const MidiInputEvent>(m->blockMidi) : std::span<const MidiInputEvent>();
}

std::vector<std::string> JackBackend::midiPorts() const {
    return impl_ ? impl_->midiNames : std::vector<std::string>{};
}

bool JackBackend::writeMidi(const MidiOutputEvent& event) {
    auto* self = active_.load(std::memory_order_acquire);
    return self && self->midiOut.write(event);
}
std::vector<std::string> JackBackend::midiOutputPorts() const {
    return impl_ ? impl_->midiOutNames : std::vector<std::string>{};
}
std::vector<std::int64_t> JackBackend::midiOutputLatency() const {
    std::vector<std::int64_t> result;
    if (impl_) {
        const auto audio = stats();
        for (std::size_t p = 0; p < impl_->midiOutNames.size(); ++p) {
            LatencyRange range{};
            if (impl_->api->portGetLatencyRange && !audio.serverGone)
                impl_->api->portGetLatencyRange(impl_->midiOutPorts[p], kPlaybackLatency, &range);
            result.push_back(static_cast<std::int64_t>(audio.outputLatency) - range.max);
        }
    }
    return result;
}
std::vector<MidiInputWarning> JackBackend::midiOutputWarnings() const {
    return impl_ ? impl_->midiOutWarnings : std::vector<MidiInputWarning>{};
}
std::uint64_t JackBackend::midiOutAsyncDropped(std::size_t port) const {
    return impl_ && port < 16 ? impl_->midiAsyncDrops[port].load(std::memory_order_relaxed) : 0;
}
std::uint64_t JackBackend::midiOutDropped() const { return impl_ ? impl_->midiOut.dropped() : 0; }

unsigned JackBackend::liveThreadCapacity() const {
    if (!impl_ || !impl_->api->clientThreadId) return 0;
    cpu_set_t callback, caller, common;
    CPU_ZERO(&callback);
    CPU_ZERO(&caller);
    const auto thread = impl_->api->clientThreadId(impl_->client);
    if (!thread || ::pthread_getaffinity_np(thread, sizeof(callback), &callback) != 0 ||
        ::sched_getaffinity(0, sizeof(caller), &caller) != 0)
        return 0;
    CPU_AND(&common, &callback, &caller);
    const int count = CPU_COUNT(&common);
    return count > 1 ? static_cast<unsigned>(count - 1) : 0;
}

ThreadPolicy JackBackend::callbackPolicy() {
    if (!impl_ || !impl_->api->clientThreadId) return {};
    const auto thread = impl_->api->clientThreadId(impl_->client);
    return thread ? threadPolicy(thread) : ThreadPolicy{};
}

Status JackBackend::prepareLiveThread(std::thread::native_handle_type thread) {
    auto fail = [](int error) -> Status {
        return Error("engine.live_threads",
                     "live helper scheduling failed: " + std::string(std::strerror(error)), {},
                     "use Audio processing threads Limit 1, or grant this process RT scheduling "
                     "at the JACK callback priority and restart the engine");
    };
    if (!impl_ || !impl_->api->clientThreadId) return fail(ENOSYS);
    const auto callback = impl_->api->clientThreadId(impl_->client);
    int policy = 0;
    sched_param params{};
    if (!callback) return fail(ESRCH);
    if (const int error = ::pthread_getschedparam(callback, &policy, &params)) return fail(error);
    if (policy == SCHED_OTHER) return okStatus();  // non-RT JACK server / test backend
    if (policy != SCHED_FIFO && policy != SCHED_RR) return fail(ENOTSUP);
    const int priority = params.sched_priority;
    int error =
        impl_->api->acquireRealtime ? impl_->api->acquireRealtime(thread, priority) : ENOSYS;
    if (error != 0) {
        rlimit limit{};
        if (::getrlimit(RLIMIT_RTPRIO, &limit) != 0) return fail(errno);
        if (limit.rlim_cur < static_cast<rlim_t>(priority)) return fail(EPERM);
        error = ::pthread_setschedparam(thread, SCHED_FIFO, &params);
        if (error != 0) return fail(error);
    }
    if (const int result = ::pthread_getschedparam(thread, &policy, &params)) return fail(result);
    if (policy != SCHED_FIFO || params.sched_priority != priority) return fail(EPERM);
    return okStatus();
}

}  // namespace supple::engine
