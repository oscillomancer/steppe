// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include "midi_output.h"
using namespace supple;
using namespace supple::engine;
namespace {
struct Sink : detail::MidiSink {
    double clock = 10;
    std::vector<double> waits;
    std::vector<detail::TimedMidiOutput> sent;
    bool fail = false, timestamps = false;
    Sink() { names = {"Synth", "Clock"}; }
    Status start() override { return okStatus(); }
    void stop() override {}
    double now() const SUPPLE_RT override { return clock; }
    bool timestamped() const override { return timestamps; }
    bool send(const detail::TimedMidiOutput& e) override {
        sent.push_back(e);
        return !fail;
    }
    void waitUntil(double deadline) override {
        waits.push_back(deadline);
        clock = std::max(clock, deadline);
    }
    bool changed() override { return false; }
};
}  // namespace
TEST_CASE("MIDI output staging validates complete messages and sorts stable frame ties") {
    detail::MidiOutputCycle cycle;
    cycle.begin(64, 2);
    CHECK(cycle.write({63, 1, 1, {0xf8}}));
    CHECK(cycle.write({1, 0, 2, {0xf1, 7}}));
    CHECK(cycle.write({1, 1, 3, {0xf2, 0, 4}}));
    CHECK(cycle.write({0, 0, 10, {0xf0, 0x7f, 0x7f, 1, 1, 0, 2, 3, 4, 0xf7}}));
    for (const auto status : {0xfa, 0xfb, 0xfc})
        CHECK(cycle.write({0, 0, 1, {static_cast<std::uint8_t>(status)}}));
    CHECK_FALSE(cycle.write({64, 0, 1, {0xf8}}));
    CHECK_FALSE(cycle.write({0, 2, 1, {0xf8}}));
    CHECK_FALSE(cycle.write({0, 0, 3, {0xf1, 0, 0}}));
    CHECK_FALSE(cycle.write({0, 0, 3, {0x90, 128, 0}}));
    CHECK_FALSE(cycle.write({0, 0, 2, {0xf0, 0}}));
    CHECK_FALSE(cycle.write({0, 0, 11, {0xf8}}));
    CHECK(cycle.dropped() == 6);
    const auto sorted = cycle.sorted();
    REQUIRE(sorted.size() == 7);
    CHECK(sorted[0].size == 10);
    CHECK(sorted[4].port == 0);
    CHECK(sorted[5].port == 1);
    CHECK(sorted[6].frame == 63);
    CHECK_FALSE(cycle.write({0, 0, 1, {0xf8}}));  // outside callback
    cycle.begin(512, 1);
    cycle.chunk(256, 128);
    for (std::size_t n = 0; n < kMaxMidiOutputEvents; ++n) CHECK(cycle.write({2, 0, 1, {0xf8}}));
    cycle.chunk(384, 128);
    CHECK_FALSE(cycle.write({0, 0, 1, {0xf8}}));  // budget spans chunks
    CHECK(cycle.sorted().front().frame == 258);
}
TEST_CASE("native output scheduler uses fake due times and preserves port order") {
    Sink sink;
    auto sender = std::make_unique<detail::MidiOutputSender>(sink);
    REQUIRE(sender->push({11, {0, 0, 1, {0xfa}}}));
    REQUIRE(sender->push({10.5, {0, 1, 1, {0xf8}}}));
    REQUIRE(sender->push({11, {0, 0, 1, {0xfc}}}));
    CHECK(sender->dispatch() == 10.5);
    CHECK(sink.sent.empty());
    sink.clock = 10.5;
    CHECK(sender->dispatch() == 11);
    REQUIRE(sink.sent.size() == 1);
    CHECK(sink.sent[0].event.port == 1);
    sink.clock = 11;
    sender->dispatch();
    REQUIRE(sink.sent.size() == 3);
    CHECK(sink.sent[1].event.bytes[0] == 0xfa);
    CHECK(sink.sent[2].event.bytes[0] == 0xfc);
    CHECK_FALSE(sender->push({10, {0, 0, 1, {0xf8}}}));  // discontinuity
    sink.timestamps = true;
    REQUIRE(sender->push({12, {0, 0, 1, {0xf8}}}));
    sender->dispatch();
    REQUIRE(sink.sent.size() == 4);
    CHECK(sink.sent.back().seconds == 12);  // OS schedules future time
    sink.fail = true;
    REQUIRE(sender->push({13, {0, 0, 1, {0xf8}}}));
    sender->dispatch();
    CHECK(sender->dropped() == 2);
    CHECK(sender->dropped(0) == 2);
    CHECK(sender->dropped(1) == 0);
}
TEST_CASE("native output queue overflow and shutdown discard pending output") {
    Sink sink;
    auto sender = std::make_unique<detail::MidiOutputSender>(sink);
    for (std::size_t n = 0; n < detail::kMidiOutputQueueSize; ++n)
        REQUIRE(sender->push({20, {0, 0, 1, {0xf8}}}));
    CHECK_FALSE(sender->push({20, {0, 0, 1, {0xf8}}}));
    CHECK(sender->dropped() == 1);
    sender->stop();
    CHECK(sender->dropped() == detail::kMidiOutputQueueSize + 1);
    CHECK(sink.sent.empty());
}
TEST_CASE("dummy MIDI output capture retains absolute frames and bounded losses") {
    DummyBackend backend(
        {.midiOutputPorts = {"Synth"}, .midiOutputLatency = {-15}, .midiCaptureEvents = 3});
    REQUIRE(backend.start({.blockSize = 64, .midiOutputs = true}, [&](auto, auto, unsigned n) {
        if (!n) return;
        REQUIRE(backend.writeMidi({63, 0, 1, {0xfc}}));
        REQUIRE(backend.writeMidi({0, 0, 1, {0xfa}}));
    }));
    CHECK(backend.midiOutputPorts() == std::vector<std::string>{"Synth"});
    CHECK(backend.midiOutputLatency() == std::vector<std::int64_t>{-15});
    backend.run(2);
    backend.stop();
    REQUIRE(backend.midiCapture().size() == 3);
    CHECK(backend.midiCapture()[0].frame == 0);
    CHECK(backend.midiCapture()[1].frame == 63);
    CHECK(backend.midiCapture()[2].frame == 64);
    CHECK(backend.midiOutDropped() == 1);
    CHECK(backend.midiOutAsyncDropped(0) == 1);
}

TEST_CASE("CoreMIDI wire encoding supports system, channel voice and full-frame SysEx7 UMPs") {
    auto encoded = detail::midiOutputUmp({0, 0, 1, {0xf8}});
    CHECK(encoded.count == 1);
    CHECK(encoded.words[0] == 0x10f80000u);
    encoded = detail::midiOutputUmp({0, 0, 3, {0x91, 60, 100}});
    CHECK(encoded.words[0] == 0x20913c64u);
    encoded = detail::midiOutputUmp({0, 0, 2, {0xf1, 4}});
    CHECK(encoded.words[0] == 0x10f10400u);
    encoded = detail::midiOutputUmp({0, 0, 3, {0xf2, 1, 2}});
    CHECK(encoded.words[0] == 0x10f20102u);
    encoded = detail::midiOutputUmp({0, 0, 10, {0xf0, 0x7f, 0x7f, 1, 1, 0, 2, 3, 4, 0xf7}});
    REQUIRE(encoded.count == 4);
    CHECK(encoded.words == std::array<std::uint32_t, 4>{0x30167f7fu, 0x01010002u, 0x30320304u, 0});
    encoded = detail::midiOutputUmp({0, 0, 2, {0xf0, 0xf7}});
    CHECK(encoded.count == 2);
    CHECK(encoded.words[0] == 0x30000000u);
}
TEST_CASE("native output shutdown sends releases now without endpoint flush") {
    Sink sink;
    auto sender = std::make_unique<detail::MidiOutputSender>(sink);
    REQUIRE(sender->push({20, {0, 0, 3, {0x90, 60, 100}}}));
    REQUIRE(sender->push(
        {10, {0, 0, 3, {0x80, 60, 0}}}));  // clamped behind previously scheduled events
    REQUIRE(sender->push({10, {0, 0, 3, {0xb0, 64, 0}}}));
    sender->stop();
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[0].event.bytes[0] == 0x80);
    CHECK(sink.sent[0].seconds == 10);
    CHECK(sink.sent[1].event.bytes[1] == 64);
    CHECK(sender->dropped() == 1);
}

TEST_CASE("timestamped closing releases follow accepted events and wait before disposal") {
    Sink sink;
    sink.timestamps = true;
    auto sender = std::make_unique<detail::MidiOutputSender>(sink);
    REQUIRE(sender->push({20, {0, 0, 3, {0x90, 60, 100}}}));
    sender->dispatch();
    REQUIRE(sender->push({20, {0, 0, 3, {0x80, 60, 0}}, true}));
    sender->dispatch();
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[0].seconds == 20);
    CHECK(sink.sent[1].seconds == 20);
    sink.clock = 11;
    REQUIRE(sender->push({21, {0, 0, 3, {0xb0, 64, 0}}, true}));
    sender->stop();
    REQUIRE(sink.sent.size() == 3);
    CHECK(sink.sent[2].seconds == 20);
    REQUIRE(sink.waits.size() == 1);
    CHECK(sink.waits[0] == doctest::Approx(11.25));
}

TEST_CASE("timestamped native close waits the remaining plan delay up to the shared bound") {
    for (double delay : {.05, 1.}) {
        Sink sink;
        sink.timestamps = true;
        auto sender = std::make_unique<detail::MidiOutputSender>(sink);
        REQUIRE(sender->push({10 + delay, {0, 0, 3, {0x90, 60, 100}}}));
        sender->dispatch();
        REQUIRE(sender->push({10, {0, 0, 3, {0x80, 60, 0}}, true}));
        sender->stop();
        REQUIRE(sink.sent.size() == 2);
        CHECK(sink.sent[1].seconds == doctest::Approx(10 + delay));
        CHECK(sink.clock ==
              doctest::Approx(10 + std::min(delay, detail::kMidiOutputCloseWaitSeconds)));
        sink.stop();  // port disposal follows sender's bounded wait
    }
    CHECK(detail::midiOutputCloseDeadline(10, 9) == 10);
}
