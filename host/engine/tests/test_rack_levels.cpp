// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>

#include "supple/engine/app.h"
#include "supple/rack/instance.h"

using namespace supple;
using namespace supple::engine;

TEST_CASE("rack levels: an unwatched oscillator and silent outputs in both executors" *
          doctest::description("rt")) {
    for (const auto& options : {EngineOptions{.anticipative = false, .workers = 0},
                                EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        auto dir = std::filesystem::temp_directory_path() /
                   ("rack-levels-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});  // outlives the app
        {
            AppOptions appOptions;
            appOptions.workdir = dir;
            auto made = EngineApp::create(appOptions);
            REQUIRE(made);
            auto app = std::move(*made);
            cmd::Registry registry;
            registerEngineCommands(registry);
            cmd::Dispatcher bus(registry, *app);
            auto run = [&](std::string name, std::string_view args = "{}") {
                auto result = bus.execute({std::move(name),
                                           json::parse(args).value(),
                                           {cmd::Origin::Type::Test, "levels", ""}});
                REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
                return *result;
            };
            run("track.add", R"({"name":"Levels"})");
            run("rack.add", R"({"track":"tr_1"})");  // mo_1 out
            run("module.add", R"({"rack":"rk_1","type":"vco","params":{"coarse":-48}})");  // mo_2
            run("module.add",
                R"({"rack":"rk_1","type":"vca","params":{"level":-6.020599913279624}})");  // mo_3
            run("module.add", R"({"rack":"rk_1","type":"vca"})");  // mo_4: unconnected and silent
            run("cable.add", R"({"from":"mo_2.sub","to":"mo_3.in"})");
            run("cable.add", R"({"from":"mo_3.out","to":"mo_1.l"})");
            const model::Id vco{model::Kind::Module, 2}, vca{model::Kind::Module, 3},
                silent{model::Kind::Module, 4};
            CHECK(app->moduleOutputLevel(vco, "sub") == 0.0f);
            REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 37}, options));
            CHECK_FALSE(app->moduleInstance(vco)->watched());
            CHECK(app->moduleOutputLevel(vco, "sub") == 0.0f);
            app->engine()->play();
            for (int i = 0; i < 8; ++i) {
                if (options.anticipative) REQUIRE(app->engine()->awaitRendered(1024));
                backend.run(1);
            }
            CHECK(app->moduleOutputLevel(vco, "sub") == doctest::Approx(1.0));
            CHECK(app->moduleOutputLevel(vca, "out") == doctest::Approx(0.5));
            CHECK(app->moduleOutputLevel(silent, "out") == 0.0f);
            CHECK(app->moduleOutputLevel(vco, "no_such_output") == 0.0f);
            CHECK(app->moduleOutputLevel({model::Kind::Module, 999}, "out") == 0.0f);
            auto levels = run("rack.levels", R"({"rack":"rk_1"})");
            CHECK(levels["modules"]["mo_2"]["sub"].asDouble() == doctest::Approx(1.0));
            CHECK(levels["modules"]["mo_3"]["out"].asDouble() == doctest::Approx(0.5));
            CHECK(levels["modules"]["mo_4"]["out"].asDouble() == 0.0);
            // A sounding output remains measured even without a cable or a watch.
            CHECK(app->moduleOutputLevel(vco, "pulse") > 0.9f);
            if (options.workers == 2) {
                app->engine()->stop();  // the app can still own a stopped Engine
            } else {
                app->engine()->pause();
                app->engine()->awaitTransport();
                backend.run(1);
            }
            CHECK(app->moduleOutputLevel(vco, "sub") == 0.0f);
            levels = run("rack.levels", R"({"rack":"rk_1"})");
            CHECK(levels["modules"]["mo_3"]["out"].asDouble() == 0.0);
            app->stopEngine();
            CHECK(app->moduleOutputLevel(vco, "sub") == 0.0f);
            auto unknown = bus.execute({"rack.levels",
                                        json::parse(R"({"rack":"rk_999"})").value(),
                                        {cmd::Origin::Type::Test, "levels", ""}});
            REQUIRE_FALSE(unknown);
            CHECK(unknown.error().code == "rack.unknown");
            CHECK(unknown.error().path == "/args/rack");
            CHECK_FALSE(unknown.error().hint.empty());
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}
