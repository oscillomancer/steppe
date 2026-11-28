// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Recording over an item makes a take of it (ADR-0103).
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <regex>
#include <set>
#include <string>

#include "../../../tests/file_platform.h"
#include "command_helpers.h"
#include "realtime_common.h"
#include "recording.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/json.h"
#include "supple/json/parse.h"
#include "supple/model/comp.h"
#include "supple/model/project_file.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
const cmd::Origin origin{cmd::Origin::Type::Test, "take-stack", {}};
constexpr double kRate = 48000.0;

// Unique to this process and call (ctest runs the tests in parallel).
fs::path uniqueDir(const std::string& prefix) {
    static std::atomic<unsigned> next{0};
    return fs::temp_directory_path() /
           (prefix + std::to_string(test::processId()) + "-" + std::to_string(next.fetch_add(1)));
}

struct StackFixture {
    fs::path dir = uniqueDir("take-stack-");
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;

    // tr_1 "Audio" with one item (it_1, one take) of two seconds from position 0: four beats at
    // 120 bpm.
    // `failingComp`: a take.comp version 2 that writes something and refuses, found before the real
    // one (the registry answers the highest version), to see how a commit survives a refusal.
    explicit StackFixture(double baseSeconds = 2.0, bool failingComp = false) {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        if (failingComp) {
            cmd::Command<cmd::NoArgs, cmd::NoArgs> fail;
            fail.name = "take.comp";
            fail.version = 2;
            fail.kind = cmd::Kind::Edit;
            fail.exposure = cmd::Exposure::Internal;
            fail.args = schema::object<cmd::NoArgs>("test_comp_args").build();
            fail.result = schema::object<cmd::NoArgs>("test_comp_result").build();
            fail.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<cmd::NoArgs> {
                ctx.service<EngineApp>().txn().modify<model::Track>(
                    model::kMaster, [](auto& tr) { tr.name = "half done"; });
                return Error("comp.span", "deliberate refusal after a write");
            };
            registry.add(std::move(fail));
        }
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name":"Audio"})");
        const auto file = writeTone("base.wav", baseSeconds, 220.0);
        auto add = *json::parse(R"({"track":"tr_1","position":0})");
        add["audio_file"] = file.string();
        run("item.add", json::writeCompact(add));
    }
    ~StackFixture() {
        bus.reset();
        app.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path writeTone(const std::string& name, double seconds, double hz) const {
        audio::AudioData data;
        data.sampleRate = kRate;
        data.channels.assign(2, std::vector<float>(static_cast<std::size_t>(seconds * kRate)));
        for (std::size_t n = 0; n < data.channels[0].size(); ++n)
            data.channels[0][n] = data.channels[1][n] = static_cast<float>(
                0.5 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
        const auto file = app->mediaDir() / name;
        fs::create_directories(file.parent_path());
        REQUIRE(audio::writeWav(file, data));
        return file;
    }
    // A pass as the recorder leaves it: in the recordings folder with its sidecar, so that
    // project.recover can place it. `loopSeconds` > 0: passes of that loop over `seconds` of
    // audio, the loop starting at `from`.
    RecordedTake onDisk(const std::string& name, double from, double seconds, model::Id track,
                        double loopSeconds = 0) {
        RecordedTake r = pass("rec/" + name, from, seconds, 440.0, track);
        engine::detail::RecordingInfo side{track, r.startSample, kRate, {}, 0, 0};
        if (loopSeconds > 0) {
            r.loop.start = r.startSample;
            r.loop.end = r.startSample + static_cast<std::int64_t>(loopSeconds * kRate);
            const auto per = static_cast<std::int64_t>(loopSeconds * kRate);
            for (std::int64_t at = 0; at < r.frames; at += per)
                r.segments.push_back({at, r.startSample});
            r.segments.erase(r.segments.begin());  // pass() put one in already
            side.loopStart = r.loop.start;
            side.loopEnd = r.loop.end;
        }
        side.segments = r.segments;
        REQUIRE(engine::detail::writeSidecar(r.file, side));
        return r;
    }
    Result<json::Value> call(const char* command, std::string_view args = "{}") {
        auto parsed = json::parse(args);
        REQUIRE_MESSAGE(parsed, "arguments of " << std::string(command) << " are not JSON");
        return bus->execute({command, *parsed, origin});
    }
    json::Value run(const char* command, std::string_view args = "{}") {
        auto result = call(command, args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    // A pass of `seconds` heard from `from` seconds on, one run.
    RecordedTake pass(const std::string& name, double from, double seconds, double hz = 440.0,
                      model::Id track = model::Id{model::Kind::Track, 1}) {
        RecordedTake r;
        r.track = track;
        r.file = writeTone(name, seconds, hz);
        r.sampleRate = kRate;
        r.frames = static_cast<std::int64_t>(seconds * kRate);
        r.startSample = static_cast<std::int64_t>(from * kRate);
        r.segments = {{0, r.startSample}};
        return r;
    }
    // The items of tr_1 on its main lane, in time order.
    std::vector<model::Id> pieces(model::Id track = model::Id{model::Kind::Track, 1}) const {
        const model::Session s = app->head();
        std::vector<model::Id> out;
        for (const auto& e : s.items)
            if (e.value->track == track && !e.value->lane) out.push_back(e.id);
        std::sort(out.begin(), out.end(), [&](model::Id a, model::Id b) {
            const double pa = s.items.find(a)->position, pb = s.items.find(b)->position;
            return pa == pb ? a < b : pa < pb;
        });
        return out;
    }
    // The master, left channel, from 0 to `seconds`, offline.
    std::vector<float> render(const model::Session& session, double seconds) const {
        auto pool = makePool();
        auto out = renderOffline(session, pool,
                                 {.startSeconds = 0, .endSeconds = seconds, .projectDir = dir});
        REQUIRE(out);
        return out->audio.channels[0];
    }
    const model::Item& item(unsigned n) const {
        return *app->head().items.find(model::Id{model::Kind::Item, n});
    }
    const model::Take& take(model::Id id) const { return *app->head().takes.find(id); }
    static bool warned(const RecordedInfo& info, std::string_view code) {
        if (!info.warnings) return false;
        return std::any_of(info.warnings->begin(), info.warnings->end(),
                           [&](const RecordingWarning& w) { return w.code == code; });
    }
};
}  // namespace

TEST_CASE("a pass inside an item is heard where it was recorded, the rest plays what it did") {
    StackFixture f;
    // 0.5 s long, heard from 0.5 s: inside the item's two seconds.
    auto done = f.app->commitTakes({f.pass("inside.wav", 0.5, 0.5)});
    REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    REQUIRE(done->size() == 1);
    // One comp of three pieces (before, the pass, after), no layered item over it.
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 3);
    for (const auto id : pieces) {
        const model::Item& it = *f.app->head().items.find(id);
        REQUIRE(it.takes.size() == 2);  // every piece keeps the original and has the new pass
        CHECK(f.take(it.takes[1]).name == "inside");
    }
    const auto activeIndex = [&](model::Id id) {
        const model::Item& it = *f.app->head().items.find(id);
        return std::find(it.takes.begin(), it.takes.end(), *it.activeTake) - it.takes.begin();
    };
    CHECK(activeIndex(pieces[0]) == 0);  // before the pass: what played
    CHECK(activeIndex(pieces[1]) == 1);  // the pass
    CHECK(activeIndex(pieces[2]) == 0);  // after it
    // The first piece begins where the item did; the last ends where it ended.
    const auto& head = *f.app->head().items.find(pieces[0]);
    const auto& tail = *f.app->head().items.find(pieces[2]);
    CHECK(head.position == doctest::Approx(0.0));
    CHECK(tail.position + tail.length == doctest::Approx(4.0));  // beats
    // The new take reads the recording by the timeline: the middle piece starts 5 ms before the
    // pass (its crossfade), so the take starts 5 ms before the file does.
    const auto& middle = *f.app->head().items.find(pieces[1]);
    CHECK(f.take(middle.takes[1]).startOffset == doctest::Approx(-0.005).epsilon(0.01));
    CHECK((*done)[0].stacked);
    CHECK((*done)[0].item == pieces[0].str());
    CHECK_FALSE(StackFixture::warned((*done)[0], "record.stack_skipped"));
}

TEST_CASE("a pass that covers the item is a take of it as long as the item") {
    StackFixture f;
    auto done = f.app->commitTakes({f.pass("cover.wav", 0.0, 3.0)});
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 1);
    const model::Item& item = f.item(1);
    CHECK(item.length == doctest::Approx(4.0));  // beats: unchanged
    REQUIRE(item.takes.size() == 2);
    CHECK(f.take(item.takes[1]).startOffset == doctest::Approx(0.0));
    // The last second of the pass lies past the item and stays in the file, hidden.
    CHECK(StackFixture::warned((*done)[0], "record.take_beyond_item"));
}

TEST_CASE("a pass that starts before an item and runs into it is a new item, and says so") {
    StackFixture f;
    f.run("item.move", R"({"item":"it_1","position":2})");            // the item is beats 2 to 6
    auto done = f.app->commitTakes({f.pass("early.wav", 0.5, 2.0)});  // 0.5 s to 2.5 s
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 2);  // layered, as before
    CHECK(f.item(1).takes.size() == 1);
    REQUIRE((*done)[0].warnings);
    CHECK(StackFixture::warned((*done)[0], "record.stack_skipped"));
    const auto& w = (*done)[0].warnings->front();
    CHECK(w.message.find("it_1") != std::string::npos);
    CHECK_FALSE(w.hint.empty());
    CHECK_FALSE((*done)[0].stacked);
}

TEST_CASE("a pass that starts clear of every item is a new item without a warning") {
    StackFixture f;
    auto done = f.app->commitTakes({f.pass("later.wav", 5.0, 1.0)});
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 2);
    CHECK_FALSE((*done)[0].warnings.has_value());
}

TEST_CASE("a locked or looped item is not stacked into, and the pass says why") {
    SUBCASE("locked") {
        StackFixture f;
        f.run("item.set", R"({"item":"it_1","locked":true})");
        auto done = f.app->commitTakes({f.pass("p.wav", 0.5, 0.5)});
        REQUIRE(done);
        CHECK(f.app->head().items.size() == 2);
        REQUIRE(StackFixture::warned((*done)[0], "record.stack_skipped"));
        CHECK((*done)[0].warnings->front().message.find("locked") != std::string::npos);
    }
    SUBCASE("looped") {
        StackFixture f;
        f.run("item.set", R"({"item":"it_1","loop":true})");
        auto done = f.app->commitTakes({f.pass("p.wav", 0.5, 0.5)});
        REQUIRE(done);
        CHECK(f.app->head().items.size() == 2);
        CHECK(StackFixture::warned((*done)[0], "record.stack_skipped"));
    }
}

TEST_CASE("record.set {stack_takes: false} lays every pass as an item again") {
    StackFixture f;
    CHECK(f.run("record.get").find("stack_takes")->asBool());  // the default
    CHECK_FALSE(f.run("record.set", R"({"stack_takes":false})").find("stack_takes")->asBool());
    CHECK_FALSE(f.run("record.get").find("stack_takes")->asBool());
    auto done = f.app->commitTakes({f.pass("inside.wav", 0.5, 0.5)});
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 2);
    CHECK_FALSE((*done)[0].warnings.has_value());
    CHECK_FALSE((*done)[0].stacked);
    // Back on: the next pass stacks.
    f.run("record.set", R"({"stack_takes":true})");
    auto again = f.app->commitTakes({f.pass("again.wav", 0.25, 0.25)});
    REQUIRE(again);
    CHECK((*again)[0].stacked);
}

TEST_CASE("a recovery never stacks") {
    StackFixture f;
    auto done = f.app->commitTakes({f.pass("inside.wav", 0.5, 0.5)}, "recover", false);
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 2);
    CHECK_FALSE((*done)[0].stacked);
}

TEST_CASE("the decision is stored: a replay does not read the setting") {
    // record.commit as history stores it: `stack` true makes a take, false or absent lays items,
    // whatever the setting is at replay time.
    auto commit = [](StackFixture& f, std::string_view stack) {
        const auto file = f.writeTone("replay.wav", 0.5, 440.0);
        auto args = *json::parse(R"({"takes":[{"track":"tr_1","hash":"","start_sample":24000,
            "frames":24000,"loop_start":0,"loop_end":0,"overflow_frames":0,"midi":false,
            "events":[],"sample_rate":48000,"segments":[{"file_frame":0,"timeline":24000}]}],
            "punch":{"enabled":false,"start":0,"end":0}})");
        args["takes"].asArray()[0]["file"] = file.string();
        if (!stack.empty()) args["stack"] = stack == "true";
        auto done = f.bus->execute({"record.commit", args, origin});
        REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    };
    for (const bool setting : {true, false}) {
        CAPTURE(setting);
        {
            StackFixture f;
            f.app->setStackRecordedTakes(setting);
            commit(f, "true");
            CHECK(f.pieces().size() == 3);  // heard where it was recorded: three pieces of one comp
            CHECK(f.item(1).takes.size() == 2);
        }
        {
            StackFixture f;
            f.app->setStackRecordedTakes(setting);
            commit(f, "false");
            CHECK(f.app->head().items.size() == 2);
        }
        {
            StackFixture f;  // an older history: no `stack`
            f.app->setStackRecordedTakes(setting);
            commit(f, "");
            CHECK(f.app->head().items.size() == 2);
        }
    }
}

TEST_CASE("loop passes over an item are takes of it, in order, the newest heard over the loop") {
    StackFixture f;
    // Two passes of the loop [0, 1 s): over the first half of the item.
    RecordedTake r = f.pass("loop.wav", 0.0, 2.0);
    r.loop.start = 0;
    r.loop.end = static_cast<std::int64_t>(kRate);
    r.segments = {{0, 0}, {static_cast<std::int64_t>(kRate), 0}};
    auto done = f.app->commitTakes({r});
    REQUIRE(done);
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 2);  // the loop range, and what is after it
    const model::Item& first = *f.app->head().items.find(pieces[0]);
    const model::Item& second = *f.app->head().items.find(pieces[1]);
    REQUIRE(first.takes.size() == 3);  // the original and two passes
    CHECK(f.take(first.takes[1]).name == "loop pass 1");
    CHECK(f.take(first.takes[2]).name == "loop pass 2");
    CHECK(*first.activeTake == first.takes[2]);    // the newest pass over the loop range
    CHECK(*second.activeTake == second.takes[0]);  // after it: what played
    REQUIRE((*done)[0].takes.size() == 2);
}

TEST_CASE("a pass over a comp's pieces is a take of every piece, and the comp stays a comp") {
    StackFixture f;
    f.run("take.add", R"({"item":"it_1","from":"tk_1"})");   // a second take: it is comp material
    f.run("item.split", R"({"item":"it_1","at":2})");        // it_1 and it_2: one run of two
    f.run("take.activate", R"({"item":"it_2","index":1})");  // it_2 plays the second take
    // The pass lies inside the first piece (beats 0 to 2, 0 to 1 s) only.
    auto done = f.app->commitTakes({f.pass("run.wav", 0.25, 0.5)});
    REQUIRE(done);
    // The comp keeps its pieces outside the pass: before (take 0), the pass, after (take 0), and
    // the second piece (take 1).
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 4);
    std::vector<std::int64_t> heard;
    for (const auto id : pieces) {
        const model::Item& it = *f.app->head().items.find(id);
        REQUIRE(it.takes.size() == 3);  // every piece has the new take: the run stays one comp
        heard.push_back(std::find(it.takes.begin(), it.takes.end(), *it.activeTake) -
                        it.takes.begin());
    }
    CHECK(heard == std::vector<std::int64_t>{0, 2, 0, 1});
    // take.comp.get still reads the four as one comp, and the seam to the second piece is kept.
    const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
    CHECK(got.find("pieces")->asArray().size() == 4);
}

namespace {
// The take index each piece of tr_1 plays, in time order.
std::vector<std::int64_t> heardIndices(const StackFixture& f) {
    const model::Session s = f.app->head();
    std::vector<std::int64_t> out;
    for (const auto id : f.pieces()) {
        const model::Item& it = *s.items.find(id);
        out.push_back(std::find(it.takes.begin(), it.takes.end(), *it.activeTake) -
                      it.takes.begin());
    }
    return out;
}
// The items and takes of a session as canonical JSON (undo keeps id counters, so not the file).
std::string objectsOf(const model::Session& s) {
    std::string out;
    for (const auto& e : s.items) out += json::writeCanonical(model::toJson(*e.value));
    for (const auto& e : s.takes) out += json::writeCanonical(model::toJson(*e.value));
    return out;
}
}  // namespace

TEST_CASE("a punch into bar 5 of an eight-bar item: the old take around it, the new one in it") {
    StackFixture f(16.0);  // 120 bpm, 4/4: eight bars of two seconds
    REQUIRE(f.app->setPunch({true, 8.0, 10.0}));
    const model::Session before = f.app->head();
    const auto old = f.render(before, 16.0);
    // Recorded from 7 s to 11 s (a second of pre-roll and tail); the punch keeps [8, 10) of it.
    auto done = f.app->commitTakes({f.pass("bar5.wav", 7.0, 4.0)});
    REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    CHECK((*done)[0].stacked);
    REQUIRE(f.pieces().size() == 3);
    CHECK(heardIndices(f) == std::vector<std::int64_t>{0, 1, 0});
    const model::Session session = f.app->head();
    const auto mixed = f.render(session, 16.0);
    // The new take alone: every piece plays it (it is silent outside the punch).
    for (const auto id : f.pieces())
        f.run("take.activate", R"({"item":")" + id.str() + R"(","index":1})");
    const auto recorded = f.render(f.app->head(), 16.0);
    // Away from the two seams (a 10 ms crossfade each), bars 1-4 and 6-8 are the old take, bar 5
    // the new one, sample for sample.
    const auto at = [&](double seconds) { return static_cast<std::size_t>(seconds * kRate); };
    const std::size_t guard = at(0.012);
    CHECK(maxDiff(mixed, 0, old, at(8.0) - guard) == 0.0);
    const auto slice = [](const std::vector<float>& v, std::size_t from) {
        return std::vector<float>(v.begin() + static_cast<std::ptrdiff_t>(from), v.end());
    };
    CHECK(maxDiff(mixed, at(8.0) + guard, slice(recorded, at(8.0) + guard), at(2.0) - 2 * guard) ==
          0.0);  // bar 5
    CHECK(maxDiff(mixed, at(10.0) + guard, slice(old, at(10.0) + guard), at(6.0) - guard) == 0.0);
    // The recording really is in bar 5 (nothing but the old tone played it before).
    double bar5 = 0;
    for (std::size_t n = at(8.1); n < at(9.9); ++n)
        bar5 = std::max(bar5, double(std::fabs(recorded[n])));
    CHECK(bar5 > 0.1);
    // Real time renders as offline, in both executors.
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        auto rtPool = makePool();
        MediaCache media;
        const std::int64_t frames = at(16.0);
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(session, f.dir));
        engine.play();
        const std::int64_t cycles = (frames + 4096) / 128;
        if (options.anticipative)
            runAhead(engine, backend, cycles, 128);
        else
            backend.run(cycles);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(maxDiff(backend.capture().channels[0], engine.outputLatency(), mixed,
                      static_cast<std::size_t>(frames)) == 0.0);
    }
}

TEST_CASE("a pass under a master pitch-keeping rate goes through the stretch converter, in step") {
    StackFixture f(4.0);
    // Half speed with the pitch kept: every take, the new one too, is read through the stretcher.
    f.run("transport.playrate.set", R"({"rate":0.5,"preserve_pitch":true})");
    const model::Session before = f.app->head();
    const auto old = f.render(before, 4.0);
    auto done = f.app->commitTakes({f.pass("rate.wav", 1.0, 1.0)});  // timeline 1 s to 2 s
    REQUIRE(done);
    REQUIRE(f.pieces().size() == 3);
    const model::Session session = f.app->head();
    const auto mixed = f.render(session, 4.0);
    const auto at = [&](double seconds) { return static_cast<std::size_t>(seconds * kRate); };
    // Output time is twice the timeline's: the pass is heard from 2 s to 4 s of output. Before it
    // the old take plays as it did; in it the new one does (another tone, so another signal).
    CHECK(maxDiff(mixed, 0, old, at(1.5)) == 0.0);
    double peak = 0, differs = 0;
    for (std::size_t n = at(2.2); n < at(3.8); ++n) {
        peak = std::max(peak, double(std::fabs(mixed[n])));
        differs = std::max(differs, double(std::fabs(mixed[n] - old[n])));
    }
    CHECK(peak > 0.1);
    CHECK(differs > 0.1);
    // Real time renders as offline, in both executors, through the stretcher.
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        auto rtPool = makePool();
        MediaCache media;
        const std::int64_t frames = static_cast<std::int64_t>(at(4.0));
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(session, f.dir));
        engine.play();
        const std::int64_t cycles = (frames + 4096) / 128;
        if (options.anticipative)
            runAhead(engine, backend, cycles, 128);
        else
            backend.run(cycles);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(maxDiff(backend.capture().channels[0], engine.outputLatency(), mixed,
                      static_cast<std::size_t>(frames)) == 0.0);
    }
}

TEST_CASE("a partial pass over a comp leaves its other seams as they were") {
    StackFixture f;
    f.run("take.add", R"({"item":"it_1","from":"tk_1"})");
    // A user's comp: 40 ms linear crossfades, two seams (beats 1 and 3: 0.5 s and 1.5 s).
    f.run("take.comp",
          R"({"item":"it_1","crossfade":0.04,"crossfade_shape":"linear","spans":[
              {"from":0,"to":1,"index":0},{"from":1,"to":3,"index":1},{"from":3,"to":4,"index":0}]})");
    // A piece's place and fades, as the pass must leave them where it did not come.
    const auto shape = [&](const model::Session& s, model::Id id) {
        const model::Item& it = *s.items.find(id);
        return std::tuple(it.position, it.length, it.fadeIn.length, it.fadeIn.shape,
                          it.fadeOut.length, it.fadeOut.shape);
    };
    const auto ids0 = f.pieces();
    REQUIRE(ids0.size() == 3);
    const model::Session beforeAll = f.app->head();
    // A pass inside the middle piece only (0.75 s to 1.25 s), far from both seams.
    auto done = f.app->commitTakes({f.pass("mid.wav", 0.75, 0.5)});
    REQUIRE(done);
    const model::Session after = f.app->head();
    REQUIRE(f.pieces().size() == 5);
    // The first and the last piece, whose seams are 0.25 s from the pass, keep their place and
    // their 40 ms linear fades exactly.
    const auto ids = f.pieces();
    CHECK(shape(after, ids.front()) == shape(beforeAll, ids0.front()));
    CHECK(shape(after, ids.back()) == shape(beforeAll, ids0.back()));
    for (const auto id : {ids.front(), ids.back()}) {
        const model::Item& it = *after.items.find(id);
        CHECK(std::max(it.fadeIn.length, it.fadeOut.length) == doctest::Approx(0.04));
        CHECK((it.fadeIn.length > 0 ? it.fadeIn.shape : it.fadeOut.shape) ==
              model::FadeShape::Linear);
    }
    // And the read-back is still the user's crossfade, uniform.
    const auto got = f.run("take.comp.get", R"({"item":")" + ids.front().str() + R"("})");
    CHECK(got.find("crossfade")->asDouble() == doctest::Approx(0.04));
    CHECK(got.find("crossfade_shape")->asString() == "linear");
    CHECK(got.find("uniform")->asBool());
}

TEST_CASE("a pass over a whole comp joins the pieces: flat across the old seams") {
    StackFixture f;
    f.run("take.add", R"({"item":"it_1","from":"tk_1"})");
    f.run("take.comp",
          R"({"item":"it_1","spans":[{"from":0,"to":2,"index":0},{"from":2,"to":4,"index":1}]})");
    REQUIRE(f.pieces().size() == 2);
    auto done = f.app->commitTakes({f.pass("whole.wav", 0.0, 3.0)});  // covers the item
    REQUIRE(done);
    REQUIRE(f.pieces().size() == 1);  // the neighbours that now play one take are one item
    const auto out = f.render(f.app->head(), 2.0);
    const auto at = [&](double seconds) { return static_cast<std::size_t>(seconds * kRate); };
    double around = 0, elsewhere = 0;
    for (std::size_t n = at(0.1); n < at(1.9); ++n) {
        const bool seam = n > at(0.98) && n < at(1.02);  // the old seam, at 1 s
        (seam ? around : elsewhere) =
            std::max(seam ? around : elsewhere, double(std::fabs(out[n])));
    }
    CHECK(elsewhere > 0.1);
    CHECK(around <= elsewhere * 1.001);  // no 3 dB swell where the pieces used to meet
}

TEST_CASE("a refused commit leaves the session editable and the project as it was") {
    StackFixture f(2.0, true);
    const std::string before = model::serializeProject(f.app->view());
    auto done = f.app->commitTakes({f.pass("refused.wav", 0.5, 0.5)});
    REQUIRE_FALSE(done);
    CHECK_FALSE(done.error().code.empty());
    // The error says the recording is kept and how to get it back.
    CHECK(done.error().message.find("kept") != std::string::npos);
    CHECK(done.error().message.find("project.recover") != std::string::npos);
    // Nothing of it is in the project, not even as a preview, and the next edit works.
    CHECK(model::serializeProject(f.app->view()) == before);
    CHECK(model::serializeProject(f.app->head()) == before);
    auto next = f.call("track.add", R"({"name":"After"})");
    CHECK_MESSAGE(next, (next ? "" : next.error().toString()));
}

namespace {
// Three channels with an item each (tr_2 and tr_3 beside the fixture's tr_1), tr_2 frozen.
void threeTracksMiddleFrozen(StackFixture& f) {
    for (const char* name : {"Room", "Bass"}) {
        f.run("track.add", std::string(R"({"name":")") + name + R"("})");
    }
    for (const char* track : {"tr_2", "tr_3"}) {
        auto add = *json::parse(std::string(R"({"position":0,"track":")") + track + R"("})");
        add["audio_file"] = (f.app->mediaDir() / "base.wav").string();
        f.run("item.add", json::writeCompact(add));
    }
    f.run("track.freeze", R"({"track":"tr_2"})");
}
}  // namespace

TEST_CASE("a recording on a frozen track is kept out, and every other track's is placed") {
    StackFixture f;
    threeTracksMiddleFrozen(f);
    const model::Id tr2{model::Kind::Track, 2}, tr3{model::Kind::Track, 3};
    const auto frozenBefore = f.pieces(tr2).size();
    std::vector<RecordedInfo> infos;
    SUBCASE("stacking") {
        auto done = f.app->commitTakes({f.pass("one.wav", 0.5, 0.5),
                                        f.pass("two.wav", 0.5, 0.5, 440.0, tr2),
                                        f.pass("three.wav", 0.5, 0.5, 440.0, tr3)});
        REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
        infos = *done;
        CHECK(f.pieces().size() == 3);  // a punch-in: before, the pass, after
        CHECK(f.pieces(tr3).size() == 3);
        CHECK(f.pieces(tr2).size() == frozenBefore);  // nothing placed on the frozen track
    }
    SUBCASE("recovery lays items, and follows the same rule") {
        auto done = f.app->commitTakes(
            {f.pass("one.wav", 0.5, 0.5), f.pass("two.wav", 0.5, 0.5, 440.0, tr2),
             f.pass("three.wav", 0.5, 0.5, 440.0, tr3)},
            "recover", false);
        REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
        infos = *done;
        CHECK(f.pieces().size() == 2);  // laid as new items
        CHECK(f.pieces(tr3).size() == 2);
        CHECK(f.pieces(tr2).size() == frozenBefore);
    }
    REQUIRE(infos.size() == 3);
    // The frozen track's file is named by a warning and kept, as a recording still to recover: not
    // a source of the project, so that project.recover places it.
    REQUIRE(StackFixture::warned(infos[1], "record.frozen_skipped"));
    const RecordingWarning& w = infos[1].warnings->back();
    CHECK(w.track == "tr_2");
    CHECK(w.message.find("two.wav") != std::string::npos);
    CHECK(w.hint.find("project.recover") != std::string::npos);
    CHECK(fs::exists(w.file));
    CHECK(fs::path(w.file).filename() == "two.wav");
    const model::Session after = f.app->head();
    CHECK_FALSE(std::any_of(after.media.begin(), after.media.end(), [&](const auto& e) {
        return fs::path(e.value->path).filename() == "two.wav";
    }));
    CHECK_FALSE(StackFixture::warned(infos[0], "record.frozen_skipped"));
    CHECK_FALSE(StackFixture::warned(infos[2], "record.frozen_skipped"));
}

TEST_CASE("a refusal's note about the recording is true of the refusal") {
    SUBCASE("a pass whose placement is out of bounds: kept, nothing more promised") {
        StackFixture f;
        RecordedTake r = f.pass("far.wav", 0.5, 0.5);
        r.startSample = std::numeric_limits<std::int64_t>::max();
        r.segments = {{0, r.startSample}};
        auto done = f.app->commitTakes({r});
        REQUIRE_FALSE(done);
        CHECK(done.error().message.find("recordings are kept") != std::string::npos);
        CHECK(done.error().message.find("project.recover") == std::string::npos);
    }
    SUBCASE("a refusal of project.recover's own commit does not point at project.recover") {
        StackFixture f;
        f.run("track.add", R"({"name":"Bus","type":"buss"})");
        auto done = f.app->commitTakes(
            {f.pass("onbus.wav", 0.5, 0.5, 440.0, model::Id{model::Kind::Track, 2})}, "recover",
            false);
        REQUIRE_FALSE(done);
        CHECK(done.error().message.find("project.recover") == std::string::npos);
    }
}

TEST_CASE(
    "a frozen track's recording is recovered exactly after unfreezing, loop passes included") {
    StackFixture f;
    threeTracksMiddleFrozen(f);
    const model::Id tr2{model::Kind::Track, 2};
    const auto before = f.pieces(tr2).size();
    // Two passes of a half-second loop starting at 0.5 s, on the frozen track.
    const RecordedTake loop = f.onDisk("frozen-loop.wav", 0.5, 1.0, tr2, 0.5);
    REQUIRE(loop.segments.size() == 2);
    auto done = f.app->commitTakes({loop});
    REQUIRE(done);
    REQUIRE(StackFixture::warned((*done)[0], "record.frozen_skipped"));
    CHECK(f.pieces(tr2).size() == before);
    f.run("track.unfreeze", R"({"track":"tr_2"})");
    json::Value listed = f.run("project.recoverable");
    REQUIRE(listed["recordings"].size() == 1);
    auto recovered = f.call("project.recover", R"({"files":["rec/frozen-loop.wav"]})");
    REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().toString()));
    // One item on the loop range, a take per pass: where it was played.
    const auto pieces = f.pieces(tr2);
    REQUIRE(pieces.size() == before + 1);
    const model::Session s = f.app->head();
    const model::Item* laid = nullptr;
    for (const auto id : pieces)
        if (s.items.find(id)->takes.size() == 2) laid = s.items.find(id);
    REQUIRE(laid != nullptr);
    // In beats, at 120 bpm: 0.5 s is one beat.
    CHECK(laid->position == doctest::Approx(1.0));
    CHECK(laid->length == doctest::Approx(1.0));
}

TEST_CASE("a commit of a frozen track's recording alone leaves its items as they were") {
    StackFixture f;
    threeTracksMiddleFrozen(f);
    const model::Id tr2{model::Kind::Track, 2};
    const auto items = f.app->head().items.size();
    auto done = f.app->commitTakes({f.pass("only.wav", 0.5, 0.5, 440.0, tr2)});
    REQUIRE(done);
    REQUIRE(done->size() == 1);
    CHECK(StackFixture::warned((*done)[0], "record.frozen_skipped"));
    CHECK(f.app->head().items.size() == items);
    CHECK(f.call("track.add", R"({"name":"After"})"));  // the session stays editable
}

TEST_CASE("a frozen track cannot be armed, and an armed track cannot be frozen") {
    StackFixture f;
    SUBCASE("arming a frozen track") {
        f.run("track.freeze", R"({"track":"tr_1"})");
        auto armed = f.call("track.set", R"({"track":"tr_1","armed":true})");
        REQUIRE_FALSE(armed);
        CHECK(armed.error().code == "track.frozen");
        CHECK_FALSE(armed.error().hint.empty());
        CHECK_FALSE(f.app->head().tracks.find(model::Id{model::Kind::Track, 1})->record.armed);
    }
    SUBCASE("freezing an armed track") {
        f.run("track.set", R"({"track":"tr_1","armed":true})");
        auto frozen = f.call("track.freeze", R"({"track":"tr_1"})");
        REQUIRE_FALSE(frozen);
        CHECK(frozen.error().code == "freeze.unsupported");
        CHECK_FALSE(f.app->head().tracks.find(model::Id{model::Kind::Track, 1})->freeze);
    }
    SUBCASE("a frozen member is left out when its group is armed") {
        f.run("track.add", R"({"name":"Room"})");
        auto add = *json::parse(R"({"track":"tr_2","position":0})");
        add["audio_file"] = (f.app->mediaDir() / "base.wav").string();
        f.run("item.add", json::writeCompact(add));
        f.run("group.add", R"({"name":"Pair","members":["tr_1","tr_2"],"links":["armed"]})");
        f.run("track.freeze", R"({"track":"tr_2"})");
        auto armed = f.call("track.set", R"({"track":"tr_1","armed":true})");
        REQUIRE_MESSAGE(armed, (armed ? "" : armed.error().toString()));
        const model::Session s = f.app->head();
        CHECK(s.tracks.find(model::Id{model::Kind::Track, 1})->record.armed);
        CHECK_FALSE(s.tracks.find(model::Id{model::Kind::Track, 2})->record.armed);
    }
}

TEST_CASE("a partial pass leaves a hand-set seam as it was, and says nothing about it") {
    StackFixture f;
    f.run("take.add", R"({"item":"it_1","from":"tk_1"})");
    // A comp whose seams differ: a piece's fade-in set by hand after the comp was made.
    f.run("take.comp",
          R"({"item":"it_1","crossfade":0.04,"crossfade_shape":"linear","spans":[
              {"from":0,"to":1,"index":0},{"from":1,"to":3,"index":1},{"from":3,"to":4,"index":0}]})");
    const auto ids = f.pieces();
    REQUIRE(ids.size() == 3);
    f.run("item.set", R"({"item":")" + ids[1].str() + R"(","fade_in":0.02})");
    REQUIRE_FALSE(
        f.run("take.comp.get", R"({"item":")" + ids[0].str() + R"("})").find("uniform")->asBool());
    const auto stored = [&](model::Id id) {
        const model::Item& it = *f.app->head().items.find(id);
        return std::tuple(it.position, it.length, it.fadeIn.length, it.fadeIn.shape,
                          it.fadeOut.length, it.fadeOut.shape);
    };
    const auto first = stored(ids[0]);
    const auto second = stored(ids[1]);
    // A pass inside the middle piece, away from the hand-set seam at 0.5 s.
    auto done = f.app->commitTakes({f.pass("mid.wav", 0.75, 0.5)});
    REQUIRE(done);
    CHECK_FALSE(
        StackFixture::warned((*done)[0], "record.comp_uniform"));  // retired: nothing changed
    REQUIRE(f.pieces().size() == 5);
    // The piece before the hand-set seam is as it was, and so is the start of the one after it
    // (its fade-in, 0.02, and its place); only its end moved, to the new seam.
    CHECK(stored(ids[0]) == first);
    const auto after = stored(ids[1]);
    CHECK(std::get<0>(after) == std::get<0>(second));
    CHECK(std::get<2>(after) == std::get<2>(second));
    CHECK(std::get<3>(after) == std::get<3>(second));
    // It still reads as a comp with a hand-set seam.
    CHECK_FALSE(
        f.run("take.comp.get", R"({"item":")" + ids[0].str() + R"("})").find("uniform")->asBool());
}

TEST_CASE("an audio pass over a MIDI item is not stacked into, and says why") {
    StackFixture f;
    f.run("track.add", R"({"name":"Keys"})");
    f.run("item.add", R"({"track":"tr_2","bar":1,"notation":"voice v; 1: C4:w"})");
    const std::string midiBefore = json::writeCompact(model::toJson(f.item(2)));
    RecordedTake r = f.pass("over-midi.wav", 0.25, 0.5);
    r.track = model::Id{model::Kind::Track, 2};
    auto done = f.app->commitTakes({r});
    REQUIRE(done);
    CHECK_FALSE((*done)[0].stacked);
    REQUIRE(StackFixture::warned((*done)[0], "record.stack_skipped"));
    CHECK((*done)[0].warnings->front().message.find("MIDI") != std::string::npos);
    CHECK(f.pieces(model::Id{model::Kind::Track, 2}).size() == 2);  // the MIDI item and the pass
    CHECK(json::writeCompact(model::toJson(f.item(2))) == midiBefore);
}

TEST_CASE("an item on another lane is not the target of a pass over it") {
    StackFixture f;
    // Fixed lanes are stored and not played (session-model §1.5): put the item on one by editing
    // the saved file, as a project made elsewhere would have it, and open that.
    auto doc = *json::parse(model::serializeProject(f.app->head()));
    doc["items"]["it_1"]["lane"] = "ln_1";
    const auto file = f.dir / "lane.supple";
    {
        std::ofstream out(file, std::ios::binary);
        out << json::writeCanonical(doc);
    }
    auto where = json::Value::object();
    where["path"] = file.string();
    auto opened = f.call("project.open", json::writeCompact(where));
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    REQUIRE(f.item(1).lane.has_value());
    const std::string before = json::writeCompact(model::toJson(f.item(1)));
    auto done = f.app->commitTakes({f.pass("lane.wav", 0.5, 0.5)});
    REQUIRE(done);
    CHECK_FALSE((*done)[0].stacked);  // the item is not played: the pass is a new item
    CHECK_FALSE(StackFixture::warned((*done)[0], "record.stack_skipped"));
    CHECK(f.app->head().items.size() == 2);
    CHECK(json::writeCompact(model::toJson(f.item(1))) == before);
}

TEST_CASE("a pass that runs across two items is a take of the first, and the second is untouched") {
    StackFixture f;
    f.run("item.split", R"({"item":"it_1","at":2})");  // it_1: 0 to 1 s, it_2: 1 to 2 s
    const std::string second = json::writeCompact(model::toJson(f.item(2)));
    // 0.5 s to 1.5 s: starts in it_1, runs into it_2.
    auto done = f.app->commitTakes({f.pass("across.wav", 0.5, 1.0)});
    REQUIRE(done);
    CHECK((*done)[0].stacked);
    CHECK((*done)[0].item == "it_1");
    CHECK(StackFixture::warned((*done)[0], "record.take_beyond_item"));
    CHECK(json::writeCompact(model::toJson(f.item(2))) == second);
    // it_1 is now comped: its first half as it was, its second half the pass.
    const auto pieces = f.pieces();
    REQUIRE(pieces.size() == 3);  // it_1's two pieces and it_2
    CHECK(heardIndices(f) == std::vector<std::int64_t>{0, 1, 0});
}

TEST_CASE("recording some tracks of an edit group says the others are left behind") {
    StackFixture f;
    f.run("track.add", R"({"name":"Room"})");
    const auto file = f.writeTone("room.wav", 2.0, 330.0);
    auto add = *json::parse(R"({"track":"tr_2","position":0})");
    add["audio_file"] = file.string();
    f.run("item.add", json::writeCompact(add));
    // Both already hold two takes (a track whose items have one take is skipped by a group comp).
    f.run("take.add", R"({"item":"it_1","from":"tk_1"})");
    f.run("take.add", R"({"item":"it_2","from":"tk_2"})");
    f.run("group.add", R"({"name":"Drums","members":["tr_1","tr_2"],"links":["edit"]})");
    SUBCASE("one track of the two") {
        auto done = f.app->commitTakes({f.pass("close.wav", 0.5, 0.5)});
        REQUIRE(done);
        REQUIRE(StackFixture::warned((*done)[0], "record.stack_group"));
        const auto& w = (*done)[0].warnings->front();
        CHECK(w.message.find("tr_2") != std::string::npos);
        CHECK(w.message.find("Room") != std::string::npos);
        CHECK_FALSE(w.hint.empty());
        // tr_2's item is as it was: two takes, so a group comp of the new index (2) is refused.
        CHECK(f.pieces(model::Id{model::Kind::Track, 2}).size() == 1);
        CHECK(f.item(2).takes.size() == 2);
        auto comp = f.call("take.comp", R"({"item":"it_1","spans":[{"from":1,"to":2,"index":2}]})");
        REQUIRE_FALSE(comp);
        CHECK(comp.error().code == "comp.group_takes");
    }
    SUBCASE("both tracks, together: in step, no warning") {
        RecordedTake close = f.pass("close.wav", 0.5, 0.5);
        RecordedTake room = f.pass("room-pass.wav", 0.5, 0.5, 330.0);
        room.track = model::Id{model::Kind::Track, 2};
        auto done = f.app->commitTakes({close, room});
        REQUIRE(done);
        REQUIRE(done->size() == 2);
        for (const auto& info : *done) {
            CHECK(info.stacked);
            CHECK_FALSE(StackFixture::warned(info, "record.stack_group"));
        }
        CHECK(f.pieces().size() == 3);
        CHECK(f.pieces(model::Id{model::Kind::Track, 2}).size() == 3);
        // The group's take counts are level: a group comp of the new index works.
        f.run("take.comp",
              R"({"item":")" + f.pieces()[0].str() + R"(","spans":[{"from":1,"to":2,"index":2}]})");
    }
}

TEST_CASE("stacking is one undo step that comes back byte for byte, and survives save and reopen") {
    StackFixture f;
    const model::Session before = f.app->head();
    const auto history = f.app->versions().history().size();
    auto done = f.app->commitTakes({f.pass("undo.wav", 0.5, 0.5)});
    REQUIRE(done);
    CHECK(f.app->versions().history().size() == history + 1);  // one step, three pieces and all
    const model::Session after = f.app->head();
    REQUIRE(f.pieces().size() == 3);
    f.run("history.undo");
    CHECK(objectsOf(f.app->head()) == objectsOf(before));
    CHECK(f.pieces().size() == 1);
    f.run("history.redo");
    CHECK(objectsOf(f.app->head()) == objectsOf(after));
    // Save, reopen, compare.
    auto where = json::Value::object();
    where["path"] = (f.dir / "stacked.supple").string();
    auto saved = f.call("project.save", json::writeCompact(where));
    REQUIRE_MESSAGE(saved, (saved ? "" : saved.error().toString()));
    REQUIRE(f.call("project.open", json::writeCompact(where)));
    CHECK(objectsOf(f.app->head()) == objectsOf(after));
    CHECK(heardIndices(f) == std::vector<std::int64_t>{0, 1, 0});
}

// --- MIDI -----------------------------------------------------------------------------------

namespace {
struct MidiFixture {
    fs::path dir = uniqueDir("take-stack-midi-");
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    // tr_1 "Keys" with a one-bar item (it_1: C4 whole note) and the default record mode.
    MidiFixture() {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name":"Keys"})");
        run("item.add", R"({"track":"tr_1","bar":1,"notation":"voice v; 1: C4:w"})");
    }
    ~MidiFixture() {
        bus.reset();
        app.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    json::Value run(const char* command, std::string_view args = "{}") {
        auto parsed = json::parse(args);
        REQUIRE(parsed);
        auto result = bus->execute({command, *parsed, origin});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    // A note E4 (key 64) from `on` to `off` seconds.
    RecordedTake played(double on, double off) const {
        RecordedTake r;
        r.track = model::Id{model::Kind::Track, 1};
        r.isMidi = true;
        r.sampleRate = kRate;
        r.frames = static_cast<std::int64_t>(2.0 * kRate);
        r.segments = {{0, 0}};
        r.midi = {{static_cast<std::int64_t>(on * kRate), 0, {0x90, 64, 100}},
                  {static_cast<std::int64_t>(off * kRate), 0, {0x80, 64, 0}}};
        return r;
    }
    const midi::Sequence& notes(model::Id take) const {
        const auto& s = app->head();
        return s.midi.find(*s.takes.find(take)->source)->sequence;
    }
};
}  // namespace

TEST_CASE("a MIDI pass over an item is a heard take of it with the played notes only") {
    MidiFixture f;
    const model::Item before = *f.app->head().items.find(model::Id{model::Kind::Item, 1});
    auto done = f.app->commitTakes({f.played(0.5, 1.0)});
    REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    REQUIRE(f.app->head().items.size() == 1);
    const model::Item& item = *f.app->head().items.find(model::Id{model::Kind::Item, 1});
    CHECK(item.position == before.position);
    CHECK(item.length == before.length);
    REQUIRE(item.takes.size() == 2);
    CHECK(*item.activeTake == item.takes[1]);
    const midi::Sequence& seq = f.notes(item.takes[1]);
    REQUIRE(seq.notes.size() == 1);  // the played E4 only, not the C4 beneath
    CHECK(seq.notes[0].pitch == 64);
    CHECK(seq.notes[0].start == 960);  // 0.5 s at 120 bpm is beat 1: one quarter at 960 PPQ
    CHECK((*done)[0].stacked);
    CHECK((*done)[0].item == "it_1");
}

TEST_CASE(
    "a MIDI pass over part of an item is heard where it was played, the rest keeps its notes") {
    MidiFixture f;
    // A pass heard from 0.5 s to 1.0 s with an E4 from 0.6 s to 0.9 s, inside the one-bar item.
    RecordedTake r;
    r.track = model::Id{model::Kind::Track, 1};
    r.isMidi = true;
    r.sampleRate = kRate;
    r.frames = static_cast<std::int64_t>(0.5 * kRate);
    r.startSample = static_cast<std::int64_t>(0.5 * kRate);
    r.segments = {{0, r.startSample}};
    r.midi = {{static_cast<std::int64_t>(0.6 * kRate), 0, {0x90, 64, 100}},
              {static_cast<std::int64_t>(0.9 * kRate), 0, {0x80, 64, 0}}};
    auto done = f.app->commitTakes({r});
    REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    CHECK((*done)[0].stacked);
    const model::Session s = f.app->head();
    std::vector<const model::Item*> pieces;
    for (const auto& e : s.items) pieces.push_back(e.value.get());
    std::sort(pieces.begin(), pieces.end(),
              [](const model::Item* a, const model::Item* b) { return a->position < b->position; });
    REQUIRE(pieces.size() == 3);  // before, the pass, after
    std::vector<std::int64_t> heard;
    for (const model::Item* it : pieces) {
        REQUIRE(it->takes.size() == 2);
        heard.push_back(std::find(it->takes.begin(), it->takes.end(), *it->activeTake) -
                        it->takes.begin());
    }
    CHECK(heard == std::vector<std::int64_t>{0, 1, 0});
    // Hard cuts between MIDI pieces: no crossfade, and no warning about one.
    for (const model::Item* it : pieces) {
        CHECK(it->fadeIn.length == 0.0);
        CHECK(it->fadeOut.length == 0.0);
    }
    CHECK_FALSE(StackFixture::warned((*done)[0], "comp.crossfade_skipped"));
}

TEST_CASE(
    "a MIDI pass over an item whose take follows is the heard take; the following one stays") {
    MidiFixture f;
    f.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    f.run("take.set", R"({"take":"tk_1","context":"1: key C major ~:w","follow":"key"})");
    const model::Take followed = *f.app->head().takes.find(model::Id{model::Kind::Take, 1});
    auto done = f.app->commitTakes({f.played(0.5, 1.0)});
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 1);
    const model::Item& item = *f.app->head().items.find(model::Id{model::Kind::Item, 1});
    REQUIRE(item.takes.size() == 2);
    CHECK(*item.activeTake == item.takes[1]);  // the newest pass is what you hear
    const model::Take& fresh = *f.app->head().takes.find(item.takes[1]);
    CHECK(fresh.follow == model::ClipFollow::None);
    CHECK(fresh.context.empty());
    // The following take is untouched.
    const model::Take& old = *f.app->head().takes.find(item.takes[0]);
    CHECK(old.follow == model::ClipFollow::Key);
    CHECK(old.context == followed.context);
    CHECK(f.notes(item.takes[0]) == f.notes(model::Id{model::Kind::Take, 1}));
    REQUIRE(StackFixture::warned((*done)[0], "record.follow_kept"));
    CHECK((*done)[0].warnings->front().message.find("another take") != std::string::npos);
}

TEST_CASE(
    "the route warnings' reason words are one list: the code's, the schema's and the spec's "
    "(round 195 F4)") {
    // Every emitter writes a model::RouteWarningReason, as its word
    // (reasonWord); the recording warning's schema exports the same words;
    // and session-model.md lists exactly these under the warnings that
    // carry a reason, each word once.
    std::vector<std::string> code;
    for (const auto& [reason, word] : model::kRouteWarningReasons) {
        CHECK(model::reasonWord(reason) == word);
        code.emplace_back(word);
    }
    CHECK(std::set<std::string>(code.begin(), code.end()).size() == code.size());
    std::vector<std::string> exported;
    const json::Value schema = routeWarningReasonSchema().jsonSchema();
    REQUIRE(schema.find("enum"));
    for (const json::Value& w : schema.find("enum")->asArray()) exported.push_back(w.asString());
    CHECK(exported == code);
    // The spec: from "carries `reason`" to the warning that carries none.
    const fs::path spec =
        fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "docs" /
        "specs" / "session-model.md";
    std::ifstream in(spec);
    REQUIRE(in);
    std::string line, text;
    bool inList = false;
    while (std::getline(in, line)) {
        if (line.find("carries `reason`") != std::string::npos) inList = true;
        if (inList && line.find("- `modulation.stacked`") != std::string::npos) break;
        if (inList) text += line + "\n";
    }
    REQUIRE_FALSE(text.empty());
    std::vector<std::string> documented;
    static const std::regex word("`([a-z_]+)`");
    for (std::sregex_iterator i(text.begin(), text.end(), word), e; i != e; ++i) {
        const std::string w = (*i)[1];
        if (w == "reason" || w == "recording_warning") continue;  // the field, the schema
        documented.push_back(w);
    }
    const std::set<std::string> inCode(code.begin(), code.end()),
        inSpec(documented.begin(), documented.end());
    CHECK(inSpec.size() == documented.size());  // each once
    for (const std::string& w : code) CHECK_MESSAGE(inSpec.count(w), "not in the spec: " << w);
    for (const std::string& w : documented)
        CHECK_MESSAGE(inCode.count(w), "not in the code: " << w);
}

TEST_CASE("an overdub past the route limit says why in its record.warning event (round 194)") {
    // The item's take has a gain modulated from the rack's Mod Out; the
    // project holds the most routes it may. The overdub's copy of the take
    // copies the route, which the commit drops (a recording is never
    // refused): the record.warning event carries it as data, its code,
    // path and reason, as the commit's reply does.
    MidiFixture f;
    f.run("track.set", R"({"track":"tr_1","record_mode":"midi_overdub"})");
    f.run("rack.add", R"({"track":"tr_1"})");  // rk_1, mo_1 its Out
    f.run("module.add", R"({"rack":"rk_1","type":"mod_out","x":300,"y":40})");  // mo_2
    json::Value fx = json::Value::object();
    fx["take"] = f.app->head().items.find(model::Id{model::Kind::Item, 1})->takes.at(0).str();
    fx["plugin"] = "com.suppleaudio.test.gain";
    auto gain = f.bus->execute({"fx.add", fx, origin});
    REQUIRE_MESSAGE(gain, (gain ? "" : gain.error().toString()));
    json::Value add =
        json::parse(R"({"source":{"module":"mo_2","input":"in_1"},"depth":0.25})").value();
    add["target"] = json::Value::object();
    add["target"]["fx"] = gain->find("fx")->asString();
    add["target"]["param"] = "gain";
    auto routed = f.bus->execute({"modulation.add", add, origin});
    REQUIRE_MESSAGE(routed, (routed ? "" : routed.error().toString()));
    const std::string original = routed->find("modulation")->asString();
    // The rest of the routes go into a gain on the track's chain, which the
    // overdub does not copy.
    auto onTrack = f.bus->execute(
        {"fx.add", json::parse(R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})").value(),
         origin});
    REQUIRE_MESSAGE(onTrack, (onTrack ? "" : onTrack.error().toString()));
    {
        const model::Session& head = f.app->head();
        model::EditTxn t(head, head.version + 1);
        const model::Id modOut{model::Kind::Module, 2};
        const model::Id target = *model::Id::parse(onTrack->find("fx")->asString());
        for (std::size_t i = head.modulations.size(); i < model::kMaxModulations; ++i)
            t.create(model::Modulation{{modOut, "in_1"}, {target, "p" + std::to_string(i)}, 0.1});
        auto full = t.commit();
        REQUIRE_MESSAGE(full, (full ? "" : full.error().toString()));
        REQUIRE(writeFileAtomic(f.dir / "full.json", model::serializeProject(*full)));
    }
    f.run("project.open", R"({"path":"full.json"})");
    REQUIRE(f.app->head().modulations.size() == model::kMaxModulations);
    (void)f.app->takeEvents();
    auto done = f.app->commitTakes({f.played(0.5, 1.0)});
    REQUIRE(done);
    std::vector<json::Value> warnings;
    for (auto& [topic, payload] : f.app->takeEvents())
        if (topic == "record.warning") warnings.push_back(payload);
    std::string seen;
    for (const json::Value& w : warnings) seen += json::writeCompact(w) + "\n";
    INFO(seen);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("code")->asString() == "modulation.not_copied");
    CHECK(warnings[0].find("path")->asString() == "/modulations/" + original);
    REQUIRE(warnings[0].find("reason"));
    CHECK(warnings[0].find("reason")->asString() == "limit");
    CHECK(warnings[0].find("track")->asString() == "tr_1");
    // The commit's own reply says the same.
    REQUIRE((*done)[0].warnings);
    CHECK((*done)[0].warnings->front().reason == model::RouteWarningReason::Limit);
    CHECK(f.app->head().modulations.size() == model::kMaxModulations);
}

TEST_CASE("midi_overdub keeps its own rule: a take with the played notes added") {
    MidiFixture f;
    f.run("track.set", R"({"track":"tr_1","record_mode":"midi_overdub"})");
    auto done = f.app->commitTakes({f.played(0.5, 1.0)});
    REQUIRE(done);
    const model::Item& item = *f.app->head().items.find(model::Id{model::Kind::Item, 1});
    REQUIRE(item.takes.size() == 2);
    // Overdub: the old notes (C4) and the played one (E4) together.
    CHECK(f.notes(item.takes[1]).notes.size() == 2);
}

// Round 134 G3/G4: a commit's own warnings go out as record.warning events once the takes are in
// the project, so a client that did not stop the recording (the GUI, when an agent, a MIDI
// controller or the engine stopped it) still sees where each take went; once each, and none for a
// commit that adds none.
TEST_CASE("a commit's warnings are record.warning events, once each, after the commit") {
    SUBCASE("a pass into a locked item") {
        StackFixture f;
        f.run("item.set", R"({"item":"it_1","locked":true})");
        (void)f.app->takeEvents();
        auto done = f.app->commitTakes({f.pass("p.wav", 0.5, 0.5)});
        REQUIRE(done);
        REQUIRE(StackFixture::warned((*done)[0], "record.stack_skipped"));
        std::vector<json::Value> warnings;
        for (auto& [topic, payload] : f.app->takeEvents())
            if (topic == "record.warning") warnings.push_back(payload);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].find("code")->asString() == "record.stack_skipped");
        CHECK(warnings[0].find("message")->asString() == (*done)[0].warnings->front().message);
        CHECK(warnings[0].find("track")->asString() == "tr_1");
        CHECK_FALSE(warnings[0].find("hint")->asString().empty());
    }
    SUBCASE("a pass into an item that takes it says nothing") {
        StackFixture f;
        (void)f.app->takeEvents();
        auto done = f.app->commitTakes({f.pass("p.wav", 0.5, 0.5)});
        REQUIRE(done);
        CHECK((*done)[0].stacked);
        for (auto& [topic, payload] : f.app->takeEvents()) CHECK(topic != "record.warning");
    }
}
