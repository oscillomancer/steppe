// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>

#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;

namespace {
// Writes the first byte of each page of memory nothing has touched yet, so
// each write faults on the calling thread. The pages come from the system, not
// the allocator: after earlier tests in the same process free large blocks,
// new[] hands back heap pages that were touched before and nothing faults.
constexpr std::size_t kPages = 256, kStride = 4096;
void touchFresh() {
    constexpr std::size_t kSize = kPages * kStride;
#if defined(_WIN32)
    void* fresh = VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!fresh) return;  // the fault count then fails the test
#else
    void* fresh = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fresh == MAP_FAILED) return;  // the fault count then fails the test
#endif
    auto* bytes = static_cast<volatile char*>(fresh);
    for (std::size_t i = 0; i < kSize; i += kStride) bytes[i] = 1;
#if defined(_WIN32)
    VirtualFree(fresh, 0, MEM_RELEASE);
#else
    munmap(fresh, kSize);
#endif
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
        // macOS counts fresh pages' faults in clusters (core-base.md), so
        // there the process's count only has to not fall.
#if defined(__APPLE__)
        CHECK(after.minor + after.major.value_or(0) >= before.minor + before.major.value_or(0));
#else
        CHECK(after.minor + after.major.value_or(0) >=
              before.minor + before.major.value_or(0) + static_cast<std::int64_t>(kPages / 2));
#endif
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
