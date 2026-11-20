// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// dsp.export (ADR-0032 §5), shared by the engine and the sandbox: a script
// as a plugin for other hosts.
//
//   - format clap: a copy of the script player that plays the script here,
//     with the script in a trailer or bundle resource (supple/script_export.hpp). The copy offers
//     that one plugin, under the script's id, and reads no library. Beside
//     it go the licenses of the player and what it includes.
//   - format source: a project of the plugin template that builds the
//     script as C++ (supple/script_native.h) with the suite's glue: the
//     template's framework and the suite from the export kit staged beside
//     the stock plugins (plugins/suite/CMakeLists.txt), the script verbatim,
//     and the product's files made from the kit's (plugins/suite/export).
//
// Host is the Context's service and has scriptDir(), findPlugin(id) and
// plugins(rescan).
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dsp_export_platform.h"
#include "dsp_shared.h"
#include "supple/base/files.h"
#include "supple/cmd/bus.h"
#include "supple/host/clap_host.h"
#include "supple/script_export.hpp"

namespace supple::dsp_commands {
namespace exporting {
namespace fs = std::filesystem;

enum class Format { Clap, Source };
enum class License { Mit, Gpl3 };

struct ExportArgs {
    std::string script;
    std::optional<Format> format;
    std::optional<std::string> folder;
    std::optional<License> license;
};
struct ExportResult {
    std::string path, id, name, format;
    std::optional<std::string> player;
    std::optional<std::int64_t> bytes;
    std::optional<std::string> notices;
};

// A plugin's name as a file name: what file systems refuse becomes '-'.
inline std::string fileName(std::string_view name, std::string_view fallback) {
    std::string out;
    for (const char c : name) {
        const bool bad = static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\' ||
                         c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                         c == '|';
        out += bad ? '-' : c;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out.empty() ? std::string(fallback) : out;
}

// The notices to ship with an export of `module`: the player's
// <stem>-LICENSE.txt and <stem>-NOTICES.txt as one text, or, when `module` is
// itself an export, the <stem>-NOTICES.txt that already is that text.
inline std::optional<std::string> playerNotices(const fs::path& module, bool fromExport) {
    const fs::path dir = module.parent_path();
    const fs::path stem = module.stem();
    auto notices = readTextFile(dir / fs::path(stem).concat("-NOTICES.txt"), std::size_t(4) << 20);
    if (!notices) return std::nullopt;
    if (fromExport) return std::move(*notices);
    auto license = readTextFile(dir / fs::path(stem).concat("-LICENSE.txt"), std::size_t(1) << 20);
    if (!license) return std::nullopt;
#if defined(__APPLE__)
    return "This plugin is a copy of the script player with a DSP script embedded. The script "
#else
    return "This plugin is a copy of the script player with a DSP script appended. The script "
#endif
           "is its author's. The player's license and the notices of the software it includes "
           "follow.\n\n"
           "==============================================================================\n" +
           *license + "\n" + *notices;
}

// --- source exports ------------------------------------------------------------------

// The kit beside the stock plugins: template/, suite/ and files/.
inline Result<fs::path> exportKit() {
    std::error_code ec;
    const auto stock = host::stockPluginDir();
    const fs::path kit = stock ? stock->parent_path() / "script-export" : fs::path();
    if (kit.empty() || !fs::is_directory(kit / "template" / "framework", ec) ||
        !fs::is_directory(kit / "suite", ec) || !fs::is_directory(kit / "files", ec))
        return Error("dsp.kit", "no source export kit beside the stock plugins", "/format",
                     "build the script-export-kit target, or export format clap");
    return kit;
}

// Text that lands in a quoted CMake string: no quotes, escapes, variables,
// list separators or line breaks.
inline std::string cmakeText(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"')
            out += '\'';
        else if (c == '\n' || c == '\r' || c == '\t')
            out += ' ';
        else if (!(static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == '$' || c == ';' ||
                   c == '@'))
            out += c;
    }
    return out;
}

// Why a plugin's name cannot name a product (a bundle's file name on every
// platform, and a quoted CMake string), or "" when it can.
inline std::string unusableName(const std::string& name) {
    for (const char c : name)
        if (static_cast<unsigned char>(c) < 0x20 ||
            std::string_view("/\\:*?\"<>|$;@").find(c) != std::string_view::npos)
            return std::string("it contains '") + c + "'";
    if (name.back() == '.' || name.back() == ' ') return "it ends in a dot or a space";
    std::string device;
    for (const char c : name.substr(0, name.find('.')))
        device += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (!device.empty() && device.back() == ' ') device.pop_back();
    for (const char* d : {"CON", "PRN", "AUX", "NUL"})
        if (device == d) return "it is a Windows device name";
    if (device.size() == 4 && (device.starts_with("COM") || device.starts_with("LPT")) &&
        std::isdigit(static_cast<unsigned char>(device[3])))
        return "it is a Windows device name";
    return "";
}

// The vendor, or a name made from the id's second word: com.example.tilt -> Example.
inline std::string vendorOf(const scriptc::InfoModel& info) {
    if (!info.vendor.empty()) return info.vendor;
    const size_t a = info.id.find('.');
    const size_t b = info.id.find('.', a + 1);
    std::string word = a == std::string::npos ? info.id : info.id.substr(a + 1, b - a - 1);
    if (!word.empty())
        word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
    return word;
}

// Four letters or digits for the AU manufacturer, made from the vendor.
inline std::string manufacturerCode(const std::string& vendor) {
    std::string code;
    for (const char c : vendor)
        if (std::isalnum(static_cast<unsigned char>(c)) && code.size() < 4) code += c;
    while (code.size() < 4) code += 'x';
    code[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(code[0])));
    return code;
}

inline std::string substitute(std::string text, const std::map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) {
        const std::string mark = "@" + key + "@";
        for (size_t at = text.find(mark); at != std::string::npos;
             at = text.find(mark, at + value.size()))
            text.replace(at, mark.size(), value);
    }
    return text;
}

// The parameter the project's host tests move, an eighth of its range up
// from its default, expecting an effect's output to differ from its input:
// a level in decibels when there is one (a neutral bell stays neutral
// whatever its frequency), else the first a host can automate. The tests
// find it by name, which travels through CMake into a C string, so only a
// name with none of " \ $ ; @ or control characters will do. "" for none.
inline bool plainName(std::string_view name) {
    return std::none_of(name.begin(), name.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '\\' || c == '$' ||
               c == ';' || c == '@';
    });
}
inline std::string smokeParam(const scriptc::Program& program) {
    std::string first;
    for (const scriptc::ParamModel& p : program.params()) {
        if (!p.automatable || p.output || p.hidden || !plainName(p.name)) continue;
        if (p.unit == scriptc::ParamModel::Unit::Decibels && p.def < p.max) return p.name;
        if (first.empty()) first = p.name;
    }
    return first;
}

// Writes the source project for `program` (compiled from `source`, the
// library's script `name`) into the empty folder `dest`.
inline Status writeSource(const fs::path& kit, const fs::path& dest, const std::string& name,
                          const std::string& source, const scriptc::Program& program,
                          License license) {
    const scriptc::InfoModel& info = program.info();
    const std::string vendor = vendorOf(info);
    const std::string smoke = smokeParam(program);
    // The kind and bus layouts the player's glue gives the script
    // (script_definition.cpp): an instrument is stereo unless it says.
    const auto kind = program.kind();
    const bool instrument = kind == scriptc::PluginKind::Instrument;
    const bool noteEffect = kind == scriptc::PluginKind::NoteEffect;
    std::vector<int> widths = info.channels;
    if (widths.empty() && !noteEffect)
        widths = instrument ? std::vector<int>{2} : std::vector<int>{2, 1};
    std::string channels;
    for (int w : noteEffect ? std::vector<int>{} : widths)
        channels += (channels.empty() ? "" : " ") + std::to_string(w);
    std::string keys;
    for (const scriptc::ParamModel& p : program.params())
        keys += "                {&Script::" + p.member + ", \"" + p.key + "\"},\n";
    if (!keys.empty()) keys.pop_back();
    std::string fileKeys;
    for (const scriptc::FileModel& f : program.files())
        fileKeys += "                {&Script::" + f.member + ", \"" + f.key + "\"},\n";
    for (const scriptc::SourceModel& s : program.sources())
        fileKeys += "                {&Script::" + s.member + ", \"" + s.key + "\"},\n";
    if (!fileKeys.empty()) fileKeys.pop_back();
    std::string code;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c))) code += c;
    auto version = readTextFile(kit / "template" / "framework" / "VERSION.txt", 64);
    if (!version) return std::move(version).error();
    while (!version->empty() && std::isspace(static_cast<unsigned char>(version->back())))
        version->pop_back();
    const auto today = std::chrono::year_month_day(
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    const std::string year = std::to_string(static_cast<int>(today.year()));
    std::string features;  // a CMake list
    for (const auto& f : info.features) features += (features.empty() ? "" : ";") + f;
    std::string outputs;  // CMake arguments, one quoted name each
    for (const auto& o : info.outputs)
        outputs += (outputs.empty() ? "\"" : " \"") + cmakeText(o) + "\"";
    if (outputs.empty()) outputs = "\"\"";
    const bool gpl = license == License::Gpl3;
    const std::string holder = cmakeText(vendor);
    const std::map<std::string, std::string> values = {
        {"NAME", info.name},
        {"ID", info.id},
        {"VERSION", info.version},
        {"DESCRIPTION",
         cmakeText(info.description.empty() ? info.name + ", a DSP script." : info.description)},
        {"VENDOR", holder},
        {"URL", cmakeText(info.url)},
        {"YEAR", year},
        {"HOLDER", holder},
        {"COPYRIGHT", "Copyright (c) " + year + " " + holder},
        {"CODE", code},
        {"KIND", instrument   ? "instrument"
                 : noteEffect ? "note-effect"
                              : "effect"},
        {"CHANNELS", channels},
        {"FEATURES", features},
        {"AUX_OUTPUTS", outputs},
        {"MANUFACTURER", manufacturerCode(vendor)},
        {"TAG", script::detail::stateTag(info.id)},
        {"SMOKE_PARAM", smoke},
        {"FILE", name + ".cpp"},
        {"TYPE", program.typeName()},
        {"KEYS", keys},
        {"FILE_KEYS", fileKeys},
        {"LICENSE", gpl ? "GPL-3.0-or-later" : "MIT"},
        {"LICENSE_TEXT", gpl ? info.name +
                                   " is free software, licensed under the GNU General Public "
                                   "License version 3 or (at your option) any later version; see "
                                   "[LICENSE](LICENSE)."
                             : info.name + " is MIT licensed; see [LICENSE](LICENSE)."},
        {"FRAMEWORK_VERSION", *version}};

    std::error_code ec;
    const auto copyTree = [&](const fs::path& from, const fs::path& to) -> Status {
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive, ec);
        if (ec)
            return Error("file.write", "cannot copy " + from.string() + " to " + to.string() +
                                           ": " + ec.message());
        return {};
    };
    SUPPLE_TRY(copyTree(kit / "template", dest));
    SUPPLE_TRY(copyTree(kit / "suite", dest / "suite"));
    const fs::path files = kit / "files";
    for (fs::recursive_directory_iterator it(files, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = fs::relative(it->path(), files, ec);
        if (rel.extension() == ".in") rel.replace_extension();
        auto text = readTextFile(it->path(), std::size_t(1) << 20);
        if (!text) return std::move(text).error();
        SUPPLE_TRY(writeFileAtomic(dest / rel, substitute(std::move(*text), values)));
    }
    if (ec) return Error("file.read", "cannot read the export kit: " + ec.message());
    SUPPLE_TRY(writeFileAtomic(dest / "product" / "script" / (name + ".cpp"), source));
    const fs::path licenses = dest / "framework";
    if (gpl) {
        auto text = readTextFile(licenses / "tools" / "licenses" / "GPL-3.0-or-later.txt");
        if (!text) return std::move(text).error();
        SUPPLE_TRY(writeFileAtomic(dest / "LICENSE", *text));
    } else {
        auto text = readTextFile(licenses / "LICENSE");
        if (!text) return std::move(text).error();
        const size_t at = text->find("Copyright (c) ");
        if (at != std::string::npos)
            text->replace(at, text->find('\n', at) - at, "Copyright (c) " + year + " " + holder);
        SUPPLE_TRY(writeFileAtomic(dest / "LICENSE", *text));
    }
    return {};
}

}  // namespace exporting

template <class Host>
void registerDspExport(cmd::Registry& registry, cmd::Exposure exposure = cmd::Exposure::McpTool) {
    using namespace exporting;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    Command<ExportArgs, ExportResult> c;
    c.name = "dsp.export";
    c.kind = Kind::Action;
    c.exposure = exposure;
    c.summary = "Export a DSP script as a plugin: a CLAP file, or C++ source.";
    c.description =
        "format clap (the default): copies the platform's script player and embeds the script "
        "in its trailer (Linux/Windows) or bundle resource (macOS, re-signed ad hoc with the "
        "system codesign tool). The copy is a plugin of its own, under the script's id and name, "
        "that plays this version of the script in any CLAP host (it reads no library), "
        "written to <folder>/<plugin name>.clap (default: the script's folder, under export/) "
        "with the licenses to ship beside it (notices). It runs on this platform, as fast as "
        "the player it copies (a debug build's is slow). format source: a C++ project of the "
        "plugin template that builds the same plugin (same id, parameters and state) as "
        "CLAP and VST3, plus AU on macOS, with the "
        "script compiled as C++, written to the empty "
        "<folder> (default: the script's folder, under export/<script>-source); its README "
        "says how to build it, and license (MIT unless GPL-3.0-or-later) is the license of its "
        "own files. Export again after changing the script; an export does not follow the "
        "library.";
    c.tags = {"dsp"};
    c.args =
        schema::object<ExportArgs>("dsp_export_args")
            .field("script", &ExportArgs::script,
                   schema::string().minLength(1).doc("a name from dsp.list"))
            .field("format", &ExportArgs::format,
                   schema::enumeration<Format>({{Format::Clap, "clap"}, {Format::Source, "source"}})
                       .doc("clap (default): a plugin file; source: a C++ project"))
            .field("folder", &ExportArgs::folder,
                   schema::string().minLength(1).doc(
                       "an absolute folder to write to (for clap, ~/.clap installs it for this "
                       "user); for source, empty or new"))
            .field("license", &ExportArgs::license,
                   schema::enumeration<License>(
                       {{License::Mit, "MIT"}, {License::Gpl3, "GPL-3.0-or-later"}})
                       .doc("source: the license of the project's own files (default MIT)"))
            .build();
    c.result = schema::object<ExportResult>("dsp_export_result")
                   .field("path", &ExportResult::path,
                          schema::string().doc("the plugin file, or the project's folder"))
                   .field("id", &ExportResult::id, schema::string())
                   .field("name", &ExportResult::name, schema::string())
                   .field("format", &ExportResult::format, schema::string().doc("clap or source"))
                   .field("player", &ExportResult::player,
                          schema::string().doc("clap: the script player it was copied from"))
                   .field("bytes", &ExportResult::bytes, schema::integer().min(0))
                   .field("notices", &ExportResult::notices,
                          schema::string().doc("clap: the licenses to ship with the plugin"))
                   .build();
    c.errors = {{"dsp.sign", "macOS: ad-hoc bundle signing failed or codesign is unavailable",
                 "restore the player and system codesign tool, or use format source"},
                {"dsp.compile", "the script does not compile", "dsp.check gives its diagnostics"},
                {"dsp.id_taken", "another script in the library has the script's id",
                 "give each script its own Info.id"},
                {"dsp.not_listed", "the script compiles but no plugin has its id",
                 "put the script player on the plugin path"},
                {"dsp.player", "the plugin with the script's id is not the script player",
                 "give the script an id no other plugin has"},
                {"dsp.notices", "the player's license or notices cannot be read",
                 "restore the license and notices beside the player before exporting"},
                {"dsp.path", "the folder is relative, or (source) not empty",
                 "give an absolute folder; for source, a new one"},
                {"dsp.plugin_name", "source: the plugin's name cannot name a project",
                 "rename it in Info.name"},
                {"dsp.no_parameter",
                 "source: the project's tests need a parameter a host can automate, plainly named",
                 "declare one"},
                {"dsp.kit", "source: no export kit beside the stock plugins",
                 "build the script-export-kit target"}};
    {
        json::Value args = json::Value::object();
        args["script"] = "gain";
        json::Value source = json::Value::object();
        source["script"] = "gain";
        source["format"] = "source";
        c.examples = {{"Export a script from the library as a plugin file", std::move(args),
                       detail::j(R"({"path":"<any>","id":"com.example.gain","name":"Gain",
                                     "format":"clap","player":"<any>","bytes":"<any>",
                                     "notices":"<any>"})"),
                       "dsp_player_export"},
                      {"Export it as a C++ project", std::move(source),
                       detail::j(R"({"path":"<any>","id":"com.example.gain","name":"Gain",
                                     "format":"source"})"),
                       "dsp_script"}};
    }
    c.run = [](const ExportArgs& a, Context& ctx) -> Result<ExportResult> {
        Host& s = ctx.service<Host>();
        if (a.folder && !binary_export::path(*a.folder).is_absolute())
            return Error("dsp.path", "'" + *a.folder + "' is not an absolute folder", "/folder",
                         "give the whole path, such as /home/<you>/.clap");
        auto text = detail::readScript(s, a.script, "/script");
        if (!text) return std::move(text).error();
        const scriptc::Result built = scriptc::compile(*text);
        if (!built.program)
            return Error("dsp.compile", "the script does not compile", "/script",
                         "dsp.check {name: \"" + a.script + "\"} gives its diagnostics");
        const scriptc::Program& program = *built.program;
        SUPPLE_TRY(detail::uniqueId(s, program.info().id, a.script, "/script"));

        if (a.format == Format::Source) {
            const scriptc::InfoModel& info = program.info();
            if (const std::string why = unusableName(info.name); !why.empty())
                return Error("dsp.plugin_name", "'" + info.name + "' cannot name a project: " + why,
                             "/script",
                             "rename it in Info.name: letters, digits, spaces and - _ . ( ) are "
                             "safe");
            if (smokeParam(program).empty())
                return Error("dsp.no_parameter",
                             info.name +
                                 " has no parameter a host can automate with a plain name, "
                                 "which the project's own tests set",
                             "/script",
                             "declare one, such as Param mix = percent(\"Mix\", 100); its name "
                             "may not hold quotes, backslashes, $, ; or @");
            auto kit = exportKit();
            if (!kit) return std::move(kit).error();
            const fs::path dest =
                a.folder ? fs::path(*a.folder)
                         : s.scriptDir() / a.script / "export" / (a.script + "-source");
            std::error_code ec;
            if (fs::exists(dest, ec) && !fs::is_empty(dest, ec))
                return Error("dsp.path", dest.string() + " is not empty", "/folder",
                             "give a new folder, such as " + dest.string() + "-2");
            SUPPLE_TRY(writeSource(*kit, dest, a.script, *text, program,
                                   a.license.value_or(License::Mit)));
            return ExportResult{dest.string(), info.id, info.name, "source", {}, {}, {}};
        }

        const std::string& id = program.info().id;
        auto info = s.findPlugin(id);
        if (!info) {
            (void)s.plugins(true);  // a script written since the last scan
            info = s.findPlugin(id);
        }
        if (!info)
            return Error("dsp.not_listed", "no plugin has the id " + id, "/script",
                         "the script player must be on the plugin path");
        if (info->format != "clap" || info->path.extension() != ".clap")
            return Error("dsp.player",
                         id + " is played by " + binary_export::text(info->path) + ", not a .clap",
                         "/script", "put the script player's .clap on the plugin path");
        auto binary = binary_export::moduleFile(info->path);
        if (!binary) return std::move(binary).error();
        auto module = readTextFile(*binary);
        if (!module) return std::move(module).error();
        // An export of an export starts again from the player, which keeps
        // its mark in its file.
        const std::string_view player = scriptc::exportPlayer(*module);
        if (player.find(scriptc::kPlayerMark) == std::string_view::npos)
            return Error("dsp.player",
                         id + " belongs to " + binary_export::text(info->path) +
                             ", which is not the script player",
                         "/script", "give the script an id no other plugin has");

        bool fromExport = player.size() != module->size();
#if defined(__APPLE__)
        std::error_code resourceError;
        fromExport =
            fs::exists(info->path / "Contents/Resources" / scriptc::kExportResource, resourceError);
#endif
        auto notices = playerNotices(info->path, fromExport);
        if (!notices)
            return Error("dsp.notices",
                         "cannot read the licenses for " + binary_export::text(info->path),
                         "/script",
                         "restore the player's -LICENSE.txt and -NOTICES.txt files beside it; "
                         "an existing export needs its -NOTICES.txt file");

        json::Value payload = json::Value::object();
        payload["schema"] = std::string(scriptc::kExportSchema);
        payload["name"] = a.script;
        payload["source"] = *text;
        const fs::path folder =
            a.folder ? binary_export::path(*a.folder) : s.scriptDir() / a.script / "export";
        const fs::path file =
            folder / binary_export::path(fileName(program.info().name, a.script) + ".clap");
        std::int64_t byteCount = 0;
#if defined(__APPLE__)
        auto written = binary_export::writeBundle(info->path, file, json::writeCompact(payload));
        if (!written) return std::move(written).error();
        byteCount = *written;
#else
        std::string bytes(player);
        bytes += scriptc::exportTrailer(json::writeCompact(payload));
        SUPPLE_TRY(writeFileAtomic(file, bytes));
        std::error_code ec;
        fs::permissions(file,
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
        byteCount = static_cast<std::int64_t>(bytes.size());
#endif
        ExportResult out{binary_export::text(file),
                         id,
                         program.info().name,
                         "clap",
                         binary_export::text(info->path),
                         byteCount,
                         {}};
        const fs::path notes = folder / fs::path(file.stem()).concat("-NOTICES.txt");
        SUPPLE_TRY(writeFileAtomic(notes, *notices));
        out.notices = binary_export::text(notes);
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::dsp_commands
