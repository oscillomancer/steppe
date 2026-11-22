// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A launched clip following the project's key and chords live (ADR-0098
// X4): as placement lays it, a note's pitch from its start, channel 10 as
// written, live as the replay, and plan swaps mid-note. Its own ctest entry,
// as the other test_clip_player files.
#include <supple/model/follow.h>
#include <algorithm>
#include <chrono>
#include <map>

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
// Lays cl_1 on bar `bar` as written (follow none) and gives the laid take what
// placement gives it under the amendment (X4d builds that into the commands):
// the clip's context in context beats, its length as the period, the phase
// that puts each note at its clip beat (c0 x r - o, c0 = 0), and `follow`.
model::Id layFollowing(Project& p, int bar, model::ClipFollow follow) {
    json::Value add = j(R"({"track": "tr_1", "clip": "cl_1", "follow": "none"})");
    add["bar"] = bar;
    const json::Value added = p.runArgs("item.add", add);
    const auto item = *model::Id::parse(added.find("item")->asString());
    const model::Session s = p.app->head();
    const model::Clip& clip = *s.clips.find(id("cl_1"));
    const model::Id takeId = *s.items.find(item)->activeTake;
    const model::Take& take = *s.takes.find(takeId);
    const double r = take.playrate > 0 ? take.playrate : 1.0;
    std::vector<model::ContextPoint> context = clip.context;
    for (auto& point : context) point.beat *= r;
    auto txn = p.app->beginTransaction({cmd::Origin::Type::Test, "x4c", {}}, "Follow");
    REQUIRE(txn);
    p.app->txn().modify<model::Take>(takeId, [&](model::Take& t) {
        t.context = context;
        t.contextPeriod = clip.length * r;
        t.contextPhase = -t.startOffset;
        t.follow = follow;
    });
    REQUIRE(p.app->commitTransaction(*txn));
    return item;
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

TEST_CASE(
    "a context edit compiles while a launched clip follows, and takes the shortcut otherwise" *
    doctest::description("rt")) {
    // cl_1 (D minor) launched on bar 2 under C minor; mid-bar 2 the context
    // track alone changes to E minor. Following, the edit compiles and bar 3
    // plays the new tables (equal to the new session's replay); as written,
    // the edit compiles nothing (ADR-0095's shortcut).
    for (const char* follow : {"key", "none"}) {
        CAPTURE(follow);
        Project p;
        followingClip(p, follow, "voice bass; 1: D3:q F3:q A3:q Bb3:q", "1: key D minor ~:w");
        p.runArgs("context.write", contextText("1: key C minor ~:w\n2: ~:w\n3: ~:w"));
        const model::Session before = p.app->head();
        p.runArgs("context.write", contextText("1: key E minor ~:w\n2: ~:w\n3: ~:w"));
        const model::Session after = p.app->head();
        {
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual});
            Engine engine(pool, media, kInCallback);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
            REQUIRE(engine.load(before, p.app->projectDir()));
            engine.play();
            backend.run(8);
            engine.service();
            const auto generation = engine.planGeneration();
            REQUIRE(engine.load(after, p.app->projectDir()));
            CHECK((engine.planGeneration() > generation) == (std::string(follow) != "none"));
            engine.stop();
        }
        if (std::string(follow) == "none") continue;
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("anticipative " << options.anticipative);
            constexpr std::uint32_t block = 128;
            const Live live =
                playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192,
                         {{20, {request("tr_1", "cl_1")}}, {(5 * kBeat) / block, {}, {}, &after}});
            REQUIRE(live.log.size() == 1);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 5.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            auto replay = renderOffline(after, pool, settings);
            REQUIRE(replay);
            bool same = true;
            for (std::int64_t t = 8 * kBeat + kFade; t < 9 * kBeat; ++t)
                same = same && live.left[std::size_t(t + live.latency)] ==
                                   replay->audio.channels[0][std::size_t(t)];
            CHECK(same);
        }
    }
}

TEST_CASE("launch.state says what a launched clip follows by, and nothing warns it unfollowed") {
    // ADR-0098 X4a: launch.unfollowed went with live following; launch.state
    // names the outcome the running plan uses for the clip a track plays, else
    // the one it has queued, and leaves it out when there is neither.
    Project p;
    followingClip(p, "chords", "voice bass; 1: D3:q F3:q A3:q Bb3:q");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    const auto track = [&]() {
        const json::Value state = p.run("launch.state", "{}");
        for (const json::Value& t : state.find("tracks")->asArray())
            if (t.find("track")->asString() == "tr_1") return t;
        FAIL("tr_1 is not listed");
        return json::Value();
    };
    CHECK_FALSE(track().find("followed"));  // the arrangement
    const json::Value done = p.run("launch.clip", R"({"clip": "cl_1"})");
    CHECK_FALSE(done.find("warnings"));
    CHECK(track().find("queued"));
    CHECK(track().find("followed")->asString() == "chords");  // queued
    for (int i = 0; i < 200; ++i) {
        backend.run(1);
        p.app->tick();
    }
    REQUIRE(track().find("plays")->asString() == "clip");
    CHECK(track().find("followed")->asString() == "chords");  // playing
    CHECK_FALSE(track().find("follow_budget"));
    p.run("clip.set", R"({"clip": "cl_1", "follow": "key"})");
    CHECK(track().find("followed")->asString() == "key");
    p.run("clip.set", R"({"clip": "cl_1", "follow": "none"})");
    CHECK(track().find("followed")->asString() == "none");
    CHECK_FALSE(p.run("launch.scene", R"({"scene": "sc_1"})").find("warnings"));
    p.app->stopEngine();
}

// --- Timeline items follow at playback (ADR-0098 amended, X4c) -----------------

TEST_CASE("timeline equals launcher for the same clip, its context read pass for pass") {
    // cl_1 is a bar of content looped over its two-bar length, written in D
    // minor for its first bar and F minor for its second. Launched on bar 2,
    // and laid there with the take members placement gives, it follows the
    // project (C minor, then E minor) by the same context beats, so its second
    // pass reads F minor on the timeline as in the launcher. Also at a take
    // playrate of 2 (four passes, the context in context beats twice as long).
    for (const double rate : {1.0, 2.0})
        for (const char* follow : {"key", "chords"}) {
            CAPTURE(rate);
            CAPTURE(follow);
            Project p;
            // Its second bar's context once it is two bars long (points lie
            // within the clip).
            followingClip(p, follow, "voice bass; 1: D3:q F3:q A3:q Bb3:q",
                          "1: key D minor {Dm}:w");
            json::Value set = j(R"({"clip": "cl_1", "length": 8})");
            json::Value loop = j(R"({"on": true, "start": 0})");
            loop["length"] = 4.0 / rate;
            set["loop"] = loop;
            if (rate != 1.0) {
                json::Value rated = json::Value::object();
                rated["take"] = p.app->head().clips.find(id("cl_1"))->activeTake->str();
                rated["playrate"] = rate;
                p.runArgs("take.set", rated);
            }
            p.runArgs("clip.set", set);
            p.run("clip.set",
                  R"({"clip": "cl_1", "context": "1: key D minor {Dm}:w\n2: key F minor {Fm}:w"})");
            const auto launched = p.render(
                7.0, {launch(4 * kBeat, "cl_1"), back(12 * kBeat, LaunchLogEntry::Action::Stop)});
            layFollowing(
                p, 2,
                std::string(follow) == "key" ? model::ClipFollow::Key : model::ClipFollow::Chords);
            const auto laid = p.render(7.0, {});
            CHECK(sameFrom(launched, laid, 4 * kBeat + kFade, 12 * kBeat));
            // It moved: the item as written sounds otherwise.
            const model::Id item = p.app->head().items.begin()->id;
            auto txn = p.app->beginTransaction({cmd::Origin::Type::Test, "x4c", {}}, "Off");
            REQUIRE(txn);
            p.app->txn().modify<model::Take>(
                *p.app->head().items.find(item)->activeTake,
                [](model::Take& t) { t.follow = model::ClipFollow::None; });
            REQUIRE(p.app->commitTransaction(*txn));
            CHECK_FALSE(sameFrom(launched, p.render(7.0, {}), 4 * kBeat + kFade, 12 * kBeat));
        }
}

TEST_CASE("a following item renders as take.bake writes it, then as written") {
    // X4c: the compile maps a following take as model::soundingNotes does, and
    // take.bake writes those pitches. cl_1 (D minor, {Dm} then {Gm}) laid
    // looped over bars 2 to 4 follows C minor, then E minor: its passes sound
    // differently, so the bake unrolls it. The render before the bake and the
    // render after it, as written, are the same samples.
    for (const char* follow : {"key", "chords"}) {
        CAPTURE(follow);
        Project p;
        followingClip(p, follow, "voice bass; 1: D3:q F3:q A3:q Bb3:q");
        const json::Value laid = p.run("scene.insert", R"({"scene": "sc_1", "bar": 2, "bars": 3})");
        const auto item =
            *model::Id::parse(laid.find("items")->asArray().at(0).find("item")->asString());
        const model::Id take = *p.app->head().items.find(item)->activeTake;
        REQUIRE(p.app->head().takes.find(take)->follow != model::ClipFollow::None);
        const auto following = p.render(9.0, {});
        json::Value bake = json::Value::object();
        bake["take"] = take.str();
        const json::Value baked = p.runArgs("take.bake", bake);
        CHECK(baked.find("unrolled")->asBool());
        CHECK(p.app->head().takes.find(take)->follow == model::ClipFollow::None);
        const auto written = p.render(9.0, {});
        CHECK(sameFrom(following, written, 0, 9 * 48000));
        // It moved: undone, and played as written without following, it differs.
        p.run("history.undo", "{}");
        auto txn = p.app->beginTransaction({cmd::Origin::Type::Test, "x4c", {}}, "Off");
        REQUIRE(txn);
        p.app->txn().modify<model::Take>(
            take, [](model::Take& t) { t.follow = model::ClipFollow::None; });
        REQUIRE(p.app->commitTransaction(*txn));
        CHECK_FALSE(sameFrom(following, p.render(9.0, {}), 0, 9 * 48000));
    }
}

TEST_CASE("a following take's notes compile to the keys model::soundingNotes gives") {
    // The compile maps each note-on of a following take by the place of its
    // start, as soundingNotes walks the same passes: a note chased at the
    // item's start (the take slipped by a beat into a half note) and the
    // loop's later passes. Each note-off ends its note-on's key, and the
    // origin says the note was followed.
    Project p;
    followingClip(p, "key", "voice bass; 1: D3:h F3:h", "1: key D minor ~:w");
    p.run("clip.set",
          R"({"clip": "cl_1", "length": 8, "loop": {"on": true, "start": 0, "length": 4}})");
    p.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w\n2: key F minor ~:w"})");
    const model::Id item = layFollowing(p, 2, model::ClipFollow::Key);
    json::Value slip = json::Value::object();
    slip["take"] = p.app->head().items.find(item)->activeTake->str();
    slip["start_offset"] = 1.0;
    p.runArgs("take.set", slip);
    const model::Session s = p.app->head();
    InstancePool pool = makePool();
    auto plan = detail::compile(s, pool, 0, 128);
    REQUIRE(plan);
    REQUIRE(detail::prepare(*plan, s));
    std::vector<detail::TimedEvent> events;
    for (const auto& node : plan->nodes)
        if (node.id == id("tr_1")) events = node.events;
    const auto notes = model::soundingNotes(s, item, -1e9, 1e9);
    REQUIRE_FALSE(notes.empty());
    const model::Take& take = *s.takes.find(*s.items.find(item)->activeTake);
    const midi::Sequence& seq = s.midi.find(*take.source)->sequence;
    std::map<std::pair<std::int64_t, std::int32_t>, int> want;  // (tick, pass): its key
    bool moves = false, chased = false;
    for (const model::SoundingNote& n : notes) {
        want[{seq.notes[n.note].start, n.pass}] = n.sounds.pitch;
        moves |= n.sounds.pitch != n.written;
        chased |= n.songBeat < 4.0;  // it began before the item, on bar 2
    }
    CHECK(moves);
    CHECK(chased);
    std::map<std::int32_t, std::int16_t> onKey;  // note id: its key
    int ons = 0;
    for (const detail::TimedEvent& e : events) {
        if (e.event.kind != host::Event::Kind::NoteOn) continue;
        ++ons;
        const auto it = want.find({e.origin.tick, e.origin.pass});
        REQUIRE(it != want.end());
        CHECK(e.event.key == it->second);
        CHECK(e.origin.followed);
        onKey[e.event.noteId] = e.event.key;
    }
    CHECK(ons == int(notes.size()));
    for (const detail::TimedEvent& e : events)
        if (e.event.kind == host::Event::Kind::NoteOff)
            CHECK(e.event.key == onKey.at(e.event.noteId));
}

TEST_CASE("a context edit mid-note leaves no following take's note sounding" *
          doctest::description("rt")) {
    // A laid item (D minor, following) plays a half note from bar 2 under C
    // minor. Mid-note a context edit maps it elsewhere (E minor): the edit
    // compiles (no shortcut while a take follows), and the note-off still ends
    // the note that sounds, so the rest is silent; bar 3 is the new session's.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        followingClip(p, "key", "voice bass; 1: D3:h r:h", "1: key D minor ~:w");
        layFollowing(p, 2, model::ClipFollow::Key);
        layFollowing(p, 3, model::ClipFollow::Key);
        // Only the takes follow: no launched clip's tables ask for the compile.
        p.run("clip.set", R"({"clip": "cl_1", "follow": "none"})");
        p.runArgs("context.write", contextText("1: key C minor ~:w\n2: ~:w\n3: ~:w"));
        const model::Session before = p.app->head();
        p.runArgs("context.write", contextText("1: key E minor ~:w\n2: ~:w\n3: ~:w"));
        const model::Session after = p.app->head();
        {
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual});
            Engine engine(pool, media, kInCallback);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
            REQUIRE(engine.load(before, p.app->projectDir()));
            const auto generation = engine.planGeneration();
            REQUIRE(engine.load(after, p.app->projectDir()));
            CHECK(engine.planGeneration() > generation);
            engine.stop();
        }
        constexpr std::uint32_t block = 128;
        const std::int64_t frames = 10 * kBeat + 8192;
        const Live live = playLive(before, p.app->projectDir(), options, block, frames,
                                   {{(5 * kBeat) / block, {}, {}, &after}});
        CHECK(live.left[std::size_t(4 * kBeat + 1000 + live.latency)] != 0.0f);
        std::size_t heard = 0;  // it plays on past the swap, to its note-off
        for (std::int64_t t = 5 * kBeat + kFade; t < 6 * kBeat - kFade; ++t)
            heard += live.left[std::size_t(t + live.latency)] != 0.0f;
        CHECK(heard > std::size_t(kBeat / 2));
        for (std::int64_t t = 6 * kBeat + kFade; t < 8 * kBeat; ++t)
            REQUIRE(live.left[std::size_t(t + live.latency)] == 0.0f);
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 5.0;
        settings.projectDir = p.app->projectDir();
        auto replay = renderOffline(after, pool, settings);
        REQUIRE(replay);
        const auto& r = replay->audio.channels[0];
        bool same = true;
        for (std::int64_t t = 8 * kBeat + kFade; t < 9 * kBeat; ++t)
            same = same && live.left[std::size_t(t + live.latency)] == r[std::size_t(t)];
        CHECK(same);
    }
}

TEST_CASE(
    "before a clip's first chord point it follows by the first chord, as the model reads it") {
    // Builder's note on X4b: soundingKey holds a context's first key and chord
    // before their first points; the live tables read none there. cl_1 (D
    // minor, its first chord {Bb} on beat 2) follows the chords: its first
    // beats map by {Bb} as soundingKey says (D, its third, to Ab under {Fm};
    // by the key it would be C), launched as laid.
    Project p;
    followingClip(p, "chords", "voice bass; 1: D3:q F3:q A3:q Bb3:q", "1: key D minor ~:h {Bb}:h");
    const auto launched =
        p.render(7.0, {launch(4 * kBeat, "cl_1"), back(8 * kBeat, LaunchLogEntry::Action::Stop)});
    layFollowing(p, 2, model::ClipFollow::Chords);
    const auto laid = p.render(7.0, {});
    CHECK(sameFrom(launched, laid, 4 * kBeat + kFade, 8 * kBeat));
}

TEST_CASE("launching a clip the follow budget holds back warns launch.follow_budget") {
    // A clip and a project whose distinct chords make more pairs than one
    // clip's budget: launch.clip says it follows the key, and launch.state
    // marks it.
    Project p;
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    const char* roots[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    const char* kinds[] = {"", "m", "7", "m7"};
    std::string project = "1: key C major", clip = "1: key C major";
    for (int i = 0; i < 48; ++i) {
        const std::string chord = std::string(" {") + roots[i / 4] + kinds[i % 4] + "}:q";
        if (i % 4 == 0 && i > 0) {
            project += "\n" + std::to_string(i / 4 + 1) + ":";
            clip += "\n" + std::to_string(i / 4 + 1) + ":";
        }
        project += chord;
        clip += chord;
    }
    p.runArgs("context.write", contextText(project.c_str()));
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:w"})");
    p.run("clip.set", R"({"clip": "cl_1", "length": 48})");
    json::Value set = j(R"({"clip": "cl_1", "follow": "chords"})");
    set["context"] = clip;
    p.runArgs("clip.set", set);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    const json::Value done = p.run("launch.clip", R"({"clip": "cl_1"})");
    const json::Value* warnings = done.find("warnings");
    REQUIRE(warnings);
    const json::Value* budget = nullptr;
    for (const json::Value& w : warnings->asArray())
        if (w.find("code")->asString() == "launch.follow_budget") budget = &w;
    REQUIRE(budget);
    CHECK(budget->find("message")->asString().find("follows the key") != std::string::npos);
    const json::Value state = p.run("launch.state", "{}");
    const json::Value& track = state.find("tracks")->asArray().front();
    CHECK(track.find("followed")->asString() == "key");
    CHECK(track.find("follow_budget")->asBool());
    p.app->stopEngine();
}

// --- Chords across a plan swap (review round 86 H1) --------------------------

namespace {
// The project's context before a swap: C minor, a chord a bar.
constexpr const char* kCMinor = "1: key C minor {Cm}:w\n2: {Cm}:w\n3: {Cm}:w";
// What makes the swap: a context edit, an edit elsewhere (a track renamed), or
// new pitch tables (the clip's own context, or the laid take's).
enum class SwapBy { ContextEdit, TrackSet, TableChange };
const char* nameOf(SwapBy by) {
    return by == SwapBy::ContextEdit ? "context edit"
           : by == SwapBy::TrackSet  ? "track.set"
                                     : "table change";
}
void swapEdit(Project& p, SwapBy by, const std::optional<model::Id>& take = std::nullopt) {
    switch (by) {
        case SwapBy::ContextEdit:
            p.runArgs("context.write", contextText("1: key E minor {Em}:w\n2: {Em}:w\n3: {Em}:w"));
            break;
        case SwapBy::TrackSet:
            p.run("track.set", R"({"track": "tr_1", "name": "Moved"})");
            break;
        case SwapBy::TableChange: {
            json::Value set = j(R"({"context": "1: key G minor {Gm}:w"})");
            if (take)
                set["take"] = take->str();
            else
                set["clip"] = "cl_1";
            p.runArgs(take ? "take.set" : "clip.set", set);
            break;
        }
    }
}
// Silent from `silentFrom` (its last note-off, in beats) to bar 3, and
// sounding before it from `heardFrom` (after the swap).
void endsAt(const Live& live, double heardFrom, double silentFrom) {
    const auto at = [&](double beats) { return std::int64_t(beats * double(kBeat)); };
    std::size_t heard = 0;
    for (std::int64_t t = at(heardFrom) + kFade; t < at(silentFrom) - kFade; ++t)
        heard += live.left[std::size_t(t + live.latency)] != 0.0f;
    CHECK(heard > std::size_t((at(silentFrom) - at(heardFrom)) / 2));
    std::int64_t sounding = -1;  // the first frame heard after the last note-off
    for (std::int64_t t = at(silentFrom) + kFade; t < 8 * kBeat && sounding < 0; ++t)
        if (live.left[std::size_t(t + live.latency)] != 0.0f) sounding = t;
    CHECK_MESSAGE(sounding == -1, "frame " << sounding);
}
// The same samples as `held`, the same plan played with no swap, up to
// `until`: each held note sounded on at its pitch to its own note-off.
void sameAsHeld(const Live& live, const Live& held, std::int64_t until) {
    CHECK(live.latency == held.latency);
    std::int64_t apart = -1;  // the first frame that differs
    for (std::int64_t t = 0; t < until && apart < 0; ++t)
        if (live.left[std::size_t(t)] != held.left[std::size_t(t)]) apart = t;
    CHECK_MESSAGE(apart == -1, "frame " << apart);
}
// cl_1 launched on bar 2 with `notation`, a swap at `swapBeat`.
Live launchedAcrossSwap(Project& p, const char* follow, const char* notation, SwapBy by,
                        const EngineOptions& options, double swapBeat) {
    followingClip(p, follow, notation, "1: key D minor {Dm}:w");
    p.runArgs("context.write", contextText(kCMinor));
    const model::Session before = p.app->head();
    swapEdit(p, by);
    const model::Session after = p.app->head();
    constexpr std::uint32_t block = 128;
    const Live live = playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192,
                               {{20, {request("tr_1", "cl_1")}},
                                {std::int64_t(swapBeat * double(kBeat)) / block, {}, {}, &after}});
    REQUIRE(live.log.size() == 1);
    CHECK(live.log[0].anchorSample == 4 * kBeat);
    return live;
}
}  // namespace

TEST_CASE("a launched clip's chord ends at its note-offs across any plan swap" *
          doctest::description("rt")) {
    // A chord's notes share their origin and their ordinal (counted per
    // key): the carry tells them apart by their written pitch, which a
    // following clip's mapping leaves alone, so each voice keeps its own note
    // and its note-off ends it. Following or not, whatever compiles.
    for (const char* follow : {"none", "key", "chords"})
        for (const SwapBy by : {SwapBy::ContextEdit, SwapBy::TrackSet, SwapBy::TableChange})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                CAPTURE(follow);
                CAPTURE(nameOf(by));
                CAPTURE(options.anticipative);
                Project p;
                const Live live = launchedAcrossSwap(p, follow, "voice keys; 1: [D3 F3 A3]:h r:h",
                                                     by, options, 5.0);
                endsAt(live, 5.0, 6.0);
            }
}

TEST_CASE("a following item's chord ends at its note-offs across any plan swap" *
          doctest::description("rt")) {
    // The same for a laid take that follows (X4c): its chord, mapped at
    // compile, is carried by written pitch.
    for (const model::ClipFollow follow : {model::ClipFollow::Key, model::ClipFollow::Chords})
        for (const SwapBy by : {SwapBy::ContextEdit, SwapBy::TrackSet, SwapBy::TableChange})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                CAPTURE(follow == model::ClipFollow::Key ? "key" : "chords");
                CAPTURE(nameOf(by));
                CAPTURE(options.anticipative);
                Project p;
                followingClip(p, "none", "voice keys; 1: [D3 F3 A3]:h r:h",
                              "1: key D minor {Dm}:w");
                const model::Id item = layFollowing(p, 2, follow);
                const model::Id take = *p.app->head().items.find(item)->activeTake;
                p.runArgs("context.write", contextText(kCMinor));
                const model::Session before = p.app->head();
                swapEdit(p, by, take);
                const model::Session after = p.app->head();
                constexpr std::uint32_t block = 128;
                const Live live =
                    playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192,
                             {{(5 * kBeat) / block, {}, {}, &after}});
                endsAt(live, 5.0, 6.0);
            }
}

TEST_CASE("one written pitch on two channels, or overlapping, is carried note by note" *
          doctest::description("rt")) {
    // D3 on channel 1 for a beat and on channel 2 for two: after the swap
    // channel 1's ends at beat 5 and channel 2's at 6. Overlapping D3s on one
    // channel (beats 4-6 and 5-7, origins apart by their start) both play on
    // past the swap; the sine ends every voice of a key at a note-off.
    for (const char* follow : {"none", "chords"})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            CAPTURE(follow);
            CAPTURE(options.anticipative);
            {
                Project p;
                const Live live = launchedAcrossSwap(
                    p, follow, "voice a ch 1; 1: D3:q r:q r:h; voice b ch 2; 1: D3:h r:h",
                    SwapBy::ContextEdit, options, 4.5);
                endsAt(live, 5.0, 6.0);
            }
            {
                Project p;
                const Live live =
                    launchedAcrossSwap(p, follow, "voice a; 1: D3:h r:h; voice b; 1: r:q D3:h r:q",
                                       SwapBy::ContextEdit, options, 5.5);
                endsAt(live, 5.5, 6.0);
            }
        }
}

TEST_CASE("a held chord ends at its note-offs whichever way its take's following changes" *
          doctest::description("rt")) {
    // Review round 87 J1: a held voice survives a plan swap at the pitch it
    // sounds and ends at its written end, whether the swap turns its take's
    // following off (take.set, or the undo of a follow), on, or from key to
    // chords. The project's {Fm} makes chords map D minor otherwise than key
    // does, so every swap here changes what the chord's notes sound.
    using F = model::ClipFollow;
    const auto nameOfFollow = [](F f) -> std::string {
        return f == F::None ? "none" : f == F::Key ? "key" : "chords";
    };
    const std::pair<F, F> swaps[] = {{F::Key, F::None},   {F::Chords, F::None},
                                     {F::None, F::Key},   {F::None, F::Chords},
                                     {F::Key, F::Chords}, {F::Chords, F::Key}};
    for (const auto& [from, to] : swaps)
        for (const bool undo : {false, true})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                const std::string swap = nameOfFollow(from) + " to " + nameOfFollow(to);
                CAPTURE(swap);
                CAPTURE(undo);
                CAPTURE(options.anticipative);
                Project p;
                followingClip(p, "none", "voice keys; 1: [D3 F3 A3]:h r:h",
                              "1: key D minor {Dm}:w");
                // An undo swaps back to how the item was laid: laid `to`,
                // set `from`, undone.
                const model::Id item = layFollowing(p, 2, undo ? to : from);
                const model::Id take = *p.app->head().items.find(item)->activeTake;
                p.runArgs("context.write",
                          contextText("1: key C minor {Fm}:w\n2: {Fm}:w\n3: {Fm}:w"));
                json::Value set = json::Value::object();
                set["take"] = take.str();
                set["follow"] = nameOfFollow(undo ? from : to);
                if (undo) p.runArgs("take.set", set);
                const model::Session before = p.app->head();
                const auto heardBefore = p.render(3.5, {});
                if (undo)
                    p.run("history.undo", "{}");
                else
                    p.runArgs("take.set", set);
                const model::Session after = p.app->head();
                REQUIRE(after.takes.find(take)->follow == to);
                REQUIRE_FALSE(sameFrom(heardBefore, p.render(3.5, {}), 4 * kBeat, 6 * kBeat));
                constexpr std::uint32_t block = 128;
                const Live live =
                    playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192,
                             {{(5 * kBeat) / block, {}, {}, &after}});
                endsAt(live, 5.0, 6.0);
                // At the pitch it sounded: the same samples as with no swap.
                const Live held =
                    playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192, {});
                sameAsHeld(live, held, 10 * kBeat);
            }
}

TEST_CASE("a routed held chord ends at its note-offs across a swap that moves its keys" *
          doctest::description("rt")) {
    // J1 through a MIDI send: tr_1 has no instrument and sends to tr_2's sine.
    // The stream ends a held note's slot, and so the voice it sent, at the
    // key the note-on sent, whatever key the new plan's note-off has: a
    // following take swapped to or from following, and a launched clip across
    // a context edit or new pitch tables. The same samples as with no swap.
    using F = model::ClipFollow;
    constexpr std::uint32_t block = 128;
    const auto routedClip = [](Project& p, const char* follow) {
        p.runArgs("context.write", contextText(kCMinor));
        json::Value clip = j(R"({"track": "tr_1", "scene": "sc_1",
                                 "notation": "voice keys; 1: [D3 F3 A3]:h r:h",
                                 "context": "1: key D minor {Dm}:w"})");
        clip["follow"] = follow;
        p.runArgs("clip.add", clip);
        p.run("track.add", R"({"name": "Synth"})");
        p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.sine"})");
        p.run("route.add", R"({"from": "tr_1", "to": "tr_2", "midi": {}})");
    };
    const auto heldAcross = [&](Project& p, const model::Session& before,
                                const model::Session& after, const EngineOptions& options,
                                bool launched) {
        std::vector<Step> plain;
        if (launched) plain.push_back({20, {request("tr_1", "cl_1")}});
        std::vector<Step> swapped = plain;
        swapped.push_back({(5 * kBeat) / block, {}, {}, &after});
        const Live live =
            playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192, swapped);
        endsAt(live, 5.0, 6.0);
        const Live held =
            playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192, plain);
        // A launched clip's next pass, from beat 8, sounds the new context.
        sameAsHeld(live, held, launched ? 8 * kBeat : 10 * kBeat);
    };
    const std::pair<F, F> swaps[] = {{F::Key, F::None}, {F::None, F::Key}, {F::Key, F::Chords}};
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        CAPTURE(options.anticipative);
        for (const auto& [from, to] : swaps) {
            const std::string swap = std::to_string(int(from)) + " to " + std::to_string(int(to));
            CAPTURE(swap);
            Project p;
            routedClip(p, "none");
            const model::Id item = layFollowing(p, 2, from);
            const model::Id take = *p.app->head().items.find(item)->activeTake;
            p.runArgs("context.write", contextText("1: key C minor {Fm}:w\n2: {Fm}:w\n3: {Fm}:w"));
            const model::Session before = p.app->head();
            json::Value set = json::Value::object();
            set["take"] = take.str();
            set["follow"] = to == F::None ? "none" : to == F::Key ? "key" : "chords";
            p.runArgs("take.set", set);
            heldAcross(p, before, p.app->head(), options, false);
        }
        for (const char* follow : {"key", "chords"})
            for (const SwapBy by : {SwapBy::ContextEdit, SwapBy::TableChange}) {
                const std::string clip = std::string(follow) + ", " + nameOf(by);
                CAPTURE(clip);
                Project p;
                routedClip(p, follow);
                const model::Session before = p.app->head();
                swapEdit(p, by);
                heldAcross(p, before, p.app->head(), options, true);
            }
    }
}

TEST_CASE("a clip played by its take's own instrument holds its chord across a plan swap" *
          doctest::description("rt")) {
    // Round 86 H4: cl_1's instrument is in its take's chain (ADR-0097's clip
    // chain), not on the track. Its chord, carried by its notes' sources,
    // plays on past a context edit to its note-offs (beat 6), then is silent.
    for (const char* follow : {"none", "key"})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            CAPTURE(follow);
            CAPTURE(options.anticipative);
            Project p;
            p.runArgs("context.write", contextText(kCMinor));
            json::Value clip = j(R"({"track": "tr_1", "scene": "sc_1",
                                     "notation": "voice keys; 1: [D3 F3 A3]:h r:h",
                                     "context": "1: key D minor {Dm}:w"})");
            clip["follow"] = follow;
            p.runArgs("clip.add", clip);
            json::Value fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");
            fx["take"] = p.app->head().clips.find(id("cl_1"))->activeTake->str();
            p.runArgs("fx.add", fx);
            const model::Session before = p.app->head();
            swapEdit(p, SwapBy::ContextEdit);
            p.run("track.set", R"({"track": "tr_1", "name": "Moved"})");  // a compile either way
            const model::Session after = p.app->head();
            constexpr std::uint32_t block = 128;
            const Live live =
                playLive(before, p.app->projectDir(), options, block, 10 * kBeat + 8192,
                         {{20, {request("tr_1", "cl_1")}}, {(5 * kBeat) / block, {}, {}, &after}});
            REQUIRE(live.log.size() == 1);
            endsAt(live, 5.0, 6.0);
        }
}

TEST_CASE("following at compile, measured (prints; decides nothing)" * doctest::skip()) {
    // Round 86 H9: a context edit compiles while anything follows. Its cost:
    // compile and prepare of sessions with `items` laid one-bar items of 16
    // notes, following nothing, the key or the chords; the extra per
    // following note-on. Run with -tc="following at compile*" --no-skip.
    const char* bar16 =
        "voice keys; 1: D3:s F3:s A3:s D4:s D3:s F3:s A3:s D4:s "
        "D3:s F3:s A3:s D4:s D3:s F3:s A3:s D4:s";
    for (const int items : {28, 400}) {
        double none = 0;
        for (const model::ClipFollow follow :
             {model::ClipFollow::None, model::ClipFollow::Key, model::ClipFollow::Chords}) {
            Project p;
            followingClip(p, "none", bar16, "1: key D minor {Dm}:h {Gm}:h");
            for (int i = 0; i < items; ++i) layFollowing(p, 1 + i, follow);
            const model::Session s = p.app->head();
            InstancePool pool = makePool();
            std::vector<double> ms;
            for (int rep = 0; rep < 7; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                auto plan = detail::compile(s, pool, 0, 128);
                REQUIRE(plan);
                REQUIRE(detail::prepare(*plan, s));
                ms.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count());
            }
            std::sort(ms.begin(), ms.end());
            const double median = ms[ms.size() / 2];
            if (follow == model::ClipFollow::None) none = median;
            const double perNote = (median - none) * 1000.0 / double(items * 16);
            MESSAGE("items " << items << " follow " << int(follow) << ": compile+prepare " << median
                             << " ms (median of 7), " << perNote << " us per following note-on");
        }
    }
}
