// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/host/clap_host.h>
#if !defined(_WIN32)
#include <signal.h>
#endif
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>
#include "../src/bridge_client.h"
#include "../src/bridge_diagnostics.h"
#include "supple/diagctx/inventory.h"
#include "supple/diagctx/scope.h"
#include "supple/diagctx/table.h"
using namespace supple;
using namespace supple::host;
namespace {
std::mutex gEventsMutex;
std::vector<diagctx::Event> gEvents;
void collect(const diagctx::Event& event) noexcept {
    std::lock_guard lock(gEventsMutex);
    gEvents.push_back(event);
}
// Collects what the host announces for the length of a test.
struct Sink {
    Sink() {
        {
            std::lock_guard lock(gEventsMutex);
            gEvents.clear();
        }
        // Not under the events lock: the sink runs under the announcement lock and then takes it.
        diagctx::setInventorySink(&collect);
    }
    ~Sink() { diagctx::setInventorySink(nullptr); }
    static std::vector<diagctx::Event> of(diagctx::EventKind kind) {
        std::lock_guard lock(gEventsMutex);
        std::vector<diagctx::Event> found;
        for (const auto& event : gEvents)
            if (event.event == kind) found.push_back(event);
        return found;
    }
};
// A guard against a lost event, not a timing assertion: the bridge is gone well before this.
void awaitEnded(const BridgeProcess& process) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (process.alive() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_FALSE(process.alive());
}
}  // namespace

TEST_CASE("a bridge's ordinals count from one and are never reused") {
    bridge::BridgeOrdinals ordinals;
    CHECK(ordinals.take() == 1);
    CHECK(ordinals.take() == 2);
    for (int i = 3; i <= 65535; ++i) (void)ordinals.take();
    CHECK(ordinals.take() == 0);  // the 65536th has none: a report reads 0 as "this process"
    CHECK(ordinals.take() == 0);
    CHECK(bridge::bridgeOrdinals().take() != bridge::bridgeOrdinals().take());
}

TEST_CASE("how a bridge ended is told from the host's own kill and from the status") {
    using diagctx::BridgeHow;
    // A kill by the host is the host's, whatever the status says.
    CHECK(bridge::classifyBridgeEnd(9, true).how == BridgeHow::Host);
    CHECK(bridge::classifyBridgeEnd(0, true).how == BridgeHow::Host);
    CHECK(bridge::classifyBridgeEnd(9, true).status == 9);
#if defined(_WIN32)
    const auto fault = bridge::classifyBridgeEnd(static_cast<int>(0xC0000005U), false);
    CHECK(fault.how == BridgeHow::Exception);
    CHECK(fault.status == static_cast<std::int32_t>(0xC0000005U));
    CHECK(bridge::classifyBridgeEnd(0, false).how == BridgeHow::Exit);
    CHECK(bridge::classifyBridgeEnd(3, false).how == BridgeHow::Exit);
    CHECK(bridge::classifyBridgeEnd(static_cast<int>(0x40000015U), false).how == BridgeHow::Exit);
#else
    CHECK(bridge::classifyBridgeEnd(11, false).how == BridgeHow::Signal);
    CHECK(bridge::classifyBridgeEnd(11, false).status == 11);
    CHECK(bridge::classifyBridgeEnd(9, false).how == BridgeHow::Signal);
    CHECK(bridge::classifyBridgeEnd(0, false).how ==
          BridgeHow::Exit);  // exitStatus() is 0 for an exit
#endif
}

TEST_CASE("a bridge the host kills is announced as the host's end, once, when it is reaped") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto ordinal = (*process)->ordinal();
    REQUIRE(ordinal >= 1);
    auto bridges = Sink::of(diagctx::EventKind::Bridge);
    REQUIRE(bridges.size() == 1);  // the start
    CHECK(bridges[0].bridge == ordinal);
    CHECK(bridges[0].how == diagctx::BridgeHow::None);
    (*process)->killBridge();
    awaitEnded(**process);
    CHECK(Sink::of(diagctx::EventKind::Bridge).size() == 1);  // nothing announced from a kill
    (*process)->reap();
    (*process)->reap();  // reaped once
    bridges = Sink::of(diagctx::EventKind::Bridge);
    REQUIRE(bridges.size() == 2);
    CHECK(bridges[1].bridge == ordinal);
    CHECK(bridges[1].how == diagctx::BridgeHow::Host);
    process->reset();  // the destructor does not say it again
    CHECK(Sink::of(diagctx::EventKind::Bridge).size() == 2);
}

namespace {
PluginInfo fixture(std::string_view id) {
    ScanOptions options;
    options.forceInProcess = true;
    const std::filesystem::path module = FIXTURE_CLAP_PATH;
    auto list = scan(std::span(&module, 1), options);
    REQUIRE_MESSAGE(list, (list ? "" : list.error().toString()));
    for (const auto& info : *list)
        if (info.id == id) return info;
    FAIL("fixture plug-in not found");
    return {};
}
}  // namespace

// The bridge answers Create with the instance's place in its own execution table, and the host
// announces it, so that the supervisor can match what the bridge's table says to the instance the
// report lists (topic 6b). What the child's numbers are can only be known from the child; what the
// host can check is that they are in range, that they differ per instance, and that they
// reach the announcement under the host's own cell.
TEST_CASE("a bridged instance announces where it is in its bridge, an in-process one does not") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto info = fixture("com.suppleaudio.test.edge");
    auto first = Instance::create(info, {}, true, *process);
    REQUIRE(first);
    auto second = Instance::create(info, {}, true, *process);  // the same bridge
    REQUIRE(second);
    auto third = Instance::create(info, {}, true);  // a bridge of its own
    REQUIRE(third);
    auto local = Instance::create(info, {}, false);
    REQUIRE(local);
    const auto adds = Sink::of(diagctx::EventKind::Add);
    const auto children = Sink::of(diagctx::EventKind::Child);
    REQUIRE(children.size() == 3);  // not for the in-process instance
    for (const auto& child : children) {
        CHECK(child.childCell < diagctx::kMaxInstances);
        CHECK(child.childGeneration <= diagctx::kGenerationMask);
        CHECK(child.image != 0);  // an address in the plug-in's binary, in the bridge
        // It belongs to an instance the host announced as bridged, under that cell and generation.
        bool found = false;
        for (const auto& add : adds)
            if (add.cell == child.cell && add.generation == child.generation &&
                add.mode == diagctx::InstanceMode::Bridged && add.bridge != 0)
                found = true;
        CHECK(found);
    }
    // Two instances of one bridge are two cells of its table.
    CHECK(children[0].childCell != children[1].childCell);
    CHECK(children[0].cell != children[1].cell);
    local->reset();
    third->reset();
    second->reset();
    first->reset();
}

// A bridge whose plug-in faults only once the host lets go of it is the bridge's own end, not the
// host's (the destructor reads the status after it waits; round 6a, F3). The host fails the bridge
// without killing it, so the instance is never destroyed by a request: the bridge destroys it when
// the socket closes, and the fixture faults in its destructor. No timing decides this: the bridge
// is alive when the host lets go, and what it did next is what the end reads as.
// What a bridge ended by abort() reads as. POSIX ends it with SIGABRT, a signal. On Windows the C
// runtime's abort() either fast-fails (an exception status, which is reported) or ends the process
// with exit code 3 (an exit, which is not): it depends on the runtime's abort behaviour, and the
// spec says "may" until this case has run there. On Windows it expects the fast-fail (an exception)
// and, if the runtime gives an exit instead, fails naming the status it got, so a native run
// settles it (the spec, STATUS and this expectation then follow); on POSIX it requires the signal.
TEST_CASE("a bridge ended by abort() is told as the system ended it, and the result is recorded") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto ordinal = (*process)->ordinal();
    auto made = Instance::create(fixture("com.suppleaudio.test.crash"), {}, true, *process);
    REQUIRE(made);
    Instance& instance = **made;
    REQUIRE(instance.activate(48000.0, 1, 64));
    REQUIRE(instance.startProcessing());
    REQUIRE(instance.setParam(1, 6.0));  // abort() in the next process()
    std::array<float, 64> in{}, out{};
    const float* inputs[1]{in.data()};
    float* outputs[1]{out.data()};
    ProcessBlock block;
    block.inputs = inputs;
    block.outputs = outputs;
    block.inputChannels = block.outputChannels = 1;
    block.frames = 64;
    (void)instance.process(block);
    awaitEnded(**process);
    (*process)->reap();
    diagctx::Event end;
    for (const auto& event : Sink::of(diagctx::EventKind::Bridge))
        if (event.bridge == ordinal && event.how != diagctx::BridgeHow::None) end = event;
    std::fprintf(stderr, "abort() ended a bridge as how=%s status=%d (0x%x)\n",
                 diagctx::bridgeHowName(end.how) ? diagctx::bridgeHowName(end.how) : "none",
                 end.status, static_cast<unsigned>(end.status));
#if defined(_WIN32)
    CHECK_MESSAGE(
        end.how == diagctx::BridgeHow::Exception,
        "abort() ended the bridge as "
            << (diagctx::bridgeHowName(end.how) ? diagctx::bridgeHowName(end.how) : "none")
            << ", status " << end.status);
#else
    CHECK(end.how == diagctx::BridgeHow::Signal);
    CHECK(end.status == SIGABRT);
#endif
    CHECK(end.how != diagctx::BridgeHow::Host);
    made->reset();
}

// AddressSanitizer and ThreadSanitizer end a faulting bridge with their own exit (after their
// report), not with the signal: the host rightly reads that as an exit, so there is no signal for
// this case to observe in those builds.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SUPPLE_TEST_SANITIZER_EXITS 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SUPPLE_TEST_SANITIZER_EXITS 1
#endif
#if !defined(SUPPLE_TEST_SANITIZER_EXITS)
TEST_CASE("a bridge that faults as the host lets go is its own end, not the host's") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto ordinal = (*process)->ordinal();
    {
        auto made = Instance::create(fixture("com.suppleaudio.test.crash"), {}, true, *process);
        REQUIRE(made);
        Instance& instance = **made;
        REQUIRE(instance.activate(48000.0, 1, 64));
        REQUIRE(instance.startProcessing());
        REQUIRE(instance.setParam(1, 5.0));  // faults at its destruction, processes until then
        std::array<float, 64> in{}, out{};
        const float* inputs[1]{in.data()};
        float* outputs[1]{out.data()};
        ProcessBlock block;
        block.inputs = inputs;
        block.outputs = outputs;
        block.inputChannels = block.outputChannels = 1;
        block.frames = 64;
        REQUIRE(instance.process(block));
        REQUIRE((*process)->alive());
        (*process)->fail(0);  // no more requests: Destroy is not sent
    }
    process->reset();  // the host lets go; the bridge destroys the instance it still holds
    diagctx::Event end;
    for (const auto& event : Sink::of(diagctx::EventKind::Bridge))
        if (event.bridge == ordinal && event.how != diagctx::BridgeHow::None) end = event;
    CHECK(end.how != diagctx::BridgeHow::Host);
    CHECK((end.how == diagctx::BridgeHow::Signal || end.how == diagctx::BridgeHow::Exception));
    CHECK(end.status != 0);
}
#endif

// The two cases below end a bridge with SIGKILL from outside. The Windows counterpart would end it
// with TerminateProcess(OpenProcess(pid), 0xC0000005); it is not written because nothing here can
// compile or run it, and an untried Windows branch has already broken this build once. Windows is
// covered by the classification case above (exception and exit statuses) and by the host-kill and
// orderly-shutdown cases; a bridge ended by an exception is proven there by native CI only, and
// STATUS.md says so.
#if !defined(_WIN32)
TEST_CASE("a bridge that dies of a signal is announced as that signal, with its instances") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto ordinal = (*process)->ordinal();
    const auto info = fixture("com.suppleaudio.test.edge");
    // One instance made in a bridge the caller made, one that made its own: each says where it is.
    auto first = Instance::create(info, {}, true, *process);
    REQUIRE(first);
    auto second = Instance::create(info, {}, true);
    REQUIRE(second);
    std::vector<diagctx::Event> adds;
    for (const auto& event : Sink::of(diagctx::EventKind::Add))
        if (event.mode == diagctx::InstanceMode::Bridged) adds.push_back(event);
    REQUIRE(adds.size() == 2);
    CHECK(adds[0].bridge == ordinal);
    CHECK(adds[1].bridge >= 1);
    CHECK(adds[1].bridge != ordinal);
    CHECK(adds[0].state == diagctx::InstanceState::Loading);  // before the plug-in's code ran
    // Not the host's kill: the operating system's.
    REQUIRE(::kill((*process)->pid(), SIGKILL) == 0);
    awaitEnded(**process);
    (*process)->reap();
    const auto bridges = Sink::of(diagctx::EventKind::Bridge);
    diagctx::Event end;
    for (const auto& event : bridges)
        if (event.bridge == ordinal && event.how != diagctx::BridgeHow::None) end = event;
    CHECK(end.how == diagctx::BridgeHow::Signal);
    CHECK(end.status == SIGKILL);
    first->reset();
    second->reset();
}

TEST_CASE("a bridge that died and was never reaped is still told when it is destroyed") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto ordinal = (*process)->ordinal();
    REQUIRE(::kill((*process)->pid(), SIGKILL) == 0);
    awaitEnded(**process);
    process->reset();
    const auto bridges = Sink::of(diagctx::EventKind::Bridge);
    REQUIRE(bridges.size() == 2);
    CHECK(bridges[1].bridge == ordinal);
    CHECK(bridges[1].how == diagctx::BridgeHow::Signal);
    CHECK(bridges[1].status == SIGKILL);
}
#endif

TEST_CASE("a bridge that is shut down in order is the host's end, not a crash") {
    setBridgeExecutable(BRIDGE_PATH);
    Sink sink;
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto ordinal = (*process)->ordinal();
    process->reset();  // the host lets go of a bridge that is still running
    const auto bridges = Sink::of(diagctx::EventKind::Bridge);
    REQUIRE(bridges.size() == 2);
    CHECK(bridges[1].bridge == ordinal);
    CHECK(bridges[1].how == diagctx::BridgeHow::Host);
}
