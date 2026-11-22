// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The scripting sandbox, the generated bindings and the JSON ⇄ Lua round trip
// (ADR-0006). Model thread; no audio.
#include <doctest/doctest.h>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "supple/lua/script.h"
#include "toy_host.h"

using namespace supple;
using namespace supple::lua;
using supple::lua::tests::ToyHost;
using supple::lua::tests::toyRegistry;

namespace {

json::Value obj(std::string_view text) { return json::parse(text).value(); }

// A registry, a host, a dispatcher and one script, wired the way a host does.
struct Scene {
    cmd::Registry registry;
    ToyHost host;
    cmd::Dispatcher dispatcher;
    Script script;

    explicit Scene(ScriptOptions options = {}, bool withScripts = false)
        : registry(toyRegistry(withScripts)),
          dispatcher(registry, host),
          script(&registry, &dispatcher, std::move(options)) {}

    Result<ScriptResult> run(std::string_view source, double budgetMs = -1.0) {
        return script.run(source, budgetMs);
    }
    Result<ScriptResult> runWith(std::string_view source, const json::Value& args) {
        return script.run(source, -1.0, args);
    }
};

// Every required member of toy.echo, as a Lua literal.
constexpr const char* kEcho = R"({text = "t", n = 3, freq = 2, flag = false, mode = "off", )"
                              R"(ints = {1}, points = {}, gains = {}, point = {x = 0}, )"
                              R"(anything = false, move = {kind = "point", x = 0}, ref = "tr_1"})";

}  // namespace

TEST_CASE("the fuzz toy counter refuses overflow without changing its value") {
    Scene scene;
    scene.host.toy.count = std::numeric_limits<std::int64_t>::max();
    auto high = scene.run("q.toy.add{value=1}");
    REQUIRE_FALSE(high);
    CHECK(high.error().code == "toy.overflow");
    CHECK(scene.host.toy.count == std::numeric_limits<std::int64_t>::max());
    scene.host.toy.count = std::numeric_limits<std::int64_t>::min();
    auto low = scene.run("q.toy.add{value=-1}");
    REQUIRE_FALSE(low);
    CHECK(low.error().code == "toy.overflow");
    CHECK(scene.host.toy.count == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("the sandbox has the five libraries and nothing else") {
    Scene scene;
    auto out = scene.run(R"(
        local absent = {}
        for _, name in ipairs({"io", "os", "debug", "package", "require", "dofile", "loadfile"}) do
            absent[name] = _G[name] == nil
        end
        return {
            absent = absent,
            libs = {
                string = type(string.rep) == "function",
                table = type(table.concat) == "function",
                math = type(math.floor) == "function",
                utf8 = type(utf8.len) == "function",
                coroutine = type(coroutine.create) == "function",
            },
            bindings = {add = type(q.toy.add) == "function", run = type(q.run) == "function",
                        batch = type(q.batch) == "function", on = type(q.on) == "function",
                        null = q.null ~= nil},
            name = q.name,
        }
    )");
    REQUIRE(out);
    for (const auto& member : out->result["absent"].asObject())
        CHECK_MESSAGE(member.value.asBool(), "_G." << member.key << " must be absent");
    for (const auto& member : out->result["libs"].asObject()) CHECK(member.value.asBool());
    for (const auto& member : out->result["bindings"].asObject()) CHECK(member.value.asBool());
    CHECK(out->result["name"] == "");
}

TEST_CASE("sandbox load accepts only text and preserves chunk names and environments") {
    Scene scene;
    auto out = scene.run(R"(
        local binary = string.dump(function() return 42 end)
        local f, err = load(binary)
        assert(f == nil and err:find('binary'))
        for _, mode in ipairs({'b', 'bt'}) do
            f, err = load('return 42', 'chunk', mode)
            assert(f == nil and err:find('binary'))
            f, err = load(binary, 'chunk', mode)
            assert(f == nil and err:find('binary'))
            f, err = load(function() return binary end, 'chunk', mode)
            assert(f == nil and err:find('binary'))
        end
        local once = false
        f, err = load(function()
            if once then return nil end
            once = true
            return binary
        end)
        assert(f == nil and err:find('binary'))
        assert(assert(load('return 42'))() == 42)
        assert(assert(load('return value', '=named', 't', {value=17}))() == 17)
        local pieces = {'return ', 'value'}
        local i = 0
        f = assert(load(function() i = i + 1; return pieces[i] end,
                        '=reader', nil, {value=23}))
        assert(f() == 23)
        f, err = load('return +', '=named')
        assert(f == nil and err:find('named:1:', 1, true))
        assert(dofile == nil and loadfile == nil and require == nil and package == nil)
        return true
    )");
    REQUIRE_MESSAGE(out, (out ? "" : out.error().toString()));
    CHECK(out->result.asBool());
}

TEST_CASE("print and q.log go to the script's log") {
    Scene scene;
    auto out = scene.run("print('hello', 42)\nq.log('bye')\nprint({})");
    REQUIRE(out);
    REQUIRE(out->log.size() == 3);
    CHECK(out->log[0] == "hello\t42");
    CHECK(out->log[1] == "bye");
    CHECK(out->log[2].rfind("table: ", 0) == 0);  // Lua uses the platform's %p spelling
    CHECK(out->log[2].size() > std::string("table: ").size());
    CHECK(scene.script.log().size() == 3);  // the log outlives the run
}

TEST_CASE("script logs bound each line and report truncated bytes") {
    Scene scene;
    auto out = scene.run("print(string.rep('x', 65536)); q.log(string.rep('y', 70000), 'end')");
    REQUIRE(out);
    REQUIRE(out->log.size() == 2);
    CHECK(out->log[0] == std::string(65536, 'x'));
    const auto& line = out->log[1];
    CHECK(line.size() == 65536);
    const auto marker = line.find(" ... [truncated ");
    REQUIRE(marker != std::string::npos);
    CHECK(line.substr(marker) == " ... [truncated " + std::to_string(70004 - marker) + " bytes]");
    CHECK(out->logDropped == 0);
}

TEST_CASE("script logs evict oldest lines by bytes and by line count") {
    Scene scene;
    auto out = scene.run(R"(
        local text = string.rep('x', 65533)
        for i=1,17 do print(string.format('%02d', i), text) end
    )");
    REQUIRE(out);
    REQUIRE(out->log.size() == 16);
    CHECK(out->log.front().starts_with("02\t"));
    CHECK(out->log.back().starts_with("17\t"));
    std::size_t bytes = 0;
    for (const auto& line : scene.script.log()) bytes += line.size();
    CHECK(bytes == 1024 * 1024);
    CHECK(out->logDropped == 1);
    out = scene.run("print('next')");
    REQUIRE(out);
    REQUIRE(out->log.size() == 1);
    CHECK(out->log[0] == "next");
    CHECK(out->logDropped == 2);
    CHECK(scene.script.log().front().starts_with("03\t"));

    ScriptOptions options;
    options.logLimitLines = 2;
    Scene shortLog(options);
    auto shortOut = shortLog.run("print('a'); print('b'); print('c')");
    REQUIRE(shortOut);
    REQUIRE(shortOut->log.size() == 2);
    CHECK(shortOut->log[0] == "b");
    CHECK(shortOut->log[1] == "c");
    CHECK(shortOut->logDropped == 1);
}

TEST_CASE("the instruction budget stops a runaway script") {
    ScriptOptions options;
    options.instructionLimit = 1000;
    Scene scene(options);
    auto out = scene.run("while true do pcall(function() while true do end end) end", 10000.0);
    REQUIRE(!out);
    CHECK(out.error().code == "script.budget");
    CHECK(out.error().hint.find("budget_ms") != std::string::npos);
}

TEST_CASE("time spent inside commands does not count against the script's budget") {
    using supple::lua::tests::AddArgs;
    using supple::lua::tests::CountResult;
    // A command slower than the whole budget, called three times: only the
    // script's own instructions are on its clock.
    cmd::Registry registry = toyRegistry(false);
    cmd::Command<AddArgs, CountResult> slow;
    slow.name = "toy.slow";
    slow.exposure = cmd::Exposure::Internal;
    slow.summary = "Wait 60 ms, then read the toy counter.";
    slow.description = "A slow command for the budget test.";
    slow.args = schema::object<AddArgs>("slow_args")
                    .field("value", &AddArgs::value, schema::integer())
                    .build();
    slow.result = schema::object<CountResult>("count_result")
                      .field("count", &CountResult::count, schema::integer())
                      .build();
    slow.run = [](const AddArgs&, cmd::Context&) -> Result<CountResult> {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        return CountResult{0};
    };
    registry.add(std::move(slow));
    ToyHost host;
    cmd::Dispatcher dispatcher(registry, host);
    Script script(&registry, &dispatcher);
    auto out = script.run("for i = 1, 3 do q.run('toy.slow', {value = 0}) end", 100.0);
    CHECK_MESSAGE(static_cast<bool>(out), (out ? std::string() : out.error().toString()));
    // The clock still runs between the calls.
    auto runaway = script.run("q.run('toy.slow', {value = 0}) while true do end", 100.0);
    REQUIRE_FALSE(runaway);
    CHECK(runaway.error().code == "script.budget");
}

TEST_CASE("the memory cap stops a script that allocates too much") {
    ScriptOptions options;
    options.memoryLimitBytes = 192 * 1024;
    Scene scene(options);
    auto out =
        scene.run("local t = {} for i = 1, 1000000 do t[i] = 'xxxxxxxxxxxx' end return #t", 8000.0);
    REQUIRE(!out);
    CHECK(out.error().code == "script.memory");
    CHECK(out.error().message.find("memory") != std::string::npos);
}

TEST_CASE("a shared table cannot multiply into an unbounded result") {
    ScriptOptions options;
    options.memoryLimitBytes = 128 * 1024;
    options.instructionLimit = 10000;
    Scene scene(options);
    // 2^30 leaves from a few hundred bytes of Lua: each level references the
    // one below twice, and conversion copies every use.
    auto out = scene.run("local t = {0} for i = 1, 30 do t = {t, t} end return t", 8000.0);
    REQUIRE(!out);
    CHECK(out.error().code == "script.value_too_large");
    CHECK_FALSE(out.error().hint.empty());
    // Text only: a byte string with no UTF-8 form is refused where it is made.
    auto binary = scene.run("return string.char(255)", 8000.0);
    REQUIRE(!binary);
    CHECK(binary.error().code == "script.value");
    // A large, ordinary result still converts.
    auto fine = scene.run("local t = {} for i = 1, 1000 do t[i] = i end return t", 8000.0);
    REQUIRE(fine);
}

TEST_CASE("every schema kind survives the round trip through Lua") {
    Scene scene;
    auto out = scene.run(std::string("return q.toy.echo{") + R"(
        text = "hello", n = 7, freq = "2 kHz", flag = true, mode = "fast",
        ints = {1, 2, 3},
        points = {{x = 1, label = "a"}, {x = 2}},
        gains = {a = 0.5, b = -3},
        point = {x = 9, label = "nine"},
        note = "note",
        anything = {nested = {1, 2}, empty = {}},
        move = {kind = "point", x = 4, label = "m"},
        ref = {name = "toy"}
    })");
    REQUIRE(out);
    const auto expected = obj(R"({
        "text": "hello", "n": 7, "freq": 2000.0, "flag": true, "mode": "fast",
        "ints": [1, 2, 3],
        "points": [{"x": 1, "label": "a"}, {"x": 2}],
        "gains": {"a": 0.5, "b": -3.0},
        "point": {"x": 9, "label": "nine"},
        "note": "note",
        "anything": {"nested": [1, 2], "empty": {}},
        "move": {"kind": "point", "x": 4, "label": "m"},
        "ref": {"name": "toy"}
    })");
    // operator== is JSON equality (member order and Int/Double spelling do not
    // matter); the types themselves are checked by the next test case.
    CHECK(out->result == expected);
    CHECK(out->result["n"].isInt());
    CHECK(out->result["freq"].isDouble());
    CHECK(out->result["ref"].isObject());
}

TEST_CASE("integers stay integers and doubles stay doubles") {
    Scene scene;
    auto out = scene.run(std::string("local r = q.toy.echo") + kEcho + R"(
        local m = q.toy.echo({text = "t", n = 3, freq = "1.5 kHz", flag = false, mode = "off",
            ints = {1, 2}, points = {}, gains = {a = 2.0}, point = {x = 1},
            anything = false, move = {kind = "point", x = 0}, ref = "tr_1"})
        return {
            n_integer = math.type(r.n) == "integer",
            freq_float = math.type(r.freq) == "float",
            ints_integer = math.type(r.ints[1]) == "integer",
            text_string = type(r.text) == "string",
            mode_string = type(r.mode) == "string",
            flag_bool = type(r.flag) == "boolean",
            unit_parsed = m.freq == 1500.0,
            empty_array = #r.points == 0,
            map_value_float = math.type(m.gains.a) == "float",
        }
    )");
    REQUIRE(out);
    for (const auto& member : out->result.asObject())
        CHECK_MESSAGE(member.value.asBool(), "check " << member.key);
}

TEST_CASE("an empty table is an array only where the schema says so") {
    Scene scene;
    // `ints` is an array in the schema: {} must become [] and survive the
    // typed decode; `anything` is untyped: {} stays an object.
    auto out = scene.run(std::string("return q.toy.echo") +
                         R"({text = "t", n = 0, freq = 1, flag = false, mode = "off",
                             ints = {}, points = {}, gains = {}, point = {x = 0},
                             anything = {}, move = {kind = "point", x = 0}, ref = "tr_1"})");
    REQUIRE(out);
    // The bus saw integers where the schema says array and an object where
    // nothing does: the log record holds exactly the JSON the script produced.
    REQUIRE(!scene.host.records.empty());
    const json::Value& sent = scene.host.records.back().args;
    REQUIRE(sent.find("ints"));
    CHECK(sent.find("ints")->isArray());
    CHECK(sent.find("ints")->size() == 0);
    REQUIRE(sent.find("points"));
    CHECK(sent.find("points")->isArray());
    REQUIRE(sent.find("anything"));
    CHECK(sent.find("anything")->isObject());
    // The echo's result schema encodes its empty vectors as arrays too, but a
    // returned empty table carries no type: it reads back as an object.
    CHECK(out->result["ints"].isObject());
}

TEST_CASE("command errors are tables that pcall can catch") {
    Scene scene;
    auto out = scene.run(R"(
        local ok, err = pcall(q.toy.fail, {value = 4})
        return {ok = ok, kind = type(err), code = err.code, path = err.path,
                hint = err.hint, details = type(err.details),
                message = err.message}
    )");
    REQUIRE(out);
    CHECK(out->result["ok"] == false);
    CHECK(out->result["kind"] == "table");
    CHECK(out->result["code"] == "toy.busy");
    CHECK(out->result["path"] == "/args/value");
    CHECK(out->result["hint"] == "use 0 .. 9, not 4");
    CHECK(out->result["details"] == "table");
    CHECK(out->result["message"] == "the toy is busy");
}

TEST_CASE("an uncaught command error keeps its code, path and hint") {
    Scene scene;
    auto out = scene.run("q.toy.fail{value = 4}");
    REQUIRE(!out);
    CHECK(out.error().code == "toy.busy");
    CHECK(out.error().path == "/args/value");
    CHECK(out.error().hint == "use 0 .. 9, not 4");
}

TEST_CASE("unknown commands and wrong argument types are the bus's own errors") {
    Scene scene;
    auto unknown = scene.run(R"(q.run("toy.nope", {}))");
    REQUIRE(!unknown);
    CHECK(unknown.error().code == "command.unknown");
    CHECK(unknown.error().message == "unknown command \"toy.nope\"");
    CHECK(!unknown.error().hint.empty());  // a suggestion, or the describe walk

    auto typed = scene.run(R"(q.toy.add{value = "three"})");
    REQUIRE(!typed);
    CHECK(typed.error().code == "command.invalid_args");
    REQUIRE(!typed.error().details.empty());
    CHECK(typed.error().details.front().path.rfind("/args", 0) == 0);

    // A sequence where the schema wants an object: the members are passed
    // through as their decimal names, so the bus names the field that is
    // missing instead of refusing the table outright.
    auto shape = scene.run(R"(q.toy.add{"three"})");
    REQUIRE(!shape);
    CHECK(shape.error().code == "command.invalid_args");
    CHECK(shape.error().details.front().hint.find("value") != std::string::npos);
}

TEST_CASE("a table in an array position must be a sequence") {
    Scene scene;
    auto out = scene.run(std::string("q.toy.echo") + R"({text = "t", n = 0, freq = 1,
        flag = false, mode = "off", ints = {a = 1}, points = {}, gains = {}, point = {x = 0},
        anything = false, move = {kind = "point", x = 0}, ref = "tr_1"})");
    REQUIRE(!out);
    CHECK(out.error().code == "script.table");
    CHECK(out.error().path == "/args/ints");
}

TEST_CASE("a value with no JSON form is refused") {
    Scene scene;
    auto out = scene.run(R"(q.toy.add{value = function() end})");
    REQUIRE(!out);
    CHECK(out.error().code == "script.value");
    CHECK(out.error().path == "/args/value");
}

TEST_CASE("q.batch is one transaction, and it rolls back on an error") {
    Scene scene;
    auto ok = scene.run(R"(
        q.toy.add{value = 2}
        q.batch("two steps", function()
            q.toy.add{value = 5}
            q.toy.add{value = 3}
        end)
        return q.toy.get()
    )");
    REQUIRE(ok);
    CHECK(ok->result["count"] == 10);
    CHECK(scene.host.begins == 2);
    CHECK(scene.host.commits == 2);
    CHECK(scene.host.rollbacks == 0);
    CHECK(scene.host.toy.count == 10);

    auto bad = scene.run(R"(
        q.batch("fails halfway", function()
            q.toy.add{value = 7}
            q.toy.fail{value = 1}
        end)
        return q.toy.get()
    )");
    REQUIRE(!bad);
    CHECK(bad.error().code == "toy.busy");
    CHECK(scene.host.begins == 3);
    CHECK(scene.host.commits == 2);
    CHECK(scene.host.rollbacks == 1);
    CHECK(scene.host.toy.count == 10);  // the +7 was rolled back
}

TEST_CASE("a nested q.batch joins the transaction it is inside") {
    Scene scene;
    auto out = scene.run(R"(
        q.batch("outer", function()
            q.toy.add{value = 1}
            q.batch("inner", function()
                q.toy.add{value = 2}
            end)
        end)
        return q.toy.get()
    )");
    REQUIRE(out);
    CHECK(out->result["count"] == 3);
    CHECK(scene.host.begins == 1);
    CHECK(scene.host.commits == 1);
}

TEST_CASE("a batch error inside pcall is re-raised as a table") {
    Scene scene;
    auto out = scene.run(R"(
        local ok, err = pcall(function()
            q.batch("fails", function() q.toy.fail{value = 1} end)
        end)
        return {ok = ok, kind = err and type(err) or "none", code = err and err.code or "none"}
    )");
    REQUIRE(out);
    CHECK(out->result["ok"] == false);
    CHECK(out->result["kind"] == "table");
    CHECK(out->result["code"] == "toy.busy");
}

TEST_CASE("the script's commands carry the lua origin") {
    Scene scene;
    ScriptOptions named;
    named.name = "tidier";
    Scene other(named);
    REQUIRE(other.run("q.toy.add{value = 1}"));
    CHECK(other.host.toy.origin == "lua:tidier");
    REQUIRE(!other.host.records.empty());
    CHECK(other.host.records.front().origin.type == cmd::Origin::Type::Lua);
    CHECK(other.host.records.front().origin.client == "tidier");
}

TEST_CASE("args becomes a global table and the state survives between runs") {
    Scene scene;
    auto first = scene.runWith("counter = (args.start or 0) + 1\nreturn {counter = counter}",
                               obj(R"({"start": 40})"));
    REQUIRE(first);
    CHECK(first->result["counter"] == 41);
    auto second = scene.run("counter = counter + 1\nreturn {counter = counter}");
    REQUIRE(second);
    CHECK(second->result["counter"] == 42);
}

TEST_CASE("a syntax error names the line") {
    Scene scene;
    auto out = scene.run("local x = 1\nreturn x +");
    REQUIRE(!out);
    CHECK(out.error().code == "script.syntax");
    REQUIRE(out.error().loc.has_value());
    CHECK(out.error().loc->line == 2);
    CHECK(out.error().message.rfind("script:", 0) == 0);
}

TEST_CASE("a Lua error that is not a command error keeps its traceback") {
    Scene scene;
    auto out = scene.run("local function boom() error('exploded') end\nboom()");
    REQUIRE(!out);
    CHECK(out.error().code == "script.error");
    CHECK(out.error().message.find("exploded") != std::string::npos);
    CHECK(out.error().message.find("traceback") != std::string::npos);
}

TEST_CASE("subscriptions fire once per event, filter, and can be removed") {
    Scene scene;
    auto setup = scene.run(R"(
        seen = {}
        filtered = 0
        handle = q.on("session.changed", function(payload, topic)
            seen[#seen + 1] = topic .. ":" .. tostring(payload.version)
        end)
        q.on("session.changed", function() filtered = filtered + 1 end, {version = 99})
        return {handle = handle}
    )");
    REQUIRE(setup);
    CHECK(scene.script.subscriptionCount() == 2);

    json::Value payload = obj(R"({"version": 7})");
    scene.script.publish("session.changed", payload);
    scene.script.publish("session.changed", payload);
    scene.script.publish("other.topic", payload);  // no subscriber
    auto out = scene.run("return {seen = seen, filtered = filtered}");
    REQUIRE(out);
    CHECK(out->result["seen"].size() == 2);
    CHECK(out->result["seen"].at(0) == "session.changed:7");
    CHECK(out->result["seen"].at(1) == "session.changed:7");
    CHECK(out->result["filtered"] == 0);

    scene.script.publish("session.changed", obj(R"({"version": 99})"));
    auto filtered = scene.run("return {filtered = filtered}");
    REQUIRE(filtered);
    CHECK(filtered->result["filtered"] == 1);

    REQUIRE(scene.run("q.off(handle)\nreturn {n = #seen}"));
    CHECK(scene.script.subscriptionCount() == 1);
    scene.script.publish("session.changed", payload);
    auto after = scene.run("return {n = #seen}");
    REQUIRE(after);
    CHECK(after->result["n"] == 3);  // the v99 event had reached the handler too
}

TEST_CASE("a wildcard subscription sees every topic") {
    Scene scene;
    REQUIRE(scene.run(R"(topics = {}
        q.on("*", function(_, topic) topics[#topics + 1] = topic end))"));
    scene.script.publish("fx.failed", obj(R"({"fx": "fx_1"})"));
    scene.script.publish("session.changed", obj(R"({"version": 2})"));
    auto out = scene.run("return {topics = topics}");
    REQUIRE(out);
    CHECK(out->result["topics"].size() == 2);
    CHECK(out->result["topics"].at(0) == "fx.failed");
}

TEST_CASE("a handler may run commands, and its failure is logged") {
    Scene scene;
    REQUIRE(scene.run(R"(q.on("toy.bang", function() q.toy.add{value = 3} end))"));
    scene.script.publish("toy.bang", obj("{}"));
    CHECK(scene.host.toy.count == 3);

    REQUIRE(scene.run(R"(q.on("toy.boom", function() error("handler exploded") end))"));
    scene.script.publish("toy.boom", obj("{}"));
    const auto log = scene.script.log();
    REQUIRE(!log.empty());
    CHECK(log.back().find("handler for toy.boom failed") != std::string::npos);
    CHECK(log.back().find("exploded") != std::string::npos);
}

TEST_CASE("a script created without a registry or dispatcher can still compute") {
    Script bare(nullptr, nullptr);
    auto out = bare.run("return {sum = 2 + 2}");
    REQUIRE_MESSAGE(out, out.error().toString());
    CHECK(out->result["sum"] == 4);
    auto call = bare.run("return q.toy.get()");
    REQUIRE_MESSAGE(!call, call.error().toString());
    CHECK(call.error().code == "script.error");  // q.toy is nil: the bindings are not there
}

TEST_CASE("Lua JSON conversion bounds cyclic and deep tables and preserves binary keys") {
    Scene scene;
    auto cyclic = scene.run("local t = {}; t.self = t; return t");
    REQUIRE_FALSE(cyclic);
    CHECK(cyclic.error().code == "script.value");
    auto deep = scene.run("local t = {}; for i = 1, 100 do t = {t} end; return t");
    REQUIRE_FALSE(deep);
    CHECK(deep.error().code == "script.value");
    auto badArgs = scene.run("local t = {}; t.self = t; q.toy.add(t)");
    REQUIRE_FALSE(badArgs);
    CHECK(badArgs.error().code == "script.value");
    CHECK(badArgs.error().path.rfind("/args", 0) == 0);
    auto cyclicError = scene.run("local t = {code='custom'}; t.details = {t}; error(t)");
    REQUIRE_FALSE(cyclicError);
    CHECK(cyclicError.error().code == "custom");
    auto version = scene.run("q.run('toy.add@999999999999999999999999', {})");
    REQUIRE_FALSE(version);
    CHECK(version.error().code == "command.version");
    for (const auto* expression : {"return 0/0", "return math.huge", "return {x=-math.huge}"}) {
        auto number = scene.run(expression);
        REQUIRE_FALSE(number);
        CHECK(number.error().code == "script.value");
    }
    auto keys = scene.runWith("return args", obj(R"({"a\u0000b":1,"a":2})"));
    REQUIRE(keys);
    CHECK(keys->result == obj(R"({"a\u0000b":1,"a":2})"));
    auto healthy = scene.run("return 42");
    REQUIRE(healthy);
    CHECK(healthy->result == 42);
}

TEST_CASE("Lua initialization and argument pushes fail inside protected calls") {
    ScriptOptions options;
    options.memoryLimitBytes = 5000;
    Scene tiny(options);
    auto initialized = tiny.run("return 1");
    REQUIRE_FALSE(initialized);
    CHECK(initialized.error().code == "script.memory");
    options.memoryLimitBytes = 192 * 1024;
    Scene limited(options);
    REQUIRE(limited.run("return 1"));
    json::Value args = json::Value::object();
    args["large"] = std::string(300000, 'x');
    auto pushed = limited.runWith("return 1", args);
    REQUIRE_FALSE(pushed);
    CHECK(pushed.error().code == "script.memory");
    REQUIRE(limited.run("return 2"));
}
TEST_CASE("Lua error tables are read raw without executing metamethods") {
    Scene scene;
    auto failed =
        scene.run("error(setmetatable({}, {__index=function() error('metamethod ran') end}))");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("metamethod ran") == std::string::npos);
}

TEST_CASE("Lua error decoding bounds repeated detail expansion outside the allocator") {
    Scene scene;
    auto failed = scene.run(R"(
        local e = {code = 'custom', message = string.rep('x', 65536)}
        for i = 1, 10 do e = {code = 'custom', details = {e, e}} end
        error(e)
    )");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "script.value_too_large");
    CHECK(failed.error().details.empty());
    auto healthy = scene.run("return 42");
    REQUIRE(healthy);
    CHECK(healthy->result == 42);
}

TEST_CASE("named arrays resolve JSON pointer refs and preserve an empty Lua array") {
    struct Arrays {
        std::vector<std::int64_t> values;
    };
    auto shape = schema::object<Arrays>("arrays")
                     .field("values", &Arrays::values,
                            schema::named("numbers", schema::arrayOf(schema::integer())))
                     .build();
    cmd::Registry registry;
    cmd::Command<Arrays, Arrays> echo;
    echo.name = "test.arrays";
    echo.exposure = cmd::Exposure::Internal;
    echo.args = shape;
    echo.result = shape;
    echo.run = [](const Arrays& a, cmd::Context&) -> Result<Arrays> { return a; };
    registry.add(std::move(echo));
    ToyHost host;
    cmd::Dispatcher bus(registry, host);
    Script script(&registry, &bus);
    auto result = script.run("return q.test.arrays{values={}}");
    REQUIRE(result);
    // Empty result tables have no schema on the return boundary; the command
    // succeeds only if the argument converted as the named array.
    CHECK(cmd::luaStubs(registry).find("values integer[]") != std::string::npos);
}

TEST_CASE("sandbox refuses finalizers before installation, including under an instruction cap") {
    ScriptOptions options;
    options.instructionLimit = 1000;
    Scene scene(options);
    auto loop = scene.run("setmetatable({}, {__gc=function() while true do end end})");
    REQUIRE_FALSE(loop);
    CHECK(loop.error().code == "script.finalizer");
    CHECK(loop.error().path == "/metatable/__gc");
    auto caught = scene.run(R"(
        local ok, e = pcall(setmetatable, {}, {__gc=false})
        assert(not ok and e.code == 'script.finalizer' and e.hint ~= '')
        local t, mt = {}, {}
        setmetatable(t, mt, 'ignored extra argument')
        mt.__gc = function() while true do end end
        local ok2, e2 = pcall(setmetatable, {}, mt)
        assert(not ok2 and e2.code == 'script.finalizer')
        t = nil
        collectgarbage('collect')
        return getmetatable(setmetatable({}, {__index={x=3}})).__index.x
    )");
    REQUIRE(caught);
    CHECK(caught->result.asInt() == 3);
    auto budget = scene.run("while true do end");
    REQUIRE_FALSE(budget);
    CHECK(budget.error().code == "script.budget");
}
