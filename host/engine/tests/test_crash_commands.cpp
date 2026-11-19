// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/files.h>
#include <supple/diagnostics/reports.h>
#include <supple/engine/app.h>
#include <filesystem>
using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;
namespace {
struct Root {
    fs::path path = fs::temp_directory_path() / "supple-crash-commands";
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};
const cmd::Origin kAgent{cmd::Origin::Type::Mcp, "test-client", "model"};
json::Value j(std::string_view text) { return json::parse(text).value(); }
const char* kOne = "00000000000000000000000000000001";
const char* kTwo = "00000000000000000000000000000002";
struct Fixture {
    Root root;
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    Fixture() {
        registerEngineCommands(registry);
        AppOptions o;
        o.workdir = root.path;
        o.pluginPath = {root.path / "no-plugins"};  // never the machine's plug-ins
        auto made = EngineApp::create(o, {.console9 = false});
        REQUIRE(made);
        app = std::move(*made);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    Result<json::Value> call(std::string command, std::string_view args) {
        return bus->execute({std::move(command), j(args), kAgent});
    }
};
}  // namespace
TEST_CASE("crash commands read the app's own diagnostics folder, never the user's by default") {
    Fixture f;
    CHECK(f.app->diagnosticsDir() == f.root.path / "diagnostics");
    auto list = f.call("crash.list", "{}");
    REQUIRE(list);
    CHECK((*list)["reports_total"].asInt() == 0);
    CHECK((*list)["live_runs"].asInt() == 0);
}
TEST_CASE("an agent lists, reads, acknowledges and deletes crash reports") {
    Fixture f;
    const auto dir = f.app->diagnosticsDir();
    REQUIRE(diagnostics::writeSampleReport(kOne, dir));
    REQUIRE(diagnostics::writeSampleReport(kTwo, dir));
    auto list = f.call("crash.list", "{}");
    REQUIRE(list);
    CHECK((*list)["reports_total"].asInt() == 2);
    REQUIRE((*list)["reports"].size() == 2);
    const auto& first = (*list)["reports"].asArray()[0];
    CHECK((*first.find("kind")).asString() == "unexpected_exit");
    CHECK((*first.find("role")).asString() == "fixture");
    CHECK_FALSE((*first.find("acknowledged")).asBool());
    CHECK((*first.find("plugin")).isNull());
    CHECK((*first.find("written_ms")).asInt() > 1'700'000'000'000LL);
    // limit caps the list, the total still counts them all.
    auto capped = f.call("crash.list", R"({"limit":1})");
    REQUIRE(capped);
    CHECK((*capped)["reports"].size() == 1);
    CHECK((*capped)["reports_total"].asInt() == 2);
    // One report in full.
    auto read = f.call("crash.read", std::string(R"({"incident":")") + kOne + "\"}");
    REQUIRE(read);
    CHECK((*read)["report"]["schema"].asString() == "supple.crash-report/1.0");
    CHECK((*read)["report"]["incident"].asString() == kOne);
    // Acknowledge, twice.
    REQUIRE(f.call("crash.acknowledge", std::string(R"({"incident":")") + kOne + "\"}"));
    REQUIRE(f.call("crash.acknowledge", std::string(R"({"incident":")") + kOne + "\"}"));
    list = f.call("crash.list", "{}");
    REQUIRE(list);
    int acknowledged = 0;
    for (const auto& r : (*list)["reports"].asArray())
        acknowledged += (*r.find("acknowledged")).asBool();
    CHECK(acknowledged == 1);
    // Delete one, then the rest.
    REQUIRE(f.call("crash.delete", std::string(R"({"incident":")") + kOne + "\"}"));
    CHECK_FALSE(fs::exists(dir / (std::string(kOne) + ".json")));
    CHECK_FALSE(fs::exists(dir / (std::string(kOne) + ".ack")));
    auto all = f.call("crash.clear", "{}");
    REQUIRE(all);
    CHECK((*all)["deleted"].asInt() == 1);
    list = f.call("crash.list", "{}");
    REQUIRE(list);
    CHECK((*list)["reports_total"].asInt() == 0);
}
TEST_CASE("crash commands say what is wrong and how to fix it") {
    Fixture f;
    const char* ids[] = {kOne, "not-an-incident", "../00000000000000000000000000000001"};
    for (const char* command : {"crash.read", "crash.acknowledge", "crash.delete"}) {
        // No such report: a code of this namespace, the argument named, a hint that says what to
        // do.
        auto missing = f.call(command, std::string(R"({"incident":")") + ids[0] + "\"}");
        REQUIRE_FALSE(missing);
        CHECK(missing.error().code == "crash.no_report");
        CHECK(missing.error().path == "/args/incident");
        CHECK(missing.error().hint.find("crash.list") != std::string::npos);
        // Not an incident id at all: refused by the argument's own schema before any file is
        // touched.
        for (int i = 1; i < 3; ++i)
            CHECK_FALSE(f.call(command, std::string(R"({"incident":")") + ids[i] + "\"}"));
    }
}
