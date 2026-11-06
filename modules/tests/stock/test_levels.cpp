// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The catalogue's level gate: every stock module with an audio output, at
// its defaults and in each of its factory patches, on a standard input near
// full scale, must keep every audio output's peak under a ceiling, and an
// effect at its defaults must not go silent.
//
// An effect (a module with a main audio input: in, in_l, in_1 or voice)
// hears the standard input there, and otherwise only what it cannot sound
// without (a vocoder's carrier, a low-pass gate's ping); its other inputs
// sit at their normals (In R follows In L, a sidechain is unpatched). It
// gets no second source on top of its input (a resonator's strike, a
// mixer's other channels): two full-scale sources at once add up, as they
// should. A source hears a gate on its trigger-like inputs (trigger, gate,
// ping, strike, pluck) and plays at its default pitch. Of a factory patch,
// its values are applied, not its routes or files.
//
// The ceiling is +1 dBFS on an input peaking at -0.9: room for a little
// filter ringing or saturation, none for a gain that is simply too high.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
namespace sd = supple::script::detail;

constexpr double kRate = 48000.0;
constexpr std::int64_t kFrames = 3 * 48000;
constexpr double kCeilingDb = 1.0;   // no audio output's peak above this
constexpr double kSilentDb = -60.0;  // an effect whose loudest output is below this is silent

double hash(std::int64_t n) {
    std::uint32_t x = static_cast<std::uint32_t>(n) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return static_cast<double>(x >> 8) / 8388608.0 - 1.0;
}
// A saw's spectrum (every harmonic to 12 kHz at 1/k) with each harmonic at
// its own fixed random phase: as bright as a saw, but without the edge a
// high pass turns into a spike twice the saw's height, which recorded
// sound does not have either.
double tone(std::int64_t n, double hz, int salt) {
    double sum = 0.0;
    for (int k = 1; k * hz < 12000.0; ++k)
        sum += std::sin(2 * kPi * k * hz * static_cast<double>(n) / kRate +
                        kPi * hash(salt * 1000 + k)) /
               k;
    return sum;
}
std::vector<double> scaled(std::vector<double> y, double top) {
    const double g = top / peak(y);
    for (double& v : y) v *= g;
    return y;
}
// A held chord of three such tones, peaking at 0.9.
const std::vector<double>& chord() {
    static const std::vector<double> y = [] {
        std::vector<double> v(kFrames);
        for (std::int64_t n = 0; n < kFrames; ++n)
            v[n] = tone(n, 110.0, 1) + tone(n, 138.59, 2) + tone(n, 164.81, 3);
        return scaled(v, 0.9);
    }();
    return y;
}
// The standard input, peaking at 0.9 (-0.9 dBFS): for 1.5 s a hit every
// quarter second (a 110 Hz tone with a little noise, dying over 80 ms),
// then for 1.5 s the chord.
const std::vector<double>& standardInput() {
    static const std::vector<double> y = [] {
        std::vector<double> hits(kFrames / 2);
        for (std::int64_t n = 0; n < kFrames / 2; ++n) {
            const double t = static_cast<double>(n % 12000) / kRate;
            hits[n] = std::exp(-t / 0.08) * (0.7 * tone(n, 110.0, 4) / 2.0 + 0.3 * hash(n));
        }
        std::vector<double> v = scaled(hits, 0.9);
        const std::vector<double>& held = chord();
        v.insert(v.end(), held.begin() + kFrames / 2, held.end());
        return v;
    }();
    return y;
}
double standard(std::int64_t n) { return n < kFrames ? standardInput()[n] : 0.0; }
// The gate a trigger-like input hears: high 0.2 s of every half second.
double gate(std::int64_t n) { return n % 24000 < 9600 ? 1.0 : 0.0; }

bool isMainInput(const std::string& key) {
    return key == "in" || key == "in_l" || key == "in_1" || key == "voice";
}
bool isTriggerInput(const std::string& key) {
    return key == "trigger" || key == "gate" || key == "ping" || key == "strike" || key == "pluck";
}

bool hasAudioOut(const sd::ModuleCode& code) {
    for (const sd::PortModel& m : code.ports())
        if (m.output && m.signal == sd::PortModel::Signal::Audio) return true;
    return false;
}
bool hasMainInput(const sd::ModuleCode& code) {
    for (const sd::PortModel& m : code.ports())
        if (!m.output && m.signal == sd::PortModel::Signal::Audio && isMainInput(m.key))
            return true;
    return false;
}
struct Loudest {
    double db = -400.0;
    std::string where;
};

// Runs the module with one factory patch's values (none: its defaults) and
// reports its loudest audio output.
Loudest run(const sd::ModuleCode& code, const sd::FactoryPatchModel* patch) {
    ModuleHarness h(code, kRate);
    if (patch) {
        for (const auto& [id, value] : patch->values)
            for (const sd::ParamModel& p : code.params())
                if (p.id == id) h.set(p.key, value);
    }
    const bool effect = hasMainInput(code);
    for (const sd::PortModel& m : code.ports()) {
        if (m.output) continue;
        if (effect && m.signal == sd::PortModel::Signal::Audio && isMainInput(m.key))
            h.input(m.key, 1, [](int, std::int64_t n) { return standard(n); });
        // What an effect needs beside its input to sound at all: the
        // vocoder's voice something to speak through, a low-pass gate (shut
        // while nothing opens it) its ping.
        if (effect && m.key == "carrier")
            h.input(m.key, 1, [](int, std::int64_t n) { return n < kFrames ? chord()[n] : 0.0; });
        if (effect && m.key == "ping")
            h.input(m.key, 1, [](int, std::int64_t n) { return gate(n); });
        if (!effect && m.signal == sd::PortModel::Signal::Gate && isTriggerInput(m.key))
            h.input(m.key, 1, [](int, std::int64_t n) { return gate(n); });
    }
    h.run(kFrames);
    Loudest most;
    for (const sd::PortModel& m : code.ports()) {
        if (!m.output || m.signal != sd::PortModel::Signal::Audio) continue;
        for (int c = 0; c < ModuleHarness::kChannels; ++c) {
            const double p = peak(h.output(m.key, c));
            if (p <= 0.0) continue;
            const double db = 20.0 * std::log10(p);
            if (db > most.db) {
                most.db = db;
                most.where = m.key + "[" + std::to_string(c) + "]";
            }
        }
    }
    return most;
}

}  // namespace

TEST_CASE("stock levels: every audio output under the ceiling, no effect silent at its defaults") {
    int modules = 0, runs = 0;
    for (const supple::rack::StockModule* stockModule : supple::rack::stockModules()) {
        const sd::ModuleCode& code = *stockModule->code;
        if (!hasAudioOut(code)) continue;
        ++modules;
        const std::string name(stockModule->name);
        CAPTURE(name);
        const Loudest base = run(code, nullptr);
        ++runs;
        {
            const std::string patch = "(defaults)";
            CAPTURE(patch);
            CAPTURE(base.where);
            CHECK(base.db <= kCeilingDb);
            if (hasMainInput(code)) CHECK(base.db > kSilentDb);
        }
        for (const sd::FactoryPatchModel& p : code.factoryPatches()) {
            const Loudest most = run(code, &p);
            ++runs;
            const std::string patch = p.name;
            CAPTURE(patch);
            CAPTURE(most.where);
            CHECK(most.db <= kCeilingDb);
        }
    }
    CHECK(modules > 30);
    CHECK(runs > 80);
}
