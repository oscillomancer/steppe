// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The module library's commands (ADR-0062, rack.md §7): module.write,
// module.edit, module.check, module.get and module.fork. A library module is
// <moduleDir>/<name>/<name>.cpp; once it compiles, racks use it under its id
// and modules playing it are rebuilt with the new code.
#include <algorithm>

#include "../../dsp-commands/dsp_shared.h"
#include "module_library.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"

namespace supple::engine {

json::Value describeModuleType(const rack::ModuleType& t);
Result<std::shared_ptr<const rack::ModuleType>> moduleTypeOf(EngineApp& a, std::string_view name,
                                                             const char* path);

namespace {

namespace fs = std::filesystem;
using cmd::Command;
using cmd::Context;
using cmd::Kind;
using dsp_commands::detail::Replacement;

json::Value j(std::string_view text) { return json::parse(text).value(); }
EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }

struct WriteArgs {
    std::string name, source;
};
struct EditArgs {
    std::optional<std::string> name, type;
    std::vector<Replacement> edits;
};
struct CheckArgs {
    std::optional<std::string> name, source;
};
struct GetArgs {
    std::optional<std::string> name, type;
};
struct ForkArgs {
    std::string type, id;
    std::optional<std::string> name;
};

template <class A>
void common(Command<A, json::Value>& c, std::string name, Kind kind, std::string summary) {
    c.name = std::move(name);
    c.kind = kind;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = std::move(summary);
    c.tags = {"rack", "modular"};
    c.result = schema::any();
}

// The library's type of that name, if one compiled.
std::shared_ptr<const rack::ModuleType> libraryType(const rack::Catalog& c,
                                                    const std::string& name) {
    for (const auto* e : c.list())
        if (e->type->origin() == rack::Origin::Library && detail::moduleName(*e->type) == name)
            return e->type;
    return nullptr;
}

std::string libraryNames(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (fs::is_regular_file(detail::modulePath(dir, name), ec)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}

Result<std::string> readModule(EngineApp& e, const std::string& name, const char* path) {
    SUPPLE_TRY(detail::validModuleName(name, path));
    const fs::path file = detail::modulePath(e.moduleDir(), name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        const std::string known = libraryNames(e.moduleDir());
        return Error("module.unknown", "no module '" + name + "' in " + e.moduleDir().string(),
                     path,
                     known.empty() ? "write one with module.write" : "the library has: " + known);
    }
    return readTextFile(file, detail::kMaxModuleBytes);
}

}  // namespace

// A library name for a type id: its last part, in library characters.
std::string libraryNameFor(const std::string& id) {
    const auto dot = id.rfind('.');
    std::string out;
    for (char ch : id.substr(dot == std::string::npos ? 0 : dot + 1)) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        out += (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ? ch : '_';
    }
    if (out.empty() || out[0] < 'a' || out[0] > 'z') out = "m" + out;
    return out.substr(0, 64);
}

namespace {

std::size_t modulesPlaying(const model::Session& s, const std::string& id) {
    std::size_t n = 0;
    for (const auto& m : s.modules) n += m.value->type == id;
    return n;
}

// Saves `source` as the library's `name` and, when it compiles, puts its type
// in the catalog racks use (a broken version leaves the last good one there).
}  // namespace

Result<json::Value> saveLibraryModule(EngineApp& e, const std::string& name,
                                      const std::string& source, const char* path) {
    SUPPLE_TRY(detail::validModuleName(name, "/name"));
    if (source.size() > detail::kMaxModuleBytes)
        return Error("module.size",
                     "the module is " + std::to_string(source.size()) + " bytes, more than 1 MiB",
                     path, "split it into smaller modules");
    const fs::path file = detail::modulePath(e.moduleDir(), name);
    detail::CompiledModule compiled = detail::compileModule(source, rack::Origin::Library, file);
    const rack::Catalog& catalog = *e.modules();
    const auto before = libraryType(catalog, name);
    if (compiled.type) {
        const std::string& id = compiled.type->id();
        for (const auto* entry :
             {catalog.find(id, rack::Origin::Host), catalog.find(id, rack::Origin::Stock)})
            if (entry)
                return Error("module.id_taken",
                             "the built-in module " + entry->type->info().name + " has the id " +
                                 id + "; not saved",
                             path,
                             "give ModuleInfo.id your own id (\"com.<you>.<name>\"); module.fork "
                             "copies a built-in module under a new id");
        if (const auto* entry = catalog.find(id, rack::Origin::Library);
            entry && detail::moduleName(*entry->type) != name)
            return Error("module.id_taken",
                         "the library's " + detail::moduleName(*entry->type) +
                             " already has the id " + id + "; not saved",
                         path, "give ModuleInfo.id a value no other library module has");
    }
    SUPPLE_TRY(writeFileAtomic(file, source));
    json::Value out = json::Value::object();
    out["name"] = name;
    out["path"] = file.string();
    out["ok"] = compiled.type != nullptr;
    out["diagnostics"] = detail::diagnosticsJson(compiled.diagnostics);
    if (compiled.type) {
        auto next = std::make_shared<rack::Catalog>(catalog);
        if (before && before->id() != compiled.type->id())
            next->remove(before->id(), rack::Origin::Library);
        if (std::string problems = next->add(compiled.type); !problems.empty())
            return Error("module.description", problems, path, "fix what it names");
        // A project's copy of it gives way: the edit is what plays, and the
        // project's next save copies it.
        next->remove(compiled.type->id(), rack::Origin::Project);
        const std::string id = compiled.type->id();
        out["type"] = describeModuleType(*compiled.type);
        e.setModules(std::move(next));
        out["reloaded"] = static_cast<std::int64_t>(modulesPlaying(e.view(), id));
    } else if (before) {
        out["playing"] = before->id() + ": racks keep the last version that compiled";
    }
    return out;
}

namespace {

using SaveFn = Result<json::Value> (*)(EngineApp&, const std::string&, const std::string&,
                                       const char*);
constexpr SaveFn save = saveLibraryModule;
std::string nameFor(const std::string& id) { return libraryNameFor(id); }

constexpr const char* kWriteResult =
    R"({"name": "wobble", "path": "<any>", "ok": true, "diagnostics": [], "type": "<any>",
        "reloaded": 0})";

}  // namespace

void registerModuleCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- module.write
    [&] {
        Command<WriteArgs, json::Value> c;
        common(c, "module.write", Kind::Action,
               "Write a module to the library and compile it; racks use it at once.");
        c.description =
            "name: its library name (lower case, digits, '_' or '-'). source: the whole module, "
            "a struct deriving from Module (the module guide; module.get shows any type's "
            "source). The file is saved even when it does not compile, so module.edit can fix "
            "it; ok and diagnostics (line, column, hint) say how it went. Once it compiles, "
            "type describes it as module.describe does and module.add takes its id; modules "
            "already playing it are rebuilt with the new code (reloaded counts them), their "
            "knobs kept. A version that does not compile leaves racks on the last that did.";
        c.args = schema::object<WriteArgs>("module_write_args")
                     .field("name", &WriteArgs::name, schema::string().minLength(1).maxLength(64))
                     .field("source", &WriteArgs::source, schema::string().minLength(1))
                     .build();
        c.errors = {{"module.name", "not a library name", "lower case, digits, '_' or '-'"},
                    {"module.size", "more than 1 MiB", "split it up"},
                    {"module.id_taken", "a built-in or another library module has its id",
                     "give ModuleInfo.id your own id"}};
        c.examples = {{"A tremolo module", json::Value(json::Value::object()), j(kWriteResult),
                       "song_with_rack"}};
        c.examples[0].args["name"] = "wobble";
        c.examples[0].args["source"] = std::string(detail::kExampleModule);
        c.run = [](const WriteArgs& a, Context& ctx) -> Result<json::Value> {
            return save(app(ctx), a.name, a.source, "/source");
        };
        registry.add(std::move(c));
    }();
    // ---------------------------------------------------------------- module.edit
    [&] {
        Command<EditArgs, json::Value> c;
        common(c, "module.edit", Kind::Action,
               "Edit a library module in place: replace text, then save and compile it.");
        c.description =
            "name (or type, a library or project module's id). A project type is edited into "
            "the library's entry of the same id, or a free name when the id is absent. "
            "Each edit replaces old, which must be in "
            "the source exactly once, with new (empty deletes it), in order and all or none. "
            "Then as module.write: saved, compiled, and racks playing it rebuilt.";
        auto replacement =
            schema::object<Replacement>("module_edit")
                .field("old", &Replacement::before,
                       schema::string().minLength(1).doc("text the source has in one place"))
                .field("new", &Replacement::after, schema::string().doc("what replaces it"))
                .build();
        c.args = schema::object<EditArgs>("module_edit_args")
                     .field("name", &EditArgs::name, schema::string().minLength(1))
                     .field("type", &EditArgs::type, schema::string().minLength(1))
                     .field("edits", &EditArgs::edits,
                            schema::arrayOf(replacement).minItems(1).maxItems(256))
                     .build();
        c.errors = {{"module.unknown", "no such library module", "module.list"},
                    {"module.edit", "an edit's old text is not in the source, or in several",
                     "copy old exactly from module.get"},
                    {"module.built_in", "built-in modules are not edited", "module.fork"}};
        c.examples = {
            {"Slow the tremolo down",
             j(R"({"name": "wobble", "edits": [{"old": "0.1, 20, 4", "new": "0.1, 20, 2"}]})"),
             j(kWriteResult), "song_with_module"}};
        c.run = [](const EditArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            std::string name;
            std::optional<std::string> project;  // a project's copy: edited into the library
            if (a.name.has_value() == a.type.has_value())
                return Error("command.invalid_args", "give name or type", "/name",
                             "e.g. {\"name\": \"wobble\", \"edits\": [...]}");
            if (a.name) {
                name = *a.name;
            } else {
                auto t = moduleTypeOf(e, *a.type, "/type");
                if (!t) return std::move(t).error();
                name = detail::moduleName(**t);
                if (name.empty())
                    return Error("module.built_in", (*t)->id() + " is built in and is not edited",
                                 "/type",
                                 "module.fork copies it into the library under your own id");
                if ((*t)->origin() == rack::Origin::Project) {
                    project = (*t)->source();
                    if (const auto* entry = e.modules()->find((*t)->id(), rack::Origin::Library)) {
                        name = detail::moduleName(*entry->type);
                    } else {
                        auto free = detail::availableModuleName(e.moduleDir(), name);
                        if (!free) return std::move(free).error();
                        name = std::move(*free);
                    }
                }
            }
            auto text = project ? Result<std::string>(*project) : readModule(e, name, "/name");
            if (!text) return std::move(text).error();
            auto edited = dsp_commands::detail::applyEdits(std::move(*text), a.edits, "module.edit",
                                                           "module.get");
            if (!edited) return std::move(edited).error();
            return save(e, name, *edited, "/edits");
        };
        registry.add(std::move(c));
    }();
    // ---------------------------------------------------------------- module.check
    [&] {
        Command<CheckArgs, json::Value> c;
        common(c, "module.check", Kind::Query,
               "Compile a module without saving it; return its diagnostics and description.");
        c.description =
            "source (a whole module) or name (one in the library). ok is true when it "
            "compiles; type then describes it as module.describe would.";
        c.args = schema::object<CheckArgs>("module_check_args")
                     .field("name", &CheckArgs::name, schema::string().minLength(1))
                     .field("source", &CheckArgs::source, schema::string().minLength(1))
                     .build();
        c.errors = {{"module.unknown", "no such library module", "module.list"}};
        c.examples = {
            {"A mistake, with its hint",
             j(R"({"source": "struct X : Module { void process(int frames) {} };"})"),
             j(R"({"ok": false, "diagnostics": "<any>"})"), "empty"},
            {"A library module", j(R"({"name": "wobble"})"),
             j(R"({"ok": true, "diagnostics": [], "type": "<any>"})"), "song_with_module"}};
        c.run = [](const CheckArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            if (a.name.has_value() == a.source.has_value())
                return Error("command.invalid_args", "give source or name", "/source",
                             "source: the module's text; name: a library module");
            std::string source;
            if (a.name) {
                auto text = readModule(e, *a.name, "/name");
                if (!text) return std::move(text).error();
                source = std::move(*text);
            } else {
                source = *a.source;
            }
            const auto compiled = detail::compileModule(source, rack::Origin::Library);
            json::Value out = json::Value::object();
            out["ok"] = compiled.type != nullptr;
            out["diagnostics"] = detail::diagnosticsJson(compiled.diagnostics);
            if (compiled.type) out["type"] = describeModuleType(*compiled.type);
            return out;
        };
        registry.add(std::move(c));
    }();
    // ---------------------------------------------------------------- module.get
    [&] {
        Command<GetArgs, json::Value> c;
        common(c, "module.get", Kind::Query, "A module type's source: stock or library.");
        c.description =
            "type (an id or short name: every stock module's source is readable, a good start "
            "for your own) or name (a library module's file, also when it does not compile).";
        c.args = schema::object<GetArgs>("module_get_args")
                     .field("type", &GetArgs::type, schema::string().minLength(1))
                     .field("name", &GetArgs::name, schema::string().minLength(1))
                     .build();
        c.errors = {{"rack.unknown_type", "no such type", "module.list"},
                    {"module.unknown", "no such library module", "module.write"},
                    {"module.host", "a host module has no source", "module.describe"}};
        c.examples = {{"The stock VCA", j(R"({"type": "vca"})"),
                       j(R"({"type": "com.suppleaudio.rack.vca", "origin": "stock",
                             "source": "<any>"})"),
                       "empty"}};
        c.run = [](const GetArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            if (a.name.has_value() == a.type.has_value())
                return Error("command.invalid_args", "give type or name", "/type",
                             "e.g. {\"type\": \"vca\"}");
            json::Value out = json::Value::object();
            if (a.name) {
                auto text = readModule(e, *a.name, "/name");
                if (!text) return std::move(text).error();
                out["name"] = *a.name;
                out["path"] = detail::modulePath(e.moduleDir(), *a.name).string();
                if (auto t = libraryType(*e.modules(), *a.name)) out["type"] = t->id();
                out["origin"] = "library";
                out["source"] = std::move(*text);
                return out;
            }
            auto t = moduleTypeOf(e, *a.type, "/type");
            if (!t) return std::move(t).error();
            const rack::ModuleType& type = **t;
            if (type.source().empty())
                return Error("module.host", type.id() + " is part of the host, written in C++",
                             "/type", "module.describe shows its ports and knobs");
            out["type"] = type.id();
            out["origin"] = std::string(rack::originName(type.origin()));
            if (const std::string name = detail::moduleName(type); !name.empty()) {
                out["name"] = name;
                out["path"] = type.path();
            }
            out["source"] = type.source();
            return out;
        };
        registry.add(std::move(c));
    }();
    // ---------------------------------------------------------------- module.fork
    [&] {
        Command<ForkArgs, json::Value> c;
        common(c, "module.fork", Kind::Action,
               "Copy a module type's source into the library under a new id.");
        c.description =
            "type: the one to copy (stock or library). id: the copy's (\"com.<you>.<name>\"). "
            "name: its library name (default: the id's last part). The copy is saved and "
            "compiled as module.write does; then module.edit changes it.";
        c.args = schema::object<ForkArgs>("module_fork_args")
                     .field("type", &ForkArgs::type, schema::string().minLength(1))
                     .field("id", &ForkArgs::id, schema::string().minLength(3).maxLength(128))
                     .field("name", &ForkArgs::name, schema::string().minLength(1).maxLength(64))
                     .build();
        c.errors = {{"rack.unknown_type", "no such type", "module.list"},
                    {"module.host", "a host module has no source", "module.describe"},
                    {"module.exists", "the library has a module of that name", "give name"}};
        c.examples = {{"Your own VCA to change", j(R"({"type": "vca", "id": "com.example.vca2"})"),
                       j(R"({"name": "vca2", "path": "<any>", "ok": true, "diagnostics": [],
                             "type": "<any>", "reloaded": 0})"),
                       "empty"}};
        c.run = [](const ForkArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            auto t = moduleTypeOf(e, a.type, "/type");
            if (!t) return std::move(t).error();
            const rack::ModuleType& type = **t;
            if (type.source().empty())
                return Error("module.host", type.id() + " is part of the host, written in C++",
                             "/type", "fork a stock or library module (module.list)");
            const std::string name = a.name.value_or(nameFor(a.id));
            SUPPLE_TRY(detail::validModuleName(name, "/name"));
            std::error_code ec;
            if (fs::exists(detail::modulePath(e.moduleDir(), name), ec))
                return Error("module.exists", "the library has a module named " + name, "/name",
                             "give the copy another name");
            const std::string from = "\"" + type.id() + "\"", to = "\"" + a.id + "\"";
            std::string source = type.source();
            std::size_t count = 0;
            for (std::size_t at = source.find(from); at != std::string::npos;
                 at = source.find(from, at + to.size()), ++count)
                source.replace(at, from.size(), to);
            if (count == 0)
                return Error("module.fork", "the source of " + type.id() + " does not spell its id",
                             "/type", "module.get it and module.write a copy with your id");
            return save(e, name, source, "/id");
        };
        registry.add(std::move(c));
    }();
}

}  // namespace supple::engine
