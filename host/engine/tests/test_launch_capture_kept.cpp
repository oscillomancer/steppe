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
// 150016 with `midway` run at block `at` (frame 120064), and ends the
// capture.
template <class F>
void heldTake(Project& p, AppLive& live, F&& midway, std::int64_t at = 938) {
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);  // 100096
    p.run("launch.capture", R"({"on": true})");
    live.until(at * kBlock);
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
    // Q3 (round 168): the plan's return is an arrangement launch where the
    // executor adopted the plan (its ack): the next cycle's start, 120064,
    // which is 120000 on tr_1's timeline (64 frames behind the stream, tr_2's
    // latent gain), where its launches are placed too (round 176 F2). Once
    // the removal is undone, the commit cuts cl_1 there: beat 5.
    const model::Id tr1{model::Kind::Track, 1};
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
    CHECK(view->capture.startSample + got[1].runningFrame == 120000);
    CHECK(got[1].songFrame == 120000);
    CHECK(got[1].anchorSample == 120000);
    CHECK(got[1].anchorBeat == doctest::Approx(5.0));
    // The edit behind the return: not as heard (engine.md §3.1). The
    // capture's own version still has cl_1, so the dry run answers.
    {
        json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
        CHECK_FALSE(dry["as_heard"].asBool());
        const std::string why = json::writeCompact(dry["not_heard_because"]);
        INFO(why);
        CHECK(why.find("\"edited_during\"") != std::string::npos);
    }
    refusedForRemovedClip(p);
    p.run("history.undo", "{}");
    json::Value done = commitResult(p);
    const std::string layout = json::writeCompact(done["layout"]);
    INFO(layout);
    CHECK(layout.find("\"tr_1 4.1706666666666665-5.0 fades") != std::string::npos);
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
}  // namespace

TEST_CASE("a launch on the track after its clip is removed holds over the return (round 172 F1)") {
    for (const bool queued : {false, true}) {
        INFO((queued ? "a quantized launch due before the return's frame"
                     : "an immediate launch at the return's frame"));
        // (a) cl_1 removed at 120064 and cl_3 launched now: the executor
        // returns tr_1 at 120064 (the next cycle), cl_3 switches at 120192;
        // (b) cl_3 on beat 9 (frame 216000), queued at 215040, then cl_1
        // removed at 215936: the return at 215936, cl_3 at 216000. Heard and
        // captured: the arrangement between, then cl_3 (a twin cannot launch
        // that: a second launch on tr_1 replaces a pending one).
        // tr_1 is 64 frames behind the stream (tr_2's latent gain): its
        // return is logged on its timeline.
        const std::int64_t removedAt = (queued ? 1687 : 938) * std::int64_t(kBlock);
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
        // The twin's, with the return before cl_3.
        const std::size_t cl3 = twinLaunches.find("0 tr_1 cl_3");
        REQUIRE(cl3 != std::string::npos);
        const std::string expected = twinLaunches.substr(0, cl3) + "1 tr_1 tr_0 @" +
                                     std::to_string(removedAt - 64 - 100096) + "\n" +
                                     twinLaunches.substr(cl3);
        const std::string got = launchesOf(p);
        INFO("removed:\n" << got << "expected:\n" << expected);
        CHECK(got == expected);
        // Restored, it commits: cl_1 up to the return, cl_3 from its switch,
        // the arrangement between.
        p.run("history.undo", "{}");
        json::Value done = commitResult(p);
        const std::string doneLayout = json::writeCompact(done["layout"]);
        const std::string twinLayout = json::writeCompact(twin["layout"]);
        INFO("committed " << doneLayout << " twin " << twinLayout);
        // One more item than the twin's: tr_1's span where the arrangement
        // played, between the return and cl_3's switch.
        CHECK(done["layout"].asArray().size() == twin["layout"].asArray().size() + 1);
    }
}

TEST_CASE("a clip removed as its capture began returns where the executor took the plan") {
    // Removed in the pass that places the capture: the executor adopts the
    // plan at the next cycle's start, which is where the capture begins, so
    // tr_1 is on the arrangement throughout, as heard (round 172 F3 asked
    // for cl_1 until the return; the return is now at the start).
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
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    CHECK(view->capture.startSample == 782 * std::int64_t(kBlock));
    CHECK(view->capture.launches.empty());
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    CHECK(returns[0].runningFrame == 782 * std::int64_t(kBlock) - 64);  // tr_1's timeline
}

TEST_CASE("under a master playrate a removed clip's return is where its plan hands over (F2)") {
    // A MIDI clip, so the capture runs under the playrate: the executor
    // returns the track at the plan's handAt, which it waits for, not at an
    // immediate launch's earlier floor; its song frame was placed when the
    // plan was published.
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
    const std::int64_t handAt = EngineTestAccess::latestHandAt(*p.app->engine());
    CHECK(handAt > 1201 * std::int64_t(kBlock));  // later than an immediate launch's floor
    live.until(1400 * kBlock);
    REQUIRE(p.app->launchCapture());  // drains the ack
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    CHECK(returns[0].runningFrame == handAt - 64);  // on tr_1's timeline
    p.run("launch.capture", R"({"on": false})");
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    REQUIRE(view->capture.launches.size() == 2);
    CHECK(view->capture.launches[1].action == LaunchLogEntry::Action::Arrangement);
    CHECK(view->capture.launches[1].songFrame == returns[0].songFrame);
}

TEST_CASE("an ack drained late does not move a removed clip's return (round 172 re-check G1)") {
    // The anticipative renderer's acks can be drained before the callback's
    // for an earlier frame, and the log is re-sorted: a late tr_2 entry
    // before cl_1's must not make cl_1's own launch count as after the
    // reset, which would drop the return and record a second, later one.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);
    p.run("launch.capture", R"({"on": true})");
    live.until(938 * kBlock);
    p.run("clip.remove", R"({"clip": "cl_1"})");
    const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
    REQUIRE(log.size() == 1);
    LaunchLogEntry late = log[0];  // cl_1 at 96000
    late.track = model::Id{model::Kind::Track, 2};
    late.action = LaunchLogEntry::Action::Arrangement;
    late.clip = {};
    late.runningFrame -= kBlock;
    late.songFrame -= kBlock;
    late.anchorSample -= kBlock;
    EngineTestAccess::drainedLate(*p.app->engine(), late);
    REQUIRE(p.app->launchCapture());  // drains it, re-sorting the log
    live.until(1100 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    const auto view = p.app->launchCapture();
    REQUIRE(view);
    REQUIRE(view->capture.launches.size() == 2);
    CHECK(view->capture.launches[1].action == LaunchLogEntry::Action::Arrangement);
    CHECK(view->capture.launches[1].songFrame == returns[0].songFrame);
}

TEST_CASE("a removed clip's return is logged where its executor adopted the plan") {
    // In-callback: cl_1 removed at 120064, between two cycles. The executor
    // adopts the plan at the next cycle's start and acks the return there,
    // which the log records (not where an immediate launch would land).
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);
    p.run("launch.capture", R"({"on": true})");
    live.until(938 * kBlock);
    p.run("clip.remove", R"({"clip": "cl_1"})");
    CHECK(EngineTestAccess::launchReturns(*p.app->engine()).empty());  // not adopted yet
    live.until(940 * kBlock);
    REQUIRE(p.app->launchCapture());  // drains the acks
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    CHECK(returns[0].track == model::Id{model::Kind::Track, 1});
    CHECK(returns[0].action == LaunchLogEntry::Action::Arrangement);
    CHECK(returns[0].runningFrame == 938 * std::int64_t(kBlock) - 64);  // tr_1's timeline
    const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
    REQUIRE(log.size() == 2);
    CHECK(returns[0].songFrame - log[0].songFrame == returns[0].runningFrame - log[0].runningFrame);
    CHECK(EngineTestAccess::returnAcksDropped(*p.app->engine()) == 0);
    CHECK(EngineTestAccess::returnAcksDuplicated(*p.app->engine()) == 0);
}

TEST_CASE("a lost return ack leaves the track on the arrangement and the capture not as heard") {
    // A full ring (simulated): the main thread sees the drop counter move,
    // logs the return where an immediate launch.arrangement would land, and
    // marks the running capture edited. launch.state never shows cl_1.
    Project p;
    twoTracks(p);
    AppLive live(p, kInCallback, kBlock, 48000 * 4);
    REQUIRE(p.app->transport("play"));
    live.until(20 * kBlock);
    p.run("launch.clip", R"({"clip": "cl_1"})");
    live.until(782 * kBlock);
    p.run("launch.capture", R"({"on": true})");
    live.until(938 * kBlock);
    p.run("clip.remove", R"({"clip": "cl_1"})");
    live.backend.run(2);  // two cycles with no service (until() services)
    live.cycles += 2;
    CHECK(EngineTestAccess::loseReturnAcks(*p.app->engine()) == 1);
    REQUIRE(p.app->launchCapture());  // the counter moved: the fallback
    const auto returns = EngineTestAccess::launchReturns(*p.app->engine());
    REQUIRE(returns.size() == 1);
    CHECK(returns[0].runningFrame >= 940 * std::int64_t(kBlock));  // a bound, not the frame
    json::Value state = p.run("launch.state", "{}");
    for (const auto& t : state["tracks"].asArray())
        if (t.find("track")->asString() == "tr_1")
            CHECK(t.find("plays")->asString() == "arrangement");
    live.until(1000 * kBlock);
    p.run("launch.capture", R"({"on": false})");
    json::Value dry = p.run("render.dry-run", R"({"performance": "captured"})");
    CHECK_FALSE(dry["as_heard"].asBool());
    const std::string why = json::writeCompact(dry["not_heard_because"]);
    INFO(why);
    CHECK(why.find("\"edited_during\"") != std::string::npos);
}

TEST_CASE(
    "a capture begun inside a scene launch's crossfade commits the scene's section (round "
    "219 F1)" *
    doctest::description("rt")) {
    // Switched on 128 frames after sc_1's switch (bar 2), the capture lists
    // each track's arrangement, what the crossfade fades from, then its clip
    // at frame 0 (fault B). That first entry is no launch of the scene: the
    // section is written (it counted the scene's launches twice), the
    // recorded performance tags no arrangement with the scene, and the
    // capture's line counts the two clips launched.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO((options.anticipative ? "anticipative" : "in-callback"));
        Project p;
        twoTracks(p);  // tr_1 cl_1 and tr_2 cl_2 in sc_1
        AppLive live(p, options, kBlock, 48000 * 4);
        REQUIRE(p.app->transport("play"));
        live.until(20 * kBlock);
        p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2 (96000)
        live.until(96000 + 128);
        p.run("launch.capture", R"({"on": true})");
        live.until(96000 + 48000);
        p.run("launch.capture", R"({"on": false})");
        const auto view = p.app->launchCapture();
        REQUIRE(view);
        INFO("launches:" << describe(view->capture.launches));
        REQUIRE(view->capture.launches.size() == 4);  // each track's arrangement, then its clip
        for (const LaunchLogEntry& e : view->capture.launches) {
            CHECK(e.runningFrame == 0);
            CHECK(e.scene.has_value() == (e.action == LaunchLogEntry::Action::Clip));
        }
        CHECK(p.app->captureUncommitted());
        const auto line = p.app->captureReplaceWarning();
        REQUIRE(line);
        CHECK(line->find("(2 launches,") != std::string::npos);
        json::Value done = p.run("launch.commit", "{}");
        REQUIRE(done["sections"].size() == 1);
        CHECK(done["sections"].at(0).find("scene")->asString() == "sc_1");
        CHECK(done["tracks"].size() == 2);
        json::Value history = p.run("log.list", R"({"limit": 1})");
        const json::Value& last = history["entries"].at(history["entries"].size() - 1);
        const json::Value* launches = last.find("args")->find("performance")->find("launches");
        REQUIRE(launches);
        for (const json::Value& e : launches->asArray())
            CHECK((e.find("scene") == nullptr || e.find("action")->asString() == "clip"));
        CHECK_FALSE(p.app->captureUncommitted());
        live.stop();
    }
}

TEST_CASE("a capture holding only a return's crossfade holds no performance (round 219 F2)" *
          doctest::description("rt")) {
    // Both tracks back to the arrangement at bar 3, the capture switched on
    // 128 frames after: it lists each track's clip, then its return, at
    // frame 0, nothing to lay down. Not uncommitted (no project-switch
    // prompt, no replace warning) and launch.commit writes nothing: one test
    // (holdsLaunches) for both. A launch inside the same capture makes it
    // hold one.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}})
        for (const bool launched : {false, true}) {
            INFO((options.anticipative ? "anticipative" : "in-callback")
                 << (launched ? ", cl_1 launched inside" : ""));
            Project p;
            twoTracks(p);
            AppLive live(p, options, kBlock, 48000 * 8);
            REQUIRE(p.app->transport("play"));
            live.until(20 * kBlock);
            p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2
            live.until(96000 + 4000);
            p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3
            p.run("launch.arrangement", R"({"track": "tr_2"})");
            live.until(192000 + 128);
            p.run("launch.capture", R"({"on": true})");
            if (launched) p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 4
            live.until(launched ? 288000 + 48000 : 192000 + 48000);
            p.run("launch.capture", R"({"on": false})");
            const auto view = p.app->launchCapture();
            REQUIRE(view);
            INFO("launches:" << describe(view->capture.launches));
            CHECK(view->capture.launches.size() == (launched ? 5u : 4u));
            CHECK(p.app->captureUncommitted() == launched);
            const auto line = p.app->captureReplaceWarning();
            CHECK(line.has_value() == launched);
            if (line) CHECK(line->find("(1 launch,") != std::string::npos);
            json::Value done = p.run("launch.commit", "{}");
            CHECK(done["tracks"].size() == (launched ? 1u : 0u));
            CHECK(done["sections"].size() == 0);
            CHECK_FALSE(p.app->captureUncommitted());
            live.stop();
        }
}
