// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A clip launcher capture is kept across an engine stop, a device restart and
// a device failure, running or ended (ADR-0097, revision 2026-11-24): it
// stays committable, committing it writes what committing it before would
// have, and the stop's answer (or the device event) names it while it is
// uncommitted; a recovery seek's end keeps it as well. Its own ctest entry.
#include <optional>
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
constexpr std::uint32_t kBlock = 128;

// Plays, captures from frame 2560, launches cl_1 and cl_2 (bar 2, frame
// 96000) and plays on to `until`; `endFirst` ends the capture there.
void take(Project& p, AppLive& live, std::int64_t until, bool endFirst) {
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.capture", R"({"on": true})");
    live.until(40 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    p.run("launch.clip", R"({"clip": "cl_2"})");
    live.until(until);
    if (endFirst) p.run("launch.capture", R"({"on": false})");
}

std::string warningsOf(const json::Value& v) {
    std::string out;
    if (const auto* w = v.find("warnings"))
        for (const auto& line : w->asArray()) out += line.asString() + "\n";
    return out;
}

// The same take committed before any stop: what a kept capture must commit.
json::Value twinCommit(std::int64_t until, bool endFirst) {
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    take(p, live, until, endFirst);
    json::Value out = json::Value::object();
    out["commit"] = p.run("launch.commit", "{}");
    (void)live.stop();
    return out;
}
}  // namespace

TEST_CASE("an engine stop keeps the capture, running or ended, committable after it") {
    for (const bool endFirst : {false, true})
        for (const bool restart : {false, true}) {
            INFO("ended first " << endFirst << ", committed after a restart " << restart);
            const json::Value twin = twinCommit(150016, endFirst);
            Project p;
            twoTracks(p);
            std::optional<AppLive> live;
            live.emplace(p, kInCallback, kBlock, 48000 * 4);
            take(p, *live, 150016, endFirst);
            const json::Value stopped = p.run("engine.stop", "{}");
            const std::string warning = warningsOf(stopped);
            INFO(warning);
            CHECK(warning.find("2 launches") != std::string::npos);
            CHECK(warning.find("kept uncommitted") != std::string::npos);
            CHECK((warning.find("was running") != std::string::npos) == !endFirst);
            const auto kept = p.app->launchCapture();
            REQUIRE(kept);
            CHECK(kept->kept);
            CHECK(kept->endedBy ==
                  (endFirst ? EngineApp::CaptureEnd::None : EngineApp::CaptureEnd::EngineStop));
            // With the engine stopped, launch.state answers for it.
            json::Value state = p.run("launch.state", "{}");
            CHECK(state["capture_kept"].asBool());
            CHECK_FALSE(state["capturing"].asBool());
            if (restart) {
                live.emplace(p, kInCallback, kBlock, 48000);
                CHECK(p.run("launch.state", "{}")["capture_kept"].asBool());
            }
            json::Value got = json::Value::object();
            got["commit"] = p.run("launch.commit", "{}");
            CHECK(got == twin);
            // Committed: kept still, and the next stop names nothing.
            CHECK_FALSE(p.app->captureUncommitted());
            if (!restart) live.emplace(p, kInCallback, kBlock, 48000);
            CHECK(warningsOf(p.run("engine.stop", "{}")).empty());
            live.reset();
        }
}

TEST_CASE("a device restart and a device failure keep the capture") {
    for (const bool endFirst : {false, true})
        for (const bool fails : {false, true}) {
            INFO("ended first " << endFirst << ", the device fails " << fails);
            const json::Value twin = twinCommit(150016, endFirst);
            Project p;
            twoTracks(p);
            p.app->setFixedClock(1'000'000);
            AppLive live(p, kInCallback, kBlock, 48000 * 4);
            take(p, live, 150016, endFirst);
            (void)p.app->takeEvents();
            if (fails) live.backend.failStarts(1000);
            live.backend.requestRestart("the device went away");
            p.app->tick();  // stops the engine, keeping the capture
            std::string announced;
            for (auto& [topic, payload] : p.app->takeEvents())
                if (topic == "engine.restarting") announced = warningsOf(payload);
            INFO(announced);
            CHECK(announced.find("kept uncommitted") != std::string::npos);
            REQUIRE(p.app->launchCapture());
            CHECK(p.app->launchCapture()->endedBy ==
                  (endFirst ? EngineApp::CaptureEnd::None : EngineApp::CaptureEnd::Device));
            if (fails) {
                // No reopen succeeds: the restart gives up, and says so.
                for (std::uint64_t t : {1'000'250ull, 1'010'000ull, 1'031'000ull}) {
                    p.app->setFixedClock(t);
                    p.app->tick();
                }
                std::string reported;
                for (auto& [topic, payload] : p.app->takeEvents())
                    if (topic == "engine.error" && payload.isObject())
                        reported += warningsOf(payload);
                CHECK(reported.find("kept uncommitted") != std::string::npos);
                CHECK(p.app->engine() == nullptr);
            } else {
                p.app->tick();  // the retry, at once
                CHECK(p.app->engine() != nullptr);
            }
            json::Value got = json::Value::object();
            got["commit"] = p.run("launch.commit", "{}");
            CHECK(got == twin);
            if (p.app->engine()) (void)live.stop();
        }
}

TEST_CASE("a kept capture at another device rate commits where it was heard") {
    // The restart grants 44100: the kept capture keeps its own 48000, which
    // launch.commit converts by (performance.sample_rate).
    json::Value twin = twinCommit(150016, false);
    Project p;
    twoTracks(p);
    p.app->setFixedClock(1'000'000);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    take(p, live, 150016, false);
    live.backend.grantRate(44100);
    live.backend.requestRestart("the device's rate changed");
    p.app->tick();
    p.app->tick();
    REQUIRE(p.app->engine() != nullptr);
    CHECK(p.app->engine()->config().sampleRate == 44100.0);
    REQUIRE(p.app->launchCapture());
    CHECK(p.app->launchCapture()->sampleRate == 48000.0);
    json::Value commit = p.run("launch.commit", "{}");
    for (const char* key : {"from", "to", "tracks", "sections"})
        CHECK(commit[key] == twin["commit"][key]);
    (void)live.stop();
}

TEST_CASE("a newer capture replaces an uncommitted one only once it begins, and says so") {
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 6);
    take(p, live, 150016, true);
    const auto first = p.app->launchCapture();
    REQUIRE(first);
    REQUIRE(p.run("transport.stop", "{}").isObject());
    live.until(151040);
    // Stopped, a capture begins where playing starts: it has not yet.
    const std::string said = warningsOf(p.run("launch.capture", R"({"on": true})"));
    INFO(said);
    CHECK(said.find("replaces the capture from beat") != std::string::npos);
    CHECK(said.find("2 launches") != std::string::npos);
    CHECK(said.find("version " + std::to_string(first->capture.version)) != std::string::npos);
    p.run("launch.capture", R"({"on": false})");  // never begun: the first is still there
    auto still = p.app->launchCapture();
    REQUIRE(still);
    CHECK(still->capture.frames == first->capture.frames);
    CHECK(still->capture.launches.size() == 2);
    // One that begins replaces it.
    p.run("launch.capture", R"({"on": true})");
    REQUIRE(p.app->transport("play"));
    live.until(160128);
    const auto newer = p.app->launchCapture();
    REQUIRE(newer);
    CHECK_FALSE(newer->kept);
    CHECK(newer->capture.on);
    (void)live.stop();
}

TEST_CASE("a recovery seek ends a running capture, which stays committable") {
    // A preparation late three times (the test hook, no clock) is recovered
    // by a prepared locate at the consumed position, which like any seek
    // ends a running capture (STATUS). The capture is kept: its launches
    // are all there, launch.commit writes them, and a stop names nothing
    // after that.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    AppLive live(p, kInCallback, kBlock, 48000 * 6);
    take(p, live, 150016, false);
    REQUIRE(p.run("launch.state", "{}")["capturing"].asBool());
    lateAttemptsForTests(*p.app->engine(), 3);
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    lateAttemptsForTests(*p.app->engine(), 0);
    live.until(160000);
    CHECK_FALSE(p.run("launch.state", "{}")["capturing"].asBool());  // the seek ended it
    const auto ended = p.app->launchCapture();
    REQUIRE(ended);
    CHECK(ended->capture.launches.size() == 2);
    CHECK(ended->capture.frames > 0);
    CHECK(p.app->captureUncommitted());
    const json::Value commit = p.run("launch.commit", "{}");
    CHECK(commit.isObject());
    CHECK_FALSE(p.app->captureUncommitted());
    CHECK(warningsOf(p.run("engine.stop", "{}")).empty());
}

TEST_CASE("launch.state says both a capture armed and a kept one") {
    // Review round 168 F2: after a stop with one uncommitted, a new capture
    // armed while stopped has not begun, so the kept one is still the one
    // launch.commit takes; launch.state says capturing (the new one, armed)
    // and capture_kept (the old) at once.
    Project p;
    twoTracks(p);
    std::optional<AppLive> live;
    live.emplace(p, kInCallback, kBlock, 48000 * 4);
    take(p, *live, 150016, true);
    (void)p.run("engine.stop", "{}");
    live.emplace(p, kInCallback, kBlock, 48000 * 4);
    json::Value armed = p.run("launch.capture", R"({"on": true})");
    CHECK(warningsOf(armed).find("this capture replaces") != std::string::npos);
    json::Value state = p.run("launch.state", "{}");
    CHECK(state["capturing"].asBool());
    CHECK(state["capture_kept"].asBool());
    // The view carries what the replace warning says, as fields.
    {
        const auto view = p.app->launchCapture();
        REQUIRE(view);
        CHECK(view->kept);
        CHECK(view->replacedOnBegin);
        CHECK(view->endedBy == EngineApp::CaptureEnd::None);  // ended before the stop
        CHECK(view->commit == EngineApp::CaptureCommit::None);
        CHECK(view->capture.launches.size() == 2);
        CHECK(view->endBeat > view->startBeat);
        // What arming would replace is said once, by launch.capture.
        CHECK(warningsOf(armed).find("2 launches") != std::string::npos);
        CHECK_FALSE(p.app->captureReplaceWarning());  // armed: nothing more to replace
    }
    // A halt now keeps the old one, not the empty new one (round 168 S2).
    (void)p.run("engine.stop", "{}");
    const auto kept = p.app->launchCapture();
    REQUIRE(kept);
    CHECK(kept->capture.launches.size() == 2);
    CHECK(kept->capture.frames > 0);
    live.reset();
}

TEST_CASE("an undo of the commit makes a kept capture uncommitted again; a redo commits it") {
    // Review round 168 F4: the commit counts while the head descends from
    // the version it was written in.
    Project p;
    twoTracks(p);
    std::optional<AppLive> live;
    live.emplace(p, kInCallback, kBlock, 48000 * 4);
    take(p, *live, 150016, true);
    (void)p.run("engine.stop", "{}");
    // The pane tells a take never committed (a click commits it) from one
    // whose commit was undone (declined: a click arms a new capture).
    const auto commitState = [&] {
        const auto view = p.app->launchCapture();
        REQUIRE(view);
        CHECK(view->kept);
        CHECK(view->endedBy == EngineApp::CaptureEnd::None);  // ended before the stop
        return view->commit;
    };
    CHECK(commitState() == EngineApp::CaptureCommit::None);
    CHECK(p.app->captureUncommitted());
    (void)p.run("launch.commit", "{}");
    CHECK(commitState() == EngineApp::CaptureCommit::Committed);
    CHECK_FALSE(p.app->captureUncommitted());
    CHECK_FALSE(p.app->captureKeptWarning());
    p.run("history.undo", "{}");
    CHECK(commitState() == EngineApp::CaptureCommit::Undone);
    CHECK(p.app->captureUncommitted());
    REQUIRE(p.app->captureKeptWarning());
    CHECK(p.app->captureKeptWarning()->find("kept uncommitted") != std::string::npos);
    p.run("history.redo", "{}");
    CHECK(commitState() == EngineApp::CaptureCommit::Committed);
    CHECK_FALSE(p.app->captureUncommitted());
    // Undone, then an edit: the redo is gone, and so is the commit.
    p.run("history.undo", "{}");
    p.run("track.add", R"({"name": "After"})");
    CHECK(commitState() == EngineApp::CaptureCommit::Undone);
    CHECK(p.app->captureUncommitted());
    // So a new capture warns that it replaces it.
    live.emplace(p, kInCallback, kBlock, 48000 * 4);
    CHECK(warningsOf(p.run("launch.capture", R"({"on": true})")).find("this capture replaces") !=
          std::string::npos);
    live.reset();
}

TEST_CASE("a halt with a quantized end pending ends the capture where it was heard") {
    // Review round 168 S1: launch.capture {on: false, quantize: "bar"} waits
    // for bar 3's boundary (running frame 192000); a stop before it ends the
    // capture at the heard frame, short of the boundary, and keeps it.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    take(p, live, 150016, false);
    json::Value ending = p.run("launch.capture", R"({"on": false, "quantize": "bar"})");
    REQUIRE(ending["ends_at"].isNumber());
    CHECK(p.run("launch.state", "{}")["capturing"].asBool());
    (void)p.run("engine.stop", "{}");
    const auto kept = p.app->launchCapture();
    REQUIRE(kept);
    CHECK_FALSE(kept->capture.on);
    CHECK(kept->capture.startSample + kept->capture.frames < 192000);
    CHECK(kept->capture.frames > 0);
    CHECK(kept->endedBy == EngineApp::CaptureEnd::EngineStop);
}

TEST_CASE("a restart that gives up after a slow last attempt names the kept capture") {
    // Review round 168 F3: the device's open fails after the deadline
    // although it began before it (the test moves the clock there).
    Project p;
    twoTracks(p);
    p.app->setFixedClock(1'000'000);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    take(p, live, 150016, true);
    live.backend.failStarts(1000);
    live.backend.requestRestart("the device went away");
    p.app->tick();  // stops the engine, keeping the capture (a first retry may fail at once)
    // The last retry begins before the 30 s deadline and fails after it.
    p.app->setFixedClock(1'029'999);
    live.backend.onStart([&] { p.app->setFixedClock(1'031'000); });
    p.app->tick();
    std::string reported;
    for (auto& [topic, payload] : p.app->takeEvents())
        if (topic == "engine.error" && payload.isObject()) reported += warningsOf(payload);
    INFO(reported);
    CHECK(reported.find("kept uncommitted") != std::string::npos);
    CHECK(p.app->engine() == nullptr);
}
