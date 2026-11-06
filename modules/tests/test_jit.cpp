// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Every stock module is the same program as C++, generated MIR and interpreted MIR.
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>

#include "module_harness.h"
#include "supple/rack/stock.h"
#include "supple/script_compiler.hpp"
#include "supple/script_program.hpp"

namespace supple::script::array_test {
#include "../../plugins/suite/compiler/tests/module_arrays.h"
}

namespace {
namespace sd = supple::script::detail;
using supple::rack::test::ModuleHarness;

// Every public parameter property, excluding C++'s unavailable member name
// and each compiler's private storage offset.
auto description(const sd::ParamModel& p) {
    return std::tie(p.key, p.id, p.kind, p.name, p.min, p.max, p.def, p.unit, p.curve, p.curveK,
                    p.choices, p.choiceKeys, p.group, p.description, p.smoothingMs, p.automatable,
                    p.modulatable, p.offAtMinimum, p.output, p.bypass, p.hidden, p.polyModulatable,
                    p.restarts, p.roles, p.activeWhen, p.routeMin, p.routeMax);
}

void sameDescription(const sd::ModuleCode& native, const sd::ModuleCode& jit) {
    CHECK(native.info() == jit.info());
    REQUIRE(native.params().size() == jit.params().size());
    for (std::size_t i = 0; i < native.params().size(); ++i) {
        CAPTURE(native.params()[i].key);
        CHECK(description(native.params()[i]) == description(jit.params()[i]));
        CHECK_FALSE(jit.params()[i].member.empty());
    }
    REQUIRE(native.ports().size() == jit.ports().size());
    for (std::size_t i = 0; i < native.ports().size(); ++i) {
        auto a = native.ports()[i], b = jit.ports()[i];
        CHECK_FALSE(b.member.empty());
        a.member.clear();
        b.member.clear();
        a.offset = b.offset = 0;
        CHECK(a == b);
    }
    REQUIRE(native.shared().size() == jit.shared().size());
    for (std::size_t i = 0; i < native.shared().size(); ++i)
        CHECK(native.shared()[i].bytes == jit.shared()[i].bytes);
    CHECK(native.factoryPatches() == jit.factoryPatches());
    CHECK(bool(native.prepare()) == bool(jit.prepare()));
    CHECK(bool(native.update()) == bool(jit.update()));
    CHECK(bool(native.process()) == bool(jit.process()));
    CHECK(bool(native.panel()) == bool(jit.panel()));
    CHECK(bool(native.draw()) == bool(jit.draw()));
    CHECK(bool(native.pointer()) == bool(jit.pointer()));
}

double signal(sd::PortModel::Signal kind, int c, std::int64_t n) {
    const double phase = static_cast<double>((n + c * 137) % 997) / 997.0;
    switch (kind) {
        case sd::PortModel::Signal::Pitch:
            return -1.0 + c * 0.25;
        case sd::PortModel::Signal::Gate:
            return (n % 811) < 400 ? 1.0 : 0.0;
        case sd::PortModel::Signal::Unipolar:
        case sd::PortModel::Signal::Phase:
            return phase;
        default:
            return std::sin(static_cast<double>(n) * (0.017 + 0.003 * c)) * 0.7;
    }
}

void drive(ModuleHarness& h, const sd::ModuleCode& code, int channels, int stage) {
    for (const auto& p : code.ports()) {
        if (p.output) continue;
        if (stage == 0) {
            h.disconnect(p.key);
            continue;
        }
        const auto kind = p.signal;
        h.input(p.key, stage == 2 ? 1 : channels,
                [kind](int c, std::int64_t n) { return signal(kind, c, n); });
    }
    for (const auto& p : code.params()) {
        if (p.output) continue;
        double value = stage == 0 ? p.def : p.min + (p.max - p.min) * (stage == 1 ? 0.37 : 0.71);
        if (p.kind != sd::ParamModel::Kind::Continuous) value = std::round(value);
        h.set(p.key, value);
        if (p.kind != sd::ParamModel::Kind::Toggle)
            h.modulate(
                p.key,
                stage == 0   ? 0
                : stage == 2 ? 1
                             : channels,
                [](int c, std::int64_t n) { return std::sin(n * 0.023 + c) * 0.5; },
                p.unit == sd::ParamModel::Unit::Hertz ? 1.0 : (p.max - p.min) * 0.2);
    }
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 137;
    t.beats = stage * 4.0;
    t.barStart = stage * 4.0;
    h.transport(t);
}

void sameAudio(const ModuleHarness& a, const ModuleHarness& b, const sd::ModuleCode& code) {
    CHECK(a.status().budgetExceeded.load() == 0);
    CHECK(b.status().budgetExceeded.load() == 0);
    CHECK(a.status().outOfRange.load() == 0);
    CHECK(b.status().outOfRange.load() == 0);
    for (const auto& p : code.ports()) {
        if (!p.output) continue;
        CAPTURE(p.key);
        CHECK(a.channels(p.key) == b.channels(p.key));
        for (int c = 0; c < ModuleHarness::kChannels; ++c) {
            const auto& x = a.output(p.key, c);
            const auto& y = b.output(p.key, c);
            REQUIRE(x.size() == y.size());
            bool identical = true;
            for (std::size_t i = 0; i < x.size(); ++i)
                identical = identical && std::bit_cast<std::uint64_t>(x[i]) ==
                                             std::bit_cast<std::uint64_t>(y[i]);
            CHECK(identical);
        }
    }
}
}  // namespace

TEST_CASE("every stock module agrees with JIT and MIR interpreter") {
    REQUIRE_FALSE(supple::rack::stockModules().empty());
    for (const auto* stock : supple::rack::stockModules()) {
        CAPTURE(stock->name);
        for (bool interpret : {false, true}) {
            CAPTURE(interpret);
            auto result = supple::scriptc::compile(stock->source, {.interpret = interpret});
            REQUIRE_MESSAGE(result.program, supple::scriptc::toText(result.diagnostics));
            REQUIRE(result.program->kind() == supple::scriptc::PluginKind::Module);
            auto jit = supple::scriptc::moduleCode(std::move(result.program));
            REQUIRE(jit);
            sameDescription(*stock->code, *jit);
            for (int channels : {1, 4}) {
                CAPTURE(channels);
                ModuleHarness native(*stock->code), compiled(*jit);
                for (int stage = 0; stage < 3; ++stage) {
                    drive(native, *stock->code, channels, stage);
                    drive(compiled, *jit, channels, stage);
                    native.run(1024);
                    compiled.run(1024);
                    sameAudio(native, compiled, *jit);
                }
            }
        }
    }
}

TEST_CASE(
    "module arrays, bit operations, gate helpers and reference arguments agree in all backends") {
    using T = supple::script::array_test::ArrayModule;
    const sd::NativeModuleCode<T> native(
        {{&T::pitch, "pitch"}, {&T::enabled, "enabled"}, {&T::amount, "amount"}},
        {{&T::signalIn, "signal_in"},
         {&T::signalOut, "signal_out"},
         {&T::gate, "gate"},
         {&T::triggerOut, "trigger_out"}});
    REQUIRE(native.valid());
    const auto sourcePath =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "plugins/suite/compiler/tests/module_arrays.h";
    std::ifstream in(sourcePath);
    REQUIRE(in);
    std::stringstream source;
    source << in.rdbuf();
    for (bool interpret : {false, true}) {
        auto result = supple::scriptc::compile(source.str(), {.interpret = interpret});
        REQUIRE_MESSAGE(result.program, supple::scriptc::toText(result.diagnostics));
        auto jit = supple::scriptc::moduleCode(result.program);
        sameDescription(native, *jit);
        for (int channels : {1, 4}) {
            ModuleHarness a(native), b(*jit);
            for (int stage = 0; stage < 3; ++stage) {
                drive(a, native, channels, stage);
                drive(b, *jit, channels, stage);
                a.run(1024);
                b.run(1024);
                sameAudio(a, b, *jit);
            }
        }
    }
}
