// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/cmd/bus.h>
#include <supple/mcp/server.h>
#include <supple/product.h>
#include <chrono>
#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define fileno _fileno
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    using namespace supple;
    std::filesystem::path path;
    std::string instance = "default";
    mcp::ToolSet tools = mcp::ToolSet::Core;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--socket" && i + 1 < argc)
            path = argv[++i];
        else if (std::string_view(argv[i]) == "--instance" && i + 1 < argc)
            instance = argv[++i];
        else if (std::string_view(argv[i]) == "--tools" && i + 1 < argc &&
                 (std::string_view(argv[i + 1]) == "core" ||
                  std::string_view(argv[i + 1]) == "all"))
            tools = std::string_view(argv[++i]) == "all" ? mcp::ToolSet::All : mcp::ToolSet::Core;
        else {
            std::fprintf(stderr,
                         "usage: %.*s-mcp [--instance NAME] [--socket PATH] [--tools core|all]\n",
                         static_cast<int>(product::kSlug.size()), product::kSlug.data());
            return 2;
        }
    }
    if (path.empty()) path = rpc::socketPath(instance);
    rpc::Client client;
    // A valid job.wait can consume the whole command wait budget. The wire
    // deadline also needs room for dispatch and delivery of its response.
    client.setTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>{2.0 * cmd::kMaxWaitSeconds}));
    auto connected = client.connect(path, "mcp-proxy", {}, true);
    if (!connected) {
        std::fprintf(stderr, "%s\nIs %.*s running (instance \"%s\")? Start it, then reconnect.\n",
                     connected.error().toString().c_str(), static_cast<int>(product::kName.size()),
                     product::kName.data(), instance.c_str());
        return 1;
    }
    // After the app restarts, calls reconnect to the same socket.
    auto server = mcp::Server::proxy(
        client,
        [&](const cmd::Origin& origin) {
            return client.connect(path, origin.client.empty() ? "mcp-proxy" : origin.client,
                                  origin.agent, true);
        },
        tools);
    if (!server) {
        std::fprintf(stderr, "%s\n", server.error().toString().c_str());
        return 1;
    }
    auto status = (*server)->serve(fileno(stdin), fileno(stdout));
    if (!status) {
        std::fprintf(stderr, "%s\n", status.error().toString().c_str());
        return 1;
    }
    return 0;
}
