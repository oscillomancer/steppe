// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// DSP scripts (ADR-0032), shared by the engine and the sandbox: the commands
// that write, edit, check, read and list the scripts the script player turns into
// plugins. A script is <library>/<name>/<name>.cpp (Host::scriptDir()); once
// it compiles, plugin.list shows it under its own id and it is loaded, set
// and saved like any other plugin. Live instances follow edits by themselves.
//
// Host needs scriptDir() and plugins(bool rescan), and is the Context's
// service: the engine's app in the engine, the session in the sandbox.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "script_library.h"
#include "supple/base/files.h"
#include "supple/cmd/bus.h"
#include "supple/script_compiler.hpp"
#include "supple/script_program.hpp"

namespace supple::dsp_commands {
namespace detail {
namespace fs = std::filesystem;

// The longest script: the player reads no more.
inline constexpr std::size_t kMaxScriptBytes = std::size_t(1) << 20;

inline json::Value j(std::string_view text) { return json::parse(text).value(); }

struct Diagnostic {
    std::string severity, code, message, hint;
    std::int64_t line = 0, column = 0;
};
struct ParamEntry {
    std::string key, name, kind;
    std::optional<std::string> unit;
    double min = 0.0, max = 0.0, def = 0.0;
    std::vector<std::string> choices;
    bool meter = false;
};
struct FileEntry {
    std::string key, name, kind;
    std::optional<std::string> description;
};
struct PluginSummary {
    std::string id, name, kind;
    std::vector<ParamEntry> params;
    std::optional<std::vector<FileEntry>> files;
};
struct Compiled {
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::optional<PluginSummary> plugin;
};

struct WriteArgs {
    std::string name, source;
};
struct WriteResult {
    std::string name, path;
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::optional<PluginSummary> plugin;
    bool listed = false;
};
struct Replacement {
    std::string before, after;
};
struct EditArgs {
    std::string name;
    std::vector<Replacement> edits;
};
struct CheckArgs {
    std::optional<std::string> name, source;
};
struct NameArgs {
    std::string name;
};
struct GetResult {
    std::string name, path, source;
};
struct ListArgs {};
struct ListEntry {
    std::string name, path;
    bool ok = false;
    std::int64_t errors = 0;
    std::optional<std::string> id, pluginName;
};
struct ListResult {
    std::string library;
    std::vector<ListEntry> scripts;
};

inline const char* unitName(scriptc::ParamModel::Unit u) {
    using U = scriptc::ParamModel::Unit;
    switch (u) {
        case U::Decibels:
            return "dB";
        case U::Hertz:
            return "Hz";
        case U::Milliseconds:
            return "ms";
        case U::Percent:
            return "%";
        case U::Ratio:
            return "ratio";
        case U::Semitones:
            return "st";
        case U::Cents:
            return "ct";
        case U::Octaves:
            return "oct";
        case U::Degrees:
            return "deg";
        case U::Bpm:
            return "bpm";
        case U::Samples:
            return "samples";
        case U::Meters:
            return "m";
        default:
            return nullptr;
    }
}

inline const char* kindName(scriptc::ParamModel::Kind k) {
    using K = scriptc::ParamModel::Kind;
    switch (k) {
        case K::Integer:
            return "integer";
        case K::Choice:
            return "choice";
        case K::Toggle:
            return "toggle";
        default:
            return "continuous";
    }
}

inline Compiled compile(const std::string& source) {
    const scriptc::Result r = scriptc::compile(source);
    Compiled out;
    out.ok = r.program != nullptr;
    for (const scriptc::Message& m : r.diagnostics)
        out.diagnostics.push_back(
            {m.error ? "error" : "warning", m.code, m.message, m.hint, m.line, m.column});
    if (r.program) {
        PluginSummary p{
            r.program->info().id,
            r.program->info().name,
            r.program->kind() == scriptc::PluginKind::Instrument ? "instrument" : "effect",
            {},
            {}};
        for (const scriptc::ParamModel& m : r.program->params()) {
            ParamEntry e{m.key, m.name, kindName(m.kind), {},      m.min,
                         m.max, m.def,  m.choices,        m.output};
            if (const char* u = unitName(m.unit)) e.unit = u;
            p.params.push_back(std::move(e));
        }
        for (const scriptc::FileModel& m : r.program->files()) {
            FileEntry e{m.key,
                        m.name,
                        m.kind == scriptc::FileModel::Kind::Impulse ? "impulse" : "audio",
                        {}};
            if (!m.description.empty()) e.description = m.description;
            if (!p.files) p.files.emplace();
            p.files->push_back(std::move(e));
        }
        out.plugin = std::move(p);
    }
    return out;
}

inline Status validName(const std::string& name, const std::string& path) {
    const bool ok =
        !name.empty() && name.size() <= 64 && name[0] >= 'a' && name[0] <= 'z' &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    if (!ok)
        return Error("dsp.name", "'" + name + "' is not a script name", path,
                     "use 1-64 lower-case letters, digits, '_' or '-', starting with a letter: "
                     "\"tilt_eq\"");
    return {};
}

template <class Host>
fs::path scriptPath(const Host& a, const std::string& name) {
    return a.scriptDir() / name / (name + ".cpp");
}

template <class Host>
Result<std::string> readScript(const Host& a, const std::string& name, const std::string& path) {
    SUPPLE_TRY(validName(name, path));
    const fs::path file = scriptPath(a, name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        std::vector<std::string> names;
        for (fs::directory_iterator it(a.scriptDir(), ec), end; !ec && it != end; it.increment(ec))
            if (fs::is_regular_file(it->path() / (it->path().filename().string() + ".cpp"), ec))
                names.push_back(it->path().filename().string());
        std::sort(names.begin(), names.end());
        std::string known;
        for (const auto& n : names) known += (known.empty() ? "" : ", ") + n;
        return Error("dsp.unknown", "no script '" + name + "' in " + a.scriptDir().string(), path,
                     known.empty() ? "write one with dsp.write" : "the library has: " + known);
    }
    return readTextFile(file, kMaxScriptBytes);
}

// The library's other scripts whose plugin has `id`, by name. The player
// offers one plugin per ID (the first it finds), so a name among them may
// not be the code that plays.
template <class Host>
std::vector<std::string> scriptsWithId(const Host& a, const std::string& id,
                                       const std::string& except) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(a.scriptDir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const fs::path file = it->path() / (name + ".cpp");
        if (name == except || !fs::is_regular_file(file, ec)) continue;
        auto text = readTextFile(file, kMaxScriptBytes);
        if (!text) continue;
        const scriptc::Result r = scriptc::compile(*text);
        if (r.program && r.program->info().id == id) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// An error when another script in the library has the plugin ID `id`.
template <class Host>
Status uniqueId(const Host& a, const std::string& id, const std::string& name,
                const std::string& path) {
    const std::vector<std::string> others = scriptsWithId(a, id, name);
    if (others.empty()) return {};
    std::string list;
    for (const auto& n : others) list += (list.empty() ? "" : ", ") + n;
    return Error("dsp.id_taken",
                 "the script " + list + " already has the id " + id +
                     ", and the player plays one plugin per id",
                 path, "give Info.id a value no other script in the library has");
}

// The line, from 1, that holds byte `at` of `text`.
inline std::int64_t lineAt(const std::string& text, std::size_t at) {
    return 1 + std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n');
}

// A few characters of `text` from `at`, line breaks and tabs shown.
inline std::string excerpt(const std::string& text, std::size_t at) {
    std::string out;
    for (std::size_t i = at; i < text.size() && i < at + 24; ++i)
        out += text[i] == '\n'   ? "\\n"
               : text[i] == '\r' ? "\\r"
               : text[i] == '\t' ? "\\t"
                                 : std::string(1, text[i]);
    return out;
}

// `text` after `edits`, each replacing the one place its old text is in the
// script as the edits before it left it; an error at the first edit whose old
// text is nowhere, or in more than one place.
// The edits in turn, each old text found exactly once; errors carry `code`
// and name `getter`, the command that shows the text as it stands.
inline Result<std::string> applyEdits(std::string text, const std::vector<Replacement>& edits,
                                      const std::string& code = "dsp.edit",
                                      const std::string& getter = "dsp.get") {
    // Plain line ends as Windows ones, where the script has those.
    auto crlf = [](const std::string& from) {
        std::string out;
        for (std::size_t i = 0; i < from.size(); ++i) {
            if (from[i] == '\n' && (i == 0 || from[i - 1] != '\r')) out += '\r';
            out += from[i];
        }
        return out;
    };
    for (std::size_t k = 0; k < edits.size(); ++k) {
        std::string old = edits[k].before, replacement = edits[k].after;
        // A script saved with Windows line ends, edited with text copied
        // without them (as a caller usually has it).
        if (text.find(old) == std::string::npos && old.find('\n') != std::string::npos &&
            old.find('\r') == std::string::npos && text.find("\r\n") != std::string::npos) {
            old = crlf(old);
            replacement = crlf(replacement);
        }
        const std::string path = "/edits/" + std::to_string(k) + "/old";
        const std::string which = "edit " + std::to_string(k) + "'s old text";
        const std::size_t at = text.find(old);
        if (at == std::string::npos) {
            // The longest start of it that the script has, to say where it
            // parts, when that start is in one place.
            std::size_t lo = 0, hi = old.size();
            while (lo < hi) {
                const std::size_t mid = (lo + hi + 1) / 2;
                if (text.find(old.data(), 0, mid) != std::string::npos)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            std::string message = which + " is not in the script";
            const std::size_t from = lo > 0 ? text.find(old.data(), 0, lo) : std::string::npos;
            if (from != std::string::npos &&
                text.find(old.data(), from + 1, lo) == std::string::npos) {
                message += ": its first " + std::to_string(lo) + " characters are, at line " +
                           std::to_string(lineAt(text, from)) + ", where the script goes on \"" +
                           excerpt(text, from + lo) + "\" and old with \"" + excerpt(old, lo) +
                           "\"";
            }
            return Error(code, message, path,
                         getter +
                             " gives the script as it stands; copy old from it exactly, "
                             "spaces and line breaks included");
        }
        if (text.find(old, at + 1) != std::string::npos) {
            std::string lines;
            std::size_t count = 0;
            for (std::size_t p = at; p != std::string::npos; p = text.find(old, p + 1))
                if (++count <= 5)
                    lines += (lines.empty() ? "" : ", ") + std::to_string(lineAt(text, p));
            return Error(code,
                         which + " is in the script " + std::to_string(count) +
                             " times (at lines " + lines + (count > 5 ? ", ..." : "") + ")",
                         path, "give old more of the text around the place, so it is in one place");
        }
        text.replace(at, old.size(), replacement);
    }
    return text;
}

// Saves `source` as the library's script `name` and compiles it: what
// dsp.write and dsp.edit return. `path` is the argument the source came from.
template <class Host>
Result<WriteResult> saveScript(Host& owner, const std::string& name, const std::string& source,
                               const std::string& path) {
    if (source.size() > kMaxScriptBytes)
        return Error("dsp.size",
                     "the script is " + std::to_string(source.size()) + " bytes, more than 1 MiB",
                     path, "split it up");
    const fs::path file = scriptPath(owner, name);
    Compiled compiled = compile(source);
    if (compiled.plugin) SUPPLE_TRY(uniqueId(owner, compiled.plugin->id, name, path));
    SUPPLE_TRY(writeFileAtomic(file, source));
    WriteResult out{name,
                    file.string(),
                    compiled.ok,
                    std::move(compiled.diagnostics),
                    std::move(compiled.plugin),
                    false};
    if (out.plugin) {
        // The player lists the library afresh at each scan.
        auto found = owner.plugins(true);
        if (found)
            out.listed = std::any_of(found->begin(), found->end(),
                                     [&](const auto& p) { return p.id == out.plugin->id; });
    }
    return out;
}

inline schema::Schema<Diagnostic> diagnosticSchema() {
    return schema::object<Diagnostic>("dsp_diagnostic")
        .field("severity", &Diagnostic::severity, schema::string().doc("error or warning"))
        .field("code", &Diagnostic::code, schema::string().doc("script.<area>.<what>"))
        .field("message", &Diagnostic::message, schema::string())
        .field("hint", &Diagnostic::hint, schema::string().doc("what to write instead"))
        .field("line", &Diagnostic::line, schema::integer())
        .field("column", &Diagnostic::column, schema::integer())
        .build();
}

inline schema::Schema<PluginSummary> pluginSchema() {
    auto param = schema::object<ParamEntry>("dsp_param")
                     .field("key", &ParamEntry::key, schema::string().doc("the patch key"))
                     .field("name", &ParamEntry::name, schema::string())
                     .field("kind", &ParamEntry::kind,
                            schema::string().doc("continuous, integer, choice or toggle"))
                     .field("unit", &ParamEntry::unit, schema::string().doc("dB, Hz, ms, % ..."))
                     .field("min", &ParamEntry::min, schema::number())
                     .field("max", &ParamEntry::max, schema::number())
                     .field("default", &ParamEntry::def, schema::number())
                     .field("choices", &ParamEntry::choices, schema::arrayOf(schema::string()))
                     .field("meter", &ParamEntry::meter,
                            schema::boolean().doc("an output the plugin reports"))
                     .build();
    auto file =
        schema::object<FileEntry>("dsp_file")
            .field("key", &FileEntry::key, schema::string().doc("the patch's properties key"))
            .field("name", &FileEntry::name, schema::string())
            .field("kind", &FileEntry::kind,
                   schema::string().doc("audio (a sample) or impulse (a Convolver's response)"))
            .field("description", &FileEntry::description, schema::string())
            .build();
    return schema::object<PluginSummary>("dsp_plugin")
        .field("id", &PluginSummary::id, schema::string())
        .field("name", &PluginSummary::name, schema::string())
        .field("kind", &PluginSummary::kind,
               schema::string().doc("effect (audio in, audio out) or instrument (notes in)"))
        .field("params", &PluginSummary::params, schema::arrayOf(param))
        .field("files", &PluginSummary::files,
               schema::arrayOf(file).doc(
                   "the script's files (AudioFile, Convolver), when it has any: give each an "
                   "absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"))
        .build();
}

inline schema::Schema<WriteResult> writeResultSchema() {
    return schema::object<WriteResult>("dsp_write_result")
        .field("name", &WriteResult::name, schema::string())
        .field("path", &WriteResult::path, schema::string())
        .field("ok", &WriteResult::ok, schema::boolean().doc("it compiled"))
        .field("diagnostics", &WriteResult::diagnostics, schema::arrayOf(diagnosticSchema()))
        .field("plugin", &WriteResult::plugin, pluginSchema())
        .field("listed", &WriteResult::listed,
               schema::boolean().doc("plugin.list shows it (the script player is on the plugin "
                                     "path)"))
        .build();
}

inline const char* const kGain = R"(#include <supple/script.h>

struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain"};
    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level change.");

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));
    }
};
)";

}  // namespace detail

template <class Host>
void registerDspCommands(cmd::Registry& registry) {
    using namespace detail;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    const auto app = [](Context& ctx) -> Host& { return ctx.service<Host>(); };
    const auto gainArgs = [] {
        json::Value a = json::Value::object();
        a["name"] = "gain";
        a["source"] = kGain;
        return a;
    };
    {
        Command<WriteArgs, WriteResult> c;
        c.name = "dsp.write";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save a DSP script to the library and compile it, with diagnostics.";
        c.description =
            "Writes <library>/<name>/<name>.cpp (replacing it), compiles it and returns the "
            "compiler's diagnostics, each with a hint. A script that compiles becomes a plugin "
            "of the script player under the id its Info gives: plugin.list shows it (listed) "
            "and fx.add loads it. Instances already playing it pick up the change within half "
            "a second, crossfaded; one that fails to compile leaves them playing the old code. "
            "Language: the dsp-language guide.";
        c.tags = {"dsp"};
        c.args = schema::object<WriteArgs>("dsp_write_args")
                     .field("name", &WriteArgs::name,
                            schema::string().minLength(1).doc("the script's folder and file name"))
                     .field("source", &WriteArgs::source,
                            schema::string().minLength(1).doc("the whole C++ file"))
                     .build();
        c.result = writeResultSchema();
        c.errors = {{"dsp.name", "not a script name", "use lower-case letters, digits, _ or -"},
                    {"dsp.size", "the script is longer than 1 MiB", "split it up"},
                    {"dsp.id_taken", "another script in the library has its id; not saved",
                     "give Info.id a value no other script has"}};
        c.examples = {{"Save a gain script", gainArgs(),
                       j(R"({"name":"gain","path":"<any>","ok":true,"diagnostics":[],
                             "plugin":"<any>","listed":"<any>"})"),
                       "empty"}};
        c.run = [app](const WriteArgs& a, Context& ctx) -> Result<WriteResult> {
            SUPPLE_TRY(validName(a.name, "/name"));
            return saveScript(app(ctx), a.name, a.source, "/source");
        };
        registry.add(std::move(c));
    }
    {
        Command<EditArgs, WriteResult> c;
        c.name = "dsp.edit";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Change part of a library script by replacing text; compile it, as dsp.write.";
        c.description =
            "For a change to part of a script, instead of sending all of it to dsp.write. Each "
            "edit replaces old, which must be in the script exactly once, with new (empty "
            "deletes it). The edits apply in order, each to the script as the ones before left "
            "it, and all or none: when one fails, nothing is saved and the error says where old "
            "and the script part. Then as dsp.write: it saves the script, compiles it and "
            "returns the diagnostics; instances playing it follow within half a second.";
        c.tags = {"dsp"};
        auto replacement =
            schema::object<Replacement>("dsp_edit")
                .field("old", &Replacement::before,
                       schema::string().minLength(1).doc("text the script has in one place"))
                .field("new", &Replacement::after, schema::string().doc("what replaces it"))
                .build();
        c.args = schema::object<EditArgs>("dsp_edit_args")
                     .field("name", &EditArgs::name, schema::string().minLength(1))
                     .field("edits", &EditArgs::edits,
                            schema::arrayOf(replacement).minItems(1).maxItems(256))
                     .build();
        c.result = writeResultSchema();
        c.errors = {{"dsp.unknown", "no such script", "dsp.list names the library's scripts"},
                    {"dsp.edit", "an edit's old text is not in the script, or in several places",
                     "copy old exactly from dsp.get's source, with enough around it to be unique"},
                    {"dsp.size", "the script would be longer than 1 MiB", "split it up"},
                    {"dsp.id_taken", "another script in the library has its id; not saved",
                     "give Info.id a value no other script has"}};
        c.examples = {{"Widen a parameter's range",
                       j(R"({"name":"gain","edits":[{"old":"-24, 24, 0","new":"-36, 36, 0"}]})"),
                       j(R"({"name":"gain","path":"<any>","ok":true,"diagnostics":[],
                             "plugin":"<any>","listed":"<any>"})"),
                       "dsp_script"}};
        c.run = [app](const EditArgs& a, Context& ctx) -> Result<WriteResult> {
            Host& owner = app(ctx);
            auto text = readScript(owner, a.name, "/name");
            if (!text) return std::move(text).error();
            auto edited = applyEdits(std::move(*text), a.edits);
            if (!edited) return std::move(edited).error();
            return saveScript(owner, a.name, *edited, "/edits");
        };
        registry.add(std::move(c));
    }
    {
        Command<CheckArgs, Compiled> c;
        c.name = "dsp.check";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Compile a DSP script without saving it; return its diagnostics.";
        c.description =
            "Give source (a whole script) or name (one in the library). ok is true when it "
            "compiles; plugin then lists its id, name and parameters as the patch shows them.";
        c.tags = {"dsp"};
        c.args = schema::object<CheckArgs>("dsp_check_args")
                     .field("name", &CheckArgs::name, schema::string().minLength(1))
                     .field("source", &CheckArgs::source, schema::string().minLength(1))
                     .build();
        c.result =
            schema::object<Compiled>("dsp_check_result")
                .field("ok", &Compiled::ok, schema::boolean())
                .field("diagnostics", &Compiled::diagnostics, schema::arrayOf(diagnosticSchema()))
                .field("plugin", &Compiled::plugin, pluginSchema())
                .build();
        c.errors = {{"dsp.args", "neither or both of name and source", "give exactly one"},
                    {"dsp.unknown", "no such script", "dsp.list names the library's scripts"}};
        c.examples = {{"Check a script in the library", j(R"({"name":"gain"})"),
                       j(R"({"ok":true,"diagnostics":[],"plugin":"<any>"})"), "dsp_script"},
                      {"A mistake, with its hint", j(R"({"source":"struct X : Effect {};"})"),
                       j(R"({"ok":false,"diagnostics":"<any>"})"), "empty"}};
        c.run = [app](const CheckArgs& a, Context& ctx) -> Result<Compiled> {
            if (a.name.has_value() == a.source.has_value())
                return Error("dsp.args", "give exactly one of name and source", "",
                             "source checks text; name checks a script in the library");
            if (a.source) return compile(*a.source);
            auto text = readScript(app(ctx), *a.name, "/name");
            if (!text) return std::move(text).error();
            return compile(*text);
        };
        registry.add(std::move(c));
    }
    {
        Command<NameArgs, GetResult> c;
        c.name = "dsp.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a DSP script from the library.";
        c.tags = {"dsp"};
        c.args = schema::object<NameArgs>("dsp_get_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<GetResult>("dsp_get_result")
                       .field("name", &GetResult::name, schema::string())
                       .field("path", &GetResult::path, schema::string())
                       .field("source", &GetResult::source, schema::string())
                       .build();
        c.errors = {{"dsp.unknown", "no such script", "dsp.list names the library's scripts"}};
        c.examples = {{"Read a script", j(R"({"name":"gain"})"),
                       j(R"({"name":"gain","path":"<any>","source":"<any>"})"), "dsp_script"}};
        c.run = [app](const NameArgs& a, Context& ctx) -> Result<GetResult> {
            auto text = readScript(app(ctx), a.name, "/name");
            if (!text) return std::move(text).error();
            return GetResult{a.name, scriptPath(app(ctx), a.name).string(), std::move(*text)};
        };
        registry.add(std::move(c));
    }
    {
        Command<ListArgs, ListResult> c;
        c.name = "dsp.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the DSP scripts in the library, compiled, with their plugin ids.";
        c.tags = {"dsp"};
        c.args = schema::object<ListArgs>("dsp_list_args").build();
        auto entry = schema::object<ListEntry>("dsp_list_entry")
                         .field("name", &ListEntry::name, schema::string())
                         .field("path", &ListEntry::path, schema::string())
                         .field("ok", &ListEntry::ok, schema::boolean().doc("it compiles"))
                         .field("errors", &ListEntry::errors, schema::integer().min(0))
                         .field("id", &ListEntry::id, schema::string().doc("its plugin's id"))
                         .field("plugin_name", &ListEntry::pluginName, schema::string())
                         .build();
        c.result = schema::object<ListResult>("dsp_list_result")
                       .field("library", &ListResult::library, schema::string())
                       .field("scripts", &ListResult::scripts, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"An empty library", j("{}"), j(R"({"library":"<any>","scripts":[]})"), "empty"}};
        c.run = [app](const ListArgs&, Context& ctx) -> Result<ListResult> {
            const Host& owner = app(ctx);
            ListResult out{owner.scriptDir().string(), {}};
            std::error_code ec;
            std::vector<fs::path> files;
            for (fs::directory_iterator it(owner.scriptDir(), ec), end; !ec && it != end;
                 it.increment(ec)) {
                const fs::path file = it->path() / (it->path().filename().string() + ".cpp");
                if (fs::is_regular_file(file, ec)) files.push_back(file);
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                ListEntry e{
                    file.parent_path().filename().string(), file.string(), false, 0, {}, {}};
                if (auto text = readTextFile(file, kMaxScriptBytes)) {
                    const Compiled compiled = compile(*text);
                    e.ok = compiled.ok;
                    e.errors =
                        std::count_if(compiled.diagnostics.begin(), compiled.diagnostics.end(),
                                      [](const Diagnostic& d) { return d.severity == "error"; });
                    if (compiled.plugin) {
                        e.id = compiled.plugin->id;
                        e.pluginName = compiled.plugin->name;
                    }
                } else {
                    e.errors = 1;
                }
                out.scripts.push_back(std::move(e));
            }
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace supple::dsp_commands
