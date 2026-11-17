// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player's catalog (core/Catalog.h). An export (a player with an embedded
// script, supple/script_export.hpp) offers that script's plugin and nothing
// else. Otherwise the player offers a plugin for each script in the library
// -- the folders SUPPLE_SCRIPT_LIBRARY names -- at <folder>/<name>/<name>.cpp,
// each with the ID, name and kind its script declares. A script that does
// not compile, or whose plugin would break the framework's rules, is left
// out; its diagnostics are what dsp.check reports. Two scripts claiming one
// ID: the first wins, by folder in the order given, then by name.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "Catalog.h"
#include "core/Catalog.h"
#include "core/Files.h"
#include "supple/script_compiler.hpp"
#include "supple/script_export.hpp"
#include "supple/script_program.hpp"
#include "supple/suite/json.h"

namespace supple::player {

std::vector<std::filesystem::path> libraryFolders() {
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::vector<std::filesystem::path> out;
    const char* list = std::getenv("SUPPLE_SCRIPT_LIBRARY");
    std::string_view rest = list ? list : "";
    while (!rest.empty()) {
        const size_t end = std::min(rest.find(kSeparator), rest.size());
        if (end > 0) out.push_back(files::fromUtf8(rest.substr(0, end)));
        rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    return out;
}

bool readScript(const std::filesystem::path& file, std::string& text) {
    // Some standard libraries accept a directory stream and report an empty EOF.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    // Bound the read itself, not only the result: a mistakenly huge file
    // must not exhaust memory while the catalog or reload tick reads it.
    text.resize(kMaxScriptBytes + 1);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    return in.eof() && !in.bad() && text.size() <= kMaxScriptBytes;
}

std::vector<std::filesystem::path> libraryScripts(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        auto filename = it->path().filename();
        filename += ".cpp";
        const std::filesystem::path script = it->path() / filename;
        if (std::filesystem::is_regular_file(script, ec)) out.push_back(script);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<const ScriptDefinition> compileScript(const std::filesystem::path& file,
                                                      const std::string& source,
                                                      std::string* problems,
                                                      scriptc::Message* firstError) {
    scriptc::Options options;
#ifndef NDEBUG
    // A diagnostic for Debug builds only: MIR's interpreter allocates on the
    // audio thread (each function's first call, each new call into C), so a
    // shipped (release) player never honours it (ADR-0032).
    options.interpret = std::getenv("SUPPLE_SCRIPT_INTERPRET") != nullptr;
#endif
    scriptc::Result result = scriptc::compile(source, options);
    if (!result.program) {
        if (firstError)
            for (const auto& d : result.diagnostics)
                if (d.error) {
                    *firstError = d;
                    break;
                }
        if (problems)
            *problems = scriptc::toText(result.diagnostics, files::toUtf8(file.filename()));
        return nullptr;
    }
    return suite::makeScriptDefinition(std::move(result.program), file, source, problems);
}

namespace {
// Its address names the module this code is in.
const char kAnchor = 0;
// What dsp.export looks for in a module's file (script_export.hpp).
#if defined(__GNUC__) || defined(__clang__)
[[gnu::used]]
#endif
const char kPlayerMark[] = SUPPLE_SCRIPT_PLAYER_MARK;
}  // namespace

std::filesystem::path ownModule() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&kAnchor), &module))
        return {};
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) return {};
    path.resize(n);
    return std::filesystem::path(path);
#else
    Dl_info info{};
    if (dladdr(&kAnchor, &info) == 0 || info.dli_fname == nullptr) return {};
    return files::fromUtf8(info.dli_fname);
#endif
}

std::optional<EmbeddedScript> embeddedScript(const std::filesystem::path& module) {
#if defined(__APPLE__)
    const auto resource =
        module.parent_path().parent_path() / "Resources" / scriptc::kExportResource;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(resource, ec)) return std::nullopt;
    std::ifstream in(resource, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > scriptc::kMaxExportPayload)
        return std::nullopt;
    std::string payload(static_cast<size_t>(size), '\0');
    in.seekg(0);
    if (!in.read(payload.data(), static_cast<std::streamsize>(payload.size()))) return std::nullopt;
#else
    std::ifstream in(module, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamoff size = in.tellg();
    if (size < static_cast<std::streamoff>(scriptc::kExportFooterBytes)) return std::nullopt;
    char footer[scriptc::kExportFooterBytes];
    in.seekg(size - static_cast<std::streamoff>(sizeof footer));
    if (!in.read(footer, sizeof footer)) return std::nullopt;
    const auto length =
        scriptc::exportPayloadLength({footer, sizeof footer}, static_cast<std::uint64_t>(size));
    if (!length) return std::nullopt;
    std::string payload(static_cast<size_t>(*length), '\0');
    in.seekg(size - static_cast<std::streamoff>(sizeof footer + payload.size()));
    if (!in.read(payload.data(), static_cast<std::streamsize>(payload.size()))) return std::nullopt;
#endif
    suite::json::Value doc;
    suite::json::Error error;
    if (!suite::json::parse(payload, doc, error) || !doc.isObject()) return std::nullopt;
    const suite::json::Value* schema = doc.find("schema");
    const suite::json::Value* name = doc.find("name");
    const suite::json::Value* source = doc.find("source");
    if (!schema || !schema->isString() || schema->asString() != scriptc::kExportSchema || !name ||
        !name->isString() || !source || !source->isString())
        return std::nullopt;
    return EmbeddedScript{name->asString(), source->asString()};
}

namespace {

// An export's one plugin, compiled when a host first asks. Nothing to
// follow: the script is part of the module.
class Embedded final : public Catalog {
  public:
    explicit Embedded(EmbeddedScript script) : script_(std::move(script)) {}

    std::vector<DefinitionPtr> list() override {
        if (!compiled_) {
            compiled_ = true;
            definition_ = compileScript({}, script_.source, nullptr);  // no file: no reloading
        }
        if (!definition_) return {};
        return {definition_};
    }

  private:
    EmbeddedScript script_;
    bool compiled_ = false;
    DefinitionPtr definition_;
};

class Library final : public Catalog {
  public:
    std::vector<DefinitionPtr> list() override {
        std::vector<DefinitionPtr> out;
        std::set<std::string> ids;
        std::vector<std::filesystem::path> scripts;
        for (const auto& folder : libraryFolders())
            for (auto& file : libraryScripts(folder)) scripts.push_back(std::move(file));
        for (const auto& file : scripts) {
            std::string source;
            if (!readScript(file, source)) continue;
            Entry& entry = cache_[file];
            if (!entry.definition || entry.source != source) {
                entry.source = source;
                entry.definition = compileScript(file, source, nullptr);
            }
            if (entry.definition && ids.insert(entry.definition->info().id).second)
                out.push_back(entry.definition);
        }
        return out;
    }

  private:
    // The same Definition while its script says the same: hosts list the
    // catalog often, and compiling is not free.
    struct Entry {
        std::string source;
        DefinitionPtr definition;
    };
    std::map<std::filesystem::path, Entry> cache_;
};

}  // namespace

}  // namespace supple::player

std::unique_ptr<supple::Catalog> product::makeCatalog() {
    using namespace supple::player;
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r"(kPlayerMark));  // kept by the linker too
#endif
    if (auto script = embeddedScript(ownModule())) return std::make_unique<Embedded>(*script);
    return std::make_unique<Library>();
}
