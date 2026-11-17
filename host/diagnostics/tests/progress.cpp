// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Progress and a hard stop for the diagnostics tests. Doctest prints nothing until the run
// ends, so a test that blocks leaves a CI log with no clue where. This names each test
// case on stderr as it starts, and ends the process, naming the case, if one runs longer
// than a minute and a half: well past any case here, and far inside ctest's timeout. The
// per-call deadlines in the code under test are the first bound; this is the one that
// holds even when a test or a platform's socket behaviour defeats them.
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
// DIAGNOSTICS_TEST_LIMIT_SECONDS overrides the limit (the test of this guard uses it).
std::chrono::seconds limit() {
    if (const char* text = std::getenv("DIAGNOSTICS_TEST_LIMIT_SECONDS"))
        return std::chrono::seconds(std::atoi(text));
    return std::chrono::seconds(90);
}
const auto kLimit = limit();

struct Guard {
    std::mutex lock;
    std::string name;
    std::chrono::steady_clock::time_point started;
    bool running = false;
    std::once_flag once;
    void watch() {
        std::thread([this] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::lock_guard<std::mutex> hold(lock);
                if (running && std::chrono::steady_clock::now() - started > kLimit) {
                    std::fprintf(stderr,
                                 "diagnostics.tests: \"%s\" ran longer than %lld s and is stuck; "
                                 "ending the run\n",
                                 name.c_str(), static_cast<long long>(kLimit.count()));
                    std::fflush(stderr);
                    std::_Exit(3);
                }
            }
        }).detach();
    }
};
Guard guard;

struct Progress : doctest::IReporter {
    explicit Progress(const doctest::ContextOptions&) {}
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {
        std::call_once(guard.once, [] { guard.watch(); });
    }
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& test) override {
        {
            std::lock_guard<std::mutex> hold(guard.lock);
            guard.name = test.m_name;
            guard.started = std::chrono::steady_clock::now();
            guard.running = true;
        }
        std::fprintf(stderr, "[diagnostics.tests] %s\n", test.m_name);
        std::fflush(stderr);
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        std::lock_guard<std::mutex> hold(guard.lock);
        guard.running = false;
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};
}  // namespace

REGISTER_LISTENER("progress", 1, Progress);
