// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include "plan.h"

using namespace supple;
using namespace supple::engine;
using namespace supple::engine::detail;
using model::Id;
using model::kMaster;
namespace {
InstancePool emptyPool() {
    return InstancePool([](const model::PluginRef&) -> Result<host::PluginInfo> {
        return Error("test.plugin", "no plugins");
    });
}
InstancePool fixturePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path path = FIXTURE_CLAP_PATH;
        auto plugins = host::scan(std::span<const std::filesystem::path>(&path, 1));
        if (!plugins) return std::move(plugins).error();
        for (const auto& p : *plugins)
            if (p.id == ref.id) return p;
        return Error("test.plugin", ref.id);
    });
}
Id strip(model::EditTxn& t, model::StripType type, Id parent = kMaster) {
    model::Track tr;
    tr.type = type;
    tr.parent = type == model::StripType::Master ? std::optional<Id>() : parent;
    const Id id = t.create(tr);
    if (tr.parent)
        t.modify<model::Track>(parent, [&](auto& p) { p.children.push_back(id); });
    else {
        auto project = t.project();
        project.masters.push_back(id);
        t.setProject(project);
    }
    return id;
}
Id fx(model::EditTxn& t, Id track, std::string plugin, bool latency = false) {
    model::FxInstance f;
    f.plugin.id = std::move(plugin);
    if (latency)
        f.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                .value();
    const Id id = t.create(f);
    t.modify<model::Track>(track, [&](auto& tr) { tr.chain.push_back(id); });
    return id;
}
void envelope(model::EditTxn& t, Id id, std::string param, double a, double b) {
    model::Envelope env;
    env.target = {id, std::move(param)};
    env.timebase = model::Timebase::Time;
    env.points = {{0, a, model::PointShape::Square}, {.01, b}};
    t.create(env);
}
model::Session monitored(bool on, int count = 1, bool buss = false) {
    model::EditTxn t(model::Session::create({.console9 = on, .seed = 12345}), 1);
    const Id parent = buss ? strip(t, model::StripType::Buss) : kMaster;
    for (int i = 0; i < count; ++i) {
        const Id ch = strip(t, model::StripType::Channel, parent);
        model::Route input;
        input.from.hwChannels = std::vector<int>{1};
        input.to.track = ch;
        const Id inputId = t.create(input);
        t.modify<model::Track>(ch, [&](auto& tr) {
            tr.record.monitor = model::Monitor::On;
            tr.record.armed = true;  // monitoring needs the arm
            tr.record.input = inputId;
        });
    }
    auto s = t.commit();
    REQUIRE(s);
    return *s;
}
Plan plan(const model::Session& s, InstancePool& pool, std::uint32_t block = 512) {
    auto p = compile(s, pool, 0, block);
    REQUIRE(p);
    REQUIRE(prepare(*p, s));
    REQUIRE(checkStrips(*p));
    return std::move(*p);
}
std::vector<float> run(Plan& p, float input, std::uint32_t block, std::uint32_t total = 2048) {
    std::vector<float> source(p.block, input), out;
    const float* inputs[] = {source.data()};
    p.deviceIn = inputs;
    p.deviceInChannels = 1;
    for (std::uint32_t pos = 0; pos < total;) {
        const auto n = std::min(block, total - pos);
        REQUIRE(processBlock(p, pos, n));
        const auto& v = p.nodes[p.mainMaster].post.l;
        out.insert(out.end(), v.begin(), v.begin() + n);
        pos += n;
    }
    p.deviceIn = nullptr;
    return out;
}
}  // namespace

TEST_CASE("Console 9: unity staging, level matched off, correlated summing and silence") {
    auto pool = emptyPool();
    auto on = plan(monitored(true), pool), off = plan(monitored(false), pool);
    const auto a = run(on, .001f, 128), b = run(off, .001f, 127);
    CHECK(20 * std::log10(static_cast<double>(a[100]) / .001) ==
          doctest::Approx(-6.518658).epsilon(.00001));
    CHECK(std::abs(a[100] - b[100]) < 1e-9f);
    locate(on, 0);
    CHECK(run(on, .001f, 17) == a);
    locate(off, 0);
    const auto twice = run(off, .002f, 256);
    CHECK(twice[100] == 2 * b[100]);
    auto sumOn = plan(monitored(true, 2, true), pool);
    auto sumOff = plan(monitored(false, 2, true), pool);
    const auto c = run(sumOn, .5f, 128), d = run(sumOff, .5f, 128);
    // The inverse Buss curve is convex: coincident encoded inputs expand before clipping.
    CHECK(c[100] > d[100] * 1.03f);
    auto quiet = plan(monitored(true), pool);
    const auto silence = run(quiet, 0, 32);
    float peak = 0;
    for (float v : silence) peak = std::max(peak, std::abs(v));
    CHECK(peak < 1e-7f);  // -140 dBFS
    std::printf(
        "Console 9 measurements: unity %.6f dB, on/off difference %.3g, correlated buss %.6f dB vs "
        "linear, silence peak %.3f dBFS\n",
        20 * std::log10(static_cast<double>(a[100]) / .001),
        static_cast<double>(std::abs(a[100] - b[100])),
        20 * std::log10(static_cast<double>(c[100]) / static_cast<double>(d[100])),
        20 * std::log10(static_cast<double>(peak)));
    locate(quiet, 0);
    CHECK(run(quiet, 0, 127) == silence);
}

TEST_CASE("Console 9: send sharing, tap encoders, control ramps and mode swaps") {
    auto pool = emptyPool();
    auto base = monitored(false);
    model::EditTxn t(base, 2);
    const Id source{model::Kind::Track, 1};
    const Id bus = strip(t, model::StripType::Buss);
    t.modify<model::Track>(source, [](auto& tr) { tr.parentSend.enabled = false; });
    model::Route route;
    route.from.track = source;
    route.to.track = bus;
    const Id send = t.create(route);
    auto s = t.commit();
    REQUIRE(s);
    auto shared = plan(*s, pool);
    CHECK(std::count_if(shared.edges.begin(), shared.edges.end(),
                        [](const auto& e) { return e.encode; }) == 0);
    const auto direct = run(shared, .25f, 128);
    for (auto tap : {model::Tap::PreFx, model::Tap::PreFader, model::Tap::PostFader}) {
        model::EditTxn edit(*s, 3);
        edit.modify<model::Route>(send, [&](auto& r) {
            r.tap = tap;
            r.pan = 0;
        });
        envelope(edit, source, "mute", 0, 1);
        auto session = edit.commit();
        REQUIRE(session);
        auto a = plan(*session, pool), b = plan(*session, pool);
        CHECK(std::count_if(a.edges.begin(), a.edges.end(),
                            [](const auto& e) { return e.encode; }) == 1);
        const auto x = run(a, .25f, 128), y = run(b, .25f, 17);
        CHECK(x == y);
        CHECK(static_cast<double>(x[100]) ==
              doctest::Approx(static_cast<double>(direct[100])).epsilon(1e-6));
        if (tap == model::Tap::PostFader) {
            CHECK(x[480] > 0);
            CHECK(x[959] == 0);
            for (std::size_t i = 481; i < 960; ++i) CHECK(x[i] <= x[i - 1]);
        } else
            CHECK(x[1000] == x[100]);
    }
    model::EditTxn mode(base, 4);
    auto project = mode.project();
    project.console9 = true;
    mode.setProject(project);
    auto session = mode.commit();
    REQUIRE(session);
    auto old = plan(base, pool), next = plan(*session, pool);
    run(old, .5f, 128);
    carryMix(old, next, -1);
    CHECK(next.nodes[next.mainMaster].decoder.left == 480);
    const auto result = run(next, .5f, 127);
    for (float x : result) CHECK(std::isfinite(x));
    CHECK(next.nodes[next.mainMaster].decoder.left == 0);
    // The checker rejects a misplaced implicit stage and an unencoded summing input.
    next.nodes[next.mainMaster].decodeInput = false;
    CHECK_FALSE(checkStrips(next));
    next.nodes[next.mainMaster].decodeInput = true;
    next.nodes[next.mainMaster].postItems = false;  // a master's items at its input sum
    CHECK_FALSE(checkStrips(next));
    next.nodes[next.mainMaster].postItems = true;
    next.edges.front().tap = model::Tap::PreFader;
    CHECK_FALSE(checkStrips(next));
}

TEST_CASE("Console 9: envelope activation and reversed mode fades preserve control semantics") {
    auto pool = emptyPool();
    const auto base = monitored(false);
    auto source = [](Plan& p) -> Node& {
        return *std::find_if(p.nodes.begin(), p.nodes.end(),
                             [](const Node& n) { return n.type == model::StripType::Channel; });
    };
    auto before = plan(base, pool);
    run(before, .25f, 64, 64);
    model::EditTxn volume(base, 2);
    volume.modify<model::Track>(source(before).id, [](auto& tr) { tr.volumeDb = -12; });
    const auto changed = volume.commit();
    REQUIRE(changed);
    auto ramp = plan(*changed, pool);
    carryMix(before, ramp, -1);
    run(ramp, .25f, 64, 64);
    CHECK(source(ramp).fader.gain.left > 0);
    model::EditTxn automation(*changed, 3);
    envelope(automation, source(ramp).id, "volume", -12, -12);
    const auto automated = automation.commit();
    REQUIRE(automated);
    auto controlled = plan(*automated, pool);
    carryMix(ramp, controlled, -1);
    const auto samples = run(controlled, .25f, 64, 64);
    CHECK(static_cast<double>(samples[0]) ==
          doctest::Approx(.25 * std::pow(10.0, -12.0 / 20) * .472135955).epsilon(1e-6));
    CHECK(source(controlled).fader.gain.left == 0);

    model::EditTxn mode(base, 4);
    auto project = mode.project();
    project.console9 = true;
    mode.setProject(project);
    const auto enabled = mode.commit();
    REQUIRE(enabled);
    auto off = plan(base, pool), on = plan(*enabled, pool), back = plan(base, pool);
    run(off, .5f, 64, 64);
    carryMix(off, on, -1);
    run(on, .5f, 64, 64);
    const float last = source(on).post.l[63];
    carryMix(on, back, -1);
    run(back, .5f, 1, 1);
    CHECK(std::abs(source(back).post.l[0] - last) < 1e-4f);
    CHECK(source(back).post.l[0] >= last);
    CHECK(source(back).encoder.left == 479);
}

namespace {
using TrackEdit = std::function<void(model::Track&)>;
// A channel hearing device inputs 1 and 2 (a stereo source), its track and
// the master's edited.
model::Session stereoSource(bool console9, const TrackEdit& channel = {},
                            const TrackEdit& master = {}) {
    model::EditTxn t(model::Session::create({.console9 = console9, .seed = 777}), 1);
    const Id ch = strip(t, model::StripType::Channel);
    model::Route input;
    input.from.hwChannels = std::vector<int>{1, 2};
    input.to.track = ch;
    const Id inputId = t.create(input);
    t.modify<model::Track>(ch, [&](auto& tr) {
        tr.record.monitor = model::Monitor::On;
        tr.record.armed = true;
        tr.record.input = inputId;
        if (channel) channel(tr);
    });
    if (master) t.modify<model::Track>(kMaster, [&](auto& tr) { master(tr); });
    auto s = t.commit();
    REQUIRE(s);
    return *s;
}
// The master's output (left, right) for a constant stereo input.
std::pair<std::vector<double>, std::vector<double>> runStereo(Plan& p, float l, float r,
                                                              std::uint32_t total = 2048) {
    std::vector<float> left(p.block, l), right(p.block, r);
    std::vector<double> outL, outR;
    const float* inputs[] = {left.data(), right.data()};
    p.deviceIn = inputs;
    p.deviceInChannels = 2;
    for (std::uint32_t pos = 0; pos < total; pos += p.block) {
        REQUIRE(processBlock(p, pos, p.block));
        const auto& post = p.nodes[p.mainMaster].post;
        for (std::uint32_t i = 0; i < p.block; ++i) {
            outL.push_back(static_cast<double>(post.l[i]));
            outR.push_back(static_cast<double>(post.r[i]));
        }
    }
    p.deviceIn = nullptr;
    return {outL, outR};
}
}  // namespace

TEST_CASE("strip polarity and mono act before the fader and the encode, ramped (ADR-0022)") {
    auto pool = emptyPool();
    for (bool console9 : {false, true}) {
        CAPTURE(console9);
        auto base = plan(stereoSource(console9), pool, 128);
        const auto [l0, r0] = runStereo(base, .25f, .05f);
        REQUIRE(l0[1000] > r0[1000]);
        // Polarity: the same signal, negated (Console 9's curves are odd; its
        // dither is far below this tolerance).
        auto inverted = plan(
            stereoSource(console9, [](model::Track& ch) { ch.phaseInvert = true; }), pool, 128);
        const auto [li, ri] = runStereo(inverted, .25f, .05f);
        CHECK(li[1000] == doctest::Approx(-l0[1000]).epsilon(1e-5));
        CHECK(ri[1000] == doctest::Approx(-r0[1000]).epsilon(1e-5));
        // Mono on the channel: as if both inputs carried their mean, before the pan.
        auto mono = plan(stereoSource(console9,
                                      [](model::Track& ch) {
                                          ch.mono = true;
                                          ch.pan = 30;
                                      }),
                         pool, 128);
        auto mean = plan(stereoSource(console9, [](model::Track& ch) { ch.pan = 30; }), pool, 128);
        const auto [lm, rm] = runStereo(mono, .25f, .05f);
        const auto [le, re] = runStereo(mean, .15f, .15f);
        CHECK(lm[1000] == doctest::Approx(le[1000]).epsilon(1e-5));
        CHECK(rm[1000] == doctest::Approx(re[1000]).epsilon(1e-5));
        CHECK(rm[1000] > lm[1000]);  // the pan (right) still places it
        // Mono on the master: both outputs equal.
        auto master =
            plan(stereoSource(console9, {}, [](model::Track& m) { m.mono = true; }), pool, 128);
        const auto [lM, rM] = runStereo(master, .25f, .05f);
        CHECK(lM[1000] == doctest::Approx(rM[1000]).epsilon(1e-6));
        CHECK(lM[1000] == doctest::Approx(.5 * (l0[1000] + r0[1000])).epsilon(1e-4));
    }
    // A switch ramps over the smoothing time when the plan changes: no step.
    auto before = plan(stereoSource(false), pool, 128);
    const auto [l0, r0] = runStereo(before, .25f, .05f);
    auto after =
        plan(stereoSource(false, [](model::Track& ch) { ch.phaseInvert = true; }), pool, 128);
    carryMix(before, after, -1);
    const auto [l, r] = runStereo(after, .25f, .05f);
    const double settled = l0.back();
    CHECK(l[0] > .9 * settled);
    CHECK(l[after.smoothFrames + 10] == doctest::Approx(-settled).epsilon(1e-5));
    for (std::uint32_t i = 1; i < after.smoothFrames; ++i)
        CHECK(std::abs(l[i] - l[i - 1]) < 4.0 * settled / after.smoothFrames);
}

TEST_CASE("engine.strips-fuzz: typed graphs, all taps, keys, PDC and deterministic renders") {
    std::mt19937 random(193819);
    for (int iteration = 0; iteration < 40; ++iteration) {
        model::EditTxn t(model::Session::create({.console9 = iteration % 2 == 0, .seed = random()}),
                         1);
        const Id second = strip(t, model::StripType::Master);
        const Id third = strip(t, model::StripType::Master);
        std::vector<Id> buses{strip(t, model::StripType::Buss),
                              strip(t, model::StripType::Buss, second),
                              strip(t, model::StripType::Buss, third)};
        std::vector<Id> channels;
        for (int i = 0; i < 4; ++i) {
            const Id ch = strip(t, model::StripType::Channel, buses[random() % buses.size()]);
            channels.push_back(ch);
            model::Route input;
            input.from.hwChannels = std::vector<int>{1};
            input.to.track = ch;
            const Id inputId = t.create(input);
            t.modify<model::Track>(ch, [&](auto& tr) {
                tr.record.monitor = model::Monitor::On;
                tr.record.armed = true;  // monitoring needs the arm
                tr.record.input = inputId;
                tr.volumeDb = -static_cast<double>(random() % 20);
                tr.pan = static_cast<double>(random() % 201) - 100;
            });
            if (random() % 2) fx(t, ch, "com.suppleaudio.test.gain", true);
            model::Route r;
            r.from.track = ch;
            r.to.track = buses[random() % buses.size()];
            r.tap = static_cast<model::Tap>(random() % 3);
            r.volumeDb = -static_cast<double>(random() % 12);
            r.mono = random() % 2;
            r.phaseInvert = random() % 2;
            if (random() % 2) r.pan = static_cast<double>(random() % 201) - 100;
            const Id send = t.create(r);
            if (random() % 2) envelope(t, send, "volume", -6, -12);
            if (random() % 2) envelope(t, ch, "pan", -30, 30);
        }
        if (iteration % 2) fx(t, buses[2], "com.suppleaudio.test.gain", true);
        if (random() % 2) {  // a master's own item with take FX (ADR-0057)
            model::FxInstance f;
            f.plugin.id = "com.suppleaudio.test.gain";
            model::Take take;
            take.chain = {t.create(f)};
            const Id tk = t.create(take);
            model::Item it;
            it.track = random() % 2 ? kMaster : second;
            it.length = 4;
            it.takes = {tk};
            it.activeTake = tk;
            t.create(it);
        }
        const Id key = fx(t, buses[2], "com.suppleaudio.test.keymix");
        model::Route route;
        route.from.track = iteration % 2 ? kMaster : channels[0];
        route.to.fx = key;
        route.tap = static_cast<model::Tap>(iteration % 3);
        t.create(route);
        auto session = t.commit();
        REQUIRE(session);
        auto pool = fixturePool();
        auto p = plan(*session, pool);
        // The fixture's delay line is active only between start/stopProcessing.
        for (auto* instance : p.instances) REQUIRE(instance->startProcessing());
        const auto a = run(p, .01f, 128);
        for (float v : a) REQUIRE(std::isfinite(v));
        for (auto* instance : p.instances) instance->stopProcessing();
        auto pool2 = fixturePool();
        auto q = plan(*session, pool2);
        for (auto* instance : q.instances) REQUIRE(instance->startProcessing());
        CHECK(run(q, .01f, 31) == a);
        for (auto* instance : q.instances) instance->stopProcessing();
    }
}
