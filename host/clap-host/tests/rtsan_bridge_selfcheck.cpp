// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Proves the rtsan preset checks the callback's bridged path (ADR-0081): a
// SUPPLE_RT function makes a bridged process() call under a live deadline,
// which must pass (its waits are the sanctioned ones), then allocates, which
// RealtimeSanitizer must catch: the sanctioned scope ends with the bridged
// call and hides nothing later in the cycle. Registered in the rtsan preset
// only; it passes when the bridged call's marker is printed and the program
// never reaches "rtsan did not fire".
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "supple/base/rt.h"
#include "supple/host/clap_host.h"

using namespace supple;
using namespace supple::host;

namespace {
volatile int sink;

struct Cycle {
    Instance* instance;
    ProcessBlock* block;
};

bool bridgedCall(Cycle& c) SUPPLE_RT {
    const LiveBridgeDeadline deadline(steadyNanoseconds() + 5'000'000'000);
    return c.instance->process(*c.block);
}

int allocateAfterBridgedCall() SUPPLE_RT {
    auto p = std::make_unique<int[]>(64);  // must trip rtsan
    p[3] = 7;
    return p[3];
}
}  // namespace

int main() {
    setBridgeExecutable(BRIDGE_PATH);
    const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    auto found = scan(std::span<const std::filesystem::path>(&fixture, 1));
    if (!found) {
        std::fprintf(stderr, "%s\n", found.error().toString().c_str());
        return 1;
    }
    const PluginInfo* gain = nullptr;
    for (const auto& p : *found)
        if (p.id == "com.suppleaudio.test.gain") gain = &p;
    if (!gain) {
        std::fprintf(stderr, "the gain fixture is missing\n");
        return 1;
    }
    auto made = Instance::create(*gain, {}, true, nullptr);
    if (!made || !(*made)->activate(48000, 1, 64) || !(*made)->startProcessing()) {
        std::fprintf(stderr, "cannot start the bridged gain fixture\n");
        return 1;
    }
    std::vector<float> l(64, 0.25f), r(64, 0.25f), ol(64), orr(64);
    float* in[2] = {l.data(), r.data()};
    float* out[2] = {ol.data(), orr.data()};
    ProcessBlock b;
    b.inputs = in;
    b.inputChannels = 2;
    b.outputs = out;
    b.outputChannels = 2;
    b.frames = 64;
    Cycle cycle{made->get(), &b};
    const auto late = bridgeLateCalls();
    if (!bridgedCall(cycle) || bridgeLateCalls() != late || ol[63] == 0.0f) {
        std::fprintf(stderr, "the bridged call did not answer in time\n");
        return 1;
    }
    std::fprintf(stderr, "bridged call under a live deadline: clean\n");
    std::fflush(stderr);
    sink = allocateAfterBridgedCall();
    std::puts("rtsan did not fire");
    std::fflush(stdout);
    return 0;
}
