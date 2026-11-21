// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What test_clip_player.cpp takes from the engine's private state, through EngineTestAccess
// (engine_test_access.h). Its own file so that it is built on every platform, where
// test_realtime.cpp (Linux only) is not.
//
// The file has a test case of its own because engine.tests is SPLIT: each source file gets a
// ctest entry that runs the test cases of that file and fails when there are none.
#include <doctest/doctest.h>

#include "engine_test_access.h"
#include "realtime_common.h"

namespace supple::engine {
// Runs `hook` in launch() between publishing a launch and deciding it.
void launchCommitHookForTests(Engine& e, void (*hook)(void*), void* context) {
    EngineTestAccess::beforeLaunchCommit(e, hook, context);
}
// The slots the executors still act on (Engine::launchesQueued_).
std::uint32_t launchesQueuedForTests(const Engine& e) {
    return EngineTestAccess::launchesQueued(e);
}
}  // namespace supple::engine

TEST_CASE(
    "a fresh engine has no launches queued, and a commit hook set then cleared is not called") {
    using namespace supple::engine;
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, EngineOptions{});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    CHECK(launchesQueuedForTests(engine) == 0);
    static int called = 0;
    called = 0;
    launchCommitHookForTests(engine, [](void*) { ++called; }, nullptr);
    launchCommitHookForTests(engine, nullptr, nullptr);
    engine.service();
    CHECK(called == 0);
    CHECK(launchesQueuedForTests(engine) == 0);
    engine.stop();
}
