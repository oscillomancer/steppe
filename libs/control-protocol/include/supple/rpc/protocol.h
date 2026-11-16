// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <supple/cmd/bus.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace supple::rpc {
inline constexpr std::size_t kMaxMessageBytes = 16 * 1024 * 1024;
using Post = std::function<void(std::function<void()>)>;

// Structured errors on the wire: code, message, path, hint, the optional text
// location as `line`/`column`, and nested `details`. errorFromJson restores a
// location only when both fields are integers in [0, INT_MAX].
json::Value errorJson(const Error& error);
Error errorFromJson(const json::Value& value);
std::filesystem::path socketPath(std::string_view instance);

enum class Delivery { Unsent, Sent };
struct CallResult {
    Result<json::Value> result;
    Delivery delivery;
};

// The I/O thread owns sockets and framing. `post` schedules every request on
// the caller's model thread; the caller must drain that queue until stop().
class Server {
  public:
    Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher, Post post);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Status start(const std::filesystem::path& path);
    void stop();
    // Called on the model thread by the host's event sink.
    void publish(std::string_view topic, const json::Value& payload);
    std::filesystem::path path() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Synchronous client for the MCP proxy and protocol tests. Calls must be made
// off the model thread when connected to a server using that same thread.
//
// Recovery: a call that fails in transport (write or read error, timeout, a
// signal interrupting a wait, an oversized or unreadable response frame, a
// response with another id) closes the connection, so a late response can
// never answer a later call. The client keeps the endpoint given to connect()
// until close(): the next call reconnects first and repeats the last accepted
// rpc.hello (connect()'s, or a later explicit one), then sends its own request.
// A failed reconnect returns rpc.connect and the request is not sent. The
// failed call itself is never resent: after rpc.timeout or rpc.interrupted the
// request may still have run on the server. Structured command errors
// returned by the server keep the connection.
class Client {
  public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    // Remembers the endpoint (even when this attempt fails), then connects and
    // performs rpc.hello.
    Status connect(const std::filesystem::path& path, std::string client, std::string agent = {},
                   bool mcp = false);
    // Disconnects and forgets the endpoint; later calls fail with rpc.connect.
    void close();
    // Observe the current connection without consuming data or sending requests.
    // Returns zero if disconnected (including an observed peer close), otherwise
    // a generation that increases after each successful connection handshake.
    std::uint64_t connectionGeneration();
    // How long connect() or a call may take in all -- connecting (a call's
    // reconnect included), the handshake, sending the request and waiting
    // for the response -- clamped to [1 ms, 24 h]; 30 s by default. A peer
    // that stops accepting data ends it with rpc.timeout, or rpc.connect
    // while connecting.
    void setTimeout(std::chrono::milliseconds timeout);
    Result<json::Value> call(std::string_view method,
                             const json::Value& params = json::Value::object());
    // Local provenance of this command, independent of a reconnect handshake or
    // remote error codes. Sent means a complete frame may have executed.
    CallResult callWithDelivery(std::string_view method,
                                const json::Value& params = json::Value::object());

  private:
    using Deadline = std::chrono::steady_clock::time_point;
    Status open(Deadline deadline);
    void drop();
    Result<json::Value> exchange(std::string_view method, const json::Value& params,
                                 Deadline deadline, Delivery* delivery = nullptr);
    std::intptr_t fd_ = -1;
    std::uint64_t nextId_ = 1;
    std::uint64_t connectionGeneration_ = 0;
    std::string input_;
    std::filesystem::path path_;
    std::optional<json::Value> hello_;  // set while an endpoint is remembered
    std::chrono::milliseconds timeout_{30000};
};
}  // namespace supple::rpc
