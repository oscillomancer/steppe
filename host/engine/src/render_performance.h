// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The launcher performance a render plays (engine.md §3.1): the render
// commands' `performance` selector, resolved to a captured_performance and
// checked against the version rendered without compiling a plan, and the
// offline settings that play it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "launch_commit.h"
#include "supple/engine/offline.h"

namespace supple::engine {

class EngineApp;

// What `performance` names: "captured", a given captured_performance, or
// {from: "launcher", start_beat, end_beat}.
struct PerformanceRequest {
    enum class Kind : std::uint8_t { Captured, Given, Launcher };
    Kind kind = Kind::Captured;
    CapturedPerformance given;
    double startBeat = 0, endBeat = 0;  // Launcher
};
// Errors point under /performance.
Result<PerformanceRequest> decodePerformance(const json::Value& v);

// The version "captured" renders by default: the capture's own (the one it
// began on), or none when there is no capture placed.
std::optional<std::uint64_t> capturedVersion(EngineApp& app);

struct RenderPerformance {
    CapturedPerformance perf;  // canonical: what is rendered, and returned
    bool asHeard = false;
    std::vector<std::string> notHeardBecause;  // when not as heard, in the contract's order
    double startBeat = 0, endBeat = 0;         // its span on the version's time map
    std::optional<std::uint64_t> capturedOn;   // "captured": the version it began on
};
// Resolves the source and checks each launch against `s`, the version
// rendered at `rate`, the render's sample rate: no plan, no decoding.
// launch.memory is the one refusal left to the render (it needs the plan).
Result<RenderPerformance> resolvePerformance(EngineApp& app, const PerformanceRequest& request,
                                             const model::Session& s, double rate);

// The render's range, loop and launches (song frames, which the render
// converts to its stream frames under a master playrate).
void applyPerformance(const RenderPerformance& p, OfflineSettings& settings);

// The result's fields: performance, range, as_heard, not_heard_because.
void addPerformanceResult(json::Value& out, const RenderPerformance& p);

// render.not_as_heard for a performance that is not, worded from its first
// reason.
Error notAsHeard(const RenderPerformance& p);

}  // namespace supple::engine
