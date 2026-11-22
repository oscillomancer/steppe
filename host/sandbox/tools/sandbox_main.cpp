// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// <slug>-sandbox: the headless plugin sandbox (spec: docs/specs/sandbox.md §5).
//
//   <slug>-sandbox exec '<json>' [--workdir DIR] [--plugin-path P]...
//       One invocation {"command": "...", "args": {...}} or a batch
//       {"batch": {"label": ..., "steps": [...]}} against a fresh session;
//       prints the result (or the error) as JSON. Exit 0 = ok, 1 = error.
//   <slug>-sandbox registry [--markdown | --mcp-tools | --lua | --groups | --group NAME]
//       The command registry document, the Markdown reference, or the MCP
//       tool list (generator output; drift-tested against skill/generated/).
//   <slug>-sandbox --mcp [--workdir DIR] [--plugin-path P]...
//       Serve MCP over stdio against a live in-process sandbox session.
//   <slug>-sandbox serve [--workdir DIR] [--plugin-path P]... [--instance NAME] [--socket PATH]
//       Serve the command bus on a Unix socket from the model thread.
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <supple/base/paths.h>
#include <supple/mcp/server.h>
#include <supple/rpc/model_loop.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <supple/product.h>

#include "../src/optimize_loss.h"
#include "supple/sandbox/commands.h"

using namespace supple;
namespace fs = std::filesystem;

namespace {
int inputDescriptor() {
#if defined(_WIN32)
    return _fileno(stdin);
#else
    return ::fileno(stdin);
#endif
}

int outputDescriptor() {
#if defined(_WIN32)
    return _fileno(stdout);
#else
    return ::fileno(stdout);
#endif
}

int usage() {
    std::fprintf(
        stderr,
        "usage: %.*s-sandbox exec '<json>' [--workdir DIR] [--record-midi] [--plugin-path P]...\n"
        "       %.*s-sandbox registry [--markdown | --mcp-tools | --lua | --groups | --group "
        "NAME]\n"
        "       %.*s-sandbox --mcp [--workdir DIR] [--record-midi] [--plugin-path P]...\n"
        "       %.*s-sandbox serve [--workdir DIR] [--plugin-path P]... "
        "[--instance NAME] [--socket PATH]\n"
        "       %.*s-sandbox eval-reference '<json>'\n",
        static_cast<int>(product::kSlug.size()), product::kSlug.data(),
        static_cast<int>(product::kSlug.size()), product::kSlug.data(),
        static_cast<int>(product::kSlug.size()), product::kSlug.data(),
        static_cast<int>(product::kSlug.size()), product::kSlug.data(),
        static_cast<int>(product::kSlug.size()), product::kSlug.data());
    return 2;
}

fs::path defaultWorkdir() {
    const fs::path base = paths::cache(true);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return base / std::string(product::kSlug) / "sandbox" /
           (std::to_string(now) + "-" + std::to_string(::getpid()));
}

// The user's DSP script library, the one the app uses:
// <XDG_DATA_HOME or ~/.local/share>/<slug>/scripts.
fs::path userScriptDir() {
    const fs::path base = paths::data(true);
    return base / std::string(product::kSlug) / "scripts";
}

// Explicit locations retain priority; the bundled player and stock plugins
// precede older installed copies, as in the engine CLI.
void addDefaultPlugins(sandbox::SessionOptions& options) {
    if (auto stock = host::stockPluginDir()) options.pluginPath.push_back(*stock);
    for (const auto& path : host::defaultSearchPath()) options.pluginPath.push_back(path);
}

json::Value errorJson(const Error& e) {
    json::Value v = json::Value::object();
    v["code"] = e.code;
    v["message"] = e.message;
    if (!e.path.empty()) v["path"] = e.path;
    if (!e.hint.empty()) v["hint"] = e.hint;
    if (e.loc) {
        v["line"] = e.loc->line;
        v["column"] = e.loc->column;
    }
    if (!e.details.empty()) {
        json::Value d = json::Value::array();
        for (const auto& x : e.details) d.push_back(errorJson(x));
        v["details"] = std::move(d);
    }
    return v;
}

void print(const json::Value& v) {
    std::string text = json::writeCanonical(v);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    // Offline evaluation tooling, outside the frozen command/MCP registry.
    if (args[0] == "eval-reference") {
        if (args.size() != 2) return usage();
        auto request = json::parse(args[1]);
        if (!request || !request->isObject()) {
            print(errorJson(Error("eval.arguments", "expected an object", {},
                                  "reference, test, reference_note_off_s, test_note_off_s")));
            return 1;
        }
        auto read = [&](std::string_view fileKey, std::string_view endKey,
                        double& noteOff) -> Result<audio::AudioData> {
            const auto* file = request->find(fileKey);
            const auto* end = request->find(endKey);
            if (!file || !file->isString() || !end || !end->isNumber() ||
                !std::isfinite(end->asDouble()) || end->asDouble() < 0)
                return Error("eval.arguments", "expected a WAV path and a nonnegative note-off",
                             "/" + std::string(fileKey),
                             "give both reference and test note-off times");
            noteOff = end->asDouble();
            return audio::readAudio(file->asString());
        };
        // The scorer knows each side's input end, so it never assumes one.
        double referenceNoteOff = 0.0, testNoteOff = 0.0;
        auto reference = read("reference", "reference_note_off_s", referenceNoteOff);
        if (!reference) {
            print(errorJson(reference.error()));
            return 1;
        }
        auto test = read("test", "test_note_off_s", testNoteOff);
        if (!test) {
            print(errorJson(test.error()));
            return 1;
        }
        // The same scored comparison `compare` reports (optimize_loss.h).
        const auto scored =
            sandbox::detail::referenceScore(*reference, referenceNoteOff, *test, testNoteOff);
        if (!scored) {
            print(errorJson(scored.error()));
            return 1;
        }
        json::Value result = json::Value::object();
        result["loss"] = scored->loss.total;
        result["terms"] = json::Value::object();
        for (const auto& [key, value] : scored->loss.terms) result["terms"][key] = value;
        result["ltas_rms_db"] = scored->ltasRmsDb;
        print(result);
        return 0;
    }
    cmd::Registry registry;
    sandbox::registerCommands(registry);

    if (args[0] == "--mcp" || args[0] == "serve") {
        const bool rpcMode = args[0] == "serve";
        sandbox::SessionOptions options;
        options.identity.name = std::string(product::kName) + " sandbox";
        options.identity.vendor = std::string(product::kVendor);
        options.identity.version = std::string(product::kVersion);
        std::string instance = "default";
        fs::path socket;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--workdir" && i + 1 < args.size())
                options.workdir = args[++i];
            else if (args[i] == "--record-midi")
                options.recordMidi = true;
            else if (args[i] == "--plugin-path" && i + 1 < args.size())
                options.pluginPath.emplace_back(args[++i]);
            else if (rpcMode && args[i] == "--instance" && i + 1 < args.size())
                instance = args[++i];
            else if (rpcMode && args[i] == "--socket" && i + 1 < args.size())
                socket = args[++i];
            else
                return usage();
        }
        if (options.workdir.empty()) {
            options.workdir = defaultWorkdir();
            options.scriptDir = userScriptDir();  // the app's; under --workdir, <workdir>/scripts
        }
        addDefaultPlugins(options);
        auto session = sandbox::Session::open(options);
        if (!session) {
            std::fprintf(stderr, "%s\n", session.error().toString().c_str());
            return 1;
        }
        sandbox::SandboxHost host(std::move(*session), options.workdir / "commands.ndjson");
        cmd::Dispatcher dispatcher(registry, host);
        const auto interval = std::chrono::milliseconds(100);
        host.attachCommands(dispatcher);
        if (rpcMode) {
            rpc::ModelLoop loop;
            rpc::Server* activeServer = nullptr;
            rpc::Server server(registry, dispatcher, [&](auto task) {
                loop.post([&, task = std::move(task)] {
                    task();
                    for (auto& [topic, payload] : host.takeEvents())
                        activeServer->publish(topic, payload);
                });
            });
            activeServer = &server;
            auto started = server.start(socket.empty() ? rpc::socketPath(instance) : socket);
            if (!started) {
                std::fprintf(stderr, "%s\n", started.error().toString().c_str());
                return 1;
            }
            // The service tick: adopt finished background renders and clean up.
            auto tick = loop.every(interval, [&] {
                host.tick();
                for (auto& [topic, payload] : host.takeEvents())
                    activeServer->publish(topic, payload);
            });
            loop.run();
            tick.cancel();
            server.stop();
            return 0;
        }
        mcp::Server server(registry, dispatcher);
        // The tick drains the host's events: MCP has no event channel.
        auto status = server.serve(inputDescriptor(), outputDescriptor(),
                                   mcp::Tick{[&] {
                                                 host.tick();
                                                 host.takeEvents();
                                             },
                                             interval});
        if (!status) {
            std::fprintf(stderr, "%s\n", status.error().toString().c_str());
            return 1;
        }
        return 0;
    }

    if (args[0] == "registry") {
        if (args.size() == 1) {
            print(registry.exportJson());
        } else if (args[1] == "--markdown") {
            const std::string md = cmd::markdownReference(registry);
            std::fwrite(md.data(), 1, md.size(), stdout);
        } else if (args[1] == "--mcp-tools") {
            print(cmd::mcpToolList(registry));
        } else if (args[1] == "--groups" && args.size() == 2) {
            print(cmd::groupIndex(registry.groupsResult()));
        } else if (args[1] == "--group" && args.size() == 3) {
            if (registry.groupsResult(args[2]).find("groups")->asArray().empty()) {
                print(errorJson(cmd::unknownGroup(args[2], registry.groupsResult())));
                return 1;
            }
            print(cmd::rowIndex(registry.listResult("", "", "", args[2])));
        } else if (args[1] == "--lua") {
            const std::string stubs = cmd::luaStubs(registry);
            std::fwrite(stubs.data(), 1, stubs.size(), stdout);
        } else {
            return usage();
        }
        return 0;
    }

    if (args[0] == "exec") {
        if (args.size() < 2) return usage();
        sandbox::SessionOptions options;
        options.identity.name = std::string(product::kName) + " sandbox";
        options.identity.vendor = std::string(product::kVendor);
        options.identity.version = std::string(product::kVersion);
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--workdir" && i + 1 < args.size())
                options.workdir = args[++i];
            else if (args[i] == "--record-midi")
                options.recordMidi = true;
            else if (args[i] == "--plugin-path" && i + 1 < args.size())
                options.pluginPath.emplace_back(args[++i]);
            else
                return usage();
        }
        if (options.workdir.empty()) {
            options.workdir = defaultWorkdir();
            options.scriptDir = userScriptDir();  // the app's; under --workdir, <workdir>/scripts
        }
        addDefaultPlugins(options);
        auto parsed = json::parse(args[1]);
        if (!parsed || !parsed->isObject()) {
            print(errorJson(parsed ? Error("cli.invocation", "the invocation must be an object", {},
                                           R"({"command": "plugin.list", "args": {}})")
                                   : parsed.error()));
            return 1;
        }
        auto session = sandbox::Session::open(options);
        if (!session) {
            print(errorJson(session.error()));
            return 1;
        }
        const fs::path log = options.workdir / "commands.ndjson";
        sandbox::SandboxHost host(std::move(*session), log);
        cmd::Dispatcher dispatcher(registry, host);
        host.attachCommands(dispatcher);
        cmd::Invocation inv;
        inv.origin = {cmd::Origin::Type::Rpc, "cli", ""};
        if (const json::Value* batch = parsed->find("batch")) {
            inv.command = "batch";
            inv.args = *batch;
        } else if (const json::Value* c = parsed->find("command"); c && c->isString()) {
            inv.command = c->asString();
            if (const json::Value* a = parsed->find("args")) inv.args = *a;
        } else {
            print(errorJson(Error("cli.invocation", "missing \"command\" or \"batch\"", {},
                                  R"({"command": "plugin.list", "args": {}})")));
            return 1;
        }
        auto result = dispatcher.execute(inv);
        if (!result) {
            print(errorJson(result.error()));
            return 1;
        }
        print(*result);
        return 0;
    }
    return usage();
}
