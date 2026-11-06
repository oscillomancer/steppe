// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "module_library.h"

#include <algorithm>
#include <set>

#include "supple/base/files.h"
#include "supple/script_compiler.hpp"
#include "supple/script_program.hpp"

namespace supple::engine::detail {

namespace fs = std::filesystem;

CompiledModule compileModule(std::string_view source, rack::Origin origin, const fs::path& path) {
    CompiledModule out;
    scriptc::Result r = scriptc::compile(source);
    for (const scriptc::Message& m : r.diagnostics)
        out.diagnostics.push_back(
            {m.error ? "error" : "warning", m.code, m.message, m.hint, m.line, m.column});
    if (!r.program) return out;
    if (r.program->kind() != scriptc::PluginKind::Module) {
        out.diagnostics.push_back(
            {"error", "rack.not_a_module", "the script is a plug-in, not a module", "", 0, 0});
        out.diagnostics.back().hint =
            "derive the struct from Module, with a ModuleInfo, Inputs, Outputs and "
            "process(int frames) (module.get com.suppleaudio.rack.vca shows one)";
        return out;
    }
    auto code = scriptc::moduleCode(std::move(r.program));
    if (!code) {
        out.diagnostics.push_back(
            {"error", "rack.module", "the module could not be built", "", 0, 0});
        return out;
    }
    auto type =
        rack::ModuleType::fromCode(std::move(code), origin, std::string(source), path.string());
    if (const std::string problems = type->problems(); !problems.empty()) {
        out.diagnostics.push_back({"error", "rack.description", problems,
                                   "fix the ModuleInfo, the keys or the counts it names", 0, 0});
        return out;
    }
    out.type = std::move(type);
    return out;
}

fs::path modulePath(const fs::path& dir, std::string_view name) {
    return dir / std::string(name) / (std::string(name) + ".cpp");
}

std::string moduleName(const rack::ModuleType& type) {
    if ((type.origin() != rack::Origin::Library && type.origin() != rack::Origin::Project) ||
        type.path().empty())
        return {};
    return fs::path(type.path()).stem().string();
}

Status validModuleName(std::string_view name, const std::string& path) {
    const bool ok =
        !name.empty() && name.size() <= 64 && name[0] >= 'a' && name[0] <= 'z' &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    if (!ok)
        return Error("module.name", "'" + std::string(name) + "' is not a library name", path,
                     "use 1-64 lower-case letters, digits, '_' or '-', starting with a letter: "
                     "\"wavefolder\"");
    return {};
}

namespace {

// `base` with `dir`'s modules as `origin` in place of its types of that origin.
std::shared_ptr<const rack::Catalog> loadFolder(const std::shared_ptr<const rack::Catalog>& base,
                                                const fs::path& dir, rack::Origin origin,
                                                std::vector<std::string>* skipped) {
    auto catalog = std::make_shared<rack::Catalog>(*base);
    catalog->clear(origin);
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (validModuleName(name, {}) && fs::is_regular_file(modulePath(dir, name), ec))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    auto skip = [&](const std::string& name, const std::string& why) {
        if (skipped) skipped->push_back(name + ": " + why);
    };
    for (const std::string& name : names) {
        const fs::path file = modulePath(dir, name);
        auto text = readTextFile(file, kMaxModuleBytes);
        if (!text) {
            skip(name, text.error().message);
            continue;
        }
        CompiledModule compiled = compileModule(*text, origin, file);
        if (!compiled.type) {
            std::string why = "it does not compile";
            for (const auto& d : compiled.diagnostics)
                if (d.severity == "error") {
                    why += ": " + d.message;
                    break;
                }
            skip(name, why);
            continue;
        }
        const auto* taken = catalog->find(compiled.type->id());
        if (taken && (taken->type->origin() == origin ||
                      taken->type->origin() == rack::Origin::Host ||
                      taken->type->origin() == rack::Origin::Stock)) {
            skip(name, "its id " + compiled.type->id() + " is taken by " +
                           (taken->type->origin() == origin ? "the folder's " +
                                                                  moduleName(*taken->type)
                                                            : std::string("a built-in module")));
            continue;
        }
        if (std::string problems = catalog->add(compiled.type); !problems.empty())
            skip(name, problems);
    }
    return catalog;
}

}  // namespace

std::shared_ptr<const rack::Catalog> loadModuleLibrary(
    const std::shared_ptr<const rack::Catalog>& base, const fs::path& dir,
    std::vector<std::string>* skipped) {
    return loadFolder(base, dir, rack::Origin::Library, skipped);
}

std::shared_ptr<const rack::Catalog> loadProjectModules(
    const std::shared_ptr<const rack::Catalog>& base, const fs::path& dir,
    std::vector<std::string>* skipped) {
    return loadFolder(base, dir, rack::Origin::Project, skipped);
}

Status saveProjectModules(const rack::Catalog& catalog, const std::vector<std::string>& types,
                          const fs::path& dir) {
    std::set<std::string> written;
    for (const std::string& id : types) {
        const auto* entry = catalog.find(id);
        if (!entry) continue;
        const rack::ModuleType& t = *entry->type;
        if (t.origin() != rack::Origin::Library && t.origin() != rack::Origin::Project) continue;
        const std::string name = moduleName(t);
        if (name.empty() || t.source().empty() || !written.insert(name).second) continue;
        const fs::path file = modulePath(dir, name);
        if (auto old = readTextFile(file, kMaxModuleBytes); old && *old == t.source()) continue;
        SUPPLE_TRY(writeFileAtomic(file, t.source()));
    }
    return {};
}

json::Value diagnosticsJson(const std::vector<ModuleDiagnostic>& diagnostics) {
    json::Value out = json::Value::array();
    for (const ModuleDiagnostic& d : diagnostics) {
        json::Value x = json::Value::object();
        x["severity"] = d.severity;
        x["code"] = d.code;
        x["message"] = d.message;
        if (!d.hint.empty()) x["hint"] = d.hint;
        if (d.line > 0) {
            x["line"] = d.line;
            x["column"] = d.column;
        }
        out.push_back(std::move(x));
    }
    return out;
}

}  // namespace supple::engine::detail
