// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/host/clap_host.h>
#if !defined(_WIN32)
#include <signal.h>
#endif
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "../src/bridge_client.h"
#include "../src/bridge_diagnostics.h"
#include "supple/diagctx/inventory.h"
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

// The two cases below end a bridge with SIGKILL from outside. The Windows counterpart would end it
// with TerminateProcess(OpenProcess(pid), 0xC0000005); it is not written because nothing here can
// compile or run it, and an untried Windows branch has already broken this build once. Windows is
// covered by the classification case above (exception and exit statuses) and by the host-kill and
// orderly-shutdown cases; a bridge ended by an exception is proven there by native CI only, and
// STATUS.md says so.
#if !defined(_WIN32)
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
