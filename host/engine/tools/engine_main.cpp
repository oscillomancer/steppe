// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// <slug>-engine: the headless DAW (spec: docs/specs/engine.md §6).
//
//   <slug>-engine exec '<json>' [--project FILE] [--save] [--workdir DIR] [--plugin-path P]...
//       One invocation {"command": ..., "args": {...}} or {"batch": {...}}
//       against the project (a new one when --project is absent or missing);
//       --save writes the project back afterwards. Prints the result JSON.
//   <slug>-engine registry [--markdown | --mcp-tools | --lua]
//       The engine's command registry, Markdown reference, MCP tool list or
//       the LuaLS annotations of the scripting bindings.
//   <slug>-engine modules --markdown
//       The built-in rack module types as a Markdown reference.
//   <slug>-engine --mcp [--project FILE] [--workdir DIR] [--plugin-path P]...
//       Serve MCP over stdio against the in-process engine session.
//   <slug>-engine serve [--project FILE] [--instance NAME] [--socket PATH]
//       Serve the command bus on a Unix socket from the model thread.
//   <slug>-engine play FILE [--bar N] [--seconds S] [--block N] [--backend jack|portaudio|dummy]
//       Play a project through the audio device (to its end, or for S
//       seconds; Ctrl-C stops), then print engine.status.
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include <supple/base/chars.h>
#include <supple/base/paths.h>
#include <supple/mcp/server.h>
#include <supple/rpc/model_loop.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <supple/product.h>

#include "supple/engine/app.h"
#include "supple/engine/racks.h"
#include "supple/host/clap_host.h"

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

int usage() {
    const auto slug = std::string(product::kSlug);
    std::fprintf(
        stderr,
        "usage: %s-engine exec '<json>' [--project FILE] [--save] [--workdir DIR] "
        "[--plugin-path P]...\n"
        "       %s-engine registry [--markdown | --mcp-tools | --lua]\n"
        "       %s-engine modules --markdown\n"
        "       %s-engine --mcp [--project FILE] [--workdir DIR] [--plugin-path P]...\n"
        "       %s-engine serve [--project FILE] [--instance NAME] [--socket PATH]\n"
        "       %s-engine play FILE [--bar N] [--seconds S] [--block N] "
        "[--backend jack|portaudio|dummy] [--host-api NAME] [--output-device NAME] "
        "[--input-device NAME] [--rate HZ] [--inputs N] [--outputs N] [--plugin-path P]...\n",
        slug.c_str(), slug.c_str(), slug.c_str(), slug.c_str(), slug.c_str(), slug.c_str());
    return 2;
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
    const std::string text = json::writeCanonical(v);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

fs::path defaultWorkdir() {
    const fs::path base = paths::cache(true);
    return base / std::string(product::kSlug) / "engine";
}

// The user's render presets, as the app keeps them: <data>/<slug>/render-presets.
fs::path userRenderPresetDir() {
    return paths::data(true) / std::string(product::kSlug) / "render-presets";
}

std::atomic<bool> g_interrupted{false};

engine::AppOptions cliOptions() {
    engine::AppOptions options;
    if (const auto root = paths::config(); !root.empty())
        options.midiPreferencesFile = root / product::kSlug / "preferences.json";
    options.identity.name = std::string(product::kName);
    options.identity.vendor = std::string(product::kVendor);
    options.identity.version = std::string(product::kVersion);
    return options;
}

template <class T>
bool parseNumber(std::string_view text, T& value) {
    const char* end = text.data() + text.size();
    std::from_chars_result parsed{};
    if constexpr (std::is_floating_point_v<T>)
        parsed = fromChars(text.data(), end, value);  // Apple's libc++ has no floating from_chars
    else
        parsed = std::from_chars(text.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end &&
           std::isfinite(static_cast<double>(value));
}

int play(cmd::Registry& registry, const std::vector<std::string>& args) {
    if (args.size() < 2) return usage();
    const fs::path project = fs::absolute(args[1]);
    engine::AppOptions options = cliOptions();
    json::Value start = json::Value::object();
    std::optional<std::int64_t> bar;
    std::optional<double> seconds;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (i + 1 >= args.size()) return usage();
        const std::string& option = args[i];
        if (option == "--bar" || option == "--block" || option == "--inputs" ||
            option == "--outputs" || option == "--seconds" || option == "--rate") {
            const std::string key = option.substr(2);
            const std::string& text = args[++i];
            const bool integer = option != "--seconds" && option != "--rate";
            std::int64_t whole = 0;
            double real = 0;
            if (!(integer ? parseNumber(text, whole) : parseNumber(text, real)) ||
                (option == "--seconds" && real < 0)) {
                print(errorJson(Error("cli.number", "invalid number for " + option, "/" + key,
                                      integer ? "provide a complete decimal integer"
                                              : "provide a complete finite nonnegative number")));
                return 2;
            }
            if (option == "--bar")
                bar = whole;
            else if (option == "--seconds")
                seconds = real;
            else
                start[key] = integer ? json::Value(whole) : json::Value(real);
        } else if (args[i] == "--backend")
            start["backend"] = args[++i];
        else if (args[i] == "--host-api")
            start["host_api"] = args[++i];
        else if (args[i] == "--input-device")
            start["input_device"] = args[++i];
        else if (args[i] == "--output-device")
            start["output_device"] = args[++i];
        else if (args[i] == "--plugin-path")
            options.pluginPath.emplace_back(args[++i]);
        else
            return usage();
    }
    options.workdir = project.parent_path();
    options.renderPresetDir = userRenderPresetDir();
    // --plugin-path first, then the stock plugins built beside this binary, then the defaults.
    if (auto stock = host::stockPluginDir()) options.pluginPath.push_back(*stock);
    for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
    auto app = engine::EngineApp::create(options);
    if (!app) {
        print(errorJson(app.error()));
        return 1;
    }
    if (auto st = (*app)->open(project); !st) {
        print(errorJson(st.error()));
        return 1;
    }
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    auto run = [&](const char* command, json::Value a) {
        return dispatcher.execute({command, std::move(a), {cmd::Origin::Type::Rpc, "cli", ""}});
    };
    json::Value seek = json::Value::object();
    seek["bar"] = bar.value_or(1);
    for (auto [command, a] : {std::pair{"engine.start", start}, std::pair{"transport.seek", seek},
                              std::pair{"transport.play", json::Value::object()}}) {
        if (auto r = run(command, a); !r) {
            print(errorJson(r.error()));
            return 1;
        }
    }
    // Default length: from the start position to the last item's end, plus a second.
    const model::Session s = (*app)->view();
    const double endBeat = model::lastItemBeat(s);
    const double from = (*app)->engine()->transport().seconds;
    const double length = seconds.value_or(s.timeMap->secondsAt(endBeat) - from + 1.0);
    std::signal(SIGINT, [](int) { g_interrupted = true; });
    std::fprintf(stderr, "playing %s for %.1f s (Ctrl-C stops)\n",
                 project.filename().string().c_str(), length);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(length);
    while (!g_interrupted && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (*app)->engine()->service();
    }
    auto status = run("engine.status", json::Value::object());
    (void)run("engine.stop", json::Value::object());
    if (!status) {
        print(errorJson(status.error()));
        return 1;
    }
    print(*status);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    // The plugin bridge (isolated fx).
    (void)host::locateBridgeExecutable(std::string(product::kSlug) + "-bridge");
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    if (args[0] == "--mcp" || args[0] == "serve") {
        const bool rpcMode = args[0] == "serve";
        engine::AppOptions options;
        options.identity.name = std::string(product::kName);
        options.identity.vendor = std::string(product::kVendor);
        options.identity.version = std::string(product::kVersion);
        fs::path project;
        std::string instance = "default";
        fs::path socket;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--project" && i + 1 < args.size())
                project = fs::absolute(args[++i]);
            else if (args[i] == "--workdir" && i + 1 < args.size())
                options.workdir = args[++i];
            else if (args[i] == "--plugin-path" && i + 1 < args.size())
                options.pluginPath.emplace_back(args[++i]);
            else if (rpcMode && args[i] == "--instance" && i + 1 < args.size())
                instance = args[++i];
            else if (rpcMode && args[i] == "--socket" && i + 1 < args.size())
                socket = args[++i];
            else
                return usage();
        }
        // Render presets are the user's, shared with the app (ADR-0055); under
        // --workdir (tests) they stay in <workdir>/render-presets.
        if (options.workdir.empty()) {
            options.workdir = project.empty() ? defaultWorkdir() : project.parent_path();
            options.renderPresetDir = userRenderPresetDir();
        }
        if (auto stock = host::stockPluginDir()) options.pluginPath.push_back(*stock);
        for (auto& path : host::defaultSearchPath()) options.pluginPath.push_back(path);
        auto app = engine::EngineApp::create(options);
        if (!app) {
            std::fprintf(stderr, "%s\n", app.error().toString().c_str());
            return 1;
        }
        if (!project.empty() && fs::exists(project)) {
            auto status = (*app)->open(project);
            if (!status) {
                std::fprintf(stderr, "%s\n", status.error().toString().c_str());
                return 1;
            }
        }
        cmd::Dispatcher dispatcher(registry, **app);
        rpc::Server* activeServer = nullptr;
        auto publishEvents = [&] {
            for (auto& [topic, payload] : (*app)->takeEvents())
                activeServer->publish(topic, payload);
        };
        (*app)->attachCommands(dispatcher);
        if (rpcMode) {
            rpc::ModelLoop loop;
            rpc::Timer deadline;
            std::vector<rpc::FdWatch> pluginWatches;
            std::function<void()> schedule;
            auto service = [&] {
                pluginWatches.clear();  // service may prune owners and close fds
                deadline.cancel();
                (*app)->tick();
                publishEvents();
                schedule();
            };
            schedule = [&] {
                deadline.cancel();
                if (auto next = (*app)->nextServiceDeadline()) deadline = loop.at(*next, service);
                pluginWatches.clear();
                for (const auto& watch : (*app)->serviceFds())
                    pluginWatches.push_back(
                        loop.watch(watch.fd, watch.events, [&, watch](short ready) {
                            (*app)->serviceFdReady(watch, ready);
                            service();
                        }));
            };
            auto wake =
                loop.watch((*app)->serviceWaitable(), supple::kWaitRead, [&](short) { service(); });
            rpc::Server server(registry, dispatcher, [&](auto task) {
                loop.post([&, task = std::move(task)] {
                    pluginWatches.clear();  // commands may replace instances
                    deadline.cancel();
                    task();
                    publishEvents();
                    schedule();
                });
            });
            activeServer = &server;
            auto started = server.start(socket.empty() ? rpc::socketPath(instance) : socket);
            if (!started) {
                std::fprintf(stderr, "%s\n", started.error().toString().c_str());
                return 1;
            }
            service();
            loop.run();
            wake.cancel();
            deadline.cancel();
            pluginWatches.clear();
            (*app)->stopEngine();  // commit captured takes while the session/bus still exist
            publishEvents();
            server.stop();
            return 0;
        }
        mcp::Server server(registry, dispatcher);
        // The tick drains the app's events: MCP has no event channel, so they
        // would only pile up.
        mcp::Tick tick;
        tick.fn = [&] {
            (*app)->tick();
            (*app)->takeEvents();
        };
        tick.deadline = [&] { return (*app)->nextServiceDeadline(); };
        tick.watches = [&] {
            std::vector<mcp::Tick::Watch> watches{
                {(*app)->serviceWaitable(), supple::kWaitRead, [](short) {}}};
            for (const auto& watch : (*app)->serviceFds())
                watches.push_back({watch.fd, watch.events, [&, watch](short ready) {
                                       (*app)->serviceFdReady(watch, ready);
                                   }});
            return watches;
        };
        tick.fn();
        auto status = server.serve(inputDescriptor(), std::cout, tick);
        (*app)->stopEngine();  // EOF/error still finishes audio and MIDI takes on the model thread
        if (!status) {
            std::fprintf(stderr, "%s\n", status.error().toString().c_str());
            return 1;
        }
        return 0;
    }
    if (args[0] == "registry") {
        if (args.size() == 1)
            print(registry.exportJson());
        else if (args[1] == "--markdown") {
            const std::string md = cmd::markdownReference(registry);
            std::fwrite(md.data(), 1, md.size(), stdout);
        } else if (args[1] == "--mcp-tools")
            print(cmd::mcpToolList(registry));
        else if (args[1] == "--lua") {
            const std::string stubs = cmd::luaStubs(registry);
            std::fwrite(stubs.data(), 1, stubs.size(), stdout);
        } else
            return usage();
        return 0;
    }
    if (args[0] == "modules") {
        if (args.size() != 2 || args[1] != "--markdown") return usage();
        const std::string md = engine::moduleReference();
        std::fwrite(md.data(), 1, md.size(), stdout);
        return 0;
    }
    if (args[0] == "play") return play(registry, args);
    if (args[0] != "exec" || args.size() < 2) return usage();

    engine::AppOptions options = cliOptions();
    fs::path project;
    bool save = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--project" && i + 1 < args.size())
            project = fs::absolute(args[++i]);
        else if (args[i] == "--save")
            save = true;
        else if (args[i] == "--workdir" && i + 1 < args.size())
            options.workdir = args[++i];
        else if (args[i] == "--plugin-path" && i + 1 < args.size())
            options.pluginPath.emplace_back(args[++i]);
        else
            return usage();
    }
    if (options.workdir.empty()) {
        options.workdir = project.empty() ? defaultWorkdir() : project.parent_path();
        options.renderPresetDir = userRenderPresetDir();
    }
    if (auto stock = host::stockPluginDir()) options.pluginPath.push_back(*stock);
    for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
    auto app = engine::EngineApp::create(options);
    if (!app) {
        print(errorJson(app.error()));
        return 1;
    }
    if (!project.empty() && fs::exists(project)) {
        auto st = (*app)->open(project);
        if (!st) {
            print(errorJson(st.error()));
            return 1;
        }
    }
    auto parsed = json::parse(args[1]);
    if (!parsed || !parsed->isObject()) {
        print(errorJson(parsed ? Error("cli.invocation", "the invocation must be an object")
                               : parsed.error()));
        return 1;
    }
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
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
                              R"({"command": "session.describe", "args": {}})")));
        return 1;
    }
    auto result = dispatcher.execute(inv);
    if (!result) {
        print(errorJson(result.error()));
        return 1;
    }
    if (save) {
        const fs::path target = project.empty() ? (*app)->projectPath() : project;
        if (target.empty()) {
            print(errorJson(Error("cli.save", "--save needs --project FILE")));
            return 1;
        }
        auto st = (*app)->save(target);
        if (!st) {
            print(errorJson(st.error()));
            return 1;
        }
    }
    print(*result);
    return 0;
}
