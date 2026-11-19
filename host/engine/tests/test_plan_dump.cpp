// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Route E stage 5: the plan dump (plan_dump.h) and the engine's check of each
// compiled plan against a second compile (EngineOptions::verifyPlans).
#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <map>

#include <supple/engine/realtime.h>

#include "plan.h"
#include "plan_dump.h"
#include "rack_runtime.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {
namespace d = supple::engine::detail;

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kSine = "com.suppleaudio.test.sine";
constexpr const char* kGain = "com.suppleaudio.test.gain";
constexpr const char* kKeymix = "com.suppleaudio.test.keymix";

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

Id addChannel(model::EditTxn& t, model::StripType type = model::StripType::Channel) {
    model::Track tr;
    tr.parent = kMaster;
    tr.type = type;
    const Id id = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(id); });
    return id;
}

model::FxInstance fxOf(const char* plugin, bool latency = false) {
    model::FxInstance fx;
    fx.plugin.id = plugin;
    if (latency)
        fx.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                .value();
    return fx;
}

Id addFx(model::EditTxn& t, Id track, model::FxInstance fx) {
    const Id id = t.create(fx);
    t.modify<model::Track>(track, [&](auto& tr) { tr.chain.push_back(id); });
    return id;
}

// An item of four notes on `track`; `chain`: its take's own FX.
void addNotes(model::EditTxn& t, Id track, std::vector<Id> chain = {}) {
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 4 * 960;
    for (int i = 0; i < 4; ++i)
        src.sequence.notes.push_back(
            {i * 960, 480, 0, static_cast<std::uint8_t>(60 + 4 * i), 100, 0});
    model::Take take;
    take.source = t.create(src);
    take.chain = std::move(chain);
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
}

Id addModule(model::EditTxn& t, Id rack, std::string_view type) {
    model::RackModule m;
    m.rack = rack;
    m.type = std::string(type);
    m.version = "1.0.0";
    return t.create(m);
}

model::Session commit(model::EditTxn& t) {
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    return std::move(*s);
}

// Something of each kind a compile makes: PDC, an oversampled stage, a
// parallel one, one outside compensation, a key route, a take chain, a pre-
// fader send with pan, a VCA, envelopes on a strip and on a plug-in's
// parameter, MIDI sends, a monitored channel (a live region, frontier edges),
// and a rack with a plug-in module.
model::Session richSession() {
    model::EditTxn t(model::Session::create({.console9 = true, .seed = 4242}), 1);
    const Id a = addChannel(t), b = addChannel(t), c = addChannel(t), m = addChannel(t);
    const Id buss = addChannel(t, model::StripType::Buss);
    addFx(t, a, fxOf(kSine));
    const Id latent = addFx(t, a, fxOf(kGain, true));
    auto oversampled = fxOf(kGain);
    oversampled.oversampling = 2;
    addFx(t, a, oversampled);
    auto parallel = fxOf(kGain, true);
    parallel.parallel = true;
    parallel.pdc = model::PdcMode::Ignore;
    addFx(t, a, parallel);
    const Id keymix = addFx(t, a, fxOf(kKeymix));
    addNotes(t, a);
    addFx(t, b, fxOf(kSine));
    addNotes(t, b, {t.create(fxOf(kGain, true))});
    addFx(t, c, fxOf(kSine));
    model::Route key;
    key.from.track = b;
    key.to.fx = keymix;
    t.create(key);
    model::Route send;
    send.from.track = a;
    send.to.track = buss;
    send.tap = model::Tap::PreFader;
    send.volumeDb = -6;
    send.pan = 30;
    t.create(send);
    model::Route notes;
    notes.from.track = b;
    notes.to.track = c;
    notes.midi = model::MidiRouting{0, 0};
    t.create(notes);
    model::TrackGroup group;
    group.members = {a, b};
    group.vca = model::Vca{-2, false, false, {}, {}};
    t.create(group);
    model::Envelope volume;
    volume.target = {a, "volume"};
    volume.points = {{0.0, -6.0}, {2.0, 0.0, model::PointShape::Square}, {3.0, -3.0}};
    t.create(volume);
    model::Envelope gain;
    gain.target = {latent, "gain"};
    gain.points = {{0.0, -12.0}, {4.0, 0.0}};
    t.create(gain);
    addFx(t, m, fxOf(kGain));
    model::Route input;
    input.from.hwChannels = std::vector<int>{1, 2};
    input.to.track = m;
    const Id in = t.create(input);
    t.modify<model::Track>(m, [&](model::Track& tr) {
        tr.record.input = in;
        tr.record.armed = true;
        tr.record.monitor = model::Monitor::On;
    });
    const Id r = addChannel(t);
    const Id rack = t.create(model::Rack{r, ""});
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id fx = t.create(fxOf(kGain, true));
    model::RackModule plugin;
    plugin.rack = rack;
    plugin.type = std::string(model::kPluginModule);
    plugin.version = "1.0.0";
    plugin.fx = fx;
    const Id module = t.create(plugin);
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    t.create(model::Cable{{transport, "playing"}, {module, "in_l"}, std::nullopt, ""});
    t.create(model::Cable{{module, "out_l"}, {out, "l"}, std::nullopt, ""});
    model::Route listen;
    listen.from.module = out;
    listen.to.track = r;
    const Id route = t.create(listen);
    t.modify<model::Track>(r, [&](model::Track& tr) { tr.record.input = route; });
    return commit(t);
}

d::Plan prepared(const model::Session& s, InstancePool& pool, MediaCache& media) {
    auto compiled = d::compile(s, pool, 0, 128, 48000);
    REQUIRE_MESSAGE(compiled, (compiled ? std::string() : compiled.error().toString()));
    d::Plan p = std::move(*compiled);
    p.media = &media;
    auto ok = d::prepare(p, s);
    REQUIRE_MESSAGE(ok, (ok ? std::string() : ok.error().toString()));
    return p;
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}
}  // namespace

TEST_CASE("plan dump: a session compiled twice dumps the same plan (route E stage 5)") {
    const auto s = richSession();
    auto pool = makePool();
    MediaCache media;
    const auto first = d::dumpPlan(prepared(s, pool, media));
    const auto second = d::dumpPlan(prepared(s, pool, media));
    CHECK(d::firstDifference(first, second).empty());
    // It reached every kind the session has.
    for (const char* part :
         {" chain1 latency ", " chain2 os 2", " chain3 parallel true", " chain3 pdcIgnore true",
          " keyed true", " take0 chain0 fx ", " vca0 volumeDb ", " lane0 curve p1 value ",
          " autoVolume p2 shape ", " midi true", " live true", " frontier true", " rack outs ",
          " rackStage0 fx ", "plan liveNodes ", "plan rendererGraph preds "})
        CHECK_MESSAGE(has(first, part), part);
    // A fresh pool: new instances, the same text.
    auto other = makePool();
    CHECK(d::firstDifference(first, d::dumpPlan(prepared(s, other, media))).empty());
}

TEST_CASE("plan dump: a change to any compiled field shows in the dump (route E stage 5)") {
    // The harness's own mutation test: each change is one a partial compile
    // could make by skipping a field, and the dump must tell, on the line
    // that names the field.
    const auto s = richSession();
    auto pool = makePool();
    MediaCache media;
    const auto reference = d::dumpPlan(prepared(s, pool, media));
    auto stage = [](d::Plan& p) -> d::Stage& {
        for (d::Node& n : p.nodes)
            for (d::Stage& st : n.chain)
                if (st.latency > 0 && !st.lanes.empty()) return st;
        FAIL("no latent stage with a lane");
        return p.nodes[0].chain[0];
    };
    auto node = [](d::Plan& p, auto pick) -> d::Node& {
        for (d::Node& n : p.nodes)
            if (pick(n)) return n;
        FAIL("no such node");
        return p.nodes[0];
    };
    const std::vector<std::pair<const char*, std::function<void(d::Plan&)>>> mutations = {
        {" latency ", [&](d::Plan& p) { stage(p).latency += 1; }},
        {" value ", [&](d::Plan& p) { stage(p).lanes[0].curve.points[0].value += 1; }},
        {" inPin ", [&](d::Plan& p) { stage(p).inPin[0] = 1; }},
        {" dry ", [&](d::Plan& p) { stage(p).dry.length += 1; }},
        {" delay ", [&](d::Plan& p) { p.edges[0].delay.length += 1; }},
        {" tap ", [&](d::Plan& p) { p.edges[0].tap = model::Tap::PreFx; }},
        {" id ", [&](d::Plan& p) { std::swap(p.nodes[0], p.nodes[1]); }},
        {" sample ",
         [&](d::Plan& p) {
             node(p, [](const d::Node& n) { return !n.events.empty(); }).events[0].sample += 1;
         }},
        {" live ",
         [&](d::Plan& p) {
             auto& live = node(p, [](const d::Node& n) { return n.live; });
             live.live = false;
         }},
        {" outLatencies ",
         [&](d::Plan& p) {
             node(p, [](const d::Node& n) { return n.rack != nullptr; }).rack->outLatencies[0] += 1;
         }},
        {"plan rendererNodes ", [&](d::Plan& p) { p.rendererNodes.pop_back(); }},
        {"plan frontier ", [&](d::Plan& p) { p.frontier.push_back(0); }},
        {" succ", [&](d::Plan& p) { p.graph->succ[0].push_back(99); }},
        {"plan warnings ", [&](d::Plan& p) { p.warnings.push_back("w"); }},
        {"slot strip", [&](d::Plan& p) { p.mix->strips[0].volumeDb.store(-1); }},
        {"plan nodeIndex ", [&](d::Plan& p) { p.nodeIndex[0].node += 1; }},
    };
    for (const auto& [field, mutate] : mutations) {
        INFO(field);
        d::Plan p = prepared(s, pool, media);
        REQUIRE(d::firstDifference(reference, d::dumpPlan(p)).empty());
        mutate(p);
        const auto difference = d::firstDifference(reference, d::dumpPlan(p));
        INFO(difference);
        CHECK(has(difference, field));
    }
}

TEST_CASE("plan dump: the engine compiles each plan twice and finds them equal (verifyPlans)" *
          doctest::description("rt")) {
    const auto s = richSession();
    model::EditTxn t(s, s.version + 1);
    t.create(model::Marker{});
    const auto marked = commit(t);
    for (bool ahead : {false, true}) {
        INFO("anticipative " << ahead);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(
            pool, media,
            {.anticipative = ahead, .renderBlock = 128, .workers = 0, .verifyPlans = true});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(s));
        engine.play();
        backend.run(20);
        REQUIRE(engine.load(marked));
        backend.run(20);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(engine.verifiedPlans() == 2);
    }
}

namespace {
// prepare()'s note-source build as it was before the compile profile's step
// 2 (a std::map of per-address counts per stage), kept as the reference.
void referenceSources(d::Stage& st, const std::vector<d::TimedEvent>& events) {
    std::map<decltype(d::NoteSource{}.addressWithoutOrdinal()), std::uint32_t> counts;
    for (const auto& timed : events)
        if (timed.event.noteId >= 0 &&
            st.noteSources.size() <= static_cast<std::size_t>(timed.event.noteId))
            st.noteSources.resize(static_cast<std::size_t>(timed.event.noteId) + 1);
    for (const auto& timed : events) {
        const auto& event = timed.event;
        if (event.kind != host::Event::Kind::NoteOn || event.noteId < 0) continue;
        const auto id = static_cast<std::size_t>(event.noteId);
        if (st.noteSources.size() <= id) st.noteSources.resize(id + 1);
        d::NoteSource source;
        source.origin = timed.origin;
        source.start = timed.sample;
        source.port = event.port;
        source.channel = event.channel;
        source.key = event.key;
        source.ordinal = counts[source.addressWithoutOrdinal()]++;
        source.id = event.noteId;
        st.noteSources[id] = source;
    }
    for (const auto& timed : events)
        if (timed.event.kind == host::Event::Kind::NoteOff && timed.event.noteId >= 0 &&
            static_cast<std::size_t>(timed.event.noteId) < st.noteSources.size())
            st.noteSources[static_cast<std::size_t>(timed.event.noteId)].end = timed.sample;
    for (const auto& source : st.noteSources)
        if (source.id >= 0) st.noteSourceIndex.push_back(source);
    std::sort(st.noteSourceIndex.begin(), st.noteSourceIndex.end(),
              [](const auto& a, const auto& b) { return a.address() < b.address(); });
}

bool sameSources(const std::vector<d::NoteSource>& a, const std::vector<d::NoteSource>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
        return x.address() == y.address() && x.start == y.start && x.end == y.end && x.id == y.id;
    });
}
}  // namespace

TEST_CASE("plan: each stage's note sources are those of the reference build") {
    // The compile profile's step 2 replaced the per-stage std::map of note
    // counts (2.2 ms of a 10 ms compile at 192 tracks, with rack MIDI). The
    // ordinals (repeats of one origin, port, channel and key) and the index
    // must stay what the old build made: adoption rebinds voices by them.
    model::EditTxn t(richSession(), 2);
    const Id doubled = addChannel(t);
    addFx(t, doubled, fxOf(kSine));
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 2 * 960;
    for (int i = 0; i < 2; ++i)  // the same key at the same tick, twice
        src.sequence.notes.push_back({0, 480, 0, 64, 100, 0});
    src.sequence.notes.push_back({960, 480, 0, 67, 100, 0});
    model::Take take;
    take.source = t.create(src);
    take.chain = {t.create(fxOf(kSine))};
    const Id tk = t.create(take);
    model::Item item;
    item.track = doubled;
    item.length = 6.0;  // three passes of the source
    item.loop = true;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const auto s = commit(t);
    auto pool = makePool();
    MediaCache media;
    d::Plan p = prepared(s, pool, media);
    std::size_t stages = 0, repeats = 0;
    auto check = [&](const d::Stage& st, const std::vector<d::TimedEvent>& events) {
        d::Stage reference;
        referenceSources(reference, events);
        CHECK(sameSources(st.noteSources, reference.noteSources));
        CHECK(sameSources(st.noteSourceIndex, reference.noteSourceIndex));
        ++stages;
        for (const auto& source : st.noteSourceIndex) repeats += source.ordinal > 0;
    };
    for (const d::Node& n : p.nodes) {
        INFO("node " << n.id.str());
        check(n.midiStage, n.events);
        if (n.noteTarget >= 0) check(n.chain[static_cast<std::size_t>(n.noteTarget)], n.events);
        for (const d::TakeChain& tc : n.takeChains)
            if (tc.noteTarget >= 0)
                check(tc.chain[static_cast<std::size_t>(tc.noteTarget)], tc.events);
    }
    CHECK(stages > 0);
    CHECK(repeats > 0);
}

TEST_CASE("plan: a compile hands out retired plans' buffers cleared, the plan unchanged (step 3)") {
    // A retired plan's buffers, filled with garbage as they enter the pool,
    // serve the next compile of the same session: each it hands out holds
    // what a new one would (zeros, no voices, default events), every one
    // comes from the pool, and the plan dumps as one compiled without it.
    // (The executors also clear a stage's delays when they adopt a plan, so
    // a reuse that skipped this would not always be heard.)
    const auto s = richSession();
    auto pool = makePool();
    MediaCache media;
    const auto fresh = d::dumpPlan(prepared(s, pool, media));
    d::BufferPool buffers;
    buffers.poisonForTests = true;
    const auto pooled = [&] {
        auto compiled = d::compile(s, pool, 0, 128, 48000);
        REQUIRE(compiled);
        d::Plan p = std::move(*compiled);
        p.media = &media;
        p.buffers = &buffers;
        REQUIRE(d::prepare(p, s));
        return p;
    };
    struct Count final : d::BufferVisitor {
        std::size_t floats_ = 0, voices_ = 0, outputs_ = 0, dirty = 0;
        void floats(std::vector<float>& v) override {
            floats_ += v.empty() ? 0 : 1;
            dirty += std::any_of(v.begin(), v.end(), [](float x) { return x != 0.0f; });
        }
        void voices(std::vector<d::HeldVoice>& v) override {
            voices_ += v.capacity() ? 1 : 0;
            dirty += !v.empty();
        }
        void outputs(std::vector<host::OutputEvent>& v) override {
            outputs_ += v.empty() ? 0 : 1;
            dirty += std::any_of(v.begin(), v.end(), [](const host::OutputEvent& e) {
                return e.kind != host::OutputEvent{}.kind;
            });
        }
    };
    d::Plan first = pooled();
    const auto made = buffers.made();
    CHECK(made > 0);
    CHECK(buffers.reused() == 0);
    Count before;
    d::visitPooledBuffers(first, before);
    CHECK(before.floats_ > 0);
    CHECK(before.voices_ > 0);
    CHECK(before.outputs_ > 0);
    buffers.harvest(first);
    buffers.trim();
    d::Plan second = pooled();
    CHECK(buffers.made() == made);  // every buffer from the pool
    CHECK(buffers.reused() == made);
    Count after;
    d::visitPooledBuffers(second, after);
    CHECK(after.dirty == 0);
    CHECK(after.floats_ == before.floats_);
    CHECK(after.voices_ == before.voices_);
    CHECK(after.outputs_ == before.outputs_);
    CHECK(d::firstDifference(fresh, d::dumpPlan(second)).empty());
}
