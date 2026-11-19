// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the execution scopes cost on the audio path (ADR-0083, topic 5b and 5e). Informational:
// it prints numbers and never fails on them (AGENTS.md: no wall-clock decision).
//
//   1. the scope itself: an AudioScope round trip around a call the compiler cannot remove,
//      against the bare call, on one cell and on four cells that share nothing;
//   2. `host::Instance::process` of the fixture gain plug-in, as the engine calls it, per
//      block size. Each call includes the scope the host puts around the plug-in's process.
//
// Both executors (the in-callback one and the anticipative renderer) reach a plug-in only
// through `Instance::process`, so the scope's cost per block is the same in each: two stores
// per instance per block. The program reports the scope's cost as a share of one process call.
//   argv: [repetitions]   (each repetition is one timed batch; the minimum and median are shown)
#include <supple/diagctx/scope.h>
#include <supple/host/clap_host.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

[[gnu::noinline]] int work(int x) {
    asm volatile("" : "+r"(x));
    return x + 1;
}

template <class F>
void measure(const char* what, std::size_t repetitions, std::size_t calls, F&& body) {
    std::vector<double> perCall;
    for (std::size_t r = 0; r < repetitions; ++r) {
        const auto start = Clock::now();
        body(calls);
        perCall.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
                          static_cast<double>(calls));
    }
    std::sort(perCall.begin(), perCall.end());
    std::printf("%-46s min %9.2f ns  median %9.2f ns\n", what, perCall.front(),
                perCall[perCall.size() / 2]);
}
}  // namespace

int main(int argc, char** argv) {
    namespace dc = supple::diagctx;
    const std::size_t repetitions = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 15;
    std::printf("scope cost (isolated)\n");
    {
        int sink = 0;
        measure("bare call", repetitions, 4'000'000, [&](std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) sink = work(sink);
        });
        const std::uint32_t cell = dc::acquireCell();
        measure("AudioScope around the call, one cell", repetitions, 4'000'000, [&](std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                dc::AudioScope scope(cell, dc::Entry::Process);
                sink = work(sink);
            }
        });
        std::uint32_t cells[4];
        for (auto& c : cells) c = dc::acquireCell();
        measure("AudioScope around the call, four cells in turn", repetitions, 4'000'000,
                [&](std::size_t n) {
                    for (std::size_t i = 0; i < n; ++i) {
                        dc::AudioScope scope(cells[i & 3], dc::Entry::Process);
                        sink = work(sink);
                    }
                });
        for (auto c : cells) dc::releaseCell(c);
        dc::releaseCell(cell);
        if (sink == 42) std::puts("");
    }

    std::printf("\nInstance::process of the fixture gain plug-in (scope included)\n");
    const std::filesystem::path path = FIXTURE_CLAP_PATH;
    auto found = supple::host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!found) return 2;
    supple::host::PluginInfo info;
    for (const auto& candidate : *found)
        if (candidate.id == "com.suppleaudio.test.gain") info = candidate;
    for (const std::uint32_t frames : {64U, 128U, 512U, 2048U}) {
        auto instance = supple::host::Instance::create(info, {});
        if (!instance || !(*instance)->activate(48000.0, 1, frames)) return 3;
        if (!(*instance)->startProcessing()) return 4;
        std::vector<float> in(frames, 0.25F), out(frames, 0.F);
        const float* inputs[1] = {in.data()};
        float* outputs[1] = {out.data()};
        supple::host::ProcessBlock block;
        block.inputs = inputs;
        block.inputChannels = 1;
        block.outputs = outputs;
        block.outputChannels = 1;
        block.frames = frames;
        char label[64];
        std::snprintf(label, sizeof label, "process, %u frames", frames);
        measure(label, repetitions, 20000, [&](std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) (void)(*instance)->process(block, nullptr);
        });
        (*instance)->stopProcessing();
    }
    return 0;
}
