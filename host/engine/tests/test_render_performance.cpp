// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Rendering a launcher performance (engine.md §3.1): the render commands'
// `performance` selector, its result fields and refusals, the queue's
// resolved object, and song frames converted under a master playrate. Its
// own ctest entry.
#include <supple/audio/audio_file.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
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

// Refused, with a code the command declares (its registry entry's errors,
// which a client walks to word refusals).
Error refused(Project& p, const char* command, const json::Value& args) {
    auto r = p.bus->execute({command, args, kOrigin});
    REQUIRE_FALSE(r);
    const cmd::CommandInfo* info = p.registry.find(command);
    REQUIRE(info);
    const std::string code = r.error().code;
    INFO(command << " answered " << code);
    CHECK(std::any_of(info->errors.begin(), info->errors.end(), [&](const cmd::ErrorSpec& e) {
        return e.code == code ||
               (e.code.ends_with(".*") && code.rfind(e.code.substr(0, e.code.size() - 1), 0) == 0);
    }));
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
    // and a later capture with no edit of its own lacks only a play start
    // (and begins inside a note).
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
    // (And began_in_note: it begins inside cl_1's E3, beat 0.8 of bar 2.)
    CHECK(why == R"(["began_mid_run","began_in_note"])");
}

TEST_CASE("a removal the executors took, then its undo, is a return (round 174)") {
    // The executor returned tr_1 at the removal's next block; the undo brings
    // cl_1 back to the plan, not to the track: launch.state says the
    // arrangement, and a capture after both lacks only a play start (and
    // begins inside a note).
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
    CHECK(returns[0].runningFrame == 800 * std::int64_t(kBlock) - 64);  // tr_1's timeline
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    // (And began_in_note: it begins inside the arrangement's A3, bar 2.)
    CHECK(why == R"(["began_mid_run","began_in_note"])");
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
    // On the failure, not the clock (a crash never seen is the runner's
    // timeout), as test_app waits.
    while (!crash->failed()) {
        live.until((live.cycles + 1) * kBlock);  // stopped: plug-ins still run
        if (!crash->failed()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    CHECK_FALSE(p.run("launch.state", "{}")["unlogged"].asBool());
    p.run("launch.clip", R"({"clip": "cl_1"})");  // on bar 2, frame 96000
    // Filled after the launch, which reads the acks first (round 205 F2).
    REQUIRE(EngineTestAccess::fillLaunchRing(*p.app->engine()) > 0);
    live.backend.run(760 - 20);  // past it, with no service
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
    // launch.state says what it knows: the log may miss a launch (round
    // 178 F4); its tracks may be stale.
    CHECK(p.run("launch.state", "{}")["unlogged"].asBool());
    // Begun later: still not as heard for it.
    p.run("launch.capture", R"({"on": true})");
    live.until(1200 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    dry = p.run("render.dry-run", R"({"performance": "captured"})");
    why = json::writeCompact(dry["not_heard_because"]);
    CHECK(why.find("\"launch_unlogged\"") != std::string::npos);
}

TEST_CASE(
    "a launched track that leaves or joins the live region while stopped is a return (round 176 "
    "F1)") {
    // The anticipative executor carries a node across the live region only
    // at a continuous handoff, so a monitor change while stopped restarts
    // tr_1 on the arrangement: the callback acks it, and launch.state and
    // the next capture say so (cl_1 is not at its frame 0).
    for (const bool off : {true, false}) {
        INFO((off ? "monitor off while stopped" : "monitor on while stopped"));
        Project p;
        twoTracks(p);
        const char* monitorOn =
            R"({"track": "tr_1", "armed": true, "record_midi": "*", "monitor": "on"})";
        if (off) p.run("track.set", monitorOn);
        AppLive live(p, EngineOptions{.workers = 0}, kBlock, 48000 * 8);
        REQUIRE(p.app->transport("play"));
        live.until(live.cycles * kBlock + 20 * kBlock);
        p.run("launch.clip", R"({"clip": "cl_1"})");
        live.until(live.cycles * kBlock + 1000 * kBlock);
        CHECK(playsOf(p, "tr_1") == "clip");
        REQUIRE(p.app->transport("stop"));
        live.until(live.cycles * kBlock + 50 * kBlock);
        if (off)
            p.run("track.set", R"({"track": "tr_1", "monitor": "off"})");
        else
            p.run("track.set", monitorOn);
        // Stopped, until() does not wait for the renderer: on the adoption,
        // not the clock, then cycles for the callback's handoff.
        while (!p.app->engine()->awaitPlan()) {
        }
        live.until(live.cycles * kBlock + 100 * kBlock);
        CHECK(playsOf(p, "tr_1") == "arrangement");
        const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
        REQUIRE(returns.size() == 1);
        CHECK(returns[0].track == id("tr_1"));
        p.run("launch.capture", R"({"on": true})");
        REQUIRE(p.app->transport("play"));
        live.until(live.cycles * kBlock + 400 * kBlock);
        p.run("launch.capture", R"({"on": false})");
        live.until(live.cycles * kBlock + 10 * kBlock);
        json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
        const std::string launches = json::writeCompact(dry["performance"]["launches"]);
        INFO(launches);
        CHECK(launches.find("cl_1") == std::string::npos);
    }
}

TEST_CASE("a return on a latency-delayed track is logged on its timeline (round 176 F2)") {
    // tr_1 is delayed 64 frames by tr_2's latent gain. Its return switches
    // its input at stream S, which is S - 64 on its timeline, where its
    // launches are placed: the whole log replayed equals live.
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    p.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_2", "notation": "voice bass; 1: D3:q r:q r:h"})");
    p.run("clip.remove", R"({"clip": "cl_1"})");
    const model::Session removed = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const Step scene{20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}};
        const Live live = playLive(s, p.app->projectDir(), options, 128, frames,
                                   {scene, {(96000 + 20000) / 128, {}, {}, &removed}});
        REQUIRE(live.log.size() == 3);
        CHECK(live.log[2].track == id("tr_1"));
        CHECK(live.log[2].action == LaunchLogEntry::Action::Arrangement);
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 5.0;
        settings.projectDir = p.app->projectDir();
        settings.launches = live.log;
        auto all = renderOffline(s, pool, settings);
        REQUIRE(all);
        const auto [worst, first] =
            compare(live.left, live.latency, all->audio.channels[0], 0, 4 * 48000);
        INFO("first differing frame " << first << ", return at " << live.log[2].runningFrame);
        CHECK(worst == 0.0);
    }
}

TEST_CASE(
    "a join the renderer carried keeps its clip when the callback takes it out of step (round 178 "
    "F1)") {
    // The renderer adopts the monitor-on plan while playing and carries cl_1
    // into tr_1's live node; the callback takes it after a stop, or a seek,
    // not continuously. tr_1 plays cl_1 on, and no return is logged.
    for (const bool seek : {false, true}) {
        INFO((seek ? "a seek" : "a stop") << " between the two adoptions");
        Project p;
        twoTracks(p);
        AppLive live(p, EngineOptions{.workers = 0}, kBlock, 48000 * 8);
        Engine& e = *p.app->engine();
        REQUIRE(p.app->transport("play"));
        live.until(live.cycles * kBlock + 20 * kBlock);
        p.run("launch.clip", R"({"clip": "cl_1"})");
        live.until(live.cycles * kBlock + 1000 * kBlock);
        REQUIRE(playsOf(p, "tr_1") == "clip");
        p.run("track.set",
              R"({"track": "tr_1", "armed": true, "record_midi": "*", "monitor": "on"})");
        while (!e.awaitPlan()) {
        }  // queued for the callback; no callback cycle since
        REQUIRE(EngineTestAccess::latestPlan(e).liveCarried);
        if (seek)
            p.run("transport.seek", R"({"seconds": 1})");
        else
            REQUIRE(p.app->transport("stop"));
        live.until(live.cycles * kBlock + 100 * kBlock);
        // Live nodes are the callback's, which runs on this thread (Manual).
        const detail::Node* n = detail::findNode(EngineTestAccess::latestPlan(e), id("tr_1"));
        REQUIRE(n);
        CHECK(n->launch.kind == detail::LaunchState::Kind::Clip);
        CHECK(EngineTestAccess::launchReturns(e).empty());
        CHECK(playsOf(p, "tr_1") == "clip");
    }
}

TEST_CASE("a rate handoff the callback skips leaves each class move acked once (round 180 F1)") {
    // Under a playrate the callback skips every handoff of an old epoch. A
    // is the callback's plan, B the skipped one(s), C the plan it adopts
    // after a seek; L live, R rendered. A node live in A is the callback's
    // own (adoptLive carries it on); one rendered in A is the renderer's,
    // and a skipped plan's live region is never played, carried or not.
    struct Case {
        const char* name;
        bool liveFirst;  // tr_1 monitored from the start: live in A
        bool playing;    // B adopted by the renderer while playing (carried)
        bool returned;   // one return logged, else none and cl_1 plays on
        bool liveInC;
    };
    for (const Case& k : {Case{"R-L-L: joined in B (carried) and C", false, true, true, true},
                          Case{"R-L-R: joined in B only", false, false, true, false},
                          Case{"L-R-L: left in B only", true, false, false, true},
                          Case{"L-L-R: left in C only", true, false, true, false}}) {
        const std::string name = k.name;
        INFO("case " << name);
        Project p;
        twoTracks(p);
        p.run("transport.playrate.set", R"({"rate": 0.6, "preserve_pitch": false})");
        const char* monitorOn =
            R"({"track": "tr_1", "armed": true, "record_midi": "*", "monitor": "on"})";
        const char* monitorOff = R"({"track": "tr_1", "monitor": "off"})";
        if (k.liveFirst) p.run("track.set", monitorOn);
        AppLive live(p, EngineOptions{.workers = 0}, kBlock, 48000 * 8);
        Engine& e = *p.app->engine();
        REQUIRE(p.app->transport("play"));
        live.until(live.cycles * kBlock + 20 * kBlock);
        p.run("launch.clip", R"({"clip": "cl_1"})");
        live.until(live.cycles * kBlock + 1500 * kBlock);  // bar 2: device 160000 at 0.6
        REQUIRE(playsOf(p, "tr_1") == "clip");
        // Each edit's plan queued for the callback (no callback cycle runs
        // here: Manual); the seek then makes them old.
        const auto liveIn = [](const detail::Plan* plan) {
            const detail::Node* n = detail::findNode(*plan, id("tr_1"));
            REQUIRE(n);
            return n->live;
        };
        const auto queued = [&](const char* edit, const char* args) {
            INFO("edit " << edit << " " << args);
            const detail::Plan* before = &EngineTestAccess::latestPlan(e);
            p.run(edit, args);
            while (!e.awaitPlan()) {
            }
            const detail::Plan& now = EngineTestAccess::latestPlan(e);
            REQUIRE(&now != before);
            return &now;
        };
        if (k.playing) {
            p.run("track.set", monitorOn);
            const detail::Plan& b = EngineTestAccess::latestPlan(e);
            REQUIRE(b.rateEnabled);
            const std::int64_t ahead = EngineTestAccess::aheadFrames(e);
            REQUIRE(ahead > kBlock);
            // The renderer reaches B's hand once the callback has played to
            // hand - ahead; the callback would take B itself at the hand.
            while (EngineTestAccess::played(e) < b.handAt - ahead) {
                while (!e.awaitRendered(kBlock)) {
                }
                live.backend.run(1);
                ++live.cycles;
            }
            REQUIRE(EngineTestAccess::played(e) < b.handAt);
            while (!e.awaitPlan()) {
            }
            REQUIRE(&EngineTestAccess::latestPlan(e) == &b);
            REQUIRE(b.liveCarried);
        } else {
            // Stopped, the callback takes the stop's plans: A. Then B.
            REQUIRE(p.app->transport("stop"));
            live.until(live.cycles * kBlock + 50 * kBlock);
            REQUIRE(playsOf(p, "tr_1") == "clip");
            REQUIRE(liveIn(&EngineTestAccess::latestPlan(e)) == k.liveFirst);
            if (!k.liveFirst) {
                CHECK(liveIn(queued("track.set", monitorOn)));
                CHECK_FALSE(liveIn(queued("track.set", monitorOff)));
            } else if (!k.returned) {
                CHECK_FALSE(liveIn(queued("track.set", monitorOff)));
                CHECK(liveIn(queued("track.set", monitorOn)));
            } else {
                CHECK(liveIn(queued("track.set", R"({"track": "tr_2", "armed": true})")));
                CHECK_FALSE(liveIn(queued("track.set", monitorOff)));
            }
        }
        const std::uint64_t old = EngineTestAccess::latestPlan(e).handEpoch;
        const detail::Plan* c = queued("transport.seek", R"({"seconds": 1})");
        REQUIRE(c->handEpoch > old);
        live.until(live.cycles * kBlock + 100 * kBlock);
        REQUIRE(EngineTestAccess::callbackGeneration(e) == c->generation);
        const detail::Node* n = detail::findNode(*c, id("tr_1"));
        REQUIRE(n);
        REQUIRE(n->live == k.liveInC);
        // A live node is the callback's (this thread).
        if (n->live)
            CHECK(n->launch.kind == (k.returned ? detail::LaunchState::Kind::Arrangement
                                                : detail::LaunchState::Kind::Clip));
        const auto returns = EngineTestAccess::launchReturns(e);
        if (k.returned) {
            CHECK(returns.size() == 1);
            if (!returns.empty()) CHECK(returns[0].track == id("tr_1"));
            CHECK(playsOf(p, "tr_1") == "arrangement");
        } else {
            CHECK(returns.empty());
            CHECK(playsOf(p, "tr_1") == "clip");
        }
    }
}

TEST_CASE("an engine stop and start forget the handoff the callback skipped (round 180 F2)") {
    // classBase_ points into plans_, which stop() frees; a restarted engine
    // must not judge its first handoff against it.
    Project p;
    twoTracks(p);
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, EngineOptions{.workers = 0});
    for (int run = 0; run < 2; ++run) {
        INFO("run " << run);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        CHECK(EngineTestAccess::classBase(engine) == nullptr);
        REQUIRE(engine.load(p.app->head(), p.app->projectDir()));
        while (!engine.awaitPlan()) {
        }
        backend.run(4);
        // As a skip leaves it: the latest plan, never adopted.
        EngineTestAccess::skipped(engine);
        REQUIRE(EngineTestAccess::classBase(engine) != nullptr);
        engine.stop();
        CHECK(EngineTestAccess::classBase(engine) == nullptr);
        if (run == 0)  // stale, before the restart
            EngineTestAccess::skipped(engine, reinterpret_cast<detail::Plan*>(&media));
    }
}

TEST_CASE("a launch ack lost just before the capture ends still marks it (round 178 F3)") {
    // The drop is not serviced before launch.capture off closes the capture.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 6);
    p.run("launch.capture", R"({"on": true})");
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");  // on bar 2, frame 96000
    // Filled after the launch, which reads the acks first (round 205 F2).
    REQUIRE(EngineTestAccess::fillLaunchRing(*p.app->engine()) > 0);
    live.backend.run(760 - 20);  // past it, with no service
    live.cycles = 760;
    REQUIRE(EngineTestAccess::launchAcksDropped(*p.app->engine()) == 1);
    EngineTestAccess::emptyLaunchRing(*p.app->engine());  // only the filler
    p.run("launch.capture", R"({"on": false})");          // no service since the drop
    live.until(800 * kBlock);
    json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why.find("\"launch_unlogged\"") != std::string::npos);
}

namespace {
// Each code an Error(...) names in `files` (its first argument's string
// literals, to its first comma at depth 0), with the file it is in.
std::map<std::string, std::string> codesIn(std::initializer_list<const char*> files) {
    const std::filesystem::path root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    REQUIRE(std::filesystem::exists(root / "host/engine/src/offline.cpp"));
    std::map<std::string, std::string> out;
    for (const char* file : files) {
        std::ifstream in(root / file);
        REQUIRE_MESSAGE(in, file);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        for (std::size_t at = text.find("Error("); at != std::string::npos;
             at = text.find("Error(", at + 6)) {
            if (at > 0 &&
                (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_'))
                continue;  // argsError(, launchesError(: helpers, scanned where they call Error(
            std::size_t end = at + 6;
            for (int depth = 0; end < text.size(); ++end) {
                const char c = text[end];
                if (c == '(') ++depth;
                if (c == ')' && depth-- == 0) break;
                if (c == ',' && depth == 0) break;
            }
            static const std::regex code(R"re("([a-z0-9_]+\.[a-z0-9_.]+)")re");
            const std::string first = text.substr(at + 6, end - at - 6);
            for (std::sregex_iterator i(first.begin(), first.end(), code), e; i != e; ++i)
                if (std::string((*i)[1]).rfind("schema.", 0) != 0) out.emplace((*i)[1], file);
        }
    }
    return out;
}

// Declared, exactly or by a family ("json.parse.*" declares json.parse.<what>).
bool declares(const cmd::CommandInfo& info, const std::string& c) {
    return std::any_of(info.errors.begin(), info.errors.end(), [&](const cmd::ErrorSpec& e) {
        return e.code == c ||
               (e.code.ends_with(".*") && c.rfind(e.code.substr(0, e.code.size() - 1), 0) == 0);
    });
}
}  // namespace

TEST_CASE("each render.* command declares every code its sources answer (round 183 F1)") {
    // A lint per command, so that no command leans on another's entry: a
    // client words a refusal from the registry entry of the command it ran
    // (engine.md §3.1). Schema codes are every command's and not listed. A
    // new code fails here until each command that answers it declares it.
    Project p;
    const auto info = [&](const char* name) {
        const cmd::CommandInfo* found = p.registry.find(name);
        REQUIRE_MESSAGE(found, name);
        return found;
    };
    std::vector<std::string> undeclared;
    // The renders: the render sources, source preparation, the offline pass,
    // the file and audio writers, and the JSON parser (the render default).
    const auto renderCodes =
        codesIn({"host/engine/src/render_commands.cpp", "host/engine/src/render_performance.cpp",
                 "host/engine/src/render_chain.cpp", "host/engine/src/offline.cpp",
                 "host/engine/src/prepared_sources.cpp", "host/engine/src/stretch.cpp",
                 "libs/core-base/src/files.cpp", "libs/audio-file/src/audio_file.cpp",
                 "libs/audio-file/src/flac_encode.cpp", "libs/audio-file/src/metadata.cpp",
                 "libs/audio-codecs/src/mp3.cpp", "libs/audio-codecs/src/vorbis.cpp",
                 "libs/core-json/src/parse.cpp"});
    // Answered in those files by other commands only.
    const std::set<std::string> others = {
        "render.queue_empty",    "render.queue_unknown", "render.preset_name",
        "render.preset_unknown",  // render.preset.* (presetTarget, the default's load)
        "freeze.tail_limit",     "freeze.tail_changed",  "freeze.tail_pending",  // track.freeze
        "file.exists",     // files.cpp's create-exclusive, which no render.* uses
        "base64.invalid",  // files.cpp's base64 decoder, likewise
    };
    // What one render answers and another does not (engine.md §3.1).
    const auto performance = [](const std::string& c) {  // not render.regions
        return c.rfind("render.performance.", 0) == 0 ||
               std::set<std::string>{"launch.capture",  "cl.unknown",          "track.frozen",
                                     "launch.playrate", "render.not_as_heard", "launch.memory"}
                   .count(c);
    };
    const std::set<std::string> runOnly = {"render.stale_target", "engine.start_failed",
                                           "job.cancelled", "launch.memory"};  // not the dry run
    for (const char* name : {"render.mix", "render.stems", "render.regions", "render.dry-run"}) {
        const std::string_view which(name);
        for (const auto& [c, file] : renderCodes) {
            if (others.count(c)) continue;
            if (which == "render.regions" && performance(c)) continue;
            if (which == "render.dry-run" && runOnly.count(c)) continue;
            if ((which == "render.mix" || which == "render.stems") && c == "region.unknown")
                continue;
            if (!declares(*info(name), c))
                undeclared.push_back(std::string(name) + ": " + c + " (" + file + ")");
        }
    }
    // The presets and render.settings: the preset files, the JSON parser
    // and the file reads and writes, against what those commands declare
    // together (each command's own refusals are tested in test_render.cpp).
    for (const auto& [c, file] :
         codesIn({"host/engine/src/render_settings.cpp", "libs/core-base/src/files.cpp",
                  "libs/core-json/src/parse.cpp"})) {
        if (c == "file.exists" || c == "base64.invalid") continue;
        bool any = false;
        for (const char* name : {"render.preset.list", "render.preset.save", "render.preset.load",
                                 "render.preset.delete", "render.settings"})
            any = any || declares(*info(name), c);
        if (!any) undeclared.push_back("render.preset.*: " + c + " (" + file + ")");
    }
    // The union above cannot see one command drop a code another declares
    // (round 185 S1, the reviewer's MP1 and MP2), so per command: each that
    // reads a preset or the default declares the reads, render.preset.save
    // the writes too, and render.settings resolveFor's own refusals.
    const std::vector<std::string> reads = {"file.open", "file.read", "file.too_large",
                                            "json.parse.trailing_comma"};
    const std::vector<std::string> writes = {"render.preset_write", "file.write", "file.sync",
                                             "file.sync_directory", "file.sync_published"};
    const std::vector<std::string> resolver = {"render.source", "history.version", "branch.unknown",
                                               "render.args", "render.secondary"};
    const std::vector<std::pair<const char*, std::vector<const std::vector<std::string>*>>> sets = {
        {"render.preset.list", {&reads}},
        {"render.preset.load", {&reads}},
        {"render.preset.save", {&reads, &writes}},
        {"render.settings", {&reads, &resolver}}};
    for (const auto& [name, groups] : sets)
        for (const auto* group : groups)
            for (const std::string& c : *group)
                if (!declares(*info(name), c))
                    undeclared.push_back(std::string(name) + ": " + c + " (per command)");
    // The queue plans a render at add and plans and runs it at run.
    for (const auto& [queue, render] : {std::pair{"render.queue.add", "render.dry-run"},
                                        std::pair{"render.queue.run", "render.mix"}})
        for (const cmd::ErrorSpec& e : info(render)->errors)
            if (!declares(*info(queue), e.code))
                undeclared.push_back(std::string(queue) + ": " + e.code + " (as " + render + ")");
    CHECK(declares(*info("render.queue.run"), "render.queue_empty"));
    CHECK(declares(*info("render.preset.delete"), "render.preset_write"));  // round 183 S2
    std::string list;
    for (const std::string& u : undeclared) list += u + "\n";
    INFO("undeclared:\n" << list);
    CHECK(undeclared.empty());
}

namespace {
// tr_1 (fadingClips) before its fader, rendered from song sample `start`,
// replaying `log`: an offline render, as a performance's or a capture's.
std::vector<float> tr1From(Project& p, std::int64_t start, std::size_t frames,
                           std::vector<LaunchLogEntry> log) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.startSeconds = double(start) / 48000;
    settings.endSeconds = settings.startSeconds + double(frames) / 48000 + 0.05;
    settings.blockSize = 64;
    settings.projectDir = p.app->projectDir();
    settings.track = id("tr_1");
    settings.tap = model::Tap::PreFader;
    settings.launches = std::move(log);
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    REQUIRE(r->audio.channels[0].size() >= frames);
    return r->audio.channels[0];
}
LaunchLogEntry tr1Clip(const char* clip, std::int64_t anchor, std::int64_t frame = 0) {
    LaunchLogEntry e;
    e.track = id("tr_1");
    e.action = LaunchLogEntry::Action::Clip;
    e.clip = id(clip);
    e.anchorSample = anchor;
    e.anchorBeat = double(anchor) / 24000;  // 120 BPM
    e.runningFrame = frame;
    return e;
}
// The first frame in [from, to) where `out` is not `expected(i)`, or -1.
template <class F>
std::int64_t firstWrong(const std::vector<float>& out, std::int64_t from, std::int64_t to,
                        F&& expected) {
    for (std::int64_t i = from; i < to; ++i)
        if (out[std::size_t(i)] != expected(i)) return i;
    return -1;
}
}  // namespace

TEST_CASE(
    "a render begins as a relocation: a track's first entry anchored off its start plays by its "
    "phase with no crossfade; one anchored at it crossfades (faults A and B; round 218)" *
    doctest::description("rt")) {
    // fadingClips: tr_1's ramps, each frame its source position; cl_2 a
    // 48000-frame loop, cl_3 a one-shot here. A render from 100000.
    Project p;
    fadingClips(p);
    p.run("clip.set", R"({"clip": "cl_3", "mode": "once"})");
    constexpr std::int64_t kStart = 100000, kN = 120000;
    const auto loopAt = [](std::int64_t anchor) {
        return [anchor](std::int64_t i) {
            return rampAt(((kStart + i - anchor) % 48000 + 48000) % 48000);
        };
    };
    SUBCASE("a loop clip anchored after the start (a seek back carried it): by its phase") {
        const auto out = tr1From(p, kStart, kN, {tr1Clip("cl_2", kStart + 30000)});
        CHECK(firstWrong(out, 0, kN, loopAt(kStart + 30000)) == -1);
    }
    SUBCASE("a loop clip anchored inside its crossfade before the start: by its phase, faded") {
        const auto out = tr1From(p, kStart, kN, {tr1Clip("cl_2", kStart - 100)});
        CHECK(firstWrong(out, 0, kN, loopAt(kStart - 100)) == -1);
    }
    SUBCASE("a one-shot anchored after the start: silent until its anchor, no crossfade there") {
        const auto out = tr1From(p, kStart, kN, {tr1Clip("cl_3", kStart + 30000)});
        CHECK(firstWrong(out, 0, 30000, [](std::int64_t) { return 0.f; }) == -1);
        CHECK(firstWrong(out, 30000, 30000 + 48000,
                         [](std::int64_t i) { return rampAt(i - 30000); }) == -1);
        CHECK(firstWrong(out, 30000 + 48000, kN, [](std::int64_t) { return 0.f; }) == -1);
    }
    SUBCASE("a launch anchored at the start crossfades in, as live launches there") {
        const auto out = tr1From(p, kStart, kN, {tr1Clip("cl_2", kStart)});
        CHECK(out[0] < rampAt(0));
        CHECK(out[200] < rampAt(200));
        CHECK(firstWrong(out, 240, kN, loopAt(kStart)) == -1);
    }
    SUBCASE("a switch at the first frame after a carried entry crossfades from it") {
        const auto out =
            tr1From(p, kStart, kN, {tr1Clip("cl_2", kStart - 5000), tr1Clip("cl_3", kStart)});
        // Both at frame 120, half way: cl_2 at its phase 5120, cl_3 at 120.
        CHECK(out[120] != rampAt(5120));
        CHECK(out[120] != rampAt(120));
        CHECK(out[120] > 0.f);
        CHECK(firstWrong(out, 240, 48000, [](std::int64_t i) { return rampAt(i); }) == -1);
    }
}

TEST_CASE("a capture begun while playing inside a sounding note says began_in_note (fault D)") {
    // twoTracks: tr_1's sine plays the arrangement's whole notes over bars
    // 1-3 (to 288000) and cl_1 (C3:e E3:e G3:q r:h, a bar). Live holds a
    // note sounding where a capture switched on while playing begins; its
    // replay, starting inside the note, never starts it: not as heard for
    // that too (after began_mid_run). From a play start live starts
    // afresh: no note sounds on, nothing to say.
    const auto reasons = [](Project& p) {
        json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
        INFO("starts at " << dry["performance"]["start"].asInt());
        std::vector<std::string> out;
        if (const json::Value* why = dry.find("not_heard_because"))
            for (const json::Value& r : why->asArray()) out.push_back(r.asString());
        return out;
    };
    using Reasons = std::vector<std::string>;
    SUBCASE("inside an arrangement note, and past the last one") {
        for (const std::int64_t at : {48000, 300032}) {
            INFO("switched on at " << at);
            Project p;
            twoTracks(p);
            AppLive live(p, kInCallback, kBlock, 48000 * 9);
            REQUIRE(p.app->transport("play"));
            live.until(at);
            p.run("launch.capture", R"({"on": true})");
            live.until(at + 9600);
            const Reasons expected =
                at < 288000 ? Reasons{"began_mid_run", "began_in_note"} : Reasons{"began_mid_run"};
            CHECK(reasons(p) == expected);
        }
    }
    SUBCASE("inside a note of the clip the track plays, and in its rest over the arrangement") {
        // cl_1 from bar 2 (96000): its G3 over beats 1-2 (120000-144000);
        // over beats 2-4 a rest, where the arrangement's A3 would sound.
        for (const std::int64_t at : {132000, 168000}) {
            INFO("switched on at " << at);
            Project p;
            twoTracks(p);
            AppLive live(p, kInCallback, kBlock, 48000 * 9);
            REQUIRE(p.app->transport("play"));
            live.until(20 * kBlock);
            p.run("launch.clip", R"({"clip": "cl_1"})");
            live.until(at);
            p.run("launch.capture", R"({"on": true})");
            live.until(at + 4800);
            const Reasons expected =
                at < 144000 ? Reasons{"began_mid_run", "began_in_note"} : Reasons{"began_mid_run"};
            CHECK(reasons(p) == expected);
        }
    }
    SUBCASE("fixed as the capture began: removing the notes after it changes nothing") {
        // round 220 F2: the fact was judged from the newest plan at each read.
        Project p;
        twoTracks(p);
        AppLive live(p, kInCallback, kBlock, 48000 * 9);
        REQUIRE(p.app->transport("play"));
        live.until(48000);
        p.run("launch.capture", R"({"on": true})");
        live.until(48000 + 9600);
        p.run("launch.capture", R"({"on": false})");
        live.until(48000 + 9600 + 1280);
        CHECK(reasons(p) == Reasons{"began_mid_run", "began_in_note"});
        const model::Session now = p.app->head();
        for (const auto& item : now.items)
            if (item.value->track == id("tr_1")) {
                json::Value args = json::Value::object();
                args["item"] = item.id.str();
                p.run("item.remove", json::writeCompact(args));
            }
        live.until(48000 + 9600 + 1280 * 3);
        const auto view = p.app->launchCapture();
        REQUIRE(view);
        CHECK(view->capture.facts.beganInNote);
        CHECK(reasons(p) == Reasons{"began_mid_run", "began_in_note"});
    }
    SUBCASE("a note the sustain pedal holds past its off, arrangement and clip") {
        // round 220 F3: G3 ends at beat 3 of bar 1 (48000) with the pedal
        // (CC 64) down to bar 2 beat 3 (144000): on an instrument that
        // honours it the note sounds on. The clip (launched on bar 2,
        // 96000) holds its G3 (to 120000) with the pedal to its beat 4
        // (168000). Past the pedal, nothing; without it, nothing.
        struct Case {
            const char* what;
            bool clip, pedal;
            std::int64_t at;
            bool inNote;
        };
        const Case cases[] = {
            {"arrangement, pedal down past the off", false, true, 72000, true},
            {"arrangement, after the pedal is let up", false, true, 150016, false},
            {"arrangement, no pedal", false, false, 72000, false},
            {"clip, pedal down past the off", true, true, 144000, true},
            {"clip, after the pedal is let up", true, true, 180000, false},
            {"clip, no pedal", true, false, 144000, false}};
        for (const Case& k : cases) {
            INFO(k.what);
            Project p;
            p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
            const std::string lane = k.pedal ? "\nlane cc sustain ch 1\n1.1 127; 2.3 0" : "";
            const std::string clipLane = k.pedal ? "\nlane cc sustain ch 1\n1.1 127; 1.4 0" : "";
            json::Value item = j(R"({"track": "tr_1", "bar": 1})");
            item["notation"] = k.clip ? std::string("voice bass; 1: r:w | r:w | r:w")
                                      : "voice bass; 1: G3:h r:h | r:w | r:w" + lane;
            p.runArgs("item.add", item);
            json::Value clip = j(R"({"track": "tr_1", "scene": "sc_1"})");
            clip["notation"] = "voice bass; 1: G3:q r:q r:h" + clipLane;
            p.runArgs("clip.add", clip);
            AppLive live(p, kInCallback, kBlock, 48000 * 9);
            REQUIRE(p.app->transport("play"));
            live.until(20 * kBlock);
            if (k.clip) p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 2
            live.until(k.at);
            p.run("launch.capture", R"({"on": true})");
            live.until(k.at + 4800);
            CHECK(reasons(p) == (k.inNote ? Reasons{"began_mid_run", "began_in_note"}
                                          : Reasons{"began_mid_run"}));
        }
    }
    SUBCASE("from a play start inside an arrangement note: as heard") {
        Project p;
        twoTracks(p);
        AppLive live(p, kInCallback, kBlock, 48000 * 9);
        p.run("transport.seek", R"({"seconds": 1})");
        p.run("launch.capture", R"({"on": true})");
        REQUIRE(p.app->transport("play"));
        live.until(9600);
        CHECK(reasons(p).empty());
    }
}
