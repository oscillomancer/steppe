// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <supple/base/files.h>
#include <supple/base/sha256.h>
#include <supple/engine/offline.h>

#include "plan.h"
#include "stretch.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace supple::engine {
struct MediaCacheTestAccess {
    static std::weak_ptr<const audio::AudioData> decoded(const MediaCache& cache) {
        REQUIRE(cache.decoded_.size() == 1);
        return cache.decoded_.begin()->second;
    }
    static std::size_t decodedEntries(const MediaCache& cache) { return cache.decoded_.size(); }
};
}  // namespace supple::engine

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kSine = "com.suppleaudio.test.sine";
constexpr const char* kGain = "com.suppleaudio.test.gain";

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

Id addTrack(model::EditTxn& t, std::string name, Id parent = kMaster,
            model::StripType type = model::StripType::Channel) {
    model::Track tr;
    tr.name = std::move(name);
    tr.type = type;
    tr.parent = parent;
    const Id id = t.create(tr);
    t.modify<model::Track>(parent, [&](model::Track& p) { p.children.push_back(id); });
    return id;
}

Id addFx(model::EditTxn& t, Id track, const char* plugin, std::optional<json::Value> patch = {}) {
    model::FxInstance fx;
    fx.plugin.id = plugin;
    fx.patch = std::move(patch);
    const Id id = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& tr) { tr.chain.push_back(id); });
    return id;
}

// A MIDI item on `track` with one note (beats).
void addNote(model::EditTxn& t, Id track, double startBeat, double lengthBeats, int key = 69,
             int velocity = 127) {
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = static_cast<std::int64_t>(lengthBeats * 960);
    src.sequence.notes = {{0, static_cast<std::int64_t>(lengthBeats * 960), 0,
                           static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity), 0}};
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.position = startBeat;
    item.length = lengthBeats;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
}

model::Session commit(model::EditTxn& t) {
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    return std::move(*s);
}

double peak(const std::vector<float>& v, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    double m = 0;
    for (std::size_t i = from; i < std::min(to, v.size()); ++i)
        m = std::max(m, std::fabs(double(v[i])));
    return m;
}

std::size_t firstNonZero(const std::vector<float>& v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f) return i;
    return v.size();
}

// The frequency of a tone, from its zero crossings in [from, to).
double frequency(const std::vector<float>& v, std::size_t from, std::size_t to) {
    int crossings = 0;
    for (std::size_t i = from + 1; i < std::min(to, v.size()); ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++crossings;
    return crossings * 48000.0 / (2.0 * static_cast<double>(to - from));
}

OfflineResult render(const model::Session& s, InstancePool& pool, double seconds, double tail = 0) {
    auto r =
        renderOffline(s, pool, {.startSeconds = 0.0, .endSeconds = seconds, .tailSeconds = tail});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return std::move(*r);
}

constexpr double kCentre = 0.70710678118654752;  // keys
constexpr double kStrip = 0.472135955;           // neutral Channel -> Buss, ADR-0019
constexpr double kHardStrip = kStrip * 1.4142135623730951;

}  // namespace

TEST_CASE("pan law gains") {
    auto [l, r] = panGains(0.0, -3.0);
    CHECK(l == doctest::Approx(kCentre).epsilon(1e-6));
    CHECK(r == doctest::Approx(kCentre).epsilon(1e-6));
    std::tie(l, r) = panGains(-1.0, -3.0);
    CHECK(l == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(std::fabs(r) < 1e-12);
    std::tie(l, r) = panGains(0.0, 0.0);
    CHECK(l == doctest::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("muted routes still order their source before their destination") {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
    const Id source = addTrack(t, "Source");  // later ID than the destination
    t.modify<model::Track>(source, [](auto& track) { track.parentSend.enabled = false; });
    addFx(t, source, kSine);
    addNote(t, source, 0.0, 1.0);
    model::Route route;
    route.from.track = source;
    route.to.track = bus;
    route.mute = true;
    const Id send = t.create(route);
    model::Envelope envelope;
    envelope.target = {send, "mute"};
    envelope.points = {{0.0, 1.0, model::PointShape::Square, 0.0},
                       {0.1, 0.0, model::PointShape::Square, 0.0}};
    t.create(envelope);
    auto pool = makePool();
    const auto result = render(commit(t), pool, 0.1);
    CHECK(peak(result.audio.channels[0], 0, 2400) == 0);
    CHECK(peak(result.audio.channels[0], 3000) > 0.01);
}

TEST_CASE("unchanged auxiliary latency survives a plan adoption") {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
    t.modify<model::Track>(bus, [](auto& track) { track.channels = 4; });
    const Id source = addTrack(t, "Source");
    t.modify<model::Track>(source, [](auto& track) { track.parentSend.enabled = false; });
    addFx(t, source, kSine);
    addNote(t, source, 0, 1);
    const auto latent =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value();
    addFx(t, bus, kGain, latent);  // delays the unwritten 3/4 along with 1/2
    const Id keyed = addFx(t, bus, "com.suppleaudio.test.keymix");
    t.modify<model::FxInstance>(keyed, [](auto& fx) { fx.pins.key = {3, 4}; });
    model::Route route;
    route.from.track = source;
    route.to.track = bus;
    route.tap = model::Tap::PreFader;
    route.channels = {{1, 2}, {3, 4}};
    t.create(route);
    const auto session = commit(t);
    auto pool = makePool();
    auto referencePool = makePool();
    auto old = detail::compile(session, pool, 0, 128).value();
    auto reference = detail::compile(session, referencePool, 0, 128).value();
    REQUIRE(detail::prepare(old, session));
    REQUIRE(detail::prepare(reference, session));
    for (auto* instance : old.instances) REQUIRE(instance->startProcessing());
    for (auto* instance : reference.instances) REQUIRE(instance->startProcessing());
    for (int pos = 0; pos < 512; pos += 128) {
        REQUIRE(detail::processBlock(old, pos, 128));
        REQUIRE(detail::processBlock(reference, pos, 128));
    }
    auto replacement = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(replacement, session));
    detail::carryMix(old, replacement, -1);
    detail::locate(replacement, 512, false);
    REQUIRE(detail::processBlock(replacement, 512, 128));
    REQUIRE(detail::processBlock(reference, 512, 128));
    const auto& actual = replacement.nodes[replacement.mainMaster].post;
    const auto& expected = reference.nodes[reference.mainMaster].post;
    CHECK(std::equal(actual.l.begin(), actual.l.end(), expected.l.begin()));
    CHECK(std::equal(actual.r.begin(), actual.r.end(), expected.r.begin()));
    for (auto* instance : old.instances) instance->stopProcessing();
    for (auto* instance : reference.instances) instance->stopProcessing();
}

TEST_CASE("shared bridge accepts a new effect after its last instance is removed") {
    host::setBridgeExecutable(BRIDGE_PATH);
    model::EditTxn initial(model::Session::create({.console9 = false}), 1);
    const Id track = addTrack(initial, "Synth");
    addFx(initial, track, kSine);
    addNote(initial, track, 0.0, 1.0);
    const model::Session base = commit(initial);
    auto pool = makePool();
    const auto dry = render(base, pool, 0.05);
    REQUIRE(peak(dry.audio.channels[0]) > 0.1);

    int sharedPid = -1;
    for (int cycle = 0; cycle < 24; ++cycle) {
        model::EditTxn edit(base, static_cast<std::uint64_t>(cycle + 2));
        const Id fx = addFx(
            edit, track, kGain,
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":"-60 dB"}})")
                .value());
        edit.modify<model::FxInstance>(
            fx, [](model::FxInstance& value) { value.isolation = model::Isolation::SharedBridge; });
        const model::Session withFx = commit(edit);
        const auto wet = render(withFx, pool, 0.05);
        REQUIRE_MESSAGE(wet.warnings.empty(), (wet.warnings.empty() ? "" : wet.warnings.front()));
        auto* instance = pool.find(fx);
        REQUIRE(instance);
        CHECK(instance->bridgePid() > 0);
        if (sharedPid > 0) CHECK(instance->bridgePid() == sharedPid);
        sharedPid = instance->bridgePid();
        CHECK(peak(wet.audio.channels[0]) == 0.0);  // missing fx would pass dry audio through
        pool.prune(base);  // destroys the last shared instance after this plan is retired
        CHECK(pool.find(fx) == nullptr);
        if (cycle % 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

TEST_CASE("an instrument track plays its MIDI item at the right samples" *
          doctest::description("rt")) {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 1.0, 1.0);  // beat 1..2 at 120 BPM = 0.5 s .. 1.0 s
    auto s = commit(t);
    auto r = render(s, pool, 2.0);
    REQUIRE(r.audio.frames() == 96000);
    CHECK(r.outputLatency == 0);
    const auto& L = r.audio.channels[0];
    CHECK(firstNonZero(L) == 24001);  // phase 0 at the note-on sample
    CHECK(peak(L, 24000, 48000) == doctest::Approx(0.25 * kStrip).epsilon(1e-4));
    CHECK(peak(L, 48000) == 0.0);  // the fixture stops at note off
    // Deterministic.
    auto again = render(s, pool, 2.0);
    CHECK(again.audio.channels == r.audio.channels);
}

TEST_CASE("mixer: volume, folder, hard pan, mute, pre-fader route") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id folder = addTrack(t, "Folder", kMaster, model::StripType::Buss);
    const Id a = addTrack(t, "A", folder);
    addFx(t, a, kSine);
    addNote(t, a, 0.0, 1.0);
    t.modify<model::Track>(a, [](model::Track& x) { x.volumeDb = -6.0; });
    t.modify<model::Track>(folder, [](model::Track& x) { x.pan = -100.0; });
    auto s = commit(t);
    auto r = render(s, pool, 0.5);
    // Two encode/decode pairs; the folder hard left is sqrt(2) above centre.
    const double expect = 0.25 * std::pow(10.0, -6.0 / 20.0) * kStrip * kHardStrip;
    CHECK(peak(r.audio.channels[0]) == doctest::Approx(expect).epsilon(1e-4));
    CHECK(peak(r.audio.channels[1]) < 1e-9);

    // Mute A but send it pre-fader to a bus: the bus still hears it.
    model::EditTxn t2(s, 2);
    const Id bus = addTrack(t2, "Bus", kMaster, model::StripType::Buss);
    t2.modify<model::Track>(a, [](model::Track& x) { x.mute = true; });
    model::Route send;
    send.from.track = a;
    send.to.track = bus;
    send.tap = model::Tap::PreFader;
    t2.create(send);
    auto s2 = commit(t2);
    auto r2 = render(s2, pool, 0.5);
    // Pre-fader: no -6 dB; send -> buss and buss -> master are neutral pairs.
    CHECK(peak(r2.audio.channels[0]) == doctest::Approx(0.25 * kStrip * kStrip).epsilon(1e-4));
}

TEST_CASE("a route's channel map swaps or duplicates channels into the destination") {
    auto pool = makePool();
    auto build = [](model::ChannelMap map) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
        const Id a = addTrack(t, "A");
        addFx(t, a, kSine);
        addNote(t, a, 0.0, 1.0);
        t.modify<model::Track>(a, [](model::Track& x) {
            x.pan = -100.0;  // all left
            x.parentSend.enabled = false;
        });
        model::Route send;
        send.from.track = a;
        send.to.track = bus;
        send.channels = std::move(map);
        t.create(send);
        return commit(t);
    };
    auto plain = render(build({}), pool, 0.5);
    REQUIRE(plain.warnings.empty());
    const double left = peak(plain.audio.channels[0]);
    CHECK(left > 0.05);
    CHECK(peak(plain.audio.channels[1]) < 1e-9);
    // Swapped: the left channel arrives on the right.
    auto swapped = render(build({{1, 2}, {2, 1}}), pool, 0.5);
    CHECK(peak(swapped.audio.channels[0]) < 1e-9);
    CHECK(peak(swapped.audio.channels[1]) == doctest::Approx(left).epsilon(1e-4));
    // The left to both sides.
    auto both = render(build({{1, 1}, {1, 2}}), pool, 0.5);
    CHECK(peak(both.audio.channels[0]) == doctest::Approx(left).epsilon(1e-4));
    CHECK(peak(both.audio.channels[1]) == doctest::Approx(left).epsilon(1e-4));
    // A channel the strips lack is warned about and skipped.
    auto beyond = render(build({{3}, {1}}), pool, 0.5);
    REQUIRE(beyond.warnings.size() == 1);
    CHECK(beyond.warnings[0].find("channel 3") != std::string::npos);
}

TEST_CASE("latency compensation keeps parallel paths aligned") {
    auto pool = makePool();
    auto build = [](bool withLatent) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id b = addTrack(t, "Plain");
        addFx(t, b, kSine);
        addNote(t, b, 0.0, 1.0);
        if (withLatent) {
            const Id a = addTrack(t, "Latent");
            addFx(t, a, kSine);
            addFx(t, a, kGain,
                  json::parse(
                      R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                      .value());
            addNote(t, a, 0.0, 1.0);
        }
        auto s = t.commit();
        REQUIRE(s);
        return std::move(*s);
    };
    auto both = render(build(true), pool, 0.5);
    auto single = render(build(false), pool, 0.5);
    CHECK(both.outputLatency == 64);
    CHECK(single.outputLatency == 0);
    REQUIRE(both.audio.frames() == single.audio.frames());
    // Aligned to the sample: exactly twice the single track (no comb filtering).
    double worst = 0;
    for (std::size_t i = 0; i < single.audio.frames(); ++i)
        worst = std::max(worst, std::fabs(double(both.audio.channels[0][i]) -
                                          2.0 * double(single.audio.channels[0][i])));
    CHECK(worst < 1e-6);
    CHECK(firstNonZero(both.audio.channels[0]) == firstNonZero(single.audio.channels[0]));
}

TEST_CASE("the tempo map places notes") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    t.create(model::TempoPointObj{4.0, 60.0, false});  // 120 BPM for 4 beats (2 s), then 60
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 5.0, 0.5);  // 2 s + 1 beat at 60 = 3 s
    auto s = commit(t);
    auto r = render(s, pool, 4.0);
    CHECK(firstNonZero(r.audio.channels[0]) == 3 * 48000 + 1);
}

TEST_CASE("a missing plugin passes audio through with a warning") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addFx(t, tr, "org.example.not-installed");
    addNote(t, tr, 0.0, 1.0);
    auto s = commit(t);
    auto r = render(s, pool, 0.5);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("org.example.not-installed") != std::string::npos);
    CHECK(peak(r.audio.channels[0]) == doctest::Approx(0.25 * kStrip).epsilon(1e-4));
    CHECK(renderOffline(s, pool, {.startSeconds = 1.0, .endSeconds = 1.0}).error().code ==
          "engine.range");
}

namespace {
Id addEnvelope(model::EditTxn& t, Id object, std::string param,
               std::vector<model::EnvPoint> points) {
    model::Envelope env;
    env.target = {object, std::move(param)};
    env.points = std::move(points);
    return t.create(env);
}
double windowPeak(const std::vector<float>& v, std::size_t centre) {
    return peak(v, centre - 120, centre + 120);
}
}  // namespace

TEST_CASE(
    "mixer automation: volume in dB, square mute starts its 10 ms ramp on the exact sample, route "
    "mute") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 0.0, 8.0);
    addEnvelope(t, tr, "volume", {{0.0, 0.0}, {2.0, -12.0}});
    addEnvelope(t, tr, "mute", {{0.0, 0.0, model::PointShape::Square}, {3.0, 1.0}});
    auto s = commit(t);
    auto r = render(s, pool, 2.0);
    REQUIRE(r.warnings.empty());
    const auto& L = r.audio.channels[0];
    const double base = 0.25 * kStrip;
    CHECK(windowPeak(L, 24000) / base ==
          doctest::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.01));
    CHECK(windowPeak(L, 60000) / base ==
          doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.01));
    CHECK(peak(L, 72480) == 0.0);  // fully muted 10 ms after beat 3 = 1.5 s
    CHECK(peak(L, 71800, 72000) > 0.0);

    // Through a latent chain the mute still lands on beat 3 of the output.
    model::EditTxn t2(s, 2);
    addFx(t2, tr, kGain,
          json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
              .value());
    auto r2 = render(commit(t2), pool, 2.0);
    CHECK(r2.outputLatency == 64);
    CHECK(peak(r2.audio.channels[0], 72480) == 0.0);
    CHECK(peak(r2.audio.channels[0], 71800, 72000) > 0.0);
}

TEST_CASE("route automation") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id src = addTrack(t, "Src");
    addFx(t, src, kSine);
    addNote(t, src, 0.0, 4.0);
    t.modify<model::Track>(src, [](model::Track& x) { x.mute = true; });
    const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
    model::Route send;
    send.from.track = src;
    send.to.track = bus;
    send.tap = model::Tap::PreFader;
    const Id sd = t.create(send);
    addEnvelope(t, sd, "mute", {{0.0, 0.0, model::PointShape::Square}, {1.0, 1.0}});
    auto r = render(commit(t), pool, 1.0);
    CHECK(peak(r.audio.channels[0], 0, 24000) > 0.05);
    CHECK(peak(r.audio.channels[0], 24480) == 0.0);
}

TEST_CASE("solo: soloed tracks and their signal path stay audible") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id folder = addTrack(t, "Folder", kMaster, model::StripType::Buss);
    const Id a = addTrack(t, "A", folder);
    const Id b = addTrack(t, "B");
    addFx(t, a, kSine);
    addFx(t, b, kSine);
    addNote(t, a, 0.0, 1.0, 69);
    addNote(t, b, 0.0, 1.0, 81);
    t.modify<model::Track>(a, [](model::Track& x) { x.solo = model::Solo::On; });
    auto s = commit(t);
    auto r = render(s, pool, 0.5);
    // Only A (through its folder) is heard: two neutral Console 9 pairs.
    CHECK(peak(r.audio.channels[0]) == doctest::Approx(0.25 * kStrip * kStrip).epsilon(1e-4));
    model::EditTxn t2(s, 2);
    t2.modify<model::Track>(b, [](model::Track& x) { x.solo = model::Solo::Defeat; });
    auto r2 = render(commit(t2), pool, 0.5);
    CHECK(peak(r2.audio.channels[0]) > 0.25 * kStrip * kStrip * 1.2);  // B is back
}

TEST_CASE("a master's own items play after its plain stage (ADR-0057)") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-print-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData ramp;
    ramp.sampleRate = 48000;
    ramp.channels.assign(2, std::vector<float>(48000));
    for (int i = 0; i < 48000; ++i) {
        ramp.channels[0][static_cast<std::size_t>(i)] = static_cast<float>(i) / 48000.0f;
        ramp.channels[1][static_cast<std::size_t>(i)] = -static_cast<float>(i) / 48000.0f;
    }
    REQUIRE(audio::writeWav(dir / "media" / "ramp.wav", ramp));
    auto item = [](model::EditTxn& t, Id track, Id md, double beat) {
        model::Take take;
        take.source = md;
        const Id tk = t.create(take);
        model::Item it;
        it.track = track;
        it.position = beat;
        it.length = 1.0;  // 0.5 s at 120 BPM
        it.takes = {tk};
        it.activeTake = tk;
        t.create(it);
    };
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id md = t.create(model::MediaSource{"ramp.wav", "", "wav", 48000, 2, 48000});
    const Id ch = addTrack(t, "Live");
    item(t, ch, md, 2.0);       // the live mix, at 1.0 s
    item(t, kMaster, md, 0.0);  // a print on the master, at 0 s
    // A fader, a balance and a latent insert on the master leave its items alone.
    t.modify<model::Track>(kMaster, [](model::Track& tr) {
        tr.volumeDb = -12;
        tr.pan = 40;
    });
    addFx(t, kMaster, kGain,
          json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
              .value());
    const auto s = commit(t);
    auto pool = makePool();
    MediaCache media;
    auto renderOf = [&](const model::Session& session, std::optional<Id> through = {}) {
        auto r = renderOffline(session, pool,
                               {.startSeconds = 0,
                                .endSeconds = 1.5,
                                .projectDir = dir,
                                .media = &media,
                                .throughMaster = through});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::move(*r);
    };
    const auto mix = renderOf(s);
    CHECK(mix.warnings.empty());
    for (std::size_t i = 0; i < 24000; ++i) {  // exactly the print, on time
        REQUIRE(mix.audio.channels[0][i] == ramp.channels[0][i]);
        REQUIRE(mix.audio.channels[1][i] == ramp.channels[1][i]);
    }
    CHECK(peak(mix.audio.channels[0], 48000, 72000) > 0);
    CHECK(peak(mix.audio.channels[0], 48000, 72000) < 0.25);  // the live mix is faded
    // A stem leaves the master's items out; the master's mute and lane mute silence them.
    const auto stem = renderOf(s, ch);
    CHECK(peak(stem.audio.channels[0], 0, 24000) == 0.0);
    CHECK(peak(stem.audio.channels[0], 48000, 72000) > 0);
    for (auto change : {+[](model::Track& tr) { tr.mute = true; },
                        +[](model::Track& tr) { tr.laneMute = true; }}) {
        model::EditTxn edit(s, 2);
        edit.modify<model::Track>(kMaster, change);
        const auto silent = renderOf(commit(edit));
        CHECK(peak(silent.audio.channels[0], 480, 24000) == 0.0);
    }
    model::EditTxn lane(s, 3);
    lane.modify<model::Track>(kMaster, [](model::Track& tr) { tr.laneMute = true; });
    CHECK(peak(renderOf(commit(lane)).audio.channels[0], 48000, 72000) > 0);  // live mix plays
    std::filesystem::remove_all(dir);
}

TEST_CASE("audio items: position, take offset, fade, channel mode, offline media") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-media-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData ramp;
    ramp.sampleRate = 48000;
    ramp.channels.assign(2, std::vector<float>(48000));
    for (int i = 0; i < 48000; ++i) {
        ramp.channels[0][static_cast<std::size_t>(i)] = static_cast<float>(i) / 48000.0f;
        ramp.channels[1][static_cast<std::size_t>(i)] = -static_cast<float>(i) / 48000.0f;
    }
    REQUIRE(audio::writeWav(dir / "media" / "ramp.wav", ramp));
    ramp.sampleRate = 44100;
    REQUIRE(audio::writeWav(dir / "media" / "slow.wav", ramp));

    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "Audio");
    const Id md = t.create(model::MediaSource{"ramp.wav", "", "wav", 48000, 2, 48000});
    model::Take take;
    take.source = md;
    take.startOffset = 0.1;  // 4800 frames in
    const Id tk = t.create(take);
    model::Item item;
    item.track = tr;
    item.position = 1.0;  // beat 1 = 0.5 s
    item.length = 0.5;    // beats = 0.25 s
    item.takes = {tk};
    item.activeTake = tk;
    item.fadeIn = {0.01, model::FadeShape::Linear, 0};  // 480 samples
    const Id it = t.create(item);
    auto s = commit(t);

    auto pool = makePool();
    MediaCache media;
    auto r = renderOffline(
        s, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(r->warnings.empty());
    const auto& L = r->audio.channels[0];
    const auto& R = r->audio.channels[1];
    CHECK(peak(L, 0, 24000) == 0.0);
    CHECK(peak(L, 36000) == 0.0);  // the item ends at 0.75 s
    // After the fade: source frame 4800 + k, centred (0.707).
    for (int k : {480, 1000, 11999}) {
        const auto i = static_cast<std::size_t>(24000 + k);
        const double src = (4800.0 + k) / 48000.0;
        CHECK(double(L[i]) == doctest::Approx(src * kStrip).epsilon(1e-5));
        CHECK(double(R[i]) == doctest::Approx(-src * kStrip).epsilon(1e-5));
    }
    CHECK(double(L[24000 + 240]) ==
          doctest::Approx(0.5 * (4800.0 + 240) / 48000.0 * kStrip).epsilon(1e-5));
    CHECK(media.size() == 1);

    model::EditTxn bent(s, 2);
    bent.modify<model::Item>(it, [](model::Item& x) { x.fadeIn.curve = 50; });
    auto curved =
        renderOffline(commit(bent), pool,
                      {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE(curved);
    CHECK(double(curved->audio.channels[0][24240]) ==
          doctest::Approx(0.25 * 5040.0 / 48000.0 * kStrip).epsilon(1e-5));

    model::EditTxn t2(s, 2);
    t2.modify<model::Take>(tk, [](model::Take& x) { x.channelMode = model::ChannelMode::Left; });
    auto r2 =
        renderOffline(commit(t2), pool,
                      {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE(r2);
    CHECK(r2->audio.channels[1][30000] == r2->audio.channels[0][30000]);

    model::EditTxn t3(s, 2);
    t3.modify<model::MediaSource>(md, [](model::MediaSource& m) { m.path = "slow.wav"; });
    (void)it;
    // A file at another rate is resampled: the same ramp, 44.1 kHz frames
    // spaced at 48 kHz, the take offset still 0.1 s (4410 source frames).
    const model::Session slow = commit(t3);
    auto r3 = renderOffline(
        slow, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE(r3);
    CHECK(r3->warnings.empty());
    CHECK(peak(r3->audio.channels[0], 36000) == 0.0);
    for (int k : {480, 1000, 11999}) {
        const auto i = static_cast<std::size_t>(24000 + k);
        const double src = (4410.0 + k * 44100.0 / 48000.0) / 48000.0;
        CHECK(double(r3->audio.channels[0][i]) == doctest::Approx(src * kStrip).epsilon(1e-3));
        CHECK(double(r3->audio.channels[1][i]) == doctest::Approx(-src * kStrip).epsilon(1e-3));
    }
    CHECK(media.size() == 2);
    auto again =
        renderOffline(slow, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir});
    REQUIRE(again);  // a fresh cache resamples to the same samples
    CHECK(again->audio.channels == r3->audio.channels);
    std::filesystem::remove_all(dir);
}

TEST_CASE("take playrate: varispeed resamples, the stretch keeps the pitch") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-media-stretch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i) {
        const double s = 0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0);
        tone.channels[0][i] = static_cast<float>(s);
        tone.channels[1][i] = static_cast<float>(s);
    }
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone));
    audio::AudioData longTone = tone;  // 4 s, for the variant size guard
    longTone.channels.assign(2, std::vector<float>(4 * 48000));
    REQUIRE(audio::writeWav(dir / "media" / "long.wav", longTone));

    auto pool = makePool();
    MediaCache media;
    // An item of `beats` (0.5 s each at 120 BPM) starting at 0.5 s.
    auto build = [&](const char* file, double playrate, bool preservePitch, double pitch,
                     const char* mode, double beats, std::uint64_t rev = 1) {
        model::EditTxn t(model::Session::create({.console9 = false}), rev);
        const Id tr = addTrack(t, "Audio");
        const Id md = t.create(model::MediaSource{file, "", "wav", 48000, 2, 48000});
        model::Take take;
        take.source = md;
        take.playrate = playrate;
        take.preservePitch = preservePitch;
        take.pitch = pitch;
        take.pitchMode = mode;
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.position = 1.0;
        item.length = beats;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return commit(t);
    };
    auto renderTake = [&](const model::Session& s, double seconds, MediaCache* cache = nullptr) {
        auto r = renderOffline(s, pool,
                               {.startSeconds = 0.0,
                                .endSeconds = seconds,
                                .projectDir = dir,
                                .media = cache ? cache : &media});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::move(*r);
    };
    constexpr double kTone = 0.5 * kStrip;

    // Varispeed: 2x halves the take's source time — its 1 s source fills a
    // 0.5 s item — and doubles the pitch. The old "time stretching is not
    // available yet" warning is gone.
    {
        auto r = renderTake(build("tone.wav", 2.0, false, 0.0, "resample", 1.0), 1.5);
        CHECK(r.warnings.empty());
        const auto& L = r.audio.channels[0];
        CHECK(peak(L, 0, 24000) == 0.0);
        CHECK(peak(L, 48000) == 0.0);
        CHECK(peak(L, 24000, 48000) == doctest::Approx(kTone).epsilon(0.02));
        CHECK(frequency(L, 27000, 45000) == doctest::Approx(880.0).epsilon(0.03));
        CHECK(media.size() == 1);
    }
    // Stretch: half the rate keeps the pitch and doubles the take's duration,
    // so 0.5 s of source fills the 1 s item.
    {
        auto r = renderTake(build("tone.wav", 0.5, true, 0.0, "signalsmith", 2.0), 2.0);
        CHECK(r.warnings.empty());
        const auto& L = r.audio.channels[0];
        CHECK(peak(L, 0, 24000) == 0.0);
        CHECK(peak(L, 72480) == 0.0);
        CHECK(peak(L, 24000, 72000) > 0.9 * kTone);
        CHECK(frequency(L, 34000, 69000) == doctest::Approx(440.0).epsilon(0.03));
        CHECK(media.size() == 2);
    }
    // The same stretch an octave up is another variant, not the cached one.
    {
        auto r = renderTake(build("tone.wav", 0.5, true, 12.0, "signalsmith", 2.0), 2.0);
        CHECK(r.warnings.empty());
        CHECK(frequency(r.audio.channels[0], 34000, 69000) == doctest::Approx(880.0).epsilon(0.03));
        CHECK(media.size() == 3);
    }
    // A pitch offset the resample cannot honour: warned, and the pitch stays.
    {
        auto r = renderTake(build("tone.wav", 1.0, true, 12.0, "resample", 1.0), 1.5);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("pitch_mode") != std::string::npos);
        CHECK(frequency(r.audio.channels[0], 27000, 45000) == doctest::Approx(440.0).epsilon(0.03));
    }
    // Without preserve_pitch the same offset is a further speed factor.
    {
        auto r = renderTake(build("tone.wav", 1.0, false, 12.0, "resample", 1.0), 1.5);
        CHECK(r.warnings.empty());
        CHECK(frequency(r.audio.channels[0], 27000, 45000) == doctest::Approx(880.0).epsilon(0.03));
    }
    // The stretched samples are the same in a cache that has to build them.
    {
        MediaCache fresh;
        auto a = renderTake(build("tone.wav", 0.5, true, 0.0, "signalsmith", 2.0), 2.0);
        auto b = renderTake(build("tone.wav", 0.5, true, 0.0, "signalsmith", 2.0), 2.0, &fresh);
        CHECK(a.audio.channels == b.audio.channels);
    }
    // A variant that would need an unreasonable amount of memory is refused,
    // not allocated (4 s at 1/100 speed and two octaves down).
    {
        auto r = renderTake(build("long.wav", 0.01, false, -48.0, "resample", 1.0), 1.5);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("memory") != std::string::npos);
        CHECK(peak(r.audio.channels[0]) == 0.0);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("offline renders are bit-identical for any thread count (B4)") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id folder = addTrack(t, "Folder", kMaster, model::StripType::Buss);
    const Id busA = addTrack(t, "Bus A", kMaster, model::StripType::Buss);
    const Id busB = addTrack(t, "Bus B", kMaster, model::StripType::Buss);
    addFx(t, busB, kGain,
          json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
              .value());
    for (int i = 0; i < 10; ++i) {
        const Id tr = addTrack(t, "T" + std::to_string(i), i % 3 == 0 ? folder : kMaster);
        addFx(t, tr, kSine);
        if (i % 4 == 1)
            addFx(t, tr, kGain,
                  json::parse(
                      R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                      .value());
        addNote(t, tr, 0.25 * i, 2.0, 48 + 3 * i, 60 + 5 * i);
        t.modify<model::Track>(tr, [&](model::Track& x) {
            x.volumeDb = -1.0 * i;
            x.pan = -90.0 + 20.0 * i;
        });
        model::Route send;
        send.from.track = tr;
        send.to.track = i % 2 ? busA : busB;
        send.tap = i % 3 ? model::Tap::PostFader : model::Tap::PreFader;
        send.volumeDb = -3.0 - i;
        t.create(send);
    }
    const auto s = commit(t);
    std::vector<OfflineResult> runs;
    for (unsigned threads : {1u, 2u, 8u}) {
        auto r =
            renderOffline(s, pool, {.startSeconds = 0.0, .endSeconds = 2.0, .threads = threads});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        runs.push_back(std::move(*r));
    }
    CHECK(peak(runs[0].audio.channels[0]) > 0.01);
    for (const OfflineResult& r : runs) {
        CHECK(r.audio.channels[0] == runs[0].audio.channels[0]);
        CHECK(r.audio.channels[1] == runs[0].audio.channels[1]);
    }
}

TEST_CASE("the pool retires replaced instances until no plan can use them (review round 6)") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "T");
    const Id fx = addFx(t, tr, kGain);
    const auto s1 = commit(t);
    pool.setGeneration(1);
    host::Instance* a = pool.get(fx, *s1.fx.find(fx), s1).value();

    // Another plugin under the same fx id: a new instance; the old one waits.
    model::EditTxn t2(s1, 2);
    t2.modify<model::FxInstance>(
        fx, [](model::FxInstance& f) { f.plugin.id = "com.suppleaudio.test.edge"; });
    const auto s2 = commit(t2);
    pool.setGeneration(2);
    host::Instance* b = pool.get(fx, *s2.fx.find(fx), s2).value();
    CHECK(b != a);
    CHECK(pool.retired() == 1);
    pool.prune(s2, 1);  // plan 1 (which used `a`) may still play
    CHECK(pool.retired() == 1);
    pool.prune(s2, 2);
    CHECK(pool.retired() == 0);

    // A plugin that cannot be found: the live instance is retired, not freed.
    model::EditTxn t3(s2, 3);
    t3.modify<model::FxInstance>(
        fx, [](model::FxInstance& f) { f.plugin.id = "com.suppleaudio.test.nope"; });
    const auto s3 = commit(t3);
    pool.setGeneration(3);
    CHECK_FALSE(pool.get(fx, *s3.fx.find(fx), s3));
    CHECK(pool.retired() == 1);
    CHECK(pool.find(fx) == nullptr);
    pool.prune(s3, 2);
    CHECK(pool.retired() == 1);
    pool.prune(s3, 3);
    CHECK(pool.retired() == 0);
}

TEST_CASE("a new state blob for a processing instance goes to a fresh instance (review round 6)") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "T");
    const Id fx = addFx(t, tr, "com.suppleaudio.test.edge");
    t.modify<model::FxInstance>(fx, [](model::FxInstance& f) { f.state = "sha256:one"; });
    t.putBlob("sha256:one", std::make_shared<const model::Blob>(model::Blob{{0, 1, 0, 0}}));
    const auto s1 = commit(t);
    host::Instance* a = pool.get(fx, *s1.fx.find(fx), s1).value();
    REQUIRE(a->activate(48000, 1, 256));
    REQUIRE(a->startProcessing());  // as if a plan were playing it

    model::EditTxn t2(s1, 2);
    t2.modify<model::FxInstance>(fx, [](model::FxInstance& f) { f.state = "sha256:two"; });
    t2.putBlob("sha256:two", std::make_shared<const model::Blob>(model::Blob{{0, 2, 0, 0}}));
    const auto s2 = commit(t2);
    host::Instance* b = pool.get(fx, *s2.fx.find(fx), s2).value();
    CHECK(b != a);  // `a` keeps processing untouched until a plan stops it
    CHECK(pool.retired() == 1);
    CHECK(b->tail() == 2);  // the edge fixture reports its state bytes as its tail
    a->stopProcessing();

    // Not processing: the state is loaded in place.
    model::EditTxn t3(s2, 3);
    t3.modify<model::FxInstance>(fx, [](model::FxInstance& f) { f.state = "sha256:three"; });
    t3.putBlob("sha256:three", std::make_shared<const model::Blob>(model::Blob{{0, 3, 0, 0}}));
    const auto s3 = commit(t3);
    CHECK(pool.get(fx, *s3.fx.find(fx), s3).value() == b);
    CHECK(b->tail() == 3);
}

TEST_CASE("rendering an initial FX version after an edit restores identical audio") {
    auto pool = makePool();
    model::EditTxn add(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(add, "Synth");
    addFx(add, tr, kSine);
    const Id fx = addFx(add, tr, kGain);
    addNote(add, tr, 0, 1);
    const auto initial = commit(add);
    const auto expected = render(initial, pool, 0.1).audio.channels;
    model::EditTxn edit(initial, 2);
    edit.modify<model::FxInstance>(fx, [](auto& f) {
        f.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12}})").value();
    });
    const auto changed = commit(edit);
    CHECK(render(changed, pool, 0.1).audio.channels != expected);
    CHECK(render(initial, pool, 0.1).audio.channels == expected);
}

TEST_CASE("plugin parameter automation: sample-accurate steps, ramps in the curve domain") {
    auto pool = makePool();
    auto build = [](std::string param, std::vector<model::EnvPoint> points) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id tr = addTrack(t, "Lead");
        addFx(t, tr, kSine);
        const Id g = addFx(t, tr, kGain);
        addNote(t, tr, 0.0, 4.0);
        model::Envelope env;
        env.target = {g, std::move(param)};
        env.points = std::move(points);
        t.create(env);
        return commit(t);
    };
    using model::PointShape;
    // A step to -60 dB (off at min) at beat 1 = sample 24000: silence from that sample.
    {
        auto r = render(build("gain", {{0.0, 0.0, PointShape::Square, 0.0},
                                       {1.0, -60.0, PointShape::Square, 0.0}}),
                        pool, 1.0);
        CHECK(r.warnings.empty());
        CHECK(peak(r.audio.channels[0], 23800, 24000) > 0.05);
        CHECK(peak(r.audio.channels[0], 24000, 48000) == 0.0);
    }
    // The same by CLAP id, in CLAP values (the normalized position for stock plugins).
    {
        auto r = render(build("0", {{0.0, 60.0 / 72.0, PointShape::Square, 0.0},
                                    {1.0, 0.0, PointShape::Square, 0.0}}),
                        pool, 1.0);
        CHECK(r.warnings.empty());
        CHECK(peak(r.audio.channels[0], 23800, 24000) > 0.05);
        CHECK(peak(r.audio.channels[0], 24000, 48000) == 0.0);
    }
    // A linear ramp 0 → -24 dB over one second: -12 dB halfway.
    {
        auto r = render(build("gain", {{0.0, 0.0, PointShape::Linear, 0.0},
                                       {2.0, -24.0, PointShape::Linear, 0.0}}),
                        pool, 1.0);
        const double start = peak(r.audio.channels[0], 200, 1000);
        const double mid = peak(r.audio.channels[0], 23800, 24200);
        CHECK(mid / start == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.03));
    }
    // An unknown parameter is reported, not fatal.
    {
        auto r = render(build("gian", {{0.0, 0.0, PointShape::Linear, 0.0}}), pool, 0.1);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("gian") != std::string::npos);
    }
    {
        const auto session = build(
            "gain", {{0.0, 0.0, PointShape::Linear, 0.0}, {2.0, -24.0, PointShape::Linear, 0.0}});
        model::EditTxn shifted(session, 2);
        for (const auto& item : session.items)
            shifted.modify<model::Item>(item.id, [](auto& it) { it.position = .02; });
        const auto audible = commit(shifted);
        for (double start : {0.0, 0.001}) {
            auto aligned = renderOffline(
                audible, pool, {.startSeconds = start, .endSeconds = 0.1, .blockSize = 512});
            auto odd = renderOffline(audible, pool,
                                     {.startSeconds = start, .endSeconds = 0.1, .blockSize = 37});
            REQUIRE(aligned);
            REQUIRE(odd);
            CHECK(peak(odd->audio.channels[0]) > .01);
            CHECK(std::equal(odd->audio.channels[0].begin(), odd->audio.channels[0].end(),
                             aligned->audio.channels[0].begin()));
        }
    }
}

TEST_CASE("plugins see the transport loop (CLAP loop flags)") {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    addTrack(t, "Track");
    const model::Session s = commit(t);
    auto pool = makePool();
    auto plan = detail::compile(s, pool, 0.0, 256).value();
    const host::Transport none = detail::transportAt(plan, 1000);
    CHECK_FALSE(none.loopActive);
    const LoopRange loop{48000, 96000, true};  // 1 s .. 2 s = beats 2 .. 4 at 120 BPM
    const host::Transport t2 = detail::transportAt(plan, 60000, &loop);
    CHECK(t2.loopActive);
    CHECK(t2.loopStartSeconds == doctest::Approx(1.0));
    CHECK(t2.loopEndSeconds == doctest::Approx(2.0));
    CHECK(t2.loopStartBeat == doctest::Approx(2.0));
    CHECK(t2.loopEndBeat == doctest::Approx(4.0));
    const LoopRange off{48000, 96000, false};
    CHECK_FALSE(detail::transportAt(plan, 60000, &off).loopActive);
}

TEST_CASE("a key route feeds a plug-in's sidechain, aligned with its stage (ADR-0018)") {
    // A plays a sine into the master; B has no audio of its own and a key mix
    // (out = main + key) keyed from A before its fader. With every track at the
    // centre, B's output is A's pre-fader signal through the route's and B's pan
    // law: mix(with key) - mix(without) == A's output * kCentre, sample for
    // sample — any misalignment of the key would break it. Latency in A's chain,
    // before the key mix, or both, must not.
    constexpr const char* kKeyMix = "com.suppleaudio.test.keymix";
    const json::Value latent =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
            .value();
    for (int layout = 0; layout < 4; ++layout) {
        CAPTURE(layout);
        auto pool = makePool();
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id a = addTrack(t, "A");
        const Id b = addTrack(t, "B");
        addFx(t, a, kSine);
        if (layout & 1) addFx(t, a, kGain, latent);  // latency at the source
        if (layout & 2) addFx(t, b, kGain, latent);  // latency before the keyed stage
        const Id keyMix = addFx(t, b, kKeyMix);
        addNote(t, a, 0.0, 1.0, 69);
        const model::Session plain = commit(t);
        model::EditTxn t2(plain, 2);
        model::Route route;
        route.from.track = a;
        route.to.fx = keyMix;
        route.tap = model::Tap::PreFader;
        const Id rt = t2.create(route);
        const model::Session keyed = commit(t2);
        const auto dry = render(plain, pool, 0.5);
        const auto wet = render(keyed, pool, 0.5);
        REQUIRE(dry.audio.frames() == wet.audio.frames());
        double worst = 0, energy = 0;
        for (std::size_t i = 0; i < dry.audio.frames(); ++i) {
            const double d = static_cast<double>(dry.audio.channels[0][i]);
            const double w = static_cast<double>(wet.audio.channels[0][i]);
            worst = std::max(worst, std::fabs((w - d) - d * kCentre));
            energy += d * d;
        }
        CHECK(energy > 1.0);
        CHECK(worst < 1e-5);

        // Solo B: A is not heard, but still keys B.
        model::EditTxn t3(keyed, 3);
        t3.modify<model::Track>(b, [](model::Track& x) { x.solo = model::Solo::On; });
        const auto soloed = render(commit(t3), pool, 0.5);
        worst = 0;
        for (std::size_t i = 0; i < dry.audio.frames(); ++i)
            worst =
                std::max(worst, std::fabs(static_cast<double>(soloed.audio.channels[0][i]) -
                                          static_cast<double>(dry.audio.channels[0][i]) * kCentre));
        CHECK(worst < 1e-5);

        // A muted: a pre-fader key still feeds; a muted route does not.
        model::EditTxn t4(keyed, 3);
        t4.modify<model::Track>(a, [](model::Track& x) { x.mute = true; });
        const auto mutedSource = render(commit(t4), pool, 0.5);
        CHECK(peak(mutedSource.audio.channels[0]) ==
              doctest::Approx(peak(dry.audio.channels[0]) * kCentre).epsilon(1e-4));
        model::EditTxn t5(keyed, 3);
        t5.modify<model::Route>(rt, [](model::Route& r) { r.mute = true; });
        const auto mutedRoute = render(commit(t5), pool, 0.5);
        CHECK(peak(mutedRoute.audio.channels[0]) ==
              doctest::Approx(peak(dry.audio.channels[0])).epsilon(1e-6));
    }
}

TEST_CASE("auxiliary channels: a route to 3/4 keys a plug-in through its pins (ADR-0020)") {
    // A plays a sine (not to its parent); B is a 4-channel buss with a key mix
    // (out = main + key). Keyed by a key route (the reference), B outputs A's
    // pre-fader signal through the route's centre pan law; keyed from B's
    // channels 3/4, fed by a route mapping A's 1/2 there, the same signal
    // arrives linear — so aux == reference / kCentre sample for sample, with
    // latency at the source, before the keyed stage, or both.
    constexpr const char* kKeyMix = "com.suppleaudio.test.keymix";
    const json::Value latent =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
            .value();
    for (int layout = 0; layout < 4; ++layout) {
        CAPTURE(layout);
        auto pool = makePool();
        auto build = [&](bool aux) {
            model::EditTxn t(model::Session::create({.console9 = false}), 1);
            const Id b = addTrack(t, "B", kMaster, model::StripType::Buss);
            const Id a = addTrack(t, "A");
            t.modify<model::Track>(a, [](model::Track& x) { x.parentSend.enabled = false; });
            t.modify<model::Track>(b, [](model::Track& x) { x.channels = 4; });
            addFx(t, a, kSine);
            if (layout & 1) addFx(t, a, kGain, latent);
            if (layout & 2) addFx(t, b, kGain, latent);
            const Id keyMix = addFx(t, b, kKeyMix);
            addNote(t, a, 0.0, 1.0, 69);
            model::Route route;
            route.from.track = a;
            route.tap = model::Tap::PreFader;
            if (aux) {
                route.to.track = b;
                route.channels = {{1, 2}, {3, 4}};
                t.modify<model::FxInstance>(keyMix,
                                            [](model::FxInstance& f) { f.pins.key = {3, 4}; });
            } else {
                route.to.fx = keyMix;
            }
            t.create(route);
            return commit(t);
        };
        const auto ref = render(build(false), pool, 0.5);
        const auto aux = render(build(true), pool, 0.5);
        REQUIRE(aux.warnings.empty());
        REQUIRE(ref.audio.frames() == aux.audio.frames());
        double worst = 0, energy = 0;
        for (std::size_t i = 0; i < ref.audio.frames(); ++i) {
            const double r = static_cast<double>(ref.audio.channels[0][i]);
            const double x = static_cast<double>(aux.audio.channels[0][i]);
            worst = std::max(worst, std::fabs(x - r / kCentre));
            energy += x * x;
        }
        CHECK(energy > 1.0);
        CHECK(worst < 1e-5);
    }
}

TEST_CASE("auxiliary channels stay in their strip; pins move them to the main pair (ADR-0020)") {
    auto pool = makePool();
    // A's 1/2 into B's 3/4; B (4 channels) has `stage` pinned or not.
    auto build = [](std::optional<model::PinMap> pins, int channels = 4) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id b = addTrack(t, "B", kMaster, model::StripType::Buss);
        const Id a = addTrack(t, "A");
        t.modify<model::Track>(a, [](model::Track& x) { x.parentSend.enabled = false; });
        t.modify<model::Track>(b, [&](model::Track& x) { x.channels = channels; });
        addFx(t, a, kSine);
        addNote(t, a, 0.0, 1.0, 69);
        if (pins) {
            const Id g = addFx(t, b, kGain);
            t.modify<model::FxInstance>(g, [&](model::FxInstance& f) { f.pins = *pins; });
        }
        model::Route route;
        route.from.track = a;
        route.to.track = b;
        route.channels = {{1, 2}, {3, 4}};
        t.create(route);
        return commit(t);
    };
    // Nothing reads 3/4: the buss's parent hears nothing.
    auto silent = render(build(std::nullopt), pool, 0.5);
    REQUIRE(silent.warnings.empty());
    CHECK(peak(silent.audio.channels[0]) < 1e-9);
    // A plug-in at its default pins (1/2) does not hear them either.
    CHECK(peak(render(build(model::PinMap{}), pool, 0.5).audio.channels[0]) < 1e-9);
    // Reading 3/4 and writing 1/2 brings them to the main pair.
    auto moved = render(build(model::PinMap{{3, 4}, {1, 2}, {}}), pool, 0.5);
    REQUIRE(moved.warnings.empty());
    CHECK(peak(moved.audio.channels[0]) > 0.05);
    CHECK(peak(moved.audio.channels[1]) > 0.05);
    // On a 2-channel buss both pairs and the pins are warned about and skipped.
    auto narrow = render(build(model::PinMap{{3, 4}, {1, 2}, {}}, 2), pool, 0.5);
    CHECK(narrow.warnings.size() == 3);
    CHECK(peak(narrow.audio.channels[0]) < 1e-9);
}

TEST_CASE("a multi-out instrument's outputs reach their channels and route on (ADR-0021)") {
    // A (6 channels, not to its parent) plays the multi-out fixture: Main
    // 0.125 on 1/2, Kick 0.25 pinned to 3/4, Snare 0.5 to 5/6. A route from
    // one pair to buss K is what K (and the master) hears; linear console.
    host::setBridgeExecutable(BRIDGE_PATH);
    constexpr const char* kMultiOut = "com.suppleaudio.test.multiout";
    for (auto isolation : {model::Isolation::InProcess, model::Isolation::Dedicated}) {
        CAPTURE(static_cast<int>(isolation));
        auto pool = makePool();
        auto build = [&](std::vector<int> from, std::vector<int> pins, bool bypass = false) {
            model::EditTxn t(model::Session::create({.console9 = false}), 1);
            const Id k = addTrack(t, "K", kMaster, model::StripType::Buss);
            const Id a = addTrack(t, "A");
            t.modify<model::Track>(a, [](model::Track& x) {
                x.parentSend.enabled = false;
                x.channels = 6;
            });
            const Id fx = addFx(t, a, kMultiOut);
            t.modify<model::FxInstance>(fx, [&](model::FxInstance& f) {
                f.pins.out = pins;
                f.isolation = isolation;
                f.bypass = bypass;
            });
            model::Route route;
            route.from.track = a;
            route.to.track = k;
            route.channels = {from, {1, 2}};
            t.create(route);
            return commit(t);
        };
        const std::vector<int> pinned{1, 2, 3, 4, 5, 6};
        auto level = [&](const OfflineResult& r) {
            REQUIRE(r.warnings.empty());
            return peak(r.audio.channels[0], r.audio.frames() / 2);
        };
        const double main = level(render(build({1, 2}, pinned), pool, 0.2));
        const double kick = level(render(build({3, 4}, pinned), pool, 0.2));
        const double snare = level(render(build({5, 6}, pinned), pool, 0.2));
        // The main pair's route goes through the pan law; aux links are
        // linear, so the outputs' levels compare exactly.
        CHECK(main > 0.01);
        CHECK(kick > 0.01);
        CHECK(snare / kick == doctest::Approx(2.0).epsilon(1e-4));
        // Unpinned or 0, an output is not connected; bypassed, none is.
        CHECK(level(render(build({3, 4}, {1, 2, 0, 0, 5, 6}), pool, 0.2)) < 1e-9);
        CHECK(level(render(build({3, 4}, {}), pool, 0.2)) < 1e-9);
        CHECK(level(render(build({5, 6}, pinned, true), pool, 0.2)) < 1e-9);
        // Pins past the track's channels are warned about and skipped.
        const auto beyond = render(build({3, 4}, {1, 2, 3, 4, 9, 10}), pool, 0.2);
        CHECK(beyond.warnings.size() == 2);
    }
}

TEST_CASE("key routes: a plug-in without a sidechain is warned about; cycles are invalid") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id a = addTrack(t, "A");
    const Id b = addTrack(t, "B");
    addFx(t, a, kSine);
    const Id gain = addFx(t, b, kGain);  // no sidechain port
    const Id aGain = addFx(t, a, kGain);
    model::Route route;
    route.from.track = a;
    route.to.fx = gain;
    t.create(route);
    const auto s = commit(t);
    const auto r = render(s, pool, 0.1);
    REQUIRE_FALSE(r.warnings.empty());
    CHECK(r.warnings.back().find("no sidechain input") != std::string::npos);
    // B keying A's gain while A keys B's: a cycle.
    model::EditTxn t2(s, 2);
    model::Route back;
    back.from.track = b;
    back.to.fx = aGain;
    t2.create(back);
    CHECK_FALSE(t2.commit());
}

TEST_CASE("Console 9 stems decode channel and buss outputs without promising exact summation") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.seed = 42}), 1);
    const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
    const Id a = addTrack(t, "A", bus), b = addTrack(t, "B", bus);
    for (Id ch : {a, b}) {
        addFx(t, ch, kSine);
        addNote(t, ch, 0, 1);
    }
    const auto s = commit(t);
    auto mix = render(s, pool, .5);
    auto aStem = renderOffline(s, pool, {.endSeconds = .5, .track = a});
    auto bStem = renderOffline(s, pool, {.endSeconds = .5, .track = b});
    auto busStem = renderOffline(s, pool, {.endSeconds = .5, .track = bus});
    REQUIRE(aStem);
    REQUIRE(bStem);
    REQUIRE(busStem);
    CHECK(peak(aStem->audio.channels[0]) == doctest::Approx(.25 * kStrip).epsilon(1e-5));
    CHECK(peak(busStem->audio.channels[0]) ==
          doctest::Approx(peak(mix.audio.channels[0])).epsilon(1e-5));
    double difference = 0;
    for (std::size_t i = 0; i < mix.audio.frames(); ++i)
        difference = std::max(
            difference, std::abs(static_cast<double>(mix.audio.channels[0][i]) -
                                 kStrip * (static_cast<double>(aStem->audio.channels[0][i]) +
                                           static_cast<double>(bStem->audio.channels[0][i]))));
    CHECK(difference > .001);
    auto again = renderOffline(s, pool, {.endSeconds = .5, .blockSize = 37, .track = bus});
    REQUIRE(again);
    CHECK(again->audio.channels == busStem->audio.channels);
}

TEST_CASE(
    "take FX: an instrument plays its MIDI take, a latent effect stays aligned, take "
    "envelopes shape the take") {
    auto pool = makePool();
    // 1. A sine in the take's chain sounds exactly like the sine on the track.
    auto midiSession = [](bool inTake) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id tr = addTrack(t, "Synth");
        if (!inTake) addFx(t, tr, kSine);
        addNote(t, tr, 1.0, 2.0);
        if (inTake) {
            model::FxInstance fx;
            fx.plugin.id = kSine;
            const Id f = t.create(fx);
            for (const Id take : t.created())
                if (take.kind == model::Kind::Take)
                    t.modify<model::Take>(take, [&](model::Take& x) { x.chain = {f}; });
        }
        return commit(t);
    };
    auto onTrack = render(midiSession(false), pool, 2.0);
    auto inTake = render(midiSession(true), pool, 2.0);
    REQUIRE(inTake.warnings.empty());
    REQUIRE(onTrack.audio.frames() == inTake.audio.frames());
    CHECK(peak(inTake.audio.channels[0]) > 0.05);
    double worst = 0;
    for (std::size_t i = 0; i < onTrack.audio.frames(); ++i)
        worst = std::max(worst, std::fabs(double(onTrack.audio.channels[0][i]) -
                                          double(inTake.audio.channels[0][i])));
    CHECK(worst == 0.0);
    CHECK(firstNonZero(inTake.audio.channels[0]) == 24000 + 1);  // beat 1 at 120 BPM
    CHECK(peak(inTake.audio.channels[0], 72001) == 0.0);         // cut at the item's end (beat 3)

    // 2. Audio through a take chain with latency lines up with the plain take;
    //    a take volume envelope halves its second half; take FX need no track FX.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-takefx-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * M_PI * 440.0 * static_cast<double>(i) / 48000.0));
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone));
    auto audioSession = [&](bool takeFx, bool envelope) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id tr = addTrack(t, "Audio");
        const Id md = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 48000});
        model::Take take;
        take.source = md;
        if (takeFx) {
            model::FxInstance fx;
            fx.plugin.id = kGain;
            fx.patch =
                json::parse(
                    R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                    .value();
            take.chain = {t.create(fx)};
        }
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.position = 0.0;
        item.length = 2.0;  // 1 s
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        if (envelope) {
            model::Envelope env;
            env.target = {tk, "volume"};
            env.points = {{0.0, 0.0, model::PointShape::Square}, {1.0, -6.0206}};  // item beats
            t.create(env);
        }
        return commit(t);
    };
    MediaCache media;
    auto renderAt = [&](const model::Session& s) {
        auto r = renderOffline(
            s, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(r->warnings.empty());
        return std::move(*r);
    };
    const auto plain = renderAt(audioSession(false, false));
    const auto through = renderAt(audioSession(true, false));
    CHECK(through.outputLatency == 0);  // the take's latency runs ahead of the track
    double diff = 0;
    for (std::size_t i = 100; i < 47900; ++i)
        diff = std::max(diff, std::fabs(double(plain.audio.channels[0][i]) -
                                        double(through.audio.channels[0][i])));
    CHECK(diff < 1e-6);
    const auto ducked = renderAt(audioSession(false, true));
    CHECK(peak(ducked.audio.channels[0], 1000, 23000) ==
          doctest::Approx(peak(plain.audio.channels[0], 1000, 23000)).epsilon(1e-4));
    CHECK(peak(ducked.audio.channels[0], 25000, 47000) ==
          doctest::Approx(0.5 * peak(plain.audio.channels[0], 25000, 47000)).epsilon(1e-3));
    std::filesystem::remove_all(dir);
}

TEST_CASE("parent and route delay history survives label-only adoption") {
    for (bool routeSend : {false, true}) {
        auto pool = makePool();
        auto refPool = makePool();
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id latent = addTrack(t, "Latent");
        addFx(t, latent, kSine);
        addNote(t, latent, 0, 1);
        addFx(t, latent, kGain,
              json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                  .value());
        const Id source = addTrack(t, "Plain");
        addFx(t, source, kSine);
        addNote(t, source, 0, 1, 72);
        if (routeSend) {
            t.modify<model::Track>(source, [](auto& tr) { tr.parentSend.enabled = false; });
            model::Route route;
            route.from.track = source;
            route.to.track = kMaster;
            t.create(route);
        }
        const auto session = commit(t);
        auto old = detail::compile(session, pool, 0, 128).value();
        auto reference = detail::compile(session, refPool, 0, 128).value();
        REQUIRE(detail::prepare(old, session));
        REQUIRE(detail::prepare(reference, session));
        for (auto* instance : old.instances) REQUIRE(instance->startProcessing());
        for (auto* instance : reference.instances) REQUIRE(instance->startProcessing());
        for (int pos = 0; pos < 512; pos += 128) {
            REQUIRE(detail::processBlock(old, pos, 128));
            REQUIRE(detail::processBlock(reference, pos, 128));
        }
        model::EditTxn rename(session, 2);
        rename.modify<model::Track>(source, [](auto& tr) { tr.name = "Renamed"; });
        const auto renamed = commit(rename);
        auto next = detail::compile(renamed, pool, 0, 128).value();
        REQUIRE(detail::prepare(next, renamed));
        detail::carryMix(old, next, -1);
        detail::locate(next, 512, false);
        REQUIRE(detail::processBlock(next, 512, 128));
        REQUIRE(detail::processBlock(reference, 512, 128));
        CHECK(std::equal(next.nodes[next.mainMaster].post.l.begin(),
                         next.nodes[next.mainMaster].post.l.end(),
                         reference.nodes[reference.mainMaster].post.l.begin()));
        // An altered delay length must start empty.
        auto changed = detail::compile(renamed, pool, 0, 128).value();
        REQUIRE(detail::prepare(changed, renamed));
        for (auto& edge : changed.edges)
            if (edge.delay.length) edge.delay.init(65);
        detail::carryMix(next, changed, -1);
        detail::locate(changed, 640, false);
        for (const auto& edge : changed.edges)
            if (edge.delay.length)
                CHECK(std::all_of(edge.delay.buf[0].begin(), edge.delay.buf[0].end(),
                                  [](float x) { return x == 0; }));
        for (auto* instance : old.instances) instance->stopProcessing();
        for (auto* instance : reference.instances) instance->stopProcessing();
    }
}

TEST_CASE("retained timeline notes are released at a loop after adoption") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "Synth");
    const Id synth = addFx(t, tr, kSine);
    addNote(t, tr, 0, 2);
    const auto session = commit(t);
    auto old = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(old, session));
    for (auto* instance : old.instances) REQUIRE(instance->startProcessing());
    REQUIRE(detail::processBlock(old, 0, 128));
    auto next = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(next, session));
    detail::carryMix(old, next, -1);
    detail::locate(next, 128, false);
    auto node =
        std::find_if(next.nodes.begin(), next.nodes.end(), [&](auto& n) { return n.id == tr; });
    REQUIRE(node != next.nodes.end());
    CHECK(node->held[69] == 1);
    detail::wrapTo(next, 0, -1);
    REQUIRE(detail::processBlock(next, 0, 128));
    CHECK(node->held[69] == 1);  // off for the retained voice, then the new loop's on
    CHECK(node->chain[static_cast<std::size_t>(node->noteTarget)].blockEvents.size() >= 2);
    if (node->chain[static_cast<std::size_t>(node->noteTarget)].blockEvents.size() >= 2) {
        CHECK(node->chain[static_cast<std::size_t>(node->noteTarget)].blockEvents[0].kind ==
              host::Event::Kind::NoteOff);
        CHECK(node->chain[static_cast<std::size_t>(node->noteTarget)].blockEvents[1].kind ==
              host::Event::Kind::NoteOn);
    }
    auto changed = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(changed, session));
    for (auto& n : changed.nodes)
        if (n.id == tr) n.chain[static_cast<std::size_t>(n.noteTarget)].instance = nullptr;
    detail::carryMix(next, changed, -1);
    for (const auto& n : changed.nodes)
        if (n.id == tr) CHECK(n.held[69] == 0);
    pool.find(synth)->stopProcessing();
}

TEST_CASE("post-fader keys ignore source solo silencing and retain Console 9 encoding") {
    for (bool console : {false, true}) {
        model::EditTxn t(model::Session::create({.console9 = console}), 1);
        const Id source = addTrack(t, "Source");
        const Id destination = addTrack(t, "Destination");
        t.modify<model::Track>(source, [](auto& tr) { tr.parentSend.enabled = false; });
        addFx(t, source, kSine);
        addNote(t, source, 0, 1);
        const Id fx = addFx(t, destination, "com.suppleaudio.test.keymix");
        model::Route key;
        key.from.track = source;
        key.to.fx = fx;
        key.tap = model::Tap::PostFader;
        t.create(key);
        const auto plain = commit(t);
        model::EditTxn solo(plain, 2);
        solo.modify<model::Track>(destination, [](auto& tr) { tr.solo = model::Solo::On; });
        auto pool = makePool();
        auto refPool = makePool();
        const auto expected = render(plain, refPool, .1);
        const auto actual = render(commit(solo), pool, .1);
        CHECK(peak(actual.audio.channels[0]) > .01);
        CHECK(std::equal(actual.audio.channels[0].begin(), actual.audio.channels[0].end(),
                         expected.audio.channels[0].begin()));
    }
}

TEST_CASE("MIDI loop source offsets wrap cyclically and cover the final partial cycle") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 0, 4);
    Id takeId;
    const auto base = commit(t);
    for (const auto& take : base.takes) takeId = take.id;
    Id midiId = *base.takes.find(takeId)->source;
    model::EditTxn loop(base, 2);
    loop.modify<model::MidiSource>(midiId, [](auto& src) {
        src.sequence.notes = {{960, 1920, 0, 69, 127, 0}, {3360, 960, 0, 72, 127, 0}};
        src.sequence.controls = {{3600, midi::ControlKind::CC, 0, 1, 60}};
    });
    for (const auto& item : base.items)
        loop.modify<model::Item>(item.id, [](auto& it) {
            it.loop = true;
            it.length = 8;
        });
    const auto session = commit(loop);
    auto eventsAt = [&](double offset) {
        model::EditTxn edit(session, 3);
        edit.modify<model::Take>(takeId, [&](auto& take) { take.startOffset = offset; });
        const auto edited = commit(edit);
        auto plan = detail::compile(edited, pool, 0, 128).value();
        REQUIRE(detail::prepare(plan, edited));
        for (const auto& node : plan.nodes)
            if (node.id == tr) return node.events;
        return std::vector<detail::TimedEvent>{};
    };
    const auto canonical = eventsAt(0);
    CHECK(std::any_of(canonical.begin(), canonical.end(), [](const auto& e) {
        return e.sample == 108000 && e.event.key == 72 &&
               e.event.kind == host::Event::Kind::NoteOff;  // beat 4.5, across the source boundary
    }));
    for (double offset : {4., 12., 400.}) {
        const auto actual = eventsAt(offset);
        REQUIRE(actual.size() == canonical.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].sample == canonical[i].sample);
            CHECK(actual[i].event.kind == canonical[i].event.kind);
            CHECK(actual[i].event.key == canonical[i].event.key);
        }
    }
    const auto fractional = eventsAt(2.5);
    auto has = [&](std::int64_t sample, host::Event::Kind kind, int key) {
        return std::any_of(fractional.begin(), fractional.end(), [&](const auto& e) {
            return e.sample == sample && e.event.kind == kind && e.event.key == key;
        });
    };
    CHECK(has(0, host::Event::Kind::NoteOn, 69));
    CHECK(has(12000, host::Event::Kind::NoteOff, 69));
    CHECK(has(156000, host::Event::Kind::NoteOn, 69));
    CHECK(has(192000, host::Event::Kind::NoteOff, 69));  // clipped at item end
}

namespace {
struct EdgeControl {
    void (*requestProcess)(const clap_plugin_t*);
    void (*rescan)(const clap_plugin_t*, bool);
    void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
    void (*restart)(const clap_plugin_t*);
};
const EdgeControl& edgeControl(host::Instance& instance) {
    const auto* control = static_cast<const EdgeControl*>(
        instance.raw()->get_extension(instance.raw(), "supple.test.edge-control"));
    REQUIRE(control);
    return *control;
}
}  // namespace

TEST_CASE("restart reactivation errors retain structured failed FX and compensated latency") {
    auto pool = makePool();
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const auto track = addTrack(edit, "Restart");
    const auto fx = addFx(edit, track, "com.suppleaudio.test.edge");
    const auto session = commit(edit);
    auto* instance = pool.get(fx, *session.fx.find(fx), session).value();
    const std::array<std::uint8_t, 4> config{5, 0, 0, 0};
    REQUIRE(instance->loadState(config));
    auto initial = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(initial, session));
    CHECK(initial.outputLatency == 64);
    edgeControl(*instance).restart(instance->raw());
    auto next = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(next, session));
    CHECK_FALSE(instance->active());
    CHECK(next.instances.empty());
    CHECK(next.outputLatency == 64);
    const auto errors = pool.failedErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].first == fx);
    CHECK(errors[0].second.code == "host.activate");
    CHECK(errors[0].second.path == "/fx/" + fx.str());
    CHECK_FALSE(errors[0].second.hint.empty());
    CHECK(pool.failed().size() == 1);
    // The failed stage still delays incoming audio exactly like a bypass.
    auto& node =
        *std::find_if(next.nodes.begin(), next.nodes.end(), [&](auto& n) { return n.id == track; });
    std::array<float, 128> impulse{};
    impulse[0] = 1;
    const float* inputs[] = {impulse.data()};
    node.monitorInputs = {0};
    next.deviceIn = inputs;
    next.deviceInChannels = 1;
    REQUIRE(detail::processBlock(next, 0, 128, false));
    CHECK(node.preFader.l[0] == 0);
    CHECK(node.preFader.l[64] == 1);
    // Neither another prepare nor another render retries the refused activation.
    auto again = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(again, session));
    CHECK(again.instances.empty());
    auto offline = renderOffline(session, pool, {.endSeconds = .01, .threads = 1});
    REQUIRE(offline);
    CHECK(offline->outputLatency == 64);
    CHECK_FALSE(offline->warnings.empty());
    pool.restart(fx);
    CHECK(pool.failedErrors().empty());
}

TEST_CASE("parameter clear and rescan retain envelopes and re-resolve orphaned automation") {
    auto pool = makePool();
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const auto track = addTrack(edit, "Dynamic parameters");
    const auto fx = addFx(edit, track, "com.suppleaudio.test.edge");
    model::Envelope envelope;
    envelope.target = {fx, "7"};
    envelope.points = {{0, 2, model::PointShape::Linear, 0}, {1, 3, model::PointShape::Linear, 0}};
    const auto env = edit.create(envelope);
    const auto session = commit(edit);
    auto* instance = pool.get(fx, *session.fx.find(fx), session).value();
    REQUIRE(instance->loadState(std::array<std::uint8_t, 4>{12, 0, 0, 0}));
    auto make = [&] {
        auto plan = detail::compile(session, pool, 0, 128).value();
        REQUIRE(detail::prepare(plan, session));
        return plan;
    };
    auto lanes = [&](detail::Plan& plan) -> const auto& {
        return std::find_if(plan.nodes.begin(), plan.nodes.end(),
                            [&](auto& n) { return n.id == track; })
            ->chain[0]
            .lanes;
    };
    auto initial = make();
    REQUIRE(lanes(initial).size() == 1);
    edgeControl(*instance).clear(instance->raw(), 8, CLAP_PARAM_CLEAR_ALL);
    auto unrelated = make();
    CHECK(lanes(unrelated).size() == 1);
    edgeControl(*instance).clear(instance->raw(), 7, CLAP_PARAM_CLEAR_MODULATIONS);
    auto modulation = make();
    CHECK(lanes(modulation).size() == 1);  // no modulation lane in this engine
    edgeControl(*instance).clear(instance->raw(), 7, CLAP_PARAM_CLEAR_AUTOMATIONS);
    auto cleared = make();
    CHECK(lanes(cleared).empty());
    CHECK_FALSE(cleared.warnings.empty());
    CHECK(session.envelopes.find(env)->points.size() == 2);
    instance->deactivate();
    edgeControl(*instance).rescan(instance->raw(), false);
    auto restored = make();
    CHECK(lanes(restored).size() == 1);
    instance->deactivate();
    edgeControl(*instance).rescan(instance->raw(), true);
    auto orphaned = make();
    CHECK(lanes(orphaned).empty());
    CHECK(std::any_of(orphaned.warnings.begin(), orphaned.warnings.end(), [&](const auto& warning) {
        return warning.find(fx.str()) != std::string::npos &&
               warning.find("orphaned automation parameter 7") != std::string::npos;
    }));
    REQUIRE(instance->startProcessing());
    REQUIRE(detail::processBlock(orphaned, 0, 128));
    for (const auto& node : orphaned.nodes)
        for (const auto& stage : node.chain)
            CHECK(std::none_of(
                stage.blockEvents.begin(), stage.blockEvents.end(),
                [](const auto& event) { return event.kind == host::Event::Kind::Param; }));
    instance->stopProcessing();
}

TEST_CASE("NoteEnd matches overlapping voices by id tuple and wildcards") {
    for (std::uint8_t mask : {0, 1, 3, 7, 15}) {
        auto pool = makePool();
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        const auto track = addTrack(edit, "Voices");
        const auto fx = addFx(edit, track, "com.suppleaudio.test.edge");
        addNote(edit, track, 0, 2);
        const auto base = commit(edit);
        model::EditTxn notes(base, 2);
        for (const auto& entry : base.midi)
            notes.modify<model::MidiSource>(entry.id, [](auto& source) {
                source.sequence.notes = {{0, 960, 0, 69, 127, 0},
                                         {0, 960, 0, 69, 127, 0},
                                         {0, 960, 1, 69, 127, 0},
                                         {0, 960, 1, 72, 127, 0}};
            });
        const auto session = commit(notes);
        auto* instance = pool.get(fx, *session.fx.find(fx), session).value();
        REQUIRE(instance->loadState(std::array<std::uint8_t, 4>{14, 0, mask, 0}));
        auto plan = detail::compile(session, pool, 0, 128).value();
        REQUIRE(detail::prepare(plan, session));
        REQUIRE(instance->startProcessing());
        REQUIRE(detail::processBlock(plan, 0, 128));
        const auto node = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                       [&](auto& n) { return n.id == track; });
        REQUIRE(node != plan.nodes.end());
        CHECK(node->held[69] == (mask == 0 ? 1 : 0));
        CHECK(node->held[128 + 69] == (mask >= 7 ? 0 : 1));
        CHECK(node->held[128 + 72] == (mask == 15 ? 0 : 1));
        instance->stopProcessing();
    }
    const detail::HeldVoice voice{42, 2, 3, 69, false};
    CHECK(detail::matchesVoice(voice, -1, -1, -1, 42));
    CHECK_FALSE(detail::matchesVoice(voice, 1, -1, -1, 42));
    CHECK_FALSE(detail::matchesVoice(voice, -1, -1, -1, 43));
    CHECK(detail::matchesVoice(voice, 2, 3, 69, -1));
    CHECK_FALSE(detail::matchesVoice(voice, 2, 3, 70, -1));
}

TEST_CASE("acknowledging output-derived patches prevents a parameter echo on reload") {
    auto pool = makePool();
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const auto track = addTrack(edit, "Output patch");
    const auto fx = addFx(
        edit, track, kGain,
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":0}})").value());
    const auto session = commit(edit);
    auto* instance = pool.get(fx, *session.fx.find(fx), session).value();
    const auto before = instance->paramValue(0).value();
    auto outputModel = *session.fx.find(fx);
    // A different valid value makes accidental re-delivery observable. This
    // method only acknowledges model state; it must never edit the instance.
    outputModel.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12}})").value();
    pool.acknowledgeOutputPatch(fx, *outputModel.patch);
    REQUIRE(pool.get(fx, outputModel, session));
    CHECK(instance->paramValue(0).value() == before);
    // An unrelated later user edit still takes the ordinary delivery path.
    outputModel.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}})").value();
    REQUIRE(pool.get(fx, outputModel, session));
    CHECK(instance->paramValue(0).value() != before);
}

TEST_CASE("voice tracking overflow drops onsets before DSP and keeps loop releases addressable") {
    auto pool = makePool();
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const auto track = addTrack(edit, "Dense voices");
    const auto fx = addFx(edit, track, "com.suppleaudio.test.edge");
    addNote(edit, track, 0, 2);
    const auto base = commit(edit);
    model::EditTxn dense(base, 2);
    for (const auto& entry : base.midi)
        dense.modify<model::MidiSource>(entry.id, [](auto& source) {
            source.sequence.notes.assign(4097, midi::Note{0, 960, 0, 69, 127, 0});
        });
    const auto session = commit(dense);
    auto plan = detail::compile(session, pool, 0, 4096).value();
    REQUIRE(detail::prepare(plan, session));
    auto* instance = pool.find(fx);
    REQUIRE(instance->startProcessing());
    REQUIRE(detail::processBlock(plan, 0, 4096));
    auto& node =
        *std::find_if(plan.nodes.begin(), plan.nodes.end(), [&](auto& n) { return n.id == track; });
    auto& stage = node.chain[0];
    CHECK(stage.voices.size() == 4096);
    CHECK(stage.voiceDropped == 1);
    CHECK(std::count_if(stage.blockEvents.begin(), stage.blockEvents.end(),
                        [](const auto& e) { return e.kind == host::Event::Kind::NoteOn; }) == 4096);
    detail::wrapTo(plan, 0, -1);
    REQUIRE(detail::processBlock(plan, 0, 4096));
    CHECK(stage.voices.size() == 4096);
    CHECK(stage.voiceDropped == 2);
    CHECK(std::count_if(stage.blockEvents.begin(), stage.blockEvents.end(), [](const auto& e) {
              return e.kind == host::Event::Kind::NoteOff;
          }) == 4096);
    CHECK(stage.voices.front().noteId == 4096);  // old NoteEnds cannot clear the new loop
    instance->stopProcessing();
}

TEST_CASE("voice source identities survive MIDI renumbering and release removed notes") {
    for (bool remove : {false, true}) {
        auto pool = makePool();
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        const auto track = addTrack(edit, "Edited notes");
        addFx(edit, track, kSine);
        addNote(edit, track, 0, 2);
        const auto session = commit(edit);
        auto old = detail::compile(session, pool, 0, 128).value();
        REQUIRE(detail::prepare(old, session));
        for (auto* instance : old.instances) REQUIRE(instance->startProcessing());
        REQUIRE(detail::processBlock(old, 0, 128));
        model::EditTxn change(session, 2);
        for (const auto& entry : session.midi)
            change.modify<model::MidiSource>(entry.id, [&](auto& source) {
                if (remove)
                    source.sequence.notes.clear();
                else
                    source.sequence.notes.insert(source.sequence.notes.begin(),
                                                 midi::Note{960, 100, 0, 72, 127, 0});
            });
        const auto edited = commit(change);
        auto next = detail::compile(edited, pool, 0, 128).value();
        REQUIRE(detail::prepare(next, edited));
        detail::carryMix(old, next, -1);
        detail::locate(next, 128, false);
        auto& node = *std::find_if(next.nodes.begin(), next.nodes.end(),
                                   [&](auto& n) { return n.id == track; });
        REQUIRE(detail::processBlock(next, 128, 128));
        CHECK(node.held[69] == (remove ? 0 : 1));
        if (!remove) {
            REQUIRE(node.chain[0].voices.size() == 1);
            CHECK(node.chain[0].voices[0].sourceId == 1);  // the compiled source was renumbered
            REQUIRE(detail::processBlock(next, 48000, 128));
            CHECK(node.held[69] == 0);
            CHECK(node.chain[0].voices.empty());
        }
        for (auto* instance : old.instances) instance->stopProcessing();
    }
}

TEST_CASE("held musical source identities survive an in-span tempo edit") {
    auto pool = makePool();
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const auto track = addTrack(edit, "Tempo edits");
    addFx(edit, track, kSine);
    addNote(edit, track, 1, 4);
    const auto session = commit(edit);
    auto old = detail::compile(session, pool, 0, 128).value();
    REQUIRE(detail::prepare(old, session));
    for (auto* instance : old.instances) REQUIRE(instance->startProcessing());
    REQUIRE(detail::processBlock(old, 24000, 128));
    model::EditTxn tempo(session, 2);
    for (const auto& point : session.tempo)
        tempo.modify<model::TempoPointObj>(point.id, [](auto& value) { value.bpm = 90; });
    const auto edited = commit(tempo);
    auto next = detail::compile(edited, pool, 0, 128).value();
    REQUIRE(detail::prepare(next, edited));
    detail::carryMix(old, next, -1);
    detail::locate(next, 48000, false);
    REQUIRE(detail::processBlock(next, 48000, 128));
    auto& node =
        *std::find_if(next.nodes.begin(), next.nodes.end(), [&](auto& n) { return n.id == track; });
    CHECK(node.held[69] == 1);  // onset sample changed, but the musical note is still current
    REQUIRE(detail::processBlock(next, 160000, 128));
    CHECK(node.held[69] == 0);
    for (auto* instance : old.instances) instance->stopProcessing();
}

TEST_CASE("through-master stems of nested channels use the solo signal path") {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
    const Id channel = addTrack(t, "Chosen", bus);
    const Id sibling = addTrack(t, "Sibling", bus);
    const Id sendBus = addTrack(t, "Send", kMaster, model::StripType::Buss);
    const Id unrelated = addTrack(t, "Unrelated");
    for (Id track : {channel, sibling, unrelated}) addFx(t, track, kSine);
    addNote(t, channel, 0, 2, 60);
    addNote(t, sibling, 0, 2, 64);
    addNote(t, unrelated, 0, 2, 67);
    t.modify<model::Track>(unrelated, [](auto& tr) { tr.solo = model::Solo::Defeat; });
    model::Route route;
    route.from.track = channel;
    route.to.track = sendBus;
    t.create(route);
    const auto session = commit(t);
    model::EditTxn solo(session, 2);
    solo.modify<model::Track>(channel, [](auto& tr) { tr.solo = model::Solo::On; });
    solo.modify<model::Track>(unrelated, [](auto& tr) { tr.solo = model::Solo::Off; });
    const auto isolated = commit(solo);
    auto referencePool = makePool();
    auto stemPool = makePool();
    const auto reference = render(isolated, referencePool, 1);
    auto stem = renderOffline(session, stemPool, {.endSeconds = 1, .throughMaster = channel});
    REQUIRE(stem);
    CHECK(peak(stem->audio.channels[0]) > 0.01);
    CHECK(bool(stem->audio.channels == reference.audio.channels));
    CHECK(session.tracks.find(channel)->solo == model::Solo::Off);
    CHECK(session.tracks.find(unrelated)->solo == model::Solo::Defeat);
}

TEST_CASE(
    "media identities invalidate decoded and stretched audio while old leases stay immutable") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-media-identity-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto file = dir / "source.wav";
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(9600));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.25 * std::sin(2 * 3.141592653589793 * 440 * i / 48000));
    REQUIRE(audio::writeWav(file, tone));
    MediaCache cache;
    auto old = cache.get(file, 48000);
    auto stretched = cache.get(file, 48000, {.playrate = 0.5});
    REQUIRE(old);
    REQUIRE(stretched);
    const auto oldSamples = (*old)->channels;
    const auto oldStretch = (*stretched)->channels;
    for (auto& channel : tone.channels)
        for (float& sample : channel) sample *= 2;
    REQUIRE(audio::writeWav(file, tone));
    auto current = cache.get(file, 48000);
    auto currentStretch = cache.get(file, 48000, {.playrate = 0.5});
    MediaCache fresh;
    auto expected = fresh.get(file, 48000);
    auto expectedStretch = fresh.get(file, 48000, {.playrate = 0.5});
    REQUIRE(current);
    REQUIRE(currentStretch);
    REQUIRE(expected);
    REQUIRE(expectedStretch);
    CHECK(bool((*current)->channels == (*expected)->channels));
    CHECK(bool((*currentStretch)->channels == (*expectedStretch)->channels));
    CHECK(bool((*old)->channels == oldSamples));
    CHECK(bool((*stretched)->channels == oldStretch));
    CHECK(*old != *current);
    std::filesystem::remove_all(dir);
}

TEST_CASE("media identities share hashed content and detect inode and nanosecond changes") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-content-identity-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto file = dir / "source.wav", alias = dir / "copy.wav";
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(9600));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.25 * std::sin(2 * 3.141592653589793 * 440 * i / 48000));
    REQUIRE(audio::writeWav(file, tone));
    const auto hashA = "sha256:" + sha256File(file).value();
    REQUIRE(writeFileAtomic(alias, readFile(file).value()));
    MediaCache cache;
    auto old = cache.get(file, 48000, {}, hashA);
    auto copy = cache.get(alias, 48000, {}, hashA);
    auto unHashed = cache.get(file, 48000);
    REQUIRE(old);
    REQUIRE(copy);
    REQUIRE(unHashed);
    CHECK(*old == *copy);
    const auto original = (*old)->channels;
    struct stat before{}, after{};
    REQUIRE(::stat(file.c_str(), &before) == 0);
    for (auto& channel : tone.channels)
        for (float& sample : channel) sample *= 2;
    REQUIRE(audio::writeWav(file, tone));
    const timespec sameTimes[]{before.st_atim, before.st_mtim};
    REQUIRE(::utimensat(AT_FDCWD, file.c_str(), sameTimes, 0) == 0);
    REQUIRE(::stat(file.c_str(), &after) == 0);
    CHECK(before.st_ino != after.st_ino);
    CHECK(before.st_size == after.st_size);
    CHECK(before.st_mtim.tv_sec == after.st_mtim.tv_sec);
    CHECK(before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
    auto replacement = cache.get(file, 48000);
    REQUIRE(replacement);
    CHECK(*replacement != *unHashed);
    CHECK(bool((*replacement)->channels == tone.channels));
    const auto hashB = "sha256:" + sha256File(file).value();
    auto hashedReplacement = cache.get(file, 48000, {}, hashB);
    REQUIRE(hashedReplacement);
    CHECK(bool((*hashedReplacement)->channels == tone.channels));
    // A new variant of the old hash must derive from its immutable decoded
    // content, even though the filename now contains different bytes.
    auto oldVariant = cache.get(file, 48000, {.playrate = 0.5}, hashA);
    const auto expectedOld = stretchAudio(**old, 0.5, 0);
    REQUIRE(oldVariant);
    REQUIRE(expectedOld);
    CHECK(bool((*oldVariant)->channels == expectedOld->channels));
    CHECK(bool((*old)->channels == original));
    // Rewrite in place, preserving size/inode and the seconds part of mtime.
    for (auto& channel : tone.channels)
        for (float& sample : channel) sample *= 0.25f;
    const auto encoded = audio::encodeWav(tone);
    REQUIRE(encoded);
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded->data()),
                     static_cast<std::streamsize>(encoded->size()));
        REQUIRE(output.good());
    }
    timespec changedTimes[]{after.st_atim, after.st_mtim};
    changedTimes[1].tv_nsec = (after.st_mtim.tv_nsec + 1) % 1000000000;
    REQUIRE(::utimensat(AT_FDCWD, file.c_str(), changedTimes, 0) == 0);
    auto changed = cache.get(file, 48000);
    REQUIRE(changed);
    CHECK(*changed != *replacement);
    CHECK(bool((*changed)->channels == tone.channels));
    // The compiler must pass MediaSource.hash into that same identity cache.
    auto session = [&](const std::filesystem::path& path) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id track = addTrack(t, "Audio");
        const Id media = t.create(model::MediaSource{path.string(), hashA, "wav", 48000, 2, 9600});
        model::Take take;
        take.source = media;
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = 0.4;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return commit(t);
    };
    auto pool = makePool();
    auto fromHash = renderOffline(session(file), pool, {.endSeconds = 0.1, .media = &cache});
    auto freshPool = makePool();
    auto fromCopy = renderOffline(session(alias), freshPool, {.endSeconds = 0.1});
    REQUIRE(fromHash);
    REQUIRE(fromCopy);
    CHECK(bool(fromHash->audio.channels == fromCopy->audio.channels));
    std::filesystem::remove_all(dir);
}

TEST_CASE("through-master stems retain the solo playback sidechain taps") {
    for (bool console : {false, true}) {
        model::EditTxn t(model::Session::create({.console9 = console}), 1);
        const Id bus = addTrack(t, "Bus", kMaster, model::StripType::Buss);
        const Id chosen = addTrack(t, "Chosen", bus);
        const Id keySource = addTrack(t, "Key");
        addFx(t, keySource, kSine);
        addNote(t, keySource, 0, 2);
        const Id fx = addFx(t, chosen, "com.suppleaudio.test.keymix");
        model::Route key;
        key.from.track = keySource;
        key.to.fx = fx;
        key.tap = model::Tap::PostFader;
        t.create(key);
        const auto session = commit(t);
        model::EditTxn solo(session, 2);
        solo.modify<model::Track>(chosen, [](auto& track) { track.solo = model::Solo::On; });
        auto refPool = makePool();
        auto stemPool = makePool();
        const auto reference = render(commit(solo), refPool, .1);
        auto stem = renderOffline(session, stemPool, {.endSeconds = .1, .throughMaster = chosen});
        REQUIRE(stem);
        CHECK(peak(stem->audio.channels[0]) > .01);
        CHECK(bool(stem->audio.channels == reference.audio.channels));
    }
}

TEST_CASE("media cache releases originals used only to build playback variants") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-decoded-lifetime-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto file = dir / "source.wav", other = dir / "other.wav";
    audio::AudioData tone;
    tone.sampleRate = 44100;
    tone.channels.assign(2, std::vector<float>(8820));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(.25 * std::sin(2 * 3.141592653589793 * 440 * double(i) / 44100));
    REQUIRE(audio::writeWav(file, tone));
    REQUIRE(audio::writeWav(other, tone));
    for (bool stretch : {false, true})
        for (double rate : {44100.0, 48000.0}) {
            if (!stretch && rate == 44100) continue;
            CAPTURE(stretch);
            CAPTURE(rate);
            MediaCache cache;
            MediaVariant variant;
            if (stretch) variant.playrate = .5;
            auto result = cache.get(file, rate, variant);
            REQUIRE(result);
            const auto original = MediaCacheTestAccess::decoded(cache);
            CHECK(original.expired());        // the variant must not own its decoded input
            CHECK(result->use_count() == 2);  // caller and playback-variant cache
            // Same-rate playback bypasses the resampler; invoking it anyway
            // would apply its low-pass filter and change the reference audio.
            auto resampled = rate == tone.sampleRate ? Result<audio::AudioData>(tone)
                                                     : audio::resample(tone, rate);
            REQUIRE(resampled);
            if (stretch) {
                auto expected = stretchAudio(*resampled, .5, 0);
                REQUIRE(expected);
                CHECK(bool((*result)->channels == expected->channels));
            } else
                CHECK(bool((*result)->channels == resampled->channels));
            auto same = cache.get(file, rate, variant);
            REQUIRE(same);
            CHECK(*same == *result);
            // A different identity on a miss opportunistically prunes the
            // expired original without touching the still-cached variant.
            auto passthrough = cache.get(other, 44100);
            REQUIRE(passthrough);
            CHECK(MediaCacheTestAccess::decodedEntries(cache) == 1);
            const auto retained = MediaCacheTestAccess::decoded(cache).lock();
            REQUIRE(retained);
            CHECK(retained == *passthrough);
            CHECK(original.expired());
            CHECK(bool((*result)->channels == (*same)->channels));
        }
    // A passthrough entry owns its original, and a lease survives the cache.
    std::shared_ptr<const audio::AudioData> lease;
    std::weak_ptr<const audio::AudioData> original;
    {
        MediaCache cache;
        auto direct = cache.get(file, 44100);
        REQUIRE(direct);
        original = MediaCacheTestAccess::decoded(cache);
        CHECK_FALSE(original.expired());
        CHECK(direct->use_count() == 2);
        auto stretched = cache.get(file, 48000, {.playrate = .5});
        REQUIRE(stretched);
        CHECK_FALSE(original.expired());
        CHECK(direct->use_count() == 2);
        CHECK(bool((*direct)->channels == tone.channels));
        lease = *direct;
    }
    CHECK_FALSE(original.expired());
    CHECK(bool(lease->channels == tone.channels));
    lease.reset();
    CHECK(original.expired());
    std::filesystem::remove_all(dir);
}

namespace {
json::Value gainPatch(double db, const char* latency = "none") {
    json::Value patch = json::parse(R"({"schema": "supple.test-gain-patch/1.0"})").value();
    patch["params"]["gain"] = db;
    patch["params"]["latency"] = latency;
    return patch;
}

// The largest difference between two signals over [from, to).
double maxDiffFrom(const std::vector<float>& a, const std::vector<float>& b, double scaleB,
                   std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = from; i < std::min({to, a.size(), b.size()}); ++i)
        m = std::max(m, std::fabs(double(a[i]) - scaleB * double(b[i])));
    return m;
}
}  // namespace

TEST_CASE("pdc ignore leaves a plug-in's latency out of compensation") {
    std::size_t onset[2] = {0, 0};
    for (bool ignore : {false, true}) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id channel = addTrack(t, "Channel");
        addFx(t, channel, kSine);
        const Id fx = addFx(t, channel, kGain, gainPatch(0, "short"));
        if (ignore)
            t.modify<model::FxInstance>(
                fx, [](model::FxInstance& f) { f.pdc = model::PdcMode::Ignore; });
        addNote(t, channel, 0, 2);
        const auto s = commit(t);
        auto pool = makePool();
        const auto r = render(s, pool, 0.5);
        CHECK(r.outputLatency == (ignore ? 0 : 64));
        onset[ignore] = firstNonZero(r.audio.channels[0]);
    }
    CHECK(onset[1] == onset[0] + 64);  // nothing waits for it: it arrives late
}

TEST_CASE("parallel FX share their input and sum, aligned to the slowest branch") {
    auto build = [](bool parallel, bool withGains) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id channel = addTrack(t, "Channel");
        addFx(t, channel, kSine);
        if (withGains) {
            addFx(t, channel, kGain, gainPatch(-6, "short"));
            const Id second = addFx(t, channel, kGain, gainPatch(-6));
            t.modify<model::FxInstance>(second,
                                        [&](model::FxInstance& f) { f.parallel = parallel; });
        }
        addNote(t, channel, 0, 2);
        return commit(t);
    };
    auto referencePool = makePool(), serialPool = makePool(), parallelPool = makePool();
    const auto reference = render(build(false, false), referencePool, 0.5);
    const auto serial = render(build(false, true), serialPool, 0.5);
    const auto parallel = render(build(true, true), parallelPool, 0.5);
    REQUIRE(peak(reference.audio.channels[0]) > 0.1);
    const double g = std::pow(10.0, -6.0 / 20.0);
    CHECK(serial.outputLatency == 64);
    CHECK(parallel.outputLatency == 64);  // the slowest branch, not the sum
    const auto n = reference.audio.channels[0].size();
    for (int c = 0; c < 2; ++c) {
        const auto& ref = reference.audio.channels[static_cast<std::size_t>(c)];
        CHECK(maxDiffFrom(serial.audio.channels[static_cast<std::size_t>(c)], ref, g * g, 0, n) <
              1e-5);
        // Both branches hear the sine; the faster one waits for the slower.
        CHECK(maxDiffFrom(parallel.audio.channels[static_cast<std::size_t>(c)], ref, 2 * g, 0, n) <
              1e-5);
    }
}

TEST_CASE("an oversampled plug-in runs at a multiple of the rate, compensated exactly") {
    auto build = [](int os, const char* latency, bool effect) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        const Id channel = addTrack(t, "Channel");
        const Id sine = addFx(t, channel, kSine);
        if (effect) {
            const Id fx = addFx(t, channel, kGain, gainPatch(0, latency));
            t.modify<model::FxInstance>(fx, [&](model::FxInstance& f) { f.oversampling = os; });
        } else {
            t.modify<model::FxInstance>(sine, [&](model::FxInstance& f) { f.oversampling = os; });
        }
        addNote(t, channel, 0, 2);
        return commit(t);
    };
    auto referencePool = makePool();
    const auto reference = render(build(1, "none", false), referencePool, 0.5);
    const auto n = reference.audio.channels[0].size();
    struct Case {
        int os;
        const char* latency;
        bool effect;
        std::int64_t expected;
    };
    // The resamplers' round trip is 72/80/83 samples at 2/4/8x; an
    // instrument's output only crosses the downward half; the plug-in's own
    // 64 high-rate samples are padded to whole plan samples.
    for (const Case& k :
         {Case{2, "none", true, 72}, Case{4, "none", true, 80}, Case{8, "none", true, 83},
          Case{2, "short", true, 72 + 32}, Case{8, "short", true, 91}, Case{2, "none", false, 36},
          Case{8, "none", false, 42}}) {
        INFO("os " << k.os << " latency " << k.latency << " effect " << k.effect);
        auto pool = makePool();
        const auto r = render(build(k.os, k.latency, k.effect), pool, 0.5);
        CHECK(r.outputLatency == k.expected);
        // A 440 Hz sine is far inside the protected band: once the note's
        // onset has rung out, the result matches the plain render.
        CHECK(maxDiffFrom(r.audio.channels[0], reference.audio.channels[0], 1.0, 2000, n) < 1e-3);
    }
}
