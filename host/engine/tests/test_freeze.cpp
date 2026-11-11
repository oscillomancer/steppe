// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/model/freeze.h"
#include "supple/model/schemas.h"
using namespace supple;
using namespace supple::engine;
namespace {
const model::Id track{model::Kind::Track, 1};
const cmd::Origin origin{cmd::Origin::Type::Test, "freeze", ""};
struct Fixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("freeze-tests-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    explicit Fixture(AppOptions options = {}) {
        std::filesystem::create_directories(dir);
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto made = EngineApp::create(options);
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name":"Frozen source"})");
        audio::AudioData source;
        source.sampleRate = 48000;
        source.channels.assign(2, std::vector<float>(2048));
        for (std::size_t i = 0; i < source.frames(); ++i)
            source.channels[0][i] = source.channels[1][i] = float(.1 * std::sin(double(i) * .07));
        REQUIRE(audio::writeWav(dir / "input.wav", source));
        auto args = json::Value::object();
        args["track"] = "tr_1";
        args["audio_file"] = (dir / "input.wav").string();
        auto item = bus->execute({"item.add", args, origin});
        REQUIRE_MESSAGE(item, (item ? "" : item.error().toString()));
    }
    ~Fixture() {
        bus.reset();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    json::Value run(std::string name, std::string_view text = "{}") {
        auto args = json::parse(text);
        REQUIRE(args);
        auto result = bus->execute({name, *args, origin});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    Result<json::Value> call(std::string name, std::string_view text) {
        return bus->execute({name, *json::parse(text), origin});
    }
};
}  // namespace
TEST_CASE("freeze: exact pre-fader capture, live mixer, history and save reopen" *
          doctest::description("rt")) {
    Fixture f;
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("fx.param.set", R"({"fx":"fx_1","values":{"gain":-9}})");
    auto before = f.app->head();
    OfflineSettings settings;
    settings.freezeTrack = track;
    settings.tap = model::Tap::PreFader;
    settings.endSeconds = 2048. / 48000.;
    settings.tailSeconds = 1.;
    settings.threads = 1;
    auto reference = f.app->renderSession(before, settings);
    REQUIRE(reference);
    const auto version = before.version;
    auto done = f.run("track.freeze", R"({"track":"tr_1"})");
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    CHECK(f.app->head().version == version + 1);
    auto cached = audio::readAudio(done["path"].asString());
    REQUIRE(cached);
    CHECK(cached->channels == reference->audio.channels);
    auto frozen = f.app->renderSession(f.app->head(), settings);
    REQUIRE(frozen);
    CHECK(frozen->audio.channels == reference->audio.channels);
    const auto cache = done["path"].asString();
    auto edit = f.call("item.set", R"({"item":"it_1","gain_db":-6})");
    REQUIRE_FALSE(edit);
    CHECK(edit.error().code == "track.frozen");
    CHECK(edit.error().hint.find("track.unfreeze") != std::string::npos);
    CHECK_FALSE(f.call("fx.param.set", R"({"fx":"fx_1","values":{"gain_db":-6}})"));
    CHECK_FALSE(f.call("tempo.set", R"({"points":[{"beat":0,"bpm":90}]})"));
    f.run("track.set", R"({"track":"tr_1","volume_db":-6,"pan":20})");
    f.run(
        "track.set",
        R"({"track":"tr_1","pan_mode":"width","width":35,"pan_left":-20,"pan_right":60,"pan_law":{"db":-3,"boost":true}})");
    f.run(
        "track.set",
        R"({"track":"tr_1","pan_mode":"pan","width":100,"pan_left":-100,"pan_right":100,"pan_law":null})");
    f.run("track.unfreeze", R"({"track":"tr_1"})");
    CHECK_FALSE(f.app->head().tracks.find(track)->freeze);
    f.run("history.undo");
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    CHECK(std::filesystem::exists(cache));
    f.run("history.redo");
    CHECK_FALSE(f.app->head().tracks.find(track)->freeze);
    f.run("history.undo");
    REQUIRE(f.app->save(f.dir / "song.project"));
    REQUIRE(f.app->open(f.dir / "song.project"));
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    auto reopened = f.app->renderSession(f.app->head(), settings);
    REQUIRE(reopened);
    CHECK(reopened->audio.channels == reference->audio.channels);
}

TEST_CASE("freeze: both executors reproduce the frozen offline graph" *
          doctest::description("rt")) {
    Fixture f;
    f.run("track.freeze", R"({"track":"tr_1"})");
    OfflineSettings settings;
    settings.endSeconds = 4096. / 48000.;
    settings.threads = 1;
    auto reference = f.app->renderSession(f.app->head(), settings);
    REQUIRE(reference);
    for (bool anticipative : {false, true}) {
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4096});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                                   {.anticipative = anticipative, .workers = 1}));
        f.app->engine()->play();
        for (int i = 0; i < 32; ++i) {
            if (anticipative) REQUIRE(f.app->engine()->awaitRendered((i + 1) * 128));
            backend.run(1);
        }
        f.app->stopEngine();
        CHECK(backend.capture().channels == reference->audio.channels);
    }
}

TEST_CASE("freeze: job permits unrelated edits and rejects changed sound") {
    for (bool stale : {false, true}) {
        std::mutex mutex;
        std::condition_variable cv;
        bool released = false, entered = false, finished = false;
        AppOptions options;
        options.jobStartedHook = [&] {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        };
        options.jobFinishedHook = [&] {
            std::lock_guard lock(mutex);
            finished = true;
            cv.notify_all();
        };
        Fixture f(options);
        f.run("track.add");
        auto job = f.run("track.freeze", R"({"track":"tr_1","job":true})");
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }
        f.run("track.set", R"({"track":"tr_2","volume_db":-6})");
        if (stale) f.run("item.set", R"({"item":"it_1","gain_db":-6})");
        {
            std::lock_guard lock(mutex);
            released = true;
            cv.notify_all();
        }
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return finished; });
        }
        f.bus->flushJobEvents();
        auto state = f.bus->jobs().status(job.find("job")->asString());
        REQUIRE(state);
        CHECK((*state)["status"].asString() == (stale ? "failed" : "done"));
        if (stale) CHECK((*state)["error"]["code"].asString() == "freeze.stale_target");
        CHECK(f.app->head().tracks.find(track)->freeze.has_value() == !stale);
    }
}

TEST_CASE("freeze: virtual latency preserves Console mix and auxiliary output" *
          doctest::description("rt")) {
    Fixture f;
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})");
    auto tx = f.app->beginTransaction(origin, "Delay fixture");
    REQUIRE(tx);
    const model::Id fx{model::Kind::Fx, 1};
    const std::string hash = "sha256:" + std::string(64, '4');
    f.app->txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{{49, 37, 0, 0}}));
    f.app->txn().modify<model::FxInstance>(fx, [&](auto& value) {
        value.patch.reset();
        value.state = hash;
    });
    REQUIRE(f.app->commitTransaction(*tx));
    f.run("track.set", R"({"track":"tr_1","channels":4})");
    f.run("fx.set", R"({"fx":"fx_1","pins":{"in":[1,2],"out":[3,4]}})");
    f.run("route.add", R"({"from":"tr_1","to":"tr_0","channels":{"from":[3,4],"to":[1,2]}})");
    OfflineSettings settings;
    settings.endSeconds = 4096. / 48000.;
    settings.threads = 1;
    auto reference = f.app->renderSession(f.app->head(), settings);
    REQUIRE(reference);
    DummyBackend captureBackend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(f.app->startEngine(captureBackend, {.sampleRate = 48000, .blockSize = 128},
                               {.anticipative = false, .workers = 1}));
    f.run("track.freeze", R"({"track":"tr_1"})");
    f.app->stopEngine();
    // Capturing the live opaque state creates and retains its actual content hash.
    CHECK(f.app->head().fx.find(fx)->state != hash);
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    CHECK(f.app->head().tracks.find(track)->freeze->latencyFrames == 37);
    auto rendered = f.app->renderSession(f.app->head(), settings);
    REQUIRE(rendered);
    CHECK(rendered->audio.channels == reference->audio.channels);
    CHECK(rendered->outputLatency == reference->outputLatency);
    for (bool anticipative : {false, true}) {
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4224});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                                   {.anticipative = anticipative, .workers = 1}));
        f.app->engine()->play();
        for (int i = 0; i < 33; ++i) {
            if (anticipative) REQUIRE(f.app->engine()->awaitRendered((i + 1) * 128));
            backend.run(1);
        }
        f.app->stopEngine();
        for (std::size_t c = 0; c < 2; ++c)
            CHECK(std::equal(reference->audio.channels[c].begin(),
                             reference->audio.channels[c].end(),
                             backend.capture().channels[c].begin() + reference->outputLatency));
    }
    f.run("track.unfreeze", R"({"track":"tr_1"})");
    auto restored = f.app->renderSession(f.app->head(), settings);
    REQUIRE(restored);
    CHECK(restored->audio.channels == reference->audio.channels);
}

TEST_CASE("freeze: branch replay reuses cache and remaps new media IDs") {
    Fixture f;
    f.run("branch.create", R"({"name":"agent/freeze","switch":false})");
    f.run("item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    f.run("branch.switch", R"({"name":"agent/freeze"})");
    // Freeze the original sound, then change only an unrelated mixer control on main.
    f.run("track.freeze", R"({"track":"tr_1"})");
    const auto source = f.app->head().tracks.find(track)->freeze->source;
    f.run("branch.switch", R"({"name":"main"})");
    auto merged = f.call("branch.merge", R"({"from":"agent/freeze"})");
    // The changed source arrangement must produce a replay conflict.
    CHECK_FALSE(f.app->head().tracks.find(track)->freeze);
    REQUIRE_FALSE(merged);
    CHECK(merged.error().code == "branch.conflict");
    f.run("history.undo");
    f.run("track.set", R"({"track":"tr_1","volume_db":-9})");
    f.run("track.add");
    audio::AudioData other;
    other.sampleRate = 48000;
    other.channels.assign(2, std::vector<float>(128, .05f));
    REQUIRE(audio::writeWav(f.dir / "other.wav", other));
    auto args = json::Value::object();
    args["track"] = "tr_2";
    args["audio_file"] = (f.dir / "other.wav").string();
    REQUIRE(f.bus->execute({"item.add", args, origin}));
    f.run("branch.merge", R"({"from":"agent/freeze"})");
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    CHECK(f.app->head().tracks.find(track)->volumeDb == -9);
    CHECK(f.app->head().tracks.find(track)->freeze->source != source);
    f.run("track.duplicate", R"({"tracks":["tr_1"]})");
    const model::Id duplicate{model::Kind::Track, 3};
    REQUIRE(f.app->head().tracks.find(duplicate)->freeze);
    f.run("track.unfreeze", R"({"track":"tr_3"})");
    CHECK(f.app->head().tracks.find(track)->freeze.has_value());
}

TEST_CASE("freeze: bridged plugins retire only after plan adoption and restore" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (const char* isolation : {"in_process", "shared_bridge", "dedicated"}) {
        Fixture f;
        f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
        f.run("fx.set", std::string("{\"fx\":\"fx_1\",\"isolation\":\"") + isolation + "\"}");
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 2048});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                                   {.anticipative = true}));
        const model::Id fx{model::Kind::Fx, 1};
        REQUIRE(f.app->instanceOf(fx));
        OfflineSettings settings;
        settings.freezeTrack = track;
        settings.tap = model::Tap::PreFader;
        settings.endSeconds = 2048. / 48000.;
        settings.tailSeconds = 1.;
        settings.threads = 1;
        auto reference = f.app->renderSession(f.app->head(), settings);
        REQUIRE(reference);
        auto frozen = f.run("track.freeze", R"({"track":"tr_1"})");
        auto audio = audio::readAudio(frozen["path"].asString());
        REQUIRE(audio);
        CHECK(audio->channels == reference->audio.channels);
        // Service/adoption, not elapsed time, determines when the instance can go.
        f.app->engine()->play();
        for (int i = 0; i < 16; ++i) {
            REQUIRE(f.app->engine()->awaitRendered((i + 1) * 128));
            backend.run(1);
            f.app->tick();
        }
        CHECK(f.app->instanceOf(fx) == nullptr);
        bool moved = false;
        CHECK_FALSE(f.app->moveParam(fx, "gain", [&](double, double) {
            moved = true;
            return std::optional<double>(.5);
        }));
        CHECK_FALSE(moved);
        auto description = f.run("fx.describe", R"({"fx":"fx_1","full":true})");
        CHECK(description["description"].asString().find("Frozen") != std::string::npos);
        CHECK(f.app->instanceOf(fx) == nullptr);
        f.run("track.unfreeze", R"({"track":"tr_1"})");
        CHECK(f.app->instanceOf(fx) != nullptr);
        f.app->stopEngine();
    }
}

TEST_CASE("freeze: MIDI instruments, take FX and pooled automation stay protected" *
          doctest::description("rt")) {
    Fixture f;
    f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("item.add", R"({"track":"tr_1","notation":"1: A4:w"})");
    f.run("env.add", R"({"fx":"fx_3","param":"gain","points":[{"beat":0,"value":-3}]})");
    f.run("autoitem.add",
          R"({"env":"en_1","position":0,"length":4,"points":[{"beat":0,"value":-6}]})");
    f.run("autoitem.duplicate", R"({"item":"ai_1","position":4})");
    f.run("track.add");
    f.run("env.add", R"({"track":"tr_2","param":"volume"})");
    auto move = f.app->beginTransaction(origin, "Cross-track pool fixture");
    REQUIRE(move);
    f.app->txn().modify<model::AutomationItem>(model::Id{model::Kind::AutoItem, 2}, [](auto& item) {
        item.envelope = model::Id{model::Kind::Envelope, 2};
    });
    REQUIRE(f.app->commitTransaction(*move));
    OfflineSettings settings;
    settings.freezeTrack = track;
    settings.tap = model::Tap::PreFader;
    settings.endSeconds = 2.;
    settings.tailSeconds = 1.;
    settings.threads = 1;
    auto reference = f.app->renderSession(f.app->head(), settings);
    REQUIRE(reference);
    auto frozen = f.run("track.freeze", R"({"track":"tr_1"})");
    auto audio = audio::readAudio(frozen["path"].asString());
    REQUIRE(audio);
    CHECK(audio->channels == reference->audio.channels);
    auto denied = f.call("autoitem.set", R"({"item":"ai_2","points":[{"beat":0,"value":0}]})");
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == "track.frozen");
    denied = f.call("take.set", R"({"take":"tk_1","gain_db":-3})");
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == "track.frozen");
    f.run("env.add", R"({"track":"tr_1","param":"volume","points":[{"beat":0,"value":-3}]})");
    f.run("track.unfreeze", R"({"track":"tr_1"})");
    auto restored = f.app->renderSession(f.app->head(), settings);
    REQUIRE(restored);
    CHECK(restored->audio.channels == reference->audio.channels);
}

TEST_CASE("freeze: a growing reported tail cannot be cut at early silence") {
    Fixture f;
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})");
    auto tx = f.app->beginTransaction(origin, "Tail fixture");
    REQUIRE(tx);
    const model::Id fx{model::Kind::Fx, 1};
    const std::string hash = "sha256:" + std::string(64, '5');
    // Mode 20 doubles the reported 48000-frame tail once processing begins.
    f.app->txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{{20, 128, 187, 0}}));
    f.app->txn().modify<model::FxInstance>(fx, [&](auto& value) {
        value.patch.reset();
        value.state = hash;
    });
    REQUIRE(f.app->commitTransaction(*tx));
    f.run("track.freeze", R"({"track":"tr_1"})");
    CHECK(f.app->head().tracks.find(track)->freeze->tailFrames == 96000);
}

TEST_CASE("freeze: cancellation and a branch switch cannot publish a finished job") {
    for (bool cancel : {false, true}) {
        std::mutex mutex;
        std::condition_variable cv;
        bool released = false, finished = false;
        AppOptions options;
        // The worker waits until track.freeze has returned: the bus runs posted
        // work right after the command, so a render that finished first would
        // be adopted before the cancel or switch below.
        options.jobStartedHook = [&] {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return released; });
        };
        options.jobFinishedHook = [&] {
            std::lock_guard lock(mutex);
            finished = true;
            cv.notify_all();
        };
        Fixture f(options);
        f.run("branch.create", R"({"name":"other","switch":false})");
        auto job = f.run("track.freeze", R"({"track":"tr_1","job":true})");
        {
            std::unique_lock lock(mutex);
            released = true;
            cv.notify_all();
            cv.wait(lock, [&] { return finished; });
        }
        // Completion is queued but not yet adopted on the model thread.
        if (cancel)
            REQUIRE(f.bus->jobs().cancel(job["job"].asString()));
        else {
            REQUIRE(f.app->switchBranch("other"));
        }
        f.bus->flushJobEvents();
        CHECK_FALSE(f.app->head().tracks.find(track)->freeze);
        auto state = f.bus->jobs().status(job.find("job")->asString());
        REQUIRE(state);
        CHECK((*state)["status"].asString() == (cancel ? "cancelled" : "failed"));
        for (const auto& entry : std::filesystem::directory_iterator(f.app->mediaDir()))
            CHECK_FALSE(entry.path().filename().string().starts_with("freeze-"));
    }
}

TEST_CASE("freeze: external inputs and armed channels refuse; outgoing sends remain live") {
    Fixture f;
    f.run("track.add");
    f.run("track.set", R"({"track":"tr_2","type":"buss"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.keymix"})");
    f.run("route.add", R"({"from":"tr_2","fx":"fx_1"})");
    auto refused = f.call("track.freeze", R"({"track":"tr_1"})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "freeze.unsupported");
    f.run("route.remove", R"({"route":"sd_1"})");
    f.run("track.set", R"({"track":"tr_1","armed":true})");
    refused = f.call("track.freeze", R"({"track":"tr_1"})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "freeze.unsupported");
    f.run("track.set", R"({"track":"tr_1","armed":false})");
    f.run("track.freeze", R"({"track":"tr_1"})");
    f.run("route.add", R"({"from":"tr_1","to":"tr_2"})");
    refused = f.call("track.set", R"({"track":"tr_1","armed":true})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "track.frozen");
    refused = f.call("tempo.set", R"({"points":[{"beat":0,"bpm":90}]})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "track.frozen");
    CHECK(refused.error().message.find("tr_1") != std::string::npos);
    CHECK(refused.error().hint.find("Frozen source (tr_1)") != std::string::npos);
}

TEST_CASE("freeze: reopen plays without plugins and failed unfreeze keeps the cache") {
    Fixture f;
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("track.freeze", R"({"track":"tr_1"})");
    REQUIRE(f.app->save(f.dir / "frozen.project"));
    AppOptions options;
    options.workdir = f.dir / "reopen";
    options.pluginPath = {f.dir / "missing-plugin-folder"};
    auto missing = EngineApp::create(options);
    REQUIRE(missing);
    REQUIRE((*missing)->open(f.dir / "frozen.project"));
    cmd::Dispatcher bus(f.registry, **missing);
    OfflineSettings settings;
    settings.endSeconds = .1;
    settings.threads = 1;
    REQUIRE((*missing)->renderSession((*missing)->head(), settings));
    auto unfreeze = bus.execute({"track.unfreeze", *json::parse(R"({"track":"tr_1"})"), origin});
    REQUIRE_FALSE(unfreeze);
    CHECK(unfreeze.error().code == "freeze.restore");
    REQUIRE((*missing)->head().tracks.find(track)->freeze);
}

TEST_CASE("freeze: branches switch between independently frozen performances") {
    Fixture f;
    f.run("track.freeze", R"({"track":"tr_1"})");
    const auto original = f.app->head().tracks.find(track)->freeze->source;
    f.run("branch.create", R"({"name":"original","switch":false})");
    f.run("track.unfreeze", R"({"track":"tr_1"})");
    f.run("item.set", R"({"item":"it_1","gain_db":-6})");
    f.run("track.freeze", R"({"track":"tr_1"})");
    const auto revised = f.app->head().tracks.find(track)->freeze->source;
    REQUIRE(original != revised);
    f.run("branch.switch", R"({"name":"original"})");
    CHECK(f.app->head().tracks.find(track)->freeze->source == original);
    f.run("branch.switch", R"({"name":"main"})");
    CHECK(f.app->head().tracks.find(track)->freeze->source == revised);
}

TEST_CASE("freeze: Save As excludes a completed but unpublished artifact") {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    AppOptions options;
    options.jobFinishedHook = [&] {
        std::lock_guard lock(mutex);
        finished = true;
        cv.notify_all();
    };
    Fixture f(options);
    REQUIRE(f.app->save(f.dir / "original" / "song.project"));
    const auto originalMedia = f.app->mediaDir();
    const auto job = f.run("track.freeze", R"({"track":"tr_1","job":true})");
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return finished; });
    }
    REQUIRE(std::filesystem::exists(originalMedia / ".freeze-pending"));
    REQUIRE(f.app->save(f.dir / "copy" / "song.project"));
    f.bus->flushJobEvents();
    CHECK_FALSE(f.app->head().tracks.find(track)->freeze);
    const auto state = f.bus->jobs().status(job.find("job")->asString());
    REQUIRE(state);
    CHECK(state->find("status")->asString() == "failed");
    for (const auto& directory : {originalMedia, f.app->mediaDir()}) {
        if (!std::filesystem::exists(directory)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            CHECK(entry.path().filename() != ".freeze-pending");
            CHECK_FALSE(entry.path().filename().string().starts_with("freeze-"));
        }
    }
}

TEST_CASE("MIDI sends: frozen channels keep sending timeline MIDI at every tap" *
          doctest::description("rt")) {
    Fixture f;
    f.run("track.add", R"({"name":"MIDI receiver"})");
    f.run("fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.sine"})");
    f.run("item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:q r:*"})");
    f.run("route.add", R"({"from":"tr_1","to":"tr_2","midi":{}})");
    OfflineSettings settings;
    settings.track = model::Id{model::Kind::Track, 2};
    settings.endSeconds = .5;
    settings.threads = 1;
    const auto reference = f.app->renderSession(f.app->head(), settings);
    REQUIRE(reference);
    f.run("track.freeze", R"({"track":"tr_1"})");
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    const auto hash = model::freezeDependencyHash(f.app->head(), track);
    for (const char* tap : {"pre_fx", "pre_fader", "post_fader"}) {
        f.run("route.set", std::string(R"({"route":"sd_1","tap":")") + tap + R"("})");
        const auto frozen = f.app->renderSession(f.app->head(), settings);
        REQUIRE(frozen);
        CHECK(bool(frozen->audio.channels == reference->audio.channels));
        CHECK(model::freezeDependencyHash(f.app->head(), track) == hash);
    }
}

TEST_CASE("MIDI bus: a frozen channel's post-FX sends carry its timeline MIDI" *
          doctest::description("rt")) {
    // Its echo sends a fifth up while it plays; frozen, the chain no longer
    // runs, and the freeze says so (ADR-0068).
    Fixture f;
    f.run("track.add", R"({"name":"MIDI receiver"})");
    f.run("fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.sine"})");
    f.run("item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:q r:*"})");
    f.run("route.add", R"({"from":"tr_1","to":"tr_2","midi":{}})");
    OfflineSettings settings;
    settings.track = model::Id{model::Kind::Track, 2};
    settings.endSeconds = .5;
    settings.threads = 1;
    const auto timeline = f.app->renderSession(f.app->head(), settings);
    REQUIRE(timeline);
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.note-echo"})");
    const auto echoed = f.app->renderSession(f.app->head(), settings);
    REQUIRE(echoed);
    CHECK_FALSE(bool(echoed->audio.channels == timeline->audio.channels));
    // midi_output is part of the sound a freeze captures.
    const auto replace = model::freezeDependencyHash(f.app->head(), track);
    f.run("fx.set", R"({"fx":"fx_2","midi_output":"merge"})");
    CHECK(model::freezeDependencyHash(f.app->head(), track) != replace);
    f.run("fx.set", R"({"fx":"fx_2","midi_output":"replace"})");
    CHECK(model::freezeDependencyHash(f.app->head(), track) == replace);
    const auto frozen = f.run("track.freeze", R"({"track":"tr_1"})");
    REQUIRE(frozen.find("warnings"));
    CHECK(frozen.find("warnings")->at(0).find("code")->asString() == "freeze.midi_output");
    REQUIRE(f.app->head().tracks.find(track)->freeze);
    const auto played = f.app->renderSession(f.app->head(), settings);
    REQUIRE(played);
    CHECK(bool(played->audio.channels == timeline->audio.channels));
    CHECK(f.call("fx.set", R"({"fx":"fx_2","midi_output":"merge"})").error().code ==
          "track.frozen");
}
