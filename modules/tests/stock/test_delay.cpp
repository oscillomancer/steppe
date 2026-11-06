// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal click() {
    return [](int, std::int64_t n) { return n == 0 ? 1.0 : 0.0; };
}
// Where the biggest sample in [from, to) is.
std::size_t loudest(const std::vector<double>& y, std::size_t from, std::size_t to) {
    std::size_t at = from;
    for (std::size_t n = from; n < to; ++n)
        if (std::abs(y[n]) > std::abs(y[at])) at = n;
    return at;
}
void plain(ModuleHarness& h) {
    h.set("wow", 0);
    h.set("flutter", 0);
    h.set("tone", 0);
    h.set("mix", 100);
}
}  // namespace

TEST_CASE("delay: echoes at the time, each feedback's share of the last") {
    ModuleHarness h(stock("delay"));
    plain(h);
    h.set("time", 100);
    h.set("feedback", 50);
    h.input("in_l", 1, click());
    h.run(48000);
    const auto& y = h.output("out_l");
    CHECK(loudest(y, 1000, 7000) == 4800);
    CHECK(loudest(y, 7000, 12000) == 9600);
    CHECK(loudest(y, 12000, 17000) == 14400);
    const double first = y[4800], second = y[9600];
    CHECK(first == doctest::Approx(1.0).epsilon(0.03));
    CHECK(second / first == doctest::Approx(0.5).epsilon(0.03));
    CHECK(std::abs(y[100]) < 1e-9);                            // mix 100: no dry
    CHECK(h.output("out_r")[4800] == doctest::Approx(first));  // the left feeds both
}

TEST_CASE("delay: synced to the tempo in note values") {
    ModuleHarness h(stock("delay"));
    plain(h);
    h.set("sync", 6);  // 1/8: 250 ms at 120 bpm
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
    h.input("in_l", 1, click());
    h.run(30000);
    CHECK(loudest(h.output("out_l"), 1000, 20000) == 12000);
}

TEST_CASE("delay: ping-pong bounces left, right, left") {
    ModuleHarness h(stock("delay"));
    plain(h);
    h.set("time", 50);
    h.set("feedback", 80);
    h.set("ping_pong", 1);
    h.input("in_l", 1, click());
    h.run(9600);
    const auto &l = h.output("out_l"), &r = h.output("out_r");
    CHECK(std::abs(l[2400]) > 0.4);
    CHECK(std::abs(r[2400]) < 1e-3);
    CHECK(std::abs(r[4800]) > 0.2);
    CHECK(std::abs(l[4800]) < 1e-3);
    CHECK(std::abs(l[7200]) > 0.1);
}

TEST_CASE("delay: feedback past 100 % builds but stays bounded") {
    ModuleHarness h(stock("delay"));
    h.set("time", 30);
    h.set("feedback", 120);
    h.set("mix", 100);
    h.input("in_l", 1, [](int, std::int64_t n) { return n < 480 ? 0.3 * std::sin(n * 0.2) : 0.0; });
    h.run(48000 * 10);
    const auto& y = h.output("out_l");
    CHECK(peak(y) < 1.5);
    CHECK(rms(slice(y, 48000 * 9)) > 0.2);  // still going
}

TEST_CASE("delay: a connected return replaces the feedback") {
    ModuleHarness h(stock("delay"));
    plain(h);
    h.set("time", 50);
    h.set("feedback", 90);
    h.input("in_l", 1, click());
    h.input("return_l", 1, [](int, std::int64_t) { return 0.0; });
    h.run(9600);
    const auto& y = h.output("out_l");
    CHECK(std::abs(y[2400]) > 0.9);
    CHECK(peak(slice(y, 3000)) < 1e-3);  // nothing came back (the first echo's tail)
    CHECK(h.output("send_l")[2400] == doctest::Approx(y[2400]));
}

TEST_CASE("delay: wow and flutter move the echo, the same every time") {
    ModuleHarness a(stock("delay")), b(stock("delay"));
    for (ModuleHarness* h : {&a, &b}) {
        h->set("wow", 100);
        h->set("flutter", 100);
        h->set("mix", 100);
        h->set("time", 200);
        h->input("in_l", 1,
                 [](int, std::int64_t n) { return std::sin(2 * kPi * 440 * n / 48000.0); });
        h->run(96000);
    }
    CHECK(a.output("out_l") == b.output("out_l"));
    const auto y = slice(a.output("out_l"), 48000);
    const double hz = frequency(y, 48000);
    CHECK(hz == doctest::Approx(440).epsilon(0.01));
    // The pitch wobbles: crossing intervals vary.
    double lo = 1e9, hi = 0, last = -1;
    for (std::size_t n = 1; n < y.size(); ++n)
        if (y[n - 1] < 0 && y[n] >= 0) {
            const double t = n - 1 + -y[n - 1] / (y[n] - y[n - 1]);
            if (last >= 0) {
                lo = std::min(lo, t - last);
                hi = std::max(hi, t - last);
            }
            last = t;
        }
    CHECK(hi / lo > 1.01);
}
