// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The factory rack patches (modules/patches, docs/specs/rack.md §7.1): each
// is a canonical rack preset of stock modules that loads into a fresh rack
// without a warning and, played as a user would play it, sounds without
// going over full scale. An instrument patch hears notes from its track, an
// effect a saw sent into its In, and a self-playing one the song running.
#include <doctest/doctest.h>

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

}  // namespace

TEST_CASE("factory rack patches: canonical, stock only, and named once each") {
    const auto& patches = rack::factoryPatches();
    REQUIRE_FALSE(patches.empty());
    std::set<std::string> names;
    for (const rack::FactoryPatch& p : patches) {
        CAPTURE(p.file);
        auto doc = json::parse(p.text);
        REQUIRE(doc);
        // As rack.preset.save writes it: identical content, identical bytes.
        CHECK(json::writeCanonical(*doc) + "\n" == std::string(p.text));
        CHECK((*doc)["schema"].asString() == "supple.rack-preset/1.0");
        const std::string name = (*doc)["name"].asString();
        CHECK(names.insert(name).second);
        const json::Value* description = doc->find("description");
        CHECK((description && description->isString() && !description->asString().empty()));
        CHECK(doc->find("sources") == nullptr);
        bool out = false;
        for (const auto& m : (*doc)["modules"].asArray()) {
            const std::string type = m.find("type")->asString();
            CHECK_MESSAGE(type.rfind("com.suppleaudio.rack.", 0) == 0, type);
            CHECK(type != "com.suppleaudio.rack.plugin");
            out = out || type == "com.suppleaudio.rack.out";
        }
        CHECK_MESSAGE(out, name << " has no Out");
    }
}

TEST_CASE("factory rack patches: each loads and plays under full scale") {
    for (const rack::FactoryPatch& p : rack::factoryPatches()) {
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
            const model::RackModule* m =
                f.app->head().modules.find(*model::Id::parse(id.asString()));
            REQUIRE(m != nullptr);
            if (m->type == "com.suppleaudio.rack.in")
                f.run("route.add", R"({"from": "tr_2", "to": ")" + id.asString() + R"("})");
        }
        json::Value report = f.run("analyze.track", R"({"track": "tr_1", "end_beat": 16})");
        const double peak = report["loudness"]["sample_peak_dbfs"].asDouble();
        CHECK_MESSAGE(peak <= kCeilingDbfs, name << " peaks at " << peak << " dBFS");
        CHECK_MESSAGE(peak > kSilentDbfs, name << " is silent (" << peak << " dBFS)");
    }
}

TEST_CASE("factory rack patches: a saved preset of the name wins, and they are not deleted") {
    const auto& patches = rack::factoryPatches();
    REQUIRE_FALSE(patches.empty());
    const std::string name = json::parse(patches.front().text)->find("name")->asString();
    Fixture f;
    f.run("track.add", R"({"name": "Patch"})");
    f.run("rack.add", R"({"track": "tr_1"})");
    json::Value args = json::Value::object();
    args["name"] = name;
    auto refused = f.bus->execute({"rack.preset.delete", args, origin});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "rack.preset_factory");
    // Saved under the factory patch's name, the user's version is the one listed and loaded.
    f.run("module.add", R"({"rack": "rk_1", "type": "vca", "label": "Mine"})");
    args["rack"] = "rk_1";
    f.run("rack.preset.save", json::writeCanonical(args));
    json::Value listed = f.run("rack.preset.list", R"({})");
    int rows = 0;
    for (const auto& row : listed["presets"].asArray())
        if (row.find("name")->asString() == name) {
            ++rows;
            CHECK(row.find("factory") == nullptr);
            CHECK(row.find("path") != nullptr);
        }
    CHECK(rows == 1);
    CHECK(f.run("rack.preset.list", R"({"factory": true})")["presets"].asArray().size() ==
          patches.size() - 1);
    // Deleting the user's version brings the factory patch back.
    json::Value del = json::Value::object();
    del["name"] = name;
    f.run("rack.preset.delete", json::writeCanonical(del));
    CHECK(f.run("rack.preset.list", R"({"factory": true})")["presets"].asArray().size() ==
          patches.size());
}
