// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// The rack runtime (docs/specs/rack.md §4–5): order, cables, feedback, the
// grid's invariance to the host's block, and the host modules.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "stock_lookup.h"
#include "supple/rack/host_modules.h"
#include "supple/rack/schedule.h"

using namespace supple::rack;

namespace {

struct FixedClock final : Clock {
    supple::script::Transport t;
    double rate = 48000.0;
    std::int64_t origin = 0;
    supple::script::Transport transport(std::int64_t position) override {
        supple::script::Transport x = t;
        if (t.playing) x.beats = t.beats + (position - origin) * t.tempo / 60.0 / rate;
        return x;
    }
};

std::shared_ptr<const ModuleType> vcaType() {
    static const auto t = ModuleType::fromCode(
        std::shared_ptr<const sd::ModuleCode>(&test::stock("vca"), [](const sd::ModuleCode*) {}),
        Origin::Stock);
    return t;
}

// Transport's beat output into a VCA's input, the VCA into Out: renders a
// gate pattern into floats.
struct Rig {
    std::unique_ptr<Instance> transport, vca, out;
    std::unique_ptr<Schedule> schedule;
    explicit Rig(ParamCurve* level = nullptr) {
        Instance::Setup setup;
        transport = host::make(host::transportType(), setup);
        vca = Instance::make(vcaType(), setup);
        out = host::make(host::outType(), setup);
        Patch p;
        p.modules = {
            {1, transport.get(), false, {}}, {2, vca.get(), false, {}}, {3, out.get(), false, {}}};
        if (level) p.modules[1].automation.emplace_back(0, level);
        p.cables = {{0, 6, 1, false, 0, 1.0},   // beat phase -> vca in
                    {1, 0, 2, false, 0, 1.0}};  // vca out -> out l
        std::string error;
        schedule = Schedule::build(p, &error);
        REQUIRE_MESSAGE(schedule, error);
    }
    std::vector<float> render(int frames, int block, FixedClock& clock, std::int64_t origin = 0) {
        std::vector<float> l(static_cast<std::size_t>(frames), 0.0f), r = l;
        auto* o = static_cast<host::Out*>(out->host());
        for (int at = 0; at < frames; at += block) {
            const int n = std::min(block, frames - at);
            o->bind(l.data() + at, r.data() + at);
            schedule->process(origin + at, n, clock);
        }
        return l;
    }
};

}  // namespace

TEST_CASE("rack: modules run in cable order and Out takes the audio") {
    Rig rig;
    CHECK(rig.schedule->steps().size() == 3);
    FixedClock clock;
    clock.t.playing = true;
    clock.t.tempo = 120;
    const std::vector<float> l = rig.render(48000, 64, clock);
    // The beat phase ramps 0..1 every half second at 120 BPM.
    CHECK(l[6000] == doctest::Approx(0.25).epsilon(1e-3));
    CHECK(l[23999] == doctest::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("rack: the output does not depend on the host's block") {
    FixedClock clock;
    clock.t.playing = true;
    clock.t.tempo = 133;
    Rig a, b, c;
    const auto x = a.render(20000, 1024, clock);
    const auto y = b.render(20000, 7, clock);
    const auto z = c.render(20000, 333, clock);
    CHECK(x == y);
    CHECK(x == z);
}

namespace {
struct LevelCurve final : ParamCurve {
    double offset = 0;
    int calls = 0;
    bool offGrid = false;
    double at(std::int64_t position) override {
        ++calls;
        offGrid |= position % kGrid != 0;
        return -20.0 + offset + static_cast<double>(position) * 0.0002;
    }
};
}  // namespace

TEST_CASE("rack: smoothed automation holds across host block fragments" *
          doctest::description("rt")) {
    for (std::int64_t origin : {0, 7, -7}) {
        INFO("origin " << origin);
        FixedClock clock;
        clock.t.playing = true;
        clock.t.tempo = 133;
        LevelCurve a, b;
        Rig large(&a), small(&b);
        const auto x = large.render(20000, 1024, clock, origin);
        const auto y = small.render(20000, 37, clock, origin);
        CHECK(x == y);
        CHECK_FALSE(a.offGrid);
        CHECK_FALSE(b.offGrid);
        CHECK(a.calls == b.calls);
    }
}

TEST_CASE("rack: automation resamples after a locate within the same grid interval" *
          doctest::description("rt")) {
    LevelCurve curve;
    Rig rig(&curve);
    FixedClock clock;
    rig.schedule->process(0, 7, clock);
    CHECK(rig.vca->paramMember(0).value() == -20.0);
    curve.offset = 10;
    rig.schedule->process(7, 0, clock);
    rig.schedule->process(7, 1, clock);  // contiguous: hold this interval's target
    CHECK(rig.vca->paramMember(0).value() == -20.0);
    CHECK(curve.calls == 1);
    rig.schedule->process(13, 1, clock);  // locate: sample the containing interval again
    CHECK(rig.vca->paramMember(0).value() == -10.0);
    CHECK(curve.calls == 2);
    CHECK_FALSE(curve.offGrid);
}

TEST_CASE("rack: a loop runs frame by frame with one frame of delay") {
    Instance::Setup setup;
    auto a = Instance::make(vcaType(), setup);
    auto b = Instance::make(vcaType(), setup);
    auto in = host::make(host::inType(), setup);
    Patch p;
    // in -> a; a -> b; b -> a (feedback: b's id is higher, so a reads it late)
    p.modules = {{1, in.get(), false, {}}, {2, a.get(), false, {}}, {3, b.get(), false, {}}};
    p.cables = {{0, 0, 1, false, 0, 1.0}, {1, 0, 2, false, 0, 1.0}, {2, 0, 1, false, 0, 1.0}};
    a->setParam(0, -6.0);  // level: the loop decays
    b->setParam(0, -6.0);
    auto s = Schedule::build(p);
    REQUIRE(s);
    CHECK(s->feedback(2));
    CHECK_FALSE(s->feedback(1));
    std::vector<float> impulse(64, 0.0f), silent(64, 0.0f);
    impulse[0] = 1.0f;
    static_cast<host::In*>(in->host())->bind(impulse.data(), silent.data());
    FixedClock clock;
    s->process(0, 32, clock);
    const double g = std::pow(10.0, -6.0 / 20.0);
    const double* out = s->output(2, 0);  // b's output
    // a(0) = g, b(0) = g^2; a(1) = g * b(0), b(1) = g^4 ...
    CHECK(out[0] == doctest::Approx(g * g));
    CHECK(out[1] == doctest::Approx(g * g * g * g));
}

TEST_CASE("rack: Notes turns MIDI into poly pitch and gates") {
    Instance::Setup setup;
    auto notes = host::make(host::notesType(), setup);
    Patch p;
    p.modules = {{1, notes.get(), false, {}}};
    auto s = Schedule::build(p);
    REQUIRE(s);
    std::vector<host::MidiEvent> events = {
        {0, 0x90, 72, 100}, {10, 0x90, 55, 64}, {20, 0x80, 72, 0}};
    static_cast<host::Notes*>(notes->host())->bind(events);
    FixedClock clock;
    s->process(0, 32, clock);
    CHECK(s->outputChannels(0, 0) == 8);
    const double* pitch = s->output(0, 0);
    const double* gate = s->output(0, 1);
    CHECK(pitch[5] == doctest::Approx(1.0));                 // C5: an octave up
    CHECK(pitch[kGrid + 15] == doctest::Approx(-5.0 / 12));  // G3 on channel 1
    CHECK(gate[19] == 1.0);
    CHECK(gate[21] == 0.0);
    CHECK(gate[kGrid + 21] == 1.0);
}

TEST_CASE("rack: Transport's clock counts the divisions") {
    Instance::Setup setup;
    auto transport = host::make(host::transportType(), setup);
    transport->setParam(0, 4);  // 1/16
    Patch p;
    p.modules = {{1, transport.get(), false, {}}};
    auto s = Schedule::build(p);
    REQUIRE(s);
    FixedClock clock;
    clock.t.playing = true;
    clock.t.tempo = 120;  // a sixteenth is 6000 frames
    int rises = 0;
    double last = 0;
    for (int at = 0; at < 48000; at += 32) {
        s->process(at, 32, clock);
        const double* c = s->output(0, 4);
        for (int i = 0; i < 32; ++i) {
            if (c[i] >= 0.5 && last < 0.5) ++rises;
            last = c[i];
        }
    }
    CHECK(rises == 8);  // one second: eight sixteenths, the first at 0
}

namespace {
class PolyLevel final : public HostModule {
  public:
    double level = -0.75;
    void process(const HostContext& ctx) SUPPLE_RT override {
        auto& out = ctx.instance->outputMember(0);
        out.setChannels(2);
        for (int f = 0; f < ctx.frames; ++f) {
            out[0][f] = 0.0;
            out[1][f] = level;
        }
    }
};
}  // namespace

TEST_CASE("rack: output levels cover every poly channel without a telemetry watch" *
          doctest::description("rt")) {
    sd::ModuleInfoModel info;
    info.id = "com.example.poly-level";
    info.name = "Poly level";
    sd::PortModel out;
    out.key = out.name = "out";
    out.output = true;
    auto host = std::make_unique<PolyLevel>();
    auto* source = host.get();
    auto instance = Instance::make(ModuleType::host(info, {}, {out}), std::move(host), {});
    REQUIRE(instance);
    CHECK_FALSE(instance->watched());
    CHECK(instance->outputLevel(0) == 0.0f);
    CHECK(instance->outputLevel(-1) == 0.0f);
    CHECK(instance->outputLevel(1) == 0.0f);
    Patch patch;
    patch.modules = {{1, instance.get(), false, {}}};
    auto schedule = Schedule::build(patch);
    REQUIRE(schedule);
    FixedClock clock;
    schedule->process(0, 37, clock);
    CHECK(instance->outputLevel(0) == 0.75f);
    CHECK(instance->outputLevel(0) == 0.75f);  // reading never consumes it
    CHECK(schedule->output(0, 0)[0] == 0.0);
    CHECK(schedule->output(0, 0)[kGrid] == -0.75);
    source->level = 2.0;
    instance->watch(true);
    schedule->process(37, 37, clock);
    CHECK(instance->outputLevel(0) == 2.0f);
    source->level = 0.0;
    instance->watch(false);
    schedule->process(74, 37, clock);
    CHECK(instance->outputLevel(0) == 0.0f);
    source->level = -0.25;
    schedule->process(111, 37, clock);
    CHECK(instance->outputLevel(0) == 0.25f);
    instance->reset();
    CHECK(instance->outputLevel(0) == 0.0f);
}

namespace {
class NoteSignal final : public HostModule {
  public:
    void process(const HostContext& ctx) SUPPLE_RT override {
        auto& pitch = ctx.instance->outputMember(0);
        auto& gate = ctx.instance->outputMember(1);
        auto& velocity = ctx.instance->outputMember(2);
        pitch.setChannels(1);
        gate.setChannels(2);
        velocity.setChannels(1);
        for (int i = 0; i < ctx.frames; ++i) {
            const auto at = ctx.position + i;
            pitch[0][i] = at < 10 ? 0.5 : 4.0;  // note-offs retain their rise's pitch
            gate[0][i] = at < 5 || at >= 12 ? 0.249 : at == 5 ? 0.5 : 0.3;
            gate[1][i] = at >= 8 && at < 20 ? 1.0 : 0.0;
            velocity[0][i] = 0.5;
        }
    }
};
}  // namespace

TEST_CASE("rack: Note Out shares capture rules, latches its notes and ends them on locate" *
          doctest::description("rt")) {
    sd::ModuleInfoModel info;
    info.id = "com.example.note-signal";
    info.name = "Note signal";
    std::vector<sd::PortModel> ports(3);
    for (int k = 0; k < 3; ++k) {
        ports[static_cast<std::size_t>(k)].key = k == 0 ? "pitch" : k == 1 ? "gate" : "velocity";
        ports[static_cast<std::size_t>(k)].name = ports[static_cast<std::size_t>(k)].key;
        ports[static_cast<std::size_t>(k)].output = true;
    }
    auto signal =
        Instance::make(ModuleType::host(info, {}, ports), std::make_unique<NoteSignal>(), {});
    CHECK(host::notesOutType()->problems().empty());
    auto output = host::make(host::notesOutType(), {});
    REQUIRE(output);
    output->setParam(0, 3);
    auto* notes = static_cast<host::NotesOut*>(output->host());
    Patch patch;
    patch.modules = {{1, signal.get(), false, {}}, {2, output.get(), false, {}}};
    patch.cables = {{0, 0, 1, false, 0, 1}, {0, 1, 1, false, 1, 1}, {0, 2, 1, false, 2, 1}};
    SUBCASE("connected mono velocity is shared by the voices") {}
    SUBCASE("unconnected velocity defaults to 96") { patch.cables.pop_back(); }
    auto schedule = Schedule::build(patch);
    REQUIRE(schedule);
    FixedClock clock;
    clock.t.playing = true;
    std::array<host::NotesOut::Event, 128> events{};
    std::size_t count = 0;
    notes->bind(events, count);
    schedule->process(0, 37, clock);
    REQUIRE(count == 4);
    CHECK(events[0].midi.frame == 5);
    CHECK(events[0].midi.status == 0x92);
    CHECK(events[0].midi.data1 == 66);
    CHECK(events[0].midi.data2 == (patch.cables.size() == 3 ? 64 : 96));
    CHECK(events[1].midi.frame == 8);
    CHECK(events[1].voice == 1);
    CHECK(events[2].midi.frame == 12);
    CHECK(events[2].midi.status == 0x82);
    CHECK(events[2].midi.data1 == 66);
    CHECK(events[3].midi.frame == 20);
    CHECK(host::noteKey(-100) == 0);
    CHECK(host::noteKey(100) == 127);
    CHECK(host::noteVelocity(-1, true) == 1);
    CHECK(host::noteVelocity(2, true) == 127);
    // A locate while gates are high ends them, then starts the held gate anew.
    notes->bind(events, count);
    schedule->process(5, 4, clock);
    REQUIRE(count == 2);
    output->setParam(0, 7);  // held notes still end on channel 3
    notes->endAll();
    notes->bind(events, count);
    schedule->process(9, 1, clock);
    REQUIRE(count == 3);
    CHECK(events[0].midi.status == 0x82);
    CHECK(events[1].midi.status == 0x82);
    CHECK(events[2].midi.status == 0x96);
    clock.t.playing = false;
    notes->bind(events, count);
    schedule->process(10, 4, clock);
    REQUIRE(count == 1);
    CHECK(events[0].midi.status == 0x86);
    notes->bind(events, count);
    schedule->process(14, 4, clock);
    CHECK(count == 0);
}

TEST_CASE("rack: Notes keeps equal-pitch MIDI sources independent in every allocation mode") {
    for (int mode = 0; mode < 6; ++mode) {
        CAPTURE(mode);
        auto notes = host::make(host::notesType(), Instance::Setup{});
        notes->setParam(1, mode);
        Patch patch;
        patch.modules = {{1, notes.get(), false, {}}};
        auto schedule = Schedule::build(patch);
        REQUIRE(schedule);
        std::array<host::MidiEvent, 4> events{{{0, 0x90, 60, 100, false, 11, 7},
                                               {1, 0x90, 60, 80, false, 12, 8},
                                               {10, 0x80, 60, 0, false, 11, 7},
                                               {20, 0x80, 60, 0, false, 12, 8}}};
        static_cast<host::Notes*>(notes->host())->bind(events);
        FixedClock clock;
        schedule->process(0, 32, clock);
        const auto* gate = schedule->output(0, 1);
        auto gates = [&](int frame) {
            double total = 0;
            for (int voice = 0; voice < schedule->outputChannels(0, 1); ++voice)
                total += gate[voice * kGrid + frame];
            return total;
        };
        CHECK(gates(5) == (mode >= 3 ? 1 : 2));
        CHECK(gates(15) == 1);
        CHECK(gates(25) == 0);
    }
}

TEST_CASE("rack: Notes source off releases the track while preserving route inputs") {
    for (int mode : {0, 3, 4, 5}) {
        CAPTURE(mode);
        auto notes = host::make(host::notesType(), Instance::Setup{});
        notes->setParam(1, mode);
        auto* input = static_cast<host::Notes*>(notes->host());
        input->trackInput(1);
        Patch patch;
        patch.modules = {{1, notes.get(), false, {}}};
        auto schedule = Schedule::build(patch);
        REQUIRE(schedule);
        std::array<host::MidiEvent, 2> events{
            {{0, 0x90, 60, 100, false, 12, 8}, {1, 0x90, 60, 80, false, 0, 3}}};
        input->bind(events);
        FixedClock clock;
        schedule->process(0, 32, clock);
        input->bind({});
        input->trackInput(UINT64_MAX);
        schedule->process(32, 32, clock);
        double gates = 0;
        for (int voice = 0; voice < schedule->outputChannels(0, 1); ++voice)
            gates += schedule->output(0, 1)[voice * kGrid + 5];
        CHECK(gates == 1);
        input->retainRoutes({});
        schedule->process(64, 32, clock);
        gates = 0;
        for (int voice = 0; voice < schedule->outputChannels(0, 1); ++voice)
            gates += schedule->output(0, 1)[voice * kGrid + 5];
        CHECK(gates == 0);
    }
}
