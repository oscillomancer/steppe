// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <clap/plugin.h>
#include <doctest/doctest.h>
#include "supple/engine/bench_probe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <semaphore>
#include <set>
#include <thread>
#include <tuple>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/engine/jack.h>
#include <supple/engine/realtime.h>
#include <supple/host/clap_host.h>

#include "fake_jack.h"
#include "live_workers.h"
#include "meters.h"
#include "plan.h"
#include "recording.h"

using namespace supple;
using namespace supple::engine;

namespace supple::engine {
// A main-thread transport write stalled halfway (preempted after the odd
// sequence store), and the executors' view of the epoch.
struct EngineTestAccess {
    static void outputConsume(Engine& e, std::uint64_t epoch, std::int64_t stream) SUPPLE_RT {
        e.rtEpoch_ = epoch;
        e.rtStream_.store(stream);
        e.rtPosEpoch_.store(epoch);
        e.wakeOutputConsumed();
    }
    static std::uint64_t outputSeen(const Engine& e) { return e.outputWakeSeen_; }
    static std::uint64_t outputSerial(const Engine& e) { return e.outputWakeSerial_.load(); }
    static void liveHooks(Engine& e, detail::LiveWorkers::Hooks hooks) {
        e.liveWorkersReady_.store(nullptr);
        e.liveWorkers_ = std::make_unique<detail::LiveWorkers>(3, hooks);
        e.liveWorkersReady_.store(e.liveWorkers_.get());
    }
    static std::size_t plans(const Engine& e) { return e.plans_.size(); }
    static std::uint64_t beginWrite(Engine& e) {
        const std::uint64_t seq = e.xportSeq_.load();
        e.xportSeq_.store(seq + 1);
        return seq;
    }
    static void loopAndEpoch(Engine& e, std::int64_t start, std::int64_t end) {
        e.loopStart_.store(start);
        e.loopEnd_.store(end);
        e.loopOn_.store(true);
        e.epoch_.fetch_add(1);
    }
    static void endWrite(Engine& e, std::uint64_t seq) { e.xportSeq_.store(seq + 2); }
    static std::uint64_t epoch(const Engine& e) { return e.epoch_.load(); }
    static std::uint64_t rtEpoch(const Engine& e) { return e.rtEpoch_; }
    static LoopRange rtLoop(const Engine& e) { return e.loopC_; }
    static std::uint64_t rendererEpoch(const Engine& e) { return e.epochDone_.load(); }
    static LoopRange rendererLoop(const Engine& e) { return e.loopR_; }  // after rendererEpoch
    static const detail::Plan& latestPlan(const Engine& e) { return *e.latestPlan_.load(); }
    static auto deadline(std::chrono::steady_clock::time_point now,
                         std::chrono::steady_clock::time_point next, bool idleWork,
                         std::chrono::steady_clock::duration safety = std::chrono::seconds(1)) {
        return Engine::rendererDeadline(now, next, idleWork, safety);
    }
    static auto stoppedDeadline(std::chrono::steady_clock::time_point next,
                                std::chrono::steady_clock::time_point last, bool requested) {
        return Engine::stoppedRenderDeadline(next, last, requested);
    }
    static auto safety(const Engine& e) { return e.rendererSafetyTimeout(); }
    static void playing(Engine& e, bool playing) { e.playing_ = playing; }
    static void rendererState(Engine& e, detail::Plan& plan, std::int64_t stream,
                              std::int64_t ahead) {
        e.plan_ = &plan;
        e.playing_ = true;
        e.streamLocal_ = stream;
        e.aheadFrames_ = ahead;
        e.ring_ = std::make_unique<detail::ChunkRing>(4, 64);
    }
    static detail::ChunkRing& ring(Engine& e) { return *e.ring_; }
    static std::int64_t target(const Engine& e, std::uint64_t epoch = 0) {
        return e.rendererSpaceTarget(epoch);
    }
    static bool room(const Engine& e, std::uint64_t epoch = 0) { return e.rendererHasRoom(epoch); }
    static void arm(Engine& e, std::int64_t target) { e.rendererWakeAt_.store(target); }
    static void consume(Engine& e, std::int64_t stream) SUPPLE_RT {
        e.rtStream_.store(stream);
        e.rtPosEpoch_.store(0);
        e.wakeRendererConsumed();
    }
    static auto event(const Engine& e) { return e.rendererEvent_.load(); }
};
}  // namespace supple::engine
using model::Id;
using model::kMaster;

namespace supple::engine::detail {
struct RecorderTestAccess {
    static void stall(Recorder& recorder) { recorder.writerStalled_.store(true); }
    static bool stalled(const Recorder& recorder) { return recorder.writerStallAck_.load(); }
    static void resume(Recorder& recorder) { recorder.writerStalled_.store(false); }
    static bool failHeader(Recorder& recorder, std::size_t index) {
        const auto& file = recorder.tracks_[index]->target.file;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            if (std::filesystem::read_symlink(entry.path(), ec) != file || ec) continue;
            const int descriptor = std::stoi(entry.path().filename().string());
            const int readOnly = ::open(file.c_str(), O_RDONLY);
            if (readOnly < 0) return false;
            const bool ok = ::dup2(readOnly, descriptor) >= 0;
            ::close(readOnly);
            return ok;
        }
        return false;
    }
    static void updateHeaders(Recorder& recorder) { recorder.updateHeaders(); }
    static void failTarget(Recorder& recorder, std::size_t index) {
        recorder.tracks_[index]->writer.abandon();
    }
};
}  // namespace supple::engine::detail

namespace {
template <class Predicate>
bool waitForRecorder(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return predicate();
}
}  // namespace

TEST_CASE("a stalled recording writer preserves overflow gaps and loop file positions") {
    using Access = detail::RecorderTestAccess;
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-stall-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    detail::Recorder recorder({{Id{model::Kind::Track, 1}, {0}, dir / "take.wav", {}}}, 16, 0);
    Access::stall(recorder);
    REQUIRE(recorder.open());
    REQUIRE(waitForRecorder([&] { return Access::stalled(recorder); }));
    std::array<float, 200> data;
    data.fill(1);
    const float* input[] = {data.data()};
    recorder.capture(input, 1, 200, 0);  // 160 fit, 40 are dropped
    Access::resume(recorder);
    REQUIRE(waitForRecorder([&] { return !Access::stalled(recorder); }));
    Access::stall(recorder);
    REQUIRE(waitForRecorder([&] { return Access::stalled(recorder); }));
    data.fill(.5f);
    recorder.capture(input, 1, 24, 0);    // loop wraps at file frame 200
    recorder.capture(input, 1, 200, 24);  // a dropped tail, no following timestamp
    const auto takes = recorder.finish({0, 200, true});
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].error.empty());
    CHECK(takes[0].overflowFrames == 104);
    CHECK(takes[0].frames == 424);
    REQUIRE(takes[0].segments.size() == 2);
    CHECK(takes[0].segments[1].fileFrame == 200);
    const auto decoded = audio::readAudio(takes[0].file);
    REQUIRE(decoded);
    REQUIRE(decoded->frames() == 424);
    for (std::size_t frame = 0; frame < decoded->frames(); ++frame) {
        const float expected = frame < 160 ? 1.f : frame < 200 ? 0.f : frame < 360 ? .5f : 0.f;
        CHECK(decoded->channels[0][frame] == expected);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a master's take starts with the device input's takes, padded by the round trip") {
    // ADR-0057: device input captured at P was heard at P - 100 (round trip);
    // the master's output at P belongs to P - 40 (its own shift). Its frames
    // go 60 frames later in its file, so both files start at P - 100.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-rec-master-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    RecordTarget input{Id{model::Kind::Track, 1}, {0}, dir / "input.wav", {}};
    RecordTarget master{model::kMaster, {}, dir / "master.wav", {}, true, 40};
    detail::Recorder recorder({input, master}, 16, 100);
    REQUIRE(recorder.open());
    std::array<float, 50> ramp;
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = float(i + 1);
    const float* in[] = {ramp.data()};
    const detail::ProcessedInput out{model::kMaster, {ramp.data(), ramp.data()}, 0};
    recorder.capture(in, 1, 50, 1000, std::span(&out, 1));
    const auto takes = recorder.finish();
    REQUIRE(takes.size() == 2);
    for (const RecordedTake& take : takes) CHECK(take.startSample == 900);
    const auto& rec = takes[0].track == model::kMaster ? takes[0] : takes[1];
    CHECK(rec.frames == 110);
    const auto decoded = audio::readAudio(rec.file);
    REQUIRE(decoded);
    REQUIRE(decoded->channels.size() == 2);
    CHECK(decoded->channels[0][59] == 0.f);  // padding: before this recording
    CHECK(decoded->channels[0][60] == 1.f);  // timeline 960 = 1000 - 40
    CHECK(decoded->channels[1][109] == 50.f);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a failed recording target leaves other targets writing") {
    using Access = detail::RecorderTestAccess;
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-error-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    detail::Recorder recorder({{Id{model::Kind::Track, 1}, {0}, dir / "bad.wav", {}},
                               {Id{model::Kind::Track, 2}, {0}, dir / "good.wav", {}}},
                              16, 0);
    Access::stall(recorder);
    REQUIRE(recorder.open());
    REQUIRE(waitForRecorder([&] { return Access::stalled(recorder); }));
    Access::failTarget(recorder, 0);
    const std::array<float, 32> data = [] {
        std::array<float, 32> result;
        result.fill(.5f);
        return result;
    }();
    const float* input[] = {data.data()};
    recorder.capture(input, 1, 32, 0);
    const auto takes = recorder.finish();
    REQUIRE(takes.size() == 2);
    CHECK_FALSE(takes[0].error.empty());
    CHECK(takes[0].overflowFrames == 32);
    CHECK(takes[1].error.empty());
    CHECK(takes[1].frames == 32);
    CHECK(takes[1].overflowFrames == 0);
    CHECK(takes[1].hash.empty());
    const auto decoded = audio::readAudio(takes[1].file);
    REQUIRE(decoded);
    CHECK(decoded->channels[0] == std::vector<float>(32, .5f));
    std::filesystem::remove_all(dir);
}

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
const std::filesystem::path kVst3Fixture = FIXTURE_VST3_PATH;

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

InstancePool makeVstPool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::array paths{kFixture, kVst3Fixture};
        auto list = host::scan(paths);
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

// A track playing a sine instrument: notes on beats 0, 1, 2, 3 (quarter notes).
model::Session synthSession(double volumeDb = 0.0, const char* extraFx = nullptr,
                            unsigned tracks = 1) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    for (unsigned channel = 0; channel < tracks; ++channel) {
        model::Track tr;
        tr.name = "Synth";
        tr.parent = kMaster;
        tr.volumeDb = volumeDb;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
        for (const char* plugin : {"com.suppleaudio.test.sine", extraFx}) {
            if (!plugin) continue;
            model::FxInstance fx;
            fx.plugin.id = plugin;
            if (std::string(plugin) == "com.suppleaudio.test.gain")
                fx.patch =
                    json::parse(
                        R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                        .value();
            const Id f = t.create(fx);
            t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
        }
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 4 * 960;
        for (int i = 0; i < 4; ++i)
            src.sequence.notes.push_back(
                {i * 960, 480, 0, static_cast<std::uint8_t>(60 + 4 * i), 100, 0});
        const Id mi = t.create(src);
        model::Take take;
        take.source = mi;
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = 4.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    return t.commit().value();
}

// A sine track whose notes start at the given ticks (ppq 960, 120 bpm: 25 samples a tick).
model::Session clusterSession(std::initializer_list<int> ticks) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Synth";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 4 * 960;
    std::uint8_t key = 60;
    for (int tick : ticks) src.sequence.notes.push_back({tick, 960, 0, key++, 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

double maxDiff(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b,
               std::size_t n) {
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(double(a[aFrom + i]) - double(b[i])));
    return worst;
}

const EngineOptions kInCallback{.anticipative = false, .workers = 0};

// Anticipative: drive a manual backend `cycles` times, letting the renderer
// keep ahead (a device's callbacks would be paced by its clock).
void runAhead(Engine& engine, DummyBackend& backend, std::int64_t cycles, std::uint32_t block) {
    while (cycles > 0) {
        REQUIRE(engine.awaitRendered(4096));
        const std::int64_t n =
            std::min<std::int64_t>(cycles, std::max<std::int64_t>(1, 4096 / block));
        backend.run(n);
        cycles -= n;
    }
}

}  // namespace

TEST_CASE("real-time output equals the offline render (any block size)" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0},
                                         EngineOptions{.workers = 1}, EngineOptions{.workers = 7}})
        for (const char* extra : {static_cast<const char*>(nullptr), "com.suppleaudio.test.gain"}) {
            INFO("anticipative " << options.anticipative << ", workers " << options.workers);
            // Eight independent roots exercise every requested execution thread.
            model::Session s = synthSession(-18.0, extra, 8);
            model::EditTxn mode(s, 2);
            auto project = mode.project();
            project.console9 = true;
            project.seed = 24681357;
            mode.setProject(project);
            s = mode.commit().value();
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();

            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
            BenchProbe probe(10000);
            probe.recording = true;
            auto measuredOptions = options;
            measuredOptions.benchProbe = &probe;
            Engine engine(rtPool, media, measuredOptions);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
            REQUIRE(engine.load(s));
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
            else
                backend.run((48000 * 2 + 4096) / 128);
            CHECK(engine.anticipationMisses() == 0);
            engine.stop();
            CHECK(probe.renderJob.dropped == 0);
            if (options.anticipative) {
                CHECK_FALSE(probe.renderJob.samples().empty());
                CHECK(probe.aheadObservations > 0);
                CHECK(probe.aheadLowWater >= 0);
            } else {
                CHECK(probe.renderJob.samples().empty());
                CHECK(probe.aheadObservations == 0);
            }
            const std::uint32_t L = engine.outputLatency();
            CHECK(L == (extra ? 64u : 0u));
            CHECK(engine.processErrors() == 0);
            const auto& cap = backend.capture();
            // The live output lags by the plan's latency; after that it is the offline render.
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
        }
}

TEST_CASE("pan modes and laws: real-time output equals the offline render (ADR-0066)" *
          doctest::description("rt")) {
    // Four strips: dual, width under an automated pan, a boosted -4.5 dB
    // project law, and a strip of its own -6 dB law with a following send into
    // a buss, all through Console 9.
    model::Session s = synthSession(-12.0, nullptr, 4);
    model::EditTxn t(s, 2);
    auto project = t.project();
    project.console9 = true;
    project.seed = 777;
    project.panLaw = {-4.5, true};
    t.setProject(project);
    const Id dual{model::Kind::Track, 1}, wide{model::Kind::Track, 2},
        boosted{model::Kind::Track, 3}, own{model::Kind::Track, 4};
    t.modify<model::Track>(dual, [](auto& x) {
        x.panMode = model::PanMode::Dual;
        x.panLeft = -20;
        x.panRight = 70;
    });
    t.modify<model::Track>(wide, [](auto& x) {
        x.panMode = model::PanMode::Width;
        x.width = 35;
    });
    model::Envelope pan;
    pan.target = {wide, "pan"};
    pan.timebase = model::Timebase::Time;
    pan.points = {{0, -80}, {1.5, 60}};
    t.create(pan);
    for (const auto& [target, param, from, to] : {std::tuple{wide, "width", -90., 75.},
                                                  {dual, "pan_left", -100., 40.},
                                                  {dual, "pan_right", 100., -60.},
                                                  {own, "pan_boost", 0., 1.}}) {
        model::Envelope env;
        env.target = {target, param};
        env.timebase = model::Timebase::Time;
        env.points = {{0, from}, {.73, to}, {1.2, from}};
        t.create(env);
    }
    t.modify<model::Track>(boosted, [](auto& x) { x.pan = 45; });
    t.modify<model::Track>(own, [](auto& x) {
        x.pan = -30;
        x.panLaw = model::PanLaw{-6, false};
    });
    model::Track bus;
    bus.type = model::StripType::Buss;
    bus.parent = kMaster;
    const Id busId = t.create(bus);
    t.modify<model::Track>(kMaster, [&](auto& m) { m.children.push_back(busId); });
    model::Route send;
    send.from.track = own;
    send.to.track = busId;
    send.volumeDb = -3;
    t.create(send);
    s = t.commit().value();
    host::setBridgeExecutable(BRIDGE_PATH);
    for (auto isolation : {model::Isolation::InProcess, model::Isolation::SharedBridge,
                           model::Isolation::Dedicated}) {
        model::EditTxn isolate(s, 3);
        for (const auto& entry : s.fx)
            isolate.modify<model::FxInstance>(entry.id, [&](auto& f) { f.isolation = isolation; });
        const auto isolated = isolate.commit().value();
        CAPTURE(isolation);
        auto pool = makePool();
        auto offline =
            renderOffline(isolated, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 2}}) {
            // The callback deliberately bypasses bridged plug-ins; only the
            // anticipative renderer runs their blocking bridge transport.
            if (!options.anticipative && isolation != model::Isolation::InProcess) continue;
            INFO("anticipative " << options.anticipative);
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
            REQUIRE(engine.load(isolated));
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, (48000 * 2 + 4096) / 96, 96);
            else
                backend.run((48000 * 2 + 4096) / 96);
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const auto& cap = backend.capture();
            const std::uint32_t L = engine.outputLatency();
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
        }
    }
}

TEST_CASE("several masters play to their own device outputs, in step (ADR-0056)" *
          doctest::description("rt")) {
    // Two synths: one under the main master (outputs 1/2), one under a cue
    // master on 3/4 whose latent gain makes it the later of the two.
    model::Session s = synthSession(-18.0, nullptr, 2);
    model::EditTxn t(s, 2);
    model::Track cueTrack;
    cueTrack.type = model::StripType::Master;
    cueTrack.name = "Cue";
    cueTrack.hwOutput = std::vector<int>{3, 4};
    const Id cue = t.create(cueTrack);
    auto project = t.project();
    project.masters.push_back(cue);
    project.seed = 97531;
    t.setProject(project);
    const Id moved = s.tracks.find(kMaster)->children.back();
    t.modify<model::Track>(kMaster, [&](auto& m) { std::erase(m.children, moved); });
    t.modify<model::Track>(cue, [&](auto& m) { m.children.push_back(moved); });
    t.modify<model::Track>(moved, [&](auto& tr) {
        tr.parent = cue;
        tr.volumeDb = -9.0;
    });
    model::FxInstance gain;
    gain.plugin.id = "com.suppleaudio.test.gain";
    gain.patch =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
            .value();
    const Id g = t.create(gain);
    t.modify<model::Track>(cue, [&](auto& m) { m.chain.push_back(g); });
    s = t.commit().value();
    auto pool = makePool();
    auto main = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
    auto cued =
        renderOffline(s, pool, {.master = cue, .startSeconds = 0, .endSeconds = 2.0}).value();
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .outputs = 6}));
        REQUIRE(engine.load(s));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
        else
            backend.run((48000 * 2 + 4096) / 128);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        const std::uint32_t L = engine.outputLatency();
        CHECK(L == 64u);  // the cue master's gain: both masters wait for it
        const auto& cap = backend.capture();
        REQUIRE(cap.channels.size() == 6);
        CHECK(maxDiff(cap.channels[0], L, main.audio.channels[0], 96000) == 0.0);
        CHECK(maxDiff(cap.channels[1], L, main.audio.channels[1], 96000) == 0.0);
        CHECK(maxDiff(cap.channels[2], L, cued.audio.channels[0], 96000) == 0.0);
        CHECK(maxDiff(cap.channels[3], L, cued.audio.channels[1], 96000) == 0.0);
        double cuePeak = 0, unused = 0;
        for (std::size_t i = 0; i < 96000; ++i) {
            cuePeak = std::max(cuePeak, std::fabs(double(cap.channels[2][i])));
            unused = std::max({unused, std::fabs(double(cap.channels[4][i])),
                               std::fabs(double(cap.channels[5][i]))});
        }
        CHECK(cuePeak > 0.01);
        CHECK(unused == 0.0);
    }
}

TEST_CASE("a master with one output plays mono; outputs past the device are skipped" *
          doctest::description("rt")) {
    model::Session s = synthSession(-18.0);
    model::EditTxn t(s, 2);
    t.modify<model::Track>(kMaster, [](auto& m) { m.hwOutput = std::vector<int>{2}; });
    s = t.commit().value();
    auto pool = makePool();
    auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(48000 / 128);
    // The same master on outputs 5/6 of a two-output device: nothing plays
    // there, and the engine says why.
    model::EditTxn far(s, 3);
    far.modify<model::Track>(kMaster, [](auto& m) { m.hwOutput = std::vector<int>{5, 6}; });
    REQUIRE(engine.load(far.commit().value()));
    const auto warnings = engine.warnings();
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
        return w.find("output 6") != std::string::npos;
    }));
    engine.stop();
    const auto& cap = backend.capture();
    double left = 0, worst = 0;
    for (std::size_t i = 0; i < 48000 - 128; ++i) {
        left = std::max(left, std::fabs(double(cap.channels[0][i])));
        const float mono = 0.5f * (offline.audio.channels[0][i] + offline.audio.channels[1][i]);
        worst = std::max(worst, std::fabs(double(cap.channels[1][i]) - double(mono)));
    }
    CHECK(left == 0.0);
    CHECK(worst == 0.0);
}

TEST_CASE("take FX: real-time output equals the offline render" * doctest::description("rt")) {
    // The synth session with its sine and a latent gain moved into the take's chain.
    model::Session base = synthSession(-3.0, "com.suppleaudio.test.gain");
    model::EditTxn t(base, 2);
    Id track, take;
    for (const auto& e : base.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    for (const auto& e : base.takes) take = e.id;
    const auto chain = base.tracks.find(track)->chain;
    t.modify<model::Track>(track, [](model::Track& x) { x.chain.clear(); });
    t.modify<model::Take>(take, [&](model::Take& x) { x.chain = chain; });
    const model::Session s = t.commit().value();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
        CHECK(offline.warnings.empty());
        float loud = 0;
        for (float v : offline.audio.channels[0]) loud = std::max(loud, std::fabs(v));
        CHECK(loud > 0.05f);
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
        else
            backend.run((48000 * 2 + 4096) / 128);
        engine.stop();
        CHECK(engine.outputLatency() == 0u);  // the take's latency runs ahead, inside the track
        CHECK(engine.processErrors() == 0);
        CHECK(maxDiff(backend.capture().channels[0], 0, offline.audio.channels[0], 96000) == 0.0);
    }
}

TEST_CASE("a master's own items: real-time output equals the offline render (ADR-0057)" *
          doctest::description("rt")) {
    // A print on the master, after its latent insert and its fader, beside the live synth.
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rt-print-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.25 * std::sin(double(i) * 0.01));
    REQUIRE(audio::writeWav(dir / "media" / "print.wav", tone));
    model::Session base = synthSession(-3.0);
    model::EditTxn t(base, 2);
    const Id md = t.create(model::MediaSource{"print.wav", "", "wav", 48000, 2, 48000});
    model::Take take;
    take.source = md;
    const Id tk = t.create(take);
    model::Item item;
    item.track = kMaster;
    item.position = 0.5;
    item.length = 2.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    model::FxInstance gain;
    gain.plugin.id = "com.suppleaudio.test.gain";
    gain.patch =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
            .value();
    const Id f = t.create(gain);
    t.modify<model::Track>(kMaster, [&](model::Track& m) {
        m.chain.push_back(f);
        m.volumeDb = -6;
    });
    const model::Session s = t.commit().value();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache offlineMedia;
        auto offline =
            renderOffline(
                s, pool,
                {.startSeconds = 0, .endSeconds = 2.0, .projectDir = dir, .media = &offlineMedia})
                .value();
        CHECK(offline.warnings.empty());
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s, dir));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
        else
            backend.run((48000 * 2 + 4096) / 128);
        engine.stop();
        const std::uint32_t L = engine.outputLatency();
        CHECK(L == 64u);
        CHECK(engine.processErrors() == 0);
        CHECK(maxDiff(backend.capture().channels[0], L, offline.audio.channels[0], 96000) == 0.0);
        CHECK(maxDiff(backend.capture().channels[1], L, offline.audio.channels[1], 96000) == 0.0);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a stretched take: real-time output equals the offline render" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-rt-stretch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i) {
        const double s = 0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0);
        tone.channels[0][i] = static_cast<float>(s);
        tone.channels[1][i] = static_cast<float>(s);
    }
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone));

    // One audio item, half speed with the pitch preserved, 2 s long: the
    // stretcher builds this variant on the main thread when the plan loads.
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Audio";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    const Id md = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 48000});
    model::Take take;
    take.source = md;
    take.playrate = 0.5;
    take.preservePitch = true;
    take.pitchMode = "signalsmith";
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const model::Session session = t.commit().value();

    auto pool = makePool();
    auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 2.0, .projectDir = dir});
    REQUIRE(offline);
    REQUIRE(offline->warnings.empty());
    double offlinePeak = 0;
    for (std::size_t i = 0; i < 96000; ++i)
        offlinePeak = std::max(offlinePeak, std::fabs(double(offline->audio.channels[0][i])));
    CHECK(offlinePeak > 0.3);  // a stretched, audible take

    auto rtPool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(session, dir));
    engine.play();
    backend.run((48000 * 2 + 4096) / 128);
    engine.stop();
    const std::uint32_t L = engine.outputLatency();
    CHECK(engine.processErrors() == 0);
    CHECK(maxDiff(backend.capture().channels[0], L, offline->audio.channels[0], 96000) == 0.0);
    CHECK(maxDiff(backend.capture().channels[1], L, offline->audio.channels[1], 96000) == 0.0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("VST3 effect in chain: real-time output equals offline" * doctest::description("rt")) {
    auto found = host::scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    const auto it = std::find_if(found->begin(), found->end(), [](const host::PluginInfo& p) {
        return p.name == "Test VST3 Gain";
    });
    REQUIRE(it != found->end());
    const model::Session session = synthSession(-3.0, it->id.c_str());
    auto offlinePool = makeVstPool();
    auto offline = renderOffline(session, offlinePool, {.startSeconds = 0, .endSeconds = 2.0});
    REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
    auto rtPool = makeVstPool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(session));
    engine.play();
    backend.run((48000 * 2 + 4096) / 128);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto latency = engine.outputLatency();
    CHECK(maxDiff(backend.capture().channels[0], latency, offline->audio.channels[0], 96000) ==
          0.0);
    CHECK(maxDiff(backend.capture().channels[1], latency, offline->audio.channels[1], 96000) ==
          0.0);
}

TEST_CASE("an automated VST3 parameter renders real time = offline" * doctest::description("rt")) {
    // The plug-in's parameter is automated in the plain domain every command
    // uses; the engine converts the lane into the normalized domain at compile
    // time (Instance::paramsNormalized), so the audio thread sends normalized
    // values and never calls the edit controller (the host fixture test asserts
    // that count is zero). This fixture's two domains coincide, so the check
    // that matters here is that the automation is applied and that both paths
    // agree to the sample.
    auto found = host::scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    const auto it = std::find_if(found->begin(), found->end(), [](const host::PluginInfo& p) {
        return p.name == "Test VST3 Gain";
    });
    REQUIRE(it != found->end());
    model::Session session = synthSession(0.0, it->id.c_str());
    Id gainFx;
    bool placed = false;
    for (const auto& e : session.fx)
        if (e.value->plugin.id == it->id) {
            gainFx = e.id;
            placed = true;
        }
    REQUIRE(placed);
    {
        model::EditTxn t(session, 2);
        model::Envelope env;
        env.target = {gainFx, "Gain"};  // the fixture declares one parameter, "Gain"
        env.points = {{0.0, 0.9, model::PointShape::Linear, 0.0},
                      {2.0, 0.1, model::PointShape::Linear, 0.0}};
        t.create(env);
        session = t.commit().value();
    }
    auto offlinePool = makeVstPool();
    auto offline = renderOffline(session, offlinePool, {.startSeconds = 0, .endSeconds = 2.0});
    REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
    CHECK(offline->warnings.empty());
    auto rtPool = makeVstPool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(session));
    engine.play();
    backend.run((48000 * 2 + 4096) / 128);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto latency = engine.outputLatency();
    CHECK(maxDiff(backend.capture().channels[0], latency, offline->audio.channels[0], 96000) ==
          0.0);
    // And the envelope is what was rendered: 0.9 at the start (the lane holds
    // the last point after beat 2), 0.1 from beat 2 on.
    const auto& audio = offline->audio.channels[0];
    auto peak = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to && i < audio.size(); ++i)
            m = std::max(m, std::fabs(double(audio[i])));
        return m;
    };
    const double early = peak(4800, 9600), late = peak(72000, 76800);
    REQUIRE(early > 0.01);
    CHECK(late / early == doctest::Approx(0.1 / 0.9).epsilon(0.05));
}

TEST_CASE("a stage that leaves processing mid-ramp does not replay the ramp" *
          doctest::description("rt")) {
    // Review-14 A4. While a stage is not processed (it belongs to the other
    // executor) its applied wet level is dropped; the ramp in flight must be
    // dropped with it, or the block where it comes back applies the old ramp's
    // start (a step up to nearly full wet).
    model::Session s = synthSession(0.0, "com.suppleaudio.test.gain");
    auto pool = makePool();
    auto compiled = detail::compile(s, pool, 0.0, 256, 48000);
    REQUIRE(compiled);
    detail::Plan p = std::move(*compiled);
    REQUIRE(detail::prepare(p, s));
    REQUIRE(p.nodes.size() >= 2);
    detail::Stage* stage = nullptr;
    for (detail::Stage& st : p.nodes[0].chain)
        if (st.instance && st.instance->info().id == "com.suppleaudio.test.gain") stage = &st;
    REQUIRE(stage != nullptr);
    REQUIRE(stage->instance->startProcessing());
    host::Transport transport;
    const void* other = &transport;  // an executor that does not own the instance
    stage->instance->setOwner(&p.nodes[0]);
    // A bypass ramp in flight: from 1.0 (wet) toward 0.0, 480 frames long.
    stage->wet = 0.0;
    stage->appliedWet = 1.0f;
    stage->rampFromWet = 1.0f;
    stage->rampToWet = 0.0f;
    stage->wetRampLeft = 480;
    REQUIRE(detail::processNode(p, 0, 0, 256, transport, other));  // not ours: pass through
    CHECK(static_cast<double>(stage->appliedWet) < 0.0);
    CHECK(stage->wetRampLeft == 0);  // the ramp went with the applied value
    stage->instance->setOwner(nullptr);
    REQUIRE(detail::processNode(p, 0, 0, 256, transport, nullptr));         // ours again
    CHECK(static_cast<double>(stage->appliedWet) == doctest::Approx(0.0));  // the new target
    stage->instance->stopProcessing();
}

TEST_CASE(
    "in the callback: plan swaps while playing, seek, pause; retired plans and instances are "
    "freed") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    backend.run(10);
    CHECK(engine.transport().playing);
    CHECK(engine.transport().sample == 2560);
    // A new version (-6 dB) replaces the plan at the next cycle; the instance is kept.
    REQUIRE(engine.load(synthSession(-6.0206)));
    CHECK(pool.size() == 1);  // same fx id and plugin: the instance (and its state) is kept
    backend.run(10);
    engine.service();
    const auto& cap = backend.capture();
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[0][i])));
        return m;
    };
    // The change ramps over 10 ms (480 samples) instead of stepping: over the
    // first period after the swap (184 samples at 261 Hz) the gain is still
    // above 0.8; after the ramp it is half.
    const double before = peakOf(0, 2560), after = peakOf(2560 + 480, 5120);
    CHECK(after / before == doctest::Approx(0.5).epsilon(0.01));
    CHECK(peakOf(2560, 2560 + 184) / before > 0.8);
    // Seek to beat 2 (1.0 s): the output there matches an offline render at that point.
    engine.seek(1.0);
    backend.run(1);
    CHECK(engine.transport().sample == 48000 + 256);
    engine.pause();
    backend.run(2);
    CHECK_FALSE(engine.transport().playing);
    CHECK(engine.transport().sample == 48000 + 256);  // paused: the position holds
    engine.stop();
}

TEST_CASE("threaded backend: plan swaps from the main thread race-free" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Thread});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    for (int i = 0; i < 20; ++i) {
        REQUIRE(engine.load(synthSession(-0.5 * i)));
        while (backend.cycles() < 20 * (i + 1)) std::this_thread::yield();
        engine.service();
        if (i % 7 == 3) engine.seek(0.25 * i);
    }
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK(backend.cycles() >= 400);
}

TEST_CASE(
    "in the callback: transport requests between callbacks are never dropped (review round 3)") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    for (int i = 0; i < 40; ++i) {  // 120 requests, more than any queue held
        engine.play();
        engine.seek(0.5);
        engine.pause();
    }
    engine.seek(1.0);
    engine.play();
    backend.run(1);
    CHECK(engine.transport().playing);
    CHECK(engine.transport().sample == 48000 + 256);
    for (int i = 0; i < 70; ++i) engine.play();
    engine.pause();
    backend.run(1);
    CHECK_FALSE(engine.transport().playing);
    CHECK(engine.transport().sample == 48000 + 256);
    engine.stop();
}

TEST_CASE("a seek off the block grid never grows event storage on the audio thread" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.renderBlock = 100}}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
        Engine engine(pool, media, options);
        // Two notes at sample 75, two at 100: aligned 100-frame blocks hold two
        // each, but the block [1, 101) after a one-sample seek holds all four.
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 100}));
        REQUIRE(engine.load(clusterSession({3, 3, 4, 4})));
        engine.seek(1.0 / 48000.0);
        engine.play();
        if (options.anticipative) REQUIRE(engine.awaitRendered(400));
        backend.run(4);  // under rtsan an allocation here aborts
        CHECK(engine.processErrors() == 0);
        double energy = 0;
        for (std::size_t i = 100; i < 400; ++i)
            energy +=
                double(backend.capture().channels[0][i]) * double(backend.capture().channels[0][i]);
        CHECK(energy > 1.0);  // all four notes sound
        engine.stop();
    }
}

TEST_CASE("a plugin that refuses to start is passed through, not processed") {
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
        model::Session s = synthSession(0.0, "com.suppleaudio.test.edge");
        model::EditTxn t(s, 2);
        for (const auto& e : s.fx)
            if (e.value->plugin.id == "com.suppleaudio.test.edge")
                t.modify<model::FxInstance>(
                    e.id, [](model::FxInstance& f) { f.state = "sha256:edge-refuses-start"; });
        t.putBlob("sha256:edge-refuses-start",
                  std::make_shared<const model::Blob>(model::Blob{{1, 0, 0, 0}}));
        Id edge;
        for (const auto& e : s.fx)
            if (e.value->plugin.id == "com.suppleaudio.test.edge") edge = e.id;
        REQUIRE(engine.load(t.commit().value()));
        engine.play();
        if (options.anticipative) REQUIRE(engine.awaitRendered(2560));
        backend.run(10);
        CHECK(engine.processErrors() >= 1);
        // Reset as it refused: a retry that succeeds later starts it clean,
        // with nothing left over (a note handed over unreset) that no
        // note-off would end.
        REQUIRE(pool.find(edge));
        CHECK(pool.find(edge)->resets() >= 1);
        CHECK(engine.transport().sample == 2560);
        double peak = 0;
        for (std::size_t i = 0; i < 2560; ++i)
            peak = std::max(peak, std::fabs(double(backend.capture().channels[0][i])));
        CHECK(peak > 0.1);  // the sine passes through the stage that did not start
        engine.stop();
    }
}

TEST_CASE("stop() stops plugins on the audio thread even with second-long gaps between callbacks" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    // 8192 frames at 8 kHz: a callback about once a second, paced like a device.
    DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
    Engine engine(pool, media);
    model::Session s = synthSession(0.0);
    model::Project project = *s.project;
    project.sampleRate = 8000;
    s.project = std::make_shared<const model::Project>(project);
    REQUIRE(engine.start(backend, {.sampleRate = 8000, .blockSize = 8192}));
    REQUIRE(engine.load(s));
    engine.play();
    while (backend.cycles() < 1) std::this_thread::yield();
    engine.stop();  // lands mid-gap: no timeout, no stop from this thread
    CHECK(engine.lastStopOnAudioThread());
    CHECK(engine.processErrors() == 0);
}

TEST_CASE("a manual backend's stop is the audio thread's stop") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    backend.run(2);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
}

TEST_CASE("the engine plays through the JACK backend (a stand-in libjack)" *
          doctest::description("rt")) {
    FakeJack& fake = fakeJack();
    fake.reset(1024);  // the graph's period is four engine blocks
    auto pool = makePool();
    MediaCache media;
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    JackBackend backend(o);
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    for (int i = 0; i < 5000 && fake.outputPeak("supple:out_1") <= 0.01f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(fake.outputPeak("supple:out_1") > 0.01f);
    CHECK(engine.transport().sample >= 256);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
    CHECK(engine.processErrors() == 0);
    fake.reset();
}

TEST_CASE("the engine survives the JACK server going away") {
    FakeJack& fake = fakeJack();
    fake.reset(256);
    auto pool = makePool();
    MediaCache media;
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    JackBackend backend(o);
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    const auto until = fake.cycles() + 5;
    while (fake.cycles() < until) std::this_thread::yield();
    fake.shutdown();
    CHECK(backend.stats().serverGone);
    engine.stop();  // no final callback: the plugins are stopped on this thread
    CHECK_FALSE(engine.lastStopOnAudioThread());
    CHECK_FALSE(engine.running());
    fake.reset();
}

TEST_CASE("the engine runs a session at the device's rate, not the project's") {
    // A 48 kHz project on a 44.1 kHz device: notes land at the same seconds
    // (0.25 s long, one per beat at 120 BPM) and the plugin runs at 44.1 kHz.
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 44100});
        EngineOptions options;
        options.anticipative = anticipative;
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 44100, .blockSize = 147}));
        const model::Session s = synthSession(0.0);
        REQUIRE(s.project->sampleRate == 48000.0);
        REQUIRE(engine.load(s));
        engine.play();
        if (anticipative)
            runAhead(engine, backend, 44100 / 147, 147);
        else
            backend.run(44100 / 147);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        REQUIRE(L.size() >= 44100);
        auto energy = [&](std::size_t from, std::size_t to) {
            double e = 0;
            for (std::size_t i = from; i < to; ++i) e += double(L[i]) * double(L[i]);
            return e;
        };
        CHECK(energy(0, 11000) > 1.0);       // note 1: 0 to 0.25 s
        CHECK(energy(11030, 22045) == 0.0);  // silence to beat 1
        CHECK(energy(22055, 33000) > 1.0);   // note 2 from 0.5 s = 22050
        CHECK(engine.transport().seconds == doctest::Approx(1.0));
    }
}

TEST_CASE(
    "anticipative: pause silences at once, seek plays no stale audio, the position is the "
    "listener's" *
    doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    engine.awaitTransport();
    CHECK(engine.transport().playing);
    CHECK(engine.transport().sample == 0);
    runAhead(engine, backend, 10, 256);
    CHECK(engine.transport().sample == 2560);
    const auto& cap = backend.capture();
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[0][i])));
        return m;
    };
    CHECK(peakOf(2304, 2560) > 0.1);  // the first note sounds
    engine.pause();
    backend.run(1);  // the rendered audio is dropped: silent at once
    CHECK(peakOf(2560, 2816) == 0.0);
    engine.awaitTransport();
    CHECK_FALSE(engine.transport().playing);
    CHECK(engine.transport().sample == 2560);  // the listener's position, not the renderer's
    engine.play();                             // resumes there
    engine.awaitTransport();
    runAhead(engine, backend, 4, 256);
    CHECK(engine.transport().sample == 2560 + 1024);
    engine.seek(1.0);  // while playing
    engine.awaitTransport();
    CHECK(engine.transport().sample == 48000);
    runAhead(engine, backend, 1, 256);
    CHECK(engine.transport().sample == 48000 + 256);
    CHECK(engine.anticipationMisses() == 0);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());  // the renderer stopped the plugins
}

TEST_CASE("anticipative: an edit takes effect after the audio already rendered, without a gap" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    runAhead(engine, backend, 10, 256);
    REQUIRE(engine.awaitRendered(1 << 20));  // 200 ms (9600 frames) ahead of 2560
    REQUIRE(engine.load(synthSession(-6.0206)));
    runAhead(engine, backend, 150, 256);
    engine.service();
    const auto& cap = backend.capture();
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[0][i])));
        return m;
    };
    // Note 1 (0 .. 12000) was rendered before the edit; note 2 (24000 ..) after.
    CHECK(peakOf(24000, 36000) / peakOf(2560, 12000) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(engine.anticipationMisses() == 0);
    CHECK(pool.size() == 1);  // the instance was kept
    engine.stop();
}

TEST_CASE("anticipative: a renderer that falls behind counts misses and plays silence") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    runAhead(engine, backend, 4, 256);
    engine.holdRenderer(true);
    REQUIRE(engine.awaitRendererHeld());
    backend.run(100);  // 25600 frames: more than was rendered ahead
    CHECK(engine.anticipationMisses() > 0);
    double tail = 0;
    for (std::size_t i = 25000; i < 26624; ++i)
        tail = std::max(tail, std::fabs(double(backend.capture().channels[0][i])));
    CHECK(tail == 0.0);
    engine.holdRenderer(false);
    runAhead(engine, backend, 10, 256);  // and it catches up
    engine.stop();
}

TEST_CASE("anticipative on a paced device: edits and transport from the main thread race-free" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
    Engine engine(pool, media, {.renderBlock = 512, .renderAheadMs = 50, .workers = 3});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(synthSession(0.0, "com.suppleaudio.test.gain")));
    engine.play();
    for (int i = 0; i < 40; ++i) {
        const auto until = backend.cycles() + 4;
        while (backend.cycles() < until) std::this_thread::yield();
        switch (i % 5) {
            case 0:
                REQUIRE(engine.load(synthSession(-0.5 * i, "com.suppleaudio.test.gain")));
                break;
            case 1:
                engine.seek(0.1 * i);
                break;
            case 2:
                engine.pause();
                break;
            case 3:
                engine.play();
                break;
            default:
                engine.service();
        }
    }
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK(engine.lastStopOnAudioThread());
}

TEST_CASE("anticipative: the renderer stops the plugins when the JACK server goes away") {
    FakeJack& fake = fakeJack();
    fake.reset(256);
    auto pool = makePool();
    MediaCache media;
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    JackBackend backend(o);
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(synthSession(0.0)));
    engine.play();
    const auto until = fake.cycles() + 5;
    while (fake.cycles() < until) std::this_thread::yield();
    fake.shutdown();
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
    CHECK_FALSE(engine.running());
    fake.reset();
}

// Phase 4 acceptance (engine.md §5): a crashing plugin in dedicated isolation
// does not take down the engine; only its node goes silent.
TEST_CASE("isolation: a plugin crashing in its bridge silences its node, the session plays on" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    Id tracks[2];
    for (int k = 0; k < 2; ++k) {
        model::Track tr;
        tr.name = k ? "Isolated" : "Steady";
        tr.parent = kMaster;
        tr.pan = k ? 100.0 : -100.0;  // steady left, isolated right
        tracks[k] = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tracks[k]); });
        std::vector<const char*> chain = {"com.suppleaudio.test.sine"};
        if (k) chain.push_back("com.suppleaudio.test.crash");
        for (const char* plugin : chain) {
            model::FxInstance fx;
            fx.plugin.id = plugin;
            if (k) fx.isolation = model::Isolation::Dedicated;
            const Id f = t.create(fx);
            t.modify<model::Track>(tracks[k], [&](model::Track& x) { x.chain.push_back(f); });
        }
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 8 * 960;
        src.sequence.notes.push_back(
            {0, 8 * 960, 0, static_cast<std::uint8_t>(60 + 7 * k), 100, 0});
        const Id mi = t.create(src);
        model::Take take;
        take.source = mi;
        const Id tk = t.create(take);
        model::Item item;
        item.track = tracks[k];
        item.length = 8.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    const model::Session s = t.commit().value();
    Id crashFx;
    for (const auto& e : s.fx)
        if (e.value->plugin.id == "com.suppleaudio.test.crash") crashFx = e.id;

    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(s));
    host::Instance* crash = pool.find(crashFx);
    REQUIRE(crash);
    CHECK(crash->isolated());
    engine.play();
    runAhead(engine, backend, 40, 256);  // 10240 frames: both tracks sound
    const auto& cap = backend.capture();
    auto peak = [&](int ch, std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[static_cast<std::size_t>(ch)][i])));
        return m;
    };
    CHECK(peak(0, 5000, 10240) > 0.1);
    CHECK(peak(1, 5000, 10240) > 0.1);
    REQUIRE(crash->setParam(1, 1.0));     // crash in its next process()
    runAhead(engine, backend, 200, 256);  // 51200 more frames
    CHECK(crash->failed());
    CHECK(pool.failed().size() == 1);
    CHECK(peak(0, 50000, 61440) > 0.1);   // the steady track plays on
    CHECK(peak(1, 50000, 61440) == 0.0);  // the crashed node is silent
    CHECK(engine.processErrors() == 0);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
}

TEST_CASE("plugin automation: real-time output equals the offline render" *
          doctest::description("rt")) {
    model::Session s = synthSession(0.0, "com.suppleaudio.test.gain");
    Id gainFx;
    for (const auto& e : s.fx)
        if (e.value->plugin.id == "com.suppleaudio.test.gain") gainFx = e.id;
    model::EditTxn t(s, 2);
    model::Envelope env;
    env.target = {gainFx, "gain"};
    env.points = {{0.0, 0.0, model::PointShape::Linear, 0.0},
                  {2.0, -30.0, model::PointShape::Slow, 0.0},
                  {3.0, 6.0, model::PointShape::Square, 0.0}};
    t.create(env);
    s = t.commit().value();
    auto pool = makePool();
    auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 3}}) {
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
        else
            backend.run((48000 * 2 + 4096) / 128);
        engine.stop();
        const std::uint32_t L = engine.outputLatency();
        const auto& cap = backend.capture();
        CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
    }
}

TEST_CASE("recording: every input frame lands in the take, placed where it was heard" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                          std::int64_t frame) {
                                  for (std::uint32_t c = 0; c < channels; ++c)
                                      for (std::uint32_t i = 0; i < n; ++i)
                                          in[c][i] = static_cast<float>((frame + i) % 1000) /
                                                     1000.0f * (c ? -1.0f : 1.0f);
                              }});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        const model::Session s = synthSession(0.0);
        REQUIRE(engine.load(s));
        Id track;
        for (const auto& e : s.tracks)
            if (e.value->type == model::StripType::Channel) track = e.id;
        const auto dir =
            std::filesystem::temp_directory_path() / ("supple-rec-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        REQUIRE(engine.startRecording({{track, {0, 1}, dir / "take.wav", {}}}));
        CHECK(engine.recording());
        engine.seek(1.0);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 100, 256);
        else
            backend.run(100);
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const RecordedTake& t = (*takes)[0];
        CHECK(t.startSample == 48000);  // no device or plan latency here
        CHECK(t.frames == 100 * 256);
        CHECK(t.overflowFrames == 0);
        auto audio = audio::readAudio(t.file);
        REQUIRE(audio);
        REQUIRE(audio->frames() == static_cast<std::size_t>(t.frames));
        int breaks = 0;
        for (std::size_t i = 1; i < audio->frames(); ++i) {
            const double d = double(audio->channels[0][i]) - double(audio->channels[0][i - 1]);
            if (std::fabs(d - 0.001) > 1e-5 && std::fabs(d + 0.999) > 1e-5)
                ++breaks;  // wraps at 1000
            if (audio->channels[1][i] != -audio->channels[0][i]) ++breaks;
        }
        CHECK(breaks == 0);     // contiguous: nothing lost, nothing doubled
        CHECK(t.hash.empty());  // header finalized; hashing belongs to the app worker
        engine.stop();
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("recording through a playback dropout keeps the take on the timeline" *
          doctest::description("rt")) {
    // The renderer stalls mid-take (a 384-frame block against 1024-frame
    // chunks, so one cycle is half played). Input that went with played
    // audio is recorded; input during the dropout had no timeline position
    // and is counted as lost, never recorded as if it had been heard.
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.5f;
         }});
    Engine engine(pool, media, EngineOptions{});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 384, .inputs = 2}));
    const model::Session s = synthSession(0.0);
    REQUIRE(engine.load(s));
    Id track;
    for (const auto& e : s.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-gap-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    REQUIRE(engine.startRecording({{track, {0}, dir / "take.wav", {}}}));
    engine.play();
    runAhead(engine, backend, 20, 384);
    engine.holdRenderer(true);
    REQUIRE(engine.awaitRendererHeld());
    backend.run(80);  // 30720 frames: more than was rendered ahead
    engine.holdRenderer(false);
    runAhead(engine, backend, 20, 384);
    CHECK(engine.anticipationMisses() > 0);
    auto takes = engine.stopRecording();
    REQUIRE(takes);
    const RecordedTake& t = (*takes)[0];
    CHECK(t.startSample == 0);
    CHECK(t.overflowFrames > 0);
    CHECK(t.frames + static_cast<std::int64_t>(t.overflowFrames) == 120 * 384);
    CHECK(t.frames == engine.transport().sample);  // exactly what was heard
    engine.stop();
    std::filesystem::remove_all(dir);
}

namespace {
// `s` with its first track monitoring device inputs 1-2 (monitor on, armed:
// monitoring needs the arm).
model::Session monitoring(model::Session s, model::Monitor mode = model::Monitor::On,
                          bool armed = true) {
    model::EditTxn t(s, 9);
    Id track;
    for (const auto& e : s.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    model::Route r;
    r.from.hwChannels = std::vector<int>{1, 2};
    r.to.track = track;
    const Id in = t.create(r);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.record.input = in;
        x.record.monitor = mode;
        x.record.armed = armed;
    });
    return t.commit().value();
}

// Device inputs: a constant 0.5 on input 1 and -0.25 on input 2.
DummyBackend::Options constantInput(std::int64_t captureFrames) {
    return {.mode = DummyBackend::Mode::Manual,
            .captureFrames = captureFrames,
            .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    in[0][i] = 0.5f;
                    if (channels > 1) in[1][i] = -0.25f;
                }
            }};
}
}  // namespace

namespace {
// Two synths into the main master; the first also feeds a cue master with no
// device outputs (ADR-0056/0057). Both masters armed.
struct MasterMix {
    model::Session session;
    Id cue, first, second;
};
MasterMix masterMix() {
    model::Session base = synthSession(-3.0, "com.suppleaudio.test.gain", 2);
    MasterMix m;
    for (const auto& e : base.tracks)
        if (e.value->type == model::StripType::Channel) (m.first.n ? m.second : m.first) = e.id;
    model::EditTxn t(base, 2);
    model::Track cue;
    cue.type = model::StripType::Master;
    cue.name = "Cue";
    cue.record.armed = true;
    m.cue = t.create(cue);
    auto project = t.project();
    project.masters.push_back(m.cue);
    t.setProject(project);
    t.modify<model::Track>(kMaster, [](model::Track& x) {
        x.record.armed = true;
        x.volumeDb = -2;
        x.pan = 30;
    });
    model::Route send;
    send.from.track = m.first;
    send.to.track = m.cue;
    t.create(send);
    m.session = t.commit().value();
    return m;
}
}  // namespace

TEST_CASE("recording a master: its take is render.mix of that master, in every executor" *
          doctest::description("rt")) {
    const MasterMix mix = masterMix();
    auto pool = makePool();
    std::map<Id, audio::AudioData> offline;
    for (Id master : {kMaster, mix.cue})
        offline[master] =
            renderOffline(mix.session, pool, {.master = master, .startSeconds = 0, .endSeconds = 2})
                .value()
                .audio;
    CHECK(maxDiff(offline[kMaster].channels[0], 0, offline[mix.cue].channels[0], 96000) > 0);
    struct Run {
        EngineOptions options;
        bool live;  // a monitored (silent) input: the callback runs tr_0, the renderer the cue
    };
    for (const Run& run :
         {Run{kInCallback, false}, Run{EngineOptions{}, false}, Run{EngineOptions{}, true}}) {
        INFO("anticipative " << run.options.anticipative << ", live " << run.live);
        const model::Session s = run.live ? monitoring(mix.session) : mix.session;
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(rtPool, media, run.options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(s));
        const auto dir = std::filesystem::temp_directory_path() /
                         ("supple-master-rec-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        RecordTarget main{kMaster, {}, dir / "main.wav", {}, true};
        RecordTarget cue{mix.cue, {}, dir / "cue.wav", {}, true};
        REQUIRE(engine.startRecording({main, cue}));
        engine.play();
        const std::int64_t cycles = (48000 * 2 + 4096) / 128;
        if (run.options.anticipative)
            runAhead(engine, backend, cycles, 128);
        else
            backend.run(cycles);
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 2);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(engine.processErrors() == 0);
        const std::int64_t L = engine.outputLatency();
        CHECK(L == 64);
        for (const RecordedTake& take : *takes) {
            INFO(take.track.str());
            CHECK(take.overflowFrames == 0);
            CHECK(take.startSample == -L);  // the play position minus the plan's latency
            auto audio = audio::readAudio(take.file);
            REQUIRE(audio);
            REQUIRE(audio->channels.size() == 2);
            REQUIRE(audio->frames() >= static_cast<std::size_t>(L + 96000));
            const auto& want = offline[take.track];
            for (int c = 0; c < 2; ++c)
                CHECK(maxDiff(audio->channels[static_cast<std::size_t>(c)],
                              static_cast<std::size_t>(L),
                              want.channels[static_cast<std::size_t>(c)], 96000) == 0.0);
        }
        engine.stop();
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("an armed master that is not recording keeps its record ring drained" *
          doctest::description("rt")) {
    const MasterMix mix = masterMix();
    auto pool = makePool();
    const auto offline =
        renderOffline(mix.session, pool, {.startSeconds = 0, .endSeconds = 2}).value();
    auto rtPool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, EngineOptions{});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(mix.session));
    engine.play();
    runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
    engine.stop();
    CHECK(engine.anticipationMisses() == 0);
    CHECK(maxDiff(backend.capture().channels[0], 64, offline.audio.channels[0], 96000) == 0.0);
}

TEST_CASE("a recording master: its prints are silent in the punch range and never recorded" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-master-punch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(96000));
    for (std::size_t i = 0; i < 96000; ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.25 * std::sin(double(i) * 0.003));
    REQUIRE(audio::writeWav(dir / "media" / "print.wav", tone));
    const MasterMix mix = masterMix();
    model::EditTxn t(mix.session, 3);
    const Id md = t.create(model::MediaSource{"print.wav", "", "wav", 48000, 2, 96000});
    model::Take take;
    take.source = md;
    const Id tk = t.create(take);
    model::Item print;  // an earlier print over the whole span: 0 .. 2 s
    print.track = kMaster;
    print.length = 4.0;
    print.takes = {tk};
    print.activeTake = tk;
    t.create(print);
    const model::Session s = t.commit().value();
    model::EditTxn without(s, 4);
    for (const auto& e : s.items)
        if (e.value->track == kMaster) {
            without.remove<model::Item>(e.id);
            for (Id x : e.value->takes) without.remove<model::Take>(x);
        }
    const model::Session dry = without.commit().value();
    auto pool = makePool();
    MediaCache offlineMedia;
    auto renderOf = [&](const model::Session& session) {
        return renderOffline(
                   session, pool,
                   {.startSeconds = 0, .endSeconds = 2, .projectDir = dir, .media = &offlineMedia})
            .value()
            .audio;
    };
    const auto withPrint = renderOf(s), live = renderOf(dry);
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        INFO("anticipative " << options.anticipative);
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        // As transport.record does: punch 0.5 .. 1.5 s, the plan first.
        engine.setRecordingMasters({{kMaster, false, 0.5, 1.5}});
        REQUIRE(engine.load(s, dir));
        const auto file = dir / "rec" / (options.anticipative ? "ahead.wav" : "callback.wav");
        REQUIRE(engine.startRecording({{kMaster, {}, file, {}, true}}));
        engine.play();
        const std::int64_t cycles = (48000 * 2 + 4096) / 128;
        if (options.anticipative)
            runAhead(engine, backend, cycles, 128);
        else
            backend.run(cycles);
        auto takes = engine.stopRecording();
        engine.stop();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const auto L = static_cast<std::size_t>(engine.outputLatency());
        auto audio = audio::readAudio((*takes)[0].file);
        REQUIRE(audio);
        // The take never holds the master's own items; what plays does, outside the punch.
        CHECK(maxDiff(audio->channels[0], L, live.channels[0], 96000) == 0.0);
        const auto& heard = backend.capture().channels[0];
        CHECK(maxDiff(heard, L, withPrint.channels[0], 24000) == 0.0);
        double inside = 0;
        for (std::size_t i = 24000; i < 72000; ++i)
            inside =
                std::max(inside, std::fabs(double(heard[L + i]) - double(live.channels[0][i])));
        CHECK(inside == 0.0);
        double after = 0;
        for (std::size_t i = 72000; i < 96000; ++i)
            after =
                std::max(after, std::fabs(double(heard[L + i]) - double(withPrint.channels[0][i])));
        CHECK(after == 0.0);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("monitoring in the callback: the input is heard through the track, stopped or playing" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(constantInput(48000));
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    // The sine track, monitoring; its notes are silent at 1.75 s (between notes).
    REQUIRE(engine.load(monitoring(synthSession(-6.0206))));
    backend.run(4);  // stopped: input only, through the fader (-6 dB) and centre pan (-3 dB)
    const auto& cap = backend.capture();
    const double g = 0.5;
    CHECK(double(cap.channels[0][600]) == doctest::Approx(0.5 * g).epsilon(1e-4));
    CHECK(double(cap.channels[1][600]) == doctest::Approx(-0.25 * g).epsilon(1e-4));
    CHECK(engine.transport().sample == 0);  // stopped: time stands still
    engine.seek(1.75);
    engine.play();
    backend.run(4);
    CHECK(double(cap.channels[0][1024 + 600]) == doctest::Approx(0.5 * g).epsilon(1e-4));
    CHECK(engine.transport().sample == static_cast<std::int64_t>(1.75 * 48000) + 1024);
    engine.stop();
}

namespace {
TEST_CASE("monitoring needs the arm; auto hears the input stopped, the timeline playing" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(constantInput(48000));
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    const auto& cap = backend.capture();
    const double g = 0.5;
    // Monitor on, not armed: nothing is heard.
    REQUIRE(engine.load(monitoring(synthSession(-6.0206), model::Monitor::On, false)));
    backend.run(4);
    CHECK(double(cap.channels[0][600]) == 0.0);
    // Auto, armed: heard while stopped...
    REQUIRE(engine.load(monitoring(synthSession(-6.0206), model::Monitor::Auto, true)));
    backend.run(4);
    CHECK(double(cap.channels[0][1024 + 600]) == doctest::Approx(0.5 * g).epsilon(1e-4));
    // ...and not while playing without recording (1.75 s is between the notes).
    engine.seek(1.75);
    engine.play();
    backend.run(4);
    CHECK(double(cap.channels[0][2048 + 600]) == 0.0);
    engine.stop();
}

// The sine track plus a "Vocal" track (a gain plugin at -6 dB) that monitors
// device inputs 1-2 when `monitored`.
model::Session withVocal(bool monitored) {
    model::Session s = synthSession(0.0);
    model::EditTxn t(s, 8);
    model::Track tr;
    tr.name = "Vocal";
    tr.parent = kMaster;
    tr.pan = 30.0;
    const Id vocal = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(vocal); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    fx.patch =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"gain": -6}})").value();
    const Id f = t.create(fx);
    t.modify<model::Track>(vocal, [&](model::Track& x) { x.chain.push_back(f); });
    model::Route r;
    r.from.hwChannels = std::vector<int>{1, 2};
    r.to.track = vocal;
    const Id in = t.create(r);
    t.modify<model::Track>(vocal, [&](model::Track& x) {
        x.record.input = in;
        x.record.monitor = monitored ? model::Monitor::On : model::Monitor::Off;
        x.record.armed = monitored;
    });
    return t.commit().value();
}

// Device input: a slow ramp on input 1, its negative on input 2.
DummyBackend::Options rampInput(std::int64_t captureFrames) {
    return {
        .mode = DummyBackend::Mode::Manual,
        .captureFrames = captureFrames,
        .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t frame) {
            for (std::uint32_t i = 0; i < n; ++i) {
                const float v = static_cast<float>((frame + i) % 4800) / 4800.0f - 0.5f;
                in[0][i] = v;
                if (channels > 1) in[1][i] = -v;
            }
        }};
}
}  // namespace

TEST_CASE("anticipative monitoring: live input and rendered playback, identical to the reference" *
          doctest::description("rt")) {
    model::Session s = withVocal(true);
    model::EditTxn mode(s, 10);
    auto project = mode.project();
    project.console9 = true;
    project.seed = 5723;
    mode.setProject(project);
    // The main master on outputs 3/4 (ADR-0056): the live executor writes them.
    mode.modify<model::Track>(kMaster, [](auto& m) { m.hwOutput = std::vector<int>{3, 4}; });
    s = mode.commit().value();
    constexpr std::int64_t kCycles = 400;  // 102400 frames at 256
    audio::AudioData reference;
    std::optional<Id> vocalTrack;
    for (const auto& e : s.tracks)
        if (e.value->name == "Vocal") vocalTrack = e.id;
    REQUIRE(vocalTrack.has_value());
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 3}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(rampInput(kCycles * 256));
        BenchProbe probe(10000);
        probe.recording = true;
        auto measuredOptions = options;
        measuredOptions.benchProbe = &probe;
        Engine engine(pool, media, measuredOptions);
        REQUIRE(engine.start(backend,
                             {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .outputs = 4}));
        REQUIRE(engine.load(s));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, kCycles, 256);
        else
            backend.run(kCycles);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(engine.processErrors() == 0);
        CHECK(engine.transport().sample == kCycles * 256);
        // The live strip's meter reports the monitored input it has heard —
        // pushed by the callback in both modes (P5.3c).
        const auto bank = engine.meterBank();
        REQUIRE(bank != nullptr);
        const Engine::MeterClock clock = engine.meterClock();
        double vocal = 0.0;
        for (const MeterReading& r : bank->read(clock.epoch, clock.heardStream))
            if (r.track == *vocalTrack)
                vocal = std::max(
                    {vocal, static_cast<double>(r.peak[0]), static_cast<double>(r.peak[1])});
        CHECK(vocal > 0.05);
        engine.stop();
        CHECK(engine.lastStopOnAudioThread());
        if (options.anticipative) {
            CHECK(probe.aheadObservations > 0);
            CHECK(probe.aheadLowWater >= 0);
            CHECK_FALSE(probe.renderJob.samples().empty());
        }
        const auto& cap = backend.capture();
        double live = 0;
        double unused = 0;
        for (std::size_t i = 60000; i < 70000; ++i) {
            live = std::max(live, std::fabs(double(cap.channels[3][i])));
            unused = std::max(unused, std::fabs(double(cap.channels[1][i])));
        }
        CHECK(live > 0.05);  // the ramp, through the vocal chain
        CHECK(unused == 0.0);
        if (!options.anticipative) {
            reference = cap;
        } else {
            for (std::size_t c = 0; c < 4; ++c) CHECK(cap.channels[c] == reference.channels[c]);
        }
    }
}

TEST_CASE("anticipative monitoring: heard while stopped; arming mid-play hands the plugin over" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(rampInput(96000));
    Engine engine(pool, media, {.workers = 2});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    REQUIRE(engine.load(withVocal(true)));
    REQUIRE(engine.awaitPlan());
    backend.run(20);  // stopped: the vocal is heard, nothing else plays
    const auto& cap = backend.capture();
    double stopped = 0;
    for (std::size_t i = 2560; i < 5120; ++i)
        stopped = std::max(stopped, std::fabs(double(cap.channels[0][i])));
    CHECK(stopped > 0.05);
    CHECK(engine.transport().sample == 0);
    // Play, then turn monitoring off and on again while playing: the gain
    // plugin moves between the callback and the renderer.
    engine.play();
    runAhead(engine, backend, 40, 256);
    for (bool monitored : {false, true, false, true}) {
        REQUIRE(engine.load(withVocal(monitored)));
        runAhead(engine, backend, 60, 256);
        engine.service();
    }
    CHECK(engine.processErrors() == 0);
    CHECK(engine.transport().sample == (40 + 4 * 60) * 256);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
}

// A plug-in's latency mode (Ferric's and Plateau's "Low Latency"): changing
// it while the engine runs makes the plug-in request a restart, which waits
// for processing to stop. The engine takes it off its executors, restarts the
// same instance and compensates its new latency.
TEST_CASE("a plug-in that asks to restart while processing gets restarted with its new latency" *
          doctest::description("rt")) {
    for (const bool anticipative : {false, true}) {
        INFO("anticipative " << anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(rampInput(96000));
        Engine engine(pool, media, anticipative ? EngineOptions{.workers = 2} : kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        auto withLatency = [](const char* choice) {
            auto s = synthSession(0, "com.suppleaudio.test.gain");
            model::EditTxn t(s, 5);
            for (const auto& e : s.fx)
                if (e.value->plugin.id == "com.suppleaudio.test.gain")
                    t.modify<model::FxInstance>(e.id, [&](model::FxInstance& fx) {
                        fx.patch =
                            json::parse(
                                std::string(
                                    R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": ")") +
                                choice + R"("}})")
                                .value();
                    });
            return t.commit().value();
        };
        const auto none = withLatency("none");
        Id gain;
        for (const auto& e : none.fx)
            if (e.value->plugin.id == "com.suppleaudio.test.gain") gain = e.id;
        REQUIRE(engine.load(none));
        REQUIRE(engine.awaitPlan());
        engine.play();
        auto run = [&](int cycles) {
            if (anticipative)
                runAhead(engine, backend, cycles, 256);
            else
                backend.run(cycles);
            engine.service();
        };
        run(20);
        host::Instance* instance = pool.find(gain);
        REQUIRE(instance);
        CHECK(instance->latency() == 0);
        CHECK(instance->processing());
        // The mode changes while playing: the plug-in asks for a restart.
        REQUIRE(engine.load(withLatency("short")));
        for (int i = 0; i < 40 && instance->latency() != 64; ++i) run(4);
        CHECK(pool.find(gain) == instance);  // restarted, not recreated
        CHECK(instance->latency() == 64);
        CHECK(pool.rememberedLatency(gain) == 64);
        CHECK_FALSE(pool.restarting(gain));
        // Back on its executor, processing, with the new compensation.
        for (int i = 0; i < 20 && !instance->processing(); ++i) run(4);
        CHECK(instance->processing());
        CHECK(engine.processErrors() == 0);
        engine.stop();
    }
}

// --- the transport loop (ADR-0015) --------------------------------------------------------------

TEST_CASE("loop playback wraps without a gap: the output is the offline render, unrolled" *
          doctest::description("rt")) {
    // Loop 1 s .. 2 s (beats 2..4: two notes); play from 0 for 3.5 s. Blocks
    // of 256 and chunks of 1024 do not divide the loop, so wraps fall
    // mid-block and chunks are cut short at the loop end.
    constexpr std::int64_t kFrames = 168000;
    auto timelineOf = [](std::int64_t s) { return s < 96000 ? s : 48000 + (s - 96000) % 48000; };
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}})
        for (const char* extra : {static_cast<const char*>(nullptr), "com.suppleaudio.test.gain"}) {
            INFO("anticipative " << options.anticipative << ", workers " << options.workers
                                 << ", latency " << (extra != nullptr));
            const model::Session s = synthSession(-3.0, extra);
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = kFrames + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
            REQUIRE(engine.load(s));
            engine.setLoop(1.0, 2.0, true);
            engine.play();
            const std::int64_t cycles = (kFrames + 4096) / 256;
            if (options.anticipative)
                runAhead(engine, backend, cycles, 256);
            else
                backend.run(cycles);
            CHECK(engine.anticipationMisses() == 0);
            CHECK(engine.processErrors() == 0);
            const TransportState t = engine.transport();
            CHECK(t.loop.active());
            CHECK(t.sample == timelineOf(cycles * 256));  // the listener's position wrapped
            engine.stop();
            const std::int64_t L = engine.outputLatency();
            const auto& cap = backend.capture();
            double worst = 0;
            std::int64_t at = -1;
            for (std::int64_t j = L; j < kFrames; ++j)
                for (int c = 0; c < 2; ++c) {
                    const double d = std::fabs(
                        double(cap.channels[c][static_cast<std::size_t>(j)]) -
                        double(offline.audio
                                   .channels[c][static_cast<std::size_t>(timelineOf(j - L))]));
                    if (d > worst) {
                        worst = d;
                        at = j;
                    }
                }
            INFO("worst at " << at);
            CHECK(worst == 0.0);
        }
}

TEST_CASE("a note held across the loop end is released at the wrap" * doctest::description("rt")) {
    // One note from 0 to 2 s; loop 0.5 s .. 1 s. The note started before the
    // loop, so after the first wrap nothing plays it again: silence.
    model::Session s = synthSession(0.0);
    {
        model::EditTxn t(s, 2);
        for (const auto& e : s.midi)
            t.modify<model::MidiSource>(e.id, [](model::MidiSource& m) {
                m.sequence.notes = {{0, 4 * 960, 0, 60, 100, 0}};
            });
        s = t.commit().value();
    }
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
        REQUIRE(engine.load(s));
        engine.setLoop(0.5, 1.0, true);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 96000 / 256, 256);
        else
            backend.run(96000 / 256);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        double before = 0, after = 0;
        for (std::size_t i = 30000; i < 47000; ++i)
            before = std::max(before, std::fabs(double(L[i])));
        for (std::size_t i = 49000; i < 90000; ++i)
            after = std::max(after, std::fabs(double(L[i])));
        CHECK(before > 0.1);
        CHECK(after == 0.0);
    }
}

TEST_CASE("anticipative monitoring with a loop: identical to the in-callback reference" *
          doctest::description("rt")) {
    model::Session s = withVocal(true);
    model::EditTxn mode(s, 10);
    auto project = mode.project();
    project.console9 = true;
    project.seed = 5723;
    mode.setProject(project);
    s = mode.commit().value();
    constexpr std::int64_t kCycles = 400;  // 102400 frames: several passes of 0.5 .. 1.2 s
    audio::AudioData reference;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 3}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(rampInput(kCycles * 256));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        REQUIRE(engine.load(s));
        engine.setLoop(0.5, 1.2, true);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, kCycles, 256);
        else
            backend.run(kCycles);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(engine.processErrors() == 0);
        const std::int64_t played = kCycles * 256;
        CHECK(engine.transport().sample == 24000 + (played - 24000) % 33600);
        engine.stop();
        const auto& cap = backend.capture();
        if (!options.anticipative) {
            reference = cap;
        } else {
            CHECK(cap.channels[0] == reference.channels[0]);
            CHECK(cap.channels[1] == reference.channels[1]);
        }
    }
}

TEST_CASE("loop recording: one contiguous file, a run per pass, placed on the timeline" *
          doctest::description("rt")) {
    // Loop 0.5 s .. 1 s, recording from 0.25 s: the first run is heard from
    // 0.25 s, every later one from the loop start. The input counts device
    // frames, so the file shows exactly what was captured.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                          std::int64_t frame) {
                                  for (std::uint32_t c = 0; c < channels; ++c)
                                      for (std::uint32_t i = 0; i < n; ++i)
                                          in[c][i] = static_cast<float>(frame + i);
                              }});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        const model::Session s = synthSession(0.0);
        REQUIRE(engine.load(s));
        Id track;
        for (const auto& e : s.tracks)
            if (e.value->type == model::StripType::Channel) track = e.id;
        const auto dir = std::filesystem::temp_directory_path() /
                         ("supple-rec-loop-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        engine.setLoop(0.5, 1.0, true);
        engine.seek(0.25);
        REQUIRE(engine.startRecording({{track, {0}, dir / "take.wav", {}}}));
        engine.play();
        constexpr std::int64_t kCycles = 400;  // 102400 frames: 36000 + 24000 * 2 + 18400
        if (options.anticipative)
            runAhead(engine, backend, kCycles, 256);
        else
            backend.run(kCycles);
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        const RecordedTake& t = (*takes)[0];
        CHECK(t.frames == kCycles * 256);
        CHECK(t.loop.start == 24000);
        CHECK(t.loop.end == 48000);
        REQUIRE(t.segments.size() == 4);
        const std::int64_t want[4][2] = {
            {0, 12000}, {36000, 24000}, {60000, 24000}, {84000, 24000}};
        for (int k = 0; k < 4; ++k) {
            CHECK(t.segments[static_cast<std::size_t>(k)].fileFrame == want[k][0]);
            CHECK(t.segments[static_cast<std::size_t>(k)].timeline == want[k][1]);
        }
        auto audio = audio::readAudio(t.file);
        REQUIRE(audio);
        REQUIRE(audio->frames() == static_cast<std::size_t>(t.frames));
        std::size_t breaks = 0;  // contiguous device input, nothing lost or doubled
        for (std::size_t i = 1; i < audio->frames(); ++i)
            if (audio->channels[0][i] != audio->channels[0][i - 1] + 1.0f) ++breaks;
        CHECK(breaks == 0);
        engine.stop();
        std::filesystem::remove_all(dir);
    }
}

// --- MIDI input (ADR-0016)
// ------------------------------------------------------------------------

namespace {
// A sine instrument track that monitors (and may record) every MIDI device.
model::Session keyboardSession(bool armed = false) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Keys";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    model::Route in;
    in.from.midiDevice = "key";  // matches the "Keyboard" port
    in.to.track = track;
    in.midi = model::MidiRouting{1, 0};  // channel 1 only
    const Id r = t.create(in);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.chain.push_back(f);
        x.record.input = r;
        x.record.armed = true;  // monitoring needs the arm; auto when it records
        x.record.monitor = armed ? model::Monitor::Auto : model::Monitor::On;
    });
    return t.commit().value();
}

// Device MIDI at fixed device frames: {frame, status, data1, data2}.
DummyBackend::Options keyboard(std::vector<std::array<int, 4>> events, std::int64_t capture) {
    return {
        .mode = DummyBackend::Mode::Manual,
        .captureFrames = capture,
        .midiPorts = {"Keyboard", "Pads"},
        .midi = [events](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
            for (const auto& e : events)
                if (e[0] >= frame && e[0] < frame + n) {
                    MidiInputEvent m;
                    m.frame = static_cast<std::uint32_t>(e[0] - frame);
                    m.size = 3;
                    m.bytes[0] = static_cast<std::uint8_t>(e[1]);
                    m.bytes[1] = static_cast<std::uint8_t>(e[2]);
                    m.bytes[2] = static_cast<std::uint8_t>(e[3]);
                    out.push_back(m);
                }
        }};
}
}  // namespace

TEST_CASE("MIDI monitoring: a keyboard plays the instrument live, sample-accurately" *
          doctest::description("rt")) {
    // Note on at device frame 1000, off at 20000; a note on channel 2 and one
    // from the other port are filtered out by the route.
    const std::vector<std::array<int, 4>> events = {
        {1000, 0x90, 60, 100}, {5000, 0x91, 67, 100}, {20000, 0x80, 60, 0}, {25000, 0x90, 72, 100}};
    for (const bool playing : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("playing " << playing << ", anticipative " << options.anticipative);
            auto pool = makePool();
            MediaCache media;
            auto opts = keyboard(events, 30720);
            opts.midi = [inner = opts.midi](std::vector<MidiInputEvent>& out, std::uint32_t n,
                                            std::int64_t frame) {
                inner(out, n, frame);
                for (auto& m : out)
                    if (m.bytes[1] == 72) m.port = 1;  // the note at 25000 comes from "Pads"
            };
            DummyBackend backend(opts);
            Engine engine(pool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
            REQUIRE(engine.load(keyboardSession()));
            REQUIRE(engine.awaitPlan());  // everything is live: nothing renders ahead
            if (playing) engine.play();
            if (playing) engine.awaitTransport();
            backend.run(120);
            engine.stop();
            const auto& L = backend.capture().channels[0];
            auto peakIn = [&](std::size_t from, std::size_t to) {
                double m = 0;
                for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(L[i])));
                return m;
            };
            CHECK(peakIn(0, 1000) == 0.0);
            CHECK(peakIn(1000, 1010) > 0.0);  // starts at its frame
            CHECK(peakIn(2000, 19000) > 0.1);
            CHECK(peakIn(20000, 30720) == 0.0);  // off at 20000; channel 2 and "Pads" never sound
        }
}

TEST_CASE("a live processing error silences the failed device block" * doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        const auto session = monitoring(synthSession());
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 384});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(session));
        if (options.anticipative) REQUIRE(engine.awaitPlan());
        engine.play();
        if (options.anticipative) engine.awaitTransport();
        backend.run(2);
        // Inject a process error in the in-tree fixture on the manual audio thread.
        std::vector<std::pair<clap_plugin_t*, decltype(clap_plugin_t::process)>> processes;
        for (const auto& fx : session.fx)
            if (auto* instance = pool.find(fx.id)) {
                auto* plugin = const_cast<clap_plugin_t*>(instance->raw());
                REQUIRE(plugin);
                processes.emplace_back(plugin, plugin->process);
                plugin->process = [](const clap_plugin_t*, const clap_process_t*) {
                    return clap_process_status{CLAP_PROCESS_ERROR};
                };
            }
        backend.run(1);
        for (const auto& [plugin, process] : processes) plugin->process = process;
        engine.stop();
        CHECK(engine.processErrors() == 1);
        const auto& captured = backend.capture().channels[0];
        CHECK(std::any_of(captured.begin() + 128, captured.begin() + 256,
                          [](float sample) { return std::abs(sample) > 0.01f; }));
        CHECK(std::all_of(captured.begin() + 256, captured.end(),
                          [](float sample) { return sample == 0; }));
    }
}

namespace {
// keyboardSession's track, its instrument `plugin`, holding one timeline
// note (key 60) for eight beats, 4 s at 120 bpm, with the given arm and
// monitor mode; the master playrate `rate`.
model::Session heldNoteSession(bool armed, model::Monitor monitor, const std::string& plugin,
                               double rate = 1.0, bool preserve = false) {
    const model::Session base = keyboardSession();
    model::EditTxn t(base, 2);
    auto project = t.project();
    project.playrate = {rate, preserve};
    t.setProject(project);
    Id track;
    for (const auto& e : base.tracks)
        if (e.value->name == "Keys") track = e.id;
    for (const auto& e : base.fx)
        t.modify<model::FxInstance>(e.id, [&](model::FxInstance& fx) { fx.plugin.id = plugin; });
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.record.armed = armed;
        x.record.monitor = monitor;
    });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 8 * 960;
    src.sequence.notes = {{0, 8 * 960, 0, 60, 100, 0}};
    model::Take take;
    take.source = t.create(src);
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 8.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

// Runs `frames` device frames (rounded up to whole callbacks), the
// anticipative engine kept ahead as a device's clock would.
void runFrames(Engine& engine, DummyBackend& backend, const EngineOptions& options,
               std::int64_t frames, std::uint32_t block) {
    const std::int64_t cycles = (frames + block - 1) / block;
    if (options.anticipative)
        runAhead(engine, backend, cycles, block);
    else
        backend.run(cycles);
}

// The instrument on twoKeysSession's "Keys" track.
Id keysInstrument(const model::Session& s) {
    for (const auto& e : s.tracks)
        if (e.value->name == "Keys") return e.value->chain.front();
    return {};
}

const std::array<EngineOptions, 4> kBothExecutors = {
    kInCallback, EngineOptions{.anticipative = false, .workers = 2}, EngineOptions{.workers = 0},
    EngineOptions{.workers = 2}};
}  // namespace

// Arming, a monitor change or disarming while the song plays moves the
// instrument between the anticipative engine's renderer and its live region;
// the timeline note it holds keeps sounding, exactly as offline, at unity and
// under a master playrate, and a key held on the keyboard ends when the
// keyboard stops being heard.
TEST_CASE("arming or monitoring mid-song keeps an instrument's held timeline note" *
          doctest::description("rt")) {
    constexpr std::int64_t kSegment = 80 * 256;  // device frames between session changes
    // A change takes effect within this (the anticipative engine's at its render horizon).
    constexpr std::int64_t kSettle = 48 * 256;
    struct Step {
        bool armed;
        model::Monitor monitor;
    };
    const std::array<Step, 5> steps = {{{false, model::Monitor::On},
                                        {true, model::Monitor::On},
                                        {true, model::Monitor::Auto},
                                        {true, model::Monitor::On},
                                        {false, model::Monitor::On}}};
    constexpr std::int64_t kFrames = 5 * kSegment;
    // Keys held on the keyboard, while monitored: one ended by Auto, one by disarming.
    constexpr std::int64_t kFirst = kSegment + kSettle + 512,
                           kSecond = 3 * kSegment + kSettle + 512;
    struct Rate {
        double rate;
        bool preserve;
    };
    // A CLAP and a VST3 instrument: their reset and processing calls differ.
    auto scanned = host::scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(scanned);
    const auto vst3 = std::find_if(scanned->begin(), scanned->end(), [](const host::PluginInfo& p) {
        return p.name == "Test VST3 Sine";
    });
    REQUIRE(vst3 != scanned->end());
    for (const std::string& plugin : {std::string("com.suppleaudio.test.sine"), vst3->id})
        for (const Rate r : {Rate{1.0, false}, Rate{1.25, false}, Rate{1.25, true}}) {
            auto pool = makeVstPool();
            // Offline output is in device frames: 3 s of timeline is 2.4 s at 1.25.
            const auto offline = renderOffline(heldNoteSession(false, model::Monitor::On, plugin,
                                                               r.rate, r.preserve),
                                               pool, {.startSeconds = 0, .endSeconds = 3.0})
                                     .value();
            const auto& want = offline.audio.channels[0];
            REQUIRE(want.size() >= static_cast<std::size_t>(kFrames + 1024));
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO(plugin << ", rate " << r.rate << ", preserve " << r.preserve << ", block "
                                << block << ", anticipative " << options.anticipative
                                << ", workers " << options.workers);
                    auto rtPool = makeVstPool();
                    MediaCache media;
                    DummyBackend backend(keyboard({{static_cast<int>(kFirst), 0x90, 76, 100},
                                                   {static_cast<int>(kSecond), 0x90, 72, 100}},
                                                  kFrames + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    const auto session = [&](const Step& s) {
                        return heldNoteSession(s.armed, s.monitor, plugin, r.rate, r.preserve);
                    };
                    REQUIRE(engine.load(session(steps[0])));
                    REQUIRE(engine.awaitPlan());
                    engine.play();
                    for (std::size_t i = 0; i < steps.size(); ++i) {
                        if (i) {
                            REQUIRE(engine.load(session(steps[i])));
                            // At unity, while everything is live, the renderer keeps a
                            // render-ahead past what the callback has played when it
                            // next runs, and hands over there. Waiting keeps that within
                            // 200 ms (below kSettle) of the change, however late a busy
                            // machine runs the renderer thread; a device's clock would
                            // pace it. A rate plan's handoff is fixed when it is loaded,
                            // and adopted only once played up to, so it is not awaited.
                            if (r.rate == 1.0) REQUIRE(engine.awaitPlan());
                        }
                        runFrames(engine, backend, options, kSegment, block);
                        engine.service();
                    }
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    REQUIRE(engine.outputLatency() == 0);
                    const auto& L = backend.capture().channels[0];
                    // The output equals the offline render from `from` to `to`.
                    const auto offlineFrom = [&](std::int64_t from, std::int64_t to) {
                        const auto a = static_cast<std::size_t>(from);
                        return maxDiff(
                            L, a,
                            std::vector<float>(want.begin() + static_cast<std::ptrdiff_t>(from),
                                               want.end()),
                            static_cast<std::size_t>(to - from));
                    };
                    const auto keyHeard = [&](std::int64_t at) {
                        const auto i = static_cast<std::size_t>(at + 100);
                        return std::fabs(double(L[i]) - double(want[i])) > 0.0;
                    };
                    // Arming keeps the timeline note; the first key only adds to it.
                    CHECK(offlineFrom(0, kFirst) == 0.0);
                    CHECK(keyHeard(kFirst));
                    // Auto does not hear the keyboard while playing: its key ends, the
                    // note plays on through Auto and On again until the second key.
                    CHECK(offlineFrom(2 * kSegment + kSettle, kSecond) == 0.0);
                    CHECK(keyHeard(kSecond));
                    // Disarmed: the second key ends, the timeline note still sounds.
                    CHECK(offlineFrom(4 * kSegment + kSettle, kFrames) == 0.0);
                    CHECK(std::fabs(double(L[static_cast<std::size_t>(kFrames - 100)])) > 0.0);
                }
        }
}

namespace {
// A track with keyboardSession's MIDI input, a sine and the edge fixture as a
// 1 s echo (a delay with pdc: ignore, so the delay is heard as a tail): one
// short note at 0, heard again at 1 s. With `pads`, a second sine track,
// "Pads", on the same input, armed and monitored, and silent, so the live
// region exists before the echo strip joins it.
model::Session echoSession(bool armed, model::Monitor monitor, bool pads = false) {
    const model::Session base = keyboardSession();
    model::EditTxn t(base, 2);
    Id track;
    for (const auto& e : base.tracks)
        if (e.value->name == "Keys") track = e.id;
    if (pads) {
        model::Track tr;
        tr.name = "Pads";
        tr.parent = kMaster;
        const Id p = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(p); });
        model::FxInstance sine;
        sine.plugin.id = "com.suppleaudio.test.sine";
        const Id f = t.create(sine);
        model::Route in;
        in.from.midiDevice = "key";
        in.to.track = p;
        in.midi = model::MidiRouting{1, 0};
        const Id r = t.create(in);
        t.modify<model::Track>(p, [&](model::Track& x) {
            x.chain.push_back(f);
            x.record.input = r;
            x.record.armed = true;
            x.record.monitor = model::Monitor::On;
        });
    }
    model::FxInstance echo;
    echo.plugin.id = "com.suppleaudio.test.edge";
    echo.pdc = model::PdcMode::Ignore;
    echo.state = "sha256:echo-48000";
    t.putBlob(*echo.state, std::make_shared<const model::Blob>(model::Blob{{49, 0x80, 0xbb, 0}}));
    const Id fx = t.create(echo);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.chain.push_back(fx);
        x.record.armed = armed;
        x.record.monitor = monitor;
    });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 960;
    src.sequence.notes = {{0, 240, 0, 60, 100, 0}};  // 6000 frames
    model::Take take;
    take.source = t.create(src);
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 1.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

// Two sine tracks on keyboardSession's MIDI input: "Pads", armed and
// monitored (so the live region exists) and silent, and "Keys", holding key
// 60 for two beats (1 s), armed as given; the master playrate `rate`, or,
// with `envelope`, a playrate envelope from 1 to 1.5 over two seconds.
model::Session twoKeysSession(bool armKeys, double rate = 1.0, bool preserve = false,
                              bool envelope = false) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    auto project = t.project();
    project.playrate = {rate, preserve};
    t.setProject(project);
    if (envelope) {
        model::Envelope env;
        env.target = {kMaster, "playrate"};
        env.timebase = model::Timebase::Time;
        env.points = {{0, 1.0}, {2.0, 1.5}};
        t.create(env);
    }
    for (const char* name : {"Pads", "Keys"}) {
        model::Track tr;
        tr.name = name;
        tr.parent = kMaster;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.sine";
        const Id f = t.create(fx);
        model::Route in;
        in.from.midiDevice = "key";
        in.to.track = track;
        in.midi = model::MidiRouting{1, 0};
        const Id r = t.create(in);
        const bool keys = std::string(name) == "Keys";
        t.modify<model::Track>(track, [&](model::Track& x) {
            x.chain.push_back(f);
            x.record.input = r;
            x.record.armed = !keys || armKeys;
            x.record.monitor = model::Monitor::On;
        });
        if (!keys) continue;
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 4 * 960;
        src.sequence.notes = {{0, 2 * 960, 0, 60, 100, 0}};
        model::Take take;
        take.source = t.create(src);
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = 4.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    return t.commit().value();
}
}  // namespace

// Under a master playrate a timeline note at the very start of the song
// plays: offline and in real time, in both executors, on the track's
// instrument and on a take's (a take chain, with its own fresh window).
TEST_CASE("master playrate: a note at tick 0 sounds, offline and in real time" *
          doctest::description("rt")) {
    enum class Kind { Track, Take };
    const auto session = [](Kind kind, bool preserve) {
        model::Session base;
        if (kind == Kind::Track) {
            base = heldNoteSession(false, model::Monitor::On, "com.suppleaudio.test.sine");
        } else {
            model::EditTxn t(model::Session::create({.console9 = false}), 1);
            model::Track track;
            track.parent = kMaster;
            const Id tr = t.create(track);
            t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
            model::FxInstance synth;
            synth.plugin.id = "com.suppleaudio.test.sine";
            model::Take take;
            take.chain = {t.create(synth)};
            model::MidiSource src;
            src.sequence.ppq = 960;
            src.sequence.length = 8 * 960;
            src.sequence.notes = {{0, 8 * 960, 0, 60, 100, 0}};
            take.source = t.create(src);
            const Id tk = t.create(take);
            model::Item item;
            item.track = tr;
            item.length = 8.0;
            item.takes = {tk};
            item.activeTake = tk;
            t.create(item);
            base = t.commit().value();
        }
        model::EditTxn t(base, 3);
        auto project = t.project();
        project.playrate = {1.25, preserve};
        t.setProject(project);
        return t.commit().value();
    };
    // The first 2000 frames' peak: the note starts at the first frame.
    const auto first = [](const std::vector<float>& x) {
        double peak = 0;
        for (std::size_t i = 0; i < 2000; ++i) peak = std::max(peak, std::fabs(double(x[i])));
        return peak;
    };
    for (const Kind kind : {Kind::Track, Kind::Take})
        for (const bool preserve : {false, true}) {
            INFO("kind " << static_cast<int>(kind) << ", preserve pitch " << preserve);
            const auto s = session(kind, preserve);
            auto pool = makePool();
            const auto offline =
                renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
            const auto& want = offline.audio.channels[0];
            REQUIRE(want.size() >= 24000);  // timeline seconds: 38400 device frames at 1.25
            CHECK(first(want) > 0.05);
            for (const EngineOptions& options : kBothExecutors) {
                INFO("anticipative " << options.anticipative << ", workers " << options.workers);
                auto rtPool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, 24000 + 4096));
                Engine engine(rtPool, media, options);
                REQUIRE(
                    engine
                        .start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                        .ok());
                REQUIRE(engine.load(s));
                REQUIRE(engine.awaitPlan());
                engine.play();
                runFrames(engine, backend, options, 24000, 256);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                REQUIRE(engine.outputLatency() == 0);
                const auto& got = backend.capture().channels[0];
                CHECK(first(got) > 0.05);
                CHECK(maxDiff(got, 0, want, 24000) == 0.0);
            }
        }
}

// A take chain compensated for the 64-frame latency after its instrument
// runs its notes that far ahead. After a locate, the notes between the
// locate and that look-ahead are due already: they play at once, late, at
// the first frame, on the unity and the rate path, at the song's start and
// after a seek. A seek plays as the offline render from there.
TEST_CASE("a compensated take's notes inside its look-ahead play at once after a locate" *
          doctest::description("rt")) {
    const auto session = [](double rate) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        auto project = t.project();
        project.playrate = {rate, false};
        t.setProject(project);
        model::Track track;
        track.parent = kMaster;
        const Id tr = t.create(track);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
        model::FxInstance synth;
        synth.plugin.id = "com.suppleaudio.test.sine";
        model::FxInstance delay;
        delay.plugin.id = "com.suppleaudio.test.gain";
        delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                      "params":{"latency":"short"}})")
                          .value();
        model::Take take;
        take.chain = {t.create(synth), t.create(delay)};
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 16 * 960;
        // At the song's start, and 25 frames after 1 s (inside the look-ahead
        // of a seek there).
        src.sequence.notes = {{0, 960, 0, 60, 100, 0}, {1921, 960, 0, 72, 100, 0}};
        take.source = t.create(src);
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.length = 8.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return t.commit().value();
    };
    const auto first = [](const std::vector<float>& x) {
        double peak = 0;
        for (std::size_t i = 0; i < 2000; ++i) peak = std::max(peak, std::fabs(double(x[i])));
        return peak;
    };
    constexpr std::int64_t kFrames = 24000;
    for (const double rate : {1.0, 1.25})
        for (const double from : {0.0, 1.0}) {
            INFO("rate " << rate << ", from " << from);
            const auto s = session(rate);
            auto pool = makePool();
            const auto offline =
                renderOffline(s, pool, {.startSeconds = from, .endSeconds = from + 1.0}).value();
            const auto& want = offline.audio.channels[0];
            REQUIRE(want.size() >= static_cast<std::size_t>(kFrames));
            CHECK(first(want) > 0.05);
            for (const EngineOptions& options : kBothExecutors) {
                INFO("anticipative " << options.anticipative << ", workers " << options.workers);
                auto rtPool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, kFrames + 4096));
                Engine engine(rtPool, media, options);
                REQUIRE(
                    engine
                        .start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                        .ok());
                REQUIRE(engine.load(s));
                REQUIRE(engine.awaitPlan());
                if (from > 0) engine.seek(from);
                engine.play();
                runFrames(engine, backend, options, kFrames, 256);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                REQUIRE(engine.outputLatency() == 0);
                const auto& got = backend.capture().channels[0];
                CHECK(first(got) > 0.05);
                CHECK(maxDiff(got, 0, want, static_cast<std::size_t>(kFrames)) == 0.0);
            }
        }
}

// A compensated take (64 frames of latency after its instrument) with a note
// just after the loop start, inside its look-ahead: every pass plays it on
// time. The chain takes the note before the wrap, its look-ahead early, at
// unity and under a master playrate. With the loop at 0.5 s the first pass
// reaches the note without a wrap, as the offline render (no loop there)
// does, and every later pass plays as the first. With the loop at the song's
// start the first pass plays the note late (it is due at the first frame);
// every later pass plays it on time, as the other loop's first pass.
TEST_CASE("a compensated take's note just after the loop start plays on time every pass" *
          doctest::description("rt")) {
    // The note `tick`s into the song, 0.125 s long.
    const auto session = [](double rate, int tick) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        auto project = t.project();
        project.playrate = {rate, false};
        t.setProject(project);
        model::Track track;
        track.parent = kMaster;
        const Id tr = t.create(track);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
        model::FxInstance synth;
        synth.plugin.id = "com.suppleaudio.test.sine";
        model::FxInstance delay;
        delay.plugin.id = "com.suppleaudio.test.gain";
        delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                      "params":{"latency":"short"}})")
                          .value();
        model::Take take;
        take.chain = {t.create(synth), t.create(delay)};
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 16 * 960;
        src.sequence.notes = {{tick, 240, 0, 72, 100, 0}};
        take.source = t.create(src);
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.length = 8.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return t.commit().value();
    };
    constexpr int kPasses = 5;
    for (const double rate : {1.0, 1.25}) {
        // Device frames: one pass (0.5 s of timeline), and the loop at 0.5 s's
        // first pass, on time, offline (the note 25 frames into it).
        const auto pass = static_cast<std::size_t>(24000 / rate);
        auto pool = makePool();
        const auto offline =
            renderOffline(session(rate, 961), pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
        REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
        const std::vector<float> onTime(
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
        double heard = 0;
        for (const float x : onTime) heard = std::max(heard, std::fabs(double(x)));
        REQUIRE(heard > 0.05);
        for (const bool atStart : {false, true})
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO("rate " << rate << ", loop at " << (atStart ? 0.0 : 0.5) << ", block "
                                 << block << ", anticipative " << options.anticipative
                                 << ", workers " << options.workers);
                    const auto s = session(rate, atStart ? 1 : 961);
                    const std::size_t start = atStart ? 0 : pass;  // the loop start
                    const std::size_t frames = start + kPasses * pass;
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(s));
                    REQUIRE(engine.awaitPlan());
                    engine.setLoop(atStart ? 0.0 : 0.5, atStart ? 0.5 : 1.0, true);
                    engine.play();
                    runFrames(engine, backend, options, static_cast<std::int64_t>(frames), block);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    REQUIRE(engine.outputLatency() == 0);
                    const auto& L = backend.capture().channels[0];
                    if (!atStart)  // up to the first wrap: the offline render
                        CHECK(maxDiff(L, 0, offline.audio.channels[0], start + pass) == 0.0);
                    // Every pass after the first: on time.
                    for (int k = 1; k < kPasses; ++k) {
                        INFO("pass " << k);
                        CHECK(maxDiff(L, start + static_cast<std::size_t>(k) * pass, onTime,
                                      pass) == 0.0);
                    }
                }
    }
}

// A short item 25 frames after the loop start, inside the look-ahead of its
// take FX (64 frames of latency): an audio take, and a MIDI one through the
// same FX after an instrument. Every pass plays it on time: the chain takes
// its input before the wrap, at unity and under a master playrate. With the
// loop at 0.5 s the first pass reaches the item without a wrap, as offline
// does; with the loop at the song's start every later pass plays as that
// first pass (the song's start itself cannot run ahead).
TEST_CASE("a short take inside its take FX's look-ahead after the loop start plays every pass" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-loop-take-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0));
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone));
    // The item at `at` beats (the loop start plus 25 frames), 0.125 s long
    // (item positions are in beats here: 120 BPM, 24000 frames a beat).
    const auto session = [](double rate, bool audio, double at) {
        model::EditTxn t(model::Session::create({.console9 = false}), 1);
        auto project = t.project();
        project.playrate = {rate, false};
        t.setProject(project);
        model::Track track;
        track.parent = kMaster;
        const Id tr = t.create(track);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
        model::FxInstance delay;
        delay.plugin.id = "com.suppleaudio.test.gain";
        delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                      "params":{"latency":"short"}})")
                          .value();
        model::Take take;
        if (audio) {
            take.source = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 48000});
        } else {
            model::FxInstance synth;
            synth.plugin.id = "com.suppleaudio.test.sine";
            take.chain.push_back(t.create(synth));
            model::MidiSource src;
            src.sequence.ppq = 960;
            src.sequence.length = 240;
            src.sequence.notes = {{0, 240, 0, 72, 100, 0}};
            take.source = t.create(src);
        }
        take.chain.push_back(t.create(delay));
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.position = at;
        item.length = 0.25;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return t.commit().value();
    };
    constexpr int kPasses = 5;
    constexpr double kAfter = 25.0 / 24000.0;  // beats
    for (const bool audio : {true, false})
        for (const double rate : {1.0, 1.25}) {
            const auto pass = static_cast<std::size_t>(24000 / rate);  // 0.5 s of timeline
            auto pool = makePool();
            const auto offline =
                renderOffline(session(rate, audio, 1.0 + kAfter), pool,
                              {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir})
                    .value();
            REQUIRE(offline.warnings.empty());
            REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
            const std::vector<float> onTime(
                offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
                offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
            double heard = 0;
            for (const float x : onTime) heard = std::max(heard, std::fabs(double(x)));
            REQUIRE(heard > 0.05);
            for (const bool atStart : {false, true})
                for (const std::uint32_t block : {256u, 37u})
                    for (const EngineOptions& options : kBothExecutors) {
                        INFO(std::string(audio ? "audio" : "midi")
                             << ", rate " << rate << ", loop at " << (atStart ? 0.0 : 0.5)
                             << ", block " << block << ", anticipative " << options.anticipative
                             << ", workers " << options.workers);
                        const auto s = session(rate, audio, (atStart ? 0.0 : 1.0) + kAfter);
                        const std::size_t start = atStart ? 0 : pass;  // the loop start
                        const std::size_t frames = start + kPasses * pass;
                        auto rtPool = makePool();
                        MediaCache media;
                        DummyBackend backend(
                            keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                        Engine engine(rtPool, media, options);
                        REQUIRE(engine
                                    .start(backend, {.sampleRate = 48000,
                                                     .blockSize = block,
                                                     .midiInputs = true})
                                    .ok());
                        REQUIRE(engine.load(s, dir));
                        REQUIRE(engine.awaitPlan());
                        engine.setLoop(atStart ? 0.0 : 0.5, atStart ? 0.5 : 1.0, true);
                        engine.play();
                        runFrames(engine, backend, options, static_cast<std::int64_t>(frames),
                                  block);
                        CHECK(engine.processErrors() == 0);
                        engine.stop();
                        REQUIRE(engine.outputLatency() == 0);
                        const auto& L = backend.capture().channels[0];
                        if (!atStart)  // up to the first wrap: the offline render
                            CHECK(maxDiff(L, 0, offline.audio.channels[0], start + pass) == 0.0);
                        for (int k = 1; k < kPasses; ++k) {
                            INFO("pass " << k);
                            CHECK(maxDiff(L, start + static_cast<std::size_t>(k) * pass, onTime,
                                          pass) == 0.0);
                        }
                    }
        }
    std::filesystem::remove_all(dir);
}

namespace {
// A compensated take (64 frames of latency after its instrument) whose
// notes are (tick, duration) pairs, 960 ticks a beat at 120 BPM (a tick is
// 25 frames), from the start of an item at `position` seconds, `length`
// seconds long.
model::Session compensatedTakeSession(double rate, std::vector<std::pair<int, int>> notes,
                                      double position = 0.0, double length = 4.0) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    auto project = t.project();
    project.playrate = {rate, false};
    t.setProject(project);
    model::Track track;
    track.parent = kMaster;
    const Id tr = t.create(track);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tr); });
    model::FxInstance synth;
    synth.plugin.id = "com.suppleaudio.test.sine";
    model::FxInstance delay;
    delay.plugin.id = "com.suppleaudio.test.gain";
    delay.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                  "params":{"latency":"short"}})")
                      .value();
    model::Take take;
    take.chain = {t.create(synth), t.create(delay)};
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 16 * 960;
    for (const auto& [tick, duration] : notes)
        src.sequence.notes.push_back({tick, duration, 0, 72, 100, 0});
    take.source = t.create(src);
    const Id tk = t.create(take);
    model::Item item;
    item.track = tr;
    item.timebase = model::ItemTimebase::Time;
    item.position = position;
    item.length = length;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

double peakOf(const std::vector<float>& v, std::size_t from, std::size_t n) {
    double peak = 0;
    for (std::size_t i = from; i < from + n && i < v.size(); ++i)
        peak = std::max(peak, std::fabs(double(v[i])));
    return peak;
}
}  // namespace

// A compensated take around a loop at [0.5 s, 1 s): every pass after the
// first plays as the first, which plays as offline, at unity and under a
// master playrate. Its item may end before the loop end (before the
// look-ahead wraps) or start at the loop start; notes may lie exactly at
// the loop's start or end (the end's is never heard). A note crossing the
// loop end ends at the wrap: every later pass plays as the second (the
// first has no released note before it).
TEST_CASE("a compensated take around the loop plays every pass as the first" *
          doctest::description("rt")) {
    struct Shape {
        const char* name;
        std::vector<std::pair<int, int>> notes;
        double position, length;
        bool crossing = false;
    };
    const std::vector<Shape> shapes = {
        {"the item ends before the loop end", {{961, 240}}, 0.0, 0.75},
        {"the item starts at the loop start", {{1, 240}}, 0.5, 0.25},
        {"the item ends inside the look-ahead", {{961, 240}}, 0.0, 0.999},
        {"notes at the loop start and end", {{960, 240}, {1920, 240}}, 0.0, 4.0},
        {"a note crossing the loop end", {{1900, 240}}, 0.0, 4.0, true},
    };
    constexpr int kPasses = 5;
    for (const Shape& shape : shapes)
        for (const double rate : {1.0, 1.25}) {
            const auto pass = static_cast<std::size_t>(24000 / rate);
            const auto session =
                compensatedTakeSession(rate, shape.notes, shape.position, shape.length);
            auto pool = makePool();
            const auto offline =
                renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
            REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
            REQUIRE(peakOf(offline.audio.channels[0], pass, pass) > 0.05);
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO(std::string(shape.name)
                         << ", rate " << rate << ", block " << block << ", anticipative "
                         << options.anticipative << ", workers " << options.workers);
                    const std::size_t frames = (kPasses + 1) * pass;
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(session));
                    REQUIRE(engine.awaitPlan());
                    engine.setLoop(0.5, 1.0, true);
                    engine.play();
                    runFrames(engine, backend, options, static_cast<std::int64_t>(frames), block);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    const auto& L = backend.capture().channels[0];
                    // Up to the first wrap: the offline render.
                    CHECK(maxDiff(L, 0, offline.audio.channels[0], 2 * pass) == 0.0);
                    // A note crossing the loop end: the second pass begins with
                    // its release, and the note has ended well before its own
                    // onset again.
                    const std::vector<float> first(
                        shape.crossing
                            ? L.begin() + static_cast<std::ptrdiff_t>(2 * pass)
                            : offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
                        (shape.crossing ? L.begin() : offline.audio.channels[0].begin()) +
                            static_cast<std::ptrdiff_t>((shape.crossing ? 3 : 2) * pass));
                    if (shape.crossing) CHECK(peakOf(first, pass / 4, pass / 2) == 0.0);
                    for (int k = shape.crossing ? 2 : 1; k < kPasses; ++k) {
                        INFO("pass " << k);
                        CHECK(maxDiff(L, (static_cast<std::size_t>(k) + 1) * pass, first, pass) ==
                              0.0);
                    }
                }
        }
}

// The loop at [0.5 s, 1 s), a compensated take's note 25 frames after the
// loop start: playing from inside its look-ahead before the loop end (a seek
// there, before play or, in the callback, while playing), and a plan swap
// there after the take has wrapped early (in the callback, where the swap's
// frame is known), every following pass plays the note on time. A swap that
// also takes the latency away (its take FX's) wraps afresh: the note the
// chain took early ends at the wrap, and the pass plays as the new
// session's. (The seek
// is before the note is due: one later is a locate past it, which plays it
// at once, late.)
TEST_CASE(
    "a compensated take's note after the loop start: a seek or a plan swap in its "
    "look-ahead" *
    doctest::description("rt")) {
    constexpr int kPasses = 4;
    // Timeline frames before the loop end: inside the look-ahead (64 device
    // frames, 80 timeline frames at 1.25), before the note's input is due.
    constexpr std::int64_t kBefore = 60;
    enum class Change { PlayThere, SeekThere, SwapThere, NoLatencyThere };
    for (const double rate : {1.0, 1.25}) {
        const auto pass = static_cast<std::size_t>(24000 / rate);
        const auto session = compensatedTakeSession(rate, {{961, 240}});
        // The plan swap: the same take beside a new, empty track.
        model::EditTxn more(session, 2);
        model::Track extra;
        extra.parent = kMaster;
        const Id extraId = more.create(extra);
        more.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(extraId); });
        const auto edited = more.commit().value();
        model::EditTxn none(session, 2);
        for (const auto& e : session.fx)
            if (e.value->plugin.id == "com.suppleaudio.test.gain")
                none.modify<model::FxInstance>(e.id, [](model::FxInstance& fx) {
                    fx.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                                               "params":{"latency":"none"}})")
                                   .value();
                });
        const auto noLatency = none.commit().value();
        auto pool = makePool();
        const auto offline =
            renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
        REQUIRE(offline.audio.channels[0].size() >= 2 * pass);
        const std::vector<float> onTime(
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
            offline.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
        REQUIRE(peakOf(onTime, 0, pass) > 0.05);
        const auto uncompensated =
            renderOffline(noLatency, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
        REQUIRE(uncompensated.audio.channels[0].size() >= 2 * pass);
        const std::vector<float> onTimeNone(
            uncompensated.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(pass),
            uncompensated.audio.channels[0].begin() + static_cast<std::ptrdiff_t>(2 * pass));
        for (const Change change :
             {Change::PlayThere, Change::SeekThere, Change::SwapThere, Change::NoLatencyThere})
            for (const std::uint32_t block : {37u, 32u})
                for (const EngineOptions& options : kBothExecutors) {
                    if (change != Change::PlayThere && options.anticipative) continue;
                    INFO("rate " << rate << ", change " << int(change) << ", block " << block
                                 << ", anticipative " << options.anticipative << ", workers "
                                 << options.workers);
                    const std::size_t frames = 4 * pass + kPasses * pass;
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, static_cast<std::int64_t>(frames) + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(session));
                    REQUIRE(engine.awaitPlan());
                    engine.setLoop(0.5, 1.0, true);
                    const double there = double(48000 - kBefore) / 48000.0;
                    std::int64_t played = 0;
                    if (change == Change::PlayThere) engine.seek(there);
                    engine.play();
                    // Device frames from here to the output's next wrap.
                    std::int64_t toWrap = static_cast<std::int64_t>(std::llround(kBefore / rate));
                    if (change == Change::SeekThere) {
                        played = static_cast<std::int64_t>(pass + pass / 2) / block * block;
                        backend.run(played / block);
                        engine.seek(there);
                    } else if (change == Change::SwapThere || change == Change::NoLatencyThere) {
                        // The second wrap's look-ahead: its last block boundary
                        // there, after the take has wrapped early.
                        const auto wrap = static_cast<std::int64_t>(3 * pass);
                        played = (wrap - 1) / block * block;
                        REQUIRE(played > wrap - static_cast<std::int64_t>(64 / rate));
                        backend.run(played / block);
                        REQUIRE(engine.load(change == Change::SwapThere ? edited : noLatency));
                        REQUIRE(engine.awaitPlan());
                        toWrap = wrap - played;
                    }
                    runFrames(engine, backend, options, static_cast<std::int64_t>(frames) - played,
                              block);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    const auto& L = backend.capture().channels[0];
                    const auto wrapped = static_cast<std::size_t>(played + toWrap);
                    for (int k = 0; k < kPasses; ++k) {
                        INFO("pass " << k);
                        CHECK(maxDiff(L, wrapped + static_cast<std::size_t>(k) * pass,
                                      change == Change::NoLatencyThere ? onTimeNone : onTime,
                                      pass) == 0.0);
                    }
                }
    }
}

// The callback's executor takes a loop edit with no locate (the
// anticipative one restarts its epoch). Edited after a compensated take has
// wrapped early, before the output's wrap, the loop the take wrapped for is
// gone: the take locates (the loop start's note it took ends), and a new
// loop's notes play every pass as offline. Turned off and later set again,
// or moved there at once.
TEST_CASE("a loop edit inside a compensated take's look-ahead is a locate in the callback" *
          doctest::description("rt")) {
    const auto session = compensatedTakeSession(1.0, {{961, 240}, {2881, 240}});
    auto pool = makePool();
    const auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
    const auto& O = offline.audio.channels[0];
    REQUIRE(O.size() >= 96000);
    REQUIRE(peakOf(O, 72000, 24000) > 0.05);
    REQUIRE(peakOf(O, 48000, 24000) == 0.0);
    const std::vector<float> onTime(O.begin() + 72000, O.begin() + 96000);
    for (const bool off : {true, false})
        for (const std::uint32_t block : {37u, 32u})
            for (const EngineOptions& options :
                 {kInCallback, EngineOptions{.anticipative = false, .workers = 2}}) {
                INFO("off first " << off << ", block " << block << ", workers " << options.workers);
                constexpr std::int64_t kFrames = 72000 + 4 * 24000;
                auto rtPool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, kFrames + 4096));
                Engine engine(rtPool, media, options);
                REQUIRE(engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                REQUIRE(engine.load(session));
                REQUIRE(engine.awaitPlan());
                engine.setLoop(0.5, 1.0, true);
                engine.play();
                // The first wrap's look-ahead, after the take wrapped early.
                const std::int64_t edit = 47999 / block * block;
                REQUIRE(edit > 48000 - 64);
                backend.run(edit / block);
                std::int64_t played = edit;
                if (off) {
                    engine.setLoop(0.5, 1.0, false);
                    backend.run((60000 - played) / block);
                    played += (60000 - played) / block * block;
                }
                engine.setLoop(1.5, 2.0, true);
                backend.run((kFrames - played + block - 1) / block);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& L = backend.capture().channels[0];
                CHECK(maxDiff(L, 0, O, static_cast<std::size_t>(edit)) == 0.0);
                // The note taken early, ended there, is past by a block.
                CHECK(peakOf(L, 48000 + 256, 72000 - 48000 - 256) == 0.0);
                for (int k = 0; k < 4; ++k) {
                    INFO("pass " << k);
                    CHECK(maxDiff(L, 72000 + static_cast<std::size_t>(k) * 24000, onTime, 24000) ==
                          0.0);
                }
            }
}

// An effect's tail crosses into the live region with its strip: a short note
// ends before the strip is armed (or starts monitoring), and its echo, still
// inside the effect's delay line at the handoff, is heard as offline. Alone
// the strip opens the live region; beside an armed track (`pads`) it joins
// the region that exists (adoptLive's continuing branch).
TEST_CASE("arming or monitoring mid-song keeps an effect's tail" * doctest::description("rt")) {
    constexpr std::int64_t kEdit = 12800, kFrames = 64000;
    struct Change {
        const char* name;
        bool armedBefore;
        model::Monitor before;
    };
    for (const bool pads : {false, true}) {
        auto pool = makePool();
        const auto offline = renderOffline(echoSession(false, model::Monitor::On, pads), pool,
                                           {.startSeconds = 0, .endSeconds = 1.5})
                                 .value();
        const auto& want = offline.audio.channels[0];
        REQUIRE(want.size() >= static_cast<std::size_t>(kFrames));
        for (const Change change : {Change{"arming", false, model::Monitor::On},
                                    Change{"monitoring on", true, model::Monitor::Off}})
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO(std::string(change.name)
                         << ", pads " << pads << ", block " << block << ", anticipative "
                         << options.anticipative << ", workers " << options.workers);
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, kFrames + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(echoSession(change.armedBefore, change.before, pads)));
                    REQUIRE(engine.awaitPlan());
                    engine.play();
                    runFrames(engine, backend, options, kEdit, block);
                    REQUIRE(engine.load(echoSession(true, model::Monitor::On, pads)));
                    runFrames(engine, backend, options, kFrames - kEdit, block);
                    engine.service();
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    REQUIRE(engine.outputLatency() == 0);
                    const auto& L = backend.capture().channels[0];
                    CHECK(maxDiff(L, 0, want, static_cast<std::size_t>(kFrames)) == 0.0);
                    double echo = 0;
                    for (std::size_t i = 48000; i < 54000; ++i)
                        echo = std::max(echo, std::fabs(double(L[i])));
                    CHECK(echo > 0.05);
                }
    }
}

// Beside an armed track, a strip whose handoff the callback reaches late
// (the renderer stalled, the callback played on past it) starts afresh in
// the live region: its delay line is dropped, so the echo the renderer
// carried is not heard, and from the handoff on the output is what a fresh
// start after the note gives (offline from there: nothing to echo).
TEST_CASE("a late handoff beside an armed track drops an effect's tail" *
          doctest::description("rt")) {
    constexpr std::int64_t kFrom = 36000, kFrames = 96000;
    auto pool = makePool();
    const auto fresh = renderOffline(echoSession(true, model::Monitor::On, true), pool,
                                     {.startSeconds = double(kFrom) / 48000.0, .endSeconds = 2.0})
                           .value();
    REQUIRE(fresh.audio.channels[0].size() >= static_cast<std::size_t>(kFrames - kFrom));
    for (const std::uint32_t block : {256u, 37u})
        for (const int workers : {0, 2}) {
            INFO("block " << block << ", workers " << workers);
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(keyboard({}, kFrames + 4096));
            Engine engine(rtPool, media, EngineOptions{.workers = workers});
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                    .ok());
            REQUIRE(engine.load(echoSession(false, model::Monitor::On, true)));
            REQUIRE(engine.awaitPlan());
            engine.play();
            std::int64_t played = 0;
            auto playTo = [&](std::int64_t to, bool ahead) {
                const std::int64_t cycles = (to - played + block - 1) / block;
                if (cycles <= 0) return;
                if (ahead)
                    runAhead(engine, backend, cycles, block);
                else
                    backend.run(cycles);
                played += cycles * block;
            };
            playTo(12000, true);  // the note (to 6000) is in the echo's delay line
            engine.holdRenderer(true);
            REQUIRE(engine.awaitRendererHeld());
            playTo(28000, false);
            REQUIRE(engine.load(echoSession(true, model::Monitor::On, true)));
            REQUIRE(engine.awaitPlan());  // adopted by the renderer where it stalled
            playTo(32000, false);         // the callback takes it at once, past the handoff
            engine.holdRenderer(false);
            playTo(kFrames, true);
            engine.service();
            CHECK(engine.processErrors() == 0);
            engine.stop();
            const auto& L = backend.capture().channels[0];
            REQUIRE(L.size() >= static_cast<std::size_t>(kFrames));
            // (The exact handoff, in the test above, hears the echo at 48000.)
            CHECK(maxDiff(L, static_cast<std::size_t>(kFrom), fresh.audio.channels[0],
                          static_cast<std::size_t>(kFrames - kFrom)) == 0.0);
        }
}

// With another track already armed the live region exists, so a strip armed
// mid-note joins it (adoptLive's continuing branch). It continues exactly,
// at unity (also with a loop wrap between the edit and the handoff) and
// under a master playrate (also looping), with a handoff inside an
// odd-sized callback too: the same engine never armed plays the same, and
// nothing hangs.
TEST_CASE("arming beside an armed track mid-note: exact at unity and under a playrate" *
          doctest::description("rt")) {
    constexpr std::int64_t kEdit = 12800, kFrames = 96000;
    auto pool = makePool();
    const auto offline =
        renderOffline(twoKeysSession(false), pool, {.startSeconds = 0, .endSeconds = 2.5}).value();
    REQUIRE(offline.audio.channels[0].size() >= static_cast<std::size_t>(kFrames));
    struct Rate {
        double rate;
        bool preserve;
        bool envelope = false;  // a playrate envelope, 1 to 1.5
    };
    for (const Rate r :
         {Rate{1.0, false}, Rate{1.25, false}, Rate{1.25, true}, Rate{1.0, false, true}})
        for (const bool loop : {false, true})
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    if (r.envelope && loop) continue;
                    const double rate = r.envelope ? 0.0 : r.rate;  // 0: the envelope
                    INFO("rate " << rate << ", preserve " << r.preserve << ", loop " << loop
                                 << ", block " << block << ", anticipative " << options.anticipative
                                 << ", workers " << options.workers);
                    // `arm`: arm Keys at kEdit; else the reference, never armed.
                    // `resets`: how often the Keys instrument was reset from the
                    // edit to the end.
                    std::uint32_t resets = 0;
                    auto run = [&](bool arm) {
                        auto rtPool = makePool();
                        MediaCache media;
                        DummyBackend backend(keyboard({}, kFrames + 4096));
                        Engine engine(rtPool, media, options);
                        REQUIRE(engine
                                    .start(backend, {.sampleRate = 48000,
                                                     .blockSize = block,
                                                     .midiInputs = true})
                                    .ok());
                        REQUIRE(engine.load(twoKeysSession(false, r.rate, r.preserve, r.envelope)));
                        REQUIRE(engine.awaitPlan());
                        // The loop wraps 2000 frames after the edit, before its handoff.
                        if (loop) engine.setLoop(0.0, double(kEdit + 2000) / 48000.0, true);
                        engine.play();
                        runFrames(engine, backend, options, kEdit, block);
                        if (loop && rate == 1.0 && options.anticipative) {
                            // A unity handoff is where the renderer is when it adopts
                            // the plan: rendered at least 4096 frames ahead of the
                            // playhead (under kEdit + block) at the edit, so past the
                            // wrap at kEdit + 2000, which the playhead has not reached.
                            REQUIRE(options.renderAheadMs * 48.0 >= 4096.0);
                            REQUIRE(engine.awaitRendered(4096));
                            REQUIRE((kEdit + block - 1) / block * block < kEdit + 2000);
                        }
                        host::Instance* const keys =
                            rtPool.find(keysInstrument(twoKeysSession(false)));
                        REQUIRE(keys);
                        const std::uint32_t resetsBefore = keys->resets();
                        if (arm)
                            REQUIRE(
                                engine.load(twoKeysSession(true, r.rate, r.preserve, r.envelope)));
                        runFrames(engine, backend, options, kFrames - kEdit, block);
                        engine.service();
                        resets = keys->resets() - resetsBefore;
                        CHECK(engine.processErrors() == 0);
                        engine.stop();
                        REQUIRE(engine.outputLatency() == 0);
                        return backend.capture().channels[0];
                    };
                    const auto reference = run(false);
                    const auto armed = run(true);
                    // An exact handoff continues the instrument: never reset.
                    CHECK(resets == 0);
                    // Arming changes nothing: the never-armed run of this engine.
                    CHECK(maxDiff(armed, 0, reference, static_cast<std::size_t>(kFrames)) == 0.0);
                    if (rate == 1.0) {
                        if (!loop)
                            CHECK(maxDiff(armed, 0, offline.audio.channels[0],
                                          static_cast<std::size_t>(kFrames)) == 0.0);
                    } else if (!loop && !r.envelope) {
                        // The note ends at 1 s of timeline, 38400 device frames
                        // at 1.25: silence after it.
                        double after = 0;
                        for (std::size_t i = 40000; i < static_cast<std::size_t>(kFrames); ++i)
                            after = std::max(after, std::fabs(double(armed[i])));
                        CHECK(after == 0.0);
                        double during = 0;
                        for (std::size_t i = 1000; i < 12000; ++i)
                            during = std::max(during, std::fabs(double(armed[i])));
                        CHECK(during > 0.05);
                    }
                }
}

// One change that both turns a playrate on (unity to 1.25) and arms a strip
// mid-note cannot continue it: the first rate plan places its handoff in a
// new device coordinate. The strip starts afresh, its instrument reset, and
// nothing sounds after the note's end, also after a loop wrap.
TEST_CASE("turning a playrate on and arming in one change restarts the strip: no note hangs" *
          doctest::description("rt")) {
    for (const bool loop : {false, true})
        for (const std::uint32_t block : {256u, 37u})
            for (const EngineOptions& options : kBothExecutors) {
                INFO("loop " << loop << ", block " << block << ", anticipative "
                             << options.anticipative << ", workers " << options.workers);
                // Looping: [0, 1.5 s) wraps at 72000 before the edit at 80000
                // (timeline 8000, mid-note); the note ends at timeline 48000,
                // under 1.25 by device 116000, and the next wrap comes after
                // 132000, when the note starts again.
                const std::int64_t edit = loop ? 80000 : 12800, frames = loop ? 150000 : 96000;
                auto pool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, frames + 4096));
                Engine engine(pool, media, options);
                REQUIRE(engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                REQUIRE(engine.load(twoKeysSession(false)));
                REQUIRE(engine.awaitPlan());
                if (loop) engine.setLoop(0.0, 1.5, true);
                engine.play();
                runFrames(engine, backend, options, edit, block);
                host::Instance* const keys = pool.find(keysInstrument(twoKeysSession(false)));
                REQUIRE(keys);
                const std::uint32_t resetsBefore = keys->resets();
                REQUIRE(engine.load(twoKeysSession(true, 1.25)));
                runFrames(engine, backend, options, frames - edit, block);
                engine.service();
                if (options.anticipative) CHECK(keys->resets() > resetsBefore);
                CHECK(engine.effectivePlayrate() == doctest::Approx(1.25).epsilon(1e-3));
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& L = backend.capture().channels[0];
                REQUIRE(L.size() >= static_cast<std::size_t>(frames));
                const auto peak = [&](std::size_t from, std::size_t to) {
                    double x = 0;
                    for (std::size_t i = from; i < to; ++i)
                        x = std::max(x, std::fabs(double(L[i])));
                    return x;
                };
                CHECK(peak(1000, 12000) > 0.05);
                if (loop) {
                    CHECK(peak(118000, 132000) == 0.0);
                    CHECK(peak(136000, 146000) > 0.05);  // the next pass, the strip now live
                } else {
                    CHECK(peak(46000, 96000) == 0.0);
                }
            }
}

// Arming at the boundary where the playrate also changes (1.25 to 1.5):
// the strip continues from the renderer's old timeline into the new plan's,
// whose history up to the handoff is the old one's, and plays exactly as the
// never-armed run of the same engine. Both wait, before the edit, until the
// renderer is full, so the prepared boundary is the same in both runs.
TEST_CASE("arming while the playrate changes: exact" * doctest::description("rt")) {
    constexpr std::int64_t kEdit = 12800, kFrames = 96000;
    for (const bool preserve : {false, true})
        for (const std::uint32_t block : {256u, 37u})
            for (const EngineOptions& options : kBothExecutors) {
                INFO("preserve " << preserve << ", block " << block << ", anticipative "
                                 << options.anticipative << ", workers " << options.workers);
                std::uint32_t resets = 0;
                auto run = [&](bool arm) {
                    auto pool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, kFrames + 4096));
                    Engine engine(pool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(twoKeysSession(false, 1.25, preserve)));
                    REQUIRE(engine.awaitPlan());
                    engine.play();
                    runFrames(engine, backend, options, kEdit, block);
                    if (options.anticipative)
                        REQUIRE(engine.awaitRendered(std::numeric_limits<std::int64_t>::max()));
                    host::Instance* const keys = pool.find(keysInstrument(twoKeysSession(false)));
                    REQUIRE(keys);
                    const std::uint32_t before = keys->resets();
                    REQUIRE(engine.load(twoKeysSession(arm, 1.5, preserve)));
                    runFrames(engine, backend, options, kFrames - kEdit, block);
                    engine.service();
                    resets = keys->resets() - before;
                    CHECK(engine.effectivePlayrate() == 1.5);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    return backend.capture().channels[0];
                };
                const auto reference = run(false);
                const auto armed = run(true);
                CHECK(resets == 0);
                CHECK(maxDiff(armed, 0, reference, static_cast<std::size_t>(kFrames)) == 0.0);
                double during = 0;
                for (std::size_t i = 1000; i < 12000; ++i)
                    during = std::max(during, std::fabs(double(armed[i])));
                CHECK(during > 0.05);
            }
}

// A handoff the callback reaches late (here past the renderer's position,
// after the renderer stalled and the frontier ran dry) cannot continue the
// carried node: what the renderer never rendered includes the note's end.
// The strip starts afresh, its instrument reset, so the note does not hang:
// at unity and under a master playrate.
TEST_CASE("a late handoff starts the newly live strip afresh: no note hangs" *
          doctest::description("rt")) {
    struct Rate {
        double rate;
        bool preserve;
    };
    for (const Rate r : {Rate{1.0, false}, Rate{1.25, false}, Rate{1.25, true}})
        for (const std::uint32_t block : {256u, 37u})
            for (const int workers : {0, 2}) {
                INFO("rate " << r.rate << ", preserve " << r.preserve << ", block " << block
                             << ", workers " << workers);
                const EngineOptions options{.workers = workers};
                auto pool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, 96000 + 4096));
                Engine engine(pool, media, options);
                REQUIRE(engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                REQUIRE(engine.load(twoKeysSession(false, r.rate, r.preserve)));
                REQUIRE(engine.awaitPlan());
                engine.play();
                std::int64_t played = 0;
                // Plays device callbacks up to frame `to`, the renderer kept ahead or not.
                auto playTo = [&](std::int64_t to, bool ahead) {
                    const std::int64_t cycles = (to - played + block - 1) / block;
                    if (cycles <= 0) return;
                    if (ahead)
                        runAhead(engine, backend, cycles, block);
                    else
                        backend.run(cycles);
                    played += cycles * block;
                };
                playTo(24000, true);
                // The renderer stops before the note's end (at 48000, or 38400 at
                // 1.25); the callback plays on past it, missing the frontier.
                engine.holdRenderer(true);
                REQUIRE(engine.awaitRendererHeld());
                host::Instance* const keys = pool.find(keysInstrument(twoKeysSession(false)));
                REQUIRE(keys);
                const std::uint32_t resetsBefore = keys->resets();
                if (r.rate == 1.0) {
                    playTo(60000, false);
                    // The renderer adopts the plan at its stalled position; the
                    // callback, past that handoff, takes it at once.
                    REQUIRE(engine.load(twoKeysSession(true, r.rate, r.preserve)));
                    REQUIRE(engine.awaitPlan());
                    playTo(68000, false);
                } else {
                    // A rate plan's handoff is its prepared boundary, a render-ahead
                    // span past the callback (before the note's end, edited here):
                    // the renderer reaches it once released, after the callback
                    // has played past it and past the note's end.
                    REQUIRE(engine.load(twoKeysSession(true, r.rate, r.preserve)));
                    playTo(68000, false);
                }
                engine.holdRenderer(false);
                playTo(96000, true);
                engine.service();
                // The callback, not continuing the instrument, reset it.
                CHECK(keys->resets() > resetsBefore);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& L = backend.capture().channels[0];
                REQUIRE(L.size() >= 96000);
                double before = 0, after = 0;
                for (std::size_t i = 1000; i < 20000; ++i)
                    before = std::max(before, std::fabs(double(L[i])));
                for (std::size_t i = 72000; i < 96000; ++i)
                    after = std::max(after, std::fabs(double(L[i])));
                CHECK(before > 0.05);
                CHECK(after == 0.0);
            }
}

TEST_CASE("MIDI monitoring preserves every event in a device block" * doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        auto pool = makePool();
        MediaCache media;
        // Both messages fit in one block: losing the off leaves a stuck voice.
        DummyBackend backend(keyboard({{10, 0x90, 60, 100}, {100, 0x80, 60, 0}}, 512));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.blockSize = 256, .midiInputs = true}));
        REQUIRE(engine.load(keyboardSession()));
        REQUIRE(engine.awaitPlan());
        backend.run(2);
        engine.stop();
        const auto& signal = backend.capture().channels[0];
        CHECK(signal[50] != 0.0f);
        for (std::size_t i = 100; i < signal.size(); ++i) REQUIRE(signal[i] == 0.0f);
    }
}

TEST_CASE("live buffers cover a device block larger than the render block" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(constantInput(512));
    Engine engine(pool, media, {.renderBlock = 64, .workers = 0});
    REQUIRE(engine.start(backend, {.blockSize = 256, .inputs = 2}));
    REQUIRE(engine.load(monitoring(synthSession())));
    // Check before driving it: the unfixed plan would overrun its buffers.
    REQUIRE(EngineTestAccess::latestPlan(engine).block >= 256);
    REQUIRE(engine.awaitPlan());
    backend.run(2);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    for (float sample : backend.capture().channels[0])
        CHECK(sample == doctest::Approx(0.5).epsilon(1e-5));
}

TEST_CASE("seeking restarts the console seeds in both executors" * doctest::description("rt")) {
    model::Session s = synthSession();
    model::EditTxn edit(s, 2);
    auto project = edit.project();
    project.console9 = true;
    project.seed = 12345;
    edit.setProject(project);
    s = edit.commit().value();
    auto offlinePool = makePool();
    auto reference = renderOffline(s, offlinePool, {.endSeconds = 0.1, .threads = 1});
    REQUIRE(reference);
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.captureFrames = 8192});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        REQUIRE(engine.load(s));
        engine.play();
        if (options.anticipative) REQUIRE(engine.awaitRendered(2048));
        backend.run(16);
        engine.seek(0);
        if (options.anticipative) REQUIRE(engine.awaitRendered(6144));
        backend.run(48);
        engine.stop();
        CHECK(maxDiff(backend.capture().channels[0], 2048, reference->audio.channels[0], 4800) ==
              0);
        CHECK(maxDiff(backend.capture().channels[1], 2048, reference->audio.channels[1], 4800) ==
              0);
    }
}

TEST_CASE("MIDI recording: events land on the timeline where they were heard, per loop pass" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        // Loop 0.5 s .. 1 s from 0: pass 0 is 0 .. 1 s, then passes of 0.5 s
        // (device frames 48000.. and 72000..). The same note in each pass.
        DummyBackend backend(keyboard({{30000, 0x90, 60, 90},
                                       {36000, 0x80, 60, 40},
                                       {54000, 0x90, 60, 90},
                                       {60000, 0x80, 60, 40},
                                       {78000, 0x90, 62, 90},
                                       {84000, 0x80, 62, 40},
                                       {90000, 0xb0, 1, 64}},
                                      0));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
        const model::Session s = keyboardSession(true);
        REQUIRE(engine.load(s));
        Id track;
        for (const auto& e : s.tracks)
            if (e.value->type == model::StripType::Channel) track = e.id;
        engine.setLoop(0.5, 1.0, true);
        RecordTarget target;
        target.track = track;
        target.midi = RecordTarget::Midi{midiPortMask(engine.midiPorts(), "key"), 1, 0};
        REQUIRE(engine.startRecording({target}));
        REQUIRE(engine.awaitPlan());  // everything is live: nothing renders ahead
        engine.play();
        engine.awaitTransport();
        backend.run(400);  // 102400 frames
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const RecordedTake& t = (*takes)[0];
        CHECK(t.isMidi);
        REQUIRE(t.midi.size() == 7);
        const std::int64_t want[7][2] = {{30000, 0}, {36000, 0}, {30000, 1}, {36000, 1},
                                         {30000, 2}, {36000, 2}, {42000, 2}};
        for (std::size_t i = 0; i < 7; ++i) {
            CHECK(t.midi[i].timeline == want[i][0]);
            CHECK(t.midi[i].run == static_cast<std::uint32_t>(want[i][1]));
        }
        engine.stop();
    }
}

TEST_CASE("turning MIDI monitoring off while a key is held releases the note" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(keyboard({{500, 0x90, 60, 100}}, 40960));  // held, never released
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
        model::Session s = keyboardSession();
        REQUIRE(engine.load(s));
        REQUIRE(engine.awaitPlan());
        backend.run(40);  // 10240 frames: the note sounds
        model::EditTxn t(s, 2);
        for (const auto& e : s.tracks)
            if (e.id != kMaster)
                t.modify<model::Track>(
                    e.id, [](model::Track& x) { x.record.monitor = model::Monitor::Off; });
        REQUIRE(engine.load(t.commit().value()));
        REQUIRE(engine.awaitPlan());
        backend.run(120);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        double before = 0, after = 0;
        for (std::size_t i = 2000; i < 10000; ++i)
            before = std::max(before, std::fabs(double(L[i])));
        for (std::size_t i = 30000; i < 40960; ++i)
            after = std::max(after, std::fabs(double(L[i])));
        CHECK(before > 0.1);
        CHECK(after == 0.0);
    }
}

TEST_CASE("a monitored MIDI route change releases held notes" * doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}})
        for (int change = 0; change < 3; ++change) {
            INFO("anticipative " << options.anticipative << ", change " << change);
            auto pool = makePool();
            MediaCache media;
            DummyBackend backend(keyboard({{500, 0x90, 60, 100}, {18000, 0x80, 60, 0}}, 30720));
            Engine engine(pool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
            const model::Session s = keyboardSession();
            REQUIRE(engine.load(s));
            REQUIRE(engine.awaitPlan());
            backend.run(40);  // the key is down at the edit
            model::EditTxn t(s, 2);
            for (const auto& e : s.routes)
                if (e.value->from.midiDevice)
                    t.modify<model::Route>(e.id, [&](model::Route& route) {
                        if (change == 1) route.from.midiDevice = "pads";
                        if (change == 2) route.midi->fromChannel = 2;
                    });
            REQUIRE(engine.load(t.commit().value()));
            REQUIRE(engine.awaitPlan());
            backend.run(80);
            engine.stop();
            const auto& audio = backend.capture().channels[0];
            auto peak = [&](std::size_t from, std::size_t to) {
                double p = 0;
                for (std::size_t i = from; i < to; ++i)
                    p = std::max(p, std::fabs(double(audio[i])));
                return p;
            };
            CHECK(peak(2000, 10000) > 0.1);
            if (change == 0) CHECK(peak(12000, 17000) > 0.1);  // unchanged route carries
            CHECK(peak(21000, 30000) == 0.0);  // the old device's off is filtered after edits
        }
}

TEST_CASE("a mute while playing fades over 10 ms instead of clicking" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    model::Session s = synthSession(0.0);
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(10);  // 2560 frames
    model::EditTxn t(s, 2);
    for (const auto& e : s.tracks)
        if (e.id != kMaster) t.modify<model::Track>(e.id, [](model::Track& x) { x.mute = true; });
    REQUIRE(engine.load(t.commit().value()));
    backend.run(10);
    engine.stop();
    const auto& L = backend.capture().channels[0];
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(L[i])));
        return m;
    };
    const double before = peakOf(0, 2560);
    CHECK(peakOf(2560, 2560 + 184) > 0.5 * before);  // still fading
    CHECK(peakOf(2560 + 480, 5120) == 0.0);          // silent after 10 ms
    double jump = 0;  // no sample-to-sample step bigger than the sine's own slope
    for (std::size_t i = 2400; i < 3200; ++i)
        jump = std::max(jump, std::fabs(double(L[i]) - double(L[i - 1])));
    CHECK(jump < 0.02);
}

TEST_CASE("wet and bypass edits retarget an active ramp without a step" *
          doctest::description("rt")) {
    for (const bool bypass : {false, true}) {
        INFO("bypass " << bypass);
        model::Session s = synthSession(0.0, "com.suppleaudio.test.gain");
        Id effect;
        {
            model::EditTxn t(s, 2);
            for (const auto& e : s.fx)
                if (e.value->plugin.id == "com.suppleaudio.test.gain") {
                    effect = e.id;
                    t.modify<model::FxInstance>(e.id, [](model::FxInstance& f) {
                        f.patch =
                            json::parse(
                                R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12,"latency":"short"}})")
                                .value();
                    });
                }
            s = t.commit().value();
        }
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
        REQUIRE(engine.load(s));
        engine.play();
        backend.run(10);
        auto edit = [&](bool turnOff, std::uint64_t version) {
            model::EditTxn t(s, version);
            t.modify<model::FxInstance>(effect, [&](model::FxInstance& f) {
                if (bypass)
                    f.bypass = turnOff;
                else
                    f.wet = turnOff ? 0.0 : 100.0;
            });
            s = t.commit().value();
            REQUIRE(engine.load(s));
        };
        edit(true, 3);
        backend.run(1);  // 256 samples into the 480-sample ramp
        edit(false, 4);
        backend.run(12);
        engine.stop();
        const auto& audio = backend.capture().channels[0];
        double step = 0, baselineStep = 0;
        for (std::size_t i = 2000; i < 2500; ++i)
            baselineStep =
                std::max(baselineStep, std::fabs(double(audio[i]) - double(audio[i - 1])));
        for (std::size_t i = 2700; i < 2900; ++i)
            step = std::max(step, std::fabs(double(audio[i]) - double(audio[i - 1])));
        CHECK(step < baselineStep + 0.005);
        double early = 0, final = 0;
        for (std::size_t i = 2000; i < 2500; ++i)
            early = std::max(early, std::fabs(double(audio[i])));
        for (std::size_t i = 3800; i < 4300; ++i)
            final = std::max(final, std::fabs(double(audio[i])));
        CHECK(final == doctest::Approx(early).epsilon(0.02));
    }
}

TEST_CASE("a long, dense MIDI performance records without overflow (the writer drains)" *
          doctest::description("rt")) {
    // A controller sweep every 8 frames (6000 events/s) for over a second:
    // more than one ring holds, drained while recording.
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Thread,
         .paced = true,
         .midiPorts = {"Keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
             for (std::uint32_t i = 0; i < n; i += 8) {
                 MidiInputEvent m;
                 m.frame = i;
                 m.size = 3;
                 m.bytes[0] = 0xb0;
                 m.bytes[1] = 1;
                 m.bytes[2] = static_cast<std::uint8_t>((frame + i) / 8 % 128);
                 out.push_back(m);
             }
         }});
    Engine engine(pool, media, kInCallback);
    REQUIRE(
        engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true}).ok());
    const model::Session s = keyboardSession(true);
    REQUIRE(engine.load(s));
    Id track;
    for (const auto& e : s.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    RecordTarget target;
    target.track = track;
    target.midi = RecordTarget::Midi{midiPortMask(engine.midiPorts(), "*"), 0, 0};
    REQUIRE(engine.startRecording({target}));
    engine.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto takes = engine.stopRecording();
    REQUIRE(takes);
    const RecordedTake& t = (*takes)[0];
    CHECK(t.overflowFrames == 0);
    CHECK(t.midi.size() > 4096);
    engine.stop();
}

TEST_CASE("monitoring off while playing: the track plays on live, with its instance" *
          doctest::description("rt")) {
    // A monitored instrument track (live) playing short notes every ~1025
    // frames; monitoring switches off mid-play. The renderer could take the
    // track only at its render horizon, before the callback lets go of the
    // instance: the track would pass through for the render-ahead time (~9600
    // silent frames), and a fresh instance instead lost the plugin's state and
    // editor. It stays live, on the same instance, until the transport stops.
    model::Session s = synthSession(0.0);
    {
        model::EditTxn t(s, 2);
        for (const auto& e : s.midi)
            t.modify<model::MidiSource>(e.id, [](model::MidiSource& m) {
                m.sequence.notes.clear();
                for (std::int64_t k = 0; k * 41 < 16 * 960; ++k)
                    m.sequence.notes.push_back({k * 41, 20, 0, 69, 100, 0});
                m.sequence.length = 16 * 960;
            });
        s = t.commit().value();
    }
    const model::Session monitored = monitoring(s, model::Monitor::On);
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
    Engine engine(pool, media, EngineOptions{.workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    REQUIRE(engine.load(monitored));
    REQUIRE(engine.awaitPlan());
    engine.play();
    engine.awaitTransport();
    backend.run(100);  // 25600 frames, all live (nothing renders ahead)
    const Id fx = s.fx.begin()->id;
    host::Instance* const instance = pool.find(fx);
    REQUIRE(instance);
    REQUIRE(engine.load(s));  // monitoring off: still live while playing
    REQUIRE(engine.awaitPlan());
    CHECK(EngineTestAccess::latestPlan(engine).heldLive);
    CHECK(pool.find(fx) == instance);
    backend.run(250);
    CHECK(engine.processErrors() == 0);
    CHECK(engine.transport().sample == 350 * 256);  // no replay from the epoch's start
    // Stopped, the track leaves the live region, still on the same instance.
    engine.pause();
    for (int i = 0; i < 400 && EngineTestAccess::latestPlan(engine).heldLive; ++i) {
        backend.run(1);
        engine.service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const detail::Plan& after = EngineTestAccess::latestPlan(engine);
    CHECK_FALSE(after.heldLive);
    CHECK_FALSE(after.anyLive);
    CHECK(pool.find(fx) == instance);
    engine.stop();
    const auto& L = backend.capture().channels[0];
    std::size_t longest = 0, run = 0;
    for (std::size_t i = 20000; i < 88000; ++i) {
        run = std::fabs(double(L[i])) < 1e-4 ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    CHECK(longest < 3000);  // the gaps between notes (~525) and one cut note, never ~9600
}

TEST_CASE("a transport write in progress never stalls the audio callback" *
          doctest::description("rt")) {
    // The main thread preempted inside a seek/loop write: the callback must
    // not wait for it (it keeps its epoch and loop for another cycle), and
    // both executors then take the new epoch with its own loop.
    for (const bool anticipative : {true, false}) {
        CAPTURE(anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, EngineOptions{.anticipative = anticipative, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
        REQUIRE(engine.load(synthSession(0.0)));
        if (anticipative) REQUIRE(engine.awaitPlan());
        engine.setLoop(0.0, 1.0, true);
        engine.play();
        engine.awaitTransport();
        backend.run(8);
        const std::uint64_t before = EngineTestAccess::epoch(engine);
        const LoopRange old = EngineTestAccess::rtLoop(engine);
        CHECK(old.end == 48000);

        const std::uint64_t seq = EngineTestAccess::beginWrite(engine);
        EngineTestAccess::loopAndEpoch(engine, 4800, 24000);  // not published yet
        auto cycles = std::async(std::launch::async, [&] { backend.run(16); });
        const bool returned = cycles.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        if (!returned) EngineTestAccess::endWrite(engine, seq);  // let it finish, then fail
        cycles.wait();
        REQUIRE(returned);
        if (anticipative) CHECK(EngineTestAccess::rtEpoch(engine) == before);
        CHECK(EngineTestAccess::rtLoop(engine).end == old.end);

        EngineTestAccess::endWrite(engine, seq);
        backend.run(1);
        const LoopRange now = EngineTestAccess::rtLoop(engine);
        CHECK(now.start == 4800);
        CHECK(now.end == 24000);
        if (anticipative) {
            CHECK(EngineTestAccess::rtEpoch(engine) == before + 1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (EngineTestAccess::rendererEpoch(engine) != before + 1 &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(EngineTestAccess::rendererEpoch(engine) == before + 1);
            CHECK(EngineTestAccess::rendererLoop(engine).start == 4800);
            CHECK(EngineTestAccess::rendererLoop(engine).end == 24000);
        }
        engine.stop();
    }
}

TEST_CASE("a keyed plug-in plays in real time as offline, rendered ahead or live (ADR-0018)" *
          doctest::description("rt")) {
    // synthSession's track (a sine and a 64-sample latent gain) keys a key mix
    // on a second track. Monitoring that track makes it live: the key then
    // crosses from the renderer to the callback through a frontier ring.
    for (const bool postSolo : {false, true})
        for (const bool monitored : {false, true})
            for (const EngineOptions& options :
                 {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}}) {
                INFO("monitored " << monitored << ", anticipative " << options.anticipative
                                  << ", workers " << options.workers);
                model::Session s = synthSession(-3.0, "com.suppleaudio.test.gain");
                {
                    model::EditTxn t(s, 2);
                    auto project = t.project();
                    project.console9 = true;
                    project.seed = 8901;
                    t.setProject(project);
                    Id source;
                    for (const auto& e : s.tracks)
                        if (e.id != kMaster) source = e.id;
                    model::Track tr;
                    tr.name = "Keyed";
                    tr.parent = kMaster;
                    const Id keyed = t.create(tr);
                    t.modify<model::Track>(kMaster,
                                           [&](model::Track& m) { m.children.push_back(keyed); });
                    model::FxInstance fx;
                    fx.plugin.id = "com.suppleaudio.test.keymix";
                    const Id f = t.create(fx);
                    t.modify<model::Track>(keyed, [&](model::Track& x) { x.chain.push_back(f); });
                    model::Route key;
                    key.from.track = source;
                    key.to.fx = f;
                    if (postSolo) {
                        key.tap = model::Tap::PostFader;
                        t.modify<model::Track>(
                            keyed, [](auto& keyedTrack) { keyedTrack.solo = model::Solo::On; });
                    }
                    t.create(key);
                    model::Track bus;
                    bus.type = model::StripType::Buss;
                    bus.parent = kMaster;
                    const Id busId = t.create(bus);
                    t.modify<model::Track>(kMaster, [&](auto& m) {
                        std::erase(m.children, keyed);
                        m.children.push_back(busId);
                    });
                    t.modify<model::Track>(keyed, [&](auto& child) { child.parent = busId; });
                    t.modify<model::Track>(busId,
                                           [&](auto& group) { group.children.push_back(keyed); });
                    model::Route send;
                    send.from.track = source;
                    send.to.track = busId;
                    send.tap = model::Tap::PreFader;
                    send.volumeDb = -6;
                    t.create(send);
                    s = t.commit().value();
                }
                if (monitored) s = monitoring(s);  // the last track: the keyed one
                auto pool = makePool();
                auto offline =
                    renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
                auto rtPool = makePool();
                MediaCache media;
                DummyBackend backend(
                    {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
                Engine engine(rtPool, media, options);
                REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2})
                            .ok());
                REQUIRE(engine.load(s));
                if (options.anticipative) REQUIRE(engine.awaitPlan());
                engine.play();
                if (options.anticipative && !monitored)
                    runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
                else if (options.anticipative) {
                    engine.awaitTransport();
                    for (int i = 0; i < (48000 * 2 + 4096) / 128; i += 16) {
                        REQUIRE(engine.awaitRendered(4096));
                        backend.run(16);
                    }
                } else {
                    backend.run((48000 * 2 + 4096) / 128);
                }
                engine.stop();
                CHECK(engine.processErrors() == 0);
                const std::uint32_t L = engine.outputLatency();
                const auto& cap = backend.capture();
                CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
                CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
                double energy = 0;
                for (std::size_t i = 0; i < 96000; ++i)
                    energy += static_cast<double>(offline.audio.channels[0][i]) *
                              static_cast<double>(offline.audio.channels[0][i]);
                CHECK(energy > 1.0);
            }
}

TEST_CASE("a key routed while playing reaches a plug-in that follows port activation (ADR-0018)" *
          doctest::description("rt")) {
    // The key mix offers CLAP port activation: a processing instance cannot
    // be told its key is routed now, so it gets a fresh one that is.
    model::Session s = synthSession();
    Id source, keyedFx;
    {
        model::EditTxn t(s, 2);
        for (const auto& e : s.tracks)
            if (e.id != kMaster) source = e.id;
        model::Track tr;
        tr.name = "Keyed";
        tr.parent = kMaster;
        const Id keyed = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(keyed); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.keymix";
        keyedFx = t.create(fx);
        t.modify<model::Track>(keyed, [&](model::Track& x) { x.chain.push_back(keyedFx); });
        s = t.commit().value();
    }
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(8);
    REQUIRE(pool.find(keyedFx));
    CHECK(pool.find(keyedFx)->processing());
    CHECK_FALSE(pool.find(keyedFx)->sidechainActive());
    {
        model::EditTxn t(s, 3);
        model::Route key;
        key.from.track = source;
        key.to.fx = keyedFx;
        t.create(key);
        s = t.commit().value();
    }
    REQUIRE(engine.load(s));
    backend.run(8);
    REQUIRE(pool.find(keyedFx));
    CHECK(pool.find(keyedFx)->sidechainActive());
    engine.stop();
    CHECK(engine.processErrors() == 0);
}

TEST_CASE("auxiliary channels play in real time as offline, rendered ahead or live (ADR-0020)" *
          doctest::description("rt")) {
    // synthSession's track feeds channels 3/4 of a 4-channel buss whose key
    // mix keys from them. Monitoring a channel under the buss makes the buss
    // live; its aux source must then play live too (aux links stay within
    // one executor).
    for (const bool monitored : {false, true})
        for (const EngineOptions& options :
             {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}}) {
            INFO("monitored " << monitored << ", anticipative " << options.anticipative
                              << ", workers " << options.workers);
            model::Session s = synthSession(-3.0, "com.suppleaudio.test.gain");
            {
                model::EditTxn t(s, 2);
                auto project = t.project();
                project.console9 = true;
                project.seed = 4321;
                t.setProject(project);
                Id source;
                for (const auto& e : s.tracks)
                    if (e.id != kMaster) source = e.id;
                model::Track bus;
                bus.type = model::StripType::Buss;
                bus.parent = kMaster;
                bus.channels = 4;
                const Id busId = t.create(bus);
                t.modify<model::Track>(kMaster, [&](auto& m) { m.children.push_back(busId); });
                model::FxInstance fx;
                fx.plugin.id = "com.suppleaudio.test.keymix";
                fx.pins.key = {3, 4};
                const Id f = t.create(fx);
                t.modify<model::Track>(busId, [&](model::Track& x) { x.chain.push_back(f); });
                model::Track mon;
                mon.name = "Mon";
                mon.parent = busId;
                const Id monId = t.create(mon);
                t.modify<model::Track>(busId, [&](auto& b) { b.children.push_back(monId); });
                model::Route aux;
                aux.from.track = source;
                aux.to.track = busId;
                aux.tap = model::Tap::PreFader;
                aux.volumeDb = -6;
                aux.channels = {{1, 2}, {3, 4}};
                t.create(aux);
                s = t.commit().value();
            }
            if (monitored) s = monitoring(s);  // the last channel: "Mon", under the buss
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
            REQUIRE(offline.warnings.empty());
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}).ok());
            REQUIRE(engine.load(s));
            if (options.anticipative) REQUIRE(engine.awaitPlan());
            engine.play();
            if (options.anticipative && !monitored)
                runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
            else if (options.anticipative) {
                engine.awaitTransport();
                for (int i = 0; i < (48000 * 2 + 4096) / 128; i += 16) {
                    REQUIRE(engine.awaitRendered(4096));
                    backend.run(16);
                }
            } else {
                backend.run((48000 * 2 + 4096) / 128);
            }
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const std::uint32_t L = engine.outputLatency();
            const auto& cap = backend.capture();
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
            double energy = 0;
            for (std::size_t i = 0; i < 96000; ++i)
                energy += static_cast<double>(offline.audio.channels[0][i]) *
                          static_cast<double>(offline.audio.channels[0][i]);
            CHECK(energy > 1.0);
        }
}

TEST_CASE("a multi-out instrument's outputs play in real time as offline (ADR-0021)" *
          doctest::description("rt")) {
    // Drums (6 channels, not to its parent) plays the multi-out fixture with
    // its Kick and Snare on 3/4 and 5/6; the Snare's pair feeds buss K. With
    // "Mon" (under K) monitored, K plays live and Drums with it.
    for (const bool monitored : {false, true})
        for (const EngineOptions& options :
             {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}}) {
            INFO("monitored " << monitored << ", anticipative " << options.anticipative
                              << ", workers " << options.workers);
            model::EditTxn t(model::Session::create({.seed = 4321}), 2);
            model::Track drums;
            drums.name = "Drums";
            drums.parent = kMaster;
            drums.channels = 6;
            drums.parentSend.enabled = false;
            const Id drumsId = t.create(drums);
            model::Track bus;
            bus.type = model::StripType::Buss;
            bus.parent = kMaster;
            const Id busId = t.create(bus);
            t.modify<model::Track>(kMaster, [&](auto& m) {
                m.children.push_back(drumsId);
                m.children.push_back(busId);
            });
            model::FxInstance fx;
            fx.plugin.id = "com.suppleaudio.test.multiout";
            fx.pins.out = {1, 2, 3, 4, 5, 6};
            const Id f = t.create(fx);
            t.modify<model::Track>(drumsId, [&](model::Track& x) { x.chain.push_back(f); });
            model::Track mon;
            mon.name = "Mon";
            mon.parent = busId;
            const Id monId = t.create(mon);
            t.modify<model::Track>(busId, [&](auto& b) { b.children.push_back(monId); });
            model::Route snare;
            snare.from.track = drumsId;
            snare.to.track = busId;
            snare.volumeDb = -6;
            snare.channels = {{5, 6}, {1, 2}};
            t.create(snare);
            model::Session s = t.commit().value();
            if (monitored) s = monitoring(s);  // the last channel: "Mon", under the buss
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 1.0}).value();
            REQUIRE(offline.warnings.empty());
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}).ok());
            REQUIRE(engine.load(s));
            if (options.anticipative) REQUIRE(engine.awaitPlan());
            engine.play();
            if (options.anticipative && !monitored)
                runAhead(engine, backend, (48000 + 4096) / 128, 128);
            else if (options.anticipative) {
                engine.awaitTransport();
                for (int i = 0; i < (48000 + 4096) / 128; i += 16) {
                    REQUIRE(engine.awaitRendered(4096));
                    backend.run(16);
                }
            } else {
                backend.run((48000 + 4096) / 128);
            }
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const std::uint32_t L = engine.outputLatency();
            const auto& cap = backend.capture();
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 48000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 48000) == 0.0);
            double energy = 0;
            for (std::size_t i = 0; i < 48000; ++i)
                energy += static_cast<double>(offline.audio.channels[0][i]) *
                          static_cast<double>(offline.audio.channels[0][i]);
            CHECK(energy > 100.0);
        }
}

TEST_CASE("multiple masters: real-time main output is independent of later roots" *
          doctest::description("rt")) {
    model::EditTxn t(synthSession(), 2);
    model::Track master;
    master.type = model::StripType::Master;
    const Id alt = t.create(master);
    auto project = t.project();
    project.masters.push_back(alt);
    t.setProject(project);
    model::FxInstance gain;
    gain.plugin.id = "com.suppleaudio.test.gain";
    gain.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value();
    const Id fx = t.create(gain);
    t.modify<model::Track>(alt, [&](auto& tr) { tr.chain.push_back(fx); });
    const auto s = t.commit();
    REQUIRE(s);
    auto pool = makePool();
    auto offline = renderOffline(*s, pool, {.startSeconds = 0, .endSeconds = 0.1});
    REQUIRE(offline);
    CHECK(offline->outputLatency == 0);
    auto other = renderOffline(*s, pool, {.master = alt, .startSeconds = 0, .endSeconds = 0.1});
    REQUIRE(other);
    CHECK(other->outputLatency == 64);
    for (const auto& options : {kInCallback, EngineOptions{.workers = 3}}) {
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(*s));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 64, 128);
        else
            backend.run(64);
        CHECK(engine.outputLatency() == 0);
        CHECK(engine.processErrors() == 0);
        engine.stop();
        CHECK(maxDiff(backend.capture().channels[0], 0, offline->audio.channels[0], 4800) == 0);
    }
}

TEST_CASE("a master key is legal and its downstream node follows the live executor") {
    model::EditTxn t(monitoring(synthSession()), 2);
    model::Track alt;
    alt.type = model::StripType::Master;
    const Id root = t.create(alt);
    auto project = t.project();
    project.masters.push_back(root);
    t.setProject(project);
    model::FxInstance keyed;
    keyed.plugin.id = "com.suppleaudio.test.keymix";
    const Id fx = t.create(keyed);
    t.modify<model::Track>(root, [&](auto& tr) { tr.chain.push_back(fx); });
    model::Route key;
    key.from.track = kMaster;
    key.to.fx = fx;
    t.create(key);
    t.modify<model::Track>(kMaster, [](auto& tr) { tr.hwOutput = std::vector<int>{1, 2}; });
    auto s = t.commit();
    REQUIRE(s);
    auto pool = makePool();
    auto plan = detail::compile(*s, pool, 0, 128);
    REQUIRE(plan);
    CHECK(plan->nodes[plan->mainMaster].live);
    auto other = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                              [&](const auto& n) { return n.id == root; });
    REQUIRE(other != plan->nodes.end());
    CHECK(other->live);
}

// The meters (P5.3c, engine.md §2.3): a strip's reading covers what the
// listener has *heard*. With anticipation the renderer is up to `render_ahead`
// ahead of the playhead, so its numbers must be held back until the callback
// has played them.
TEST_CASE("meters report what has been heard, not what has been rendered" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-meters-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    // A -6 dBFS sine item (amplitude 0.5) on one track, a silent track beside it.
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(96000));
    for (std::size_t i = 0; i < tone.channels[0].size(); ++i) {
        const double s = 0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0);
        tone.channels[0][i] = static_cast<float>(s);
        tone.channels[1][i] = static_cast<float>(s);
    }
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone));

    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track sine;
    sine.name = "Sine";
    sine.parent = kMaster;
    sine.volumeDb = -6.0;  // so the meter's fader factor is not unity either
    const Id sineTrack = t.create(sine);
    model::Track quiet;
    quiet.name = "Quiet";
    quiet.parent = kMaster;
    const Id quietTrack = t.create(quiet);
    t.modify<model::Track>(kMaster, [&](model::Track& m) {
        m.children.push_back(sineTrack);
        m.children.push_back(quietTrack);
    });
    const Id md = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 96000});
    model::Take take;
    take.source = md;
    const Id tk = t.create(take);
    model::Item item;
    item.track = sineTrack;
    item.length = 2.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const model::Session session = t.commit().value();

    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, EngineOptions{.workers = 0});  // anticipative
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(session, dir));
    const auto bank = engine.meterBank();
    REQUIRE(bank != nullptr);
    const Id master = kMaster;
    // One read per call is one GUI frame: the readings cover the audio heard
    // since the previous frame, so a test must not read twice and expect both.
    struct Levels {
        double peak = -1.0, rms = -1.0;
    };
    auto frame = [&] {
        const Engine::MeterClock clock = engine.meterClock();
        return bank->read(clock.epoch, clock.heardStream);
    };
    auto levelOf = [](const std::vector<MeterReading>& readings, model::Id which) {
        Levels out;
        for (const MeterReading& r : readings)
            if (r.track == which) {
                out.peak = std::max(static_cast<double>(r.peak[0]), static_cast<double>(r.peak[1]));
                out.rms = std::max(static_cast<double>(r.rms[0]), static_cast<double>(r.rms[1]));
            }
        return out;
    };

    engine.play();
    // The renderer is 200 ms ahead of the playhead and the callback has pulled
    // nothing, so nothing has been heard yet: every meter is silent.
    REQUIRE(engine.awaitRendered(9600));
    CHECK(engine.cycles() == 0);
    const auto before = frame();
    CHECK(levelOf(before, sineTrack).peak == 0.0);
    CHECK(levelOf(before, quietTrack).peak == 0.0);
    CHECK(levelOf(before, master).peak == 0.0);

    // The callback pulls 200 ms: the sine is heard, the silent strip is not.
    runAhead(engine, backend, 9600 / 128, 128);
    CHECK(engine.cycles() >= 9600 / 128);
    const auto after = frame();
    CHECK(levelOf(after, quietTrack).peak == 0.0);
    CHECK(levelOf(after, quietTrack).rms == 0.0);
    // A strip's meter is its signal after the fader and after the pan law, and
    // before the Console 9 encode — so a -6 dBFS item on a -6 dB fader reads
    // its own peak times the fader law times the pan law at centre: unity
    // under the default 0 dB law (ADR-0066).
    const double panLeft = 1.0;
    double sourcePeak = 0.0;
    for (std::size_t i = 0; i < 9600 && i < tone.channels[0].size(); ++i)
        sourcePeak = std::max(sourcePeak, std::fabs(static_cast<double>(tone.channels[0][i])));
    CHECK(sourcePeak > 0.49);  // the item is -6 dBFS
    const double fader = console9::faderGain(-6.0);
    CHECK(levelOf(after, sineTrack).peak ==
          doctest::Approx(sourcePeak * fader * panLeft).epsilon(1e-3));
    // A steady sine: the RMS is the peak over √2, so a strip's peak and energy
    // are of the same signal.
    CHECK(levelOf(after, sineTrack).rms * std::sqrt(2.0) ==
          doctest::Approx(levelOf(after, sineTrack).peak).epsilon(0.01));
    // What is heard: the master's meter is exactly the level an offline render
    // of the same span writes — metering never touches the signal.
    auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 0.2, .projectDir = dir});
    REQUIRE(offline);
    double offlinePeak = 0.0;
    for (float sample : offline->audio.channels[0])
        offlinePeak = std::max(offlinePeak, std::fabs(static_cast<double>(sample)));
    CHECK(levelOf(after, master).peak == doctest::Approx(offlinePeak).epsilon(1e-6));
    CHECK_FALSE(bank->droppedRenderer());  // the reader kept up
    engine.stop();
    std::filesystem::remove_all(dir);
}

// A monitored input is heard while the transport is stopped (nothing is
// rendered ahead of it, so there is no play position to wait for): its meter
// shows it, and the master's meter is exactly what the device was given.
TEST_CASE("meters show a monitored input while the transport is stopped" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(constantInput(4096));
    Engine engine(pool, media, kInCallback);  // no anticipation: the callback meters
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    REQUIRE(engine.load(monitoring(synthSession(-6.0206))));  // its track monitors 1-2
    backend.run(4);                                           // stopped: input only

    const auto bank = engine.meterBank();
    REQUIRE(bank != nullptr);
    const Engine::MeterClock clock = engine.meterClock();
    const auto readings = bank->read(clock.epoch, clock.heardStream);
    double monitorPeak = -1.0, masterPeak = -1.0;
    for (const MeterReading& r : readings) {
        const double peak =
            std::max(static_cast<double>(r.peak[0]), static_cast<double>(r.peak[1]));
        if (r.track == kMaster)
            masterPeak = peak;
        else
            monitorPeak = std::max(monitorPeak, peak);
    }
    CHECK(monitorPeak > 0.0);  // the monitored input reached the strip
    const auto& cap = backend.capture();
    double devicePeak = 0.0;
    for (float sample : cap.channels[0])
        devicePeak = std::max(devicePeak, std::fabs(static_cast<double>(sample)));
    CHECK(masterPeak == doctest::Approx(devicePeak).epsilon(1e-6));
    engine.stop();
}

TEST_CASE("a count-in clicks a bar while the position holds, then plays and records on time" *
          doctest::description("rt")) {
    // 120 BPM 4/4 at 48 kHz: a beat is 24000 frames, the bar 96000. Block 333
    // splits a cycle where the count ends.
    constexpr std::uint32_t kBlock = 333;
    constexpr std::int64_t kBar = 96000, kTotal = 130000, kCycles = kTotal / kBlock + 1;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto runOnce = [&](bool countIn, std::vector<float>& out, RecordedTake* recorded) {
            auto pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                                  .captureFrames = kCycles * kBlock,
                                  .input = [](float* const* in, std::uint32_t channels,
                                              std::uint32_t n, std::int64_t frame) {
                                      for (std::uint32_t c = 0; c < channels; ++c)
                                          for (std::uint32_t i = 0; i < n; ++i)
                                              in[c][i] = static_cast<float>(frame + i);
                                  }});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock, .inputs = 2})
                        .ok());
            const model::Session s = synthSession(0.0);
            REQUIRE(engine.load(s));
            engine.setMetronome(false, 0.0);  // the count clicks regardless
            const auto dir = std::filesystem::temp_directory_path() /
                             ("supple-count-" + std::to_string(::getpid()));
            std::filesystem::remove_all(dir);
            if (recorded) {
                Id track;
                for (const auto& e : s.tracks)
                    if (e.value->type == model::StripType::Channel) track = e.id;
                REQUIRE(engine.startRecording({{track, {0, 1}, dir / "take.wav", {}}}));
            }
            if (countIn)
                engine.playCountIn(kBar, kBar / 4, 4);
            else
                engine.play();
            const std::int64_t half = kBar / 2 / kBlock;  // mid-count
            if (options.anticipative)
                runAhead(engine, backend, half, kBlock);
            else
                backend.run(half);
            if (countIn) {
                const TransportState mid = engine.transport();
                CHECK(mid.countingIn);
                CHECK(mid.sample == 0);
            }
            if (options.anticipative)
                runAhead(engine, backend, kCycles - half, kBlock);
            else
                backend.run(kCycles - half);
            CHECK_FALSE(engine.transport().countingIn);
            if (recorded) {
                auto takes = engine.stopRecording();
                REQUIRE(takes);
                REQUIRE(takes->size() == 1);
                *recorded = (*takes)[0];
                auto audio = audio::readAudio(recorded->file);
                REQUIRE(audio);
                REQUIRE(audio->frames() > 0);
                // The take's first frame is the input heard when the count ended.
                CHECK(audio->channels[0][0] == static_cast<float>(kBar));
            }
            engine.stop();
            out = backend.capture().channels[0];
            std::filesystem::remove_all(dir);
        };
        std::vector<float> reference, counted;
        runOnce(false, reference, nullptr);
        RecordedTake take;
        runOnce(true, counted, &take);
        REQUIRE(counted.size() >= static_cast<std::size_t>(kTotal));
        // Four clicks, the first accented, then silence to the end of the bar.
        std::vector<std::size_t> onsets;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kBar); ++i)
            if (std::fabs(counted[i]) > 0.05f && (onsets.empty() || i > onsets.back() + 4000))
                onsets.push_back(i);
        REQUIRE(onsets.size() == 4);
        for (std::size_t b = 0; b < 4; ++b)  // (a click starts at phase 0: its second sample)
            CHECK(onsets[b] - b * 24000 <= 2);
        float accent = 0, other = 0;
        for (std::size_t i = 0; i < 1920; ++i) {
            accent = std::max(accent, std::fabs(counted[i]));
            other = std::max(other, std::fabs(counted[24000 + i]));
        }
        CHECK(accent > other);
        for (std::size_t i = 74000; i < static_cast<std::size_t>(kBar); ++i)
            REQUIRE(counted[i] == 0.0f);
        // Then the session plays exactly as it does without a count, from frame 0.
        CHECK(maxDiff(counted, kBar, reference, kTotal - kBar) == 0.0);
        CHECK(take.startSample == 0);
        CHECK(take.frames == kCycles * kBlock - kBar);
    }
}

TEST_CASE("the metronome clicks every beat in the live output, accented on the bar" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const model::Session s = model::Session::create({.console9 = false});  // 120 BPM, 4/4
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s));
        engine.setMetronome(true, 0.0);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 48000 * 2 / 128, 128);
        else
            backend.run(48000 * 2 / 128);
        engine.stop();
        const auto& l = backend.capture().channels[0];
        const auto& r = backend.capture().channels[1];
        REQUIRE(l.size() >= 96000);
        // Find the clicks: the first sample louder than 0.05 after silence.
        std::vector<std::size_t> onsets;
        for (std::size_t i = 0; i < 96000; ++i)
            if (std::fabs(l[i]) > 0.05f && (onsets.empty() || i > onsets.back() + 4000))
                onsets.push_back(i);
        REQUIRE(onsets.size() == 4);  // beats 1-4 of bar 1 in two seconds
        for (std::size_t b = 1; b < onsets.size(); ++b)
            CHECK(static_cast<double>(onsets[b] - onsets[b - 1]) ==
                  doctest::Approx(24000).epsilon(0.002));
        auto peak = [&](std::size_t from) {
            float p = 0;
            for (std::size_t i = from; i < from + 1920 && i < l.size(); ++i)
                p = std::max(p, std::fabs(l[i]));
            return p;
        };
        CHECK(peak(onsets[0]) > 0.9f);  // the bar's first beat
        CHECK(peak(onsets[1]) < 0.7f);  // the others, softer
        CHECK(peak(onsets[1]) > 0.4f);
        CHECK(std::fabs(l[onsets[1] + 6000]) == 0.0f);    // silence between clicks
        CHECK(l[onsets[2] + 100] == r[onsets[2] + 100]);  // centred
    }
    // A two-beat loop, at unity and under a master playrate: every pass
    // clicks both beats, the bar's first, at the loop start, after each wrap too.
    for (const double rate : {1.0, 1.25})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("loop, rate " << rate << ", anticipative " << options.anticipative);
            model::EditTxn t(model::Session::create({.console9 = false}), 1);
            auto project = t.project();
            project.playrate = {rate, false};
            t.setProject(project);
            const model::Session s = t.commit().value();
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
            REQUIRE(engine.load(s));
            REQUIRE(engine.awaitPlan());
            engine.setMetronome(true, 0.0);
            engine.setLoop(0.0, 1.0, true);  // beats 1 and 2 of bar 1
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, 48000 * 2 / 128, 128);
            else
                backend.run(48000 * 2 / 128);
            engine.stop();
            const auto& l = backend.capture().channels[0];
            REQUIRE(l.size() >= 96000);
            std::vector<std::size_t> onsets;
            for (std::size_t i = 0; i < 96000; ++i)
                if (std::fabs(l[i]) > 0.05f && (onsets.empty() || i > onsets.back() + 4000))
                    onsets.push_back(i);
            const double beat = 24000.0 / rate;  // device frames
            // Two seconds: 4 beats at unity, 5 at 1.25 (0, 19200, ... 76800).
            REQUIRE(onsets.size() ==
                    static_cast<std::size_t>(std::floor(96000.0 / beat - 1e-9)) + 1);
            for (std::size_t b = 0; b < onsets.size(); ++b) {
                INFO("beat " << b);
                CHECK(std::fabs(double(onsets[b]) - double(b) * beat) <= 2.0);
                float p = 0;
                for (std::size_t i = onsets[b]; i < onsets[b] + 1920; ++i)
                    p = std::max(p, std::fabs(l[i]));
                if (b % 2 == 0)
                    CHECK(p > 0.9f);  // the loop start: the bar's first beat
                else
                    CHECK(p < 0.7f);
            }
        }
}

TEST_CASE("live bridged effects pass through without callback lifecycle or processing") {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (auto isolation : {model::Isolation::SharedBridge, model::Isolation::Dedicated})
        for (const auto& options : {kInCallback, EngineOptions{.workers = 0}}) {
            auto pool = makePool();
            MediaCache media;
            DummyBackend backend(constantInput(1024));
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
            auto base = monitoring(synthSession(0, "com.suppleaudio.test.gain"));
            model::EditTxn t(base, 2);
            Id fxId;
            for (const auto& fx : base.fx)
                if (fx.value->plugin.id == "com.suppleaudio.test.gain") {
                    fxId = fx.id;
                    t.modify<model::FxInstance>(fx.id, [&](auto& f) {
                        f.isolation = isolation;
                        f.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0",
                        "params":{"gain":-12,"latency":"short"}})")
                                      .value();
                    });
                }
            auto session = t.commit().value();
            REQUIRE(engine.load(session));
            if (options.anticipative) REQUIRE(engine.awaitPlan());
            backend.run(4);
            REQUIRE(pool.find(fxId));
            CHECK_FALSE(pool.find(fxId)->processing());
            auto warnings = engine.warnings();
            CHECK(std::any_of(warnings.begin(), warnings.end(), [&](const auto& w) {
                return w.find(fxId.str()) != std::string::npos &&
                       w.find("in_process") != std::string::npos;
            }));
            CHECK(double(backend.capture().channels[0][300]) == doctest::Approx(.5).epsilon(1e-4));
            CHECK(double(backend.capture().channels[1][300]) ==
                  doctest::Approx(-.25).epsilon(1e-4));
            CHECK(std::all_of(backend.capture().channels[0].begin(),
                              backend.capture().channels[0].begin() + 64,
                              [](float value) { return value == 0; }));
            engine.stop();
            model::EditTxn unmonitor(session, 3);
            for (const auto& track : session.tracks)
                unmonitor.modify<model::Track>(
                    track.id, [](auto& tr) { tr.record.monitor = model::Monitor::Off; });
            const auto aheadSession = unmonitor.commit().value();
            auto referencePool = makePool();
            const auto reference =
                renderOffline(aheadSession, referencePool, {.startSeconds = 0, .endSeconds = .1})
                    .value();
            CHECK(reference.warnings.empty());
            if (options.anticipative) {
                DummyBackend aheadBackend(
                    {.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
                Engine aheadEngine(pool, media, options);
                REQUIRE(aheadEngine.start(aheadBackend,
                                          {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
                REQUIRE(aheadEngine.load(aheadSession));
                REQUIRE(aheadEngine.awaitPlan());
                aheadEngine.play();
                runAhead(aheadEngine, aheadBackend, 48, 128);
                aheadEngine.stop();
                CHECK(maxDiff(aheadBackend.capture().channels[0], aheadEngine.outputLatency(),
                              reference.audio.channels[0], 4800) == 0);
            }
        }
}

TEST_CASE("recording header errors retain the first error and spare other targets") {
    using Access = detail::RecorderTestAccess;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-rec-header-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    detail::Recorder recorder({{Id{model::Kind::Track, 1}, {0}, dir / "bad.wav", {}},
                               {Id{model::Kind::Track, 2}, {0}, dir / "good.wav", {}}},
                              16, 0);
    Access::stall(recorder);
    REQUIRE(recorder.open());
    REQUIRE(waitForRecorder([&] { return Access::stalled(recorder); }));
    REQUIRE(Access::failHeader(recorder, 0));
    Access::updateHeaders(recorder);  // the same routine the writer runs periodically
    Access::failTarget(recorder, 0);  // later append errors must not replace the header error
    const std::array<float, 32> data{};
    const float* input[] = {data.data()};
    recorder.capture(input, 1, 32, 0);
    const auto takes = recorder.finish();
    REQUIRE(takes.size() == 2);
    CHECK(takes[0].error.find("file.write") != std::string::npos);
    CHECK(takes[1].error.empty());
    CHECK(takes[1].frames == 32);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a full recording loop table keeps its runs and stops adding input") {
    using Access = detail::RecorderTestAccess;
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-runs-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    detail::Recorder recorder({{Id{model::Kind::Track, 1}, {0}, dir / "take.wav", {}}}, 1, 0);
    Access::stall(recorder);
    REQUIRE(recorder.open());
    REQUIRE(waitForRecorder([&] { return Access::stalled(recorder); }));
    const float sample = .5f;
    const float* input[] = {&sample};
    for (int pass = 0; pass < 4100; ++pass) recorder.capture(input, 1, 1, 0);
    const auto takes = recorder.finish({0, 1, true});
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].error.empty());
    CHECK(takes[0].frames == 4097);
    REQUIRE(takes[0].segments.size() == 4097);
    CHECK(takes[0].segments.back().fileFrame == 4096);
    REQUIRE(takes[0].warnings.size() == 1);
    CHECK(takes[0].warnings[0].code == "recording.loop_runs_full");
    const auto side = detail::readSidecar(takes[0].file);
    REQUIRE(side);
    CHECK(side->segments.size() == 4097);
    std::filesystem::remove_all(dir);
}

TEST_CASE("recording recovery requires a complete valid sidecar") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-rec-sidecar-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto wav = dir / "take.wav";
    auto original = json::parse(R"({"schema":"supple.recording/1.0","track":"tr_000001",
        "start_sample":-4,"sample_rate":48000,"segments":[[0,-4],[10,0]],"loop":[0,10]})")
                        .value();
    // Use the model's actual canonical id spelling.
    original["track"] = Id{model::Kind::Track, 1}.str();
    REQUIRE(writeFileAtomic(detail::sidecarPath(wav), json::writeCanonical(original)));
    REQUIRE(detail::readSidecar(wav));
    for (int mutation = 0; mutation < 12; ++mutation) {
        auto v = original;
        switch (mutation) {
            case 0:
                v["schema"] = "other/1.0";
                break;
            case 1:
                v["schema"] = "supple.recording/2.0";
                break;
            case 2:
                v["track"] = Id{model::Kind::Fx, 1}.str();
                break;
            case 3:
                v["sample_rate"] = 0;
                break;
            case 4:
                v["sample_rate"] = -1;
                break;
            case 5:
                v["sample_rate"] = "48000";
                break;
            case 6:
                v["segments"] = json::Value::array();
                break;
            case 7:
                v["loop"] = json::Value();
                break;
            case 8:
                v["segments"] = json::parse("[[0,-4],[-1,0]]").value();
                break;
            case 9:
                v["segments"] = json::parse("[[0,-4],[0,0]]").value();
                break;
            case 10:
                v["loop"] = json::parse("[4,2]").value();
                break;
            case 11:
                v["segments"] = json::parse("[[0,0],[10,0]]").value();
                break;
        }
        REQUIRE(writeFileAtomic(detail::sidecarPath(wav), json::writeCanonical(v)));
        const auto info = detail::readSidecar(wav);
        CHECK_FALSE(info);
        if (!info) {
            CHECK(info.error().code == "record.sidecar");
            CHECK_FALSE(info.error().path.empty());
            CHECK_FALSE(info.error().hint.empty());
        }
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("renderer waits for stopped block deadlines with bounded safety waits") {
    using Access = EngineTestAccess;
    using namespace std::chrono;
    const steady_clock::time_point now{};
    // 1024 frames / 48 kHz: the actual stopped processing deadline, not 1 ms.
    const auto next = now + duration_cast<steady_clock::duration>(duration<double>(1024.0 / 48000));
    CHECK(Access::deadline(now, next, true) == next);
    CHECK(Access::deadline(now, now - milliseconds(5), true) == now - milliseconds(5));
    CHECK(Access::deadline(now, now + seconds(5), true) == now + seconds(1));
    CHECK(Access::deadline(now, next, false) == now + seconds(1));
}

TEST_CASE("stopped processing requests wait for minimum spacing without a poll") {
    using Access = EngineTestAccess;
    using namespace std::chrono;
    const steady_clock::time_point completed{};
    const auto next = completed + milliseconds(21);
    CHECK(Access::stoppedDeadline(next, completed, true) == completed + milliseconds(1));
    CHECK(Access::stoppedDeadline(next, completed, false) == next);
    // A sequence notification without a pending audio request changes no due date.
    CHECK(Access::deadline(completed + microseconds(500),
                           Access::stoppedDeadline(next, completed, false), true) == next);
    // A plug-in requests again from each process(): each completion defers
    // the next requested block by 1 ms, rather than triggering immediate work.
    auto finished = completed;
    for (int block = 0; block < 8; ++block) {
        const auto due = Access::stoppedDeadline(finished + milliseconds(21), finished, true);
        CHECK(due == finished + milliseconds(1));
        CHECK(Access::deadline(finished, due, true) == due);
        finished = due + microseconds(20);
    }
    // Normal blocks may have a shorter period than the early-request spacing.
    CHECK(Access::stoppedDeadline(completed + microseconds(333), completed, true) ==
          completed + microseconds(333));
}

TEST_CASE("playback safety wait is half the effective render-ahead span") {
    auto pool = makePool();
    MediaCache media;
    Engine engine(pool, media, {.workers = 0});
    detail::Plan plan;
    using Access = EngineTestAccess;
    using namespace std::chrono;
    const steady_clock::time_point now{};
    Access::rendererState(engine, plan, 9600, 9600);  // 200 ms at 48 kHz
    REQUIRE(Access::safety(engine) == milliseconds(100));
    CHECK(Access::deadline(now, now, false, Access::safety(engine)) == now + milliseconds(100));
    Access::rendererState(engine, plan, 480, 480);  // 10 ms at 48 kHz
    CHECK(Access::safety(engine) == milliseconds(5));
    Access::playing(engine, false);
    CHECK(Access::safety(engine) == seconds(1));
    Access::playing(engine, true);
    engine.holdRenderer(true);
    CHECK(Access::safety(engine) == seconds(1));
    engine.holdRenderer(false);
    plan.split = true;  // all live: no playback buffer to replenish
    CHECK(Access::safety(engine) == seconds(1));
    Engine planless(pool, media, {.workers = 0});
    Access::playing(planless, true);
    CHECK(Access::safety(planless) == seconds(1));
}

TEST_CASE("callback wakes renderer once at the consumption threshold") {
    auto pool = makePool();
    MediaCache media;
    Engine engine(pool, media, {.workers = 0});
    detail::Plan plan;
    using Access = EngineTestAccess;
    Access::rendererState(engine, plan, 320, 256);
    REQUIRE(Access::target(engine) == 65);
    CHECK_FALSE(Access::room(engine));
    Access::arm(engine, Access::target(engine));
    const auto before = Access::event(engine);
    Access::consume(engine, 64);
    CHECK(Access::event(engine) == before);
    CHECK_FALSE(Access::room(engine));
    Access::consume(engine, 65);
    CHECK(Access::event(engine) == before + 1);
    CHECK(Access::room(engine));
    Access::consume(engine, 128);
    CHECK(Access::event(engine) == before + 1);
    // Consumption preceding the arm is found by the renderer's second check.
    Access::arm(engine, 65);
    CHECK(Access::room(engine));
    // All-live graphs and held renderers have no consumption deadline.
    plan.split = true;
    CHECK(Access::target(engine) == INT64_MAX);
    CHECK_FALSE(Access::room(engine));
}

TEST_CASE("renderer waits for full master and frontier rings to release a chunk") {
    auto pool = makePool();
    MediaCache media;
    Engine engine(pool, media, {.workers = 0});
    detail::Plan plan;
    using Access = EngineTestAccess;
    Access::rendererState(engine, plan, 256, 512);
    const auto fill = [](detail::ChunkRing& ring, std::uint32_t frames) {
        for (int i = 0; i < 4; ++i) {
            auto* chunk = ring.slot();
            REQUIRE(chunk);
            chunk->epoch = 0;
            chunk->stream = i * frames;
            chunk->frames = frames;
            ring.publish();
        }
    };
    fill(Access::ring(engine), 64);
    REQUIRE(Access::target(engine) == 64);
    CHECK_FALSE(Access::room(engine));
    Access::arm(engine, Access::target(engine));
    const auto before = Access::event(engine);
    Access::consume(engine, 32);
    CHECK(Access::event(engine) == before);
    Access::ring(engine).pop();
    Access::consume(engine, 64);
    CHECK(Access::event(engine) == before + 1);
    CHECK(Access::room(engine));

    plan.split = true;
    plan.frontier = {0, 1};
    for (std::uint32_t frames : {64u, 128u}) {
        plan.frontierRings.push_back(std::make_unique<detail::ChunkRing>(4, frames));
        fill(*plan.frontierRings.back(), frames);
    }
    REQUIRE(Access::target(engine) == 128);
    Access::arm(engine, Access::target(engine));
    plan.frontierRings[0]->pop();
    Access::consume(engine, 64);
    CHECK(Access::event(engine) == before + 1);
    CHECK_FALSE(Access::room(engine));
    plan.frontierRings[1]->pop();
    Access::consume(engine, 128);
    CHECK(Access::event(engine) == before + 2);
    CHECK(Access::room(engine));
    // A seek may flush old chunks without consuming any new-epoch frames.
    plan.split = false;
    Access::rendererState(engine, plan, 0, 512);
    fill(Access::ring(engine), 64);  // epoch 0, while the renderer is now in 1
    REQUIRE(Access::target(engine, 1) == 0);
    Access::arm(engine, Access::target(engine, 1));
    const auto flushed = Access::event(engine);
    Access::ring(engine).pop();
    Access::consume(engine, 0);
    CHECK(Access::event(engine) == flushed + 1);
    CHECK(Access::room(engine, 1));
    // Flushing before the arm is found by the room recheck as well.
    Access::arm(engine, 0);
    CHECK(Access::room(engine, 1));
}

namespace {
struct EngineEdgeControl {
    void (*requestProcess)(const clap_plugin_t*);
    void (*rescan)(const clap_plugin_t*, bool);
    void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
    void (*restart)(const clap_plugin_t*);
};
const EngineEdgeControl& engineEdgeControl(host::Instance& instance) {
    const auto* control = static_cast<const EngineEdgeControl*>(
        instance.raw()->get_extension(instance.raw(), "supple.test.edge-control"));
    REQUIRE(control);
    return *control;
}
model::Session edgeSession(std::uint8_t mode, bool automaticBypass = false, bool live = false) {
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    model::Track track;
    track.parent = kMaster;
    track.name = "Edge";
    const auto tr = edit.create(track);
    edit.modify<model::Track>(kMaster, [&](auto& master) { master.children.push_back(tr); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.edge";
    fx.state = "sha256:engine-edge-config";
    fx.autoBypassOnSilence = automaticBypass;
    const auto id = edit.create(fx);
    edit.modify<model::Track>(tr, [&](auto& t) { t.chain.push_back(id); });
    edit.putBlob(*fx.state, std::make_shared<const model::Blob>(model::Blob{{mode, 0, 0, 0}}));
    if (live) {
        model::Route input;
        input.from.hwChannels = std::vector<int>{1, 2};
        input.to.track = tr;
        const auto route = edit.create(input);
        edit.modify<model::Track>(tr, [&](auto& t) {
            t.record.input = route;
            t.record.monitor = model::Monitor::On;
            t.record.armed = true;
        });
    }
    return edit.commit().value();
}
}  // namespace

TEST_CASE("renderer notifications publish transport holds and plug-in requests") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.blockSize = 64}));
    const auto session = edgeSession(0);
    REQUIRE(engine.load(session));
    backend.run(1);
    auto* instance = pool.find(session.fx.begin()->id);
    REQUIRE(instance);
    std::atomic<std::uint32_t> event{0};
    instance->setAudioWake(&event);
    const auto& control = engineEdgeControl(*instance);
    control.requestProcess(instance->raw());
    CHECK(event.load() == 1);
    CHECK(instance->processRequested());
    REQUIRE(instance->setParam(7, 5));
    CHECK(event.load() == 2);
    // Loading fixture state calls host.params.request_flush.
    const std::array<std::uint8_t, 4> state{};
    REQUIRE(instance->loadState(state));
    CHECK(instance->needsAudioFlush());
    CHECK(event.load() == 3);
    instance->setOwner(nullptr);
    CHECK(event.load() == 4);
    instance->setAudioWake(nullptr);
    control.requestProcess(instance->raw());
    CHECK(event.load() == 4);
    engine.stop();

    Engine ahead(pool, media, {.workers = 0});  // no thread needed to inspect publication
    using Access = EngineTestAccess;
    const auto before = Access::event(ahead);
    ahead.play();
    CHECK(Access::event(ahead) == before + 1);
    ahead.pause();
    CHECK(Access::event(ahead) == before + 2);
    ahead.seek(1);
    CHECK(Access::event(ahead) == before + 3);
    ahead.setLoop(0, 1, true);
    CHECK(Access::event(ahead) == before + 4);
    ahead.holdRenderer(true);
    ahead.holdRenderer(false);
    CHECK(Access::event(ahead) == before + 6);
    CHECK(rt::waitUntil(event, 3, std::chrono::steady_clock::time_point{}));
}

TEST_CASE("plug-in wake detachment permits word destruction during concurrent requests") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.blockSize = 64}));
    const auto session = edgeSession(0);
    REQUIRE(engine.load(session));
    auto* instance = pool.find(session.fx.begin()->id);
    REQUIRE(instance);
    const auto& control = engineEdgeControl(*instance);
    std::binary_semaphore start{0}, finished{0};
    std::thread producer([&] {
        for (int round = 0; round < 128; ++round) {
            start.acquire();
            for (int request = 0; request < 64; ++request) control.requestProcess(instance->raw());
            finished.release();
        }
    });
    for (int round = 0; round < 128; ++round) {
        auto word = std::make_unique<std::atomic<std::uint32_t>>(0);
        instance->setAudioWake(word.get());
        start.release();
        instance->setAudioWake(nullptr);
        word.reset();  // ASan/TSan can observe any notifier outliving detach.
        finished.acquire();
    }
    producer.join();
    CHECK(instance->processRequested());
    engine.stop();
}

TEST_CASE("plugin outputs retain FX epoch timeline flags addresses and counted overflow") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    const auto session = edgeSession(13);
    const auto fx = session.fx.begin()->id;
    REQUIRE(engine.load(session));
    engine.seek(.5);
    engine.play();
    backend.run(1);
    auto first = engine.takePluginOutputs();
    REQUIRE(first.events.size() == 256);
    CHECK(first.dropped == 900 - 256);
    CHECK(first.events[0].fx == fx);
    CHECK(first.events[0].planGeneration == EngineTestAccess::latestPlan(engine).generation);
    CHECK(first.events[0].epoch == EngineTestAccess::epoch(engine));
    CHECK(first.events[0].timelineSample == 24001);
    CHECK(first.events[0].event.kind == host::OutputEvent::Kind::GestureBegin);
    CHECK(first.events[1].event.kind == host::OutputEvent::Kind::ParamValue);
    CHECK(first.events[2].event.kind == host::OutputEvent::Kind::GestureEnd);
    const auto& value = first.events[1].event;
    CHECK(value.flags == (host::OutputEvent::IsLive | host::OutputEvent::DontRecord));
    CHECK(value.port == 1);
    CHECK(value.channel == 2);
    CHECK(value.key == 3);
    CHECK(value.noteId == 4);
    CHECK_FALSE(first.events[1].idle);
    backend.run(20);
    auto overflow = engine.takePluginOutputs();
    CHECK(overflow.events.size() == 4096);
    CHECK(overflow.dropped == first.dropped + 20 * 900 - 4096);
    CHECK(engine.pluginOutputDrops() == overflow.dropped);
    CHECK(engine.takePluginOutputs().events.empty());
    engine.stop();
}

TEST_CASE("idle flush outputs join the main-thread engine take API") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    const auto session = edgeSession(10);
    const auto fx = session.fx.begin()->id;
    REQUIRE(engine.load(session));
    engine.service();
    auto outputs = engine.takePluginOutputs();
    REQUIRE(outputs.events.size() == host::Instance::IdleOutputs::kCapacity);
    CHECK(outputs.dropped == 1800 - host::Instance::IdleOutputs::kCapacity);
    for (const auto& output : outputs.events) {
        CHECK(output.idle);
        CHECK(output.fx == fx);
    }
    CHECK((outputs.events[1].event.flags & host::OutputEvent::DontRecord) != 0);
    engine.stop();
}

TEST_CASE("request_process wakes a silent sleeping FX at the next block") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    const auto session = edgeSession(11, true);
    REQUIRE(engine.load(session));
    engine.play();
    backend.run(1);
    REQUIRE(engine.takePluginOutputs().events.size() == 1);
    auto* instance = pool.find(session.fx.begin()->id);
    REQUIRE(instance);
    CHECK(instance->sleeping());
    backend.run(3);
    CHECK(engine.takePluginOutputs().events.empty());
    engineEdgeControl(*instance).requestProcess(instance->raw());
    backend.run(1);
    auto woke = engine.takePluginOutputs();
    REQUIRE(woke.events.size() == 1);
    CHECK(woke.events[0].event.value == 2);
    CHECK_FALSE(instance->processRequested());
    backend.run(1);
    CHECK(engine.takePluginOutputs().events.empty());
    REQUIRE(instance->setParam(7, 5));
    backend.run(1);
    CHECK(engine.takePluginOutputs().events.size() == 1);
    engine.pause();
    backend.run(1);
    engine.takePluginOutputs();
    engineEdgeControl(*instance).requestProcess(instance->raw());
    backend.run(1);
    CHECK(engine.takePluginOutputs().events.size() == 1);
    engine.stop();
    instance->deactivate();
    CHECK_FALSE(instance->active());
    engineEdgeControl(*instance).requestProcess(instance->raw());
    engine.service();
    CHECK(instance->active());  // lifecycle stays on main
}

TEST_CASE("plug-ins keep running while the transport is stopped, in both executors") {
    // Hosts run plug-ins while stopped: a sampler loads the file its editor
    // picked only in process() (LSP's multi-samplers do). The edge fixture
    // reports every call with an output event.
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.renderBlock = 64, .renderAheadMs = 20, .workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 2}));
        REQUIRE(engine.load(edgeSession(0)));
        if (options.anticipative) REQUIRE(engine.awaitPlan());
        std::size_t calls = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (calls < 8 && std::chrono::steady_clock::now() < deadline) {
            backend.run(1);
            calls += engine.takePluginOutputs().events.size();
            if (options.anticipative) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(calls >= 8);
        CHECK(engine.processErrors() == 0);
        engine.stop();
    }
}

TEST_CASE("both anticipative executors publish plug-in outputs through their own queues") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media,
                  {.anticipative = true, .renderBlock = 64, .renderAheadMs = 20, .workers = 2});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 2}));
    auto session = edgeSession(12, false, true);
    model::EditTxn edit(session, 2);
    model::Track track;
    track.parent = kMaster;
    track.name = "Rendered";
    const auto tr = edit.create(track);
    edit.modify<model::Track>(kMaster, [&](auto& master) { master.children.push_back(tr); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.edge";
    const auto renderedFx = edit.create(fx);
    edit.modify<model::Track>(tr, [&](auto& t) { t.chain.push_back(renderedFx); });
    session = edit.commit().value();
    REQUIRE(engine.load(session));
    engine.play();
    REQUIRE(engine.awaitRendered(128));
    backend.run(2);
    auto outputs = engine.takePluginOutputs();
    CHECK(std::any_of(outputs.events.begin(), outputs.events.end(), [&](const auto& output) {
        return output.fx == renderedFx && output.anticipative;
    }));
    CHECK(std::any_of(outputs.events.begin(), outputs.events.end(), [&](const auto& output) {
        return output.fx != renderedFx && !output.anticipative;
    }));
    CHECK(outputs.dropped == 0);
    engine.stop();
}

TEST_CASE("main servicing publishes a new plan for parameter clear and rescan") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    auto session = edgeSession(12);
    const auto fx = session.fx.begin()->id;
    model::EditTxn edit(session, 2);
    model::Envelope envelope;
    envelope.target = {fx, "7"};
    envelope.points = {{0, 2, model::PointShape::Linear, 0}};
    const auto env = edit.create(envelope);
    session = edit.commit().value();
    REQUIRE(engine.load(session));
    backend.run(1);
    auto* instance = pool.find(fx);
    REQUIRE(instance);
    engineEdgeControl(*instance).clear(instance->raw(), 7, CLAP_PARAM_CLEAR_ALL);
    engine.service();
    backend.run(1);
    auto checkLanes = [&] {
        for (const auto& node : EngineTestAccess::latestPlan(engine).nodes)
            for (const auto& stage : node.chain)
                if (stage.fx == fx) CHECK(stage.lanes.empty());
    };
    checkLanes();
    CHECK(session.envelopes.find(env)->points.size() == 1);
    // A valid ALL rescan happens while deactivated; request_process reactivates.
    instance->stopProcessing();
    instance->setOwner(nullptr);
    instance->deactivate();
    engineEdgeControl(*instance).rescan(instance->raw(), true);
    engineEdgeControl(*instance).requestProcess(instance->raw());
    engine.service();
    backend.run(1);
    checkLanes();
    CHECK(instance->active());
    CHECK(std::any_of(engine.warnings().begin(), engine.warnings().end(), [](const auto& warning) {
        return warning.find("orphaned automation parameter 7") != std::string::npos;
    }));
    engine.stop();
}

TEST_CASE("plug-in output timeline samples subtract the latency ahead of the stage") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    auto session = synthSession(0, "com.suppleaudio.test.gain");
    model::EditTxn edit(session, 2);
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.edge";
    const auto outputFx = edit.create(fx);
    for (const auto& track : session.tracks)
        if (!track.value->chain.empty())
            edit.modify<model::Track>(track.id, [&](auto& t) { t.chain.push_back(outputFx); });
    session = edit.commit().value();
    REQUIRE(engine.load(session));
    engine.seek(.5);
    engine.play();
    backend.run(1);
    const auto outputs = engine.takePluginOutputs();
    REQUIRE(outputs.events.size() == 1);
    CHECK(outputs.events[0].fx == outputFx);
    CHECK(outputs.events[0].timelineSample == 24000 - 64);
    engine.stop();
}

TEST_CASE("a bypassed note target still receives note releases before it resumes") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 32000});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    const auto session = synthSession();
    const auto fx = session.fx.begin()->id;
    REQUIRE(engine.load(session));
    engine.play();
    backend.run(1);
    model::EditTxn bypass(session, 2);
    bypass.modify<model::FxInstance>(fx, [](auto& value) { value.bypass = true; });
    const auto bypassed = bypass.commit().value();
    REQUIRE(engine.load(bypassed));
    backend.run(250);  // the first note-off, before the next note-on
    model::EditTxn resume(bypassed, 3);
    resume.modify<model::FxInstance>(fx, [](auto& value) { value.bypass = false; });
    REQUIRE(engine.load(resume.commit().value()));
    backend.run(20);
    float peak = 0;
    for (std::size_t i = 251 * 64; i < 271 * 64; ++i)
        peak = std::max(peak, std::fabs(backend.capture().channels[0][i]));
    CHECK(peak == 0);  // the held voice was released while bypassed
    engine.stop();
}

TEST_CASE("pause resets DSP and its owned voice tracker together") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.anticipative = false});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    REQUIRE(engine.load(synthSession()));
    engine.play();
    backend.run(1);
    const auto& plan = EngineTestAccess::latestPlan(engine);
    CHECK(std::any_of(plan.nodes.begin(), plan.nodes.end(), [](const auto& node) {
        return !node.chain.empty() && !node.chain[0].voices.empty();
    }));
    engine.pause();
    backend.run(1);
    for (const auto& node : plan.nodes) {
        for (const auto& stage : node.chain) CHECK(stage.voices.empty());
        CHECK(
            std::all_of(node.held.begin(), node.held.end(), [](auto count) { return count == 0; }));
    }
    engine.stop();
}

TEST_CASE("anticipative Auto monitoring keeps callback state off renderer nodes") {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track offline;
    offline.parent = kMaster;
    offline.record.monitor = model::Monitor::Auto;
    const Id a = t.create(offline);
    model::Track live;
    live.parent = kMaster;
    live.record.armed = true;
    live.record.monitor = model::Monitor::On;
    const Id b = t.create(live);
    model::Route input;
    input.from.hwChannels = std::vector<int>{1, 2};
    input.to.track = b;
    const Id route = t.create(input);
    t.modify<model::Track>(b, [&](auto& tr) { tr.record.input = route; });
    t.modify<model::Track>(kMaster, [&](auto& tr) { tr.children = {a, b}; });
    const auto session = t.commit();
    REQUIRE(session);
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4096});
    Engine engine(pool, media, {.renderBlock = 64, .renderAheadMs = 10, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 2}));
    REQUIRE(engine.load(*session));
    REQUIRE(engine.awaitPlan());
    engine.play();
    backend.run(20000);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    for (const auto& channel : backend.capture().channels)
        CHECK(
            std::all_of(channel.begin(), channel.end(), [](float sample) { return sample == 0; }));
}

TEST_CASE("replaced media plans null against fresh offline in both executors" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-rt-media-identity-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(9600));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(.25 * std::sin(2 * 3.141592653589793 * 440 * double(i) / 48000));
    const auto file = dir / "media" / "tone.wav";
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track strip;
    strip.parent = kMaster;
    const Id track = t.create(strip);
    t.modify<model::Track>(kMaster, [&](auto& master) { master.children.push_back(track); });
    const Id source = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 9600});
    model::Take take;
    take.source = source;
    take.playrate = .5;
    const Id takeId = t.create(take);
    model::Item item;
    item.track = track;
    item.length = .8;
    item.takes = {takeId};
    item.activeTake = takeId;
    t.create(item);
    const auto session = t.commit().value();
    for (const auto& options : {kInCallback, EngineOptions{.workers = 0}}) {
        CAPTURE(options.anticipative);
        REQUIRE(audio::writeWav(file, tone));
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 19200});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(session, dir));  // leases the original stretch variant
        REQUIRE(engine.awaitPlan());
        auto replaced = tone;
        for (auto& channel : replaced.channels)
            for (float& sample : channel) sample *= 2;
        REQUIRE(audio::writeWav(file, replaced));
        REQUIRE(engine.load(session, dir));  // replacement is detected at prepare
        REQUIRE(engine.awaitPlan());
        auto offlinePool = makePool();
        auto offline = renderOffline(session, offlinePool, {.endSeconds = .4, .projectDir = dir});
        REQUIRE(offline);
        engine.seek(0);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 150, 128);
        else
            backend.run(150);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(maxDiff(backend.capture().channels[0], 0, offline->audio.channels[0], 19200) == 0);
        CHECK(maxDiff(backend.capture().channels[1], 0, offline->audio.channels[1], 19200) == 0);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("parallel, oversampled and pdc-ignored FX: real-time output equals the offline render" *
          doctest::description("rt")) {
    model::Session base = synthSession(-3.0);
    model::EditTxn t(base, 2);
    Id track;
    for (const auto& e : base.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    auto gain = [&](const char* latency, auto&& edit) {
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.gain";
        fx.patch = json::parse(std::string(R"({"schema": "supple.test-gain-patch/1.0", "params": )"
                                           R"({"gain": -6, "latency": ")") +
                               latency + "\"}}")
                       .value();
        edit(fx);
        const Id f = t.create(fx);
        t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    };
    gain("short", [](model::FxInstance& f) { f.oversampling = 4; });
    gain("none", [](model::FxInstance& f) { f.parallel = true; });
    gain("short", [](model::FxInstance& f) { f.pdc = model::PdcMode::Ignore; });
    const model::Session s = t.commit().value();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
        CHECK(offline.warnings.empty());
        CHECK(offline.outputLatency == 80 + 16);  // 4x round trip, 64 at the high rate
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
        else
            backend.run((48000 * 2 + 4096) / 128);
        engine.stop();
        const std::uint32_t L = engine.outputLatency();
        CHECK(L == 96u);
        CHECK(engine.processErrors() == 0);
        for (std::size_t c = 0; c < 2; ++c)
            CHECK(maxDiff(backend.capture().channels[c], L, offline.audio.channels[c], 96000) ==
                  0.0);
    }
}

TEST_CASE("input FX process the armed input: monitored and recorded, aligned with the plain input" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        INFO("anticipative " << options.anticipative);
        // Two channels armed on the same inputs; the second has a -6 dB input
        // FX with 64 samples of latency, and is not monitored.
        model::Session base = monitoring(synthSession(0.0), model::Monitor::Off);
        model::EditTxn t(base, 3);
        Id plain;
        for (const auto& e : base.tracks)
            if (e.value->type == model::StripType::Channel) plain = e.id;
        model::Track second;
        second.name = "Processed";
        second.parent = kMaster;
        const Id processed = t.create(second);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(processed); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.gain";
        fx.patch =
            json::parse(
                R"({"schema": "supple.test-gain-patch/1.0", "params": {"gain": -6, "latency": "short"}})")
                .value();
        const Id f = t.create(fx);
        model::Route input;
        input.from.hwChannels = std::vector<int>{1, 2};
        input.to.track = processed;
        const Id route = t.create(input);
        t.modify<model::Track>(processed, [&](model::Track& x) {
            x.inputChain = {f};
            x.record.input = route;
            x.record.armed = true;
            x.record.monitor = model::Monitor::Off;
        });
        auto committed = t.commit();
        REQUIRE_MESSAGE(committed, (committed ? std::string() : committed.error().toString()));
        const model::Session s = std::move(*committed);

        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                          std::int64_t frame) {
                                  for (std::uint32_t c = 0; c < channels; ++c)
                                      for (std::uint32_t i = 0; i < n; ++i)
                                          in[c][i] = static_cast<float>((frame + i) % 1000) /
                                                     1000.0f * (c ? -1.0f : 1.0f);
                              }});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        REQUIRE(engine.load(s));
        CHECK(engine.processErrors() == 0);
        const auto dir = std::filesystem::temp_directory_path() /
                         ("supple-inputfx-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        REQUIRE(engine
                    .startRecording({{plain, {0, 1}, dir / "plain.wav", {}},
                                     {processed, {0, 1}, dir / "processed.wav", {}}})
                    .ok());
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 100, 256);
        else
            backend.run(100);
        auto takes = engine.stopRecording();
        engine.stop();
        REQUIRE(takes);
        REQUIRE(takes->size() == 2);
        auto a = audio::readAudio((*takes)[0].track == plain ? (*takes)[0].file : (*takes)[1].file);
        auto b = audio::readAudio((*takes)[0].track == plain ? (*takes)[1].file : (*takes)[0].file);
        REQUIRE(a);
        REQUIRE(b);
        const double g = std::pow(10.0, -6.0 / 20.0);
        const std::size_t frames = 100 * 256 - 64;  // the processed file's last 64 are padding
        double worst = 0;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                worst = std::max(
                    worst, std::fabs(double(b->channels[c][i]) - g * double(a->channels[c][i])));
        CHECK(worst < 1e-6);
        CHECK(engine.processErrors() == 0);
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("a monitored input is heard through its input FX" * doctest::description("rt")) {
    model::Session base = monitoring(synthSession(-6.0206));
    model::EditTxn t(base, 3);
    Id track;
    for (const auto& e : base.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    fx.patch =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"gain": -6}})").value();
    const Id f = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.inputChain = {f}; });
    const model::Session s = t.commit().value();
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(constantInput(48000));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        REQUIRE(engine.load(s));
        REQUIRE(engine.awaitPlan());
        backend.run(20);  // stopped: the input, the input FX, the fader and centre pan
        const auto& cap = backend.capture();
        const double g = 0.5 * std::pow(10.0, -6.0 / 20.0);
        CHECK(double(cap.channels[0][4000]) == doctest::Approx(0.5 * g).epsilon(1e-4));
        CHECK(double(cap.channels[1][4000]) == doctest::Approx(-0.25 * g).epsilon(1e-4));
        CHECK(engine.processErrors() == 0);
        engine.stop();
    }
}

TEST_CASE("a recording split by a seek, not a loop, has a sidecar its reader accepts") {
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-seek-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    detail::Recorder recorder({{Id{model::Kind::Track, 1}, {0}, dir / "take.wav", {}}}, 48000, 0);
    REQUIRE(recorder.open());
    const std::array<float, 64> data{};
    const float* in[] = {data.data()};
    recorder.capture(in, 1, 64, 0);
    recorder.capture(in, 1, 64, 48000);  // a seek during capture: a second run
    // A loop range that is set but switched off does not make the runs passes.
    const auto takes = recorder.finish({.start = 24000, .end = 96000, .on = false});
    REQUIRE(takes.size() == 1);
    REQUIRE(takes[0].segments.size() == 2);
    CHECK(takes[0].loop.start == 0);
    CHECK(takes[0].loop.end == 0);
    const auto side = detail::readSidecar(dir / "take.wav");
    REQUIRE_MESSAGE(side, (side ? std::string() : side.error().toString()));
    CHECK(side->segments.size() == 2);
    CHECK(side->segments[1].timeline == 48000);
    CHECK(side->loopEnd == 0);
    std::filesystem::remove_all(dir);
}

namespace {
model::Session parallelLive(unsigned liveTracks) {
    auto base = synthSession(-18, "com.suppleaudio.test.gain", 8);
    model::EditTxn edit(base, 2);
    auto project = edit.project();
    project.console9 = true;
    project.seed = 82735;
    edit.setProject(project);
    unsigned count = 0;
    for (const auto& track : base.tracks) {
        if (track.id == kMaster || count++ >= liveTracks) continue;
        model::Route route;
        route.from.hwChannels = std::vector<int>{1, 2};
        route.to.track = track.id;
        const Id input = edit.create(route);
        edit.modify<model::Track>(track.id, [&](model::Track& tr) {
            tr.record.input = input;
            tr.record.armed = true;
            tr.record.monitor = model::Monitor::On;
        });
    }
    return edit.commit().value();
}
}  // namespace

TEST_CASE("live multiprocessing: exact offline output with frontier and all-live graphs" *
          doctest::description("rt")) {
    for (unsigned live : {4u, 8u}) {
        const auto session = parallelLive(live);
        auto offlinePool = makePool();
        const auto offline =
            renderOffline(session, offlinePool, {.startSeconds = 0, .endSeconds = 0.25}).value();
        for (bool ahead : {false, true})
            for (int helpers : {0, 1, 2, 7}) {
                INFO("live " << live << " ahead " << ahead << " helpers " << helpers);
                auto pool = makePool();
                MediaCache media;
                DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 12288});
                Engine engine(pool, media, {.anticipative = ahead, .workers = helpers});
                REQUIRE(engine.start(backend, {.blockSize = 128, .inputs = 2}));
                CHECK(engine.liveHelpers() ==
                      std::min(static_cast<unsigned>(helpers), detail::LiveWorkers::maxHelpers()));
                REQUIRE(engine.load(session));
                engine.play();
                if (ahead)
                    runAhead(engine, backend, 96, 128);
                else
                    backend.run(96);
                engine.stop();
                CHECK(engine.processErrors() == 0);
                CHECK(engine.anticipationMisses() == 0);
                CHECK(engine.lastStopOnAudioThread());
                for (unsigned c = 0; c < 2; ++c)
                    CHECK(maxDiff(backend.capture().channels[c], engine.outputLatency(),
                                  offline.audio.channels[c], 12000) == 0.0);
            }
    }
}

TEST_CASE("live multiprocessing: delayed departure protects plans during transport edits" *
          doctest::description("rt")) {
    // Same sequence with and without preemption must have exactly the same state
    // and audio. The gate is released by the test, never by an elapsed deadline.
    for (bool ahead : {false, true}) {
        audio::AudioData reference;
        for (bool delayed : {false, true}) {
            struct Gate {
                std::atomic<bool> enabled{false}, claimed{false}, joining{false};
                std::binary_semaphore entered{0}, resume{0};
            } gate;
            auto pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 128 * 12});
            Engine engine(pool, media, {.anticipative = ahead, .workers = 3});
            REQUIRE(engine.start(backend, {.blockSize = 128, .inputs = 2}));
            EngineTestAccess::liveHooks(
                engine, {[](void* p, detail::LiveWorkers::Event event) {
                             auto& g = *static_cast<Gate*>(p);
                             // Ensure this tiny job has a helper
                             // inside it before the caller can
                             // close it. No scheduling deadline.
                             if (event == detail::LiveWorkers::Event::Published && g.enabled.load())
                                 while (!g.claimed.load()) rt::relax();
                             if (event == detail::LiveWorkers::Event::Leave && g.enabled.load() &&
                                 !g.claimed.exchange(true)) {
                                 g.entered.release();
                                 g.resume.acquire();
                             }
                             if (event == detail::LiveWorkers::Event::JoinWait) g.joining = true;
                         },
                         &gate});
            auto session = parallelLive(8);
            REQUIRE(engine.load(session));
            engine.play();
            if (ahead) REQUIRE(engine.awaitRendered(4096));
            backend.run(1);
            // Stall a helper with completed DSP but retained job/plan ownership.
            gate.enabled = delayed;
            gate.joining = false;
            std::atomic<bool> returned{false};
            std::thread cycle([&] {
                backend.run(1);
                returned = true;
            });
            if (delayed) {
                gate.entered.acquire();
                while (!gate.joining.load()) std::this_thread::yield();
                CHECK_FALSE(returned.load());
            } else
                cycle.join();
            engine.pause();
            engine.seek(0.125);
            engine.setLoop(0.125, 0.129, true);  // non-block-aligned wrap
            model::EditTxn edit(session, 3);
            // A rename makes the level edit compile (ADR-0060): a plan reload.
            edit.modify<model::Track>(Id{model::Kind::Track, 1}, [](model::Track& t) {
                t.volumeDb = -21;
                t.name = "reloaded";
            });
            REQUIRE(engine.load(edit.commit().value()));
            engine.service();
            if (delayed) {
                CHECK(EngineTestAccess::plans(engine) >= 2);
                CHECK_FALSE(returned.load());
                gate.resume.release();
                cycle.join();
            }
            if (ahead) REQUIRE(engine.awaitPlan());
            backend.run(1);  // acknowledge pause/seek/plan
            if (ahead) REQUIRE(engine.awaitRendered(0));
            engine.play();
            if (ahead) REQUIRE(engine.awaitRendered(4096));
            backend.run(9);
            engine.stop();
            CHECK(engine.lastStopOnAudioThread());
            CHECK(engine.processErrors() == 0);
            if (!delayed)
                reference = backend.capture();
            else
                CHECK(backend.capture().channels == reference.channels);
        }
    }
}

TEST_CASE("live multiprocessing: scheduling failure is serial and survives plan reloads") {
    struct RefusingBackend : DummyBackend {
        RefusingBackend() : DummyBackend({.mode = Mode::Manual, .captureFrames = 256}) {}
        Status prepareLiveThread(std::thread::native_handle_type) override {
            return Error("engine.live_threads", "test denied scheduling", {},
                         "test corrective hint");
        }
    } backend;
    auto pool = makePool();
    MediaCache media;
    Engine engine(pool, media, {.anticipative = false, .workers = 1});
    REQUIRE(engine.start(backend, {.blockSize = 128}));
    CHECK(engine.liveHelpers() == 0);
    REQUIRE(engine.load(parallelLive(8)));
    REQUIRE(engine.warnings().size() == 1);
    CHECK(engine.warnings()[0].find("test corrective hint") != std::string::npos);
    engine.play();
    backend.run(1);
    REQUIRE(engine.load(parallelLive(4)));
    CHECK(engine.warnings().size() == 1);
    backend.run(1);
    engine.stop();
    CHECK(engine.processErrors() == 0);
}

// --- floating-point environment (ADR-0025) ----------------------------------------------

namespace {

// One track's fixture: S1 a sine; S2 tiny normal values on both sides of the
// probe's 2^-26 threshold; S3 values near FLT_MIN, which the strip's gains
// take below it; S4 subnormal samples; S5 a decay through every range.
constexpr std::size_t kTinySegment = 4800, kTinyDecay = 8000;
constexpr std::size_t kTinyFixture = 4 * kTinySegment + kTinyDecay;
constexpr std::size_t kTinySpacing = 36000;  // items at 0, 0.75 and 1.5 s
constexpr double kTinyBeat = 24000.0;        // frames per beat at 120 BPM
constexpr std::size_t kTinyFrames = 2 * kTinySpacing + 28800;
constexpr double kTinySeconds = 2.25;

std::vector<float> tinySignal() {
    std::vector<float> x(kTinyFixture);
    const float s2[] = {1e-9f, -1e-9f, 1e-7f, -1e-7f};
    const float s3[] = {1.5e-38f, -1.5e-38f, 4e-38f, -4e-38f};
    const float s4[] = {std::numeric_limits<float>::denorm_min(), -1e-40f, 1e-39f, -1e-39f};
    for (std::size_t i = 0; i < kTinySegment; ++i) {
        x[i] = static_cast<float>(0.25 *
                                  std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0));
        x[kTinySegment + i] = s2[i % 4];
        x[2 * kTinySegment + i] = s3[i % 4];
        x[3 * kTinySegment + i] = s4[i % 4];
    }
    for (std::size_t n = 0; n < kTinyDecay; ++n)
        x[4 * kTinySegment + n] =
            static_cast<float>((n % 2 ? -0.5 : 0.5) * std::pow(0.98, double(n)));
    return x;
}

std::filesystem::path tinyMedia(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-fp-" + std::string(name) + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData data;
    data.sampleRate = 48000;
    data.channels.assign(2, tinySignal());
    REQUIRE(audio::writeWav(dir / "media" / "tiny.wav", data));
    return dir;
}

// Linear strips (Console 9 off) keep tiny values visible. Plain plays the
// fixture with no FX, Probe through the edge fixture's underflow probe, and
// Bridged through a dedicated-isolation probe; each at its own time, so each
// part of the master output is one track's path. `live`: Plain and Probe are
// armed and monitored (silent inputs), so the callback runs them.
model::Session tinySignalSession(bool bridged, bool live) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const Id media = t.create(model::MediaSource{"tiny.wav", "", "wav", 48000, 2,
                                                 static_cast<std::int64_t>(kTinyFixture)});
    const char* names[] = {"Plain", "Probe", "Bridged"};
    for (std::size_t k = 0; k < (bridged ? 3u : 2u); ++k) {
        model::Track tr;
        tr.name = names[k];
        tr.parent = kMaster;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
        if (k > 0) {
            model::FxInstance fx;
            fx.plugin.id = "com.suppleaudio.test.edge";
            fx.state = "sha256:underflow-probe";
            if (k == 2) fx.isolation = model::Isolation::Dedicated;
            const Id f = t.create(fx);
            t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
        }
        model::Take take;
        take.source = media;
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.position = static_cast<double>(k * kTinySpacing) / kTinyBeat;  // in beats
        item.length = 28800.0 / kTinyBeat;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        if (live && k < 2) {
            model::Route route;
            route.from.hwChannels = std::vector<int>{1, 2};
            route.to.track = track;
            const Id input = t.create(route);
            t.modify<model::Track>(track, [&](model::Track& x) {
                x.record.input = input;
                x.record.armed = true;
                x.record.monitor = model::Monitor::On;
            });
        }
    }
    t.putBlob("sha256:underflow-probe",
              std::make_shared<const model::Blob>(model::Blob{{50, 0, 0, 0}}));
    return t.commit().value();
}

OfflineResult renderTiny(const model::Session& s, const std::filesystem::path& dir,
                         unsigned threads, std::optional<Id> track = {}) {
    auto pool = makePool();
    auto r = renderOffline(
        s, pool,
        {.endSeconds = kTinySeconds, .projectDir = dir, .threads = threads, .track = track});
    REQUIRE(r);
    CHECK(r->outputLatency == 0);
    REQUIRE(r->audio.frames() >= kTinyFrames);
    return std::move(r).value();
}

// Plays the session from 0 through a manual device whose callbacks run on
// this thread; `beforeStop` runs on it before the engine stops.
std::vector<std::vector<float>> playTiny(const model::Session& s, const std::filesystem::path& dir,
                                         const EngineOptions& options, std::uint32_t block,
                                         const std::function<void()>& beforeStop = {}) {
    auto pool = makePool();
    MediaCache media;
    const auto frames = static_cast<std::int64_t>(kTinySeconds * 48000);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block, .inputs = 2}));
    REQUIRE(engine.load(s, dir));
    engine.play();
    if (options.anticipative)
        runAhead(engine, backend, frames / block, block);
    else
        backend.run(frames / block);
    if (beforeStop) beforeStop();
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.outputLatency() == 0);
    return backend.capture().channels;
}

#if defined(__SSE2__)
std::size_t subnormals(const std::vector<float>& v) {
    return static_cast<std::size_t>(std::count_if(
        v.begin(), v.end(), [](float x) { return std::fpclassify(x) == FP_SUBNORMAL; }));
}
#endif

}  // namespace

TEST_CASE("tiny signals and long decays render identically in every executor and the bridge" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    const auto dir = tinyMedia("executors");
    const auto full = tinySignalSession(true, false);
    const auto reference = renderTiny(full, dir, 1);

#if defined(__SSE2__)
    // The policy, in the reference: nothing subnormal leaves a node, and the
    // probe (in process and bridged) ran with FTZ/DAZ. It equals the plain
    // path where its intermediate stays normal and is zero elsewhere, while
    // the plain path keeps tiny normal values.
    const auto x = tinySignal();
    for (const auto& y : reference.audio.channels) {
        CHECK(subnormals(y) == 0);
        CHECK(y[kTinySegment] != 0.0f);  // 1e-9 through linear strips alone
        std::size_t sourceSubnormalsHeard = 0;
        for (std::size_t i = 3 * kTinySegment; i < 4 * kTinySegment; ++i)
            sourceSubnormalsHeard += y[i] != 0.0f;
        CHECK(sourceSubnormalsHeard == 0);
        for (std::size_t probe : {kTinySpacing, 2 * kTinySpacing}) {
            INFO("probe at " << probe);
            std::size_t wrong = 0, flushed = 0;
            for (std::size_t i = 0; i < kTinyFixture; ++i) {
                const bool normal = std::fabs(x[i]) >= 0x1p-26f;
                const float expected = normal ? y[i] : 0.0f;
                wrong += y[probe + i] != expected;
                flushed += !normal && y[i] != 0.0f;
            }
            CHECK(wrong == 0);
            CHECK(flushed > 2000);  // the probe flushed what the plain path kept
        }
    }
    // The offline stem decoder runs under the same policy as the graph.
    std::optional<Id> plain;
    for (const auto& entry : full.tracks)
        if (entry.value->name == "Plain") plain = entry.id;
    REQUIRE(plain);
    const auto stem = renderTiny(full, dir, 1, plain);
    for (const auto& y : stem.audio.channels) CHECK(subnormals(y) == 0);
#endif

    const auto threaded = renderTiny(full, dir, 4);
    for (unsigned c = 0; c < 2; ++c)
        CHECK(maxDiff(threaded.audio.channels[c], 0, reference.audio.channels[c], kTinyFrames) ==
              0.0);

    // Rendered ahead, serially and with background workers (the bridge runs there).
    for (int workers : {0, 3}) {
        INFO("anticipative, workers " << workers);
        const auto heard = playTiny(full, dir, {.workers = workers}, 128);
        for (unsigned c = 0; c < 2; ++c)
            CHECK(maxDiff(heard[c], 0, reference.audio.channels[c], kTinyFrames) == 0.0);
    }
    // In the callback, serially and with live helpers. A bridged stage passes
    // through on a callback node by contract, so these sessions have none.
    const auto callbackSession = tinySignalSession(false, false);
    const auto callbackReference = renderTiny(callbackSession, dir, 1);
    for (unsigned c = 0; c < 2; ++c)
        CHECK(maxDiff(callbackReference.audio.channels[c], 0, reference.audio.channels[c],
                      2 * kTinySpacing) == 0.0);
    for (int workers : {0, 3})
        for (std::uint32_t block : {128u, 64u}) {
            INFO("in the callback, workers " << workers << ", block " << block);
            const auto heard =
                playTiny(callbackSession, dir, {.anticipative = false, .workers = workers}, block);
            for (unsigned c = 0; c < 2; ++c)
                CHECK(maxDiff(heard[c], 0, callbackReference.audio.channels[c], kTinyFrames) ==
                      0.0);
        }
    // Live nodes on the callback and its helpers, the rest (and the bridge)
    // rendered ahead.
    const auto live = tinySignalSession(true, true);
    const auto liveReference = renderTiny(live, dir, 1);
    for (unsigned c = 0; c < 2; ++c)
        CHECK(maxDiff(liveReference.audio.channels[c], 0, reference.audio.channels[c],
                      kTinyFrames) == 0.0);
    const auto heard = playTiny(live, dir, {.workers = 3}, 128);
    for (unsigned c = 0; c < 2; ++c)
        CHECK(maxDiff(heard[c], 0, liveReference.audio.channels[c], kTinyFrames) == 0.0);
    std::filesystem::remove_all(dir);
}

#if defined(__SSE2__)
TEST_CASE("the device thread's MXCSR is restored after every node" * doctest::description("rt")) {
    struct ScopedMxcsr {
        unsigned saved = _mm_getcsr();
        explicit ScopedMxcsr(unsigned value) { _mm_setcsr(value); }
        ~ScopedMxcsr() { _mm_setcsr(saved); }
        ScopedMxcsr(const ScopedMxcsr&) = delete;
        ScopedMxcsr& operator=(const ScopedMxcsr&) = delete;
    };
    constexpr unsigned kControl = 0xffc0;  // without the sticky exception flags
    const auto dir = tinyMedia("mxcsr");
    const auto callbackSession = tinySignalSession(false, false);
    const auto liveSession = tinySignalSession(false, true);
    const auto callbackReference = renderTiny(callbackSession, dir, 1);
    const auto liveReference = renderTiny(liveSession, dir, 1);
    // The measured callback (FTZ and DAZ clear), then FTZ alone: whatever the
    // device thread brings, it gets back, and the output does not depend on it.
    for (unsigned before : {0x1f80u, 0x9f80u})
        for (int variant = 0; variant < 3; ++variant) {
            INFO("MXCSR " << before << ", variant " << variant);
            const bool live = variant == 2;
            const EngineOptions options =
                live ? EngineOptions{.workers = 3}
                     : EngineOptions{.anticipative = false, .workers = variant == 0 ? 0 : 3};
            unsigned during = 0, after = 0;
            std::vector<std::vector<float>> heard;
            {
                ScopedMxcsr device(before);
                heard = playTiny(live ? liveSession : callbackSession, dir, options, 128,
                                 [&] { during = _mm_getcsr(); });
                after = _mm_getcsr();  // after the backend's final, zero-frame callback
            }
            CHECK((during & kControl) == before);
            CHECK((after & kControl) == before);
            const auto& reference = live ? liveReference : callbackReference;
            for (unsigned c = 0; c < 2; ++c)
                CHECK(maxDiff(heard[c], 0, reference.audio.channels[c], kTinyFrames) == 0.0);
        }
    std::filesystem::remove_all(dir);
}
#endif
namespace {
std::set<std::string> threadNames() {
    std::set<std::string> names;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream comm(task.path() / "comm");
        std::string name;
        if (std::getline(comm, name)) names.insert(name);
    }
    return names;
}
}  // namespace

TEST_CASE("the engine's threads carry their role names") {
    DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
    auto pool = makePool();
    MediaCache media;
    Engine engine(pool, media, {.workers = 2});
    REQUIRE(engine.start(backend, {.blockSize = 128}));
    REQUIRE(engine.liveHelpers() == 2);
    const std::set<std::string> want = {"audio-dummy", "audio-render", "audio-bg-1",
                                        "audio-bg-2",  "audio-live-1", "audio-live-2"};
    auto named = [&] {
        const auto names = threadNames();
        return std::includes(names.begin(), names.end(), want.begin(), want.end());
    };
    // Each thread names itself as it starts; the deadline only bounds a failure.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!named() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(named());
    engine.stop();
}

// --- background scheduling (ADR-0026) -----------------------------------------------------

namespace {
using Kind = ThreadPolicy::Kind;
constexpr ThreadPolicy kNormal{Kind::Normal, 0};

// A paced dummy device whose callback reports `reported`; its own thread stays
// normal (the engine only reads the callback's policy).
struct ReportingBackend : DummyBackend {
    explicit ReportingBackend(ThreadPolicy policy)
        : DummyBackend({.mode = Mode::Thread, .paced = true}), reported(policy) {}
    ThreadPolicy callbackPolicy() override { return reported; }
    ThreadPolicy reported;
};

rlim_t softLimit(int resource) {
    rlimit limit{};
    REQUIRE(::getrlimit(resource, &limit) == 0);
    return limit.rlim_cur;
}
// Lowers a soft limit for the scope (process-local; restorable up to the hard limit).
struct LimitScope {
    int resource;
    rlimit saved{};
    LimitScope(int r, rlim_t value) : resource(r) {
        REQUIRE(::getrlimit(resource, &saved) == 0);
        rlimit lowered = saved;
        lowered.rlim_cur = std::min(value, saved.rlim_max);
        REQUIRE(::setrlimit(resource, &lowered) == 0);
    }
    ~LimitScope() { ::setrlimit(resource, &saved); }
};
int allowedCpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    REQUIRE(::sched_getaffinity(0, sizeof(set), &set) == 0);
    return CPU_COUNT(&set);
}
// Whether this process may run `threads` background threads real-time at `priority`.
bool mayElevate(int threads, int priority) {
    return softLimit(RLIMIT_RTTIME) == RLIM_INFINITY &&
           softLimit(RLIMIT_RTPRIO) >= static_cast<rlim_t>(priority) &&
           threads + 2 <= allowedCpus();
}
bool warnsNormal(const Engine& engine, std::string_view because) {
    for (const auto& w : engine.warnings())
        if (w.find("background audio threads run at normal priority") != std::string::npos &&
            w.find(because) != std::string::npos)
            return true;
    return false;
}
}  // namespace

TEST_CASE("background threads run real-time below a real-time callback (ADR-0026)") {
    auto pool = makePool();
    MediaCache media;
    struct Case {
        ThreadPolicy callback;
        int expected;  // the background priority
    };
    const std::array<Case, 3> cases = {Case{{Kind::Fifo, 83}, Engine::kBackgroundPriority},
                                       Case{{Kind::RoundRobin, 40}, Engine::kBackgroundPriority},
                                       Case{{Kind::Fifo, 5}, 4}};
    for (const Case& c : cases) {
        CAPTURE(c.callback.priority);
        ReportingBackend backend(c.callback);
        Engine engine(pool, media, {.workers = 2});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        const Engine::Scheduling& s = engine.scheduling();
        CHECK(s.callback == c.callback);
        if (engine.liveHelpers() > 0) CHECK(s.live == kNormal);  // a dummy's stay normal
        if (mayElevate(3, c.expected)) {
            const ThreadPolicy fifo{Kind::Fifo, c.expected};
            CHECK(s.renderer == fifo);
            CHECK(s.background == fifo);
            CHECK(host::bridgeAudioPolicy() == fifo);
            CHECK(engine.warnings().empty());
        } else {
            MESSAGE("no real-time scheduling permission: the fallback is checked instead");
            CHECK(s.renderer == kNormal);
            CHECK(s.background == kNormal);
            CHECK(engine.warnings().size() == 1);
        }
        REQUIRE(engine.load(parallelLive(2)));  // start warnings stay with plan warnings
        CHECK(engine.warnings().size() == (mayElevate(3, c.expected) ? 0u : 1u));
        engine.stop();
        CHECK(engine.scheduling().renderer == ThreadPolicy{});
        CHECK(engine.scheduling().callback == ThreadPolicy{});
        CHECK(host::bridgeAudioPolicy() == kNormal);
    }
}

TEST_CASE("background threads stay normal under a normal callback or when forced normal") {
    auto pool = makePool();
    MediaCache media;
    {
        DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
        Engine engine(pool, media, {.workers = 2});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        CHECK(engine.scheduling().callback == kNormal);
        CHECK(engine.scheduling().renderer == kNormal);
        CHECK(engine.scheduling().background == kNormal);
        CHECK(engine.warnings().empty());
        engine.stop();
    }
    {
        ReportingBackend backend({Kind::Fifo, 83});
        Engine engine(pool, media, {.workers = 2, .background = EngineOptions::Background::Normal});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        CHECK(engine.scheduling().renderer == kNormal);
        CHECK(engine.scheduling().background == kNormal);
        CHECK(engine.warnings().empty());
        engine.stop();
    }
    {
        // Not anticipative: no renderer or background pool to schedule.
        ReportingBackend backend({Kind::Fifo, 83});
        Engine engine(pool, media, {.anticipative = false, .workers = 2});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        CHECK(engine.scheduling().renderer == ThreadPolicy{});
        CHECK(engine.scheduling().background == ThreadPolicy{});
        CHECK(host::bridgeAudioPolicy() == kNormal);
        engine.stop();
    }
}

TEST_CASE("forced background policies apply for benchmarks, within the same checks") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
    Engine engine(
        pool, media,
        {.workers = 2, .background = EngineOptions::Background::Fifo, .backgroundPriority = 3});
    REQUIRE(engine.start(backend, {.blockSize = 128}));
    if (mayElevate(3, 3)) {  // forced even under a normal callback
        CHECK(engine.scheduling().renderer == ThreadPolicy{Kind::Fifo, 3});
        CHECK(engine.scheduling().background == ThreadPolicy{Kind::Fifo, 3});
    } else {
        CHECK(engine.scheduling().renderer == kNormal);
        CHECK(engine.warnings().size() == 1);
    }
    engine.stop();
}

TEST_CASE("under a restricted CPU mask the default pools fit it and stay real-time") {
    cpu_set_t all;
    CPU_ZERO(&all);
    REQUIRE(::sched_getaffinity(0, sizeof(all), &all) == 0);
    std::vector<int> cpus;
    for (int c = 0; c < CPU_SETSIZE; ++c)
        if (CPU_ISSET(c, &all)) cpus.push_back(c);
    if (cpus.size() < 4) {
        MESSAGE("fewer than four allowed CPUs: no smaller mask to try");
        return;
    }
    // One CPU fewer than allowed, as taskset or a cpuset would give. Sized
    // from every online core, the default pool would exceed the cap.
    const std::size_t n = cpus.size() - 1;
    auto pool = makePool();
    MediaCache media;
    bool restricted = false, started = false, elevated = false, capWarning = true;
    unsigned live = 0;
    ThreadPolicy renderer, background;
    std::thread([&] {
        cpu_set_t some;
        CPU_ZERO(&some);
        for (std::size_t i = 0; i < n; ++i) CPU_SET(cpus[i], &some);
        restricted = ::sched_setaffinity(0, sizeof(some), &some) == 0;
        ReportingBackend backend({Kind::Fifo, 83});
        Engine engine(pool, media);  // default (Automatic) pools
        started = engine.start(backend, {.blockSize = 128}).ok();
        if (!started) return;
        live = engine.liveHelpers();
        renderer = engine.scheduling().renderer;
        background = engine.scheduling().background;
        capWarning = warnsNormal(engine, "allowed CPUs");
        elevated = mayElevate(static_cast<int>(n) - 2, Engine::kBackgroundPriority);
        engine.stop();
    }).join();
    REQUIRE(restricted);
    REQUIRE(started);
    CHECK(live <= n - 1);
    CHECK_FALSE(capWarning);
    if (elevated) {
        CHECK(renderer == ThreadPolicy{Kind::Fifo, Engine::kBackgroundPriority});
        CHECK(background == renderer);
    }
}

TEST_CASE("background elevation refusals keep every background thread normal and warn") {
    auto pool = makePool();
    MediaCache media;
    const ThreadPolicy callback{Kind::Fifo, 83};
    SUBCASE("the pool would leave fewer than two CPUs to the system") {
        ReportingBackend backend(callback);
        Engine engine(pool, media, {.workers = allowedCpus()});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        CHECK(engine.scheduling().renderer == kNormal);
        CHECK(engine.scheduling().background == kNormal);
        // A zero RT priority limit is refused before the CPU cap (ADR-0026).
        CHECK(warnsNormal(engine, softLimit(RLIMIT_RTPRIO) < 1 ? "RLIMIT_RTPRIO" : "allowed CPUs"));
        engine.stop();
    }
    SUBCASE("no real-time priority within RLIMIT_RTPRIO") {
        LimitScope rtprio(RLIMIT_RTPRIO, 0);
        ReportingBackend backend(callback);
        Engine engine(pool, media, {.workers = 2});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        CHECK(engine.scheduling().renderer == kNormal);
        CHECK(engine.scheduling().background == kNormal);
        CHECK(warnsNormal(engine, "RLIMIT_RTPRIO"));
        CHECK(host::bridgeAudioPolicy() == kNormal);
        engine.stop();
    }
    SUBCASE("a finite real-time CPU time limit") {
        LimitScope rttime(RLIMIT_RTTIME, 200000);
        ReportingBackend backend(callback);
        Engine engine(pool, media, {.workers = 2});
        REQUIRE(engine.start(backend, {.blockSize = 128}));
        CHECK(engine.scheduling().renderer == kNormal);
        CHECK(engine.scheduling().background == kNormal);
        if (softLimit(RLIMIT_RTPRIO) >= 1 && allowedCpus() >= 5)
            CHECK(warnsNormal(engine, "RLIMIT_RTTIME"));
        engine.stop();
    }
}

TEST_CASE("output consumption targets are one-shot and tagged with the playback epoch") {
    InstancePool pool([](const model::PluginRef&) -> Result<host::PluginInfo> {
        return Error("test.unused", "unused resolver");
    });
    MediaCache media;
    MainThreadWake wake;
    Engine engine(pool, media);
    engine.setMainThreadWake(wake);
    engine.armOutputWake(3, 128);
    const auto serial = EngineTestAccess::outputSerial(engine);
    EngineTestAccess::outputConsume(engine, 2, 1000);  // wrong epoch cannot satisfy the target
    CHECK(EngineTestAccess::outputSeen(engine) != serial);
    EngineTestAccess::outputConsume(engine, 3, 127);
    CHECK(EngineTestAccess::outputSeen(engine) != serial);
    EngineTestAccess::outputConsume(engine, 3, 128);
    CHECK(EngineTestAccess::outputSeen(engine) == serial);
    EngineTestAccess::outputConsume(engine, 3, 256);
    CHECK(EngineTestAccess::outputSeen(engine) == serial);
    engine.armOutputWake(3, 512);
    CHECK(EngineTestAccess::outputSerial(engine) != serial);
    EngineTestAccess::outputConsume(engine, 3, 511);
    CHECK(EngineTestAccess::outputSeen(engine) == serial);
    EngineTestAccess::outputConsume(engine, 3, 512);
    CHECK(EngineTestAccess::outputSeen(engine) == EngineTestAccess::outputSerial(engine));
    engine.armOutputWake(3, INT64_MAX);
    EngineTestAccess::outputConsume(engine, 3, 1024);
    CHECK(EngineTestAccess::outputSeen(engine) == EngineTestAccess::outputSerial(engine));
    auto drain = [&] {
        pollfd fd{wake.fd(), POLLIN, 0};
        int ready;
        do {
            ready = ::poll(&fd, 1, -1);
        } while (ready < 0 && errno == EINTR);
        REQUIRE(ready == 1);
        REQUIRE(wake.drain());
    };
    drain();                       // earlier callback notifications coalesced
    engine.armOutputWake(3, 128);  // consumption crossed before the arm
    drain();                       // main's arm/recheck signals without another callback
    engine.armOutputWake(3, INT64_MAX);
    EngineTestAccess::outputConsume(engine, 3, 2048);
    pollfd fd{wake.fd(), POLLIN, 0};
    CHECK(::poll(&fd, 1, 0) == 0);  // disarmed/unchanged work does not notify
}

TEST_CASE("MIDI sends: timeline chains null offline in both executors and all isolations" *
          doctest::description("rt")) {
    const auto reference = synthSession(-18.0, "com.suppleaudio.test.gain", 1);
    const auto source = reference.tracks.find(kMaster)->children.front();
    model::EditTxn t(reference, 2);
    model::Track relay;
    relay.parent = kMaster;
    relay.parentSend.enabled = false;
    const auto middle = t.create(relay);
    model::Track destination = *reference.tracks.find(source);
    destination.name = "Destination";
    destination.children.clear();
    const auto target = t.create(destination);
    t.modify<model::Track>(kMaster, [&](auto& master) {
        master.children.insert(master.children.end(), {middle, target});
    });
    t.modify<model::Track>(source, [](auto& track) {
        track.chain.clear();
        track.parentSend.enabled = false;
    });
    model::Route send;
    send.from.track = source;
    send.to.track = middle;
    send.midi = model::MidiRouting{};
    t.create(send);
    send.from.track = middle;
    send.to.track = target;
    t.create(send);
    const auto session = t.commit();
    REQUIRE(session);
    host::setBridgeExecutable(BRIDGE_PATH);
    auto referencePool = makePool();
    const auto offlineReference =
        renderOffline(reference, referencePool, {.startSeconds = 0, .endSeconds = 2.0});
    REQUIRE(offlineReference);
    for (auto isolation : {model::Isolation::InProcess, model::Isolation::SharedBridge,
                           model::Isolation::Dedicated}) {
        model::EditTxn isolate(*session, 3);
        for (const auto& fx : session->fx)
            isolate.modify<model::FxInstance>(fx.id, [&](auto& f) { f.isolation = isolation; });
        const auto isolated = isolate.commit();
        REQUIRE(isolated);
        auto pool = makePool();
        const auto offline = renderOffline(*isolated, pool, {.startSeconds = 0, .endSeconds = 2.0});
        REQUIRE(offline);
        CHECK(offline->audio.channels == offlineReference->audio.channels);
        for (const auto& options : {kInCallback, EngineOptions{.workers = 2}}) {
            if (!options.anticipative && isolation != model::Isolation::InProcess) continue;
            CAPTURE(isolation);
            CAPTURE(options.anticipative);
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 100096});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
            REQUIRE(engine.load(*isolated));
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, 100096 / 96, 96);
            else
                backend.run(100096 / 96);
            engine.stop();
            CHECK(engine.processErrors() == 0);
            for (int channel = 0; channel < 2; ++channel)
                CHECK(maxDiff(backend.capture().channels[static_cast<std::size_t>(channel)],
                              engine.outputLatency(),
                              offline->audio.channels[static_cast<std::size_t>(channel)],
                              96000) == 0.0);
        }
    }
}

TEST_CASE("MIDI sends: Notes destinations and compensated taps null offline in both executors" *
          doctest::description("rt")) {
    // The source plays a sine through a latency plug-in: post-FX sends wait
    // for its chain (PDC), a pre-FX send does not. A third send plays a rack
    // whose Notes gates a VCA on the transport's running signal.
    const auto base = synthSession(-18.0, "com.suppleaudio.test.gain", 1);
    const auto source = base.tracks.find(kMaster)->children.front();
    model::EditTxn t(base, 2);
    auto addTrack = [&](const char* name, bool synth) {
        model::Track track;
        track.name = name;
        track.parent = kMaster;
        const auto id = t.create(track);
        t.modify<model::Track>(kMaster, [&](auto& master) { master.children.push_back(id); });
        if (synth) {
            model::FxInstance fx;
            fx.plugin.id = "com.suppleaudio.test.sine";
            const auto f = t.create(fx);
            t.modify<model::Track>(id, [&](auto& x) { x.chain.push_back(f); });
        }
        return id;
    };
    auto send = [&](model::Route route, model::Tap tap) {
        route.from.track = source;
        route.midi = model::MidiRouting{};
        route.tap = tap;
        t.create(route);
    };
    model::Route to;
    to.to.track = addTrack("Post-fader", true);
    send(to, model::Tap::PostFader);
    to.to.track = addTrack("Pre-FX", true);
    send(to, model::Tap::PreFx);
    const auto keys = addTrack("Rack", false);
    const auto rack = t.create(model::Rack{keys, "Gate"});
    auto module = [&](std::string_view type) {
        model::RackModule m;
        m.rack = rack;
        m.type = std::string(type);
        m.version = "1.0.0";
        return t.create(m);
    };
    const auto notes = module(model::kNotesModule);
    t.modify<model::RackModule>(notes, [](auto& m) { m.sourceOff = true; });
    const auto clock = module("com.suppleaudio.rack.transport");
    const auto vca = module("com.suppleaudio.rack.vca");
    const auto out = module(model::kOutModule);
    t.create(model::Cable{{clock, "playing"}, {vca, "in"}, std::nullopt, ""});
    t.create(model::Cable{{notes, "gate"}, {vca, "cv"}, std::nullopt, ""});
    t.create(model::Cable{{vca, "out"}, {out, "l"}, std::nullopt, ""});
    model::Route input;
    input.from.module = out;
    input.to.track = keys;
    const auto record = t.create(input);
    t.modify<model::Track>(keys, [&](auto& track) { track.record.input = record; });
    model::Route gate;
    gate.to.module = notes;
    send(gate, model::Tap::PostFader);
    const auto session = t.commit();
    REQUIRE_MESSAGE(session, (session ? "" : session.error().toString()));
    auto pool = makePool();
    const auto offline = renderOffline(*session, pool, {.startSeconds = 0, .endSeconds = 2.0});
    REQUIRE(offline);
    // Each destination sounds on its own: the comparison below is not of silence.
    for (const auto& entry : session->tracks) {
        if (entry.id == kMaster || entry.id == source) continue;
        auto own = makePool();
        OfflineSettings settings{.startSeconds = 0, .endSeconds = 2.0};
        settings.track = entry.id;
        const auto alone = renderOffline(*session, own, settings);
        REQUIRE(alone);
        float peak = 0;
        for (float v : alone->audio.channels[0]) peak = std::max(peak, std::abs(v));
        CAPTURE(entry.value->name);
        CHECK(peak > 0.01f);
    }
    for (const auto& options : {kInCallback, EngineOptions{.workers = 2}}) {
        CAPTURE(options.anticipative);
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 100096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
        REQUIRE(engine.load(*session));
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 100096 / 96, 96);
        else
            backend.run(100096 / 96);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        for (int channel = 0; channel < 2; ++channel)
            CHECK(maxDiff(backend.capture().channels[static_cast<std::size_t>(channel)],
                          engine.outputLatency(),
                          offline->audio.channels[static_cast<std::size_t>(channel)],
                          96000) == 0.0);
    }
}
