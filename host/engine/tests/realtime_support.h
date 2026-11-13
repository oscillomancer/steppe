// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Fixtures the real-time tests share (test_realtime*.cpp, split so each part
// stays within the test timeout under the sanitizers).
#pragma once

#include <clap/plugin.h>
#include <doctest/doctest.h>
#include "supple/engine/bench_probe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <semaphore>
#include <set>
#include <thread>
#include <tuple>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/engine/jack.h>
#include <supple/engine/realtime.h>
#include <supple/host/clap_host.h>

#include "fake_jack.h"
#include "live_workers.h"
#include "meters.h"
#include "plan.h"
#include "recording.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
const std::filesystem::path kVst3Fixture = FIXTURE_VST3_PATH;

inline InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

inline InstancePool makeVstPool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::array paths{kFixture, kVst3Fixture};
        auto list = host::scan(paths);
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

// A track playing a sine instrument: notes on beats 0, 1, 2, 3 (quarter notes).
inline model::Session synthSession(double volumeDb = 0.0, const char* extraFx = nullptr,
                                   unsigned tracks = 1) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    for (unsigned channel = 0; channel < tracks; ++channel) {
        model::Track tr;
        tr.name = "Synth";
        tr.parent = kMaster;
        tr.volumeDb = volumeDb;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
        for (const char* plugin : {"com.suppleaudio.test.sine", extraFx}) {
            if (!plugin) continue;
            model::FxInstance fx;
            fx.plugin.id = plugin;
            if (std::string(plugin) == "com.suppleaudio.test.gain")
                fx.patch =
                    json::parse(
                        R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                        .value();
            const Id f = t.create(fx);
            t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
        }
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 4 * 960;
        for (int i = 0; i < 4; ++i)
            src.sequence.notes.push_back(
                {i * 960, 480, 0, static_cast<std::uint8_t>(60 + 4 * i), 100, 0});
        const Id mi = t.create(src);
        model::Take take;
        take.source = mi;
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = 4.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    return t.commit().value();
}

// A sine track whose notes start at the given ticks (ppq 960, 120 bpm: 25 samples a tick).
inline model::Session clusterSession(std::initializer_list<int> ticks) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Synth";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 4 * 960;
    std::uint8_t key = 60;
    for (int tick : ticks) src.sequence.notes.push_back({tick, 960, 0, key++, 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

inline double maxDiff(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b,
                      std::size_t n) {
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(double(a[aFrom + i]) - double(b[i])));
    return worst;
}

const EngineOptions kInCallback{.anticipative = false, .workers = 0};

// Anticipative: drive a manual backend `cycles` times, letting the renderer
// keep ahead (a device's callbacks would be paced by its clock).
inline void runAhead(Engine& engine, DummyBackend& backend, std::int64_t cycles,
                     std::uint32_t block) {
    while (cycles > 0) {
        REQUIRE(engine.awaitRendered(4096));
        const std::int64_t n =
            std::min<std::int64_t>(cycles, std::max<std::int64_t>(1, 4096 / block));
        backend.run(n);
        cycles -= n;
    }
}

}  // namespace

namespace {
// A sine instrument track that monitors (and may record) every MIDI device.
inline model::Session keyboardSession(bool armed = false) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Keys";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    model::Route in;
    in.from.midiDevice = "key";  // matches the "Keyboard" port
    in.to.track = track;
    in.midi = model::MidiRouting{1, 0};  // channel 1 only
    const Id r = t.create(in);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.chain.push_back(f);
        x.record.input = r;
        x.record.armed = true;  // monitoring needs the arm; auto when it records
        x.record.monitor = armed ? model::Monitor::Auto : model::Monitor::On;
    });
    return t.commit().value();
}

// Device MIDI at fixed device frames: {frame, status, data1, data2}.
inline DummyBackend::Options keyboard(std::vector<std::array<int, 4>> events,
                                      std::int64_t capture) {
    return {
        .mode = DummyBackend::Mode::Manual,
        .captureFrames = capture,
        .midiPorts = {"Keyboard", "Pads"},
        .midi = [events](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
            for (const auto& e : events)
                if (e[0] >= frame && e[0] < frame + n) {
                    MidiInputEvent m;
                    m.frame = static_cast<std::uint32_t>(e[0] - frame);
                    m.size = 3;
                    m.bytes[0] = static_cast<std::uint8_t>(e[1]);
                    m.bytes[1] = static_cast<std::uint8_t>(e[2]);
                    m.bytes[2] = static_cast<std::uint8_t>(e[3]);
                    out.push_back(m);
                }
        }};
}
}  // namespace

namespace {
// keyboardSession's track, its instrument `plugin`, holding one timeline
// note (key 60) for eight beats, 4 s at 120 bpm, with the given arm and
// monitor mode; the master playrate `rate`.
inline model::Session heldNoteSession(bool armed, model::Monitor monitor, const std::string& plugin,
                                      double rate = 1.0, bool preserve = false) {
    const model::Session base = keyboardSession();
    model::EditTxn t(base, 2);
    auto project = t.project();
    project.playrate = {rate, preserve};
    t.setProject(project);
    Id track;
    for (const auto& e : base.tracks)
        if (e.value->name == "Keys") track = e.id;
    for (const auto& e : base.fx)
        t.modify<model::FxInstance>(e.id, [&](model::FxInstance& fx) { fx.plugin.id = plugin; });
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.record.armed = armed;
        x.record.monitor = monitor;
    });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 8 * 960;
    src.sequence.notes = {{0, 8 * 960, 0, 60, 100, 0}};
    model::Take take;
    take.source = t.create(src);
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 8.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

// Runs `frames` device frames (rounded up to whole callbacks), the
// anticipative engine kept ahead as a device's clock would.
inline void runFrames(Engine& engine, DummyBackend& backend, const EngineOptions& options,
                      std::int64_t frames, std::uint32_t block) {
    const std::int64_t cycles = (frames + block - 1) / block;
    if (options.anticipative)
        runAhead(engine, backend, cycles, block);
    else
        backend.run(cycles);
}

// The instrument on twoKeysSession's "Keys" track.
inline Id keysInstrument(const model::Session& s) {
    for (const auto& e : s.tracks)
        if (e.value->name == "Keys") return e.value->chain.front();
    return {};
}

const std::array<EngineOptions, 4> kBothExecutors = {
    kInCallback, EngineOptions{.anticipative = false, .workers = 2}, EngineOptions{.workers = 0},
    EngineOptions{.workers = 2}};
}  // namespace

namespace {
// A track with keyboardSession's MIDI input, a sine and the edge fixture as a
// 1 s echo (a delay with pdc: ignore, so the delay is heard as a tail): one
// short note at 0, heard again at 1 s. With `pads`, a second sine track,
// "Pads", on the same input, armed and monitored, and silent, so the live
// region exists before the echo strip joins it.
inline model::Session echoSession(bool armed, model::Monitor monitor, bool pads = false) {
    const model::Session base = keyboardSession();
    model::EditTxn t(base, 2);
    Id track;
    for (const auto& e : base.tracks)
        if (e.value->name == "Keys") track = e.id;
    if (pads) {
        model::Track tr;
        tr.name = "Pads";
        tr.parent = kMaster;
        const Id p = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(p); });
        model::FxInstance sine;
        sine.plugin.id = "com.suppleaudio.test.sine";
        const Id f = t.create(sine);
        model::Route in;
        in.from.midiDevice = "key";
        in.to.track = p;
        in.midi = model::MidiRouting{1, 0};
        const Id r = t.create(in);
        t.modify<model::Track>(p, [&](model::Track& x) {
            x.chain.push_back(f);
            x.record.input = r;
            x.record.armed = true;
            x.record.monitor = model::Monitor::On;
        });
    }
    model::FxInstance echo;
    echo.plugin.id = "com.suppleaudio.test.edge";
    echo.pdc = model::PdcMode::Ignore;
    echo.state = "sha256:echo-48000";
    t.putBlob(*echo.state, std::make_shared<const model::Blob>(model::Blob{{49, 0x80, 0xbb, 0}}));
    const Id fx = t.create(echo);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.chain.push_back(fx);
        x.record.armed = armed;
        x.record.monitor = monitor;
    });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 960;
    src.sequence.notes = {{0, 240, 0, 60, 100, 0}};  // 6000 frames
    model::Take take;
    take.source = t.create(src);
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 1.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

// Two sine tracks on keyboardSession's MIDI input: "Pads", armed and
// monitored (so the live region exists) and silent, and "Keys", holding key
// 60 for two beats (1 s), armed as given; the master playrate `rate`, or,
// with `envelope`, a playrate envelope from 1 to 1.5 over two seconds.
inline model::Session twoKeysSession(bool armKeys, double rate = 1.0, bool preserve = false,
                                     bool envelope = false) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    auto project = t.project();
    project.playrate = {rate, preserve};
    t.setProject(project);
    if (envelope) {
        model::Envelope env;
        env.target = {kMaster, "playrate"};
        env.timebase = model::Timebase::Time;
        env.points = {{0, 1.0}, {2.0, 1.5}};
        t.create(env);
    }
    for (const char* name : {"Pads", "Keys"}) {
        model::Track tr;
        tr.name = name;
        tr.parent = kMaster;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.sine";
        const Id f = t.create(fx);
        model::Route in;
        in.from.midiDevice = "key";
        in.to.track = track;
        in.midi = model::MidiRouting{1, 0};
        const Id r = t.create(in);
        const bool keys = std::string(name) == "Keys";
        t.modify<model::Track>(track, [&](model::Track& x) {
            x.chain.push_back(f);
            x.record.input = r;
            x.record.armed = !keys || armKeys;
            x.record.monitor = model::Monitor::On;
        });
        if (!keys) continue;
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 4 * 960;
        src.sequence.notes = {{0, 2 * 960, 0, 60, 100, 0}};
        model::Take take;
        take.source = t.create(src);
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = 4.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    return t.commit().value();
}
}  // namespace

namespace {
// A compensated take (64 frames of latency after its instrument) whose
// notes are (tick, duration) pairs, 960 ticks a beat at 120 BPM (a tick is
// 25 frames), from the start of an item at `position` seconds, `length`
// seconds long.
inline model::Session compensatedTakeSession(double rate, std::vector<std::pair<int, int>> notes,
                                             double position = 0.0, double length = 4.0) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    auto project = t.project();
    project.playrate = {rate, false};
    t.setProject(project);
    model::Track track;
    track.parent = kMaster;
    const Id tr = t.create(track);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
    model::FxInstance synth;
    synth.plugin.id = "com.suppleaudio.test.sine";
    model::FxInstance delay;
    delay.plugin.id = "com.suppleaudio.test.gain";
    delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                  "params":{"latency":"short"}})")
                      .value();
    model::Take take;
    take.chain = {t.create(synth), t.create(delay)};
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 16 * 960;
    for (const auto& [tick, duration] : notes)
        src.sequence.notes.push_back({tick, duration, 0, 72, 100, 0});
    take.source = t.create(src);
    const Id tk = t.create(take);
    model::Item item;
    item.track = tr;
    item.timebase = model::ItemTimebase::Time;
    item.position = position;
    item.length = length;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

inline double peakOf(const std::vector<float>& v, std::size_t from, std::size_t n) {
    double peak = 0;
    for (std::size_t i = from; i < from + n && i < v.size(); ++i)
        peak = std::max(peak, std::fabs(double(v[i])));
    return peak;
}
}  // namespace
