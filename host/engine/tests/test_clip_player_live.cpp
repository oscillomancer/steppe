// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's players live against the offline replay of their log
// (ADR-0097): both executors, block sizes, the bridge, take chains. Split
// from test_clip_player.cpp so each runs as its own ctest entry, in time
// under the sanitizers.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("live launches play as the offline replay of their log, in both executors" *
          doctest::description("rt")) {
    // A scene launch (both tracks, one target) on bar 2, then tr_1 back to
    // the arrangement and tr_2 stopped on bar 3.
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("looped " << bool(loop) << ", block " << block << ", anticipative "
                               << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(96000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live =
                    playLive(s, p.app->projectDir(), options, block, frames, steps, loop);
                REQUIRE(live.log.size() == 4);
                CHECK(live.log[0].anchorSample == 96000);  // bar 2
                CHECK(live.log[0].runningFrame == 96000);
                CHECK(live.log[1].anchorSample == 96000);  // one target for the scene
                // Bar 3, or bar 2 again after the wrap at 3 s.
                CHECK(live.log[2].anchorSample == (loop ? 96000 : 192000));
                CHECK(live.log[2].runningFrame == 192000);
                InstancePool pool = makePool();
                OfflineSettings settings;
                settings.endSeconds = 5.0;
                settings.projectDir = p.app->projectDir();
                settings.launches = live.log;
                settings.loop = loop;
                auto offline = renderOffline(s, pool, settings);
                REQUIRE(offline);
                CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 5) ==
                      0.0);
                // The launches changed the output: equality says something.
                settings.launches.clear();
                auto plain = renderOffline(s, pool, settings);
                REQUIRE(plain);
                CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 5) > 0.0);
            }
}

TEST_CASE("a replay from a play start past 0 places the launches as live, looped or not" *
          doctest::description("rt")) {
    // Round 74 F1: the log's anchors are timeline samples, and the replay's
    // plan counts from its start. Played from 2 s, with the loop at 1-3 s
    // (the start after the loop's start) and without it.
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 4 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("looped " << bool(loop) << ", anticipative " << options.anticipative);
            constexpr std::uint32_t block = 128;
            Engine::LaunchRequest stop = backTo("tr_2");
            stop.action = LaunchLogEntry::Action::Stop;
            const std::vector<Step> steps{{20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                                          {(96000 + 4000) / block, {backTo("tr_1"), stop}}};
            const Live live =
                playLive(s, p.app->projectDir(), options, block, frames, steps, loop, 2.0);
            REQUIRE(live.log.size() == 4);
            // Bar 2 after the wrap at 3 s, or bar 3 (4 s) without the loop.
            CHECK(live.log[0].anchorSample == (loop ? 96000 : 192000));
            CHECK(live.log[0].runningFrame == 96000);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.startSeconds = 2.0;
            settings.endSeconds = 6.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            settings.loop = loop;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 4) == 0.0);
            settings.launches.clear();
            auto plain = renderOffline(s, pool, settings);
            REQUIRE(plain);
            CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 4) > 0.0);
        }
}

TEST_CASE("live launches through the bridge play as the offline replay" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    Project p;
    twoTracks(p);
    p.run("fx.set", R"({"fx": "fx_1", "isolation": "shared_bridge"})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 4 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("looped " << bool(loop) << ", anticipative " << options.anticipative);
            const std::vector<Step> steps{{10, {request("tr_1", "cl_1")}}};
            const Live live = playLive(s, p.app->projectDir(), engine::test::patient(options), 128,
                                       frames, steps, loop);
            REQUIRE(live.log.size() == 1);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 4.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            settings.loop = loop;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 4) == 0.0);
        }
}

TEST_CASE("a clip chain through the bridge plays as the offline replay" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    Project p;
    latentClip(p);
    p.run("fx.set", R"({"fx": "fx_1", "isolation": "shared_bridge"})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 4 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("looped " << bool(loop) << ", anticipative " << options.anticipative);
            const std::vector<Step> steps{{10, {request("tr_1", "cl_1")}}};
            const Live live = playLive(s, p.app->projectDir(), engine::test::patient(options), 128,
                                       frames, steps, loop);
            REQUIRE(live.log.size() == 1);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 4.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            settings.loop = loop;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 4) == 0.0);
            // The clip plays from bar 2, through its chain: a ramp at -6 dB.
            CHECK(double(live.left[live.latency + 96000 + 5000]) ==
                  doctest::Approx(chained(5000)).epsilon(1e-5));
        }
}

TEST_CASE("live launches of clips with take chains play as the offline replay" *
          doctest::description("rt")) {
    // An audio clip through a latent chain on tr_1, a MIDI clip through an
    // instrument and a latent gain on tr_2; launched, relaunched, swapped,
    // back to the arrangement and stopped.
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
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("looped " << bool(loop) << ", block " << block << ", anticipative "
                               << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                // Looped, the last two are decided after the wrap at 3 s (song
                // 1.08 s), before their anchor on bar 2 of the second pass.
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(96000 + 4000) / block,
                     {request("tr_1", "cl_1")},
                     model::LaunchQuantize::Beat},
                    {(144000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live =
                    playLive(s, p.app->projectDir(), options, block, frames, steps, loop);
                REQUIRE(live.log.size() == 5);
                if (loop) {
                    CHECK(live.log[3].anchorSample == 96000);
                    CHECK(live.log[3].runningFrame == 192000);
                }
                InstancePool pool = makePool();
                OfflineSettings settings;
                settings.endSeconds = 5.0;
                settings.projectDir = p.app->projectDir();
                settings.launches = live.log;
                settings.loop = loop;
                auto offline = renderOffline(s, pool, settings);
                REQUIRE(offline);
                // Said only on a failure (macOS once differed here, intermittently).
                INFO(differences(live.left, live.latency, offline->audio.channels[0], 48000 * 5));
                INFO("live steps and counters:" << live.report);
                INFO("replay log:" << describe(live.log));
                INFO("offline warnings:" << joined(offline->warnings));
                INFO("media at the compare:" << mediaPresence(s, p.app->projectDir()));
                CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 5) ==
                      0.0);
                settings.launches.clear();
                auto plain = renderOffline(s, pool, settings);
                REQUIRE(plain);
                CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 5) > 0.0);
            }
}
