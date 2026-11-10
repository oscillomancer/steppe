// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Shared in-process contracts, extracted unchanged from the Linux host suite.
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>
#include "../src/platform.h"
#include "../src/unicode.h"
#include "supple/clap/semantic_params.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"
using namespace supple;
using namespace supple::host;
namespace {
const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
const std::filesystem::path kVst3Fixture = FIXTURE_VST3_PATH;

PluginInfo find(const std::string& id) {
    ScanOptions options;
    options.forceInProcess = true;
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1), options);
    REQUIRE(list);
    for (auto& p : *list)
        if (p.id == id) return p;
    FAIL("fixture plugin not found: ", id);
    return {};
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
    ProcessBlock block(std::uint32_t n, std::span<const Event> events = {}, bool withInput = true) {
        ProcessBlock b;
        b.inputs = withInput ? in : nullptr;
        b.inputChannels = withInput ? 2 : 0;
        b.outputs = out;
        b.outputChannels = 2;
        b.frames = n;
        b.events = events;
        return b;
    }
};

double gainClap(double db) {
    // The fixture uses ADR-0010 normalization: linear curve over -60..12 dB.
    return (db + 60.0) / 72.0;
}

}  // namespace

TEST_CASE("host Unicode conversion bounds and validates plugin text") {
    const char16_t text[] = {u'A', 0x00e9, 0xd83d, 0xde00, u'Z', 0};
    const std::string utf8 = "A\xc3\xa9\xf0\x9f\x98\x80Z";
    CHECK(unicode::toUtf8(text) == utf8);
    std::array<char16_t, 6> decoded{};
    REQUIRE(unicode::toUtf16(utf8, decoded));
    CHECK(std::equal(decoded.begin(), decoded.end(), std::begin(text)));
    const char16_t malformed[] = {0xd800, u'x', 0xdc00, 0xd800};
    CHECK(unicode::toUtf8(malformed) == "\xef\xbf\xbdx\xef\xbf\xbd\xef\xbf\xbd");
    for (const std::string bad : {"\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80",
                                  "\xf4\x90\x80\x80", "\xe2\x82", "\xc2x"})
        CHECK_FALSE(unicode::toUtf16(bad, decoded));
    CHECK_FALSE(unicode::toUtf16(utf8, std::span(decoded).first(5)));
    CHECK(unicode::toUtf16("", decoded));
    CHECK(decoded[0] == 0);
    CHECK_FALSE(unicode::toUtf16("", {}));
}

TEST_CASE("VST3 fixture scans, processes, and restores state") {
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    REQUIRE(found->size() == 9);
    CHECK((*found)[0].format == "vst3");
    CHECK((*found)[0].id.rfind("vst3:", 0) == 0);
    CHECK((*found)[0].version == "2.0");
    auto gain = std::find_if(found->begin(), found->end(),
                             [](const PluginInfo& p) { return p.name == "Test VST3 Gain"; });
    REQUIRE(gain != found->end());
    auto made = Instance::create(*gain, {});
    REQUIRE(made);
    auto& instance = **made;
    REQUIRE(instance.params().size() == 1);
    CHECK((instance.params()[0].flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
    REQUIRE(instance.setParam(1, 0.25));
    CHECK(*instance.paramValue(1) == doctest::Approx(0.25));
    auto text = instance.paramText(1, 0.25);
    REQUIRE(text);
    auto parsed = instance.paramFromText(1, *text);
    REQUIRE(parsed);
    CHECK(*parsed == doctest::Approx(0.25));
    auto invalid = instance.paramFromText(1, "\xed\xa0\x80");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "host.param_text");
    auto state = instance.saveState();
    REQUIRE_MESSAGE(state, (state ? "" : state.error().message));
    REQUIRE(instance.setParam(1, 0.75));
    REQUIRE(instance.loadState(*state));
    CHECK(*instance.paramValue(1) == doctest::Approx(0.25));
    REQUIRE(instance.activate(48000, 1, 64));
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::fill(buffer.l.begin(), buffer.l.end(), 1.0f);
    std::fill(buffer.r.begin(), buffer.r.end(), 1.0f);
    REQUIRE(instance.process(buffer.block(64)));
    CHECK(static_cast<double>(buffer.ol[0]) == doctest::Approx(0.25));
    Event automation{.kind = Event::Kind::Param, .time = 20, .paramId = 1, .value = 0.5};
    REQUIRE(instance.process(buffer.block(64, std::span<const Event>(&automation, 1))));
    CHECK(static_cast<double>(buffer.ol[19]) == doctest::Approx(0.25));
    CHECK(static_cast<double>(buffer.ol[20]) == doctest::Approx(0.5));
    instance.stopProcessing();
    instance.deactivate();
}

TEST_CASE("VST3 plug-ins with a sidechain bus get every bus; kNotImplemented starts processing") {
    // A partial bus layout is refused by strict plug-ins, which then output
    // silence; setProcessing may answer kNotImplemented (sfizz does).
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto sidechain = std::find_if(found->begin(), found->end(), [](const PluginInfo& p) {
        return p.name == "Test VST3 Sidechain Gain";
    });
    REQUIRE(sidechain != found->end());
    auto made = Instance::create(*sidechain, {});
    REQUIRE(made);
    auto& instance = **made;
    CHECK(instance.audioInputChannels() == 2);
    REQUIRE(instance.setParam(1, 0.5));
    REQUIRE(instance.activate(48000, 1, 64));
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::fill(buffer.l.begin(), buffer.l.end(), 1.0f);
    std::fill(buffer.r.begin(), buffer.r.end(), 1.0f);
    REQUIRE(instance.process(buffer.block(64)));
    CHECK(static_cast<double>(buffer.ol[10]) == doctest::Approx(0.5));
    CHECK(static_cast<double>(buffer.orr[10]) == doctest::Approx(0.5));
    instance.stopProcessing();
    instance.deactivate();
}

TEST_CASE("a VST3 reset ends the notes the host delivered") {
    // A VST3 has no reset of its own: the host remembers the notes it sent and
    // releases them at the next process (the engine relies on that for "no
    // hanging notes" when an instance leaves an executor).
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    const auto sine = std::find_if(found->begin(), found->end(),
                                   [](const PluginInfo& p) { return p.name == "Test VST3 Sine"; });
    REQUIRE(sine != found->end());
    auto made = Instance::create(*sine, {});
    REQUIRE(made);
    auto& instrument = **made;
    REQUIRE(instrument.activate(48000, 1, 64));
    REQUIRE(instrument.startProcessing());
    Buffers buffer(64);
    Event note{.kind = Event::Kind::NoteOn, .time = 0, .key = 69, .noteId = 3};
    REQUIRE(instrument.process(buffer.block(64, std::span<const Event>(&note, 1), false)));
    CHECK(std::abs(buffer.ol[32]) > 0.01f);  // the voice is sounding
    instrument.reset();
    buffer = Buffers(64);
    REQUIRE(instrument.process(buffer.block(64, {}, false)));
    CHECK(buffer.ol[32] == 0.0f);  // the reset note-off ended it
    instrument.stopProcessing();
    instrument.deactivate();
}

TEST_CASE("scan finds the fixtures with their capabilities") {
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    REQUIRE(list->size() == 19);
    const auto gain = find("com.suppleaudio.test.gain");
    CHECK_FALSE(gain.instrument);
    CHECK(gain.semantic);
    CHECK(gain.name == "Test Gain");
    const auto sine = find("com.suppleaudio.test.sine");
    CHECK(sine.instrument);
    CHECK_FALSE(sine.semantic);
    auto bad = Instance::create(PluginInfo{.id = "nope", .path = kFixture}, {});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "host.create");
}

TEST_CASE("gain: ports, params, sample-accurate automation") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    CHECK(g.audioInputChannels() == 2);
    CHECK(g.audioOutputChannels() == 2);
    CHECK_FALSE(g.acceptsNotes());
    auto ps = g.params();
    REQUIRE(ps.size() == 2);
    CHECK(ps[0].name == "Gain");
    CHECK((ps[1].flags & CLAP_PARAM_IS_STEPPED) != 0);
    CHECK(*g.paramText(0, gainClap(-6)) == "-6.0 dB");
    CHECK(*g.paramFromText(0, "-6 dB") == doctest::Approx(gainClap(-6)));
    CHECK_FALSE(g.paramFromText(0, "loud"));

    REQUIRE(g.activate(48000, 1, 512));
    g.startProcessing();
    Buffers buf(256);
    for (std::uint32_t i = 0; i < 256; ++i) buf.l[i] = buf.r[i] = 0.5f;
    Event e{.kind = Event::Kind::Param, .time = 100, .paramId = 0, .value = gainClap(-60)};
    REQUIRE(g.process(buf.block(256, std::span<const Event>(&e, 1))));
    CHECK(static_cast<double>(buf.ol[99]) == doctest::Approx(0.5));
    CHECK(buf.ol[100] == 0.0f);  // -60 dB is off-at-min: silence exactly from sample 100
    // A queued change applies at the start of the next block.
    REQUIRE(g.setParam(0, gainClap(0)));
    REQUIRE(g.process(buf.block(256)));
    CHECK(static_cast<double>(buf.ol[0]) == doctest::Approx(0.5));
    g.stopProcessing();
    g.deactivate();
    // Inactive: flushed immediately on the main thread.
    REQUIRE(g.setParam(0, gainClap(-12)));
    CHECK(*g.paramValue(0) == doctest::Approx(gainClap(-12)));
}

TEST_CASE("gain: state round trip across instances") {
    auto a = Instance::create(find("com.suppleaudio.test.gain"), {});
    auto b = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE((*a)->setParam(0, gainClap(-3)));
    auto state = (*a)->saveState();
    REQUIRE(state);
    REQUIRE((*b)->loadState(*state));
    CHECK(*(*b)->paramValue(0) == doctest::Approx(gainClap(-3)));
    std::vector<std::uint8_t> junk = {1, 2, 3};
    CHECK_FALSE((*b)->loadState(junk).ok());
}

TEST_CASE("gain: semantic schema, patches, errors, validate-only") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    auto schema = g.semanticSchema();
    REQUIRE(schema);
    CHECK(schema->find("params")->find("gain")->find("unit")->asString() == "dB");
    CHECK(*g.getPatch(true) ==
          "{\n  \"schema\": \"supple.test-gain-patch/1.0\",\n  \"params\": {}\n}\n");
    REQUIRE(g.setPatch(R"({"params": {"gain": "-4.5 dB"}})", CLAP_SUPPLE_SET_MERGE));
    CHECK(*g.paramValue(0) == doctest::Approx(gainClap(-4.5)));
    CHECK(g.getPatch(true)->find("\"gain\": -4.5") != std::string::npos);
    CHECK(*g.describe(true) == "Gain: Gain -4.5 dB");
    auto bad = g.setPatch(R"({"params": {"gian": 1}})", CLAP_SUPPLE_SET_MERGE);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "params.unknown_key");
    CHECK(bad.error().path == "/params/gian");
    CHECK(bad.error().hint.find("gain") != std::string::npos);
    REQUIRE(g.setPatch(R"({"params": {"gain": 6}})", CLAP_SUPPLE_SET_VALIDATE_ONLY));
    CHECK(*g.paramValue(0) == doctest::Approx(gainClap(-4.5)));  // unchanged
}

TEST_CASE("gain: latency change requests a restart; serviceMainThread reactivates") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256));
    CHECK(g.latency() == 0);
    REQUIRE(g.setPatch(R"({"params": {"latency": "short"}})", CLAP_SUPPLE_SET_MERGE));
    CHECK(g.serviceMainThread());
    CHECK(g.active());
    CHECK(g.latency() == 64);
    g.startProcessing();
    Buffers buf(128);
    buf.l[0] = buf.r[0] = 1.0f;
    REQUIRE(g.process(buf.block(128)));
    CHECK(buf.ol[0] == 0.0f);
    CHECK(static_cast<double>(buf.ol[64]) == doctest::Approx(1.0));
    g.stopProcessing();
}

TEST_CASE("sine: notes at sample offsets, note end events, MIDI dialect") {
    auto inst = Instance::create(find("com.suppleaudio.test.sine"), {});
    REQUIRE(inst);
    auto& s = **inst;
    CHECK(s.audioInputChannels() == 0);
    CHECK(s.acceptsNotes());
    REQUIRE(s.activate(48000, 1, 512));
    s.startProcessing();
    Buffers buf(512);
    const Event events[] = {
        {.kind = Event::Kind::NoteOn, .time = 10, .key = 69, .noteId = 7, .velocity = 1.0},
        {.kind = Event::Kind::NoteOff, .time = 300, .key = 69, .noteId = 7},
    };
    std::vector<OutputEvent> storage(16);
    OutputEvents out{storage};
    REQUIRE(s.process(buf.block(512, events, false), &out));
    for (int i = 0; i <= 10; ++i) CHECK(buf.ol[i] == 0.0f);  // phase 0 at the note start
    double energy = 0;
    for (int i = 11; i < 300; ++i)
        energy += static_cast<double>(buf.ol[i]) * static_cast<double>(buf.ol[i]);
    CHECK(energy > 1.0);
    for (int i = 300; i < 512; ++i) CHECK(buf.ol[i] == 0.0f);
    REQUIRE(out.count == 1);
    CHECK(out.storage[0].kind == OutputEvent::Kind::NoteEnd);
    CHECK(out.storage[0].time == 300);
    CHECK(out.storage[0].noteId == 7);
    CHECK(out.storage[0].port == 0);
    CHECK(out.storage[0].channel == 0);
    CHECK(out.storage[0].key == 69);
    // Raw MIDI events also reach the plugin.
    Event midiOn{.kind = Event::Kind::Midi, .time = 0};
    midiOn.midi[0] = 0x90;
    midiOn.midi[1] = 60;
    midiOn.midi[2] = 100;
    REQUIRE(s.process(buf.block(64, std::span<const Event>(&midiOn, 1), false)));
    double e2 = 0;
    for (int i = 0; i < 64; ++i)
        e2 += static_cast<double>(buf.ol[i]) * static_cast<double>(buf.ol[i]);
    CHECK(e2 > 0.0);
    s.stopProcessing();
}

namespace {
bool rtBlock(Instance& g, const ProcessBlock& b, OutputEvents& out) SUPPLE_RT {
    return g.process(b, &out);
}
}  // namespace

TEST_CASE("the process path is real-time safe" * doctest::description("rt")) {
    auto inst = Instance::create(find("com.suppleaudio.test.sine"), {});
    REQUIRE(inst);
    REQUIRE((*inst)->activate(48000, 1, 256));
    (*inst)->startProcessing();
    Buffers buf(256);
    std::vector<OutputEvent> storage(64);
    OutputEvents out{storage};
    const Event events[] = {{.kind = Event::Kind::NoteOn, .time = 3, .key = 60, .velocity = 0.8},
                            {.kind = Event::Kind::NoteOff, .time = 200, .key = 60}};
    bool ok = true;
    for (int i = 0; i < 50; ++i) ok = ok && rtBlock(**inst, buf.block(256, events, false), out);
    CHECK(ok);
    (*inst)->stopProcessing();
}

TEST_CASE("lifecycle: a restart requested while processing waits until processing stops") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256));
    CHECK(g.latency() == 0);
    REQUIRE(g.startProcessing());
    Buffers buf(64);
    Event e;
    e.kind = Event::Kind::Param;
    e.paramId = 1;  // latency: "short" -> the fixture calls request_restart on the audio thread
    e.value = 1.0;
    REQUIRE(g.process(buf.block(64, std::span<const Event>(&e, 1))));
    g.serviceMainThread();  // still processing: must not restart, must keep the request
    CHECK(g.active());
    CHECK(g.latency() == 0);
    CHECK(g.restartPending());  // the engine takes it off its executors for this
    g.deactivate();             // refused while processing
    CHECK(g.active());
    bool logged = false;
    for (const auto& line : g.takeLog())
        logged = logged || line.find("deactivate") != std::string::npos;
    CHECK(logged);
    g.stopProcessing();
    CHECK(g.serviceMainThread());
    CHECK(g.active());
    CHECK(g.latency() == 64);
    CHECK_FALSE(g.restartPending());
}

TEST_CASE("lifecycle: request_flush while active is served on the audio thread") {
    auto inst = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(inst);
    auto& p = **inst;
    auto flushes = [&] { return p.paramValue(7).value(); };
    const std::uint8_t plain[4] = {0, 0, 0, 0};
    // Inactive: the host flushes on the main thread.
    REQUIRE(p.loadState(plain));
    CHECK_FALSE(p.needsAudioFlush());
    p.serviceMainThread();
    CHECK(flushes() == 1.0);
    // Active: the main thread must not flush; flushParams() on the audio thread does.
    REQUIRE(p.activate(48000, 1, 256));
    REQUIRE(p.loadState(plain));
    CHECK(p.needsAudioFlush());
    p.serviceMainThread();
    CHECK(flushes() == 1.0);
    p.flushParams();
    CHECK(flushes() == 2.0);
    CHECK_FALSE(p.needsAudioFlush());
    // A process call also satisfies the request.
    REQUIRE(p.loadState(plain));
    REQUIRE(p.startProcessing());
    Buffers buf(32);
    REQUIRE(p.process(buf.block(32)));
    CHECK_FALSE(p.needsAudioFlush());
    p.stopProcessing();
}

TEST_CASE("lifecycle: refused start_processing blocks process(); tail is reported") {
    auto inst = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(inst);
    auto& p = **inst;
    const std::uint8_t refuse[4] = {1, 0xe8, 0x03, 0};  // tail 1000 samples
    REQUIRE(p.loadState(refuse));
    CHECK(p.reportsTail());
    CHECK(p.tail() == 1000);
    REQUIRE(p.activate(48000, 1, 256));
    CHECK_FALSE(p.startProcessing());
    Buffers buf(32);
    CHECK_FALSE(p.process(buf.block(32)));
    CHECK_FALSE(Instance::create(find("com.suppleaudio.test.gain"), {}).value()->reportsTail());
}

TEST_CASE("events: queued params wait when the arena is full; overflow is counted") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256));
    REQUIRE(g.startProcessing());
    REQUIRE(g.setParam(0, gainClap(-6)));  // queued (active)
    std::vector<Event> midi(9000);
    for (auto& e : midi) {
        e.kind = Event::Kind::Midi;  // ignored by the gain fixture
        e.midi[0] = 0xb0;
    }
    Buffers buf(256);
    REQUIRE(g.process(buf.block(256, midi)));
    CHECK(g.takeNotifications().droppedEvents == 9000 - 8192);
    CHECK(g.paramValue(0).value() == doctest::Approx(gainClap(0)));  // not delivered yet
    REQUIRE(g.process(buf.block(256)));
    CHECK(g.paramValue(0).value() == doctest::Approx(gainClap(-6)));
    g.stopProcessing();
}

// --- isolation (ADR-0013) ----------------------------------------------------------------

TEST_CASE("CLAP without port activation follows key buffers while processing") {
    auto made = Instance::create(find("com.suppleaudio.test.legacy-keymix"), {});
    REQUIRE(made);
    Instance& in = **made;
    CHECK_FALSE(in.followsKeyActivation());
    REQUIRE(in.setSidechainActive(false));
    REQUIRE(in.activate(48000, 1, 64));
    REQUIRE(in.startProcessing());
    Buffers buffer(64);
    std::fill(buffer.l.begin(), buffer.l.end(), 0.25f);
    std::fill(buffer.r.begin(), buffer.r.end(), 0.25f);
    REQUIRE(in.process(buffer.block(64)));
    CHECK(buffer.ol[7] == 0.25f);
    // Adding a route changes the block, without restarting this instance.
    std::vector<float> keySamples(64, 0.5f);
    const float* key[] = {keySamples.data(), keySamples.data()};
    auto block = buffer.block(64);
    block.sidechain = key;
    block.sidechainChannels = 2;
    REQUIRE(in.process(block));
    CHECK(buffer.ol[7] == 0.75f);
    // Removing it restores silent key buffers, still without a restart.
    REQUIRE(in.process(buffer.block(64)));
    CHECK(buffer.ol[7] == 0.25f);
    in.stopProcessing();
}

TEST_CASE("CLAP timers may remove themselves and replace registrations") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    const std::array<std::uint8_t, 4> config{2, 0, 0, 0};
    REQUIRE((*made)->loadState(config));
    REQUIRE((*made)->serviceMainThread());
    CHECK(*(*made)->paramValue(9) == 1);
    REQUIRE((*made)->serviceMainThread());
    CHECK(*(*made)->paramValue(9) == 2);
    CHECK((*made)->nextTimerMs() == -1);
}
TEST_CASE("VST3 queue rejection preserves controller value and malformed MIDI is dropped") {
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto gain = std::find_if(found->begin(), found->end(),
                             [](const auto& p) { return p.name == "Test VST3 Gain"; });
    REQUIRE(gain != found->end());
    auto made = Instance::create(*gain, {});
    REQUIRE(made);
    auto& instance = **made;
    CHECK_FALSE(instance.activate(48000, 1, UINT32_MAX));
    bool full = false;
    for (int i = 0; i < 4096; ++i) {
        if (!instance.setParam(1, 0.25)) {
            full = true;
            break;
        }
    }
    REQUIRE(full);
    CHECK_FALSE(instance.setParam(1, 0.75));
    CHECK(*instance.paramValue(1) == doctest::Approx(0.25));
    REQUIRE(instance.activate(48000, 1, 64));
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::array<Event, 128> events{};
    for (std::size_t i = 0; i < events.size(); ++i) {
        events[i].kind = Event::Kind::Midi;
        events[i].midi[0] = 0xb0;
        events[i].midi[1] = static_cast<std::uint8_t>(128 + i);
        events[i].midi[2] = 127;
    }
    CHECK(instance.process(buffer.block(64, events)));
    CHECK(instance.takeNotifications().droppedEvents == events.size());
    instance.stopProcessing();
    instance.deactivate();
}
TEST_CASE("malformed semantic schema falls back to plain CLAP parameters with a warning") {
    auto info = find("com.suppleaudio.test.malformed-schema");
    CHECK_FALSE(info.semantic);
    auto instance = Instance::create(info, {});
    REQUIRE(instance);
    CHECK_FALSE((*instance)->info().semantic);
    CHECK_FALSE((*instance)->semanticSchema());
    CHECK_FALSE((*instance)->params().empty());
    REQUIRE((*instance)->setParam(0, gainClap(-3)));
    CHECK(*(*instance)->paramValue(0) == doctest::Approx(gainClap(-3)));
    const auto log = (*instance)->takeLog();
    REQUIRE_FALSE(log.empty());
    CHECK(log[0].find("invalid semantic schema") != std::string::npos);
}

TEST_CASE("restart activation failures are structured notifications") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    REQUIRE((*made)->activate(48000, 1, 64));
    const std::array<std::uint8_t, 4> config{5, 0, 0, 0};
    REQUIRE((*made)->loadState(config));
    REQUIRE((*made)->serviceMainThread());
    CHECK_FALSE((*made)->active());
    const auto notes = (*made)->takeNotifications();
    REQUIRE(notes.restartError);
    CHECK(notes.restartError->code == "host.activate");
    CHECK_FALSE((*made)->takeNotifications().restartError);
}

TEST_CASE("CLAP layouts beyond the supported port and channel limits are refused") {
    for (const auto* id :
         {"com.suppleaudio.test.many-ports", "com.suppleaudio.test.many-channels"}) {
        auto made = Instance::create(find(id), {});
        REQUIRE_FALSE(made);
        CHECK(made.error().code == "host.layout");
    }
}

TEST_CASE("CLAP process requests and ordered parameter clear notifications are observable") {
    struct Control {
        void (*requestProcess)(const clap_plugin_t*);
        void (*rescan)(const clap_plugin_t*, bool);
        void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
        void (*restart)(const clap_plugin_t*);
    };
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    auto& instance = **made;
    const auto* plugin = instance.raw();
    const auto* control =
        static_cast<const Control*>(plugin->get_extension(plugin, "supple.test.edge-control"));
    REQUIRE(control);
    CHECK_FALSE(instance.processRequested());
    control->requestProcess(plugin);
    CHECK(instance.processRequested());
    CHECK(instance.takeProcessRequest());
    CHECK_FALSE(instance.takeProcessRequest());
    control->clear(plugin, 7, CLAP_PARAM_CLEAR_AUTOMATIONS);
    control->clear(plugin, 8, CLAP_PARAM_CLEAR_MODULATIONS);
    auto notes = instance.takeNotifications();
    REQUIRE(notes.paramsClear.size() == 2);
    CHECK(notes.paramsClear[0].id == 7);
    CHECK(notes.paramsClear[0].flags == CLAP_PARAM_CLEAR_AUTOMATIONS);
    CHECK(notes.paramsClear[1].id == 8);
    CHECK(notes.paramsClear[1].flags == CLAP_PARAM_CLEAR_MODULATIONS);
    CHECK(instance.takeNotifications().paramsClear.empty());
    // Rescan wins over earlier clears; a subsequent clear remains effective.
    control->clear(plugin, 7, CLAP_PARAM_CLEAR_ALL);
    control->rescan(plugin, false);
    control->clear(plugin, 8, CLAP_PARAM_CLEAR_ALL);
    notes = instance.takeNotifications();
    CHECK(notes.paramsRescan);
    CHECK(notes.paramsRescanFlags == CLAP_PARAM_RESCAN_ALL);
    REQUIRE(notes.paramsClear.size() == 1);
    CHECK(notes.paramsClear[0].id == 8);
}

TEST_CASE("a VST3 state its controller rejects leaves the plug-in as it was") {
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    const auto info = std::find_if(list->begin(), list->end(), [](const auto& p) {
        return p.name == "Test VST3 Separate Gain";
    });
    REQUIRE(info != list->end());
    auto made = Instance::create(*info, {});
    REQUIRE(made);
    auto state = (*made)->saveState();
    REQUIRE(state);
    const auto before = (*made)->paramValue(1);
    REQUIRE(before);
    // The component's 8-byte value changed to 0.9, then a one-byte
    // controller state the controller refuses (framing stays valid).
    const double changed = 0.9;
    std::memcpy(state->data() + 16, &changed, 8);
    state->resize(25);
    (*state)[12] = 1;
    (*state)[13] = (*state)[14] = (*state)[15] = 0;
    (*state)[24] = 0;
    auto loaded = (*made)->loadState(*state);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == "host.state");
    CHECK_FALSE(loaded.error().hint.empty());
    const auto after = (*made)->paramValue(1);
    REQUIRE(after);
    CHECK(*after == doctest::Approx(*before));
}

TEST_CASE("CLAP parameter strings stay within their fixed buffers") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    const std::array<std::uint8_t, 4> config{51, 0, 0, 0};
    REQUIRE((*made)->loadState(config));
    const auto params = (*made)->params();
    REQUIRE(params.size() == 2);
    CHECK(params[0].name == std::string(CLAP_NAME_SIZE, 'N'));
    CHECK(params[0].module == std::string(CLAP_PATH_SIZE, 'M'));
    const auto text = (*made)->paramText(7, 0);
    REQUIRE(text);
    CHECK(*text == std::string(256, 'V'));
}

TEST_CASE("VST3 parameter and bus strings stay within their fixed buffers") {
    auto module = platform::openLibrary(FIXTURE_VST3_MODULE);
    REQUIRE(module);
    struct ModuleGuard {
        void* module;
        void (*mode)(bool);
        ~ModuleGuard() {
            if (mode) mode(false);
            platform::closeLibrary(module);
        }
    } guard{*module,
            reinterpret_cast<void (*)(bool)>(platform::symbol(*module, "fixtureUnterminatedText"))};
    REQUIRE(guard.mode);
    guard.mode(true);
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    const auto info = std::find_if(list->begin(), list->end(),
                                   [](const auto& p) { return p.name == "Test VST3 Multi-Out"; });
    REQUIRE(info != list->end());
    auto made = Instance::create(*info, {});
    REQUIRE(made);
    const auto params = (*made)->params();
    REQUIRE(params.size() == 1);
    CHECK(params[0].name == std::string(127, 'N'));
    const auto ports = (*made)->auxOutputPorts();
    REQUIRE(ports.size() == 1);
    CHECK(ports[0].name == std::string(127, 'B'));
    const auto text = (*made)->paramText(1, 0);
    REQUIRE(text);
    CHECK(*text == std::string(127, 'V'));
}

#if !defined(_WIN32)
TEST_CASE("CLAP descriptor readiness is ignored after unregistration") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    const auto* plugin = (*made)->raw();
    const auto* host =
        static_cast<const clap_host_t*>(plugin->get_extension(plugin, "supple.test.edge-host"));
    REQUIRE(host);
    const auto* fds = static_cast<const clap_host_posix_fd_support_t*>(
        host->get_extension(host, CLAP_EXT_POSIX_FD_SUPPORT));
    REQUIRE(fds);
    // Simulate a readiness snapshot invalidated by an earlier callback in
    // the same batch. No actual descriptor is polled by this unit test.
    REQUIRE(fds->register_fd(host, 42, CLAP_POSIX_FD_READ));
    (*made)->onFd(42, CLAP_POSIX_FD_READ);
    CHECK(*(*made)->paramValue(10) == 1);
    REQUIRE(fds->unregister_fd(host, 42));
    (*made)->onFd(42, CLAP_POSIX_FD_READ);
    CHECK(*(*made)->paramValue(10) == 1);
}
#endif

// An executor takes a free instance with one compare-exchange: of several
// that find it free at the same moment, exactly one gets it.
TEST_CASE("claimOwner takes a free instance for exactly one owner") {
    auto made = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(made);
    Instance& in = **made;
    const char a = 0, b = 0, refused = 0;
    CHECK(in.owner() == nullptr);
    CHECK(in.claimOwner(&a));
    CHECK(in.owner() == &a);
    CHECK_FALSE(in.claimOwner(&b));  // held: another token cannot take it
    CHECK_FALSE(in.claimOwner(&a));  // nor the owner again
    CHECK(in.owner() == &a);
    in.setOwner(&refused);  // the owner marks it (a refused start)
    CHECK_FALSE(in.claimOwner(&b));
    in.setOwner(nullptr);  // let go
    CHECK(in.claimOwner(&b));
    CHECK(in.owner() == &b);
    in.setOwner(nullptr);
    // Contended: every round, threads released together race for it. The
    // waits yield: a busy spin starves the threads it waits for on a loaded
    // machine with fewer cores than threads (it timed out on Windows CI).
    constexpr int kThreads = 4, kRounds = 2000;
    std::array<char, kThreads> tokens{};
    for (int round = 0; round < kRounds; ++round) {
        std::atomic<int> ready{0}, winners{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] {
                ready.fetch_add(1);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                if (in.claimOwner(&tokens[static_cast<std::size_t>(t)])) winners.fetch_add(1);
            });
        while (ready.load() < kThreads) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();
        REQUIRE(winners.load() == 1);
        in.setOwner(nullptr);
    }
}
