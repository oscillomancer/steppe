// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Portable fixtures shared by real-time equivalence and plan-swap soak tests.
#pragma once
#include <doctest/doctest.h>
#include <supple/engine/realtime.h>
#include <algorithm>
#include <cmath>

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;
namespace {
const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

inline InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
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

// Runs a manual backend for what is rendered ahead of `cycles` (at most
// 4096 frames) and returns how many cycles it ran: called once awaitRendered
// has returned, so that no cycle's audio depends on when the renderer's
// thread runs (round 219: a seed's repeat differed by a cycle at play
// starts under load).
// - It plays only frames already rendered: past them the renderer races it.
// - With nothing of the epoch rendered while playing (awaitRendered also
//   returns for a renderer that can do nothing until the callback runs:
//   its rings full of an older epoch's chunks, which the callback drops, or
//   holding for the end the callback's next cycle records), one cycle,
//   with the renderer held: a pre-roll on every run. Unheld, the renderer,
//   woken in that cycle, rendered the block its pre-roll waits for before
//   the cycle looked, or after, as the load had it.
inline std::int64_t runRendered(Engine& engine, DummyBackend& backend, std::int64_t cycles,
                                std::uint32_t block) {
    const std::int64_t ahead = engine.renderedAhead();
    if (ahead == 0 && engine.transport().playing) {
        engine.holdRenderer(true);
        while (!engine.awaitRendererHeld()) {
        }
        backend.run(1);
        engine.holdRenderer(false);
        return 1;
    }
    const std::int64_t n = std::min<std::int64_t>(
        cycles, std::max<std::int64_t>(1, std::min<std::int64_t>(ahead, 4096) / block));
    backend.run(n);
    return n;
}

// Anticipative: drive a manual backend `cycles` times, letting the renderer
// keep ahead (a device's callbacks would be paced by its clock).
inline void runAhead(Engine& engine, DummyBackend& backend, std::int64_t cycles,
                     std::uint32_t block) {
    while (cycles > 0) {
        REQUIRE(engine.awaitRendered(4096));
        cycles -= runRendered(engine, backend, cycles, block);
    }
}

// As runAhead, decided by the renderer's state alone: each wait lasts until
// it is ahead, however long a loaded machine (or a renderer waiting on a
// crashed bridge) takes; one that never gets there is the test runner's
// timeout. (awaitRendered is false only when its deadline passes.)
inline void runAheadOnState(Engine& engine, DummyBackend& backend, std::int64_t cycles,
                            std::uint32_t block) {
    while (cycles > 0) {
        while (!engine.awaitRendered(4096)) {
        }
        cycles -= runRendered(engine, backend, cycles, block);
    }
}

}  // namespace
