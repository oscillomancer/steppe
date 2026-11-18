// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <numeric>
#include "carry_matches.h"
#include "rack_runtime.h"
#include "realtime_common.h"
#include "supple/base/files.h"
#include "supple/base/thread.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::engine {
// This executable is separate from engine.tests, which has its own friend probe.
struct EngineTestAccess {
    static std::size_t plans(const Engine& e) { return e.plans_.size(); }
    static std::size_t planCapacity(const Engine& e) { return e.plans_.capacity(); }
    static bool adopted(const Engine& e) {
        return e.stoppedGen_.load() >= e.publishedGen_ &&
               (!e.options_.anticipative || e.callbackGen_.load() >= e.publishedGen_);
    }
    static bool planReady(const Engine& e) {
        return !e.ring_ || e.stoppedGen_.load(std::memory_order_acquire) >= e.publishedGen_;
    }
    static bool rendered(Engine& e, std::int64_t frames) {
        if (!e.ring_) return true;
        const auto epoch = e.epoch_.load();
        const bool playing = e.wantPlaying_.load();
        if (e.epochDone_.load() >= epoch && e.reportedPlaying_.load() == playing &&
            (!playing ||
             e.renderedStream_.load(std::memory_order_acquire) - e.playedStream(epoch) >=
                 std::min(frames, e.aheadFrames_)))
            return true;
        e.wakeRenderer();
        return false;
    }
    static std::size_t pending(const Engine& e) {
        return e.planHandoffs_.size() + e.ratePlanRequests_.size();
    }
    static std::size_t rackRetired(const detail::RackPool& pool) { return pool.retired_.size(); }
    // Callback-only probe, after its driver's completion acknowledgement.
    static std::uint32_t wetRamp(const Engine& e, Id fx) {
        std::uint32_t remaining = 0;
        for (const auto& node : e.plan_->nodes)
            detail::forEachStage(node, [&](const detail::Stage& stage) {
                if (stage.fx == fx) remaining = stage.wetRampLeft;
            });
        return remaining;
    }
};
}  // namespace supple::engine

namespace {
constexpr std::uint32_t kBlock = 128;
constexpr std::size_t kSteps = 64;
constexpr float kOutputBound = 8.f;  // bounded graph, attenuating gains, no feedback

struct SoakRandom {
    std::uint64_t state;
    std::uint64_t next() {
        // SplitMix64: same operation stream with every standard library/platform.
        auto x = (state += 0x9e3779b97f4a7c15ULL);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    std::size_t pick(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

std::uint64_t setting(const char* key, std::uint64_t fallback) {
    const char* text = std::getenv(key);
    if (!text) return fallback;
    std::uint64_t value = 0;
    const auto end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    REQUIRE_MESSAGE((parsed.ec == std::errc{} && parsed.ptr == end),
                    key << " must be an unsigned decimal integer");
    return value;
}

std::string watchdogContext;
template <class Predicate>
void untilReady(const char* stage, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            // A hung handshake is an aborted run, never a numeric audio verdict.
            // Do not unwind through a potentially hung engine or keep fuzzing.
            std::cerr << "plan-soak WATCHDOG ABORT stage=" << stage << ' ' << watchdogContext
                      << " (no audio mismatch verdict), exit=124" << std::endl;
            std::_Exit(124);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void planReady(Engine& engine) {
    untilReady("renderer plan adoption", [&] { return EngineTestAccess::planReady(engine); });
}

void runReady(Engine& engine, DummyBackend& backend, std::int64_t cycles) {
    for (std::int64_t i = 0; i < cycles; ++i) {
        untilReady("manual render readiness",
                   [&] { return EngineTestAccess::rendered(engine, kBlock); });
        backend.run(1);
    }
}
// Readiness is supplied separately so the regression can place a real
// manual callback acknowledgement at the same boundary as the racing callback.
template <class Ready>
bool collectAdoptedPlan(Engine& engine, Ready ready) {
    const auto generation = engine.planGeneration();
    if (!ready()) return false;
    engine.service();
    // Service may publish a replacement after a latency/restart notification.
    // Its acknowledgements need another collection pass even if they arrive now.
    return engine.planGeneration() == generation && ready();
}

void replayRetirementCheckpoint() {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, {.workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
    const auto original = model::Session::create({.console9 = false});
    REQUIRE(engine.load(original));
    planReady(engine);
    backend.run(1);
    engine.service();
    REQUIRE(EngineTestAccess::plans(engine) == 1);

    model::EditTxn edit(original, original.version + 1);
    edit.modify<model::Track>(kMaster, [](auto& track) { track.name = "Checkpoint"; });
    auto changed = edit.commit();
    REQUIRE(changed);
    REQUIRE(engine.load(*changed));
    planReady(engine);
    // Renderer ready, callback still using the preceding plan. Collection
    // must retain it until that callback has released it.
    REQUIRE_FALSE(EngineTestAccess::adopted(engine));
    REQUIRE(EngineTestAccess::plans(engine) == 2);
    bool acknowledged = false;
    REQUIRE(collectAdoptedPlan(engine, [&] {
        if (!acknowledged) {
            backend.run(1);
            acknowledged = true;
        }
        return EngineTestAccess::adopted(engine);
    }));
    REQUIRE(EngineTestAccess::plans(engine) == 1);
    REQUIRE(engine.processErrors() == 0);
    engine.stop();
    REQUIRE(EngineTestAccess::plans(engine) == 0);
}

json::Value args(std::string_view text) { return json::parse(text).value(); }
std::string content(const model::Session& s) {
    auto doc = model::projectToJson(s);
    // ADR-0008: undo retains lineage allocation high-water marks.
    doc["project"].asObject().erase("next_ids");
    return json::writeCanonical(doc);
}

// Observe every sample, including samples beyond the bounded capture buffer.
// Only the audio thread writes its counters; the model thread reads atomics.
class ObservedDummy : public DummyBackend {
  public:
    ObservedDummy() : DummyBackend({.mode = Mode::Manual}) {}
    ~ObservedDummy() override { stop(); }
    Status start(const BackendConfig& config, Callback callback) override {
        callback_ = std::move(callback);
        channels_ = config.outputs;
        auto result =
            DummyBackend::start(config, [this](auto in, auto out, auto n) { observe(in, out, n); });
        if (!result) return result;
        running_.store(true);
        driver_ = std::thread([this, config] {
            nameThread("audio-dummy");
            const auto period = std::chrono::duration<double>(config.blockSize / config.sampleRate);
            while (running_.load()) {
                // Driver pacing/parking is outside the annotated audio entry.
                // The real callback never waits, locks or allocates.
                const bool free = free_.load();
                if (!free && permits_.load() == 0) {
                    parkSeen_.store(parkRequest_.load(), std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                if (!free) permits_.fetch_sub(1);
                DummyBackend::run(1);
                completed_.fetch_add(1, std::memory_order_release);
                std::this_thread::sleep_for(period);
            }
        });
        return okStatus();
    }
    void stop() override {
        running_.store(false);
        if (driver_.joinable()) driver_.join();
        DummyBackend::stop();
    }
    bool ownsThread() const override { return true; }
    ThreadPolicy callbackPolicy() override {
        return driver_.joinable() ? threadPolicy(driver_.native_handle()) : ThreadPolicy{};
    }
    void park() {
        free_.store(false);
        const auto request = parkRequest_.fetch_add(1) + 1;
        untilReady("park paced driver",
                   [&] { return parkSeen_.load(std::memory_order_acquire) >= request; });
    }
    void resume() { free_.store(true); }
    void arm(std::size_t frames) {
        // Model thread, with the driver parked. Reused bounded window storage.
        window_.sampleRate = config().sampleRate;
        window_.channels.assign(channels_, std::vector<float>(frames));
        captured_ = 0;
        capturing_ = true;
    }
    void advance(Engine& engine, std::int64_t cycles) {
        for (std::int64_t i = 0; i < cycles; ++i) {
            untilReady("paced render readiness",
                       [&] { return EngineTestAccess::rendered(engine, kBlock); });
            const auto goal = completed_.load() + 1;
            permits_.fetch_add(1, std::memory_order_release);
            untilReady("paced callback completion",
                       [&] { return completed_.load(std::memory_order_acquire) >= goal; });
        }
    }
    const audio::AudioData& window() {
        capturing_ = false;  // after the final completion acknowledgement
        REQUIRE(captured_ == window_.frames());
        return window_;
    }
    bool bad() const { return bad_.load(); }
    bool sounded() const { return sounded_.load(); }

  private:
    void observe(const float* const* in, float* const* out, std::uint32_t n) SUPPLE_RT {
        callback_(in, out, n);
        if (capturing_) {
            const auto frames = std::min<std::size_t>(n, window_.frames() - captured_);
            for (std::uint32_t c = 0; c < channels_; ++c)
                std::copy_n(out[c], frames, window_.channels[c].data() + captured_);
            captured_ += frames;
        }
        for (std::uint32_t c = 0; c < channels_; ++c)
            for (std::uint32_t i = 0; i < n; ++i) {
                const float value = out[c][i];
                if (!std::isfinite(value) || std::fabs(value) > kOutputBound) bad_.store(true);
                if (std::fabs(value) > 0.0001f) sounded_.store(true);
            }
    }
    Callback callback_;
    std::uint32_t channels_ = 0;
    std::atomic<bool> bad_{false}, sounded_{false};
    std::thread driver_;
    std::atomic<bool> running_{false}, free_{true};
    std::atomic<std::uint64_t> parkRequest_{0}, parkSeen_{0};
    std::atomic<std::uint64_t> permits_{0}, completed_{0};
    audio::AudioData window_;
    std::size_t captured_ = 0;
    bool capturing_ = false;
};

struct Soak {
    std::filesystem::path root;
    ObservedDummy backend;  // both backends outlive the app, even on REQUIRE failure
    DummyBackend manual{DummyBackend::Options{.mode = DummyBackend::Mode::Manual,
                                              .captureFrames = 6 * 48000 + 4096}};
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    std::ofstream journal;
    std::string trace;
    std::string phase = "setup";
    std::map<std::string, model::Counters> highWater;
    std::map<std::string, std::size_t> commands;
    std::string initial, rack, rackOut, clock, vca, buss, rackPlugin, rackPluginFx;
    // Route E stage 5: each settled head from the edits on, played again at
    // the end with and without plan swaps that change nothing.
    std::vector<model::Session> heads;
    std::vector<std::string> headCommands;  // the journal row that made each
    std::string lastCommand;
    EngineOptions options;
    bool passed = false;
    std::uint64_t swaps = 0;
    std::uint64_t verified = 0;
    std::uint64_t checkpoints = 0;
    bool automaticCheckpoints = true;
    // Route E stage 6: each settled head compiled again with a pool of the
    // test's own (the app's pool and its retirement checks stay untouched),
    // the last three kept, to compare the carries' lookups with the pairwise
    // scans they replaced.
    InstancePool matchPool = makePool();
    std::deque<supple::engine::detail::Plan> matchPlans;
    supple::engine::carry_test::CarryMatches carryMatches;

    Soak(std::uint64_t seed, bool ahead, bool explicitCheckpoints = false)
        : options{.anticipative = ahead,
                  .renderBlock = 128,
                  .renderAheadMs = 20,
                  .workers = 2,
                  .verifyPlans = true} {
        automaticCheckpoints = !explicitCheckpoints;
        root = std::filesystem::temp_directory_path() /
               ("supple-plan-soak-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto candidate = root;
        for (unsigned suffix = 0;; ++suffix) {
            root = candidate.string() + "-" + std::to_string(suffix);
            std::error_code error;
            if (std::filesystem::create_directory(root, error)) break;
            REQUIRE_MESSAGE((!error || error == std::errc::file_exists), error.message());
        }
        journal.open(root / "replay.jsonl");
        REQUIRE(journal.good());
        auto header = json::Value::object();
        header["schema"] = "supple.plan-soak/1";
        header["seed"] = std::to_string(seed);
        header["anticipative"] = ahead;
        journal << json::writeCompact(header) << '\n' << std::flush;
        std::cout << "plan-soak seed=" << seed << " anticipative=" << ahead
                  << " replay=" << (root / "replay.jsonl").string() << std::endl;
        watchdogContext = "seed=" + std::to_string(seed) +
                          " anticipative=" + std::to_string(ahead) +
                          " replay=" + (root / "replay.jsonl").string();
        AppOptions config;
        config.workdir = root;
        config.pluginPath = {kFixture};
        auto created = EngineApp::create(config, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        app->setFixedClock(10000);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        const auto baseFile = root / "base.json";
        const auto base = synthSession(-24, "com.suppleaudio.test.gain", 2);
        model::EditTxn configured(base, base.version + 1);
        auto project = configured.project();
        project.console9 = bool(seed & 1);
        project.seed = seed & 0x7fffffff;
        configured.setProject(project);
        auto initialSession = configured.commit();
        REQUIRE(initialSession);
        REQUIRE(writeFileAtomic(baseFile, model::serializeProject(*initialSession)));
        REQUIRE(app->open(baseFile));
        buss = call("track.add", args(R"({"name":"Bus","type":"buss"})"))["track"].asString();
        auto added = call("rack.add", args(R"({"name":"Rack"})"));
        rack = added["rack"].asString();
        rackOut = added["out"].asString();
        call("track.set",
             args("{\"track\":\"" + added["track"].asString() + "\",\"volume_db\":-24}"));
        clock =
            call("module.add", args("{\"rack\":\"" + rack + "\",\"type\":\"transport\"}"))["module"]
                .asString();
        vca = call("module.add", args("{\"rack\":\"" + rack +
                                      "\",\"type\":\"vca\",\"params\":{\"level\":-30}}"))["module"]
                  .asString();
        connect(clock + ".playing", vca + ".in");
        // A latent plug-in module at half wet: what a plan swap must carry of
        // a rack's plug-in stage (its dry delay, a wet or bypass ramp) is
        // audible here, and the edits below move its bypass and latency.
        rackPlugin =
            call("module.add", args("{\"rack\":\"" + rack +
                                    "\",\"plugin\":\"com.suppleaudio.test.gain\"}"))["module"]
                .asString();
        rackPluginFx = app->head().modules.find(*Id::parse(rackPlugin))->fx->str();
        call("fx.patch.merge", args("{\"fx\":\"" + rackPluginFx +
                                    "\",\"patch\":{\"params\":{\"latency\":\"short\"}}}"));
        call("fx.set", args("{\"fx\":\"" + rackPluginFx + "\",\"wet\":50}"));
        connect(vca + ".out", rackPlugin + ".in_l");
        connect(rackPlugin + ".out_l", rackOut + ".l");
        call("env.add",
             args(R"({"track":"tr_2","param":"volume","points":[{"beat":0,"value":-24}]})"));
        initial = content(app->head());
        call("branch.create", args(R"({"name":"left","switch":false})"));
        call("branch.create", args(R"({"name":"right","switch":false})"));
        call("branch.switch", args(R"({"name":"left"})"));
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = kBlock}, options));
        call("transport.loop", args(R"({"start_seconds":0,"end_seconds":0.25})"));
        call("transport.play");
        settle();
        // Setup is fixed; replay starts with its initial window, then the edits.
        phase = "edits";
        heads.push_back(app->head());
        headCommands.push_back("(setup)");
        commands.clear();
        trace.clear();
        journal.close();
        journal.open(root / "replay.jsonl", std::ios::trunc);
        REQUIRE(journal.good());
        journal << json::writeCompact(header) << '\n' << std::flush;
        // Establish audibility from a fixed, ready window even for empty replays.
        if (automaticCheckpoints) audioCheckpoint();
    }
    ~Soak() {
        bus.reset();
        app.reset();
        journal.close();
        if (passed && !std::getenv("SUPPLE_SOAK_KEEP_TRACE"))
            std::filesystem::remove_all(root);
        else if (!passed)
            std::cerr << "plan-soak retained replay: " << (root / "replay.jsonl").string() << '\n'
                      << trace;
    }
    json::Value call(std::string command, json::Value a = json::Value::object()) {
        auto row = json::Value::object();
        row["phase"] = phase;
        row["command"] = command;
        row["args"] = a;
        const auto line = json::writeCompact(row) + "\n";
        trace += line;
        lastCommand = line;
        journal << line << std::flush;  // flushed BEFORE dispatch, also survives a sanitizer abort
        REQUIRE(journal.good());
        ++commands[command];
        auto result =
            bus->execute({command, std::move(a), {cmd::Origin::Type::Test, "plan-soak", {}}});
        INFO(trace);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        checkModel();
        return *result;
    }
    void checkModel() {
        auto valid = model::checkInvariants(app->head());
        REQUIRE_MESSAGE(valid, (valid ? "" : valid.error().toString()));
        auto& prior = highWater[app->versions().branch()];
        for (std::size_t i = 0; i < model::kKindCount; ++i) {
            REQUIRE(app->head().next.next[i] >= prior.next[i]);
            prior.next[i] = app->head().next.next[i];
        }
    }
    void connect(const std::string& from, const std::string& to) {
        call("cable.add", args("{\"from\":\"" + from + "\",\"to\":\"" + to + "\"}"));
    }
    void settle() {
        INFO(trace);
        untilReady("source preparation", [&] {
            app->tick();
            return !app->preparingAudio();
        });
        REQUIRE_MESSAGE(app->preparationError().empty(), app->preparationError());
        REQUIRE(app->engine() != nullptr);
        Engine& engine = *app->engine();
        // service can itself publish a plan after a latency/restart callback.
        // Drain that adoption too before asserting reclamation at quiescence.
        untilReady("plan reclamation", [&] {
            planReady(engine);
            untilReady("transport playing acknowledgement",
                       [&] { return engine.transport().playing; });
            const auto target = engine.cycles() + 4;
            untilReady("four callback acknowledgements", [&] { return engine.cycles() >= target; });
            return collectAdoptedPlan(engine, [&] { return EngineTestAccess::adopted(engine); });
        });
        REQUIRE(engine.transport().playing);
        REQUIRE(engine.processErrors() == 0);
        REQUIRE_FALSE(backend.bad());
        REQUIRE(EngineTestAccess::plans(engine) == 1);
        REQUIRE(EngineTestAccess::planCapacity(engine) <= 8);
        REQUIRE(EngineTestAccess::pending(engine) == 0);
        REQUIRE(app->instances().retired() == 0);
        REQUIRE(EngineTestAccess::rackRetired(app->instances().racks()) == 0);
        REQUIRE(app->instances().size() <= app->head().fx.size());
        REQUIRE(app->instances().racks().size() <= app->head().modules.size());
        REQUIRE(app->head().tracks.size() <= 8);
        REQUIRE(app->head().fx.size() <= 8);
        REQUIRE(app->head().items.size() <= 9);
        REQUIRE(app->head().routes.size() <= 7);
        REQUIRE(app->head().modules.size() <= 7);
        checkModel();
        if (phase == "edits" && app->head().version != heads.back().version) {
            heads.push_back(app->head());
            headCommands.push_back(lastCommand);
        }
        if (phase == "edits") checkCarryMatches();
        if (phase == "edits" && automaticCheckpoints) audioCheckpoint();
    }
    void audioCheckpoint(const json::Value* replay = nullptr) {
        constexpr std::size_t frames = 1024;
        const auto index = replay ? replay->find("index")->asInt() : std::int64_t(checkpoints);
        const auto start =
            replay ? replay->find("start_frame")->asInt()
                   : std::llround(app->head().timeMap->secondsAt(checkpoints % 4) * 48000);
        const auto context = watchdogContext;
        watchdogContext += " checkpoint=" + std::to_string(index);
        auto row = json::Value::object();
        row["phase"] = "checkpoint";
        row["index"] = index;
        row["start_frame"] = start;
        row["frames"] = frames;
        const auto line = json::writeCompact(row) + "\n";
        trace += line;
        journal << line << std::flush;
        REQUIRE(journal.good());
        INFO(trace);
        ++checkpoints;
        INFO("intermediate audio checkpoint " << index << " start_frame=" << start);
        // Flush intent before any handshake, so a watchdog-aborted checkpoint
        // remains in an explicit replay rather than silently disappearing.
        auto& engine = *app->engine();
        backend.park();
        const auto restore = engine.transport();
        // Adoption can acknowledge at the start of an in-flight block. Drain
        // the specified 10 ms swap/wet fade after parking, in sample counts,
        // before comparing with an offline plan that starts at its target.
        constexpr std::int64_t fadeFrames = 480;  // fixed 48 kHz fixture
        backend.advance(engine, (fadeFrames + kBlock - 1) / kBlock);
        // A locate is the defined RT/offline DSP reset boundary. Keep the
        // running engine, pooled instances and audio thread through the probe.
        const OfflineSettings settings{.startSeconds = double(start) / 48000,
                                       .endSeconds = double(start + frames) / 48000,
                                       .threads = 1};
        auto offline = app->renderSession(app->head(), settings);
        REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
        engine.setLoop(0, 0, false);
        engine.seek(settings.startSeconds);
        const auto latency = engine.outputLatency();
        const auto cycles = (frames + latency + kBlock - 1) / kBlock;
        backend.arm(cycles * kBlock);
        const auto misses = engine.anticipationMisses();
        backend.advance(engine, cycles);
        equalAudio(offline->audio, backend.window(), latency);
        REQUIRE(engine.anticipationMisses() == misses);
        REQUIRE(engine.processErrors() == 0);
        engine.setLoop(double(restore.loop.start) / 48000, double(restore.loop.end) / 48000,
                       restore.loop.on);
        engine.seek(restore.seconds);
        backend.resume();
        watchdogContext = context;
    }
    // The carries pair what the scans before route E stage 6 paired: from
    // the previous plan and from the one before it (an executor that skipped
    // a plan), in each executor's classes: the callback's (-1), the
    // renderer's (0), the live region's (1), the renderer's handoff into it
    // (1 from 0), against those scans (carry_matches.h). The soak has no
    // MIDI send and no live region: test_racks.cpp's "carries pair what the
    // pairwise scans paired" has them.
    void checkCarryMatches() {
        auto compiled = supple::engine::detail::compile(app->head(), matchPool, 0, kBlock, 48000);
        REQUIRE_MESSAGE(compiled, (compiled ? "" : compiled.error().toString()));
        matchPlans.push_back(std::move(*compiled));
        if (matchPlans.size() > 3) matchPlans.pop_front();
        for (std::size_t k = 0; k + 1 < matchPlans.size(); ++k)
            supple::engine::carry_test::check(matchPlans[k], matchPlans.back(), carryMatches);
        INFO(trace);
        REQUIRE(carryMatches.wrong == 0);
    }
    void step(std::string command, json::Value a = json::Value::object()) {
        call(std::move(command), std::move(a));
        settle();
    }
    template <class Map, class Predicate>
    std::vector<std::string> ids(const Map& map, Predicate include) {
        std::vector<std::string> out;
        for (const auto& entry : map)
            if (include(entry.id, *entry.value)) out.push_back(entry.id.str());
        return out;
    }
    void randomStep(unsigned kind, SoakRandom& r) {
        const auto& s = app->head();
        auto channels = ids(s.tracks, [](Id id, const auto& t) {
            return id.n != 0 && id.n != 3 && id.n != 4 && t.type == model::StripType::Channel;
        });
        auto extraTracks = ids(s.tracks, [](Id id, const auto&) { return id.n > 4; });
        auto gains = ids(s.fx, [&](Id id, const auto& f) {
            return f.plugin.id == "com.suppleaudio.test.gain" && id.str() != rackPluginFx;
        });
        // Bypass and patch edits reach the rack's plug-in too (it stays).
        auto tunable = gains;
        tunable.push_back(rackPluginFx);
        auto items = ids(s.items, [](Id id, const auto&) { return id.n != 1; });
        auto sends = ids(s.routes, [](Id, const auto& route) {
            return !route.to.track || route.to.track->n != 4;
        });
        auto modules = ids(s.modules, [&](Id id, const auto&) {
            return id.str() != rackOut && id.str() != clock && id.str() != vca &&
                   id.str() != rackPlugin;
        });
        auto choose = [&](const auto& values) { return values[r.pick(values.size())]; };
        auto a = json::Value::object();
        auto addTrack = [&] { step("track.add", args(R"({"name":"Extra"})")); };
        auto addFx = [&] {
            a["track"] = choose(channels);
            a["plugin"] = "com.suppleaudio.test.gain";
            a["patch"] = args(R"({"params":{"gain":-6,"latency":"short"}})");
            step("fx.add", a);
        };
        auto addItem = [&] {
            a["track"] = choose(channels);
            a["position"] = double(r.pick(4));
            a["notation"] = "voice a; 1: C4:q E4:q G4:q C5:q";
            step("item.add", a);
        };
        switch (kind) {
            case 0:
                if (extraTracks.size() < 3) {
                    addTrack();
                    break;
                }
                [[fallthrough]];
            case 1:
                if (extraTracks.empty()) {
                    addTrack();
                    break;
                }
                a["track"] = choose(extraTracks);
                step("track.remove", a);
                break;
            case 2:
                a["track"] = choose(channels);
                a["parent"] = r.pick(2) ? buss : "tr_0";
                a["index"] = 0;
                step("track.move", a);
                break;
            case 3:
                if (gains.size() < 6) {
                    addFx();
                    break;
                }
                [[fallthrough]];
            case 4:
                if (gains.empty()) {
                    addFx();
                    break;
                }
                a["fx"] = choose(gains);
                step("fx.remove", a);
                break;
            case 5:
                a["fx"] = choose(tunable);
                a["bypass"] = bool(r.pick(2));
                step("fx.set", a);
                break;
            case 6:
                a["fx"] = choose(tunable);
                a["patch"] = args("{\"params\":{\"gain\":" + std::to_string(-int(r.pick(19))) +
                                  ",\"latency\":\"" + (r.pick(2) ? "short" : "none") + "\"}}");
                step("fx.patch.merge", a);
                break;
            case 7:
            case 8:
                if (sends.size() >= 6) {
                    a["route"] = choose(sends);
                    step("route.remove", a);
                    break;
                }
                a["from"] = kind == 7 ? choose(channels) : rackOut;
                a["to"] = buss;
                a["volume_db"] = -18;
                step("route.add", a);
                break;
            case 9:
                if (sends.empty()) {
                    a["from"] = "tr_1";
                    a["to"] = buss;
                    step("route.add", a);
                    break;
                }
                a["route"] = choose(sends);
                step("route.remove", a);
                break;
            case 10:
                if (items.size() < 6) {
                    addItem();
                    break;
                }
                [[fallthrough]];
            case 11:
                if (items.empty()) {
                    addItem();
                    break;
                }
                a["item"] = choose(items);
                a["position"] = double(r.pick(4));
                a["track"] = choose(channels);
                step("item.move", a);
                break;
            case 12: {
                auto split = ids(s.items, [](Id id, const auto& item) {
                    return id.n != 1 && item.length > 0.5;
                });
                if (items.size() >= 8) {
                    a["item"] = choose(items);
                    step("item.remove", a);
                    break;
                }
                if (split.empty()) {
                    addItem();
                    break;
                }
                auto id = choose(split);
                const auto& item = *s.items.find(*Id::parse(id));
                a["item"] = id;
                a["at"] = item.position + item.length / 2;
                step("item.split", a);
                break;
            }
            case 13:
                if (items.empty()) {
                    addItem();
                    break;
                }
                a["item"] = choose(items);
                step("item.remove", a);
                break;
            case 14:
                a["points"] = args("[{\"beat\":0,\"bpm\":" + std::to_string(90 + r.pick(61)) +
                                   ",\"ramp\":true},{\"beat\":4,\"bpm\":120}]");
                step("tempo.set", a);
                break;
            case 15:
                a["env"] = "en_1";
                a["points"] = args("[{\"beat\":0,\"value\":-24},{\"beat\":2,\"value\":" +
                                   std::to_string(-int(r.pick(25))) + "}]");
                step("env.points.set", a);
                break;
            case 16: {
                if (modules.size() >= 3) {
                    a["module"] = choose(modules);
                    step("module.remove", a);
                    break;
                }
                a["rack"] = rack;
                a["type"] = "vca";
                a["params"] = args(R"({"level":-36})");
                auto id = call("module.add", a)["module"].asString();
                settle();
                connect(clock + ".playing", id + ".in");
                settle();
                connect(id + ".out", rackOut + ".r");
                settle();
                break;
            }
            case 17:
                if (modules.empty()) {
                    a["module"] = vca;
                    a["bypass"] = true;
                    step("module.set", a);
                    break;
                }
                a["module"] = choose(modules);
                step("module.remove", a);
                break;
            case 18:
                a["module"] = modules.empty() ? vca : choose(modules);
                a["bypass"] = bool(r.pick(2));
                a["params"] = args("{\"level\":" + std::to_string(-24 - int(r.pick(25))) + "}");
                step("module.set", a);
                break;
            case 19:
                a["start_seconds"] = 0;
                a["end_seconds"] = (1 + double(r.pick(5))) / 20;
                step("transport.loop", a);
                break;
            case 20:
                a["seconds"] = double(r.pick(8)) / 20;
                step("transport.seek", a);
                break;
            case 21:
                if (app->versions().canUndo()) {
                    step("history.undo");
                    step("history.redo");
                } else
                    addTrack();
                break;
            case 22:
                a["name"] = app->versions().branch() == "left" ? "right" : "left";
                step("branch.switch", a);
                break;
        }
    }
    void finish() {
        INFO(trace);
        phase = "finish";
        REQUIRE(backend.sounded());
        REQUIRE_FALSE(backend.bad());
        auto& engine = *app->engine();
        swaps = engine.planGeneration();
        verified = engine.verifiedPlans();
        REQUIRE(verified > 0);  // each compile checked against a second (route E stage 5)
        engine.stop();
        REQUIRE(EngineTestAccess::plans(engine) == 0);
        REQUIRE(EngineTestAccess::pending(engine) == 0);
        app->instances().prune(app->head());
        REQUIRE(app->instances().retired() == 0);
        REQUIRE(EngineTestAccess::rackRetired(app->instances().racks()) == 0);
        app->stopEngine();
        const auto finalFile = root / "final.json";
        REQUIRE(app->save(finalFile));
        double endBeat = 4;
        for (const auto& item : app->head().items)
            endBeat = std::max(endBeat, item.value->position + item.value->length);
        const OfflineSettings settings{.startSeconds = 0,
                                       .endSeconds = app->head().timeMap->secondsAt(endBeat) + 0.1};
        auto offline = app->renderSession(app->head(), settings);
        REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
        auto fresh = EngineApp::create(app->options(), {.console9 = false});
        REQUIRE(fresh);
        REQUIRE((*fresh)->open(finalFile));
        REQUIRE(model::serializeProject(app->head()) == model::serializeProject((*fresh)->head()));
        auto reopened = (*fresh)->renderSession((*fresh)->head(), settings);
        REQUIRE(reopened);
        equalAudio(offline->audio, reopened->audio, 0);
        // Reuse the original RT pool, not the fresh app's pool: locates must
        // reset DSP history left by all preceding swaps before the exact null.
        auto finalOptions = options;
        finalOptions.renderAheadMs = 200;  // runAhead drives up to 4096 frames at a time
        REQUIRE(app->startEngine(manual, {.sampleRate = 48000, .blockSize = kBlock}, finalOptions));
        auto& finalEngine = *app->engine();
        finalEngine.setLoop(0, 0, false);
        finalEngine.seek(0);
        finalEngine.play();
        const auto latency = finalEngine.outputLatency();
        const auto cycles = (offline->audio.frames() + latency + kBlock - 1) / kBlock;
        runReady(finalEngine, manual, cycles);
        finalEngine.stop();
        REQUIRE(finalEngine.processErrors() == 0);
        app->stopEngine();
        for (const char* branch : {"left", "right"}) {
            call("branch.switch", args(std::string("{\"name\":\"") + branch + "\"}"));
            while (app->versions().canUndo()) call("history.undo");
            REQUIRE(content(app->head()) == initial);
            const auto prior = highWater[branch];
            for (std::size_t i = 0; i < model::kKindCount; ++i)
                REQUIRE(app->head().next.next[i] >= prior.next[i]);
        }
        equalAudio(offline->audio, manual.capture(), latency);
        std::size_t count = 0;
        for (const auto& [name, n] : commands) count += n;
        std::cout << "plan-soak completed commands=" << count << " published_plans=" << swaps
                  << " verified_plans=" << verified << " audio_checkpoints=" << checkpoints
                  << " retired_plans=0 retired_fx=0 retired_modules=0" << std::endl;
        passed = true;
    }
    static void equalAudio(const audio::AudioData& expected, const audio::AudioData& actual,
                           std::size_t offset) {
        REQUIRE(expected.channels.size() == actual.channels.size());
        for (std::size_t c = 0; c < expected.channels.size(); ++c) {
            const auto& e = expected.channels[c];
            const auto& a = actual.channels[c];
            REQUIRE(a.size() >= e.size() + offset);
            REQUIRE(std::all_of(e.begin(), e.end(), [](float value) {
                return std::isfinite(value) && std::fabs(value) <= kOutputBound;
            }));
            INFO("channel=" << c << " offset=" << offset
                            << " max difference=" << maxDiff(a, offset, e, e.size()));
            std::size_t first = 0;
            while (first < e.size() && std::bit_cast<std::uint32_t>(e[first]) ==
                                           std::bit_cast<std::uint32_t>(a[offset + first]))
                ++first;
            REQUIRE_MESSAGE(first == e.size(),
                            "plan-soak AUDIO MISMATCH channel=" << c << " frame=" << first);
        }
    }
};

// Route E stage 5: a plan swap that changes nothing is inaudible. The soak's
// heads play in turn on a manual device, one every kStepCycles callbacks
// from a fixed cycle, the renderer full at each load, so that every run
// takes each plan at the same position of both executors. With `noOps`, two
// swaps that change nothing follow each edit: a marker added two cycles
// after it (inside its 10 ms ramps) and removed at cycle 13. The two runs
// must be the same, sample for sample: whatever a swap fails to carry
// (a ramp, a delay history, a voice, a rack's plug-in stage) is heard.
constexpr std::int64_t kStepCycles = 24, kMarkerAt = 2, kUnmarkAt = 13;
struct Played {
    std::vector<std::vector<float>> channels;
    std::uint64_t noOps = 0;  // swaps that changed nothing
};
Played playHeads(const std::vector<model::Session>& heads, bool ahead, bool noOps) {
    auto pool = makePool();
    MediaCache media;
    const auto frames = static_cast<std::int64_t>(heads.size()) * kStepCycles * kBlock;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
    Engine engine(pool, media,
                  {.anticipative = ahead,
                   .renderBlock = 128,
                   .renderAheadMs = 20,
                   .workers = 2,
                   .verifyPlans = true});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
    Played played;
    std::int64_t cycle = 0;
    // The renderer has rendered all it may: what it does next is decided
    // by the callback's position alone, the same in every run.
    const auto full = [&] {
        untilReady("no-change swap: renderer full", [&] {
            return EngineTestAccess::rendered(engine, std::numeric_limits<std::int64_t>::max());
        });
    };
    const auto runTo = [&](std::int64_t until) {
        runReady(engine, backend, until - cycle);
        cycle = until;
        // Both runs hear the engine's notifications at the same cycles, so
        // a no-op compile never takes one early. A plug-in asks for a
        // restart (a latency change) where its executor applies a patch: the
        // renderer, ahead, so it first renders all it may.
        full();
        engine.service();
    };
    const auto load = [&](const model::Session& s, bool compiles) {
        full();  // it adopts at the same position in every run
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(s));
        if (compiles) REQUIRE(engine.planGeneration() == generation + 1);
        engine.service();
    };
    REQUIRE(engine.load(heads[0]));
    engine.setLoop(0, 0.25, true);
    engine.play();
    for (std::size_t i = 0; i < heads.size(); ++i) {
        const std::int64_t start = static_cast<std::int64_t>(i) * kStepCycles;
        if (i > 0) load(heads[i], false);
        runTo(start + kMarkerAt);
        if (noOps) {
            model::EditTxn t(heads[i], heads[i].version + 1);
            t.create(model::Marker{});
            load(t.commit().value(), true);
        }
        runTo(start + kUnmarkAt);
        if (noOps) load(heads[i], true);
        played.noOps += noOps ? 2 : 0;
        runTo(start + kStepCycles);
    }
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.verifiedPlans() > 0);
    played.channels = backend.capture().channels;
    return played;
}

std::uint64_t checkNoChangeSwaps(const std::vector<model::Session>& heads,
                                 const std::vector<std::string>& commands, bool ahead) {
    const auto plain = playHeads(heads, ahead, false);
    const auto swapped = playHeads(heads, ahead, true);
    REQUIRE(plain.channels.size() == swapped.channels.size());
    bool sounded = false;
    for (std::size_t c = 0; c < plain.channels.size(); ++c) {
        const auto& a = plain.channels[c];
        const auto& b = swapped.channels[c];
        REQUIRE(a.size() == b.size());
        std::size_t first = 0;
        while (first < a.size() &&
               std::bit_cast<std::uint32_t>(a[first]) == std::bit_cast<std::uint32_t>(b[first]))
            ++first;
        const auto step = first / (kStepCycles * kBlock);
        INFO("the head's edit: " << (step < commands.size() ? commands[step] : "(none)"));
        INFO("channel=" << c << " frame=" << first << " head=" << step
                        << " cycle in step=" << (first / kBlock) % kStepCycles
                        << " max difference=" << maxDiff(b, 0, a, a.size()));
        CHECK_MESSAGE(first == a.size(), "plan-soak NO-CHANGE SWAP AUDIBLE");
        sounded |= std::any_of(a.begin(), a.end(), [](float v) { return std::fabs(v) > 1e-4f; });
    }
    CHECK(sounded);
    return swapped.noOps;
}

void replayBypassCheckpoint() {
    Soak soak(202611150122, false, true);
    soak.call("fx.patch.merge", args(R"({"fx":"fx_2","patch":{"params":{"gain":-6}}})"));
    soak.settle();
    auto& engine = *soak.app->engine();
    soak.backend.park();
    soak.backend.advance(engine, 4);
    soak.call("fx.set", args(R"({"fx":"fx_2","bypass":true})"));
    // Adoption and three completed blocks leave 96 samples of the intended
    // 480-sample fade. A fresh offline plan starts fully bypassed.
    soak.backend.advance(engine, 3);
    REQUIRE(EngineTestAccess::wetRamp(engine, {model::Kind::Fx, 2}) == 96);
    soak.audioCheckpoint();
    soak.finish();
}
}  // namespace

TEST_CASE("soak audio checkpoints drain a bypass fade after adoption" *
          doctest::description("rt")) {
    replayBypassCheckpoint();
}

TEST_CASE("soak reclamation observes callback adoption before collection" *
          doctest::description("rt")) {
    replayRetirementCheckpoint();
}

TEST_CASE("seeded plan-swap soak during playback in both executors" * doctest::description("rt")) {
    const auto seconds = setting("SUPPLE_SOAK_SECONDS", 0);
    const auto keepGoing = setting("SUPPLE_SOAK_CONTINUE", 0);
    REQUIRE(keepGoing <= 1);
    REQUIRE(seconds <= 86400);
    const auto steps = setting("SUPPLE_SOAK_STEPS", kSteps);
    REQUIRE(steps > 0);
    REQUIRE(steps <= 4096);
    const std::string executor =
        std::getenv("SUPPLE_SOAK_EXECUTOR") ? std::getenv("SUPPLE_SOAK_EXECUTOR") : "both";
    REQUIRE((executor == "both" || executor == "callback" || executor == "ahead"));
    std::vector<json::Value> replayRows;
    bool explicitCheckpoints = false;
    auto masterSeed = setting("SUPPLE_SOAK_SEED", 20261114);
    std::string replayExecutor;
    const char* replayPath = std::getenv("SUPPLE_SOAK_REPLAY");
    if (replayPath) {
        std::ifstream input(replayPath);
        REQUIRE(input);
        std::string line;
        REQUIRE(static_cast<bool>(std::getline(input, line)));
        auto header = json::parse(line);
        REQUIRE(header);
        REQUIRE((*header)["schema"] == "supple.plan-soak/1");
        if (const auto* probe = header->find("probe")) {
            if (probe->asString() == "bypass-checkpoint") {
                replayBypassCheckpoint();
            } else {
                REQUIRE(probe->asString() == "retirement-checkpoint");
                replayRetirementCheckpoint();
            }
            return;
        }
        const auto& seedText = (*header)["seed"].asString();
        const auto parsedSeed =
            std::from_chars(seedText.data(), seedText.data() + seedText.size(), masterSeed);
        REQUIRE(
            (parsedSeed.ec == std::errc{} && parsedSeed.ptr == seedText.data() + seedText.size()));
        replayExecutor = (*header)["anticipative"].asBool() ? "ahead" : "callback";
        while (std::getline(input, line)) {
            auto row = json::parse(line);
            REQUIRE(row);
            if ((*row)["phase"] == "edits") {
                REQUIRE((*row)["command"].isString());
                REQUIRE((*row)["args"].isObject());
            } else if ((*row)["phase"] == "checkpoint") {
                for (const char* key : {"index", "start_frame", "frames"})
                    REQUIRE((*row)[key].isInt());
                REQUIRE((*row)["index"].asInt() >= 0);
                REQUIRE((*row)["start_frame"].asInt() >= 0);
                REQUIRE((*row)["start_frame"].asInt() <= 10 * 48000);
                REQUIRE((*row)["frames"].asInt() == 1024);
                explicitCheckpoints = true;
            } else {
                continue;  // setup and finish rows are diagnostics
            }
            REQUIRE(replayRows.size() < 32768);
            replayRows.push_back(std::move(*row));
        }
    }
    std::map<std::string, std::size_t> coverage;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    SoakRandom seeds{masterSeed};
    unsigned epochs = 0;
    unsigned failures = 0;
    do {
        const auto seed = epochs == 0 ? masterSeed : seeds.next();
        for (bool ahead : {false, true}) {
            const auto& selected = replayPath ? replayExecutor : executor;
            if ((selected == "callback" && ahead) || (selected == "ahead" && !ahead)) continue;
            CAPTURE(seed);
            CAPTURE(ahead);
            try {
                Soak soak(seed, ahead, explicitCheckpoints);
                if (replayPath) {
                    for (const auto& row : replayRows) {
                        if (*row.find("phase") == "checkpoint")
                            soak.audioCheckpoint(&row);
                        else
                            soak.step(row.find("command")->asString(), *row.find("args"));
                    }
                } else {
                    SoakRandom random{seed};
                    std::array<unsigned, 23> order;
                    for (std::uint64_t step = 0; step < steps; ++step) {
                        if (step % order.size() == 0) {
                            std::iota(order.begin(), order.end(), 0u);
                            for (std::size_t i = order.size() - 1; i > 0; --i)
                                std::swap(order[i], order[random.pick(i + 1)]);
                        }
                        soak.randomStep(order[step % order.size()], random);
                    }
                }
                soak.finish();
                std::cout << "plan-soak carry matches: nodes=" << soak.carryMatches.nodes
                          << " edges=" << soak.carryMatches.edges << std::endl;
                // A seeded run compares many plans; a short replay may compile one.
                if (!replayPath) {
                    REQUIRE(soak.carryMatches.nodes > 0);
                    REQUIRE(soak.carryMatches.edges > 0);
                }
                const auto noOps = checkNoChangeSwaps(soak.heads, soak.headCommands, ahead);
                std::cout << "plan-soak no-change swaps: heads=" << soak.heads.size()
                          << " swaps=" << noOps << std::endl;
                for (const auto& [name, count] : soak.commands) coverage[name] += count;
            } catch (const doctest::detail::TestFailureException&) {
                if (!keepGoing || replayPath) throw;
                ++failures;
                std::cout << "plan-soak assertion failure seed=" << seed
                          << " anticipative=" << ahead << std::endl;
            }
        }
        ++epochs;
    } while (!replayPath && (epochs < (std::getenv("SUPPLE_SOAK_SEED") ? 1u : 2u) ||
                             std::chrono::steady_clock::now() < deadline));
    std::cout << "plan-soak epochs=" << epochs << " failed_scenarios=" << failures << std::endl;
    if (!replayPath && steps >= kSteps && executor == "both" && !std::getenv("SUPPLE_SOAK_SEED")) {
        for (const char* command :
             {"track.add",    "track.remove",   "track.move",   "fx.add",         "fx.remove",
              "fx.set",       "fx.patch.merge", "route.add",    "route.remove",   "item.add",
              "item.move",    "item.split",     "item.remove",  "tempo.set",      "env.points.set",
              "module.add",   "module.remove",  "module.set",   "transport.loop", "transport.seek",
              "history.undo", "history.redo",   "branch.switch"}) {
            INFO(command);
            REQUIRE(coverage[command] > 0);
        }
    }
}
