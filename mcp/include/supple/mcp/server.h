// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <supple/base/waitable.h>
#include <supple/rpc/protocol.h>
#include <chrono>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>

namespace supple::mcp {
enum class ToolSet { Core, All };

// Service on the serving thread, between requests or bytes of partial frames.
// The native reader waits for async sources/deadlines; legacy callers use an interval.
struct Tick {
    std::function<void()> fn;
    std::chrono::milliseconds interval{100};
    struct Watch {
        Waitable source;
        short events;
        std::function<void(short)> ready;
    };
    // When supplied, these replace the periodic interval. Sources/deadlines
    // are recomputed after service and requests; fn stays on the serving thread.
    std::function<std::vector<Watch>()> watches = {};
    std::function<std::optional<std::chrono::steady_clock::time_point>()> deadline = {};
};

// MCP over newline-delimited JSON-RPC on stdio. The in-process deployment
// executes on the caller's model thread; the proxy forwards through rpc::Client.
class Server {
  public:
    Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher,
           ToolSet tools = ToolSet::Core);
    // A proxy to an app's control socket. `reconnect` (optional) connects the
    // client again after the app restarted; a call whose request never reached
    // the app (it was gone) reconnects once and is sent again.
    using Reconnect = std::function<Status(const cmd::Origin&)>;
    static Result<std::unique_ptr<Server>> proxy(rpc::Client& client, Reconnect reconnect = {},
                                                 ToolSet tools = ToolSet::Core);
    Status serve(std::istream& input, std::ostream& output, Tick tick = {});
    // As above, reading requests from `fd` so it can be polled with the tick's
    // deadline (a negative fd reads blocking).
    Status serve(int fd, std::ostream& output, Tick tick = {});
    std::optional<json::Value> handle(const json::Value& request);
    // Embedders drain pending notifications after handle(); serve() does this
    // automatically, before writing that request's response.
    json::Value takeNotifications();

  private:
    Server(rpc::Client& client, Reconnect reconnect, ToolSet tools);
    Status prepareProxy();
    Status refreshCatalogue();
    Result<json::Value> call(std::string_view name, const json::Value& args);
    Result<json::Value> callTool(std::string_view name, const json::Value& args);
    json::Value tools() const;
    std::function<Result<json::Value>(std::string_view, const json::Value&, const cmd::Origin&)>
        invoke_;
    json::Value dedicated_ = json::Value::array();
    std::map<std::string, std::string> commands_;
    std::map<std::string, cmd::Kind> kinds_;
    rpc::Client* proxyClient_ = nullptr;
    rpc::Delivery delivery_ = rpc::Delivery::Sent;
    Reconnect reconnect_;
    ToolSet toolSet_ = ToolSet::Core;
    std::uint64_t catalogueGeneration_ = 0;
    json::Value index_;
    bool toolsChanged_ = false;
    bool queryTool_ = false;
    bool initialized_ = false;
    cmd::Origin origin_{cmd::Origin::Type::Mcp, {}, {}};
};
}  // namespace supple::mcp
