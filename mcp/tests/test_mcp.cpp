// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/mcp/server.h>
#include <supple/product.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
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
cmd::Registry registry() {
    cmd::Registry r;
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
// MCP clients refuse a tool list whose output schemas are not object schemas
// (the Python SDK fails the whole list), and structured content is an object.
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
    }
    CHECK(sawHistory);
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
