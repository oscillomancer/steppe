// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// External sync in the engine (ADR-0071): a manual dummy device scripted with
// MIDI clock, MTC and LTC from libs/sync's generators; the transport follows,
// the chase rate never reaches the project, offline renders ignore it, and
// LTC output decodes back to the transport.
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

    explicit Rig(bool ahead, std::uint32_t outputs = 2, std::int64_t capture = 0)
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
                                  .midiInputs = true},
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
