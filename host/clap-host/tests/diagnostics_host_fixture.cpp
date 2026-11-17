// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A program that runs the real host under the diagnostics frontend, for host_inventory.py: it
// loads the fixture plug-ins through host::Instance, as the engine does, then faults. The
// report must list what the host announced, not what a test announced by hand.
//   argv: <diagnostics directory> loaded
//         <diagnostics directory> bridged-crash   a bridged plug-in kills its bridge, five times,
//                                                 then the program faults (topic 6a)
//         <diagnostics directory> bridged-hang    the host kills a hung bridge, then the
//                                                 program faults
#include <supple/diagnostics/frontend.h>
#include <supple/host/clap_host.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
[[gnu::noinline]] void fault() { *reinterpret_cast<volatile int*>(8) = 1; }

// A bridged crash plug-in is made to misbehave in its next process(), and the host then makes a
// main-thread request, which is where it observes the bridge's end.
bool misbehave(const supple::host::PluginInfo& info, double mode) {
    auto made = supple::host::Instance::create(info, {}, true);
    if (!made) return false;
    supple::host::Instance& instance = **made;
    if (!instance.activate(48000.0, 1, 64) || !instance.startProcessing()) return false;
    if (!instance.setParam(1, mode)) return false;
    std::array<float, 64> in{}, out{};
    const float* inputs[1]{in.data()};
    float* outputs[1]{out.data()};
    supple::host::ProcessBlock block;
    block.inputs = inputs;
    block.outputs = outputs;
    block.inputChannels = block.outputChannels = 1;
    block.frames = 64;
    (void)instance.process(block);
    (void)instance.paramValue(1);  // fails: the bridge is gone, and this is where it is reaped
    return instance.failed();
}
// Waits until the supervisor has written a report, so that what follows cannot overtake it. The
// bound is the clock's: a count of 1 ms sleeps lasts fifteen times as long on Windows, whose sleeps
// round up to the timer tick, and a wait for a report that never comes must fail in 30 s.
bool awaitReport(const std::filesystem::path& directory) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < until) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            if (entry.path().extension() == ".json") return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int body(int argc, char** argv) {
    const std::filesystem::path path = FIXTURE_CLAP_PATH;
    auto found = supple::host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!found) return 2;
    const auto find = [&](const char* id) {
        for (const auto& info : *found)
            if (info.id == id) return info;
        return supple::host::PluginInfo{};
    };
    const std::string mode = argc >= 3 ? argv[2] : "loaded";
    if (mode == "bridged-crash" || mode == "bridged-hang") {
        supple::host::setBridgeExecutable(BRIDGE_PATH);
        const auto crash = find("com.suppleaudio.test.crash");
        if (mode == "bridged-hang") {
            supple::host::setBridgeDeadlineMs(200);
            if (!misbehave(crash, 2.0)) return 6;
        } else {
            // Mode 4 is a real crash on every platform (a signal, an access violation); mode 1
            // would end a Windows bridge with exit code 3, which is not a crash to the host.
            for (int i = 0; i < 5; ++i)
                if (!misbehave(crash, 4.0)) {
                    std::fprintf(stderr, "bridged-crash: death %d was not seen by the host\n",
                                 i + 1);
                    return 7;
                }
            // The first death has a report of its own; the other four repeat it.
            if (!awaitReport(argv[1])) {
                std::fprintf(stderr, "bridged-crash: no report of the first death in 30 s\n");
                return 8;
            }
        }
        fault();
        return 0;
    }
    // One active, one only created, one that was created and destroyed before the fault.
    auto gain = supple::host::Instance::create(find("com.suppleaudio.test.gain"), {});
    auto crash = supple::host::Instance::create(find("com.suppleaudio.test.crash"), {});
    if (!gain || !crash) return 3;
    if (!(*gain)->activate(48000.0, 64, 64)) return 4;
    {
        auto transient = supple::host::Instance::create(find("com.suppleaudio.test.gain"), {});
        if (!transient) return 5;
    }
    fault();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    return supple::diagnostics::runFrontend(argc, argv, supple::diagnostics::Role::Fixture, body,
                                            {std::filesystem::path(argv[1])});
}
