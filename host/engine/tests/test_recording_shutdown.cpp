// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

#include "../../../tests/file_platform.h"
#include "../../../tests/wav_stream_test_support.h"
#include "recording.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/main_thread_wake.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace supple::engine {
struct RecordingErrorTestAccess {
    static void clock(detail::Recorder& recorder, std::atomic<std::int64_t>& milliseconds) {
        recorder.clockContext_ = &milliseconds;
        recorder.nowForTests_ = [](void* context) {
            return std::chrono::steady_clock::time_point(std::chrono::milliseconds(
                static_cast<std::atomic<std::int64_t>*>(context)->load()));
        };
    }
    static void fault(detail::Recorder& recorder, std::size_t index,
                      audio::WavStreamWriterTestAccess::Fault& fault) {
        audio::WavStreamWriterTestAccess::inject(recorder.tracks_[index]->writer, fault);
    }
    static void passes(detail::Recorder& recorder, unsigned count = 2) {
        const auto target = recorder.writerPasses_.load() + count;
        for (;;) {
            const auto at = recorder.writerPasses_.load(std::memory_order_acquire);
            if (at >= target) return;
            recorder.writerPasses_.wait(at);
        }
    }
    static void failNextAudioWrite(Engine& engine, audio::WavStreamWriterTestAccess::Fault& fault) {
        auto& recorder = *engine.recorderOwned_;
        recorder.writerStalled_.store(true);
        while (!recorder.writerStallAck_.load()) recorder.writerStallAck_.wait(false);
        audio::WavStreamWriterTestAccess::inject(recorder.tracks_[0]->writer, fault);
        recorder.writerStalled_.store(false);
    }
    static void awaitError(Engine& engine) {
        auto& pending = engine.recorderOwned_->tracks_[0]->errorPending;
        while (!pending.load()) pending.wait(false);
    }
};
}  // namespace supple::engine

TEST_CASE("audio checkpoints share the five-second journal cadence and isolate sync failures") {
    const auto root =
        fs::temp_directory_path() / ("supple-record-sync-" + std::to_string(test::processId()));
    REQUIRE(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    std::atomic<std::int64_t> clock{0};
    audio::WavStreamWriterTestAccess::Fault failed, healthy;
    MainThreadWake wake;
    detail::Recorder recorder({{model::Id{model::Kind::Track, 1}, {0}, root / "failed.wav", {}},
                               {model::Id{model::Kind::Track, 2}, {0}, root / "healthy.wav", {}}},
                              48000, 0);
    RecordingErrorTestAccess::clock(recorder, clock);
    RecordingErrorTestAccess::fault(recorder, 0, failed);
    RecordingErrorTestAccess::fault(recorder, 1, healthy);
    recorder.setMainThreadWake(&wake);
    REQUIRE(recorder.open());
    REQUIRE(failed.syncCalls.load() == 1);
    std::array<float, 64> samples;
    samples.fill(.25f);
    const float* input[]{samples.data()};
    recorder.capture(input, 1, 64, 0);
    RecordingErrorTestAccess::passes(recorder);
    clock.store(4999);
    RecordingErrorTestAccess::passes(recorder);
    CHECK(failed.syncCalls.load() == 1);
    auto info = audio::inspectWavStream(root / "failed.wav");
    REQUIRE(info);
    CHECK(info->headerFrames == 0);
    CHECK(info->fileFrames == 64);
    clock.store(5000);
    RecordingErrorTestAccess::passes(recorder);
    REQUIRE(failed.syncCalls.load() == 2);
    REQUIRE(healthy.syncCalls.load() == 2);
    info = audio::inspectWavStream(root / "failed.wav");
    REQUIRE(info);
    CHECK(info->headerFrames == 64);
    failed.failSync = true;
    clock.store(10000);
    RecordingErrorTestAccess::passes(recorder);
    WaitItem ready{wake.waitable(), kWaitRead};
    REQUIRE(waitFor(std::span(&ready, 1), -1) == 1);
    CHECK(wake.drain());
    const auto errors = recorder.takeErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].error.code == "file.sync");
    CHECK(errors[0].file == root / "failed.wav");
    CHECK(recorder.takeErrors().empty());
    recorder.capture(input, 1, 64, 64);
    RecordingErrorTestAccess::passes(recorder);
    const auto takes = recorder.finish();
    REQUIRE(takes.size() == 2);
    CHECK(takes[0].frames == 64);
    CHECK(takes[0].overflowFrames == 64);
    CHECK(takes[0].failure->code == "file.sync");
    CHECK(takes[1].frames == 128);
    CHECK(takes[1].error.empty());
    CHECK(failed.syncThread != std::this_thread::get_id());
    CHECK(healthy.syncThread != std::this_thread::get_id());
    REQUIRE(audio::readAudio(root / "healthy.wav"));
}

TEST_CASE("audio write failure wakes the model before stop and retains the physical prefix") {
    struct Root {
        fs::path path = fs::temp_directory_path() /
                        ("supple-record-live-error-" + std::to_string(test::processId()));
        Root() { REQUIRE(fs::create_directory(path)); }
        ~Root() { fs::remove_all(path); }
    } root;
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c) std::fill_n(in[c], n, .25f);
         }});
    audio::WavStreamWriterTestAccess::Fault fault{.bytes = 106};
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    const auto txn = app.beginTransaction({cmd::Origin::Type::Test, "write-error", ""}, "Arm");
    REQUIRE(txn);
    for (int channel = 0; channel < 2; ++channel) {
        model::Track track;
        track.name = channel == 0 ? "Failed" : "Healthy";
        track.parent = model::kMaster;
        track.record.armed = true;
        const auto id = app.txn().create(track);
        model::Route route;
        route.from.hwChannels = std::vector<int>{1};
        route.to.track = id;
        const auto input = app.txn().create(route);
        app.txn().modify<model::Track>(id, [&](auto& value) { value.record.input = input; });
        app.txn().modify<model::Track>(model::kMaster,
                                       [&](auto& value) { value.children.push_back(id); });
    }
    REQUIRE(app.commitTransaction(*txn));
    REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1},
                            {.anticipative = false, .workers = 0}));
    REQUIRE(app.transport("record"));
    RecordingErrorTestAccess::failNextAudioWrite(*app.engine(), fault);
    backend.run(2);
    RecordingErrorTestAccess::awaitError(*app.engine());
    app.tick();
    std::size_t errors = 0;
    for (const auto& [topic, payload] : app.takeEvents())
        if (topic == "record.error") {
            ++errors;
            REQUIRE(payload.find("code"));
            REQUIRE(payload.find("hint"));
            REQUIRE(payload.find("file"));
            CHECK(payload.find("code")->asString() == "file.write");
            CHECK_FALSE(payload.find("hint")->asString().empty());
            CHECK(payload.find("file")->asString().find("Failed-1.wav") != std::string::npos);
        }
    CHECK(errors == 1);
    CHECK(app.engine()->recording());
    backend.run(2);
    const auto stopped = app.transport("stop");
    REQUIRE(stopped);
    REQUIRE(stopped->recorded);
    REQUIRE(stopped->recorded->size() == 2);
    for (const auto& info : *stopped->recorded)
        if (info.file.find("Failed-1.wav") != std::string::npos) CHECK(info.lostFrames == 486);
    for (const auto& [topic, payload] : app.takeEvents()) {
        (void)payload;
        CHECK(topic != "record.error");
    }
    const auto failed = audio::readAudio(app.recordDir() / "Failed-1.wav");
    const auto healthy = audio::readAudio(app.recordDir() / "Healthy-1.wav");
    REQUIRE(failed);
    REQUIRE(healthy);
    CHECK(failed->channels[0] == std::vector<float>(26, .25f));
    CHECK(healthy->channels[0] == std::vector<float>(512, .25f));
    app.stopEngine();
}

TEST_CASE("simultaneous recording names are distinct after ASCII case folding") {
    struct Root {
        fs::path path =
            fs::temp_directory_path() /
            ("supple-record-case-" + std::to_string(test::processId()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Root() { REQUIRE(fs::create_directory(path)); }
        ~Root() { fs::remove_all(path); }
    } root;
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t frames, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 std::fill_n(in[c], frames, c == 0 ? .25f : -.5f);
         }});
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    auto txn = app.beginTransaction({cmd::Origin::Type::Test, "record-case", ""}, "Arm");
    REQUIRE(txn);
    for (int channel = 0; channel < 2; ++channel) {
        model::Track track;
        track.name = channel == 0 ? "Vocal" : "vocal";
        track.parent = model::kMaster;
        track.record.armed = true;
        const auto id = app.txn().create(track);
        model::Route route;
        route.from.hwChannels = std::vector<int>{channel + 1};
        route.to.track = id;
        const auto input = app.txn().create(route);
        app.txn().modify<model::Track>(id, [&](auto& value) { value.record.input = input; });
        app.txn().modify<model::Track>(model::kMaster,
                                       [&](auto& value) { value.children.push_back(id); });
    }
    REQUIRE(app.commitTransaction(*txn));
    REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2},
                            {.anticipative = false, .workers = 0}));
    REQUIRE(app.transport("record"));
    backend.run(4);
    REQUIRE(app.transport("stop"));
    CHECK(fs::exists(app.recordDir() / "Vocal-1.wav"));
    CHECK(fs::exists(app.recordDir() / "vocal-2.wav"));
    const auto first = audio::readAudio(app.recordDir() / "Vocal-1.wav");
    const auto second = audio::readAudio(app.recordDir() / "vocal-2.wav");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->frames() > 0);
    CHECK(first->channels[0] == std::vector<float>(first->frames(), .25f));
    CHECK(second->channels[0] == std::vector<float>(second->frames(), -.5f));
    app.stopEngine();
}

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

namespace {
// Polls a recording's MIDI journal until it holds `events` events (the
// writer thread appends every few milliseconds), as a crash would find it.
std::optional<detail::MidiCapture> journalWith(const fs::path& file, std::size_t events) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        if (auto c = detail::readMidiJournal(file, file.parent_path());
            c && c->events.size() >= events)
            return *c;
        if (std::chrono::steady_clock::now() > deadline) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void sameTake(const RecordedTake& a, const RecordedTake& b) {
    CHECK(a.track == b.track);
    CHECK(a.startSample == b.startSample);
    CHECK(a.frames == b.frames);
    CHECK(a.loop.start == b.loop.start);
    CHECK(a.loop.end == b.loop.end);
    REQUIRE(a.segments.size() == b.segments.size());
    for (std::size_t i = 0; i < a.segments.size(); ++i) {
        CHECK(a.segments[i].fileFrame == b.segments[i].fileFrame);
        CHECK(a.segments[i].timeline == b.segments[i].timeline);
    }
    REQUIRE(a.midi.size() == b.midi.size());
    for (std::size_t i = 0; i < a.midi.size(); ++i) {
        CHECK(a.midi[i].timeline == b.midi[i].timeline);
        CHECK(a.midi[i].run == b.midi[i].run);
        CHECK(std::equal(a.midi[i].bytes, a.midi[i].bytes + 3, b.midi[i].bytes));
        CHECK(a.midi[i].route == b.midi[i].route);
        CHECK(a.midi[i].voice == b.midi[i].voice);
    }
}
}  // namespace

TEST_CASE("a MIDI take's journal places what stopping would commit, written off the audio thread") {
    // ADR-0080. capture(), captureMidi() and captureRoutedMidi() are the
    // audio thread's (SUPPLE_RT, checked under the rtsan preset): they only
    // push to the rings and publish how far capture got; the writer thread
    // journals. A journal read while recording places the take as finish()
    // does, loop passes included.
    const fs::path dir =
        fs::temp_directory_path() / ("supple-midi-journal-" + std::to_string(test::processId()));
    fs::remove_all(dir);
    const model::Id track{model::Kind::Track, 2}, route{model::Kind::Route, 1};
    RecordTarget target;
    target.track = track;
    const bool routed = GENERATE(false, true);
    target.midi = RecordTarget::Midi{1, 0, 0, routed, routed ? 40 : 0};
    target.file = dir / "Keys-1.midi.jsonl";
    detail::Recorder recorder({target}, 48000, 100);
    recorder.setLoop({1000, 1256, true});
    REQUIRE(recorder.open());
    auto note = [](std::uint32_t frame, std::uint8_t status, std::uint8_t pitch) {
        MidiInputEvent e;
        e.frame = frame;
        e.size = 3;
        e.bytes[0] = status;
        e.bytes[1] = pitch;
        e.bytes[2] = status == 0x90 ? 100 : 0;
        return e;
    };
    // Two loop passes over [1000, 1256): a note in each, one held over the
    // wrap; with routed input, a Note Out voice too.
    const MidiInputEvent first[] = {note(10, 0x90, 60), note(200, 0x80, 60), note(250, 0x90, 64)};
    const MidiInputEvent second[] = {note(5, 0x80, 64), note(30, 0x90, 67)};
    detail::RoutedMidiEvent voice;
    voice.frame = 20;
    voice.bytes[0] = 0x90;
    voice.bytes[1] = 72;
    voice.bytes[2] = 90;
    voice.route = route;
    voice.voice = 3;
    recorder.capture(nullptr, 0, 256, 1000);
    recorder.captureMidi(first, 0, 256, 1000);
    if (routed) recorder.captureRoutedMidi(track, {&voice, 1}, 256);
    recorder.capture(nullptr, 0, 256, 1000);  // the loop wrapped
    recorder.captureMidi(second, 0, 256, 1000);
    const std::size_t events = 5 + (routed ? 1 : 0);
    const auto crashed = journalWith(target.file, events);
    REQUIRE(crashed);
    const RecordedTake recovered = detail::placeMidi(*crashed);
    const auto takes = recorder.finish({1000, 1256, true});
    REQUIRE(takes.size() == 1);
    const RecordedTake& stopped = takes.front();
    CHECK(stopped.file == target.file);
    CHECK(stopped.warnings.empty());
    CHECK(stopped.segments.size() == 2);  // two passes
    sameTake(recovered, stopped);
    // The journal stays (the app deletes it once a save holds the take), and
    // reads back as the whole take.
    auto after = detail::readMidiJournal(target.file, dir);
    REQUIRE(after);
    sameTake(detail::placeMidi(*after), stopped);
    fs::remove_all(dir);
}

TEST_CASE("a MIDI journal's torn or damaged tail is left out") {
    const fs::path dir = fs::temp_directory_path() /
                         ("supple-midi-journal-torn-" + std::to_string(test::processId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string header =
        R"({"device":true,"latency":0,"loop":[0,0],"routed":false,"sample_rate":48000,)"
        R"("schema":"supple.midi-recording/1.0","shift":0,"start_sample":48000,"track":"tr_1"})"
        "\n";
    const fs::path file = dir / "Keys-1.midi.jsonl";
    bool torn = false;
    SUBCASE("a torn last line") {
        REQUIRE(writeFileAtomic(file, header + "[0,144,60,100]\n{\"captured\":256}\n[300,128,6"));
        auto c = detail::readMidiJournal(file, dir, &torn);
        REQUIRE(c);
        CHECK(torn);
        CHECK(c->events.size() == 1);
        CHECK(c->captured == 256);
    }
    SUBCASE("a damaged line: nothing after it is trusted") {
        REQUIRE(writeFileAtomic(file, header + "[0,144,60,100]\n[0,999,1,1]\n[10,128,60,0]\n"));
        auto c = detail::readMidiJournal(file, dir, &torn);
        REQUIRE(c);
        CHECK(torn);
        CHECK(c->events.size() == 1);
    }
    SUBCASE("no header") {
        REQUIRE(writeFileAtomic(file, "[0,144,60,100]\n"));
        auto c = detail::readMidiJournal(file, dir, &torn);
        REQUIRE_FALSE(c);
        CHECK(c.error().code == "record.journal");
    }
    fs::remove_all(dir);
}

TEST_CASE("a MIDI journal holds only what its writer writes: channel messages and bounded clocks") {
    // Round-10 review, findings 1 and 2: a data byte over 127 reached the
    // project (it saved, then would not reopen), and header clocks reached
    // placement's arithmetic unbounded (signed overflow).
    const fs::path dir = fs::temp_directory_path() /
                         ("supple-midi-journal-bounds-" + std::to_string(test::processId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "Keys-1.midi.jsonl";
    const std::string far = std::to_string(std::int64_t(1) << 62);
    auto header = [](std::string_view fields) {
        return std::string(R"({"schema":"supple.midi-recording/1.0","track":"tr_1",)") +
               std::string(fields) + "}\n";
    };
    const std::string device = header(
        R"("sample_rate":48000,"start_sample":48000,"latency":0,"device":true,"routed":false,)"
        R"("shift":0,"loop":[0,0])");
    const std::string routed = header(
        R"("sample_rate":48000,"start_sample":48000,"latency":0,"device":false,"routed":true,)"
        R"("shift":0,"loop":[0,0])");
    const std::string route = model::Id{model::Kind::Route, 1}.str();
    SUBCASE("a damaged event, run or count ends what is trusted") {
        struct Case {
            const std::string* head;
            std::string line;
        };
        const Case cases[] = {
            {&device, "[10,144,255,100]"},          // data byte over 127
            {&device, "[10,144,60,128]"},           // velocity over 127
            {&device, "[10,112,60,0]"},             // a data byte where the status goes
            {&device, "[10,248,0,0]"},              // system real-time: never captured
            {&device, "[10,240,0,0]"},              // system exclusive: never captured
            {&device, "[" + far + ",144,60,100]"},  // beyond any timeline
            {&device, "{\"run\":[" + far + ",0]}"},
            {&device, "{\"run\":[0," + far + "]}"},
            {&device, "{\"captured\":" + far + "}"},
            {&device, "[10,144,60,100,\"" + route + "\",1]"},  // a routed event, unrouted take
            {&routed, "[10,144,60,100]"},                      // a device event, no device
            {&routed, "[10,144,60,100,\"" + route + "\",-1]"},
            {&routed, "[10,144,60,100,\"" + route + "\",2147483648]"},
        };
        for (const Case& c : cases) {
            INFO(c.line);
            const std::string good =
                c.head == &device ? "[0,144,60,100]\n" : "[0,144,60,100,\"" + route + "\",1]\n";
            REQUIRE(writeFileAtomic(file, *c.head + good + c.line + "\n[20,128,60,0]\n"));
            bool torn = false;
            auto read = detail::readMidiJournal(file, dir, &torn);
            REQUIRE(read);
            CHECK(torn);
            CHECK(read->events.size() == 1);
            CHECK(read->captured == 0);
        }
    }
    SUBCASE("a damaged header is not a journal") {
        const std::string headers[] = {
            header(R"("sample_rate":48000,"start_sample":0,"latency":9223372036854775807,)"
                   R"("device":true,"routed":true,"shift":-9223372036854775808,"loop":[0,0])"),
            header(R"("sample_rate":48000,"start_sample":0,"latency":-1,"device":true,)"
                   R"("routed":false,"shift":0,"loop":[0,0])"),
            header(R"("sample_rate":48000,"start_sample":)" + far +
                   R"(,"latency":0,"device":true,"routed":false,"shift":0,"loop":[0,0])"),
            header(R"("sample_rate":48000,"start_sample":0,"latency":0,"device":false,)"
                   R"("routed":true,"shift":)" +
                   far + R"(,"loop":[0,0])"),
            header(R"("sample_rate":48000,"start_sample":0,"latency":0,"device":true,)"
                   R"("routed":false,"shift":0,"loop":[0,)" +
                   far + "]"),
            header(R"("sample_rate":48000,"start_sample":0,"latency":0,"device":true,)"
                   R"("routed":false,"shift":0,"loop":[-5,10])"),
            header(R"("sample_rate":48000,"start_sample":0,"latency":0,"device":false,)"
                   R"("routed":false,"shift":0,"loop":[0,0])"),  // records nothing
        };
        for (const std::string& h : headers) {
            INFO(h);
            REQUIRE(writeFileAtomic(file, h + "[0,144,60,100]\n{\"captured\":1}\n"));
            auto read = detail::readMidiJournal(file, dir);
            REQUIRE_FALSE(read);
            CHECK(read.error().code == "record.journal");
        }
    }
    fs::remove_all(dir);
}

TEST_CASE("a MIDI take a crash interrupted comes back from its journal, and goes once saved") {
    // ADR-0080: a MIDI take had no crash-recovery file; now its journal is
    // offered by project.recoverable and placed by project.recover as stop
    // would have committed it. Once a saved file holds the take, it goes.
    struct Root {
        fs::path path =
            fs::temp_directory_path() / ("supple-midi-crash-" + std::to_string(test::processId()));
        Root() { fs::remove_all(path); }
        ~Root() { fs::remove_all(path); }
    } root;
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    auto call = [&](std::string_view name, std::string_view args) {
        auto result = dispatcher.execute(
            {std::string(name), *json::parse(args), {cmd::Origin::Type::Test, "crash", ""}});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    call("track.add", R"({"name":"Keys"})");
    call("track.set", R"({"track":"tr_1","armed":true,"record_midi":"*"})");
    REQUIRE(app.save(root.path / "song" / "song.json"));  // what the crash leaves on disk
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .midiPorts = {"Test keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
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
    REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true},
                            {.anticipative = false, .workers = 0}));
    call("transport.record", "{}");
    backend.run(4);
    const fs::path journal = app.recordDir() / "Keys-1.midi.jsonl";
    // As a crash would find it: written while recording, never finished.
    REQUIRE(journalWith(journal, 2));
    const std::string crashed = *readTextFile(journal);
    CHECK(app.recoverable().empty());  // recording now: not a leftover
    call("transport.stop", "{}");
    REQUIRE(app.head().midi.size() == 1);
    const auto stopped = app.head().midi.begin()->value->sequence;
    REQUIRE(stopped.notes.size() == 1);
    CHECK(fs::exists(journal));        // kept until a saved file holds the take
    CHECK(app.recoverable().empty());  // and not offered: the project has it

    SUBCASE("a crash before the save: offered and put back as stop committed it") {
        app.stopEngine();
        REQUIRE(app.open(root.path / "song" / "song.json"));  // as the next run opens it
        REQUIRE(writeFileAtomic(journal, crashed));
        auto listed = call("project.recoverable", "{}");
        REQUIRE(listed["recordings"].size() == 1);
        const json::Value& row = listed["recordings"].at(0);
        CHECK(row.find("file")->asString() == "rec/Keys-1.midi.jsonl");
        CHECK(row.find("midi_events")->asInt() == 2);
        CHECK(row.find("track")->asString() == "tr_1");
        auto recovered = call("project.recover", "{}");
        REQUIRE(recovered["recovered"].size() == 1);
        REQUIRE(app.head().midi.size() == 1);
        const auto& back = app.head().midi.begin()->value->sequence;
        REQUIRE(back.notes.size() == 1);
        CHECK(back.notes[0].pitch == stopped.notes[0].pitch);
        CHECK(back.notes[0].start == stopped.notes[0].start);
        CHECK(back.notes[0].length == stopped.notes[0].length);
        CHECK(app.recoverable().empty());
        REQUIRE(app.save(root.path / "song" / "song.json"));
        CHECK_FALSE(fs::exists(journal));
    }
    SUBCASE("saved: the journal goes") {
        REQUIRE(app.save(root.path / "song" / "song.json"));
        CHECK_FALSE(fs::exists(journal));
        app.stopEngine();
        REQUIRE(app.open(root.path / "song" / "song.json"));
        CHECK(app.recoverable().empty());
        CHECK(app.head().midi.size() == 1);
    }
    SUBCASE("not saved: the journal stays, offered on the next open") {
        app.stopEngine();
        REQUIRE(app.open(root.path / "song" / "song.json"));
        CHECK(fs::exists(journal));
        CHECK(app.recoverable().size() == 1);
    }
}

TEST_CASE("a recording keeps its project's folder: no save as into another while it runs") {
    // Round-11 review, finding 2: a save as during a recording copied the
    // journal's (and a WAV's) first part to the new folder while the
    // recorder went on writing the old one; the new file then offered a
    // stale partial take and the old folder lost its journal. As an
    // untitled project's first save, it waits until the recording stops.
    const fs::path dir =
        fs::temp_directory_path() / ("supple-midi-save-as-" + std::to_string(test::processId()));
    fs::remove_all(dir);
    {
        auto created = EngineApp::create({.workdir = dir});
        REQUIRE(created);
        auto& app = **created;
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher dispatcher(registry, app);
        auto call = [&](std::string_view name, std::string_view args) {
            return dispatcher.execute(
                {std::string(name), *json::parse(args), {cmd::Origin::Type::Test, "save-as", ""}});
        };
        REQUIRE(call("track.add", R"({"name":"Keys"})"));
        REQUIRE(call("track.set", R"({"track":"tr_1","armed":true,"record_midi":"*"})"));
        const fs::path first = dir / "first/song.json", second = dir / "second/song.json";
        REQUIRE(app.save(first));
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual,
             .midiPorts = {"Keys"},
             .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
                 for (std::int64_t f = frame; f < frame + n; ++f) {
                     if (f != 0 && f != 256 && f != 1024 && f != 1280) continue;
                     MidiInputEvent e;
                     e.frame = static_cast<std::uint32_t>(f - frame);
                     e.size = 3;
                     e.bytes[0] = f == 0 || f == 1024 ? 0x90 : 0x80;
                     e.bytes[1] = f < 1024 ? 60 : 64;
                     e.bytes[2] = f == 0 || f == 1024 ? 100 : 0;
                     out.push_back(e);
                 }
             }});
        REQUIRE(app.startEngine(backend,
                                {.sampleRate = 48000, .blockSize = 256, .midiInputs = true},
                                {.anticipative = false, .workers = 0}));
        REQUIRE(call("transport.record", "{}"));
        backend.run(4);
        const fs::path journal = app.recordDir() / "Keys-1.midi.jsonl";
        REQUIRE(journalWith(journal, 2));
        auto refused = call("project.save", R"({"path":")" + second.generic_string() + R"("})");
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "project.recording");
        CHECK_FALSE(refused.error().hint.empty());
        CHECK_FALSE(fs::exists(dir / "second"));
        CHECK(app.projectPath() == first);
        REQUIRE(app.save(first));  // in place: the recording's folder stays the project's
        backend.run(4);
        REQUIRE(call("transport.stop", "{}"));
        app.stopEngine();
        REQUIRE(app.head().midi.size() == 1);
        CHECK(app.head().midi.begin()->value->sequence.notes.size() == 2);
        REQUIRE(app.save(second));  // stopped: the save as copies the whole journal
        CHECK_FALSE(fs::exists(dir / "second/media/rec/Keys-1.midi.jsonl"));
        CHECK(fs::exists(journal));  // the first file does not hold the take
        REQUIRE(app.open(second));
        CHECK(app.head().midi.size() == 1);
        CHECK(app.recoverable().empty());
        REQUIRE(app.open(first));
        CHECK(app.head().midi.empty());
        CHECK(app.recoverable().size() == 1);
    }
    fs::remove_all(dir);
}

TEST_CASE("a journal this recording did not create is never the take's, nor deleted at save") {
    // Round-10 review, finding 5: a dangling link at the journal's name was
    // refused by the exclusive creation, yet the take still named it as its
    // journal, and the next save deleted the link.
    const fs::path dir = fs::temp_directory_path() /
                         ("supple-midi-journal-owner-" + std::to_string(test::processId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    SUBCASE("the recorder: a refused journal is not the take's file") {
        RecordTarget target;
        target.track = {model::Kind::Track, 1};
        target.midi = RecordTarget::Midi{1, 0, 0, false, 0};
        target.file = dir / "Keys-1.midi.jsonl";
        REQUIRE(writeFileAtomic(target.file, "someone else's"));
        detail::Recorder recorder({target}, 48000, 0);
        REQUIRE(recorder.open());
        MidiInputEvent on;
        on.size = 3;
        on.bytes[0] = 0x90;
        on.bytes[1] = 60;
        on.bytes[2] = 100;
        recorder.capture(nullptr, 0, 256, 0);
        recorder.captureMidi({&on, 1}, 0, 256, 0);
        const auto takes = recorder.finish();
        REQUIRE(takes.size() == 1);
        CHECK(takes.front().file.empty());
        CHECK(std::any_of(takes.front().warnings.begin(), takes.front().warnings.end(),
                          [](const auto& w) { return w.code == "record.journal"; }));
        CHECK(*readTextFile(target.file) == "someone else's");
    }
    SUBCASE("the app: a dangling link at a journal's name survives recording and saving") {
        auto created = EngineApp::create({.workdir = dir});
        REQUIRE(created);
        auto& app = **created;
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher dispatcher(registry, app);
        auto call = [&](std::string_view name, std::string_view args) {
            auto result = dispatcher.execute(
                {std::string(name), *json::parse(args), {cmd::Origin::Type::Test, "owner", ""}});
            REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        };
        call("track.add", R"({"name":"Keys"})");
        call("track.set", R"({"track":"tr_1","armed":true,"record_midi":"*"})");
        const fs::path saved = dir / "song/song.json";
        REQUIRE(app.save(saved));
        fs::create_directories(app.recordDir());
        const fs::path link = app.recordDir() / "Keys-1.midi.jsonl";
        fs::create_symlink(dir / "absent-target", link);
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual,
             .midiPorts = {"Keys"},
             .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
                 for (std::int64_t f = frame; f < frame + n; ++f) {
                     if (f != 0 && f != 256) continue;
                     MidiInputEvent e;
                     e.frame = static_cast<std::uint32_t>(f - frame);
                     e.size = 3;
                     e.bytes[0] = f == 0 ? 0x90 : 0x80;
                     e.bytes[1] = 60;
                     e.bytes[2] = f == 0 ? 100 : 0;
                     out.push_back(e);
                 }
             }});
        REQUIRE(app.startEngine(backend,
                                {.sampleRate = 48000, .blockSize = 256, .midiInputs = true},
                                {.anticipative = false, .workers = 0}));
        call("transport.record", "{}");
        backend.run(4);
        call("transport.stop", "{}");
        app.stopEngine();
        REQUIRE(app.head().midi.size() == 1);
        CHECK(fs::is_symlink(fs::symlink_status(link)));
        // The link takes the name: the take's journal is the next one.
        CHECK(fs::is_regular_file(fs::symlink_status(app.recordDir() / "Keys-2.midi.jsonl")));
        REQUIRE(app.save(saved));
        CHECK_FALSE(fs::exists(app.recordDir() / "Keys-2.midi.jsonl"));
        CHECK(fs::is_symlink(fs::symlink_status(link)));
        CHECK_FALSE(fs::exists(dir / "absent-target"));
        CHECK(app.recoverable().empty());  // the take's own journal went at the save
    }
    fs::remove_all(dir);
}

TEST_CASE("audio crash recovery trusts journal runs and progress instead of flattening the WAV") {
    const fs::path root =
        fs::temp_directory_path() / ("supple-audio-placement-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    auto created = EngineApp::create({.workdir = root});
    REQUIRE(created);
    auto& app = **created;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    REQUIRE(dispatcher.execute({"track.add",
                                *json::parse(R"({"name":"Audio"})"),
                                {cmd::Origin::Type::Test, "audio", ""}}));
    REQUIRE(app.save(root / "song.json"));
    const auto before = readTextFile(root / "song.json");
    REQUIRE(before);
    const fs::path wav = app.recordDir() / "take.wav";
    fs::create_directories(wav.parent_path());
    audio::WavStreamWriter writer;
    REQUIRE(writer.open(wav, 48000, 1));
    std::array<float, 320> samples{};
    REQUIRE(writer.append(samples.data(), samples.size()));
    REQUIRE(writer.close());
    REQUIRE(detail::writeSidecar(wav, {model::Id{model::Kind::Track, 1}, 1000, 48000, {}, 0, 0}));
    fs::path journal = wav;
    journal.replace_extension(".audio.jsonl");
    REQUIRE(writeFileAtomic(journal,
                            "{\"schema\":\"supple.audio-recording/"
                            "1.0\",\"track\":\"tr_1\",\"sample_rate\":48000,\"start_sample\":1000,"
                            "\"latency\":0,\"channels\":1,\"loop\":[1000,1064]}\n"
                            "{\"run\":[64,1000]}\n{\"run\":[128,1000]}\n{\"run\":[192,1000]}\n"
                            "{\"captured\":256,\"frames\":256}\n{\"captured\":320,\"frames\":"));
    auto list = app.recoverable();
    REQUIRE(list.size() == 1);
    CHECK(list[0].seconds == doctest::Approx(256. / 48000));
    auto recovered = app.recover({}, std::nullopt);
    REQUIRE(recovered);
    REQUIRE(recovered->size() == 1);
    CHECK(recovered->front().takes.size() == 4);
    CHECK(audio::inspectWavStream(wav)->fileFrames == 320);
    REQUIRE(app.save(root / "song.json"));
    CHECK_FALSE(fs::exists(journal));
    // A project saved before the take can still recover it after retirement.
    REQUIRE(writeFileAtomic(root / "before.json", *before));
    auto reopened = EngineApp::create({.workdir = root});
    REQUIRE(reopened);
    REQUIRE((*reopened)->open(root / "before.json"));
    list = (*reopened)->recoverable();
    REQUIRE(list.size() == 1);
    CHECK(list[0].seconds == doctest::Approx(256. / 48000));
    auto again = (*reopened)->recover({}, std::nullopt);
    REQUIRE(again);
    REQUIRE(again->size() == 1);
    CHECK(again->front().takes.size() == 4);
}

TEST_CASE("audio recording publishes an append-only placement journal on its writer") {
    const fs::path root =
        fs::temp_directory_path() / ("supple-audio-journal-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    RecordTarget target{model::Id{model::Kind::Track, 1}, {0}, root / "take.wav", {}};
    detail::Recorder recorder({target}, 48000, 8);
    recorder.setLoop({1000, 1064, true});
    REQUIRE(recorder.open());
    std::array<float, 64> samples{};
    const float* in[]{samples.data()};
    for (int i = 0; i != 4; ++i) {
        recorder.capture(in, 1, 64, 1000);
        RecordingErrorTestAccess::passes(recorder);
    }
    const auto takes = recorder.finish({1000, 1064, true});
    REQUIRE(takes.size() == 1);
    const auto journal = root / "take.audio.jsonl";
    auto text = readTextFile(journal);
    REQUIRE(text);
    CHECK(text->find("supple.audio-recording/1.0") != std::string::npos);
    CHECK(text->find("\"run\":[64,1000]") != std::string::npos);
    CHECK(text->find("\"frames\":256") != std::string::npos);
}

TEST_CASE("audio journal placement matches stop for loops seeks and rendered clocks") {
    const fs::path root = fs::temp_directory_path() /
                          ("supple-audio-journal-clocks-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    const bool looping = GENERATE(false, true);
    const int mode = GENERATE(0, 1, 2, 3);
    RecordTarget target{model::Id{model::Kind::Track, 1}, {0}, root / "take.wav", {}};
    target.output = mode == 2;
    target.rackInput = mode == 3;
    target.outputShift = mode == 2 ? -8 : 24;
    detail::Recorder recorder({target}, 48000, 8);
    recorder.setLoop(looping ? LoopRange{1000, 1064, true} : LoopRange{});
    REQUIRE(recorder.open());
    std::array<float, 64> samples{};
    const float* input[]{samples.data()};
    detail::ProcessedInput processed{target.track, {samples.data(), samples.data()}, 16};
    for (int i = 0; i != 4; ++i) {
        const auto pos = looping ? 1000 : (i < 2 ? 1000 + 64 * i : 2000 + 64 * (i - 2));
        recorder.capture(
            input, 1, 64, pos,
            mode ? std::span(&processed, 1) : std::span<const detail::ProcessedInput>{});
        RecordingErrorTestAccess::passes(recorder);
    }
    const auto prefix = detail::readAudioJournal(target.file, root);
    REQUIRE(prefix);
    REQUIRE(prefix->frames);
    CHECK(*prefix->frames > 0);
    const auto takes = recorder.finish(looping ? LoopRange{1000, 1064, true} : LoopRange{});
    REQUIRE(takes.size() == 1);
    auto after = detail::readAudioJournal(target.file, root);
    REQUIRE(after);
    CHECK(*after->frames == takes[0].frames);
    CHECK(after->startSample == takes[0].startSample);
    CHECK(after->loopStart == takes[0].loop.start);
    CHECK(after->loopEnd == takes[0].loop.end);
    REQUIRE(after->segments.size() == takes[0].segments.size());
    for (std::size_t i = 0; i < after->segments.size(); ++i) {
        CHECK(after->segments[i].fileFrame == takes[0].segments[i].fileFrame);
        CHECK(after->segments[i].timeline == takes[0].segments[i].timeline);
    }
    CHECK(takes[0].journal == detail::audioJournalPath(target.file));
    const auto side = detail::readSidecar(target.file, root);
    REQUIRE(side);
    CHECK(side->frames == after->frames);
}

TEST_CASE("an audio journal creation failure wakes the model and isolates the failed target") {
    const fs::path root = fs::temp_directory_path() /
                          ("supple-audio-journal-error-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    fs::create_directories(root);
    const auto bad = root / "failed.wav";
    const auto occupied = detail::audioJournalPath(bad);
    REQUIRE(writeFileAtomic(occupied, "another recording's metadata"));
    MainThreadWake wake;
    detail::Recorder recorder({{model::Id{model::Kind::Track, 1}, {0}, bad, {}},
                               {model::Id{model::Kind::Track, 2}, {0}, root / "healthy.wav", {}}},
                              48000, 0);
    recorder.setMainThreadWake(&wake);
    REQUIRE(recorder.open());
    const auto errors = recorder.takeErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].error.code == "record.journal");
    WaitItem ready{wake.waitable(), kWaitRead};
    REQUIRE(waitFor(std::span(&ready, 1), -1) == 1);
    CHECK(wake.drain());
    std::array<float, 64> samples{};
    const float* input[]{samples.data()};
    recorder.capture(input, 1, 64, 1000);
    RecordingErrorTestAccess::passes(recorder);
    const auto takes = recorder.finish();
    CHECK(takes[0].frames == 0);
    CHECK(takes[0].journal.empty());
    CHECK(takes[1].frames == 64);
    CHECK(takes[1].error.empty());
    CHECK(*readTextFile(occupied) == "another recording's metadata");
}

TEST_CASE("audio placement journals retire only owned durable save and autosave copies") {
    const int mode = GENERATE(0, 1, 2, 3);
    const bool anticipative = GENERATE(false, true);
    const fs::path root = fs::temp_directory_path() /
                          ("supple-audio-journal-save-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    auto created = EngineApp::create(
        {.workdir = root, .scratchDir = mode == 1 ? root / "scratch" : fs::path{}});
    REQUIRE(created);
    auto& app = **created;
    app.setFixedClock(1'000'000);
    auto txn = app.beginTransaction({cmd::Origin::Type::Test, "audio", ""}, "Audio target");
    REQUIRE(txn);
    model::Track track;
    track.name = "Audio";
    track.parent = model::kMaster;
    track.record.armed = true;
    const auto id = app.txn().create(track);
    model::Route route;
    route.from.hwChannels = std::vector<int>{1};
    route.to.track = id;
    const auto input = app.txn().create(route);
    app.txn().modify<model::Track>(id, [&](auto& value) { value.record.input = input; });
    app.txn().modify<model::Track>(model::kMaster,
                                   [&](auto& value) { value.children.push_back(id); });
    REQUIRE(app.commitTransaction(*txn));
    const auto first = root / "old" / "song.json";
    if (mode >= 2) REQUIRE(app.save(first));
    REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 1},
                            {.anticipative = anticipative, .workers = anticipative ? 2 : 0}));
    REQUIRE(app.transport("record"));
    if (anticipative)
        REQUIRE_MESSAGE(app.engine()->awaitRendered(192), "renderer watchdog aborted the test");
    backend.run(3);
    REQUIRE(app.transport("stop"));
    app.stopEngine();
    const auto journal = app.recordDir() / "Audio-1.audio.jsonl";
    REQUIRE(fs::exists(journal));
    const auto side = detail::readSidecar(app.recordDir() / "Audio-1.wav");
    REQUIRE(side);
    CHECK(side->frames == 192);
    if (mode == 3) {
        app.setFixedClock(1'060'000);
        app.tick();
        CHECK(fs::exists(app.autosaveDir() / "autosave-1.json"));
        CHECK_FALSE(fs::exists(journal));
    } else {
        const auto next = root / "new" / "song.json";
        REQUIRE(app.save(next));
        CHECK_FALSE(fs::exists(app.recordDir() / "Audio-1.audio.jsonl"));
        if (mode == 2)
            CHECK(fs::exists(journal));  // belongs to the older project
        else
            CHECK_FALSE(fs::exists(journal));
        REQUIRE(app.open(next));
        CHECK(app.head().media.size() == 1);
        CHECK(app.recoverable().empty());
        if (mode == 0) {
            REQUIRE(
                app.startEngine(backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 1},
                                {.anticipative = anticipative, .workers = anticipative ? 2 : 0}));
            REQUIRE(app.transport("record"));
            const auto emptyJournal = app.recordDir() / "Audio-1.audio.jsonl";
            REQUIRE(app.transport("stop"));
            app.stopEngine();
            REQUIRE(fs::exists(emptyJournal));
            REQUIRE(app.save(next));
            CHECK_FALSE(fs::exists(emptyJournal));
        }
        if (mode == 2) {
            REQUIRE(app.open(first));
            CHECK(app.head().media.empty());
            CHECK(app.recoverable().size() == 1);
        }
    }
}

TEST_CASE("audio journal validation bounds clocks and stops at a malformed progress record") {
    const fs::path root = fs::temp_directory_path() /
                          ("supple-audio-journal-bounds-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    const auto wav = root / "take.wav";
    const auto journal = detail::audioJournalPath(wav);
    const std::string header =
        R"({"schema":"supple.audio-recording/1.0","track":"tr_1","sample_rate":48000,"start_sample":1000,"latency":8,"channels":2,"loop":[0,0]})"
        "\n";
    const std::string prefix = header + "{\"captured\":64,\"frames\":64}\n";
    for (const std::string bad :
         {"{\"captured\":128}\n", "{\"captured\":128,\"frames\":281474976710657}\n",
          "{\"captured\":32,\"frames\":128}\n", "[10,144,60,100]\n"}) {
        REQUIRE(writeFileAtomic(journal, prefix + bad + "{\"captured\":256,\"frames\":256}\n"));
        bool torn = false;
        const auto read = detail::readAudioJournal(wav, root, &torn);
        REQUIRE(read);
        CHECK(torn);
        CHECK(read->frames == 64);
    }
    auto invalid = header;
    invalid.replace(invalid.find("\"channels\":2"), 12, "\"channels\":0");
    REQUIRE(writeFileAtomic(journal, invalid));
    const auto read = detail::readAudioJournal(wav, root);
    REQUIRE_FALSE(read);
    CHECK(read.error().code == "record.journal");
    CHECK(read.error().path == "/0/channels");
}
