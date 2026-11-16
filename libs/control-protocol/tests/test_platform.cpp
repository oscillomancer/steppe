// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/base/waitable.h>
#include <supple/rpc/model_loop.h>
#include <array>
#include <cstring>
#include "../src/socket_platform.h"
#include "protocol_fixture.h"

TEST_CASE("malformed request ids are never echoed into portable RPC error envelopes") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, bus, [&](auto task) { queue.post(std::move(task)); });
    std::filesystem::path dir;
    for (std::uint64_t suffix = 0;; ++suffix) {
        dir = std::filesystem::temp_directory_path() / ("rpc-boundary-" + std::to_string(suffix));
        std::error_code error;
        if (std::filesystem::create_directory(dir, error)) break;
        REQUIRE((!error || error == std::errc::file_exists));
    }
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    const auto path = dir / "endpoint.sock";
    REQUIRE(server.start(path));
    struct Socket {
        rpc::net::Socket fd = rpc::net::open();
        ~Socket() {
            if (fd != rpc::net::invalid) rpc::net::close(fd);
        }
    } socket;
    REQUIRE(socket.fd != rpc::net::invalid);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = rpc::net::pathName(path);
    REQUIRE(name.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    REQUIRE(rpc::net::connect(socket.fd, address) == 0);
    auto exchange = [&](const std::string& frame) {
        const auto line = frame + '\n';
        std::size_t sent = 0;
        while (sent < line.size()) {
            auto n = rpc::net::send(socket.fd, line.data() + sent, line.size() - sent);
            REQUIRE(n > 0);
            sent += static_cast<std::size_t>(n);
        }
        std::string response;
        while (response.find('\n') == std::string::npos) {
            rpc::net::PollFd item{socket.fd, POLLIN, 0};
            REQUIRE_MESSAGE(rpc::net::poll(&item, 1, kGuardMs) > 0,
                            "WATCHDOG ABORT waiting for RPC reply");
            char bytes[4096];
            const auto n = rpc::net::recv(socket.fd, bytes, sizeof(bytes));
            REQUIRE(n > 0);
            response.append(bytes, static_cast<std::size_t>(n));
        }
        return parse(response);
    };
    for (const auto* id : {"true", "[]", "{}", "1.5"}) {
        const auto reply =
            exchange(std::string(R"({"jsonrpc":"2.0","method":"rpc.hello","id":)") + id + "}");
        CHECK(reply.find("id")->isNull());
        CHECK(reply.find("error")->find("code")->asInt() == -32600);
        const auto* detail = reply.find("error")->find("data");
        for (const char* key : {"code", "message", "path", "hint"}) {
            REQUIRE(detail->find(key));
            CHECK(detail->find(key)->isString());
        }
    }
    const auto reply = exchange(R"({"jsonrpc":"1.0","method":"rpc.hello","id":"valid-id"})");
    CHECK(*reply.find("id") == "valid-id");
    for (const auto& [id, fits] :
         std::vector<std::pair<std::string, bool>>{{std::string(1022, 'i'), true},
                                                   {std::string(1023, 'i'), false},
                                                   {std::string(511, '\n'), true},
                                                   {std::string(512, '\n'), false},
                                                   {std::string(170, '\0'), true},
                                                   {std::string(171, '\0'), false},
                                                   {std::string(65536, 'i'), false}}) {
        auto frame = parse(
            R"({"jsonrpc":"2.0","method":"rpc.hello","params":{"client":"id-limit","protocol":1}})");
        frame["id"] = id;
        const auto result = exchange(json::writeCompact(frame));
        if (fits) {
            CHECK(*result.find("id") == id);
            CHECK(result.find("result"));
        } else {
            CHECK(result.find("id")->isNull());
            REQUIRE(result.find("error"));
            CHECK(result.find("error")->find("data")->find("code")->asString() == "rpc.id_size");
            CHECK(json::writeCompact(result).size() < 1024);
        }
    }
    server.stop();
    CHECK(host.count == 0);
}

TEST_CASE("portable startup refuses an occupied socket even with an undrained backlog") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    rpc::Server server(commands, bus, [](auto) {});
    std::filesystem::path dir;
    for (std::uint64_t suffix = 0;; ++suffix) {
        dir = std::filesystem::temp_directory_path() / ("rpc-backlog-" + std::to_string(suffix));
        std::error_code error;
        if (std::filesystem::create_directory(dir, error)) break;
        REQUIRE((!error || error == std::errc::file_exists));
    }
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    struct Sockets {
        std::vector<rpc::net::Socket> handles;
        ~Sockets() {
            for (auto fd : handles) rpc::net::close(fd);
        }
        rpc::net::Socket add() {
            const auto fd = rpc::net::open();
            REQUIRE(fd != rpc::net::invalid);
            handles.push_back(fd);
            return fd;
        }
    } sockets;
    const auto path = dir / "endpoint.sock";
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = rpc::net::pathName(path);
    REQUIRE(name.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    const auto listener = sockets.add();
    REQUIRE(rpc::net::bind(listener, address) == 0);
    REQUIRE(rpc::net::listen(listener) == 0);
    // The synthetic listener never accepts. On Linux this fills its 16-slot
    // backlog; other platforms can report a pending connect earlier. Neither
    // outcome proves an endpoint stale, so startup must conservatively refuse.
    for (int i = 0; i < 64; ++i) {
        const auto client = sockets.add();
        REQUIRE(rpc::net::nonblocking(client) == 0);
        (void)rpc::net::connect(client, address);
    }
    // An external test watchdog may abort a broken blocking implementation;
    // elapsed time is never used to accept or reject endpoint ownership.
    const auto result = server.start(path);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "rpc.address_in_use");
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("model loop portable waitable snapshots respect model-thread cancellation") {
    rpc::ModelLoop loop;
    WakeEvent event;
    int called = 0;
    auto watch = loop.watch(event.waitable(), kWaitRead, [&](short ready) {
        CHECK(ready == kWaitRead);
        REQUIRE(event.drain());
        ++called;
    });
    event.notify();
    loop.pump();
    CHECK(called == 1);
    event.notify();
    loop.post([&] { watch.cancel(); });
    loop.pump();
    CHECK(called == 1);  // posted cancellation suppresses an already-ready snapshot
}
TEST_CASE("model loop one-shot cancellation and posts use the portable wake source") {
    rpc::ModelLoop loop;
    const auto due = std::chrono::steady_clock::time_point{};
    int called = 0;
    rpc::Timer second;
    auto first = loop.at(due, [&] {
        second.cancel();
        ++called;
    });
    second = loop.at(due, [&] { ++called; });
    loop.pump();
    CHECK(called == 1);
    CHECK_FALSE(first.active());
    CHECK_FALSE(second.active());
    std::thread producer([&] { loop.post([&] { ++called; }); });
    WaitItem source{loop.waitable()};
    const int ready = supple::waitFor(std::span(&source, 1), -1);
    producer.join();
    CHECK(ready == 1);
    loop.pump();
    CHECK(called == 2);
}
TEST_CASE("portable AF_UNIX transport dispatches and protects a live endpoint") {
    auto reg = registry();
    cmd::Command<Amount, Count> warning;
    warning.name = "counter.warn";
    warning.exposure = cmd::Exposure::Internal;
    warning.summary = "Return a diagnostic.";
    warning.args = schema::object<Amount>("warning_args")
                       .field("value", &Amount::value, schema::integerOf<int>())
                       .build();
    warning.result = schema::object<Count>("warning_result")
                         .field("count", &Count::count, schema::integerOf<int>())
                         .build();
    warning.run = [](const Amount& a, cmd::Context& ctx) -> Result<Count> {
        ctx.warn("counter.warning", "counter diagnostic", "/value");
        return Count{a.value};
    };
    reg.add(std::move(warning));
    Host host;
    cmd::Dispatcher dispatcher(reg, host);
    ModelQueue queue;
    rpc::Server server(reg, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    const auto dir = std::filesystem::temp_directory_path() / "supple-rpc-platform-test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "endpoint.sock";
    REQUIRE(server.start(path));
    rpc::Server competing(reg, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    auto refused = competing.start(path);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "rpc.address_in_use");
    rpc::Client client;
    auto absent = client.callWithDelivery("counter.add", parse(R"({"value":7})"));
    REQUIRE_FALSE(absent.result);
    CHECK(absent.delivery == rpc::Delivery::Unsent);
    REQUIRE(client.connect(path, "portable-test"));
    auto remoteError = client.callWithDelivery("counter.fail", parse(R"({"value":0})"));
    REQUIRE_FALSE(remoteError.result);
    CHECK(remoteError.delivery == rpc::Delivery::Sent);
    CHECK(remoteError.result.error().code == "test.located");
    auto result = client.call("counter.add", parse(R"({"value":7})"));
    REQUIRE(result);
    CHECK((*result)["count"].asInt() == 7);
    auto warned = client.call("counter.warn", parse(R"({"value":9})"));
    REQUIRE(warned);
    CHECK((*warned)["count"] == 9);
    REQUIRE((*warned)["warnings"].size() == 1);
    CHECK((*warned)["warnings"].at(0).find("code")->asString() == "counter.warning");
    CHECK((*warned)["warnings"].at(0).find("path")->asString() == "/value");
    auto failure = client.call("counter.fail", parse(R"({"value":0})"));
    REQUIRE_FALSE(failure);
    CHECK(failure.error().code == "test.located");
    REQUIRE(failure.error().loc);
    CHECK(failure.error().loc->line == 3);
    CHECK(failure.error().loc->column == 14);
    client.close();
    server.stop();
    CHECK_FALSE(std::filesystem::exists(path));
    std::filesystem::remove_all(dir);
}
TEST_CASE("portable AF_UNIX startup refuses paths beyond the platform limit") {
    auto reg = registry();
    Host host;
    cmd::Dispatcher dispatcher(reg, host);
    rpc::Server server(reg, dispatcher, [](auto) {});
    const auto dir = std::filesystem::temp_directory_path() / "supple-rpc-path-test";
    const auto result = server.start(dir / (std::string(200, 'x') + ".sock"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "rpc.path");
    std::filesystem::remove_all(dir);
}

TEST_CASE("client connection generations distinguish reconnects from calls") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    ModelQueue queue;
    rpc::Server first(commands, bus, [&](auto task) { queue.post(std::move(task)); });
    // Atomic directory creation keeps concurrent test processes independent
    // without native process-id APIs or long platform temporary paths.
    std::filesystem::path dir;
    for (std::uint64_t suffix = 0;; ++suffix) {
        dir = std::filesystem::temp_directory_path() / ("supple-rpc-gen-" + std::to_string(suffix));
        std::error_code error;
        if (std::filesystem::create_directory(dir, error)) break;
        REQUIRE_MESSAGE((!error || error == std::errc::file_exists), error.message());
    }
    const auto path = dir / "endpoint.sock";
    REQUIRE(first.start(path));
    rpc::Client client;
    CHECK(client.connectionGeneration() == 0);
    REQUIRE(client.connect(path, "generation-test"));
    const auto original = client.connectionGeneration();
    REQUIRE(original != 0);
    REQUIRE(client.call("registry.list"));
    CHECK(client.connectionGeneration() == original);
    first.stop();
    // Observe EOF deterministically, rather than deciding by how quickly
    // a native socket reports its peer's close to a nonblocking probe.
    REQUIRE_FALSE(client.call("registry.list"));
    CHECK(client.connectionGeneration() == 0);
    rpc::Server second(commands, bus, [&](auto task) { queue.post(std::move(task)); });
    REQUIRE(second.start(path));
    REQUIRE(client.call("registry.list"));  // automatic reconnect
    const auto automatic = client.connectionGeneration();
    CHECK(automatic > original);
    client.close();
    CHECK(client.connectionGeneration() == 0);
    REQUIRE(client.connect(path, "generation-test"));
    CHECK(client.connectionGeneration() > automatic);
    client.close();
    second.stop();
    std::filesystem::remove_all(dir);
}
