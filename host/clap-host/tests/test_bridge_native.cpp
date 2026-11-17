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
#include <ostream>
#include <thread>
#include <vector>
#include "../src/bridge_client.h"
#include "../src/bridge_protocol.h"
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
    auto remote =
        Instance::create(nativePlugin(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash"), {}, true);
    REQUIRE(remote);
    REQUIRE((*remote)->activate(48000, 1, 64));
    REQUIRE((*remote)->startProcessing());
    REQUIRE((*remote)->setParam(1, 2));
    Audio audio;
    REQUIRE((*remote)->process(audio.block()));
    CHECK((*remote)->failed());
    CHECK((*remote)->failure().find("stopped responding") != std::string::npos);
    CHECK(std::all_of(audio.outLeft.begin(), audio.outLeft.end(), [](float v) { return v == 0; }));
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
