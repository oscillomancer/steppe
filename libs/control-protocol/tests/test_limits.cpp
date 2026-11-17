// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/product.h>
#include <cstring>
#include "../src/socket_platform.h"
#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#endif
#include "protocol_fixture.h"

namespace {
struct Directory {
    std::filesystem::path path;
    Directory() {
        for (unsigned n = 0;; ++n) {
            path = std::filesystem::temp_directory_path() / ("rpc-limits-" + std::to_string(n));
            std::error_code ec;
            if (std::filesystem::create_directory(path, ec)) break;
            REQUIRE((!ec || ec == std::errc::file_exists));
        }
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Raw {
    rpc::net::Socket fd = rpc::net::open();
    explicit Raw(const std::filesystem::path& path) {
        REQUIRE(fd != rpc::net::invalid);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = rpc::net::pathName(path);
        REQUIRE(name.size() < sizeof(address.sun_path));
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        REQUIRE(rpc::net::connect(fd, address) == 0);
    }
    ~Raw() { rpc::net::close(fd); }
    void send(const std::string& value) {
        const auto frame = value + '\n';
        std::size_t sent = 0;
        while (sent < frame.size()) {
            auto n = rpc::net::send(fd, frame.data() + sent, frame.size() - sent);
            REQUIRE(n > 0);
            sent += static_cast<std::size_t>(n);
        }
    }
    json::Value read() {
        std::string frame;
        while (frame.find('\n') == std::string::npos) {
            rpc::net::PollFd item{fd, POLLIN, 0};
            REQUIRE_MESSAGE(rpc::net::poll(&item, 1, kGuardMs) > 0, "WATCHDOG ABORT RPC reply");
            char bytes[4096];
            auto n = rpc::net::recv(fd, bytes, sizeof(bytes));
            REQUIRE(n > 0);
            frame.append(bytes, static_cast<std::size_t>(n));
        }
        return parse(frame);
    }
};
const std::string hello =
    R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"limits","protocol":1}})";
struct Held {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> work;
    std::size_t posted = 0;
    void post(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            work.push_back(std::move(task));
            ++posted;
        }
        cv.notify_all();
    }
    void await(std::size_t count) {
        std::unique_lock lock(mutex);
        REQUIRE_MESSAGE(cv.wait_for(lock, kGuard, [&] { return posted >= count; }),
                        "WATCHDOG ABORT posted work");
    }
    std::function<void()> pop() {
        std::lock_guard lock(mutex);
        auto task = std::move(work.front());
        work.pop_front();
        return task;
    }
    void discard() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lock(mutex);
            tasks.swap(work);
        }
    }
};
void busy(const json::Value& reply) {
    REQUIRE(reply.find("error"));
    const auto* detail = reply.find("error")->find("data");
    REQUIRE(detail);
    CHECK(*detail->find("code") == "rpc.busy");
    CHECK_FALSE(detail->find("hint")->asString().empty());
    CHECK(*reply.find("id") == 1);
}
}  // namespace

TEST_CASE("global posted request count survives disconnected owners and releases at execution") {
    Directory dir;
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    Held held;
    rpc::Server server(commands, bus, [&](auto task) { held.post(std::move(task)); });
    const auto path = dir.path / "endpoint.sock";
    REQUIRE(server.start(path));
    std::vector<std::unique_ptr<Raw>> owners;
    for (int i = 0; i < 64; ++i) {
        owners.push_back(std::make_unique<Raw>(path));
        owners.back()->send(hello);
        for (int j = 0; j < 15; ++j)
            owners.back()->send(
                R"({"jsonrpc":"2.0","id":1,"method":"counter.add","params":{"value":1}})");
        held.await(static_cast<std::size_t>((i + 1) * 16));
    }
    // Joining I/O is a deterministic disconnect barrier, freeing live peer slots
    // without destroying the posted closures or the server's global ledger.
    server.stop();
    owners.clear();
    REQUIRE(server.start(path));
    Raw next(path);
    next.send(R"({"jsonrpc":"2.0","method":"rpc.hello"})");
    next.send(hello);
    busy(next.read());
    auto retained = held.pop();
    retained();  // global credit returns now, even while this copy lives
    next.send(hello);
    held.await(1025);
    for (int i = 0; i < 1024; ++i) {
        auto task = held.pop();
        task();
    }
    CHECK(host.count == 64 * 15);  // admitted edits execute despite disconnection
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 16; ++i) next.send(hello);
        held.await(1025 + static_cast<std::size_t>((round + 1) * 16));
        held.discard();  // destruction must release global AND per-peer credits
    }
    server.stop();
}

TEST_CASE("global posted frame bytes refuse excess work independently of request count") {
    Directory dir;
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    Held held;
    rpc::Server server(commands, bus, [&](auto task) { held.post(std::move(task)); });
    const auto path = dir.path / "endpoint.sock";
    REQUIRE(server.start(path));
    std::string frame =
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"protocol":1,"client":")";
    const std::string suffix = R"("}})";
    frame.append(rpc::kMaxMessageBytes - 64 - frame.size() - suffix.size(), 'x');
    frame += suffix;
    std::vector<std::unique_ptr<Raw>> owners;
    for (int i = 0; i < 4; ++i) {
        owners.push_back(std::make_unique<Raw>(path));
        owners.back()->send(frame);
        held.await(i + 1);
    }
    Raw next(path);
    auto request = parse(hello);
    request["params"]["client"] = std::string(1024, 'x');
    next.send(json::writeCompact(request));
    busy(next.read());
    held.discard();
    next.send(hello);
    held.await(5);
    held.discard();
    server.stop();
}

TEST_CASE("subscription cap refuses the 65th and recovers after unsubscribe") {
    Directory dir;
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    ModelQueue model;
    rpc::Server server(commands, bus, [&](auto task) { model.post(std::move(task)); });
    const auto path = dir.path / "endpoint.sock";
    REQUIRE(server.start(path));
    rpc::Client client;
    REQUIRE(client.connect(path, "subscriptions"));
    const auto args = parse(R"({"topics":[]})");
    for (int i = 0; i < 64; ++i) REQUIRE(client.call("rpc.subscribe", args));
    const auto refused = client.call("rpc.subscribe", args);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "rpc.subscriptions");
    CHECK_FALSE(refused.error().hint.empty());
    REQUIRE(client.call("rpc.unsubscribe", parse(R"({"subscription":"sub_1"})")));
    REQUIRE(client.call("rpc.subscribe", args));
    client.close();
    server.stop();
}

namespace {
#if defined(_WIN32)
// An ordinary directory junction avoids changing Developer Mode or requiring
// symbolic-link elevation. Mount-point layout is MS-FSCC 2.1.2.5.
bool directoryAlias(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::filesystem::create_directory(link);
    const auto print = std::filesystem::canonical(target).native();
    const auto substitute = std::wstring(L"\\??\\") + print;
    struct Header {
        DWORD tag;
        WORD bytes, reserved, substituteOffset, substituteBytes, printOffset, printBytes;
    } header{IO_REPARSE_TAG_MOUNT_POINT,
             static_cast<WORD>(8 + (substitute.size() + print.size() + 2) * sizeof(wchar_t)),
             0,
             0,
             static_cast<WORD>(substitute.size() * sizeof(wchar_t)),
             static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)),
             static_cast<WORD>(print.size() * sizeof(wchar_t))};
    static_assert(sizeof(Header) == 16);
    std::vector<std::byte> buffer(sizeof(Header) +
                                  (substitute.size() + print.size() + 2) * sizeof(wchar_t));
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.c_str(),
                (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) + (substitute.size() + 1) * sizeof(wchar_t),
                print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    const auto handle = CreateFileW(
        link.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const bool ok =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(handle);
    return ok;
}
#else
bool directoryAlias(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    return !error;
}
#endif
}  // namespace

TEST_CASE("product runtime directory refuses aliases without mutating their targets") {
    Directory dir;
    const auto target = dir.path / "target";
    const auto productDirectory = dir.path / std::string(product::kSlug);
    std::filesystem::create_directory(target);
#if !defined(_WIN32)
    std::filesystem::permissions(
        target, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                    std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                    std::filesystem::perms::others_exec);
#endif
    const auto before = std::filesystem::status(target).permissions();
    REQUIRE(directoryAlias(productDirectory, target));
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    rpc::Server server(commands, bus, [](auto) {});
    const auto result = server.start(productDirectory / "endpoint.sock");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "rpc.path");
    CHECK(result.error().path.find(std::string(product::kSlug)) != std::string::npos);
    CHECK_FALSE(result.error().hint.empty());
    CHECK(std::filesystem::status(target).permissions() == before);
    CHECK_FALSE(std::filesystem::exists(target / "endpoint.sock"));
    CHECK_FALSE(std::filesystem::exists(target / "endpoint.sock.lock"));
}

TEST_CASE("owned product runtime directory is accepted with private native permissions") {
    Directory dir;
    const auto productDirectory = dir.path / std::string(product::kSlug);
    auto commands = registry();
    Host host;
    cmd::Dispatcher bus(commands, host);
    rpc::Server server(commands, bus, [](auto) {});
    REQUIRE(server.start(productDirectory / "endpoint.sock"));
#if !defined(_WIN32)
    CHECK((std::filesystem::status(productDirectory).permissions() & std::filesystem::perms::all) ==
          std::filesystem::perms::owner_all);
#endif
    server.stop();
}
