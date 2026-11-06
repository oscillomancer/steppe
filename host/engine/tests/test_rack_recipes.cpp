// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The recipes in the agents' rack guide (skill/guides/rack.md, "Recipes"),
// run as printed: each JSON block goes to rack.patch on a fresh rack, and
// what it cables into the Out is probed, so a renamed module, port, param or
// choice key breaks this test instead of the guide.
#include <doctest/doctest.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/write.h"

using namespace supple;
using namespace supple::engine;

namespace {

const cmd::Origin origin{cmd::Origin::Type::Test, "recipes", ""};

struct Recipe {
    std::string title;  // the bold words that open its paragraph
    std::string json;   // its ```json block, as printed
};

// Every recipe with a JSON block, in the guide's order.
std::vector<Recipe> recipes() {
    auto text = readTextFile(SUPPLE_RACK_GUIDE, 1 << 20);
    REQUIRE_MESSAGE(text, "the rack guide is missing: " << SUPPLE_RACK_GUIDE);
    const std::string& t = *text;
    const auto begin = t.find("\n## Recipes");
    REQUIRE(begin != std::string::npos);
    const auto end = t.find("\n## ", begin + 1);
    std::vector<Recipe> out;
    std::string title;
    std::size_t at = begin;
    while (true) {
        const auto bold = t.find("\n**", at);
        const auto fence = t.find("```json\n", at);
        if (fence == std::string::npos || fence > end) break;
        if (bold != std::string::npos && bold < fence) {
            const auto close = t.find("**", bold + 3);
            title = t.substr(bold + 3, close - bold - 3);
            at = close + 2;
            continue;
        }
        const auto body = fence + 8;
        const auto stop = t.find("```", body);
        out.push_back({title, t.substr(body, stop - body)});
        at = stop + 3;
    }
    return out;
}

struct Fixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("rack-recipes-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    Fixture() {
        std::filesystem::create_directories(dir);
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto made = EngineApp::create(options);
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~Fixture() {
        bus.reset();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    json::Value run(std::string name, const json::Value& args) {
        auto result = bus->execute({std::move(name), args, origin});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
};

// What a recipe patches and how its Out sounds over a probe.
struct Played {
    std::map<std::string, std::string> ids;  // name -> module id
    json::Value out;                         // the probe of what feeds mo_1.l
    json::Value ports;                       // the probes asked for, by name
};

Played play(const Recipe& recipe, double seconds, const std::vector<std::string>& extra = {}) {
    Fixture f;
    f.run("track.add", *json::parse(R"({"name": "Recipe"})"));
    f.run("rack.add", *json::parse(R"({"track": "tr_1"})"));  // rk_1, its Out mo_1
    auto args = json::parse(recipe.json);
    REQUIRE_MESSAGE(args, recipe.title << ": the JSON does not parse");
    std::string feed;
    for (const auto& cable : (*args)["cables"].asArray())
        if (cable.asArray()[1].asString() == "mo_1.l") feed = cable.asArray()[0].asString();
    (*args)["rack"] = "rk_1";
    json::Value patched = f.run("rack.patch", *args);
    Played p;
    for (const auto& m : patched["modules"].asObject()) p.ids[m.key] = m.value.asString();
    auto resolve = [&](const std::string& port) {
        const auto dot = port.find('.');
        const auto it = p.ids.find(port.substr(0, dot));
        return (it == p.ids.end() ? port.substr(0, dot) : it->second) + port.substr(dot);
    };
    REQUIRE_MESSAGE(!feed.empty(), recipe.title << ": nothing is cabled into mo_1.l");
    json::Value probe = json::Value::object();
    probe["rack"] = "rk_1";
    probe["seconds"] = seconds;
    json::Value ports = json::Value::array();
    ports.push_back(resolve(feed));
    for (const auto& e : extra) ports.push_back(resolve(e));
    probe["ports"] = std::move(ports);
    json::Value r = f.run("rack.probe", probe);
    auto& got = r["ports"].asArray();
    REQUIRE(got.size() == extra.size() + 1);
    p.out = got[0];
    p.ports = json::Value::object();
    for (std::size_t k = 0; k < extra.size(); ++k) p.ports[extra[k]] = got[k + 1];
    return p;
}

const Recipe& find(const std::vector<Recipe>& all, const std::string& title) {
    for (const auto& r : all)
        if (r.title.rfind(title, 0) == 0) return r;
    FAIL("the guide has no recipe \"" << title << "\"");
    return all.front();
}

}  // namespace

TEST_CASE("rack recipes: every recipe in the guide patches as printed") {
    const auto all = recipes();
    CHECK(all.size() >= 6);
    for (const auto& recipe : all) {
        CAPTURE(recipe.title);
        // A plug-in recipe needs that plug-in installed; the rest stand alone.
        if (recipe.json.find("\"plugin\"") != std::string::npos) continue;
        // An effect is silent until a send reaches its In, so it only patches.
        const bool effect = recipe.json.find("\"type\": \"in\"") != std::string::npos;
        Played p = play(recipe, 2);
        CHECK(p.ids.size() >= 3);
        if (!effect) CHECK_MESSAGE(p.out["peak"].asDouble() > 0.01, json::writeCompact(p.out));
    }
}

TEST_CASE("rack recipes: the drums play 16 starts a bar") {
    const auto all = recipes();
    Played p = play(find(all, "Polyrhythmic drums"), 4, {"lanes.any"});
    CHECK(p.ports["lanes.any"]["gates"]["count"].asInt() == 32);  // two bars at 120
    CHECK(p.out["peak_db"].asDouble() > -12);
    CHECK(p.out["peak_db"].asDouble() < -3);
}

TEST_CASE("rack recipes: the evolving melody's passes differ") {
    const auto all = recipes();
    Played p = play(find(all, "An evolving melody"), 10, {"tune.start", "tune.pitch"});
    CHECK(p.ports["tune.start"]["gates"]["count"].asInt() == 3);
    CHECK(p.ports["tune.pitch"]["notes"].asArray().size() >= 6);
    CHECK(p.out["peak_db"].asDouble() > -12);
}

TEST_CASE("rack recipes: the modulation web moves the drone") {
    const auto all = recipes();
    Played p = play(find(all, "A modulation web"), 10, {"web.out_2", "web.out_3"});
    CHECK(p.ports["web.out_2"]["rate_hz"].asDouble() == doctest::Approx(0.5).epsilon(0.2));
    CHECK(p.ports["web.out_3"]["min"].asDouble() > 0.3);
    CHECK(p.ports["web.out_3"]["max"].asDouble() > p.ports["web.out_3"]["min"].asDouble() + 0.2);
    CHECK(p.out["peak_db"].asDouble() > -20);
}

TEST_CASE("rack recipes: the song keeps its counter-melody below the lead") {
    const auto all = recipes();
    Played p =
        play(find(all, "A song that writes itself"), 10, {"low.interval", "song.edge", "song.a"});
    CHECK(p.ports["low.interval"]["min"].asDouble() >= -16.0 / 12 - 1e-3);
    CHECK(p.ports["low.interval"]["max"].asDouble() <= -3.0 / 12 + 1e-3);
    CHECK(p.ports["song.edge"]["gates"]["duty"].asDouble() == doctest::Approx(0.4).epsilon(0.05));
    CHECK(p.ports["song.a"]["gates"]["duty"].asDouble() == doctest::Approx(0.6).epsilon(0.05));
    CHECK(p.out["peak_db"].asDouble() > -12);
}
