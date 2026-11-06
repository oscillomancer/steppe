// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr std::int64_t kBar = 96000;  // 4/4 at 120 beats a minute
constexpr std::int64_t kSixteenth = 6000;

struct Note {
    std::int64_t at;  // the frame it starts
    int semis;        // from C4
};

std::vector<Note> notes(ModuleHarness& h) {
    const auto& g = h.output("gate");
    const auto& p = h.output("pitch");
    std::vector<Note> out;
    for (std::size_t n = 0; n < g.size(); ++n)
        if (g[n] >= 0.5 && (n == 0 || g[n - 1] < 0.5))
            out.push_back({static_cast<std::int64_t>(n), static_cast<int>(std::lround(p[n] * 12))});
    return out;
}

// The notes of bar b, their frames from the bar's start.
std::vector<Note> bar(const std::vector<Note>& all, std::int64_t b) {
    std::vector<Note> out;
    for (const auto& n : all)
        if (n.at >= b * kBar && n.at < (b + 1) * kBar) out.push_back({n.at - b * kBar, n.semis});
    return out;
}

void song(ModuleHarness& h) {
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
}

int pc(int s) { return ((s % 12) + 12) % 12; }

}  // namespace

TEST_CASE("phrase: a call, then an answer on its rhythm that comes home") {
    ModuleHarness h(supple::rack::test::stock("phrase"));
    song(h);
    h.set("renew", 0);
    h.run(kBar * 4);
    const auto all = notes(h);
    const std::set<int> cMinor = {0, 2, 3, 5, 7, 8, 10};
    for (const auto& n : all) {
        CHECK(cMinor.count(pc(n.semis)) == 1);
        CHECK(n.at % (kSixteenth / 2) <= 96 + 1);  // on a sixteenth, or 2 ms after a tie
    }
    const auto call = bar(all, 0);
    const auto reply = bar(all, 1);
    REQUIRE(call.size() >= 3);
    REQUIRE(call.size() == reply.size());
    CHECK(call[0].at == 0);
    for (std::size_t k = 0; k < call.size(); ++k) CHECK(call[k].at == reply[k].at);
    // The call ends open, on D or F; the answer on C.
    CHECK((pc(call.back().semis) == 2 || pc(call.back().semis) == 5));
    CHECK(pc(reply.back().semis) == 0);
    // Answer is high through the second bar; Start fires at each bar.
    CHECK(h.output("answer")[kBar - 100] == 0.0);
    CHECK(h.output("answer")[kBar + 100] == 1.0);
    CHECK(h.output("start")[kBar + 10] == 1.0);
    CHECK(h.output("start")[kBar + 100] == 0.0);
    // Accent: 1 for the form's first note, less after.
    CHECK(h.output("accent")[100] == 1.0);
    CHECK(h.output("accent")[static_cast<std::size_t>(kBar + 100)] == doctest::Approx(0.85));
    // Locked: the next time round is the same.
    const auto again = bar(all, 2);
    REQUIRE(again.size() == call.size());
    for (std::size_t k = 0; k < call.size(); ++k) CHECK(again[k].semis == call[k].semis);
}

TEST_CASE("phrase: at Contrast 0 only the answer's last note differs") {
    for (int s = 0; s < 4; ++s) {
        CAPTURE(s);
        ModuleHarness h(supple::rack::test::stock("phrase"), 48000.0, 100 + s);
        song(h);
        h.set("contrast", 0);
        h.set("density", 60);
        h.run(kBar * 2);
        const auto all = notes(h);
        const auto call = bar(all, 0);
        const auto reply = bar(all, 1);
        REQUIRE(call.size() == reply.size());
        for (std::size_t k = 0; k + 1 < call.size(); ++k) CHECK(call[k].semis == reply[k].semis);
        CHECK(pc(reply.back().semis) == 0);
    }
}

TEST_CASE("phrase: the period's half answer ends on the fifth, its full answer home") {
    ModuleHarness h(supple::rack::test::stock("phrase"));
    song(h);
    h.set("form", 2);
    h.set("mode", 0);  // major
    h.set("key", 7);   // G: the fifth is D
    h.set("renew", 0);
    h.run(kBar * 4);
    const auto all = notes(h);
    CHECK(pc(bar(all, 1).back().semis) == 2);
    CHECK(pc(bar(all, 3).back().semis) == 7);
    // The calls are the same; the answers share their rhythm.
    const auto a = bar(all, 0);
    const auto c = bar(all, 2);
    REQUIRE(a.size() == c.size());
    for (std::size_t k = 0; k < a.size(); ++k) CHECK(a[k].semis == c[k].semis);
    CHECK(h.output("answer")[static_cast<std::size_t>(2 * kBar + 100)] == 0.0);
    CHECK(h.output("answer")[static_cast<std::size_t>(3 * kBar + 100)] == 1.0);
}

TEST_CASE("phrase: Renew writes new calls as the form comes round; Write asks for one") {
    auto calls = [](double renew, bool write) {
        ModuleHarness h(supple::rack::test::stock("phrase"));
        song(h);
        h.set("renew", renew);
        if (write)
            h.input("write", 1,
                    [](int, std::int64_t n) { return n > 1000 && n < 1100 ? 1.0 : 0.0; });
        h.run(kBar * 8);
        const auto all = notes(h);
        int differ = 0;
        for (std::int64_t b = 2; b < 8; b += 2) {
            const auto first = bar(all, 0);
            const auto later = bar(all, b);
            bool same = first.size() == later.size();
            for (std::size_t k = 0; same && k < first.size(); ++k)
                same = first[k].at == later[k].at && first[k].semis == later[k].semis;
            differ += same ? 0 : 1;
        }
        return differ;
    };
    CHECK(calls(0, false) == 0);
    CHECK(calls(100, false) == 3);
    CHECK(calls(0, true) >= 1);
}

TEST_CASE("phrase: a clock steps it; Reset starts the same phrases over") {
    ModuleHarness h(supple::rack::test::stock("phrase"));
    h.set("renew", 50);
    h.input("clock", 1, [](int, std::int64_t n) { return n % 1000 < 10 ? 1.0 : 0.0; });
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 64 * 1000 + 500 && n < 64 * 1000 + 600 ? 1.0 : 0.0;
    });
    h.run(1000 * 128);
    const auto all = notes(h);
    std::vector<Note> before, after;
    for (const auto& n : all) {
        if (n.at < 64000)
            before.push_back(n);
        else if (n.at >= 65000)
            after.push_back({n.at - 65000, n.semis});
    }
    REQUIRE(before.size() > 8);
    REQUIRE(after.size() >= before.size());
    for (std::size_t k = 0; k < before.size(); ++k) {
        CHECK(before[k].at == after[k].at);
        CHECK(before[k].semis == after[k].semis);
    }
    for (const auto& n : before) CHECK(n.at % 1000 <= 96);
}
