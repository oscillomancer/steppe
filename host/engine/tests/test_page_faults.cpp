// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;

namespace {
// Writes the first byte of each page of memory nothing has touched yet, so
// each write faults on the calling thread.
constexpr std::size_t kPages = 256, kStride = 4096;
void touchFresh() {
    std::unique_ptr<char[]> fresh(new char[kPages * kStride]);
    auto* bytes = reinterpret_cast<volatile char*>(fresh.get());
    for (std::size_t i = 0; i < kPages * kStride; i += kStride) bytes[i] = 1;
}
}  // namespace

TEST_CASE("engine.status counts the page faults the audio threads take") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-page-faults-" +
                      std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    std::filesystem::remove_all(dir);
    {
        AppOptions options;
        options.workdir = dir;
        options.chainDir = dir / "chains";
        options.templateDir = dir / "templates";
        options.projectTemplateDir = dir / "project-templates";
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        auto app = std::move(*created);
        CHECK(app->engineStatus().audioPageFaults.scope == "none");
        // Manual: the cycles run on this thread, which is then the callback's.
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        backend.run(2);
        app->tick();  // sees the callback thread
        const auto before = app->engineStatus().audioPageFaults;
#if defined(__linux__)
        CHECK(before.scope == "audio_threads");
        CHECK(before.threads >= 1);
        REQUIRE(before.major);
#else
        CHECK(before.scope == "process");
        CHECK(before.threads == 0);
#endif
        touchFresh();  // on the callback thread
        const auto after = app->engineStatus().audioPageFaults;
        CHECK(after.minor + after.major.value_or(0) >=
              before.minor + before.major.value_or(0) + static_cast<std::int64_t>(kPages / 2));
#if defined(__linux__)
        // Another thread's faults are not the audio threads'.
        std::thread other(touchFresh);
        other.join();
        CHECK(app->engineStatus().audioPageFaults.minor < after.minor + 64);
#endif
        app->stopEngine();
        CHECK(app->engineStatus().audioPageFaults.scope == "none");
        CHECK(app->engineStatus().audioPageFaults.minor == 0);
    }
    std::filesystem::remove_all(dir);
}
