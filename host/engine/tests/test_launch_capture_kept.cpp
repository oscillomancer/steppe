// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A clip launcher capture is kept across an engine stop, a device restart and
// a device failure, running or ended (ADR-0097, revision 2026-11-24): it
// stays committable, committing it writes what committing it before would
// have, and the stop's answer (or the device event) names it while it is
// uncommitted; a recovery seek's end keeps it as well. Its own ctest entry.
#include <optional>
#include "clip_player_support.h"
#include "engine_test_access.h"

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
    (void)p.run("engine.stop", "{}");  // before its backend goes with `live`
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

namespace {
// A take kept across a stop, nothing committed yet (the engine stopped).
void keptTake(Project& p) {
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    take(p, live, 150016, true);
    (void)p.run("engine.stop", "{}");
}

EngineApp::CaptureCommit commitOf(Project& p) {
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    return view->commit;
}
}  // namespace

TEST_CASE("a history squash after an undone commit leaves it undone") {
    // Review round 168 re-check R1: commit, undo, then edits under a budget
    // that squashes all it can. The step before the commit is squashed into
    // the first edit after the undo; the commit's version is pinned, so the
    // check still sees the commit off the head's line.
    Project p;
    p.run("project.set", R"({"history_budget_bytes": 1})");
    keptTake(p);
    (void)p.run("launch.commit", "{}");
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    p.run("history.undo", "{}");
    p.run("track.add", R"({"name": "E1"})");
    p.run("track.add", R"({"name": "E2"})");
    p.run("track.add", R"({"name": "E3"})");
    CHECK(p.run("history.list", "{}")["truncated"].asBool());
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Undone);
    CHECK(p.app->captureUncommitted());
    CHECK(p.app->captureKeptWarning());
}

TEST_CASE("a commit merged into main from a branch counts as committed") {
    // Review round 168 re-check R2: committed on a branch, then merged into a
    // main that moved meanwhile (a replay, not a fast-forward).
    Project p;
    keptTake(p);
    p.run("branch.create", R"({"name": "idea"})");
    (void)p.run("launch.commit", "{}");
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    // An edit after it on the branch: the merge's last step replays that,
    // so the merge finds the commit through its replays or second parent
    // (review round 168 recheck 2 S2).
    p.run("track.add", R"({"name": "On idea"})");
    p.run("branch.switch", R"({"name": "main"})");
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Undone);  // main has not got it
    p.run("track.add", R"({"name": "Main moved"})");
    json::Value merged = p.run("branch.merge", R"({"from": "idea"})");
    CHECK_FALSE(merged["fast_forward"].asBool());
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    CHECK_FALSE(p.app->captureUncommitted());
    CHECK_FALSE(p.app->captureKeptWarning());
}

TEST_CASE("a rolled-back launch.commit leaves the capture unmarked") {
    // Review round 168 re-check R3: a batch commits the capture, then fails
    // and rolls back; a launch.commit of an explicit performance then takes
    // the same version number. The capture was never written.
    json::Value performance;
    {
        Project twin;
        keptTake(twin);
        (void)twin.run("launch.commit", "{}");
        json::Value log = twin.run("log.list", R"({"limit": 1})");
        const json::Value& last = log["entries"].at(log["entries"].size() - 1);
        REQUIRE(last.find("command")->asString() == "launch.commit");
        performance = *last.find("args")->find("performance");
    }
    Project p;
    keptTake(p);
    const std::uint64_t before = p.app->head().version;
    auto failed = p.bus->execute(
        {"batch",
         j(R"({"steps": [{"command": "launch.commit", "args": {}},)"
           R"( {"command": "track.set", "args": {"track": "tr_99", "name": "x"}}]})"),
         kOrigin});
    REQUIRE_FALSE(failed);
    CHECK(p.app->head().version == before);
    CHECK(commitOf(p) == EngineApp::CaptureCommit::None);
    json::Value args = json::Value::object();
    args["performance"] = performance;
    (void)p.runArgs("launch.commit", args);
    CHECK(commitOf(p) == EngineApp::CaptureCommit::None);
    CHECK(p.app->captureUncommitted());
    CHECK(p.app->captureKeptWarning());
}

TEST_CASE("a commit stays committed while edits after it squash history") {
    // Review round 168 recheck 2 F2: without the pin, a budget squash folds
    // the commit's version into the next one, and the walk passes below it.
    Project p;
    p.run("project.set", R"({"history_budget_bytes": 1})");
    keptTake(p);
    (void)p.run("launch.commit", "{}");
    p.run("track.add", R"({"name": "E1"})");
    p.run("track.add", R"({"name": "E2"})");
    p.run("track.add", R"({"name": "E3"})");
    CHECK(p.run("history.list", "{}")["truncated"].asBool());
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    CHECK_FALSE(p.app->captureUncommitted());
}

TEST_CASE("a commit merged twice stays committed when only the last merged step is undone") {
    // Review round 168 recheck 2 F1: committed on a, merged into c, then c
    // merged into main (two replayed steps, neither a fast-forward). Undoing
    // only main's last merged step leaves the head on a replay of c's
    // replay of the commit: a replayed step stands for its source.
    Project p;
    keptTake(p);
    p.run("branch.create", R"({"name": "a"})");
    (void)p.run("launch.commit", "{}");
    p.run("branch.switch", R"({"name": "main"})");
    p.run("track.add", R"({"name": "Main 1"})");
    p.run("branch.create", R"({"name": "c"})");
    json::Value first = p.run("branch.merge", R"({"from": "a", "into": "c"})");
    CHECK_FALSE(first["fast_forward"].asBool());
    p.run("track.add", R"({"name": "On c"})");
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    p.run("branch.switch", R"({"name": "main"})");
    p.run("track.add", R"({"name": "Main 2"})");
    json::Value second = p.run("branch.merge", R"({"from": "c"})");
    CHECK_FALSE(second["fast_forward"].asBool());
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    p.run("history.undo", "{}");  // c's own edit, replayed last
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Committed);
    CHECK_FALSE(p.app->captureUncommitted());
    p.run("history.undo", "{}");  // the commit's replay too: undone
    CHECK(commitOf(p) == EngineApp::CaptureCommit::Undone);
}

namespace {
// Plays cl_1 from bar 2 (frame 96000), captures from frame 100096 to
// 150016 with `midway` run at frame 120064, and ends the capture.
template <class F>
void heldTake(Project& p, AppLive& live, F&& midway) {
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);  // 100096
    p.run("launch.capture", R"({"on": true})");
    live.until(938 * kBlock);  // 120064
    midway();
    live.until(150016);
    p.run("launch.capture", R"({"on": false})");
}

// The commit's reply and the timeline it leaves: each item's track, span
// and fades, in order (ids aside, which a twin need not share).
json::Value commitResult(Project& p) {
    json::Value out = json::Value::object();
    out["commit"] = p.run("launch.commit", "{}");
    const model::Session s = p.app->head();
    std::vector<std::string> items;
    for (const auto& e : s.items) {
        const model::ItemSpan span = model::itemSpan(s, *e.value);
        items.push_back(e.value->track.str() + " " + json::formatDouble(span.startBeat) + "-" +
                        json::formatDouble(model::itemEndBeat(s, *e.value)) + " fades " +
                        json::formatDouble(e.value->fadeIn.length) + "/" +
                        json::formatDouble(e.value->fadeOut.length));
    }
    std::sort(items.begin(), items.end());
    json::Value layout = json::Value::array();
    for (const std::string& item : items) layout.push_back(item);
    out["layout"] = std::move(layout);
    return out;
}

// launch.commit refused for the removed cl_1, the capture kept.
void refusedForRemovedClip(Project& p) {
    const auto before = p.app->launchCapture();
    REQUIRE(before);
    auto r = p.bus->execute({"launch.commit", json::Value::object(), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.commit");
    CHECK(r.error().path == "/args/performance/launches/0/clip");
    CHECK(r.error().message.find("cl_1") != std::string::npos);
    const auto after = p.app->launchCapture();
    REQUIRE(after);
    CHECK(after->capture.launches.size() == before->capture.launches.size());
}
}  // namespace

TEST_CASE("a clip removed after its capture keeps what it played, and commits once restored") {
    // Q3 (round 168): the clip playing as the capture began stays in it
    // when a plan returns its track to the arrangement later; the commit
    // refuses the removed clip, and after the removal is undone writes what
    // a commit before the removal writes.
    json::Value twin;
    {
        Project q;
        twoTracks(q);
        AppLive live(q, kInCallback, kBlock, 48000 * 4);
        heldTake(q, live, [] {});
        twin = commitResult(q);
    }
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    heldTake(p, live, [] {});
    const auto taken = p.app->launchCapture();
    REQUIRE(taken);
    REQUIRE(taken->capture.launches.size() == 1);  // cl_1, from frame 0
    p.run("clip.remove", R"({"clip": "cl_1"})");
    const auto kept = p.app->launchCapture();
    REQUIRE(kept);
    CHECK(kept->capture.launches.size() == 1);
    refusedForRemovedClip(p);
    p.run("history.undo", "{}");
    CHECK(commitResult(p) == twin);
}

TEST_CASE("a clip removed mid-capture is kept up to its return to the arrangement") {
    // Q3 (round 168): the plan's return is an arrangement launch where an
    // immediate launch.arrangement would have landed: the same capture and
    // commit as that launch, once the removal is undone.
    const model::Id tr1{model::Kind::Track, 1};
    json::Value twin;
    std::vector<LaunchLogEntry> twinLaunches;
    {
        Project q;
        twoTracks(q);
        AppLive live(q, kInCallback, kBlock, 48000 * 4);
        heldTake(q, live, [&] {
            const Engine::LaunchRequest back{tr1, LaunchLogEntry::Action::Arrangement, {}, {}};
            REQUIRE(q.app->engine()->launch(std::span(&back, 1), model::LaunchQuantize::None));
        });
        twinLaunches = q.app->launchCapture()->capture.launches;
        twin = commitResult(q);
    }
    REQUIRE(twinLaunches.size() == 2);
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    heldTake(p, live, [&] { p.run("clip.remove", R"({"clip": "cl_1"})"); });
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    const auto& got = view->capture.launches;
    REQUIRE(got.size() == 2);
    CHECK(got[0].action == LaunchLogEntry::Action::Clip);
    CHECK(got[0].runningFrame == 0);
    CHECK(got[1].action == LaunchLogEntry::Action::Arrangement);
    CHECK(got[1].track == tr1);
    // Heard from the first block after the removal.
    const std::int64_t back = view->capture.startSample + got[1].runningFrame;
    CHECK(back >= 120064);
    CHECK(back <= 120064 + 2 * kBlock);
    CHECK(got[1].runningFrame == twinLaunches[1].runningFrame);
    CHECK(got[1].songFrame == twinLaunches[1].songFrame);
    CHECK(got[1].anchorSample == twinLaunches[1].anchorSample);
    CHECK(got[1].anchorBeat == twinLaunches[1].anchorBeat);
    refusedForRemovedClip(p);
    p.run("history.undo", "{}");
    CHECK(commitResult(p) == twin);
}

namespace {
// cl_1 playing from bar 2 (frame 96000), a capture from frame 100096, the
// performance's middle in `midway`, and its end at `until`. cl_3 is a
// second clip on tr_1.
template <class F>
void longTake(Project& p, AppLive& live, std::int64_t until, F&& midway) {
    p.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_2", "notation": "voice bass; 1: D3:q F3:q A3:h"})");
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);  // 100096
    p.run("launch.capture", R"({"on": true})");
    midway();
    live.until(until);
    p.run("launch.capture", R"({"on": false})");
}

void launchNow(Project& p, const char* track, const char* clip, model::LaunchQuantize q) {
    const Engine::LaunchRequest r =
        clip ? request(track, clip)
             : Engine::LaunchRequest{
                   *model::Id::parse(track), LaunchLogEntry::Action::Arrangement, {}, {}};
    REQUIRE(p.app->engine()->launch(std::span(&r, 1), q));
}

// The capture's launches as (action, clip, frame), for a failure's message
// and comparison.
std::string launchesOf(Project& p) {
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    std::string out;
    for (const LaunchLogEntry& e : view->capture.launches)
        out += std::to_string(int(e.action)) + " " + e.track.str() + " " + e.clip.str() + " @" +
               std::to_string(e.runningFrame) + "\n";
    return out;
}

// Removed and twin: the same capture, and after the undo the same commit.
void sameAsTwin(Project& p, const std::string& twinLaunches, const json::Value& twinCommit) {
    const std::string got = launchesOf(p);
    INFO("removed:\n" << got << "twin:\n" << twinLaunches);
    CHECK(got == twinLaunches);
    p.run("history.undo", "{}");
    CHECK(commitResult(p) == twinCommit);
}
}  // namespace

TEST_CASE("a launch on the track after its clip is removed holds over the return (round 172 F1)") {
    for (const bool queued : {false, true}) {
        INFO((queued ? "a quantized launch due before the return's frame"
                     : "an immediate launch at the return's frame"));
        // (a) cl_3 now, at the floor the return also takes; (b) cl_3 on beat
        // 9 (frame 216000), queued at 215040, then cl_1 removed at 215936:
        // the plan reaches the executor at 215936, cl_3 switches at 216000,
        // and the return is placed at 216064.
        const auto midway = [&](Project& q, AppLive& live, bool remove) {
            if (!queued) {
                live.until(938 * kBlock);
                if (remove) q.run("clip.remove", R"({"clip": "cl_1"})");
                launchNow(q, "tr_1", "cl_3", model::LaunchQuantize::None);
                return;
            }
            live.until(1680 * kBlock);
            launchNow(q, "tr_1", "cl_3", model::LaunchQuantize::Beat);
            live.until(1687 * kBlock);
            if (remove) q.run("clip.remove", R"({"clip": "cl_1"})");
        };
        std::string twinLaunches;
        json::Value twin;
        {
            Project q;
            twoTracks(q);
            AppLive live(q, kInCallback, kBlock, 48000 * 6);
            longTake(q, live, 250112, [&] { midway(q, live, false); });
            twinLaunches = launchesOf(q);
            twin = commitResult(q);
        }
        Project p;
        twoTracks(p);
        AppLive live(p, kInCallback, kBlock, 48000 * 6);
        longTake(p, live, 250112, [&] { midway(p, live, true); });
        sameAsTwin(p, twinLaunches, twin);
    }
}

TEST_CASE("a clip removed just before its capture began is kept until its return (round 172 F3)") {
    // The capture begins in the same pass as the removal's reset, while its
    // return is still ahead: frame 0 keeps cl_1, and the return cuts it, as
    // an immediate launch.arrangement would.
    std::string twinLaunches;
    json::Value twin;
    {
        Project q;
        twoTracks(q);
        AppLive live(q, kInCallback, kBlock, 48000 * 4);
        REQUIRE(q.app->transport("play"));
        live.until(20 * kBlock);
        q.run("launch.clip", R"({"clip": "cl_1"})");
        live.until(782 * kBlock);
        launchNow(q, "tr_1", nullptr, model::LaunchQuantize::None);
        q.run("launch.capture", R"({"on": true})");
        live.until(150016);
        q.run("launch.capture", R"({"on": false})");
        twinLaunches = launchesOf(q);
        twin = commitResult(q);
    }
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);
    p.run("clip.remove", R"({"clip": "cl_1"})");
    p.run("launch.capture", R"({"on": true})");
    live.until(150016);
    p.run("launch.capture", R"({"on": false})");
    REQUIRE(p.app->launchCapture()->capture.launches.size() == 2);
    sameAsTwin(p, twinLaunches, twin);
}

TEST_CASE("under a master playrate a removed clip's return is where its plan hands over (F2)") {
    // A MIDI clip, so the capture runs under the playrate: the return is at
    // the plan's handAt, which the executors wait for, not an immediate
    // launch's earlier floor.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.8, "preserve_pitch": false})");
    AppLive live(p, kInCallback, kBlock, 48000 * 6);
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 2: device 120000
    live.until(1000 * kBlock);
    p.run("launch.capture", R"({"on": true})");
    live.until(1200 * kBlock);
    p.run("clip.remove", R"({"clip": "cl_1"})");
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    const std::int64_t handAt = EngineTestAccess::latestHandAt(*p.app->engine());
    CHECK(handAt > 1201 * std::int64_t(kBlock));  // later than an immediate launch's floor
    CHECK(returns[0].runningFrame == handAt);
    live.until(1400 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    REQUIRE(view->capture.launches.size() == 2);
    CHECK(view->capture.launches[1].action == LaunchLogEntry::Action::Arrangement);
    CHECK(view->capture.launches[1].songFrame == returns[0].songFrame);
}
