// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#if defined(__linux__)
#include <doctest/doctest.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <supple/product.h>
#include <supple/rpc/model_loop.h>
#include <supple/rpc/protocol.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include "protocol_fixture.h"

using namespace supple;
namespace {
int rawConnect(const std::filesystem::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto name = path.string();
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}
json::Value readFrame(int fd) {
    std::string frame;
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        REQUIRE(::poll(&pfd, 1, kGuardMs) > 0);
        char ch = 0;
        REQUIRE(::recv(fd, &ch, 1, 0) == 1);
        if (ch == '\n') break;
        frame += ch;
    }
    return parse(frame);
}
json::Value readFrameFast(int fd, std::string& pending) {
    while (true) {
        auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            auto frame = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            return parse(frame);
        }
        pollfd pfd{fd, POLLIN, 0};
        REQUIRE(::poll(&pfd, 1, kGuardMs) > 0);
        char bytes[8192];
        auto count = ::recv(fd, bytes, sizeof(bytes), 0);
        REQUIRE(count > 0);
        pending.append(bytes, static_cast<std::size_t>(count));
    }
}
json::Value exchange(int fd, std::string request) {
    request += '\n';
    REQUIRE(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size()));
    return readFrame(fd);
}
// Counts the server's posted requests that have not started on the model
// thread yet (never more than the server's own outstanding count, which also
// includes the running request), and the most seen at once.
struct QueuedPosts {
    Probe probe;
    std::size_t queued = 0, most = 0;
    rpc::Post into(ModelQueue& queue) {
        return [this, &queue](std::function<void()> task) {
            probe.update([&] { most = std::max(most, ++queued); });
            queue.post([this, task = std::move(task)] {
                probe.update([&] { --queued; });
                task();
            });
        };
    }
};
std::filesystem::path testSocket(std::string_view name) {
    return std::filesystem::temp_directory_path() / std::string(product::kSlug) /
           (std::string(name) + "-" + std::to_string(::getpid()) + ".sock");
}
// Non-asserting I/O for a scripted fake server on its own thread (doctest's
// REQUIRE must not throw there).
std::optional<std::string> readLine(int fd, std::string& pending) {
    while (true) {
        auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            auto frame = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            return frame;
        }
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, kGuardMs) <= 0) return std::nullopt;
        char bytes[65536];
        auto count = ::recv(fd, bytes, sizeof(bytes), 0);
        if (count <= 0) return std::nullopt;
        pending.append(bytes, static_cast<std::size_t>(count));
    }
}
bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
// Waits for the peer to close (EOF), with the guard.
bool peerClosed(int fd) {
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, kGuardMs) <= 0) return false;
        char bytes[65536];
        auto count = ::recv(fd, bytes, sizeof(bytes), 0);
        if (count == 0) return true;
        if (count < 0 && errno != EINTR) return errno == ECONNRESET;
    }
}
std::int64_t frameId(const std::string& frame) {
    auto parsed = json::parse(frame);
    auto* id = parsed && parsed->isObject() ? parsed->find("id") : nullptr;
    return id && id->isInt() ? id->asInt() : -1;
}
void onTestSignal(int) {}
}  // namespace
TEST_CASE("socket JSON-RPC handshake, dispatch, errors, events and permissions") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    host.events = [&](std::string_view topic, const json::Value& payload) {
        server.publish(topic, payload);
    };
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("test-" + std::to_string(::getpid()) + ".sock");
    auto started = server.start(path);
    if (!started) MESSAGE(started.error().toString());
    REQUIRE(started);
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    rpc::Client client;
    REQUIRE(client.connect(path, "test-client", "agent-1"));
    // Both the listener and the accepted connection must close on exec:
    // plugin bridge/scanner children must not keep the control socket alive.
    int serverSockets = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const int descriptor = std::stoi(entry.path().filename().string());
        sockaddr_un address{};
        socklen_t size = sizeof(address);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0 &&
            address.sun_family == AF_UNIX && address.sun_path == path.string()) {
            ++serverSockets;
            CHECK((::fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0);
        }
    }
    CHECK(serverSockets == 2);
    auto added = client.call("counter.add", parse(R"({"value":3})"));
    REQUIRE(added);
    CHECK(added->find("count")->asInt() == 3);
    auto unknown = client.call("counter.missing");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "command.unknown");
    auto invalid = client.call("counter.add", parse(R"({"value":"wrong"})"));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "command.invalid_args");
    int fd = rawConnect(path);
    auto premature =
        exchange(fd, R"({"jsonrpc":"2.0","id":1,"method":"counter.add","params":{"value":1}})");
    CHECK(premature.find("error")->find("data")->find("code")->asString() == "rpc.handshake");
    auto hello = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":2,"method":"rpc.hello","params":{"client":"raw","protocol":1}})");
    CHECK(hello.find("result")->find("protocol")->asInt() == 1);
    auto batch = exchange(fd, R"([{"jsonrpc":"2.0","id":3,"method":"counter.add"}])");
    CHECK(batch.find("error")->find("code")->asInt() == -32600);
    auto malformed = exchange(fd, "{");
    CHECK(malformed.find("error")->find("code")->asInt() == -32700);
    for (const auto& frame : {"42", "null"}) {
        auto invalidFrame = exchange(fd, frame);
        CHECK(invalidFrame.find("error")->find("code")->asInt() == -32600);
        CHECK(invalidFrame.find("id")->isNull());
    }
    for (const auto& params : {"[]", "7"}) {
        auto invalidParams = exchange(
            fd, std::string(R"({"jsonrpc":"2.0","id":7,"method":"counter.add","params":)") +
                    params + "}");
        CHECK(invalidParams.find("error")->find("code")->asInt() == -32602);
        CHECK(invalidParams.find("error")->find("data")->find("code")->asString() ==
              "rpc.invalid_params");
    }
    auto sub = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":4,"method":"rpc.subscribe","params":{"topics":["session.changed"],"filter":{"branch":"main"}}})");
    CHECK(sub.find("result")->find("subscription")->asString() == "sub_1");
    queue.post(
        [&] { server.publish("session.changed", parse(R"({"branch":"main","version":2})")); });
    auto event = readFrame(fd);
    CHECK(event.find("method")->asString() == "event");
    CHECK(event.find("params")->find("topic")->asString() == "session.changed");
    CHECK(event.find("params")->find("seq")->asInt() == 1);
    auto unsub = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":5,"method":"rpc.unsubscribe","params":{"subscription":"sub_1"}})");
    CHECK(unsub.find("result") != nullptr);
    auto bulk = exchange(
        fd, R"({"jsonrpc":"2.0","id":6,"method":"rpc.subscribe","params":{"topics":["bulk"]}})");
    CHECK(bulk.find("result") != nullptr);
    std::promise<void> published;
    auto publishedFuture = published.get_future();
    queue.post([&] {
        json::Value payload = json::Value::object();
        payload["data"] = std::string(8192, 'x');
        for (int i = 0; i < 512; ++i) server.publish("bulk", payload);
        published.set_value();
    });
    publishedFuture.wait();
    std::string pending;
    bool dropped = false;
    for (int i = 0; i < 512; ++i) {
        auto message = readFrameFast(fd, pending);
        if (message.find("method") && message.find("method")->asString() == "event.dropped") {
            CHECK(message.find("params")->find("count")->asInt() > 0);
            dropped = true;
            break;
        }
    }
    CHECK(dropped);
    auto large = client.call("counter.blob", parse(R"({"value":5242880})"));
    REQUIRE(large);
    CHECK(large->find("data")->asString().size() == 5242880);
    auto tooLarge = client.call("counter.blob", parse(R"({"value":17825792})"));
    REQUIRE_FALSE(tooLarge);
    CHECK(tooLarge.error().code == "rpc.response_too_large");
    int slow = rawConnect(path);
    auto slowHello = exchange(
        slow,
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"slow","protocol":1}})");
    CHECK(slowHello.find("result") != nullptr);
    std::string slowRequest =
        R"({"jsonrpc":"2.0","id":2,"method":"counter.blob","params":{"value":5242880}})";
    slowRequest += '\n';
    REQUIRE(::send(slow, slowRequest.data(), slowRequest.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(slowRequest.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string slowPending;
    auto slowResult = readFrameFast(slow, slowPending);
    CHECK(slowResult.find("result")->find("data")->asString().size() == 5242880);
    ::close(slow);
    ::close(fd);
    client.close();
    server.stop();
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("socket ownership survives competing bind and backpressure bounds posted work") {
    auto commands = registry();
    Host firstHost, secondHost;
    cmd::Dispatcher firstDispatcher(commands, firstHost), secondDispatcher(commands, secondHost);
    QueuedPosts posts;
    ModelQueue firstQueue, secondQueue;
    rpc::Server first(commands, firstDispatcher, posts.into(firstQueue));
    rpc::Server second(commands, secondDispatcher,
                       [&](auto task) { secondQueue.post(std::move(task)); });
    auto path = testSocket("race");
    Status a, b;
    std::thread left([&] { a = first.start(path); });
    std::thread right([&] { b = second.start(path); });
    left.join();
    right.join();
    CHECK(static_cast<bool>(a) != static_cast<bool>(b));
    CHECK((a ? b.error().code : a.error().code) == "rpc.address_in_use");
    rpc::Client client;
    REQUIRE(client.connect(path, "race-test"));
    auto result = client.call("counter.add", parse(R"({"value":1})"));
    REQUIRE(result);
    CHECK(result->find("count")->asInt() == 1);
    client.close();
    first.stop();
    second.stop();
    CHECK_FALSE(std::filesystem::exists(path));

    // Hold the model thread while the I/O thread reads 64 pipelined calls: it
    // posts up to its limit of 16 outstanding requests, then stops reading.
    REQUIRE(first.start(path));
    int fd = rawConnect(path);
    auto hello = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"raw","protocol":1}})");
    (void)hello;
    std::promise<void> hold;
    auto blocked = hold.get_future().share();
    firstQueue.post([blocked] { blocked.wait(); });
    {
        std::lock_guard lock(posts.probe.mutex);
        posts.most = posts.queued;
    }
    std::string batch;
    for (int i = 0; i < 64; ++i)
        batch += R"({"jsonrpc":"2.0","id":2,"method":"counter.add","params":{"value":1}})"
                 "\n";
    REQUIRE(::send(fd, batch.data(), batch.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(batch.size()));
    const bool reachedLimit = posts.probe.waitFor([&] { return posts.queued >= 16; });
    // Detection only: a broken limit gets a moment to post more. A slow machine
    // can make this check pass, never fail.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    hold.set_value();
    CHECK(reachedLimit);
    std::string pending;
    for (int i = 0; i < 64; ++i) CHECK(readFrameFast(fd, pending).find("result") != nullptr);
    {
        std::lock_guard lock(posts.probe.mutex);
        CHECK(posts.most == 16);
    }
    ::close(fd);
    first.stop();
}

TEST_CASE("large pipelined frames stop at the per-peer posted byte limit") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    QueuedPosts posts;
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, posts.into(queue));
    auto path = testSocket("large");
    REQUIRE(server.start(path));
    int fd = rawConnect(path);
    auto hello = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"large","protocol":1}})");
    REQUIRE(hello.find("result") != nullptr);
    std::promise<void> release;
    auto gate = release.get_future().share();
    queue.post([gate] { gate.wait(); });
    std::string frame =
        R"({"jsonrpc":"2.0","id":2,"method":"counter.add","params":{"value":1,"pad":")";
    frame += std::string(10 * 1024 * 1024, 'x');
    frame += R"("}})"
             "\n";
    // Four 10 MiB frames: three fit the 32 MiB posted-byte limit, the fourth
    // is read but waits in the peer's input.
    std::promise<bool> sendDone;
    auto sent = sendDone.get_future();
    std::thread sender([&] {
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) ok = sendAll(fd, frame);
        sendDone.set_value(ok);
    });
    const bool reachedLimit = posts.probe.waitFor([&] { return posts.queued >= 3; });
    const bool sentAll = sent.wait_for(kGuard) == std::future_status::ready && sent.get();
    // Detection only, as above.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.set_value();
    sender.join();
    CHECK(reachedLimit);
    CHECK(sentAll);
    {
        std::lock_guard lock(posts.probe.mutex);
        CHECK(posts.most == 3);
    }
    ::close(fd);
    server.stop();
}

TEST_CASE("structured errors keep their text locations across RPC") {
    Error source("test.located", "located problem", "/text", "fix line 3");
    source.loc = SourceLoc{3, 14};
    Error inner("test.detail", "inner problem");
    inner.loc = SourceLoc{4, 2};
    source.details.push_back(inner);
    auto decoded = rpc::errorFromJson(rpc::errorJson(source));
    REQUIRE(decoded.loc);
    CHECK(decoded.loc->line == 3);
    CHECK(decoded.loc->column == 14);
    REQUIRE(decoded.details.size() == 1);
    REQUIRE(decoded.details[0].loc);
    CHECK(decoded.details[0].loc->line == 4);
    CHECK(decoded.details[0].loc->column == 2);
    CHECK_FALSE(rpc::errorFromJson(rpc::errorJson(Error("test.plain", "no location"))).loc);
    // Partial or invalid positions are dropped, never invented.
    for (const char* text :
         {R"({"code":"x","line":3})", R"({"code":"x","column":3})",
          R"({"code":"x","line":"3","column":1})", R"({"code":"x","line":-1,"column":1})",
          R"({"code":"x","line":3,"column":4294967296})", R"({"code":"x","line":1.5,"column":2})"})
        CHECK_FALSE(rpc::errorFromJson(parse(text)).loc);

    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    auto path = testSocket("loc");
    REQUIRE(server.start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "loc-client"));
    auto failed = client.call("counter.fail", parse(R"({"value":0})"));
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "test.located");
    CHECK(failed.error().path == "/args/text");
    REQUIRE(failed.error().loc);
    CHECK(failed.error().loc->line == 3);
    CHECK(failed.error().loc->column == 14);
    REQUIRE(failed.error().details.size() == 1);
    REQUIRE(failed.error().details[0].loc);
    CHECK(failed.error().details[0].loc->line == 4);
    // A structured command error keeps the connection.
    auto added = client.call("counter.add", parse(R"({"value":2})"));
    REQUIRE(added);
    CHECK(added->find("count")->asInt() == 2);
    client.close();
    server.stop();
}

// The model thread is held until the call has returned, so the call can only
// end by timing out; no wall-clock threshold decides the result.
TEST_CASE("a timed-out call closes the connection and the next call reconnects") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    auto path = testSocket("timeout");
    REQUIRE(server.start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "timeout-client", "agent-3"));
    std::promise<void> release;
    auto gate = release.get_future().share();
    queue.post([gate] { gate.wait(); });
    client.setTimeout(std::chrono::milliseconds(20));
    auto late = client.call("counter.add", parse(R"({"value":3})"));
    release.set_value();
    REQUIRE_FALSE(late);
    CHECK(late.error().code == "rpc.timeout");
    client.setTimeout(kGuard);
    // Not the late response to the first call (that one went to the closed
    // connection), and the first call still ran before it.
    auto next = client.call("counter.add", parse(R"({"value":1})"));
    REQUIRE(next);
    CHECK(next->find("count")->asInt() == 4);
    std::promise<cmd::Origin> origin;
    queue.post([&] { origin.set_value(host.last); });
    auto seen = origin.get_future().get();
    CHECK(seen.client == "timeout-client");  // the reconnect repeated the handshake
    CHECK(seen.agent == "agent-3");
    client.close();  // forgets the endpoint
    auto closed = client.call("counter.add", parse(R"({"value":1})"));
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code == "rpc.connect");
    server.stop();
    rpc::Client down;
    CHECK_FALSE(down.connect(path, "late-client"));
    auto unreachable = down.call("counter.add", parse(R"({"value":1})"));
    REQUIRE_FALSE(unreachable);
    CHECK(unreachable.error().code == "rpc.connect");  // a failed reconnect sends nothing
}

TEST_CASE("a signal interrupting a call closes the connection and the next call reconnects") {
    struct sigaction action{}, previous{};
    action.sa_handler = onTestSignal;  // no SA_RESTART: poll returns EINTR
    sigemptyset(&action.sa_mask);
    REQUIRE(::sigaction(SIGUSR1, &action, &previous) == 0);
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    auto path = testSocket("interrupt");
    REQUIRE(server.start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "interrupt-client"));
    std::promise<void> release;
    auto gate = release.get_future().share();
    queue.post([gate] { gate.wait(); });
    const auto beforeRequest = queue.postCount();
    std::optional<Result<json::Value>> interrupted;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        interrupted = client.call("counter.add", parse(R"({"value":3})"));
        done = true;
    });
    // Interrupt the response wait only after the server has queued add-3.
    // Otherwise a signal can interrupt the send and the counter stays at zero.
    const bool queued = queue.waitForPostAfter(beforeRequest);
    if (!queued) {
        release.set_value();
        caller.join();
        ::sigaction(SIGUSR1, &previous, nullptr);
        REQUIRE(queued);
    }
    // The response cannot arrive while the model thread is held, so only a
    // signal ends the wait; keep signalling until one lands in it.
    while (!done) {
        ::pthread_kill(caller.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    caller.join();
    release.set_value();
    ::sigaction(SIGUSR1, &previous, nullptr);
    REQUIRE(interrupted);
    REQUIRE_FALSE(*interrupted);
    CHECK(interrupted->error().code == "rpc.interrupted");
    auto next = client.call("counter.add", parse(R"({"value":1})"));
    REQUIRE(next);
    CHECK(next->find("count")->asInt() == 4);
    client.close();
    server.stop();
}

// A scripted fake server sends a response whose frame is one byte over the
// limit, ending in the same chunk as its newline: the client refuses it before
// parsing, closes, and its next call reconnects with the last accepted hello.
TEST_CASE("an oversized response frame is refused before parsing and the client reconnects") {
    auto path = testSocket("oversized");
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);
    int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto name = path.string();
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 4) == 0);
    std::string prefix = R"({"jsonrpc":"2.0","id":3,"result":{"data":")";
    std::string suffix = R"("}})";
    std::string oversized =
        prefix + std::string(rpc::kMaxMessageBytes + 1 - prefix.size() - suffix.size(), 'x') +
        suffix;
    REQUIRE(oversized.size() == rpc::kMaxMessageBytes + 1);
    oversized += '\n';
    bool script = false, firstClosed = false;
    std::string rehello;
    std::thread fake([&] {
        auto reply = [](std::int64_t id, const char* result) {
            return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":)" + result +
                   "}\n";
        };
        int first = ::accept(listener, nullptr, nullptr);
        if (first < 0) return;
        std::string pending;
        auto hello = readLine(first, pending);
        auto explicitHello = hello && sendAll(first, reply(frameId(*hello), "{}"))
                                 ? readLine(first, pending)
                                 : std::nullopt;
        auto call = explicitHello && sendAll(first, reply(frameId(*explicitHello), "{}"))
                        ? readLine(first, pending)
                        : std::nullopt;
        if (call && frameId(*call) == 3) {
            (void)sendAll(first, oversized);  // the client may close before the newline
            firstClosed = peerClosed(first);
        }
        ::close(first);
        int second = ::accept(listener, nullptr, nullptr);
        if (second < 0) return;
        pending.clear();
        auto again = readLine(second, pending);
        if (again) {
            auto parsed = json::parse(*again);
            auto* params = parsed && parsed->isObject() ? parsed->find("params") : nullptr;
            auto* client = params && params->isObject() ? params->find("client") : nullptr;
            if (client && client->isString()) rehello = client->asString();
        }
        auto next = again && sendAll(second, reply(frameId(*again), "{}"))
                        ? readLine(second, pending)
                        : std::nullopt;
        script = next && frameId(*next) == 2 && sendAll(second, reply(2, R"({"ok":true})")) &&
                 peerClosed(second);
        ::close(second);
    });
    rpc::Client client;
    CHECK(client.connect(path, "fake-client", "agent-7"));
    auto renamed = client.call(
        "rpc.hello", parse(R"({"client":"renamed","agent":"agent-7","protocol":1,"mcp":false})"));
    CHECK(renamed);
    auto big = client.call("counter.blob", parse(R"({"value":1})"));
    CHECK_FALSE(big);
    if (!big) CHECK(big.error().code == "rpc.message_size");
    auto again = client.call("counter.add", parse(R"({"value":1})"));
    CHECK(again);
    if (again) CHECK(again->find("ok")->asBool());
    client.close();
    fake.join();
    ::close(listener);
    std::filesystem::remove(path);
    CHECK(firstClosed);
    CHECK(rehello == "renamed");
    CHECK(script);
}

// A raw listener at `path`, never accepting unless the test does.
int fakeListener(const std::filesystem::path& path, int backlog) {
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);
    int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto name = path.string();
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        ::listen(listener, backlog)) {
        if (listener >= 0) ::close(listener);
        return -1;
    }
    return listener;
}

TEST_CASE("a server that stops reading ends a call's write at its timeout") {
    auto path = testSocket("stalled");
    int listener = fakeListener(path, 4);
    REQUIRE(listener >= 0);
    std::promise<void> done;
    auto finished = done.get_future();
    std::thread fake([&] {
        int peer = ::accept(listener, nullptr, nullptr);
        if (peer < 0) return;
        std::string pending;
        if (auto hello = readLine(peer, pending))
            (void)sendAll(peer, R"({"jsonrpc":"2.0","id":)" + std::to_string(frameId(*hello)) +
                                    R"(,"result":{}})" + "\n");
        finished.wait();  // reading nothing more
        ::close(peer);
    });
    rpc::Client client;
    REQUIRE(client.connect(path, "stalled-client"));
    client.setTimeout(std::chrono::milliseconds(100));
    json::Value params = json::Value::object();
    params["data"] = std::string(8 << 20, 'x');  // far more than the socket buffers hold
    auto stalled = client.call("counter.blob", params);
    done.set_value();
    fake.join();
    REQUIRE_FALSE(stalled);
    CHECK(stalled.error().code == "rpc.timeout");
    CHECK(stalled.error().message.find("sending") != std::string::npos);
    ::close(listener);
    std::filesystem::remove(path);
}

TEST_CASE("a server that accepts no connections ends a connect at its timeout") {
    auto path = testSocket("backlog");
    int listener = fakeListener(path, 0);
    REQUIRE(listener >= 0);
    // Fill the backlog: the listener accepts nothing.
    std::vector<int> waiting;
    for (int i = 0; i < 64; ++i) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto name = path.string();
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        const bool queued =
            ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        waiting.push_back(fd);
        if (!queued) break;
    }
    rpc::Client client;
    client.setTimeout(std::chrono::milliseconds(100));
    auto refused = client.connect(path, "late-client");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "rpc.connect");
    for (int fd : waiting) ::close(fd);
    ::close(listener);
    std::filesystem::remove(path);
}

// Embedded in another loop (the GUI's): pump() runs what is due without
// blocking and reports the next deadline; the waker fires for posts and timer
// changes from any thread, so the owner's wait can be interrupted.
TEST_CASE("model-loop pump runs due work without blocking and wakes its owner") {
    rpc::ModelLoop loop;
    std::atomic<int> wakes{0};
    loop.setWaker([&] { ++wakes; });
    CHECK(loop.pump() == -1);  // nothing scheduled
    int ran = 0;
    std::thread poster([&] { loop.post([&] { ++ran; }); });
    poster.join();
    CHECK(wakes.load() >= 1);
    CHECK(loop.pump() == -1);
    CHECK(ran == 1);
    // A timer an hour away is not due, and bounds the owner's wait.
    int distantTicks = 0;
    const int before = wakes.load();
    auto distant = loop.every(std::chrono::hours(1), [&] { ++distantTicks; });
    CHECK(wakes.load() > before);
    const int next = loop.pump();
    CHECK(next > 0);
    CHECK(next <= 3600 * 1000);
    CHECK(distantTicks == 0);
    // sleep_for waits at least its duration, so the 1 ms timer is due after it.
    int ticks = 0;
    auto soon = loop.every(std::chrono::milliseconds(1), [&] { ++ticks; });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(loop.pump() >= 0);
    CHECK(ticks == 1);
    CHECK(distantTicks == 0);
    soon.cancel();
    distant.cancel();
    CHECK(loop.pump() == -1);
    CHECK_FALSE(loop.stopRequested());
}

TEST_CASE("model-loop waits round up and huge intervals saturate") {
    using namespace std::chrono;
    CHECK(rpc::detail::waitMilliseconds(nanoseconds(-5)) == 0);
    CHECK(rpc::detail::waitMilliseconds(steady_clock::duration::zero()) == 0);
    CHECK(rpc::detail::waitMilliseconds(nanoseconds(1)) == 1);  // not a 0 ms busy poll
    CHECK(rpc::detail::waitMilliseconds(microseconds(999)) == 1);
    CHECK(rpc::detail::waitMilliseconds(milliseconds(1)) == 1);
    CHECK(rpc::detail::waitMilliseconds(microseconds(1001)) == 2);
    CHECK(rpc::detail::waitMilliseconds(hours(24 * 365)) == INT_MAX);
    CHECK(rpc::detail::waitMilliseconds(steady_clock::duration::max()) == INT_MAX);
    rpc::ModelLoop loop;
    int ran = 0;
    auto never = loop.every(milliseconds::max(), [&] { ++ran; });
    CHECK(loop.pump() == INT_MAX);
    CHECK(ran == 0);
    auto clamped = loop.every(milliseconds(-5), [&] { ++ran; });  // runs every 1 ms
    const int wait = loop.pump();
    CHECK(wait >= 0);
    CHECK(wait <= 1);
}

// The cancellation guarantee on the model thread: a callback that cancels (or
// destroys) another timer that is due in the same pass stops it from running.
TEST_CASE("a timer cancelled by another callback in the same pass does not run") {
    rpc::ModelLoop loop;
    int firstRuns = 0, secondRuns = 0;
    std::optional<rpc::Timer> second;
    auto first = loop.every(std::chrono::milliseconds(1), [&] {
        ++firstRuns;
        second.reset();
    });
    second = loop.every(std::chrono::milliseconds(1), [&] { ++secondRuns; });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // both are due
    loop.pump();
    CHECK(firstRuns == 1);
    CHECK(secondRuns == 0);
    // A posted task cancelling a due timer acts before the next pass too.
    int thirdRuns = 0;
    auto third = loop.every(std::chrono::milliseconds(1), [&] { ++thirdRuns; });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    loop.post([&] { third.cancel(); });
    loop.pump();  // timers run before posted tasks: this pass still runs it
    const int afterCancel = thirdRuns;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    loop.pump();
    CHECK(thirdRuns == afterCancel);
}

// The service timers run on the model thread between posted tasks, without a
// busy wait (the loop's poll gets the nearest deadline as its timeout). Every
// wait is a handshake on the callbacks' progress.
TEST_CASE("model-loop timers run on the model thread in order and stop on cancel") {
    rpc::ModelLoop loop;
    Probe probe;
    int fast = 0, slow = 0;
    bool wrongThread = false, taskRan = false;
    std::vector<std::string> order;
    std::thread::id model;
    auto record = [&](const char* what, int* counter) {
        probe.update([&] {
            if (std::this_thread::get_id() != model) wrongThread = true;
            order.emplace_back(what);
            if (counter) ++*counter;
        });
    };
    std::thread runner([&] {
        model = std::this_thread::get_id();
        loop.run();
    });
    struct Stop {  // ends the loop (its wait ends on a signal) on every path
        std::thread& runner;
        ~Stop() {
            ::raise(SIGTERM);
            runner.join();
        }
    } stop{runner};
    loop.post([&] {  // a posted task shares the thread with the timers
        record("task", nullptr);
        probe.update([&] { taskRan = true; });
    });
    REQUIRE(probe.waitFor([&] { return taskRan; }));
    auto fastTimer = loop.every(std::chrono::milliseconds(10), [&] { record("timer-10", &fast); });
    auto slowTimer = loop.every(std::chrono::milliseconds(30), [&] { record("timer-30", &slow); });
    REQUIRE(probe.waitFor([&] { return fast >= 4 && slow >= 2; }));
    {
        std::lock_guard lock(probe.mutex);
        CHECK_FALSE(wrongThread);
        CHECK(order.front() == "task");
        // The fast timer is due first; when both are due in one pass, the
        // earlier registration runs first.
        const auto firstFast = std::find(order.begin(), order.end(), "timer-10");
        const auto firstSlow = std::find(order.begin(), order.end(), "timer-30");
        CHECK(firstFast < firstSlow);
    }
    // Cancelling from this thread: at most one invocation already under way
    // may still complete.
    fastTimer.cancel();
    CHECK_FALSE(fastTimer.active());
    int fastAtCancel = 0, slowAtCancel = 0, slowNow = 0;
    probe.update([&] {
        fastAtCancel = fast;
        slowNow = slow;
    });
    REQUIRE(probe.waitFor([&] { return slow >= slowNow + 2; }));  // the loop kept running
    slowTimer.cancel();
    CHECK_FALSE(slowTimer.active());
    probe.update([&] { slowAtCancel = slow; });
    loop.post([&] { probe.update([&] { taskRan = false; }); });
    REQUIRE(probe.waitFor([&] { return !taskRan; }));  // a pass after both cancels
    std::lock_guard lock(probe.mutex);
    CHECK(fast <= fastAtCancel + 1);
    CHECK(slow <= slowAtCancel + 1);
}

TEST_CASE("one-shot deadlines expire once and cancel other due owners") {
    rpc::ModelLoop loop;
    int calls = 0;
    rpc::Timer second;
    auto first = loop.at(std::chrono::steady_clock::time_point::min(), [&] {
        ++calls;
        second.cancel();
    });
    second = loop.at(std::chrono::steady_clock::time_point::min(), [&] { calls += 100; });
    CHECK(loop.pump() == -1);
    CHECK(calls == 1);
    CHECK_FALSE(first.active());
    CHECK_FALSE(second.active());
    CHECK(loop.pump() == -1);
    CHECK(calls == 1);
}

TEST_CASE("fd watches cancel ready snapshots before owners or descriptors are reused") {
    rpc::ModelLoop loop;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    char byte = 1;
    REQUIRE(::write(fds[1], &byte, 1) == 1);
    int calls = 0;
    rpc::FdWatch second, replacement;
    auto first = loop.watch(fds[0], POLLIN, [&](short ready) {
        CHECK((ready & POLLIN) != 0);
        ++calls;
        second.cancel();
        replacement = loop.watch(fds[0], POLLIN, [&](short) {
            REQUIRE(::read(fds[0], &byte, 1) == 1);
            calls += 10;
        });
    });
    second = loop.watch(fds[0], POLLIN, [&](short) { calls += 100; });
    loop.pump();
    CHECK(calls == 1);
    first.cancel();
    loop.pump();
    CHECK(calls == 11);
    replacement.cancel();
    CHECK(loop.pump() == -1);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("embedded loop wake fd covers posted work and idle shutdown signals") {
    rpc::ModelLoop loop;
    REQUIRE(loop.wakeFd() >= 0);
    pollfd watched{loop.wakeFd(), POLLIN, 0};
    CHECK(::poll(&watched, 1, 0) == 0);
    int calls = 0;
    loop.post([&] { ++calls; });
    CHECK(::poll(&watched, 1, 0) == 1);
    CHECK(loop.pump() == -1);
    CHECK(calls == 1);
    CHECK(::poll(&watched, 1, 0) == 0);
    REQUIRE(::raise(SIGTERM) == 0);
    CHECK(::poll(&watched, 1, 0) == 1);
    CHECK(loop.stopRequested());
    CHECK(loop.pump() == -1);
}

#endif  // Direct Linux descriptor/permission/signal probes; portable coverage is in
        // test_platform.cpp.
