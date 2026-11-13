// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the factory rack patch tests share (test_rack_patches*.cpp, split so
// each part stays within the test timeout under the sanitizers).
#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>

#include "supple/engine/app.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/rack/stock.h"

using namespace supple;
using namespace supple::engine;

namespace {

const cmd::Origin origin{cmd::Origin::Type::Test, "patches", ""};

constexpr double kCeilingDbfs = 1.0;   // no factory patch peaks above this
constexpr double kSilentDbfs = -50.0;  // one whose peak is below this is silent

struct Fixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("rack-patches-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    Fixture() {
        std::filesystem::create_directories(dir);
        AppOptions options;
        options.workdir = dir;
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
    json::Value run(std::string name, const std::string& args) {
        auto parsed = json::parse(args);
        REQUIRE_MESSAGE(parsed, args);
        auto result = bus->execute({std::move(name), *parsed, origin});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
};

// Loads the factory patch `p` into a fresh rack, plays it as a user would and
// checks it stays under full scale without being silent.
inline void playsUnderFullScale(const rack::FactoryPatch& p) {
    auto doc = json::parse(p.text);
    REQUIRE(doc);
    const std::string name = (*doc)["name"].asString();
    CAPTURE(name);
    Fixture f;
    f.run("track.add", R"({"name": "Patch"})");
    f.run("track.add", R"({"name": "Source"})");
    f.run("rack.add", R"({"track": "tr_1"})");  // rk_1, its Out mo_1
    f.run("rack.add", R"({"track": "tr_2"})");  // rk_2, its Out mo_2
    // What an effect hears: a saw, -6 dB.
    f.run("rack.patch", R"({"rack": "rk_2", "modules": {"osc": {"type": "vco"},
        "amp": {"type": "vca", "params": {"level": -6}}},
        "cables": [["osc.saw", "amp.in"], ["amp.out", "mo_2.l"]]})");
    // What an instrument hears: a line, then a chord.
    f.run("item.add", R"({"track": "tr_1", "position": 0, "notation":
        "voice v; 1: C3:q E3:q G3:q C4:q | [C3 E3 G3 B3]:w | A2:e C3:e E3:e A3:e G3:h"})");
    json::Value listed = f.run("rack.preset.list", R"({"factory": true})");
    bool found = false;
    for (const auto& row : listed["presets"].asArray())
        found = found || row.find("name")->asString() == name;
    CHECK(found);
    json::Value args = json::Value::object();
    args["rack"] = "rk_1";
    args["name"] = name;
    auto loaded = f.bus->execute({"rack.preset.load", args, origin});
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
    CHECK(loaded->find("warnings") == nullptr);
    CHECK(loaded->find("installed") == nullptr);
    for (const auto& id : (*loaded)["modules"].asArray()) {
        const model::RackModule* m = f.app->head().modules.find(*model::Id::parse(id.asString()));
        REQUIRE(m != nullptr);
        if (m->type == "com.suppleaudio.rack.in")
            f.run("route.add", R"({"from": "tr_2", "to": ")" + id.asString() + R"("})");
    }
    // In a fresh rack the patch keeps its layout, its Out (the rack's own) included.
    {
        const auto& entries = doc->find("modules")->asArray();
        double minX = 1e9, minY = 1e9, rackX = 1e9, rackY = 1e9;
        for (const auto& m : entries) {
            minX = std::min(minX, m.find("x")->asDouble());
            minY = std::min(minY, m.find("y")->asDouble());
        }
        for (const auto& m : f.app->head().modules)
            if (m.value->rack == model::Id{model::Kind::Rack, 1}) {
                rackX = std::min(rackX, m.value->x);
                rackY = std::min(rackY, m.value->y);
            }
        for (const auto& m : entries)
            if (m.find("type")->asString() == "com.suppleaudio.rack.out") {
                const model::RackModule* out =
                    f.app->head().modules.find(model::Id{model::Kind::Module, 1});
                REQUIRE(out != nullptr);
                CHECK(out->x - rackX == doctest::Approx(m.find("x")->asDouble() - minX));
                CHECK(out->y - rackY == doctest::Approx(m.find("y")->asDouble() - minY));
                break;
            }
    }
    json::Value report = f.run("analyze.track", R"({"track": "tr_1", "end_beat": 16})");
    const double peak = report["loudness"]["sample_peak_dbfs"].asDouble();
    CHECK_MESSAGE(peak <= kCeilingDbfs, name << " peaks at " << peak << " dBFS");
    CHECK_MESSAGE(peak > kSilentDbfs, name << " is silent (" << peak << " dBFS)");
}

}  // namespace
