// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A held track's audio items under a master playrate (ADR-0097, S3): while
// a launched clip holds a track, its items' stretch converters keep
// converting, on a pitch-keeping plan and through a switch to tape's
// crossfade. Split from test_clip_player_rate.cpp so each entry stays well
// under its limit under ThreadSanitizer (candidate batch-c's run-all).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("under a pitch-keeping rate a track's audio items come back after a MIDI clip held it" *
          doctest::description("rt")) {
    // While a launched clip holds a track its arrangement is silent, and its
    // audio items were not read at all; a stretch converter then fell behind
    // the song and refused the jump when the arrangement came back, failing
    // every block after (process errors, and the replay refused). tr_2's
    // item (5 s from bar 1) is held by a MIDI clip from bar 2 to bar 3.
    Project p;
    twoTracks(p);
    p.run("scene.add", "{}");  // sc_2
    const std::string midi = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                               R"("notation": "voice v; 1: C4:w"})")["clip"]
                                 .asString();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    Engine::LaunchRequest notes = request("tr_2", "cl_2");
    notes.clip = id(midi.c_str());
    Replays replays;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        constexpr std::uint32_t block = 128;
        const std::vector<Step> steps{{20, {notes}}, {(128000 + 4000) / block, {backTo("tr_2")}}};
        const Live live = playLive(s, p.app->projectDir(), options, block, 420000 + 16384, steps);
        REQUIRE(live.log.size() == 2);
        CHECK(live.log[1].runningFrame == 256000);  // bar 3
        liveIsReplay(s, p.app->projectDir(), live, 9.0, 420000, replays);
    }
}

TEST_CASE("after a switch to tape a held track's items come back through the crossfade" *
          doctest::description("rt")) {
    // Preserve pitch turned off at 0.75: the new plan reads by tape, and its
    // items read the old stretch converters (RateClipState::previous) until
    // the switch and through its crossfade. A clip holds tr_2 over the swap
    // and returns to the arrangement at bar 3; the edit's loads step over
    // the frames where its switch lands just before the return (a skipped
    // old converter then refused the jump: process errors from the return
    // on, at 9984 and 10112 frames before bar 3 in the callback executor
    // and 10240 in the anticipative one when it was found; the steps reach
    // some 770 frames past those either way, should placement move them).
    Project p;
    twoTracks(p);
    p.run("scene.add", "{}");  // sc_2
    const std::string midi = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                               R"("notation": "voice v; 1: C4:w"})")["clip"]
                                 .asString();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session kept = p.app->head();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    const model::Session tape = p.app->head();
    Engine::LaunchRequest notes = request("tr_2", "cl_2");
    notes.clip = id(midi.c_str());
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}})
        for (std::int64_t lead = 9216; lead <= 11008; lead += 128) {
            INFO("anticipative " << options.anticipative << ", load " << lead
                                 << " frames before bar 3");
            constexpr std::uint32_t block = 128;
            const std::vector<Step> steps{{20, {notes}},
                                          {(128000 + 4000) / block, {backTo("tr_2")}},
                                          {(256000 - lead) / block, {}, {}, &tape}};
            const Live live = playLive(kept, p.app->projectDir(), options, block, 300000, steps);
            REQUIRE_MESSAGE(live.log.size() == 2, describe(live.log) << live.report);
            CHECK(live.log[1].runningFrame == 256000);  // processErrors() == 0 in playLive
            // The item plays on after the return (its 5 s run to song 240000).
            const auto at = static_cast<std::size_t>(256000 + live.latency + 4800);
            float peak = 0;
            for (std::size_t i = at; i < at + 4800 && i < live.left.size(); ++i)
                peak = std::max(peak, std::abs(live.left[i]));
            CHECK(peak > 0.01f);
        }
}
