// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "plan.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/realtime.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {
InstancePool fixturePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path path = FIXTURE_CLAP_PATH;
        auto plugins = host::scan(std::span<const std::filesystem::path>(&path, 1));
        if (!plugins) return std::move(plugins).error();
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("test.plugin", ref.id);
    });
}

Id track(model::EditTxn& edit, model::StripType type = model::StripType::Channel) {
    model::Track value;
    value.parent = kMaster;
    value.type = type;
    const Id id = edit.create(value);
    edit.modify<model::Track>(kMaster, [&](auto& master) { master.children.push_back(id); });
    return id;
}

Id delay(model::EditTxn& edit, Id owner, unsigned frames) {
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.edge";
    fx.state = "sha256:pdc-delay-" + std::to_string(frames);
    edit.putBlob(*fx.state,
                 std::make_shared<const model::Blob>(model::Blob{
                     {49, static_cast<std::uint8_t>(frames), static_cast<std::uint8_t>(frames >> 8),
                      static_cast<std::uint8_t>(frames >> 16)}}));
    const Id id = edit.create(fx);
    edit.modify<model::Track>(owner, [&](auto& value) { value.chain.push_back(id); });
    return id;
}

void monitor(model::EditTxn& edit, Id owner) {
    model::Route input;
    input.from.hwChannels = std::vector<int>{1, 2};
    input.to.track = owner;
    const Id route = edit.create(input);
    edit.modify<model::Track>(owner, [&](auto& value) {
        value.record.input = route;
        value.record.armed = true;
        value.record.monitor = model::Monitor::On;
    });
}

model::Session commit(model::EditTxn& edit) {
    auto result = edit.commit();
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    return *result;
}

DummyBackend::Options impulse(std::int64_t at) {
    return {.mode = DummyBackend::Mode::Manual,
            .captureFrames = 4096,
            .input = [at](float* const* in, std::uint32_t channels, std::uint32_t frames,
                          std::int64_t first) {
                for (std::uint32_t c = 0; c < channels; ++c)
                    for (std::uint32_t i = 0; i < frames; ++i)
                        in[c][i] = first + i == at ? .25f : 0.f;
            }};
}

void run(Engine& engine, DummyBackend& backend, bool anticipative, int cycles) {
    for (int i = 0; i < cycles; ++i) {
        if (anticipative) REQUIRE(engine.awaitRendered((backend.cycles() + 1) * 128));
        backend.run(1);
    }
}

struct AudioFile {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("supple-pdc-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "source.wav";
    explicit AudioFile(bool pulses = false) {
        std::filesystem::create_directories(directory);
        audio::AudioData data;
        data.sampleRate = 48000;
        data.channels.assign(2, std::vector<float>(16384));
        for (std::size_t i = 0; i < data.frames(); ++i)
            data.channels[0][i] = data.channels[1][i] =
                pulses ? (i == 479 || i == 480 ? .25f : 0.f)
                       : static_cast<float>(std::sin(double(i) * .07) * .1);
        REQUIRE(audio::writeWav(path, data));
    }
    ~AudioFile() { std::filesystem::remove_all(directory); }
    void add(model::EditTxn& edit, Id owner) const {
        const Id media = edit.create(model::MediaSource{path.string(), "", "wav", 48000, 2, 16384});
        model::Take take;
        take.source = media;
        const Id taken = edit.create(take);
        model::Item item;
        item.track = owner;
        item.length = 16384. / 24000.;
        item.takes = {taken};
        item.activeTake = taken;
        edit.create(item);
    }
};

detail::Plan prepared(const model::Session& session, InstancePool& pool, MediaCache& media) {
    auto plan = detail::compile(session, pool, 0, 128);
    REQUIRE(plan);
    plan->media = &media;
    REQUIRE(detail::prepare(*plan, session));
    REQUIRE_MESSAGE(plan->warnings.empty(), (plan->warnings.empty() ? "" : plan->warnings.front()));
    return std::move(*plan);
}
}  // namespace

TEST_CASE("read-ahead PDC: unrelated playback latency does not delay monitoring" *
          doctest::description("rt")) {
    for (bool anticipative : {false, true})
        for (unsigned latency : {1000u, 12000u}) {  // also beyond the 200 ms render horizon
            CAPTURE(anticipative);
            CAPTURE(latency);
            model::EditTxn edit(model::Session::create({.console9 = false}), 1);
            const Id playback = track(edit), live = track(edit);
            delay(edit, playback, latency);
            delay(edit, live, 37);
            delay(edit, kMaster, 11);
            monitor(edit, live);
            const auto session = commit(edit);
            auto pool = fixturePool();
            MediaCache media;
            DummyBackend backend(impulse(512));
            Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
            REQUIRE(engine.load(session));
            engine.play();
            run(engine, backend, anticipative, 24);
            engine.stop();
            const auto& samples = backend.capture().channels[0];
            const auto first =
                std::find_if(samples.begin(), samples.end(), [](float v) { return v != 0; });
            CHECK(std::distance(samples.begin(), first) == 512 + 37 + 11);
            CHECK(engine.processErrors() == 0);
            CHECK(engine.anticipationMisses() == 0);
        }
}

TEST_CASE("read-ahead PDC: monitored playback equals offline including playrate" *
          doctest::description("rt")) {
    AudioFile file;
    for (double rate : {1., 1.5}) {
        model::EditTxn edit(model::Session::create({.console9 = true}), 1);
        const Id playback = track(edit), live = track(edit);
        for (Id owner : {playback, live}) file.add(edit, owner);
        delay(edit, playback, 37);
        monitor(edit, live);
        auto project = edit.project();
        project.playrate.rate = rate;
        edit.setProject(project);
        const auto session = commit(edit);
        auto pool = fixturePool();
        auto reference = renderOffline(session, pool, {.endSeconds = 2048. / 48000., .threads = 1});
        REQUIRE(reference);
        auto repeated = renderOffline(session, pool, {.endSeconds = 2048. / 48000., .threads = 1});
        REQUIRE(repeated);
        CHECK(reference->audio.channels == repeated->audio.channels);
        for (bool anticipative : {false, true}) {
            CAPTURE(rate);
            CAPTURE(anticipative);
            auto livePool = fixturePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4096});
            Engine engine(livePool, media, {.anticipative = anticipative, .workers = 1});
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
            REQUIRE(engine.load(session));
            engine.play();
            run(engine, backend, anticipative, 32);
            engine.stop();
            for (std::size_t c = 0; c < 2; ++c)
                CHECK(std::equal(reference->audio.channels[c].begin(),
                                 reference->audio.channels[c].end(),
                                 backend.capture().channels[c].begin() + reference->outputLatency));
            CHECK(engine.processErrors() == 0);
            CHECK(engine.anticipationMisses() == 0);
        }
    }
}

TEST_CASE("read-ahead PDC: arm and monitor edits retain timing and continuous playback" *
          doctest::description("rt")) {
    // Linear strips isolate timing continuity from Console RNG phase on class changes.
    AudioFile file;
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const Id playback = track(edit), live = track(edit);
    for (Id owner : {playback, live}) file.add(edit, owner);
    delay(edit, playback, 37);
    monitor(edit, live);
    edit.modify<model::Track>(live, [](auto& value) { value.record.armed = false; });
    const auto initial = commit(edit);
    auto pool = fixturePool();
    MediaCache media;
    auto baseline = prepared(initial, pool, media);
    auto reference = renderOffline(initial, pool, {.endSeconds = 8192. / 48000., .threads = 1});
    REQUIRE(reference);
    for (bool anticipative : {false, true}) {
        auto livePool = fixturePool();
        MediaCache liveMedia;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 16384});
        Engine engine(
            livePool, liveMedia,
            {.anticipative = anticipative, .renderBlock = 128, .renderAheadMs = 10, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(initial));
        engine.play();
        run(engine, backend, anticipative, 16);
        auto session = initial;
        for (const auto& [armed, mode] :
             {std::pair{true, model::Monitor::On}, std::pair{true, model::Monitor::Off},
              std::pair{false, model::Monitor::Off}}) {
            model::EditTxn change(session, session.version + 1);
            change.modify<model::Track>(live, [&](auto& value) {
                value.record.armed = armed;
                value.record.monitor = mode;
            });
            session = commit(change);
            auto next = prepared(session, pool, media);
            REQUIRE(next.nodes.size() == baseline.nodes.size());
            REQUIRE(next.edges.size() == baseline.edges.size());
            CHECK(next.outputLatency == baseline.outputLatency);
            for (std::size_t i = 0; i < next.nodes.size(); ++i)
                CHECK(next.nodes[i].timelineShift == baseline.nodes[i].timelineShift);
            for (std::size_t i = 0; i < next.edges.size(); ++i)
                CHECK(next.edges[i].delay.length == baseline.edges[i].delay.length);
            REQUIRE(engine.load(session));
            run(engine, backend, anticipative, 16);
        }
        run(engine, backend, anticipative, 1);  // flush the output latency
        engine.stop();
        for (std::size_t c = 0; c < 2; ++c)
            CHECK(std::equal(reference->audio.channels[c].begin(),
                             reference->audio.channels[c].end(),
                             backend.capture().channels[c].begin() + reference->outputLatency));
        CHECK(engine.processErrors() == 0);
        CHECK(engine.anticipationMisses() == 0);
    }
}

TEST_CASE("read-ahead PDC: live dry and latent bus paths meet at one sample" *
          doctest::description("rt")) {
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const Id live = track(edit), bus = track(edit, model::StripType::Buss);
    monitor(edit, live);
    delay(edit, bus, 37);
    model::Route send;
    send.from.track = live;
    send.to.track = bus;
    edit.create(send);
    const auto session = commit(edit);
    for (bool anticipative : {false, true}) {
        auto pool = fixturePool();
        MediaCache media;
        DummyBackend backend(impulse(512));
        Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(session));
        engine.play();
        run(engine, backend, anticipative, 16);
        engine.stop();
        const auto& samples = backend.capture().channels[0];
        CHECK(std::count_if(samples.begin(), samples.end(), [](float x) { return x != 0; }) == 1);
        constexpr double strip = 1.0;  // a centred hop at unity (ADR-0066)
        CHECK(samples[512 + 37] == doctest::Approx(.25 * strip * (1 + strip)).epsilon(1e-6));
        CHECK(engine.processErrors() == 0);
    }
}

TEST_CASE("read-ahead PDC: recording retains the performed timeline position" *
          doctest::description("rt")) {
    AudioFile files;
    audio::AudioData cue;
    cue.sampleRate = 48000;
    cue.channels.assign(2, std::vector<float>(16384));
    // Cue at 1008 reaches output at 2008; device output/input latency returns
    // the performed impulse at 2048. The take must place it back at 1008.
    cue.channels[0][1008] = cue.channels[1][1008] = .25f;
    REQUIRE(audio::writeWav(files.path, cue));
    for (bool anticipative : {false, true}) {
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        const Id playback = track(edit), live = track(edit);
        delay(edit, playback, 1000);
        files.add(edit, playback);
        delay(edit, live, 37);
        monitor(edit, live);
        const auto session = commit(edit);
        auto pool = fixturePool();
        MediaCache media;
        struct LatentBackend : DummyBackend {
            using DummyBackend::DummyBackend;
            BackendStatus status() const override {
                auto value = DummyBackend::status();
                value.outputLatency = 17;
                value.inputLatency = 23;
                return value;
            }
        } backend(impulse(2048));
        Engine engine(pool, media, {.anticipative = anticipative, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(session));
        const auto path = files.directory / (anticipative ? "ahead.wav" : "callback.wav");
        REQUIRE(engine.startRecording({{live, {0, 1}, path, {}}}));
        engine.play();
        run(engine, backend, anticipative, 24);
        auto takes = engine.stopRecording();
        engine.stop();
        CHECK(backend.capture().channels[0][2008] == doctest::Approx(.25).epsilon(1e-6));
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const auto& take = takes->front();
        auto recorded = audio::readAudio(take.file);
        REQUIRE(recorded);
        const auto& samples = recorded->channels[0];
        const auto at =
            std::find_if(samples.begin(), samples.end(), [](float x) { return x != 0; });
        CHECK(take.startSample + std::distance(samples.begin(), at) == 2048 - 1000 - 17 - 23);
        CHECK(take.overflowFrames == 0);
    }
}

TEST_CASE("read-ahead PDC: non-grid latency aligns exact automation points with impulses" *
          doctest::description("rt")) {
    AudioFile file(true);
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const Id silent = track(edit), source = track(edit);
    delay(edit, silent, 37);
    file.add(edit, source);
    model::FxInstance gain;
    gain.plugin.id = "com.suppleaudio.test.gain";
    const Id fx = edit.create(gain);
    edit.modify<model::Track>(source, [&](auto& value) { value.chain.push_back(fx); });
    model::Envelope envelope;
    envelope.target = {fx, "gain"};
    envelope.timebase = model::Timebase::Time;
    envelope.points = {{0, -40, model::PointShape::Square}, {.01, 0, model::PointShape::Square}};
    edit.create(envelope);
    const auto session = commit(edit);
    auto pool = fixturePool();
    auto output = renderOffline(session, pool, {.endSeconds = .02, .threads = 1});
    REQUIRE(output);
    const auto& samples = output->audio.channels[0];
    CHECK(std::count_if(samples.begin(), samples.end(), [](float x) { return x != 0; }) == 2);
    CHECK(samples[480] == doctest::Approx(.25).epsilon(1e-6));
    CHECK(samples[479] == doctest::Approx(double(samples[480]) * .01).epsilon(1e-6));
}

TEST_CASE("read-ahead PDC: unity plugin transport follows its stage input" *
          doctest::description("rt")) {
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    const Id silent = track(edit), source = track(edit);
    delay(edit, silent, 1000);
    delay(edit, source, 37);
    edit.create(model::TempoPointObj{1, 60, false});  // frame 24000 at the initial 120 BPM
    model::FxInstance probe;
    probe.plugin.id = "com.suppleaudio.test.edge";
    probe.state = "sha256:pdc-tempo-probe";
    edit.putBlob(*probe.state, std::make_shared<const model::Blob>(model::Blob{{48, 0, 0, 0}}));
    const Id fx = edit.create(probe);  // outputs the received tempo / 1000
    edit.modify<model::Track>(source, [&](auto& value) { value.chain.push_back(fx); });
    auto pool = fixturePool();
    MediaCache media;
    auto plan = prepared(commit(edit), pool, media);
    for (auto* instance : plan.instances) REQUIRE(instance->startProcessing());
    REQUIRE(detail::processBlock(plan, 24000, 1));
    // Its input is still at frame 23000: the node's 963 plus 37 before this stage.
    CHECK(plan.nodes[plan.mainMaster].post.l[0] == doctest::Approx(.12).epsilon(1e-6));
    for (auto* instance : plan.instances) instance->stopProcessing();
}
