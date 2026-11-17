// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Every stock module is the same program as C++, generated MIR and interpreted MIR.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>

#include "module_harness.h"
#include "restart_check.h"
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

// A locate's restart, through the compiled program's copy (Program::copy):
// every stock module, compiled to the JIT, plays again as when new.
TEST_CASE("every stock module compiled as a script restarts to its prepared state" *
          doctest::description("rt")) {
    int restarted = 0;
    for (const auto* stock : supple::rack::stockModules()) {
        CAPTURE(stock->name);
        auto result = supple::scriptc::compile(stock->source, {});
        REQUIRE_MESSAGE(result.program, supple::scriptc::toText(result.diagnostics));
        auto jit = supple::scriptc::moduleCode(std::move(result.program));
        REQUIRE(jit);
        restarted +=
            supple::rack::test::replaysAfterRestart(std::move(jit), supple::rack::Origin::Library);
    }
    CHECK(restarted > 40);
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

// What prepare() leaves that is not zero is copied back at a restart; what
// is all zero (a buffer it only sized, a delay line nothing was pushed into)
// is kept as its size alone and cleared in place. Both restart exactly.
TEST_CASE("a module whose prepare leaves content restarts to it, and zeros cost no copy" *
          doctest::description("rt")) {
    const std::string source = R"(#include <supple/script.h>
struct Primed : Module {
    static constexpr ModuleInfo info = {
        .name = "Primed",
        .id = "com.suppleaudio.test.primed",
        .vendor = "Supple Audio",
        .description = "prepare() fills a table and a line; a buffer and a line stay zero.",
        .tags = {"test"}};
    Input in = audioIn("In");
    Output out = audioOut("Out");
    Buffer<double> table;
    Buffer<double> scratch;
    DelayLine primed;
    DelayLine fresh;
    int at = 0;
    void prepare() {
        table.resize(4096);
        scratch.resize(4096);
        for (int i = 0; i < 4096; ++i) table[i] = 0.25 + 0.0001 * i;
        primed.prepare(2048);
        fresh.prepare(2048);
        for (int i = 0; i < 300; ++i) primed.push(0.5 - 0.001 * i);
        at = 0;
    }
    void process(int frames) {
        for (int i = 0; i < frames; ++i) {
            const double x = in[0][i];
            const int k = at % 4096;
            const double y = table[k] + scratch[k] + primed.read(250.5) + fresh.read(100.25);
            table[k] = table[k] * 0.5 + x;
            scratch[k] = scratch[k] + x * 0.25;
            primed.push(x);
            fresh.push(y * 0.5);
            out[0][i] = y;
            at = at + 1;
        }
    }
};
)";
    auto result = supple::scriptc::compile(source, {});
    REQUIRE_MESSAGE(result.program, supple::scriptc::toText(result.diagnostics));
    auto jit = supple::scriptc::moduleCode(std::move(result.program));
    REQUIRE(jit);
    {
        // The instance's two buffers, and the prepared table kept beside them:
        // the zero scratch buffer is kept as its size alone.
        const auto type = supple::rack::ModuleType::fromCode(jit, supple::rack::Origin::Project);
        REQUIRE(type);
        const std::size_t before = sd::bufferAllocations();
        auto instance = supple::rack::Instance::make(type, {});
        REQUIRE(instance);
        CHECK(sd::bufferAllocations() == before + 3);
    }
    CHECK(supple::rack::test::replaysAfterRestart(std::move(jit), supple::rack::Origin::Project));
}

namespace {
// A project module's song phase from 3217 on, in calls of `block` frames,
// through the production schedule: `body` is its process() loop's
// expression; `feedback` puts it in a feedback group (a cable into itself).
// `callShift`: how far the clock's callTransport() beats are from its
// transport()'s.
std::vector<double> songPhase(const std::string& body, int block, bool feedback, bool interpret,
                              double callShift = 0) {
    using namespace supple::rack;
    const std::string source =
        "#include <supple/script.h>\n"
        "using namespace supple::script;\n"
        "struct Song : Module {\n"
        "    static constexpr ModuleInfo info = {.name = \"Song\",\n"
        "        .id = \"org.example.song\", .vendor = \"Example\"};\n"
        "    Input dummy = cvIn(\"Dummy\");\n"
        "    Output phase = cvOut(\"Phase\");\n"
        "    void process(int frames) {\n" +
        body +
        "    }\n"
        "};\n";
    auto result = supple::scriptc::compile(source, {.interpret = interpret});
    REQUIRE_MESSAGE(result.program, supple::scriptc::toText(result.diagnostics));
    auto code = supple::scriptc::moduleCode(std::move(result.program));
    REQUIRE(code);
    if (interpret) {
        // MIR's interpreter is a test backend that builds each function's
        // code and its shims into C on the first call (ADR-0032): one block
        // through the harness, off the audio path, does that before the
        // schedule plays the same program there.
        ModuleHarness first(*code);
        first.run(32);
    }
    const auto type = ModuleType::fromCode(std::move(code), Origin::Project);
    REQUIRE(type);
    Instance::Setup setup;
    auto in = Instance::make(type, setup);
    auto out = host::make(host::outType(), setup);
    Patch p;
    p.modules = {{1, in.get(), false, {}}, {2, out.get(), false, {}}};
    if (feedback) p.cables.push_back({0, 0, 0, false, 0, 0.5});
    p.cables.push_back({0, 0, 1, false, 0, 1.0});
    std::string error;
    auto schedule = Schedule::build(p, &error);
    REQUIRE_MESSAGE(schedule, error);
    struct Song final : Clock {
        supple::script::Transport transport(std::int64_t position) override {
            supple::script::Transport t;
            t.playing = true;
            t.tempo = 128;
            t.beats = 0.25 + static_cast<double>(position - 3217) * 128.0 / 60.0 / 48000.0;
            return t;
        }
        supple::script::Transport callTransport(std::int64_t position) override {
            auto t = transport(position);
            t.beats += shift;
            return t;
        }
        double shift = 0;
    } clock;
    clock.shift = callShift;
    std::vector<double> samples;
    for (int at = 0; at < 512;) {
        const int n = std::min(block, 512 - at);
        std::array<float, 1024> l{}, r{};
        static_cast<host::Out*>(out->host())->bind(l.data(), r.data());
        schedule->process(3217 + at, n, clock);
        for (int i = 0; i < n; ++i) samples.push_back(double(l[static_cast<std::size_t>(i)]));
        at += n;
    }
    return samples;
}
}  // namespace

TEST_CASE("module source reading transport() plays as it did however host blocks split the grid") {
    // Round 19: transport() had become the grid interval's start, so source
    // written as the guide used to say, beats + i * perFrame, was off by up
    // to 31 frames' worth whenever a call started off the grid.
    const std::string old =
        "        const Transport t = transport();\n"
        "        for (int i = 0; i < frames; ++i)\n"
        "            phase[0][i] = t.beats + i * t.tempo / 60.0 / sampleRate();\n";
    for (const bool interpret : {false, true}) {
        CAPTURE(interpret);
        const auto whole = songPhase(old, 32, false, interpret);
        const auto split = songPhase(old, 17, false, interpret);
        REQUIRE(whole.size() == split.size());
        double worst = 0;
        for (std::size_t i = 0; i < whole.size(); ++i)
            worst = std::max(worst, std::abs(whole[i] - split[i]));
        CAPTURE(worst);
        CHECK(worst < 1e-7);  // the last bits, as before d82c9569
        CHECK(whole.front() == doctest::Approx(0.25));
    }
}

TEST_CASE("transport() reads the clock's call transport, gridTransport() its exact one") {
    // Review round 20: the host computes transport() as it always did, which
    // can round apart from the exact grid arithmetic in the last bit.
    const std::string call =
        "        for (int i = 0; i < frames; ++i) phase[0][i] = transport().beats;\n";
    const std::string grid =
        "        for (int i = 0; i < frames; ++i) phase[0][i] = gridTransport().beats;\n";
    for (const bool interpret : {false, true}) {
        CAPTURE(interpret);
        CHECK(songPhase(call, 32, false, interpret, 1000.0).front() == 1000.25);
        // The grid interval's start: 3217 is 17 frames into it.
        CHECK(songPhase(grid, 32, false, interpret, 1000.0).front() ==
              doctest::Approx(0.25 - 17 * 128.0 / 60.0 / 48000.0));
    }
}

TEST_CASE("gridTransport() gives exact song positions however host blocks split the grid") {
    const std::string exact =
        "        const Transport t = gridTransport();\n"
        "        for (int i = 0; i < frames; ++i)\n"
        "            phase[0][i] = t.beats + (t.offset + i) * t.tempo / 60.0 / sampleRate();\n";
    for (const bool feedback : {false, true})
        for (const bool interpret : {false, true}) {
            CAPTURE(feedback);
            CAPTURE(interpret);
            const auto whole = songPhase(exact, 32, feedback, interpret);
            const auto split = songPhase(exact, 17, feedback, interpret);
            CHECK(whole == split);
            CHECK(whole.front() == 0.25);
            CHECK(whole.back() > whole.front());
        }
}
