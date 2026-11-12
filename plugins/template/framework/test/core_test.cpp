// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The framework's SDK-free guarantees, checked against whatever product it
// is built with: parameter text and tapers, the DSP building blocks, the
// engine's block, precision and bypass contracts, state, and the editor-to-
// host edit bridge.
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <map>
#include <random>
#include <thread>

#include "Check.h"
#include "Testing.h"
#include "core/Files.h"
#include "core/Preset.h"
#include "core/PresetLibrary.h"
#include "core/StateFormat.h"
#include "dsp/AudioFile.h"
#include "plugin/EditBridge.h"
#include "ProductInfo.h"

#define main renderCliMain
#include "../tools/render_cli.cpp"
#undef main

using namespace supple;

namespace {

std::string text(const ParamDef& p, double host) {
    char buffer[64];
    formatValue(p, host, buffer, sizeof(buffer));
    return buffer;
}

double parsed(const ParamDef& p, const char* s, bool* ok = nullptr) {
    double v = -12345.0;
    const bool good = parseValue(p, s, v);
    if (ok) *ok = good;
    return v;
}

}  // namespace

TEST(the_renderer_rejects_restart_parameter_automation) {
    if (!product::info::kAudioOutput) return;
    for (size_t i = 0; i < params::kCount; ++i) {
        const auto& p = params::at(i);
        if (!p.is(pflag::kRestart)) continue;
        std::string assignment = std::string(p.name) + "=" + text(p, hostDefault(p));
        char program[] = "render-cli", input[] = "nonexistent-input.wav", output[] = "unused.wav";
        char at[] = "--at", zero[] = "0";
        char* argv[] = {program, input, output, at, zero, assignment.data()};
        CHECK(renderCliMain(6, argv) == 2);
    }
}

// --- parameter text -------------------------------------------------------------

TEST(signal_differences_never_hide_nonfinite_samples) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (double bad : {nan, double(INFINITY), -double(INFINITY)}) {
        CHECK(std::isinf(test::maxDifference({{0.0}}, {{bad}})));
        CHECK(std::isinf(test::maxDifference({{bad}}, {{0.0}})));
        CHECK(std::isinf(test::maxDifference({{bad}}, {{bad}})));
    }
}

TEST(nonautomatable_modifiers_clear_every_modulation_scope) {
    const auto poly = percent(1, "Level", 50).polyModulatable();
    for (const auto& p : {poly.notAutomatable(), poly.retired(), poly.output()})
        CHECK(!p.is(pflag::kAutomatable | pflag::kModulatable | pflag::kPolyMod));
}

TEST(nan_or_infinite_parameter_numbers_do_not_build) {
    // What the product table's static_assert runs, on tables of one.
    constexpr double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    constexpr ParamDef good = decibels(0, "Gain", -24.0, 24.0, 0.0);
    constexpr auto table = [](ParamDef p) { return std::array<ParamDef, 1>{p}; };
    constexpr auto with = [](ParamDef p, double ParamDef::*field, double v) {
        p.*field = v;
        return p;
    };
    static_assert(params::detail::rangesValid(table(good)));
    for (double ParamDef::*field : {&ParamDef::min, &ParamDef::max, &ParamDef::def, &ParamDef::curve, &ParamDef::smoothing})
        for (double bad : {nan, inf, -inf}) CHECK(!params::detail::rangesValid(table(with(good, field, bad))));
    static_assert(!params::detail::rangesValid(table(with(good, &ParamDef::def, nan))));
    static_assert(!params::detail::rangesValid(table(with(good, &ParamDef::max, inf))));
}

TEST(frequency_text_switches_units_where_the_rounded_value_does) {
    const ParamDef f = frequency(0, "F", 20.0, 20000.0, 1000.0);
    CHECK(text(f, toHost(f, 1000.0)) == "1.00 kHz");
    CHECK(text(f, toHost(f, 999.6)) == "1.00 kHz");  // rounds across the threshold
    CHECK(text(f, toHost(f, 99.96)) == "100 Hz");
    CHECK(text(f, toHost(f, 45.25)) == "45.3 Hz");
    CHECK(text(f, toHost(f, 12500.0)) == "12.5 kHz");
    CHECK(text(f, toHost(f, 9996.0)) == "10.0 kHz");
}

TEST(text_parses_the_way_people_type) {
    const ParamDef f = frequency(0, "F", 20.0, 20000.0, 1000.0);
    CHECK_NEAR(toPlain(f, parsed(f, "1.2k")), 1200.0, 1e-9);
    CHECK_NEAR(toPlain(f, parsed(f, "1200 hz")), 1200.0, 1e-9);
    CHECK_NEAR(toPlain(f, parsed(f, " 1.2 kHz ")), 1200.0, 1e-9);
    const ParamDef mix = percent(1, "Mix", 100.0);
    CHECK_NEAR(parsed(mix, "50"), 0.5, 1e-12);
    CHECK_NEAR(parsed(mix, "50 %"), 0.5, 1e-12);
    const ParamDef gain = decibels(2, "Gain", -24.0, 24.0, 0.0);
    CHECK_NEAR(parsed(gain, "-6db"), -6.0, 1e-12);
    CHECK_NEAR(parsed(gain, "\xE2\x88\x92" "6 dB"), -6.0, 1e-12);  // the editor's minus sign
    CHECK_NEAR(parsed(gain, "99"), 24.0, 1e-12);                     // clamped, not rejected
    const ParamDef time = milliseconds(3, "Time", 0.1, 5000.0, 10.0);
    CHECK_NEAR(toPlain(time, parsed(time, "0.25 s")), 250.0, 1e-9);
    CHECK_NEAR(toPlain(time, parsed(time, "250ms")), 250.0, 1e-9);
    CHECK(text(time, toHost(time, 1500.0)) == "1.50 s");
}

TEST(text_rejects_garbage_and_non_finite_input) {
    const ParamDef gain = decibels(0, "Gain", -24.0, 24.0, 0.0);
    for (const char* bad : {"", " ", "garbage", "nan", "-nan", "inf", "-inf", "infinity", "1e999", "-1e999",
                            "nan dB", "12 apples", "1.2k"}) {
        bool ok = true;
        parsed(gain, bad, &ok);
        CHECK_MSG(!ok, "accepted \"%s\"", bad);
    }
}

TEST(choices_and_toggles_read_their_labels) {
    static constexpr const char* kModes[] = {"Clean", "Warm", "Hard"};
    const ParamDef mode = choice(0, "Mode", kModes, 1);
    CHECK(text(mode, 2.0) == "Hard");
    CHECK(parsed(mode, "warm") == 1.0);
    CHECK(parsed(mode, " HARD ") == 2.0);
    CHECK(parsed(mode, "0") == 0.0);
    bool ok = true;
    parsed(mode, "7", &ok);
    CHECK(!ok);
    const ParamDef on = toggle(1, "On", false);
    CHECK(text(on, 1.0) == "On");
    CHECK(parsed(on, "off") == 0.0);
    CHECK(parsed(on, "yes") == 1.0);
}

TEST(no_negative_zero_and_sign_only_when_bipolar) {
    const ParamDef gain = decibels(0, "Gain", -24.0, 24.0, 0.0);
    CHECK(text(gain, -0.01) == "0.0 dB");
    CHECK(text(gain, 3.0) == "+3.0 dB");
    const ParamDef threshold = decibels(1, "Threshold", -60.0, 0.0, -20.0);
    CHECK(text(threshold, -20.0) == "-20.0 dB");
}

TEST(text_ignores_the_process_locale) {
    // A host may run with a locale whose decimal separator is a comma.
    const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
    const char* applied = nullptr;
    for (const char* l : locales)
        if ((applied = std::setlocale(LC_NUMERIC, l))) break;
    const ParamDef gain = decibels(0, "Gain", -24.0, 24.0, 0.0);
    CHECK(text(gain, 1.5) == "+1.5 dB");
    CHECK_NEAR(parsed(gain, "1.5"), 1.5, 1e-12);
    std::setlocale(LC_NUMERIC, "C");
    if (!applied) std::printf("      (no comma locale installed; checked under C only)\n");
}

TEST(a_level_off_at_its_minimum_reads_minus_infinity) {
    const ParamDef send = decibels(0, "Send", -60.0, 12.0, 0.0).offAtMinimum();
    CHECK(send.off(-60.0) && !send.off(-59.99));
    CHECK(text(send, -60.0) == "-inf dB");
    CHECK(text(send, -59.9) == "-59.9 dB");  // just above it is on, and says so
    for (const char* typed : {"-inf", "-INF dB", " -inf db ", "-infinity", "off", "\xE2\x88\x92\xE2\x88\x9E",
                              "\xE2\x88\x92\xE2\x88\x9E dB", "-60"}) {
        bool ok = false;
        CHECK_MSG(parsed(send, typed, &ok) == -60.0 && ok, "\"%s\"", typed);
    }
    for (const char* bad : {"inf", "+inf", "infinity", "-inf Hz", "-in"}) {
        bool ok = true;
        parsed(send, bad, &ok);
        CHECK_MSG(!ok, "accepted \"%s\"", bad);
    }
    // Anything else reads "Off" there, in no unit.
    const ParamDef cut = frequency(1, "Low Cut", 20.0, 2000.0, 20.0).offAtMinimum();
    CHECK(text(cut, toHost(cut, 20.0)) == "Off");
    CHECK(text(cut, toHost(cut, 21.0)) == "21.0 Hz");
    CHECK(parsed(cut, "OFF") == 0.0);  // the taper's start
    bool ok = true;
    parsed(cut, "-inf", &ok);
    CHECK(!ok);
}

TEST(each_unit_writes_and_reads_its_own_text) {
    const ParamDef phase = degrees(0, "Phase", 0.0, 180.0, 90.0);
    CHECK(text(phase, 90.0) == "90 deg");
    CHECK(text(phase, 2.5) == "2.5 deg");
    CHECK(parsed(phase, "45\xC2\xB0") == 45.0 && parsed(phase, "45 degrees") == 45.0 && parsed(phase, "45") == 45.0);
    CHECK(text(degrees(1, "Rotate", -180.0, 180.0, 0.0), 45.0) == "+45 deg");
    const ParamDef offset = samples(2, "Offset", 0, 4096, 64);
    CHECK(text(offset, 64.0) == "64 smp");
    CHECK(parsed(offset, "65 samples") == 65.0 && parsed(offset, "65.4") == 65.0);
    CHECK(text(octaves(3, "Width", 0.1, 4.0, 1.0), 1.0) == "1.00 oct");
    const ParamDef tempo = bpm(4, "Tempo", 20.0, 300.0, 120.0);
    CHECK(text(tempo, 120.0) == "120.0 BPM");
    CHECK(parsed(tempo, "98.5 bpm") == 98.5);
    CHECK(text(cents(5, "Fine", -100.0, 100.0, 0.0), -12.0) == "-12 ct");
    const ParamDef across = meters(9, "Width", 1.0, 200.0, 18.0);
    CHECK(text(across, 18.0) == "18.0 m" && text(across, 2.44) == "2.4 m" && text(across, 120.0) == "120 m");
    CHECK(parsed(across, "12 m") == 12.0 && parsed(across, "12.5 metres") == 12.5 && parsed(across, "7") == 7.0);
    CHECK(parsed(across, "250 cm") == 2.5 && std::fabs(parsed(across, "10 ft") - 3.048) < 1e-12);

    const ParamDef balance = pan(6, "Pan");
    CHECK(text(balance, 0.0) == "C" && text(balance, -0.5) == "50L" && text(balance, 0.25) == "25R");
    CHECK(text(balance, 0.004) == "C");  // what rounds to the centre is the centre
    CHECK(parsed(balance, "L30") == -0.3 && parsed(balance, "30 r") == 0.3 && parsed(balance, "-30") == -0.3);
    CHECK(parsed(balance, "centre") == 0.0 && parsed(balance, "c") == 0.0 && parsed(balance, "100%L") == -1.0);
    for (const char* bad : {"30X", "-30L", "L", "LR30", ""}) {
        bool ok = true;
        parsed(balance, bad, &ok);
        CHECK_MSG(!ok, "pan accepted \"%s\"", bad);
    }

    const ParamDef key = midiNote(7, "Key");
    CHECK(text(key, 60.0) == "C4" && text(key, 61.0) == "C#4" && text(key, 69.0) == "A4");
    CHECK(text(key, 0.0) == "C-1" && text(key, 127.0) == "G9");
    CHECK(parsed(key, "c#4") == 61.0 && parsed(key, "Db4") == 61.0 && parsed(key, " a4 ") == 69.0);
    CHECK(parsed(key, "B-1") == 11.0 && parsed(key, "bb3") == 58.0 && parsed(key, "60") == 60.0);
    for (const char* bad : {"H4", "C", "C#", "4C", "C4x"}) {
        bool ok = true;
        parsed(key, bad, &ok);
        CHECK_MSG(!ok, "note accepted \"%s\"", bad);
    }

    const ParamDef division = noteValue(8, "Time", "1/8.");
    CHECK(text(division, division.def) == "1/8.");
    CHECK(noteValueBeats(static_cast<int>(division.def)) == 0.75);
    CHECK(parsed(division, "1/4T") == static_cast<double>(detail::noteValueIndex("1/4T")));
    CHECK(noteValueBeats(detail::noteValueIndex("1/4")) == 1.0 && noteValueBeats(detail::noteValueIndex("4/1")) == 16.0);
    for (size_t i = 1; i < std::size(kNoteValueBeats); ++i) CHECK(kNoteValueBeats[i] > kNoteValueBeats[i - 1]);
}

namespace {
// A width shown as "Mono", "Stereo" or a percentage between.
void widthText(const ParamDef&, double plain, ValueText& out) {
    if (plain <= 0.0) std::snprintf(out.number, sizeof(out.number), "Mono");
    else if (plain == 1.0) std::snprintf(out.number, sizeof(out.number), "Stereo");
    else std::snprintf(out.number, sizeof(out.number), "%.0f%%", plain * 100.0);
    out.unit[0] = '\0';
}
bool widthParse(const ParamDef&, const char* text, double& plain) {
    if (!std::strcmp(text, "Mono") || !std::strcmp(text, "mono")) return plain = 0.0, true;
    if (!std::strcmp(text, "Stereo") || !std::strcmp(text, "stereo")) return plain = 1.0, true;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || (*end && std::strcmp(end, "%"))) return false;
    plain = v * 0.01;
    return true;
}
}  // namespace

TEST(a_custom_format_owns_the_text_both_ways) {
    const ParamDef width = continuous(0, "Width", 0.0, 2.0, 1.0).formatted(widthText, widthParse);
    CHECK(text(width, 0.0) == "Mono" && text(width, 1.0) == "Stereo" && text(width, 1.5) == "150%");
    CHECK(parsed(width, "stereo") == 1.0 && parsed(width, "150%") == 1.5);
    CHECK(parsed(width, "900") == 2.0);  // the framework clamps what the parser returns
    bool ok = true;
    parsed(width, "wide", &ok);
    CHECK(!ok);
    // Off at the minimum is the framework's, ahead of the format.
    const ParamDef offWidth = width.offAtMinimum();
    CHECK(text(offWidth, 0.0) == "Off" && parsed(offWidth, "off") == 0.0);
}

TEST(every_product_parameter_round_trips_its_own_text) {
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        const int steps = p.stepped() ? static_cast<int>(p.hostMax() - p.hostMin()) : 40;
        for (int k = 0; k <= steps; ++k) {
            const double host = p.stepped() ? p.hostMin() + k : fromNormalized(p, k / 40.0);
            const std::string shown = text(p, host);
            bool ok = false;
            const double back = parsed(p, shown.c_str(), &ok);
            CHECK_MSG(ok, "%s: \"%s\" does not parse", p.name, shown.c_str());
            CHECK_MSG(text(p, back) == shown, "%s: \"%s\" reads back as \"%s\"", p.name, shown.c_str(),
                      text(p, back).c_str());
            if (p.stepped()) CHECK_MSG(back == host, "%s: step %g reads back as %g", p.name, host, back);
        }
        const std::string def = text(p, hostDefault(p));
        CHECK_MSG(!def.empty(), "%s has no default text", p.name);
    }
}

TEST(tapers_are_monotonic_and_invert) {
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        double previous = -INFINITY;
        for (int k = 0; k <= 100; ++k) {
            const double host = fromNormalized(p, k / 100.0);
            const double plain = toPlain(p, host);
            CHECK_MSG(plain >= previous, "%s: taper not monotonic at %d", p.name, k);
            CHECK_MSG(plain >= p.min && plain <= p.max, "%s: %g outside its range", p.name, plain);
            CHECK_NEAR(toHost(p, plain), host, 1e-9);
            previous = plain;
        }
        CHECK_NEAR(toPlain(p, hostDefault(p)), p.def, 1e-9 * std::max(1.0, std::fabs(p.def)));
    }
}

TEST(plain_host_value_keeps_the_knob_on_its_taper) {
    // The same frequency parameter, with the host storing position or Hz.
    const ParamDef position = frequency(0, "F", 20.0, 20000.0, 1000.0);
    const ParamDef plainHost = position.plainHostValue();
    CHECK(plainHost.hostMin() == 20.0 && plainHost.hostMax() == 20000.0);
    CHECK(plainHost.hostTapered() == false && plainHost.tapered());
    CHECK(hostDefault(plainHost) == 1000.0);
    CHECK(toPlain(plainHost, 440.0) == 440.0 && toHost(plainHost, 440.0) == 440.0);
    CHECK(sanitizeHost(plainHost, 1e9) == 20000.0);
    // The knob is where it would be with a position-valued host.
    for (int k = 0; k <= 10; ++k) {
        const double n = k / 10.0;
        CHECK_NEAR(toPlain(plainHost, fromNormalized(plainHost, n)), toPlain(position, fromNormalized(position, n)),
                   1e-9 * 20000.0);
        CHECK_NEAR(toNormalized(plainHost, fromNormalized(plainHost, n)), n, 1e-12);
    }
    CHECK_NEAR(toNormalized(plainHost, 632.455532), 0.5, 1e-6);  // the geometric middle
    // Text is unchanged: it is formatted from the plain value either way.
    CHECK(text(plainHost, 440.0) == text(position, toHost(position, 440.0)));
    bool ok = false;
    CHECK_NEAR(parsed(plainHost, "1.2k", &ok), 1200.0, 1e-9);
    CHECK(ok);
    // A power taper too.
    const ParamDef square = continuous(1, "S", 0.0, 1.0, 0.02, Unit::None, Taper::Power, 2.0).plainHostValue();
    CHECK_NEAR(toNormalized(square, 0.25), 0.5, 1e-12);
    CHECK_NEAR(fromNormalized(square, 0.5), 0.25, 1e-12);
}

// --- DSP building blocks ------------------------------------------------------------

TEST(normalized_host_value_reaches_hosts_as_a_position) {
    // A linear parameter whose host value is its 0..1 position, as hosts
    // that keep normalized values expect; the plain value is unchanged.
    const ParamDef plain = decibels(0, "Gain", -24.0, 12.0, 0.0);
    const ParamDef normal = plain.normalizedHostValue();
    CHECK(normal.hostTapered() && !plain.hostTapered());
    CHECK(normal.hostMin() == 0.0 && normal.hostMax() == 1.0);
    CHECK(toHost(normal, -24.0) == 0.0 && toHost(normal, 12.0) == 1.0);
    CHECK_NEAR(hostDefault(normal), 24.0 / 36.0, 1e-15);
    for (int k = 0; k <= 36; ++k) {
        const double db = -24.0 + k;
        CHECK_NEAR(toPlain(normal, toHost(normal, db)), db, 1e-12);
        CHECK(toNormalized(normal, toHost(normal, db)) == toNormalized(plain, toHost(plain, db)));
    }
    CHECK(text(normal, toHost(normal, -6.0)) == text(plain, -6.0));
    // A tapered parameter is a position already; the flag changes nothing.
    const ParamDef f = frequency(1, "F", 20.0, 20000.0, 1000.0);
    CHECK(toHost(f.normalizedHostValue(), 440.0) == toHost(f, 440.0));
    // Not for stepped parameters, nor with plainHostValue().
    ParamTable::Builder b;
    b.add(integer(2, "Steps", 1, 8, 4).normalizedHostValue());
    b.add(f.plainHostValue().normalizedHostValue());
    const Definition bad(productDefinition()->info(), b.build());
    CHECK(bad.problems().find("'Steps': normalizedHostValue()") != std::string::npos);
    CHECK(bad.problems().find("'F': normalizedHostValue()") != std::string::npos);
}

TEST(smoother_settles_exactly) {
    dsp::Smoother s;
    s.setTime(10.0, 48000.0);
    s.reset(0.0);
    s.set(1.0);
    int n = 0;
    while (!s.settled() && n < 48000) {
        s.next();
        ++n;
    }
    CHECK(s.settled());
    CHECK(s.value() == 1.0);
    CHECK(n > 480 && n < 20000);
}

TEST(ramp_lasts_exactly_its_length) {
    dsp::Ramp r;
    r.setTime(1.0, 48000.0);  // 48 samples
    r.set(0.0, true);
    r.set(1.0);
    for (int i = 0; i < 47; ++i) CHECK(r.next() < 1.0);
    CHECK(r.next() == 1.0);
    CHECK(r.settled());
}

TEST(delay_line_delays_by_exactly_the_requested_samples) {
    dsp::DelayLine d;
    d.prepare(100);
    std::vector<double> out;
    for (int i = 0; i < 200; ++i) out.push_back(d.process(i + 1.0, 37));
    for (int i = 0; i < 37; ++i) CHECK(out[static_cast<size_t>(i)] == 0.0);
    for (int i = 37; i < 200; ++i) CHECK(out[static_cast<size_t>(i)] == i - 37 + 1.0);
}

TEST(dry_wet_aligns_the_dry_path_with_a_latent_wet_path) {
    const ProcessSetup setup{48000.0, 64, 1};
    dsp::DryWet mix;
    mix.prepare(setup, 10);
    mix.setLatency(10);
    mix.setMix(0.5, true);
    // A wet path that is the input delayed by the same 10 samples must blend
    // with the delayed dry into exactly that delayed signal: no comb filter.
    dsp::DelayLine wet;
    wet.prepare(10);
    std::vector<double> buffer(64);
    double* data[] = {buffer.data()};
    double worst = 0.0;
    for (int block = 0, n = 0; block < 8; ++block) {
        std::vector<double> input(64);
        for (int i = 0; i < 64; ++i) input[static_cast<size_t>(i)] = std::sin(0.05 * (n + i));
        buffer = input;
        Block b;
        b.data = data;
        b.channels = 1;
        b.frames = 64;
        mix.capture(b);
        for (int i = 0; i < 64; ++i) buffer[static_cast<size_t>(i)] = wet.process(input[static_cast<size_t>(i)], 10);
        mix.apply(b);
        for (int i = 0; i < 64; ++i) {
            const int t = n + i - 10;
            const double expected = t >= 0 ? std::sin(0.05 * t) : 0.0;
            worst = std::max(worst, std::fabs(buffer[static_cast<size_t>(i)] - expected));
        }
        n += 64;
    }
    CHECK_NEAR(worst, 0.0, 1e-12);
}

TEST(svf_response_matches_what_it_does_to_a_sine) {
    const double sr = 48000.0;
    for (double fc : {100.0, 1000.0, 15000.0}) {
        dsp::Svf f;
        f.setCoefficients(fc, 0.7071, sr);
        for (double hz : {50.0, 1000.0, 8000.0}) {
            dsp::Svf::State s;
            double sum = 0.0;
            const int n = 48000;
            for (int i = 0; i < n; ++i) {
                const double y = f.tick(s, std::sin(2.0 * dsp::kPi * hz * i / sr)).low;
                if (i >= n / 2) sum += y * y;
            }
            const double measured = std::sqrt(2.0 * sum / (n / 2));
            CHECK_NEAR(measured, std::abs(f.lowpassResponse(hz, sr)), 2e-3);
        }
    }
}

// --- the engine -------------------------------------------------------------------------

namespace {

// Random automation of every continuous, automatable parameter.
std::vector<test::Automation> randomAutomation(int frames, uint32_t seed, int count) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> frame(0, frames - 1);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<test::Automation> a;
    for (int k = 0; k < count; ++k) {
        const ParamDef& p = params::at(static_cast<size_t>(rng() % params::kCount));
        if (!p.is(pflag::kAutomatable) || p.is(pflag::kBypass) || p.is(pflag::kRestart)) continue;
        a.push_back({frame(rng), p.id, toPlain(p, fromNormalized(p, unit(rng)))});
    }
    return a;
}

// Whatever else the product takes, so a render exercises all of it: a
// sidechain that comes and goes, and notes with MIDI, expressions and
// per-note modulation among them.
void everyInput(test::RenderOptions& o, int frames, uint32_t seed) {
    o.frames = frames;
    if (product::info::kSidechain) {
        o.sidechain = test::noise(2, frames, seed);
        for (auto& c : o.sidechain)
            for (size_t i = 0; i < c.size(); ++i)
                if ((i / 3000) % 2) c[i] = 0.0;
    }
    if (!product::info::kNoteInput) return;
    std::mt19937 rng(seed);
    for (int k = 0; k < 40; ++k) {
        const int start = static_cast<int>(rng() % static_cast<uint32_t>(frames)), key = 40 + static_cast<int>(rng() % 40);
        NoteAddress address;
        address.noteId = k;
        o.events.push_back(test::noteOn(start, key, 0.5 + (rng() % 50) / 100.0, k, k % 3));
        o.events.push_back(test::midi(start + 5, MidiMessage::cc(k % 3, 1, static_cast<int>(rng() % 128))));
        o.events.push_back(test::midi(start + 9, MidiMessage::bend(k % 3, ((rng() % 200) - 100) / 100.0)));
        o.events.push_back(test::expression(start + 11, NoteExpression::Type::Tuning, 0.5, address));
        for (size_t i = 0; i < params::kCount; ++i)
            if (params::at(i).is(pflag::kPolyMod)) o.events.push_back(test::noteModulation(start + 13, params::at(i).id, 0.1, address));
        o.events.push_back(test::noteOff(start + 1 + static_cast<int>(rng() % 6000), key, k, k % 3));
    }
}

// Everything a render produced: main output, aux outputs, what was sent.
struct Rendered {
    test::Signal main;
    std::vector<test::Signal> aux;
    std::vector<test::Sent> sent;
};

Rendered renderAll(const test::Signal& in, test::RenderOptions o, Engine* engine = nullptr) {
    Rendered r;
    o.aux = &r.aux;
    o.sent = &r.sent;
    r.main = test::render(in, o, engine);
    return r;
}

// The largest difference anywhere; infinite when what was sent differs.
double difference(const Rendered& x, const Rendered& y) {
    double d = test::maxDifference(x.main, y.main);
    if (x.aux.size() != y.aux.size() || x.sent.size() != y.sent.size()) return INFINITY;
    for (size_t k = 0; k < x.aux.size(); ++k) d = std::max(d, test::maxDifference(x.aux[k], y.aux[k]));
    for (size_t k = 0; k < x.sent.size(); ++k) {
        const test::Sent &a = x.sent[k], &b = y.sent[k];
        if (a.frame != b.frame || a.kind != b.kind || a.note.key != b.note.key || a.note.type != b.note.type ||
            a.note.noteId != b.note.noteId || std::memcmp(a.midi.data, b.midi.data, 3) != 0)
            return INFINITY;
    }
    return d;
}

// A reused instance also retires notes from before reset. They precede the
// fresh render's events at frame zero; all subsequent output must match.
double differenceAfterReset(Rendered reused, const Rendered& fresh) {
    if (reused.sent.size() < fresh.sent.size()) return INFINITY;
    const size_t retired = reused.sent.size() - fresh.sent.size();
    for (size_t i = 0; i < retired; ++i)
        if (reused.sent[i].frame != 0 || reused.sent[i].kind != NoteOutput::Kind::End) return INFINITY;
    reused.sent.erase(reused.sent.begin(), reused.sent.begin() + static_cast<std::ptrdiff_t>(retired));
    return difference(reused, fresh);
}

}  // namespace

TEST(output_is_independent_of_host_block_size_under_automation) {
    const int frames = 24000;
    const test::Signal in = test::noise(2, frames, 7);
    test::RenderOptions a;
    a.automation = randomAutomation(frames, 11, 400);
    a.blocks = {4096};
    everyInput(a, frames, 19);
    test::RenderOptions b = a;
    b.blocks = {1, 7, 64, 513, 2, 1000};
    const Rendered x = renderAll(in, a), y = renderAll(in, b);
    CHECK(test::allFinite(x.main));
    for (const auto& bus : x.aux) CHECK(test::allFinite(bus));
    CHECK_NEAR(difference(x, y), 0.0, 0.0);  // bit-identical, and the same events at the same frames
    if (product::info::kNoteOutput || product::info::kNoteInput)
        std::printf("      %zu events sent, identically\n", x.sent.size());
}

TEST(float_and_double_host_buffers_agree) {
    auto engineF = std::make_unique<Engine>(), engineD = std::make_unique<Engine>();
    engineF->activate(48000.0, 256, product::info::kAudioInput ? 2 : 0, 2);
    engineD->activate(48000.0, 256, product::info::kAudioInput ? 2 : 0, 2);
    std::vector<float> lf(256), rf(256);
    std::vector<double> ld(256), rd(256);
    double worst = 0.0;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(-0.8f, 0.8f);
    for (int block = 0; block < 64; ++block) {
        for (int i = 0; i < 256; ++i) {
            lf[static_cast<size_t>(i)] = d(rng);
            rf[static_cast<size_t>(i)] = d(rng);
            ld[static_cast<size_t>(i)] = lf[static_cast<size_t>(i)];
            rd[static_cast<size_t>(i)] = rf[static_cast<size_t>(i)];
        }
        float* f[] = {lf.data(), rf.data()};
        double* g[] = {ld.data(), rd.data()};
        engineF->process<float>(f, f, 256);
        engineD->process<double>(g, g, 256);
        for (int i = 0; i < 256; ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(lf[static_cast<size_t>(i)]) - ld[static_cast<size_t>(i)]));
    }
    CHECK(worst < 1e-6);
}

TEST(separate_and_in_place_buffers_agree) {
    auto a = std::make_unique<Engine>(), b = std::make_unique<Engine>();
    a->activate(44100.0, 128, product::info::kAudioInput ? 2 : 0, 2);
    b->activate(44100.0, 128, product::info::kAudioInput ? 2 : 0, 2);
    const test::Signal in = test::noise(2, 128 * 20, 5);
    double worst = 0.0;
    for (int block = 0; block < 20; ++block) {
        std::vector<double> i0(in[0].begin() + block * 128, in[0].begin() + (block + 1) * 128);
        std::vector<double> i1(in[1].begin() + block * 128, in[1].begin() + (block + 1) * 128);
        std::vector<double> o0(128, NAN), o1(128, NAN), p0 = i0, p1 = i1;
        const double* inputs[] = {i0.data(), i1.data()};
        double* outputs[] = {o0.data(), o1.data()};
        double* inPlace[] = {p0.data(), p1.data()};
        a->process<double>(inputs, outputs, 128);
        b->process<double>(inPlace, inPlace, 128);
        for (int k = 0; k < 128; ++k)
            worst = std::max({worst, std::fabs(o0[static_cast<size_t>(k)] - p0[static_cast<size_t>(k)]),
                              std::fabs(o1[static_cast<size_t>(k)] - p1[static_cast<size_t>(k)])});
    }
    CHECK(worst == 0.0);
}

TEST(the_measurement_helpers_read_known_signals_exactly) {
    const double sr = 48000.0;
    std::vector<double> x(24000);
    for (size_t n = 0; n < x.size(); ++n) {
        const double t = static_cast<double>(n) / sr;
        x[n] = 0.3 * std::sin(2.0 * dsp::kPi * 997.0 * t + 0.4) + 0.006 * std::sin(2.0 * dsp::kPi * 1994.0 * t) +
               0.003 * std::cos(2.0 * dsp::kPi * 2991.0 * t) + 0.1 * std::sin(2.0 * dsp::kPi * 6100.0 * t);
    }
    CHECK_NEAR(test::toneAmplitude(x, 997.0, sr), 0.3, 1e-6);
    CHECK_NEAR(test::toneAmplitude(x, 6100.0, sr), 0.1, 1e-6);
    CHECK_NEAR(test::toneAmplitude(x, 4000.0, sr), 0.0, 1e-6);
    // 6100 Hz is not a harmonic of 997 Hz, so it is not distortion.
    CHECK_NEAR(test::thd(x, 997.0, sr), std::hypot(0.006, 0.003) / 0.3, 1e-6);
    const test::Signal noise = test::noise(1, 6000, 4);
    std::vector<double> late(noise[0].size(), 0.0);
    for (size_t n = 37; n < late.size(); ++n) late[n] = -0.5 * noise[0][n - 37];
    double strength = 0.0;
    CHECK(test::delayBetween(noise[0], late, 200, &strength) == 37);
    CHECK(strength > 0.99);
}

TEST(the_latency_reported_is_the_latency_measured) {
    if (!product::info::kAudioInput) {
        std::printf("      (no audio input in this product)\n");
        return;
    }
    auto engine = std::make_unique<Engine>();
    const int channels = product::info::kChannelLayouts[0];
    engine->activate(48000.0, 512, channels, channels);
    const int reported = static_cast<int>(engine->latency());
    test::RenderOptions o;
    double strength = 0.0;
    const int measured = test::measureLatency(o, std::max(2048, 2 * reported), &strength);
    if (strength < 0.5) {
        std::printf("      (the output at the defaults does not follow the input clearly: %.2f)\n", strength);
        return;
    }
    CHECK_MSG(measured == reported, "reported %d, measured %d (correlation %.3f)", reported, measured, strength);
}

TEST(bypass_reaches_the_exact_input_and_back) {
    if (params::kBypassIndex < 0) return;
    const uint32_t bypass = params::at(static_cast<size_t>(params::kBypassIndex)).id;
    const int frames = 48000;
    const test::Signal in = test::noise(2, frames, 9);
    test::RenderOptions o;
    o.initial = {{bypass, 1.0}};
    // Whatever else is set, a settled bypass is the input delayed by latency.
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (!p.is(pflag::kBypass) && !p.is(pflag::kRestart))
            o.initial.push_back({p.id, toPlain(p, fromNormalized(p, 0.8))});
    }
    auto engine = std::make_unique<Engine>();
    const test::Signal out = test::render(in, o, engine.get());
    const int latency = engine->latency();
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = latency; i < frames; ++i)
            worst = std::max(worst, std::fabs(out[static_cast<size_t>(c)][static_cast<size_t>(i)] -
                                              in[static_cast<size_t>(c)][static_cast<size_t>(i - latency)]));
    CHECK(worst == 0.0);
}

TEST(telemetry_rate_does_not_follow_event_density) {
    test::RenderOptions o;
    o.metering = true;
    o.blocks = {512};
    const uint32_t id = params::at(0).id;
    for (int f = 0; f < 48000; f += 4) o.automation.push_back({f, id, toPlain(params::at(0), fromNormalized(params::at(0), (f % 400) / 400.0))});
    auto engine = std::make_unique<Engine>();
    test::render(test::noise(2, 48000, 2), o, engine.get());
    int frames = 0;
    TelemetryFrame f;
    while (engine->telemetry().pop(f)) ++frames;
    const int expected = static_cast<int>(1.0 * kTelemetryHz);  // one second of audio
    CHECK_MSG(std::abs(frames - expected) <= 1, "%d frames for 1 s at %g Hz", frames, kTelemetryHz);
}

TEST(non_finite_output_never_reaches_the_host) {
    // Injected through the input, so this needs a product that reads it.
    if (!product::info::kAudioInput) return;
    test::Signal in = test::noise(2, 4096, 4);
    in[0][100] = NAN;
    in[1][2000] = INFINITY;
    auto engine = std::make_unique<Engine>();
    test::RenderOptions o;
    o.blocks = {256};
    const test::Signal out = test::render(in, o, engine.get());
    CHECK(test::allFinite(out));
    // A processor that cleans its own input never trips the safety net; one
    // that passes the NaN on (the template's starter does) must have been
    // caught by it. Either way nothing non-finite left.
    if (engine->safetyResets() == 0) {
        auto bare = std::make_unique<product::Processor>();
        ProcessSetup setup;
        ParamValues values;
        bare->prepare(setup, values);
        bare->update(values);
        std::vector<double> l(in[0].begin(), in[0].begin() + 256), r(in[1].begin(), in[1].begin() + 256);
        double* io[] = {l.data(), r.data()};
        Block block;
        block.data = io;
        block.channels = 2;
        block.frames = 256;
        bare->process(block);
        CHECK_MSG(test::allFinite({l, r}), "the processor emitted non-finite output and no reset caught it");
    }
}

TEST(non_finite_input_never_passes_through_bypass_or_overflows_a_float) {
    if (!product::info::kAudioInput) return;
    // Bypassed, the output is the host's own input: NaN in, silence out.
    if (params::kBypassIndex >= 0) {
        test::Signal in = test::noise(2, 2048, 9);
        in[0][700] = NAN;
        auto engine = std::make_unique<Engine>();
        test::RenderOptions o;
        o.blocks = {128};
        o.initial = {{params::at(static_cast<size_t>(params::kBypassIndex)).id, 1.0}};
        CHECK(test::allFinite(test::render(in, o, engine.get())));
    }
    // A finite double beyond FLT_MAX becomes infinity in a float buffer.
    auto engine = std::make_unique<Engine>();
    engine->activate(48000.0, 256, 2, 2);
    std::vector<float> l(256, 3.0e38f), r(256, -3.0e38f);
    const float* in[] = {l.data(), r.data()};
    std::vector<float> ol(256), orr(256);
    float* out[] = {ol.data(), orr.data()};
    struct All {
        size_t k = 0;
        bool next(EngineEvent& e) {
            while (k < params::kCount && params::at(k).is(pflag::kBypass)) ++k;
            if (k == params::kCount) return false;
            e = EngineEvent{};
            e.index = static_cast<int>(k);
            e.value = fromNormalized(params::at(k), 1.0);
            ++k;
            return true;
        }
    } loud;
    engine->process<float>(in, out, 256, loud, nullptr);
    for (int i = 0; i < 256; ++i) CHECK(std::isfinite(ol[static_cast<size_t>(i)]) && std::isfinite(orr[static_cast<size_t>(i)]));
}

namespace {

// Records what each split of a host block is told about the transport.
class TransportProbe : public ProcessorBase<TransportProbe> {
  public:
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues&) {}
    void process(Block& b) {
        if (count < 16) seen[count++] = {b.frames, b.transport ? b.transport->beats : -1.0};
    }
    struct Split {
        int frames;
        double beats;
    } seen[16]{};
    int count = 0;
};

}  // namespace

TEST(reset_note_ends_survive_until_the_next_audio_block) {
    class ResetProbe : public ProcessorBase<ResetProbe> {
      public:
        void prepare(const ProcessSetup&, const ParamValues&) {}
        void update(const ParamValues&) {}
        void process(Block&) {}
        void reset() { noteEnded(NoteEvent{}); }
    };
    auto e = std::make_unique<BasicEngine<ResetProbe>>();
    e->activate(48000, 64, 2, 2);
    double left[64]{}, right[64]{};
    double* io[] = {left, right};
    e->process<double>(io, io, 64);
    e->reset();
    NoEvents none;
    e->flush(none);
    e->process<double>(io, io, 64);
    CHECK(e->noteOutput().count() == 1);
    if (e->noteOutput().count()) CHECK(e->noteOutput()[0].time == 0);
    e->process<double>(io, io, 64);
    CHECK(e->noteOutput().count() == 0);
}

TEST(a_store_update_ends_notes_at_the_start_of_the_new_block) {
    class EndsOnUpdate : public ProcessorBase<EndsOnUpdate> {
      public:
        void prepare(const ProcessSetup&, const ParamValues&) {}
        void update(const ParamValues& v) {
            if (!v.initial()) noteEnded(NoteEvent{});
        }
        void process(Block&) {}
    };
    auto e = std::make_unique<BasicEngine<EndsOnUpdate>>();
    e->activate(48000.0, 64, 2, 2);
    double left[64]{}, right[64]{};
    double* io[] = {left, right};
    e->process<double>(io, io, 64);
    size_t index = 0;
    while (index < params::kCount && params::at(index).isOutput()) ++index;
    if (index == params::kCount) return;
    const ParamDef& p = params::at(index);
    e->store().set(index, hostDefault(p) == p.hostMax() ? p.hostMin() : p.hostMax());
    e->process<double>(io, io, 64);
    CHECK(e->noteOutput().count() == 1);
    CHECK(e->noteOutput().count() == 1 && e->noteOutput()[0].time == 0);
}

TEST(each_split_sees_the_transport_where_it_starts) {
    auto engine = std::make_unique<BasicEngine<TransportProbe>>();
    engine->activate(48000.0, 512, 2, 2);
    struct Two {
        int k = 0;
        bool next(EngineEvent& e) {
            if (k == 2) return false;
            e = EngineEvent{};
            e.index = 0;
            e.time = k == 0 ? 100u : 300u;
            e.value = hostDefault(params::at(0));
            ++k;
            return true;
        }
    } splits;
    Transport t;
    t.playing = t.hasTempo = t.hasBeats = true;
    t.tempo = 120.0;
    t.beats = 4.0;
    std::vector<double> l(480), r(480);
    double* io[] = {l.data(), r.data()};
    engine->process<double>(io, io, 480, splits, &t);
    const auto& p = engine->processor();
    CHECK(p.count == 3);
    int offset = 0;
    for (int i = 0; i < p.count; ++i) {
        // Two beats a second at 120 BPM.
        CHECK_NEAR(p.seen[i].beats, 4.0 + offset / 48000.0 * 2.0, 1e-12);
        offset += p.seen[i].frames;
    }
}

namespace {

// A delay's tail: one beat at the tempo it last heard, none once reset.
class TempoTail : public ProcessorBase<TempoTail> {
  public:
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues&) {}
    void reset() { tail = 0; }
    void process(Block& b) {
        if (b.transport) tail = static_cast<uint32_t>(48000.0 * 60.0 / b.transport->tempo);
    }
    uint32_t tailSamples() const { return tail; }
    uint32_t tail = 0;
};

}  // namespace

TEST(a_tail_that_changes_as_the_processor_plays_reaches_the_host) {
    auto engine = std::make_unique<BasicEngine<TempoTail>>();
    engine->activate(48000.0, 512, 2, 2);
    CHECK(!engine->takeTailChange());
    Transport t;
    t.hasTempo = true;
    t.tempo = 120.0;
    std::vector<double> l(256), r(256);
    double* io[] = {l.data(), r.data()};
    NoEvents none;
    engine->process<double>(io, io, 256, none, &t);
    CHECK(engine->tail() == 24000);
    CHECK(engine->takeTailChange());
    CHECK(!engine->takeTailChange());
    engine->process<double>(io, io, 256, none, &t);
    CHECK(!engine->takeTailChange());  // told once, not every block
    t.tempo = 60.0;
    engine->process<double>(io, io, 256, none, &t);
    CHECK(engine->tail() == 48000);
    CHECK(engine->takeTailChange());
}

TEST(a_reset_tail_reaches_the_host_before_the_next_block) {
    auto engine = std::make_unique<BasicEngine<TempoTail>>();
    engine->activate(48000.0, 512, 2, 2);
    Transport t;
    t.hasTempo = true;
    t.tempo = 120.0;
    std::vector<double> l(256), r(256);
    double* io[] = {l.data(), r.data()};
    NoEvents none;
    engine->process<double>(io, io, 256, none, &t);
    CHECK(engine->takeTailChange());
    engine->reset();
    CHECK(engine->tail() == 0);
    CHECK(engine->takeTailChange());
}

TEST(reset_preserves_parameters_and_clears_history) {
    // After reset, the engine behaves as a fresh one with the same values.
    test::RenderOptions o;
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (!p.is(pflag::kBypass) && !p.isOutput()) o.initial.push_back({p.id, toPlain(p, fromNormalized(p, 0.3))});
    }
    test::RenderOptions busy = o;
    everyInput(busy, 9000, 8);
    auto used = std::make_unique<Engine>();
    renderAll(test::noise(2, 9000, 8), busy, used.get());
    used->reset();
    const test::Signal probe = test::sine(2, 4000, 440.0, 48000.0);
    test::RenderOptions after = o;
    after.blocks = {4000};
    everyInput(after, 4000, 9);
    test::RenderOptions carryOn = after;
    carryOn.activate = false;
    const Rendered a = renderAll(probe, carryOn, used.get()), b = renderAll(probe, after);
    CHECK_NEAR(differenceAfterReset(a, b), 0.0, 1e-12);
}

TEST(after_reset_output_depends_only_on_the_state) {
    // CLAP's reset contract, as a host that reuses one instance relies on it
    // (an offline renderer, a sandbox): load a state, reset, render -- and
    // get exactly what a fresh instance with that state renders. The state
    // lands in the store just before reset(), so reset must adopt it snapped,
    // not glide to it from the old one. Restart parameters keep their values:
    // changing them needs a reactivation, which is a different contract.
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto randomState = [&] {
        std::vector<double> host(params::kCount);
        for (size_t i = 0; i < params::kCount; ++i) {
            const ParamDef& p = params::at(i);
            host[i] = p.is(pflag::kRestart) || p.is(pflag::kHidden) || p.isOutput() ? hostDefault(p)
                                                                                     : fromNormalized(p, unit(rng));
        }
        return host;
    };
    const test::Signal before = test::noise(2, 12000, 31), probe = test::noise(2, 9000, 32);
    for (int trial = 0; trial < 6; ++trial) {
        const std::vector<double> first = randomState(), second = randomState();
        auto used = std::make_unique<Engine>();
        for (size_t i = 0; i < params::kCount; ++i) used->store().set(i, first[i]);
        test::RenderOptions o;
        o.blocks = {500};
        everyInput(o, 12000, 40 + static_cast<uint32_t>(trial));
        renderAll(before, o, used.get());
        for (size_t i = 0; i < params::kCount; ++i) used->store().set(i, second[i]);
        used->reset();
        test::RenderOptions next;
        next.blocks = {500};
        everyInput(next, 9000, 50 + static_cast<uint32_t>(trial));
        test::RenderOptions carryOn = next;
        carryOn.activate = false;
        const Rendered x = renderAll(probe, carryOn, used.get());

        auto fresh = std::make_unique<Engine>();
        for (size_t i = 0; i < params::kCount; ++i) fresh->store().set(i, second[i]);
        const Rendered y = renderAll(probe, next, fresh.get());
        CHECK_MSG(differenceAfterReset(x, y) == 0.0, "trial %d differs by %g", trial, differenceAfterReset(x, y));
    }
}

TEST(modulation_is_not_saved_and_events_past_the_block_are_kept) {
    size_t index = params::kCount;
    for (size_t i = 0; i < params::kCount; ++i)
        if (params::at(i).is(pflag::kModulatable)) {
            index = i;
            break;
        }
    if (index == params::kCount) return;
    auto e = std::make_unique<Engine>();
    e->activate(48000.0, 64, 2, 2);
    const double base = e->store().get(index);
    struct Two {
        size_t index;
        int k = 0;
        bool next(EngineEvent& ev) {
            if (k == 2) return false;
            ev = EngineEvent{};
            ev.type = k == 0 ? EngineEvent::Type::Modulation : EngineEvent::Type::Value;
            ev.index = static_cast<int>(index);
            ev.time = k == 0 ? 0u : 999u;  // stamped past this 64-frame block
            ev.value = k == 0 ? 0.1 : fromNormalized(params::at(index), 0.25);
            ++k;
            return true;
        }
    } events{index};
    std::vector<double> l(64), r(64);
    double* io[] = {l.data(), r.data()};
    e->process<double>(io, io, 64, events, nullptr);
    CHECK(e->modulation(index) == 0.1);
    CHECK(e->store().get(index) != base);
    CHECK_NEAR(e->store().get(index), fromNormalized(params::at(index), 0.25), 1e-12);
}

// --- smoothing, outputs, levels -----------------------------------------------------------
//
// These follow the product's own table: a product without smoothed, output or
// level parameters skips them, and the template's examples have each.

namespace {

// Records every smoothed parameter's value at each frame, as a processor
// reads it.
class GlideProbe : public ProcessorBase<GlideProbe> {
  public:
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues&) {}
    void process(Block& b) {
        for (size_t k = 0; k < params::kSmoothedCount; ++k) {
            const uint32_t id = params::at(params::kSmoothedIndices[k]).id;
            const double* v = b.smoothed(id);
            for (int i = 0; i < b.frames && static_cast<size_t>(at + i) < trace[k].size(); ++i) trace[k][static_cast<size_t>(at + i)] = v[i];
            if (!b.gliding(id))
                for (int i = 1; i < b.frames; ++i) constantLied |= v[i] != v[0];
        }
        at += b.frames;
    }
    std::vector<std::vector<double>> trace;
    int at = 0;
    bool constantLied = false;
};

// Every smoothed parameter starts at `from` and is moved to `to` (both
// normalized) at frame `when`, through host blocks of the given sizes.
void glide(BasicEngine<GlideProbe>& e, std::vector<int> blocks, int frames, int when, double from, double to, int trailing = 0) {
    e.processor().trace.assign(params::kSmoothedCount, std::vector<double>(static_cast<size_t>(frames + trailing)));
    for (uint16_t i : params::kSmoothedIndices) e.store().set(i, fromNormalized(params::at(i), from));
    e.activate(48000.0, *std::max_element(blocks.begin(), blocks.end()), 2, 2);
    struct Moves {
        int when, start, end;
        double to;
        size_t k = 0;
        bool next(EngineEvent& ev) {
            if (k == params::kSmoothedCount || when < start || when >= end) return false;
            const size_t i = params::kSmoothedIndices[k++];
            ev = EngineEvent{};
            ev.time = static_cast<uint32_t>(when - start);
            ev.index = static_cast<int>(i);
            ev.value = fromNormalized(params::at(i), to);
            return true;
        }
    };
    std::vector<double> l(4096), r(4096);
    double* io[] = {l.data(), r.data()};
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(blocks[static_cast<size_t>(b) % blocks.size()], frames - frame);
        Moves moves{when, frame, frame + n, to};
        e.process<double>(io, io, n, moves, nullptr);
        frame += n;
    }
}

}  // namespace

TEST(smoothing_capture_is_sized_for_the_requested_duration) {
    if (!params::kSmoothedCount) return;
    auto e = std::make_unique<BasicEngine<GlideProbe>>();
    glide(*e, {512}, 70000, 1000, 0.2, 0.8);
    CHECK(e->processor().trace.front().size() >= 70000);
}

TEST(smoothed_parameters_glide_sample_accurately_and_settle_exactly) {
    if (params::kSmoothedCount == 0) {
        std::printf("      (no smoothed parameters in this product)\n");
        return;
    }
    // Long enough for the slowest to settle: 25 time constants and more.
    double slowest = 0.0;
    for (size_t k = 0; k < params::kSmoothedCount; ++k) slowest = std::max(slowest, params::at(params::kSmoothedIndices[k]).smoothing);
    const int when = 3000, frames = when + std::max(57000, static_cast<int>(25.0 * slowest * 48.0));
    auto one = std::make_unique<BasicEngine<GlideProbe>>(), split = std::make_unique<BasicEngine<GlideProbe>>();
    glide(*one, {4096}, frames, when, 0.2, 0.8);
    glide(*split, {1, 7, 64, 513, 2, 1000}, frames, when, 0.2, 0.8);
    CHECK(!one->processor().constantLied && !split->processor().constantLied);
    for (size_t k = 0; k < params::kSmoothedCount; ++k) {
        const ParamDef& p = params::at(params::kSmoothedIndices[k]);
        const std::vector<double>& x = one->processor().trace[k];
        const double from = toPlain(p, fromNormalized(p, 0.2)), to = toPlain(p, fromNormalized(p, 0.8));
        CHECK_MSG(x == split->processor().trace[k], "%s depends on where the host splits blocks", p.name);
        bool before = true, monotonic = true;
        for (int i = 0; i < when; ++i) before &= x[static_cast<size_t>(i)] == from;
        for (int i = when; i < frames; ++i) {
            const double a = x[static_cast<size_t>(i - 1)], b = x[static_cast<size_t>(i)];
            monotonic &= (to > from ? b >= a && b <= to : b <= a && b >= to);
        }
        CHECK_MSG(before, "%s moved before its change", p.name);
        CHECK_MSG(x[when] != from, "%s did not start moving at its change's frame", p.name);
        CHECK_MSG(monotonic, "%s overshot or turned back", p.name);
        const size_t last = static_cast<size_t>(frames - 1);
        CHECK_MSG(x[last] == to, "%s settled at %.17g, not %.17g", p.name, x[last], to);
        // One time constant covers 1 - 1/e of the way, measured where it glides
        // (log frequency for a log taper).
        auto domain = [&](double v) { return p.taper == Taper::Log ? std::log(v) : v; };
        const int tau = static_cast<int>(std::lround(p.smoothing * 48.0));
        const double covered = (domain(x[static_cast<size_t>(when + tau - 1)]) - domain(from)) / (domain(to) - domain(from));
        CHECK_MSG(std::fabs(covered - (1.0 - std::exp(-1.0))) < 0.01, "%s covered %g in %g ms", p.name, covered,
                  p.smoothing);
    }
    // Reset snaps: what follows starts at the value, not on the way to it.
    auto reused = std::make_unique<BasicEngine<GlideProbe>>();
    glide(*reused, {512}, 4096, 1000, 0.2, 0.8, 1024);
    reused->reset();
    std::vector<double> l(512), r(512);
    double* io[] = {l.data(), r.data()};
    reused->process<double>(io, io, 512);
    for (size_t k = 0; k < params::kSmoothedCount; ++k) {
        const ParamDef& p = params::at(params::kSmoothedIndices[k]);
        const double to = toPlain(p, fromNormalized(p, 0.8));
        bool snapped = true;
        for (int i = 4096; i < 4096 + 512; ++i) snapped &= reused->processor().trace[k][static_cast<size_t>(i)] == to;
        CHECK_MSG(snapped, "%s glided on after reset()", p.name);
    }
    // So does a change flushed after reset(), before any audio: a host's
    // render resets, flushes the values it wants, then processes.
    struct Back {
        size_t k = 0;
        bool next(EngineEvent& ev) {
            if (k == params::kSmoothedCount) return false;
            const size_t i = params::kSmoothedIndices[k++];
            ev = EngineEvent{};
            ev.index = static_cast<int>(i);
            ev.value = fromNormalized(params::at(i), 0.2);
            return true;
        }
    } back;
    reused->reset();
    reused->flush(back);
    reused->process<double>(io, io, 512);
    for (size_t k = 0; k < params::kSmoothedCount; ++k) {
        const ParamDef& p = params::at(params::kSmoothedIndices[k]);
        const double to = toPlain(p, fromNormalized(p, 0.2));
        bool snapped = true;
        for (int i = 4096 + 512; i < 4096 + 1024; ++i) snapped &= reused->processor().trace[k][static_cast<size_t>(i)] == to;
        CHECK_MSG(snapped, "%s glided to a value flushed after reset()", p.name);
    }
}

namespace {

// Reports every output at 70% of its range, and remembers what update() was
// told about them.
class OutputProbe : public ProcessorBase<OutputProbe> {
  public:
    static double reported(size_t i) { return toPlain(params::at(i), fromNormalized(params::at(i), 0.7)); }
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues& v) {
        for (uint16_t i : params::kOutputIndices) told[i] = v.atIndex(i);
    }
    void process(Block&) {
        for (uint16_t i : params::kOutputIndices) setOutput(params::at(i).id, reported(i));
    }
    double told[params::kCount]{};
};

}  // namespace

TEST(outputs_reach_the_store_at_the_hosts_rate_and_nobody_else_sets_them) {
    if (params::kOutputCount == 0) {
        std::printf("      (no output parameters in this product)\n");
        return;
    }
    auto e = std::make_unique<BasicEngine<OutputProbe>>();
    e->activate(48000.0, 480, 2, 2);
    for (uint16_t i : params::kOutputIndices) CHECK(e->store().get(i) == hostDefault(params::at(i)));
    // A host's value for an output is dropped before the processor hears it.
    struct HostSets {
        size_t k = 0;
        bool next(EngineEvent& ev) {
            if (k == params::kOutputCount) return false;
            const size_t i = params::kOutputIndices[k++];
            ev = EngineEvent{};
            ev.index = static_cast<int>(i);
            ev.value = fromNormalized(params::at(i), 0.1);
            return true;
        }
    } sets;
    std::vector<double> l(480), r(480);
    double* io[] = {l.data(), r.data()};
    e->process<double>(io, io, 480, sets, nullptr);
    for (uint16_t i : params::kOutputIndices) {
        const ParamDef& p = params::at(i);
        CHECK_MSG(e->processor().told[i] == p.def, "%s: the processor was told a host's value", p.name);
        CHECK_MSG(e->store().get(i) == toHost(p, OutputProbe::reported(i)), "%s: the store has %g", p.name,
                  e->store().get(i));
    }
    e->takeOutputsDue();
    int due = 0;
    for (int b = 0; b < 100; ++b) {  // one second
        e->process<double>(io, io, 480);
        due += e->takeOutputsDue() ? 1 : 0;
    }
    CHECK_MSG(std::abs(due - static_cast<int>(Engine::kOutputHz)) <= 1, "outputs due %d times in a second", due);
}

TEST(a_level_reads_as_an_exact_gain_and_off_is_silence) {
    int levels = 0;
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (p.unit != Unit::Decibels || p.isOutput()) continue;
        ++levels;
        ParamValues v;
        v.setIndex(i, p.min);
        if (p.is(pflag::kOffAtMin))
            CHECK_MSG(v.gain(p.id) == 0.0, "%s is not silent when off", p.name);
        else
            CHECK_NEAR(v.gain(p.id), std::pow(10.0, p.min / 20.0), 1e-12);
        CHECK(v.off(p.id) == p.is(pflag::kOffAtMin));
        if (p.min < 0.0 && p.max > 0.0) {
            v.setIndex(i, 0.0);
            CHECK_MSG(v.gain(p.id) == 1.0, "%s at 0 dB", p.name);
        }
        v.setIndex(i, p.max);
        CHECK_NEAR(v.gain(p.id), std::pow(10.0, p.max / 20.0), 1e-12);
        CHECK(!v.off(p.id));
    }
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (p.labels != kNoteValueLabels) continue;
        ParamValues v;
        v.setIndex(i, detail::noteValueIndex("1/4."));
        CHECK(v.beats(p.id) == 1.5);
    }
    std::printf("      %d level parameters\n", levels);
}

// --- state ----------------------------------------------------------------------------

TEST(state_round_trips_byte_for_byte) {
    state::Snapshot a;
    for (size_t i = 0; i < params::kCount; ++i) a.host[i] = fromNormalized(params::at(i), 0.37);
    a.editorScale = 1.25;
    a.defaultProperties();
    for (size_t i = 0; i < properties::kCount; ++i)
        a.property[i] = std::string("v\xC3\xA9") + std::to_string(i);  // UTF-8 survives
    a.hasPreset = true;
    a.presetName = "Somewhere";
    a.presetModified = true;
    const auto bytes = state::save(0x54455354, a);
    CHECK(bytes.size() > 8 && std::memcmp(bytes.data(), "TEST", 4) == 0);
    state::Snapshot b;
    CHECK(state::load(bytes.data(), bytes.size(), 0x54455354, b) == state::LoadResult::Ok);
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        // An output is the processor's report, not state: it loads as its default.
        CHECK_MSG(b.host[i] == (p.isOutput() ? hostDefault(p) : a.host[i]), "%s", p.name);
    }
    CHECK(b.editorScale == 1.25);
    CHECK(b.hasPreset && b.presetName == "Somewhere" && b.presetModified);
    for (size_t i = 0; i < properties::kCount; ++i) CHECK(b.hasProperty[i] && b.property[i] == a.property[i]);
    CHECK(state::save(0x54455354, b) == bytes);
    // Nor does an output's value change what is saved.
    if (params::kOutputCount > 0) {
        for (uint16_t i : params::kOutputIndices) a.host[i] = fromNormalized(params::at(i), 0.81);
        CHECK(state::save(0x54455354, a) == bytes);
    }
    std::printf("      %zu properties\n", properties::kCount);
}

TEST(a_preset_leaves_out_the_session) {
    state::Snapshot a;
    a.editorScale = 1.5;
    a.defaultProperties();
    for (size_t i = 0; i < properties::kCount; ++i) a.property[i] = "x" + std::to_string(i);
    a.hasPreset = true;
    a.presetName = "Name";
    const auto bytes = state::save(0x54455354, a, state::Context::Preset);
    state::Snapshot b;
    CHECK(state::load(bytes.data(), bytes.size(), 0x54455354, b) == state::LoadResult::Ok);
    CHECK(b.editorScale == 0.0);
    CHECK(!b.hasPreset);
    int perProject = 0;
    for (size_t i = 0; i < properties::kCount; ++i) {
        const bool session = properties::at(i).perProjectOnly();
        perProject += session ? 1 : 0;
        CHECK_MSG(b.hasProperty[i] == !session, "%s", properties::at(i).name);
        if (!session) CHECK(b.property[i] == a.property[i]);
    }
    // A preset of the defaults is the same bytes however the session differs.
    state::Snapshot fresh;
    fresh.defaultProperties();
    state::Snapshot other = fresh;
    other.editorScale = 2.0;
    other.hasPreset = true;
    other.presetName = "Elsewhere";
    for (size_t i = 0; i < properties::kCount; ++i)
        if (properties::at(i).perProjectOnly()) other.property[i] = "session only";
    CHECK(state::save(0x54455354, fresh, state::Context::Preset) ==
          state::save(0x54455354, other, state::Context::Preset));
    std::printf("      %d perProject properties\n", perProject);
}

TEST(a_property_value_too_long_for_it_loads_as_its_default) {
    if constexpr (properties::kCount == 0) {
        std::puts("      (no properties in this product)");
    } else {
        const PropertyDef& d = properties::at(0);
        state::Writer w;
        w.tag(0x54455354);
        w.u32(1);
        const size_t params = w.beginChunk(state::kParamChunk);
        w.u32(0);
        w.endChunk(params);
        const size_t props = w.beginChunk(state::kPropertyChunk);
        w.u32(2);
        w.u32(d.id);
        w.text(std::string(d.limit + 1, 'a'));
        w.u32(0xFFFFFFF0u);  // an ID nobody has
        w.text("ignored");
        w.endChunk(props);
        state::Snapshot out;
        CHECK(state::load(w.bytes().data(), w.bytes().size(), 0x54455354, out) == state::LoadResult::Ok);
        CHECK(!out.hasProperty[0]);
        // A length running past its chunk is damage.
        auto bytes = w.bytes();
        bytes[bytes.size() - 8] = 0x7F;
        CHECK(state::load(bytes.data(), bytes.size(), 0x54455354, out) == state::LoadResult::Corrupt);
    }
}

TEST(state_rejects_damage_and_skips_what_it_does_not_know) {
    state::Snapshot s;
    s.defaultProperties();
    s.hasPreset = true;
    s.presetName = "Cut me";
    auto bytes = state::save(0x54455354, s);
    state::Snapshot out;
    CHECK(state::load(bytes.data(), bytes.size(), 0x41424344, out) == state::LoadResult::NotOurs);
    // Cut anywhere inside a chunk. (Cut exactly between two, what is left is
    // a whole, shorter state, and loads as one.)
    std::vector<size_t> boundaries = {8};
    for (size_t at = 8; at + 8 <= bytes.size();) {
        const size_t length = static_cast<size_t>(bytes[at + 4]) | static_cast<size_t>(bytes[at + 5]) << 8 |
                              static_cast<size_t>(bytes[at + 6]) << 16 | static_cast<size_t>(bytes[at + 7]) << 24;
        at += 8 + length;
        boundaries.push_back(at);
    }
    for (size_t cut = 1; cut < bytes.size(); cut += 3) {
        if (std::find(boundaries.begin(), boundaries.end(), cut) != boundaries.end()) continue;
        CHECK_MSG(state::load(bytes.data(), cut, 0x54455354, out) != state::LoadResult::Ok, "accepted %zu bytes", cut);
    }
    // An unknown chunk from a future version is skipped.
    state::Writer w;
    w.tag(0x54455354);
    w.u32(1);
    const size_t future = w.beginChunk(state::fourcc("ZZZZ"));
    w.f64(1.0);
    w.endChunk(future);
    size_t first = 0;
    while (first + 1 < params::kCount && params::at(first).isOutput()) ++first;
    const size_t p = w.beginChunk(state::kParamChunk);
    w.u32(1);
    w.u32(params::at(first).id);
    w.f64(params::at(first).hostMax());
    w.u32(0);  // padding inside the chunk is tolerated too
    w.endChunk(p);
    CHECK(state::load(w.bytes().data(), w.bytes().size(), 0x54455354, out) == state::LoadResult::Ok);
    CHECK(out.host[first] == params::at(first).hostMax());
    for (size_t i = 0; i < params::kCount; ++i)
        if (i != first) CHECK(out.host[i] == hostDefault(params::at(i)));
}

// --- properties --------------------------------------------------------------------------

namespace {

// Records each property value it hears, and when.
class PropertyProbe : public ProcessorBase<PropertyProbe> {
  public:
    void prepare(const ProcessSetup&, const ParamValues&) { prepared = true; }
    void update(const ParamValues&) {}
    void process(Block&) { ++blocks; }
    void property(uint32_t id, std::string_view value) {
        lastLength = std::min<size_t>(value.size(), sizeof(last));
        std::memcpy(last, value.data(), lastLength);
        if (count == 32) return;
        Heard& h = heard[count++];
        h.id = id;
        h.length = std::min<size_t>(value.size(), sizeof(h.text));
        std::memcpy(h.text, value.data(), h.length);
        h.prepared = prepared;
        h.blocks = blocks;
    }
    struct Heard {
        uint32_t id = 0;
        char text[64] = {};
        size_t length = 0;
        bool prepared = false;
        int blocks = 0;
        std::string_view value() const { return {text, length}; }
    } heard[32];
    int count = 0, blocks = 0;
    bool prepared = false;
    char last[64] = {};
    size_t lastLength = 0;
    std::string_view lastValue() const { return {last, lastLength}; }
};

void oneBlock(BasicEngine<PropertyProbe>& e) {
    std::vector<double> l(64), r(64);
    double* io[] = {l.data(), r.data()};
    e.process<double>(io, io, 64);
}

}  // namespace

TEST(the_processor_hears_every_property_after_prepare_and_each_change_before_the_next_block) {
    if constexpr (properties::kCount == 0) {
        std::puts("      (no properties in this product)");
        return;
    }
    auto e = std::make_unique<BasicEngine<PropertyProbe>>();
    const int width = product::info::kAudioOutput ? 2 : 0;
    CHECK(e->properties().set(0, "before"));
    e->activate(48000.0, 64, product::info::kAudioInput ? width : 0, width);
    const PropertyProbe& p = e->processor();
    CHECK(p.count == static_cast<int>(properties::kCount));
    for (size_t k = 0; k < properties::kCount && k < 32; ++k) {
        CHECK(p.heard[k].id == properties::at(k).id && p.heard[k].prepared);
        CHECK(p.heard[k].value() == e->properties().get(k));
    }
    CHECK(p.heard[0].value() == "before");

    // A change waits for the next block, and arrives before it runs.
    const int before = p.count;
    CHECK(e->properties().set(0, "after"));
    CHECK(p.count == before);
    oneBlock(*e);
    CHECK(p.count == before + 1 && p.heard[before].value() == "after" && p.heard[before].blocks == 0);

    // Two changes between blocks: only the last is heard. The same value
    // again is no change at all.
    CHECK(e->properties().set(0, "first") && e->properties().set(0, "second"));
    oneBlock(*e);
    CHECK(p.count == before + 2 && p.heard[before + 1].value() == "second");
    CHECK(e->properties().set(0, "second"));
    oneBlock(*e);
    CHECK(p.count == before + 2);

    // Too long for the property: refused, nothing changes.
    CHECK(!e->properties().set(0, std::string(properties::at(0).limit + 1, 'x')));
    CHECK(e->properties().get(0) == "second");
    // Many changes: the copies the audio thread let go are freed, and none
    // is lost (asan watches for both).
    for (int k = 0; k < 500; ++k) {
        CHECK(e->properties().set(0, std::to_string(k)));
        oneBlock(*e);
    }
    e->properties().collect();
    CHECK(p.lastValue() == "499");
}

// --- what the host says about the session ----------------------------------------------------

namespace {

// Records offline mode, retuning as a processor asks for it, and the notes
// it is given; says its note names changed when told to.
class SessionProbe : public ProcessorBase<SessionProbe> {
  public:
    void prepare(const ProcessSetup& setup, const ParamValues&) { preparedOffline = setup.offline; }
    void update(const ParamValues&) {
        if (renameNext) noteNamesChanged();
        renameNext = false;
    }
    void note(const NoteEvent& e) {
        if (notes < 8) keys[notes++] = e.key;
        atNote = retuning(e.key, e.channel);
    }
    void process(Block& b) {
        blockOffline = b.offline;
        if (splits < 8) heard[splits++] = retuning(60, 0, 1);
    }
    bool preparedOffline = false, blockOffline = false, renameNext = false;
    int keys[8] = {}, notes = 0, splits = 0;
    double atNote = 0.0, heard[8] = {};
};

// Retuning that says where it was asked: the key in hundredths, the frame in
// millionths, plus the tuning the host chose. Key 61 is unmapped.
struct ScriptedTuning : TuningSource {
    uint32_t chosen = 0;
    double retuning(int key, int, uint32_t frame) override { return key * 0.01 + frame * 1e-6 + chosen; }
    bool plays(int key, int) override { return key != 61; }
    void select(int, int, uint32_t id) override { chosen = id; }
};

struct SessionEvents {
    std::vector<EngineEvent> list;
    size_t k = 0;
    bool next(EngineEvent& e) {
        if (k == list.size()) return false;
        e = list[k++];
        return true;
    }
    void note(uint32_t time, int key) {
        EngineEvent e;
        e.type = EngineEvent::Type::Note;
        e.time = time;
        e.note.key = static_cast<int16_t>(key);
        list.push_back(e);
    }
    void tuning(uint32_t time, uint32_t id) {
        EngineEvent e;
        e.type = EngineEvent::Type::Tuning;
        e.time = time;
        e.address.channel = -1;
        e.tuning = id;
        list.push_back(e);
    }
};

}  // namespace

TEST(offline_rendering_reaches_prepare_and_the_next_block) {
    auto e = std::make_unique<BasicEngine<SessionProbe>>();
    e->setOffline(true);
    e->activate(48000.0, 64, 2, 2);
    CHECK(e->processor().preparedOffline);
    std::vector<double> l(64), r(64);
    double* io[] = {l.data(), r.data()};
    e->process<double>(io, io, 64);
    CHECK(e->processor().blockOffline);
    e->setOffline(false);  // while active: heard from the next block
    e->process<double>(io, io, 64);
    CHECK(!e->processor().blockOffline);
}

TEST(a_tuning_is_read_where_asked_and_unmapped_keys_never_sound) {
    auto e = std::make_unique<BasicEngine<SessionProbe>>();
    ScriptedTuning tuning;
    e->setTuning(&tuning);
    e->activate(48000.0, 256, 2, 2);
    SessionEvents events;
    events.note(10, 60);
    events.note(20, 61);       // unmapped: retired at once, never heard
    events.tuning(30, 7);      // from frame 30 on
    std::vector<double> l(100), r(100);
    double* io[] = {l.data(), r.data()};
    e->process<double>(io, io, 100, events, nullptr);
    const SessionProbe& p = e->processor();
    CHECK(p.notes == 1 && p.keys[0] == 60);
    CHECK_NEAR(p.atNote, 0.60 + 10e-6, 1e-12);  // at the note's own frame
    const NoteOutput& out = e->noteOutput();
    CHECK(out.count() == 1 && out[0].kind == NoteOutput::Kind::End && out[0].note.key == 61 && out[0].time == 20);
    // Splits at 0, 10, 20 and 30; each asks one frame in, and the last after
    // the host chose tuning 7.
    CHECK(p.splits == 4);
    CHECK_NEAR(p.heard[0], 0.60 + 1e-6, 1e-12);
    CHECK_NEAR(p.heard[2], 0.60 + 21e-6, 1e-12);
    CHECK_NEAR(p.heard[3], 7.60 + 31e-6, 1e-12);
}

TEST(a_processor_says_when_its_note_names_change) {
    auto e = std::make_unique<BasicEngine<SessionProbe>>();
    e->activate(48000.0, 64, 2, 2);
    CHECK(!e->takeNoteNamesChange());
    e->processor().renameNext = true;
    // Any update will do: move the first parameter the processor reads.
    size_t i = 0;
    while (params::at(i).isOutput()) ++i;
    const ParamDef& p = params::at(i);
    const double moved = fromNormalized(p, 0.37) != e->store().get(i) ? fromNormalized(p, 0.37) : fromNormalized(p, 0.71);
    e->store().set(i, moved);
    NoEvents none;
    e->flush(none);
    CHECK(e->takeNoteNamesChange() && !e->takeNoteNamesChange());
}

// --- what the editor plays and sees ----------------------------------------------------------

namespace {
// Halves its input, and asks for the audio feed.
class FeedProbe : public ProcessorBase<FeedProbe> {
  public:
    static constexpr bool kAudioFeed = true;
    void prepare(const ProcessSetup&, const ParamValues&) {}
    void update(const ParamValues&) {}
    void process(Block& b) {
        for (int c = 0; c < b.channels; ++c)
            for (int i = 0; i < b.frames; ++i) b.channel(c)[i] *= 0.5;
    }
};
}  // namespace

TEST(the_editor_gets_the_input_and_output_only_while_it_is_open) {
    auto e = std::make_unique<BasicEngine<FeedProbe>>();
    e->activate(48000.0, 64, 2, 2);
    CHECK(e->feed() != nullptr);
    std::vector<double> l(64), r(64);
    auto fill = [&] {
        for (size_t i = 0; i < 64; ++i) l[i] = r[i] = 0.01 * static_cast<double>(i);
    };
    double* io[] = {l.data(), r.data()};
    fill();
    e->process<double>(io, io, 64);
    FeedFrame frames[256];
    CHECK(e->feed()->read(frames, 256) == 0);  // nobody is looking
    e->setMetering(true);
    fill();
    e->process<double>(io, io, 64);  // in place: the input is gone by the end
    CHECK(e->feed()->read(frames, 256) == 64);
    bool exact = true;
    for (size_t i = 0; i < 64; ++i) {
        const float in = static_cast<float>(0.01 * static_cast<double>(i));
        exact = exact && frames[i].in[0] == in && frames[i].in[1] == in;
        exact = exact && frames[i].out[0] == static_cast<float>(0.5 * 0.01 * static_cast<double>(i));
    }
    CHECK(exact);
    // A processor that did not ask has none.
    auto plain = std::make_unique<BasicEngine<SessionProbe>>();
    plain->activate(48000.0, 64, 2, 2);
    CHECK(plain->feed() == nullptr);
}

TEST(the_audio_feed_silences_a_disconnected_input_channel) {
    auto e = std::make_unique<BasicEngine<FeedProbe>>();
    e->activate(48000.0, 64, 2, 2);
    e->setMetering(true);
    double input[64], left[64]{}, right[64]{};
    std::fill_n(input, 64, 0.25);
    const double* in[] = {input, input};
    double* out[] = {left, right};
    FeedFrame frames[64];
    e->process<double>(in, out, 64);
    CHECK(e->feed()->read(frames, 64) == 64);
    in[1] = nullptr;
    e->process<double>(in, out, 64);
    CHECK(e->feed()->read(frames, 64) == 64);
    for (const auto& f : frames) CHECK(f.in[0] == 0.25f && f.in[1] == 0.0f);
    in[0] = nullptr;
    e->process<double>(in, out, 64);
    CHECK(e->feed()->read(frames, 64) == 64);
    for (const auto& f : frames) CHECK(f.in[0] == 0.0f && f.in[1] == 0.0f);
}

TEST(editor_note_off_survives_a_full_queue_and_reset_clears_keys) {
    if (!product::info::kNoteInput) return;
    class KeyboardProbe : public ProcessorBase<KeyboardProbe> {
      public:
        void prepare(const ProcessSetup&, const ParamValues&) {}
        void update(const ParamValues&) {}
        void process(Block&) {}
        void note(const NoteEvent& n) { held = n.type == NoteEvent::Type::On; }
        bool held = false;
    };
    auto e = std::make_unique<BasicEngine<KeyboardProbe>>();
    e->activate(48000, 512, 2, 2);
    e->setMetering(true);
    double l[512]{}, r[512]{};
    double* io[] = {l, r};
    for (int i = 0; i < 100; ++i) e->playNote(60, 0.5, true);
    e->playNote(60, 0, false);
    e->process<double>(io, io, 512);
    CHECK(!e->processor().held);
    e->playNote(64, 0.5, true);
    e->process<double>(io, io, 512);
    e->reset();
    TelemetryFrame frame{};
    while (e->telemetry().pop(frame)) {}
    for (int i = 0; i < 8; ++i) e->process<double>(io, io, 512);
    bool seen = false;
    while (e->telemetry().pop(frame)) {
        CHECK(frame.keys[0] == 0 && frame.keys[1] == 0);
        seen = true;
    }
    CHECK(seen);
}

TEST(the_editors_notes_play_at_the_next_block_and_held_keys_reach_telemetry) {
    auto e = std::make_unique<BasicEngine<SessionProbe>>();
    e->activate(48000.0, 512, 2, 2);
    e->setMetering(true);
    std::vector<double> l(512), r(512);
    double* io[] = {l.data(), r.data()};
    // Long enough for a telemetry frame at any product's rate.
    auto run = [&] {
        for (int b = 0; b < 8; ++b) e->process<double>(io, io, 512);
    };
    e->playNote(64, 0.5, true);
    run();
    if (!product::info::kNoteInput) {
        CHECK(e->processor().notes == 0);  // nothing to play it on
        std::printf("      (no note input in this product)\n");
        return;
    }
    CHECK(e->processor().notes == 1 && e->processor().keys[0] == 64);
    TelemetryFrame f{}, next{};
    CHECK(e->telemetry().pop(f));
    while (e->telemetry().pop(next)) f = next;
    CHECK(f.keys[1] == uint64_t{1} << (64 - 64) && f.keys[0] == 0);
    e->playNote(64, 0.0, false);
    run();
    CHECK(e->processor().notes == 2);
    while (e->telemetry().pop(next)) f = next;
    CHECK(f.keys[0] == 0 && f.keys[1] == 0);
}

// --- presets ---------------------------------------------------------------------------------

namespace {

constexpr uint32_t kTestMagic = state::fourcc(product::info::kStateTag);

// The state a preset library sees, kept in plain memory the way the plugin
// keeps it in its stores.
struct MemoryState : StateAccess {
    double host[params::kCount];
    PropertyStore props;
    int loads = 0;

    MemoryState() {
        for (size_t i = 0; i < params::kCount; ++i) host[i] = hostDefault(params::at(i));
    }
    std::vector<uint8_t> saveState(state::Context context) override {
        state::Snapshot s;
        for (size_t i = 0; i < params::kCount; ++i) s.host[i] = host[i];
        for (size_t i = 0; i < properties::kCount; ++i) {
            s.property[i] = props.get(i);
            s.hasProperty[i] = true;
        }
        return state::save(kTestMagic, s, context);
    }
    bool loadState(const uint8_t* data, size_t size, state::Context context) override {
        state::Snapshot s;
        if (state::load(data, size, kTestMagic, s) != state::LoadResult::Ok) return false;
        for (size_t i = 0; i < params::kCount; ++i)
            if (!params::at(i).isOutput()) host[i] = s.host[i];
        for (size_t i = 0; i < properties::kCount; ++i)
            if (context == state::Context::Project || !properties::at(i).perProjectOnly())
                props.set(i, s.hasProperty[i] ? std::string_view(s.property[i]) : std::string_view(properties::at(i).def));
        ++loads;
        return true;
    }
};

// The first parameter a preset can change, and a value other than its default.
size_t settable() {
    for (size_t i = 0; i < params::kCount; ++i)
        if (!params::at(i).isOutput() && !params::at(i).is(pflag::kBypass)) return i;
    return 0;
}
double otherValue(size_t i) {
    const ParamDef& p = params::at(i);
    return hostDefault(p) == p.hostMax() ? p.hostMin() : p.hostMax();
}

struct TempFolder {
    std::filesystem::path path;
    TempFolder() {
        path = std::filesystem::temp_directory_path() /
               ("supple-presets-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::rand()));
    }
    ~TempFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

TEST(the_defaults_are_unmodified_when_the_state_carries_a_layers_chunk) {
    // A layer's chunk follows the framework's state in every save; the
    // library's own defaults must carry it too to compare equal.
    struct Chunked : MemoryState {
        std::vector<uint8_t> saveState(state::Context context) override {
            std::vector<uint8_t> bytes = MemoryState::saveState(context);
            appendChunks(bytes, state::Snapshot{}, context);
            return bytes;
        }
        void appendChunks(std::vector<uint8_t>& bytes, const state::Snapshot&, state::Context) override {
            state::appendChunk(bytes, state::fourcc("LAYR"), {1, 2, 3});
        }
    };
    TempFolder folder;
    Chunked st;
    PresetLibrary lib(st, folder.path);
    CHECK(lib.currentName() == "Default" && !lib.modified());
    const size_t i = settable();
    st.host[i] = otherValue(i);
    CHECK(lib.modified());
}

TEST(user_presets_save_load_rename_and_delete) {
    TempFolder folder;
    MemoryState st;
    PresetLibrary lib(st, folder.path / "nested");  // created on the first save
    const size_t i = settable();
    CHECK(lib.currentName() == "Default" && !lib.modified() && lib.current() < 0);
    st.host[i] = otherValue(i);
    CHECK(lib.modified());

    CHECK(lib.save("My Sound", false) == PresetLibrary::SaveResult::Ok);
    CHECK(std::filesystem::exists(folder.path / "nested" / "My Sound.preset"));
    CHECK(lib.currentName() == "My Sound" && !lib.modified() && lib.current() >= 0);
    CHECK(lib.entries()[static_cast<size_t>(lib.current())].user());
    CHECK(lib.save("My Sound", false) == PresetLibrary::SaveResult::Exists);
    CHECK(lib.save("a/b", false) == PresetLibrary::SaveResult::InvalidName);
    CHECK(lib.entries().size() == presets::kFactoryCount + 1);

    // Defaults, then the preset again: its value comes back exactly.
    CHECK(lib.loadDefaults() && st.host[i] == hostDefault(params::at(i)) && lib.currentName() == "Default");
    CHECK(!lib.modified());
    const int mine = static_cast<int>(presets::kFactoryCount);
    CHECK(lib.load(mine) && st.host[i] == otherValue(i) && !lib.modified());

    // A user preset sorts by name, whatever the case.
    st.host[i] = hostDefault(params::at(i));
    CHECK(lib.save("another", false) == PresetLibrary::SaveResult::Ok);
    CHECK(lib.entries()[presets::kFactoryCount].name == "another");
    CHECK(lib.entries()[presets::kFactoryCount + 1].name == "My Sound");

    // Next and previous wrap around the whole list.
    const int n = static_cast<int>(lib.entries().size());
    CHECK(lib.load(n - 1) && lib.step(1) && lib.current() == 0);
    CHECK(lib.step(-1) && lib.current() == n - 1);

    // Rename keeps it current under its new name; a clash is refused.
    const int another = static_cast<int>(presets::kFactoryCount);
    CHECK(lib.load(another));
    CHECK(!lib.rename(another, "My Sound"));
    CHECK(lib.rename(another, "Zed"));
    CHECK(lib.currentName() == "Zed" && lib.entries()[static_cast<size_t>(lib.current())].name == "Zed");
    CHECK(std::filesystem::exists(folder.path / "nested" / "Zed.preset"));
    CHECK(!std::filesystem::exists(folder.path / "nested" / "another.preset"));

    // Deleting the current one leaves the sound, unnamed.
    CHECK(lib.remove(lib.current()));
    CHECK(lib.current() < 0 && lib.currentName().empty());
    CHECK(lib.entries().size() == presets::kFactoryCount + 1);
    // A factory preset is not the user's to rename or delete.
    if (presets::kFactoryCount > 0) CHECK(!lib.rename(0, "Mine") && !lib.remove(0));
}

TEST(comparison_slots_follow_preset_identity_through_library_changes) {
    TempFolder folder;
    MemoryState st;
    PresetLibrary lib(st, folder.path);
    CHECK(lib.save("Zulu", false) == PresetLibrary::SaveResult::Ok);
    const auto saved = st.saveState(state::Context::Preset);
    lib.compare(1);
    CHECK(lib.save("Alpha", false) == PresetLibrary::SaveResult::Ok);
    lib.compare(0);
    CHECK(lib.current() >= 0 && lib.entries()[static_cast<size_t>(lib.current())].name == "Zulu");
    CHECK(lib.rename(lib.current(), "Beta"));
    lib.compare(1);
    lib.compare(0);
    CHECK(lib.currentName() == "Beta");
    CHECK(lib.current() >= 0 && lib.entries()[static_cast<size_t>(lib.current())].name == "Beta");
    lib.copyToOther();
    CHECK(lib.remove(lib.current()));
    lib.compare(1);
    CHECK(lib.current() == -1 && lib.modified());
    CHECK(st.saveState(state::Context::Preset) == saved);
    lib.refresh();
    CHECK(lib.current() == -1);
}

TEST(preset_overwrite_preserves_other_writers_temporary_files) {
    TempFolder folder;
    MemoryState st;
    PresetLibrary lib(st, folder.path);
    CHECK(lib.save("Mine", false) == PresetLibrary::SaveResult::Ok);
    const auto other = folder.path / "Mine.preset.tmp";
    std::ofstream(other) << "other writer";
    st.host[settable()] = otherValue(settable());
    const auto wanted = st.saveState(state::Context::Preset);
    CHECK(lib.save("Mine", true) == PresetLibrary::SaveResult::Ok);
    CHECK(lib.loadDefaults());
    CHECK(lib.loadFile(folder.path / "Mine.preset"));
    CHECK(st.saveState(state::Context::Preset) == wanted);
    std::string text;
    std::getline(std::ifstream(other), text);
    CHECK(text == "other writer");
}

TEST(a_failed_preset_rename_is_not_reported_as_a_success) {
    TempFolder folder;
    MemoryState st;
    PresetLibrary lib(st, folder.path);
    const auto target = folder.path / "Blocked.preset";
    std::filesystem::create_directories(target);
    std::ofstream(target / "keep") << "keep";
    st.host[settable()] = otherValue(settable());
    CHECK(!lib.exportFile(target));
    CHECK(lib.save("Blocked", true) == PresetLibrary::SaveResult::Failed);
    CHECK(lib.currentName() == "Default" && lib.modified());
    CHECK(std::filesystem::exists(target / "keep"));
    CHECK(!std::filesystem::exists(folder.path / "Blocked.preset.tmp"));
}

TEST(a_file_that_is_not_ours_is_not_listed_or_loaded) {
    TempFolder folder;
    std::filesystem::create_directories(folder.path);
    for (const char* name : {"stray.preset", "empty.preset"}) {
        std::FILE* f = std::fopen((folder.path / name).string().c_str(), "wb");
        if (name[0] == 's') std::fputs("RIFF....WAVE", f);
        std::fclose(f);
    }
    MemoryState st;
    PresetLibrary lib(st, folder.path);
    CHECK(lib.entries().size() == presets::kFactoryCount);
    CHECK(!lib.loadFile(folder.path / "stray.preset") && st.loads == 0);
    CHECK(!lib.loadFile(folder.path / "missing.preset"));
}

TEST(factory_presets_set_what_they_name_and_default_the_rest) {
    std::printf("      %zu factory presets\n", presets::kFactoryCount);
    MemoryState st;
    PresetLibrary lib(st, {});
    CHECK(!lib.canSave());
    for (size_t k = 0; k < presets::kFactoryCount; ++k) {
        const Preset& preset = presets::factory(k);
        // Something else first, so every value has to be set.
        for (size_t i = 0; i < params::kCount; ++i) st.host[i] = otherValue(i);
        for (size_t i = 0; i < properties::kCount; ++i) st.props.set(i, "elsewhere");
        CHECK(lib.loadFactory(k));
        CHECK_MSG(lib.currentName() == preset.name && !lib.modified(), "%s", preset.name);
        for (size_t i = 0; i < params::kCount; ++i) {
            const ParamDef& p = params::at(i);
            if (p.isOutput()) continue;
            double want = hostDefault(p);
            for (const PresetValue& v : preset.values)
                if (v.id == p.id) want = sanitizeHost(p, toHost(p, v.plain));
            CHECK_MSG(st.host[i] == want, "%s: %s", preset.name, p.name);
        }
        for (size_t i = 0; i < properties::kCount; ++i) {
            std::string want = properties::at(i).perProjectOnly() ? "elsewhere" : properties::at(i).def;
            for (const PresetText& t : preset.texts)
                if (t.id == properties::at(i).id) want = t.value;
            CHECK_MSG(st.props.get(i) == want, "%s: %s", preset.name, properties::at(i).name);
        }
        CHECK(presets::findFactory(preset.name) == static_cast<int>(k));
    }
    CHECK(presets::findFactory("no such preset") < 0);
}

TEST(a_b_comparison_keeps_two_states) {
    MemoryState st;
    PresetLibrary lib(st, {});
    const size_t i = settable();
    const double a = st.host[i], b = otherValue(i);
    CHECK(lib.slot() == 0);
    lib.compare(1);  // B starts as A
    CHECK(lib.slot() == 1 && st.host[i] == a);
    st.host[i] = b;
    lib.compare(0);
    CHECK(st.host[i] == a);
    lib.compare(1);
    CHECK(st.host[i] == b && lib.modified());
    lib.copyToOther();  // B into A
    lib.compare(0);
    CHECK(st.host[i] == b);
}

TEST(settings_round_trip_as_text_and_garbage_changes_nothing) {
    MemoryState st;
    PresetLibrary lib(st, {});
    const size_t i = settable();
    st.host[i] = otherValue(i);
    const std::string text = lib.copyText();
    CHECK(text.find(product::info::kName) == 0);
    st.host[i] = hostDefault(params::at(i));
    CHECK(lib.pasteText("  " + text + "\n"));
    CHECK(st.host[i] == otherValue(i) && lib.currentName().empty() && !lib.modified());
    const int loads = st.loads;
    for (const char* junk : {"", "hello", "Starter state: !!!!", "AAAA", "QUJDRA=="})
        CHECK_MSG(!lib.pasteText(junk), "accepted '%s'", junk);
    CHECK(st.loads == loads);
}

TEST(a_session_remembers_the_preset_and_whether_it_changed) {
    TempFolder folder;
    MemoryState st;
    PresetLibrary lib(st, folder.path);
    const size_t i = settable();
    CHECK(lib.save("Kept", false) == PresetLibrary::SaveResult::Ok);
    st.host[i] = otherValue(i);
    state::Snapshot session;
    lib.describe(session);
    CHECK(session.hasPreset && session.presetName == "Kept" && session.presetModified);

    PresetLibrary other(st, folder.path);
    other.restore(session);
    CHECK(other.currentName() == "Kept" && other.current() >= 0 && other.modified());
    session.presetModified = false;
    other.restore(session);
    CHECK(!other.modified());
    state::Snapshot none;
    other.restore(none);
    CHECK(other.currentName().empty() && other.current() < 0);
}

TEST(preset_names_are_files_on_every_platform) {
    for (const char* good : {"Lead 1", "a", "Ünïcødé ✓", "a-b_c (2) [x] 'y' #3"})
        CHECK_MSG(PresetLibrary::validName(good), "refused '%s'", good);
    for (const char* bad : {"", " lead", "lead ", ".hidden", "end.", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b",
                            "a<b", "a>b", "a|b", "CON", "con.txt", "Com1", "LPT9", "nul ", "tab\there"})
        CHECK_MSG(!PresetLibrary::validName(bad), "accepted '%s'", bad);
    CHECK(!PresetLibrary::validName(std::string(65, 'x')));
    CHECK(PresetLibrary::validName(std::string(64, 'x')));
}

TEST(base64_round_trips_and_refuses_damage) {
    for (size_t n = 0; n < 10; ++n) {
        std::vector<uint8_t> bytes(n);
        for (size_t k = 0; k < n; ++k) bytes[k] = static_cast<uint8_t>(k * 77 + 3);
        std::vector<uint8_t> back;
        const std::string text = base64::encode(bytes);
        CHECK(text.size() % 4 == 0);
        CHECK(n == 0 ? !base64::decode(text, back) : base64::decode(text, back) && back == bytes);
    }
    std::vector<uint8_t> out;
    CHECK(base64::decode("TWFu", out) && out.size() == 3 && out[0] == 'M');
    for (const char* bad : {"TWF", "TW=u", "=WFu", "TWFu=", "T@Fu", "TW==TWFu"})
        CHECK_MSG(!base64::decode(bad, out), "accepted '%s'", bad);
}

// --- edit bridge --------------------------------------------------------------------------

namespace {

struct HostLog {
    std::map<size_t, int> depth;
    bool nested = false, unbalancedEnd = false, valueOutsideGesture = false;
    std::map<size_t, int> values;
    bool emit(EditBridge::Kind k, size_t i) {
        if (k == EditBridge::Kind::Begin) nested |= ++depth[i] > 1;
        else if (k == EditBridge::Kind::End) unbalancedEnd |= --depth[i] < 0;
        else ++values[i];
        return true;
    }
};

}  // namespace

TEST(edit_bridge_always_delivers_balanced_gestures_and_the_final_value) {
    std::mt19937 rng(21);
    for (int round = 0; round < 200; ++round) {
        EditBridge bridge;
        HostLog log;
        std::uniform_real_distribution<double> refuse(0.0, 1.0);
        for (int step = 0; step < 300; ++step) {
            const size_t i = rng() % params::kCount;
            switch (rng() % 5) {
                case 0: bridge.begin(i); break;
                case 1: bridge.value(i); break;
                case 2: bridge.end(i); break;
                default:
                    // A host output queue that refuses some events.
                    bridge.drain([&](EditBridge::Kind k, size_t j) { return refuse(rng) < 0.2 ? false : log.emit(k, j); });
            }
        }
        for (size_t i = 0; i < params::kCount; ++i) bridge.end(i);
        for (int k = 0; k < 50 && bridge.pending(); ++k)
            bridge.drain([&](EditBridge::Kind kind, size_t j) { return log.emit(kind, j); });
        CHECK(!bridge.pending());
        CHECK(!log.nested);
        CHECK(!log.unbalancedEnd);
        for (const auto& [i, d] : log.depth) CHECK_MSG(d == 0, "parameter %zu left %d gestures open", i, d);
    }
}

TEST(edit_bridge_makes_progress_when_the_host_takes_one_event_per_drain) {
    // A host whose output queue holds a single event. Every gesture must
    // still complete, in order, with nothing repeated forever.
    EditBridge bridge;
    HostLog log;
    std::vector<std::pair<EditBridge::Kind, size_t>> seen;
    auto drainOne = [&] {
        bool room = true;
        return bridge.drain([&](EditBridge::Kind k, size_t i) {
            if (!room) return false;
            room = false;
            seen.push_back({k, i});
            return log.emit(k, i);
        });
    };
    const size_t n = std::min<size_t>(params::kCount, 3);
    for (size_t i = 0; i < n; ++i) {
        bridge.begin(i);
        bridge.value(i);
        bridge.end(i);
    }
    int drains = 0;
    while (bridge.pending() && drains < 100) {
        drainOne();
        ++drains;
    }
    CHECK(!bridge.pending());
    CHECK_MSG(drains == static_cast<int>(3 * n), "%d drains for %zu complete gestures", drains, n);
    CHECK(!log.nested && !log.unbalancedEnd);
    for (size_t i = 0; i < n; ++i) {
        CHECK(log.values[i] == 1);
        CHECK(log.depth[i] == 0);
    }
    // A value that arrives while END is owed is delivered before it.
    seen.clear();
    bridge.begin(0);
    bridge.value(0);
    drainOne();  // BEGIN
    drainOne();  // VALUE
    bridge.value(0);
    bridge.end(0);
    for (int k = 0; k < 20 && bridge.pending(); ++k) drainOne();
    CHECK(!bridge.pending());
    CHECK(seen.size() == 4);
    CHECK(seen.size() == 4 && seen[2].first == EditBridge::Kind::Value && seen[3].first == EditBridge::Kind::End);
}

TEST(edit_bridge_is_safe_across_threads) {
    EditBridge bridge;
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        HostLog log;
        while (!done.load()) bridge.drain([&](EditBridge::Kind k, size_t i) { return log.emit(k, i); });
        bridge.drain([&](EditBridge::Kind k, size_t i) { return log.emit(k, i); });
        CHECK(!log.nested);
        for (const auto& [i, d] : log.depth) CHECK(d == 0);
    });
    std::mt19937 rng(5);
    for (int k = 0; k < 200000; ++k) {
        const size_t i = rng() % params::kCount;
        bridge.begin(i);
        bridge.value(i);
        if (rng() % 3) bridge.end(i);
    }
    for (size_t i = 0; i < params::kCount; ++i) bridge.end(i);
    done = true;
    consumer.join();
}

namespace {
// `n` properties, each `limit` bytes long at most.
PropertyList propertyList(size_t n, uint32_t limit = 4096) {
    PropertyList::Builder b;
    for (size_t i = 0; i < n; ++i)
        b.add(property(static_cast<uint32_t>(i), b.text("Blob " + std::to_string(i))).maxBytes(limit));
    return b.build();
}
}  // namespace

TEST(properties_cannot_add_up_to_more_state_than_loads_back) {
    // What a product's build checks and Definition::problems() says: 63 full
    // properties fit, 64 do not.
    const size_t params = params::kCount - params::kOutputCount;
    constexpr uint64_t kFull = PropertyList::kMaxBytes;
    CHECK(state::maxSavedBytes(params, 63, 63 * kFull) <= state::kMaxBytes);
    CHECK(state::maxSavedBytes(params, 64, 64 * kFull) > state::kMaxBytes);
    const Definition full(productDefinition()->info(), ParamTable(product::kParams, params::kCount),
                          propertyList(64, PropertyList::kMaxBytes));
    CHECK(full.problems().find("more state than will load back") != std::string::npos);
    // And the bound is the real worst case: every property at its limit,
    // the editor's scale and the longest preset name kept.
    const Definition& product = *productDefinition();
    state::Snapshot s;
    s.defaultProperties();
    for (size_t i = 0; i < properties::kCount; ++i) s.property[i].assign(properties::at(i).limit, 'x');
    s.editorScale = 1.5;
    s.hasPreset = true;
    s.presetName.assign(state::kMaxPresetName, 'n');
    CHECK(state::save(state::fourcc("TEST"), s).size() == state::maxSavedBytes(product));
    // A longer name would not load, so it is not saved.
    s.presetName += 'n';
    CHECK(state::save(state::fourcc("TEST"), s).size() < state::maxSavedBytes(product));
    std::printf("      at most %zu bytes of state in this product\n", state::maxSavedBytes(product));
}

TEST(activation_drains_more_properties_than_the_retirement_queue_holds) {
    const PropertyList many = propertyList(130);
    PropertyStore store(many);
    for (size_t i = 0; i < many.count(); ++i) CHECK(store.set(i, "new"));
    size_t seen = 0;
    store.deliverAll([&](size_t, std::string_view value) {
        CHECK(value == "new");
        ++seen;
    });
    CHECK(seen == many.count());
    for (size_t i = 0; i < many.count(); ++i) CHECK(store.set(i, "next"));
    seen = 0;
    store.deliver([&](size_t, std::string_view value) { CHECK(value == "next"); ++seen; });
    CHECK(seen == 64 && store.needsCollection());
    store.collect();
    CHECK(!store.needsCollection());
    store.deliverAll([&](size_t, std::string_view value) { CHECK(value == "next"); });
}

// --- file references --------------------------------------------------------------

namespace {

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (!value.empty()) setenv(name, value.c_str(), 1);
    else unsetenv(name);
#endif
}

// A scratch folder that is also the settings folder (remembered folders go
// there), and is gone afterwards.
struct FileScratch {
    std::filesystem::path root;
    FileScratch() {
        root = std::filesystem::temp_directory_path() /
               ("supple-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "settings");
        setEnv("SUPPLE_SETTINGS_DIR", files::toUtf8(root / "settings"));
    }
    ~FileScratch() {
        setEnv("SUPPLE_SETTINGS_DIR", "");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::string touch(const std::filesystem::path& relative) const {
        const std::filesystem::path p = (root / relative).make_preferred();  // as relocate() spells it
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p) << "x";
        return files::toUtf8(p);
    }
    std::string wav(const std::filesystem::path& relative, double rate = 44100.0) const {
        const std::filesystem::path p = (root / relative).make_preferred();  // as relocate() spells it
        std::filesystem::create_directories(p.parent_path());
        dsp::AudioData a;
        a.sampleRate = rate;
        a.channel.assign(2, std::vector<float>(static_cast<size_t>(rate / 10)));
        for (size_t n = 0; n < a.channel[0].size(); ++n) a.channel[0][n] = a.channel[1][n] = (n % 100 == 0) ? 0.5f : 0.0f;
        dsp::saveWavFile(files::toUtf8(p), a);
        return files::toUtf8(p);
    }
};

}  // namespace

namespace {
struct SlowFileProbe {
    using FileContents = int;
    static inline std::atomic<bool> started{false}, finish{true};
    static std::unique_ptr<int> loadFile(const FileRequest&, std::string&) {
        started.store(true);
        while (!finish.load()) std::this_thread::yield();
        return std::make_unique<int>(42);
    }
};
}

TEST(file_defaults_load_and_same_session_activation_waits) {
    FileScratch scratch;
    const std::string path = scratch.touch("default.file");
    PropertyList::Builder b;
    b.add(property(1, "File", b.text(path)).file());
    const PropertyList list = b.build();
    FileSlots<SlowFileProbe> slots(list);
    const FileSlots<SlowFileProbe>::Session session{48000, 64, 2};
    slots.prepare(session);
    bool loaded = false;
    slots.deliverAll([&](size_t, const int* p) { loaded = p && *p == 42; });
    CHECK(loaded);
    // A file that could not be made (a full disk) is never loaded, and the
    // wait below would never end.
    if (!files::exists(path)) {
        CHECK_MSG(false, "cannot make %s", path.c_str());
        return;
    }
    SlowFileProbe::started = false;
    SlowFileProbe::finish = false;
    slots.request(0, path, false);
    while (!SlowFileProbe::started.load()) std::this_thread::yield();
    std::atomic<bool> returned{false};
    std::thread activation([&] { slots.prepare(session); returned = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!returned.load());
    SlowFileProbe::finish = true;
    activation.join();
    CHECK(returned.load());
    slots.deliverAll([&](size_t, const int* p) { loaded = p && *p == 42; });
    CHECK(loaded);
}

namespace {
struct KindFileProbe {
    using FileContents = uint32_t;
    static std::unique_ptr<uint32_t> loadFile(const FileRequest& r, std::string&) {
        return std::make_unique<uint32_t>(r.kind);
    }
};
}  // namespace

TEST(a_file_propertys_kind_reaches_load_file) {
    FileScratch scratch;
    const std::string path = scratch.touch("kinds.file");
    PropertyList::Builder b;
    b.add(property(1, "Sample", b.text(path)).file());
    b.add(property(2, "Room", b.text(path)).file().ofKind(7).help("A room's echo."));
    const PropertyList list = b.build();
    CHECK(std::string(list.at(1).description) == "A room's echo.");
    FileSlots<KindFileProbe> slots(list);
    slots.prepare({48000, 64, 2});
    std::vector<uint32_t> kinds(2, 99);
    slots.deliverAll([&](size_t i, const uint32_t* k) { kinds[i] = k ? *k : 99; });
    CHECK(kinds[0] == 0 && kinds[1] == 7);
}

TEST(concurrent_atomic_writes_leave_a_whole_file_and_no_temporary_files) {
    FileScratch s;
    const auto target = s.root / "state";
    const std::vector<uint8_t> a(8192, 'a'), b(8192, 'b');
    auto write = [&](const std::vector<uint8_t>& bytes) {
        for (int i = 0; i < 30; ++i) CHECK(files::writeAtomic(target, bytes.data(), bytes.size()));
    };
    std::thread first([&] { write(a); }), second([&] { write(b); });
    first.join();
    second.join();
    std::vector<uint8_t> found;
    CHECK(files::readBounded(target, found, 8192) && (found == a || found == b));
    CHECK(files::writeAtomic(target, a.data(), a.size()));
    CHECK(files::readBounded(target, found, 8192) && found == a);
    for (const auto& entry : std::filesystem::directory_iterator(s.root))
        CHECK(entry.path() == target || entry.path() == s.root / "settings");
}

TEST(bounded_file_reads_reject_oversized_missing_and_nonregular_files) {
    FileScratch scratch;
    const auto path = files::fromUtf8(scratch.touch("data"));
    std::vector<uint8_t> bytes;
    CHECK(files::readBounded(path, bytes, 1) && bytes == std::vector<uint8_t>{'x'});
    CHECK(!files::readBounded(path, bytes, 0) && bytes.empty());
    CHECK(!files::readBounded(scratch.root, bytes, 1024));
    CHECK(!files::readBounded(scratch.root / "missing", bytes, 1024));
#ifndef _WIN32
    std::filesystem::permissions(path, std::filesystem::perms::none);
    // Elevated test runners may still read it; ordinary CI must reject it.
    if (!std::ifstream(path).is_open()) CHECK(!files::readBounded(path, bytes, 1024));
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
#endif
}

TEST(file_extensions_and_names_whichever_system_saved_them) {
    CHECK(files::hasExtension("/a/b/Kick.WAV", "wav flac"));
    CHECK(files::hasExtension("C:\\x\\y.flac", "wav flac"));
    CHECK(!files::hasExtension("/a/b/notes.txt", "wav flac"));
    CHECK(!files::hasExtension("/a/b/wav", "wav"));
    CHECK(files::hasExtension("/a/b/anything.xyz", ""));
    CHECK(files::fileName("C:\\Users\\ana\\kick.wav") == "kick.wav");
    CHECK(files::fileName("/home/ana/kick.wav") == "kick.wav");
}

TEST(paths_that_are_not_utf8_are_missing_rather_than_thrown) {
    // A damaged session can hold any bytes; Windows cannot make a path of these.
    FileScratch s;
    const std::string bad = "/samples/\xff\xfe\xc0.wav";
    CHECK(!files::exists(bad));
    CHECK(files::relocate(bad).empty());
    CHECK(files::fileName(bad) == "\xff\xfe\xc0.wav");
    (void)files::folderOf(bad);
    (void)PresetLibrary::fromUtf8(bad);
}

TEST(a_moved_file_is_found_by_its_tail_under_remembered_folders) {
    FileScratch s;
    const std::string moved = s.touch("new disk/Library/Samples/Drums/kick.wav");
    const std::string saved = "/Volumes/Old Disk/Library/Samples/Drums/kick.wav";
    CHECK(files::relocate(saved).empty());  // nowhere to look yet
    files::rememberFolder(files::toUtf8(s.root / "new disk"));
    CHECK(files::relocate(saved) == moved);
    // Saved on Windows, the same.
    CHECK(files::relocate("D:\\Library\\Samples\\Drums\\kick.wav") == moved);
    // The deepest tail that matches wins over a same-named file higher up.
    const std::string decoy = s.touch("new disk/kick.wav");
    CHECK(files::relocate(saved) == moved);
    CHECK(files::relocate("/elsewhere/kick.wav") == decoy);
    // A folder the caller adds is tried first.
    const std::string other = s.touch("other/Drums/kick.wav");
    CHECK(files::relocate(saved, {files::toUtf8(s.root / "other")}) == other);
    // An existing path is itself; nothing found is "".
    CHECK(files::relocate(moved) == moved);
    CHECK(files::relocate("/nowhere/at/all.wav").empty());
}

TEST(remembered_folder_overwrite_preserves_other_writers_temporary_files) {
    FileScratch s;
    files::rememberFolder("/first");
    const auto other = s.root / "settings/file-folders.txt.tmp";
    std::ofstream(other) << "other writer";
    files::rememberFolder("/second");
    CHECK(files::searchFolders() == std::vector<std::string>({"/second", "/first"}));
    std::string text;
    std::getline(std::ifstream(other), text);
    CHECK(text == "other writer");
}

TEST(remembered_folders_are_newest_first_and_bounded) {
    FileScratch s;
    for (int k = 0; k < 20; ++k) files::rememberFolder("/folder/" + std::to_string(k));
    files::rememberFolder("/folder/5");
    const std::vector<std::string> folders = files::searchFolders();
    CHECK(folders.size() == 16);
    CHECK(!folders.empty() && folders[0] == "/folder/5");
    CHECK(folders.size() > 1 && folders[1] == "/folder/19");
    CHECK(std::count(folders.begin(), folders.end(), "/folder/5") == 1);
}

TEST(a_file_under_another_users_home_is_found_under_this_ones) {
    FileScratch s;
    const std::string mine = s.touch("home/Music/loop.wav");
#ifdef _WIN32
    const char* homeVar = "USERPROFILE";
#else
    const char* homeVar = "HOME";
#endif
    const char* before = std::getenv(homeVar);
    const std::string saved = before ? before : "";
    setEnv(homeVar, files::toUtf8(s.root / "home"));
    CHECK(files::relocate("/Users/somebody/Music/loop.wav") == mine);
    CHECK(files::relocate("/home/somebody/Music/loop.wav") == mine);
    CHECK(files::relocate("C:\\Users\\somebody\\Music\\loop.wav") == mine);
    setEnv(homeVar, saved);
}

TEST(file_properties_load_relocate_and_report_what_became_of_them) {
    int k = -1;
    for (size_t i = 0; i < properties::kCount && k < 0; ++i)
        if (properties::at(i).isFile() && files::hasExtension("x.wav", properties::at(i).extensions)) k = static_cast<int>(i);
    if (k < 0) {
        std::printf("      (no file property taking WAV in this product)\n");
        return;
    }
    const size_t index = static_cast<size_t>(k);
    FileScratch s;
    using State = FileStatus::State;
    auto e = std::make_unique<Engine>();
    const int in = product::info::kAudioInput ? 2 : 0;
    CHECK(e->fileStatus(index).state == State::Empty);
    // Before the first activation nothing loads; activating loads it.
    const std::string first = s.wav("a/first.wav");
    CHECK(e->setProperty(index, first, true));
    CHECK(e->fileStatus(index).state == State::Loading);
    e->activate(48000.0, 256, in, 2);
    CHECK(e->fileStatus(index).state == State::Ready);
    CHECK(!e->fileStatus(index).summary.empty());
    std::vector<double> l(256, 0.0), r(256, 0.0);
    double* io[2] = {l.data(), r.data()};
    e->process(io, io, 256);
    // Missing: kept, and reported.
    CHECK(e->setProperty(index, "/nowhere/at/all/missing.wav", true));
    CHECK(e->properties().get(index) == "/nowhere/at/all/missing.wav");
    CHECK(e->fileStatus(index).state == State::Missing);
    e->process(io, io, 256);
    // Moved: found, and the property follows it.
    const std::string moved = s.wav("b/Samples/moved.wav");
    files::rememberFolder(files::toUtf8(s.root / "b"));
    CHECK(e->setProperty(index, "/old/place/Samples/moved.wav", true));
    CHECK(e->properties().get(index) == moved);
    CHECK(e->fileStatus(index).state == State::Ready);
    // Not audio: failed, with a reason.
    const std::string bad = s.touch("c/bad.wav");
    CHECK(e->setProperty(index, bad, true));
    CHECK(e->fileStatus(index).state == State::Failed && !e->fileStatus(index).error.empty());
    // A file the user chose: its folder is remembered for next time.
    const std::string chosen = s.wav("d/Kit/snare.wav");
    CHECK(e->setProperty(index, chosen, true));
    e->fileChosen(index);
    CHECK(!files::searchFolders().empty() && files::searchFolders()[0] == files::folderOf(chosen));
    // A new rate reloads it.
    CHECK(e->setProperty(index, moved, true));
    e->deactivate();
    e->activate(96000.0, 256, in, 2);
    CHECK(e->fileStatus(index).state == State::Ready);
    e->process(io, io, 256);
    // Cleared.
    CHECK(e->setProperty(index, "", true));
    CHECK(e->fileStatus(index).state == State::Empty);
    e->process(io, io, 256);
}

TEST(factory_references_name_the_products_own_files) {
    FileScratch s;
    CHECK(files::isFactoryReference("factory:pad.flac"));
    CHECK(!files::isFactoryReference("/samples/factory:pad.flac") && !files::isFactoryReference("pad.flac"));
    // One the product lacks: missing, and never looked for, even where a
    // file of that name lies.
    const std::string decoy = s.wav("library/no-such-factory-file.wav");
    files::rememberFolder(files::toUtf8(s.root / "library"));
    CHECK(files::exists(decoy));
    CHECK(!files::factoryFile("factory:no-such-factory-file.wav"));
    CHECK(!files::exists("factory:no-such-factory-file.wav") && !files::exists("factory:"));
    CHECK(files::relocate("factory:no-such-factory-file.wav").empty());
    dsp::AudioData none;
    std::string error;
    CHECK(!dsp::loadAudioFile("factory:no-such-factory-file.wav", none, &error) && !error.empty());
    // Each the product has: there, as it is.
    std::string audio;
    for (size_t i = 0; i < assets::count(); ++i) {
        const std::string ref = std::string(files::kFactoryPrefix) + std::string(assets::at(i).name);
        CHECK(files::exists(ref) && files::relocate(ref) == ref);
        CHECK(files::factoryFile(ref).data == assets::at(i).data);
        if (audio.empty() && files::hasExtension(ref, kAudioFileTypes)) audio = ref;
    }
    int k = -1;
    for (size_t i = 0; i < properties::kCount && k < 0; ++i)
        if (properties::at(i).isFile() && files::hasExtension(audio, properties::at(i).extensions)) k = static_cast<int>(i);
    if (audio.empty() || k < 0) {
        std::printf("      (no factory audio file and file property taking it in this product)\n");
        return;
    }
    // Decoded as the same bytes on disk are, and loaded by a property.
    const assets::Asset a = files::factoryFile(audio);
    const std::filesystem::path copy = s.root / files::fileName(audio.substr(files::kFactoryPrefix.size()));
    std::ofstream(copy, std::ios::binary).write(reinterpret_cast<const char*>(a.data), static_cast<std::streamsize>(a.size));
    dsp::AudioData fromBinary, fromDisk;
    CHECK(dsp::loadAudioFile(audio, fromBinary) && dsp::loadAudioFile(files::toUtf8(copy), fromDisk));
    CHECK(fromBinary.frames() > 0 && fromBinary.channel == fromDisk.channel);
    auto e = std::make_unique<Engine>();
    const size_t index = static_cast<size_t>(k);
    CHECK(e->setProperty(index, audio, true));
    e->activate(48000.0, 256, product::info::kAudioInput ? 2 : 0, 2);
    CHECK(e->fileStatus(index).state == FileStatus::State::Ready);
    CHECK(e->properties().get(index) == audio);
}

TEST(a_file_loaded_while_the_audio_runs_arrives_whole) {
    int k = -1;
    for (size_t i = 0; i < properties::kCount && k < 0; ++i)
        if (properties::at(i).isFile() && files::hasExtension("x.wav", properties::at(i).extensions)) k = static_cast<int>(i);
    if (k < 0) {
        std::printf("      (no file property taking WAV in this product)\n");
        return;
    }
    FileScratch s;
    auto e = std::make_unique<Engine>();
    e->activate(48000.0, 128, product::info::kAudioInput ? 2 : 0, 2);
    const std::string a = s.wav("one.wav"), b = s.wav("two.wav", 22050.0);
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<double> l(128, 0.0), r(128, 0.0);
        double* io[2] = {l.data(), r.data()};
        while (!stop.load()) {
            std::fill(l.begin(), l.end(), 0.0);
            std::fill(r.begin(), r.end(), 0.0);
            e->process(io, io, 128);
        }
    });
    for (int n = 0; n < 40; ++n) e->setProperty(static_cast<size_t>(k), n % 2 ? a : b, n % 5 == 0);
    e->waitForFiles();
    stop = true;
    audio.join();
    CHECK(e->fileStatus(static_cast<size_t>(k)).state == FileStatus::State::Ready);
    CHECK(e->properties().get(static_cast<size_t>(k)) == a);
}

SUPPLE_TEST_MAIN
