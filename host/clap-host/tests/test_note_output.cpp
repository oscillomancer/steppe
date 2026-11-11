// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Plug-in note and MIDI output (ADR-0068): what CLAP and VST3 plug-ins emit
// reaches the caller's sink as the same events, in the same order and with
// the same counts, in process, in a shared bridge and in a bridge of its own.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "../src/midi_output.h"
#include "supple/host/clap_host.h"

using namespace supple;
using namespace supple::host;

namespace {
const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
const std::filesystem::path kVst3Fixture = FIXTURE_VST3_PATH;
using Kind = OutputEvent::Kind;

PluginInfo findClap(const std::string& id) {
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    for (auto& p : *list)
        if (p.id == id) return p;
    FAIL("fixture plugin not found: ", id);
    return {};
}

PluginInfo findVst3(const std::string& name) {
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    for (auto& p : *list)
        if (p.name == name) return p;
    FAIL("VST3 fixture not found: ", name);
    return {};
}

// The three ways an instance runs: in process, in a bridge shared with
// another instance, and in a bridge of its own.
enum class Isolation { Local, Shared, Own };
constexpr Isolation kIsolations[] = {Isolation::Local, Isolation::Shared, Isolation::Own};
const char* label(Isolation i) {
    return i == Isolation::Local ? "local" : i == Isolation::Shared ? "shared" : "own";
}

struct Running {
    std::shared_ptr<BridgeProcess> bridge;
    std::unique_ptr<Instance> neighbour;  // shares the bridge
    std::unique_ptr<Instance> instance;
};

Running start(const PluginInfo& info, Isolation isolation, std::uint32_t maxFrames = 64) {
    setBridgeExecutable(BRIDGE_PATH);
    Running r;
    if (isolation == Isolation::Shared) {
        auto bridge = createBridgeProcess();
        REQUIRE(bridge);
        r.bridge = *bridge;
        auto neighbour =
            Instance::create(findClap("com.suppleaudio.test.gain"), {}, true, r.bridge);
        REQUIRE(neighbour);
        r.neighbour = std::move(*neighbour);
    }
    auto made = Instance::create(info, {}, isolation != Isolation::Local, r.bridge);
    REQUIRE_MESSAGE(made, (made ? "" : made.error().message));
    r.instance = std::move(*made);
    REQUIRE(r.instance->activate(48000, 1, maxFrames));
    REQUIRE(r.instance->startProcessing());
    return r;
}

struct Buffers {
    std::vector<float> l, r, ol, orr;
    float* in[2];
    float* out[2];
    explicit Buffers(std::uint32_t n) : l(n), r(n), ol(n), orr(n) {
        in[0] = l.data();
        in[1] = r.data();
        out[0] = ol.data();
        out[1] = orr.data();
    }
    ProcessBlock block(std::uint32_t n, std::span<const Event> events) {
        ProcessBlock b;
        b.inputs = in;
        b.inputChannels = 2;
        b.outputs = out;
        b.outputChannels = 2;
        b.frames = n;
        b.events = events;
        return b;
    }
};

bool rtProcess(Instance& p, const ProcessBlock& b, OutputEvents& out) SUPPLE_RT {
    return p.process(b, &out);
}

Event noteOn(std::uint32_t time, std::int16_t key, std::int32_t id = -1, double velocity = 1.0,
             std::int16_t channel = 0) {
    return {.kind = Event::Kind::NoteOn,
            .time = time,
            .port = 0,
            .channel = channel,
            .key = key,
            .noteId = id,
            .velocity = velocity};
}

Event noteOff(std::uint32_t time, std::int16_t key, std::int32_t id = -1,
              std::int16_t channel = 0) {
    Event e = noteOn(time, key, id, 0.0, channel);
    e.kind = Event::Kind::NoteOff;
    return e;
}

// Every field, values bit for bit.
bool same(const OutputEvent& a, const OutputEvent& b) {
    return a.kind == b.kind && std::memcmp(a.midi, b.midi, 3) == 0 && a.time == b.time &&
           a.paramId == b.paramId && std::memcmp(&a.value, &b.value, sizeof(double)) == 0 &&
           a.noteId == b.noteId && a.flags == b.flags && a.port == b.port &&
           a.channel == b.channel && a.key == b.key;
}

// One block's output.
struct Captured {
    std::vector<OutputEvent> events;
    std::size_t dropped = 0, unsupported = 0;
    bool operator==(const Captured& o) const {
        return dropped == o.dropped && unsupported == o.unsupported &&
               std::equal(events.begin(), events.end(), o.events.begin(), o.events.end(), same);
    }
};

Captured run(Instance& p, std::span<const Event> events, std::size_t capacity,
             std::uint32_t frames = 64) {
    Buffers buffers(frames);
    std::vector<OutputEvent> storage(capacity);
    OutputEvents sink{storage};
    REQUIRE(rtProcess(p, buffers.block(frames, events), sink));
    Captured c;
    c.events.assign(storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(sink.count));
    c.dropped = sink.dropped;
    c.unsupported = sink.unsupported;
    return c;
}

bool midi(const OutputEvent& e, std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    return e.kind == Kind::Midi && e.midi[0] == a && e.midi[1] == b && e.midi[2] == c;
}

}  // namespace

TEST_CASE("plug-ins declare note output through note ports and event buses") {
    setBridgeExecutable(BRIDGE_PATH);
    for (const bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto echo = Instance::create(findClap("com.suppleaudio.test.note-echo"), {}, isolated);
        REQUIRE(echo);
        CHECK((*echo)->acceptsNotes());
        CHECK((*echo)->outputsNotes());
        auto sine = Instance::create(findClap("com.suppleaudio.test.sine"), {}, isolated);
        REQUIRE(sine);
        CHECK((*sine)->acceptsNotes());
        CHECK_FALSE((*sine)->outputsNotes());
        auto gain = Instance::create(findClap("com.suppleaudio.test.gain"), {}, isolated);
        REQUIRE(gain);
        CHECK_FALSE((*gain)->outputsNotes());
        auto vst3Echo = Instance::create(findVst3("Test VST3 Note Echo"), {}, isolated);
        REQUIRE(vst3Echo);
        CHECK((*vst3Echo)->acceptsNotes());
        CHECK((*vst3Echo)->outputsNotes());
        auto vst3Sine = Instance::create(findVst3("Test VST3 Sine"), {}, isolated);
        REQUIRE(vst3Sine);
        CHECK_FALSE((*vst3Sine)->outputsNotes());
    }
}

TEST_CASE("a CLAP plug-in's note and MIDI output reaches the sink in every isolation") {
    const auto info = findClap("com.suppleaudio.test.note-echo");
    const Event in[] = {noteOn(5, 60, 11, 0.5, 2), noteOff(40, 60, 11, 2)};
    Captured reference;
    for (const auto isolation : kIsolations) {
        CAPTURE(label(isolation));
        auto r = start(info, isolation);
        const auto got = run(*r.instance, in, 16);
        REQUIRE(got.events.size() == 3);
        CHECK(got.dropped == 0);
        CHECK(got.unsupported == 0);
        const auto& on = got.events[0];
        CHECK(on.kind == Kind::NoteOn);
        CHECK(on.time == 5);
        CHECK(on.port == 0);
        CHECK(on.channel == 2);
        CHECK(on.key == 67);
        CHECK(on.noteId == 11);
        CHECK(on.value == 0.5);
        CHECK(on.midiOutput());
        CHECK(got.events[1].time == 5);  // the CC follows its note at the same time
        CHECK(midi(got.events[1], 0xb2, 74, 64));
        CHECK(got.events[1].port == 0);
        CHECK(got.events[2].kind == Kind::NoteOff);
        CHECK(got.events[2].time == 40);
        CHECK(got.events[2].key == 67);
        CHECK(got.events[2].noteId == 11);
        if (isolation == Isolation::Local)
            reference = got;
        else
            CHECK(got == reference);  // bit-identical events and counts
        r.instance->stopProcessing();
        r.instance->deactivate();
    }
}

TEST_CASE("note output is sorted, clamped to the block, mapped to MIDI 1.0 or counted") {
    const auto info = findClap("com.suppleaudio.test.note-echo");
    const Event in[] = {noteOn(0, 120)};  // the burst (fixture_plugins.cpp, noteEchoStress)
    Captured reference;
    for (const auto isolation : kIsolations) {
        CAPTURE(label(isolation));
        auto r = start(info, isolation);
        const auto got = run(*r.instance, in, 32);
        CHECK(got.dropped == 0);
        // Per-note pitch bend (MIDI 2.0), SysEx, a tuning expression and a
        // malformed MIDI message have no MIDI 1.0 form.
        CHECK(got.unsupported == 4);
        REQUIRE(got.events.size() == 8);
        const auto& e = got.events;
        CHECK(e[0].time == 0);
        CHECK(midi(e[0], 0xb0, 7, 100));
        CHECK(e[1].time == 1);  // three at time 1, in the order they were pushed
        CHECK(e[1].kind == Kind::NoteChoke);
        CHECK(e[1].channel == -1);
        CHECK(e[1].key == -1);
        CHECK(e[1].value == 0);
        CHECK(e[2].time == 1);
        CHECK(midi(e[2], 0xe3, 0x00, 0x40));  // MIDI 2.0 pitch bend, centre
        CHECK(e[3].time == 1);
        CHECK(midi(e[3], 0xf8, 0, 0));  // clock
        CHECK(e[4].time == 2);
        CHECK(midi(e[4], 0xa2, 64, 64));  // a pressure expression: poly pressure
        CHECK(e[5].time == 3);
        CHECK(midi(e[5], 0xb0, 10, 0x40));  // MIDI 2.0 CC, top 7 bits
        CHECK(e[6].time == 63);
        CHECK(e[6].kind == Kind::NoteOn);
        CHECK(e[6].key == 64);
        CHECK(e[6].value == 0.75);
        CHECK(e[7].time == 63);  // emitted past the block: placed on its last frame
        CHECK(e[7].kind == Kind::NoteOn);
        CHECK(e[7].key == 65);
        CHECK(std::is_sorted(e.begin(), e.end(),
                             [](const auto& a, const auto& b) { return a.time < b.time; }));
        if (isolation == Isolation::Local)
            reference = got;
        else
            CHECK(got == reference);
        r.instance->stopProcessing();
        r.instance->deactivate();
    }
}

TEST_CASE("note output overflow drops and counts the same events in every isolation") {
    const auto info = findClap("com.suppleaudio.test.note-echo");
    const Event burst[] = {noteOn(0, 120)};
    const Event flood[] = {noteOn(10, 121)};
    Captured burstReference, floodReference;
    for (const auto isolation : kIsolations) {
        CAPTURE(label(isolation));
        auto r = start(info, isolation);
        // Room for four: the first four pushes that map are kept (then sorted),
        // the four after them are dropped; unsupported ones take no room.
        const auto small = run(*r.instance, burst, 4);
        REQUIRE(small.events.size() == 4);
        CHECK(small.dropped == 4);
        CHECK(small.unsupported == 4);
        CHECK(midi(small.events[0], 0xb0, 7, 100));
        CHECK(midi(small.events[1], 0xa2, 64, 64));
        CHECK(midi(small.events[2], 0xb0, 10, 0x40));
        CHECK(small.events[3].kind == Kind::NoteOn);
        // 300 CCs from frame 10, wrapping at 64: 256 kept, sorted stably.
        const auto many = run(*r.instance, flood, 256);
        REQUIRE(many.events.size() == 256);
        CHECK(many.dropped == 44);
        CHECK(many.events.front().time == 0);
        CHECK(many.events.front().midi[2] == 54);  // the first push at frame 0 (k = 54)
        CHECK(many.events.back().time == 63);
        // No sink: nothing is collected and nothing fails.
        Buffers buffers(64);
        REQUIRE(r.instance->process(buffers.block(64, flood)));
        if (isolation == Isolation::Local) {
            burstReference = small;
            floodReference = many;
        } else {
            CHECK(small == burstReference);
            CHECK(many == floodReference);
        }
        r.instance->stopProcessing();
        r.instance->deactivate();
    }
}

TEST_CASE("a CLAP plug-in's MIDI-dialect output keeps its bytes") {
    const auto info = findClap("com.suppleaudio.test.note-echo");
    Event in{.kind = Event::Kind::Midi, .time = 9, .port = 0};
    in.midi[0] = 0x93;
    in.midi[1] = 50;
    in.midi[2] = 99;
    for (const auto isolation : kIsolations) {
        CAPTURE(label(isolation));
        auto r = start(info, isolation);
        const auto got = run(*r.instance, std::span<const Event>(&in, 1), 8);
        REQUIRE(got.events.size() == 1);
        CHECK(got.events[0].time == 9);
        CHECK(got.events[0].port == 0);
        CHECK(midi(got.events[0], 0x93, 57, 99));  // a MIDI note out stays MIDI
        r.instance->stopProcessing();
        r.instance->deactivate();
    }
}

TEST_CASE("a VST3 plug-in's note, pressure and legacy MIDI CC output reaches the sink") {
    const auto info = findVst3("Test VST3 Note Echo");
    const Event in[] = {noteOn(4, 60, 3, 1.0, 1), noteOff(30, 60, 3, 1)};
    Captured reference;
    for (const auto isolation : kIsolations) {
        CAPTURE(label(isolation));
        auto r = start(info, isolation);
        const auto got = run(*r.instance, in, 16);
        CHECK(got.dropped == 0);
        CHECK(got.unsupported == 1);  // the data (SysEx) event
        REQUIRE(got.events.size() == 5);
        const auto& e = got.events;
        CHECK(e[0].kind == Kind::NoteOn);
        CHECK(e[0].time == 4);
        CHECK(e[0].channel == 1);
        CHECK(e[0].key == 67);
        CHECK(e[0].noteId == 3);
        CHECK(e[0].value == 1.0);
        CHECK(e[1].time == 4);
        CHECK(midi(e[1], 0xb1, 74, 127));
        CHECK(e[2].time == 4);
        CHECK(midi(e[2], 0xa1, 67, 64));
        CHECK(e[2].noteId == 3);
        CHECK(e[3].kind == Kind::NoteOff);
        CHECK(e[3].time == 30);
        CHECK(e[3].key == 67);
        CHECK(e[4].time == 63);
        CHECK(midi(e[4], 0xe0, 1, 64));  // legacy pitch bend: LSB, MSB
        if (isolation == Isolation::Local)
            reference = got;
        else
            CHECK(got == reference);
        // Room for two: the plug-in's later events are dropped and counted.
        const auto small = run(*r.instance, in, 2);
        CHECK(small.events.size() == 2);
        CHECK(small.dropped == 3);
        r.instance->stopProcessing();
        r.instance->deactivate();
    }
}

TEST_CASE("MIDI output conversions") {
    using namespace midiout;
    OutputEvent e;
    CHECK(shortMessage(0xc5, 9, 77, e));
    CHECK((e.midi[0] == 0xc5 && e.midi[1] == 9 && e.midi[2] == 0));  // unused byte zeroed
    CHECK_FALSE(shortMessage(0xf0, 1, 2, e));                        // SysEx start
    CHECK_FALSE(shortMessage(0x40, 1, 2, e));                        // a data byte
    CHECK_FALSE(shortMessage(0x90, 60, 0x80, e));
    CHECK(shortMessage(0xf2, 1, 2, e));  // song position
    CHECK_FALSE(shortMessage(0xf4, 0, 0, e));
    const std::uint32_t noteOn2[4] = {0x40913c00u, 0x00000000u, 0, 0};  // velocity 0
    REQUIRE(fromUmp(noteOn2, e));
    CHECK((e.midi[0] == 0x91 && e.midi[1] == 60 && e.midi[2] == 1));    // never an off
    const std::uint32_t program[4] = {0x40c20001u, 0x05000000u, 0, 0};  // with bank
    CHECK_FALSE(fromUmp(program, e));
    const std::uint32_t programOnly[4] = {0x40c20000u, 0x05000000u, 0, 0};
    REQUIRE(fromUmp(programOnly, e));
    CHECK((e.midi[0] == 0xc2 && e.midi[1] == 5));
    const std::uint32_t midi1[4] = {0x20b40740u, 0, 0, 0};  // MIDI 1.0 channel voice in UMP
    REQUIRE(fromUmp(midi1, e));
    CHECK((e.midi[0] == 0xb4 && e.midi[1] == 7 && e.midi[2] == 0x40));
    const std::uint32_t sysex7[4] = {0x30000000u, 0, 0, 0};
    CHECK_FALSE(fromUmp(sysex7, e));
    CHECK(note(Kind::NoteOff, -1, -1, -1, -1, 2.0, e));
    CHECK(e.value == 1.0);
    CHECK_FALSE(note(Kind::NoteOn, 0, -1, 60, -1, 1.0, e));  // an on needs its channel
    CHECK_FALSE(note(Kind::NoteOn, 0, 0, 128, -1, 1.0, e));
    CHECK(blockTime(-3, 64) == 0);
    CHECK(blockTime(64, 64) == 63);
    std::array<OutputEvent, 5> events{};
    const std::uint32_t times[] = {4, 1, 4, 0, 1};
    for (std::size_t i = 0; i < events.size(); ++i) {
        events[i].time = times[i];
        events[i].noteId = static_cast<std::int32_t>(i);
    }
    sortByTime(events);
    const std::int32_t order[] = {3, 1, 4, 0, 2};
    for (std::size_t i = 0; i < events.size(); ++i) CHECK(events[i].noteId == order[i]);
}
