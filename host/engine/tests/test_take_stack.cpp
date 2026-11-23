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
#include <numbers>
#include <string>

#include "../../../tests/file_platform.h"
#include "command_helpers.h"
#include "realtime_common.h"
#include "supple/audio/audio_file.h"
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
    explicit StackFixture(double baseSeconds = 2.0) {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
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
        fs::create_directories(app->mediaDir());
        const auto file = app->mediaDir() / name;
        REQUIRE(audio::writeWav(file, data));
        return file;
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
    RecordedTake pass(const std::string& name, double from, double seconds, double hz = 440.0) {
        RecordedTake r;
        r.track = model::Id{model::Kind::Track, 1};
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

TEST_CASE("a pass over an item at a pitch-keeping rate reads the new take by the timeline too") {
    StackFixture f(4.0);
    // The item plays at half speed with its pitch kept (the stretch path reads the take).
    f.run("take.set", R"({"take":"tk_1","playrate":0.5,"preserve_pitch":true})");
    const model::Session before = f.app->head();
    auto done = f.app->commitTakes({f.pass("rate.wav", 1.0, 1.0)});
    REQUIRE(done);
    CHECK(f.pieces().size() >= 2);
    const auto mixed = f.render(f.app->head(), 4.0);
    double peak = 0;
    for (std::size_t n = 0; n < mixed.size(); ++n)
        peak = std::max(peak, double(std::fabs(mixed[n])));
    CHECK(peak > 0.1);
    CHECK(f.render(f.app->head(), 4.0) == mixed);  // deterministic
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
