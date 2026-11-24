// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// How a run loads a named plug-in whatever the project says (topic 7d): the
// override map every pool takes, the commands that set it, and what a
// disabled or bridged plug-in does to renders, playback and saves.
#include <doctest/doctest.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "bridge_patience.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kGain = "com.suppleaudio.test.gain";

// A name no other run of this test shares, without any one platform's process-id call.
std::string runId() {
    static const std::string id = [] {
        std::random_device device;
        return std::to_string(device()) + std::to_string(device());
    }();
    return id;
}

struct Root {
    fs::path path = fs::temp_directory_path() / ("supple-load-overrides-" + runId());
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::unique_ptr<EngineApp> makeApp(const fs::path& dir) {
    AppOptions o;
    o.workdir = dir;
    o.chainDir = dir / "chains";  // never the user's config
    o.presetDir = dir / "presets";
    o.templateDir = dir / "templates";
    o.projectTemplateDir = dir / "project-templates";
    o.pluginPath = {kFixture};
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    return std::move(*app);
}

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

Result<host::PluginInfo> fixtureResolve(const model::PluginRef& ref) {
    host::ScanOptions options;
    options.forceInProcess = true;
    auto plugins = host::scan(std::span(&kFixture, 1), options);
    if (!plugins) return plugins.error();
    for (const auto& plugin : *plugins)
        if (plugin.id == ref.id) return plugin;
    return Error("engine.plugin_missing", "fixture " + ref.id + " not found");
}

std::string fileText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

struct Bus {
    Root root;
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    Bus() {
        host::setBridgeExecutable(BRIDGE_PATH);
        registerEngineCommands(registry);
        app = makeApp(root.path);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    Result<json::Value> run(std::string command, std::string_view args) {
        return bus->execute({std::move(command), j(args), kTest});
    }
    json::Value call(std::string command, std::string_view args) {
        auto r = run(std::move(command), args);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    }
    // A track with the sine instrument and a gain at -12 dB playing a note.
    void song() {
        call("batch", R"({"steps": [
            {"command": "track.add", "args": {"name": "Lead"}, "as": "t"},
            {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
            {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "com.suppleaudio.test.gain"}},
            {"command": "fx.patch.merge", "args": {"fx": "fx_2", "patch": {"params": {"gain": -12}}}},
            {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice lead; 1: C4:w"}}]})");
    }
};

}  // namespace

TEST_CASE(
    "a pool makes a bridged plug-in on a dedicated bridge whatever its fx and the default say") {
    host::setBridgeExecutable(BRIDGE_PATH);
    const model::Id id{model::Kind::Fx, 1};
    model::FxInstance fx;
    fx.plugin.id = kGain;
    fx.isolation = model::Isolation::InProcess;  // the fx names in process
    const auto session = model::Session::create();
    InstancePool pool(fixtureResolve);
    pool.setStockDir(kFixture.parent_path());  // and it is a stock plug-in at that
    auto first = pool.get(id, fx, session);
    REQUIRE(first);
    CHECK_FALSE((*first)->isolated());
    CHECK(pool.effectiveIsolation(id, fx) == model::Isolation::InProcess);

    LoadOverrides overrides;
    CHECK(overrides.set("clap", kGain, LoadMode::Bridge));
    CHECK_FALSE(overrides.set("clap", kGain, LoadMode::Bridge));
    pool.setOverrides(overrides);
    // The instance made before stays until it is retired, and nothing says it moved.
    CHECK(pool.get(id, fx, session).value() == *first);
    CHECK(pool.effectiveIsolation(id, fx) == model::Isolation::InProcess);
    pool.restartPlugin("clap", kGain);
    CHECK(pool.retired() == 1);
    auto bridged = pool.get(id, fx, session);
    REQUIRE(bridged);
    CHECK((*bridged)->isolated());
    CHECK(pool.effectiveIsolation(id, fx) == model::Isolation::Dedicated);
    // Another plug-in is not touched, nor is the same id in another format.
    CHECK(overrides.mode("vst3", kGain) == LoadMode::Normal);
    CHECK(overrides.mode("clap", "com.suppleaudio.test.edge") == LoadMode::Normal);

    // Back to normal: the next instance is in process again.
    pool.setOverrides({});
    pool.restartPlugin("clap", kGain);
    auto again = pool.get(id, fx, session);
    REQUIRE(again);
    CHECK_FALSE((*again)->isolated());
}

TEST_CASE("an instance in the pool that was never activated is not a plug-in that ran (Q8)") {
    const model::Id id{model::Kind::Fx, 1};
    model::FxInstance fx;
    fx.plugin.id = kGain;
    const auto session = model::Session::create();
    InstancePool pool(fixtureResolve);
    auto made = pool.get(id, fx, session);
    REQUIRE(made);
    // It exists, in process, and nothing has asked it to process, so it is not active: a clean
    // end must not count it as evidence that the plug-in is safe.
    CHECK_FALSE((*made)->active());
    CHECK(pool.effectiveIsolation(id, fx) == model::Isolation::InProcess);
    CHECK(pool.ranInProcess().empty());
}

TEST_CASE("a pool refuses a disabled plug-in and makes nothing of it") {
    const model::Id id{model::Kind::Fx, 1}, other{model::Kind::Fx, 2};
    const auto session = model::Session::create();
    InstancePool pool(fixtureResolve);
    LoadOverrides overrides;
    overrides.set("clap", kGain, LoadMode::Disable);
    pool.setOverrides(overrides);
    model::FxInstance gain;
    gain.plugin.id = kGain;
    auto refused = pool.get(id, gain, session);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "plugin.disabled");
    CHECK(refused.error().hint.find("plugin.override") != std::string::npos);
    CHECK(pool.size() == 0);
    model::FxInstance edge;
    edge.plugin.id = "com.suppleaudio.test.edge";
    CHECK(pool.get(other, edge, session));
}

TEST_CASE("the override commands, and what they do to a project that is playing") {
    Bus f;
    f.song();
    const model::Id fx = *model::Id::parse("fx_2");
    auto listed = f.call("plugin.overrides", "{}");
    CHECK(listed["overrides"].size() == 0);
    CHECK(listed["bridge_available"].asBool());

    f.call("engine.start", R"({"backend": "dummy"})");
    f.call("transport.play", "{}");
    host::Instance* original = f.app->instanceOf(fx);
    REQUIRE(original);
    CHECK_FALSE(original->isolated());
    // A probe made now is in process, and goes with the override.
    f.call("fx.describe", R"({"fx": "fx_2"})");
    REQUIRE(f.app->scratchProbe(kGain));
    CHECK_FALSE(f.app->scratchProbe(kGain)->isolated());

    SUBCASE("bridge") {
        auto set = f.call("plugin.override",
                          std::string(R"({"plugin":")") + kGain + R"(","mode":"bridge"})");
        CHECK(set["changed"].asBool());
        CHECK(set["mode"].asString() == "bridge");
        CHECK(set["fx"].size() == 1);
        CHECK(set["fx"].at(0).asString() == "fx_2");
        // The next plan plays a new instance on a bridge of its own.
        // plugin.override builds the new instance inside the call, so no pass
        // normally runs; the bound covers a load deferred to a later tick, and
        // is 60 s of the clock, not a count of ticks.
        host::Instance* now = f.app->instanceOf(fx);
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while ((!now || now == original) && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            f.app->tick();
            now = f.app->instanceOf(fx);
        }
        REQUIRE(now);
        CHECK(now != original);
        CHECK(now->isolated());
        CHECK(f.app->scratchProbe(kGain) == nullptr);
        auto described = f.call("fx.describe", R"({"fx": "fx_2"})");
        CHECK(described["isolation_effective"].asString() == "dedicated");
        CHECK(described["override"].asString() == "bridge");
        REQUIRE(f.app->scratchProbe(kGain));
        CHECK(f.app->scratchProbe(kGain)->isolated());
        // The project keeps what it said.
        CHECK(f.app->head().fx.find(fx)->isolation == model::Isolation::Inherit);
        CHECK(f.call("plugin.overrides", "{}")["overrides"].size() == 1);
        // Setting it again changes nothing.
        CHECK_FALSE(f.call("plugin.override", std::string(R"({"plugin":")") + kGain +
                                                  R"(","mode":"bridge"})")["changed"]
                        .asBool());
        // The latency probe borrows the bridged probe: no process of its own per call.
        const auto pid = f.app->scratchProbe(kGain)->bridgePid();
        f.call("engine.stop", "{}");
        for (int i = 0; i < 3; ++i)
            REQUIRE(f.app->fxLatencySamples(fx, *f.app->head().fx.find(fx)));
        CHECK(f.app->scratchProbe(kGain)->bridgePid() == pid);
        // Normal again.
        f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"normal"})");
        CHECK(f.app->scratchProbe(kGain) == nullptr);
        CHECK(f.call("plugin.overrides", "{}")["overrides"].size() == 0);
    }

    SUBCASE("disable") {
        // A save captures what plug-ins are running (the sine's state): the
        // comparison is between two saves of the same playing project.
        const auto before = f.root.path / "before.json";
        REQUIRE(f.app->save(before));
        f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"disable"})");
        // As above: the instance normally goes inside the call; the bound covers
        // a removal deferred to a later tick.
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (f.app->instanceOf(fx) && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            f.app->tick();
        }
        CHECK(f.app->instanceOf(fx) == nullptr);
        CHECK(f.app->scratchProbe(kGain) == nullptr);
        auto status = f.call("engine.status", "{}");
        REQUIRE(status["disabled_fx"].size() == 1);
        CHECK(status["disabled_fx"].at(0).asString() == "fx_2");
        CHECK(status["running"].asBool());
        // It still describes: from the session and the override alone.
        auto described = f.call("fx.describe", R"({"fx": "fx_2"})");
        CHECK(described["override"].asString() == "disable");
        CHECK(described.find("isolation_effective") == nullptr);
        CHECK(described.find("params") == nullptr);
        CHECK(described["description"].asString().find("Disabled") != std::string::npos);
        CHECK(described.find("patch") != nullptr);
        // Nothing can add it, or ask it for parameters.
        auto added = f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
        REQUIRE_FALSE(added);
        CHECK(added.error().code == "plugin.disabled");
        // Disabled or not, the saved project is the same bytes.
        const auto after = f.root.path / "after.json";
        REQUIRE(f.app->save(after));
        CHECK(fileText(after) == fileText(before));
        // The override survives a new project; the project itself is untouched.
        REQUIRE(f.app->newProject({.console9 = false}));
        CHECK(f.call("plugin.overrides", "{}")["overrides"].size() == 1);
        f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"normal"})");
        CHECK(f.call("engine.status", "{}")["disabled_fx"].size() == 0);
    }
    f.call("engine.stop", "{}");
}

TEST_CASE("bridge is refused without a bridge executable and for a stock plug-in, naming the fix") {
    struct Restore {
        ~Restore() { host::setBridgeExecutable(BRIDGE_PATH); }
    } restore;
    Bus f;
    f.song();
    REQUIRE(f.app->plugins());
    host::setBridgeExecutable({});
    CHECK_FALSE(f.call("plugin.overrides", "{}")["bridge_available"].asBool());
    auto refused =
        f.run("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"bridge"})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "plugin.override_unavailable");
    CHECK_FALSE(refused.error().hint.empty());
    CHECK(f.call("plugin.overrides", "{}")["overrides"].size() == 0);  // nothing was set
    // Disabling needs no bridge.
    f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"disable"})");
    auto unknown = f.run("plugin.override", R"({"plugin":"com.example.nothing","mode":"disable"})");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "plugin.unknown");
    auto badMode =
        f.run("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"later"})");
    CHECK_FALSE(badMode);
    // The pool's own check, for a plug-in in the stock directory.
    InstancePool pool(fixtureResolve);
    auto info = fixtureResolve({.id = kGain});
    REQUIRE(info);
    CHECK_FALSE(pool.isStock(*info));
    pool.setStockDir(kFixture.parent_path());
    CHECK(pool.isStock(*info));
}

TEST_CASE("an offline render of a disabled plug-in is the render with that plug-in missing") {
    Bus f;
    f.song();
    OfflineSettings settings;
    settings.endSeconds = 1.0;
    auto rendered = [&](const model::Session& s) {
        auto r = f.app->renderSession(s, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::move(*r);
    };
    const model::Id fx = *model::Id::parse("fx_2");
    const auto normal = rendered(f.app->head());
    model::EditTxn edit(f.app->head(), 99);
    edit.modify<model::FxInstance>(
        fx, [](model::FxInstance& x) { x.plugin.id = "com.example.missing"; });
    auto missingSession = edit.commit();
    REQUIRE(missingSession);
    const auto missing = rendered(*missingSession);
    f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"disable"})");
    const auto disabled = rendered(f.app->head());
    // The gain at -12 dB would change the level; the placeholder passes audio through.
    REQUIRE(normal.audio.channels[0].size() == disabled.audio.channels[0].size());
    CHECK(normal.audio.channels[0] != disabled.audio.channels[0]);
    CHECK(disabled.audio.channels[0] == missing.audio.channels[0]);
    CHECK(disabled.audio.channels[1] == missing.audio.channels[1]);
    CHECK(*std::max_element(disabled.audio.channels[0].begin(), disabled.audio.channels[0].end()) >
          0.01f);
    bool said = false;
    for (const auto& w : disabled.warnings) said = said || w.find("disabled") != std::string::npos;
    CHECK(said);
    // Normal again renders as before, bit for bit.
    f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"normal"})");
    CHECK(rendered(f.app->head()).audio.channels[0] == normal.audio.channels[0]);
}

TEST_CASE("an offline render of a bridged plug-in is the in-process render, bit for bit") {
    Bus f;
    f.song();
    OfflineSettings settings;
    settings.endSeconds = 1.0;
    auto render = [&] {
        auto r = f.app->renderSession(f.app->head(), settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::move(*r);
    };
    const test::BridgeDeadline patient(test::kPatientBridgeMs);
    const auto inProcess = render();
    f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":"bridge"})");
    const auto bridged = render();  // the render's own pool took the override too
    CHECK(bridged.audio.channels[0] == inProcess.audio.channels[0]);
    CHECK(bridged.audio.channels[1] == inProcess.audio.channels[1]);
    CHECK(*std::max_element(bridged.audio.channels[0].begin(), bridged.audio.channels[0].end()) >
          0.01f);
}

TEST_CASE(
    "a render job takes the overrides too: a disabled plug-in is missing, a bridged one is "
    "bridged") {
    Bus f;
    f.song();
    const test::BridgeDeadline patient(test::kPatientBridgeMs);
    int count = 0;
    // render.mix with job:true renders a snapshot of the project on a job thread, in a pool of
    // its own (renderSnapshot), and writes a file: read back as samples.
    auto mixed = [&]() -> Result<audio::AudioData> {
        const auto queued = f.call("render.mix", R"({"name":"job)" + std::to_string(++count) +
                                                     R"(","end_beat":2,"job":true})");
        const std::string id = queued.find("job")->asString();
        for (int waited = 0; waited < 6000; ++waited) {  // 60 s, bounded and named below
            f.bus->flushJobEvents();
            auto state = f.bus->jobs().status(id);
            REQUIRE(state);
            const auto status = state->find("status")->asString();
            if (status == "done") {
                const auto* result = state->find("result");
                REQUIRE(result);
                REQUIRE(result->find("path"));
                return audio::readAudio(result->find("path")->asString());
            }
            if (status == "failed") return Error("render.job_failed", json::writeCompact(*state));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL("the render.mix job did not finish in 60 s");
        return Error("render.job_timeout", "timed out");
    };
    auto peakOf = [](const audio::AudioData& a) {
        float peak = 0;
        for (float x : a.channels[0]) peak = std::max(peak, std::fabs(x));
        return peak;
    };
    const auto gain = std::string(R"({"plugin":")") + kGain + R"(","mode":")";
    auto normal = mixed();
    REQUIRE_MESSAGE(normal, (normal ? std::string() : normal.error().toString()));
    REQUIRE(peakOf(*normal) > 0.01f);

    SUBCASE("disabled: the job renders the plug-in as missing") {
        f.call("plugin.override", gain + R"(disable"})");
        auto disabled = mixed();
        REQUIRE_MESSAGE(disabled, (disabled ? std::string() : disabled.error().toString()));
        // The gain at -12 dB lowers the level; the placeholder passes the audio through.
        const float ratio = peakOf(*disabled) / peakOf(*normal);
        CHECK(ratio > 3.5f);
        CHECK(ratio < 4.5f);
    }
    SUBCASE("bridged: the job's plug-in is made on a bridge, and renders the same") {
        f.call("plugin.override", gain + R"(bridge"})");
        auto bridged = mixed();
        REQUIRE_MESSAGE(bridged, (bridged ? std::string() : bridged.error().toString()));
        const bool same = bridged->channels == normal->channels;  // not printed: it is long
        CHECK(same);
        // With no bridge to make, the job cannot make it: the override is what asked for one.
        // (In process it would render as before, which is what the audio above cannot tell apart.)
        const auto executable = host::bridgeExecutable();
        host::setBridgeExecutable(fs::path("/nonexistent") / "bridge");
        const auto without = mixed();
        host::setBridgeExecutable(executable);
        if (without) {
            const bool alike = without->channels == normal->channels;
            CHECK_FALSE(alike);
        } else
            CHECK_FALSE(without.error().code.empty());
    }
}

TEST_CASE("changing an override while playing swaps the instance in both executors" *
          doctest::description("rt")) {
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        Bus f;
        f.song();
        const model::Id fx = *model::Id::parse("fx_2");
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 1 << 16});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64},
                                   test::patient({.anticipative = anticipative})));
        f.call("transport.play", "{}");
        auto cycles = [&](int n) {
            for (int i = 0; i < n; ++i) {
                if (anticipative) REQUIRE(f.app->engine()->awaitRendered(64));
                backend.run(1);
                f.app->tick();
            }
        };
        cycles(40);
        host::Instance* first = f.app->instanceOf(fx);
        REQUIRE(first);
        CHECK_FALSE(first->isolated());
        const std::string gain = std::string(R"({"plugin":")") + kGain;
        // Bridge: a new instance, on a bridge, once a plan with it is adopted.
        f.call("plugin.override", gain + R"(","mode":"bridge"})");
        cycles(80);
        host::Instance* bridged = f.app->instanceOf(fx);
        REQUIRE(bridged);
        CHECK(bridged != first);
        CHECK(bridged->isolated());
        // Disable: no instance; the strip passes audio through and the engine plays on.
        f.call("plugin.override", gain + R"(","mode":"disable"})");
        cycles(80);
        CHECK(f.app->instanceOf(fx) == nullptr);
        // Normal: a fresh in-process instance.
        f.call("plugin.override", gain + R"(","mode":"normal"})");
        cycles(80);
        host::Instance* last = f.app->instanceOf(fx);
        REQUIRE(last);
        CHECK_FALSE(last->isolated());
        CHECK(f.app->engine()->processErrors() == 0);
        CHECK(f.app->engine()->bridgeLateCycles() == 0);
        f.call("engine.stop", "{}");
    }
}

TEST_CASE("opening a project with cables into plug-in modules starts one bridge, not one per fx") {
    Bus f;
    f.call("track.add", R"({"name": "Synth"})");
    f.call("rack.add", R"({"track": "tr_1"})");                                         // mo_1 Out
    f.call("module.add", R"({"rack": "rk_1", "type": "lfo", "params": {"rate": 3}})");  // mo_2
    const char* amounts[] = {"-9", "-6", "-3"};
    for (int i = 0; i < 3; ++i) {  // mo_3..mo_5, fx_1..fx_3, each with a state of its own
        f.call("module.add", R"({"rack": "rk_1", "plugin": "com.suppleaudio.test.gain"})");
        f.call("fx.patch.merge", std::string(R"({"fx": "fx_)") + std::to_string(i + 1) +
                                     R"(", "patch": {"params": {"gain": )" + amounts[i] + "}}}");
        f.call("cable.add", std::string(R"({"from": "mo_2.out", "to": "fx_)") +
                                std::to_string(i + 1) + R"(.gain", "amount": 0.3})");
    }
    const fs::path song = f.root.path / "song.json";
    REQUIRE(f.app->save(song));
    f.call("project.new", "{}");

    auto open = [&](LoadMode mode) {
        f.call("plugin.override", std::string(R"({"plugin":")") + kGain + R"(","mode":")" +
                                      std::string(loadModeName(mode)) + "\"}");
        std::vector<schema::Warning> warnings;
        const auto before = f.app->instancesMade();
        REQUIRE(f.app->open(song, &warnings));
        CHECK(warnings.empty());
        CHECK(f.app->head().fx.size() == 3);
        return f.app->instancesMade() - before;
    };
    CHECK(open(LoadMode::Normal) == 3);  // in process: a probe per fx, as always
    f.call("project.new", "{}");
    // Under a bridge each probe is a process: the plug-in's one cached probe serves all three.
    CHECK(open(LoadMode::Bridge) == 1);  // (the project that loads replaces the cached probe)
}
