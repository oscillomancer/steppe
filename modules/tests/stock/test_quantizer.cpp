// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

// Semitones in, one per 32-frame block; the notes out.
std::vector<double> quantize(ModuleHarness& h, const std::vector<double>& semis) {
    h.input("pitch", 1, [semis](int, std::int64_t n) {
        return semis[static_cast<std::size_t>(n / 32) % semis.size()] / 12.0;
    });
    h.run(static_cast<std::int64_t>(semis.size()) * 32);
    std::vector<double> out;
    for (std::size_t k = 0; k < semis.size(); ++k)
        out.push_back(std::round(h.output("out")[k * 32 + 16] * 12.0 * 1000.0) / 1000.0);
    return out;
}

}  // namespace

TEST_CASE("quantizer: the nearest note of the scale, any octave") {
    ModuleHarness h(supple::rack::test::stock("quantizer"));  // C major
    CHECK(quantize(h, {0, 0.4, 0.6, 1.2, 5.9, 6.4, -1.2, 13, 25.1}) ==
          std::vector<double>{0, 0, 0, 2, 5, 7, -1, 12, 26});
}

TEST_CASE("quantizer: root, rounding and degrees") {
    ModuleHarness h(supple::rack::test::stock("quantizer"));
    h.set("scale", 11);  // minor pentatonic
    h.set("root", 9);    // A: A C D E G
    CHECK(quantize(h, {0, 1, 3, 6, 8, 10}) == std::vector<double>{0, 0, 2, 7, 7, 9});

    ModuleHarness d(supple::rack::test::stock("quantizer"));
    d.set("rounding", 1);  // down
    CHECK(quantize(d, {1.9, 3.5, 11.9}) == std::vector<double>{0, 2, 11});
    ModuleHarness u(supple::rack::test::stock("quantizer"));
    u.set("rounding", 2);  // up
    CHECK(quantize(u, {0.1, 2.0, 5.5}) == std::vector<double>{2, 2, 7});

    ModuleHarness g(supple::rack::test::stock("quantizer"));
    g.set("degrees", 2);  // a third up in C major
    CHECK(quantize(g, {0, 2, 9, 11}) == std::vector<double>{4, 5, 12, 14});
}

TEST_CASE("quantizer: a custom scale, transpose, poly and the change trigger") {
    ModuleHarness h(supple::rack::test::stock("quantizer"));
    h.set("scale", 22);  // custom: C D E G A by default
    h.set("custom_g", 0);
    h.set("transpose", 12);
    CHECK(quantize(h, {7, 8, 4.4}) == std::vector<double>{21, 21, 16});

    ModuleHarness p(supple::rack::test::stock("quantizer"));
    p.input("pitch", 3, [](int c, std::int64_t n) { return (n < 480 ? 1.0 + c : 3.2 + c) / 12.0; });
    p.run(960);
    CHECK(p.channels("out") == 3);
    CHECK(p.output("out", 2)[100] * 12.0 == doctest::Approx(2.0));
    CHECK(p.output("out", 2)[600] * 12.0 == doctest::Approx(5.0));
    CHECK(p.output("changed", 1)[100] == 0.0);
    CHECK(p.output("changed", 1)[485] == 1.0);  // D to E
    CHECK(p.output("changed", 0)[485] == 1.0);  // C (C# ties down) to E
}

TEST_CASE("quantizer: a clock samples and holds") {
    ModuleHarness h(supple::rack::test::stock("quantizer"));
    h.set("scale", 0);
    h.input("pitch", 1,
            [](int, std::int64_t n) { return n / 1200.0 / 12.0; });  // a semitone / 1200
    h.input("clock", 1, [](int, std::int64_t n) { return n % 4800 < 48 ? 1.0 : 0.0; });
    h.run(9600);
    CHECK(h.output("out")[4000] == 0.0);
    CHECK(h.output("out")[4900] * 12.0 == doctest::Approx(4.0));
    CHECK(h.output("out")[9000] * 12.0 == doctest::Approx(4.0));
}
