// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// What every stock module must be (ADR-0062, modules/AGENTS.md): described
// for agents, keyed uniquely, finite and deterministic on any input.
#include <doctest/doctest.h>

#include <cctype>
#include <cmath>
#include <ostream>  // doctest prints string_view through it (MSVC STL)
#include <set>
#include <string>

#include "module_harness.h"
#include "supple/rack/stock.h"

using supple::rack::StockModule;
using supple::rack::stockModules;
using supple::rack::test::ModuleHarness;
namespace sd = supple::script::detail;

namespace {

bool lowerKey(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(std::islower(static_cast<unsigned char>(c)) ||
              std::isdigit(static_cast<unsigned char>(c)) || c == '-'))
            return false;
    return true;
}

bool version(const std::string& v) {
    int dots = 0;
    bool digit = false;
    for (char c : v) {
        if (c == '.') {
            if (!digit) return false;
            ++dots;
            digit = false;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digit;
}

// A deterministic test signal: a different mix of tones and noise per input.
double probe(int input, int c, std::int64_t n) {
    std::uint32_t h = static_cast<std::uint32_t>(n * 2654435761u + input * 40503u + c * 977u);
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const double noise = static_cast<double>(h & 0xffff) / 32768.0 - 1.0;
    const double tone = std::sin(static_cast<double>(n) * (0.013 + 0.007 * input + 0.001 * c));
    return 0.5 * tone + 0.3 * noise + ((n / 2400) % 2 ? 0.2 : -0.2);
}

void drive(ModuleHarness& h, const sd::ModuleCode& code, int channels) {
    int k = 0;
    for (const sd::PortModel& p : code.ports())
        if (!p.output) {
            const int input = k++;
            h.input(p.key, channels, [input](int c, std::int64_t n) { return probe(input, c, n); });
        }
}

}  // namespace

TEST_CASE("stock modules are described for agents") {
    REQUIRE_FALSE(stockModules().empty());
    for (const StockModule* m : stockModules()) {
        CAPTURE(m->name);
        const sd::ModuleCode& code = *m->code;
        const sd::ModuleInfoModel& info = code.info();
        CHECK(info.id == "com.suppleaudio.rack." + std::string(m->name));
        CHECK_FALSE(info.name.empty());
        CHECK(info.vendor == "Supple Audio");
        CHECK(version(info.version));
        CHECK_FALSE(info.description.empty());
        CHECK_FALSE(info.tags.empty());
        for (const std::string& t : info.tags) CHECK(lowerKey(t));
        std::set<std::string> keys;
        int inputs = 0, outputs = 0;
        for (const sd::ParamModel& p : code.params()) {
            CAPTURE(p.key);
            CHECK(keys.insert(p.key).second);
            CHECK_FALSE(p.description.empty());
        }
        for (const sd::PortModel& p : code.ports()) {
            CAPTURE(p.key);
            CHECK(keys.insert(p.key).second);
            CHECK_FALSE(p.description.empty());
            (p.output ? outputs : inputs)++;
        }
        CHECK(inputs <= 32);
        CHECK(outputs <= 32);
        CHECK(outputs >= 1);
        CHECK(m->source.find("struct ") != std::string_view::npos);
    }
}

TEST_CASE("stock modules stay finite and within budget on any input") {
    for (const StockModule* m : stockModules()) {
        for (int channels : {1, 4}) {
            CAPTURE(m->name);
            CAPTURE(channels);
            ModuleHarness h(*m->code);
            drive(h, *m->code, channels);
            ModuleHarness::Transport t;
            t.playing = true;
            t.tempo = 128;
            h.transport(t);
            h.run(48000);
            CHECK(h.status().budgetExceeded.load() == 0);
            CHECK(h.status().outOfRange.load() == 0);
            for (const sd::PortModel& p : m->code->ports())
                if (p.output) {
                    CAPTURE(p.key);
                    bool finite = true;
                    double peak = 0;
                    for (int c = 0; c < h.channels(p.key); ++c)
                        for (double x : h.output(p.key, c)) {
                            finite = finite && std::isfinite(x);
                            peak = std::max(peak, std::abs(x));
                        }
                    CHECK(finite);
                    CHECK(peak < 100.0);  // nothing runs away
                }
        }
    }
}

TEST_CASE("stock modules render the same twice") {
    for (const StockModule* m : stockModules()) {
        CAPTURE(m->name);
        ModuleHarness a(*m->code), b(*m->code);
        drive(a, *m->code, 2);
        drive(b, *m->code, 2);
        a.run(9600);
        b.run(9600);
        for (const sd::PortModel& p : m->code->ports())
            if (p.output)
                for (int c = 0; c < 2; ++c) CHECK(a.output(p.key, c) == b.output(p.key, c));
    }
}
