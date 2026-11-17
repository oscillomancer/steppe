// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include "../src/scan.h"
#include "supple/base/files.h"
#include "supple/json/json.h"
#include "supple/script_export.hpp"

using namespace supple;
using namespace supple::host;
namespace fs = std::filesystem;
namespace {
struct ScanTest {
    fs::path dir;
    ScanOptions options;
    std::optional<std::string> previousLog;
    ScanTest() {
        char pattern[] = "/tmp/supple-scan-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        REQUIRE(created);
        dir = created;
        options.cacheDirectory = dir / "cache";
        options.bridgeExecutable = BRIDGE_PATH;
        options.timeoutMs = 2000;
        if (const char* log = std::getenv("SUPPLE_SCAN_FIXTURE_LOG")) previousLog = log;
        REQUIRE(::setenv("SUPPLE_SCAN_FIXTURE_LOG", (dir / "calls").c_str(), 1) == 0);
    }
    ~ScanTest() {
        if (previousLog)
            ::setenv("SUPPLE_SCAN_FIXTURE_LOG", previousLog->c_str(), 1);
        else
            ::unsetenv("SUPPLE_SCAN_FIXTURE_LOG");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path copy(const fs::path& source, const char* name) {
        const auto target = dir / name;
        fs::copy_file(source, target);
        return target;
    }
    std::map<int, int> calls() {
        std::ifstream log(dir / "calls");
        std::map<int, int> counts;
        int mode;
        long pid;
        while (log >> mode >> pid) {
            ++counts[mode];
            int status = 0;
            errno = 0;
            CHECK(::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == -1);
            CHECK(errno == ECHILD);  // scanners were reaped, not left as zombies
        }
        return counts;
    }
    fs::path bridgeScript(std::string_view body) {
        const auto script = dir / "scanner";
        REQUIRE(writeFileAtomic(script, "#!/bin/sh\n" + std::string(body)));
        fs::permissions(script, fs::perms::owner_all);
        options.bridgeExecutable = script;
        return script;
    }
};
void touch(const fs::path& path) {
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(1));
}
std::size_t fdCount() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& file : fs::directory_iterator("/proc/self/fd")) ++count;
    return count;
}
}  // namespace

TEST_CASE("supervised scan isolates entry crashes/hangs and caches results until touch") {
    ScanTest fixture;
    const std::vector<fs::path> paths{fixture.copy(SCAN_GOOD_PATH, "good.clap"),
                                      fixture.copy(SCAN_CRASH_PATH, "crash.clap"),
                                      fixture.copy(SCAN_HANG_PATH, "hang.clap")};
    const auto beforeFds = fdCount();
    ScanReport report;
    auto first = scan(paths, fixture.options, &report);
    REQUIRE(first);
    REQUIRE(first->size() == 1);
    CHECK(first->front().id == "com.suppleaudio.test.scan");
    REQUIRE(report.failures.size() == 2);
    CHECK(report.failures[0].reason.code == "host.scan.crashed");
    CHECK(report.failures[0].reason.message.find("signal 9") != std::string::npos);
    CHECK(report.failures[1].reason.code == "host.scan.timeout");
    CHECK(report.failures[1].reason.hint == "repair/reinstall the module to retry its scan");
    CHECK(report.failures[1].reason.message == "plugin scanner timed out after 2000 ms");
    CHECK_FALSE(report.failures[0].cached);
    CHECK_FALSE(report.failures[1].cached);
    CHECK(report.cachedModules == 0);
    CHECK(fixture.calls() == std::map<int, int>{{0, 1}, {1, 1}, {2, 1}});
    auto second = scan(paths, fixture.options, &report);
    REQUIRE(second);
    REQUIRE(second->size() == 1);
    REQUIRE(report.failures.size() == 2);
    CHECK(report.cachedModules == 3);
    CHECK(report.failures[0].cached);
    CHECK(report.failures[1].cached);
    CHECK(fixture.calls() == std::map<int, int>{{0, 1}, {1, 1}, {2, 1}});
    fixture.options.bridgeExecutable = fixture.dir / "missing";
    REQUIRE(scan(paths, fixture.options, &report));
    CHECK(report.cachedModules == 3);
    REQUIRE(report.failures.size() == 2);
    CHECK(report.failures[0].cached);
    CHECK(report.failures[1].cached);
    CHECK(fixture.calls() == std::map<int, int>{{0, 1}, {1, 1}, {2, 1}});
    fixture.options.bridgeExecutable = BRIDGE_PATH;
    for (const auto& path : paths) touch(path);
    auto third = scan(paths, fixture.options, &report);
    REQUIRE(third);
    CHECK(report.cachedModules == 0);
    CHECK(fixture.calls() == std::map<int, int>{{0, 2}, {1, 2}, {2, 2}});
    CHECK(fdCount() == beforeFds);
    for (const auto& file : fs::directory_iterator(fixture.options.cacheDirectory)) {
        auto text = readTextFile(file.path());
        REQUIRE(text);
        auto json = json::parse(*text);
        REQUIRE(json);
        CHECK(*text == json::writeCanonical(*json));
    }
}

TEST_CASE("the agent skill ships beside the binary") {
    const auto skill = skillDir();
    REQUIRE(skill);
    CHECK(fs::is_regular_file(*skill / "SKILL.md"));
}

TEST_CASE("the stock folder's modules are scanned afresh, never from the cache") {
    // The script player lists the scripts in its library: its file does not
    // change when they do.
    // Skipped only without stock plugins; a stock folder without the player
    // is a build that left it out (review round 33).
    const auto stock = stockPluginDir();
    if (!stock) {
        MESSAGE("skipped: no stock plugin folder");
        return;
    }
    const fs::path player = *stock / "Script Player.clap";
    REQUIRE_MESSAGE(fs::exists(player), player.string() << " is missing from the stock folder");
    ScanTest fixture;
    fixture.options.superviseAll = true;  // as a background plugin.scan does
    REQUIRE(scan(std::span<const fs::path>(&player, 1), fixture.options));
    std::error_code ec;
    CHECK((!fs::exists(fixture.options.cacheDirectory, ec) ||
           fs::is_empty(fixture.options.cacheDirectory, ec)));
}

TEST_CASE("a script player anywhere is scanned afresh, never from the cache") {
    // Its plugins are its library's scripts, which its file does not show.
    ScanTest fixture;
    const fs::path plain = fixture.copy(SCAN_GOOD_PATH, "plain.clap");
    const fs::path player = fixture.copy(SCAN_GOOD_PATH, "player.clap");
    std::ofstream(player, std::ios::app | std::ios::binary) << SUPPLE_SCRIPT_PLAYER_MARK;
    const std::vector<fs::path> paths{plain, player};
    ScanReport report;
    REQUIRE(scan(paths, fixture.options, &report));
    REQUIRE(scan(paths, fixture.options, &report));
    CHECK(report.cachedModules == 1);  // the plain copy only
}

TEST_CASE("scan canonicalizes and deduplicates aliases in search path order") {
    ScanTest fixture;
    const auto good = fixture.copy(SCAN_GOOD_PATH, "good.clap");
    const auto alias = fixture.dir / "alias.clap";
    fs::create_symlink(good, alias);
    const std::vector<fs::path> paths{alias, good, fixture.dir};
    auto result = scan(paths, fixture.options);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().path == fs::canonical(good));
    CHECK(fixture.calls()[0] == 1);
}

TEST_CASE("invalid cache metadata causes a fresh scan") {
    ScanTest fixture;
    const auto good = fixture.copy(SCAN_GOOD_PATH, "good.clap");
    REQUIRE(scan(std::span<const fs::path>(&good, 1), fixture.options));
    const auto recordPath = fs::directory_iterator(fixture.options.cacheDirectory)->path();
    auto text = readTextFile(recordPath);
    REQUIRE(text);
    auto record = json::parse(*text);
    REQUIRE(record);
    (*record)["reply"]["plugins"].asArray().front()["instrument"] = 42;
    REQUIRE(writeFileAtomic(recordPath, json::writeCanonical(*record)));
    ScanReport report;
    auto result = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
    REQUIRE(result);
    CHECK(result->size() == 1);
    CHECK(report.cachedModules == 0);
    CHECK(fixture.calls()[0] == 2);
}

TEST_CASE("scan caches invalid and excessive child output without losing healthy modules") {
    for (const bool excessive : {false, true}) {
        ScanTest fixture;
        const auto good = fixture.copy(SCAN_GOOD_PATH, "good.clap");
        fixture.bridgeScript(excessive ? "while :; do printf '12345678901234567890'; done\n"
                                       : "printf 'not JSON'\n");
        fixture.options.maxOutputBytes = 128;
        ScanReport report;
        auto result = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
        REQUIRE(result);
        CHECK(result->empty());
        REQUIRE(report.failures.size() == 1);
        CHECK(report.failures[0].reason.code ==
              (excessive ? "host.scan.output_limit" : "host.scan.invalid_output"));
        fixture.options.bridgeExecutable = BRIDGE_PATH;
        fixture.options.maxOutputBytes = 1u << 20;
        auto cached = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
        REQUIRE(cached);
        CHECK(cached->empty());
        REQUIRE(report.failures.size() == 1);
        CHECK(report.failures[0].cached);
        CHECK(fixture.calls().empty());
        touch(good);
        auto retried = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
        REQUIRE(retried);
        CHECK(retried->size() == 1);
        CHECK(report.failures.empty());
    }
}

TEST_CASE("scan kills descendants retaining stdout at its deadline") {
    struct Subreaper {
        int previous = 0;
        Subreaper() {
            REQUIRE(::prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0);
            REQUIRE(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
        }
        ~Subreaper() { ::prctl(PR_SET_CHILD_SUBREAPER, previous); }
    } subreaper;
    ScanTest fixture;
    const auto good = fixture.copy(SCAN_GOOD_PATH, "good.clap");
    fixture.bridgeScript("sleep 60 &\necho $! > '" + (fixture.dir / "descendant").string() +
                         "'\nexit 0\n");
    ScanReport report;
    auto result = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
    REQUIRE(result);
    REQUIRE(report.failures.size() == 1);
    CHECK(report.failures[0].reason.code == "host.scan.timeout");
    // No wall-clock assertion: the reported deadline is the behavior under test.
    std::ifstream descendant(fixture.dir / "descendant");
    long pid = 0;
    REQUIRE(static_cast<bool>(descendant >> pid));
    int status = 0;
    REQUIRE(::waitpid(static_cast<pid_t>(pid), &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
}

TEST_CASE("missing scan bridge warns and preserves catalogue; explicit test hook bypasses cache") {
    ScanTest fixture;
    const auto good = fixture.copy(SCAN_GOOD_PATH, "good.clap");
    fixture.options.bridgeExecutable = fixture.dir / "missing";
    ScanReport report;
    auto fallback = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
    REQUIRE(fallback);
    CHECK(fallback->size() == 1);
    REQUIRE(report.warnings.size() == 1);
    CHECK(report.warnings[0].code == "host.scan.unsupervised");
    CHECK(lastScanReport().warnings.size() == 1);
    fixture.options.bridgeExecutable = fixture.dir;
    REQUIRE(scan(std::span<const fs::path>(&good, 1), fixture.options, &report));
    REQUIRE(report.warnings.size() == 1);
    CHECK(report.warnings[0].code == "host.scan.unsupervised");
    fixture.options.forceInProcess = true;
    auto local = scan(std::span<const fs::path>(&good, 1), fixture.options, &report);
    REQUIRE(local);
    CHECK(local->size() == 1);
    CHECK(report.warnings.empty());
    CHECK_FALSE(fs::exists(fixture.options.cacheDirectory));
}

TEST_CASE("VST3 scan bundle cache includes binary and metadata timestamps") {
    ScanTest fixture;
    const auto bundle = fixture.dir / "test-vst3.vst3";
    fs::copy(FIXTURE_VST3_PATH, bundle, fs::copy_options::recursive);
    ScanReport report;
    auto first = scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report);
    REQUIRE(first);
    CHECK_FALSE(first->empty());
    CHECK(report.failures.empty());
    auto second = scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report);
    REQUIRE(second);
    CHECK(report.cachedModules == 1);
    touch(bundle / "Contents/x86_64-linux/test-vst3.so");
    REQUIRE(scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report));
    CHECK(report.cachedModules == 0);
    touch(bundle / "Contents/Resources/moduleinfo.json");
    REQUIRE(scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report));
    CHECK(report.cachedModules == 0);
    // Linked architecture folders are valid bundles. Follow them while avoiding
    // directory cycles, and include changes to their actual binaries in the key.
    const auto architecture = bundle / "Contents/x86_64-linux";
    fs::rename(architecture, fixture.dir / "binaries");
    fs::create_directory_symlink(fixture.dir / "binaries", architecture);
    fs::create_directory_symlink(bundle, bundle / "Contents/cycle");
    REQUIRE(scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report));
    touch(architecture / "test-vst3.so");
    REQUIRE(scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report));
    CHECK(report.cachedModules == 0);
    const auto installed = fixture.dir / "test-vst3";
    fs::rename(bundle, installed);
    fs::create_directory_symlink(installed, bundle);
    auto linked = scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report);
    REQUIRE(linked);
    CHECK_FALSE(linked->empty());
    REQUIRE(report.failures.empty());
    CHECK(linked->front().path == bundle);
    REQUIRE(Instance::create(linked->front(), HostIdentity{}));
    REQUIRE(scan(std::span<const fs::path>(&bundle, 1), fixture.options, &report));
    CHECK(report.cachedModules == 1);
}

TEST_CASE("scan reply rejects incomplete metadata and foreign module paths") {
    PluginInfo info;
    info.id = "com.suppleaudio.test.scan";
    info.path = "/tmp/test.clap";
    auto reply = scanReply(std::vector<PluginInfo>{info});
    REQUIRE(parseScanReply(reply, info.path));
    reply["plugins"].asArray().front()["path"] = "/tmp/other.clap";
    CHECK_FALSE(parseScanReply(reply, info.path));
    reply["plugins"].asArray().front()["path"] = info.path.string();
    reply["plugins"].asArray().front().asObject().erase("instrument");
    CHECK_FALSE(parseScanReply(reply, info.path));
}
