// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/mcp/server.h>
#include <supple/product.h>
#include "../src/wire_size.h"
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
using namespace supple;
namespace {
int makePipe(int* fds) {
#if defined(_WIN32)
    return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT);
#else
    return ::pipe(fds);
#endif
}
std::ptrdiff_t writePipe(int fd, const void* data, std::size_t size) {
#if defined(_WIN32)
    return _write(fd, data, static_cast<unsigned>(size));
#else
    return ::write(fd, data, size);
#endif
}
int closePipe(int fd) {
#if defined(_WIN32)
    return _close(fd);
#else
    return ::close(fd);
#endif
}
struct Count {
    int count = 0;
};
struct Args {
    int value = 0;
};
struct Host : cmd::Host {
    int count = 0, prior = 0;
    cmd::Origin last;
    cmd::Context makeContext(const cmd::Origin& origin) override {
        last = origin;
        cmd::Context ctx(origin);
        ctx.provide(count);
        return ctx;
    }
    Result<std::uint64_t> beginTransaction(const cmd::Origin&, std::string_view) override {
        prior = count;
        return std::uint64_t{1};
    }
    Status commitTransaction(std::uint64_t) override { return {}; }
    Status rollbackTransaction(std::uint64_t) override {
        count = prior;
        return okStatus();
    }
    Result<std::function<Status()>> invocationSavepoint() override {
        return std::function<Status()>([this, saved = count] {
            count = saved;
            return okStatus();
        });
    }
    void log(const cmd::LogRecord&) override {}
    void emit(std::string_view, json::Value) override {}
};
struct RecordingHost : Host {
    std::mutex logMutex;
    std::vector<cmd::LogRecord> records;
    void log(const cmd::LogRecord& record) override {
        std::lock_guard lock(logMutex);
        records.push_back(record);
    }
    std::vector<cmd::LogRecord> takeLog() {
        std::lock_guard lock(logMutex);
        std::vector<cmd::LogRecord> result;
        result.swap(records);
        return result;
    }
};
cmd::Registry registry() {
    cmd::Registry r;
    r.describeGroup("counter", "A counter the tests add to and read.");
    cmd::Command<Args, Count> command;
    command.name = "counter.add";
    command.kind = cmd::Kind::Edit;
    command.exposure = cmd::Exposure::McpTool;
    command.summary = "Add to counter.";
    command.args =
        schema::object<Args>("args").field("value", &Args::value, schema::integerOf<int>()).build();
    command.result = schema::object<Count>("count")
                         .field("count", &Count::count, schema::integerOf<int>())
                         .build();
    command.examples = {{"Add", json::parse(R"({"value":1})").value(),
                         json::parse(R"({"count":1})").value(), "empty"}};
    command.run = [](const Args& args, cmd::Context& ctx) -> Result<Count> {
        return Count{ctx.service<int>() += args.value};
    };
    r.add(std::move(command));
    // A result declared as any JSON value, here an array.
    cmd::Command<Args, json::Value> history;
    history.name = "counter.history";
    history.kind = cmd::Kind::Query;
    history.exposure = cmd::Exposure::McpTool;
    history.summary = "List the counter's values.";
    history.args = schema::object<Args>("history_args").build();
    history.result = schema::any();
    history.examples = {{"Values", json::Value::object(), json::parse("[0]").value(), "empty"}};
    history.run = [](const Args&, cmd::Context& ctx) -> Result<json::Value> {
        json::Value values = json::Value::array();
        values.push_back(ctx.service<int>());
        return values;
    };
    r.add(std::move(history));
    cmd::Command<cmd::NoArgs, Count> warning;
    warning.name = "counter.warn";
    warning.kind = cmd::Kind::Query;
    warning.exposure = cmd::Exposure::McpTool;
    warning.summary = "Return a counter with a warning.";
    warning.args = schema::object<cmd::NoArgs>("no_args").build();
    warning.result = schema::object<Count>("warning_count")
                         .field("count", &Count::count, schema::integerOf<int>())
                         .build();
    warning.examples = {
        {"Diagnostic", json::Value::object(),
         json::parse(
             R"({"count":0,"warnings":[{"code":"counter.warning","message":"counter diagnostic","path":"/value"}]})")
             .value(),
         "empty"}};
    warning.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<Count> {
        ctx.warn("counter.warning", "counter diagnostic", "/value");
        return Count{ctx.service<int>()};
    };
    r.add(std::move(warning));
    cmd::Command<cmd::NoArgs, Count> peek;
    peek.name = "counter.peek";
    peek.kind = cmd::Kind::Query;
    peek.exposure = cmd::Exposure::McpExec;
    peek.summary = "Read the counter.";
    peek.description = "Inspect the current value without editing it.";
    peek.args = schema::object<cmd::NoArgs>("no_args").build();
    peek.result = schema::object<Count>("count")
                      .field("count", &Count::count, schema::integerOf<int>())
                      .build();
    peek.examples = {
        {"Read", json::Value::object(), json::parse(R"({"count":0})").value(), "empty"}};
    peek.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<Count> {
        return Count{ctx.service<int>()};
    };
    r.add(peek);
    peek.name = "counter.secret";
    peek.exposure = cmd::Exposure::Internal;
    r.add(std::move(peek));
    return r;
}
json::Value parse(std::string_view input) { return json::parse(input).value(); }
json::Value request(mcp::Server& server, std::string_view input) {
    auto output = server.handle(parse(input));
    REQUIRE(output);
    return *output;
}
void checkWarnings(mcp::Server& server) {
    auto reply = request(
        server,
        R"({"jsonrpc":"2.0","id":50,"method":"tools/call","params":{"name":"counter_warn","arguments":{}}})");
    const auto& result = *reply.find("result");
    const auto* structured = result.find("structuredContent");
    REQUIRE(structured);
    const auto* warnings = structured->find("warnings");
    REQUIRE(warnings);
    REQUIRE(warnings->size() == 1);
    CHECK(warnings->at(0).find("code")->asString() == "counter.warning");
    CHECK(warnings->at(0).find("path")->asString() == "/value");
    auto text = json::parse(result.find("content")->at(0).find("text")->asString());
    REQUIRE(text);
    CHECK(*text == *structured);
}
// Dedicated tools omit output schemas; describe retains them, and structured
// content remains an object (array results are text only).
void checkMcpShapes(mcp::Server& server, int id) {
    auto listed = request(
        server, R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"tools/list"})");
    bool sawHistory = false;
    for (const auto& tool : listed.find("result")->find("tools")->asArray()) {
        if (auto* output = tool.find("outputSchema")) {
            auto* type = output->find("type");
            CHECK((type && type->isString() && type->asString() == "object"));
        }
        if (tool.find("name")->asString() == "counter_history") {
            sawHistory = true;
            CHECK(tool.find("outputSchema") == nullptr);
        }
        if (tool.find("name")->asString().starts_with("counter_"))
            CHECK(tool.find("outputSchema") == nullptr);
    }
    CHECK(sawHistory);
    auto described = request(
        server,
        R"({"jsonrpc":"2.0","id":)" + std::to_string(id + 2) +
            R"(,"method":"tools/call","params":{"name":"describe","arguments":{"command":"counter.warn"}}})");
    const auto& schema = *described.find("result")->find("structuredContent")->find("result");
    CHECK(*schema.find("additionalProperties") == false);
    CHECK(*schema.find("properties")->find("warnings") ==
          parse(R"({"type":"array","items":{"type":"object"}})"));
    auto history = request(
        server,
        R"({"jsonrpc":"2.0","id":)" + std::to_string(id + 1) +
            R"(,"method":"tools/call","params":{"name":"counter_history","arguments":{}}})");
    CHECK(history.find("result")->find("structuredContent") == nullptr);
    CHECK(
        history.find("result")->find("content")->asArray()[0].find("text")->asString().starts_with(
            "["));
}
struct ModelQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread thread;
    ModelQueue()
        : thread([this] {
              while (true) {
                  std::function<void()> task;
                  {
                      std::unique_lock lock(mutex);
                      cv.wait(lock, [&] { return stopping || !tasks.empty(); });
                      if (stopping && tasks.empty()) break;
                      task = std::move(tasks.front());
                      tasks.pop_front();
                  }
                  task();
              }
          }) {}
    ~ModelQueue() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_all();
        thread.join();
    }
    void post(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
    }
};
}  // namespace
TEST_CASE("in-process MCP negotiates, lists and executes with structured errors") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    auto early = request(server, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    CHECK(early.find("error")->find("code")->asInt() == -32000);
    auto initialized = request(
        server,
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"unit-client"}}})");
    CHECK(initialized.find("result")->find("protocolVersion")->asString() == "2025-06-18");
    CHECK(initialized.find("result")
              ->find("instructions")
              ->asString()
              .find("{code: string, message: string, path: string}") != std::string::npos);
    auto listed = request(server, R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    bool found = false;
    for (const auto& tool : listed.find("result")->find("tools")->asArray())
        if (tool.find("name")->asString() == "counter_add") found = true;
    CHECK(found);
    checkMcpShapes(server, 20);
    auto success = request(
        server,
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":3}}})");
    CHECK(success.find("result")->find("structuredContent")->find("count")->asInt() == 3);
    CHECK(host.last.type == cmd::Origin::Type::Mcp);
    CHECK(host.last.client == "unit-client");
    checkWarnings(server);
    auto failure = request(
        server,
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":"bad"}}})");
    CHECK(failure.find("result")->find("isError")->asBool());
    CHECK(failure.find("result")->find("structuredContent")->find("code")->asString() ==
          "command.invalid_args");
    auto described = request(
        server,
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"describe","arguments":{"command":"counter.add"}}})");
    CHECK(described.find("result")->find("structuredContent")->find("name")->asString() ==
          "counter.add");
    // describe walks in three steps: the groups, one group's commands, one command.
    auto groups = request(
        server,
        R"({"jsonrpc":"2.0","id":30,"method":"tools/call","params":{"name":"describe","arguments":{}}})");
    const auto* index = groups.find("result")->find("structuredContent");
    REQUIRE(index);
    CHECK(index->find("columns")->size() == 3);
    CHECK(index->find("namespaces") == nullptr);
    bool sawCounter = false;
    for (const auto& row : index->find("groups")->asArray())
        if (row.asArray().at(0).asString() == "counter") {
            sawCounter = true;
            CHECK(row.asArray().at(2).asString() == "A counter the tests add to and read.");
            CHECK(row.asArray().at(1).asInt() >= 3);
        }
    CHECK(sawCounter);
    auto rows = request(
        server,
        R"({"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"describe","arguments":{"group":"counter"}}})");
    const auto* oneGroup = rows.find("result")->find("structuredContent");
    REQUIRE(oneGroup);
    CHECK(oneGroup->find("truncated") == nullptr);
    REQUIRE(oneGroup->find("namespaces")->find("counter"));
    CHECK(oneGroup->find("namespaces")->size() == 1);
    CHECK(oneGroup->find("namespaces")->find("counter")->asArray().at(0).asArray().size() == 3);
    auto nowhere = request(
        server,
        R"({"jsonrpc":"2.0","id":32,"method":"tools/call","params":{"name":"describe","arguments":{"group":"nonesuch"}}})");
    const auto* refused = nowhere.find("result")->find("structuredContent");
    CHECK(nowhere.find("result")->find("isError")->asBool());
    CHECK(refused->find("code")->asString() == "command.unknown_group");
    CHECK(refused->find("hint")->asString().find("counter") != std::string::npos);
    auto searched = request(
        server,
        R"({"jsonrpc":"2.0","id":33,"method":"tools/call","params":{"name":"describe","arguments":{"text":"counter.add"}}})");
    CHECK(searched.find("result")->find("structuredContent")->find("namespaces")->size() == 1);
    auto executed = request(
        server,
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.add","args":{"value":2}}}})");
    CHECK(executed.find("result")->find("structuredContent")->find("count")->asInt() == 5);
    auto ping = request(server, R"({"jsonrpc":"2.0","id":10,"method":"ping"})");
    CHECK(ping.find("result")->isObject());
    CHECK_FALSE(server.handle(parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})")));
    auto unknown = request(
        server, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"wrong"}})");
    CHECK(unknown.find("error")->find("code")->asInt() == -32601);
    auto newer = request(
        server,
        R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2026-07-28"}})");
    CHECK(newer.find("result")->find("protocolVersion")->asString() == "2025-11-25");
    std::istringstream input(R"({"jsonrpc":"2.0","id":8,"method":"ping"})"
                             "\n");
    std::ostringstream output;
    CHECK(server.serve(input, output));
    CHECK(parse(output.str()).find("result") != nullptr);
}
TEST_CASE("proxy MCP forwards calls over the Unix control socket") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue model;
    rpc::Server rpcServer(commands, dispatcher, [&](auto task) { model.post(std::move(task)); });
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("mcp-test-" + std::to_string(::getpid()) + ".sock");
    auto started = rpcServer.start(path);
    if (!started) MESSAGE(started.error().toString());
    REQUIRE(started);
    rpc::Client client;
    REQUIRE(client.connect(path, "proxy", {}, true));
    auto proxy = mcp::Server::proxy(client);
    REQUIRE(proxy);
    auto initialized = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"remote-client"}}})");
    CHECK(initialized.find("result") != nullptr);
    CHECK(initialized.find("result")
              ->find("instructions")
              ->asString()
              .find("{code: string, message: string, path: string}") != std::string::npos);
    auto listed = request(**proxy, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    bool found = false;
    for (const auto& tool : listed.find("result")->find("tools")->asArray())
        if (tool.find("name")->asString() == "counter_add") found = true;
    CHECK(found);
    checkMcpShapes(**proxy, 20);
    auto success = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":5}}})");
    CHECK(success.find("result")->find("structuredContent")->find("count")->asInt() == 5);
    checkWarnings(**proxy);
    auto failure = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":"bad"}}})");
    CHECK(failure.find("result")->find("isError")->asBool());
    client.close();
    rpcServer.stop();
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("a new MCP initialization replaces the previous agent identity") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    request(
        server,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"first"},"_meta":{"agent":"agent-1"}}})");
    const char* add =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":1}}})";
    request(server, add);
    CHECK(host.last.agent == "agent-1");
    request(
        server,
        R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"second"}}})");
    request(server, add);
    CHECK(host.last.client == "second");
    CHECK(host.last.agent.empty());
}

TEST_CASE("the proxy reconnects after the app restarts") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue model;
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("mcp-reconnect-" + std::to_string(::getpid()) + ".sock");
    auto first = std::make_unique<rpc::Server>(commands, dispatcher,
                                               [&](auto task) { model.post(std::move(task)); });
    REQUIRE(first->start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "proxy", {}, true));
    int reconnects = 0;
    auto proxy = mcp::Server::proxy(client, [&](const cmd::Origin&) {
        ++reconnects;
        return client.connect(path, "proxy", {}, true);
    });
    REQUIRE(proxy);
    request(
        **proxy,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"remote-client"}}})");
    const char* add =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":2}}})";
    CHECK(request(**proxy, add).find("result")->find("structuredContent")->find("count")->asInt() ==
          2);
    first->stop();  // the app quits ...
    first.reset();
    rpc::Server second(commands, dispatcher, [&](auto task) { model.post(std::move(task)); });
    REQUIRE(second.start(path));  // ... and starts again on the same socket
    auto again = request(**proxy, add);
    CHECK(reconnects == 1);
    auto* result = again.find("result");
    REQUIRE(result);
    auto* content = result->find("structuredContent");
    REQUIRE(content);
    if (auto* error = result->find("isError"); error && error->asBool()) {
        // A stopped peer may accept the write locally and fail on the read
        // (Winsock), or reject the write (Unix). An unanswered edit is never
        // retried automatically. This fixture knows the old server stopped
        // before the call, so an explicit retry is safe and runs once.
        REQUIRE(content->find("code"));
        CHECK(content->find("code")->asString() == "rpc.read");
        again = request(**proxy, add);
        result = again.find("result");
        REQUIRE(result);
        content = result->find("structuredContent");
        REQUIRE(content);
    }
    REQUIRE(content->find("count"));
    CHECK(content->find("count")->asInt() == 4);
    CHECK(reconnects == 1);
    client.close();
    second.stop();
}

TEST_CASE("restarted catalogues refresh query admission and notify the MCP client") {
    auto catalogue = [](bool updated) {
        cmd::Registry result;
        auto add = [&](const char* name, cmd::Kind kind, cmd::Exposure exposure,
                       const char* summary, int version = 1) {
            cmd::Command<cmd::NoArgs, Count> command;
            command.name = name;
            command.version = version;
            command.kind = kind;
            command.exposure = exposure;
            command.summary = summary;
            command.description = summary;
            command.args = schema::object<cmd::NoArgs>("no_args").build();
            command.result = schema::object<Count>("count")
                                 .field("count", &Count::count, schema::integerOf<int>())
                                 .build();
            command.examples = {{"Run", parse("{}"), parse(R"({"count":"<any>"})"), "empty"}};
            command.run = [kind](const cmd::NoArgs&, cmd::Context& ctx) -> Result<Count> {
                auto& count = ctx.service<int>();
                if (kind == cmd::Kind::Edit) ++count;
                return Count{count};
            };
            result.add(std::move(command));
        };
        add("session.describe", cmd::Kind::Query, cmd::Exposure::McpExec, "Read the session.");
        add("catalogue.change", updated ? cmd::Kind::Edit : cmd::Kind::Query,
            cmd::Exposure::McpTool, updated ? "Change the counter." : "Read the counter.",
            updated ? 2 : 1);
        add(updated ? "catalogue.new" : "catalogue.old", cmd::Kind::Query, cmd::Exposure::McpTool,
            "Read the counter.");
        return result;
    };
    for (auto tools : {mcp::ToolSet::Core, mcp::ToolSet::All}) {
        for (bool customReconnect : {false, true}) {
            INFO("all tools: ", tools == mcp::ToolSet::All, ", callback: ", customReconnect);
            auto oldCommands = catalogue(false), newCommands = catalogue(true);
            RecordingHost host;
            cmd::Dispatcher oldBus(oldCommands, host), newBus(newCommands, host);
            ModelQueue model;
            auto path = std::filesystem::temp_directory_path() /
                        ("mcp-catalogue-" + std::to_string(::getpid()) + ".sock");
            rpc::Server first(oldCommands, oldBus, [&](auto task) { model.post(std::move(task)); });
            REQUIRE(first.start(path));
            rpc::Client client;
            REQUIRE(client.connect(path, "catalogue-test", {}, true));
            mcp::Server::Reconnect reconnect;
            if (customReconnect)
                reconnect = [&](const cmd::Origin&) {
                    return client.connect(path, "catalogue-test", {}, true);
                };
            auto proxy = mcp::Server::proxy(client, reconnect, tools);
            REQUIRE(proxy);
            auto initialized = request(
                **proxy,
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
            first.stop();
            if (customReconnect) {
                client.close();  // force the explicit reconnect callback
            } else {
                // A failed call retains the endpoint. The next RPC call will
                // automatically reopen it, without a proxy reconnect callback.
                auto lost = client.call("registry.list");
                REQUIRE_FALSE(lost);
            }
            rpc::Server second(newCommands, newBus,
                               [&](auto task) { model.post(std::move(task)); });
            REQUIRE(second.start(path));
            host.takeLog();
            const std::string changedQuery =
                R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"query","arguments":{"command":"catalogue.change"}}})"
                "\n";
            const std::string newQuery =
                R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"query","arguments":{"command":"catalogue.new"}}})"
                "\n";
            // Exercise both first-call orders: a new query must reconnect
            // before it can be rejected by a stale local admission map.
            std::istringstream input(
                (customReconnect ? newQuery + changedQuery : changedQuery + newQuery) +
                R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})"
                "\n"
                R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"catalogue_old"}})"
                "\n"
                R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"catalogue_new"}})"
                "\n");
            std::ostringstream output;
            REQUIRE((*proxy)->serve(input, output));
            std::map<std::int64_t, json::Value> replies;
            int notifications = 0;
            std::istringstream frames(output.str());
            for (std::string frame; std::getline(frames, frame);) {
                auto value = parse(frame);
                if (auto* id = value.find("id")) {
                    replies[id->asInt()] = std::move(value);
                } else {
                    CHECK(value.find("method")->asString() == "notifications/tools/list_changed");
                    CHECK(value.find("jsonrpc")->asString() == "2.0");
                    ++notifications;
                }
            }
            REQUIRE(replies.size() == 5);
            const auto& denied = *replies.at(2).find("result");
            CHECK(denied.find("isError"));
            if (denied.find("isError"))
                CHECK(denied.find("structuredContent")->find("code")->asString() == "mcp.query");
            for (auto id : {3, 6}) {
                const auto* read = replies.at(id).find("result");
                CHECK(read);
                if (!read) continue;
                CHECK_FALSE(read->find("isError"));
                if (!read->find("isError"))
                    CHECK(read->find("structuredContent")->find("count")->asInt() == 0);
            }
            const auto* removed = replies.at(5).find("error");
            CHECK(removed);
            if (removed) CHECK(removed->find("code")->asInt() == -32601);
            bool sawNew = false;
            for (const auto& tool : replies.at(4).find("result")->find("tools")->asArray()) {
                auto name = tool.find("name")->asString();
                CHECK(name != "catalogue_old");
                if (name == "catalogue_new") sawNew = true;
                if (name == "catalogue_change") {
                    CHECK_FALSE(tool.find("annotations")->find("readOnlyHint")->asBool());
                    CHECK(tool.find("description")->asString().starts_with("Change the counter."));
                }
            }
            CHECK(sawNew);
            CHECK(notifications == 1);
            const auto* capability = initialized.find("result")
                                         ->find("capabilities")
                                         ->find("tools")
                                         ->find("listChanged");
            CHECK(capability);
            if (capability) CHECK(capability->asBool());
            auto calls = host.takeLog();
            CHECK(std::count_if(calls.begin(), calls.end(), [](const auto& call) {
                      return call.command == "registry.list";
                  }) == 1);
            CHECK(std::none_of(calls.begin(), calls.end(), [](const auto& call) {
                return call.command == "catalogue.change";
            }));
            second.stop();
            auto disconnected = client.call("registry.list");
            REQUIRE_FALSE(disconnected);
            rpc::Server third(newCommands, newBus, [&](auto task) { model.post(std::move(task)); });
            REQUIRE(third.start(path));
            host.takeLog();
            std::istringstream sameInput(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})"
                                         "\n");
            std::ostringstream sameOutput;
            REQUIRE((*proxy)->serve(sameInput, sameOutput));
            // Identical index: only the index request, with no notification
            // and no schema fetches. The single output frame is the reply.
            auto sameReply = json::parse(sameOutput.str());
            REQUIRE(sameReply);
            CHECK(sameReply->find("id")->asInt() == 7);
            calls = host.takeLog();
            REQUIRE(calls.size() == 1);
            CHECK(calls.front().command == "registry.list");
            client.close();
            third.stop();
        }
    }
}

TEST_CASE("an edit whose reply is lost is not resent after catalogue refresh") {
    struct StopAfterEdit : Host {
        std::function<void()> afterEdit;
        void log(const cmd::LogRecord& record) override {
            if (record.command == "counter.add" && record.ok && afterEdit) {
                auto stop = std::move(afterEdit);
                stop();  // after commit, before RPC can enqueue the reply
            }
        }
    } host;
    auto commands = registry();
    cmd::Dispatcher bus(commands, host);
    ModelQueue model;
    auto path = std::filesystem::temp_directory_path() /
                ("mcp-lost-reply-" + std::to_string(::getpid()) + ".sock");
    rpc::Server first(commands, bus, [&](auto task) { model.post(std::move(task)); });
    rpc::Server second(commands, bus, [&](auto task) { model.post(std::move(task)); });
    REQUIRE(first.start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "lost-reply", {}, true));
    std::promise<Status> restarted;
    auto ready = restarted.get_future().share();
    auto proxy = mcp::Server::proxy(client, [&](const cmd::Origin& origin) -> Status {
        const auto& status = ready.get();
        if (!status) return status.error();
        return client.connect(path, origin.client, origin.agent, true);
    });
    REQUIRE(proxy);
    request(
        **proxy,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"lost-reply"}}})");
    std::promise<void> armed;
    auto armedFuture = armed.get_future();
    model.post([&] {
        host.afterEdit = [&] {
            first.stop();
            restarted.set_value(second.start(path));
        };
        armed.set_value();
    });
    armedFuture.get();
    auto lost = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":1}}})");
    const auto* result = lost.find("result");
    REQUIRE(result);
    CHECK(result->find("isError"));
    CHECK(result->find("structuredContent")->find("code")->asString() == "rpc.read");
    auto read = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.peek"}}})");
    CHECK(read.find("result")->find("structuredContent")->find("count")->asInt() == 1);
    client.close();
    second.stop();
}

TEST_CASE("remote transport-looking command errors never authorize edit replay") {
    for (const auto* code : {"rpc.write", "rpc.connect", "rpc.read", "rpc.timeout"}) {
        auto commands = registry();
        std::atomic<int> invoked{0};
        cmd::Command<cmd::NoArgs, Count> command;
        command.name = "counter.remote-error";
        command.kind = cmd::Kind::Edit;
        command.exposure = cmd::Exposure::McpTool;
        command.summary = "Return a remote command error.";
        command.args = schema::object<cmd::NoArgs>("none").build();
        command.result = schema::object<Count>("count")
                             .field("count", &Count::count, schema::integerOf<int>())
                             .build();
        command.examples = {{"Fail", parse("{}"), parse("{}"), "empty"}};
        command.run = [&](const cmd::NoArgs&, cmd::Context&) -> Result<Count> {
            ++invoked;
            return Error(code, "remote command failure", "/value", "check before retrying");
        };
        commands.add(std::move(command));
        Host host;
        cmd::Dispatcher bus(commands, host);
        ModelQueue model;
        const auto path = std::filesystem::temp_directory_path() /
                          ("mcp-delivery-" + std::to_string(::getpid()) + ".sock");
        rpc::Server remote(commands, bus, [&](auto task) { model.post(std::move(task)); });
        REQUIRE(remote.start(path));
        rpc::Client client;
        REQUIRE(client.connect(path, "delivery"));
        auto proxy = mcp::Server::proxy(client);
        REQUIRE(proxy);
        request(
            **proxy,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
        const auto reply = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"counter_remote-error"}})");
        CHECK(reply.find("result")->find("structuredContent")->find("code")->asString() == code);
        CHECK(invoked == 1);
        client.close();
        remote.stop();
    }
}

TEST_CASE("proxy startup fetches only selected tools and resolves other commands on demand") {
    auto commands = registry();
    cmd::Command<Args, Count> adjust;
    adjust.name = "counter.adjust";
    adjust.kind = cmd::Kind::Edit;
    adjust.exposure = cmd::Exposure::McpExec;
    adjust.summary = "Adjust the counter.";
    adjust.args =
        schema::object<Args>("args").field("value", &Args::value, schema::integerOf<int>()).build();
    adjust.result = schema::object<Count>("count")
                        .field("count", &Count::count, schema::integerOf<int>())
                        .build();
    adjust.examples = {{"Adjust", parse(R"({"value":1})"), parse(R"({"count":1})"), "empty"}};
    adjust.run = [](const Args& args, cmd::Context& ctx) -> Result<Count> {
        return Count{ctx.service<int>() += args.value};
    };
    commands.add(adjust);
    for (int i = 0; i < 24; ++i) {
        adjust.name = "deferred.command" + std::to_string(i);
        commands.add(adjust);
    }
    // Select the latest version before filtering: exposures may change.
    adjust.name = "counter.former";
    adjust.exposure = cmd::Exposure::McpTool;
    commands.add(adjust);
    adjust.version = 2;
    adjust.exposure = cmd::Exposure::McpExec;
    commands.add(adjust);
    adjust.name = "counter.promoted";
    adjust.version = 1;
    adjust.exposure = cmd::Exposure::Internal;
    commands.add(adjust);
    adjust.version = 2;
    adjust.exposure = cmd::Exposure::McpTool;
    commands.add(adjust);
    cmd::Command<cmd::NoArgs, Count> session;
    session.name = "session.describe";  // enables the generic query tool
    session.summary = "Read the counter's session.";
    session.args = schema::object<cmd::NoArgs>("no_args").build();
    session.result = adjust.result;
    session.examples = {{"Read", parse("{}"), parse(R"({"count":0})"), "empty"}};
    session.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<Count> {
        return Count{ctx.service<int>()};
    };
    commands.add(std::move(session));
    RecordingHost host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server remote(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    auto path = std::filesystem::temp_directory_path() /
                ("mcp-tools-" + std::to_string(::getpid()) + ".sock");
    REQUIRE(remote.start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "tool-set-test"));
    for (auto set : {mcp::ToolSet::Core, mcp::ToolSet::All}) {
        mcp::Server local(commands, dispatcher, set);
        host.takeLog();
        auto proxy = mcp::Server::proxy(client, {}, set);
        REQUIRE(proxy);
        auto startup = host.takeLog();
        std::set<std::string> expected;
        for (const auto* info : commands.all()) {
            if (commands.find(info->name) != info || info->exposure == cmd::Exposure::Internal)
                continue;
            if (set == mcp::ToolSet::All || info->exposure == cmd::Exposure::McpTool)
                expected.insert(info->name + "@" + std::to_string(info->version));
        }
        REQUIRE(startup.size() == 1 + expected.size());
        CHECK(startup.front().command == "registry.list");
        for (std::size_t i = 1; i < startup.size(); ++i) {
            CHECK(startup[i].command == "registry.describe");
            CHECK(expected.erase(startup[i].args.find("command")->asString()) == 1);
        }
        CHECK(expected.empty());
        std::vector<json::Value> catalogues;
        for (auto* server : {&local, proxy->get()}) {
            request(
                *server,
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})");
            auto listed = request(*server, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
            catalogues.push_back(*listed.find("result"));
            bool peek = false;
            for (const auto& tool : listed.find("result")->find("tools")->asArray()) {
                auto name = tool.find("name")->asString();
                CHECK(name != "counter_secret");
                if (name == "counter_peek") {
                    peek = true;
                    CHECK(tool.find("description")->asString() ==
                          "Inspect the current value without editing it. See describe for errors "
                          "and examples.");
                }
                if (name == "counter_add")
                    CHECK(tool.find("description")->asString() ==
                          "Add to counter. See describe for errors and examples.");
            }
            CHECK(peek == (set == mcp::ToolSet::All));
            auto result = request(
                *server,
                R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.peek"}}})");
            CHECK(result.find("result")->find("structuredContent")->find("count")->asInt() == 0);
            auto dedicated = request(
                *server,
                R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"counter_peek"}})");
            if (set == mcp::ToolSet::Core)
                CHECK(dedicated.find("error")->find("code")->asInt() == -32601);
            else
                CHECK(dedicated.find("result")->find("structuredContent")->find("count")->asInt() ==
                      0);
        }
        CHECK(catalogues[0] == catalogues[1]);
        // No description was fetched for this edit during core startup. Even
        // before describe, its real dispatcher validates and executes it.
        host.takeLog();
        auto invalid = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.adjust","args":{"value":"bad"}}}})");
        const auto* failure = invalid.find("result")->find("structuredContent");
        REQUIRE(failure);
        CHECK(failure->find("code")->asString() == "command.invalid_args");
        auto changed = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.adjust","args":{"value":4}}}})");
        CHECK(changed.find("result")->find("structuredContent")->find("count")->asInt() == 4);
        auto calls = host.takeLog();
        REQUIRE(calls.size() == 2);
        CHECK(calls[0].command == "counter.adjust");
        CHECK_FALSE(calls[0].ok);
        CHECK(calls[1].command == "counter.adjust");
        CHECK(calls[1].ok);
        auto read = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"query","arguments":{"command":"counter.peek"}}})");
        CHECK(read.find("result")->find("structuredContent")->find("count")->asInt() == 4);
        calls = host.takeLog();
        REQUIRE(calls.size() == 1);
        CHECK(calls.front().command == "counter.peek");
        auto denied = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"query","arguments":{"command":"counter.adjust","args":{"value":4}}}})");
        CHECK(denied.find("result")->find("structuredContent")->find("code")->asString() ==
              "mcp.query");
        CHECK(host.takeLog().empty());
        auto described = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"describe","arguments":{"command":"counter.adjust"}}})");
        const auto* detail = described.find("result")->find("structuredContent");
        REQUIRE(detail);
        CHECK(detail->find("name")->asString() == "counter.adjust");
        CHECK(detail->find("args")->find("properties")->find("value")->find("type")->asString() ==
              "integer");
        CHECK(detail->find("result")->find("type")->asString() == "object");
        CHECK(detail->find("errors")->isArray());
        CHECK(detail->find("examples")->size() == 1);
        calls = host.takeLog();
        REQUIRE(calls.size() == 1);
        CHECK(calls.front().command == "registry.describe");
        CHECK(calls.front().args.find("command")->asString() == "counter.adjust");
        // The groups and a group's rows come over the same connection: one request each (the bus
        // refuses an unknown group itself).
        auto proxied = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":41,"method":"tools/call","params":{"name":"describe","arguments":{}}})");
        REQUIRE(proxied.find("result")->find("structuredContent")->find("groups"));
        calls = host.takeLog();
        REQUIRE(calls.size() == 1);
        CHECK(calls.front().command == "registry.groups");
        auto proxiedGroup = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"describe","arguments":{"group":"counter"}}})");
        REQUIRE(proxiedGroup.find("result")
                    ->find("structuredContent")
                    ->find("namespaces")
                    ->find("counter"));
        calls = host.takeLog();
        REQUIRE(calls.size() == 1);
        CHECK(calls.front().command == "registry.list");
        auto restored = request(
            **proxy,
            R"({"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.adjust","args":{"value":-4}}}})");
        CHECK(restored.find("result")->find("structuredContent")->find("count")->asInt() == 0);
    }
    client.close();
    remote.stop();
}

TEST_CASE("MCP stdio caps an unterminated oversized frame") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    std::istringstream input(std::string(rpc::kMaxMessageBytes + 1, 'x'));
    std::ostringstream output;
    REQUIRE(server.serve(input, output));
    auto reply = json::parse(output.str());
    REQUIRE(reply);
    CHECK(reply->find("error")->find("code")->asInt() == -32600);
    CHECK(reply->find("id")->isNull());
}

// The stdio reader polls with the tick's deadline: a service tick keeps
// running while the server waits, and a request is still answered at once.
TEST_CASE("the stdio loop runs the service tick while it waits for a request") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    int fds[2];
    REQUIRE(makePipe(fds) == 0);
    std::atomic<int> ticks{0};
    std::ostringstream output;
    Status served;
    std::thread runner([&] {
        served = server.serve(fds[0], output,
                              mcp::Tick{[&] {
                                            ++ticks;
                                            ticks.notify_one();
                                        },
                                        std::chrono::milliseconds(10)});
    });
    for (int value = ticks.load(); value < 3; value = ticks.load()) ticks.wait(value);
    CHECK(ticks.load() >= 3);  // it waited, ticking, instead of blocking
    const std::string initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"tick-test"}}})"
        "\n";
    CHECK(writePipe(fds[1], initialize.data(), initialize.size()) ==
          static_cast<std::ptrdiff_t>(initialize.size()));
    closePipe(fds[1]);  // end of input: the loop answers what it read, then ends
    runner.join();
    CHECK(served);
    const std::string text = output.str();
    CHECK(text.find("\"protocolVersion\"") != std::string::npos);
    CHECK(text.find("\"id\":1") != std::string::npos);
    closePipe(fds[0]);
}

TEST_CASE("R16 malformed envelopes without ids return Invalid Request") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    for (const auto input :
         {R"({})", R"({"method":"ping"})", R"({"jsonrpc":"1.0","method":"ping"})",
          R"({"jsonrpc":"2.0","method":3})",
          R"({"jsonrpc":"2.0","method":"ping","params":false})"}) {
        auto reply = server.handle(parse(input));
        REQUIRE(reply);
        CHECK(reply->find("id")->isNull());
        CHECK(reply->find("error")->find("code")->asInt() == -32600);
    }
    CHECK_FALSE(server.handle(parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})")));
}

TEST_CASE("malformed request ids are never echoed into MCP error envelopes") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    for (const auto* id : {"true", "[]", "{}", "1.5"}) {
        auto request = parse(std::string(R"({"jsonrpc":"2.0","method":"ping","id":)") + id + "}");
        const auto reply = server.handle(request);
        REQUIRE(reply);
        CHECK(reply->find("id")->isNull());
        CHECK(reply->find("error")->find("code")->asInt() == -32600);
    }
    const auto reply = server.handle(parse(R"({"jsonrpc":"1.0","method":"ping","id":"valid-id"})"));
    REQUIRE(reply);
    CHECK(*reply->find("id") == "valid-id");
    CHECK(host.count == 0);
}

TEST_CASE("MCP request ids have a serialized byte budget") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    for (const auto& [id, fits] :
         std::vector<std::pair<std::string, bool>>{{std::string(1022, 'i'), true},
                                                   {std::string(1023, 'i'), false},
                                                   {std::string(511, '\n'), true},
                                                   {std::string(512, '\n'), false},
                                                   {std::string(170, '\0'), true},
                                                   {std::string(171, '\0'), false}}) {
        auto input = parse(R"({"jsonrpc":"2.0","method":"ping"})");
        input["id"] = id;
        const auto result = server.handle(input);
        REQUIRE(result);
        if (fits)
            CHECK(*result->find("id") == id);
        else {
            CHECK(result->find("id")->isNull());
            REQUIRE(result->find("error")->find("data"));
            CHECK(result->find("error")->find("data")->find("code")->asString() == "rpc.id_size");
        }
    }
}

TEST_CASE("MCP refuses wide frames at its value budget and resumes at the next frame") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    std::string wide = R"({"jsonrpc":"2.0","id":2,"method":"ping","params":{"values":[)";
    for (int i = 0; i < 1000000; ++i) wide += i ? ",0" : "0";
    wide += "]}}\n";
    std::istringstream input(
        std::string(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})") +
        "\n" + wide + R"({"jsonrpc":"2.0","id":3,"method":"ping"})" + "\n");
    std::ostringstream output;
    REQUIRE(server.serve(input, output));
    std::istringstream replies(output.str());
    std::string line;
    REQUIRE(std::getline(replies, line));
    REQUIRE(std::getline(replies, line));
    const auto refused = parse(line);
    REQUIRE(refused.find("error"));
    REQUIRE(refused.find("error")->find("data"));
    CHECK(refused.find("error")->find("data")->find("code")->asString() ==
          "json.parse.too_many_values");
    CHECK(refused.find("id")->isNull());
    REQUIRE(std::getline(replies, line));
    CHECK(*parse(line).find("id") == 3);
}

TEST_CASE("generic MCP argument envelopes are validated before executing edits") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    request(
        server,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    auto call = [&](std::string_view name, const json::Value& args) -> Result<json::Value> {
        auto input = parse(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{}})");
        input["params"]["name"] = name;
        input["params"]["arguments"] = args;
        const auto reply = server.handle(input);
        REQUIRE(reply);
        const auto* result = reply->find("result");
        REQUIRE(result);
        const auto* structured = result->find("structuredContent");
        REQUIRE(structured);
        if (const auto* failed = result->find("isError"); failed && failed->asBool())
            return rpc::errorFromJson(*structured);
        return *structured;
    };
    for (
        const auto* args :
        {R"({"command":"counter.add","args":{"value":1},"typo":true})",
         R"({"command":false,"args":false,"batch":{"steps":[{"command":"counter.add","args":{"value":1}}]}})",
         R"({"batch":{"steps":[]},"args":[]})", R"({"batch":{"steps":[]},"command":null})"}) {
        auto result = call("exec", parse(args));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "mcp.arguments");
        CHECK(result.error().path.starts_with("/arguments/"));
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().hint.empty());
        CHECK(host.count == 0);
    }
    for (const auto* args :
         {R"({"command":"counter.add","tag":false})", R"({"command":"counter.add","unknown":0})"}) {
        const auto result = call("describe", parse(args));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "mcp.arguments");
    }
    REQUIRE(call("exec", parse(R"({"command":"counter.add","args":{"value":2}})")));
    REQUIRE(call("exec",
                 parse(R"({"batch":{"steps":[{"command":"counter.add","args":{"value":3}}]}})")));
    CHECK(host.count == 5);
}

TEST_CASE("malformed initialize identity cannot replace the accepted audit identity") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    const auto initial = parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
        "protocolVersion":"2025-11-25","clientInfo":{"name":"accepted"},"_meta":{"agent":"original"}}})");
    REQUIRE(server.handle(initial)->find("result"));
    for (const auto* params : {R"({"clientInfo":false})", R"({"clientInfo":{"name":3}})",
                               R"({"clientInfo":{"version":false}})", R"({"_meta":[]})",
                               R"({"_meta":{"agent":null}})"}) {
        auto input = initial;
        const auto patch = parse(params);
        for (const auto& field : patch.asObject()) input["params"][field.key] = field.value;
        const auto reply = server.handle(input);
        REQUIRE(reply);
        REQUIRE(reply->find("error"));
        CHECK(reply->find("error")->find("code")->asInt() == -32602);
        request(
            server,
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"counter_history"}})");
        CHECK(host.last.client == "accepted");
        CHECK(host.last.agent == "original");
    }
}

TEST_CASE("stdio service fds run while an incomplete request waits without a periodic tick") {
    auto r = registry();
    Host host;
    cmd::Dispatcher bus(r, host);
    mcp::Server server(r, bus);
    int input[2];
    WakeEvent wake;
    REQUIRE(makePipe(input) == 0);
    REQUIRE(wake.waitable().valid());
    const std::string prefix = R"({"jsonrpc":"2.0","id":1,"method":"initialize")";
    REQUIRE(writePipe(input[1], prefix.data(), prefix.size()) ==
            static_cast<std::ptrdiff_t>(prefix.size()));
    wake.notify();
    bool ready = false;
    int calls = 0;
    mcp::Tick service;
    service.watches = [&] {
        return std::vector<mcp::Tick::Watch>{{wake.waitable(), kWaitRead, [&](short) {
                                                  REQUIRE(wake.drain());
                                                  ready = true;
                                              }}};
    };
    service.deadline = [] { return std::optional<std::chrono::steady_clock::time_point>{}; };
    service.fn = [&] {
        CHECK(ready);
        ++calls;
        REQUIRE(writePipe(input[1], "}\n", 2) == 2);
        closePipe(input[1]);
    };
    std::ostringstream output;
    REQUIRE(server.serve(input[0], output, service));
    CHECK(calls == 1);
    CHECK(output.str().find("protocolVersion") != std::string::npos);
    closePipe(input[0]);
}

TEST_CASE("stdio recomputes one-shot deadlines after service") {
    auto r = registry();
    Host host;
    cmd::Dispatcher bus(r, host);
    mcp::Server server(r, bus);
    int input[2];
    REQUIRE(makePipe(input) == 0);
    int calls = 0;
    mcp::Tick service;
    service.deadline = [&] {
        return calls ? std::optional<std::chrono::steady_clock::time_point>{}
                     : std::optional(std::chrono::steady_clock::time_point::min());
    };
    service.fn = [&] {
        ++calls;
        closePipe(input[1]);
    };
    std::ostringstream output;
    REQUIRE(server.serve(input[0], output, service));
    CHECK(calls == 1);
    closePipe(input[0]);
}

TEST_CASE("stdio service is fair during a long request already buffered by the reader") {
    auto r = registry();
    Host host;
    cmd::Dispatcher bus(r, host);
    mcp::Server server(r, bus);
    auto* input = ::tmpfile();
    REQUIRE(input);
    const std::string frame =
        std::string(6000, ' ') + R"({"jsonrpc":"2.0","id":1,"method":"initialize"})" + '\n';
    REQUIRE(::fwrite(frame.data(), 1, frame.size(), input) == frame.size());
    ::rewind(input);
    WakeEvent wake;
    REQUIRE(wake.waitable().valid());
    int inspections = 0, calls = 0;
    mcp::Tick service;
    service.watches = [&] {
        if (++inspections == 2) wake.notify();
        return std::vector<mcp::Tick::Watch>{
            {wake.waitable(), kWaitRead, [&](short) { REQUIRE(wake.drain()); }}};
    };
    service.fn = [&] {
        ++calls;
        CHECK(inspections == 2);
    };
    std::ostringstream output;
    REQUIRE(server.serve(::fileno(input), output, service));
    CHECK(calls == 1);
    CHECK(output.str().find("protocolVersion") != std::string::npos);
    ::fclose(input);
}

TEST_CASE("fd output backpressure preserves service, admission order and EOF replies") {
    bool eventService = false, deadlineService = false;
    SUBCASE("periodic service") {}
    SUBCASE("native service wake") { eventService = true; }
    SUBCASE("absolute service deadline") { deadlineService = true; }
    WakeEvent serviceWake;
    auto commands = registry();
    std::atomic<int> invoked{0};
    cmd::Command<cmd::NoArgs, json::Value> large;
    large.name = "counter.large";
    large.kind = cmd::Kind::Query;
    large.exposure = cmd::Exposure::Internal;
    large.summary = "Return a large test packet.";
    large.args = schema::object<cmd::NoArgs>("none").build();
    large.result = schema::any();
    large.run = [&](const cmd::NoArgs&, cmd::Context&) -> Result<json::Value> {
        if (++invoked == 1) serviceWake.notify();
        auto value = json::Value::object();
        value["data"] = std::string(1024 * 1024, 'x');
        return value;
    };
    commands.add(std::move(large));
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    request(
        server,
        R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    int input[2], output[2];
    REQUIRE(makePipe(input) == 0);
    REQUIRE(makePipe(output) == 0);
    const std::string frames =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.large"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.large"}}})"
        "\n";
    REQUIRE(writePipe(input[1], frames.data(), frames.size()) ==
            static_cast<std::ptrdiff_t>(frames.size()));
    closePipe(input[1]);
    std::promise<void> serviced;
    auto ready = serviced.get_future();
    bool signaled = false;
    mcp::Tick tick;
    tick.fn = [&] {
        if (invoked.load() == 1 && !signaled) {
            signaled = true;
            serviced.set_value();
        }
    };
    if (eventService)
        tick.watches = [&] {
            return std::vector<mcp::Tick::Watch>{
                {serviceWake.waitable(), kWaitRead, [&](short) { serviceWake.drain(); }}};
        };
    if (deadlineService)
        tick.deadline = [&]() -> std::optional<std::chrono::steady_clock::time_point> {
            if (invoked.load() == 1 && !signaled) return std::chrono::steady_clock::time_point{};
            return std::nullopt;
        };
    Status status;
    std::thread serving([&] {
        status = server.serve(input[0], output[1], tick);
        closePipe(output[1]);
    });
    // The first packet is much larger than a pipe's capacity. Without a reader,
    // native writer progress cannot complete it. Service must still acknowledge.
    if (ready.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
        std::fprintf(stderr, "WATCHDOG ABORT fd output service\n");
        std::_Exit(124);
    }
    CHECK(invoked.load() == 1);  // buffered second command has not been admitted
    std::string wire;
    char bytes[65536];
    for (;;) {
#if defined(_WIN32)
        const auto count = _read(output[0], bytes, sizeof(bytes));
#else
        const auto count = ::read(output[0], bytes, sizeof(bytes));
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        wire.append(bytes, static_cast<std::size_t>(count));
    }
    serving.join();
    REQUIRE(status);
    CHECK(invoked.load() == 2);
    std::istringstream replies(wire);
    std::string line;
    for (int id : {1, 2}) {
        REQUIRE(std::getline(replies, line));
        const auto value = parse(line);
        CHECK(*value.find("id") == id);
        CHECK_FALSE(value.find("result")->find("isError"));
    }
    CHECK_FALSE(std::getline(replies, line));
    closePipe(input[0]);
    closePipe(output[0]);
}

TEST_CASE("fd output closed consumer reports failure and joins instead of SIGPIPE") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    int input[2], output[2];
    REQUIRE(makePipe(input) == 0);
    REQUIRE(makePipe(output) == 0);
    const std::string frame =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})"
        "\n";
    REQUIRE(writePipe(input[1], frame.data(), frame.size()) ==
            static_cast<std::ptrdiff_t>(frame.size()));
    closePipe(input[1]);
    closePipe(output[0]);
    const auto status = server.serve(input[0], output[1]);
    REQUIRE_FALSE(status);
    CHECK(status.error().code == "mcp.write");
    closePipe(input[0]);
    closePipe(output[1]);
}

TEST_CASE("an oversized tool reply becomes an explicit bounded error") {
    auto commands = registry();
    cmd::Command<cmd::NoArgs, json::Value> huge;
    huge.name = "counter.huge";
    huge.kind = cmd::Kind::Query;
    huge.exposure = cmd::Exposure::Internal;
    huge.summary = "Return an oversized result.";
    huge.args = schema::object<cmd::NoArgs>("none").build();
    huge.result = schema::any();
    huge.run = [](const cmd::NoArgs&, cmd::Context&) -> Result<json::Value> {
        auto value = json::Value::object();
        value["data"] = std::string(40 * 1024 * 1024, 'x');
        return value;
    };
    commands.add(std::move(huge));
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    request(
        server,
        R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    const auto reply = request(
        server,
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.huge"}}})");
    CHECK(*reply.find("result")->find("structuredContent")->find("code") ==
          "mcp.response_too_large");
    CHECK(json::writeCompact(reply).size() < 4096);
}

TEST_CASE("final packet accounting does not charge another escaping layer") {
    auto commands = registry();
    cmd::Command<cmd::NoArgs, json::Value> quoted;
    quoted.name = "counter.quoted";
    quoted.kind = cmd::Kind::Query;
    quoted.exposure = cmd::Exposure::Internal;
    quoted.summary = "Return quote-heavy data.";
    quoted.args = schema::object<cmd::NoArgs>("none").build();
    quoted.result = schema::any();
    quoted.run = [](const cmd::NoArgs&, cmd::Context&) -> Result<json::Value> {
        auto value = json::Value::object();
        value["s"] = std::string(6 * 1024 * 1024, '"');
        return value;
    };
    commands.add(std::move(quoted));
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    std::istringstream input(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.quoted"}}})"
        "\n");
    std::ostringstream output;
    REQUIRE(server.serve(input, output));
    std::istringstream frames(output.str());
    std::string line;
    REQUIRE(std::getline(frames, line));
    REQUIRE(std::getline(frames, line));
    CHECK(line.size() > 32 * 1024 * 1024);
    CHECK(line.size() < 64 * 1024 * 1024);
    const auto reply = parse(line);
    CHECK_FALSE(reply.find("result")->find("isError"));
}

TEST_CASE("fd output invalid setup and repeated empty sessions cleanly join") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    mcp::Server server(commands, bus);
    for (int i = 0; i < 32; ++i) {
        int input[2], output[2];
        REQUIRE(makePipe(input) == 0);
        REQUIRE(makePipe(output) == 0);
        closePipe(input[1]);
        REQUIRE(server.serve(input[0], output[1]));
        closePipe(output[1]);
        closePipe(output[0]);
        const auto closedOutput = server.serve(input[0], output[1]);
        REQUIRE_FALSE(closedOutput);
        CHECK(closedOutput.error().code == "mcp.write");
        closePipe(input[0]);
        const auto closedInput = server.serve(input[0], -1);
        REQUIRE_FALSE(closedInput);
        CHECK(closedInput.error().code == "mcp.read");
    }
}

TEST_CASE("wire size counter matches compact escaping") {
    for (const auto& value : std::vector<json::Value>{
             json::Value("quote\"\\\n\t"), json::Value(std::string("\0", 1)), json::Value("é"),
             json::Value(std::string("\x80", 1)), *json::parse(R"({"a":[null,1.25,true,"b"]})")}) {
        mcp::detail::WireSize size;
        REQUIRE(size.value(value));
        const auto wire = json::writeCompact(value);
        CHECK(size.plain == wire.size());
        CHECK(size.quoted == json::writeCompact(json::Value(wire)).size() - 2);
    }
}
