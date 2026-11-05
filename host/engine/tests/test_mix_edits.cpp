// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mix edits on a running engine: dummy devices and the in-process fixture
// instrument only.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
#include "supple/model/session.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

const EngineOptions kInCallback{.anticipative = false, .workers = 0};

Id synthTrack(model::EditTxn& t, Id parent, std::uint8_t key) {
    model::Track tr;
    tr.name = "Synth";
    tr.parent = parent;
    const Id track = t.create(tr);
    t.modify<model::Track>(parent, [&](model::Track& p) { p.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 8 * 960;
    for (int i = 0; i < 8; ++i)
        src.sequence.notes.push_back({i * 960, 900, 0, static_cast<std::uint8_t>(key + i), 100, 0});
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
    return track;
}

// Two instruments feeding the master and, through a post-fader send with its
// own pan and a pre-fader send following its source's pan, a buss; both
// under a VCA. Console 9 on, so every level runs through the encoders.
struct Mix {
    model::Session session;
    Id a, b, bus, sendA, sendB, vca;
};
Mix mixSession() {
    Mix m;
    model::EditTxn t(model::Session::create({.console9 = true, .seed = 97531}), 1);
    m.a = synthTrack(t, kMaster, 60);
    m.b = synthTrack(t, kMaster, 67);
    model::Track bus;
    bus.type = model::StripType::Buss;
    bus.parent = kMaster;
    m.bus = t.create(bus);
    t.modify<model::Track>(kMaster, [&](model::Track& p) { p.children.push_back(m.bus); });
    model::Route route;
    route.from.track = m.a;
    route.to.track = m.bus;
    route.volumeDb = -6;
    route.pan = 30;
    m.sendA = t.create(route);
    route.from.track = m.b;
    route.tap = model::Tap::PreFader;
    route.volumeDb = -3;
    route.pan = {};
    m.sendB = t.create(route);
    model::TrackGroup group;
    group.members = {m.a, m.b};
    group.vca = model::Vca{-2, false, false, {}, {}};
    m.vca = t.create(group);
    m.session = t.commit().value();
    return m;
}

}  // namespace

TEST_CASE("a playrate edit together with a VCA edit compiles (not scalar-only)") {
    const Mix m = mixSession();
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    model::EditTxn rate(m.session, 2);
    auto project = rate.project();
    project.playrate.rate = 1.25;
    rate.setProject(project);
    const auto s = rate.commit().value();
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(8);
    model::EditTxn both(s, 3);
    project.playrate.rate = 1.5;
    both.setProject(project);
    both.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->volumeDb = -20; });
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(both.commit().value()));
    CHECK(engine.planGeneration() > generation);  // the VCA level reaches the engine
    engine.stop();
}
