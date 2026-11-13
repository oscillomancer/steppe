// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
#include <filesystem>
#include <vector>
#include "supple/host/clap_host.h"
using namespace supple;
using namespace supple::host;
namespace fs = std::filesystem;
namespace {
struct WorkDir {
    fs::path path;
    explicit WorkDir(const char* name) {
        // Atomic creation also isolates runs in separate PID namespaces that
        // share a temp directory (parallel sanitizer presets in the sandbox).
        for (std::size_t index = 0;; ++index) {
            auto candidate =
                fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(index));
            if (fs::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
    }
    ~WorkDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
}  // namespace
TEST_CASE("missing scan helper names the executable to install") {
    ScanOptions options;
    options.bridgeExecutable = fs::path(SCANNER_PATH) / "missing";
    options.superviseAll = true;
    auto result = scan({}, options);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "host.scan.unsupervised");
    CHECK(result.error().path == "/bridge");
#if defined(__linux__)
    CHECK(result.error().message == "a background scan needs the plugin bridge");
    CHECK(result.error().hint ==
          "install the bridge beside the application, or scan in the foreground");
#else
    CHECK(result.error().message == "a background scan needs the plugin scanner");
    CHECK(result.error().hint ==
          "install the scanner beside the application, or scan in the foreground");
#endif
}
TEST_CASE("native supervised scanner isolates crashes and hangs and caches results") {
    WorkDir work("supple-native-scan");
    const std::vector<fs::path> modules{SCAN_GOOD_PATH, SCAN_CRASH_PATH, SCAN_HANG_PATH};
    ScanOptions options;
    options.bridgeExecutable = SCANNER_PATH;
    options.cacheDirectory = work.path / "cache";
    options.superviseAll = true;
    ScanReport report;
    // Neither the good plugin's scan nor the crash may race a deadline: under
    // a sanitizer on a loaded machine the helper can take seconds to start.
    options.timeoutMs = 120000;
    const std::vector<fs::path> goodAndCrash{modules[0], modules[1]};
    auto first = scan(goodAndCrash, options, &report);
    REQUIRE(first);
    CHECK(first->size() == 1);
    REQUIRE(report.failures.size() == 1);
    CHECK(report.failures[0].reason.code == "host.scan.crashed");
    // A hang outlasts any deadline, so a short one decides nothing by the clock.
    options.timeoutMs = 2000;
    const std::vector<fs::path> hang{modules[2]};
    auto second = scan(hang, options, &report);
    REQUIRE(second);
    CHECK(second->empty());
    REQUIRE(report.failures.size() == 1);
    CHECK(report.failures[0].reason.code == "host.scan.timeout");
    // All three are cached: none is scanned again.
    REQUIRE(scan(modules, options, &report));
    CHECK(report.cachedModules == 3);
}
TEST_CASE("native VST3 bundle scanning is supervised and cached") {
    const fs::path module = FIXTURE_VST3_PATH;
    ScanOptions options;
    options.bridgeExecutable = SCANNER_PATH;
    options.superviseAll = true;
    WorkDir work("supple-native-vst3-scan");
    options.cacheDirectory = work.path;
    ScanReport report;
    auto result = scan(std::span<const fs::path>(&module, 1), options, &report);
    REQUIRE(result);
    CHECK_FALSE(result->empty());
    CHECK(report.failures.empty());
    REQUIRE(scan(std::span<const fs::path>(&module, 1), options, &report));
    CHECK(report.cachedModules == 1);
}
