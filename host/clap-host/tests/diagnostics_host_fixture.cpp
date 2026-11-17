// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A program that runs the real host under the diagnostics frontend, for host_inventory.py: it
// loads the fixture plug-ins through host::Instance, as the engine does, then faults. The
// report must list what the host announced, not what a test announced by hand.
//   argv: <diagnostics directory> loaded
#include <supple/diagnostics/frontend.h>
#include <supple/host/clap_host.h>
#include <span>
#include <string>
#include <vector>

namespace {
[[gnu::noinline]] void fault() { *reinterpret_cast<volatile int*>(8) = 1; }

int body(int, char**) {
    const std::filesystem::path path = FIXTURE_CLAP_PATH;
    auto found = supple::host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!found) return 2;
    const auto find = [&](const char* id) {
        for (const auto& info : *found)
            if (info.id == id) return info;
        return supple::host::PluginInfo{};
    };
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
