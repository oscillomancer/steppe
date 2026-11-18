// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/main_thread_wake.h>
#include <supple/base/rt.h>
#include <supple/base/waitable.h>
#include <supple/host/clap_host.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <fcntl.h>
#include <cerrno>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <ostream>
#include <thread>
#include <vector>
#include "../src/bridge_client.h"
#include "../src/bridge_protocol.h"
#include "lifecycle_gate.h"
#include "process_pause.h"
using namespace supple;
using namespace supple::host;
namespace {
PluginInfo nativePlugin(const std::filesystem::path& module, std::string_view id = {}) {
    ScanOptions options;
    options.forceInProcess = true;
    auto list = scan(std::span(&module, 1), options);
    REQUIRE_MESSAGE(list, (list ? "" : list.error().toString()));
    for (const auto& info : *list)
        if ((!id.empty() && info.id == id) || (id.empty() && info.name == "Test VST3 Gain"))
            return info;
    FAIL("native bridge fixture not found");
    return {};
}
struct Audio {
    std::array<float, 64> left{}, right{}, outLeft{}, outRight{};
    const float* in[2]{left.data(), right.data()};
    float* out[2]{outLeft.data(), outRight.data()};
    ProcessBlock block() {
        ProcessBlock b;
        b.inputs = in;
        b.outputs = out;
        b.inputChannels = b.outputChannels = 2;
        b.frames = 64;
        return b;
    }
};
void awaitWake(MainThreadWake& wake) {
    WaitItem item{wake.waitable()};
    REQUIRE(waitFor(std::span(&item, 1), 60000) ==
            1);  // guard against a lost event, not a timing assertion
    REQUIRE(wake.drain());
}
}  // namespace
#if defined(__linux__)
TEST_CASE("Linux bridge memory cannot shrink or grow but remains writable") {
    struct Memory {
        bridge::Resource fd = bridge::createMemory(4096);
        void* map = nullptr;
        ~Memory() {
            bridge::unmapMemory(map, 4096);
            bridge::closeResource(fd);
        }
    } memory;
    REQUIRE(memory.fd != bridge::noResource);
    const int seals = ::fcntl(memory.fd, F_GET_SEALS);
    REQUIRE(seals >= 0);
    CHECK((seals & (F_SEAL_SHRINK | F_SEAL_GROW)) == (F_SEAL_SHRINK | F_SEAL_GROW));
    CHECK(::ftruncate(memory.fd, 0) == -1);
    CHECK(errno == EPERM);
    CHECK(::ftruncate(memory.fd, 8192) == -1);
    CHECK(errno == EPERM);
    memory.map = bridge::mapMemory(memory.fd, 4096);
    REQUIRE(memory.map);
    auto* bytes = static_cast<unsigned char*>(memory.map);
    bytes[0] = 17;
    bytes[4095] = 23;
    CHECK(bytes[0] == 17);
    CHECK(bytes[4095] == 23);
}
#endif
TEST_CASE("bridge lookup resolves the platform executable suffix") {
    for (const std::filesystem::path& executable :
         {std::filesystem::path(BRIDGE_PATH), std::filesystem::path(SCANNER_PATH)}) {
        setBridgeExecutable({});
        REQUIRE(locateBridgeExecutable(executable.stem().string()));
        CHECK(std::filesystem::equivalent(bridgeExecutable(), executable));
    }
    setBridgeExecutable(BRIDGE_PATH);
}
#if defined(_WIN32)
TEST_CASE("Windows bridge initializes DPI awareness before creating plugins") {
    setBridgeExecutable(BRIDGE_PATH);
    const auto info = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge");
    auto made = Instance::create(info, {}, true);
    REQUIRE(made);
    const auto aware = (*made)->paramValue(13);
    REQUIRE(aware);
    CHECK(*aware == 1);
}
#endif
#if !defined(_WIN32)
TEST_CASE("native bridge launch uses the helper private process group") {
    setBridgeExecutable(BRIDGE_PATH);
    auto process = BridgeProcess::spawn();
    REQUIRE_MESSAGE(process, (process ? "" : process.error().toString()));
    CHECK(::getpgid((*process)->pid()) == (*process)->pid());
    CHECK(::getpgid((*process)->pid()) != ::getpgrp());
}
#endif
TEST_CASE("native bridge shared memory and wakes preserve exact CLAP and VST3 audio and state") {
    setBridgeExecutable(BRIDGE_PATH);
    for (const std::filesystem::path& module :
         {std::filesystem::path(FIXTURE_CLAP_PATH), std::filesystem::path(FIXTURE_VST3_PATH)}) {
        const auto info = nativePlugin(module, module == std::filesystem::path(FIXTURE_CLAP_PATH)
                                                   ? "com.suppleaudio.test.gain"
                                                   : "");
        auto local = Instance::create(info, {});
        auto remote = Instance::create(info, {}, true);
        REQUIRE(local);
        REQUIRE_MESSAGE(remote, (remote ? "" : remote.error().toString()));
        CHECK((*remote)->isolated());
        const auto params = (*local)->params();
        REQUIRE_FALSE(params.empty());
        REQUIRE((*remote)->setParam(params[0].id, params[0].def));
        auto state = (*remote)->saveState();
        REQUIRE(state);
        REQUIRE((*local)->loadState(*state));
        REQUIRE((*local)->activate(48000, 1, 64));
        REQUIRE((*remote)->activate(48000, 1, 64));
        REQUIRE((*local)->startProcessing());
        REQUIRE((*remote)->startProcessing());
        Audio a, b;
        for (std::size_t i = 0; i < a.left.size(); ++i) {
            a.left[i] = b.left[i] = static_cast<float>(i) / 128.0f;
            a.right[i] = b.right[i] = -a.left[i];
        }
        for (int cycle = 0; cycle < 200; ++cycle) {
            auto first = a.block(), second = b.block();
            Event change{.kind = Event::Kind::Param,
                         .time = 17,
                         .paramId = params[0].id,
                         .value = (*local)->paramsNormalized() ? 0.5 : params[0].def};
            first.events = second.events = std::span(&change, 1);
            REQUIRE((*local)->process(first));
            REQUIRE((*remote)->process(second));
            CHECK(a.outLeft == b.outLeft);
            CHECK(a.outRight == b.outRight);
        }
        (*remote)->reset();
        (*remote)->stopProcessing();
        (*local)->stopProcessing();
        CHECK_FALSE((*remote)->failed());
        (*remote)->deactivate();
        (*local)->deactivate();
    }
}
TEST_CASE("native bridge crashes isolate shared members and preserve a dedicated process") {
    setBridgeExecutable(BRIDGE_PATH);
    auto shared = createBridgeProcess();
    REQUIRE_MESSAGE(shared, (shared ? "" : shared.error().toString()));
    const auto gain = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.gain");
    const auto crash = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash");
    auto first = Instance::create(gain, {}, true, *shared);
    auto second = Instance::create(crash, {}, true, *shared);
    auto dedicated = Instance::create(gain, {}, true);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(dedicated);
    CHECK((*first)->bridgePid() == (*second)->bridgePid());
    CHECK((*first)->bridgePid() != (*dedicated)->bridgePid());
    for (auto* instance : {first->get(), second->get(), dedicated->get()}) {
        REQUIRE(instance->activate(48000, 1, 64));
        REQUIRE(instance->startProcessing());
    }
    Audio audio;
    audio.left.fill(0.25f);
    audio.right.fill(0.25f);
    REQUIRE((*second)->setParam(1, 1));
    REQUIRE((*second)->process(audio.block()));
    CHECK((*first)->failed());
    CHECK((*second)->failed());
    CHECK_FALSE((*dedicated)->failed());
    CHECK(std::all_of(audio.outLeft.begin(), audio.outLeft.end(), [](float v) { return v == 0; }));
    REQUIRE((*dedicated)->process(audio.block()));
    CHECK(audio.outLeft[0] == doctest::Approx(0.25));
    auto failure = (*second)->paramValue(1);
    REQUIRE_FALSE(failure);
    CHECK(failure.error().code == "host.bridge_failed");
    for (auto* instance : {first->get(), second->get(), dedicated->get()}) {
        instance->stopProcessing();
        instance->deactivate();
    }
}
TEST_CASE("native bridge deadlines kill a hung audio worker and return silence") {
    setBridgeExecutable(BRIDGE_PATH);
    // The deadline is process-wide (review round 26: tests that compare
    // bridged renders raise it); a hang fails at whatever it is set to.
    const int previous = bridgeDeadlineMs();
    struct Restore {
        int ms;
        ~Restore() { setBridgeDeadlineMs(ms); }
    } restore{previous};
    CHECK(previous >= 1);
    setBridgeDeadlineMs(0);
    CHECK(bridgeDeadlineMs() == 1);
    setBridgeDeadlineMs(previous);
    auto remote =
        Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash"), {}, true);
    REQUIRE(remote);
    REQUIRE((*remote)->activate(48000, 1, 64));
    REQUIRE((*remote)->startProcessing());
    REQUIRE((*remote)->setParam(1, 2));
    // Only the call that hangs meets the short deadline: the lifecycle
    // commands before it keep theirs, so a stall cannot fail them (review
    // round 30).
    setBridgeDeadlineMs(100);
    Audio audio;
    REQUIRE((*remote)->process(audio.block()));
    CHECK((*remote)->failed());
    CHECK((*remote)->failure().find("stopped responding") != std::string::npos);
    CHECK(std::all_of(audio.outLeft.begin(), audio.outLeft.end(), [](float v) { return v == 0; }));
}
TEST_CASE("a bridged instance's own call deadline outlasts a stall the process-wide one fails") {
    // An instance's own deadline replaces the process-wide one. The bridge
    // stops for 1.5 s, three times the process-wide deadline here and far
    // inside the instance's own; the outcome decides.
    setBridgeExecutable(BRIDGE_PATH);
    const int previous = bridgeDeadlineMs();
    struct Restore {
        int ms;
        ~Restore() { setBridgeDeadlineMs(ms); }
    } restore{previous};
    for (const bool own : {false, true}) {
        CAPTURE(own);
        setBridgeDeadlineMs(previous);
        auto remote = Instance::create(
            nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash"), {}, true);
        REQUIRE(remote);
        Instance& c = **remote;
        REQUIRE(c.activate(48000, 1, 64));
        REQUIRE(c.startProcessing());
        if (own) c.setCallDeadlineMs(60'000);
        setBridgeDeadlineMs(500);
        Audio audio;
        audio.left.fill(0.25f);
        audio.right.fill(0.25f);
        const int pid = c.bridgePid();
        REQUIRE(pid > 0);
        std::mutex lock;
        std::condition_variable answered;
        bool done = false;
        REQUIRE(supple::test::pauseProcess(pid));
        // Resumed after 1.5 s, unless the call came back first: then the
        // bridge was killed, and its pid is not signalled again.
        std::thread resume([&] {
            std::unique_lock held(lock);
            if (!answered.wait_for(held, std::chrono::milliseconds(1500), [&] { return done; }))
                supple::test::resumeProcess(pid);
        });
        const bool processed = c.process(audio.block());
        {
            std::lock_guard held(lock);
            done = true;
        }
        answered.notify_all();
        resume.join();
        REQUIRE(processed);
        CHECK(c.failed() == !own);
        if (own) {
            CHECK(audio.outLeft[0] == doctest::Approx(0.25));
            c.stopProcessing();
            c.deactivate();
        }
    }
}
TEST_CASE("a bridged plug-in late for the callback's deadline is silent for that block only") {
    // ADR-0081: a block not answered by the live deadline is silence, the
    // request stays outstanding and is answered before the next one; the
    // process is not checked or killed by the audio thread.
    setBridgeExecutable(BRIDGE_PATH);
    auto remote =
        Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash"), {}, true);
    REQUIRE(remote);
    Instance& c = **remote;
    REQUIRE(c.activate(48000, 1, 64));
    REQUIRE(c.startProcessing());
    Audio audio;
    audio.left.fill(0.25f);
    audio.right.fill(0.25f);
    REQUIRE(c.process(audio.block()));
    REQUIRE(audio.outLeft[0] == doctest::Approx(0.25));
    const auto before = bridgeLateCalls();
    REQUIRE(c.setParam(1, 3));  // 20 ms over the next block
    {
        LiveBridgeDeadline deadline(steadyNanoseconds() + 1'000'000);
        REQUIRE(c.process(audio.block()));
    }
    CHECK(std::all_of(audio.outLeft.begin(), audio.outLeft.end(), [](float v) { return v == 0; }));
    CHECK(bridgeLateCalls() == before + 1);
    CHECK_FALSE(c.failed());
    REQUIRE(c.setParam(1, 0));  // passes again from the next block
    {
        // Waits for the late block's answer, then answers this one.
        LiveBridgeDeadline deadline(steadyNanoseconds() + 5'000'000'000);
        REQUIRE(c.process(audio.block()));
    }
    CHECK(audio.outLeft[0] == doctest::Approx(0.25));
    CHECK(bridgeLateCalls() == before + 1);
    CHECK_FALSE(c.failed());
    c.stopProcessing();
    c.deactivate();
}
TEST_CASE("a hung bridged plug-in fails after 50 ms of late blocks; main kills its process") {
    setBridgeExecutable(BRIDGE_PATH);
    auto remote =
        Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash"), {}, true);
    REQUIRE(remote);
    Instance& c = **remote;
    REQUIRE(c.activate(48000, 1, 64));
    REQUIRE(c.startProcessing());
    REQUIRE(c.setParam(1, 2));  // hangs in the next block
    Audio audio;
    const auto before = bridgeLateCalls();
    int blocks = 0;
    // Deadlines already past: no block waits. The instance fails once its
    // late blocks span 50 ms; the guard only keeps a broken build from looping.
    const auto guard = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool silent = true;
    while (!c.failed() && std::chrono::steady_clock::now() < guard) {
        LiveBridgeDeadline deadline(steadyNanoseconds());
        REQUIRE(c.process(audio.block()));
        ++blocks;
        silent = silent && std::all_of(audio.outLeft.begin(), audio.outLeft.end(),
                                       [](float v) { return v == 0; });
    }
    CHECK(silent);
    // It failed through late blocks, none of which waited for the hang.
    CHECK(blocks > 1);
    CHECK(bridgeLateCalls() - before == static_cast<std::uint64_t>(blocks));
    REQUIRE(c.failed());
    CHECK(c.failure().find("stopped responding") != std::string::npos);
#if !defined(_WIN32)
    const int pid = c.bridgePid();
    REQUIRE(pid > 0);
    // Exited (a zombie until reaped counts), observed without reaping it.
    auto exited = [pid] {
        siginfo_t info{};
        return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
               info.si_pid == pid;
    };
    CHECK_FALSE(exited());  // the audio thread killed nothing
    while (!exited() && std::chrono::steady_clock::now() < guard) {
        c.serviceMainThread();  // main reaps the failure, killing the bridge
        std::this_thread::yield();
    }
    CHECK(exited());
#endif
}
TEST_CASE("native bridge notifications ACK and rearm without periodic parent polling") {
    setBridgeExecutable(BRIDGE_PATH);
    MainThreadWake wake;
    auto remote =
        Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge"), {}, true);
    REQUIRE(remote);
    (*remote)->setMainThreadWake(wake);
    awaitWake(wake);
    (*remote)->serviceMainThread();
    (void)(*remote)->takeNotifications();
    REQUIRE((*remote)->activate(48000, 1, 64));
    REQUIRE((*remote)->startProcessing());
    Audio audio;
    for (int round = 0; round < 3; ++round) {
        REQUIRE((*remote)->loadState(std::array<std::uint8_t, 4>{44, 0, 0, 0}));
        REQUIRE((*remote)->process(audio.block()));
        bool dirty = false;
        while (!dirty) {
            awaitWake(wake);
            (*remote)->serviceMainThread();
            dirty = (*remote)->takeNotifications().stateDirty;
        }
        CHECK_FALSE((*remote)->failed());
    }
    (*remote)->stopProcessing();
    (*remote)->deactivate();
}

TEST_CASE("removing one plug-in from a stalled shared bridge leaves the others alive") {
    // Review round 30 (F3): Destroy waited 2 s for its reply, then killed the
    // bridge and every plug-in in it. The bridge stops for 3 s while one of
    // its two instances is destroyed; the other must still play.
    setBridgeExecutable(BRIDGE_PATH);
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto info = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash");
    auto x = Instance::create(info, {}, true, *process);
    auto y = Instance::create(info, {}, true, *process);
    REQUIRE(x);
    REQUIRE(y);
    Instance& kept = **y;
    const int pid = kept.bridgePid();
    REQUIRE(pid > 0);
    REQUIRE(supple::test::pauseProcess(pid));
    std::thread resume([&kept, pid] {
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        if (!kept.failed()) supple::test::resumeProcess(pid);  // a killed one stays killed
    });
    x->reset();  // Destroy, answered once the bridge goes on
    resume.join();
    CHECK_FALSE(kept.failed());
    REQUIRE(kept.activate(48000, 1, 64));
    REQUIRE(kept.startProcessing());
    Audio audio;
    audio.left.fill(0.25f);
    audio.right.fill(0.25f);
    REQUIRE(kept.process(audio.block()));
    CHECK(audio.outLeft[0] == doctest::Approx(0.25));
    kept.stopProcessing();
    kept.deactivate();
}
TEST_CASE("a publication from a bridged audio thread reaches its instance through the named ACK") {
    // Review round 26 (F1): every other test that sees a bridge-side
    // publication also holds a pull of the same instance's own, which carries
    // the data. Here X publishes from its audio thread (edge mode 44: process
    // -> request_callback -> on_main_thread marks the state dirty) after its
    // own pulls are consumed, so only an ACK that names X brings the change.
    // Passes by completing; with an ACK that names nobody no further wake
    // comes and awaitWake fails at its lost-event guard.
    setBridgeExecutable(BRIDGE_PATH);
    MainThreadWake wake;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto info = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge");
    auto x = Instance::create(info, {}, true, *process);
    auto y = Instance::create(info, {}, true, *process);
    REQUIRE(x);
    REQUIRE(y);
    (*x)->setMainThreadWake(wake);
    (*y)->setMainThreadWake(wake);
    REQUIRE((*x)->loadState(std::array<std::uint8_t, 4>{44, 0, 0, 0}));
    REQUIRE((*x)->activate(48000, 1, 64));
    REQUIRE((*x)->startProcessing());
    awaitWake(wake);
    for (Instance* i : {x->get(), y->get()}) {  // consumes the queued own-request pulls
        i->serviceMainThread();
        (void)i->takeNotifications();
    }
    Audio audio;
    REQUIRE((*x)->process(audio.block()));
    bool dirty = false;
    while (!dirty) {
        awaitWake(wake);
        (*y)->serviceMainThread();  // another member's consumer runs the pass first
        (void)(*y)->takeNotifications();
        (*x)->serviceMainThread();
        dirty = (*x)->takeNotifications().stateDirty;
    }
    CHECK_FALSE((*x)->failed());
    (*x)->stopProcessing();
    (*x)->deactivate();
}

TEST_CASE("a destroyed bridged instance is not named by the next ACK") {
    // Review round 26: a destroyed slot stayed listed until the next ACK, so
    // instances created, changed and destroyed between refreshes grew the list.
    setBridgeExecutable(BRIDGE_PATH);
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto info = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge");
    auto x = Instance::create(info, {}, true, *process);
    auto y = Instance::create(info, {}, true, *process);
    REQUIRE(x);
    REQUIRE(y);
    for (auto* i : {x->get(), y->get()})  // each requests a flush: a publication
        REQUIRE(i->loadState(std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    x->reset();
    auto reply = (*process)->call(bridge::Op::AckNotifications, 0, bridge::Writer{});
    REQUIRE(reply);
    bridge::Reader r(reply->data(), reply->size());
    const auto count = r.u32();
    std::vector<std::uint32_t> named;
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) named.push_back(r.u32());
    REQUIRE(r.done());
    CHECK(named == std::vector<std::uint32_t>{2});  // y; x (id 1) was destroyed
}

TEST_CASE("native bridge malformed payloads and unexpected resources close cleanly") {
    setBridgeExecutable(BRIDGE_PATH);
    auto process = createBridgeProcess();
    REQUIRE(process);
    auto instance = Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.gain"),
                                     {}, true, *process);
    REQUIRE(instance);
    const auto memory = bridge::createMemory(sizeof(bridge::Header));
    REQUIRE(memory != bridge::noResource);
    auto malformed = (*process)->call(bridge::Op::Activate, 1, bridge::Writer{}, memory);
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == "bridge.malformed");
    auto unexpected = (*process)->call(bridge::Op::Params, 1, bridge::Writer{}, memory);
    bridge::closeResource(memory);
    REQUIRE_FALSE(unexpected);
    CHECK(unexpected.error().code == "bridge.fd");
    CHECK_FALSE((*process)->failed());
    REQUIRE((*instance)->paramValue(0));
}
TEST_CASE("native idle process death wakes every subscribed parent") {
    setBridgeExecutable(BRIDGE_PATH);
    MainThreadWake firstWake, secondWake;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto info = nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.gain");
    auto first = Instance::create(info, {}, true, *process);
    auto second = Instance::create(info, {}, true, *process);
    REQUIRE(first);
    REQUIRE(second);
    (*first)->setMainThreadWake(firstWake);
    (*second)->setMainThreadWake(secondWake);
    awaitWake(firstWake);
    awaitWake(secondWake);
    (*first)->serviceMainThread();
    (*second)->serviceMainThread();
    (*process)->killBridge();
    awaitWake(firstWake);
    awaitWake(secondWake);
    CHECK((*first)->failed());
    CHECK((*second)->failed());
}
#if defined(_WIN32)
TEST_CASE("Windows bridge reads enforce deadlines even with buffered input") {
    bridge::Resource pair[2];
    REQUIRE(bridge::controlPair(pair));
    bridge::NativeControl parent(reinterpret_cast<HANDLE>(pair[0]));
    bridge::NativeControl child(reinterpret_cast<HANDLE>(pair[1]), true);
    const std::array<char, 2> sent{'a', 'b'};
    REQUIRE(child.write(sent.data(), sent.size(), bridge::deadlineAfter(1000)));
    char received = 0;
    REQUIRE(parent.read(&received, 1, bridge::deadlineAfter(1000)));
    CHECK(received == 'a');
    // The second byte is already buffered: expiry must not depend on whether
    // the next native read would block. No elapsed-time assertion is needed.
    CHECK_FALSE(parent.read(&received, 1, bridge::Deadline::min()));
    CHECK(received == 'a');
    REQUIRE(parent.read(&received, 1, bridge::deadlineAfter(1000)));
    CHECK(received == 'b');
}

TEST_CASE("a numeric handle in an untrusted bridge reply never closes a parent handle") {
    bridge::Resource pair[2];
    REQUIRE(bridge::controlPair(pair));
    bridge::NativeControl parent(reinterpret_cast<HANDLE>(pair[0]));
    bridge::NativeControl child(reinterpret_cast<HANDLE>(pair[1]), true);
    WakeEvent retained;
    REQUIRE(retained.waitable().valid());
    const auto handle = reinterpret_cast<bridge::Resource>(retained.waitable().nativeHandle());
    REQUIRE(bridge::sendFrame(&child, 0, 1, {}, handle));
    std::uint32_t status = 0, id = 0;
    std::vector<std::uint8_t> bytes;
    bridge::Resource received = bridge::noResource;
    CHECK_FALSE(bridge::recvFrame(&parent, status, id, bytes, &received, 1000));
    CHECK(parent.rejectedResource());
    retained.notify();
    CHECK(retained.drain());
}
#endif

TEST_CASE("native missing bridge executable preserves the structured host error") {
    const auto missing =
        std::filesystem::path(BRIDGE_PATH).parent_path() / "missing-bridge-test-executable";
    setBridgeExecutable(missing);
    auto remote =
        Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.gain"), {}, true);
    setBridgeExecutable(BRIDGE_PATH);
    REQUIRE_FALSE(remote);
    CHECK(remote.error().code == "host.bridge");
    CHECK_FALSE(remote.error().hint.empty());
}

TEST_CASE("abandoning a bridge returns a call waiting on it from another thread") {
    // ADR-0090 (review round 34): a stopped offline render kills its own
    // bridges from a watch thread. The gain holds the call at a gate; the
    // instance's deadline is 60 s, so only the abandonment can end the wait
    // within the test, and the failure says which did.
    setBridgeExecutable(BRIDGE_PATH);
    using supple::test::GatedCall;
    for (const GatedCall call : {GatedCall::Activate, GatedCall::Process}) {
        const bool activating = call == GatedCall::Activate;
        CAPTURE(activating);
        std::optional<supple::test::LifecycleGate> gate(std::in_place, call);
        REQUIRE(gate->mapped());
        auto remote = Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.gain"),
                                       {}, true);
        REQUIRE(remote);
        gate->hide();
        Instance& gain = **remote;
        gain.setCallDeadlineMs(60'000);
        std::thread watch([&] {
            if (gate->entered()) gain.abandonBridge();
        });
        Audio audio;
        audio.left.fill(0.25f);
        audio.right.fill(0.25f);
        if (activating) {
            // A control request: its socket closes.
            CHECK_FALSE(gain.activate(48000, 1, 64));
        } else {
            // A data-plane wait: it sees the bridge failed at its next step.
            REQUIRE(gain.activate(48000, 1, 64));
            REQUIRE(gain.startProcessing());
            CHECK(gain.process(audio.block()));
            CHECK(std::all_of(audio.outLeft.begin(), audio.outLeft.end(),
                              [](float v) { return v == 0; }));
        }
        watch.join();
        REQUIRE(gate->entered());
        CHECK_FALSE(gate->passed());
        CHECK(gain.failed());
        CHECK(gain.failure().find("the host abandoned the plugin's bridge") != std::string::npos);
        CHECK(gain.callDeadlineMs() == 60'000);
        gate.reset();
    }
}

TEST_CASE(
    "a Create reply carries the instance's place in the bridge's table, and no less or more") {
    // Protocol 26 on POSIX, 27 on Windows: the reply gained its tail with that number.
#if defined(_WIN32)
    CHECK(bridge::kVersion == 27);
#else
    CHECK(bridge::kVersion == 26);
#endif
    bridge::Writer w;
    w.u32(3);  // the instance id
    w.u32(2);
    w.u32(2);  // channels
    w.u8(1);   // accepts notes
    w.u8(0);   // semantic
    w.u32(0);  // key channels
    w.u8(0);   // editor
    w.u8(0);   // outputs notes
    w.u32(1);  // one auxiliary output
    w.str("Aux");
    w.u32(2);
    const auto bare = w.bytes.size();
    w.u32(7);  // cell
    w.u16(5);  // generation
    w.u64(0x7f0000001000ULL);
    const auto valid = [&](std::size_t size) {
        return bridge::validReply(bridge::Op::Create, bridge::Reader(w.bytes.data(), size));
    };
    CHECK(valid(w.bytes.size()));
    CHECK_FALSE(valid(bare));                // a reply of the protocol before: no place
    CHECK_FALSE(valid(w.bytes.size() - 1));  // cut short
    w.u8(0);
    CHECK_FALSE(valid(w.bytes.size()));  // something trailing
    bridge::Reader r(w.bytes.data(), w.bytes.size());
    r.skip(bare);
    CHECK(r.u32() == 7);
    CHECK(r.u16() == 5);
    CHECK(r.u64() == 0x7f0000001000ULL);
}
