// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// External sync in the engine (ADR-0071): a manual dummy device scripted with
// MIDI clock, MTC and LTC from libs/sync's generators; the transport follows,
// the chase rate never reaches the project, offline renders ignore it, and
// LTC output decodes back to the transport. MIDI clock and MTC output
// (ADR-0072) land on the device's captured MIDI where their audio is heard.
#include <doctest/doctest.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/json/parse.h"
#include "supple/sync/ltc.h"
#include "supple/sync/midi_clock.h"
#include "supple/sync/mtc.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;
constexpr std::uint32_t kBlock = 256;
const cmd::Origin kOrigin{cmd::Origin::Type::Test, "sync", ""};

// A master: MIDI messages scheduled at absolute device frames, delivered in
// the cycle that holds them, or audio on an input channel.
struct Rig {
    fs::path dir = fs::temp_directory_path() / ("supple-sync-" + std::to_string(::getpid()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    std::unique_ptr<DummyBackend> backend;
    std::function<void(std::int64_t from, std::uint32_t n, sync::EventBuffer& out)> midi;
    std::function<void(std::int64_t from, std::span<float> out)> audio;
    std::vector<sync::MidiEvent> storage = std::vector<sync::MidiEvent>(4096);
    sync::EventBuffer events{storage};
    bool anticipative;

    // MIDI outputs "Synth" and "Clock Out", with `midiLatency` (audio minus
    // MIDI, frames) each.
    explicit Rig(bool ahead, std::uint32_t outputs = 2, std::int64_t capture = 0,
                 std::vector<std::int64_t> midiLatency = {0, 0})
        : anticipative(ahead) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};  // Test Gain, for plan latency
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        DummyBackend::Options o;
        o.mode = DummyBackend::Mode::Manual;
        o.captureFrames = capture;
        o.midiPorts = {"Desk", "Clock In"};
        o.midiOutputPorts = {"Synth", "Clock Out"};
        o.midiOutputLatency = std::move(midiLatency);
        o.sync = [this](std::vector<SyncMidiEvent>& out, std::uint32_t n, std::int64_t from) {
            if (!midi) return;
            events.clear();
            midi(from, n, events);
            for (const sync::MidiEvent& e : events.events()) {
                SyncMidiEvent s;
                s.frame = static_cast<std::uint32_t>(
                    std::clamp<double>(std::ceil(e.frame) - double(from), 0, n - 1));
                s.port = 1;
                s.size = e.size;
                std::copy_n(e.bytes.begin(), e.size, s.bytes.begin());
                out.push_back(s);
            }
        };
        o.input = [this](float* const* in, std::uint32_t channels, std::uint32_t n,
                         std::int64_t from) {
            for (std::uint32_t c = 0; c < channels; ++c) std::fill_n(in[c], n, 0.0f);
            if (audio && channels > 0) audio(from, std::span<float>(in[0], n));
        };
        backend = std::make_unique<DummyBackend>(o);
        REQUIRE(app->startEngine(*backend,
                                 {.sampleRate = kRate,
                                  .blockSize = kBlock,
                                  .inputs = 1,
                                  .outputs = outputs,
                                  .midiInputs = true,
                                  .midiOutputs = true},
                                 {.anticipative = ahead})
                    .ok());
    }
    ~Rig() {
        app->stopEngine();
        bus.reset();
        app.reset();
        fs::remove_all(dir);
    }
    json::Value run(const std::string& command, std::string_view args) {
        auto r = bus->execute({command, json::parse(args).value(), kOrigin});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    }
    std::int64_t device = 0;  // frames run
    // One cycle, then the main thread's service, as the app's loop does.
    void cycle() {
        backend->run(1);
        device += kBlock;
        app->tick();
        // The renderer relocates and renders ahead on its own thread: a
        // manual device waits for it, as a real one's time would.
        if (anticipative) REQUIRE(app->engine()->awaitRendered(kBlock));
    }
    void seconds(double s) {
        for (double end = double(device) + s * kRate; double(device) < end;) cycle();
    }
    TransportState transport() { return app->engine()->transport(); }
    SyncStatus status() { return app->engine()->syncStatus(); }
    double beat() { return app->head().timeMap->beatAt(transport().seconds); }
    // Plan latency: `count` latent Test Gains (64 frames each) on the master.
    void latency(int count) {
        for (int i = 0; i < count; ++i) {
            const std::string fx =
                run("fx.add", R"({"track": "tr_0", "plugin": "Test Gain"})")["fx"].asString();
            run("fx.param.set", R"({"fx": ")" + fx + R"(", "values": {"latency": "short"}})");
        }
        // A plug-in's new latency takes a restart and a plan on the main thread.
        for (int i = 0; i < 400 && app->engine()->outputLatency() != std::uint32_t(64 * count); ++i)
            cycle();
        REQUIRE(app->engine()->outputLatency() == static_cast<std::uint32_t>(64 * count));
    }
    // A track playing `audio` (stereo, at kRate) from time 0.
    void item(const audio::AudioData& data) {
        run("track.add", R"({"name": "Keys"})");
        const fs::path file = dir / ("item-" + std::to_string(items++) + ".wav");
        REQUIRE(audio::writeWav(file, data));
        json::Value add = json::Value::object();
        add["track"] = "tr_1";
        add["audio_file"] = file.string();
        REQUIRE(bus->execute({"item.add", add, kOrigin}));
        // The app prepares the media off the main thread; tick() adopts it.
        for (int i = 0; i < 2000 && app->preparingAudio(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app->tick();
        }
        REQUIRE_FALSE(app->preparingAudio());
        REQUIRE(app->engine()->awaitPlan());
    }
    int items = 0;
};

// Silence with a full-scale click at each of `seconds`, `length` seconds long.
audio::AudioData clicks(double length, std::initializer_list<double> seconds) {
    audio::AudioData a;
    a.sampleRate = kRate;
    a.channels.assign(2, std::vector<float>(static_cast<std::size_t>(length * kRate)));
    for (double s : seconds)
        a.channels[0][static_cast<std::size_t>(s * kRate)] =
            a.channels[1][static_cast<std::size_t>(s * kRate)] = 1.0f;
    return a;
}

// The first sample at or after `from` louder than a tenth, or -1.
std::int64_t click(const std::vector<float>& ch, std::int64_t from) {
    for (auto i = std::max<std::int64_t>(from, 0); i < static_cast<std::int64_t>(ch.size()); ++i)
        if (std::abs(ch[static_cast<std::size_t>(i)]) > 0.1f) return i;
    return -1;
}

// Every LTC frame in a captured channel (device frames are capture indices).
std::vector<sync::LtcFrame> decodeLtc(const std::vector<float>& ch) {
    sync::LtcDecoderConfig dc;
    dc.sampleRate = kRate;
    dc.expected = sync::FrameRate::Fps30;
    auto decoder = sync::LtcDecoder::create(dc);
    REQUIRE(decoder);
    std::vector<sync::LtcFrame> frames;
    for (std::size_t at = 0; at + kBlock <= ch.size(); at += kBlock)
        for (const auto& f :
             decoder->feed(double(at), std::span<const float>(ch.data() + at, kBlock)))
            frames.push_back(f);
    return frames;
}

// A clock master: Start at `startFrame`, `bpm` from beat `fromBeat`.
struct ClockMaster {
    sync::ClockGenerator g;
    double bpm = 120;
    std::int64_t startFrame = 0;
    double fromBeat = 0;
    bool started = false;
    std::optional<std::int64_t> stopAt, continueAt;
    double continueBeat = 0;
    sync::LinearMap map() const {
        return sync::LinearMap(double(startFrame), fromBeat, bpm / 60.0 / kRate);
    }
    void operator()(std::int64_t from, std::uint32_t n, sync::EventBuffer& out) {
        const double to = double(from + n);
        if (!started && double(startFrame) < to) {
            g.start(double(std::max(startFrame, from)), map(), out);
            started = true;
        }
        if (stopAt && *stopAt >= from && *stopAt < from + n) g.stop(double(*stopAt), out);
        if (continueAt && *continueAt >= from && *continueAt < from + n) {
            startFrame = *continueAt;
            fromBeat = continueBeat;
            g.start(double(*continueAt), map(), out);
        }
        if (started) g.run(double(from), to, map(), out);
    }
    double beatAt(std::int64_t frame) const { return map().positionAt(double(frame)); }
};

}  // namespace

TEST_CASE("sync settings: get, set, validation, and the status in engine.status") {
    Rig rig(false);
    auto got = rig.run("sync.get", "{}");
    CHECK(got["settings"]["source"].asString() == "off");
    CHECK(got["status"]["state"].asString() == "idle");
    auto set = rig.run("sync.set", R"({"source": "midi_clock", "midi_input": "clock"})");
    CHECK(set["settings"]["source"].asString() == "midi_clock");
    CHECK(set["status"]["input"].asString() == "Clock In");  // a part of the name, any case
    CHECK(rig.app->engine()->syncConfig().midiPort == 1);
    // Refused with a path and a hint; nothing changes.
    for (const char* bad :
         {R"({"source": "smpte"})", R"({"offset_ms": 20000})", R"({"ltc_input": 0})",
          R"({"on_loss": "pause"})", R"({"frames": 3})", R"({"ltc_level_db": 3})"}) {
        auto r = rig.bus->execute({"sync.set", json::parse(bad).value(), kOrigin});
        REQUIRE_FALSE(r);
        INFO(bad, r.error().toString());
        const Error& e = r.error().details.empty() ? r.error() : r.error().details.front();
        CHECK(e.code == "sync.settings");
        CHECK_FALSE(e.hint.empty());
    }
    CHECK(rig.app->syncSettings().source == "midi_clock");
    // An unknown port is a warning, and the engine listens to nothing.
    auto missing = rig.bus->execute(
        {"sync.set", json::parse(R"({"midi_input": "Nowhere"})").value(), kOrigin});
    REQUIRE(missing);
    CHECK(rig.app->engine()->syncConfig().midiPort == -1);
    auto status = rig.run("engine.status", "{}");
    CHECK(status["sync"]["source"].asString() == "midi_clock");
    REQUIRE(status["sync"]["warnings"].size() == 1);
    // The settings round-trip through their JSON.
    SyncSettings s;
    s.source = "ltc";
    s.ltcInput = 2;
    s.offsetMs = -12.5;
    s.onLoss = "freewheel";
    s.ltcOutput = true;
    auto back = parseSyncSettings(syncSettingsJson(s));
    REQUIRE(back);
    CHECK(*back == s);
    // LTC output is off by default; on, a channel the device lacks (3 of 2)
    // uses its last output, says so, and the setting is kept.
    CHECK_FALSE(SyncSettings{}.ltcOutput);
    auto ltc = rig.run("sync.set", R"({"source": "off", "ltc_output": true})");
    CHECK(rig.app->engine()->syncConfig().ltcOutput == 1);
    CHECK(rig.app->syncSettings().ltcOutputChannel == 3);
    bool said = false;
    for (std::size_t i = 0; i < ltc["status"]["warnings"].size(); ++i)
        said |= ltc["status"]["warnings"].at(i).asString().find(
                    "LTC goes to output 2 instead of 3") != std::string::npos;
    CHECK(said);
}

TEST_CASE("midi clock: Start, tempo follow, Stop, Song Position and Continue") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        Rig rig(ahead);
        rig.run("sync.set", R"({"source": "midi_clock", "midi_input": "Clock In"})");
        ClockMaster master;
        master.bpm = 126;  // the project is at 120: playrate 1.05
        master.startFrame = 4800;
        rig.midi = std::ref(master);
        rig.seconds(0.05);
        CHECK_FALSE(rig.transport().playing);
        // Start: the transport plays from beat 0 at the first clock.
        rig.seconds(0.5);
        REQUIRE(rig.transport().playing);
        CHECK(rig.status().running);
        // Following: the live rate converges on 126/120, the saved rate stays.
        rig.seconds(10);
        const double expected = 126.0 / 120.0;
        CHECK(rig.app->engine()->effectivePlayrate() == doctest::Approx(expected).epsilon(0.004));
        CHECK(rig.app->head().project->playrate.rate == 1.0);
        CHECK(rig.run("transport.playrate.get", "{}")["requested_rate"].asDouble() == 1.0);
        CHECK(rig.status().locked);
        // The transport is where the master is, to a few milliseconds.
        const double error = (rig.beat() - master.beatAt(rig.device)) * 60.0 / 126.0;
        MESSAGE("clock chase error ", error * 1000, " ms, rate ",
                rig.app->engine()->effectivePlayrate());
        CHECK(std::abs(error) < 0.01);
        CHECK(std::abs(rig.status().errorSeconds) < 0.005);
        // Stop: the transport stops; Song Position while stopped locates.
        master.stopAt = rig.device + 100;
        rig.seconds(0.1);
        CHECK_FALSE(rig.transport().playing);
        master.started = true;
        rig.midi = [&](std::int64_t from, std::uint32_t, sync::EventBuffer& out) {
            if (from == rig.device) {
                const std::uint8_t pointer[3] = {0xF2, 32, 0};  // 32 sixteenths: beat 8
                out.push(double(from), pointer);
            }
        };
        rig.cycle();
        rig.seconds(0.05);
        CHECK(rig.beat() == doctest::Approx(8.0).epsilon(1e-6));
        CHECK_FALSE(rig.transport().playing);
        // Continue: from beat 8 at the first clock.
        master.continueAt = rig.device + 512;
        master.continueBeat = 8.0;
        master.stopAt.reset();
        rig.midi = std::ref(master);
        rig.seconds(0.3);
        REQUIRE(rig.transport().playing);
        CHECK(rig.beat() > 8.0);
        CHECK(rig.beat() < 8.0 + 0.4 * 126 / 60.0);
        // Without follow tempo, the transport keeps the saved rate.
        // (A setting changed while running keeps what the receiver knows:
        // the transport plays on, and the master's Stop still stops it.)
        rig.run("sync.set", R"({"follow_tempo": false})");
        rig.seconds(2);
        CHECK(rig.transport().playing);
        CHECK(rig.app->engine()->effectivePlayrate() == doctest::Approx(1.0).epsilon(1e-3));
        master.stopAt = rig.device + 100;
        rig.seconds(0.1);
        CHECK_FALSE(rig.transport().playing);
    }
}

TEST_CASE("mtc: chase from a locate, follow a jump, and stop or freewheel on loss") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        Rig rig(ahead);
        rig.run("project.set", R"({"timecode": {"rate": "25", "start": "01:00:00:00"}})");
        rig.run("sync.set", R"({"source": "mtc", "midi_input": "Clock In"})");
        // The master plays its timeline from 10 s (01:00:10:00), slightly
        // fast: 1.001.
        sync::MtcGenerator g(sync::TimecodeMapping{sync::FrameRate::Fps25, {1, 0, 0, 0}});
        double origin = 10.0, speed = 1.001;
        std::int64_t originFrame = 9600;
        bool running = true, started = false;
        auto map = [&] { return sync::LinearMap(double(originFrame), origin, speed / kRate); };
        rig.midi = [&](std::int64_t from, std::uint32_t n, sync::EventBuffer& out) {
            if (!running) return;
            if (!started && originFrame < from + n) {
                g.start(double(std::max(originFrame, from)), map(), out);
                started = true;
            }
            if (started) g.run(double(from), double(from + n), map(), out);
        };
        rig.seconds(0.5);
        REQUIRE(rig.transport().playing);
        rig.seconds(8);
        auto truth = [&] { return map().positionAt(double(rig.device)); };
        double error = rig.transport().seconds - truth();
        MESSAGE("mtc chase error ", error * 1000, " ms");
        CHECK(std::abs(error) < 0.01);
        CHECK(rig.app->engine()->effectivePlayrate() == doctest::Approx(speed).epsilon(5e-4));
        CHECK(rig.app->head().project->playrate.rate == 1.0);
        // The master locates to 30 s: Steppe relocates and chases again.
        origin = 30.0;
        originFrame = rig.device;
        started = false;
        rig.seconds(4);
        error = rig.transport().seconds - truth();
        CHECK(std::abs(error) < 0.01);
        CHECK(rig.status().relocates + rig.status().jumps > 0);
        // A dropout: the transport stops.
        running = false;
        rig.seconds(0.4);
        CHECK_FALSE(rig.transport().playing);
        // A full frame while stopped (the master located): Steppe follows.
        rig.midi = [&](std::int64_t from, std::uint32_t, sync::EventBuffer& out) {
            if (from != rig.device) return;
            const std::uint8_t full[10] = {0xF0,         0x7F, 0x7F, 0x01, 0x01,
                                           (1 << 5) | 1, 0,    20,   0,    0xF7};
            out.push(double(from), full);  // 01:00:20:00 at 25 fps: timeline 20 s
        };
        rig.cycle();
        rig.seconds(0.05);
        CHECK_FALSE(rig.transport().playing);
        CHECK(rig.transport().seconds == doctest::Approx(20.0).epsilon(1e-6));
        rig.midi = [&](std::int64_t from, std::uint32_t n, sync::EventBuffer& out) {
            if (!running) return;
            if (!started && originFrame < from + n) {
                g.start(double(std::max(originFrame, from)), map(), out);
                started = true;
            }
            if (started) g.run(double(from), double(from + n), map(), out);
        };
        // Freewheel: it keeps going for 25 frames (one second) first.
        rig.run("sync.set", R"({"on_loss": "freewheel", "freewheel_frames": 25})");
        running = true;
        started = false;
        originFrame = rig.device;
        origin = 50.0;
        rig.seconds(2);
        REQUIRE(rig.transport().playing);
        running = false;
        rig.seconds(0.6);
        CHECK(rig.transport().playing);
        rig.seconds(0.8);
        CHECK_FALSE(rig.transport().playing);
    }
}

TEST_CASE("ltc: chase LTC on an input channel, and write LTC that decodes to the transport") {
    Rig rig(true, 4, static_cast<std::int64_t>(kRate * 12));
    rig.run("project.set", R"({"timecode": {"rate": "30", "start": "00:59:50:00"}})");
    rig.run("sync.set", R"({"source": "ltc", "ltc_input": 1, "ltc_output": true,
                            "ltc_output_channel": 3})");
    // The master's LTC: from its 01:00:00:00 (timeline 10 s here).
    sync::LtcEncoderConfig ec;
    ec.sampleRate = kRate;
    ec.mapping = sync::TimecodeMapping{sync::FrameRate::Fps30, {0, 59, 50, 0}};
    auto master = sync::LtcEncoder::create(ec);
    REQUIRE(master);
    const sync::LinearMap masterMap(0.0, 10.0, 1.0 / kRate);
    master->start(10.0);
    rig.audio = [&](std::int64_t from, std::span<float> out) {
        master->render(double(from), out, masterMap);
    };
    rig.seconds(1.0);
    REQUIRE(rig.transport().playing);
    rig.seconds(5.0);
    const double error = rig.transport().seconds - masterMap.positionAt(double(rig.device));
    MESSAGE("ltc chase error ", error * 1000, " ms");
    CHECK(std::abs(error) < 0.01);
    CHECK(rig.status().locked);
    CHECK(rig.status().ltcOutput);
    // The LTC written on output 3 names the transport's timecode.
    rig.app->stopEngine();
    const auto& captured = rig.backend->capture();
    REQUIRE(captured.channels.size() == 4);
    sync::LtcDecoderConfig dc;
    dc.sampleRate = kRate;
    dc.expected = sync::FrameRate::Fps30;
    auto decoder = sync::LtcDecoder::create(dc);
    REQUIRE(decoder);
    std::vector<sync::LtcFrame> frames;
    const auto& ch = captured.channels[2];
    for (std::size_t at = 0; at + kBlock <= ch.size(); at += kBlock)
        for (const auto& f :
             decoder->feed(double(at), std::span<const float>(ch.data() + at, kBlock)))
            frames.push_back(f);
    REQUIRE(frames.size() > 100);
    // Late frames are where the master's timecode is (to a frame: the chase).
    const auto& last = frames.back();
    const double heard = sync::toSeconds(double(last.count + 1), sync::FrameRate::Fps30);
    const double masterSeconds =
        10.0 + last.observation.frame / kRate +
        sync::TimecodeMapping{sync::FrameRate::Fps30, {0, 59, 50, 0}}.startSeconds();
    CHECK(std::abs(heard - masterSeconds) < 2.0 / 30.0);
    for (std::size_t i = frames.size() / 2 + 1; i < frames.size(); ++i)
        CHECK(frames[i].count == frames[i - 1].count + 1);
}

TEST_CASE("sync: offline renders ignore it and recording while slaved works") {
    Rig rig(true);
    rig.run("track.add", R"({"name": "Keys"})");
    audio::AudioData tone;
    tone.sampleRate = kRate;
    tone.channels.assign(2, std::vector<float>(96000));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] = i % 64 < 32 ? .2f : -.2f;
    REQUIRE(audio::writeWav(rig.dir / "tone.wav", tone));
    json::Value add = json::Value::object();
    add["track"] = "tr_1";
    add["audio_file"] = (rig.dir / "tone.wav").string();
    REQUIRE(rig.bus->execute({"item.add", add, kOrigin}));
    auto before = rig.run("render.mix", R"({"name": "before", "end_beat": 4})");
    rig.run("sync.set", R"({"source": "midi_clock", "midi_input": "Clock In"})");
    ClockMaster master;
    master.bpm = 123;
    master.startFrame = 2400;
    rig.midi = std::ref(master);
    rig.seconds(3);
    REQUIRE(rig.transport().playing);
    CHECK(rig.app->engine()->effectivePlayrate() != doctest::Approx(1.0).epsilon(1e-4));
    auto during = rig.run("render.mix", R"({"name": "during", "end_beat": 4})");
    auto a = audio::readAudio(before["path"].asString());
    auto b = audio::readAudio(during["path"].asString());
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->channels == b->channels);
    // Arm and record while the chase rate is not 1: refused only for a saved rate.
    rig.run("track.set", R"({"track": "tr_1", "armed": true, "record_input": [1]})");
    auto recording = rig.run("transport.record", "{}");
    CHECK(recording["recording"].asBool());
    rig.seconds(1);
    auto stopped = rig.run("transport.stop", "{}");
    REQUIRE(stopped["recorded"].size() == 1);
    // One take, one run: the chase's drift does not split it, nothing lost.
    const json::Value& take = stopped["recorded"].at(0);
    CHECK(take.find("takes")->size() == 1);
    CHECK(take.find("lost_frames")->asInt() == 0);
    CHECK(take.find("seconds")->asDouble() == doctest::Approx(1.0).epsilon(0.05));
    const model::Item* item = nullptr;
    for (const auto& e : rig.app->head().items)
        if (e.value->track == *model::Id::parse("tr_1") && e.value->takes.size() == 1 &&
            rig.app->head().takes.find(e.value->takes.front())->source &&
            rig.app->head().takes.find(e.value->takes.front())->source->kind == model::Kind::Media)
            item = e.value.get();
    REQUIRE(item);
}

TEST_CASE("ltc output: labelled with the audio leaving the device, through a loop wrap") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        Rig rig(ahead, 4, static_cast<std::int64_t>(kRate * 6));
        rig.latency(8);  // 512 frames: the masters' audio leaves that much after processing
        // Clicks at timeline 2 s and 6 s: LTC frames 60 and 180 at 30 fps.
        rig.item(clicks(8, {2.0, 6.0}));
        rig.run("sync.set", R"({"ltc_output": true, "ltc_output_channel": 3})");
        CHECK_FALSE(rig.app->engine()->syncConfig().listens());  // no sync plan for LTC alone
        rig.run("transport.seek", R"({"seconds": 5})");
        rig.run("transport.play", "{}");
        rig.seconds(1.5);
        rig.run("transport.pause", "{}");
        rig.seconds(0.1);
        const std::int64_t looped = rig.device;
        // A loop from 2 s to 3 s, from its middle: each wrap is heard at the 2 s click.
        rig.run("transport.loop", R"({"start_seconds": 2, "end_seconds": 3})");
        rig.run("transport.seek", R"({"seconds": 2.5})");
        rig.run("transport.play", "{}");
        rig.seconds(1.0);
        rig.app->stopEngine();
        const auto& captured = rig.backend->capture();
        const auto frames = decodeLtc(captured.channels[2]);
        const std::int64_t first = click(captured.channels[0], 0);
        const std::int64_t wrap = click(captured.channels[0], looped);
        REQUIRE(first > 0);
        REQUIRE(wrap > looped);
        // The frame that starts at 6 s starts where its audio is heard, to a
        // few samples (the latency is 512).
        const sync::LtcFrame* six = nullptr;
        const sync::LtcFrame* two = nullptr;
        for (const auto& f : frames) {
            if (f.count == 180 && !six) six = &f;
            if (f.count == 60 && f.start > double(looped) && !two) two = &f;
        }
        REQUIRE(six);
        MESSAGE("6 s: LTC frame at ", six->start, ", audio at ", first);
        CHECK(std::abs(six->start - double(first)) < 8.0);
        // After the wrap, LTC starts again at 2 s (frame 60) where the click is heard.
        REQUIRE(two);
        MESSAGE("wrap: LTC frame at ", two->start, ", audio at ", wrap);
        CHECK(std::abs(two->start - double(wrap)) < 8.0);
        // Silent while stopped: nothing between the pause (heard) and the next play.
        const auto& ltc = captured.channels[2];
        const auto paused = static_cast<std::size_t>(looped - 2400);
        CHECK(std::all_of(ltc.begin() + static_cast<std::ptrdiff_t>(paused),
                          ltc.begin() + static_cast<std::ptrdiff_t>(looped),
                          [](float x) { return x == 0.0f; }));
    }
}

TEST_CASE("sync: the rate path only for a source that can change the rate") {
    // What the masters play for a second and a half from 0 under each
    // setting, from its first sound: a sync plan (all live) starts sooner
    // after transport.play than one that renders ahead.
    auto play = [](const char* settings, bool ahead) {
        Rig rig(ahead, 4, static_cast<std::int64_t>(kRate * 2.5));
        audio::AudioData tone;
        tone.sampleRate = kRate;
        tone.channels.assign(2, std::vector<float>(static_cast<std::size_t>(kRate * 3)));
        for (std::size_t i = 0; i < tone.frames(); ++i)
            tone.channels[0][i] = tone.channels[1][i] = static_cast<float>(
                0.3 * std::sin(2 * 3.14159265358979 * 441.0 * double(i) / kRate));
        rig.item(tone);
        rig.run("sync.set", settings);
        rig.run("transport.play", "{}");
        rig.seconds(2);
        rig.app->stopEngine();
        const auto& captured = rig.backend->capture().channels;
        const auto onset =
            static_cast<std::ptrdiff_t>(std::find_if(captured[0].begin(), captured[0].end(),
                                                     [](float x) { return x != 0.0f; }) -
                                        captured[0].begin());
        std::vector<std::vector<float>> out;
        for (std::size_t c = 0; c < 2; ++c) {
            REQUIRE(onset + 72000 <= static_cast<std::ptrdiff_t>(captured[c].size()));
            out.emplace_back(captured[c].begin() + onset, captured[c].begin() + onset + 72000);
        }
        return out;
    };
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        const auto off = play(R"({"source": "off"})", ahead);
        // LTC output alone and MIDI clock without follow tempo never change the
        // rate: no stretch, the same samples as without sync.
        CHECK(play(R"({"ltc_output": true, "ltc_output_channel": 3})", ahead) == off);
        CHECK(play(R"({"source": "midi_clock", "midi_input": "Clock In", "follow_tempo": false})",
                   ahead) == off);
        // A chasing source (no master yet, so at exactly 1) plays through the
        // pitch-preserving stretch converters: close, not the same samples.
        const auto mtc = play(R"({"source": "mtc", "midi_input": "Clock In"})", ahead);
        double peak = 0.0, diff = 0.0, steady = 0.0;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < off[c].size(); ++i) {
                const double d = std::abs(double(off[c][i]) - double(mtc[c][i]));
                peak = std::max(peak, double(std::abs(off[c][i])));
                diff = std::max(diff, d);
                if (i >= 24000) steady = std::max(steady, d);  // after half a second
            }
        MESSAGE("stretch at 1.0: peak ", peak, ", largest difference ", diff, ", after 0.5 s ",
                steady);
        CHECK(peak > 0.1);
        CHECK(diff < 0.1 * peak);  // close, at the same time
    }
}

TEST_CASE("midi clock without follow tempo: Start plays where the master is heard") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        Rig rig(ahead);
        rig.latency(8);  // 512 frames, 10.7 ms
        rig.run("sync.set",
                R"({"source": "midi_clock", "midi_input": "Clock In", "follow_tempo": false})");
        ClockMaster master;  // 120 bpm, the project's tempo
        master.startFrame = 4800;
        rig.midi = std::ref(master);
        // The lead the start uses: the locate time measured so far (none yet).
        const double locate = rig.run("sync.get", "{}")["status"]["locate_ms"].asDouble() / 1000.0;
        rig.seconds(1.0);
        REQUIRE(rig.transport().playing);
        rig.seconds(2.0);
        CHECK(rig.app->engine()->effectivePlayrate() == 1.0);
        // Heard: what was processed the plan's latency ago. The start led by
        // that latency and the locate time, which a manual device does not
        // spend: so the transport is ahead by the locate time alone.
        const double heard = rig.transport().seconds - 512.0 / kRate;
        const double error = heard - master.beatAt(rig.device) * 0.5 - locate;
        MESSAGE("clock start error without follow tempo ", error * 1000, " ms (lead ",
                locate * 1000, " ms)");
        CHECK(std::abs(error) < 0.002);
    }
}

// --- MIDI clock and MTC output (ADR-0072) ------------------------------------

namespace {

// What one output port was sent, in order (absolute device frames).
std::vector<DummyBackend::CapturedMidi> sentTo(const Rig& rig, std::uint16_t port) {
    std::vector<DummyBackend::CapturedMidi> out;
    for (const auto& m : rig.backend->midiCapture())
        if (m.event.port == port) out.push_back(m);
    return out;
}

bool is(const DummyBackend::CapturedMidi& m, std::uint8_t status) {
    return m.event.size > 0 && m.event.bytes[0] == status;
}

// The clocks after `from` (absolute frame), in order.
std::vector<std::int64_t> clocksFrom(const std::vector<DummyBackend::CapturedMidi>& sent,
                                     std::int64_t from) {
    std::vector<std::int64_t> frames;
    for (const auto& m : sent)
        if (m.frame >= from && is(m, 0xF8)) frames.push_back(m.frame);
    return frames;
}

// The first message with `status` at or after `from`, or null.
const DummyBackend::CapturedMidi* first(const std::vector<DummyBackend::CapturedMidi>& sent,
                                        std::uint8_t status, std::int64_t from = 0) {
    for (const auto& m : sent)
        if (m.frame >= from && is(m, status)) return &m;
    return nullptr;
}

int songPosition(const DummyBackend::CapturedMidi& m) {
    return m.event.bytes[1] | (m.event.bytes[2] << 7);
}

}  // namespace

TEST_CASE("midi clock output: clocks at the heard frames across a tempo change and a loop wrap") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        Rig rig(ahead, 2, static_cast<std::int64_t>(kRate * 8));
        rig.latency(8);  // 512 frames: the audio leaves that much after processing
        rig.item(clicks(8, {1.0, 2.0}));
        // 120 bpm for four beats (2 s), then 90.
        rig.run("tempo.set",
                R"({"points": [{"beat": 0, "bpm": 120}, {"beat": 4, "bpm": 90, "ramp": false}]})");
        rig.run("midi.outputs.set",
                R"({"port": "Synth", "clock": true, "transport": true, "spp": true})");
        REQUIRE(rig.app->engine()->awaitPlan());
        rig.run("transport.seek", R"({"seconds": 0})");
        const std::int64_t played = rig.device;
        rig.run("transport.play", "{}");
        rig.seconds(3.0);
        rig.run("transport.pause", "{}");
        rig.seconds(0.2);
        const std::int64_t looped = rig.device;
        rig.run("transport.loop", R"({"start_seconds": 2, "end_seconds": 3})");
        rig.run("transport.seek", R"({"seconds": 2.5})");
        rig.run("transport.play", "{}");
        rig.seconds(1.0);
        rig.app->stopEngine();
        const auto& audio = rig.backend->capture().channels[0];
        const auto sent = sentTo(rig, 0);
        // Timeline 1 s is heard at the first click: the frame every position is heard at.
        const std::int64_t one = click(audio, played);
        REQUIRE(one > 0);
        const auto heard = [&](double seconds) { return double(one) + (seconds - 1.0) * kRate; };
        const auto& map = *rig.app->head().timeMap;
        // Start where the transport starts to be heard (the plan's pre-roll
        // before timeline 0), then clock k where beat k/24 is.
        const auto* start = first(sent, 0xFA, played);
        REQUIRE(start);
        CHECK(std::abs(double(start->frame) - heard(-512.0 / kRate)) <= 1.0);
        const auto clocks = clocksFrom(sent, start->frame);
        REQUIRE(clocks.size() > 140);  // 3 s: 96 clocks at 120, then 36 a second at 90
        int worst = 0;
        for (std::size_t k = 0; k < 130; ++k) {
            const double expected = heard(map.secondsAt(double(k) / 24.0));
            worst = std::max(worst, int(std::abs(double(clocks[k]) - std::ceil(expected))));
        }
        MESSAGE("largest clock error, frames: ", worst);
        CHECK(worst <= 1);
        // The pause: Stop, then the Song Position where it stands.
        const auto* stop = first(sent, 0xFC, start->frame);
        REQUIRE(stop);
        CHECK(stop->frame < looped);
        // Resuming at 2.5 s (beat 4.75): Song Position 19 (the next 16th) and Continue.
        const auto* resume = first(sent, 0xFB, looped);
        REQUIRE(resume);
        const auto* spp = first(sent, 0xF2, looped);
        REQUIRE(spp);
        CHECK(songPosition(*spp) == 19);
        // The wrap at 3 s, heard where the 2 s click is: Stop, Song Position 16
        // (beat 4), Continue, and the clocks of beat 4 on at 90 bpm.
        const std::int64_t wrap = click(audio, looped);
        REQUIRE(wrap > looped);
        const auto* wrapStop = first(sent, 0xFC, resume->frame + 1);
        REQUIRE(wrapStop);
        CHECK(std::abs(wrapStop->frame - wrap) <= 1);
        const auto* wrapSpp = first(sent, 0xF2, wrapStop->frame);
        const auto* wrapContinue = first(sent, 0xFB, wrapStop->frame);
        REQUIRE(wrapSpp);
        REQUIRE(wrapContinue);
        CHECK(songPosition(*wrapSpp) == 16);
        CHECK(std::abs(wrapContinue->frame - wrap) <= 1);
        const auto after = clocksFrom(sent, wrapContinue->frame);
        REQUIRE(after.size() > 10);
        const double period = 60.0 / 90.0 / 24.0 * kRate;
        for (std::size_t k = 0; k < 10; ++k)
            CHECK(std::abs(double(after[k]) - std::ceil(double(wrap) + double(k) * period)) <= 1.0);
    }
}

TEST_CASE("midi clock output: Song Position and Continue from mid-bar, a locate, a count-in") {
    Rig rig(false, 2, static_cast<std::int64_t>(kRate * 10));
    rig.run("midi.outputs.set",
            R"({"port": "Synth", "clock": true, "transport": true, "spp": true})");
    // From 1.3 s at 120 bpm (beat 2.6): Song Position 11, Continue at once,
    // and the first clock at the 16th's frame (beat 2.75, 0.075 s later).
    rig.run("transport.seek", R"({"seconds": 1.3})");
    rig.seconds(0.1);
    const std::int64_t played = rig.device;
    rig.run("transport.play", "{}");
    rig.seconds(0.5);
    const std::int64_t located = rig.device;
    rig.run("transport.seek", R"({"seconds": 4})");  // a locate while playing
    rig.seconds(0.5);
    rig.run("transport.pause", "{}");
    rig.seconds(0.1);
    // A count-in: nothing is sent until the timeline moves.
    rig.run("transport.metronome", R"({"count_in_bars": 1, "count_in_playback": true})");
    rig.run("transport.seek", R"({"seconds": 0})");
    rig.seconds(0.1);
    const std::int64_t counted = rig.device;
    rig.run("transport.play", "{}");
    rig.seconds(3.0);
    rig.app->stopEngine();
    const auto sent = sentTo(rig, 0);
    // The cycle the play landed in: Song Position, Continue, then clocks.
    const auto* spp = first(sent, 0xF2, played);
    const auto* cont = first(sent, 0xFB, played);
    REQUIRE(spp);
    REQUIRE(cont);
    CHECK(songPosition(*spp) == 11);
    CHECK(spp->frame <= cont->frame);
    const auto* stray = first(sent, 0xFA, played);  // no Start away from beat 0
    CHECK((stray == nullptr || stray->frame >= counted));
    const auto clocks = clocksFrom(sent, cont->frame);
    REQUIRE(clocks.size() > 2);
    CHECK(std::abs(double(clocks[0] - cont->frame) - 0.075 * kRate) <= 1.0);
    CHECK(std::abs(double(clocks[1] - clocks[0]) - 1000.0) <= 1.0);  // 120 bpm: 1000 frames
    // The locate: Stop, Song Position 32 (beat 8), Continue, in that order at one frame.
    const auto* stop = first(sent, 0xFC, located);
    REQUIRE(stop);
    const auto* spp2 = first(sent, 0xF2, located);
    const auto* cont2 = first(sent, 0xFB, located);
    REQUIRE(spp2);
    REQUIRE(cont2);
    CHECK(songPosition(*spp2) == 32);
    CHECK(stop->frame == spp2->frame);
    CHECK(spp2->frame == cont2->frame);
    CHECK(stop < spp2);
    CHECK(spp2 < cont2);
    // The count-in (one bar, 2 s): Start comes when it ends, not before.
    const auto* start = first(sent, 0xFA, counted);
    REQUIRE(start);
    CHECK(start->frame - counted > static_cast<std::int64_t>(1.9 * kRate));
    CHECK(clocksFrom(sent, counted).front() >= start->frame);
}

TEST_CASE("mtc output: quarter frames decode to the heard position, with plan latency") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        Rig rig(ahead, 2, static_cast<std::int64_t>(kRate * 6));
        rig.latency(8);  // 512 frames
        rig.item(clicks(6, {2.0}));
        rig.run("midi.outputs.set", R"({"port": "Synth", "mtc": true})");
        rig.run("transport.seek", R"({"seconds": 1})");
        const std::int64_t played = rig.device;
        rig.run("transport.play", "{}");
        rig.seconds(2.5);
        rig.app->stopEngine();
        const auto& audio = rig.backend->capture().channels[0];
        const std::int64_t two = click(audio, played);
        REQUIRE(two > 0);
        const double start = rig.app->head().project->timecode.startSeconds();
        sync::MtcReceiverConfig rc;
        rc.sampleRate = kRate;
        rc.expected = rig.app->head().project->timecode.rate;
        sync::MtcReceiver receiver(rc);
        double worst = 0.0;
        int observed = 0;
        for (const auto& m : sentTo(rig, 0)) {
            if (!receiver.feed(double(m.frame),
                               std::span<const std::uint8_t>(m.event.bytes, m.event.size)))
                continue;
            if (!receiver.locked() || double(m.frame) < double(two) - 0.5 * kRate) continue;
            const sync::Observation o = receiver.lastObservation();
            // Heard at: the 2 s click's frame, plus the position's distance from 2 s.
            const double expected = double(two) + (o.position - start - 2.0) * kRate;
            worst = std::max(worst, std::abs(o.frame - expected));
            ++observed;
        }
        MESSAGE("MTC: largest error ", worst, " frames over ", observed, " quarter frames");
        CHECK(observed > 100);
        CHECK(worst < 16.0);  // a 30 fps frame is 1600
    }
}

TEST_CASE("midi clock output: per-port offset, and a negative latency predicted ahead") {
    for (bool ahead : {false, true}) {
        CAPTURE(ahead);
        // Synth's MIDI is 100 frames faster than its audio; Clock Out's 300 slower.
        Rig rig(ahead, 2, static_cast<std::int64_t>(kRate * 4), {100, -300});
        rig.item(clicks(4, {0.5}));
        rig.run("midi.outputs.set",
                R"({"port": "Synth", "clock": true, "transport": true, "offset_ms": 5})");
        rig.run("midi.outputs.set", R"({"port": "Clock Out", "clock": true, "transport": true,
                                        "spp": true})");
        auto ports = rig.run("midi.outputs.get", "{}")["ports"];
        REQUIRE(ports.size() == 2);  // the device's order
        CHECK(ports.asArray()[1]["name"].asString() == "Clock Out");
        CHECK(ports.asArray()[1]["delay_ms"].asDouble() == doctest::Approx(-300.0 / 48.0));
        CHECK(ports.asArray()[0]["delay_ms"].asDouble() ==
              doctest::Approx(340.0 / 48.0));  // 100 frames + 5 ms
        CHECK(ports.asArray()[0]["latency_ms"].asDouble() == doctest::Approx(100.0 / 48.0));
        rig.run("transport.seek", R"({"seconds": 0})");
        const std::int64_t played = rig.device;
        rig.run("transport.play", "{}");
        rig.seconds(2.0);
        auto report = rig.run("midi.outputs.get", "{}")["ports"];
        CHECK(report.asArray()[0]["dropped"].asInt() == 0);
        CHECK(report.asArray()[0]["sent"].asInt() > 40);
        rig.app->stopEngine();
        const std::int64_t heard0 = click(rig.backend->capture().channels[0], played) - 24000;
        // 120 bpm: clock k is heard at heard0 + 1000 k.
        const auto synth = clocksFrom(sentTo(rig, 0), played);
        REQUIRE(synth.size() > 40);
        for (std::size_t k = 0; k < 40; ++k)
            CHECK(std::abs(synth[k] - (heard0 + 1000 * std::int64_t(k) + 340)) <= 1);
        // A negative delay cannot be met for what is already processed: the
        // start lands with the first cycle (late by up to 300 frames), at the
        // 16th the timeline will be at, and the clocks are predicted ahead.
        const auto out = sentTo(rig, 1);
        const auto* cont = first(out, 0xFB, played);
        REQUIRE(cont);
        const DummyBackend::CapturedMidi* spp = nullptr;  // sent with the Continue
        for (const auto& m : out)
            if (is(m, 0xF2) && m.frame == cont->frame) spp = &m;
        REQUIRE(spp);
        CHECK(songPosition(*spp) == 1);
        CHECK(cont->frame == heard0);
        const auto clocks = clocksFrom(out, played);
        REQUIRE(clocks.size() > 40);
        for (std::size_t k = 0; k < 30; ++k)
            CHECK(std::abs(clocks[k] - (heard0 + 1000 * std::int64_t(k + 6) - 300)) <= 1);
    }
}

TEST_CASE("midi clock output: clock while stopped, and nothing echoed to the sync master") {
    Rig rig(false, 2, 0);
    rig.run("midi.outputs.set", R"({"port": "Synth", "clock": true, "clock_while_stopped": true})");
    rig.seconds(0.5);
    const auto clocks = clocksFrom(sentTo(rig, 0), 0);
    REQUIRE(clocks.size() > 20);
    for (std::size_t k = 1; k < clocks.size(); ++k)
        CHECK(std::abs(double(clocks[k] - clocks[k - 1]) - 1000.0) <= 1.0);  // 120 bpm
    // The sync master's device ("Clock In"/"Clock Out"): its clock is not sent
    // back to it, with a warning; MTC would still go.
    rig.run("sync.set", R"({"source": "midi_clock", "midi_input": "Clock In"})");
    auto r = rig.bus->execute(
        {"midi.outputs.set",
         json::parse(R"({"port": "Clock Out", "clock": true, "transport": true, "mtc": true})")
             .value(),
         kOrigin});
    REQUIRE(r);
    const SyncOutput& out = rig.app->engine()->syncConfig().outputs[1];
    CHECK_FALSE(out.clock);
    CHECK_FALSE(out.transport);
    CHECK(out.mtc);
    auto ports = (*r)["ports"];
    REQUIRE(ports.size() == 2);
    auto& clockOut = ports.asArray()[1];
    CHECK(clockOut["name"].asString() == "Clock Out");
    CHECK(clockOut["index"].asInt() == 1);
    REQUIRE(clockOut["suppressed"].size() == 2);
    CHECK(clockOut["suppressed"].asArray()[0].asString() == "clock");
    CHECK(clockOut["sends"].asArray()[0].asString() == "mtc");
    REQUIRE(clockOut["warnings"].size() == 1);  // on the port, and in the sync warnings
    bool warned = false;
    const json::Value got = rig.run("sync.get", "{}");
    for (const auto& w : got.find("status")->find("warnings")->asArray())
        warned = warned || w.asString().find("Clock Out") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("midi outputs: get, set, validation, engine.status and the JSON round trip") {
    Rig rig(false);
    // Every output the device has, at the defaults.
    auto got = rig.run("midi.outputs.get", "{}");
    REQUIRE(got["ports"].size() == 2);
    auto& synth = got["ports"].asArray()[0];
    CHECK(synth["name"].asString() == "Synth");
    CHECK(synth["index"].asInt() == 0);
    CHECK(synth["enabled"].asBool());
    CHECK(synth["reset_on_stop"].asBool());
    CHECK_FALSE(synth["clock"].asBool());
    CHECK(rig.app->midiOutputSettings().empty());
    auto set = rig.run("midi.outputs.set", R"({"port": "Synth", "clock": true})");
    CHECK(set["ports"].asArray()[0]["clock"].asBool());
    CHECK(set["ports"].asArray()[0]["sends"].asArray()[0].asString() == "clock");
    CHECK(rig.app->engine()->syncConfig().outputs[0].clock);
    // Omitted members keep their values.
    set = rig.run("midi.outputs.set", R"({"port": "Synth", "spp": true})");
    CHECK(set["ports"].asArray()[0]["clock"].asBool());
    CHECK(set["ports"].asArray()[0]["spp"].asBool());
    // A disabled port sends nothing, and keeps its clock settings.
    rig.run("midi.outputs.set", R"({"port": "Synth", "enabled": false})");
    CHECK_FALSE(rig.app->engine()->syncConfig().outputs[0].clock);
    CHECK(rig.app->midiOutputSettings().front().clock);
    rig.run("midi.outputs.set", R"({"port": "Synth", "enabled": true})");
    CHECK(rig.app->engine()->syncConfig().outputs[0].clock);
    for (const char* bad :
         {R"({"port": "Synth", "offset_ms": 5000})", R"({"port": "Synth", "beat": true})",
          R"({"clock": true})", R"({"port": ""})", R"({"port": "Synth", "mtc": 1})",
          R"({"port": "Synth", "enabled": "yes"})"}) {
        auto r = rig.bus->execute({"midi.outputs.set", json::parse(bad).value(), kOrigin});
        REQUIRE_FALSE(r);
        INFO(bad, r.error().toString());
        const Error& e = r.error().details.empty() ? r.error() : r.error().details.front();
        CHECK_FALSE(e.hint.empty());
    }
    // A port that is not there is kept, after the device's, with a warning.
    auto r = rig.bus->execute({"midi.outputs.set",
                               json::parse(R"({"port": "Drum Machine", "mtc": true})").value(),
                               kOrigin});
    REQUIRE(r);
    REQUIRE((*r)["ports"].size() == 3);
    CHECK((*r)["ports"].asArray()[2]["name"].asString() == "Drum Machine");
    CHECK((*r)["ports"].asArray()[2]["index"].asInt() == -1);
    auto status = rig.run("engine.status", "{}");
    REQUIRE(status["midi_outputs"].size() == 3);
    CHECK(status["midi_outputs"].asArray()[0]["sends"].asArray()[0].asString() == "clock");
    // Back at every default: the record goes.
    rig.run("midi.outputs.set", R"({"port": "Drum Machine", "mtc": false})");
    CHECK(rig.app->midiOutputSettings().size() == 1);
    // The records round-trip through preferences' {"<port>": {...}}.
    MidiOutputSettings a;
    a.port = "B";
    a.mtc = true;
    a.offsetMs = -2.5;
    a.resetOnStop = false;
    MidiOutputSettings b;
    b.port = "A";
    b.enabled = false;
    b.clock = b.transport = b.spp = b.clockWhileStopped = true;
    const json::Value saved = midiOutputListJson({a, b});
    CHECK(saved.find("A"));
    CHECK_FALSE(saved.find("A")->find("port"));
    auto back = parseMidiOutputList(saved);
    REQUIRE(back);
    REQUIRE(back->size() == 2);
    CHECK((*back)[0] == b);
    CHECK((*back)[1] == a);
    CHECK_FALSE(parseMidiOutputList(json::parse(R"([{"port": "A"}])").value()));
    auto wrong = parseMidiOutputList(json::parse(R"({"A": {"offset_ms": 5000}})").value());
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().path == "/A/offset_ms");
}

TEST_CASE("midi clock output: no slave is left playing") {
    Rig rig(false, 2, 0);
    rig.run("midi.outputs.set", R"({"port": "Synth", "clock": true, "transport": true})");
    rig.run("midi.outputs.set", R"({"port": "Clock Out", "clock": true, "transport": true})");
    rig.run("transport.play", "{}");
    rig.seconds(0.3);
    // Transport switched off while playing: Stop at once, nothing after.
    const std::int64_t off = rig.device;
    rig.run("midi.outputs.set", R"({"port": "Synth", "transport": false, "clock": false})");
    rig.seconds(0.2);
    auto synth = sentTo(rig, 0);
    const auto* stop = first(synth, 0xFC, off);
    REQUIRE(stop);
    CHECK(stop->frame - off <= 2 * std::int64_t(kBlock));
    CHECK(synth.back().frame == stop->frame);
    // A sync rebuild: the old runtime stops Clock Out, the new one continues it.
    const std::int64_t rebuilt = rig.device;
    rig.run("sync.set", R"({"source": "mtc", "midi_input": "Desk"})");
    rig.seconds(0.2);
    auto out = sentTo(rig, 1);
    const auto* stop2 = first(out, 0xFC, rebuilt);
    const auto* cont = first(out, 0xFB, rebuilt);
    REQUIRE(stop2);
    REQUIRE(cont);
    CHECK(stop2->frame <= cont->frame);
    rig.run("sync.set", R"({"source": "off"})");
    rig.seconds(0.1);
    // Disabled while playing: Stop.
    const std::int64_t disabled = rig.device;
    rig.run("midi.outputs.set", R"({"port": "Clock Out", "enabled": false})");
    rig.seconds(0.1);
    out = sentTo(rig, 1);
    REQUIRE(first(out, 0xFC, disabled));
    CHECK(out.back().event.bytes[0] == 0xFC);
    // The engine stops while a slave plays: the final callback sends Stop.
    rig.run("midi.outputs.set", R"({"port": "Clock Out", "enabled": true})");
    rig.seconds(0.1);
    const std::int64_t stopped = rig.device;
    rig.app->stopEngine();
    out = sentTo(rig, 1);
    REQUIRE_FALSE(out.empty());
    CHECK(out.back().event.bytes[0] == 0xFC);
    CHECK(out.back().frame >= stopped);
}

TEST_CASE("midi clock output: engine stop sends Stop from a running cycle") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-sync-stop-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    AppOptions options;
    options.workdir = dir;
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    DummyBackend::Options o;
    o.mode = DummyBackend::Mode::Thread;
    o.paced = true;
    o.midiOutputPorts = {"Synth"};
    DummyBackend backend(o);
    REQUIRE(app->startEngine(backend,
                             {.sampleRate = kRate, .blockSize = kBlock, .midiOutputs = true}, {})
                .ok());
    auto run = [&](const char* name, const char* args) {
        auto r = bus->execute({name, json::parse(args).value(), kOrigin});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    };
    run("midi.outputs.set", R"({"port": "Synth", "clock": true, "transport": true})");
    run("transport.play", "{}");
    for (int i = 0; i < 300 && !app->engine()->syncConfig().outputs[0].transport; ++i) {
        app->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    app->tick();
    app->stopEngine();
    const auto& sent = backend.midiCapture();
    REQUIRE_FALSE(sent.empty());
    CHECK(sent.back().event.bytes[0] == 0xFC);  // the last message: Stop
    int stops = 0;
    for (const auto& m : sent) stops += m.event.bytes[0] == 0xFC;
    CHECK(stops == 1);
    bus.reset();
    app.reset();
    fs::remove_all(dir);
}
