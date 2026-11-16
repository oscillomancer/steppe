// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/engine/app.h>
#include <supple/model/schemas.h>
#include <supple/rpc/protocol.h>
#include <filesystem>
#include <iostream>
#include "../../../tests/file_platform.h"
#include "../../../tests/fuzz/command_inputs.h"

using namespace supple;

TEST_CASE("registered engine argument mutations fail structurally without changing the project") {
    constexpr std::uint64_t seed = 0xc017a01;
    bool recording = false, anticipative = false;
    SUBCASE("stopped") {}
    SUBCASE("recording, in-callback executor") { recording = true; }
    SUBCASE("recording, anticipative executor") { recording = anticipative = true; }
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-command-inputs-" + std::to_string(test::processId()));
    REQUIRE(std::filesystem::create_directory(dir));
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    engine::AppOptions options;
    options.workdir = dir;
    // No discovery of installed plug-ins, no user library writes or device actions.
    options.pluginPath = {dir / "no-plugins"};
    options.chainDir = dir / "chains";
    options.presetDir = dir / "presets";
    options.templateDir = dir / "track-templates";
    options.projectTemplateDir = dir / "templates";
    engine::DummyBackend backend({.mode = engine::DummyBackend::Mode::Manual});
    auto app = engine::EngineApp::create(options, {.console9 = false});
    REQUIRE(app);
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    if (recording) {
        const cmd::Origin origin{cmd::Origin::Type::Test, "schema-fuzz", ""};
        REQUIRE(bus.execute({"track.add", json::Value::object(), origin}));
        REQUIRE(bus.execute({"track.set",
                             *json::parse(R"({"track":"tr_1","armed":true,"record_input":[1]})"),
                             origin}));
        REQUIRE((*app)->startEngine(backend, {.blockSize = 128, .inputs = 1},
                                    {.anticipative = anticipative, .workers = 0}));
        REQUIRE((*app)->transport("record"));
        backend.run(2);
        REQUIRE((*app)->save(dir / "project.json"));
    }
    const auto original = model::serializeProject((*app)->head());
    const auto version = (*app)->head().version;
    test::CommandInputs inputs(seed);
    std::size_t generated = 0, rejected = 0, accepted = 0;
    for (const auto* command : registry.all()) {
        std::size_t commandRejected = 0;
        inputs.generate(command->argsSchema, [&](const json::Value& args, const std::string& path) {
            ++generated;
            INFO("seed=" << seed << " case=" << generated << " command=" << command->name << "@"
                         << command->version << " target=" << path
                         << " args=" << json::writeCompact(args));
            if (command->validate(args)) {
                REQUIRE_MESSAGE(inputs.allows(command->argsSchema, args),
                                "decoder accepted an input outside its exported contract");
                ++accepted;
                return;
            }
            ++rejected;
            ++commandRejected;
            auto result = bus.execute({command->name + "@" + std::to_string(command->version),
                                       args,
                                       {cmd::Origin::Type::Test, "schema-fuzz", ""}});
            REQUIRE_FALSE(result);
            const auto& error = result.error();
            CHECK(error.code == "command.invalid_args");
            CHECK_FALSE(error.message.empty());
            CHECK(error.path == "/args");
            CHECK_FALSE(error.hint.empty());
            CHECK_FALSE(error.details.empty());
            const auto wire = rpc::errorJson(error);
            for (const char* key : {"code", "message", "path", "hint"}) {
                REQUIRE(wire.find(key));
                CHECK(wire.find(key)->isString());
            }
            CHECK((*app)->head().version == version);
            CHECK(model::serializeProject((*app)->head()) == original);
        });
        CHECK_MESSAGE(commandRejected > 0, "no rejected inputs for " << command->name);
        if (recording) {
            backend.run(2);
            if (generated % 7 == 0) REQUIRE((*app)->save(dir / "project.json"));
        }
    }
    std::cout << "command-inputs seed=" << seed << " commands=" << registry.all().size()
              << " generated=" << generated << " rejected=" << rejected
              << " schema-accepted-not-executed=" << accepted << " recording=" << recording
              << " anticipative=" << anticipative << '\n';
    if (recording) {
        REQUIRE((*app)->transport("stop"));
        (*app)->stopEngine();
    }
}
