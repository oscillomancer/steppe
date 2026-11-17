// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Offline renders and bridged plug-ins (ADR-0090): a stalled bridge is
// waited for, a plug-in that fails while the render runs fails it, and a
// stop or a cancel does not wait for a bridged call.
#include <doctest/doctest.h>
#include <supple/base/stop.h>
#include <supple/engine/offline.h>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../../clap-host/tests/lifecycle_gate.h"
#include "../../clap-host/tests/process_pause.h"
#include "bridge_patience.h"
#include "realtime_common.h"

namespace {
using supple::test::GatedCall;
using supple::test::killProcess;
using supple::test::LifecycleGate;
using supple::test::pauseProcess;
using supple::test::resumeProcess;

// The sine and gain fixtures on one track, both in `isolation`.
model::Session isolated(model::Isolation isolation) {
    const auto base = synthSession(-6.0, "com.suppleaudio.test.gain");
    model::EditTxn t(base, 2);
    for (const auto& entry : base.fx)
        t.modify<model::FxInstance>(entry.id, [&](auto& f) { f.isolation = isolation; });
    return t.commit().value();
}

Id gainOf(const model::Session& s) {
    for (const auto& entry : s.fx)
        if (entry.value->plugin.id == "com.suppleaudio.test.gain") return entry.id;
    return {};
}

// The render's bridge processes, as its first block reports progress.
std::vector<int> bridges(const InstancePool& pool) {
    std::vector<int> pids;
    for (host::Instance* in : pool.instances())
        if (in->bridgePid() > 0 &&
            std::find(pids.begin(), pids.end(), in->bridgePid()) == pids.end())
            pids.push_back(in->bridgePid());
    return pids;
}

OfflineSettings twoSeconds() { return {.startSeconds = 0, .endSeconds = 2.0}; }

// Why the host failed an instance whose bridge a stop or cancel killed.
constexpr const char* kAbandoned = "the host abandoned the plugin's bridge";

// Asks a render to stop: through its job's cancel flag, or its thread's stop.
struct Stopper {
    bool cancel;
    std::shared_ptr<std::atomic<bool>> flag = std::make_shared<std::atomic<bool>>(false);
    supple::JoiningThread source{[] {}};  // a stop source on every platform
    void arm(OfflineSettings& settings) {
        if (cancel)
            settings.cancelFlag = flag;
        else
            settings.stopToken = source.get_stop_token();
    }
    void operator()() {
        if (cancel)
            flag->store(true);
        else
            source.request_stop();
    }
};
}  // namespace

TEST_CASE("an offline render waits for a bridge stalled past the live path's deadline") {
    host::setBridgeExecutable(BRIDGE_PATH);
    // The product's deadline off the callback, which the live and
    // anticipative paths keep: an offline render does not use it.
    const engine::test::BridgeDeadline productDefault(2000);
    auto referencePool = makePool();
    const auto reference =
        renderOffline(isolated(model::Isolation::InProcess), referencePool, twoSeconds());
    REQUIRE(reference);
    for (auto isolation : {model::Isolation::SharedBridge, model::Isolation::Dedicated}) {
        CAPTURE(isolation);
        auto pool = makePool();
        std::atomic<bool> stalled{false};
        std::thread resume;
        std::vector<int> paused;
        OfflineSettings settings = twoSeconds();
        // From the first block, every bridge stops for longer than the live
        // path's deadline, and then goes on.
        settings.progress = [&](double) {
            if (stalled.exchange(true)) return;
            paused = bridges(pool);
            for (int pid : paused) CHECK(pauseProcess(pid));
            resume = std::thread([&paused] {
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                for (int pid : paused) resumeProcess(pid);
            });
        };
        auto rendered = renderOffline(isolated(isolation), pool, settings);
        if (resume.joinable()) resume.join();
        REQUIRE(stalled.load());
        CHECK_FALSE(paused.empty());
        REQUIRE_MESSAGE(rendered, (rendered ? "" : rendered.error().toString()));
        CHECK(pool.failedErrors().empty());
        const bool same = rendered->audio.channels == reference->audio.channels;
        CHECK(same);
    }
}

TEST_CASE("a bridged plug-in that fails during an offline render fails the render, named") {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (const bool hangs : {false, true}) {
        CAPTURE(hangs);
        const auto session = isolated(model::Isolation::Dedicated);
        auto pool = makePool();
        std::atomic<bool> struck{false};
        OfflineSettings settings = twoSeconds();
        // A short deadline, for the test only: a hung plug-in is still
        // killed by the render's own, however long.
        if (hangs) settings.bridgeDeadlineMs = 300;
        const Id gain = gainOf(session);
        settings.progress = [&](double) {
            if (struck.exchange(true)) return;
            if (host::Instance* in = pool.find(gain))
                CHECK((hangs ? pauseProcess : killProcess)(in->bridgePid()));
        };
        auto rendered = renderOffline(session, pool, settings);
        REQUIRE(struck.load());
        REQUIRE_FALSE(rendered);
        const Error& error = rendered.error();
        CHECK(error.code == "engine.fx_failed");
        CHECK(error.path == "/fx/" + gain.str());
        CHECK(error.message.find("Test Gain") != std::string::npos);
        CHECK(error.message.find(gain.str()) != std::string::npos);
        CHECK_FALSE(error.hint.empty());
        if (hangs) CHECK(error.message.find("stopped responding") != std::string::npos);

        // Rendered again with the same instances, the plug-in had failed
        // before this render started: that is as before, a refusal to start
        // (the product's renders make their own instances, so a plug-in that
        // failed in the session renders afresh).
        auto again = renderOffline(session, pool, twoSeconds());
        REQUIRE_FALSE(again);
        CHECK(again.error().code == "engine.start_failed");
    }
}

TEST_CASE("an offline render keeps its deadline while it runs and gives it back after") {
    // Review round 34 (F3): a pool used again after a render must not keep
    // the render's patience, or its live use would wait a minute.
    host::setBridgeExecutable(BRIDGE_PATH);
    const auto session = isolated(model::Isolation::Dedicated);
    auto pool = makePool();
    std::vector<int> during;
    OfflineSettings settings = twoSeconds();
    settings.progress = [&](double) {
        if (!during.empty()) return;
        for (host::Instance* in : pool.instances()) during.push_back(in->callDeadlineMs());
    };
    REQUIRE(renderOffline(session, pool, settings));
    REQUIRE(during.size() == 2);  // the sine and the gain, each in its bridge
    for (int ms : during) CHECK(ms == kOfflineBridgeDeadlineMs);
    for (host::Instance* in : pool.instances()) {
        REQUIRE(in->isolated());
        CHECK(in->callDeadlineMs() == 0);
    }
    // And after a render that failed: the gain's bridge killed mid-render.
    const Id gain = gainOf(session);
    auto again = makePool();
    OfflineSettings failing = twoSeconds();
    bool struck = false;
    failing.progress = [&](double) {
        if (std::exchange(struck, true)) return;
        CHECK(killProcess(again.find(gain)->bridgePid()));
    };
    auto rendered = renderOffline(session, again, failing);
    REQUIRE_FALSE(rendered);
    CHECK(rendered.error().code == "engine.fx_failed");
    for (host::Instance* in : again.instances()) CHECK(in->callDeadlineMs() == 0);
}

TEST_CASE("a stopped or cancelled offline render does not wait for a bridged call") {
    // Review round 34 (F1): the render's own bridges are killed, wherever its
    // thread waits for one, and the outcome is the cancellation. The gain
    // holds the named call at a gate until the test has stopped the render;
    // the render's deadline is the product's (60 s, lifecycle 152 s), so
    // only the stop can end the wait within the test, and the instance's
    // failure says which did.
    host::setBridgeExecutable(BRIDGE_PATH);
    const std::pair<GatedCall, const char*> calls[] = {
        {GatedCall::Activate, "activate"},  {GatedCall::Reset, "reset"},
        {GatedCall::Flush, "params flush"}, {GatedCall::StartProcessing, "start_processing"},
        {GatedCall::Process, "process"},    {GatedCall::StopProcessing, "stop_processing"},
    };
    for (const auto& [call, name] : calls)
        for (const bool cancel : {true, false}) {
            CAPTURE(name);
            CAPTURE(cancel);
            const auto session = isolated(model::Isolation::Dedicated);
            const Id gain = gainOf(session);
            auto pool = makePool();
            Stopper stop{cancel};
            OfflineSettings settings = twoSeconds();
            stop.arm(settings);
            // Named while the render spawns its bridges; nothing else makes
            // a gain meanwhile.
            std::optional<LifecycleGate> gate(std::in_place, call);
            REQUIRE(gate->mapped());
            std::thread stopper([&] {
                if (gate->entered()) stop();
            });
            auto rendered = renderOffline(session, pool, settings);
            stopper.join();
            REQUIRE(gate->entered());
            CHECK_FALSE(gate->passed());  // the call never went on: its bridge is gone
            REQUIRE_FALSE(rendered);
            CHECK(rendered.error().code == "job.cancelled");
            host::Instance* in = pool.find(gain);
            REQUIRE(in);
            CHECK(in->failed());
            CHECK(in->failure().find(kAbandoned) != std::string::npos);
            CHECK(in->callDeadlineMs() == 0);
            gate.reset();
        }
}

TEST_CASE("a render cancelled while its bridge is stopped does not wait out the deadline") {
    // The reviewer's probe (round 34): the bridge is stopped and the job
    // cancelled in the first block's progress. The loop sees the cancel and
    // stops processing, which waits on the stopped bridge until the watch
    // kills it. With the product's deadline (152 s for that call) the
    // failure names the abandonment, not a plug-in that stopped responding.
    host::setBridgeExecutable(BRIDGE_PATH);
    for (auto isolation : {model::Isolation::SharedBridge, model::Isolation::Dedicated}) {
        CAPTURE(isolation);
        const auto session = isolated(isolation);
        const Id gain = gainOf(session);
        auto pool = makePool();
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> struck{false};
        OfflineSettings settings = twoSeconds();
        settings.cancelFlag = cancel;
        settings.progress = [&](double) {
            if (struck.exchange(true)) return;
            if (host::Instance* in = pool.find(gain)) CHECK(pauseProcess(in->bridgePid()));
            cancel->store(true);
        };
        auto rendered = renderOffline(session, pool, settings);
        REQUIRE(struck.load());
        REQUIRE_FALSE(rendered);
        CHECK(rendered.error().code == "job.cancelled");
        host::Instance* in = pool.find(gain);
        REQUIRE(in);
        CHECK(in->failure().find(kAbandoned) != std::string::npos);
    }
}
