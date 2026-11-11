// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include "../../../tests/file_platform.h"
#include "midi_input.h"
#include "midi_output.h"
#include "portaudio_api.h"
#include "supple/engine/app.h"
#include "supple/engine/offline.h"
#include "supple/engine/portaudio.h"
#include "thread_startup.h"
#if defined(__APPLE__)
#include <os/workgroup.h>
#endif
using namespace supple;
using namespace supple::engine;
namespace {
struct Driver {
    static inline Driver* current;
    PaHostApiInfo host{1, paCoreAudio, "Test API", 2, 0, 1};
    std::array<PaDeviceInfo, 2> devices{{{2, "Capture", 0, 2, 0, .001, 0, .01, 0, 48000},
                                         {2, "Playback", 0, 0, 2, 0, .002, 0, .02, 48000}}};
    PaStreamInfo info{1, .001, .002, 48000};
    PaStreamCallback* callback = nullptr;
    PaStreamFinishedCallback* finished = nullptr;
    void* context = nullptr;
    PaStreamParameters input{}, output{};
    PaError openError = paNoError, startError = paNoError, formatError = paFormatIsSupported;
    int active = 1, initialized = 0, closed = 0;
    std::thread audio;
    std::atomic<unsigned> audioRequest{0};
    std::atomic<double> hardwareRate{48000};
    std::atomic<unsigned> rateQueries{0};
    std::mutex rateQueryMutex;
    std::vector<std::thread::id> rateQueryThreads;
    std::array<std::array<float, 4096>, 2> in{};
    std::array<std::array<float, 4096>, 4> out{};
    std::array<float*, 2> inputs{in[0].data(), in[1].data()};
    std::array<float*, 4> outputs{out[0].data(), out[1].data(), out[2].data(), out[3].data()};
    Driver() { current = this; }
    ~Driver() {
        if (audio.joinable()) audio.join();
        current = nullptr;
    }
    int drive(unsigned long frames, PaStreamCallbackFlags flags = 0,
              const PaStreamCallbackTimeInfo* time = nullptr) {
        return callback(input.channelCount ? inputs.data() : nullptr, outputs.data(), frames, time,
                        flags, context);
    }
    static const detail::PortAudioApi api;
};
const detail::PortAudioApi Driver::api{
    []() -> PaError {
        ++current->initialized;
        return paNoError;
    },
    []() -> PaError {
        --current->initialized;
        return paNoError;
    },
    [](PaError) -> const char* { return "fake driver failure"; },
    [] { return 1; },
    [] { return 0; },
    [](int index) -> const PaHostApiInfo* { return index == 0 ? &current->host : nullptr; },
    [] { return 2; },
    [](int index) -> const PaDeviceInfo* {
        return index >= 0 && index < 2 ? &current->devices[static_cast<unsigned>(index)] : nullptr;
    },
    [](const PaStreamParameters*, const PaStreamParameters*, double) {
        return current->formatError;
    },
    [](PaStream** stream, const PaStreamParameters* input, const PaStreamParameters* output, double,
       unsigned long, PaStreamFlags flags, PaStreamCallback* callback, void* context) -> PaError {
        CHECK(flags == (paClipOff | paDitherOff));
        current->input = input ? *input : PaStreamParameters{};
        current->output = *output;
        current->callback = callback;
        current->context = context;
        if (current->openError != paNoError) return current->openError;
        *stream = reinterpret_cast<PaStream*>(current);
        return paNoError;
    },
    [](PaStream*, PaStreamFinishedCallback* callback) -> PaError {
        current->finished = callback;
        return paNoError;
    },
    [](PaStream*) -> PaError { return current->startError; },
    [](PaStream*) -> PaError { return paNoError; },
    [](PaStream*) -> PaError {
        if (current->audio.joinable()) current->audio.join();
        ++current->closed;
        return paNoError;
    },
    [](PaStream*) -> const PaStreamInfo* { return &current->info; },
    [](PaStream*) {
        current->audioRequest.fetch_add(1, std::memory_order_release);
        current->audioRequest.notify_one();
        return current->active;
    },
    nullptr,
    false,
    false};
InstancePool pool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path path = FIXTURE_CLAP_PATH;
        auto plugins = host::scan(std::span<const std::filesystem::path>(&path, 1));
        if (!plugins) return std::move(plugins).error();
        for (auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("test.plugin", "fixture missing");
    });
}
model::Session session() {
    model::EditTxn edit(model::Session::create({.console9 = true}), 1);
    for (int n = 0; n < 8; ++n) {
        model::Track track;
        track.name = "Instrument";
        track.parent = model::kMaster;
        track.volumeDb = -24;
        const auto tr = edit.create(track);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(tr); });
        for (const char* id : {"com.suppleaudio.test.sine", "com.suppleaudio.test.gain"}) {
            model::FxInstance fx;
            fx.plugin.id = id;
            if (std::string_view(id) == "com.suppleaudio.test.gain")
                fx.patch =
                    json::parse(
                        R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                        .value();
            const auto f = edit.create(fx);
            edit.modify<model::Track>(tr, [&](auto& t) { t.chain.push_back(f); });
        }
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        midi.sequence.length = 960;
        midi.sequence.notes.push_back({0, 480, 0, static_cast<std::uint8_t>(60 + n), 100, 0});
        model::Take take;
        take.source = edit.create(midi);
        model::Item item;
        item.track = tr;
        item.length = 1;
        item.takes = {edit.create(take)};
        item.activeTake = item.takes[0];
        edit.create(item);
    }
    return edit.commit().value();
}
}  // namespace
TEST_CASE("PortAudio selection, planar chunks, latency and callback status without hardware") {
    Driver driver;
    PortAudioBackend backend(
        {.hostApi = "Test API", .inputDevice = "Capture", .outputDevice = "Playback"}, Driver::api);
    auto devices = backend.driverDevices();
    REQUIRE(devices);
    REQUIRE(devices->size() == 2);
    CHECK((*devices)[1].defaultOutput);
    CHECK(driver.initialized == 0);
    unsigned calls = 0, frames = 0, finals = 0, largest = 0;
    REQUIRE(backend.start({.blockSize = 64, .inputs = 2},
                          [&](const float* const* in, float* const* out, std::uint32_t n) {
                              if (!n) {
                                  ++finals;
                                  return;
                              }
                              ++calls;
                              largest = std::max(largest, n);
                              for (unsigned c = 0; c < 2; ++c)
                                  for (unsigned i = 0; i < n; ++i) out[c][i] = in[c][i];
                              frames += n;
                          }));
    // Fill outside the RT callback.
    for (unsigned i = 0; i < 211; ++i) driver.in[0][i] = float(i) / 211.0f;
    driver.drive(211, paOutputUnderflow | paInputOverflow);
    CHECK(calls == 4);
    CHECK(largest == 64);
    CHECK(frames == 211);
    CHECK(driver.out[0] == driver.in[0]);
    auto status = backend.status();
    CHECK(status.period == 211);
    CHECK(status.xruns == 1);  // one affected callback, even with two flags
    CHECK(status.inputLatency == 48);
    CHECK(status.outputLatency == 96);
    driver.info.sampleRate = 44100;
    CHECK(backend.status().restartReason == "sample rate changed");
    driver.finished(driver.context);
    CHECK(backend.status().restartReason == "device lost");
    backend.stop();
    CHECK(finals == 1);
    CHECK(driver.initialized == 0);
    CHECK(driver.closed == 1);
}
TEST_CASE("unused native device outputs are silent for the stereo engine") {
    Driver driver;
    driver.devices[1].maxOutputChannels = 4;
    for (auto& channel : driver.out) channel.fill(0.5f);
    PortAudioBackend backend({}, Driver::api);
    REQUIRE(backend.start({.blockSize = 64, .outputs = 4},
                          [](auto, float* const* out, std::uint32_t n) {
                              if (!n) return;
                              for (unsigned c = 0; c < 2; ++c) std::fill_n(out[c], n, 0.25f);
                          }));
    driver.drive(211);
    for (unsigned c = 0; c < 4; ++c) {
        CHECK(std::all_of(driver.out[c].begin(), driver.out[c].begin() + 211,
                          [c](float x) { return x == (c < 2 ? 0.25f : 0.f); }));
        CHECK(driver.out[c][211] == 0.5f);
    }
    backend.stop();
}

TEST_CASE("native rate queries stay on the monitor and publish rate changes to status") {
    Driver driver;
    auto api = Driver::api;
    api.native = true;
    api.deviceRate = [](PaStream*, PaDeviceIndex) {
        const double rate = Driver::current->hardwareRate.load(std::memory_order_acquire);
        {
            std::lock_guard lock(Driver::current->rateQueryMutex);
            Driver::current->rateQueryThreads.push_back(std::this_thread::get_id());
        }
        Driver::current->rateQueries.fetch_add(1, std::memory_order_release);
        Driver::current->rateQueries.notify_one();
        return rate;
    };
    PortAudioBackend backend({}, api);
    const auto owner = std::this_thread::get_id();
    REQUIRE(backend.start({}, [](auto, auto, auto) {}));
    driver.rateQueries.wait(0, std::memory_order_acquire);
    for (unsigned i = 0; i < 8; ++i) CHECK_FALSE(backend.status().restartNeeded);
    driver.hardwareRate.store(44100, std::memory_order_release);
    while (!backend.status().restartNeeded) std::this_thread::yield();
    CHECK(backend.status().restartReason == "sample rate changed");
    backend.stop();
    REQUIRE_FALSE(driver.rateQueryThreads.empty());
    const auto monitor = driver.rateQueryThreads.front();
    CHECK(monitor != owner);
    for (const auto thread : driver.rateQueryThreads) CHECK(thread == monitor);
    CHECK(driver.initialized == 0);
}
TEST_CASE("PortAudio failed selection and stream startup release resources") {
    Driver driver;
    auto nativeApi = Driver::api;
    nativeApi.native = true;
    PortAudioBackend native({}, nativeApi);
    std::atomic<unsigned> releaseHelper{0};
    std::thread idle([&] { releaseHelper.wait(0); });
    auto refused = native.prepareLiveThread(idle.native_handle());
    releaseHelper.store(1);
    releaseHelper.notify_one();
    idle.join();
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "engine.live_threads");
    CHECK(native.callbackPolicy().kind == ThreadPolicy::Kind::None);
    PortAudioBackend missing({.hostApi = "unknown"}, Driver::api);
    auto bad = missing.start({}, [](auto, auto, auto) {});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().path == "/host_api");
    CHECK_FALSE(bad.error().hint.empty());
    CHECK(driver.initialized == 0);
    PortAudioBackend backend({}, Driver::api);
    REQUIRE(backend.start({.midiInputs = true}, [](auto, auto, auto) {}));
    CHECK(backend.midiPorts().empty());  // injected audio-only driver
    backend.stop();
    CHECK(driver.initialized == 0);
    driver.closed = 0;
    driver.startError = paDeviceUnavailable;
    CHECK_FALSE(backend.start({}, [](auto, auto, auto) {}));
    CHECK(driver.closed == 1);
    CHECK(driver.initialized == 0);
    driver.startError = paNoError;
    driver.formatError = paInvalidSampleRate;
    CHECK_FALSE(backend.start({}, [](auto, auto, auto) {}));
    CHECK(driver.initialized == 0);
    driver.formatError = paNoError;
    CHECK_FALSE(backend.start({.inputs = 3}, [](auto, auto, auto) {}));
    CHECK(driver.initialized == 0);
}
TEST_CASE("host-specific stream settings follow the host API and an explicit device rate") {
    Driver driver;
    static PaHostApiTypeId seenType;
    static bool seenChange;
    static int settings;
    auto api = Driver::api;
    api.streamInfo = [](PaHostApiTypeId type, bool changeRate) -> void* {
        seenType = type;
        seenChange = changeRate;
        return &settings;
    };
    for (const bool change : {false, true}) {
        PortAudioOptions options;
        options.changeDeviceRate = change;
        PortAudioBackend backend(options, api);
        REQUIRE(backend.start({.sampleRate = 96000, .inputs = 2}, [](auto, auto, auto) {}));
        CHECK(seenType == paCoreAudio);
        CHECK(seenChange == change);
        // Duplex streams pass the same settings for input and output.
        CHECK(driver.input.hostApiSpecificStreamInfo == &settings);
        CHECK(driver.output.hostApiSpecificStreamInfo == &settings);
        backend.stop();
    }
    // Without the hook (Linux, older drivers) PortAudio's defaults apply.
    PortAudioBackend plain({}, Driver::api);
    REQUIRE(plain.start({}, [](auto, auto, auto) {}));
    CHECK(driver.output.hostApiSpecificStreamInfo == nullptr);
    plain.stop();
}

TEST_CASE("PortAudio fake device output is bit-identical to offline in both executors") {
    const auto model = session();
    auto offlinePool = pool();
    auto offline = renderOffline(model, offlinePool, {.startSeconds = 0, .endSeconds = .1});
    REQUIRE(offline);
    for (bool ahead : {false, true})
        for (int workers : {0, 1, 7}) {
            INFO(ahead, workers);
            Driver driver;
            PortAudioBackend backend({}, Driver::api);
            auto rtPool = pool();
            MediaCache media;
            Engine engine(rtPool, media, {.anticipative = ahead, .workers = workers});
            REQUIRE(engine.start(backend, {.blockSize = 64}));
            REQUIRE(engine.load(model));
            engine.play();

            std::array<std::vector<float>, 2> captured;
            const std::size_t needed = offline->audio.frames() + engine.outputLatency();
            while (captured[0].size() < needed) {
                if (ahead) REQUIRE(engine.awaitRendered(4096));
                // Odd periods exercise the final short chunk as well as full blocks.
                driver.drive(211);
                for (unsigned c = 0; c < 2; ++c)
                    captured[c].insert(captured[c].end(), driver.out[c].begin(),
                                       driver.out[c].begin() + 211);
            }
            CHECK(engine.anticipationMisses() == 0);
            const auto offset = engine.outputLatency();
            engine.stop();
            CHECK(engine.processErrors() == 0);
            for (unsigned c = 0; c < 2; ++c)
                CHECK(std::equal(offline->audio.channels[c].begin(),
                                 offline->audio.channels[c].end(), captured[c].begin() + offset));
        }
}
#if defined(__APPLE__) || defined(_WIN32)
TEST_CASE("real native audio device is opt-in") {
    const char* enabled = std::getenv("SUPPLE_AUDIO_DEVICE_TEST");
    if (!enabled || std::string_view(enabled) != "1") return;
    PortAudioBackend backend;
    REQUIRE(backend.start({.blockSize = 128}, [](auto, float* const* out, std::uint32_t n) {
        if (n)
            for (unsigned c = 0; c < 2; ++c) std::fill_n(out[c], n, 0.0f);
    }));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    backend.stop();
}
#endif

TEST_CASE("PortAudio stop makes its final callback on the device thread and joins it") {
    Driver driver;
    auto api = Driver::api;
    api.threaded = true;
    PortAudioBackend backend({}, api);
    unsigned finals = 0;
    std::thread::id finalThread;
    REQUIRE(backend.start({}, [&](auto, auto, std::uint32_t frames) {
        if (!frames) {
            ++finals;
            finalThread = std::this_thread::get_id();
        }
    }));
    driver.audio = std::thread([&] {
        driver.audioRequest.wait(0, std::memory_order_acquire);
        driver.drive(128);
    });
    const auto deviceThread = driver.audio.get_id();
    backend.stop();
    CHECK(finals == 1);
    CHECK(finalThread == deviceThread);
    CHECK_FALSE(driver.audio.joinable());
    backend.stop();
    CHECK(finals == 1);
}
TEST_CASE("native helper setup is self-applied and rolls back all helpers on refusal") {
    struct PolicyBackend : DummyBackend {
        PolicyBackend() : DummyBackend({.mode = DummyBackend::Mode::Manual}) {}
        std::thread::id owner = std::this_thread::get_id();
        std::atomic<unsigned> prepared{0}, released{0}, wrongThread{0};
        bool refuse = false;
        Status prepareCurrentThread(bool) override {
            if (std::this_thread::get_id() == owner) ++wrongThread;
            const auto n = prepared.fetch_add(1);
            if (refuse && n == 0) return Error("thread.test", "forced refusal");
            return okStatus();
        }
        void releaseCurrentThread() override { ++released; }
    };
    for (bool refuse : {false, true}) {
        PolicyBackend backend;
        backend.refuse = refuse;
        detail::ThreadStartup setup(2, &backend, true);
        std::array<std::thread, 2> helpers;
        std::atomic<unsigned> working{0};
        for (unsigned i = 0; i < 2; ++i)
            helpers[i] = std::thread([&, i] {
                const bool ready = setup.enter(i);
                detail::ThreadStartup::Cleanup cleanup{ready ? &backend : nullptr};
                if (ready) ++working;
            });
        auto configured = setup.finish();
        CHECK(configured.ok() == !refuse);
        for (auto& helper : helpers) helper.join();
        CHECK(backend.prepared == 2);
        CHECK(backend.released == 2);
        CHECK(backend.wrongThread == 0);
        CHECK(working == (refuse ? 0 : 2));
    }
}

namespace {
// Records which threads join and leave; stands in for CoreAudio's workgroup.
struct FakeWorkgroup : detail::AudioWorkgroup {
    static inline FakeWorkgroup* current = nullptr;
    std::mutex mutex;
    std::vector<std::thread::id> joined, left;
    bool refuse = false;
    Status join() override {
        std::lock_guard lock(mutex);
        if (refuse) return Error("workgroup.join", "forced refusal", "22", "restart the engine");
        joined.push_back(std::this_thread::get_id());
        return okStatus();
    }
    void leave() noexcept override {
        std::lock_guard lock(mutex);
        left.push_back(std::this_thread::get_id());
    }
};
detail::PortAudioApi workgroupApi() {
    auto api = Driver::api;
    api.native = true;
    api.osThreadPolicy = false;  // the OS may refuse real-time policy (CI VMs)
    api.workgroup = [](PaStream*) -> Result<std::shared_ptr<detail::AudioWorkgroup>> {
        if (!FakeWorkgroup::current)
            return Error("workgroup.unavailable", "no workgroup", "-10851", "use another device");
        return std::shared_ptr<detail::AudioWorkgroup>(std::shared_ptr<detail::AudioWorkgroup>(),
                                                       FakeWorkgroup::current);
    };
    return api;
}
// Runs `live` helpers and `background` helpers through the startup barrier.
void runHelpers(PortAudioBackend& backend, unsigned live, unsigned background,
                const std::function<void()>& whileRunning) {
    detail::ThreadStartup liveSetup(live, &backend, true);
    detail::ThreadStartup backgroundSetup(background, &backend, false);
    std::atomic<unsigned> entered{0}, release{0};
    std::vector<std::thread> helpers;
    auto spawn = [&](detail::ThreadStartup& setup, unsigned i) {
        helpers.emplace_back([&, i] {
            const bool ready = setup.enter(i);
            detail::ThreadStartup::Cleanup cleanup{ready ? &backend : nullptr};
            entered.fetch_add(1);
            entered.notify_all();
            if (ready) release.wait(0);
        });
    };
    for (unsigned i = 0; i < live; ++i) spawn(liveSetup, i);
    for (unsigned i = 0; i < background; ++i) spawn(backgroundSetup, i);
    CHECK(liveSetup.finish());
    CHECK(backgroundSetup.finish());
    for (unsigned seen; (seen = entered.load()) < live + background;) entered.wait(seen);
    whileRunning();
    release.store(1);
    release.notify_all();
    for (auto& helper : helpers) helper.join();
}
}  // namespace
TEST_CASE("live helpers join the device audio workgroup on their own threads and leave") {
    Driver driver;
    FakeWorkgroup group;
    FakeWorkgroup::current = &group;
    const auto api = workgroupApi();
    PortAudioBackend backend({}, api);
    CHECK(backend.workgroup().state == "none");
    REQUIRE(backend.start({}, [](auto, auto, auto) {}));
    CHECK(backend.workgroup().state == "ready");
    runHelpers(backend, 2, 1, [&] {
        const auto status = backend.workgroup();
        CHECK(status.state == "joined");
        CHECK(status.joined == 2);  // the background helper does not join
        CHECK(status.failed == 0);
    });
    CHECK(group.joined.size() == 2);
    CHECK(group.joined.size() == group.left.size());
    for (const auto id : group.joined) {
        CHECK(id != std::this_thread::get_id());
        CHECK(std::count(group.left.begin(), group.left.end(), id) == 1);
    }
    const auto after = backend.workgroup();
    CHECK(after.joined == 0);
    CHECK(after.state == "ready");
    backend.stop();
    FakeWorkgroup::current = nullptr;
}
TEST_CASE("a refused workgroup join is reported and keeps the helpers running") {
    Driver driver;
    FakeWorkgroup group;
    group.refuse = true;
    FakeWorkgroup::current = &group;
    const auto api = workgroupApi();
    PortAudioBackend backend({}, api);
    REQUIRE(backend.start({}, [](auto, auto, auto) {}));
    runHelpers(backend, 2, 0, [&] {
        const auto status = backend.workgroup();
        CHECK(status.state == "failed");
        CHECK(status.joined == 0);
        CHECK(status.failed == 2);
        CHECK(status.osError == 22);
        CHECK(status.message == "forced refusal");
        CHECK(status.hint == "restart the engine");
    });
    CHECK(group.left.empty());
    backend.stop();
    FakeWorkgroup::current = nullptr;
}
TEST_CASE("a missing device workgroup is a reported failure, not a start error") {
    Driver driver;
    FakeWorkgroup::current = nullptr;
    const auto api = workgroupApi();
    PortAudioBackend backend({}, api);
    REQUIRE(backend.start({}, [](auto, auto, auto) {}));
    runHelpers(backend, 1, 0, [&] {
        const auto status = backend.workgroup();
        CHECK(status.state == "failed");
        CHECK(status.osError == -10851);
        CHECK(status.message == "no workgroup");
        CHECK(status.joined == 0);
    });
    backend.stop();
    // A driver without the hook never claims one.
    PortAudioBackend plain({}, Driver::api);
    REQUIRE(plain.start({}, [](auto, auto, auto) {}));
    CHECK(plain.workgroup().state == "none");
    plain.stop();
}
TEST_CASE("engine start and status report a refused workgroup") {
    if (allowedCpuCount() < 2) return;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-workgroup-status-" + std::to_string(supple::test::processId()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    struct GroupBackend : DummyBackend {
        GroupBackend() : DummyBackend({.mode = DummyBackend::Mode::Manual}) {}
        bool nativeThreadPolicy() const override { return true; }
        Status prepareCurrentThread(bool) override { return okStatus(); }
        WorkgroupStatus workgroup() const override {
            return {"partial", 1, 1, 22, "forced refusal", "restart the engine"};
        }
    } backend;
    AppOptions options;
    options.workdir = dir;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto opened = EngineApp::create(options);
    REQUIRE(opened);
    auto& app = **opened;
    REQUIRE(app.startEngine(backend, {}, {.workers = 1}));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    app.attachCommands(dispatcher);
    auto result = dispatcher.execute(
        {"engine.status", json::Value::object(), {cmd::Origin::Type::Test, "workgroup", ""}});
    REQUIRE(result);
    auto group = (*result)["workgroup"];
    CHECK(group["state"] == "partial");
    CHECK(group["joined"] == 1);
    CHECK(group["failed"] == 1);
    CHECK(group["os_error"] == 22);
    CHECK(group["message"] == "forced refusal");
    CHECK(group["hint"] == "restart the engine");
    bool warned = false;
    for (const auto& warning : (*result)["warnings"].asArray())
        warned = warned || warning.asString().find("audio workgroup partial: forced refusal") == 0;
    CHECK(warned);
    app.stopEngine();
}

#if defined(__APPLE__)
TEST_CASE("macOS workgroup wrapper joins once per thread, leaves and rejoins") {
    os_workgroup_t group = os_workgroup_parallel_create("supple-test", nullptr);
    REQUIRE(group != nullptr);
    auto wrapped = detail::wrapOsWorkgroup(group, true);
    Status first, second, third;
    bool policy = false;
    std::thread helper([&] {
        // Production order: the time-constraint policy first (joining a
        // workgroup needs it), then the join.
        policy = detail::prepareDeviceThread(48000, 128, true).ok();
        if (!policy) return;  // some CI VMs refuse real-time policy
        first = wrapped->join();
        second = wrapped->join();  // already a member
        wrapped->leave();
        wrapped->leave();  // not a member: harmless
        third = wrapped->join();
        wrapped->leave();
        detail::releaseDeviceThread();
    });
    helper.join();
    if (!policy) {
        MESSAGE("real-time thread policy refused; workgroup join not exercised");
        return;
    }
    CHECK(first);
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == "workgroup.join");
    CHECK(third);
    // A thread that never joined cannot leave another thread's membership.
    wrapped->leave();
}
TEST_CASE("real device workgroup is joined by live helpers only on explicit opt-in") {
    const char* enabled = std::getenv("SUPPLE_AUDIO_DEVICE_TEST");
    if (!enabled || std::string_view(enabled) != "1") return;
    PortAudioBackend backend;
    REQUIRE(backend.start({.blockSize = 128}, [](auto, float* const* out, std::uint32_t n) {
        if (n)
            for (unsigned c = 0; c < 2; ++c) std::fill_n(out[c], n, 0.0f);
    }));
    CHECK(backend.workgroup().state == "ready");
    runHelpers(backend, 2, 1, [&] {
        const auto status = backend.workgroup();
        INFO(status.message);
        CHECK(status.state == "joined");
        CHECK(status.joined == 2);
    });
    CHECK(backend.workgroup().joined == 0);
    backend.stop();
}
#endif
TEST_CASE("native background policy leaves two usable CPUs free") {
    const unsigned cpus = allowedCpuCount();
    // EngineOptions caps helpers at 64; above 66 CPUs every legal pool fits.
    if (cpus > 66) return;
    struct CapacityBackend : DummyBackend {
        CapacityBackend() : DummyBackend({.mode = DummyBackend::Mode::Manual}) {}
        std::atomic<unsigned> prepared{0};
        bool nativeThreadPolicy() const override { return true; }
        unsigned liveThreadCapacity() const override { return 0; }
        Status prepareCurrentThread(bool) override {
            ++prepared;
            return okStatus();
        }
    } backend;
    auto instances = pool();
    MediaCache media;
    Engine engine(instances, media, {.workers = static_cast<int>(cpus > 1 ? cpus - 2 : 0)});
    REQUIRE(engine.start(backend, {}));
    CHECK(backend.prepared == 0);
    CHECK(engine.scheduling().renderer.kind == ThreadPolicy::Kind::Normal);
    engine.stop();
}

TEST_CASE("engine status serializes verified native helper policies") {
    if (allowedCpuCount() < 2) return;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-native-status-" + std::to_string(supple::test::processId()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    struct PolicyBackend : DummyBackend {
        PolicyBackend() : DummyBackend({.mode = DummyBackend::Mode::Manual}) {}
        bool nativeThreadPolicy() const override { return true; }
        Status prepareCurrentThread(bool) override { return okStatus(); }
        BackendStatus status() const override {
            auto result = DummyBackend::status();
            result.midiDevicesChanged = true;
            return result;
        }
    } backend;
    AppOptions options;
    options.workdir = dir;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto opened = EngineApp::create(options);
    REQUIRE(opened);
    auto& app = **opened;
    REQUIRE(app.startEngine(backend, {}, {.workers = 1}));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    app.attachCommands(dispatcher);
    auto result = dispatcher.execute(
        {"engine.status", json::Value::object(), {cmd::Origin::Type::Test, "native-status", ""}});
    REQUIRE(result);
    CHECK((*result)["scheduling"]["live"]["policy"] == "high");
    CHECK((*result)["midi_devices_changed"] == true);
    CHECK((*result)["restart_needed"] == false);
    CHECK((*result)["midi_devices_message"] ==
          "MIDI devices changed; restart the engine to pick up the new set");
    app.stopEngine();
}

namespace {
struct FakeMidi : detail::MidiSource {
    detail::MidiInputBuffer* buffer = nullptr;
    std::atomic<double> clock{10};
    std::atomic<bool> topology{false};
    bool refused = false, failed = false;
    unsigned starts = 0, stops = 0;
    Status start(detail::MidiInputBuffer& target) override {
        ++starts;
        buffer = &target;
        target.names = {"Keyboard", "Pads"};
        if (failed) return Error("backend.midi", "fixture setup failure", "/midi_inputs", "retry");
        if (refused) {
            target.names = {"Pads"};
            target.warnings.push_back({"Keyboard", 42, "fixture refused MIDI"});
        }
        return okStatus();
    }
    void stop() override {
        if (buffer) ++stops;
        buffer = nullptr;
    }
    double now() const SUPPLE_RT override { return clock.load(std::memory_order_relaxed); }
    bool changed() override { return topology.load(std::memory_order_acquire); }
    void send(unsigned port, unsigned frame, unsigned frames, std::uint32_t message) {
        buffer->push(port, now() + (static_cast<double>(frame) - frames) / 48000, message);
    }
};
constexpr std::uint32_t midiWord(unsigned status, unsigned key, unsigned value) {
    return status | key << 8 | value << 16;
}
}  // namespace
TEST_CASE("native MIDI maps timestamps, sorts ports and rebases every PortAudio chunk") {
    Driver driver;
    auto source = std::make_unique<FakeMidi>();
    auto& midi = *source;
    PortAudioBackend backend({}, Driver::api, std::move(source));
    std::array<MidiInputEvent, 16> events{};
    unsigned count = 0, base = 0;
    REQUIRE(backend.start({.blockSize = 64, .midiInputs = true}, [&](auto, auto, unsigned frames) {
        for (auto event : backend.midiInput()) {
            event.frame += base;
            events[count++] = event;
        }
        base += frames;
    }));
    CHECK(backend.midiPorts() == std::vector<std::string>{"Keyboard", "Pads"});
    midi.send(0, 64, 211, midiWord(0x90, 61, 100));
    midi.send(0, 63, 211, midiWord(0x90, 60, 100));  // deliberately unsorted arrival
    midi.send(1, 210, 211, midiWord(0xb1, 7, 99));
    midi.send(1, 128, 211, midiWord(0xc1, 8, 0));
    midi.buffer->push(1, 9, midiWord(0x91, 59, 80));  // late -> first frame
    midi.buffer->push(1, 11, midiWord(0x81, 59, 0));  // future -> last frame
    driver.drive(211);
    REQUIRE(count == 6);
    const unsigned expected[]{0, 63, 64, 128, 210, 210};
    for (unsigned i = 0; i < count; ++i) CHECK(events[i].frame == expected[i]);
    CHECK(events[3].size == 2);
    CHECK(events[2].port == 0);
    CHECK(events[4].port == 1);
    CHECK(backend.midiInput().empty());
    count = base = 0;
    driver.drive(211);
    CHECK(count == 0);
    backend.stop();
    CHECK(midi.stops == 1);
    REQUIRE(backend.start({.midiInputs = true}, [](auto, auto, auto) {}));
    CHECK(backend.midiDropped() == 0);
    backend.stop();
    CHECK(midi.stops == 2);
}
TEST_CASE("native MIDI rejects system messages, bounds queues and counts period overflow") {
    Driver driver;
    auto source = std::make_unique<FakeMidi>();
    auto& midi = *source;
    PortAudioBackend backend({}, Driver::api, std::move(source));
    unsigned count = 0;
    REQUIRE(backend.start({.midiInputs = true}, [&](auto, auto, auto) {
        count += static_cast<unsigned>(backend.midiInput().size());
    }));
    for (unsigned i = 0; i < detail::kMidiQueueSize + 10; ++i)
        midi.send(0, 0, 256, midiWord(0x90, 60, 100));
    midi.buffer->push(1, 10, 0xf0);
    midi.buffer->push(1, 10, 0xfe);  // active sensing (clock goes to syncMidi(), ADR-0071)
    midi.buffer->push(1, 10, midiWord(0x90, 255, 100));
    driver.drive(256);
    CHECK(count == kMaxMidiInputEvents);
    CHECK(backend.midiDropped() == detail::kMidiQueueSize + 10 - kMaxMidiInputEvents + 3);
    backend.stop();
}
TEST_CASE("native MIDI delivers sync messages in time order beside channel messages") {
    auto buffer = std::make_unique<detail::MidiInputBuffer>();
    buffer->names = {"Clock", "Timecode"};
    // A period of 64 frames ending at 10 s (48 kHz): frame f is 10 - (64 - f) / 48000.
    auto at = [](int frame) { return 10 + (frame - 64) / 48000.0; };
    buffer->push(0, at(40), 0xf8);
    buffer->push(0, at(2), 0xfa);
    buffer->push(1, at(10), midiWord(0xf1, 0x23, 0));
    buffer->push(0, at(20), midiWord(0xf2, 0x10, 0x01));
    const std::uint8_t full[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 2, 3, 4, 0xF7};
    buffer->pushSync(1, at(30), full, 10);
    const std::uint8_t other[4] = {0xF0, 0x41, 0x10, 0xF7};
    buffer->pushSync(1, at(31), other, 4);  // not a sync message: a drop
    buffer->push(0, at(50), midiWord(0x90, 60, 100));
    buffer->collect(10, 48000, 64);
    const auto first = buffer->syncChunk(0, 32);
    REQUIRE(first.size() == 4);
    CHECK(first[0].bytes[0] == 0xfa);
    CHECK(first[0].frame == 2);
    CHECK(first[1].bytes[0] == 0xf1);
    CHECK(first[1].bytes[1] == 0x23);
    CHECK(first[1].port == 1);
    CHECK(first[2].size == 3);
    CHECK(first[2].bytes[2] == 0x01);
    CHECK(first[3].size == 10);
    CHECK(first[3].frame == 30);
    const auto second = buffer->syncChunk(32, 32);
    REQUIRE(second.size() == 1);
    CHECK(second[0].bytes[0] == 0xf8);
    CHECK(second[0].frame == 8);
    CHECK(buffer->chunk(0, 64).size() == 1);
    CHECK(buffer->dropped() == 1);
}
TEST_CASE("WinMM timestamps retain their QPC anchor across 32-bit rollover") {
    CHECK(detail::winMidiSeconds(125, 10, 10.2) == doctest::Approx(10.125));
    CHECK(detail::winMidiSeconds(5, 10, 4294977.31) == doctest::Approx(4294977.301));
    CHECK(detail::winMidiSeconds(0xfffffffeU, 10, 4294977.295) == doctest::Approx(4294977.294));
}
TEST_CASE("native MIDI skips a refusing input while audio and other inputs run") {
    Driver driver;
    auto source = std::make_unique<FakeMidi>();
    auto& midi = *source;
    midi.refused = true;
    PortAudioBackend backend({}, Driver::api, std::move(source));
    unsigned received = 0;
    REQUIRE(backend.start({.midiInputs = true}, [&](auto, auto, auto) {
        received += static_cast<unsigned>(backend.midiInput().size());
    }));
    CHECK(backend.midiPorts() == std::vector<std::string>{"Pads"});
    const auto warnings = backend.midiWarnings();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].device == "Keyboard");
    CHECK(warnings[0].osError == 42);
    CHECK(warnings[0].hint == "close the other application and restart the engine");
    CHECK(midi.stops == 0);
    midi.send(0, 0, 256, midiWord(0x90, 60, 100));
    driver.drive(256);
    CHECK(received == 1);
    CHECK_FALSE(backend.status().restartNeeded);
    backend.stop();
    CHECK(midi.stops == 1);
    CHECK(driver.initialized == 0);
    CHECK(driver.closed == 1);
}
TEST_CASE("native MIDI source setup failure still permits audio startup") {
    Driver driver;
    auto source = std::make_unique<FakeMidi>();
    source->failed = true;
    PortAudioBackend backend({}, Driver::api, std::move(source));
    unsigned frames = 0;
    REQUIRE(backend.start({.midiInputs = true}, [&](auto, auto, unsigned n) { frames += n; }));
    CHECK(backend.midiPorts().empty());
    REQUIRE(backend.midiWarnings().size() == 1);
    CHECK(backend.midiWarnings()[0].message == "fixture setup failure");
    driver.drive(256);
    CHECK(frames == 256);
    backend.stop();
}
TEST_CASE("engine status serializes skipped MIDI input warnings") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-midi-status-" + std::to_string(supple::test::processId()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    Driver driver;
    auto source = std::make_unique<FakeMidi>();
    source->refused = true;
    PortAudioBackend backend({}, Driver::api, std::move(source));
    AppOptions options;
    options.workdir = dir;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto opened = EngineApp::create(options);
    REQUIRE(opened);
    auto& app = **opened;
    REQUIRE(app.startEngine(backend, {.midiInputs = true}, {.workers = 0}));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    app.attachCommands(dispatcher);
    auto result = dispatcher.execute(
        {"engine.status", json::Value::object(), {cmd::Origin::Type::Test, "midi-status", ""}});
    REQUIRE(result);
    CHECK((*result)["running"] == true);
    REQUIRE((*result)["midi_warnings"].size() == 1);
    CHECK((*result)["midi_warnings"].asArray()[0]["device"] == "Keyboard");
    CHECK((*result)["midi_warnings"].asArray()[0]["os_error"] == 42);
    CHECK((*result)["midi_warnings"].asArray()[0]["hint"] ==
          "close the other application and restart the engine");
    app.stopEngine();
}
TEST_CASE("native MIDI merges all port runs stably, including nonmonotonic timestamps") {
    for (bool reverse : {false, true}) {
        auto buffer = std::make_unique<detail::MidiInputBuffer>();
        for (unsigned port = 0; port < detail::kMidiInputPorts; ++port) {
            buffer->names.push_back("Input " + std::to_string(port));
            for (unsigned i = 0; i < 64; ++i) {
                const unsigned key = reverse ? 63 - i : i;
                const double seconds = 10 + (static_cast<double>(key / 2) - 64) / 48000;
                buffer->push(port, seconds, midiWord(0x90, key, port));
            }
        }
        buffer->collect(10, 48000, 64);
        const auto events = buffer->chunk(0, 64);
        REQUIRE(events.size() == kMaxMidiInputEvents);
        for (unsigned frame = 0; frame < 32; ++frame)
            for (unsigned port = 0; port < detail::kMidiInputPorts; ++port)
                for (unsigned tie = 0; tie < 2; ++tie) {
                    const auto& event = events[(frame * detail::kMidiInputPorts + port) * 2 + tie];
                    CHECK(event.frame == frame);
                    CHECK(event.port == port);
                    CHECK(event.bytes[1] == frame * 2 + (reverse ? 1 - tie : tie));
                }
        CHECK(buffer->dropped() == 0);
    }
}
TEST_CASE("native MIDI SPSC receives concurrent producers per device with bounded loss") {
    auto buffer = std::make_unique<detail::MidiInputBuffer>();
    buffer->names = {"Keys", "Pads"};
    std::atomic<unsigned> done{0};
    std::array<std::thread, 2> producers;
    for (unsigned port = 0; port < producers.size(); ++port)
        producers[port] = std::thread([&, port] {
            for (unsigned i = 0; i < 10000; ++i) buffer->push(port, 10, midiWord(0x90, 60, 100));
            done.fetch_add(1, std::memory_order_release);
        });
    std::uint64_t received = 0;
    do {
        buffer->collect(10, 48000, 64);
        received += buffer->chunk(0, 64).size();
    } while (done.load(std::memory_order_acquire) < 2);
    for (auto& producer : producers) producer.join();
    buffer->collect(10, 48000, 64);
    received += buffer->chunk(0, 64).size();
    CHECK(received + buffer->dropped() == 20000);
}
TEST_CASE("PortAudio MIDI routes, reaches learn and records the heard timeline in both executors") {
    for (bool ahead : {false, true}) {
        Driver driver;
        auto source = std::make_unique<FakeMidi>();
        auto& midi = *source;
        PortAudioBackend backend({}, Driver::api, std::move(source));
        auto instances = pool();
        MediaCache media;
        Engine engine(instances, media, {.anticipative = ahead, .workers = 0});
        REQUIRE(engine.start(backend, {.blockSize = 64, .midiInputs = true}));
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        model::Track track;
        track.parent = model::kMaster;
        track.name = "Keys";
        const auto tr = edit.create(track);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(tr); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.sine";
        const auto instrument = edit.create(fx);
        model::Route route;
        route.from.midiDevice = "keyboard";
        route.to.track = tr;
        route.midi = model::MidiRouting{1, 2};  // channel 1 -> 2
        const auto input = edit.create(route);
        edit.modify<model::Track>(tr, [&](auto& value) {
            value.chain.push_back(instrument);
            value.record.input = input;
            value.record.armed = true;
            value.record.monitor = model::Monitor::On;
        });
        REQUIRE(engine.load(edit.commit().value()));
        RecordTarget target;
        target.track = tr;
        target.midi = RecordTarget::Midi{midiPortMask(engine.midiPorts(), "key"), 1, 2};
        REQUIRE(engine.startRecording({target}));
        REQUIRE(engine.awaitPlan());
        engine.play();
        engine.awaitTransport();
        midi.send(1, 10, 211, midiWord(0x90, 65, 100));  // wrong device
        midi.send(0, 20, 211, midiWord(0x91, 66, 100));  // wrong channel
        midi.send(0, 64, 211, midiWord(0x90, 60, 100));
        midi.send(0, 192, 211, midiWord(0x80, 60, 0));
        driver.drive(211);
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        REQUIRE((*takes)[0].midi.size() == 2);
        // Existing recorder compensation: 48 input + 96 output frames.
        CHECK((*takes)[0].midi[0].timeline == -80);
        CHECK((*takes)[0].midi[0].bytes[0] == 0x91);
        CHECK((*takes)[0].midi[1].timeline == 48);
        CHECK(std::any_of(driver.out[0].begin() + 64, driver.out[0].begin() + 192,
                          [](float sample) { return sample != 0; }));
        CHECK(std::all_of(driver.out[0].begin(), driver.out[0].begin() + 64,
                          [](float sample) { return sample == 0; }));
        std::vector<ControlMidiEvent> controls;
        engine.takeControlMidi(controls);
        CHECK(controls.size() == 4);  // learn hears every input/channel
        engine.stop();
    }
}
#if defined(__APPLE__) || defined(_WIN32)
TEST_CASE("real native MIDI inputs enumerate and receive only on explicit opt-in") {
    const char* enabled = std::getenv("SUPPLE_MIDI_DEVICE_TEST");
    if (!enabled || std::string_view(enabled) != "1") return;
    Driver driver;  // MIDI alone: never open the audio device
    auto source = detail::nativeMidiSource();
    auto sink = detail::nativeMidiSink(source.get());
    PortAudioBackend backend({}, Driver::api, std::move(source), std::move(sink));
    REQUIRE(backend.start({.midiInputs = true, .midiOutputs = true}, [](auto, auto, auto) {}));
    CHECK_FALSE(backend.midiPorts().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    driver.drive(256);
    backend.stop();
}
#endif

TEST_CASE("native MIDI topology changes are advisory and audio monitoring continues") {
    Driver driver;
    auto api = Driver::api;
    api.native = true;
    api.deviceRate = [](PaStream*, PaDeviceIndex) {
        return Driver::current->hardwareRate.load(std::memory_order_acquire);
    };
    MainThreadWake wake;
    auto source = std::make_unique<FakeMidi>();
    source->topology.store(true);
    PortAudioBackend backend({}, api, std::move(source));
    backend.setMainThreadWake(&wake);
    unsigned frames = 0;
    REQUIRE(backend.start({.midiInputs = true}, [&](auto, auto, unsigned n) { frames += n; }));
    WaitItem item{wake.waitable(), kWaitRead, 0};
    REQUIRE(waitFor(std::span(&item, 1), -1) > 0);
    CHECK(wake.drain());
    REQUIRE_FALSE(backend.status().restartNeeded);
    CHECK(backend.status().restartReason.empty());
    CHECK(backend.status().midiDevicesChanged);
    driver.drive(256);
    CHECK(frames == 256);
    // A MIDI advisory must not end the monitor: audio-rate changes still wake it.
    driver.hardwareRate.store(44100);
    item = {wake.waitable(), kWaitRead, 0};
    REQUIRE(waitFor(std::span(&item, 1), -1) > 0);
    CHECK(wake.drain());
    CHECK(backend.status().restartNeeded);
    CHECK(backend.status().restartReason == "sample rate changed");
    backend.stop();
}

namespace {
struct FakeSink : detail::MidiSink {
    std::atomic<double> clock{10};
    std::mutex mutex;
    std::vector<detail::TimedMidiOutput> sent;
    std::atomic<unsigned> sentCount{0}, waits{0};
    std::atomic<unsigned> prepared{0}, released{0};
    std::atomic<bool> topology{false};
    unsigned starts = 0, stops = 0;
    bool fail = false;
    Status start() override {
        ++starts;
        if (fail)
            return Error("backend.midi", "fixture output setup failed", "/midi_outputs", "retry");
        names = {"Synth", "Clock"};
        latencySeconds = {.001, .004};
        warnings = {{"Busy synth", 5, "fixture output busy"}};
        return okStatus();
    }
    void stop() override { ++stops; }
    double now() const SUPPLE_RT override { return clock.load(std::memory_order_relaxed); }
    bool timestamped() const override { return true; }
    bool send(const detail::TimedMidiOutput& e) override {
        {
            std::lock_guard lock(mutex);
            sent.push_back(e);
        }
        sentCount.fetch_add(1, std::memory_order_release);
        return true;
    }
    bool changed() override { return topology.load(std::memory_order_acquire); }
    void prepareThread() override { prepared.fetch_add(1, std::memory_order_release); }
    void releaseThread() override { released.fetch_add(1, std::memory_order_release); }
    void waitUntil(double seconds) override {
        waits.fetch_add(1, std::memory_order_release);
        detail::MidiSink::waitUntil(seconds);
    }
};
}  // namespace
TEST_CASE("PortAudio MIDI output maps its device clock into the sink clock across chunks") {
    Driver driver;
    auto sink = std::make_unique<FakeSink>();
    auto& output = *sink;
    PortAudioBackend backend({}, Driver::api, {}, std::move(sink));
    bool accepted = true;
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [&](auto, auto, unsigned n) {
        if (n) {
            accepted = backend.writeMidi({n - 1, 1, 2, {0xf1, 4}}) && accepted;
            accepted = backend.writeMidi({0, 0, 1, {0xf8}}) && accepted;
        }
    }));
    CHECK(output.starts == 1);
    CHECK(backend.midiOutputPorts() == std::vector<std::string>{"Synth", "Clock"});
    CHECK(backend.midiOutputLatency() == std::vector<std::int64_t>{48, -96});
    REQUIRE(backend.midiOutputWarnings().size() == 1);
    CHECK(backend.midiOutputWarnings()[0].osError == 5);
    // A different PortAudio epoch: output frame zero reaches DAC at 100.012,
    // currentTime is 100. Sink time is 10; audio latency is .002 seconds.
    PaStreamCallbackTimeInfo time{100, 100, 100.012};
    driver.drive(211, 0, &time);
    CHECK(accepted);
    while (output.sentCount.load(std::memory_order_acquire) < 8) std::this_thread::yield();
    {
        std::lock_guard lock(output.mutex);
        REQUIRE(output.sent.size() == 8);
        // Independent port queues can become visible between sender scans.
        // Compare their requested due times, preserving the port association.
        auto sent = output.sent;
        std::sort(sent.begin(), sent.end(),
                  [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
        const unsigned frames[] = {0, 63, 64, 127, 128, 191, 192, 210};
        for (unsigned n = 0; n < 8; ++n) {
            CHECK(sent[n].seconds == doctest::Approx(10.01 + frames[n] / 48000.0));
            CHECK(sent[n].event.port == n % 2);
        }
    }
    CHECK(backend.midiOutDropped() == 0);
    backend.stop();
    CHECK(output.stops == 1);
    CHECK(output.prepared.load() == 1);
    CHECK(output.released.load() == 1);
    REQUIRE(backend.start({.midiOutputs = true}, [](auto, auto, auto) {}));
    backend.stop();
    CHECK(output.starts == 2);
    CHECK(output.released.load() == 2);
}
TEST_CASE("native MIDI output setup refusal is nonfatal and command status reports it") {
    Driver driver;
    auto sink = std::make_unique<FakeSink>();
    sink->fail = true;
    PortAudioBackend backend({}, Driver::api, {}, std::move(sink));
    unsigned frames = 0;
    REQUIRE(backend.start({.midiOutputs = true}, [&](auto, auto, unsigned n) { frames += n; }));
    driver.drive(256);
    CHECK(frames == 256);
    CHECK(backend.midiOutputPorts().empty());
    REQUIRE(backend.midiOutputWarnings().size() == 1);
    CHECK(backend.midiOutputWarnings()[0].message == "fixture output setup failed");
    CHECK(backend.midiOutputWarnings()[0].hint == "retry");
    backend.stop();
}
TEST_CASE("MIDI hardware output listing, warning and latency survive the command boundary") {
    Driver driver;
    auto sink = std::make_unique<FakeSink>();
    PortAudioBackend backend({}, Driver::api, {}, std::move(sink));
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-midi-output-" + std::to_string(test::processId()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    AppOptions options;
    options.workdir = dir;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto opened = EngineApp::create(options);
    REQUIRE(opened);
    auto& app = **opened;
    REQUIRE(app.startEngine(backend, {.midiOutputs = true}, {.workers = 1}));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    app.attachCommands(dispatcher);
    auto status = dispatcher.execute(
        {"engine.status", json::Value::object(), {cmd::Origin::Type::Test, "midi-output", ""}});
    REQUIRE(status);
    CHECK((*status)["midi_out_dropped"] == 0);
    REQUIRE((*status)["midi_output_latency"].asArray().size() == 2);
    CHECK((*status)["midi_output_latency"].asArray()[1] == -96);
    REQUIRE((*status)["midi_output_warnings"].asArray().size() == 1);
    CHECK((*status)["midi_output_warnings"].asArray()[0]["device"] == "Busy synth");
    const auto& outputPorts = (*status)["midi_outputs"].asArray();
    REQUIRE(outputPorts.size() == 3);
    CHECK(*outputPorts[2].find("name") == "Busy synth");
    CHECK(outputPorts[2].find("index")->asInt() == -1);
    CHECK(outputPorts[2].find("warnings")->asArray()[0].find("os_error")->asInt() == 5);
    auto ports = dispatcher.execute(
        {"engine.inputs", json::Value::object(), {cmd::Origin::Type::Test, "midi-output", ""}});
    REQUIRE(ports);
    REQUIRE((*ports)["midi_outputs"].asArray().size() == 2);
    CHECK((*ports)["midi_outputs"].asArray()[0] == "Synth");
    app.stopEngine();
}

TEST_CASE("native sender shutdown discards future messages without waiting for due time") {
    struct WaitingSink : FakeSink {
        bool timestamped() const override { return false; }
    };
    Driver driver;
    auto sink = std::make_unique<WaitingSink>();
    auto& output = *sink;
    PortAudioBackend backend({}, Driver::api, {}, std::move(sink));
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [&](auto, auto, unsigned n) {
        if (n) backend.writeMidi({0, 0, 1, {0xf8}});
    }));
    PaStreamCallbackTimeInfo time{100, 100, 110};
    driver.drive(64, 0, &time);  // event due ten seconds ahead; fake clock never advances
    backend.stop();
    CHECK(backend.midiOutDropped() == 1);
    CHECK(output.sentCount.load() == 0);
    CHECK(output.released.load() == 1);
    CHECK(output.stops == 1);
}

TEST_CASE("native output topology is advisory and leaves audio running") {
    Driver driver;
    auto api = Driver::api;
    api.native = true;  // enables the production device monitor with fake devices
    auto sink = std::make_unique<FakeSink>();
    auto& output = *sink;
    output.topology.store(true);
    PortAudioBackend backend({}, api, {}, std::move(sink));
    unsigned frames = 0;
    REQUIRE(backend.start({.midiOutputs = true}, [&](auto, auto, unsigned n) { frames += n; }));
    while (!backend.status().midiDevicesChanged) std::this_thread::yield();
    CHECK_FALSE(backend.status().restartNeeded);
    driver.drive(256);
    CHECK(frames == 256);
    CHECK(backend.midiOutputPorts().size() == 2);
    backend.stop();
}

TEST_CASE("native output snapshots host time before collecting MIDI input") {
    Driver driver;
    auto sink = std::make_unique<FakeSink>();
    auto& output = *sink;
    struct BusyInput : FakeMidi {
        std::atomic<double>& outputClock;
        explicit BusyInput(std::atomic<double>& outputClockRef) : outputClock(outputClockRef) {}
        double now() const SUPPLE_RT override {
            outputClock.store(20, std::memory_order_relaxed);  // simulate receive work after entry
            return 10;
        }
    };
    auto source = std::make_unique<BusyInput>(output.clock);
    PortAudioBackend backend({}, Driver::api, std::move(source), std::move(sink));
    REQUIRE(backend.start({.midiInputs = true, .midiOutputs = true}, [&](auto, auto, unsigned n) {
        if (n) backend.writeMidi({0, 0, 1, {0xf8}});
    }));
    driver.drive(256);
    while (!output.sentCount.load(std::memory_order_acquire)) std::this_thread::yield();
    {
        std::lock_guard lock(output.mutex);
        REQUIRE(output.sent.size() == 1);
        CHECK(output.sent[0].seconds == 10);
    }
    backend.stop();
}

TEST_CASE("native output releases survive the audio-device-loss final callback fallback") {
    Driver driver;
    driver.active = 0;
    auto api = Driver::api;
    api.threaded = true;
    auto sink = std::make_unique<FakeSink>();
    auto& output = *sink;
    PortAudioBackend backend({}, api, {}, std::move(sink));
    unsigned finals = 0;
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [&](auto, auto, unsigned frames) {
        if (!frames) {
            ++finals;
            CHECK(output.stops == 0);
            CHECK(backend.writeMidi({0, 0, 3, {0x80, 60, 0}}));
        }
    }));
    backend.stop();
    CHECK(finals == 1);
    CHECK(output.stops == 1);
    REQUIRE(output.sent.size() == 1);
    CHECK(output.sent[0].event.bytes[0] == 0x80);
    CHECK(output.sent[0].seconds == output.clock.load());
}
