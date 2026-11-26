// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes into a clip's take chain (ADR-0096 step M5; engine.md
// §1.4.1, "Clip chains: two switches"): the incoming clip's chain is entered
// one grid interval before its input reaches the anchor, so a route moves it
// from its first frame at any block size, and that early entry changes
// nothing else: the input switch stays where it was (review round 188 G1).
#include <cmath>

#include "clip_player_support.h"
#include "supple/params/params.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {

// rk_1 on tr_1 (its Out uncabled, silent): Transport's `playing`, 1 while
// playing, into the Mod Out mo_2's in_1.
void modulators(Project& p) {
    p.run("rack.add", R"({"track": "tr_1"})");                        // rk_1, mo_1 its Out
    p.run("module.add", R"({"rack": "rk_1", "type": "mod_out"})");    // mo_2
    p.run("module.add", R"({"rack": "rk_1", "type": "transport"})");  // mo_3
    p.run("cable.add", R"({"from": "mo_3.playing", "to": "mo_2.in_1"})");
}

// A plug-in added to `clip`'s take chain; its id.
std::string takeFx(Project& p, const char* clip, const char* plugin) {
    json::Value fx = json::Value::object();
    fx["take"] = takeOf(p, clip);
    fx["plugin"] = plugin;
    return p.runArgs("fx.add", fx)["fx"].asString();
}

// A route from mo_2.in_1 into `fx`'s gain; its id.
std::string route(Project& p, const std::string& fx, double depth, bool enabled = true) {
    json::Value add = j(R"({"source": {"module": "mo_2", "input": "in_1"}})");
    add["target"] = json::Value::object();
    add["target"]["fx"] = fx;
    add["target"]["param"] = "gain";
    add["depth"] = depth;
    const std::string id = p.runArgs("modulation.add", add)["modulation"].asString();
    if (!enabled) {
        json::Value off = json::Value::object();
        off["modulation"] = id;
        off["enabled"] = false;
        p.runArgs("modulation.set", off);
    }
    return id;
}

void removeRoute(Project& p, const std::string& id) {
    json::Value args = json::Value::object();
    args["modulation"] = id;
    p.runArgs("modulation.remove", args);
}

// The gain fixture's level at -6 dB moved by `clapOffset` (ADR-0010), as
// test_modulation_engine.cpp computes it.
double gainAt(double clapOffset) {
    params::ParamSpec spec;
    spec.type = params::Type::Float;
    spec.unit = units::Unit::dB;
    spec.min = -60;
    spec.max = 12;
    const double db =
        params::fromClap(spec, std::clamp(params::toClap(spec, -6) + clapOffset, 0.0, 1.0));
    return std::pow(10.0, db / 20.0);
}

}  // namespace

TEST_CASE("a modulated clip chain: a MIDI clip's switch ends its notes where it did (M5, G1)") {
    // Two MIDI clips, each playing through a sine and a gain in its take
    // chain; a route (disabled: it moves nothing) into the second's gain
    // makes tr_1 a node with a modulated clip chain. The switch from the
    // first to the second must end the first's notes at the switch, not a
    // grid interval before it: the output equals the same launches with no
    // route at all, at any block size.
    Project p;
    modulators(p);
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice a; 1: C3:w"})");
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "notation": "voice b; 1: E3:w"})");
    std::string gain;
    for (const char* clip : {"cl_1", "cl_2"}) {
        takeFx(p, clip, "com.suppleaudio.test.sine");
        gain = takeFx(p, clip, "com.suppleaudio.test.gain");  // cl_2's last
    }
    const std::vector<LaunchLogEntry> log = {launch(kBeat + 5, "cl_1"),
                                             launch(3 * kBeat + 11, "cl_2"),
                                             back(5 * kBeat, LaunchLogEntry::Action::Stop)};
    const std::string r = route(p, gain, 0.3, false);
    std::vector<std::vector<float>> routed;
    for (const std::uint32_t block : {37u, 128u, 512u}) routed.push_back(p.render(3.0, log, block));
    removeRoute(p, r);
    std::size_t k = 0;
    for (const std::uint32_t block : {37u, 128u, 512u}) {
        INFO("block " << block);
        const auto plain = p.render(3.0, log, block);
        CHECK(plain[std::size_t(3 * kBeat + 10)] != 0.0f);  // cl_1 sounds to the switch
        CHECK(routed[k] == plain);
        ++k;
    }
}

TEST_CASE("a modulated clip chain: relaunching the playing audio clip leaves no gap (M5, G1)") {
    // The playing clip relaunched: its chain is already active and is not
    // entered again, and its input runs to the switch, so the pass before
    // it is heard to its last frame, as with no route.
    Project p;
    modulators(p);
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("ramp.wav", 96000).string();
    p.runArgs("clip.add", args);  // cl_1
    const std::string gain = takeFx(p, "cl_1", "com.suppleaudio.test.gain");
    const std::vector<LaunchLogEntry> log = {launch(kBeat, "cl_1"), launch(2 * kBeat + 77, "cl_1")};
    const std::string r = route(p, gain, 0.3, false);
    std::vector<std::vector<float>> routed;
    for (const std::uint32_t block : {37u, 128u, 512u}) routed.push_back(p.render(2.0, log, block));
    removeRoute(p, r);
    std::size_t k = 0;
    for (const std::uint32_t block : {37u, 128u, 512u}) {
        INFO("block " << block);
        const auto plain = p.render(2.0, log, block);
        // The first pass to the frame before the relaunch (its crossfade
        // into the new pass starts at the switch).
        for (std::int64_t t = 2 * kBeat + 77 - 40; t < 2 * kBeat + 77; ++t)
            REQUIRE(routed[k][std::size_t(t)] == rampAt(t - kBeat));
        CHECK(routed[k] == plain);
        ++k;
    }
}

TEST_CASE("a route into a clip's take FX moves it from the launch's first frame (M5, F1)") {
    // A clip of constant 0.5 through the gain at -6 dB, launched off the
    // 32-frame grid: the route's offset (Transport playing x -0.25) is on
    // the plug-in from the anchor itself, at every block size, because the
    // chain was entered at the grid point before it. The launch fades in
    // over 5 ms, so its first frames are compared with the same launch
    // unrouted: the ratio is the route's from the first frame that sounds.
    Project p;
    modulators(p);
    audio::AudioData half;
    half.sampleRate = 48000;
    half.channels.assign(1, std::vector<float>(96000, 0.5f));
    const fs::path file = p.root / "half.wav";
    REQUIRE(audio::writeWav(file, half));
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = file.string();
    p.runArgs("clip.add", args);  // cl_1
    const std::string gain = takeFx(p, "cl_1", "com.suppleaudio.test.gain");
    json::Value level = json::Value::object();
    level["fx"] = gain;
    level["values"] = j(R"({"gain": -6})");
    p.runArgs("fx.param.set", level);
    constexpr std::int64_t kAt = kBeat + 7;
    const std::vector<LaunchLogEntry> log = {launch(kAt, "cl_1")};
    const auto plain = p.render(1.0, log, 512);
    route(p, gain, -0.25);
    const auto at512 = p.render(1.0, log, 512);
    CHECK(at512[std::size_t(kAt - 1)] == 0.0f);
    const double ratio = gainAt(-0.25) / gainAt(0);
    for (std::int64_t t = kAt + 1; t <= kAt + 240; ++t) {
        INFO("frame " << t);
        REQUIRE(plain[std::size_t(t)] > 0.0f);
        REQUIRE(double(at512[std::size_t(t)]) ==
                doctest::Approx(double(plain[std::size_t(t)]) * ratio).epsilon(1e-4));
    }
    CHECK(double(at512[std::size_t(kAt + 3000)]) ==
          doctest::Approx(0.5 * gainAt(-0.25)).epsilon(1e-4));
    for (const std::uint32_t block : {37u, 128u}) {
        INFO("block " << block);
        CHECK(p.render(1.0, log, block) == at512);
    }
}

// --- The live launch path (review round 192 F1 a) -------------------------------------

namespace {

// tr_1's launches at 113 BPM, so a bar (101946.9 frames) and the anchors
// quantized to it fall off the 32-frame grid.
void offGrid(Project& p) { p.run("tempo.set", R"({"points": [{"beat": 0, "bpm": 113}]})"); }

// The whole mix offline, replaying `log`.
std::vector<float> mixOf(Project& p, const model::Session& s, double seconds,
                         const std::vector<LaunchLogEntry>& log, std::uint32_t block) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.blockSize = block;
    settings.projectDir = p.app->projectDir();
    settings.launches = log;
    auto r = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// A clip of constant 0.5 on tr_1 through the gain at -6 dB, a route from
// Transport's `playing` (x depth) on that gain.
std::string halfClip(Project& p, const char* scene, double depth) {
    audio::AudioData half;
    half.sampleRate = 48000;
    half.channels.assign(1, std::vector<float>(96000, 0.5f));
    const fs::path file = p.root / (std::string("half-") + scene + ".wav");
    REQUIRE(audio::writeWav(file, half));
    auto args = j(R"({"track": "tr_1"})");
    args["scene"] = scene;
    args["audio_file"] = file.string();
    const std::string clip = p.runArgs("clip.add", args)["clip"].asString();
    const std::string gain = takeFx(p, clip.c_str(), "com.suppleaudio.test.gain");
    json::Value level = json::Value::object();
    level["fx"] = gain;
    level["values"] = j(R"({"gain": -6})");
    p.runArgs("fx.param.set", level);
    route(p, gain, depth);
    return clip;
}

}  // namespace

TEST_CASE("a modulated clip chain launched live plays as the replay of its log (M5, F1 a)" *
          doctest::description("rt")) {
    // An audio clip launched live on an off-grid bar and relaunched a bar
    // later: the executors deliver the chain's entry a grid interval before
    // the switch (launch_runtime.cpp, preenter), and the replay of their own
    // log has it at the same frame, so real time equals the offline render,
    // in both executors, at 128 and 512.
    Project p;
    modulators(p);
    offGrid(p);
    halfClip(p, "sc_1", -0.25);  // cl_1, mr_1
    const model::Session s = p.app->head();
    p.run("modulation.set", R"({"modulation": "mr_1", "enabled": false})");
    const model::Session unrouted = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const std::uint32_t block : {128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("block " << block << ", anticipative " << options.anticipative);
            const Live live =
                playLive(s, p.app->projectDir(), options, block, frames,
                         {{4000 / block, {request("tr_1", "cl_1")}},
                          {(101947 + 50000) / std::int64_t(block), {request("tr_1", "cl_1")}}});
            INFO(live.report);
            REQUIRE(live.log.size() == 2);
            CHECK(live.log[0].anchorSample % 32 != 0);
            const auto offline = mixOf(p, s, 5.0, live.log, block);
            CHECK(maxDiff(live.left, live.latency, offline, 48000 * 5) == 0.0);
            // The route moves it from the launch's anchor.
            const auto at = std::size_t(live.log[0].anchorSample + 1000);
            CHECK(offline[at] != mixOf(p, unrouted, 5.0, live.log, block)[at]);
        }
}

TEST_CASE("a modulated MIDI clip switch plays live as the replay of its log (M5, F1 a, G1)" *
          doctest::description("rt")) {
    // cl_1 to cl_2, each through a sine and a gain, a route on cl_2's gain:
    // the switch keeps its two moments live (the entry, then the input) as
    // the replay does. At 112 BPM bars 2 and 3 (102857, 205714) are off the
    // block grid, and each clip's first note sounds (at 113 BPM their
    // anchors round up, and until round 195's first-note fix on main the
    // notes were dropped: the replay compared silence with silence). Red
    // without the live preenter (review round 195, probe G).
    Project p;
    modulators(p);
    p.run("tempo.set", R"({"points": [{"beat": 0, "bpm": 112}]})");
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice a; 1: C3:w"})");
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "notation": "voice b; 1: E3:w"})");
    std::string gain;
    for (const char* clip : {"cl_1", "cl_2"}) {
        takeFx(p, clip, "com.suppleaudio.test.sine");
        gain = takeFx(p, clip, "com.suppleaudio.test.gain");
    }
    route(p, gain, 0.3);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const std::uint32_t block : {128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("block " << block << ", anticipative " << options.anticipative);
            const Live live =
                playLive(s, p.app->projectDir(), options, block, frames,
                         {{4000 / block, {request("tr_1", "cl_1")}},
                          {(102857 + 50000) / std::int64_t(block), {request("tr_1", "cl_2")}}});
            INFO(live.report);
            REQUIRE(live.log.size() == 2);
            CHECK(live.log[1].anchorSample % 32 != 0);  // off the grid
            const auto replay = mixOf(p, s, 5.0, live.log, block);
            double peak = 0;  // not silence against silence
            for (std::size_t f = std::size_t(live.log[1].runningFrame); f < replay.size(); ++f)
                peak = std::max(peak, std::fabs(double(replay[f])));
            CHECK(peak > 0.01);
            CHECK(maxDiff(live.left, live.latency, replay, 48000 * 5) == 0.0);
        }
}

TEST_CASE("a render without launches keeps a node's item-chain route live (M5, F5)") {
    // tr_1 holds an item and a clip, each through a modulated gain. A render
    // without launches drops the clip chains (dropClipChains) and leaves the
    // clip's route unplayed; the item's route still moves the item, with no
    // inert warning.
    Project p;
    modulators(p);
    halfClip(p, "sc_1", 0.3);  // cl_1, its route mr_1
    auto item = j(R"({"track": "tr_1", "bar": 1})");
    item["audio_file"] = (p.root / "half-sc_1.wav").string();
    const auto added = p.runArgs("item.add", item);
    json::Value fx = json::Value::object();
    fx["take"] = added.find("take")->asString();
    fx["plugin"] = "com.suppleaudio.test.gain";
    const std::string gain = p.runArgs("fx.add", fx)["fx"].asString();
    const std::string itemRoute = route(p, gain, -0.25);
    const auto rendered = [&](const model::Session& s) {
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 1.0;
        settings.projectDir = p.app->projectDir();
        auto r = renderOffline(s, pool, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::move(*r);
    };
    const auto live = rendered(p.app->head());
    for (const std::string& w : live.warnings) {
        INFO(w);
        CHECK(w.find(itemRoute) == std::string::npos);
    }
    json::Value off = json::Value::object();
    off["modulation"] = itemRoute;
    off["enabled"] = false;
    p.runArgs("modulation.set", off);
    CHECK(rendered(p.app->head()).audio.channels[0][24000] != live.audio.channels[0][24000]);
}
