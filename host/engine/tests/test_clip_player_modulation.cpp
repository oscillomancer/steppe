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
