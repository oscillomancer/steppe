// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Rendering a launcher performance (engine.md §3.1): the render commands'
// `performance` selector, its result fields and refusals, the queue's
// resolved object, and song frames converted under a master playrate. Its
// own ctest entry.
#include <supple/audio/audio_file.h>
#include <chrono>
#include <optional>
#include <thread>
#include "clip_player_support.h"
#include "engine_test_access.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
constexpr std::uint32_t kBlock = 128;

std::vector<float> leftOf(json::Value rendered) {
    auto audio = audio::readAudio(rendered["path"].asString());
    REQUIRE_MESSAGE(audio, (audio ? std::string() : audio.error().toString()));
    return audio->channels[0];
}

Error refused(Project& p, const char* command, const json::Value& args) {
    auto r = p.bus->execute({command, args, kOrigin});
    REQUIRE_FALSE(r);
    return r.error();
}

// cl_1 on tr_1 from bar 2 for a bar, given outright.
json::Value given() {
    return j(R"({"sample_rate": 48000, "start": 0, "frames": 192000,
        "launches": [{"frame": 96000, "track": "tr_1", "action": "clip", "clip": "cl_1",
                      "anchor": 96000, "anchor_beat": 4}]})");
}

json::Value withPerformance(json::Value args, json::Value performance) {
    args["performance"] = std::move(performance);
    return args;
}
}  // namespace

TEST_CASE("a capture from a play start renders as heard, bit for bit, and queues as an object") {
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 5);
    // Armed while stopped: it begins where playing begins.
    p.run("launch.capture", R"({"on": true})");
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    p.run("launch.clip", R"({"clip": "cl_2"})");
    live.until(150016);
    json::Value ended = p.run("launch.capture", R"({"on": false})");
    live.until(160000);
    const std::int64_t version = std::int64_t(p.app->head().version);
    CHECK(ended["version"].asInt() == version);
    CHECK(p.run("launch.state", "{}")["capture_version"].asInt() == version);

    // The dry run: what it would play, compiling nothing.
    json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
    CHECK(dry["as_heard"].asBool());
    CHECK_FALSE(dry.find("not_heard_because"));
    json::Value perf = dry["performance"];
    CHECK(perf["start"].asInt() == 0);
    CHECK(perf["launches"].size() == 2);
    CHECK(dry["range"]["start_beat"].asDouble() == 0);
    CHECK(dry["range"]["end_beat"].asDouble() > 4);

    // The render, as heard: what played, bit for bit.
    json::Value rendered =
        p.run("render.mix",
              R"({"performance": "captured", "as_heard": true, "bit_depth": 32, "name": "heard",
            "tail_s": 0})");
    CHECK(rendered["as_heard"].asBool());
    CHECK(rendered["performance"] == perf);
    const std::vector<float> offline = leftOf(rendered);
    // The queue stores the object it resolved to, pinned to its version.
    json::Value queued = p.run("render.queue.add", R"({"command": "render.mix",
                                      "args": {"performance": "captured", "name": "queued"}})");
    CHECK(queued["version"].asInt() == version);
    CHECK(queued["args"]["performance"] == perf);
    CHECK_FALSE(queued["args"].find("bounds"));
    CHECK_FALSE(queued["args"].find("as_heard"));
    const std::vector<float> heard = live.stop();
    const std::int64_t frames = perf["frames"].asInt();
    REQUIRE(std::int64_t(offline.size()) >= frames);
    const auto [worst, first] = compare(heard, live.latency, offline, 0, frames);
    INFO("first differing frame " << first);
    CHECK(worst == 0.0);
}

TEST_CASE("a performance render refuses what it cannot play, one code per cause") {
    Project p;
    twoTracks(p);
    // Nothing captured.
    Error e = refused(p, "render.dry-run", j(R"({"performance": "captured"})"));
    CHECK(e.code == "launch.capture");
    CHECK(e.path == "/args/performance");
    // as_heard alone.
    e = refused(p, "render.dry-run", j(R"({"as_heard": true})"));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/as_heard");
    // Regions, bounds in the call, seamless_loop.
    e = refused(p, "render.regions", withPerformance(j("{}"), given()));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/performance");
    e = refused(p, "render.dry-run", withPerformance(j(R"({"bounds": "project"})"), given()));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/performance");
    e = refused(p, "render.dry-run", withPerformance(j(R"({"seamless_loop": true})"), given()));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/seamless_loop");
    // Another sample rate.
    json::Value other = given();
    other["sample_rate"] = 44100;
    e = refused(p, "render.dry-run", withPerformance(j("{}"), other));
    CHECK(e.code == "render.performance.sample_rate");
    CHECK(e.path == "/args/performance/sample_rate");
    // Out of frame order, and past frames.
    const auto launches = [](const char* list) {
        json::Value perf = given();
        perf["launches"] = j(list);
        return perf;
    };
    e = refused(p, "render.dry-run", withPerformance(j("{}"), launches(R"([
        {"frame": 96000, "track": "tr_1", "action": "clip", "clip": "cl_1", "anchor": 96000,
         "anchor_beat": 4},
        {"frame": 1000, "track": "tr_1", "action": "stop", "anchor": 1000, "anchor_beat": 0}])")));
    CHECK(e.code == "render.performance.launches");
    CHECK(e.path == "/args/performance/launches/1/frame");
    e = refused(p, "render.dry-run", withPerformance(j("{}"), launches(R"([
        {"frame": 192000, "track": "tr_1", "action": "clip", "clip": "cl_1", "anchor": 96000,
         "anchor_beat": 4}])")));
    CHECK(e.code == "render.performance.launches");
    CHECK(e.path == "/args/performance/launches/0/frame");
    // A track the version lacks.
    e = refused(p, "render.dry-run", withPerformance(j("{}"), launches(R"([
        {"frame": 96000, "track": "tr_9", "action": "clip", "clip": "cl_1", "anchor": 96000,
         "anchor_beat": 4}])")));
    CHECK(e.code == "track.unknown");
    CHECK(e.path == "/args/performance/launches/0/track");
    // A given object cannot be as heard.
    e = refused(p, "render.dry-run", withPerformance(j(R"({"as_heard": true})"), given()));
    CHECK(e.code == "render.not_as_heard");
    CHECK(e.path == "/args/performance");
    // start_beat or end_beat in the call (round 174 F3).
    e = refused(p, "render.mix", withPerformance(j(R"({"start_beat": 8})"), given()));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/start_beat");
    e = refused(p, "render.dry-run", withPerformance(j(R"({"end_beat": 16})"), given()));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/end_beat");
    // A clip launch with no clip, a clip of another track (F5).
    e = refused(p, "render.dry-run", withPerformance(j("{}"), launches(R"([
        {"frame": 96000, "track": "tr_1", "action": "clip", "anchor": 96000,
         "anchor_beat": 4}])")));
    CHECK(e.code == "render.performance.no_clip");
    CHECK(e.path == "/args/performance/launches/0/clip");
    e = refused(p, "render.dry-run", withPerformance(j("{}"), launches(R"([
        {"frame": 96000, "track": "tr_1", "action": "clip", "clip": "cl_2", "anchor": 96000,
         "anchor_beat": 4}])")));
    CHECK(e.code == "render.performance.clip_track");
    CHECK(e.path == "/args/performance/launches/0/clip");
    CHECK(e.hint.find("tr_2") != std::string::npos);
    // No running time; a launcher range that ends before it starts.
    json::Value none = given();
    none["frames"] = 0;
    e = refused(p, "render.dry-run", withPerformance(j("{}"), none));
    CHECK(e.code == "render.empty");
    CHECK(e.path == "/args/performance/frames");
    e = refused(p, "render.dry-run",
                withPerformance(j("{}"), j(R"({"from": "launcher", "start_beat": 8,
                                               "end_beat": 8})")));
    CHECK(e.code == "render.empty");
    CHECK(e.path == "/args/performance/end_beat");
    // Given and accepted: canonical, and not as heard because given.
    json::Value dry = p.runArgs("render.dry-run", withPerformance(j("{}"), given()));
    CHECK_FALSE(dry["as_heard"].asBool());
    CHECK(dry["not_heard_because"] == j(R"(["given"])"));
    CHECK(dry["performance"]["launches"].size() == 1);
    CHECK(dry["range"]["end_beat"].asDouble() == doctest::Approx(8));
}

TEST_CASE("a dry run with a performance renders the mix whatever the saved source (round 174 F2)") {
    Project p;
    twoTracks(p);
    p.run("project.set", R"({"render": {"source": "regions"}})");
    json::Value dry = p.runArgs("render.dry-run", withPerformance(j("{}"), given()));
    CHECK(dry["not_heard_because"] == j(R"(["given"])"));
    // operation regions in the call is still refused, naming operation.
    Error e =
        refused(p, "render.dry-run", withPerformance(j(R"({"operation": "regions"})"), given()));
    CHECK(e.code == "render.args");
    CHECK(e.path == "/args/performance");
    CHECK(e.hint.find("operation mix") != std::string::npos);
}

TEST_CASE("a looped performance's range is the song span it covers (round 174 F4)") {
    // A bar looped three times: the range ends at the loop's end (bar 2,
    // beat 4), not at three bars of running time.
    Project p;
    twoTracks(p);
    json::Value looped = j(R"({"sample_rate": 48000, "start": 0, "frames": 288000,
        "loop": {"start": 0, "end": 96000},
        "launches": [{"frame": 0, "track": "tr_1", "action": "clip", "clip": "cl_1",
                      "anchor": 0, "anchor_beat": 0}]})");
    json::Value dry = p.runArgs("render.dry-run", withPerformance(j("{}"), looped));
    CHECK(dry["range"]["start_beat"].asDouble() == doctest::Approx(0));
    CHECK(dry["range"]["end_beat"].asDouble() == doctest::Approx(4));
    CHECK(dry["performance"]["frames"].asInt() == 288000);
}

TEST_CASE("from the launcher with nothing playing renders the arrangement over the range") {
    Project p;
    twoTracks(p);
    json::Value launcher =
        p.runArgs("render.mix",
                  withPerformance(j(R"({"bit_depth": 32, "name": "launcher", "tail_s": 0})"),
                                  j(R"({"from": "launcher", "start_beat": 0, "end_beat": 4})")));
    CHECK(launcher["performance"]["launches"].size() == 0);
    CHECK(launcher["not_heard_because"] == j(R"(["from_launcher"])"));
    json::Value arrangement =
        p.run("render.mix", R"({"bounds": "custom", "start_beat": 0, "end_beat": 4,
                                "bit_depth": 32, "name": "arrangement", "tail_s": 0})");
    CHECK(leftOf(launcher) == leftOf(arrangement));
}

TEST_CASE("under a master playrate a performance's song frames play where live placed them") {
    // 0.6: bar 2 (song 96000) is device frame 160000, which live placement
    // logs; the render of the same launch given in song frames plays there.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.6, "preserve_pitch": false})");
    const model::Session s = p.app->head();
    const Live heard = playLive(s, p.app->projectDir(), kInCallback, kBlock, 48000 * 6,
                                {{20, {request("tr_1", "cl_1")}}});
    REQUIRE(heard.log.size() == 1);
    REQUIRE(heard.log[0].runningFrame == 160000);
    const auto render = [&](std::vector<LaunchLogEntry> launches, bool inSong) {
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 4.0;
        settings.blockSize = kBlock;
        settings.projectDir = p.app->projectDir();
        settings.launches = std::move(launches);
        settings.launchesInSong = inSong;
        auto r = renderOffline(s, pool, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return r->audio.channels[0];
    };
    std::vector<LaunchLogEntry> song = heard.log;
    song[0].runningFrame = heard.log[0].songFrame;
    CHECK(song[0].runningFrame == 96000);
    const std::vector<float> fromSong = render(song, true);
    CHECK(fromSong == render(heard.log, false));
    const std::int64_t to = std::min(std::int64_t(fromSong.size()),
                                     std::int64_t(heard.left.size()) - heard.latency - kBlock);
    REQUIRE(to > 200000);  // past the launch at 160000
    const auto [worst, first] = compare(heard.left, heard.latency, fromSong, 0, to);
    INFO("first differing frame " << first);
    CHECK(worst == 0.0);
}

TEST_CASE(
    "under a slow master playrate an immediate launch renders where it played (round 174 F1)") {
    // Quantize none: live places it on the next whole song sample, at or
    // after the floor, so the render's first frame reaching that sample is
    // the stream frame live switched on. At 0.25 with 126-frame blocks the
    // floor's song position is fractional (21 * 126 * 0.25 = 661.5), as at
    // 0.6 with 128-frame blocks.
    for (const auto& [rate, block] : {std::pair{0.25, 126u}, std::pair{0.6, 128u}}) {
        INFO("rate " << rate << " block " << block);
        Project p;
        twoTracks(p);
        json::Value set = j(R"({"preserve_pitch": false})");
        set["rate"] = rate;
        p.runArgs("transport.playrate.set", set);
        const model::Session s = p.app->head();
        const Live heard = playLive(s, p.app->projectDir(), kInCallback, block, 48000 * 4,
                                    {{20, {request("tr_1", "cl_1")}, model::LaunchQuantize::None}});
        REQUIRE(heard.log.size() == 1);
        // On a whole song sample: the song frame is where it switched.
        CHECK(double(heard.log[0].songFrame) <= rate * double(heard.log[0].runningFrame) + 1e-6);
        CHECK(double(heard.log[0].songFrame) > rate * double(heard.log[0].runningFrame - 1));
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 1.5;
        settings.blockSize = kBlock;
        settings.projectDir = p.app->projectDir();
        std::vector<LaunchLogEntry> song = heard.log;
        song[0].runningFrame = heard.log[0].songFrame;
        settings.launches = song;
        settings.launchesInSong = true;
        auto r = renderOffline(s, pool, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        const std::vector<float>& fromSong = r->audio.channels[0];
        const std::int64_t to = std::min(std::int64_t(fromSong.size()),
                                         std::int64_t(heard.left.size()) - heard.latency - block);
        REQUIRE(to > heard.log[0].runningFrame + 4096);
        const auto [worst, first] = compare(heard.left, heard.latency, fromSong, 0, to);
        INFO("first differing frame " << first << ", launch at " << heard.log[0].runningFrame);
        CHECK(worst == 0.0);
    }
}

TEST_CASE("a clip removed and restored within one block leaves the capture edited (G2)") {
    // A client that sends without waiting: the executors may skip the plan
    // without cl_1 and play on, though a return was recorded. The capture
    // says it is not as heard.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    p.run("launch.capture", R"({"on": true})");
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(800 * kBlock);
    p.run("clip.remove", R"({"clip": "cl_1"})");
    p.run("history.undo", "{}");
    live.until(1000 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
    CHECK_FALSE(dry["as_heard"].asBool());
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why.find("\"edited_during\"") != std::string::npos);
    // A capture with no edit is as heard, so the reason is the edit's.
    CHECK(why.find("\"began_mid_run\"") == std::string::npos);
}

namespace {
std::string playsOf(Project& p, const char* track) {
    json::Value state = p.run("launch.state", "{}");
    for (const auto& t : state["tracks"].asArray())
        if (t.find("track")->asString() == track) return t.find("plays")->asString();
    return "";
}

// cl_1 playing from bar 2, then `edit` at frame 102400, then a capture
// from 115200 to 128000, begun with no edit since.
template <class F>
json::Value laterCapture(Project& p, AppLive& live, F&& edit) {
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(800 * kBlock);
    edit();
    live.until(900 * kBlock);
    p.run("launch.capture", R"({"on": true})");
    live.until(1000 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    return p.run("render.dry-run", R"({"performance": "captured"})");
}
}  // namespace

TEST_CASE("a removal undone before the executors took it is not a return (round 172 K1)") {
    // The executor went from the plan with cl_1 to the one with it again:
    // it never returned tr_1, and acked nothing. launch.state says cl_1,
    // and a later capture with no edit of its own lacks only a play start.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 5);
    json::Value dry = laterCapture(p, live, [&] {
        p.run("clip.remove", R"({"clip": "cl_1"})");
        p.run("history.undo", "{}");  // no block between
    });
    CHECK(playsOf(p, "tr_1") == "clip");
    CHECK(EngineTestAccess::launchReturns(*p.app->engine()).empty());
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why == R"(["began_mid_run"])");
}

TEST_CASE("a removal the executors took, then its undo, is a return (round 174)") {
    // The executor returned tr_1 at the removal's next block; the undo brings
    // cl_1 back to the plan, not to the track: launch.state says the
    // arrangement, and a capture after both lacks only a play start.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    json::Value dry = laterCapture(p, live, [&] {
        p.run("clip.remove", R"({"clip": "cl_1"})");
        live.until(801 * kBlock);  // a block: the executor takes the plan
        p.run("history.undo", "{}");
    });
    CHECK(playsOf(p, "tr_1") == "arrangement");
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    CHECK(returns[0].runningFrame == 800 * std::int64_t(kBlock));
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why == R"(["began_mid_run"])");
}

TEST_CASE("a plug-in that failed before the capture leaves it not as heard (round 174 U1)") {
    // A failed bridged instance plays silence and answers ok, so no
    // processing error is counted; a render's pool plays a working one.
    host::setBridgeExecutable(BRIDGE_PATH);
    Project p;
    twoTracks(p);
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.crash"})");  // fx_3
    p.run("fx.set", R"({"fx": "fx_3", "isolation": "dedicated"})");
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    host::Instance* crash = p.app->instanceOf(*model::Id::parse("fx_3"));
    REQUIRE(crash);
    REQUIRE(crash->isolated());
    REQUIRE(crash->setParam(1, 1.0));  // crash in its next process()
    for (int i = 0; i < 2000 && !crash->failed(); ++i) {
        live.until((live.cycles + 1) * kBlock);  // stopped: plug-ins still run
        if (!crash->failed()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(crash->failed());
    p.run("launch.capture", R"({"on": true})");
    REQUIRE(p.app->transport("play"));
    live.until(live.cycles * kBlock + 20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(live.cycles * kBlock + 1000 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
    CHECK_FALSE(dry["as_heard"].asBool());
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why == R"(["plugin_failed"])");
}

TEST_CASE("a launch ack lost to a full ring leaves every later capture not as heard") {
    // cl_1's ack finds the ring full: the log misses a launch that was
    // heard. The capture running then, and one begun after, say so.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 6);
    p.run("launch.capture", R"({"on": true})");  // from the play start
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    REQUIRE(EngineTestAccess::fillLaunchRing(*p.app->engine()) > 0);
    p.run("launch.clip", R"({"clip": "cl_1"})");  // on bar 2, frame 96000
    live.backend.run(760 - 20);                   // past it, with no service
    live.cycles = 760;
    CHECK(EngineTestAccess::launchAcksDropped(*p.app->engine()) == 1);
    EngineTestAccess::emptyLaunchRing(*p.app->engine());  // only the filler
    live.until(1000 * kBlock);
    CHECK(p.app->engine()->launchLog().empty());  // the launch was not logged
    p.run("launch.capture", R"({"on": false})");
    json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
    CHECK_FALSE(dry["as_heard"].asBool());
    std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why == R"(["launch_unlogged"])");
    // Begun later: still not as heard for it.
    p.run("launch.capture", R"({"on": true})");
    live.until(1200 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    dry = p.run("render.dry-run", R"({"performance": "captured"})");
    why = json::writeCompact(dry["not_heard_because"]);
    CHECK(why.find("\"launch_unlogged\"") != std::string::npos);
}
