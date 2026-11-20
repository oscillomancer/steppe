// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include "source_preparation.h"
#include "stretch.h"
#include "supple/engine/app.h"
#include "supple/model/audio_warp.h"
#include "supple/model/schemas.h"
#include "transients.h"
using namespace supple;
using namespace supple::engine;
namespace {
struct WarpHost {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("supple-warp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    WarpHost(std::function<void()> finished = {}, std::function<void()> started = {}) {
        std::filesystem::create_directories(root / "plugins");
        AppOptions options;
        options.workdir = root;
        options.jobFinishedHook = std::move(finished);
        options.jobStartedHook = std::move(started);
        options.pluginPath = {root / "plugins"};
        auto created = EngineApp::create(options);
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->setupFixture(*bus, "song_with_audio");
    }
    ~WarpHost() {
        bus.reset();
        app.reset();
        std::filesystem::remove_all(root);
    }
    Result<json::Value> call(const char* name, std::string_view args = "{}") {
        return bus->execute({name, *json::parse(args), {cmd::Origin::Type::Test, "warp", {}}});
    }
    json::Value run(const char* name, std::string_view args = "{}") {
        auto r = call(name, args);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
};
}  // namespace
TEST_CASE("zero-crossing split uses the heard source offset and falls back on DC") {
    WarpHost h;
    bool dc = false;
    double rate = 1;
    SUBCASE("latest crossing in an offset sine") {}
    SUBCASE("DC keeps the cursor") { dc = true; }
    SUBCASE("double-speed source") { rate = 2; }
    audio::AudioData signal;
    signal.sampleRate = 48000;
    signal.channels.assign(1, std::vector<float>(96000));
    for (std::size_t n = 0; n < signal.frames(); ++n)
        signal.channels[0][n] = dc ? .25f
                                   : static_cast<float>(.5 * std::sin(2 * 3.141592653589793 * 100 *
                                                                      (double(n) + .25) / 48000));
    const auto path = h.root / "split.wav";
    REQUIRE(audio::writeWav(path, signal));
    auto args = json::Value::object();
    args["track"] = "tr_1";
    args["position"] = 2;
    args["audio_file"] = path.string();
    REQUIRE(h.bus->execute({"item.add", args, {cmd::Origin::Type::Test, "split", {}}}));
    h.run("take.set",
          rate == 1
              ? R"({"take":"tk_2","start_offset":0.013})"
              : R"({"take":"tk_2","start_offset":0.013,"playrate":2,"preserve_pitch":false})");
    REQUIRE(h.app->media().prepareSources(h.app->head(), h.app->projectDir(), 48000));
    const auto before = model::serializeProject(h.app->head());
    const auto result = h.run("item.split", R"({"item":"it_2","at":2.376,"snap":"zero_crossing"})");
    const auto rightId = *model::Id::parse(result.find("right")->asString());
    const auto& right = *h.app->head().items.find(rightId);
    const double expected = dc ? 1.188 : (rate == 1 ? 1.187 : 1.186);
    CHECK(model::itemSpan(h.app->head(), right).startSec ==
          doctest::Approx(expected).epsilon(1e-9));
    CHECK(h.app->head().takes.find(*right.activeTake)->startOffset ==
          doctest::Approx(.013 + (expected - 1) * rate));
    REQUIRE(h.app->undo());
    auto restored = h.app->head();
    restored.next = model::parseProject(before)->next;
    CHECK(model::serializeProject(restored) == before);
}

TEST_CASE("zero-crossing split refuses MIDI with a corrective hint") {
    WarpHost h;
    h.run("item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const auto before = model::serializeProject(h.app->head());
    auto result = h.call("item.split", R"({"item":"it_2","at":1,"snap":"zero_crossing"})");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "item.split_not_audio");
    CHECK(result.error().path == "/args/item");
    CHECK(result.error().hint.find("snap none") != std::string::npos);
    CHECK(model::serializeProject(h.app->head()) == before);
}

TEST_CASE("audio warp commands quantize split and undo atomically") {
    WarpHost h;
    h.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
    const auto before = model::serializeProject(h.app->head());
    h.run("audio.quantize", R"({"items":["it_1"],"grid":0.25})");
    const auto* take = h.app->head().takes.find({model::Kind::Take, 1});
    REQUIRE(take);
    REQUIRE(take->stretchMarkers.size() == 4);
    CHECK(take->stretchMarkers[1].sourcePos == .3);
    CHECK(take->stretchMarkers[1].itemPos == .25);
    CHECK(take->stretchMarkers[2].itemPos == .75);
    const auto after = model::serializeProject(h.app->head());
    REQUIRE(model::parseProject(after));
    REQUIRE(h.app->undo());
    CHECK(model::serializeProject(h.app->head()) == before);
    REQUIRE(h.app->redo());
    CHECK(model::serializeProject(h.app->head()) == after);
    auto bad = h.call("take.stretch.move", R"({"take":"tk_1","index":1,"item_pos":0.9})");
    CHECK_FALSE(bad);
    CHECK(model::serializeProject(h.app->head()) == after);
    CHECK_FALSE(h.call("take.set", R"({"take":"tk_1","playrate":100})"));
    CHECK(model::serializeProject(h.app->head()) == after);
    h.run("audio.warp.reset", R"({"items":["it_1"]})");
    h.run("audio.quantize", R"({"items":["it_1"],"grid":0.25,"mode":"slice","strength":0})");
    CHECK(h.app->head().items.size() == 3);
    REQUIRE(h.app->undo());
    CHECK(h.app->head().items.size() == 1);
}
TEST_CASE("stretch marker insertion keeps source timing and does not duplicate an anchor") {
    WarpHost h;
    h.run("take.set", R"({"take":"tk_1","start_offset":0.125})");
    auto resolver = [](const model::PluginRef&) -> Result<host::PluginInfo> {
        return Error("test.plugin", "no plugin");
    };
    InstancePool pool(resolver);
    const OfflineSettings settings{
        .startSeconds = 0, .endSeconds = 1, .projectDir = h.app->projectDir()};
    auto before = renderOffline(h.app->head(), pool, settings);
    REQUIRE(before);
    h.run("take.stretch.add", R"({"take":"tk_1","item_pos":0.375})");
    const model::Id take{model::Kind::Take, 1};
    const auto& markers = h.app->head().takes.find(take)->stretchMarkers;
    REQUIRE(markers.size() == 3);
    CHECK(markers[1].sourcePos == .5);
    auto after = renderOffline(h.app->head(), pool, settings);
    REQUIRE(after);
    CHECK(bool(before->audio.channels == after->audio.channels));
    REQUIRE(h.app->undo());
    CHECK(h.app->head().takes.find(take)->stretchMarkers.empty());
    REQUIRE(h.app->redo());
    h.run("take.stretch.add", R"({"take":"tk_1","item_pos":0.375})");
    CHECK(h.app->head().takes.find(take)->stretchMarkers.size() == 3);
    h.run("take.stretch.move", R"({"take":"tk_1","index":1,"item_pos":0.5})");
    h.run("take.stretch.add", R"({"take":"tk_1","item_pos":0.25})");
    CHECK(h.app->head().takes.find(take)->stretchMarkers[1].sourcePos == .3125);
}

TEST_CASE("short variable warp is deterministic and preserves linked channel polarity") {
    audio::AudioData input;
    input.sampleRate = 48000;
    input.channels.assign(3, std::vector<float>(24000));
    for (std::size_t i = 0; i < input.frames(); ++i) {
        input.channels[0][i] = static_cast<float>(.3 * std::sin(double(i) * .07));
        input.channels[1][i] = -input.channels[0][i];
        input.channels[2][i] = static_cast<float>(.3 * std::sin((double(i) - 17) * .07));
    }
    model::Take take;
    take.stretchMarkers = {{0, 0, 0}, {.1, .12, 0}, {.13, .14, 0}, {.5, .5, 0}};
    auto a = warpAudio(input, take, 24000), b = warpAudio(input, take, 24000);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(bool(a->channels == b->channels));
    double peak = 0, residual = 0;
    for (std::size_t i = 0; i < a->frames(); ++i) {
        peak = std::max(peak, std::abs(double(a->channels[0][i])));
        residual =
            std::max(residual, std::abs(double(a->channels[0][i]) + double(a->channels[1][i])));
    }
    CHECK(peak > .1);
    CHECK(residual < 1e-5);
    // A delayed microphone is not a duplicate: retain its relative phase too.
    for (int begin : {2400, 16000}) {
        double real = 0, imaginary = 0;
        for (int n = begin; n < begin + 4000; ++n) {
            const auto at = static_cast<std::size_t>(n);
            real += double(a->channels[0][at]) * double(a->channels[2][at]);
            imaginary += double(a->channels[0][at]) * double(a->channels[2][at + 22]);
        }
        // Quarter-cycle quadrature, allowing its 0.031 rad sample rounding.
        CHECK(std::atan2(imaginary, real) == doctest::Approx(17 * .07).epsilon(.04));
    }
    take.stretchMarkers = {{0, 0, 0}, {.5, .5, 0}};
    auto identity = warpAudio(input, take, 24000);
    REQUIRE(identity);
    CHECK(bool(identity->channels == input.channels));
}
TEST_CASE("transient detection handles silence antiphase drums and repeatability") {
    audio::AudioData input;
    input.sampleRate = 48000;
    input.channels.assign(2, std::vector<float>(48000));
    auto silence = detectTransients(input, .5);
    REQUIRE(silence);
    CHECK(silence->empty());
    for (int at : {4800, 19200, 33600})
        for (int n = 0; n < 4800; ++n) {
            const auto i = static_cast<std::size_t>(at + n);
            const float value =
                static_cast<float>(.8 * std::exp(-double(n) / 500) * std::cos(double(n) * .08));
            input.channels[0][i] = value;
            input.channels[1][i] = -value;
        }
    auto first = detectTransients(input, .5), second = detectTransients(input, .5);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*first == *second);
    REQUIRE(first->size() == 3);
    for (std::size_t i = 0; i < 3; ++i) CHECK(std::abs((*first)[i] - (.1 + double(i) * .3)) < .006);
}
TEST_CASE("warp prepared source snapshots retain old buffers and both executors equal offline") {
    WarpHost h;
    h.run("take.stretch.add", R"({"take":"tk_1","source_pos":0.6,"item_pos":0.5})");
    const auto session = h.app->head();
    auto resolver = [](const model::PluginRef&) -> Result<host::PluginInfo> {
        return Error("test.plugin", "no plugin");
    };
    InstancePool offlinePool(resolver);
    auto rendered =
        renderOffline(session, offlinePool,
                      {.startSeconds = 0, .endSeconds = 1, .projectDir = h.app->projectDir()});
    REQUIRE_MESSAGE(rendered, (rendered ? "" : rendered.error().toString()));
    REQUIRE(rendered->warnings.empty());
    for (bool ahead : {false, true}) {
        InstancePool pool(resolver);
        MediaCache cache;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 49152});
        Engine engine(pool, cache, {.anticipative = ahead, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(session, h.app->projectDir()));
        const auto sources = cache.sources();
        REQUIRE(sources);
        engine.play();
        for (int n = 0; n < 384; ++n) {
            if (ahead) REQUIRE(engine.awaitRendered((n + 1) * 128));
            backend.run(1);
        }
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(engine.anticipationMisses() == 0);
        for (std::size_t c = 0; c < 2; ++c)
            CHECK(std::equal(rendered->audio.channels[c].begin(), rendered->audio.channels[c].end(),
                             backend.capture().channels[c].begin() + engine.outputLatency()));
        CHECK(sources->takes.at({model::Kind::Take, 1}).itemLocal);
    }
}
TEST_CASE("background source preparation returns complete immutable data") {
    WarpHost h;
    h.run("take.stretch.add", R"({"take":"tk_1","source_pos":0.6,"item_pos":0.5})");
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    SourcePreparation service([&] {
        {
            std::lock_guard lock(mutex);
            ready = true;
        }
        changed.notify_one();
    });
    service.request(h.app->head(), h.app->projectDir(), 48000);
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return ready; });
    }
    auto result = service.take();
    REQUIRE(result);
    REQUIRE(*result);
    REQUIRE((**result)->takes.contains({model::Kind::Take, 1}));
    CHECK((**result)->takes.at({model::Kind::Take, 1}).data->frames() == 96000);
}

TEST_CASE("audio bundles retain shared preparation through stem selection and copy remapping") {
    WarpHost h;
    h.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
    h.run("item.duplicate", R"({"item":"it_1","to":0})");
    h.run("audio.quantize", R"({"items":["it_1","it_2"],"grid":0.25})");
    const auto s = h.app->head();
    const auto& t = *s.takes.find({model::Kind::Take, 1});
    REQUIRE(t.warpPeers.size() == 2);
    MediaCache cache;
    auto prepared = cache.prepareSources(s, h.app->projectDir(), 48000);
    REQUIRE(prepared);
    CHECK(bool((*prepared)->takes.at({model::Kind::Take, 1}).data->channels ==
               (*prepared)->takes.at({model::Kind::Take, 2}).data->channels));
    h.run("take.stretch.move", R"({"take":"tk_1","index":1,"item_pos":0.26})");
    CHECK(h.app->head().takes.find({model::Kind::Take, 2})->stretchMarkers[1].itemPos == .26);
    const auto before = model::serializeProject(h.app->head());
    CHECK_FALSE(h.call("item.move", R"({"item":"it_1","position":1})"));
    CHECK(model::serializeProject(h.app->head()) == before);
    h.run(
        "item.paste",
        R"({"items":[{"item":"it_1","track":"tr_1","position":4},{"item":"it_2","track":"tr_1","position":4}]})");
    REQUIRE(h.app->head().takes.size() == 4);
    const auto& copy = *h.app->head().takes.find({model::Kind::Take, 3});
    REQUIRE(copy.warpPeers.size() == 2);
    CHECK(copy.warpPeers[0] == model::Id{model::Kind::Take, 3});
    CHECK(copy.warpPeers[1] == model::Id{model::Kind::Take, 4});
    h.run("audio.warp.detach", R"({"items":["it_1"]})");
    CHECK(h.app->head().takes.find({model::Kind::Take, 1})->warpPeers.empty());
    CHECK(h.app->head().takes.find({model::Kind::Take, 2})->warpPeers.empty());
}
TEST_CASE("audio warp split keeps the source map and source offset aligned") {
    WarpHost h;
    h.run("take.stretch.add", R"({"take":"tk_1","source_pos":0.6,"item_pos":0.5})");
    const auto old = *h.app->head().takes.find({model::Kind::Take, 1});
    h.run("item.split", R"({"item":"it_1","at":0.75})");
    const auto& right = *h.app->head().takes.find({model::Kind::Take, 2});
    for (double x : {0., .05, .2, 1.})
        CHECK(model::audioSourceAt(right, x) ==
              doctest::Approx(model::audioSourceAt(old, x + .375)));
    h.run("take.set", R"({"take":"tk_2","start_offset":0.9})");
    CHECK(model::audioSourceAt(*h.app->head().takes.find({model::Kind::Take, 2}), 0) ==
          doctest::Approx(.9));
}
TEST_CASE("audio quantization swings beat targets and preserves failed edits") {
    WarpHost h;
    h.run("take.hits.set", R"({"take":"tk_1","positions":[0.15,0.28]})");
    h.run("audio.quantize", R"({"items":["it_1"],"grid":0.25,"swing":1,"strength":0.5})");
    const auto& t = *h.app->head().takes.find({model::Kind::Take, 1});
    CHECK(t.stretchMarkers[1].itemPos == doctest::Approx((.15 + .1875) / 2));
    const auto before = model::serializeProject(h.app->head());
    CHECK_FALSE(h.call("audio.quantize", R"({"items":["it_1"],"grid":1})"));
    CHECK(model::serializeProject(h.app->head()) == before);
}

TEST_CASE("hit detection job completes only after its one result edit commits") {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, released = false, finished = false;
    WarpHost h(
        [&] {
            std::lock_guard lock(mutex);
            finished = true;
            wake.notify_all();
        },
        [&] {
            std::unique_lock lock(mutex);
            entered = true;
            wake.notify_all();
            wake.wait(lock, [&] { return released; });
        });
    // Do not strand the worker if a REQUIRE unwinds before release.
    struct Release {
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& released;
        ~Release() {
            std::lock_guard lock(mutex);
            released = true;
            wake.notify_all();
        }
    } release{mutex, wake, released};
    const auto version = h.app->head().version;
    const auto original = model::serializeProject(h.app->head());
    auto job = h.run("take.hits.detect", R"({"takes":["tk_1"]})");
    {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return entered; });
    }
    // execute() flushes posted work before returning. Hold the worker until
    // that flush has passed, instead of assuming analysis will take longer.
    CHECK_FALSE(h.app->head().takes.find({model::Kind::Take, 1})->transients);
    CHECK(h.app->head().version == version);
    {
        std::unique_lock lock(mutex);
        released = true;
        wake.notify_all();
        wake.wait(lock, [&] { return finished; });
    }
    // Analysis has finished and posted its result, but model publication and
    // job completion must still wait for the one result edit to commit.
    CHECK_FALSE(h.app->head().takes.find({model::Kind::Take, 1})->transients);
    CHECK(h.app->head().version == version);
    const auto id = job.find("job")->asString();
    auto pending = h.bus->jobs().status(id);  // read without another automatic flush
    REQUIRE(pending);
    CHECK(pending->find("status")->asString() == "running");
    h.bus->flushJobEvents();
    REQUIRE(h.app->head().takes.find({model::Kind::Take, 1})->transients);
    auto status = h.bus->execute({"job.get", job, {cmd::Origin::Type::Test, "warp", {}}});
    REQUIRE(status);
    CHECK(status->find("status")->asString() == "done");
    const auto* committed = h.app->versions().version(h.app->head().version);
    REQUIRE(committed);
    CHECK(committed->parents == std::vector<std::uint64_t>{version});
    REQUIRE(h.app->undo());
    CHECK_FALSE(h.app->head().takes.find({model::Kind::Take, 1})->transients);
    CHECK(model::serializeProject(h.app->head()) == original);
}

TEST_CASE("audio hit detection finds bass and plucked harmonic attacks") {
    for (double fundamental : {55., 196.}) {
        INFO("fundamental " << fundamental);
        audio::AudioData input;
        input.sampleRate = 48000;
        input.channels.assign(1, std::vector<float>(72000));
        for (int at : {4800, 24000, 43200})
            for (int n = 0; n < 16000; ++n) {
                const double time = double(n) / 48000;
                double note = 0;
                for (int harmonic = 1; harmonic <= 6; ++harmonic)
                    note +=
                        std::sin(2 * 3.141592653589793 * fundamental * harmonic * time) / harmonic;
                input.channels[0][static_cast<std::size_t>(at + n)] += static_cast<float>(
                    .3 * std::min(1., time / .003) * std::exp(-time * 12) * note);
            }
        auto hits = detectTransients(input, .5);
        REQUIRE(hits);
        REQUIRE(hits->size() == 3);
        for (std::size_t i = 0; i < 3; ++i)
            CHECK(std::abs((*hits)[i] - (.1 + double(i) * .4)) < .012);
    }
}
TEST_CASE("variable audio warp retains pitch and places drum attacks near anchors") {
    audio::AudioData input;
    input.sampleRate = 48000;
    input.channels.assign(1, std::vector<float>(48000));
    for (std::size_t i = 0; i < input.frames(); ++i)
        input.channels[0][i] =
            static_cast<float>(.3 * std::sin(2 * 3.141592653589793 * 440 * double(i) / 48000));
    model::Take take;
    take.stretchMarkers = {{0, 0, 0}, {.5, .35, 0}, {1, 1, 0}};
    auto tone = warpAudio(input, take, 48000);
    REQUIRE(tone);
    for (const auto range : {std::pair{4800, 12000}, std::pair{24000, 36000}}) {
        std::vector<std::size_t> crossings;
        for (int i = range.first; i < range.second; ++i)
            if (tone->channels[0][static_cast<std::size_t>(i)] <= 0 &&
                tone->channels[0][static_cast<std::size_t>(i + 1)] > 0)
                crossings.push_back(static_cast<std::size_t>(i));
        REQUIRE(crossings.size() > 20);
        const double frequency =
            48000 * double(crossings.size() - 1) / double(crossings.back() - crossings.front());
        CHECK(frequency == doctest::Approx(440).epsilon(.01));
    }
    std::fill(input.channels[0].begin(), input.channels[0].end(), 0.f);
    for (int at : {12000, 24000, 36000})
        for (int n = 0; n < 2400; ++n)
            input.channels[0][static_cast<std::size_t>(at + n)] =
                static_cast<float>(.8 * std::exp(-double(n) / 250) * std::cos(double(n) * .08));
    take.stretchMarkers = {{0, 0, 0}, {.25, .2, 0}, {.5, .55, 0}, {.75, .8, 0}, {1, 1, 0}};
    auto drums = warpAudio(input, take, 48000);
    REQUIRE(drums);
    auto hits = detectTransients(*drums, .5);
    REQUIRE(hits);
    REQUIRE(hits->size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(std::abs((*hits)[i] - take.stretchMarkers[i + 1].itemPos) < .02);
}

TEST_CASE("pending hit detection does not overwrite newer manual candidates") {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, released = false, finished = false;
    WarpHost h(
        [&] {
            std::lock_guard lock(mutex);
            finished = true;
            wake.notify_all();
        },
        [&] {
            std::unique_lock lock(mutex);
            entered = true;
            wake.notify_all();
            wake.wait(lock, [&] { return released; });
        });
    // Do not strand the worker if a REQUIRE unwinds before release.
    struct Release {
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& released;
        ~Release() {
            std::lock_guard lock(mutex);
            released = true;
            wake.notify_all();
        }
    } release{mutex, wake, released};
    auto job = h.run("take.hits.detect", R"({"takes":["tk_1"]})");
    // execute() flushes posted work before returning: a worker that finished
    // by then had its result applied before the manual edit below (seen on
    // macOS CI). Hold it until that flush has passed, as the test above does.
    {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return entered; });
        released = true;
        wake.notify_all();
        wake.wait(lock, [&] { return finished; });
    }
    REQUIRE_FALSE(h.app->head().takes.find({model::Kind::Take, 1})->transients);
    // An explicit edit before draining the posted worker result wins.
    auto transaction =
        h.app->beginTransaction({cmd::Origin::Type::Test, "warp", {}}, "Manual hits");
    REQUIRE(transaction);
    h.app->txn().modify<model::Take>({model::Kind::Take, 1}, [](auto& t) {
        t.transients = model::Transients{"manual", "", .5, {.25}};
    });
    REQUIRE(h.app->commitTransaction(*transaction));
    h.bus->flushJobEvents();
    CHECK(h.app->head().takes.find({model::Kind::Take, 1})->transients->positions ==
          std::vector<double>{.25});
    auto status = h.bus->execute({"job.get", job, {cmd::Origin::Type::Test, "warp", {}}});
    REQUIRE(status);
    CHECK(status->find("status")->asString() == "failed");
}

TEST_CASE("warped sources render identically through a dedicated bridge") {
    WarpHost h;
    h.run("take.stretch.add", R"({"take":"tk_1","source_pos":0.6,"item_pos":0.5})");
    host::setBridgeExecutable(BRIDGE_PATH);
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    fx.isolation = model::Isolation::Dedicated;
    const auto id = edit.create(fx);
    edit.modify<model::Track>({model::Kind::Track, 1}, [&](auto& tr) { tr.chain.push_back(id); });
    auto session = edit.commit();
    REQUIRE(session);
    auto resolver = [](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
        host::ScanOptions options;
        options.forceInProcess = true;
        auto plugins = host::scan(std::span(&fixture, 1), options);
        if (!plugins) return plugins.error();
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("engine.plugin_missing", "gain fixture missing");
    };
    InstancePool offlinePool(resolver);
    auto offline =
        renderOffline(*session, offlinePool,
                      {.startSeconds = 0, .endSeconds = .5, .projectDir = h.app->projectDir()});
    REQUIRE(offline);
    REQUIRE(offline->warnings.empty());
    InstancePool pool(resolver);
    MediaCache cache;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 32768});
    Engine engine(pool, cache, {.anticipative = true, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(*session, h.app->projectDir()));
    engine.play();
    for (int n = 0; n < 256; ++n) {
        REQUIRE(engine.awaitRendered((n + 1) * 128));
        backend.run(1);
    }
    engine.stop();
    CHECK(engine.processErrors() == 0);
    for (std::size_t c = 0; c < 2; ++c)
        CHECK(std::equal(offline->audio.channels[c].begin(), offline->audio.channels[c].end(),
                         backend.capture().channels[c].begin() + engine.outputLatency()));
}

TEST_CASE("slice at closely spaced hits keeps complementary fades and the original signal") {
    WarpHost h;
    h.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.305,0.8]})");
    auto resolver = [](const model::PluginRef&) -> Result<host::PluginInfo> {
        return Error("test.plugin", "no plugin");
    };
    InstancePool beforePool(resolver), afterPool(resolver);
    auto before =
        renderOffline(h.app->head(), beforePool,
                      {.startSeconds = 0, .endSeconds = 1, .projectDir = h.app->projectDir()});
    REQUIRE(before);
    h.run("audio.quantize", R"({"items":["it_1"],"grid":0.25,"mode":"slice","strength":0})");
    auto after =
        renderOffline(h.app->head(), afterPool,
                      {.startSeconds = 0, .endSeconds = 1, .projectDir = h.app->projectDir()});
    REQUIRE(after);
    double residual = 0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t n = 0; n < before->audio.frames(); ++n)
            residual = std::max(residual, std::abs(double(before->audio.channels[c][n]) -
                                                   double(after->audio.channels[c][n])));
    CHECK(residual < 1e-5);
}

TEST_CASE("a command that runs another command is recorded with its own arguments") {
    // audio.quantize in slice mode runs item.split, which resolves its arguments: history must
    // keep what the caller sent (a branch merge replays it), not the inner command's.
    WarpHost h;
    h.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
    const model::Session base = h.app->head();
    h.run("audio.quantize", R"({"items":["it_1"],"grid":0.25,"mode":"slice","strength":0})");
    REQUIRE(h.app->head().items.size() > base.items.size());  // it split
    const auto* version = h.app->versions().version(h.app->head().version);
    REQUIRE(version);
    REQUIRE(version->txn.invocations.size() == 1);
    const auto invocations = version->txn.invocations;
    CHECK(invocations[0].command == "audio.quantize");
    CHECK(invocations[0].args.find("items"));
    CHECK(invocations[0].args.find("grid"));
    CHECK_FALSE(invocations[0].args.find("at"));  // item.split's
    const auto replayed = h.app->replay(h.registry, base, invocations, h.app->head().version + 1);
    REQUIRE_MESSAGE(replayed, (replayed ? "" : replayed.error().toString()));
    CHECK(replayed->session.items.size() == h.app->head().items.size());
    for (const auto& entry : h.app->head().items) {
        const auto* other = replayed->session.items.find(entry.id);
        REQUIRE(other);
        CHECK(model::itemSpan(replayed->session, *other).startSec ==
              doctest::Approx(model::itemSpan(h.app->head(), *entry.value).startSec));
    }
}

TEST_CASE("a warped stem retains its bundle inputs and ignores unrelated missing media") {
    WarpHost h;
    h.run("track.add", R"({"name":"Other microphone"})");
    h.run("item.duplicate", R"({"item":"it_1","to":0})");
    h.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
    h.run("audio.quantize", R"({"items":["it_1","it_2"],"grid":0.25})");
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    edit.modify<model::Item>({model::Kind::Item, 2},
                             [](auto& it) { it.track = {model::Kind::Track, 2}; });
    model::Track missingTrack;
    missingTrack.parent = model::kMaster;
    const auto track = edit.create(missingTrack);
    edit.modify<model::Track>(model::kMaster, [&](auto& tr) { tr.children.push_back(track); });
    const auto source = edit.create(model::MediaSource{"missing.wav", "", "wav", 48000, 2, 48000});
    model::Take take;
    take.source = source;
    take.stretchMarkers = {{0, 0, 0}, {1, 1, 0}};
    const auto tk = edit.create(take);
    model::Item item;
    item.track = track;
    item.length = 2;
    item.takes = {tk};
    item.activeTake = tk;
    edit.create(item);
    auto s = edit.commit();
    REQUIRE(s);
    auto resolver = [](const model::PluginRef&) -> Result<host::PluginInfo> {
        return Error("test.plugin", "no plugin");
    };
    InstancePool pool(resolver);
    MediaCache cache;
    OfflineSettings settings;
    settings.endSeconds = 1;
    settings.projectDir = h.app->projectDir();
    settings.track = model::Id{model::Kind::Track, 1};
    settings.media = &cache;
    auto stem = renderOffline(*s, pool, settings);
    REQUIRE_MESSAGE(stem, (stem ? "" : stem.error().toString()));
    CHECK(stem->warnings.empty());
    REQUIRE(cache.sources());
    CHECK(cache.sources()->takes.contains({model::Kind::Take, 2}));
    CHECK_FALSE(cache.sources()->takes.contains(tk));
}

TEST_CASE("source budgets count shared scalar media once across repeated clips") {
    WarpHost h;
    const auto original = h.app->head();
    model::EditTxn edit(original, original.version + 1);
    const auto take = *original.takes.find({model::Kind::Take, 1});
    auto item = *original.items.find({model::Kind::Item, 1});
    model::Id last{};
    for (int i = 0; i < 800; ++i) {
        last = edit.create(take);
        item.takes = {last};
        item.activeTake = last;
        item.position = double(i + 1) * 4;
        edit.create(item);
    }
    auto session = edit.commit();
    REQUIRE(session);
    MediaCache cache;
    auto sources = cache.prepareSources(*session, h.app->projectDir(), 48000);
    REQUIRE_MESSAGE(sources, (sources ? "" : sources.error().toString()));
    CHECK((*sources)->takes.size() == 801);
    CHECK((*sources)->takes.at(last).data == (*sources)->takes.at({model::Kind::Take, 1}).data);
}

TEST_CASE("warped buffers cover separately rounded fractional item boundaries") {
    WarpHost h;
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    constexpr double duration = 10.1 / 48000;
    edit.modify<model::Item>({model::Kind::Item, 1}, [](auto& item) {
        item.timebase = model::ItemTimebase::Time;
        item.position = .49 / 48000;
        item.length = duration;
    });
    edit.modify<model::Take>({model::Kind::Take, 1}, [](auto& take) {
        take.stretchMarkers = {{0, 0, 0}, {duration, duration, 0}};
    });
    auto s = edit.commit();
    REQUIRE(s);
    MediaCache cache;
    auto sources = cache.prepareSources(*s, h.app->projectDir(), 48000);
    REQUIRE(sources);
    const auto& data = (*sources)->takes.at({model::Kind::Take, 1}).data;
    REQUIRE(data);
    REQUIRE(data->frames() == 11);
    CHECK(std::abs(data->channels[0][10]) > .1f);
}
