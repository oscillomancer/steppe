// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
#include <fstream>
#include <iterator>
#include "supple/host/child_process.h"
#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include "../src/child_status.h"
#include "process_pause.h"
#endif
using namespace supple::host;
TEST_CASE("child capture preserves arguments, exit code and bounded output") {
    const std::array<std::string, 5> args{"failure", "space value", "quote\"inside", "trailing\\",
                                          ""};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    auto output =
        (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(10), 1024);
    REQUIRE(output);
    CHECK(output->text == "space value\nquote\"inside\ntrailing\\\n\n");
    CHECK(output->exitCode == 7);
    CHECK_FALSE(output->crashed);
}
TEST_CASE("child stderr is suppressed by default and inherited only when requested") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("supple-child-stderr-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } remove{file};
    for (const std::string inherit : {"0", "1"}) {
        for (const std::string capture : {"0", "1"}) {
            const std::array<std::string, 4> args{"stderr-parent", file.string(), inherit, capture};
            auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
            REQUIRE(child);
            auto output =
                (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(10), 32);
            REQUIRE(output);
            CHECK(output->exitCode == 0);
            CHECK(output->text.empty());
            std::ifstream input(file, std::ios::binary);
            REQUIRE(input);
            const std::string diagnostic((std::istreambuf_iterator<char>(input)), {});
            CHECK(diagnostic == (inherit == "1" ? "child stderr" : ""));
        }
    }
}
TEST_CASE("child timeout and output overflow are structured and clean up the child") {
    for (const std::string mode : {"hang", "flood"}) {
        const std::array<std::string, 1> args{mode};
        auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
        REQUIRE(child);
        // The flood ends at the output limit, so its deadline only has to
        // outlast a slow child start (a loaded Windows runner took 500 ms).
        auto output = (*child)->collect(std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(mode == "hang" ? 500 : 30000),
                                        64);
        REQUIRE_FALSE(output);
        CHECK(output.error().code == (mode == "hang" ? "child.timeout" : "child.output_limit"));
        CHECK_FALSE(output.error().path.empty());
        CHECK_FALSE(output.error().hint.empty());
    }
    const std::array<std::string, 0> args{};
    auto missing = ChildProcess::spawn("/nonexistent/supple-child", args);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "child.spawn");
}

#if defined(_WIN32)
#include <windows.h>
TEST_CASE("Windows child job terminates descendants retaining stdout") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("supple-child-descendant-" + std::to_string(GetCurrentProcessId()) + ".pid");
    const std::array<std::string, 2> args{"tree", file.string()};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    auto output = (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(5), 64);
    REQUIRE_FALSE(output);
    CHECK(output.error().code == "child.timeout");
    DWORD pid = 0;
    std::ifstream input(file);
    REQUIRE(static_cast<bool>(input >> pid));
    input.close();
    HANDLE descendant = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    REQUIRE(descendant);
    struct Close {
        HANDLE value;
        ~Close() { CloseHandle(value); }
    } close{descendant};
    child->reset();
    CHECK(WaitForSingleObject(descendant, 5000) == WAIT_OBJECT_0);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
#endif

#if !defined(_WIN32)
TEST_CASE("a stopped child is alive; only an exit or a killing signal ends it") {
    // Native CI on macOS (review round 34): waitid(WEXITED | WNOHANG |
    // WNOWAIT) there also returns the record of a stopped child, and the
    // host took a stopped bridge for one that died of SIGSTOP. Linux returns
    // that record only when asked (WSTOPPED), so ask, and judge it.
    const std::array<std::string, 1> args{"hang"};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    const int pid = (*child)->pid();
    REQUIRE(supple::test::pauseProcess(pid));
    siginfo_t stopped{};
    REQUIRE(::waitid(P_PID, static_cast<id_t>(pid), &stopped,
                     WEXITED | WSTOPPED | WNOHANG | WNOWAIT) == 0);
    REQUIRE(stopped.si_pid == pid);
    CHECK(stopped.si_code == CLD_STOPPED);
    CHECK(stopped.si_status == SIGSTOP);
    CHECK_FALSE(supple::host::childEnded(stopped));
    supple::test::resumeProcess(pid);
    REQUIRE(supple::test::killProcess(pid));
    siginfo_t ended{};
    REQUIRE(::waitid(P_PID, static_cast<id_t>(pid), &ended, WEXITED | WNOWAIT) == 0);
    CHECK(ended.si_code == CLD_KILLED);
    CHECK(supple::host::childEnded(ended));
}

TEST_CASE("child control inheritance preserves stdout capture with closed stdio") {
    const std::array<std::string, 1> args{"control-stdio"};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    auto output =
        (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(10), 32);
    REQUIRE(output);
    CHECK(output->exitCode == 0);
    CHECK_FALSE(output->crashed);
}
#endif
