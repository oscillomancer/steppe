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
    Rig() {
        Instance::Setup setup;
        transport = host::make(host::transportType(), setup);
        vca = Instance::make(vcaType(), setup);
        out = host::make(host::outType(), setup);
        Patch p;
        p.modules = {
            {1, transport.get(), false, {}}, {2, vca.get(), false, {}}, {3, out.get(), false, {}}};
        p.cables = {{0, 6, 1, false, 0, 1.0},   // beat phase -> vca in
                    {1, 0, 2, false, 0, 1.0}};  // vca out -> out l
        std::string error;
        schedule = Schedule::build(p, &error);
        REQUIRE_MESSAGE(schedule, error);
    }
    std::vector<float> render(int frames, int block, FixedClock& clock) {
        std::vector<float> l(static_cast<std::size_t>(frames), 0.0f), r = l;
        auto* o = static_cast<host::Out*>(out->host());
        for (int at = 0; at < frames; at += block) {
            const int n = std::min(block, frames - at);
            o->bind(l.data() + at, r.data() + at);
            schedule->process(at, n, clock);
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
