// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What test_clip_player.cpp takes from the engine's private state, through EngineTestAccess
// (engine_test_access.h). Its own file so that it is built on every platform, where
// test_realtime.cpp (Linux only) is not.
#include "engine_test_access.h"

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
