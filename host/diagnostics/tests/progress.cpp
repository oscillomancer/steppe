// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Progress for the diagnostics tests. Doctest prints nothing until the run ends, so a test
// that blocks leaves a CI log with no clue where. This names each test case on stderr as it
// starts, and says on stderr, naming the case, when one has run longer than a minute and a
// half. It never ends the run or decides a result: whether a slow case is stuck depends on
// the runner and the sanitizer, and ctest's timeout is what ends a case that never returns.
// The per-call deadlines in the code under test are the first bound.
#include <doctest/doctest.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
// The seconds after which a running case is reported: DIAGNOSTICS_TEST_LIMIT_SECONDS if it
// is a whole number of at least 1, else 90. Anything else (empty, text, a sign or a space,
// zero, trailing characters, out of range) is ignored rather than read as a value.
std::chrono::seconds parseLimit(const char* text) {
    constexpr long long kDefault = 90, kMax = 1'000'000;
    if (!text || *text < '0' || *text > '9') return std::chrono::seconds(kDefault);
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > kMax)
        return std::chrono::seconds(kDefault);
    return std::chrono::seconds(value);
}

struct Guard {
    std::mutex lock;
    std::string name;
    std::chrono::steady_clock::time_point started;
    bool running = false, reported = false;
    std::chrono::seconds limit = parseLimit(std::getenv("DIAGNOSTICS_TEST_LIMIT_SECONDS"));
    std::once_flag once;
    void watch() {
        std::thread([this] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::lock_guard<std::mutex> hold(lock);
                if (running && !reported && std::chrono::steady_clock::now() - started > limit) {
                    reported = true;  // once for each case; the run carries on
                    std::fprintf(stderr,
                                 "diagnostics.tests: \"%s\" has run longer than %lld s and may "
                                 "be stuck; still waiting (ctest's timeout decides)\n",
                                 name.c_str(), static_cast<long long>(limit.count()));
                    std::fflush(stderr);
                }
            }
        }).detach();
    }
};
// Never destroyed: the watcher runs until the process ends, and it takes this lock after
// main returns and static destructors have run (a destroyed mutex is an error, or worse,
// on some standard libraries).
Guard& guard = *new Guard;

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
            guard.reported = false;
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

TEST_CASE("the progress limit reads whole seconds and ignores anything else") {
    CHECK(parseLimit(nullptr).count() == 90);
    CHECK(parseLimit("").count() == 90);
    CHECK(parseLimit("5").count() == 5);
    CHECK(parseLimit("600").count() == 600);
    for (const char* bad :
         {"abc", "0", "-3", "12x", " 7", "1e3", "99999999999999999999", "2000000"})
        CHECK(parseLimit(bad).count() == 90);
}
