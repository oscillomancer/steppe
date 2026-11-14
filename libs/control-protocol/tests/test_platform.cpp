// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/base/waitable.h>
#include <supple/rpc/model_loop.h>
#include <array>
#include "protocol_fixture.h"

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
    REQUIRE(client.connect(path, "portable-test"));
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
    const auto dir = std::filesystem::temp_directory_path() / "supple-rpc-generation-test";
    std::filesystem::create_directories(dir);
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
