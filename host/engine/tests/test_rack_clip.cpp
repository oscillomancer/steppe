// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>

#include "rack_runtime.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace {
struct ClipFixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("rack-clip-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    DummyBackend backend{DummyBackend::Options{.mode = DummyBackend::Mode::Manual}};
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    ClipFixture() {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto made = EngineApp::create(options);
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name":"First"})");
        run("rack.add", R"({"track":"tr_1"})");
    }
    ~ClipFixture() {
        bus.reset();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Result<json::Value> call(std::string name, std::string_view text = "{}") {
        return bus->execute(
            {std::move(name), json::parse(text).value(), {cmd::Origin::Type::Test, "clip", ""}});
    }
    json::Value run(std::string name, std::string_view text = "{}") {
        auto result = call(std::move(name), text);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
};
model::Id id(const json::Value& v) { return *model::Id::parse(v.asString()); }
}  // namespace

TEST_CASE("rack clipboard: cut and paste retain live opaque plug-in state and internal cables" *
          doctest::description("rt")) {
    ClipFixture f;
    f.run("module.add",
          R"({"rack":"rk_1","type":"transport","x":10,"y":30,"label":"Clock"})");  // mo_2
    f.run(
        "module.add",
        R"({"rack":"rk_1","plugin":"com.suppleaudio.test.edge","x":210,"y":50,"label":"Effect"})");  // mo_3, fx_1
    f.run("cable.add", R"({"from":"mo_2.beat_phase","to":"mo_3.in_l","color":"red"})");
    f.run("cable.add", R"({"from":"mo_3.out_l","to":"mo_1.l"})");  // external, omitted
    REQUIRE(f.app->startEngine(f.backend, {.sampleRate = 48000, .blockSize = 64},
                               {.anticipative = false, .workers = 0}));
    auto* original = f.app->instanceOf({model::Kind::Fx, 1});
    REQUIRE(original);
    const std::vector<std::uint8_t> state{0, 123, 2, 0};
    REQUIRE(original->loadState(state));  // live change not yet captured in the project
    const auto beforeCopy = model::serializeProject(f.app->head());
    auto copied = f.run("module.copy", R"({"modules":["mo_3","mo_2"]})");
    CHECK(copied["modules"].asInt() == 2);
    CHECK(copied["cables"].asInt() == 1);
    CHECK(model::serializeProject(f.app->head()) == beforeCopy);
    REQUIRE(original->loadState(
        std::vector<std::uint8_t>{0, 0, 0, 0}));  // clipboard has copy-time state
    f.run("module.remove", R"({"module":"mo_3"})");
    f.run("module.remove", R"({"module":"mo_2"})");
    f.run("track.add", R"({"name":"Second"})");
    f.run("rack.add", R"({"track":"tr_2"})");
    const auto beforePaste = f.app->head();
    auto pasted = f.run("rack.paste", R"({"rack":"rk_2","x":500,"y":100})");
    REQUIRE(pasted["modules"].asArray().size() == 2);
    REQUIRE(pasted["cables"].asArray().size() == 1);
    const auto plugin = id(pasted["modules"].asArray()[0]),
               clock = id(pasted["modules"].asArray()[1]);
    const auto* m = f.app->head().modules.find(plugin);
    REQUIRE(m);
    REQUIRE(m->fx);
    CHECK(m->label == "Effect");
    CHECK(m->x == 700);
    CHECK(m->y == 120);
    CHECK(m->rack == model::Id{model::Kind::Rack, 2});
    const auto* c = f.app->head().modules.find(clock);
    REQUIRE(c);
    CHECK(c->x == 500);
    CHECK(c->y == 100);
    CHECK(c->label == "Clock");
    auto* live = f.app->instanceOf(*m->fx);
    REQUIRE(live);
    auto restored = live->saveState();
    REQUIRE(restored);
    CHECK(*restored == state);  // state already loaded before the first callback
    const auto& cb = *f.app->head().cables.find(id(pasted["cables"].asArray()[0]));
    CHECK(cb.from.object == clock);
    CHECK(cb.to.object == plugin);
    CHECK(cb.color == "red");
    f.run("history.undo");
    // IDs remain monotonic through undo; every pasted object and its blob go.
    auto undone = f.app->head();
    undone.next = beforePaste.next;
    CHECK(model::serializeProject(undone) == model::serializeProject(beforePaste));
    auto same = f.run("rack.paste", R"({"rack":"rk_1"})");
    CHECK(f.app->head().modules.find(id(same["modules"].asArray()[0]))->rack ==
          model::Id{model::Kind::Rack, 1});
}

TEST_CASE("rack clipboard: semantic settings, module fields and learn bindings survive") {
    ClipFixture f;
    f.run("module.add", R"({"rack":"rk_1","type":"vca","params":{"level":-12},"x":10,"y":20})");
    f.run("module.set", R"({"module":"mo_2","bypass":true,"label":"Quiet"})");
    f.run("module.learn.set",
          R"({"module":"mo_2","param":"level","midi":{"kind":"cc","channel":1,"number":21}})");
    f.run("module.add", R"({"rack":"rk_1","type":"notes","source":"tr_1","device":"controller"})");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
    REQUIRE(f.app->startEngine(f.backend, {.sampleRate = 48000, .blockSize = 64},
                               {.anticipative = false, .workers = 0}));
    auto* gain = f.app->instanceOf({model::Kind::Fx, 1});
    REQUIRE(gain);
    REQUIRE(gain->setPatch(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-17}})",
                           CLAP_SUPPLE_SET_REPLACE));
    f.run("module.add", R"({"rack":"rk_1","type":"transport"})");
    f.run("cable.add",
          R"({"from":"mo_5.beat_phase","to":"fx_1.gain","amount":0.25,"color":"#123456"})");
    f.run("module.copy", R"({"modules":["mo_2","mo_3","mo_4","mo_5"]})");
    auto paste = f.run("rack.paste", R"({"rack":"rk_1","x":300,"y":200})");
    const auto& vca = *f.app->head().modules.find(id(paste["modules"].asArray()[0]));
    CHECK(vca.bypass);
    CHECK(vca.label == "Quiet");
    CHECK(vca.params.find("level")->asDouble() == -12);
    REQUIRE(vca.learn.size() == 1);
    CHECK(vca.learn.front().param == "level");
    const auto& notes = *f.app->head().modules.find(id(paste["modules"].asArray()[1]));
    CHECK(notes.source == model::Id{model::Kind::Track, 1});
    CHECK(notes.device == "controller");
    const auto& plugin = *f.app->head().modules.find(id(paste["modules"].asArray()[2]));
    REQUIRE(paste["cables"].asArray().size() == 1);
    const auto& cb = *f.app->head().cables.find(id(paste["cables"].asArray()[0]));
    CHECK(cb.from.object == id(paste["modules"].asArray()[3]));
    CHECK(cb.to.object == *plugin.fx);
    CHECK(cb.to.key == "gain");
    CHECK(cb.amount == 0.25);
    CHECK(cb.color == "#123456");
    auto patch = f.app->instanceOf(*plugin.fx)->getPatch(false);
    REQUIRE(patch);
    CHECK(json::parse(*patch)->operator[]("params")["gain"].asDouble() == -17);
}

TEST_CASE("rack clipboard: unavailable clips, modules and plug-ins leave the project unchanged") {
    ClipFixture f;
    auto expect = [&](std::string command, std::string_view args, std::string_view code) {
        const auto before = model::serializeProject(f.app->head());
        auto result = f.call(std::move(command), args);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().path.empty());
        CHECK_FALSE(result.error().hint.empty());
        CHECK(model::serializeProject(f.app->head()) == before);
    };
    expect("rack.paste", R"({"rack":"rk_1"})", "rack.clip_empty");
    expect("module.copy", R"({"modules":["mo_999"]})", "module.unknown");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.edge"})");
    f.run("module.copy", R"({"modules":["mo_2"]})");
    f.app->installPlugins({}, {});
    expect("rack.paste", R"({"rack":"rk_1"})", "chain.plugin_missing");
    f.run("module.copy", R"({"modules":["mo_1"]})");
    f.app->setModules(std::make_shared<const rack::Catalog>());
    expect("rack.paste", R"({"rack":"rk_1"})", "rack.unknown_type");
    expect("rack.paste", R"({"rack":"rk_999"})", "rack.unknown");
}
