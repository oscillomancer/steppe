// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// A locate's restart (ADR-0062 §5, 2026-11-14 note), for tests: a module in a
// rack, Transport's beat phase in each of its inputs and its first output
// into Out, played, restarted as the engine restarts it on the audio thread
// (rack::Instance::restart()) and played again from the same position. It
// plays as it did when new, whatever its code: C++ or compiled script.
#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "supple/rack/host_modules.h"
#include "supple/rack/instance.h"
#include "supple/rack/module_type.h"
#include "supple/rack/schedule.h"

namespace supple::rack::test {

struct SongClock final : Clock {
    script::Transport transport(std::int64_t position) override {
        script::Transport x;
        x.playing = true;
        x.tempo = 133;
        x.beats = static_cast<double>(position) * 133.0 / 60.0 / 48000.0;
        return x;
    }
};

// False when the module has no output to compare (nothing is checked).
inline bool replaysAfterRestart(std::shared_ptr<const script::detail::ModuleCode> code,
                                Origin origin) {
    const auto type = ModuleType::fromCode(std::move(code), origin);
    REQUIRE(type);
    Instance::Setup setup;
    setup.seed = 77;
    auto transport = host::make(host::transportType(), setup);
    auto module = Instance::make(type, setup);
    auto out = host::make(host::outType(), setup);
    REQUIRE(module);
    int inputs = 0, outputs = 0;
    for (const auto& p : type->ports()) ++(p.output ? outputs : inputs);
    if (outputs == 0) return false;
    Patch p;
    p.modules = {
        {1, transport.get(), false, {}}, {2, module.get(), false, {}}, {3, out.get(), false, {}}};
    for (int k = 0; k < inputs; ++k) p.cables.push_back({0, 6, 1, false, k, 1.0});  // beat phase
    p.cables.push_back({1, 0, 2, false, 0, 1.0});
    std::string error;
    auto schedule = Schedule::build(p, &error);
    REQUIRE_MESSAGE(schedule, error);
    SongClock clock;
    auto render = [&] {
        std::vector<float> l(6000, 0.0f), r = l;
        auto* o = static_cast<host::Out*>(out->host());
        for (int at = 0; at < 6000; at += 256) {
            const int n = std::min(256, 6000 - at);
            o->bind(l.data() + at, r.data() + at);
            schedule->process(at, n, clock);
        }
        return l;
    };
    const auto fresh = render();
    for (Instance* in : {transport.get(), module.get(), out.get()}) in->restart();
    CHECK(render() == fresh);
    return true;
}

}  // namespace supple::rack::test
