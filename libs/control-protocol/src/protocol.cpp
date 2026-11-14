// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/base/paths.h>
#include <supple/product.h>
#include <supple/rpc/model_loop.h>
#include <supple/rpc/protocol.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "endpoint_platform.h"
#include "socket_platform.h"

namespace supple::rpc {
namespace {
json::Value response(const json::Value& id, json::Value result) {
    json::Value out = json::Value::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}
json::Value rpcError(const json::Value& id, int code, std::string message, Error detail) {
    json::Value out = json::Value::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = json::Value::object();
    out["error"]["code"] = code;
    out["error"]["message"] = std::move(message);
    out["error"]["data"] = errorJson(detail);
    return out;
}
bool validId(const json::Value& id) { return id.isString() || id.isInt() || id.isNull(); }
std::string line(const json::Value& value) { return json::writeCompact(value) + "\n"; }
}  // namespace

json::Value errorJson(const Error& error) {
    json::Value out = json::Value::object();
    out["code"] = error.code;
    out["message"] = error.message;
    out["path"] = error.path;
    out["hint"] = error.hint;
    if (error.loc) {
        out["line"] = error.loc->line;
        out["column"] = error.loc->column;
    }
    if (!error.details.empty()) {
        out["details"] = json::Value::array();
        for (const auto& detail : error.details) out["details"].push_back(errorJson(detail));
    }
    return out;
}
Error errorFromJson(const json::Value& value) {
    if (!value.isObject()) return Error("rpc.remote", "remote error has no structured data");
    auto get = [&](std::string_view key) {
        auto* part = value.find(key);
        return part && part->isString() ? part->asString() : std::string{};
    };
    Error error(get("code"), get("message"), get("path"), get("hint"));
    auto position = [&](std::string_view key) -> std::optional<int> {
        auto* part = value.find(key);
        if (!part || !part->isInt() || part->asInt() < 0 || part->asInt() > INT_MAX)
            return std::nullopt;
        return static_cast<int>(part->asInt());
    };
    if (auto line = position("line"), column = position("column"); line && column)
        error.loc = SourceLoc{*line, *column};
    if (auto* details = value.find("details"); details && details->isArray())
        for (const auto& detail : details->asArray())
            error.details.push_back(errorFromJson(detail));
    return error;
}
std::filesystem::path socketPath(std::string_view instance) {
    const auto root = paths::runtime();
    return root / std::string(product::kSlug) / (std::string(instance) + ".sock");
}

struct Server::Impl : std::enable_shared_from_this<Impl> {
    struct Subscription {
        std::string id;
        std::vector<std::string> topics;
        json::Value filter = json::Value::object();
    };
    struct Peer {
        std::atomic<net::Socket> fd = net::invalid;
        std::thread thread;
        std::mutex outputMutex;
        std::deque<std::string> output;
        std::size_t outputBytes = 0;
        std::size_t pendingRequests = 0;
        std::size_t pendingBytes = 0;
        std::string input, sending;
        std::size_t sent = 0;
        bool closeAfterWrite = false;
        bool greeted = false;
        cmd::Origin origin{cmd::Origin::Type::Rpc, {}, {}};
        std::vector<Subscription> subscriptions;
        std::uint64_t nextSubscription = 1;
        std::uint64_t dropped = 0;
    };
    const cmd::Registry& registry;
    cmd::Dispatcher& dispatcher;
    Post post;
    std::filesystem::path socket;
    net::Socket listener = net::invalid;
    net::Identity socketIdentity;
    std::thread acceptThread;
    std::mutex peersMutex;
    std::vector<std::shared_ptr<Peer>> peers;
    std::map<std::string, std::uint64_t> topicSequence;
    std::atomic<bool> running = false;
    Impl(const cmd::Registry& r, cmd::Dispatcher& d, Post p)
        : registry(r), dispatcher(d), post(std::move(p)) {}
    ~Impl() { stop(); }

    void enqueue(const std::shared_ptr<Peer>& peer, json::Value value, bool event = false) {
        std::string message = line(value);
        std::lock_guard lock(peer->outputMutex);
        if (!event && message.size() > kMaxMessageBytes) {
            const auto* id = value.find("id");
            message = line(rpcError(id ? *id : json::Value(nullptr), -32000, "Response too large",
                                    Error("rpc.response_too_large", "response exceeds 16 MiB", "",
                                          "narrow the query")));
        }
        if (event &&
            (peer->output.size() >= 256 || peer->outputBytes + message.size() > 1024 * 1024)) {
            ++peer->dropped;
            return;
        }
        if (!event && peer->outputBytes + message.size() > 32 * 1024 * 1024 - 64 * 1024) {
            const auto* id = value.find("id");
            message = line(rpcError(
                id ? *id : json::Value(nullptr), -32000, "Response too large",
                Error("rpc.response_too_large", "outbound queue is full", "", "narrow the query")));
        }
        peer->outputBytes += message.size();
        peer->output.push_back(std::move(message));
    }
    void dropNotice(const std::shared_ptr<Peer>& peer) {
        std::lock_guard lock(peer->outputMutex);
        if (!peer->dropped || peer->output.size() >= 256) return;
        json::Value notice = json::Value::object();
        notice["jsonrpc"] = "2.0";
        notice["method"] = "event.dropped";
        notice["params"] = json::Value::object();
        notice["params"]["count"] = peer->dropped;
        peer->dropped = 0;
        auto message = line(notice);
        peer->outputBytes += message.size();
        peer->output.push_back(std::move(message));
    }
    void process(const std::shared_ptr<Peer>& peer, const std::string& frame) {
        auto parsed = json::parse(frame);
        if (!parsed) {
            enqueue(peer, rpcError(nullptr, -32700, "Parse error", parsed.error()));
            return;
        }
        const auto& request = *parsed;
        if (request.isArray()) {
            enqueue(peer, rpcError(nullptr, -32600, "Invalid Request",
                                   Error("rpc.batch", "JSON-RPC arrays are unsupported", "",
                                         "use the atomic batch command")));
            return;
        }
        if (!request.isObject()) {
            enqueue(peer, rpcError(nullptr, -32600, "Invalid Request",
                                   Error("rpc.invalid_request", "expected a request object")));
            return;
        }
        auto* id = request.find("id");
        json::Value idValue = id ? *id : json::Value(nullptr);
        // Notifications are ignored and recorded without a response.
        if (!id) {
            std::fprintf(stderr, "rpc: rejected client notification\n");
            return;
        }
        auto* version = request.isObject() ? request.find("jsonrpc") : nullptr;
        auto* method = request.isObject() ? request.find("method") : nullptr;
        auto* params = request.isObject() ? request.find("params") : nullptr;
        if (!validId(*id) || !version || !version->isString() || version->asString() != "2.0" ||
            !method || !method->isString()) {
            enqueue(peer, rpcError(idValue, -32600, "Invalid Request",
                                   Error("rpc.invalid_request", "expected JSON-RPC 2.0 request", "",
                                         "use an object with id, method and object params")));
            return;
        }
        if (params && !params->isObject()) {
            enqueue(peer, rpcError(idValue, -32602, "Invalid params",
                                   Error("rpc.invalid_params", "params must be an object",
                                         "/params", "send an object of named arguments")));
            return;
        }
        const auto& name = method->asString();
        json::Value args = params ? *params : json::Value::object();
        if (name == "rpc.hello") {
            auto* client = args.find("client");
            auto* protocol = args.find("protocol");
            auto* agent = args.find("agent");
            auto* mcp = args.find("mcp");
            if (!client || !client->isString() || !protocol || !protocol->isInt() ||
                protocol->asInt() != 1 || (agent && !agent->isString()) ||
                (mcp && !mcp->isBool())) {
                enqueue(peer, rpcError(idValue, -32602, "Invalid params",
                                       Error("rpc.hello", "unsupported or incomplete handshake",
                                             "/params", "send client and protocol 1")));
                return;
            }
            peer->greeted = true;
            peer->origin = {mcp && mcp->asBool() ? cmd::Origin::Type::Mcp : cmd::Origin::Type::Rpc,
                            client->asString(), agent ? agent->asString() : std::string{}};
            json::Value result = json::Value::object();
            result["server"] = std::string(product::kSlug);
            result["product"] = std::string(product::kName);
            result["version"] = std::string(product::kVersion);
            result["protocol"] = 1;
            result["commands"] = static_cast<std::uint64_t>(registry.all().size());
            enqueue(peer, response(idValue, std::move(result)));
            return;
        }
        if (!peer->greeted) {
            enqueue(peer, rpcError(idValue, -32000, "Handshake required",
                                   Error("rpc.handshake", "rpc.hello is required first", "",
                                         "call rpc.hello with protocol 1")));
            return;
        }
        if (name == "rpc.subscribe") {
            auto* topics = args.find("topics");
            auto* filter = args.find("filter");
            if (!topics || !topics->isArray() || (filter && !filter->isObject())) {
                enqueue(peer, rpcError(idValue, -32602, "Invalid params",
                                       Error("rpc.subscribe", "topics must be an array",
                                             "/params/topics")));
                return;
            }
            Subscription sub;
            sub.id = "sub_" + std::to_string(peer->nextSubscription++);
            for (const auto& topic : topics->asArray()) {
                if (!topic.isString()) {
                    enqueue(peer, rpcError(idValue, -32602, "Invalid params",
                                           Error("rpc.subscribe", "topic must be a string",
                                                 "/params/topics")));
                    return;
                }
                sub.topics.push_back(topic.asString());
            }
            if (filter) sub.filter = *filter;
            auto subId = sub.id;
            peer->subscriptions.push_back(std::move(sub));
            json::Value result = json::Value::object();
            result["subscription"] = subId;
            enqueue(peer, response(idValue, std::move(result)));
            return;
        }
        if (name == "rpc.unsubscribe") {
            auto* sub = args.find("subscription");
            if (!sub || !sub->isString()) {
                enqueue(peer, rpcError(idValue, -32602, "Invalid params",
                                       Error("rpc.unsubscribe", "subscription must be a string",
                                             "/params/subscription")));
                return;
            }
            auto& subscriptions = peer->subscriptions;
            subscriptions.erase(std::remove_if(subscriptions.begin(), subscriptions.end(),
                                               [&](const Subscription& item) {
                                                   return item.id == sub->asString();
                                               }),
                                subscriptions.end());
            enqueue(peer, response(idValue, json::Value::object()));
            return;
        }
        auto result = dispatcher.execute({name, args, peer->origin});
        if (result)
            enqueue(peer, response(idValue, std::move(*result)));
        else {
            auto error = result.error();
            int code = error.code == "command.unknown" || error.code == "command.version" ? -32601
                       : error.code == "command.invalid_args"                             ? -32602
                                                                                          : -32000;
            enqueue(peer, rpcError(idValue, code,
                                   code == -32601   ? "Method not found"
                                   : code == -32602 ? "Invalid params"
                                                    : "Command error",
                                   std::move(error)));
        }
    }
    void io(const std::shared_ptr<Peer>& peer) {
        while (running) {
            // Consume already framed input when a response frees a request slot.
            while (true) {
                std::size_t newline = peer->input.find('\n');
                if (newline == std::string::npos) break;
                {
                    std::lock_guard lock(peer->outputMutex);
                    if (peer->closeAfterWrite || peer->pendingRequests >= 16 ||
                        peer->pendingBytes + newline > 32 * 1024 * 1024 ||
                        peer->outputBytes >= 32 * 1024 * 1024 - 64 * 1024)
                        break;
                    ++peer->pendingRequests;
                    peer->pendingBytes += newline;
                }
                auto frame = peer->input.substr(0, newline);
                peer->input.erase(0, newline + 1);
                if (frame.size() > kMaxMessageBytes) {
                    enqueue(peer, rpcError(nullptr, -32600, "Message too large",
                                           Error("rpc.message_size", "message exceeds 16 MiB")));
                    std::lock_guard lock(peer->outputMutex);
                    --peer->pendingRequests;
                    peer->pendingBytes -= newline;
                    peer->closeAfterWrite = true;
                    break;
                }
                auto weak = weak_from_this();
                post([weak, peer, size = frame.size(), frame = std::move(frame)] {
                    if (auto self = weak.lock(); self && self->running) self->process(peer, frame);
                    std::lock_guard lock(peer->outputMutex);
                    --peer->pendingRequests;
                    peer->pendingBytes -= size;
                });
            }
            short events = 0;
            {
                std::lock_guard lock(peer->outputMutex);
                if (!peer->closeAfterWrite && peer->pendingRequests < 16 &&
                    peer->pendingBytes < 32 * 1024 * 1024 &&
                    peer->outputBytes < 32 * 1024 * 1024 - 64 * 1024)
                    events |= POLLIN;
                if (!peer->sending.empty() || !peer->output.empty()) events |= POLLOUT;
                if (peer->closeAfterWrite && peer->sending.empty() && peer->output.empty()) break;
            }
            net::PollFd pfd{peer->fd, events, 0};
            int ready = net::poll(&pfd, 1, 25);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            if (pfd.revents & POLLIN) {
                char bytes[8192];
                auto count = net::recv(peer->fd, bytes, sizeof(bytes));
                if (count <= 0) {
                    if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    break;
                }
                peer->input.append(bytes, static_cast<std::size_t>(count));
                auto newline = peer->input.find('\n');
                if ((newline == std::string::npos && peer->input.size() > kMaxMessageBytes) ||
                    (newline != std::string::npos && newline > kMaxMessageBytes)) {
                    enqueue(peer, rpcError(nullptr, -32600, "Message too large",
                                           Error("rpc.message_size", "message exceeds 16 MiB")));
                    std::lock_guard lock(peer->outputMutex);
                    peer->closeAfterWrite = true;
                }
            }
            if (pfd.revents & POLLOUT) {
                {
                    std::lock_guard lock(peer->outputMutex);
                    if (peer->sending.empty() && !peer->output.empty()) {
                        peer->sending = std::move(peer->output.front());
                        peer->output.pop_front();
                        peer->sent = 0;
                    }
                }
                {
                    std::lock_guard lock(peer->outputMutex);
                    if (peer->sending.empty()) continue;
                    auto n = net::send(peer->fd, peer->sending.data() + peer->sent,
                                       peer->sending.size() - peer->sent);
                    if (n > 0) {
                        peer->sent += static_cast<std::size_t>(n);
                        peer->outputBytes -= static_cast<std::size_t>(n);
                        if (peer->sent == peer->sending.size()) {
                            peer->sending.clear();
                        }
                    } else if (n < 0 && errno != EAGAIN && errno != EINTR)
                        break;
                }
                if (peer->sending.empty()) dropNotice(peer);
            }
        }
        auto closingFd = peer->fd.exchange(net::invalid);
        if (closingFd != net::invalid) net::close(closingFd);
    }
    void acceptLoop() {
        while (running) {
            net::PollFd pfd{listener, POLLIN, 0};
            int ready = net::poll(&pfd, 1, 50);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) continue;
            if (!(pfd.revents & POLLIN)) continue;
            auto fd = net::accept(listener);
            if (fd == net::invalid) continue;
            std::vector<std::shared_ptr<Peer>> retired;
            {
                std::lock_guard lock(peersMutex);
                auto it = peers.begin();
                while (it != peers.end()) {
                    if ((*it)->fd.load() == net::invalid) {
                        retired.push_back(*it);
                        it = peers.erase(it);
                    } else
                        ++it;
                }
            }
            for (auto& old : retired)
                if (old->thread.joinable()) old->thread.join();
            auto peer = std::make_shared<Peer>();
            peer->fd = fd;
            {
                std::lock_guard lock(peersMutex);
                if (peers.size() >= 64) {
                    net::close(fd);
                    continue;
                }
                peers.push_back(peer);
            }
            peer->thread = std::thread([self = shared_from_this(), peer] { self->io(peer); });
        }
    }
    Status start(const std::filesystem::path& requested) {
        if (running) return Error("rpc.running", "server is already running");
        std::error_code ec;
        auto path = std::filesystem::absolute(requested, ec);
        if (ec) return Error("rpc.path", ec.message(), requested.string());
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return Error("rpc.path", ec.message(), path.string());
        if (path.parent_path().filename() == product::kSlug &&
            net::privatePermissions(path.parent_path(), true) != 0)
            return Error("rpc.path", std::strerror(errno), path.parent_path().string());
        // Serialize stale-path inspection and bind across competing processes.
        auto lockPath = path;
        lockPath += ".lock";
        net::EndpointLock lock(lockPath);
        if (!lock.valid()) return Error("rpc.path", std::strerror(errno), lockPath.string());
        net::Identity existing{};
        if (net::identity(path, existing)) {
            auto probe = net::open();
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            auto name = net::pathName(path);
            if (name.size() >= sizeof(addr.sun_path)) {
                if (probe != net::invalid) net::close(probe);
                return Error("rpc.path", "socket path is too long", name);
            }
            std::memcpy(addr.sun_path, name.c_str(), name.size() + 1);
            int connected = probe != net::invalid ? net::connect(probe, addr) : -1;
            int connectError = errno;
            if (probe != net::invalid) net::close(probe);
            if (connected == 0 || !existing.socket ||
                (connectError != ECONNREFUSED && connectError != ENOENT)) {
                return Error("rpc.address_in_use", "socket address is in use", name,
                             "choose another instance or socket path");
            }
            if (net::remove(path) != 0 && errno != ENOENT) {
                auto message = std::strerror(errno);
                return Error("rpc.path", message, name);
            }
        }
        listener = net::open();
        if (listener == net::invalid) {
            return Error("rpc.socket", std::strerror(errno));
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto pathString = net::pathName(path);
        if (pathString.size() >= sizeof(address.sun_path)) {
            net::close(listener);
            listener = net::invalid;
            return Error("rpc.path", "socket path is too long", pathString);
        }
        std::memcpy(address.sun_path, pathString.c_str(), pathString.size() + 1);
        if (net::bind(listener, address) != 0) {
            int bindError = errno;
            auto message = std::strerror(errno);
            net::close(listener);
            listener = net::invalid;
            return Error(bindError == EADDRINUSE ? "rpc.address_in_use" : "rpc.socket", message,
                         pathString);
        }
        net::identity(path, socketIdentity);
        if (net::listen(listener) != 0) {
            auto message = std::strerror(errno);
            net::close(listener);
            listener = net::invalid;
            net::Identity current{};
            if (net::identity(path, current) && current == socketIdentity) net::remove(path);
            return Error("rpc.socket", message, pathString);
        }
        if (net::privatePermissions(path, false) != 0) {
            auto message = std::strerror(errno);
            net::close(listener);
            listener = net::invalid;
            net::Identity current{};
            if (net::identity(path, current) && current == socketIdentity) net::remove(path);
            return Error("rpc.path", message, pathString);
        }
        socket = path;
        running = true;
        acceptThread = std::thread([self = shared_from_this()] { self->acceptLoop(); });
        return {};
    }
    void stop() {
        if (!running.exchange(false)) return;
        // The accept and peer loops poll with short timeouts. Join before
        // closing descriptors so neither loop observes a reused fd.
        if (acceptThread.joinable()) acceptThread.join();
        net::close(listener);
        listener = net::invalid;
        std::vector<std::shared_ptr<Peer>> copy;
        {
            std::lock_guard lock(peersMutex);
            copy = peers;
            peers.clear();
        }
        for (auto& peer : copy)
            if (peer->thread.joinable()) peer->thread.join();
        net::Identity current{};
        if (net::identity(socket, current) && current == socketIdentity) net::remove(socket);
    }
    void publish(std::string_view topic, const json::Value& payload) {
        auto sequence = ++topicSequence[std::string(topic)];
        std::vector<std::shared_ptr<Peer>> copy;
        {
            std::lock_guard lock(peersMutex);
            copy = peers;
        }
        for (auto& peer : copy)
            if (peer->greeted) {
                for (const auto& sub : peer->subscriptions) {
                    bool matches =
                        std::find(sub.topics.begin(), sub.topics.end(), topic) !=
                            sub.topics.end() ||
                        std::find(sub.topics.begin(), sub.topics.end(), "*") != sub.topics.end();
                    if (!matches) continue;
                    if (sub.filter.isObject() && !sub.filter.asObject().empty()) {
                        if (!payload.isObject()) continue;
                        bool filtered = false;
                        for (const auto& member : sub.filter.asObject()) {
                            auto* actual = payload.find(member.key);
                            if (!actual || *actual != member.value) {
                                filtered = true;
                                break;
                            }
                        }
                        if (filtered) continue;
                    }
                    json::Value event = json::Value::object();
                    event["jsonrpc"] = "2.0";
                    event["method"] = "event";
                    event["params"] = json::Value::object();
                    event["params"]["subscription"] = sub.id;
                    event["params"]["topic"] = std::string(topic);
                    event["params"]["seq"] = sequence;
                    event["params"]["payload"] = payload;
                    enqueue(peer, std::move(event), true);
                }
            }
    }
};
Server::Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher, Post post)
    : impl_(std::make_shared<Impl>(registry, dispatcher, std::move(post))) {}
Server::~Server() { stop(); }
Status Server::start(const std::filesystem::path& path) { return impl_->start(path); }
void Server::stop() { impl_->stop(); }
void Server::publish(std::string_view topic, const json::Value& payload) {
    impl_->publish(topic, payload);
}
std::filesystem::path Server::path() const { return impl_->socket; }

Client::~Client() { close(); }
Status Client::connect(const std::filesystem::path& path, std::string client, std::string agent,
                       bool mcp) {
    close();
    json::Value hello = json::Value::object();
    hello["client"] = std::move(client);
    hello["agent"] = std::move(agent);
    hello["protocol"] = 1;
    hello["mcp"] = mcp;
    path_ = path;
    hello_ = std::move(hello);
    return open(std::chrono::steady_clock::now() + timeout_);
}
Status Client::open(Deadline deadline) {
    drop();
    fd_ = static_cast<std::intptr_t>(net::open());
    if (fd_ == -1) return Error("rpc.socket", std::strerror(errno));
    // Nonblocking, so that no wait outlasts the deadline: a server that stops
    // accepting connections or reading requests would otherwise block forever.
    if (net::nonblocking(static_cast<net::Socket>(fd_)) != 0) {
        auto message = std::strerror(errno);
        drop();
        return Error("rpc.socket", message);
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto pathString = net::pathName(path_);
    if (pathString.size() >= sizeof(address.sun_path)) {
        drop();
        return Error("rpc.path", "socket path is too long", pathString);
    }
    std::memcpy(address.sun_path, pathString.c_str(), pathString.size() + 1);
    const auto socket = static_cast<net::Socket>(fd_);
    auto refused = [&](std::string message) -> Status {
        drop();
        return Error("rpc.connect", std::move(message), pathString);
    };
    using clock = std::chrono::steady_clock;
    constexpr const char* kUnaccepted = "timed out waiting for the server to accept";
    while (net::connect(socket, address) != 0) {
        if (errno == EINTR) continue;
        if (errno == EISCONN) break;  // an interrupted connect() finished meanwhile
        if (errno == EAGAIN) {
            // A full backlog (Linux): the server has not accepted the ones before.
            const auto remaining = deadline - clock::now();
            if (remaining <= clock::duration::zero()) return refused(kUnaccepted);
            std::this_thread::sleep_for(
                std::min<clock::duration>(remaining, std::chrono::milliseconds(5)));
            continue;
        }
        if (errno != EINPROGRESS && errno != EALREADY) return refused(std::strerror(errno));
        // Under way: over when the socket can be written.
        int ready;
        do {
            net::PollFd pfd{socket, POLLOUT, 0};
            ready = net::poll(&pfd, 1, detail::waitMilliseconds(deadline - clock::now()));
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) return refused(std::strerror(errno));
        if (ready == 0) return refused(kUnaccepted);
        if (net::connectError(socket) != 0) return refused(std::strerror(errno));
        break;
    }
    auto result = exchange("rpc.hello", *hello_, deadline);
    if (result) {
        ++connectionGeneration_;
        return {};
    }
    drop();  // a connection without a handshake cannot serve calls
    return result.error();
}
void Client::drop() {
    if (fd_ != -1) net::close(static_cast<net::Socket>(fd_));
    fd_ = -1;
    input_.clear();
    nextId_ = 1;
}
void Client::close() {
    drop();
    path_.clear();
    hello_.reset();
}
std::uint64_t Client::connectionGeneration() {
    if (fd_ != -1) {
        const auto ready = net::peek(static_cast<net::Socket>(fd_));
        if (ready == 0 || (ready < 0 && errno != EAGAIN && errno != EINTR)) drop();
    }
    return fd_ == -1 ? 0 : connectionGeneration_;
}
void Client::setTimeout(std::chrono::milliseconds timeout) {
    timeout_ = std::clamp(timeout, std::chrono::milliseconds(1),
                          std::chrono::milliseconds(std::chrono::hours(24)));
}
Result<json::Value> Client::call(std::string_view method, const json::Value& params) {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    if (fd_ == -1) {
        if (!hello_)
            return Error("rpc.connect", "client is not connected", "", "call connect() first");
        if (auto reopened = open(deadline); !reopened) {
            Error error("rpc.connect", "could not reconnect: " + reopened.error().message,
                        path_.string(), "start the app, then retry the call");
            error.details.push_back(reopened.error());
            return error;
        }
    }
    auto result = exchange(method, params, deadline);
    if (result && method == "rpc.hello") hello_ = params;
    return result;
}
Result<json::Value> Client::exchange(std::string_view method, const json::Value& params,
                                     Deadline deadline) {
    // Any transport failure leaves the stream in an unknown state (a partial
    // request, or a response still on its way): close it.
    auto fail = [this](Error error) -> Result<json::Value> {
        drop();
        return error;
    };
    auto id = nextId_++;
    json::Value request = json::Value::object();
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = std::string(method);
    request["params"] = params;
    auto output = line(request);
    using clock = std::chrono::steady_clock;
    const auto busy = [] { return errno == EAGAIN || errno == EWOULDBLOCK; };
    std::size_t sent = 0;
    while (sent < output.size()) {
        auto n =
            net::send(static_cast<net::Socket>(fd_), output.data() + sent, output.size() - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && busy()) {
            // A line the server never received in full is never run.
            const auto remaining = deadline - clock::now();
            if (remaining <= clock::duration::zero())
                return fail(Error("rpc.timeout", "timed out sending the request", "",
                                  "the server stopped reading, so the call did not run; retry "
                                  "once the app responds"));
            net::PollFd pfd{static_cast<net::Socket>(fd_), POLLOUT, 0};
            const int ready = net::poll(&pfd, 1, detail::waitMilliseconds(remaining));
            if (ready < 0 && errno == EINTR)
                return fail(Error("rpc.interrupted", "a signal interrupted sending the request", "",
                                  "the call did not run; repeat it if it is still wanted"));
            if (ready < 0) return fail(Error("rpc.write", std::strerror(errno)));
            continue;
        }
        if (n <= 0) return fail(Error("rpc.write", std::strerror(errno)));
        sent += static_cast<std::size_t>(n);
    }
    std::size_t scanned = 0;  // input_ before this offset holds no newline
    while (true) {
        auto newline = input_.find('\n', scanned);
        // The size limit applies before parsing, also when the newline came
        // with the chunk that crossed it.
        if ((newline != std::string::npos && newline > kMaxMessageBytes) ||
            (newline == std::string::npos && input_.size() > kMaxMessageBytes))
            return fail(
                Error("rpc.message_size", "response exceeds 16 MiB", "", "narrow the query"));
        if (newline != std::string::npos) {
            auto frame = input_.substr(0, newline);
            input_.erase(0, newline + 1);
            scanned = 0;
            auto parsed = json::parse(frame);
            if (!parsed) return fail(parsed.error());
            auto* responseId = parsed->isObject() ? parsed->find("id") : nullptr;
            if (!responseId) continue;  // subscribed event
            if (!responseId->isInt() || responseId->asInt() != static_cast<std::int64_t>(id))
                return fail(Error("rpc.id", "unexpected response id"));
            if (auto* error = parsed->find("error")) {
                auto* data = error->find("data");
                return data ? errorFromJson(*data) : Error("rpc.remote", "remote command failed");
            }
            auto* result = parsed->find("result");
            if (!result) return fail(Error("rpc.response", "missing result"));
            return Result<json::Value>(*result);
        }
        scanned = input_.size();
        const auto remaining = deadline - clock::now();
        if (remaining <= clock::duration::zero())
            return fail(Error("rpc.timeout", "timed out waiting for response", "",
                              "the call may still have run; check before repeating it"));
        net::PollFd pfd{static_cast<net::Socket>(fd_), POLLIN, 0};
        const int ready = net::poll(&pfd, 1, detail::waitMilliseconds(remaining));
        if (ready < 0 && errno == EINTR)
            return fail(Error("rpc.interrupted", "a signal interrupted the wait for the response",
                              "", "the call may still have run; check before repeating it"));
        if (ready < 0) return fail(Error("rpc.read", std::strerror(errno)));
        if (ready == 0) continue;  // the deadline check above decides
        char bytes[8192];
        auto n = net::recv(static_cast<net::Socket>(fd_), bytes, sizeof(bytes));
        if (n < 0 && (errno == EINTR || busy())) continue;
        if (n <= 0) return fail(Error("rpc.read", "socket closed while waiting for response"));
        input_.append(bytes, static_cast<std::size_t>(n));
    }
}
}  // namespace supple::rpc
