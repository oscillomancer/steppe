// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "../../../tests/file_platform.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

TEST_CASE("output record mode loads but recording fails before creating files") {
    struct Root {
        fs::path path = fs::temp_directory_path() /
                        ("supple-record-output-" + std::to_string(test::processId()));
        Root() { fs::remove_all(path); }
        ~Root() { fs::remove_all(path); }
    } root;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    auto transaction = app.beginTransaction({cmd::Origin::Type::Test, "output-mode", ""}, "Arm");
    REQUIRE(transaction);
    // An ordinary armed input precedes the unsupported output track: neither
    // may create a file if any track in the recording is unsupported.
    model::Id output;
    for (const bool unsupported : {false, true}) {
        model::Track track;
        track.parent = model::kMaster;
        track.record.armed = true;
        track.record.mode = unsupported ? model::RecordMode::Output : model::RecordMode::Input;
        const auto id = app.txn().create(track);
        model::Route route;
        route.from.hwChannels = std::vector<int>{1, 2};
        route.to.track = id;
        const auto input = app.txn().create(route);
        app.txn().modify<model::Track>(id, [&](auto& value) { value.record.input = input; });
        app.txn().modify<model::Track>(model::kMaster,
                                       [&](auto& value) { value.children.push_back(id); });
        if (unsupported) output = id;
    }
    REQUIRE(app.commitTransaction(*transaction));
    REQUIRE(app.save("project.json"));
    REQUIRE(app.open("project.json"));
    CHECK(app.head().tracks.find(output)->record.mode == model::RecordMode::Output);
    REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                            {.anticipative = false, .workers = 0}));
    const auto result = app.transport("record");
    CHECK_FALSE(result);
    if (!result) {
        CHECK(result.error().code == "record.output_unsupported");
        CHECK(result.error().path == "/tracks/" + output.str() + "/record/mode");
        CHECK_FALSE(result.error().message.empty());
        CHECK(result.error().hint.find("not supported yet") != std::string::npos);
        CHECK(result.error().hint.find("input") != std::string::npos);
    }
    CHECK_FALSE(app.engine()->recording());
    CHECK_FALSE(fs::exists(app.recordDir()));
    app.stopEngine();
}

TEST_CASE("engine shutdown commits captured audio and MIDI to the session") {
    struct Root {
        fs::path path =
            fs::temp_directory_path() / ("supple-record-stop-" + std::to_string(test::processId()));
        Root() { fs::remove_all(path); }
        ~Root() { fs::remove_all(path); }
    } root;
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto dispatcher = std::make_unique<cmd::Dispatcher>(registry, app);
    auto call = [&](std::string_view name, std::string_view args) {
        auto result = dispatcher->execute(
            {std::string(name), *json::parse(args), {cmd::Origin::Type::Test, "shutdown", ""}});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    };
    call("track.add", R"({"name":"Audio"})");
    call("track.set", R"({"track":"tr_1","armed":true,"record_input":[1,2]})");
    call("track.add", R"({"name":"MIDI"})");
    call("track.set", R"({"track":"tr_2","armed":true,"record_midi":"*"})");
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input =
             [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
                 for (std::uint32_t c = 0; c < channels; ++c) std::fill_n(in[c], n, 0.25f);
             },
         .midiPorts = {"Test keyboard"},
         .midi =
             [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
                 for (std::int64_t f = frame; f < frame + n; ++f) {
                     if (f != 256 && f != 768) continue;
                     MidiInputEvent event;
                     event.frame = static_cast<std::uint32_t>(f - frame);
                     event.size = 3;
                     event.bytes[0] = f == 256 ? 0x90 : 0x80;
                     event.bytes[1] = 60;
                     event.bytes[2] = f == 256 ? 100 : 0;
                     out.push_back(event);
                 }
             }});
    REQUIRE(app.startEngine(
        backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .midiInputs = true},
        {.anticipative = false, .workers = 0}));
    call("transport.record", "{}");
    backend.run(4);
    const auto before = app.head().version;
    SUBCASE("engine.stop command") { call("engine.stop", "{}"); }
    SUBCASE("dispatcher already gone during app teardown") {
        dispatcher.reset();
        app.stopEngine();
    }
    SUBCASE("engine already stopped by its owner") {
        app.engine()->stop();
        app.stopEngine();
    }
    CHECK(app.engine() == nullptr);
    REQUIRE(app.head().items.size() == 2);
    REQUIRE(app.head().media.size() == 1);
    REQUIRE(app.head().midi.size() == 1);
    const auto& audioSource = *app.head().media.begin()->value;
    auto audio = audio::readAudio(audioSource.path);
    REQUIRE(audio);
    CHECK(audio->frames() == 1024);
    CHECK(audio->channels[0][256] == 0.25f);
    const auto& sequence = app.head().midi.begin()->value->sequence;
    REQUIRE(sequence.notes.size() == 1);
    CHECK(sequence.notes[0].pitch == 60);
    CHECK(app.head().version != before);
    REQUIRE(app.save("recorded.json"));
    auto reopened = model::parseProject(model::serializeProject(app.head()));
    REQUIRE(reopened);
    CHECK(reopened->midi.begin()->value->sequence.notes.size() == 1);
    REQUIRE(app.undo());
    CHECK(app.head().version == before);
    CHECK(app.head().items.empty());
}

TEST_CASE("MIDI sends: armed destinations record internal MIDI without a device or instrument") {
    struct Root {
        fs::path path = fs::temp_directory_path() /
                        ("supple-midi-send-record-" + std::to_string(test::processId()));
        Root() { fs::remove_all(path); }
        ~Root() { fs::remove_all(path); }
    } root;
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    auto call = [&](const char* command, std::string_view args) {
        auto result = dispatcher.execute(
            {command, *json::parse(args), {cmd::Origin::Type::Test, "midi-send", ""}});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    call("track.add", R"({"name":"Source"})");
    call("track.add", R"({"name":"Recorder"})");
    call("item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:e r:*"})");
    call("route.add", R"({"from":"tr_1","to":"tr_2","midi":{}})");
    call("track.set",
         R"({"track":"tr_2","armed":true,"monitor":"off","record_mode":"midi_overdub"})");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 256},
                            {.anticipative = false, .workers = 0}));
    call("transport.record", "{}");
    backend.run(96);
    call("transport.stop", "{}");
    app.stopEngine();
    const auto target = model::Id::parse("tr_2");
    std::size_t notes = 0;
    for (const auto& item : app.head().items) {
        if (item.value->track != *target || !item.value->activeTake) continue;
        const auto* take = app.head().takes.find(*item.value->activeTake);
        if (take && take->source)
            if (const auto* midi = app.head().midi.find(*take->source)) {
                notes += midi->sequence.notes.size();
                // Where the source plays it: the first beat, an eighth long
                // (one tick of tolerance for the beat conversion).
                const double ppq = midi->sequence.ppq;
                for (const auto& note : midi->sequence.notes) {
                    const double start =
                        item.value->position - take->startOffset + double(note.start) / ppq;
                    CHECK(std::abs(start) <= 1 / ppq);
                    CHECK(std::abs(double(note.length) / ppq - 0.5) <= 1 / ppq);
                    CHECK(note.pitch == 60);
                }
            }
    }
    CHECK(notes == 1);
}
