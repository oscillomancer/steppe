// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The JACK backend against a stand-in libjack (fake_jack.cpp): no server and
// no device. The real server is exercised only on request:
//   SUPPLE_AUDIO_DEVICE_TEST=1 engine.tests -tc='*real JACK*'
#include <doctest/doctest.h>
#include <supple/base/environment.h>

#include <pthread.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

#include <supple/engine/bench_probe.h>
#include <supple/engine/jack.h>

#include "fake_jack.h"

using namespace supple;
using namespace supple::engine;

namespace {

// Records what the backend passes to the engine's callback.
struct Recorder {
    std::atomic<std::uint64_t> calls{0}, frames{0};
    std::atomic<std::uint32_t> maxFrames{0}, lastFrames{0};
    std::atomic<int> finals{0};
    std::atomic<bool> finalOnAudioThread{false};
    std::atomic<std::thread::id> audioThread{};
    std::atomic<float> firstInput{0.0f};

    Backend::Callback callback() {
        return [this](const float* const* in, float* const* out, std::uint32_t n) {
            if (n == 0) {
                finals.fetch_add(1);
                finalOnAudioThread = audioThread.load() == std::this_thread::get_id();
                return;
            }
            audioThread = std::this_thread::get_id();
            calls.fetch_add(1);
            frames.fetch_add(n);
            lastFrames = n;
            if (n > maxFrames.load()) maxFrames = n;
            if (in) firstInput = in[0][0];
            for (std::uint32_t i = 0; i < n; ++i) out[0][i] = out[1][i] = 0.25f;
        };
    }
};

JackBackend::Options fakeOptions() {
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    o.clientName = "supple";
    return o;
}

bool waitFor(const std::function<bool()>& pred, double seconds = 5.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST_CASE("a missing libjack is a clean error") {
    JackBackend::Options o;
    o.library = "libsupple-no-such-jack.so.0";
    JackBackend backend(o);
    Recorder rec;
    auto st = backend.start(BackendConfig{}, rec.callback());
    REQUIRE_FALSE(st);
    CHECK(st.error().code == "backend.unavailable");
    CHECK_FALSE(JackBackend::available(o.library));
    CHECK(JackBackend::available(FakeJack::kPath));
    backend.stop();  // harmless when not started
}

TEST_CASE("an unreachable server is a clean error") {
    FakeJack& fake = fakeJack();
    fake.reset();
    fake.failOpen(1);
    JackBackend backend(fakeOptions());
    Recorder rec;
    auto st = backend.start(BackendConfig{}, rec.callback());
    REQUIRE_FALSE(st);
    CHECK(st.error().code == "backend.open");
    CHECK(st.error().message.find("server failed") != std::string::npos);
    fake.reset();
}

TEST_CASE("capture names retain channel indices when a connection fails") {
    FakeJack& fake = fakeJack();
    fake.reset();
    fake.failCapture(1);
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.inputs = 2;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(backend.inputNames() == std::vector<std::string>{"", "system:capture_2"});
    backend.stop();
    fake.reset();
}

TEST_CASE("playback ports: one engine output each; extra outputs stay unconnected (ADR-0056)") {
    FakeJack& fake = fakeJack();
    fake.reset();
    auto names = JackBackend::playbackNames(fakeOptions());
    REQUIRE(names);
    CHECK(*names == std::vector<std::string>{"system:playback_1", "system:playback_2"});
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.outputs = 4;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(backend.outputNames() ==
          std::vector<std::string>{"system:playback_1", "system:playback_2", "", ""});
    backend.stop();
    fake.reset();
}

TEST_CASE("the backend runs the engine in blocks, whatever the graph's period") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    ::unsetenv("PIPEWIRE_LATENCY");
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 256;
    REQUIRE(backend.start(cfg, rec.callback()));
    // The latency request reached the client and left the environment clean.
    CHECK(std::string(fake.latencyEnv()) == "256/48000");
    CHECK(std::getenv("PIPEWIRE_LATENCY") == nullptr);
    CHECK(fake.connections() == 2);
    CHECK(backend.config().sampleRate == 48000.0);
    CHECK(backend.config().blockSize == 256);

    REQUIRE(waitFor([&] { return rec.calls.load() >= 40; }));
    CHECK(rec.maxFrames.load() == 256);
    CHECK(backend.stats().period == 1024);
    CHECK(backend.stats().outputLatency == 512);
    CHECK(static_cast<double>(fake.outputPeak("supple:out_1")) == doctest::Approx(0.25));

    // A smaller period passes through; an odd one is split with a remainder.
    fake.setPeriod(300);
    REQUIRE(waitFor([&] { return backend.stats().period == 300; }));
    const auto before = rec.calls.load();
    REQUIRE(waitFor([&] { return rec.calls.load() > before + 20; }));
    CHECK(rec.maxFrames.load() == 256);
    fake.setPeriod(64);
    REQUIRE(waitFor([&] { return backend.stats().period == 64; }));
    REQUIRE(waitFor([&] { return rec.lastFrames.load() == 64; }));

    backend.stop();
    CHECK(rec.finals.load() == 1);
    CHECK(rec.finalOnAudioThread.load());
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("unused JACK device outputs are silent for the stereo engine") {
    FakeJack& fake = fakeJack();
    fake.reset(300);
    JackBackend backend(fakeOptions());
    Recorder rec;
    REQUIRE(backend.start({.blockSize = 64, .outputs = 4}, rec.callback()));
    REQUIRE(waitFor([&] { return fake.cycles() >= 10; }));
    CHECK(fake.outputPeak("supple:out_1") == 0.25f);
    CHECK(fake.outputPeak("supple:out_2") == 0.25f);
    CHECK(fake.outputPeak("supple:out_3") == 0.f);
    CHECK(fake.outputPeak("supple:out_4") == 0.f);
    backend.stop();
    fake.reset();
}

TEST_CASE("a user's PIPEWIRE_LATENCY wins; forceBufferSize asks for the period") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    ::setenv("PIPEWIRE_LATENCY", "512/48000", 1);
    auto o = fakeOptions();
    o.forceBufferSize = true;
    JackBackend backend(o);
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 64;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(std::string(fake.latencyEnv()) == "512/48000");
    CHECK(std::string(std::getenv("PIPEWIRE_LATENCY")) == "512/48000");
    CHECK(fake.bufferSizeRequest() == 64);
    backend.stop();
    ::unsetenv("PIPEWIRE_LATENCY");
    fake.reset();
}

TEST_CASE("a requested rate is a suggestion; forcing holds the quantum and rate while active") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    for (const char* name : {"PIPEWIRE_LATENCY", "PIPEWIRE_RATE", "PIPEWIRE_PROPS"})
        ::unsetenv(name);
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 128;
    {
        auto o = fakeOptions();
        o.requestRate = 44100;
        JackBackend backend(o);
        REQUIRE(backend.start(cfg, rec.callback()));
        CHECK(std::string(fake.latencyEnv()) == "128/44100");
        CHECK(std::string(fake.rateEnv()) == "1/44100");
        CHECK(std::string(fake.propsEnv()).empty());
        // The graph keeps its own rate: the backend runs at what it was given.
        CHECK(backend.config().sampleRate == 48000.0);
        backend.stop();
    }
    {
        auto o = fakeOptions();
        o.requestRate = 96000;
        o.forceGraph = true;
        JackBackend backend(o);
        REQUIRE(backend.start(cfg, rec.callback()));
        CHECK(std::string(fake.rateEnv()) == "1/96000");
        CHECK(std::string(fake.propsEnv()) == "{ node.force-quantum=128 node.force-rate=96000 }");
        backend.stop();
    }
    {
        auto o = fakeOptions();
        o.forceGraph = true;  // no rate: only the quantum is forced
        JackBackend backend(o);
        REQUIRE(backend.start(cfg, rec.callback()));
        CHECK(std::string(fake.rateEnv()).empty());
        CHECK(std::string(fake.propsEnv()) == "{ node.force-quantum=128 }");
        backend.stop();
    }
    for (const char* name : {"PIPEWIRE_LATENCY", "PIPEWIRE_RATE", "PIPEWIRE_PROPS"})
        CHECK(std::getenv(name) == nullptr);
    {
        // The user's own properties win over forcing.
        ::setenv("PIPEWIRE_PROPS", "{ node.latency=64/48000 }", 1);
        auto o = fakeOptions();
        o.forceGraph = true;
        JackBackend backend(o);
        REQUIRE(backend.start(cfg, rec.callback()));
        CHECK(std::string(fake.propsEnv()) == "{ node.latency=64/48000 }");
        backend.stop();
        CHECK(std::string(std::getenv("PIPEWIRE_PROPS")) == "{ node.latency=64/48000 }");
        ::unsetenv("PIPEWIRE_PROPS");
    }
    {
        auto o = fakeOptions();
        o.requestRate = -1;
        JackBackend backend(o);
        auto refused = backend.start(cfg, rec.callback());
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "backend.config");
        CHECK(refused.error().path == "/rate");
    }
    fake.reset();
}

TEST_CASE("inputs reach the callback") {
    FakeJack& fake = fakeJack();
    fake.reset(128);
    fake.setInput(0.5f);
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.inputs = 2;
    REQUIRE(backend.start(cfg, rec.callback()));
    REQUIRE(waitFor([&] { return rec.firstInput.load() == 0.5f; }));
    backend.stop();
    fake.reset();
}

TEST_CASE("xruns are counted and a rate change is reported") {
    FakeJack& fake = fakeJack();
    fake.reset();
    JackBackend backend(fakeOptions());
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    CHECK_FALSE(backend.stats().rateChanged);  // the registration call carries the same rate
    fake.xrun();
    fake.xrun();
    fake.xrun();
    CHECK(backend.stats().xruns == 3);
    fake.setRate(44100);
    CHECK(backend.stats().rateChanged);
    backend.stop();
    fake.reset();
}

TEST_CASE("the backend reports the graph's rate, not the requested one") {
    FakeJack& fake = fakeJack();
    fake.reset();
    fake.setRate(44100);
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.sampleRate = 48000.0;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(backend.config().sampleRate == 44100.0);
    backend.stop();
    fake.reset();
}

TEST_CASE("a rate PipeWire reports before the first cycle neither sticks nor restarts") {
    // A 44.1 kHz graph (a browser started it) tells a starting client 48 kHz,
    // then flips while it activates. Reading that rate made the engine run at
    // the wrong one and restart for ever, glitching every other client.
    FakeJack& fake = fakeJack();
    fake.reset();
    fake.setRate(44100);
    fake.setStaleRate(48000);
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.sampleRate = 48000.0;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(backend.config().sampleRate == 44100.0);
    CHECK_FALSE(backend.stats().rateChanged);
    CHECK_FALSE(backend.status().restartNeeded);
    fake.setRate(48000);  // a real change later still restarts
    CHECK(backend.status().restartNeeded);
    backend.stop();
    fake.reset();
}

TEST_CASE("stop closes the client when no cycle comes") {
    FakeJack& fake = fakeJack();
    fake.reset();
    auto o = fakeOptions();
    o.stopTimeoutSeconds = 0.05;
    JackBackend backend(o);
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    REQUIRE(waitFor([&] { return rec.calls.load() > 0; }));
    fake.stall(1);  // returns with no cycle in flight
    backend.stop();
    CHECK(rec.finals.load() == 0);  // no final callback: the engine recovers itself
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("a server shutdown is reported and stop does not wait") {
    FakeJack& fake = fakeJack();
    fake.reset();
    auto o = fakeOptions();
    o.stopTimeoutSeconds = 60.0;  // must not be spent waiting for a final cycle
    JackBackend backend(o);
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    REQUIRE(waitFor([&] { return rec.calls.load() > 0; }));
    fake.shutdown();
    CHECK(backend.stats().serverGone);
    const auto t0 = std::chrono::steady_clock::now();
    backend.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
    CHECK(rec.finals.load() == 0);
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("the backend restarts") {
    FakeJack& fake = fakeJack();
    fake.reset();
    JackBackend backend(fakeOptions());
    for (int round = 0; round < 3; ++round) {
        Recorder rec;
        REQUIRE(backend.start(BackendConfig{}, rec.callback()));
        CHECK_FALSE(backend.start(BackendConfig{}, rec.callback()));  // already running
        REQUIRE(waitFor([&] { return rec.calls.load() > 0; }));
        backend.stop();
        CHECK(rec.finals.load() == 1);
    }
}

// Opens the real server for two seconds of silence at a 64-frame block and
// reports what the graph granted. Opt-in: it uses the audio device.
TEST_CASE("real JACK: silence at 64 frames" *
          doctest::skip(!supple::environmentFlagEnabled("SUPPLE_AUDIO_DEVICE_TEST"))) {
    JackBackend::Options o;
    o.clientName = "supple-device-test";
    JackBackend backend(o);
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 64;
    const Backend::Callback record = rec.callback();
    auto st = backend.start(cfg, [&](const float* const* in, float* const* out, std::uint32_t n) {
        record(in, out, n);
        for (std::uint32_t i = 0; i < n; ++i) out[0][i] = out[1][i] = 0.0f;  // silence
    });
    REQUIRE_MESSAGE(st, (st ? "" : st.error().toString()));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto s = backend.stats();
    MESSAGE("rate " << backend.config().sampleRate << " Hz, period " << s.period
                    << " frames, output latency " << s.outputLatency << " frames, " << s.cycles
                    << " cycles, " << s.xruns << " xruns");
    backend.stop();
    CHECK(s.cycles > 0);
    CHECK(rec.maxFrames.load() <= 64);
    CHECK(rec.finals.load() == 1);
    CHECK(rec.finalOnAudioThread.load());
}

TEST_CASE("MIDI from each device reaches the callback in its block (ADR-0016)") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);  // four engine blocks of 256 per period
    JackBackend backend(fakeOptions());
    // Recorded lock-free: the callback runs in the real-time process callback.
    std::array<std::array<int, 4>, 64> seen{};  // {block frame, port, status, data1}
    std::atomic<int> count{0};
    std::atomic<bool> listening{false};
    std::atomic<std::uint64_t> blocks{0};
    BackendConfig cfg;
    cfg.midiInputs = true;
    REQUIRE(backend.start(cfg, [&](const float* const*, float* const* out, std::uint32_t n) {
        if (n == 0) return;
        const auto b = blocks.fetch_add(1);
        if (listening.load(std::memory_order_acquire))
            for (const MidiInputEvent& e : backend.midiInput()) {
                const int k = count.load(std::memory_order_relaxed);
                if (k >= static_cast<int>(seen.size())) break;
                seen[static_cast<std::size_t>(k)] = {static_cast<int>(b % 4 * 256 + e.frame),
                                                     e.port, e.bytes[0], e.bytes[1]};
                count.store(k + 1, std::memory_order_release);
            }
        for (std::uint32_t i = 0; i < n; ++i) out[0][i] = out[1][i] = 0.0f;
    }));
    const std::vector<std::string> ports = backend.midiPorts();
    REQUIRE(ports.size() == 2);
    CHECK(ports[1] == "USB Keyboard:midi_out");
    CHECK(midiPortMask(ports, "keyboard") == 2u);
    CHECK(midiPortMask(ports, "*") == 3u);
    CHECK(midiPortMask(ports, "nothing") == 0u);
    // A note on the keyboard at 300 (the second block), a CC on the first
    // device at 900 (the fourth), and a system message that is not passed on.
    REQUIRE(waitFor([&] { return blocks.load() >= 8; }));
    listening.store(true, std::memory_order_release);
    fake.midi(1, 300, 0x90, 60, 100);
    fake.midi(0, 900, 0xb0, 7, 90);
    fake.midi(0, 10, 0xf8, 0, 0);
    REQUIRE(waitFor([&] { return count.load(std::memory_order_acquire) >= 2; }));
    backend.stop();
    REQUIRE(count.load() == 2);
    CHECK(seen[0] == std::array<int, 4>{300, 1, 0x90, 60});
    CHECK(seen[1] == std::array<int, 4>{900, 0, 0xb0, 7});
    fake.reset();
}

TEST_CASE("JACK rejects an invalid rate before constructing a latency request") {
    for (double rate : {0.0, -1.0, 768001.0, 1e300, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        JackBackend backend({.library = FAKE_JACK_PATH});
        const auto status = backend.start({.sampleRate = rate},
                                          [](const float* const*, float* const*, std::uint32_t) {});
        CHECK_FALSE(status);
        if (!status) CHECK(status.error().code == "backend.config");
    }
}

TEST_CASE("bench callback probe measures complete JACK periods, not engine chunks") {
    FakeJack& fake = fakeJack();
    fake.reset(128);
    BenchProbe probe(10000);
    probe.recording = true;
    auto options = fakeOptions();
    options.benchProbe = &probe;
    JackBackend backend(options);
    Recorder rec;
    REQUIRE(backend.start({.blockSize = 64}, rec.callback()));
    REQUIRE(waitFor([&] { return rec.calls.load() >= 20; }));
    backend.stop();
    CHECK(probe.callback.samples().size() == fake.cycles());
    CHECK(probe.callback.dropped == 0);
    REQUIRE_FALSE(probe.callback.samples().empty());
    for (const auto& sample : probe.callback.samples()) CHECK(sample.frames == 128);
    CHECK(rec.frames.load() == rec.calls.load() * 64);
    CHECK(fake.bufferSizeRequest() == 0);
    fake.reset();
}

TEST_CASE("JACK live helper setup needs a started backend and accepts non-RT test threads") {
    FakeJack& fake = fakeJack();
    fake.reset();
    JackBackend backend(fakeOptions());
    CHECK(backend.liveThreadCapacity() == 0);
    auto unavailable = backend.prepareLiveThread(::pthread_self());
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error().code == "engine.live_threads");
    CHECK_FALSE(unavailable.error().hint.empty());
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    // The fake JACK callback is normal scheduling. The calling test thread is
    // a normal helper stand-in; setup must not promote it or require RT rights.
    int beforePolicy = 0, afterPolicy = 0;
    sched_param before{}, after{};
    REQUIRE(::pthread_getschedparam(::pthread_self(), &beforePolicy, &before) == 0);
    REQUIRE(backend.prepareLiveThread(::pthread_self()));
    REQUIRE(::pthread_getschedparam(::pthread_self(), &afterPolicy, &after) == 0);
    CHECK(afterPolicy == beforePolicy);
    CHECK(after.sched_priority == before.sched_priority);
    backend.stop();
}

TEST_CASE("JACK reports its callback thread's scheduling once started") {
    FakeJack& fake = fakeJack();
    fake.reset();
    JackBackend backend(fakeOptions());
    CHECK(backend.callbackPolicy() == ThreadPolicy{});  // no client yet
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    // The fake library's process thread is an ordinary thread.
    CHECK(backend.callbackPolicy() == ThreadPolicy{ThreadPolicy::Kind::Normal, 0});
    backend.stop();
    CHECK(backend.callbackPolicy() == ThreadPolicy{});
}

TEST_CASE("JACK MIDI outputs connect, clear periods, sort frames and report signed latency") {
    auto& fake = fakeJack();
    fake.reset(211);
    JackBackend backend(fakeOptions());
    std::atomic<bool> emit{false}, done{false};
    unsigned chunk = 0;
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [&](auto, auto, unsigned n) {
        if (!n || !emit.load(std::memory_order_acquire)) return;
        // Two callbacks in one 211-frame period. Deliberately unsorted ties.
        if (!chunk) {
            backend.writeMidi({63, 0, 1, {0xf8}});
            backend.writeMidi({1, 1, 3, {0xf2, 1, 2}});
            backend.writeMidi({1, 1, 2, {0xf1, 3}});
            backend.writeMidi({0, 0, 10, {0xf0, 0x7f, 0x7f, 1, 1, 0, 2, 3, 4, 0xf7}});
        } else if (chunk == 1) {
            backend.writeMidi({0, 0, 1, {0xfa}});
        }
        ++chunk;
        if (chunk == 4) {
            emit.store(false);
            done.store(true, std::memory_order_release);
        }
    }));
    CHECK(backend.midiOutputPorts() ==
          std::vector<std::string>{"system:midi_playback_1", "Synth:midi_in"});
    CHECK(backend.midiOutputLatency() == std::vector<std::int64_t>{384, -256});
    CHECK(fake.connections() == 4);  // two audio + two MIDI
    fake.topology();
    CHECK(backend.status().midiDevicesChanged);
    CHECK_FALSE(backend.status().restartNeeded);
    // Stall establishes the start of an entire device period.
    fake.stall(1);
    emit.store(true, std::memory_order_release);
    fake.stall(0);
    while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    fake.stall(1);
    REQUIRE(fake.midiWritten() == 5);
    const auto clears = fake.midiClears();
    CHECK(clears >= 2);
    const auto cycles = fake.cycles();
    FakeJack::WrittenMidi e;
    fake.midiRead(0, &e);
    CHECK(e.frame == 0);
    CHECK(e.size == 10);
    fake.midiRead(1, &e);
    CHECK(e.frame == 1);
    CHECK(e.bytes[0] == 0xf2);
    fake.midiRead(2, &e);
    CHECK(e.frame == 1);
    CHECK(e.bytes[0] == 0xf1);
    fake.midiRead(3, &e);
    CHECK(e.frame == 63);
    fake.midiRead(4, &e);
    CHECK(e.frame == 64);
    CHECK(backend.midiOutDropped() == 0);
    fake.stall(0);
    while (fake.cycles() <= cycles + 1) std::this_thread::yield();
    fake.stall(1);
    CHECK(fake.midiClears() > clears);
    CHECK(fake.midiWritten() == 5);  // no event repeated into subsequent periods
    fake.stall(0);
    backend.stop();
}

TEST_CASE("JACK MIDI output failures are best effort and writes count losses") {
    auto& fake = fakeJack();
    fake.reset(64);
    SUBCASE("port registration refusal") { fake.midiFail(0, 1, 0); }
    SUBCASE("connection refusal") { fake.midiFail(0, 0, 1); }
    JackBackend backend(fakeOptions());
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [](auto, auto, auto) {}));
    CHECK(backend.midiOutputPorts().empty());
    REQUIRE(backend.midiOutputWarnings().size() == 2);
    CHECK_FALSE(backend.midiOutputWarnings()[0].hint.empty());
    backend.stop();
    fake.reset(64);
    fake.midiFail(1, 0, 0);
    std::atomic<bool> emit{false}, done{false};
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [&](auto, auto, unsigned n) {
        if (!n || !emit.exchange(false, std::memory_order_acq_rel)) return;
        for (std::size_t k = 0; k <= kMaxMidiOutputEvents; ++k)
            backend.writeMidi({0, 0, 1, {0xf8}});
        done.store(true, std::memory_order_release);
    }));
    emit.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    fake.stall(1);
    CHECK(backend.midiOutDropped() == kMaxMidiOutputEvents + 1);
    CHECK(fake.midiWritten() == 0);
    fake.stall(0);
    backend.stop();
    fake.reset(64);
}
