// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// script.run through the engine's registry. Its own binary because
// ThreadSanitizer cannot follow Lua's longjmp-based error handling ("can't
// find longjmp buf"), so the label tsan-skip keeps it out of that preset while
// engine.tests keeps its TSan coverage.
#include <doctest/doctest.h>
#include "../../../tests/file_platform.h"

#include <filesystem>

#include <supple/engine/app.h>

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
struct Workdir {
    fs::path path = fs::temp_directory_path() /
                    ("supple-engine-scripts-" + std::to_string(supple::test::processId()));
    Workdir() {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Workdir() { fs::remove_all(path); }
};

std::unique_ptr<EngineApp> makeApp(const fs::path& path) {
    AppOptions options;
    options.workdir = path;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto app = EngineApp::create(options);
    REQUIRE(app);
    return std::move(*app);
}

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "scripts", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }
json::Value run(cmd::Dispatcher& bus, std::string name, std::string_view args) {
    auto result = bus.execute({std::move(name), j(args), kOrigin});
    REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
    return *result;
}
}  // namespace

TEST_CASE("a Lua script edits through the bus: q.batch is one undo step") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    app->attachCommands(bus);

    auto scripted = run(bus, "script.run", R"json({
        "name": "builder",
        "code": "local ids = {}\nids[#ids + 1] = q.track.add{name = 'Lead'}.track\nq.batch('drum bus', function()\n  ids[#ids + 1] = q.track.add{name = 'Drums'}.track\n  ids[#ids + 1] = q.track.add{name = 'Bass'}.track\nend)\nreturn {tracks = ids}"
    })json");
    CHECK(scripted["script"] == "builder");
    CHECK(scripted["result"]["tracks"].size() == 3);
    CHECK(app->head().tracks.size() == 4);  // the master and the three the script made

    // The batch is exactly one undo step: undoing once takes both its tracks.
    run(bus, "history.undo", "{}");
    CHECK(app->head().tracks.size() == 2);
    run(bus, "history.redo", "{}");
    CHECK(app->head().tracks.size() == 4);

    // A command error inside a script carries the bus's own code.
    auto bad =
        bus.execute({"script.run",
                     j(R"({"code": "q.track.set{track = 'tr_1', volume_db = 'loud'}"})"), kOrigin});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "command.invalid_args");
}

TEST_CASE("a script's subscription sees a version after the edit that made it") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    app->attachCommands(bus);

    run(bus, "script.run", R"json({
        "name": "watcher",
        "code": "versions = {}\nq.on('session.changed', function(payload)\n  versions[#versions + 1] = payload.version\nend)"
    })json");
    run(bus, "track.add", R"({"name": "Watched"})");
    auto seen = run(bus, "script.run",
                    R"json({"name": "watcher", "code": "return {versions = versions}"})json");
    REQUIRE(seen["result"]["versions"].size() >= 1);
    CHECK(seen["result"]["versions"].at(0).asInt() > 0);
}

TEST_CASE("caught command failures restore only their invocation savepoint") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Command<cmd::NoArgs, cmd::NoArgs> fail;
    fail.name = "test.partial";
    fail.kind = cmd::Kind::Edit;
    fail.exposure = cmd::Exposure::Internal;
    fail.args = schema::object<cmd::NoArgs>("test_args").build();
    fail.result = schema::object<cmd::NoArgs>("test_result").build();
    fail.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<cmd::NoArgs> {
        auto& txn = ctx.service<EngineApp>().txn();
        txn.modify<model::Track>(model::kMaster, [](auto& tr) { tr.name = "partial"; });
        auto project = txn.project();
        project.name = "partial";
        txn.setProject(std::move(project));
        ctx.service<EngineApp>().emit("test.failed_event", json::Value::object());
        return Error("test.failed", "deliberate failure after mutations");
    };
    registry.add(std::move(fail));
    cmd::Dispatcher bus(registry, *app);
    app->attachCommands(bus);
    run(bus, "script.run",
        R"lua({"name":"watcher","code":"seen=0; q.on('test.failed_event',function() seen=seen+1 end)"})lua");
    const auto name = app->head().project->name;
    auto out = run(
        bus, "script.run",
        R"lua({"code":"q.batch('caught', function() q.track.add{name='kept'}; local ok,err=pcall(q.test.partial,{}); assert(not ok and err.code=='test.failed'); q.track.add{name='also kept'} end)"})lua");
    CHECK(out.isObject());
    auto seen = run(bus, "script.run", R"lua({"name":"watcher","code":"return seen"})lua");
    INFO(json::writeCompact(seen));
    CHECK(seen["result"] == 0);
    CHECK(app->head().tracks.size() == 3);
    CHECK(app->head().tracks.find(model::kMaster)->name == "Master");
    CHECK(app->head().project->name == name);
    CHECK(app->versions().history().front()->txn.invocations.size() == 2);
    run(bus, "history.undo", "{}");
    CHECK(app->head().tracks.size() == 1);
}

TEST_CASE("replacing a project inside Lua clears scripts after the outer call returns") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    app->attachCommands(bus);
    run(bus, "script.run", R"lua({"name":"old","code":"saved=42"})lua");
    run(bus, "script.run",
        R"lua({"name":"replace","code":"q.project.new{name='Fresh'}; return 7"})lua");
    CHECK(app->head().project->name == "Fresh");
    auto reset = run(bus, "script.run", R"lua({"name":"old","code":"return saved==nil"})lua");
    CHECK(reset["result"] == true);
}
