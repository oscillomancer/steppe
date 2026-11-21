// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launches of clips with take chains, and a rack's Notes module, under a
// master playrate (ADR-0097, dated revision 2026-11-21; S3), live as the
// offline replay. Split from test_clip_player_rate.cpp so each runs as its
// own ctest entry, in time under ThreadSanitizer.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("launches of clips with take chains under a master playrate play as the replay" *
          doctest::description("rt")) {
    // As the unity test: an audio clip through a latent chain on tr_1, a
    // MIDI clip through an instrument and a latent gain on tr_2.
    for (const bool envelope : {false, true}) {
        Project p;
        latentClip(p);
        p.run("track.add", R"({"name": "Keys"})");  // tr_2
        p.run("clip.add",
              R"({"track": "tr_2", "scene": "sc_1", "notation": "voice bass; 1: C3:q E3:q G3:h"})");
        auto fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");
        fx["take"] = takeOf(p, "cl_2");
        p.runArgs("fx.add", fx);
        auto gain = j(R"({"plugin": "com.suppleaudio.test.gain"})");
        gain["take"] = takeOf(p, "cl_2");
        const std::string fxId = p.runArgs("fx.add", gain)["fx"].asString();
        auto patch = j(R"({"patch": {"params": {"latency": "short"}}})");
        patch["fx"] = fxId;
        p.runArgs("fx.patch.merge", patch);
        auto args = j(R"({"track": "tr_1", "bar": 1})");
        args["audio_file"] = p.ramp("item.wav", 240000).string();
        p.runArgs("item.add", args);
        underRate(p, envelope);
        const model::Session s = p.app->head();
        // What is compared (300000 frames, past the last switch at 256000)
        // and room for the output latency: no more is played.
        const std::int64_t frames = 300000 + 16384;
        Replays replays;
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                // Both block sizes under the constant rate; the envelope, whose
                // trajectory is what it adds, at one.
                if (envelope && block != 128) continue;
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(128000 + 4000) / block,
                     {request("tr_1", "cl_1")},
                     model::LaunchQuantize::Beat},
                    {(192000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
                REQUIRE(live.log.size() == 5);
                liveIsReplay(s, p.app->projectDir(), live, 5.0, 300000, replays);
            }
    }
}

TEST_CASE("a rack's Notes module follows launches under a master playrate as the replay" *
          doctest::description("rt")) {
    Project p;
    rackFollows(p, false);
    underRate(p, false);
    const model::Session s = p.app->head();
    const std::int64_t frames = 380000 + 16384;  // what is compared, and the latency
    Replays replays;
    const auto plain = gates(p, 6.5, {});  // the same for both runs
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Engine::LaunchRequest stop = backTo("tr_1");
        stop.action = LaunchLogEntry::Action::Stop;
        const std::vector<Step> steps{{20, {request("tr_1", "cl_1")}},
                                      {(128000 + 4000) / 128, {stop}},
                                      {(256000 + 4000) / 128, {backTo("tr_1")}}};
        const Live live = playLive(s, p.app->projectDir(), options, 128, frames, steps);
        REQUIRE(live.log.size() == 3);
        CHECK(live.log[2].anchorSample == 288000);
        CHECK(live.log[2].runningFrame == 384000);
        liveIsReplay(s, p.app->projectDir(), live, 6.5, 380000, replays);
        // The gate follows the clip, at song times over 0.75 in device time.
        const auto g = gates(p, 6.5, live.log);
        const auto at = [](const std::vector<float>& v, double song) {
            return v[std::size_t(song / 0.75 * 48000)];
        };
        CHECK(at(g, 2.1) == 1.0f);   // the clip's first eighth note
        CHECK(at(g, 2.35) == 0.0f);  // its rest: the arrangement's A3 passes unplayed
        CHECK(at(plain, 2.35) == 1.0f);
        CHECK(at(g, 2.6) == 1.0f);  // its second eighth
        CHECK(at(g, 4.5) == 0.0f);  // stopped
        CHECK(at(g, 6.3) == 1.0f);  // the arrangement again
    }
}
