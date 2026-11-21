// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A launched clip following the project's key and chords live (ADR-0098
// X4): as placement lays it, a note's pitch from its start, channel 10 as
// written, live as the replay, and plan swaps mid-note. Its own ctest entry,
// as the other test_clip_player files.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

// --- A launched clip follows live (ADR-0098 X4) -----------------------------

namespace {
// Project context: C minor for bars 1-2, E minor from bar 3, with a chord a bar.
constexpr const char* kTwoKeys =
    "1: key C minor {Cm}:w\n2: {Fm}:w\n3: key E minor {Em}:w\n4: {B7}:w\n5: {Em}:w";
json::Value contextText(const char* text) {
    json::Value args = json::Value::object();
    args["text"] = text;
    return args;
}
// A sine track (tr_1) whose bar-long clip cl_1, written in D minor, has `follow`.
void followingClip(Project& p, const char* follow, const char* notation,
                   const char* clipContext = "1: key D minor {Dm}:h {Gm}:h") {
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    p.runArgs("context.write", contextText(kTwoKeys));
    json::Value clip = j(R"({"track": "tr_1", "scene": "sc_1"})");
    clip["notation"] = notation;
    clip["context"] = clipContext;
    clip["follow"] = follow;
    p.runArgs("clip.add", clip);
}
bool sameFrom(const std::vector<float>& a, const std::vector<float>& b, std::int64_t from,
              std::int64_t to) {
    REQUIRE(a.size() >= std::size_t(to));
    REQUIRE(b.size() >= std::size_t(to));
    for (std::int64_t t = from; t < to; ++t)
        if (a[std::size_t(t)] != b[std::size_t(t)]) return false;
    return true;
}
}  // namespace

TEST_CASE("a launched clip follows the project's key and chords as placement lays it") {
    // Launched on bar 2 for two bars, cl_1 plays a pass in C minor and one in
    // E minor; laid on bars 2 and 3 by item.add {clip}, placement maps the
    // same notes (X2). The renders agree past the switch's fade.
    for (const char* follow : {"key", "chords"}) {
        CAPTURE(follow);
        Project p;
        followingClip(p, follow, "voice bass; 1: D3:q F3:q A3:q Bb3:q");
        const auto launched = p.render(
            7.0, {launch(4 * kBeat, "cl_1"), back(12 * kBeat, LaunchLogEntry::Action::Stop)});
        p.run("item.add", R"({"track": "tr_1", "bar": 2, "clip": "cl_1"})");
        p.run("item.add", R"({"track": "tr_1", "bar": 3, "clip": "cl_1"})");
        const auto laid = p.render(7.0, {});
        CHECK(sameFrom(launched, laid, 4 * kBeat + kFade, 12 * kBeat));
        // It moved: as written, the launch sounds otherwise.
        p.run("clip.set", R"({"clip": "cl_1", "follow": "none"})");
        const auto written = p.render(
            7.0, {launch(4 * kBeat, "cl_1"), back(12 * kBeat, LaunchLogEntry::Action::Stop)});
        CHECK_FALSE(sameFrom(launched, written, 4 * kBeat + kFade, 12 * kBeat));
    }
}

TEST_CASE("a following clip's note keeps the pitch of its start across a key change") {
    // A whole note from bar 2 crosses nothing in the clip but plays into
    // bar 3's E minor: it keeps C minor's pitch (C3) to its note-off, and
    // nothing sounds after the stop.
    Project p;
    followingClip(p, "key", "voice bass; 1: D3:w", "1: key D minor ~:w");
    p.runArgs("context.write", contextText("1: key C minor ~:w\n2: ~:h key E minor ~:h\n3: ~:w"));
    const auto followed =
        p.render(6.0, {launch(4 * kBeat, "cl_1"), back(8 * kBeat, LaunchLogEntry::Action::Stop)});
    Project q;
    q.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    q.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:w"})");
    const auto written =
        q.render(6.0, {launch(4 * kBeat, "cl_1"), back(8 * kBeat, LaunchLogEntry::Action::Stop)});
    CHECK(sameFrom(followed, written, 4 * kBeat, 8 * kBeat + kFade));
    for (std::int64_t t = 8 * kBeat + kFade; t < 11 * kBeat; ++t)
        REQUIRE(followed[std::size_t(t)] == 0.0f);
}

TEST_CASE("a following clip's channel-10 notes play as written") {
    Project p;
    followingClip(p, "key", "voice drums ch 10; 1: C2:q D2:q E2:q F2:q", "1: key D minor ~:w");
    const auto followed =
        p.render(5.0, {launch(4 * kBeat, "cl_1"), back(8 * kBeat, LaunchLogEntry::Action::Stop)});
    p.run("clip.set", R"({"clip": "cl_1", "follow": "none"})");
    const auto written =
        p.render(5.0, {launch(4 * kBeat, "cl_1"), back(8 * kBeat, LaunchLogEntry::Action::Stop)});
    CHECK(sameFrom(followed, written, 0, 8 * kBeat + kFade));
    // Beside pitched notes, which follow (D minor to C minor: down a tone),
    // the channel-10 ones stay.
    Project mixed;
    followingClip(mixed, "key", "voice bass; 1: D3:w\nvoice kit ch 10; 1: C2:q D2:q E2:q F2:q",
                  "1: key D minor ~:w");
    Project mapped;
    mapped.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    mapped.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation":
        "voice bass; 1: C3:w\nvoice kit ch 10; 1: C2:q D2:q E2:q F2:q"})");
    const std::vector<LaunchLogEntry> bar2{launch(4 * kBeat, "cl_1"),
                                           back(8 * kBeat, LaunchLogEntry::Action::Stop)};
    CHECK(sameFrom(mixed.render(5.0, bar2), mapped.render(5.0, bar2), 0, 8 * kBeat + kFade));
}

TEST_CASE(
    "live launches of a following clip play as the replay, across key changes, wraps and seeks" *
    doctest::description("rt")) {
    // twoTracks' MIDI clip (written in C major) follows the key; the project
    // changes key on bar 2 (beat 4, inside the 1-3 s loop) and bar 3. A wrap
    // takes the song back over the change: the lookup is by song position.
    // Played from 0 and from a seek to 2 s, looped or not, and under a
    // master playrate (unlooped: a looped replay refuses a rate).
    for (const bool rate : {false, true})
        for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
            for (const double start : {0.0, 2.0})
                for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                    if (rate && (loop || start > 0)) continue;
                    INFO("rate " << rate << ", looped " << bool(loop) << ", start " << start
                                 << ", anticipative " << options.anticipative);
                    Project p;
                    twoTracks(p);
                    p.runArgs("context.write",
                              contextText("1: key C major ~:w\n2: key Eb major ~:w\n3: key A "
                                          "minor ~:w\n4: ~:w\n5: ~:w"));
                    p.run("clip.set",
                          R"({"clip": "cl_1", "context": "1: key C major ~:w", "follow": "key"})");
                    if (rate) underRate(p, false);
                    const model::Session s = p.app->head();
                    constexpr std::uint32_t block = 128;
                    const std::int64_t frames = 48000 * (rate ? 7 : 4) + 8192;
                    const std::vector<Step> steps{{20, {request("tr_1", "cl_1")}}};
                    const Live live = playLive(s, p.app->projectDir(), options, block, frames,
                                               steps, loop, start);
                    REQUIRE(live.log.size() == 1);
                    InstancePool pool = makePool();
                    OfflineSettings settings;
                    settings.startSeconds = start;
                    settings.endSeconds = start + (rate ? 7.0 : 4.0);
                    settings.projectDir = p.app->projectDir();
                    settings.launches = live.log;
                    settings.loop = loop;
                    auto offline = renderOffline(s, pool, settings);
                    REQUIRE(offline);
                    const std::size_t n = 48000 * (rate ? 7 : 4);
                    CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], n) == 0.0);
                    // Following changed what plays: the clip as written differs.
                    p.run("clip.set", R"({"clip": "cl_1", "follow": "none"})");
                    auto written = renderOffline(p.app->head(), pool, settings);
                    REQUIRE(written);
                    CHECK(maxDiff(live.left, live.latency, written->audio.channels[0], n) > 0.0);
                }
}

TEST_CASE("a plan swap mid-note with other pitch tables leaves no following note sounding" *
          doctest::description("rt")) {
    // cl_1 (D minor, following) plays a half note from bar 2 under C minor.
    // Mid-note a new plan maps it elsewhere (the project now in E minor): its
    // note-off must still end the note that sounds, so the rest is silent.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        followingClip(p, "key", "voice bass; 1: D3:h r:h", "1: key D minor ~:w");
        p.runArgs("context.write", contextText("1: key C minor ~:w\n2: ~:w\n3: ~:w"));
        const model::Session before = p.app->head();
        p.runArgs("context.write", contextText("1: key E minor ~:w\n2: ~:w\n3: ~:w"));
        p.run("track.set", R"({"track": "tr_1", "name": "Moved"})");  // a compile, not a shortcut
        const model::Session after = p.app->head();
        constexpr std::uint32_t block = 128;
        const std::int64_t frames = 10 * kBeat + 8192;
        const Live live =
            playLive(before, p.app->projectDir(), options, block, frames,
                     {{20, {request("tr_1", "cl_1")}}, {(5 * kBeat) / block, {}, {}, &after}});
        REQUIRE(live.log.size() == 1);
        CHECK(live.log[0].anchorSample == 4 * kBeat);
        // Sounding from bar 2, silent from its note-off (beat 6) to bar 3.
        CHECK(live.left[std::size_t(4 * kBeat + 1000 + live.latency)] != 0.0f);
        std::size_t heard = 0;  // it plays on past the swap, to its note-off
        for (std::int64_t t = 5 * kBeat + kFade; t < 6 * kBeat - kFade; ++t)
            heard += live.left[std::size_t(t + live.latency)] != 0.0f;
        CHECK(heard > std::size_t(kBeat / 2));
        for (std::int64_t t = 6 * kBeat + kFade; t < 8 * kBeat; ++t)
            REQUIRE(live.left[std::size_t(t + live.latency)] == 0.0f);
        // The new tables play from then on: bar 3 is the new session's.
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 5.0;
        settings.projectDir = p.app->projectDir();
        settings.launches = live.log;
        auto replay = renderOffline(after, pool, settings);
        REQUIRE(replay);
        const auto& r = replay->audio.channels[0];
        bool same = true;
        for (std::int64_t t = 8 * kBeat + kFade; t < 9 * kBeat; ++t)
            same = same && live.left[std::size_t(t + live.latency)] == r[std::size_t(t)];
        CHECK(same);
    }
}
