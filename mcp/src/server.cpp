// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/mcp/server.h>
#include <supple/product.h>
#include <supple/rpc/model_loop.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <string>

namespace supple::mcp {
namespace {
json::Value typeSchema(std::string_view type) {
    json::Value value = json::Value::object();
    value["type"] = std::string(type);
    return value;
}
json::Value objectSchema(std::initializer_list<std::pair<std::string, json::Value>> properties,
                         std::initializer_list<std::string> required = {}) {
    auto value = typeSchema("object");
    value["properties"] = json::Value::object();
    for (const auto& [name, schema] : properties) value["properties"][name] = schema;
    value["required"] = json::Value::array();
    for (const auto& name : required) value["required"].push_back(name);
    value["additionalProperties"] = false;
    return value;
}
json::Value response(const json::Value& id, json::Value value) {
    json::Value out = json::Value::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(value);
    return out;
}
json::Value error(const json::Value& id, int code, std::string message) {
    json::Value out = json::Value::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = json::Value::object();
    out["error"]["code"] = code;
    out["error"]["message"] = std::move(message);
    return out;
}
std::string stringField(const json::Value& value, std::string_view key) {
    auto* part = value.isObject() ? value.find(key) : nullptr;
    return part && part->isString() ? part->asString() : std::string{};
}
json::Value tool(std::string name, std::string title, std::string description, json::Value input,
                 json::Value output = typeSchema("object")) {
    json::Value value = json::Value::object();
    value["name"] = std::move(name);
    value["title"] = std::move(title);
    value["description"] = std::move(description);
    value["inputSchema"] = std::move(input);
    // MCP output schemas must describe an object; results declared as any
    // JSON value (an empty schema) advertise none.
    if (stringField(output, "type") == "object") value["outputSchema"] = std::move(output);
    return value;
}
json::Value toolFromInfo(const json::Value& info) {
    auto name = stringField(info, "name");
    std::replace(name.begin(), name.end(), '.', '_');
    auto title = stringField(info, "summary");
    auto description =
        title + " " + stringField(info, "description") + " See describe for errors and examples.";
    auto* args = info.find("args");
    auto input = args ? *args : objectSchema({});
    if (auto* props = input.find("properties"); props && props->isObject())
        for (auto& field : input["properties"].asObject()) {
            auto* unit = field.value.find("x-unit");
            if (!unit || !unit->isString()) continue;
            auto prior = stringField(field.value, "description");
            field.value["description"] = prior + (prior.empty() ? "" : " ") + "(" +
                                         unit->asString() + "; accepts quantity strings)";
        }
    auto* result = info.find("result");
    auto value = tool(std::move(name), std::move(title), std::move(description), std::move(input),
                      result ? *result : typeSchema("object"));
    auto kind = stringField(info, "kind");
    value["annotations"] = json::Value::object();
    value["annotations"]["readOnlyHint"] = kind == "query";
    value["annotations"]["destructiveHint"] = kind == "action";
    auto command = stringField(info, "name");
    value["annotations"]["idempotentHint"] =
        command.ends_with(".set") || command.ends_with(".merge");
    return value;
}
}  // namespace

Server::Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher) {
    invoke_ = [&dispatcher](std::string_view name, const json::Value& args,
                            const cmd::Origin& origin) {
        return dispatcher.execute({std::string(name), args, origin});
    };
    dedicated_ = cmd::mcpToolList(registry);
    for (auto* info : registry.all()) {
        if (registry.find(info->name) != info) continue;
        kinds_[info->name] = info->kind;
        if (info->exposure == cmd::Exposure::McpTool) {
            std::string toolName = info->name;
            std::replace(toolName.begin(), toolName.end(), '.', '_');
            commands_[toolName] = info->name;
        }
    }
    queryTool_ = registry.find("session.describe") != nullptr;
}
Server::Server(rpc::Client& client, Reconnect reconnect)
    : proxyClient_(&client), reconnect_(std::move(reconnect)) {
    invoke_ = [this, &client](std::string_view name, const json::Value& args,
                              const cmd::Origin& origin) {
        auto result = client.call(name, args);
        if (result || !reconnect_) return result;
        // The app went away. Not sent (write failed, or no connection): connect
        // and send it once more. Sent but unanswered: reconnect for the next
        // call, but do not repeat a command that may have run.
        const std::string& code = result.error().code;
        const bool unsent = code == "rpc.write" || code == "rpc.connect";
        if (!unsent && code != "rpc.read") return result;
        if (!reconnect_(origin)) return result;
        return unsent ? client.call(name, args) : result;
    };
}
Result<std::unique_ptr<Server>> Server::proxy(rpc::Client& client, Reconnect reconnect) {
    auto server = std::unique_ptr<Server>(new Server(client, std::move(reconnect)));
    auto listed = client.call("registry.list");
    if (!listed) return listed.error();
    auto* commands = listed->find("commands");
    if (!commands || !commands->isArray())
        return Error("mcp.registry", "remote registry.list returned no commands");
    std::map<std::string, int> latest;
    for (const auto& item : commands->asArray()) {
        auto name = stringField(item, "name");
        auto* version = item.find("version");
        if (version && version->isInt())
            latest[name] = std::max(latest[name], static_cast<int>(version->asInt()));
    }
    for (const auto& [name, version] : latest) {
        json::Value args = json::Value::object();
        args["command"] = name + "@" + std::to_string(version);
        auto described = client.call("registry.describe", args);
        if (!described) return described.error();
        auto kind = stringField(*described, "kind");
        server->kinds_[name] = kind == "query"  ? cmd::Kind::Query
                               : kind == "edit" ? cmd::Kind::Edit
                                                : cmd::Kind::Action;
        if (stringField(*described, "exposure") == "mcp-tool") {
            auto definition = toolFromInfo(*described);
            auto toolName = stringField(definition, "name");
            server->commands_[toolName] = name;
            server->dedicated_.push_back(std::move(definition));
        }
    }
    server->queryTool_ = latest.contains("session.describe");
    return server;
}
json::Value Server::tools() const {
    auto out = dedicated_;
    auto string = typeSchema("string"), object = typeSchema("object");
    out.push_back(tool(
        "describe", "Describe commands", "Search or describe registered commands.",
        objectSchema({{"command", string}, {"tag", string}, {"kind", string}, {"text", string}})));
    auto execInput = objectSchema({{"command", string}, {"args", object}, {"batch", object}});
    out.push_back(tool("exec", "Execute a command", "Run one command or an atomic batch.",
                       std::move(execInput)));
    if (queryTool_)
        out.push_back(tool("query", "Query the session", "Run a read-only command.",
                           objectSchema({{"command", string}, {"args", object}}, {"command"})));
    return out;
}
Result<json::Value> Server::call(std::string_view name, const json::Value& args) {
    if (name == "describe") {
        auto command = stringField(args, "command");
        if (!command.empty()) {
            json::Value params = json::Value::object();
            params["command"] = command;
            return invoke_("registry.describe", params, origin_);
        }
        return invoke_("registry.list", args, origin_);
    }
    if (name == "exec") {
        if (auto* batch = args.find("batch")) return invoke_("batch", *batch, origin_);
        auto command = stringField(args, "command");
        if (command.empty())
            return Error("mcp.arguments", "exec needs command or batch", "/arguments",
                         "provide command and args, or batch");
        auto* params = args.find("args");
        return invoke_(command, params ? *params : json::Value::object(), origin_);
    }
    if (name == "query") {
        auto command = stringField(args, "command");
        auto it = kinds_.find(command);
        if (it == kinds_.end() || it->second != cmd::Kind::Query)
            return Error("mcp.query", "query needs a registered read-only command",
                         "/arguments/command", "use describe to find a query command");
        auto* params = args.find("args");
        return invoke_(command, params ? *params : json::Value::object(), origin_);
    }
    auto it = commands_.find(std::string(name));
    if (it == commands_.end()) return Error("mcp.tool", "unknown tool", "/name", "use tools/list");
    return invoke_(it->second, args, origin_);
}
std::optional<json::Value> Server::handle(const json::Value& request) {
    if (!request.isObject()) return error(nullptr, -32600, "Invalid Request");
    auto* id = request.find("id");
    auto* version = request.find("jsonrpc");
    auto* method = request.find("method");
    auto* params = request.find("params");
    if (!version || !version->isString() || version->asString() != "2.0" || !method ||
        !method->isString() || (id && !(id->isString() || id->isInt() || id->isNull())) ||
        (params && !(params->isObject() || params->isArray())))
        return error(id ? *id : json::Value(nullptr), -32600, "Invalid Request");
    if (!id) return std::nullopt;
    if (params && !params->isObject()) return error(*id, -32602, "Invalid params");
    auto args = params ? *params : json::Value::object();
    auto name = method->asString();
    if (name == "initialize") {
        auto* requested = args.find("protocolVersion");
        if (!requested || !requested->isString())
            return error(*id, -32602, "Missing protocolVersion");
        auto* clientInfo = args.find("clientInfo");
        origin_.client = clientInfo ? stringField(*clientInfo, "name") : std::string{};
        auto* meta = args.find("_meta");
        origin_.agent = meta ? stringField(*meta, "agent") : std::string{};
        auto versionName = requested->asString();
        if (versionName != "2025-06-18" && versionName != "2025-11-25") versionName = "2025-11-25";
        if (proxyClient_) {
            json::Value hello = json::Value::object();
            hello["client"] = origin_.client.empty() ? "mcp-client" : origin_.client;
            hello["agent"] = origin_.agent;
            hello["protocol"] = 1;
            hello["mcp"] = true;
            auto greeted = proxyClient_->call("rpc.hello", hello);
            if (!greeted) return error(*id, -32000, greeted.error().toString());
        }
        initialized_ = true;
        json::Value result = json::Value::object();
        result["protocolVersion"] = versionName;
        result["capabilities"] = json::Value::object();
        result["capabilities"]["tools"] = json::Value::object();
        result["serverInfo"] = json::Value::object();
        result["serverInfo"]["name"] = std::string(product::kSlug);
        result["serverInfo"]["version"] = std::string(product::kVersion);
        result["instructions"] =
            "Successful command results may include warnings: command warnings followed by "
            "bus diagnostics {code: string, message: string, path: string}. Paths are retained "
            "as emitted. Non-object results with bus warnings become {result: value, warnings: "
            "[...]}. Batch warnings stay on each step's result.";
        return response(*id, std::move(result));
    }
    if (!initialized_) return error(*id, -32000, "Initialize first");
    if (name == "ping") return response(*id, json::Value::object());
    if (name == "tools/list") {
        json::Value result = json::Value::object();
        result["tools"] = tools();
        return response(*id, std::move(result));
    }
    if (name == "tools/call") {
        auto toolName = stringField(args, "name");
        auto* arguments = args.find("arguments");
        if (toolName.empty() || (arguments && !arguments->isObject()))
            return error(*id, -32602, "tools/call needs name and object arguments");
        if (toolName != "describe" && toolName != "exec" && !(toolName == "query" && queryTool_) &&
            !commands_.contains(toolName))
            return error(*id, -32601, "Unknown tool");
        auto result = call(toolName, arguments ? *arguments : json::Value::object());
        json::Value output = json::Value::object();
        output["content"] = json::Value::array();
        json::Value text = json::Value::object();
        text["type"] = "text";
        if (result) {
            // MCP structured content is an object; other results go as text only.
            if (result->isObject()) output["structuredContent"] = *result;
            text["text"] = json::writeCompact(*result);
        } else {
            output["isError"] = true;
            output["structuredContent"] = rpc::errorJson(result.error());
            text["text"] = result.error().toString();
        }
        output["content"].push_back(std::move(text));
        return response(*id, std::move(output));
    }
    return error(*id, -32601, "Method not found");
}
namespace {

// The byte source the framing loop waits on: a stream (tests, in-process) or an
// fd it can poll with the tick's deadline.
struct Byter {
    virtual ~Byter() = default;
    virtual int wait(
        int timeoutMs,
        const std::vector<Tick::Watch>& watches) = 0;  // >0 ready, 0 not ready (yet), <0 end
    virtual bool buffered() const { return false; }
    virtual int get() = 0;  // the byte, or <0 at the end
};

struct StreamByter : Byter {
    explicit StreamByter(std::istream& input) : input_(input) {}
    int wait(int, const std::vector<Tick::Watch>&) override {
        if (input_.rdbuf()->in_avail() > 0) return 1;
        return input_.peek() == std::char_traits<char>::eof() ? -1 : 1;
    }
    int get() override { return input_.get(); }
    std::istream& input_;
};

// Buffered: a multi-megabyte request costs a few reads, not a poll and a read
// per byte.
struct FdByter : Byter {
    explicit FdByter(int fd) : fd_(fd) {
#if defined(_WIN32)
        reader_ = std::thread([this] { readLoop(); });
#endif
    }
#if defined(_WIN32)
    ~FdByter() override {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        consumed_.notify_one();
        // Cancellation can race the reader's entry into synchronous ReadFile.
        // Retry only during shutdown until it exits; no idle polling.
        while (WaitForSingleObject(reader_.native_handle(), 10) == WAIT_TIMEOUT)
            CancelSynchronousIo(reader_.native_handle());
        reader_.join();
    }
#endif
    bool buffered() const override { return pos_ < end_; }
    int wait(int timeoutMs, const std::vector<Tick::Watch>& watches) override {
        const bool buffered = pos_ < end_;
        if (buffered && watches.empty()) return 1;
#if defined(_WIN32)
        std::vector<WaitItem> fds{{inputReady_.waitable(), kWaitRead, 0}};
#else
        std::vector<WaitItem> fds{{fd_, kWaitRead, 0}};
#endif
        for (const auto& watch : watches) fds.push_back({watch.source, watch.events, 0});
        const int ready = waitFor(fds, buffered ? 0 : timeoutMs);
        if (ready < 0) return errno == EINTR ? 0 : -1;
        bool serviced = false;
        for (std::size_t i = 0; i < watches.size(); ++i) {
            if (!fds[i + 1].ready) continue;
            // The owner validates snapshots if a preceding callback removed a
            // later fd; callbacks only publish work, service runs below.
            watches[i].ready(fds[i + 1].ready);
            serviced = true;
        }
        if (serviced) return 0;
        if (buffered) return 1;
        if (ready == 0) return 0;
        return (fds[0].ready & (kWaitRead | kWaitHangup | kWaitError)) ? 1 : -1;
    }
    int get() override {
        if (pos_ == end_) {
#if defined(_WIN32)
            {
                std::lock_guard lock(mutex_);
                if (!inboxSize_) return -1;
                inputReady_.drain();
                std::copy_n(inbox_, inboxSize_, buffer_);
                pos_ = 0;
                end_ = inboxSize_;
                inboxSize_ = 0;
            }
            consumed_.notify_one();
#else
            ssize_t n;
            do n = ::read(fd_, buffer_, sizeof(buffer_));
            while (n < 0 && errno == EINTR);
            if (n <= 0) return -1;
            pos_ = 0;
            end_ = static_cast<std::size_t>(n);
#endif
        }
        return static_cast<unsigned char>(buffer_[pos_++]);
    }
#if defined(_WIN32)
    void readLoop() {
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                consumed_.wait(lock, [this] { return quit_ || inboxSize_ == 0; });
                if (quit_) return;
            }
            DWORD count = 0;
            const bool ok = ReadFile(handle, inbox_, sizeof(inbox_), &count, nullptr) != 0;
            {
                std::lock_guard lock(mutex_);
                inboxSize_ = ok ? count : 0;
                inputReady_.notify();
            }
            if (!ok || !count) return;
        }
    }
    WakeEvent inputReady_;
    std::mutex mutex_;
    std::condition_variable consumed_;
    bool quit_ = false;
    char inbox_[65536];
    std::size_t inboxSize_ = 0;
    std::thread reader_;
#endif
    int fd_;
    char buffer_[65536];
    std::size_t pos_ = 0, end_ = 0;
};

// Frames newline-delimited requests. Async service also runs between bytes
// of partial frames and buffered input; dispatch itself stays model-thread only.
Status pump(Server& server, Byter& byter, std::ostream& output, const Tick& tick) {
    const bool periodic = tick.fn && !tick.deadline && !tick.watches;
    std::size_t bytes = 0;
    bool boundary = true;
    std::string frame;
    bool overflow = false;
    auto last = std::chrono::steady_clock::now();
    auto runTick = [&] {
        if (tick.fn) tick.fn();
        last = std::chrono::steady_clock::now();
    };
    while (true) {
        // Inspect async sources at frame boundaries and every 4 KiB while
        // framing long/continuous input, including the reader's buffered bytes.
        const bool inspect = boundary || bytes % 4096 == 0 || !byter.buffered();
        int timeout = inspect && periodic
                          ? rpc::detail::waitMilliseconds(last + tick.interval -
                                                          std::chrono::steady_clock::now())
                          : -1;
        if (inspect && tick.deadline)
            if (const auto deadline = tick.deadline()) {
                const auto now = std::chrono::steady_clock::now();
                timeout = *deadline <= now ? 0 : rpc::detail::waitMilliseconds(*deadline - now);
            }
        if (inspect && timeout == 0) {
            runTick();
            continue;
        }
        auto watches = inspect && tick.watches ? tick.watches() : std::vector<Tick::Watch>{};
        const int ready = byter.wait(timeout, watches);
        if (ready < 0) break;
        if (ready == 0) {
            runTick();
            continue;
        }
        const int next = byter.get();
        if (next < 0) break;
        ++bytes;
        boundary = next == '\n';
        if (next != '\n') {
            if (!overflow) {
                if (frame.size() < rpc::kMaxMessageBytes)
                    frame.push_back(static_cast<char>(next));
                else {
                    frame.clear();
                    overflow = true;
                }
            }
            if (periodic && std::chrono::steady_clock::now() - last >= tick.interval) runTick();
            continue;
        }
        if (overflow) {
            output << json::writeCompact(error(nullptr, -32600, "Message too large")) << '\n';
            output.flush();
            if (!output) return Error("mcp.write", "failed to write stdio response");
            frame.clear();
            overflow = false;
            continue;
        }
        auto parsed = json::parse(frame);
        auto reply = parsed ? server.handle(*parsed)
                            : std::optional<json::Value>(error(nullptr, -32700, "Parse error"));
        if (reply) {
            output << json::writeCompact(*reply) << '\n';
            output.flush();
            if (!output) return Error("mcp.write", "failed to write stdio response");
        }
        frame.clear();
    }
    if (overflow) {
        output << json::writeCompact(error(nullptr, -32600, "Message too large")) << '\n';
        output.flush();
        if (!output) return Error("mcp.write", "failed to write stdio response");
    }
    return {};
}

}  // namespace

Status Server::serve(std::istream& input, std::ostream& output, Tick tick) {
    StreamByter byter(input);
    return pump(*this, byter, output, tick);
}

Status Server::serve(int fd, std::ostream& output, Tick tick) {
    FdByter byter(fd);
    return pump(*this, byter, output, tick);
}
}  // namespace supple::mcp
