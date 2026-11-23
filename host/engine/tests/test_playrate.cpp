// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Portable master-rate tests: dummy devices and in-process test fixtures only.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>

#include "../../../tests/file_platform.h"
#include "stalled_backend.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
struct PlayrateHost {
    fs::path root = supple::test::uniqueTempDir("supple-playrate-");
    DummyBackend backend{
        DummyBackend::Options{.mode = DummyBackend::Mode::Manual, .captureFrames = 131072}};
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    PlayrateHost() {
        fs::create_directories(root / "plugins");
        AppOptions options;
        options.workdir = root;
        options.pluginPath = {root / "plugins"};
        auto created = EngineApp::create(options);
        REQUIRE_MESSAGE(created, (created ? "" : created.error().toString()));
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~PlayrateHost() {
        bus.reset();
        app.reset();  // stop the engine/writer before destroying backend and files
        fs::remove_all(root);
    }
    Result<json::Value> execute(const char* command, std::string_view args = "{}",
                                std::string gesture = {}) {
        auto parsed = json::parse(args);
        REQUIRE(parsed);
        return bus->execute({command, *parsed, {cmd::Origin::Type::Test, "playrate", {}, gesture}});
    }
    json::Value run(const char* command, std::string_view args = "{}", std::string gesture = {}) {
        auto result = execute(command, args, std::move(gesture));
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    void start(bool anticipative = false) {
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1},
                                 {.anticipative = anticipative, .workers = 0}));
    }
    void arm(bool midi = false) {
        run("track.add", R"({"name":"Input"})");
        run("track.set", midi ? R"({"track":"tr_1","armed":true,"record_midi":"*"})"
                              : R"({"track":"tr_1","armed":true,"record_input":[1]})");
    }
    std::vector<fs::path> files() const {
        std::vector<fs::path> out;
        for (const auto& entry : fs::recursive_directory_iterator(root))
            if (entry.is_regular_file()) out.push_back(entry.path());
        std::sort(out.begin(), out.end());
        return out;
    }
};

void recordError(const Result<json::Value>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "record.playrate");
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().path.empty());
    CHECK_FALSE(result.error().hint.empty());
}
}  // namespace

TEST_CASE("master playrate commands: schemas, queries, gesture undo and save/reopen") {
    PlayrateHost h;
    auto info = h.run("transport.playrate.get");
    CHECK(info["requested_rate"].asDouble() == 1);
    CHECK(info["effective_rate"].asDouble() == 1);
    CHECK_FALSE(info["preserve_pitch"].asBool());
    CHECK(info.find("envelope") == nullptr);
    const auto initial = h.app->head().version;
    h.run("transport.playrate.set");
    CHECK(h.app->head().version == initial);
    h.run("transport.playrate.set", R"({"rate":0.25})", "rate-drag");
    h.run("transport.playrate.set", R"({"rate":4,"preserve_pitch":true})", "rate-drag");
    CHECK(h.app->versions().history().size() == 2);
    info = h.run("transport.playrate.get");
    CHECK(info["requested_rate"].asDouble() == 4);
    CHECK(info["effective_rate"].asDouble() == 1);  // no engine is running
    CHECK(info["preserve_pitch"].asBool());
    const auto content = model::serializeProject(h.app->head());
    for (const char* args :
         {R"({"rate":0})", R"({"rate":4.001})", R"({"rate":true})", R"({"preserve_pitch":1})"}) {
        CHECK_FALSE(h.execute("transport.playrate.set", args));
        CHECK(model::serializeProject(h.app->head()) == content);
    }
    h.run("project.save", R"({"path":"rate.json"})");
    h.run("transport.playrate.set", R"({"rate":1,"preserve_pitch":false})");
    h.run("project.open", R"({"path":"rate.json"})");
    CHECK(h.app->head().project->playrate == model::Playrate{4, true});
    CHECK(model::serializeProject(h.app->head()) == content);
}

TEST_CASE("master playrate commands: tempo bake is atomic and retains tempo point IDs") {
    PlayrateHost h;
    h.run("tempo.set", R"({"points":[{"beat":0,"bpm":120,"ramp":true},
        {"beat":8,"bpm":180},{"beat":16,"bpm":240}]})");
    h.run("transport.playrate.set", R"({"rate":1.5,"preserve_pitch":true})");
    const auto before = h.app->head();
    const auto steps = h.app->versions().history().size();
    h.run("transport.playrate.apply-to-tempo");
    CHECK(h.app->versions().history().size() == steps + 1);
    CHECK(h.app->head().project->playrate == model::Playrate{1, true});
    CHECK(h.app->head().meters.begin()->id == before.meters.begin()->id);
    for (const auto& point : before.tempo) {
        const auto* baked = h.app->head().tempo.find(point.id);
        REQUIRE(baked);
        CHECK(baked->bpm == point.value->bpm * 1.5);
        CHECK(baked->beat == point.value->beat);
        CHECK(baked->ramp == point.value->ramp);
    }
    REQUIRE(h.app->undo());
    CHECK(model::serializeProject(h.app->head()) == model::serializeProject(before));
    h.run("tempo.set", R"({"points":[{"beat":0,"bpm":900}]})");
    const auto bytes = model::serializeProject(h.app->head());
    auto rejected = h.execute("transport.playrate.apply-to-tempo");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "tempo.range");
    CHECK(rejected.error().path.find("/args/tempo/points/") == 0);
    CHECK_FALSE(rejected.error().hint.empty());
    CHECK(model::serializeProject(h.app->head()) == bytes);
}

TEST_CASE("master playrate envelope commands: target, bounds, items and bake guard") {
    PlayrateHost h;
    h.run("track.add");
    CHECK_FALSE(h.execute("env.add", R"({"track":"tr_1","param":"playrate"})"));
    for (const char* value : {"0", "4.01", "true"}) {
        const auto args =
            std::string(R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":)") +
            value + "}]}";
        CHECK_FALSE(h.execute("env.add", args));
    }
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
    CHECK(h.run("transport.playrate.get")["envelope"].asString() == "en_1");
    CHECK_FALSE(h.execute("env.add", R"({"track":"tr_0","param":"playrate"})"));
    const auto version = h.app->head().version;
    auto refused = h.execute("transport.playrate.apply-to-tempo");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "playrate.envelope");
    CHECK(h.app->head().version == version);
    CHECK_FALSE(h.execute("autoitem.add", R"({"env":"en_1","position":0,"length":4,
        "baseline":1,"amplitude":2,"points":[{"beat":0,"value":3}]})"));
    h.run("env.set", R"({"env":"en_1","active":false})");
    h.run("transport.playrate.apply-to-tempo");
    CHECK(h.run("transport.playrate.get").find("envelope") == nullptr);
    h.run("env.points.set", R"({"env":"en_1","points":[]})");
    h.run("env.set", R"({"env":"en_1","active":true})");
    h.run("transport.playrate.apply-to-tempo");  // empty envelope does not drive the rate
    h.run("autoitem.add", R"({"env":"en_1","position":0,"length":4,
        "points":[{"beat":0,"value":1},{"beat":4,"value":2}]})");
    CHECK(h.run("transport.playrate.get")["envelope"].asString() == "en_1");
    CHECK_FALSE(h.execute("transport.playrate.apply-to-tempo"));
}

TEST_CASE("master playrate recording guard: no files opened for audio or MIDI") {
    for (bool midi : {false, true}) {
        PlayrateHost h;
        h.arm(midi);
        h.run("transport.playrate.set", R"({"rate":2})");
        h.start();
        const auto files = h.files();
        const auto version = h.app->head().version;
        recordError(h.execute("transport.record"));
        CHECK(h.files() == files);
        CHECK(h.app->head().version == version);
        RecordTarget target;
        target.track = model::Id{model::Kind::Track, 1};
        target.inputs = {0};
        target.file = h.root / "must-not-exist.wav";
        auto direct = h.app->engine()->startRecording({target});
        REQUIRE_FALSE(direct);
        CHECK(direct.error().code == "record.playrate");
        CHECK_FALSE(fs::exists(target.file));
        h.run("transport.playrate.set", R"({"rate":1})");
        h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[
            {"beat":0,"value":1},{"beat":100,"value":2}]})");
        recordError(h.execute("transport.record"));
        CHECK(h.files() == files);
        h.run("env.set", R"({"env":"en_1","active":false})");
        h.run("transport.record");
        CHECK(h.app->engine()->recording());
        h.run("transport.stop");
    }
}

TEST_CASE("master playrate recording guard: edits and engine loads roll back atomically") {
    PlayrateHost h;
    h.arm();
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
    h.run("env.set", R"({"env":"en_1","mode":"latch"})");
    h.start();
    h.run("transport.record");
    h.backend.run(4);
    const auto before = model::serializeProject(h.app->head());
    recordError(h.execute("transport.playrate.set", R"({"rate":2})"));
    recordError(h.execute("env.points.set", R"({"env":"en_1","points":[{"beat":0,"value":2}]})"));
    recordError(h.execute("autoitem.add", R"({"env":"en_1","position":0,"length":4,
        "points":[{"beat":0,"value":2}]})"));
    CHECK(model::serializeProject(h.app->head()) == before);
    CHECK_FALSE(h.execute("batch", R"({"steps":[
        {"command":"project.set","args":{"title":"must roll back"}},
        {"command":"transport.playrate.set","args":{"rate":2}}]})"));
    CHECK(model::serializeProject(h.app->head()) == before);
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    auto project = edit.project();
    project.playrate.rate = 2;
    edit.setProject(project);
    const auto disallowed = edit.commit().value();
    auto refused = h.app->engine()->load(disallowed);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "record.playrate");
    CHECK(h.app->engine()->recording());
    h.run("transport.stop");
    const auto* envelope = h.app->head().envelopes.find({model::Kind::Envelope, 1});
    REQUIRE(envelope);
    CHECK(std::all_of(envelope->points.begin(), envelope->points.end(), [](const auto& p) {
        return p.value == 1;
    }));  // rejected knob moves cannot become a later latch hold
}

TEST_CASE("master playrate recording guard: history and branches cannot bypass unity") {
    for (bool redo : {false, true}) {
        PlayrateHost h;
        h.arm();
        h.run("branch.create", R"({"name":"unsafe"})");
        h.run("transport.playrate.set", R"({"rate":2})");
        h.run("branch.switch", R"({"name":"main"})");
        h.run("transport.playrate.set", R"({"rate":2})");
        if (redo)
            REQUIRE(h.app->undo());
        else
            h.run("transport.playrate.set", R"({"rate":1})");
        h.start();
        h.run("transport.record");
        const auto before = model::serializeProject(h.app->head());
        const auto result = redo ? h.app->redo() : h.app->undo();
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "record.playrate");
        CHECK(model::serializeProject(h.app->head()) == before);
        recordError(h.execute("branch.switch", R"({"name":"unsafe"})"));
        auto merged = h.app->mergeBranch(h.registry, "unsafe", "main");
        CHECK_FALSE(merged.conflicts.empty());
        CHECK(h.app->versions().branch() == "main");
        CHECK(model::serializeProject(h.app->head()) == before);
        CHECK(h.app->engine()->recording());
        h.run("transport.stop");
    }
}

TEST_CASE("master playrate writing: touch latch and write use strip capture timestamps") {
    for (const char* mode : {"touch", "latch", "write"}) {
        PlayrateHost h;
        h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
        const auto args = std::string(R"({"env":"en_1","mode":")") + mode + "\"}";
        h.run("env.set", args);
        h.start();
        h.run("transport.play");
        h.backend.run(8);
        h.run("transport.playrate.set", R"({"rate":1.25})", "write-rate");
        h.backend.run(8);
        h.run("transport.pause");
        const auto* envelope = h.app->head().envelopes.find({model::Kind::Envelope, 1});
        REQUIRE(envelope);
        CHECK(std::any_of(envelope->points.begin(), envelope->points.end(),
                          [](const auto& p) { return p.pos > 0 && p.value == 1.25; }));
        CHECK(h.run("transport.playrate.get")["effective_rate"].asDouble() == 1);
    }
}

TEST_CASE("master playrate envelope replaces scalar in audio and telemetry") {
    PlayrateHost h;
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(4800));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] = i % 64 < 32 ? .2f : -.2f;
    REQUIRE(audio::writeWav(h.root / "tone.wav", tone));
    h.run("track.add");
    auto args = json::Value::object();
    args["track"] = "tr_1";
    args["audio_file"] = (h.root / "tone.wav").string();
    h.run("item.add", json::writeCompact(args));
    OfflineSettings settings;
    settings.endSeconds = .05;
    settings.threads = 1;
    const auto unity = h.app->renderMix(settings);
    REQUIRE(unity);
    h.run("transport.playrate.set", R"({"rate":2,"preserve_pitch":true})");
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":0.25}]})");
    const auto pending = h.app->renderMix(settings);
    REQUIRE(pending);
    CHECK(pending->audio.frames() == unity->audio.frames() * 4);
    h.start();
    h.run("transport.play");
    h.backend.run(20);
    CHECK(h.app->engine()->transport().sample == 640);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto& captured = h.backend.capture().channels[channel];
        const auto& expected = pending->audio.channels[channel];
        const auto count =
            std::min(static_cast<std::size_t>(h.backend.cycles() * 128), expected.size());
        REQUIRE(count > 0);
        double maxDiff = 0;
        for (std::size_t i = 0; i < count; ++i)
            maxDiff = std::max(maxDiff, std::abs(double(captured[i]) - double(expected[i])));
        CHECK(maxDiff == 0);
    }
    CHECK(h.run("transport.playrate.get")["effective_rate"].asDouble() == .25);
    auto refused = h.execute("transport.playrate.apply-to-tempo");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "playrate.playing");
    h.run("transport.stop");
}

TEST_CASE("master playrate recording guard distinguishes fast forward from safe replay") {
    for (bool safeReplay : {false, true}) {
        PlayrateHost h;
        h.arm();
        if (safeReplay) h.run("transport.playrate.set", R"({"rate":2})");
        h.run("branch.create", R"({"name":"source"})");
        if (safeReplay)
            h.run("project.set", R"({"title":"safe replay"})");
        else
            h.run("transport.playrate.set", R"({"rate":2})");
        h.run("branch.switch", R"({"name":"main"})");
        if (safeReplay) h.run("transport.playrate.set", R"({"rate":1})");
        h.start();
        h.run("transport.record");
        const auto before = model::serializeProject(h.app->head());
        const auto merged = h.app->mergeBranch(h.registry, "source", "main");
        if (safeReplay) {
            CHECK(merged.conflicts.empty());
            CHECK(h.app->head().project->title == "safe replay");
            CHECK(h.app->head().project->playrate.rate == 1);
        } else {
            CHECK_FALSE(merged.conflicts.empty());
            CHECK(model::serializeProject(h.app->head()) == before);
        }
        h.run("transport.stop");
    }
}

TEST_CASE("master playrate Lua bindings expose nested commands and tempo bake") {
    PlayrateHost h;
    h.app->attachCommands(*h.bus);
    auto scripted = h.run("script.run", R"json({"code":
        "q.transport.playrate.set{rate=1.5}; local before=q.transport.playrate.get(); q.transport.playrate['apply-to-tempo'](); return {before=before,after=q.transport.playrate.get()}"
    })json");
    CHECK(scripted["result"]["before"]["requested_rate"].asDouble() == 1.5);
    CHECK(scripted["result"]["after"]["requested_rate"].asDouble() == 1);
    CHECK(h.app->head().timeMap->bpmAt(0) == 180);
}

TEST_CASE("master playrate UI accessor reads cached curves without commands or history") {
    PlayrateHost h;
    auto state = h.app->playrateState();
    CHECK(state.requestedRate == 1);
    CHECK(state.effectiveRate == 1);
    CHECK_FALSE(state.envelope);
    CHECK_FALSE(state.envelopeValue);
    h.run("transport.playrate.set", R"({"rate":1.137,"preserve_pitch":true})");
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[
        {"beat":0,"value":0.5},{"beat":1,"value":2.5}]})");
    const auto version = h.app->head().version;
    const auto steps = h.app->versions().history().size();
    for (int i = 0; i < 1000; ++i) {
        state = h.app->playrateState();
        CHECK(state.requestedRate == 1.137);
        CHECK(state.preservePitch);
        CHECK(state.envelope == model::Id{model::Kind::Envelope, 1});
        CHECK(state.envelopeValue == .5);
    }
    CHECK(h.app->head().version == version);
    CHECK(h.app->versions().history().size() == steps);
    h.run("env.set", R"({"env":"en_1","active":false})");
    CHECK_FALSE(h.app->playrateState().envelopeValue);
    REQUIRE(h.app->undo());
    CHECK(h.app->playrateState().envelopeValue == .5);
}

TEST_CASE("master playrate writing: a failed batch cannot leave a later latch hold") {
    PlayrateHost h;
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
    h.run("env.set", R"({"env":"en_1","mode":"latch"})");
    h.start();
    h.run("transport.play");
    h.backend.run(8);
    CHECK_FALSE(h.execute("batch", R"({"steps":[
        {"command":"transport.playrate.set","args":{"rate":1.25}},
        {"command":"track.set","args":{"track":"tr_999","mute":true}}]})"));
    CHECK(h.app->head().project->playrate.rate == 1);
    h.backend.run(8);
    h.run("transport.pause");
    const auto* envelope = h.app->head().envelopes.find({model::Kind::Envelope, 1});
    REQUIRE(envelope);
    CHECK(std::all_of(envelope->points.begin(), envelope->points.end(),
                      [](const auto& p) { return p.value == 1; }));
}

TEST_CASE("master playrate audio: scalar conversion is exact offline and in both executors") {
    for (bool anticipative : {false, true})
        for (bool live : {false, true})
            for (bool preserve : {false, true})
                for (double rate : {.25, 1.137, 4.}) {
                    CAPTURE(anticipative);
                    CAPTURE(live);
                    CAPTURE(preserve);
                    CAPTURE(rate);
                    PlayrateHost h;
                    audio::AudioData tone;
                    tone.sampleRate = 48000;
                    tone.channels.assign(2, std::vector<float>(48000));
                    for (std::size_t i = 0; i < tone.frames(); ++i)
                        tone.channels[0][i] = tone.channels[1][i] =
                            static_cast<float>(.2 * std::sin(double(i) * .03));
                    REQUIRE(audio::writeWav(h.root / "rate-tone.wav", tone));
                    h.run("track.add");
                    auto item = json::Value::object();
                    item["track"] = "tr_1";
                    item["audio_file"] = (h.root / "rate-tone.wav").string();
                    h.run("item.add", json::writeCompact(item));
                    if (live)
                        h.run("track.set",
                              R"({"track":"tr_1","armed":true,"monitor":"on","record_input":[1]})");
                    auto args = json::Value::object();
                    args["rate"] = rate;
                    args["preserve_pitch"] = preserve;
                    h.run("transport.playrate.set", json::writeCompact(args));
                    OfflineSettings settings;
                    settings.endSeconds = .03;
                    settings.blockSize = 127;
                    settings.threads = 1;
                    const auto offline = h.app->renderMix(settings);
                    REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
                    settings.blockSize = 512;
                    const auto other = h.app->renderMix(settings);
                    REQUIRE(other);
                    CHECK(other->audio.channels == offline->audio.channels);
                    CHECK(std::abs(double(offline->audio.frames()) - 1440. / rate) < 2);
                    h.start(anticipative);
                    h.run("transport.play");
                    if (anticipative)
                        REQUIRE(h.app->engine()->awaitRendered(
                            static_cast<std::int64_t>(offline->audio.frames())));
                    const auto cycles =
                        static_cast<std::int64_t>((offline->audio.frames() + 127) / 128);
                    h.backend.run(cycles);
                    for (std::size_t c = 0; c < 2; ++c) {
                        const auto& actual = h.backend.capture().channels[c];
                        REQUIRE(actual.size() >= offline->audio.frames());
                        double maxDiff = 0;
                        for (std::size_t i = 0; i < offline->audio.frames(); ++i)
                            maxDiff =
                                std::max(maxDiff, std::abs(double(actual[i]) -
                                                           double(offline->audio.channels[c][i])));
                        CHECK(maxDiff == 0);
                    }
                    CHECK(h.app->engine()->effectivePlayrate() == rate);
                    CHECK(h.app->playrateState().effectiveRate == rate);
                    CHECK(h.app->engine()->transport().seconds ==
                          doctest::Approx(double(cycles * 128) * rate / 48000));
                    h.run("transport.stop");
                }
}

namespace {
InstancePool playrateFixturePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const fs::path fixture = FIXTURE_CLAP_PATH;
        host::ScanOptions options;
        options.forceInProcess = true;
        auto list = host::scan(std::span(&fixture, 1), options);
        if (!list) return list.error();
        for (const auto& plugin : *list)
            if (plugin.id == ref.id) return plugin;
        return Error("engine.plugin_missing", "playrate fixture is missing", {},
                     "rebuild the fixture target");
    });
}
}  // namespace

TEST_CASE("master playrate transitions: compensated take sources remain aligned") {
    for (bool looped : {false, true})
        for (bool anticipative : {false, true})
            for (bool preserve : {false, true}) {
                CAPTURE(looped);
                CAPTURE(preserve);
                CAPTURE(anticipative);
                PlayrateHost h;
                audio::AudioData tone;
                tone.sampleRate = 48000;
                tone.channels.assign(2, std::vector<float>(48000));
                for (std::size_t i = 0; i < tone.frames(); ++i)
                    tone.channels[0][i] = tone.channels[1][i] =
                        static_cast<float>(.2 * std::sin(double(i) * .03));
                REQUIRE(audio::writeWav(h.root / "alignment.wav", tone));
                h.run("track.add");
                h.run("track.add");
                auto item = json::Value::object();
                item["audio_file"] = (h.root / "alignment.wav").string();
                for (const char* track : {"tr_1", "tr_2"}) {
                    item["track"] = track;
                    h.run("item.add", json::writeCompact(item));
                }
                model::EditTxn edit(h.app->head(), h.app->head().version + 1);
                auto project = edit.project();
                project.console9 = false;
                project.playrate = {looped ? 2. : 1.137, preserve};
                edit.setProject(project);
                model::FxInstance fx;
                fx.plugin.id = "com.suppleaudio.test.gain";
                fx.patch =
                    json::parse(
                        R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                        .value();
                const auto id = edit.create(fx);
                for (const auto& entry : h.app->head().items)
                    if (entry.value->track.n == 2)
                        edit.modify<model::Take>(*entry.value->activeTake, [&](auto& take) {
                            take.chain.push_back(id);
                            take.phaseInvert = true;
                        });
                const auto session = edit.commit().value();
                auto pool = playrateFixturePool();
                MediaCache media;
                DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 140000});
                Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
                REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
                REQUIRE(engine.load(session));
                if (looped) engine.setLoop(0, 512. / 48000., true);
                engine.play();
                auto run = [&](int cycles) {
                    for (int i = 0; i < cycles; ++i) {
                        if (anticipative)
                            REQUIRE(engine.awaitRendered((backend.cycles() + 1) * 128));
                        backend.run(1);
                        engine.service();
                    }
                };
                run(64);
                double initial = 0;
                for (std::size_t i = 512; i < static_cast<std::size_t>(backend.cycles() * 128); ++i)
                    initial = std::max(initial, std::abs(double(backend.capture().channels[0][i])));
                REQUIRE(initial == 0);
                model::EditTxn change(session, session.version + 1);
                project.playrate.rate = 2;
                project.playrate.preservePitch = !preserve;
                change.setProject(project);
                const auto changed = change.commit().value();
                REQUIRE(engine.load(changed));
                model::EditTxn newer(changed, changed.version + 1);
                project.playrate.rate = 2.5;
                newer.setProject(project);
                REQUIRE(engine.load(newer.commit().value()));
                run(1000);  // lookahead, then the knob's ramp and smoothing settle
                CHECK(engine.effectivePlayrate() == 2.5);
                double after = 0;
                for (std::size_t i = 8192; i < static_cast<std::size_t>(backend.cycles() * 128);
                     ++i)
                    after = std::max(after, std::abs(double(backend.capture().channels[0][i])));
                CHECK(after == 0);
                engine.stop();
            }
}

namespace {
model::Session rateMidiSession(bool split, bool preserve) {
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    auto project = edit.project();
    project.playrate = {1.137, preserve};
    edit.setProject(project);
    model::Track track;
    track.parent = model::kMaster;
    const auto tr = edit.create(track);
    edit.modify<model::Track>(model::kMaster, [&](auto& master) { master.children.push_back(tr); });
    model::FxInstance synth;
    synth.plugin.id = "com.suppleaudio.test.sine";
    const auto instrument = edit.create(synth);
    model::FxInstance delay;
    delay.plugin.id = "com.suppleaudio.test.gain";
    delay.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value();
    const auto fx = edit.create(delay);
    model::MidiSource source;
    source.sequence.ppq = 960;
    source.sequence.length = 960;
    source.sequence.notes = {{4, 14, 0, 69, 100, 0}, {15, 20, 0, 72, 100, 0}};
    model::Take take;
    take.source = edit.create(source);
    take.chain = {instrument, fx};
    const auto tk = edit.create(take);
    model::Item item;
    item.track = tr;
    item.length = 1;
    item.takes = {tk};
    item.activeTake = tk;
    edit.create(item);
    model::Envelope env;
    env.target = {model::kMaster, "playrate"};
    env.timebase = model::Timebase::Time;
    env.points = {
        {0, .5}, {.01, 2}, {.02, 4, model::PointShape::Square}, {.025, .25}, {.035, 1.137}};
    edit.create(env);
    if (split) {
        model::Track live;
        live.parent = model::kMaster;
        const auto id = edit.create(live);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(id); });
        model::Route route;
        route.from.hwChannels = std::vector<int>{1};
        route.to.track = id;
        const auto input = edit.create(route);
        edit.modify<model::Track>(id, [&](auto& t) {
            t.record.armed = true;
            t.record.monitor = model::Monitor::On;
            t.record.input = input;
        });
    }
    auto session = edit.commit();
    REQUIRE_MESSAGE(session, (session ? "" : session.error().toString()));
    return *session;
}
}  // namespace

TEST_CASE(
    "master playrate MIDI: authored curves, compensated takes and split executors match offline") {
    for (bool split : {false, true}) {
        const auto session = rateMidiSession(split, false);
        auto offlinePool = playrateFixturePool();
        OfflineSettings settings;
        settings.endSeconds = .05;
        settings.blockSize = 127;
        settings.threads = 1;
        const auto reference = renderOffline(session, offlinePool, settings);
        REQUIRE_MESSAGE(reference, (reference ? "" : reference.error().toString()));
        REQUIRE(reference->outputLatency == 0);  // take-chain latency is source lookahead
        settings.blockSize = 17;
        const auto partitioned = renderOffline(session, offlinePool, settings);
        REQUIRE(partitioned);
        CHECK(bool(reference->audio.channels == partitioned->audio.channels));
        double peak = 0;
        for (float sample : reference->audio.channels[0])
            peak = std::max(peak, std::abs(double(sample)));
        REQUIRE(peak > .01);
        for (bool anticipative : {false, true}) {
            CAPTURE(split);
            CAPTURE(anticipative);
            auto pool = playrateFixturePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 10000});
            Engine engine(pool, media, {.anticipative = anticipative, .workers = 1});
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 73, .inputs = 1}));
            REQUIRE(engine.load(session));
            engine.play();
            const auto count = static_cast<std::int64_t>((reference->audio.frames() + 72) / 73);
            if (anticipative) REQUIRE(engine.awaitRendered(count * 73));
            backend.run(count);
            double maxDiff = 0;
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t i = 0; i < reference->audio.frames(); ++i)
                    maxDiff = std::max(maxDiff, std::abs(double(reference->audio.channels[c][i]) -
                                                         double(backend.capture().channels[c][i])));
            CHECK(maxDiff == 0);
            CHECK(engine.processErrors() == 0);
            engine.stop();
        }
    }
}

TEST_CASE(
    "master playrate MIDI: compensated loop boundaries and device clicks agree in both executors") {
    for (bool split : {false, true}) {
        std::vector<std::vector<float>> expected;
        for (bool anticipative : {false, true}) {
            CAPTURE(split);
            CAPTURE(anticipative);
            auto pool = playrateFixturePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 16000});
            Engine engine(pool, media, {.anticipative = anticipative, .workers = 1});
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 73, .inputs = 1}));
            REQUIRE(engine.load(rateMidiSession(split, false)));
            engine.setLoop(0, 600. / 48000., true);
            engine.setMetronome(true, -12);
            engine.play();
            constexpr std::int64_t frames = 73 * 90;
            if (anticipative) REQUIRE(engine.awaitRendered(frames));
            backend.run(90);
            if (!anticipative) {
                expected.resize(2);
                for (std::size_t c = 0; c < 2; ++c)
                    expected[c].assign(backend.capture().channels[c].begin(),
                                       backend.capture().channels[c].begin() + frames);
            } else {
                double diff = 0;
                for (std::size_t c = 0; c < 2; ++c)
                    for (std::size_t i = 0; i < expected[c].size(); ++i)
                        diff = std::max(diff, std::abs(double(expected[c][i]) -
                                                       double(backend.capture().channels[c][i])));
                CHECK(diff == 0);
            }
            CHECK(engine.transport().sample < 600);
            CHECK(engine.processErrors() == 0);
            // Fresh seeks reseed source/graph state rather than retaining loop history.
            REQUIRE(engine.prepareSeek(0));
            if (anticipative) REQUIRE(engine.awaitRendered(frames));
            backend.run(90);
            double repeatDiff = 0;
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t i = 0; i < static_cast<std::size_t>(frames); ++i)
                    repeatDiff = std::max(
                        repeatDiff, std::abs(double(backend.capture().channels[c][i]) -
                                             double(backend.capture().channels[c][i + frames])));
            CHECK(repeatDiff == 0);
            engine.stop();
        }
    }
}

TEST_CASE("master playrate tempo: plugins receive effective BPM through the bridge") {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (auto isolation : {model::Isolation::InProcess, model::Isolation::SharedBridge}) {
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        model::Envelope envelope;
        envelope.target = {model::kMaster, "playrate"};
        envelope.timebase = model::Timebase::Time;
        envelope.points = {{0, .5, model::PointShape::Square}, {.01, 2}};
        edit.create(envelope);
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.edge";
        fx.state = "sha256:master-rate-tempo-probe";
        fx.isolation = isolation;
        fx.autoBypassOnSilence = false;
        edit.putBlob(*fx.state, std::make_shared<const model::Blob>(model::Blob{{48, 0, 0, 0}}));
        const auto id = edit.create(fx);
        edit.modify<model::Track>(model::kMaster, [&](auto& track) { track.chain.push_back(id); });
        const auto session = edit.commit().value();
        auto pool = playrateFixturePool();
        OfflineSettings settings;
        settings.endSeconds = .02;
        settings.blockSize = 17;
        settings.threads = 1;
        const auto small = renderOffline(session, pool, settings);
        REQUIRE_MESSAGE(small, (small ? "" : small.error().toString()));
        settings.blockSize = 512;
        const auto reference = renderOffline(session, pool, settings);
        REQUIRE(reference);
        CHECK(bool(reference->audio.channels == small->audio.channels));
        CHECK(reference->audio.channels[0][0] == .06f);
        CHECK(reference->audio.channels[0][960] == .24f);
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4096});
        Engine engine(pool, media, {.anticipative = true, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 73}));
        REQUIRE(engine.load(session));
        engine.play();
        const auto cycles = static_cast<std::int64_t>((reference->audio.frames() + 72) / 73);
        REQUIRE(engine.awaitRendered(cycles * 73));
        backend.run(cycles);
        double diff = 0;
        for (std::size_t i = 0; i < reference->audio.frames(); ++i)
            diff = std::max(diff, std::abs(double(reference->audio.channels[0][i]) -
                                           double(backend.capture().channels[0][i])));
        CHECK(diff == 0);
        engine.stop();
    }
}

TEST_CASE("master playrate monitoring: live input stays in device time across edits and seeks") {
    for (bool anticipative : {false, true}) {
        auto base = rateMidiSession(true, false);
        model::EditTxn edit(base, base.version + 1);
        // Only the armed input track remains audible; timeline instruments
        // still exercise the renderer share of a split graph.
        edit.modify<model::Track>({model::Kind::Track, 1}, [](auto& track) { track.mute = true; });
        auto project = edit.project();
        project.playrate = {1, false};
        edit.setProject(project);
        for (const auto& env : base.envelopes)
            edit.modify<model::Envelope>(env.id, [](auto& lane) { lane.active = false; });
        auto session = edit.commit().value();
        auto pool = playrateFixturePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .captureFrames = 20000,
                              .input = [](float* const* input, std::uint32_t, std::uint32_t frames,
                                          std::int64_t first) {
                                  for (std::uint32_t i = 0; i < frames; ++i)
                                      input[0][i] = (first + i) % 2 == 0 ? .125f : -.125f;
                              }});
        Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1}));
        REQUIRE(engine.load(session));
        engine.play();
        if (anticipative) REQUIRE(engine.awaitRendered(4096));
        backend.run(32);
        const float even = backend.capture().channels[0][2048];
        const float odd = backend.capture().channels[0][2049];
        REQUIRE(even != 0);
        model::EditTxn change(session, session.version + 1);
        project.playrate = {.25, true};
        change.setProject(project);
        session = change.commit().value();
        REQUIRE(engine.load(session));
        for (int i = 0; i < 80; ++i) {
            if (anticipative) REQUIRE(engine.awaitRendered((backend.cycles() + 1) * 128));
            backend.run(1);
            engine.service();
        }
        REQUIRE(engine.prepareSeek(.005));
        if (anticipative) REQUIRE(engine.awaitRendered(2048));
        backend.run(16);
        bool same = true;
        for (std::size_t i = 4096; i < static_cast<std::size_t>(backend.cycles() * 128); ++i)
            same &= backend.capture().channels[0][i] == (i % 2 == 0 ? even : odd);
        CHECK(same);
        engine.stop();
    }
}

TEST_CASE(
    "master playrate recording: a unity request does not clear an audible non-unity trajectory") {
    PlayrateHost h;
    h.arm();
    h.run("transport.playrate.set", R"({"rate":2})");
    h.start();
    h.run("transport.play");
    h.backend.run(8);
    REQUIRE(h.app->playrateState().effectiveRate == 2);
    h.run("transport.playrate.set", R"({"rate":1})");
    REQUIRE(h.app->head().project->playrate.rate == 1);
    const auto files = h.files();
    recordError(h.execute("transport.record"));
    CHECK(h.files() == files);
    h.run("transport.pause");
    h.run("transport.record");
    h.backend.run(8);
    h.run("transport.stop");
}

TEST_CASE("master playrate recording: returning to unity preserves loop pass coordinates") {
    for (bool anticipative : {false, true}) {
        PlayrateHost h;
        h.arm();
        auto pool = playrateFixturePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 127, .inputs = 1}));
        model::EditTxn rate(h.app->head(), h.app->head().version + 1);
        auto project = rate.project();
        project.playrate.rate = 2;
        rate.setProject(project);
        auto session = rate.commit().value();
        REQUIRE(engine.load(session));
        model::EditTxn unity(session, session.version + 1);
        project.playrate.rate = 1;
        unity.setProject(project);
        REQUIRE(engine.load(unity.commit().value()));
        engine.setLoop(0, .01, true);
        REQUIRE(engine.startRecording(
            {{{model::Kind::Track, 1}, {0}, h.root / "unity-loop.wav", std::nullopt}}));
        engine.play();
        for (int i = 0; i < 100; ++i) {
            if (anticipative) REQUIRE(engine.awaitRendered((i + 1) * 127));
            backend.run(1);
        }
        const auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const auto& take = takes->front();
        CHECK(take.frames == 12700);
        REQUIRE(take.segments.size() == 27);
        for (std::size_t i = 0; i < take.segments.size(); ++i) {
            CHECK(take.segments[i].timeline == 0);
            CHECK(take.segments[i].fileFrame == static_cast<std::int64_t>(i * 480));
        }
        engine.stop();
    }
}

TEST_CASE("master playrate seek: play retains a prepared stopped seek before the first callback") {
    PlayrateHost h;
    h.run("transport.playrate.set", R"({"rate":2})");
    h.start();
    h.run("transport.seek", R"({"seconds":0.125})");
    CHECK(h.app->engine()->transport().sample == 6000);
    h.run("transport.play");
    h.backend.run(1);
    CHECK(h.app->engine()->transport().sample == 6256);
    h.run("transport.stop");
}
TEST_CASE("master playrate range: a fresh seek matches a nonzero offline timeline range") {
    for (bool curve : {false, true})
        for (bool preserve : {false, true}) {
            CAPTURE(curve);
            CAPTURE(preserve);
            PlayrateHost h;
            audio::AudioData tone;
            tone.sampleRate = 48000;
            tone.channels.assign(2, std::vector<float>(48000));
            for (std::size_t i = 0; i < tone.frames(); ++i)
                tone.channels[0][i] = tone.channels[1][i] =
                    static_cast<float>(.2 * std::sin(double(i) * .03));
            REQUIRE(audio::writeWav(h.root / "range.wav", tone));
            h.run("track.add");
            auto item = json::Value::object();
            item["track"] = "tr_1";
            item["audio_file"] = (h.root / "range.wav").string();
            h.run("item.add", json::writeCompact(item));
            auto args = json::Value::object();
            args["rate"] = 1.137;
            args["preserve_pitch"] = preserve;
            h.run("transport.playrate.set", json::writeCompact(args));
            if (curve)
                h.run(
                    "env.add",
                    R"({"track":"tr_0","param":"playrate","points":[{"seconds":0,"value":0.5},{"seconds":0.02,"value":2}]})");
            if (curve)
                h.run(
                    "env.add",
                    R"({"track":"tr_1","param":"volume","points":[{"seconds":0,"value":-12},{"seconds":0.02,"value":0},{"seconds":0.03,"value":-24}]})");
            OfflineSettings settings;
            settings.startSeconds = .007;
            settings.endSeconds = .027;
            settings.threads = 1;
            settings.blockSize = 127;
            const auto reference = h.app->renderMix(settings);
            REQUIRE(reference);
            h.start();
            h.run("transport.seek", R"({"seconds":0.007})");
            h.run("transport.play");
            h.backend.run(static_cast<std::int64_t>((reference->audio.frames() + 127) / 128));
            double diff = 0;
            for (std::size_t i = 0; i < reference->audio.frames(); ++i)
                diff = std::max(diff, std::abs(double(reference->audio.channels[0][i]) -
                                               double(h.backend.capture().channels[0][i])));
            CHECK(diff == 0);
            h.run("transport.stop");
        }
}

TEST_CASE(
    "master playrate bounds: expanded output is rejected before files or source preparation") {
    PlayrateHost h;
    h.run("transport.playrate.set", R"({"rate":0.25})");
    const auto files = h.files();
    const auto command = h.execute("render.mix", R"({"name":"too-long","end_beat":3600})");
    REQUIRE_FALSE(command);
    CHECK(command.error().code == "render.budget");
    CHECK_FALSE(command.error().path.empty());
    CHECK_FALSE(command.error().hint.empty());
    CHECK(h.files() == files);
    auto pool = playrateFixturePool();
    OfflineSettings settings;
    settings.endSeconds = 1800;
    const auto direct = renderOffline(h.app->head(), pool, settings);
    REQUIRE_FALSE(direct);
    CHECK(direct.error().code == "engine.too_long");
    settings.startSeconds = std::numeric_limits<double>::infinity();
    settings.endSeconds = std::numeric_limits<double>::infinity();
    CHECK_FALSE(renderOffline(h.app->head(), pool, settings));
}

TEST_CASE("master playrate analysis: opt-out keeps the original timeline audio and duration") {
    PlayrateHost h;
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(4800));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(.1 * std::sin(double(i) * .05));
    REQUIRE(audio::writeWav(h.root / "analysis.wav", tone));
    h.run("track.add");
    auto item = json::Value::object();
    item["track"] = "tr_1";
    item["audio_file"] = (h.root / "analysis.wav").string();
    h.run("item.add", json::writeCompact(item));
    OfflineSettings settings;
    settings.applyMasterPlayrate = false;
    settings.endSeconds = .04;
    settings.threads = 1;
    const auto unity = h.app->renderMix(settings);
    REQUIRE(unity);
    h.run("transport.playrate.set", R"({"rate":4,"preserve_pitch":true})");
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":0.25}]})");
    const auto ignored = h.app->renderMix(settings);
    REQUIRE(ignored);
    CHECK(bool(ignored->audio.channels == unity->audio.channels));
    CHECK(ignored->audio.frames() == 1920);
}

TEST_CASE("master playrate recording: delayed non-unity audio must drain after curve removal") {
    struct DelayedBackend : DummyBackend {
        DelayedBackend() : DummyBackend({.mode = Mode::Manual}) {}
        BackendStatus status() const override {
            auto value = DummyBackend::status();
            value.outputLatency = 20000;
            return value;
        }
    } backend;
    PlayrateHost h;
    h.arm();
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    model::Envelope curve;
    curve.target = {model::kMaster, "playrate"};
    curve.timebase = model::Timebase::Time;
    curve.points = {{0, 1, model::PointShape::Square},
                    {3000. / 48000., .25, model::PointShape::Square},
                    {3500. / 48000., 1}};
    const auto lane = edit.create(curve);
    auto session = edit.commit().value();
    auto pool = playrateFixturePool();
    MediaCache media;
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1}));
    REQUIRE(engine.load(session));
    engine.play();
    backend.run(8);
    model::EditTxn remove(session, session.version + 1);
    remove.modify<model::Envelope>(lane, [](auto& envelope) { envelope.active = false; });
    REQUIRE(engine.load(remove.commit().value()));
    backend.run(100);
    REQUIRE(engine.effectivePlayrate() ==
            1);  // the earlier 0.25x span has not reached the device yet
    const auto files = h.files();
    const std::vector<RecordTarget> targets = {
        {{model::Kind::Track, 1}, {0}, h.root / "delayed.wav", std::nullopt}};
    const auto refused = engine.startRecording(targets);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "record.playrate");
    CHECK(h.files() == files);
    backend.run(160);
    REQUIRE(engine.startRecording(targets));
    backend.run(1);
    REQUIRE(engine.stopRecording());
    engine.stop();
}

TEST_CASE("master playrate recording: a pitch-mode transition at unity remains safe") {
    PlayrateHost h;
    h.arm();
    h.run("transport.playrate.set", R"({"rate":2})");
    h.start();
    h.run("transport.playrate.set", R"({"rate":1})");
    h.run("transport.play");
    h.backend.run(8);
    h.run("transport.playrate.set", R"({"preserve_pitch":true})");
    h.backend.run(76);  // inside the mode transition, after the normal edit horizon
    h.run("transport.record");
    h.backend.run(1);
    h.run("transport.stop");
}

TEST_CASE("master playrate edits: inserting multi-second latent FX preserves playback") {
    constexpr double fs = 8000;
    for (bool anticipative : {false, true})
        for (bool preserve : {false, true})
            for (int seconds : {3, 12})
                for (bool takeFx : {false, true}) {
                    CAPTURE(anticipative);
                    CAPTURE(preserve);
                    CAPTURE(seconds);
                    CAPTURE(takeFx);
                    PlayrateHost h;
                    audio::AudioData tone;
                    tone.sampleRate = fs;
                    tone.channels.assign(2, std::vector<float>(128000));
                    for (std::size_t i = 0; i < tone.frames(); ++i)
                        tone.channels[0][i] = tone.channels[1][i] =
                            static_cast<float>(.2 * std::sin(double(i) * .07));
                    REQUIRE(audio::writeWav(h.root / "latent.wav", tone));
                    h.run("track.add");
                    h.run("track.add");
                    auto args = json::Value::object();
                    args["audio_file"] = (h.root / "latent.wav").string();
                    for (const char* tr : {"tr_1", "tr_2"}) {
                        args["track"] = tr;
                        h.run("item.add", json::writeCompact(args));
                    }
                    model::EditTxn initial(h.app->head(), 100);
                    auto project = initial.project();
                    project.console9 = false;
                    project.playrate = {.8, preserve};
                    initial.setProject(project);
                    const auto inverted = h.app->head().items.begin()->value->activeTake.value();
                    initial.modify<model::Take>(inverted,
                                                [](auto& take) { take.phaseInvert = true; });
                    auto session = initial.commit().value();
                    auto pool = playrateFixturePool();
                    MediaCache media;
                    DummyBackend backend(
                        {.mode = DummyBackend::Mode::Manual, .captureFrames = 200000});
                    Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
                    REQUIRE(engine.start(backend, {.sampleRate = fs, .blockSize = 128}));
                    REQUIRE(engine.load(session));
                    engine.play();
                    auto run = [&](int cycles) {
                        for (int i = 0; i < cycles; ++i) {
                            // Recovery can start a new epoch in service(), so wait
                            // relative to its consumed stream, not lifetime cycles.
                            engine.service();
                            if (anticipative) {
                                const auto clock = engine.meterClock();
                                REQUIRE(engine.awaitRendered(clock.heardStream + 128));
                            }
                            backend.run(1);
                        }
                    };
                    run(100);
                    const auto epoch = engine.meterClock().epoch;
                    const auto before = engine.transport().seconds;
                    const auto latency = static_cast<std::uint32_t>(seconds * fs);
                    model::EditTxn insert(session, session.version + 1);
                    model::FxInstance fx;
                    fx.plugin.id = "com.suppleaudio.test.edge";
                    fx.state = "sha256:playrate-long-latency";
                    insert.putBlob(*fx.state, std::make_shared<const model::Blob>(model::Blob{
                                                  {49, static_cast<std::uint8_t>(latency),
                                                   static_cast<std::uint8_t>(latency >> 8),
                                                   static_cast<std::uint8_t>(latency >> 16)}}));
                    const auto id = insert.create(fx);
                    if (takeFx)
                        insert.modify<model::Take>(inverted,
                                                   [&](auto& take) { take.chain.push_back(id); });
                    else
                        insert.modify<model::Track>(
                            model::Id{model::Kind::Track, 1},
                            [&](auto& track) { track.chain.push_back(id); });
                    session = insert.commit().value();
                    REQUIRE(engine.load(session));
                    // The old engine used to reject this insertion in the callback
                    // executor; both executors must now adopt it automatically.
                    run(static_cast<int>(latency / 128) + 100);
                    CHECK(engine.transport().playing);
                    CHECK(engine.transport().seconds > before);
                    CHECK(engine.effectivePlayrate() == .8);
                    CHECK(engine.outputLatency() == (takeFx ? 0 : latency));
                    CHECK(engine.processErrors() == 0);
                    CHECK(engine.warnings().empty());
                    // Read-ahead moves the sibling source when track FX latency
                    // grows too. Stretch phase needs the existing fresh-history
                    // recovery for either kind of compensated source move.
                    if (seconds == 3 && !preserve)
                        CHECK(engine.meterClock().epoch == epoch);
                    else
                        CHECK(engine.meterClock().epoch == epoch + 1);
                    const auto end = static_cast<std::size_t>(backend.cycles() * 128);
                    double difference = 0;
                    for (auto i = end - 2048; i < end; ++i)
                        difference = std::max(difference,
                                              std::abs(double(backend.capture().channels[0][i])));
                    CHECK(difference == 0);  // latent and unlatented takes still cancel
                    // A volume edit and an FX bypass remain ordinary graph edits,
                    // including after the automatic recovery. Confirm audibility
                    // rather than accepting an engine that kept its original plan.
                    const auto adoptedEpoch = engine.meterClock().epoch;
                    model::EditTxn volume(session, session.version + 1);
                    volume.modify<model::Track>(model::Id{model::Kind::Track, 1},
                                                [](auto& track) { track.volumeDb = -6; });
                    session = volume.commit().value();
                    REQUIRE(engine.load(session));
                    // Include the renderer frontier and canonical publication
                    // headroom, rather than assuming the old consumed horizon.
                    run(64);
                    double peak = 0;
                    const auto volumeEnd = static_cast<std::size_t>(backend.cycles() * 128);
                    for (auto i = volumeEnd - 1024; i < volumeEnd; ++i)
                        peak = std::max(peak, std::abs(double(backend.capture().channels[0][i])));
                    CHECK(peak > .01);
                    model::EditTxn bypass(session, session.version + 1);
                    bypass.modify<model::FxInstance>(
                        id, [](auto& instance) { instance.bypass = true; });
                    REQUIRE(engine.load(bypass.commit().value()));
                    // Include the renderer frontier and canonical publication
                    // headroom, rather than assuming the old consumed horizon.
                    run(64);
                    CHECK(engine.outputLatency() == (takeFx ? 0 : latency));  // compensated bypass
                    CHECK(engine.meterClock().epoch == adoptedEpoch);
                    CHECK(engine.transport().playing);
                    CHECK(engine.processErrors() == 0);
                    CHECK(engine.warnings().empty());
                    engine.stop();
                }
}

TEST_CASE("master playrate handoffs: unity graph survives an epoch before its first callback") {
    for (bool changeLoop : {false, true}) {
        CAPTURE(changeLoop);
        PlayrateHost h;
        h.arm();
        model::EditTxn edit(h.app->head(), 100);
        auto project = edit.project();
        project.console9 = false;
        edit.setProject(project);
        const auto session = edit.commit().value();
        auto pool = playrateFixturePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .captureFrames = 128,
                              .input = [](float* const* inputs, std::uint32_t channels,
                                          std::uint32_t frames, std::int64_t) {
                                  for (std::uint32_t c = 0; c < channels; ++c)
                                      std::fill_n(inputs[c], frames, .125f);
                              }});
        Engine engine(pool, media, {.anticipative = true, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1}));
        REQUIRE(engine.load(session));
        engine.play();
        REQUIRE(engine.awaitRendered(4096));  // old-epoch handoff is now queued
        // No callback has adopted the graph yet. A locate/loop must not make
        // that graph disappear just because its handoff predates the epoch.
        if (changeLoop)
            engine.setLoop(.5, 1.2, true);
        else
            REQUIRE(engine.prepareSeek(1));
        REQUIRE(engine.awaitRendered(4096));
        backend.run(1);
        CHECK(engine.transport().sample == (changeLoop ? 128 : 48128));
        CHECK(engine.processErrors() == 0);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(std::abs(backend.capture().channels[0][100]) > .01f);
        engine.stop();
    }
}

TEST_CASE("master playrate RT adoption: prepared knob edits keep their epoch") {
    constexpr double fs = 8000;
    for (bool anticipative : {false, true})
        for (bool live : {false, true})
            for (bool preserve : {false, true}) {
                CAPTURE(anticipative);
                CAPTURE(live);
                CAPTURE(preserve);
                PlayrateHost h;
                if (live)
                    h.arm();
                else
                    h.run("track.add");
                audio::AudioData tone;
                tone.sampleRate = fs;
                tone.channels.assign(2, std::vector<float>(32000));
                for (std::size_t i = 0; i < tone.frames(); ++i)
                    tone.channels[0][i] = tone.channels[1][i] =
                        static_cast<float>(.2 * std::sin(double(i) * .07));
                REQUIRE(audio::writeWav(h.root / "prepared.wav", tone));
                auto args = json::Value::object();
                args["track"] = "tr_1";
                args["audio_file"] = (h.root / "prepared.wav").string();
                h.run("item.add", json::writeCompact(args));
                model::EditTxn edit(h.app->head(), 100);
                auto project = edit.project();
                project.console9 = false;
                project.playrate = {.8, preserve};
                edit.setProject(project);
                const auto initial = edit.commit().value();
                auto pool = playrateFixturePool();
                MediaCache media;
                DummyBackend backend({.mode = DummyBackend::Mode::Manual});
                Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
                REQUIRE(engine.start(backend, {.sampleRate = fs, .blockSize = 128, .inputs = 1}));
                REQUIRE(engine.load(initial));
                engine.play();
                auto run = [&](int cycles) {
                    for (int i = 0; i < cycles; ++i) {
                        if (anticipative) REQUIRE(engine.awaitRendered(1024));
                        backend.run(1);
                        engine.service();
                        (void)engine.transport();  // concurrent renderer/control metadata reads
                    }
                };
                run(32);
                const auto epoch = engine.meterClock().epoch;
                model::EditTxn knob(initial, initial.version + 1);
                project.playrate.rate = 1.137;
                knob.setProject(project);
                REQUIRE(engine.load(knob.commit().value()));
                run(256);  // the knob's smoothing settles
                REQUIRE(engine.awaitPlan());
                CHECK(engine.effectivePlayrate() == 1.137);
                CHECK(engine.meterClock().epoch == epoch);  // non-seek RT adoption actually ran
                CHECK(engine.processErrors() == 0);
                CHECK(engine.warnings().empty());
                engine.stop();
            }
}

TEST_CASE("master playrate scalar control: sustained sweep retains converters and epoch") {
    constexpr double fs = 8000;
    for (bool anticipative : {false, true})
        for (bool live : {false, true})
            for (bool preserve : {false, true}) {
                CAPTURE(anticipative);
                CAPTURE(live);
                CAPTURE(preserve);
                PlayrateHost h;
                if (live)
                    h.arm();
                else
                    h.run("track.add");
                for (int i = 1; i < 8; ++i) h.run("track.add");
                audio::AudioData tone;
                tone.sampleRate = fs;
                tone.channels.assign(1, std::vector<float>(200000));
                for (std::size_t i = 0; i < tone.frames(); ++i)
                    tone.channels[0][i] = static_cast<float>(
                        .2 * std::sin(double(i) * 2 * 3.141592653589793 * 55 / fs));
                REQUIRE(audio::writeWav(h.root / "sweep.wav", tone));
                auto args = json::Value::object();
                args["track"] = "tr_1";
                args["audio_file"] = (h.root / "sweep.wav").string();
                h.run("item.add", json::writeCompact(args));
                if (live) {
                    args["track"] = "tr_2";
                    h.run("item.add", json::writeCompact(args));
                }
                model::EditTxn edit(h.app->head(), 100);
                auto project = edit.project();
                project.console9 = false;
                project.playrate = {1.1, preserve};
                edit.setProject(project);
                if (live) {
                    // Compare a callback source with its renderer
                    // twin in separate output channels through the same glide.
                    edit.modify<model::Track>({model::Kind::Track, 1},
                                              [](auto& track) { track.pan = -100; });
                    edit.modify<model::Track>({model::Kind::Track, 2},
                                              [](auto& track) { track.pan = 100; });
                }
                auto session = edit.commit().value();
                auto pool = playrateFixturePool();
                MediaCache media;
                DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 80000});
                Engine engine(pool, media,
                              {.anticipative = anticipative, .renderBlock = 256, .workers = 0});
                REQUIRE(engine.start(backend, {.sampleRate = fs, .blockSize = 64, .inputs = 1}));
                REQUIRE(engine.load(session));
                engine.play();
                std::size_t capturedFrames = 0;
                auto run = [&](int cycles) {
                    for (int i = 0; i < cycles; ++i) {
                        if (anticipative) REQUIRE(engine.awaitRendered(1024));
                        backend.run(1);
                        capturedFrames += 64;
                        engine.service();
                    }
                };
                auto rate = [&](double value) {
                    project.playrate.rate = value;
                    model::EditTxn knob(session, session.version + 1);
                    knob.setProject(project);
                    session = knob.commit().value();
                    REQUIRE(engine.load(session));
                };
                run(64);
                const auto epoch = engine.meterClock().epoch;
                const auto generation = engine.planGeneration();
                // 128 device frames/edit at 8 kHz = 62.5 Hz. All assertions
                // use device progress, never elapsed wall time.
                for (int step = 1; step <= 180; ++step) {
                    const double requested = 1.1 + .4 * std::sin(step * 3.141592653589793 / 180);
                    rate(requested);
                    run(2);
                    CHECK(engine.planGeneration() == generation);
                    CHECK(engine.meterClock().epoch == epoch);
                    // Lookahead and the knob's smoothing both lag the request.
                    if (step > 40) CHECK(std::abs(engine.effectivePlayrate() - requested) < .25);
                }
                rate(1.37);
                run(384);
                CHECK(engine.effectivePlayrate() == 1.37);
                CHECK(engine.meterClock().epoch == epoch);
                // A structural edit must inherit already scheduled scalar
                // controls as well as the clock's consumed ramp state.
                rate(1.43);
                model::EditTxn rename(session, session.version + 1);
                rename.modify<model::Track>({model::Kind::Track, 1},
                                            [](auto& track) { track.name = "Renamed"; });
                session = rename.commit().value();
                REQUIRE(engine.load(session));
                run(384);
                CHECK(engine.effectivePlayrate() == 1.43);
                CHECK(engine.meterClock().epoch == epoch);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& captured = backend.capture();
                const auto& x = captured.channels[0];
                std::size_t clicks = 0, gaps = 0, zeros = 0;
                for (std::size_t i = 2048; i < std::min(capturedFrames, captured.frames()); ++i) {
                    if (live) CHECK(x[i] == captured.channels[1][i]);
                    if (std::abs(x[i] - 2 * x[i - 1] + x[i - 2]) > .02f) ++clicks;
                    if (x[i] == 0) {
                        if (++zeros == 64) ++gaps;
                    } else
                        zeros = 0;
                }
                CHECK(clicks == 0);
                CHECK(gaps == 0);
            }
}

TEST_CASE("master playrate scalar control: spaced knob steps join into one smooth glide") {
    constexpr double fs = 8000;
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        PlayrateHost h;
        h.run("track.add");
        model::EditTxn edit(h.app->head(), 100);
        auto project = edit.project();
        project.playrate = {1.1, false};
        edit.setProject(project);
        auto session = edit.commit().value();
        auto pool = playrateFixturePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media,
                      {.anticipative = anticipative, .renderBlock = 256, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = fs, .blockSize = 64}));
        REQUIRE(engine.load(session));
        engine.play();
        std::vector<double> heard;
        auto run = [&](int cycles) {
            for (int i = 0; i < cycles; ++i) {
                if (anticipative) REQUIRE(engine.awaitRendered(1024));
                backend.run(1);
                engine.service();
                heard.push_back(engine.effectivePlayrate());
            }
        };
        run(64);
        heard.clear();
        // A MIDI knob's coarse steps, 512 device frames (64 ms) apart.
        for (int step = 1; step <= 10; ++step) {
            project.playrate.rate = 1.1 + .03 * step;
            model::EditTxn knob(session, session.version + 1);
            knob.setProject(project);
            session = knob.commit().value();
            REQUIRE(engine.load(session));
            run(8);
        }
        run(384);
        REQUIRE(heard.back() == doctest::Approx(1.4));
        // From the first audible change to the final value the rate rises at
        // every block: no step is a jump followed by a hold.
        const auto first =
            std::find_if(heard.begin(), heard.end(), [](double r) { return r > 1.1; });
        const auto last =
            std::find_if(heard.begin(), heard.end(), [](double r) { return r >= 1.4 - 1e-9; });
        REQUIRE(first < last);
        for (auto it = first + 1; it <= last; ++it) CHECK(*it > *(it - 1));
        // And its slope bends gradually: where one step's ramp meets the next
        // there is no corner (unsmoothed ramps bend by about 80 % of the
        // steepest slope in one block; smoothed, under 10 %).
        double steepest = 0, corner = 0;
        for (auto it = first + 2; it <= last; ++it) {
            const double slope = *it - *(it - 1), before = *(it - 1) - *(it - 2);
            steepest = std::max(steepest, slope);
            corner = std::max(corner, std::abs(slope - before));
        }
        CHECK(corner < .2 * steepest);
        engine.stop();
    }
}

TEST_CASE("master playrate scalar control: a knob glides through and away from 1.0") {
    constexpr double fs = 8000;
    for (bool anticipative : {false, true})
        for (bool preserve : {false, true})
            for (double start : {.94, 1.}) {
                CAPTURE(anticipative);
                CAPTURE(preserve);
                CAPTURE(start);
                PlayrateHost h;
                h.run("track.add");
                audio::AudioData tone;
                tone.sampleRate = fs;
                tone.channels.assign(1, std::vector<float>(200000));
                for (std::size_t i = 0; i < tone.frames(); ++i)
                    tone.channels[0][i] = static_cast<float>(
                        .2 * std::sin(double(i) * 2 * 3.141592653589793 * 55 / fs));
                REQUIRE(audio::writeWav(h.root / "unity.wav", tone));
                auto args = json::Value::object();
                args["track"] = "tr_1";
                args["audio_file"] = (h.root / "unity.wav").string();
                h.run("item.add", json::writeCompact(args));
                model::EditTxn edit(h.app->head(), 100);
                auto project = edit.project();
                project.console9 = false;
                project.playrate = {start, preserve};
                edit.setProject(project);
                auto session = edit.commit().value();
                auto pool = playrateFixturePool();
                MediaCache media;
                DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 80000});
                Engine engine(pool, media,
                              {.anticipative = anticipative, .renderBlock = 256, .workers = 0});
                REQUIRE(engine.start(backend, {.sampleRate = fs, .blockSize = 64}));
                REQUIRE(engine.load(session));
                engine.play();
                std::vector<double> heard;
                std::size_t capturedFrames = 0;
                auto run = [&](int cycles) {
                    for (int i = 0; i < cycles; ++i) {
                        if (anticipative) REQUIRE(engine.awaitRendered(1024));
                        backend.run(1);
                        capturedFrames += 64;
                        engine.service();
                        heard.push_back(engine.effectivePlayrate());
                    }
                };
                auto rate = [&](double value) {
                    project.playrate.rate = value;
                    model::EditTxn knob(session, session.version + 1);
                    knob.setProject(project);
                    session = knob.commit().value();
                    REQUIRE(engine.load(session));
                };
                // A smooth glide: the slope bends gradually (see "spaced knob steps").
                auto smooth = [&](std::size_t from) {
                    double steepest = 0, corner = 0;
                    for (std::size_t i = from + 2; i < heard.size(); ++i) {
                        const double slope = heard[i] - heard[i - 1];
                        const double before = heard[i - 1] - heard[i - 2];
                        steepest = std::max(steepest, std::abs(slope));
                        corner = std::max(corner, std::abs(slope - before));
                    }
                    return corner < .2 * steepest;
                };
                run(64);
                const auto epoch = engine.meterClock().epoch;
                // A MIDI knob through its centre (CC 64 is exactly 1.0), or away
                // from 1.0 when playback started there (the unity bypass).
                const auto generation = engine.planGeneration();
                std::size_t mark = heard.size();
                for (const double value : {.96, .98, 1., 1.02, 1.04, 1.06}) {
                    if (value < start) continue;
                    rate(value);
                    run(8);
                }
                run(384);
                if (start != 1) CHECK(engine.planGeneration() == generation);  // no rebuild
                CHECK(heard.back() == doctest::Approx(1.06));
                CHECK(smooth(mark));
                // Back to rest at 1.0, where an unrelated edit compiles the plan;
                // a preserve-pitch plan compiled at 1.0 reads tape and must
                // prepare the stretch converter to leave it, still gliding.
                rate(1);
                run(384);
                CHECK(engine.effectivePlayrate() == 1);
                model::EditTxn rename(session, session.version + 1);
                rename.modify<model::Track>({model::Kind::Track, 1},
                                            [](auto& track) { track.name = "Renamed"; });
                session = rename.commit().value();
                REQUIRE(engine.load(session));
                run(64);
                const auto compiled = engine.planGeneration();
                mark = heard.size();
                for (const double value : {1.02, 1.04}) {
                    rate(value);
                    run(8);
                }
                run(384);
                CHECK((engine.planGeneration() != compiled) == preserve);
                CHECK(heard.back() == doctest::Approx(1.04));
                CHECK(smooth(mark));
                CHECK(engine.meterClock().epoch == epoch);
                CHECK(engine.processErrors() == 0);
                CHECK(engine.warnings().empty());
                engine.stop();
                const auto& captured = backend.capture();
                const auto& x = captured.channels[0];
                std::size_t clicks = 0, gaps = 0, zeros = 0;
                for (std::size_t i = 2048; i < std::min(capturedFrames, captured.frames()); ++i) {
                    if (std::abs(x[i] - 2 * x[i - 1] + x[i - 2]) > .02f) ++clicks;
                    if (x[i] == 0) {
                        if (++zeros == 64) ++gaps;
                    } else
                        zeros = 0;
                }
                CHECK(clicks == 0);
                CHECK(gaps == 0);
            }
}

TEST_CASE("master playrate scalar control: transport changes retain later edits") {
    for (bool anticipative : {false, true})
        for (bool preserve : {false, true}) {
            CAPTURE(anticipative);
            CAPTURE(preserve);
            PlayrateHost h;
            h.run("track.add");
            audio::AudioData tone;
            tone.sampleRate = 8000;
            tone.channels.assign(2, std::vector<float>(64000));
            for (std::size_t i = 0; i < tone.frames(); ++i)
                tone.channels[0][i] = tone.channels[1][i] =
                    static_cast<float>(.2 * std::sin(double(i) * .07));
            REQUIRE(audio::writeWav(h.root / "transport.wav", tone));
            auto args = json::Value::object();
            args["track"] = "tr_1";
            args["audio_file"] = (h.root / "transport.wav").string();
            h.run("item.add", json::writeCompact(args));
            auto session = h.app->head();
            model::EditTxn first(session, 2);
            auto project = first.project();
            project.playrate = {.8, preserve};
            first.setProject(project);
            session = first.commit().value();
            auto pool = playrateFixturePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual});
            Engine engine(pool, media,
                          {.anticipative = anticipative, .renderBlock = 256, .workers = 0});
            REQUIRE(engine.start(backend, {.sampleRate = 8000, .blockSize = 128}));
            REQUIRE(engine.load(session));
            engine.play();
            auto run = [&](int cycles) {
                for (int i = 0; i < cycles; ++i) {
                    if (anticipative) REQUIRE(engine.awaitRendered(1024));
                    backend.run(1);
                    engine.service();
                }
            };
            auto rate = [&](double value) {
                model::EditTxn knob(session, session.version + 1);
                project.playrate.rate = value;
                knob.setProject(project);
                session = knob.commit().value();
                REQUIRE(engine.load(session));
            };
            run(32);
            for (int i = 0; i < 20; ++i) {
                rate(.9 + .01 * i);
                run(1);
            }
            REQUIRE(engine.prepareSeek(.1));
            rate(1.31);  // control queued before the prepared seek reaches the callback
            run(48);
            engine.pause();
            run(4);
            rate(1.22);
            engine.play();
            run(48);
            engine.setLoop(.1, .3, true);
            run(96);  // wraps repeatedly; the device coordinate remains monotonic
            const auto epoch = engine.meterClock().epoch;
            rate(1.47);
            run(256);
            CHECK(engine.effectivePlayrate() == 1.47);
            CHECK(engine.meterClock().epoch == epoch);
            CHECK(engine.processErrors() == 0);
            engine.stop();
        }
}

TEST_CASE("master playrate scalar control: saturation coalesces without losing the final value") {
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        auto session = model::Session::create();
        model::EditTxn initial(session, 2);
        auto project = initial.project();
        project.playrate.rate = .8;
        initial.setProject(project);
        session = initial.commit().value();
        auto pool = playrateFixturePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media,
                      {.anticipative = anticipative, .renderBlock = 256, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 8000, .blockSize = 128}));
        REQUIRE(engine.load(session));
        engine.play();
        auto run = [&](int cycles) {
            for (int i = 0; i < cycles; ++i) {
                if (anticipative) REQUIRE(engine.awaitRendered(1024));
                backend.run(1);
                engine.service();
            }
        };
        run(32);
        const auto generation = engine.planGeneration();
        const auto epoch = engine.meterClock().epoch;
        for (int step = 0; step < 1100; ++step) {
            model::EditTxn edit(session, session.version + 1);
            project.playrate.rate = step == 1099 ? 1.47 : .7 + .001 * (step % 100);
            edit.setProject(project);
            session = edit.commit().value();
            REQUIRE(engine.load(session));
        }
        run(384);
        CHECK(engine.effectivePlayrate() == 1.47);
        CHECK(engine.planGeneration() == generation);
        CHECK(engine.meterClock().epoch == epoch);
        engine.stop();
    }
}

TEST_CASE("master playrate scalar control: plugin maintenance still recompiles compensation") {
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        model::EditTxn edit(model::Session::create(), 2);
        auto project = edit.project();
        project.playrate.rate = .8;
        edit.setProject(project);
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.gain";
        fx.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"none"}})")
                .value();
        const auto id = edit.create(fx);
        edit.modify<model::Track>(model::kMaster, [&](auto& track) { track.chain.push_back(id); });
        auto session = edit.commit().value();
        auto pool = playrateFixturePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media,
                      {.anticipative = anticipative, .renderBlock = 256, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 8000, .blockSize = 128}));
        REQUIRE(engine.load(session));
        engine.play();
        auto run = [&](int cycles) {
            for (int i = 0; i < cycles; ++i) {
                if (anticipative) REQUIRE(engine.awaitRendered(1024));
                backend.run(1);
                engine.service();
            }
        };
        run(32);
        auto* instance = pool.find(id);
        REQUIRE(instance);
        CHECK(instance->processing());
        model::EditTxn change(session, session.version + 1);
        change.modify<model::FxInstance>(id, [](auto& plugin) {
            plugin.patch =
                json::parse(
                    R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                    .value();
        });
        REQUIRE(engine.load(change.commit().value()));
        // The restart's notifications reload the same model twice: first to
        // release the instance, then to restore it with its new compensation.
        run(128);
        CHECK(pool.find(id) == instance);
        CHECK(instance->latency() == 64);
        CHECK(instance->processing());
        CHECK_FALSE(pool.restarting(id));
        CHECK(engine.outputLatency() == 64);
        CHECK(engine.processErrors() == 0);
        engine.stop();
    }
}

TEST_CASE(
    "recording from stop under a scheduled playrate is refused when the device never takes the "
    "place") {
    // Recording starts where the renderer was placed: with the playrate
    // scheduled (here an active envelope held at 1, which recording allows),
    // from stop the engine places it and waits for the device to take it. A
    // device that stopped calling back never does, so the start is refused
    // with engine.transport and nothing is written.
    engine::test::StalledBackend backend;
    PlayrateHost h;
    h.arm();
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    model::Envelope curve;
    curve.target = {model::kMaster, "playrate"};
    curve.timebase = model::Timebase::Time;
    curve.points = {{0, 1}, {1, 1}};
    edit.create(curve);
    const auto session = edit.commit().value();
    REQUIRE(model::validateRecordingPlayrate(session));
    auto pool = playrateFixturePool();
    MediaCache media;
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1}));
    REQUIRE(engine.load(session));
    const auto files = h.files();
    const std::vector<RecordTarget> targets = {
        {{model::Kind::Track, 1}, {0}, h.root / "stalled.wav", std::nullopt}};
    const auto refused = engine.startRecording(targets);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "engine.transport");
    CHECK(h.files() == files);
    engine.stop();
}
