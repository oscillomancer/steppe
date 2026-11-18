// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// For tests only: points inside an offline render's watch (ADR-0090).
#pragma once

namespace supple::engine {

// Set while no render runs. `killed` runs on the watch's thread after it has
// killed the render's bridges and before it records that it did; `finishing`
// runs on the thread that ends the watch, before it waits for it. A test
// orders the two to show the render's outcome does not depend on which comes
// first.
struct RenderWatchTestHooks {
    void (*killed)(void*) = nullptr;
    void (*finishing)(void*) = nullptr;
    void* context = nullptr;
};
void setRenderWatchTestHooks(const RenderWatchTestHooks& hooks);

}  // namespace supple::engine
