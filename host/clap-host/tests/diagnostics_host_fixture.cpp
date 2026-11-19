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
//         <diagnostics directory> bridged-one <n> the crash plug-in is made to misbehave in mode
//                                                 n (fixture_plugins.cpp) in a bridge of its own,
//                                                 once; then the program faults (topic 6c.5)
//         <diagnostics directory> bridged-shared  a gain plug-in and the crash plug-in share a
//                                                 bridge, which the crash plug-in kills
//         <diagnostics directory> bridged-slow-writer  a bridged plug-in kills its bridge, and the
//                                                 program faults before the report of that is
//                                                 written: the supervisor holds that write open
#include <supple/diagcap/selfcapture.h>
#include <supple/diagnostics/frontend.h>
#include <supple/host/clap_host.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../../diagnostics/src/inventory.h"
#include "../../diagnostics/src/watch.h"

namespace {
// The supervisor's side of topic 6c, from inside the supervisor process: when a bridge's end
// that the operating system caused is applied, the capture slot the bridge wrote must hold a
// complete CTC5 capsule with the bridge's execution table copied after it. Said on stderr, for
// host_inventory.py (the report is made from it: step 6c.4).
void lookAtSlot(const supple::diagnostics::detail::Packet& packet,
                const supple::diagnostics::detail::Inventory& inventory) {
    namespace capture = supple::diagnostics::selfcapture;
    const auto& r = packet.instance;
    if (packet.kind != supple::diagnostics::detail::Message::Instance ||
        r.event != static_cast<std::uint8_t>(supple::diagctx::EventKind::Bridge) ||
        (r.how != static_cast<std::uint8_t>(supple::diagctx::BridgeHow::Signal) &&
         r.how != static_cast<std::uint8_t>(supple::diagctx::BridgeHow::Exception)))
        return;
    const auto capsule = inventory.slotCapsule(r.bridge);
    const char* verdict = "none";
    if (capsule) {
        supple::diagnostics::detail::CapsuleHeader header{};
        verdict = "bad";
        if (capsule->size() >= sizeof header + supple::diagctx::kTableBytes) {
            std::memcpy(&header, capsule->data(), sizeof header);
            if (header.magic == capture::kBridgeCapsuleMagic &&
                (header.flags & capture::kCapsuleHasTable) != 0 && header.reserved != 0)
                verdict = "ok";
        }
    }
    // Whether the supervisor's parser accepts it: what the report is made from.
    const bool parsed = capsule && supple::diagnostics::detail::parseBridgeCapsule(capsule->data(),
                                                                                   capsule->size());
    std::fprintf(stderr, "slot bridge=%u capsule=%s bytes=%zu parsed=%s\n",
                 static_cast<unsigned>(r.bridge), verdict, capsule ? capsule->size() : 0,
                 parsed ? "yes" : "no");
}

[[gnu::noinline]] void fault() { *reinterpret_cast<volatile int*>(8) = 1; }

// A bridged crash plug-in is made to misbehave in its next process(), and the host then makes a
// main-thread request, which is where it observes the bridge's end.
bool misbehave(const supple::host::PluginInfo& info, double mode,
               const std::shared_ptr<supple::host::BridgeProcess>& shared = nullptr) {
    auto made = shared ? supple::host::Instance::create(info, {}, true, shared)
                       : supple::host::Instance::create(info, {}, true);
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
    if (mode == 5.0) {
        // The instance passes this block and faults when the bridge destroys it: the host lets
        // go of the instance, and of the bridge with it, and the bridge's end is observed there.
        made->reset();
        return true;
    }
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

// The supervisor's order of writing, from inside it (round 6b): a bridge report's write is held
// open until the supervisor reaches the worker's report, which it must not do before the write
// has ended. If it does (a supervisor that does not wait for the writer), the hold is over at
// once and the steps say the wrong order; if it does not, the hold ends at its bound, which only
// makes a wrong order unseen, never a right one fail.
std::atomic<bool> gWorkerReportReached{false};
void sayStep(supple::diagnostics::detail::ReportStep step) {
    using supple::diagnostics::detail::ReportStep;
    if (step == ReportStep::BridgeReportWrite) {
        std::fprintf(stderr, "step bridge-write\n");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!gWorkerReportReached.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } else if (step == ReportStep::BridgeReportWritten) {
        std::fprintf(stderr, "step bridge-written\n");
    } else {
        gWorkerReportReached.store(true, std::memory_order_release);
        std::fprintf(stderr, "step worker-report\n");
    }
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
    if (mode == "bridged-slow-writer") {
        supple::host::setBridgeExecutable(BRIDGE_PATH);
        if (!misbehave(find("com.suppleaudio.test.crash"), 4.0)) return 7;
        fault();  // before the supervisor has finished writing the bridge's report
        return 0;
    }
    if (mode == "bridged-one" || mode == "bridged-shared") {
        supple::host::setBridgeExecutable(BRIDGE_PATH);
        const auto crash = find("com.suppleaudio.test.crash");
        double plugin = 4.0;
        if (mode == "bridged-one") {
            if (argc < 4) return 9;
            plugin = std::atof(argv[3]);
            if (!misbehave(crash, plugin)) {
                std::fprintf(stderr, "bridged-one: mode %g was not seen by the host\n", plugin);
                return 7;
            }
        } else {
            auto process = supple::host::createBridgeProcess();
            if (!process) return 10;
            // The gain plug-in is active in the same bridge, in the same binary, when the crash
            // plug-in faults.
            auto gain = supple::host::Instance::create(find("com.suppleaudio.test.gain"), {}, true,
                                                       *process);
            if (!gain || !(*gain)->activate(48000.0, 1, 64)) return 11;
            if (!misbehave(crash, plugin, *process)) {
                std::fprintf(stderr, "bridged-shared: the death was not seen by the host\n");
                return 7;
            }
        }
        if (!awaitReport(argv[1])) {
            std::fprintf(stderr, "%s: no report of the death in 30 s\n", mode.c_str());
            return 8;
        }
        fault();
        return 0;
    }
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
    // Only the supervisor process calls it (the worker never applies a packet).
    if (const std::string_view mode = argv[2];
        mode == "bridged-crash" || mode == "bridged-one" || mode == "bridged-shared")
        supple::diagnostics::detail::setInventoryHook(&lookAtSlot);
    if (std::string_view(argv[2]) == "bridged-slow-writer")
        supple::diagnostics::detail::setReportStepHook(&sayStep);
    return supple::diagnostics::runFrontend(argc, argv, supple::diagnostics::Role::Fixture, body,
                                            {std::filesystem::path(argv[1])});
}
