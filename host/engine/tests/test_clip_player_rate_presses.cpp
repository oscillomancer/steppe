// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Presses during an audio launch's warm-up under a pitch-keeping master
// playrate (S3 step D): each lands after the warm-up's switch and plays as
// the offline replay. Split from test_clip_player_rate_stretch.cpp, which
// ran 573 s under ThreadSanitizer at -j 2 with them.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a press during an audio launch's warm-up lands after its switch, and wins" *
          doctest::description("rt")) {
    // Round 139 S1: cl_2 (audio) is applied from 2944 frames before bar 2.
    // A stop, a return or a MIDI clip pressed inside that warm-up would land
    // before its switch (unquantized) or on the same frame (on Bar), and the
    // earlier audio launch would then play over the later press. Each lands
    // after the switch instead (unquantized a frame after at least, on Bar
    // on bar 3), and the track ends on it; live is the replay.
    Project p;
    twoTracks(p);
    p.run("scene.add", "{}");  // sc_2
    const std::string midi = p.run("clip.add", R"({"track": "tr_2", "scene": "sc_2", )"
                                               R"("notation": "voice v; 1: C4:w"})")["clip"]
                                 .asString();
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    Engine::LaunchRequest stop = backTo("tr_2");
    stop.action = LaunchLogEntry::Action::Stop;
    Engine::LaunchRequest notes = request("tr_2", "cl_2");
    notes.clip = id(midi.c_str());
    const std::pair<const char*, Engine::LaunchRequest> presses[] = {
        {"stop", stop}, {"return", backTo("tr_2")}, {"MIDI clip", notes}};
    Replays replays;
    for (const auto& [name, later] : presses)
        for (const bool quantized : {false, true})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                const std::string what = name;
                INFO(what << ", quantized " << quantized << ", anticipative "
                          << options.anticipative);
                constexpr std::uint32_t block = 128;
                const std::vector<Step> steps = serviced(
                    {{20, {request("tr_2", "cl_2")}},
                     {(128000 - 1000) / block,
                      {later},
                      quantized ? model::LaunchQuantize::Bar : model::LaunchQuantize::None}},
                    128000, 290000, block);
                const Live live =
                    playLive(s, p.app->projectDir(), options, block, 300000 + 16384, steps);
                REQUIRE_MESSAGE(live.log.size() == 2, describe(live.log) << live.report);
                CHECK(live.log[0].clip == id("cl_2"));
                CHECK(live.log[0].runningFrame == 128000);
                CHECK(live.log[1].action == later.action);
                if (later.action == LaunchLogEntry::Action::Clip)
                    CHECK(live.log[1].clip == notes.clip);
                if (quantized) {
                    CHECK(live.log[1].runningFrame == 256000);  // bar 3
                } else {
                    CHECK(live.log[1].runningFrame > 128000);
                }
                liveIsReplay(s, p.app->projectDir(), live, 7.0, 300000, replays);
            }
}

TEST_CASE("a launch made during another's warm-up lands after it, and plays as the replay" *
          doctest::description("rt")) {
    // S3 step D: from launchWarm before its switch a launch is applied (its
    // converter warming on reader 2), so a later launch on the track does
    // not replace it. Reader 2 warms one launch at a time: a launch made
    // inside that warm-up is placed no earlier than launchWarm after the
    // first's switch, so the first still comes in warm. Unquantized it would
    // otherwise land within it (about 127000 + 128 + 2944). Then cl_2 again
    // on bar 4. Live is the replay throughout.
    Project p;
    twoTracks(p);
    auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("other.wav", 96000).string();
    p.runArgs("clip.add", args);  // cl_3
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 460000 + 16384;
    constexpr std::int64_t kWarm = 2944;  // at 48 kHz (the plan-level case above)
    Replays replays;
    for (const std::uint32_t block : {32u, 128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            // Blocks under a quantum (round 139) in the callback executor.
            if (block == 32 && options.anticipative) continue;
            INFO("block " << block << ", anticipative " << options.anticipative);
            const std::vector<Step> steps = serviced(
                {{2560 / block, {request("tr_2", "cl_2")}},
                 {(128000 - 1000) / block, {request("tr_2", "cl_3")}, model::LaunchQuantize::None},
                 {(256000 + 4000) / block, {request("tr_2", "cl_2")}}},
                128000, 250000, block);
            const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
            REQUIRE_MESSAGE(live.log.size() == 3, describe(live.log) << live.report);
            CHECK(live.log[0].runningFrame == 128000);
            CHECK(live.log[1].clip == id("cl_3"));
            CHECK(live.log[1].runningFrame >= 128000 + kWarm);
            CHECK(live.log[2].runningFrame == 384000);  // bar 4
            liveIsReplay(s, p.app->projectDir(), live, 10.0, 460000, replays);
        }
}
