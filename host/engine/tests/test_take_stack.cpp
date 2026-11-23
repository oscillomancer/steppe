// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Recording over an item makes a take of it (ADR-0103).
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>

#include "../../../tests/file_platform.h"
#include "command_helpers.h"
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
    StackFixture() {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name":"Audio"})");
        const auto file = writeTone("base.wav", 2.0, 220.0);
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

TEST_CASE("a pass inside an item is a heard take of it, and the item does not change") {
    StackFixture f;
    const model::Item before = f.item(1);
    // 0.5 s long, heard from 0.5 s: inside the item's two seconds.
    auto done = f.app->commitTakes({f.pass("inside.wav", 0.5, 0.5)});
    REQUIRE_MESSAGE(done, (done ? "" : done.error().toString()));
    REQUIRE(done->size() == 1);
    CHECK(f.app->head().items.size() == 1);  // no new item
    const model::Item& item = f.item(1);
    CHECK(item.position == before.position);
    CHECK(item.length == before.length);
    REQUIRE(item.takes.size() == 2);
    CHECK(item.takes[0] == before.takes[0]);
    CHECK(*item.activeTake == item.takes[1]);  // the new pass is what you hear
    // The take reads the recording from the item's start: silent for the first half second.
    CHECK(f.take(item.takes[1]).startOffset == doctest::Approx(-0.5));
    CHECK(f.take(item.takes[1]).name == "inside");
    CHECK((*done)[0].stacked);
    CHECK((*done)[0].item == "it_1");
    CHECK((*done)[0].take == item.takes[1].str());
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
            CHECK(f.app->head().items.size() == 1);
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

TEST_CASE("loop passes over an item are takes of it, in order, the newest whole one heard") {
    StackFixture f;
    // Two passes of the loop [0, 1 s): both are whole passes of a one-second plan, over the item.
    RecordedTake r = f.pass("loop.wav", 0.0, 2.0);
    r.loop.start = 0;
    r.loop.end = static_cast<std::int64_t>(kRate);
    r.segments = {{0, 0}, {static_cast<std::int64_t>(kRate), 0}};
    auto done = f.app->commitTakes({r});
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 1);
    const model::Item& item = f.item(1);
    REQUIRE(item.takes.size() == 3);  // the original and two passes
    CHECK(f.take(item.takes[1]).name == "loop pass 1");
    CHECK(f.take(item.takes[2]).name == "loop pass 2");
    CHECK(f.take(item.takes[2]).startOffset ==
          doctest::Approx(1.0));  // pass 2 starts one second in
    CHECK(*item.activeTake == item.takes[2]);
    REQUIRE((*done)[0].takes.size() == 2);
}

TEST_CASE("a pass over a comp's pieces is a take of every piece, and the comp stays a comp") {
    StackFixture f;
    f.run("take.add", R"({"item":"it_1","from":"tk_1"})");   // a second take: it is comp material
    f.run("item.split", R"({"item":"it_1","at":2})");        // it_1 and it_2: one run of two
    f.run("take.activate", R"({"item":"it_2","index":1})");  // it_2 plays the second take
    const model::Id second{model::Kind::Take, 2};
    (void)second;
    // The pass lies inside the first piece (beats 0 to 2, 0 to 1 s) only.
    auto done = f.app->commitTakes({f.pass("run.wav", 0.25, 0.5)});
    REQUIRE(done);
    CHECK(f.app->head().items.size() == 2);
    const model::Item& first = f.item(1);
    const model::Item& next = f.item(2);
    REQUIRE(first.takes.size() == 3);
    REQUIRE(next.takes.size() == 3);             // the run stays take-compatible
    CHECK(*first.activeTake == first.takes[2]);  // touched: plays the pass
    CHECK(*next.activeTake == next.takes[1]);    // untouched: keeps its take
    // Both read the one recording by the same timeline mapping: the second piece starts at 1 s,
    // 0.75 s into the file, which lasts 0.5 s: past its end, silent there.
    CHECK(f.take(first.takes[2]).startOffset == doctest::Approx(-0.25));
    CHECK(f.take(next.takes[2]).startOffset == doctest::Approx(0.75));
    CHECK(f.take(first.takes[2]).name == f.take(next.takes[2]).name);
    // take.comp.get still reads the two as one comp.
    const auto got = f.run("take.comp.get", R"({"item":"it_1"})");
    CHECK(got.find("pieces")->asArray().size() == 2);
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
