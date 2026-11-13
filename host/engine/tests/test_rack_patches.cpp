// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The factory rack patches (modules/patches, docs/specs/rack.md §7.1): each
// is a canonical rack preset of stock modules that loads into a fresh rack
// without a warning and, played as a user would play it, sounds without
// going over full scale. An instrument patch hears notes from its track, an
// effect a saw sent into its In, and a self-playing one the song running.
#include <doctest/doctest.h>

#include "rack_patches_support.h"

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

TEST_CASE("factory rack patches: each of the first half loads and plays under full scale") {
    const auto& patches = rack::factoryPatches();
    const std::size_t mid = patches.size() / 2;
    for (std::size_t i = 0; i < mid; ++i) playsUnderFullScale(patches[i]);
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
