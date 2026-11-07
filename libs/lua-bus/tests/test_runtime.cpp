// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The runtime: named scripts that keep their state, bus events reaching their
// subscriptions, and `script.run` over the command bus.
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "supple/lua/runtime.h"
#include "toy_host.h"

using namespace supple;
using namespace supple::lua;
using supple::lua::tests::ToyHost;
using supple::lua::tests::toyRegistry;

namespace {
json::Value obj(std::string_view text) { return json::parse(text).value(); }
cmd::Origin visitor{cmd::Origin::Type::Mcp, "agent", ""};

struct Scene {
    cmd::Registry registry = toyRegistry();
    ToyHost host;
    cmd::Dispatcher dispatcher;
    Runtime runtime;

    Scene() : dispatcher(registry, host), runtime(&registry) {
        runtime.setCommands(registry, dispatcher);
        host.useScripts(runtime);
    }
    Result<json::Value> execute(std::string_view command, const char* args = "{}") {
        return dispatcher.execute({std::string(command), obj(args), visitor});
    }
};
}  // namespace

TEST_CASE("the registry's examples pass, script.run among them") {
    auto registry = toyRegistry();
    auto result =
        cmd::runExamples(registry, [](std::string_view fixture) -> std::unique_ptr<cmd::Host> {
            if (fixture != "empty" && fixture != "job_running") return nullptr;
            auto host = std::make_unique<ToyHost>();
            host->attachOwnScripts();
            return host;
        });
    REQUIRE_MESSAGE(result.ok(), result.error().toString());
}

TEST_CASE("script.run runs Lua on the bus and records lua:<name>") {
    Scene scene;
    auto out = scene.execute("script.run", R"({
        "name": "tidier",
        "code": "q.toy.add{value = 4} print('added') return {count = q.toy.get().count}",
        "args": {"unused": true}
    })");
    REQUIRE(out);
    CHECK((*out)["result"]["count"] == 4);
    CHECK((*out)["script"] == "tidier");
    CHECK((*out)["log"].size() == 1);
    CHECK((*out)["log"].at(0) == "added");
    CHECK((*out)["log_dropped"] == 0);
    CHECK((*out)["instructions"].isInt());
    CHECK((*out)["instructions"].asInt() > 0);
    CHECK(scene.host.toy.origin == "lua:tidier");
}

TEST_CASE("script.run's name defaults to the caller's client") {
    Scene scene;
    auto out = scene.execute("script.run", R"({"code": "q.toy.add{value = 1} return {}"})");
    REQUIRE(out);
    CHECK((*out)["script"] == "agent");
    CHECK(scene.host.toy.origin == "lua:agent");
}

TEST_CASE("script.run reports the sandbox's errors as the bus's own") {
    Scene scene;
    auto syntax = scene.execute("script.run", R"({"code": "return 1 +"})");
    REQUIRE(!syntax);
    CHECK(syntax.error().code == "script.syntax");

    auto budget = scene.execute("script.run", R"({"code": "while true do end", "budget_ms": 40})");
    REQUIRE_MESSAGE(!budget, budget.error().toString());
    CHECK(budget.error().code == "script.budget");

    auto bad = scene.execute("script.run", R"({"code": "q.toy.add{value = 1}", "budget_ms": 0})");
    REQUIRE(!bad);
    CHECK(bad.error().code == "command.invalid_args");  // budget_ms is out of range

    auto missing = scene.execute("script.run", R"({"budget_ms": 10})");
    REQUIRE(!missing);
    CHECK(missing.error().code == "command.invalid_args");
}

TEST_CASE("a named script keeps its state and its subscriptions") {
    Scene scene;
    REQUIRE(scene.runtime.run(
        "keeper", "counter = 0\nq.on('session.changed', function() counter = counter + 1 end)",
        -1.0));
    REQUIRE(scene.runtime.run("keeper", "counter = counter + 10", -1.0));
    CHECK(scene.runtime.size() == 1);
    REQUIRE(scene.execute("toy.add", R"({"value": 1})"));
    auto out = scene.runtime.run("keeper", "return {counter = counter}", -1.0);
    REQUIRE(out);
    CHECK(out->result["counter"] == 11);

    // A second name is a second state.
    REQUIRE(scene.runtime.run("other", "counter = 99", -1.0));
    CHECK(scene.runtime.size() == 2);
    auto fresh = scene.runtime.run("keeper", "return {counter = counter}", -1.0);
    REQUIRE(fresh);
    CHECK(fresh->result["counter"] == 11);
}

TEST_CASE("a handler runs after the command that emitted the event") {
    Scene scene;
    REQUIRE(scene.runtime.run("watcher", R"(
        seen = {}
        q.on("session.changed", function(payload)
            seen[#seen + 1] = {event = payload.count, now = q.toy.get().count}
        end)
    )",
                              -1.0));
    REQUIRE(scene.execute("toy.add", R"({"value": 5})"));
    auto out = scene.runtime.run("watcher", "return {seen = seen}", -1.0);
    REQUIRE(out);
    REQUIRE(out->result["seen"].size() == 1);
    const json::Value& observed = out->result["seen"].at(0);
    CHECK(observed.find("event")->asInt() == 5);
    CHECK(observed.find("now")->asInt() == 5);  // the transaction had committed
}

TEST_CASE("events are delivered only after a command finished") {
    Scene scene;
    REQUIRE(scene.runtime.run(
        "watcher", "q.on('session.changed', function() seen = (seen or 0) + 1 end)", -1.0));
    CHECK(scene.runtime.pendingEvents() == 0);
    // An event queued by hand is not delivered until the host says so.
    scene.runtime.event("session.changed", obj(R"({"count": 0})"));
    CHECK(scene.runtime.pendingEvents() == 1);
    scene.runtime.deliver();
    CHECK(scene.runtime.pendingEvents() == 0);
    auto out = scene.runtime.run("watcher", "return {seen = seen}", -1.0);
    REQUIRE(out);
    CHECK(out->result["seen"] == 1);
}

TEST_CASE("a handler that edits the session cannot run away") {
    Scene scene;
    REQUIRE(scene.runtime.run(
        "loop", "q.on('session.changed', function() q.toy.add{value = 1} end)", -1.0));
    REQUIRE(scene.execute("toy.add", R"({"value": 1})"));
    CHECK(scene.runtime.droppedEvents() > 0);
    CHECK(scene.host.toy.count < 50);  // it stopped instead of looping forever
}

TEST_CASE("a handler run by the script's own command is on the script's clock") {
    Scene scene;
    // Commands do not spend the budget, but the handlers they trigger in the
    // running script do: a runaway one still stops the run.
    auto out = scene.runtime.run(
        "spin", "q.on('session.changed', function() while true do end end)\nq.toy.add{value = 1}",
        100.0);
    REQUIRE_FALSE(out);
    CHECK(out.error().code == "script.budget");
}

TEST_CASE("clear() forgets every script") {
    Scene scene;
    REQUIRE(scene.runtime.run("one", "return 1", -1.0));
    CHECK(scene.runtime.find("one") != nullptr);
    scene.runtime.clear();
    CHECK(scene.runtime.size() == 0);
    CHECK(scene.runtime.find("one") == nullptr);
}

TEST_CASE("a host that attaches its commands later rebinds its scripts") {
    // The engine and the sandbox create their runtime before the dispatcher
    // exists; scripts run in between (the registry's examples) must still
    // reach the commands once it does.
    Runtime runtime;
    auto early = runtime.run("early", "return {sum = 1 + 1}", -1.0);
    REQUIRE(early);
    CHECK(early->result["sum"] == 2);

    cmd::Registry registry = toyRegistry();
    ToyHost host;
    cmd::Dispatcher dispatcher(registry, host);
    runtime.setCommands(registry, dispatcher);
    auto later =
        runtime.run("early", "q.toy.add{value = 3} return {count = q.toy.get().count}", -1.0);
    REQUIRE(later);
    CHECK(later->result["count"] == 3);
}

TEST_CASE("a script cannot run itself from a handler") {
    Scene scene;
    // The script's own edit emits the event that reaches its handler while it
    // is still running: the nested run of the same name must be refused.
    REQUIRE(scene.runtime.run("self", R"(
        q.on('session.changed', function()
            q.run('script.run', {name = 'self', code = 'return 1'})
        end)
        q.toy.add{value = 1}
    )",
                              -1.0));
    CHECK(scene.host.toy.count == 1);
    const auto log = scene.runtime.find("self")->log();
    REQUIRE(!log.empty());
    CHECK(log.back().find("script.busy") != std::string::npos);
    CHECK(scene.runtime.droppedEvents() == 0);
}

TEST_CASE("delivering a handler preserves the running script's instruction hook") {
    Scene scene;
    auto out = scene.runtime.run("budget", R"(
        q.on('session.changed', function() seen = true end)
        q.toy.add{value = 1}
        local sum = 0
        for i = 1, 10000 do sum = sum + i end
        return {seen = seen, sum = sum}
    )",
                                 -1.0);
    REQUIRE(out);
    CHECK(out->result["seen"] == true);
    CHECK(out->result["sum"] == 50005000);
    CHECK(out->instructions >= scene.runtime.find("budget")->options().hookCount);
}

TEST_CASE("Lua receives command warnings directly and inside a batch") {
    auto registry = toyRegistry();
    cmd::Command<cmd::NoArgs, json::Value> warning;
    warning.name = "toy.warning";
    warning.exposure = cmd::Exposure::Internal;
    warning.summary = "Return a warning to a script.";
    warning.args = schema::object<cmd::NoArgs>("no_args").build();
    warning.result = schema::any();
    warning.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<json::Value> {
        ctx.warn("toy.warning", "a diagnostic", "/source");
        return obj(R"({"ok":true})");
    };
    registry.add(std::move(warning));
    ToyHost host;
    cmd::Dispatcher bus(registry, host);
    Runtime runtime(&registry);
    runtime.setCommands(registry, bus);
    host.useScripts(runtime);
    auto result = bus.execute(
        {"script.run",
         obj(R"({"name":"warnings","code":"local a=q.toy.warning(); local b; q.batch('warnings',function() b=q.run('toy.warning',{}) end); assert(q.toy.get().warnings==nil); return {direct=a,batch=b}"})"),
         visitor});
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    const auto& value = (*result)["result"];
    const auto* direct = value.find("direct");
    const auto* batch = value.find("batch");
    REQUIRE(direct);
    REQUIRE(batch);
    REQUIRE(direct->find("warnings")->size() == 1);
    REQUIRE(batch->find("warnings")->size() == 1);
    CHECK(direct->find("warnings")->at(0).find("code")->asString() == "toy.warning");
    CHECK(batch->find("warnings")->at(0).find("path")->asString() == "/source");
}
