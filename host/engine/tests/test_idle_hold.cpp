// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// ADR-0086: the CPU idle-latency request is held while a bridged plug-in
// plays live, without the preference, on Linux; never elsewhere.
#include <doctest/doctest.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "runtime_protection.h"
#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;

namespace {
#if defined(__linux__)
constexpr bool kHolds = true;
#else
constexpr bool kHolds = false;
#endif

// Counts the request's opens and closes; no device is touched.
struct FakeLatency {
    std::shared_ptr<RuntimeProtectionOps> ops = std::make_shared<RuntimeProtectionOps>();
    int opens = 0, closes = 0, openError = 0;
    FakeLatency() {
        ops->openLatency = [this](const char*, int) {
            ++opens;
            errno = openError;
            return openError ? -1 : 73;
        };
        ops->writeLatency = [](int, const void*, std::size_t size) {
            return static_cast<ProtectionWriteSize>(size);
        };
        ops->closeLatency = [this](int) {
            ++closes;
            return 0;
        };
        ops->thpDisable = [](unsigned long, unsigned long) { return 0; };
        ops->thpMode = [] { return 0; };
    }
};
}  // namespace

TEST_CASE("the idle-latency request is held while a bridged plug-in plays live") {
    FakeLatency fake;
    detail::RuntimeProtection controls(fake.ops);
    controls.start();
    // While stopped, nothing is held; the state applies at start.
    REQUIRE(controls.setBridgedLive(true, false));
    CHECK(fake.opens == 0);
    CHECK(controls.cpuIdleLatency().automatic == kHolds);
    controls.stop();
    CHECK_FALSE(controls.cpuIdleLatency().automatic);  // no engine, nothing live

    controls.start();
    REQUIRE(controls.setBridgedLive(true, true));
    CHECK(fake.opens == (kHolds ? 1 : 0));
    CHECK(controls.cpuIdleLatency().active == kHolds);
    CHECK(controls.cpuIdleLatency().automatic == kHolds);
    CHECK_FALSE(controls.cpuIdleLatency().requested);  // the preference stays off
    REQUIRE(controls.setBridgedLive(true, true));      // acts on a change only
    CHECK(fake.opens == (kHolds ? 1 : 0));
    REQUIRE(controls.setBridgedLive(false, true));
    CHECK(fake.closes == (kHolds ? 1 : 0));
    CHECK_FALSE(controls.cpuIdleLatency().active);
    CHECK_FALSE(controls.cpuIdleLatency().automatic);

    // The preference keeps meaning always: no live plug-in releases it.
    if (kHolds) {
        REQUIRE(controls.setPreventDeepIdle(true, true));
        CHECK(fake.opens == 2);
        REQUIRE(controls.setBridgedLive(true, true));
        REQUIRE(controls.setBridgedLive(false, true));
        CHECK(fake.closes == 1);
        CHECK(controls.cpuIdleLatency().active);
        // And turning it off while one plays live keeps the hold.
        REQUIRE(controls.setBridgedLive(true, true));
        REQUIRE(controls.setPreventDeepIdle(false, true));
        CHECK(controls.cpuIdleLatency().active);
        CHECK(fake.closes == 1);
        REQUIRE(controls.setBridgedLive(false, true));
        CHECK(fake.closes == 2);
    }
    controls.stop();
}

TEST_CASE("an automatic idle-latency hold that fails says why, and is no warning") {
    FakeLatency fake;
    fake.openError = EACCES;
    detail::RuntimeProtection controls(fake.ops);
    controls.start();
    const auto held = controls.setBridgedLive(true, true);
    if (!kHolds) {
        CHECK(held);
        CHECK(fake.opens == 0);
        CHECK_FALSE(controls.cpuIdleLatency().error);
        return;
    }
    REQUIRE_FALSE(held);
    REQUIRE(controls.cpuIdleLatency().error);
    CHECK(controls.cpuIdleLatency().error->errnoValue == EACCES);
    CHECK(controls.cpuIdleLatency().error->hint.find("plays live") != std::string::npos);
    CHECK(controls.cpuIdleLatency().error->hint.find("disable the option") == std::string::npos);
    CHECK_FALSE(controls.cpuIdleLatency().active);
    CHECK(fake.opens == 1);
    REQUIRE(controls.setBridgedLive(true, true));  // no retry every tick
    CHECK(fake.opens == 1);
    REQUIRE(controls.setBridgedLive(false, true));
    CHECK_FALSE(controls.cpuIdleLatency().error);  // nothing asked for: no error kept
    controls.stop();
}

TEST_CASE("EngineApp holds the idle-latency request while a bridged effect plays live") {
    host::setBridgeExecutable(BRIDGE_PATH);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-idle-hold-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    FakeLatency fake;
    {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        options.runtimeProtectionOps = fake.ops;
        auto made = EngineApp::create(options, {.console9 = false});
        REQUIRE(made);
        auto& app = **made;
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, app);
        const cmd::Origin origin{cmd::Origin::Type::Test, "idle-hold", ""};
        auto run = [&](std::string name, std::string_view text) {
            auto result = bus.execute({std::move(name), *json::parse(text), origin});
            REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        };
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        // The in-callback executor processes every node in the callback.
        REQUIRE(app.startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        run("track.add", R"({"name":"Bridged"})");
        run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
        app.tick();
        CHECK_FALSE(app.engineStatus().cpuIdleLatency.automatic);  // in process
        CHECK(fake.opens == 0);
        run("fx.set", R"({"fx":"fx_1","isolation":"shared_bridge"})");
        app.tick();
        CHECK(app.engineStatus().cpuIdleLatency.automatic == kHolds);
        CHECK(app.engineStatus().cpuIdleLatency.active == kHolds);
        CHECK_FALSE(app.engineStatus().cpuIdleLatency.requested);
        CHECK(fake.opens == (kHolds ? 1 : 0));
        run("fx.set", R"({"fx":"fx_1","isolation":"in_process"})");
        app.tick();
        CHECK_FALSE(app.engineStatus().cpuIdleLatency.active);
        CHECK(fake.closes == (kHolds ? 1 : 0));

        // Refused: recorded, but audio plays and nothing warns.
        fake.openError = EACCES;
        run("fx.set", R"({"fx":"fx_1","isolation":"shared_bridge"})");
        app.tick();
        const EngineStatus status = app.engineStatus();
        CHECK(status.running);
        CHECK(bool(status.cpuIdleLatency.error) == kHolds);
        for (const auto& warning : status.warnings)
            CHECK(warning.find("cpu_dma_latency") == std::string::npos);
        app.stopEngine();
        CHECK_FALSE(app.engineStatus().cpuIdleLatency.automatic);
        CHECK_FALSE(app.engineStatus().cpuIdleLatency.error);
    }
    std::filesystem::remove_all(dir);
}
