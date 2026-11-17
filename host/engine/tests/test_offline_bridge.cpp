// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Offline renders and bridged plug-ins (ADR-0090): a stalled bridge is
// waited for, and a plug-in that fails while the render runs fails it.
#include <doctest/doctest.h>
#include <supple/engine/offline.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>
#include "../../clap-host/tests/process_pause.h"
#include "bridge_patience.h"
#include "realtime_common.h"

namespace {
using supple::test::killProcess;
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
