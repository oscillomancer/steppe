// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "crash_report_text.h"
#include <cstdio>
#include <map>
#include "supple/product.h"

namespace supple::gui {
namespace {
const json::Value* get(const json::Value& v, const char* key) {
    return v.isObject() ? v.find(key) : nullptr;
}
std::string str(const json::Value& v, const char* key) {
    const auto* x = get(v, key);
    return x && x->isString() ? x->asString() : std::string();
}
long long num(const json::Value& v, const char* key, long long fallback = 0) {
    const auto* x = get(v, key);
    return x && x->isInt() ? x->asInt() : fallback;
}
bool flag(const json::Value& v, const char* key) {
    const auto* x = get(v, key);
    return x && x->isBool() && x->asBool();
}
const json::Array* list(const json::Value& v, const char* key) {
    const auto* x = get(v, key);
    return x && x->isArray() ? &x->asArray() : nullptr;
}
std::string hex(unsigned long long v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%llX", v);
    return buffer;
}
std::string appName(const std::string& role) {
    std::string app(product::kName);
    if (role == "engine") app += " engine";
    return app;
}
std::string who(const std::string& name, const std::string& vendor) {
    return vendor.empty() ? name : name + " by " + vendor;
}
std::string shortId(const std::string& id) { return id.size() > 12 ? id.substr(0, 12) + "…" : id; }
std::string formatName(const std::string& kind) {
    if (kind == "clap" || kind == "clap_library") return "CLAP";
    if (kind == "vst3" || kind == "vst3_library") return "VST3";
    return kind.empty() ? "unknown format" : kind;
}
std::string threadRole(const std::string& role) {
    static const std::map<std::string, std::string> names{
        {"fault", "Faulting thread"},
        {"model", "Model thread"},
        {"main", "Model thread"},
        {"audio_render", "Audio render thread"},
        {"audio_live", "Live audio helper"},
        {"audio_background", "Background audio helper"},
        {"audio_offline", "Offline render thread"},
        {"recorder", "Recorder thread"},
        {"midi_output", "MIDI output thread"},
        {"bridge_control", "Plug-in process: control thread"},
        {"bridge_audio", "Plug-in process: audio thread"},
        {"other", "Other thread"},
    };
    const auto it = names.find(role);
    if (it != names.end()) return it->second;
    return role.empty() ? "Thread" : role;
}
std::string stopReason(const std::string& stop) {
    static const std::map<std::string, std::string> names{
        {"end", "reached the end of its stack"},
        {"depth", "stopped at the depth limit"},
        {"unreadable", "stopped at memory it could not read"},
        {"not_executable", "stopped at an address that is not code"},
        {"bad_frame_pointer", "stopped at a frame it could not follow"},
        {"cycle", "stopped at a loop in its frames"},
        {"deadline", "ran out of time"},
    };
    const auto it = names.find(stop);
    return it != names.end() ? it->second : stop;
}
std::string unavailableReason(const std::string& reason) {
    static const std::map<std::string, std::string> names{
        {"ptrace_denied", "the system did not allow reading it"},
        {"access_denied", "the system did not allow reading it"},
        {"thread_exited", "it had already ended"},
        {"deadline", "there was not enough time"},
        {"registers_unreadable", "its registers could not be read"},
        {"maps_unreadable", "its memory map could not be read"},
        {"unsupported", "this platform does not support reading it"},
        {"not_collected", "it was not collected"},
    };
    const auto it = names.find(reason);
    return it != names.end() ? it->second : reason;
}
struct Modules {
    struct Entry {
        std::string format, id;
    };
    std::vector<Entry> entries;
    std::map<std::string, std::string> plugin;  // "format:id" -> the plug-in's name
};
std::string commandOrigin(const std::string& origin) {
    static const std::map<std::string, std::string> names{
        {"ui", "app"},    {"script", "script"},     {"mcp", "agent (MCP)"}, {"rpc", "agent (RPC)"},
        {"test", "test"}, {"internal", "internal"}, {"agent", "agent"},
    };
    const auto it = names.find(origin);
    return it != names.end() ? it->second : origin;
}
const json::Value* pluginById(const json::Value& report, long long id) {
    const auto* plugins = list(report, "plugins");
    if (!plugins) return nullptr;
    for (const auto& p : *plugins)
        if (get(p, "id") && num(p, "id", -1) == id) return &p;
    return nullptr;
}
std::string kindOf(const json::Value& report) { return str(report, "kind"); }
}  // namespace

std::string roleName(const std::string& role) {
    if (role == "gui") return "the app";
    if (role == "engine") return "the headless engine";
    if (role == "fixture") return "a test run";
    return role.empty() ? "an unknown app" : role;
}

std::string causeSentence(const std::string& platform, const std::string& kind, long long cause,
                          long long code, bool nativeFatal) {
    if (cause == 1) {
        std::string what;
        switch (code) {
            case 11:
                what = "a segmentation fault";
                break;
            case 6:
                what = "an abort";
                break;
            case 4:
                what = "an illegal instruction";
                break;
            case 8:
                what = "an arithmetic fault";
                break;
            case 7:
                if (platform != "macos") what = "a bus error";
                break;
            case 10:
                if (platform == "macos") what = "a bus error";
                break;
            default:
                break;
        }
        return "It stopped on " + (what.empty() ? std::string("a fatal signal") : what) +
               " (signal " + std::to_string(code) + ").";
    }
    if (cause == 2) {
        const auto status = static_cast<unsigned long long>(code) & 0xFFFFFFFFull;
        std::string what;
        switch (status) {
            case 0xC0000005:
                what = "an access violation";
                break;
            case 0xC00000FD:
                what = "a stack overflow";
                break;
            case 0xC0000094:
                what = "an integer division by zero";
                break;
            case 0xC000001D:
                what = "an illegal instruction";
                break;
            case 0xC0000409:
                what = "a fast-fail (a stack buffer overrun, or an abort)";
                break;
            case 0xC0000374:
                what = "heap corruption";
                break;
            default:
                break;
        }
        return "It stopped on " + (what.empty() ? std::string("a Windows exception") : what) +
               " (" + hex(status) + ").";
    }
    if (cause == 3)
        return "It stopped because an exception was thrown and nothing caught it "
               "(std::terminate).";
    if (cause == 4) return "It stopped because the program called abort().";
    if (kind == "bridge_exit") {
        const auto status = static_cast<unsigned long long>(code) & 0xFFFFFFFFull;
        return platform == "windows" ? "It ended with the status " + hex(status) + "."
                                     : "It ended with the status " + std::to_string(code) + ".";
    }
    if (kind == "crash")
        return "It stopped on a fatal error (status " + std::to_string(code) +
               "), and this report cannot say more about it.";
    const std::string exit = platform == "windows" && (code < 0 || code > 255)
                                 ? hex(static_cast<unsigned long long>(code) & 0xFFFFFFFFull)
                                 : std::to_string(code);
    return std::string("It exited with the status ") + exit +
           (nativeFatal ? ", which the system uses for a fatal error" : "") +
           " and left no stack to look at.";
}

ReportPage describeReport(const json::Value& report, const std::string& saved) {
    ReportPage page;
    const std::string role = str(report, "role");
    const std::string kind = kindOf(report);
    const std::string platform = str(report, "platform");
    const std::string app = appName(role);
    const auto* attribution = get(report, "attribution");
    const std::string level = attribution ? str(*attribution, "confidence") : std::string();
    const json::Value* named = nullptr;
    if (attribution && get(*attribution, "plugin") && get(*attribution, "plugin")->isInt())
        named = pluginById(report, num(*attribution, "plugin"));
    const std::string namedName = named ? str(*named, "name") : std::string();
    const std::string namedVendor = named ? str(*named, "vendor") : std::string();

    // The headline. A plug-in is named only when the report's own evidence names one.
    const bool points =
        named && !namedName.empty() &&
        (level == "fault_in_image" || level == "active_token" || level == "bridge_exit");
    if (kind == "bridge_exit") {
        page.headline = points ? who(namedName, namedVendor) +
                                     " was running in a separate process that stopped."
                               : "A separate process that runs plug-ins stopped.";
    } else if (kind == "crash" || kind == "unexpected_exit") {
        page.headline = points ? who(namedName, namedVendor) + " was running when " + app +
                                     " closed unexpectedly."
                               : app + " closed unexpectedly.";
    } else {
        page.headline =
            "A report this version of " + std::string(product::kName) + " cannot fully read.";
    }
    if (points) page.plugin = namedName;

    // The subtitle: when, by which build, on what.
    std::string subtitle =
        saved.empty() ? std::string("Saved at an unknown time") : "Saved " + saved;
    const auto* build = get(report, "build");
    if (build) {
        const std::string version = str(*build, "version");
        if (!version.empty()) subtitle += " · " + std::string(product::kName) + " " + version;
    }
    if (!platform.empty()) {
        const std::string arch = str(report, "architecture");
        subtitle += " · " + platform + (arch.empty() ? "" : " " + arch);
    }
    subtitle += " · " + roleName(role);
    page.subtitle = subtitle;

    // The plain sentences: what stopped it, then what the report points at and how far to trust it.
    if (kind == "crash" || kind == "unexpected_exit" || kind == "bridge_exit") {
        page.sentences.push_back(causeSentence(platform, kind, num(report, "cause"),
                                               num(report, "code"),
                                               flag(report, "native_fatal_status")));
    }
    // A separate process that wrote down where it stopped itself (the report's `capture`): said
    // as that, since a plug-in runs in that process and could have written the account too.
    const bool ownAccount = kind == "bridge_exit" && str(report, "capture") == "bridge";
    if (kind == "bridge_exit") {
        page.sentences.push_back(std::string(product::kName) +
                                 " kept running. A plug-in's separate process is the host's "
                                 "protection against exactly this.");
        if (ownAccount && level == "fault_in_image") {
            page.sentences.push_back(
                "The process wrote down where it stopped itself: inside " +
                (points ? who(namedName, namedVendor) + "'s" : std::string("a listed plug-in's")) +
                " code. A plug-in runs in that process and could have written that account too, "
                "so it points at the plug-in but does not prove it.");
        } else if (ownAccount && level == "active_token") {
            page.sentences.push_back(
                std::string(points ? who(namedName, namedVendor) : "A plug-in") +
                " was running on the thread that stopped, but where it stopped was in code the "
                "report could not identify. The process wrote that account itself: a clue, not "
                "proof.");
        } else if (ownAccount) {
            page.sentences.push_back(
                "The process wrote down where it stopped itself (below). It names no plug-in.");
        }
    } else if (level == "fault_in_image") {
        page.sentences.push_back(
            "The program stopped inside " +
            (points ? who(namedName, namedVendor) + "'s" : std::string("a listed plug-in's")) +
            " code. That points at the plug-in, but a report cannot prove it "
            "was the cause.");
    } else if (level == "active_token") {
        page.sentences.push_back(std::string(points ? who(namedName, namedVendor) : "A plug-in") +
                                 " was running on the thread that stopped, but the stop was in "
                                 "code the report could not "
                                 "identify. That is a clue, not proof.");
    } else if (level == "concurrent_activity") {
        page.sentences.push_back(
            "Plug-ins were running on other threads when it stopped. That is activity, not blame.");
    } else if (level == "recent_command") {
        page.sentences.push_back(
            "No plug-in is implicated. The last commands the model ran are listed below.");
    } else if (level == "unknown") {
        page.sentences.push_back("The report cannot say what the program was doing.");
    } else if (kind == "crash" || kind == "unexpected_exit") {
        page.sentences.push_back("This report says nothing about what the program was doing.");
    } else {
        page.sentences.push_back(
            "This report is of a kind this version does not know; what it holds is below as far as "
            "it can be read.");
    }
    if (kind == "crash" || kind == "unexpected_exit") {
        const auto* threads = list(report, "threads");
        if (!flag(report, "terminal_context") && (!threads || threads->empty()))
            page.sentences.push_back("No stack was captured for this report.");
    }
    page.sentences.push_back("This report is on this computer only. It was not sent anywhere.");

    // Technical detail.
    Modules modules;
    if (const auto* mods = list(report, "modules"))
        for (const auto& m : *mods) modules.entries.push_back({str(m, "format"), str(m, "id")});
    if (const auto* plugins = list(report, "plugins"))
        for (const auto& p : *plugins) {
            const auto* b = get(p, "build");
            if (b && !str(*b, "id").empty() && !str(p, "name").empty())
                modules.plugin[str(*b, "format") + ":" + str(*b, "id")] = str(p, "name");
        }

    if (const auto* plugins = list(report, "plugins")) {
        ReportSection s;
        s.id = "plugins";
        s.title = "Plug-ins";
        s.count = plugins->size();
        s.note = "What the program had loaded, as each plug-in described itself.";
        for (const auto& p : *plugins) {
            std::string line =
                who(str(p, "name").empty() ? "(unnamed)" : str(p, "name"), str(p, "vendor"));
            if (!str(p, "version").empty()) line += " " + str(p, "version");
            line += " · " + formatName(str(p, "kind"));
            const std::string origin = str(p, "origin");
            if (!origin.empty() && origin != "foreign") line += " · " + origin;
            const std::string mode = str(p, "mode");
            if (mode == "bridged") line += " · in a separate process";
            if (!str(p, "state").empty()) line += " · " + str(p, "state");
            if (const auto* b = get(p, "build"))
                if (!str(*b, "id").empty()) line += " · build " + shortId(str(*b, "id"));
            if (named == &p) line += " · named by the report";
            s.lines.push_back(std::move(line));
        }
        page.sections.push_back(std::move(s));
    }

    if (const auto* bridges = list(report, "bridges"); bridges && !bridges->empty()) {
        ReportSection s;
        s.id = "bridges";
        s.title = "Separate processes";
        s.count = bridges->size();
        s.note = "Processes that ran plug-ins away from the program.";
        for (const auto& b : *bridges) {
            std::string line = "Process " + std::to_string(num(b, "id"));
            if (const auto* ended = get(b, "ended")) {
                const std::string how = str(*ended, "how");
                line += " · ended" + (how.empty() ? std::string() : " by a " + how) + " (status " +
                        std::to_string(num(*ended, "code")) + ")";
            } else {
                line += " · still running";
            }
            if (const auto* instances = list(b, "instances"); instances && !instances->empty()) {
                std::string names;
                for (const auto& i : *instances) {
                    const auto* p = i.isInt() ? pluginById(report, i.asInt()) : nullptr;
                    names += (names.empty() ? "" : ", ") +
                             (p && !str(*p, "name").empty() ? str(*p, "name") : "(unnamed)");
                }
                line += " · " + names;
            }
            s.lines.push_back(std::move(line));
        }
        page.sections.push_back(std::move(s));
    }

    if (const auto* threads = list(report, "threads"); threads && !threads->empty()) {
        ReportSection s;
        s.id = "threads";
        s.title = "Threads";
        s.count = threads->size();
        s.note =
            "Where each thread was. A frame is a place in a program or plug-in file, as an offset "
            "from where that file was loaded; no names of functions are kept." +
            std::string(ownAccount ? " The process that stopped wrote these itself, and could read "
                                     "only the thread that stopped."
                                   : "");
        for (const auto& t : *threads) {
            std::string head = threadRole(str(t, "role"));
            if (str(t, "state") == "walked") {
                head += " · " + stopReason(str(t, "stop"));
            } else if (str(t, "state") == "unavailable") {
                head += " · not read: " + unavailableReason(str(t, "reason"));
            }
            s.lines.push_back(std::move(head));
            const auto* frames = list(t, "frames");
            if (!frames) continue;
            std::size_t n = 0;
            for (const auto& f : *frames) {
                std::string line = "    #" + std::to_string(n++) + "  ";
                const auto* module = get(f, "module");
                if (module && module->isInt() && module->asInt() >= 0 &&
                    static_cast<std::size_t>(module->asInt()) < modules.entries.size()) {
                    const auto& m = modules.entries[static_cast<std::size_t>(module->asInt())];
                    const auto name = modules.plugin.find(m.format + ":" + m.id);
                    line += name != modules.plugin.end()
                                ? name->second
                                : (m.format.empty() ? std::string("module") : m.format) + " " +
                                      (m.id.empty() ? std::string("(no identity)") : shortId(m.id));
                } else {
                    line += "(code with no file)";
                }
                if (!str(f, "offset").empty()) line += " +" + str(f, "offset");
                if (get(f, "return") && !flag(f, "return")) line += "  (where it stopped)";
                s.lines.push_back(std::move(line));
            }
        }
        page.sections.push_back(std::move(s));
    }

    if (const auto* commands = list(report, "commands"); commands && !commands->empty()) {
        ReportSection s;
        s.id = "commands";
        s.title = "Recent commands";
        s.count = commands->size();
        s.note =
            "The last commands the model thread began, oldest first. One marked \"running\" had "
            "not finished when the program stopped. Names only; no arguments or project content "
            "are recorded.";
        for (const auto& c : *commands) {
            const std::string state = str(c, "state");
            s.lines.push_back((state == "begun" ? "running  " : "done     ") +
                              (str(c, "name").empty() ? std::string("unknown") : str(c, "name")) +
                              "  (" + commandOrigin(str(c, "origin")) + ")");
        }
        page.sections.push_back(std::move(s));
    }

    // What the report could not record: its own list, and every counter that is not zero.
    {
        ReportSection s;
        s.id = "limits";
        s.note = "What this report does not hold, and what it had to leave out.";
        if (const auto* unavailable = list(report, "unavailable"))
            for (const auto& u : *unavailable)
                if (u.isString()) s.lines.push_back(u.asString());
        if (const auto* dropped = get(report, "dropped"); dropped && dropped->isObject()) {
            static const std::map<std::string, std::string> names{
                {"threads", "threads left out beyond the report's limit"},
                {"unregistered_threads", "threads the program had no record of"},
                {"plugins", "plug-ins left out beyond the last 64 destroyed"},
                {"commands",
                 "commands left out because their records were damaged or came from "
                 "another thread"},
                {"cells", "plug-in activity that could not be tied to a thread"},
                {"messages", "messages from the program that were lost"},
                {"untracked", "plug-in instances that were never tracked"},
                {"bridges", "separate processes left out beyond the last 256"},
                {"bridge_reports", "separate-process stops without a report of their own"},
            };
            for (const auto& [key, value] : dropped->asObject()) {
                if (!value.isInt() || value.asInt() == 0) continue;
                const auto it = names.find(key);
                s.lines.push_back(std::to_string(value.asInt()) + " " +
                                  (it != names.end() ? it->second : key));
            }
        } else if (dropped && dropped->isInt() && dropped->asInt() != 0) {
            s.lines.push_back(std::to_string(dropped->asInt()) + " items left out");
        }
        if (s.lines.empty()) s.lines.push_back("Nothing is noted as missing.");
        s.title = "What is missing";
        s.count = s.lines.size();
        page.sections.push_back(std::move(s));
    }
    return page;
}

ReportPage describeUnreportedRun(const std::string& role, const std::string& started) {
    ReportPage page;
    const std::string app = appName(role);
    page.headline = app + " closed unexpectedly, and no crash report was written.";
    page.subtitle =
        (started.empty() ? std::string("Started at an unknown time") : "Started " + started) +
        " · " + roleName(role);
    page.sentences = {
        "That happens when the program that watches it stopped too, or could not save one.",
        "Nothing else is known about this run. Nothing was sent anywhere."};
    return page;
}

std::string ReportPage::text() const {
    std::string out = headline + "\n" + subtitle + "\n";
    for (const auto& s : sentences) out += "\n" + s;
    if (!sentences.empty()) out += "\n";
    for (const auto& section : this->sections) {
        out += "\n" + section.title + " (" + std::to_string(section.count) + ")\n";
        if (!section.note.empty()) out += section.note + "\n";
        for (const auto& line : section.lines) out += line + "\n";
    }
    return out;
}

}  // namespace supple::gui
