// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// FX chain presets: ordered, canonical documents independent of session ids.
// File actions never create history; a load validates first and commits once.
#include <supple/base/paths.h>
#include "file_platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include "command_validation.h"
#include "fx_presets.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

namespace supple::engine {
namespace {
namespace fs = std::filesystem;
using model::Id;
constexpr std::string_view kSchema = "supple.fx-chain/1.0";
constexpr std::size_t kMaxFile = 64 * 1024 * 1024;

struct SaveArgs {
    std::string track, name, chain = "main";
    bool replace = false;
};
struct LoadArgs {
    std::string track;
    std::optional<std::string> name, path;
    std::string chain = "main", mode = "append";
};
struct NoArgs {};
struct CopyArgs {
    std::string fx, track, chain = "main";
    std::optional<std::int64_t> index;
};
struct CopyResult {
    std::string fx;
    std::vector<std::string> chain;
};
struct SaveResult {
    std::string path;
    std::int64_t effects = 0;
};
struct LoadResult {
    std::vector<std::string> effects;
};
struct ChainEntry {
    std::string name, path;
    std::int64_t effects = 0;
};
struct ListResult {
    std::vector<ChainEntry> chains;
};
struct Document {
    std::string schema, name;
    std::vector<json::Value> effects;
};

json::Value j(std::string_view text) { return json::parse(text).value(); }

const schema::Schema<Document>& documentSchema() {
    static const auto value =
        schema::object<Document>("fx_chain")
            .field("schema", &Document::schema,
                   schema::string().pattern("^supple\\.fx-chain/1\\.0$", "supple.fx-chain/1.0"))
            .field("name", &Document::name, schema::string().minLength(1))
            .field("effects", &Document::effects, schema::arrayOf(schema::any()))
            .build();
    return value;
}

Status validName(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 200 ||
        std::any_of(name.begin(), name.end(),
                    [](unsigned char c) { return c < 32 || c == '/' || c == '\\'; }))
        return Error("chain.name", "invalid chain name", "/name",
                     "use 1–200 characters without path separators or control characters");
    return {};
}

Result<fs::path> chainDir(const EngineApp& app) {
    if (!app.options().chainDir.empty()) return app.options().chainDir;
    if (const auto root = paths::config(); !root.empty()) return root / product::kSlug / "chains";
    return Error("chain.directory", "no configuration directory", {},
                 "set AppOptions::chainDir to an explicit writable directory");
}

Result<fs::path> namedPath(const EngineApp& app, const std::string& name) {
    SUPPLE_TRY(validName(name));
    auto dir = chainDir(app);
    if (!dir) return std::move(dir).error();
    return *dir / (name + ".json");
}

Result<Id> trackOf(const model::Session& s, const std::string& name) {
    auto id = Id::parse(name);
    if (!id || id->kind != model::Kind::Track || !s.tracks.find(*id))
        return Error("track.unknown", "no track " + name, "/track",
                     "use a live id from track.list");
    return *id;
}

Result<Document> readDocument(const fs::path& path) {
    auto text = readTextFile(path, kMaxFile);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    auto decoded = documentSchema().decode(*doc);
    if (!decoded) return std::move(decoded).error();
    if (decoded->effects.empty())
        return Error("chain.empty", path.string() + " contains no effects", "/effects",
                     "save a nonempty FX chain");
    SUPPLE_TRY(validName(decoded->name));
    for (std::size_t i = 0; i < decoded->effects.size(); ++i) {
        auto fx = model::schemaOf<model::FxInstance>().decode(decoded->effects[i]);
        const std::string at = "/effects/" + std::to_string(i);
        if (!fx) {
            auto error = std::move(fx).error();
            if (error.code == "fx.oversampling") error.code = "chain.invalid";
            error.path.insert(0, at);
            return error;
        }
        if (fx->patch.has_value() == fx->state.has_value())
            return Error("chain.invalid", "effect needs exactly one patch or state", at + "/state",
                         "provide patch for a semantic plugin or base64 state for a plain plugin; "
                         "omit the other field");
    }
    return decoded;
}

// The model's blob references are content hashes ("sha256:<hex>").
std::string stateHash(std::span<const std::uint8_t> bytes) { return "sha256:" + sha256Hex(bytes); }

}  // namespace

// --- shared with rack presets (fx_presets.h) ----------------------------------------

namespace detail {

Status writePreset(const fs::path& path, std::string_view text, bool replace) {
    if (replace) return writeFileAtomic(path, text);
    // Publishing with link is atomic and refuses an existing destination,
    // including when another process saves the same name concurrently.
    static std::atomic<unsigned> serial{0};
    fs::path temp = path;
    temp += "." + std::to_string(detail::files::processId()) + "." +
            std::to_string(serial.fetch_add(1)) + ".preset-tmp";
    SUPPLE_TRY(writeFileAtomic(temp, text));
    const int result = detail::files::publishNew(temp, path);
    const int error = errno;
    std::error_code ignored;
    fs::remove(temp, ignored);
    if (result == 0) return {};
    return Error(error == EEXIST ? "chain.exists" : "chain.write",
                 "cannot save " + path.string() + ": " + std::strerror(error), "/name",
                 error == EEXIST  ? "choose another name or set replace: true"
                 : error == EBUSY ? "another program has the file open; close it and save again"
                                  : "check directory permissions and free space");
}

// The effects of a chain as preset entries: the semantic patch when the
// plugin has one, else its state as base64 (fx.chain.save, track templates).
Result<std::vector<json::Value>> encodeEffects(EngineApp& app, const model::Session& s,
                                               const std::vector<Id>& chain) {
    std::vector<json::Value> effects;
    for (Id id : chain) {
        const auto& fx = *s.fx.find(id);
        auto info = app.findPlugin(fx.plugin.id);
        if (!info) return std::move(info).error();
        auto value = model::toJson(fx);
        value["plugin"]["name"] = fx.plugin.name;
        value["plugin"]["vendor"] = fx.plugin.vendor;
        value["plugin"]["version"] = fx.plugin.version;
        value["label"] = fx.label;
        value["bypass"] = fx.bypass;
        value["offline"] = fx.offline;
        value["wet"] = fx.wet;
        static constexpr const char* isolation[] = {"inherit", "in_process", "shared_bridge",
                                                    "dedicated"};
        value["isolation"] = isolation[static_cast<std::size_t>(fx.isolation)];
        value["oversampling"] = fx.oversampling;
        if (info->semantic) {
            auto patch = app.mergedPatch(fx, json::Value::object(), false);
            if (!patch) return std::move(patch).error();
            value["patch"] = std::move(*patch);
            value.asObject().erase("state");
        } else {
            std::vector<std::uint8_t> bytes;
            if (fx.state) {
                const auto blob = s.blobs.find(*fx.state);
                if (blob == s.blobs.end())
                    return Error("engine.blob_missing", "missing state for " + id.str(), "/effects",
                                 "capture the plugin state again");
                bytes = blob->second->data;
            } else {
                auto probe = host::Instance::create(*info, app.options().identity);
                if (!probe) return std::move(probe).error();
                for (const auto& [param, v] : fx.params) SUPPLE_TRY((*probe)->setParam(param, v));
                auto state = (*probe)->saveState();
                if (!state) return std::move(state).error();
                bytes = std::move(*state);
            }
            value.asObject().erase("patch");
            value["state"] = base64Encode(bytes);
        }
        effects.push_back(std::move(value));
    }
    return effects;
}

Result<std::vector<PreparedFx>> prepareEffects(EngineApp& app,
                                               const std::vector<json::Value>& entries,
                                               const std::string& base) {
    std::vector<PreparedFx> prepared;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string at = base + "/" + std::to_string(i);
        auto fx = model::schemaOf<model::FxInstance>().decode(entries[i]);
        if (!fx) return std::move(fx).error().at(at);
        if ((fx->oversampling & (fx->oversampling - 1)) != 0)
            return Error("chain.invalid", "oversampling must be 1, 2, 4 or 8", at + "/oversampling",
                         "set oversampling to one of 1, 2, 4 or 8");
        auto info = app.findPlugin(fx->plugin.id);
        if (!info || info->id != fx->plugin.id || info->format != fx->plugin.format)
            return Error("chain.plugin_missing",
                         "plugin " + fx->plugin.name + " (" + fx->plugin.format + ":" +
                             fx->plugin.id + ") is not installed",
                         at + "/plugin", "install this plugin or choose another preset");
        PreparedFx p{*fx, {}};
        if (info->semantic) {
            if (!fx->patch || fx->state)
                return Error("chain.invalid", "a semantic effect needs a patch", at + "/patch",
                             "provide patch containing this semantic plugin's patch document and "
                             "omit state");
            auto patch = app.mergedPatch(*fx, *fx->patch, true);
            if (!patch) return std::move(patch).error().at(at + "/patch");
            p.fx.patch = std::move(*patch);
        } else {
            if (!fx->state || fx->patch)
                return Error(
                    "chain.invalid", "a plain effect needs base64 state", at + "/state",
                    "provide base64 state from a saved instance of this plugin and omit patch");
            auto bytes = base64Decode(*fx->state);
            if (!bytes) return std::move(bytes).error().at(at + "/state");
            auto probe = host::Instance::create(*info, app.options().identity);
            if (!probe) return std::move(probe).error().at(at + "/plugin");
            SUPPLE_TRY((*probe)->loadState(*bytes));
            p.fx.state = stateHash(*bytes);
            p.blob = std::make_shared<const model::Blob>(model::Blob{std::move(*bytes)});
        }
        prepared.push_back(std::move(p));
    }
    return prepared;
}

// A plugin's settings as the running instance has them right now (what the
// user hears: an editor edit the app has not consumed yet is not in the model),
// else as the model has them: `fx` gets the state, `blob` its bytes when it is
// an opaque state.
Status captureLiveState(EngineApp& app, Id id, model::FxInstance& fx,
                        std::shared_ptr<const model::Blob>& blob) {
    if (model::frozenOwner(app.view(), id)) return {};
    host::Instance* live = app.instanceOf(id);
    if (!live || live->failed()) return {};
    if (live->info().semantic) {
        auto patch = live->getPatch(false);
        if (!patch) return std::move(patch).error();
        auto parsed = json::parse(*patch);
        if (!parsed) return std::move(parsed).error();
        fx.patch = std::move(*parsed);
        fx.state.reset();
        return {};
    }
    auto state = live->saveState();
    if (!state) return std::move(state).error();
    fx.state = stateHash(*state);
    fx.patch.reset();
    blob = std::make_shared<const model::Blob>(model::Blob{std::move(*state)});
    return {};
}

Id installEffect(EngineApp& app, PreparedFx& prepared) {
    if (prepared.blob) app.txn().putBlob(*prepared.fx.state, prepared.blob);
    return app.txn().create(std::move(prepared.fx));
}

}  // namespace detail

namespace {

using detail::captureLiveState;
using detail::encodeEffects;
using detail::prepareEffects;
using detail::writePreset;
using Prepared = detail::PreparedFx;

// Adds prepared effects to the open transaction, appended to `chain`; their ids.
std::vector<std::string> installEffects(EngineApp& app, std::vector<Prepared>& prepared,
                                        std::vector<Id>& chain) {
    std::vector<std::string> ids;
    for (auto& p : prepared) {
        const Id id = detail::installEffect(app, p);
        chain.push_back(id);
        ids.push_back(id.str());
    }
    return ids;
}

Result<json::Value> invoke(const char* name, json::Value args, cmd::Context& ctx) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
}
// Removes a saved preset or template file by name (fx.chain.delete,
// track.template.delete, project.template.delete); the file's own directory is never removed.
Status removeNamed(const fs::path& path, const char* code, const std::string& what) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)))
        return Error(code, "no saved " + what + " " + path.stem().string(), "/name",
                     "use a name the list command returns");
    if (!fs::remove(path, ec) || ec)
        return Error(code, "cannot delete " + path.string() + ": " + ec.message(), "/name",
                     "check the directory permissions");
    return {};
}

struct NameArgs {
    std::string name;
};
struct PathResult {
    std::string path;
};

// --- track templates ----------------------------------------------------------------------

constexpr std::string_view kTemplateSchema = "supple.track-template/1.0";

Result<fs::path> templateDir(const EngineApp& app) {
    if (!app.options().templateDir.empty()) return app.options().templateDir;
    if (const auto root = paths::config(); !root.empty())
        return root / product::kSlug / "track-templates";
    return Error("template.directory", "no configuration directory", {},
                 "set AppOptions::templateDir to an explicit writable directory");
}

Result<fs::path> templatePath(const EngineApp& app, const std::string& name) {
    SUPPLE_TRY(validName(name));
    auto dir = templateDir(app);
    if (!dir) return std::move(dir).error();
    return *dir / (name + ".json");
}

struct TemplateDoc {
    std::string schema, name;
    json::Value track = json::Value::object();
    std::vector<json::Value> chain, inputChain;
};

const schema::Schema<TemplateDoc>& templateSchema() {
    static const auto value =
        schema::object<TemplateDoc>("track_template")
            .field("schema", &TemplateDoc::schema,
                   schema::string().pattern("^supple\\.track-template/1\\.0$",
                                            "supple.track-template/1.0"))
            .field("name", &TemplateDoc::name, schema::string().minLength(1))
            .field("track", &TemplateDoc::track, schema::any())
            .field("chain", &TemplateDoc::chain, schema::arrayOf(schema::any()))
            .field("input_chain", &TemplateDoc::inputChain, schema::arrayOf(schema::any()))
            .build();
    return value;
}

struct TemplateSaveArgs {
    std::string track, name;
    bool replace = false;
};
struct TemplateLoadArgs {
    std::optional<std::string> name, path, parent;
};
struct TemplateLoadResult {
    std::string track;
    std::vector<std::string> effects;
};
struct TemplateListResult {
    std::vector<ChainEntry> templates;
};

void registerTemplateCommands(cmd::Registry& registry) {
    {
        cmd::Command<TemplateSaveArgs, SaveResult> c;
        c.name = "track.template.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save a strip (settings and FX chains) as a named track template.";
        c.description =
            "Writes a supple.track-template/1.0 file to the user's track-templates directory: "
            "the strip's type, name, colour, notes, level, pan and monitoring, and its main and "
            "input chains as fx.chain.save stores them (semantic patches or base64 state). Not "
            "routing or items. replace: true overwrites an existing name.";
        c.tags = {"track", "presets"};
        c.args = schema::object<TemplateSaveArgs>("track_template_save_args")
                     .field("track", &TemplateSaveArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("name", &TemplateSaveArgs::name, schema::string().minLength(1))
                     .field("replace", &TemplateSaveArgs::replace, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<SaveResult>("track_template_save_result")
                       .field("path", &SaveResult::path, schema::string())
                       .field("effects", &SaveResult::effects, schema::integer().min(0))
                       .build();
        c.examples = {{"Save the bass strip", j(R"({"track":"tr_1","name":"Bass"})"),
                       j(R"({"path":"<any>","effects":2})"), "song_with_chain"}};
        c.run = [](const TemplateSaveArgs& a, cmd::Context& ctx) -> Result<SaveResult> {
            auto& app = ctx.service<EngineApp>();
            auto path = templatePath(app, a.name);
            if (!path) return std::move(path).error();
            const auto s = app.view();
            auto track = trackOf(s, a.track);
            if (!track) return std::move(track).error();
            const model::Track& strip = *s.tracks.find(*track);
            if (strip.type == model::StripType::Master)
                return Error("track.strip_type", "a master is not a template", "/track",
                             "save a channel or a buss");
            TemplateDoc doc;
            doc.schema = std::string(kTemplateSchema);
            doc.name = a.name;
            doc.track["type"] = strip.type == model::StripType::Channel ? "channel" : "buss";
            doc.track["name"] = strip.name;
            doc.track["channels"] = strip.channels;
            doc.track["color"] = strip.color;
            doc.track["notes"] = strip.notes;
            doc.track["volume_db"] = strip.volumeDb;
            doc.track["pan"] = strip.pan;
            static constexpr const char* kMonitor[] = {"off", "on", "auto"};
            doc.track["monitor"] = kMonitor[static_cast<std::size_t>(strip.record.monitor) % 3];
            auto main = encodeEffects(app, s, strip.chain);
            if (!main) return std::move(main).error();
            auto input = encodeEffects(app, s, strip.inputChain);
            if (!input) return std::move(input).error();
            doc.chain = std::move(*main);
            doc.inputChain = std::move(*input);
            const auto text = json::writeCanonical(templateSchema().encode(doc)) + "\n";
            if (text.size() > kMaxFile)
                return Error("template.size", "template exceeds the 64 MiB file limit", "/track",
                             "save a strip with smaller plugin states");
            SUPPLE_TRY(writePreset(*path, text, a.replace));
            return SaveResult{path->string(),
                              static_cast<std::int64_t>(doc.chain.size() + doc.inputChain.size())};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<TemplateLoadArgs, TemplateLoadResult> c;
        c.name = "track.template.load";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a strip from a track template (settings and FX) in one undo step.";
        c.description =
            "Give exactly one of name (in the user's track-templates directory) or path. parent "
            "defaults to the master. Every plugin and state is checked before the model changes.";
        c.tags = {"track", "presets"};
        c.args = schema::object<TemplateLoadArgs>("track_template_load_args")
                     .field("name", &TemplateLoadArgs::name, schema::string().minLength(1))
                     .field("path", &TemplateLoadArgs::path, schema::string().minLength(1))
                     .field("parent", &TemplateLoadArgs::parent,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .build();
        c.result =
            schema::object<TemplateLoadResult>("track_template_load_result")
                .field("track", &TemplateLoadResult::track, schema::string())
                .field("effects", &TemplateLoadResult::effects, schema::arrayOf(schema::string()))
                .build();
        c.examples = {{"Add the saved bass strip", j(R"({"name":"Bass"})"),
                       j(R"({"track":"<any>","effects":["<any>","<any>"]})"),
                       "song_with_saved_template"}};
        c.run = [](const TemplateLoadArgs& a, cmd::Context& ctx) -> Result<TemplateLoadResult> {
            auto& app = ctx.service<EngineApp>();
            if (a.name.has_value() == a.path.has_value())
                return Error("template.source", "give exactly one of name or path", "/name",
                             "use track.template.list to find a saved name");
            auto path =
                a.name ? templatePath(app, *a.name) : Result<fs::path>(app.resolve(*a.path));
            if (!path) return std::move(path).error();
            auto text = readTextFile(*path);
            if (!text) return std::move(text).error();
            auto parsed = json::parse(*text);
            if (!parsed) return std::move(parsed).error();
            auto doc = templateSchema().decode(*parsed);
            if (!doc) return std::move(doc).error();
            auto main = prepareEffects(app, doc->chain, "/chain");
            if (!main) return std::move(main).error();
            auto input = prepareEffects(app, doc->inputChain, "/input_chain");
            if (!input) return std::move(input).error();
            auto field = [&](const char* key) { return doc->track.find(key); };
            json::Value add = json::Value::object();
            const json::Value* type = field("type");
            add["type"] = type && type->isString() ? type->asString() : std::string("channel");
            if (const json::Value* name = field("name"); name && name->isString())
                add["name"] = name->asString();
            if (const json::Value* channels = field("channels")) add["channels"] = *channels;
            if (a.parent) add["parent"] = *a.parent;
            auto added = invoke("track.add", std::move(add), ctx);
            if (!added) return std::move(added).error();
            const std::string track = (*added)["track"].asString();
            json::Value set = json::Value::object();
            set["track"] = track;
            for (const char* key : {"color", "notes", "volume_db", "pan"})
                if (const json::Value* v = field(key); v && !v->isNull()) set[key] = *v;
            const bool channel = !type || !type->isString() || type->asString() == "channel";
            if (const json::Value* v = field("monitor"); channel && v && v->isString())
                set["monitor"] = *v;
            auto changed = invoke("track.set", std::move(set), ctx);
            if (!changed) return std::move(changed).error();
            const Id id = *model::Id::parse(track);
            const model::Track& strip = *app.view().tracks.find(id);
            std::vector<Id> chain = strip.chain, inputChain = strip.inputChain;
            TemplateLoadResult result;
            result.track = track;
            for (auto& e : installEffects(app, *main, chain)) result.effects.push_back(e);
            for (auto& e : installEffects(app, *input, inputChain)) result.effects.push_back(e);
            app.txn().modify<model::Track>(id, [&](model::Track& t) {
                t.chain = chain;
                t.inputChain = inputChain;
            });
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NoArgs, TemplateListResult> c;
        c.name = "track.template.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List saved track templates by name, path and effect count.";
        c.tags = {"track", "presets"};
        c.args = schema::object<NoArgs>("track_template_list_args").build();
        auto entry = schema::object<ChainEntry>("track_template_entry")
                         .field("name", &ChainEntry::name, schema::string())
                         .field("path", &ChainEntry::path, schema::string())
                         .field("effects", &ChainEntry::effects, schema::integer().min(0))
                         .build();
        c.result = schema::object<TemplateListResult>("track_template_list_result")
                       .field("templates", &TemplateListResult::templates, schema::arrayOf(entry))
                       .build();
        c.examples = {{"None yet", j("{}"), j(R"({"templates":[]})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<TemplateListResult> {
            auto dir = templateDir(ctx.service<EngineApp>());
            if (!dir) return std::move(dir).error();
            TemplateListResult result;
            std::error_code ec;
            if (!fs::exists(*dir, ec)) return result;
            for (const auto& e : fs::directory_iterator(*dir, ec)) {
                if (e.path().extension() != ".json") continue;
                auto text = readTextFile(e.path());
                if (!text) continue;
                auto parsed = json::parse(*text);
                if (!parsed) continue;
                auto doc = templateSchema().decode(*parsed);
                if (!doc) continue;
                result.templates.push_back(
                    {doc->name, e.path().string(),
                     static_cast<std::int64_t>(doc->chain.size() + doc->inputChain.size())});
            }
            std::sort(result.templates.begin(), result.templates.end(),
                      [](const ChainEntry& x, const ChainEntry& y) {
                          return std::tie(x.name, x.path) < std::tie(y.name, y.path);
                      });
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NameArgs, PathResult> c;
        c.name = "track.template.delete";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a saved track template by name.";
        c.tags = {"track", "presets"};
        c.args = schema::object<NameArgs>("track_template_delete_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<PathResult>("track_template_delete_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"template.unknown", "no template has that name",
                     "use a name from track.template.list"}};
        c.examples = {{"Delete the bass strip", j(R"({"name":"Bass"})"), j(R"({"path":"<any>"})"),
                       "song_with_saved_template"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto path = templatePath(ctx.service<EngineApp>(), a.name);
            if (!path) return std::move(path).error();
            SUPPLE_TRY(removeNamed(*path, "template.unknown", "track template"));
            return PathResult{path->string()};
        };
        addChecked(registry, std::move(c));
    }
}

// --- project templates ----------------------------------------------------------------------

Result<fs::path> projectTemplateDir(const EngineApp& app) {
    if (!app.options().projectTemplateDir.empty()) return app.options().projectTemplateDir;
    if (const auto root = paths::config(); !root.empty())
        return root / product::kSlug / "templates";
    return Error("template.directory", "no configuration directory", {},
                 "set AppOptions::projectTemplateDir to an explicit writable directory");
}

// A template is a folder, <dir>/<name>/project.json with media/; an older one
// is a single file, <dir>/<name>.json, with absolute media paths.
struct TemplateLocation {
    fs::path folder, legacy;
    bool folderExists() const {
        std::error_code ec;
        return fs::is_regular_file(folder / "project.json", ec);
    }
    bool legacyExists() const {
        std::error_code ec;
        return fs::is_regular_file(legacy, ec);
    }
};

Result<TemplateLocation> projectTemplateAt(const EngineApp& app, const std::string& name) {
    SUPPLE_TRY(validName(name));
    if (name.front() == '.')
        return Error("chain.name", "a template name cannot start with a dot", "/name",
                     "use a name that starts with a letter or digit");
    auto dir = projectTemplateDir(app);
    if (!dir) return std::move(dir).error();
    return TemplateLocation{*dir / name, *dir / (name + ".json")};
}

// Writes the template into a staging folder, then puts it in place (a
// replaced template is removed only once the new one is there).
Status publishProjectTemplate(EngineApp& app, const TemplateLocation& at, bool replace,
                              std::vector<schema::Warning>* warnings) {
    if ((at.folderExists() || at.legacyExists()) && !replace)
        return Error("template.exists",
                     "a project template named " + at.folder.filename().string() + " exists",
                     "/name", "choose another name or set replace: true");
    static std::atomic<unsigned> serial{0};
    const std::string tag =
        std::to_string(detail::files::processId()) + "-" + std::to_string(serial.fetch_add(1));
    const fs::path staging = at.folder.parent_path() / ".staging";
    const fs::path fresh = staging / ("new-" + tag), old = staging / ("old-" + tag);
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (auto written = app.writeTemplate(fresh, warnings); !written) {
        fs::remove_all(fresh, ec);
        return written;
    }
    const bool moved = fs::exists(at.folder, ec);
    if (moved) fs::rename(at.folder, old, ec);
    if (!ec) fs::rename(fresh, at.folder, ec);
    if (ec) {
        const std::string why = ec.message();
        std::error_code ignored;
        if (moved && !fs::exists(at.folder, ignored)) fs::rename(old, at.folder, ignored);
        fs::remove_all(fresh, ignored);
        return Error("template.write", "cannot save " + at.folder.string() + ": " + why, "/name",
                     "check the templates folder's permissions and free space");
    }
    fs::remove_all(old, ec);
    fs::remove(at.legacy, ec);
    fs::remove(staging, ec);  // only when empty
    return {};
}

struct ProjectTemplateSaveArgs {
    std::string name;
    bool replace = false;
};
struct TemplateSaveResult {
    std::string path;
    std::optional<std::vector<schema::Warning>> warnings;  // plug-in states not read
};
struct ProjectTemplateLoadResult {
    std::string name;
};
struct ProjectTemplateEntry {
    std::string name, path;
};
struct ProjectTemplateListResult {
    std::vector<ProjectTemplateEntry> templates;
};

void registerProjectTemplateCommands(cmd::Registry& registry) {
    {
        cmd::Command<ProjectTemplateSaveArgs, TemplateSaveResult> c;
        c.name = "project.template.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save the whole project as a named project template.";
        c.description =
            "Writes the project as it is now (tracks, FX and their states, routing, items, "
            "settings; not the history) to a folder of its own in the user's templates "
            "directory, with a copy of every audio file it uses. The project keeps its own file. "
            "replace: true replaces a template of that name. warnings: running plug-ins whose "
            "current state could not be read; the template has the state each had before.";
        c.tags = {"project", "presets"};
        c.args = schema::object<ProjectTemplateSaveArgs>("project_template_save_args")
                     .field("name", &ProjectTemplateSaveArgs::name, schema::string().minLength(1))
                     .field("replace", &ProjectTemplateSaveArgs::replace, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<TemplateSaveResult>("project_template_save_result")
                       .field("path", &TemplateSaveResult::path, schema::string())
                       .field("warnings", &TemplateSaveResult::warnings,
                              schema::arrayOf(
                                  schema::object<schema::Warning>("save_warning")
                                      .field("code", &schema::Warning::code, schema::string())
                                      .field("message", &schema::Warning::message, schema::string())
                                      .field("path", &schema::Warning::path, schema::string())
                                      .build()))
                       .build();
        c.errors = {{"template.exists", "the name already exists",
                     "choose another name or set replace: true"},
                    {"media.missing", "an audio file the project uses is gone",
                     "restore it, or remove the items that use it"}};
        c.examples = {
            {"Save the band setup", j(R"({"name":"Band"})"), j(R"({"path":"<any>"})"), "song"}};
        c.run = [](const ProjectTemplateSaveArgs& a,
                   cmd::Context& ctx) -> Result<TemplateSaveResult> {
            auto& app = ctx.service<EngineApp>();
            auto at = projectTemplateAt(app, a.name);
            if (!at) return std::move(at).error();
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(publishProjectTemplate(app, *at, a.replace, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            TemplateSaveResult result{at->folder.string(), {}};
            if (!warnings.empty()) result.warnings = std::move(warnings);
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NameArgs, ProjectTemplateLoadResult> c;
        c.name = "project.template.load";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start a new, unsaved project from a project template.";
        c.description =
            "Replaces the current project and its history, like project.new: unsaved changes are "
            "lost, so save first. The new project has no file until project.save {path} and "
            "gets its own copy of the template's audio; the template itself is never changed.";
        c.tags = {"project", "presets"};
        c.args = schema::object<NameArgs>("project_template_load_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<ProjectTemplateLoadResult>("project_template_load_result")
                       .field("name", &ProjectTemplateLoadResult::name, schema::string())
                       .build();
        c.examples = {{"Start from the band setup", j(R"({"name":"Band"})"),
                       j(R"({"name":"<any>"})"), "song_with_project_template"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<ProjectTemplateLoadResult> {
            auto& app = ctx.service<EngineApp>();
            auto at = projectTemplateAt(app, a.name);
            if (!at) return std::move(at).error();
            if (!at->folderExists() && !at->legacyExists())
                return Error("template.unknown", "no project template " + a.name, "/name",
                             "use project.template.list to find a saved name");
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(app.openTemplate(at->folderExists() ? at->folder : at->legacy, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            return ProjectTemplateLoadResult{app.head().project->name};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NoArgs, ProjectTemplateListResult> c;
        c.name = "project.template.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List saved project templates by name and path.";
        c.tags = {"project", "presets"};
        c.args = schema::object<NoArgs>("project_template_list_args").build();
        auto entry = schema::object<ProjectTemplateEntry>("project_template_entry")
                         .field("name", &ProjectTemplateEntry::name, schema::string())
                         .field("path", &ProjectTemplateEntry::path, schema::string())
                         .build();
        c.result =
            schema::object<ProjectTemplateListResult>("project_template_list_result")
                .field("templates", &ProjectTemplateListResult::templates, schema::arrayOf(entry))
                .build();
        c.examples = {{"None yet", j("{}"), j(R"({"templates":[]})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<ProjectTemplateListResult> {
            auto dir = projectTemplateDir(ctx.service<EngineApp>());
            if (!dir) return std::move(dir).error();
            ProjectTemplateListResult result;
            std::error_code ec;
            if (!fs::exists(*dir, ec)) return result;
            std::set<std::string> folders;
            for (const auto& e : fs::directory_iterator(*dir, ec)) {
                const std::string name = e.path().filename().string();
                std::error_code kind;
                if (name.front() == '.') continue;  // .staging
                if (e.is_directory(kind) && fs::is_regular_file(e.path() / "project.json", kind)) {
                    folders.insert(name);
                    result.templates.push_back({name, e.path().string()});
                }
            }
            for (const auto& e : fs::directory_iterator(*dir, ec)) {  // older single files
                std::error_code kind;
                const std::string stem = e.path().stem().string();
                if (e.is_regular_file(kind) && e.path().extension() == ".json" &&
                    stem.front() != '.' && !folders.contains(stem))
                    result.templates.push_back({stem, e.path().string()});
            }
            std::sort(result.templates.begin(), result.templates.end(),
                      [](const auto& x, const auto& y) { return x.name < y.name; });
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NameArgs, PathResult> c;
        c.name = "project.template.delete";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a saved project template by name.";
        c.tags = {"project", "presets"};
        c.args = schema::object<NameArgs>("project_template_delete_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<PathResult>("project_template_delete_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"template.unknown", "no template has that name",
                     "use a name from project.template.list"}};
        c.examples = {{"Delete the band setup", j(R"({"name":"Band"})"), j(R"({"path":"<any>"})"),
                       "song_with_project_template"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto at = projectTemplateAt(ctx.service<EngineApp>(), a.name);
            if (!at) return std::move(at).error();
            if (!at->folderExists()) {
                SUPPLE_TRY(removeNamed(at->legacy, "template.unknown", "project template"));
                return PathResult{at->legacy.string()};
            }
            std::error_code ec;
            fs::remove_all(at->folder, ec);
            if (ec)
                return Error("template.unknown",
                             "cannot delete " + at->folder.string() + ": " + ec.message(), "/name",
                             "check the templates folder's permissions");
            fs::remove(at->legacy, ec);
            return PathResult{at->folder.string()};
        };
        addChecked(registry, std::move(c));
    }
}

// --- plugin presets -----------------------------------------------------------------------
// One plugin's settings by name: its factory patches (read-only, from the
// plugin) and the user's own, a supple.preset/1.0 file each (the sandbox's
// format, sandbox.md) in <dir>/<plugin id>/<name>.json.

struct PresetDocument {
    std::optional<std::string> schema;
    std::optional<std::int64_t> version;
    std::string plugin, name;
    std::optional<std::string> pluginName, notes;
    std::optional<std::vector<std::string>> tags;
    std::optional<json::Value> patch;
    std::optional<std::string> state;
};

const schema::Schema<PresetDocument>& presetSchema() {
    static const auto value =
        schema::object<PresetDocument>("preset")
            .field("schema", &PresetDocument::schema,
                   schema::string().pattern("^supple\\.preset/1\\.0$", "supple.preset/1.0"))
            .field("version", &PresetDocument::version, schema::integer().min(1).max(1))
            .field("plugin", &PresetDocument::plugin, schema::string().minLength(1))
            .field("plugin_name", &PresetDocument::pluginName, schema::string())
            .field("name", &PresetDocument::name, schema::string().minLength(1))
            .field("tags", &PresetDocument::tags, schema::arrayOf(schema::string()))
            .field("notes", &PresetDocument::notes, schema::string())
            .field("patch", &PresetDocument::patch, schema::any())
            .field("state", &PresetDocument::state, schema::string())
            .build();
    return value;
}

// A preset name: a chain name that is also a file name on every platform.
Status validPresetName(const std::string& name) {
    if (validName(name) && name.find_first_of(":*?\"<>|") == std::string::npos &&
        name.front() != ' ' && name.front() != '.' && name.back() != ' ' && name.back() != '.')
        return {};
    return Error("fx.preset_name", "invalid preset name", "/name",
                 "use 1–200 characters, without / \\ : * ? \" < > | or control characters, "
                 "not starting or ending with a space or a dot");
}

Result<fs::path> presetRoot(const EngineApp& app) {
    fs::path root = app.options().presetDir;
    if (root.empty()) {
        root = paths::config();
        if (root.empty())
            return Error("fx.preset_directory", "no configuration directory", {},
                         "set AppOptions::presetDir to an explicit writable directory");
        root = root / product::kSlug / "presets";
    }
    return root;
}

// Where the plugin's presets are saved: a folder no other plugin ID shares.
Result<fs::path> presetDir(const EngineApp& app, const model::PluginRef& plugin) {
    auto root = presetRoot(app);
    if (!root) return root;
    return *root / paths::idFolder(plugin.id);
}

const model::FxInstance* presetFx(const model::Session& s, const std::string& fx, Id& id) {
    auto parsed = Id::parse(fx);
    if (!parsed || parsed->kind != model::Kind::Fx) return nullptr;
    id = *parsed;
    return s.fx.find(id);
}

Error unknownFx(const std::string& fx) {
    return Error("fx.unknown", "no plugin instance " + fx, "/fx",
                 "use an fx id from fx.add, fx.copy or session.describe");
}

// The factory patches' names in the plugin's order; none for a plain plugin.
Result<std::vector<std::string>> factoryNames(EngineApp& app, Id id, const model::FxInstance& fx) {
    auto patches = app.fxFactoryPatches(id, fx);
    if (!patches) return std::move(patches).error();
    std::vector<std::string> names;
    if (patches->isArray())
        for (const auto& p : patches->asArray())
            if (const auto* name = p.find("name"); name && name->isString())
                names.push_back(name->asString());
    return names;
}

// The user's presets for fx's plugin, sorted by name, those in its folder
// before those of the same name in the folder earlier versions used (which
// other plugins' IDs could share); documents for another plugin or that do
// not parse are skipped with a warning.
struct UserPreset {
    std::string name;
    fs::path path;
    PresetDocument doc;
    bool legacy = false;
};
Status readPresets(const fs::path& dir, const model::FxInstance& fx, bool legacy, cmd::Context* ctx,
                   std::vector<UserPreset>& rows) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return {};
    fs::directory_iterator it(dir, ec), end;
    while (!ec && it != end) {
        const auto entry = *it;
        it.increment(ec);
        std::error_code kindError;
        if (!entry.is_regular_file(kindError) || entry.path().extension() != ".json") continue;
        auto text = readTextFile(entry.path(), kMaxFile);
        auto parsed = text ? json::parse(*text) : Result<json::Value>(text.error());
        auto doc = parsed ? presetSchema().decode(*parsed) : Result<PresetDocument>(parsed.error());
        if (doc) {
            if (doc->plugin != fx.plugin.id) continue;
            if (doc->patch.has_value() == doc->state.has_value())
                doc = Error("fx.preset_invalid", "a preset needs exactly one patch or state",
                            "/state", "save the preset again");
        }
        if (!doc) {
            if (ctx)
                ctx->warn("fx.preset_invalid", entry.path().string() + ": " + doc.error().message);
            continue;
        }
        rows.push_back({doc->name, entry.path(), std::move(*doc), legacy});
    }
    if (ec)
        return Error("fx.preset_directory", "cannot list " + dir.string() + ": " + ec.message(), {},
                     "check the directory permissions");
    return {};
}
Result<std::vector<UserPreset>> userPresets(const EngineApp& app, const model::FxInstance& fx,
                                            cmd::Context* ctx) {
    auto root = presetRoot(app);
    if (!root) return std::move(root).error();
    std::vector<UserPreset> rows;
    SUPPLE_TRY(readPresets(*root / paths::idFolder(fx.plugin.id), fx, false, ctx, rows));
    SUPPLE_TRY(readPresets(*root / paths::legacyIdFolder(fx.plugin.id), fx, true, ctx, rows));
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return std::tie(a.name, a.legacy, a.path) < std::tie(b.name, b.legacy, b.path);
    });
    return rows;
}

struct PresetArgs {
    std::string fx, name;
    std::optional<std::string> source;
};
struct PresetDeleteArgs {
    std::string fx, name;
};
struct PresetSaveArgs {
    std::string fx, name;
    bool replace = false;
};
struct PresetEntry {
    std::string name, source;
    std::optional<std::string> path;
};
struct PresetListArgs {
    std::string fx;
};
struct PresetListResult {
    std::vector<PresetEntry> presets;
};
struct PresetLoadResult {
    std::string name, source;
};

void registerPresetCommands(cmd::Registry& registry) {
    {
        cmd::Command<PresetListArgs, PresetListResult> c;
        c.name = "fx.preset.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List a plugin's presets: its factory ones, then the user's.";
        c.description =
            "source factory: the plugin's own patches in its order (read-only; the same as "
            "fx.patch.list's names). source user: the user's saved presets for this plugin, "
            "sorted by name, with their file path. See fx.preset.load and fx.preset.save.";
        c.tags = {"fx", "presets"};
        c.args =
            schema::object<PresetListArgs>("fx_preset_list_args")
                .field("fx", &PresetListArgs::fx, schema::string().pattern("^fx_[0-9]+$", "fx id"))
                .build();
        const auto entrySchema = schema::object<PresetEntry>("fx_preset_entry")
                                     .field("name", &PresetEntry::name, schema::string())
                                     .field("source", &PresetEntry::source, schema::string())
                                     .field("path", &PresetEntry::path, schema::string())
                                     .build();
        c.result = schema::object<PresetListResult>("fx_preset_list_result")
                       .field("presets", &PresetListResult::presets, schema::arrayOf(entrySchema))
                       .build();
        c.errors = {{"fx.unknown", "no such plugin instance", "use an fx id from fx.add"}};
        c.examples = {
            {"List presets", j(R"({"fx":"fx_2"})"), j(R"({"presets":[]})"), "song_with_fx"}};
        c.run = [](const PresetListArgs& a, cmd::Context& ctx) -> Result<PresetListResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            Id id;
            const auto* fx = presetFx(s, a.fx, id);
            if (!fx) return unknownFx(a.fx);
            auto factory = factoryNames(app, id, *fx);
            if (!factory) return std::move(factory).error();
            auto user = userPresets(app, *fx, &ctx);
            if (!user) return std::move(user).error();
            PresetListResult result;
            for (auto& name : *factory) result.presets.push_back({std::move(name), "factory", {}});
            for (auto& p : *user)
                if (result.presets.empty() || result.presets.back().source != "user" ||
                    result.presets.back().name != p.name)
                    result.presets.push_back({p.name, "user", p.path.string()});
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<PresetArgs, PresetLoadResult> c;
        c.name = "fx.preset.load";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Apply a factory or user preset to a plugin instance in one undo step.";
        c.description =
            "name from fx.preset.list. source (factory or user) is needed only when both have "
            "the name. A factory preset or a semantic plugin's user preset replaces the whole "
            "patch (omitted parameters take defaults); a plain plugin's user preset is its saved "
            "state. Options such as wet and bypass are not part of a preset.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<PresetArgs>("fx_preset_load_args")
                     .field("fx", &PresetArgs::fx, schema::string().pattern("^fx_[0-9]+$", "fx id"))
                     .field("name", &PresetArgs::name, schema::string().minLength(1))
                     .field("source", &PresetArgs::source,
                            schema::string().pattern("^(factory|user)$", "factory or user"))
                     .build();
        c.result = schema::object<PresetLoadResult>("fx_preset_load_result")
                       .field("name", &PresetLoadResult::name, schema::string())
                       .field("source", &PresetLoadResult::source, schema::string())
                       .build();
        c.errors = {
            {"fx.unknown_preset", "no preset has that name", "choose a name from fx.preset.list"},
            {"fx.preset_ambiguous", "a factory and a user preset share the name",
             "add source: factory or source: user"},
            {"fx.preset_invalid", "the preset file is not valid for this plugin",
             "save the preset again"}};
        c.examples = {{"Load a saved preset", j(R"({"fx":"fx_2","name":"Quiet"})"),
                       j(R"({"name":"Quiet","source":"user"})"), "song_with_preset"}};
        c.run = [](const PresetArgs& a, cmd::Context& ctx) -> Result<PresetLoadResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            Id id;
            const auto* fx = presetFx(s, a.fx, id);
            if (!fx) return unknownFx(a.fx);
            const bool wantFactory = !a.source || *a.source == "factory";
            const bool wantUser = !a.source || *a.source == "user";
            std::vector<std::string> factory;
            if (wantFactory) {
                auto names = factoryNames(app, id, *fx);
                if (!names) return std::move(names).error();
                factory = std::move(*names);
            }
            std::vector<UserPreset> user;
            if (wantUser) {
                auto rows = userPresets(app, *fx, &ctx);
                if (!rows) return std::move(rows).error();
                user = std::move(*rows);
            }
            const bool inFactory =
                std::find(factory.begin(), factory.end(), a.name) != factory.end();
            const auto mine = std::find_if(user.begin(), user.end(),
                                           [&](const UserPreset& p) { return p.name == a.name; });
            const bool inUser = mine != user.end();
            if (inFactory && inUser)
                return Error("fx.preset_ambiguous",
                             "both a factory and a user preset are named '" + a.name + "'",
                             "/source", "add source: factory or source: user");
            if (!inFactory && !inUser)
                return Error("fx.unknown_preset", "no preset named '" + a.name + "'", "/name",
                             "choose a name from fx.preset.list");
            if (inFactory) {
                auto args = json::Value::object();
                args["fx"] = a.fx;
                args["name"] = a.name;
                auto set = invoke("fx.patch.set", std::move(args), ctx);
                if (!set) return std::move(set).error();
                return PresetLoadResult{a.name, "factory"};
            }
            const PresetDocument& doc = mine->doc;
            if (doc.patch) {
                auto patch = app.mergedPatch(*fx, *doc.patch, true);
                if (!patch)
                    return Error("fx.preset_invalid",
                                 mine->path.string() + ": " + patch.error().message, "/name",
                                 "the plugin refused this preset; save it again or delete it");
                app.txn().modify<model::FxInstance>(
                    id, [&](model::FxInstance& f) { f.patch = *patch; });
                return PresetLoadResult{a.name, "user"};
            }
            auto info = app.findPlugin(fx->plugin.id);
            if (!info) return std::move(info).error();
            if (info->semantic)
                return Error("fx.preset_invalid",
                             mine->path.string() + " holds a plain state for a semantic plugin",
                             "/name", "save the preset again");
            auto bytes = base64Decode(*doc.state);
            if (!bytes)
                return Error("fx.preset_invalid", mine->path.string() + ": the state is not base64",
                             "/name", "save the preset again");
            // The plugin checks the state on a probe before the model changes.
            auto probe = host::Instance::create(*info, app.options().identity);
            if (!probe) return std::move(probe).error();
            if (auto loaded = (*probe)->loadState(*bytes); !loaded)
                return Error("fx.preset_invalid",
                             mine->path.string() + ": " + loaded.error().message, "/name",
                             "the plugin refused this preset; save it again or delete it");
            const std::string hash = stateHash(*bytes);
            app.txn().putBlob(hash,
                              std::make_shared<const model::Blob>(model::Blob{std::move(*bytes)}));
            app.txn().modify<model::FxInstance>(id, [&](model::FxInstance& f) {
                f.state = hash;
                f.params.clear();  // the state holds every value
            });
            return PresetLoadResult{a.name, "user"};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<PresetSaveArgs, PathResult> c;
        c.name = "fx.preset.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save a plugin instance's current settings as a named user preset.";
        c.description =
            "Saves what the running plugin has now (editor edits included): the patch of a "
            "semantic plugin, else its state. The preset is the user's, for every instance of "
            "this plugin in any project. A factory preset's name cannot be taken; replace: true "
            "overwrites a user preset of that name.";
        c.tags = {"fx", "presets"};
        c.args =
            schema::object<PresetSaveArgs>("fx_preset_save_args")
                .field("fx", &PresetSaveArgs::fx, schema::string().pattern("^fx_[0-9]+$", "fx id"))
                .field("name", &PresetSaveArgs::name, schema::string().minLength(1))
                .field("replace", &PresetSaveArgs::replace, schema::boolean())
                .defaultValue(false)
                .build();
        c.result = schema::object<PathResult>("fx_preset_save_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"fx.preset_name", "invalid preset name", "use a name without path separators"},
                    {"fx.preset_factory", "a factory preset has the name",
                     "choose another name; factory presets are read-only"},
                    {"fx.preset_exists", "a user preset has the name",
                     "choose another name or set replace: true"}};
        c.examples = {{"Save the gain's settings", j(R"({"fx":"fx_2","name":"Quiet"})"),
                       j(R"({"path":"<any>"})"), "song_with_fx"}};
        c.run = [](const PresetSaveArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto& app = ctx.service<EngineApp>();
            SUPPLE_TRY(validPresetName(a.name));
            const auto s = app.view();
            Id id;
            const auto* fx = presetFx(s, a.fx, id);
            if (!fx) return unknownFx(a.fx);
            auto factory = factoryNames(app, id, *fx);
            if (!factory) return std::move(factory).error();
            if (std::find(factory->begin(), factory->end(), a.name) != factory->end())
                return Error("fx.preset_factory",
                             "'" + a.name + "' is one of the plugin's own presets", "/name",
                             "choose another name; factory presets are read-only");
            PresetDocument doc;
            doc.schema = "supple.preset/1.0";
            doc.plugin = fx->plugin.id;
            doc.pluginName = fx->plugin.name;
            doc.name = a.name;
            doc.tags = std::vector<std::string>{};
            doc.notes = "";
            // The running plugin's settings when there is one, else the model's.
            model::FxInstance now = *fx;
            now.patch.reset();
            now.state.reset();
            std::shared_ptr<const model::Blob> blob;
            SUPPLE_TRY(captureLiveState(app, id, now, blob));
            if (blob) {
                doc.state = base64Encode(blob->data);
            } else if (now.patch) {
                doc.patch = *now.patch;
            } else {
                auto effects = encodeEffects(app, s, {id});
                if (!effects) return std::move(effects).error();
                const json::Value& entry = effects->front();
                if (const auto* patch = entry.find("patch"))
                    doc.patch = *patch;
                else
                    doc.state = entry.find("state")->asString();
            }
            auto user = userPresets(app, *fx, &ctx);
            if (!user) return std::move(user).error();
            const auto taken = [&](const UserPreset& p) { return p.name == a.name; };
            if (!a.replace && std::any_of(user->begin(), user->end(), taken))
                return Error("fx.preset_exists", "a user preset named '" + a.name + "' exists",
                             "/name", "choose another name or set replace: true");
            auto dir = presetDir(app, fx->plugin);
            if (!dir) return std::move(dir).error();
            std::error_code ec;
            fs::create_directories(*dir, ec);
            if (ec)
                return Error("fx.preset_directory",
                             "cannot create " + dir->string() + ": " + ec.message(), {},
                             "check the configuration directory's permissions");
            const fs::path path = *dir / (a.name + ".json");
            const auto text = json::writeCanonical(presetSchema().encode(doc)) + "\n";
            if (text.size() > kMaxFile)
                return Error("fx.preset_size", "the preset exceeds the 64 MiB file limit", "/fx",
                             "this plugin's state is too large to keep as a preset");
            if (auto written = writePreset(path, text, a.replace); !written) {
                Error e = std::move(written).error();
                if (e.code == "chain.exists")
                    return Error("fx.preset_exists", "a user preset named '" + a.name + "' exists",
                                 "/name", "choose another name or set replace: true");
                return e;
            }
            // A copy of the same name in the older folder would come back.
            for (const auto& p : *user)
                if (p.legacy && taken(p)) fs::remove(p.path, ec);
            return PathResult{path.string()};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<PresetDeleteArgs, PathResult> c;
        c.name = "fx.preset.delete";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete one of the user's presets for a plugin; factory presets stay.";
        c.description =
            "Removes the user preset's file. Factory presets belong to the plugin and cannot be "
            "deleted. Instances that loaded the preset keep their settings.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<PresetDeleteArgs>("fx_preset_delete_args")
                     .field("fx", &PresetDeleteArgs::fx,
                            schema::string().pattern("^fx_[0-9]+$", "fx id"))
                     .field("name", &PresetDeleteArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<PathResult>("fx_preset_delete_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"fx.unknown_preset", "no user preset has that name",
                     "choose a user preset from fx.preset.list"},
                    {"fx.preset_factory", "factory presets cannot be deleted",
                     "delete only presets whose source is user"}};
        c.examples = {{"Delete a saved preset", j(R"({"fx":"fx_2","name":"Quiet"})"),
                       j(R"({"path":"<any>"})"), "song_with_preset"}};
        c.run = [](const PresetDeleteArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            Id id;
            const auto* fx = presetFx(s, a.fx, id);
            if (!fx) return unknownFx(a.fx);
            auto user = userPresets(app, *fx, &ctx);
            if (!user) return std::move(user).error();
            // Every copy: one in the older folder would come back.
            std::optional<std::string> deleted;
            for (const auto& p : *user)
                if (p.name == a.name) {
                    std::error_code ec;
                    if (!fs::remove(p.path, ec) || ec)
                        return Error("fx.preset_delete",
                                     "cannot delete " + p.path.string() + ": " + ec.message(),
                                     "/name", "check the presets folder's permissions");
                    if (!deleted) deleted = p.path.string();
                }
            if (deleted) return PathResult{*deleted};
            auto factory = factoryNames(app, id, *fx);
            if (factory && std::find(factory->begin(), factory->end(), a.name) != factory->end())
                return Error("fx.preset_factory",
                             "'" + a.name + "' is one of the plugin's own presets", "/name",
                             "factory presets cannot be deleted; delete only user presets");
            return Error("fx.unknown_preset", "no user preset named '" + a.name + "'", "/name",
                         "choose a user preset from fx.preset.list");
        };
        addChecked(registry, std::move(c));
    }
}

}  // namespace

struct DuplicateArgs {
    std::vector<std::string> tracks;
};
struct DuplicateResult {
    std::vector<std::string> tracks;
};

void registerChainCommands(cmd::Registry& registry) {
    registerPresetCommands(registry);
    {
        cmd::Command<SaveArgs, SaveResult> c;
        c.name = "fx.chain.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save an ordered FX chain as a named preset.";
        c.description =
            "Save main (default) or input FX to the user's chains directory. Preserves semantic "
            "patches or opaque base64 state. Refuses empty chains; replace: true explicitly "
            "overwrites an existing name.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<SaveArgs>("fx_chain_save_args")
                     .field("track", &SaveArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("name", &SaveArgs::name, schema::string().minLength(1))
                     .field("chain", &SaveArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .defaultValue(std::string("main"))
                     .field("replace", &SaveArgs::replace, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<SaveResult>("fx_chain_save_result")
                       .field("path", &SaveResult::path, schema::string())
                       .field("effects", &SaveResult::effects, schema::integer().min(0))
                       .build();
        c.errors = {{"chain.name", "invalid preset name", "use a name without path separators"},
                    {"chain.empty", "the chain has no effects", "add effects before saving"},
                    {"chain.exists", "the name already exists",
                     "choose another name or set replace: true"}};
        c.examples = {{"Save the bass chain", j(R"({"track":"tr_1","name":"Bass"})"),
                       j(R"({"path":"<any>","effects":2})"), "song_with_chain"}};
        c.run = [](const SaveArgs& a, cmd::Context& ctx) -> Result<SaveResult> {
            auto& app = ctx.service<EngineApp>();
            auto path = namedPath(app, a.name);
            if (!path) return std::move(path).error();
            const auto s = app.view();
            auto track = trackOf(s, a.track);
            if (!track) return std::move(track).error();
            const auto& strip = *s.tracks.find(*track);
            const auto& chain = a.chain == "input" ? strip.inputChain : strip.chain;
            if (chain.empty())
                return Error("chain.empty", "the " + a.chain + " chain has no effects", "/chain",
                             "add effects before saving");
            Document doc{std::string(kSchema), a.name, {}};
            auto effects = encodeEffects(app, s, chain);
            if (!effects) return std::move(effects).error();
            doc.effects = std::move(*effects);
            const auto text = json::writeCanonical(documentSchema().encode(doc)) + "\n";
            if (text.size() > kMaxFile)
                return Error("chain.size", "preset exceeds the 64 MiB file limit", "/effects",
                             "save fewer effects or smaller plugin states");
            SUPPLE_TRY(writePreset(*path, text, a.replace));
            return SaveResult{path->string(), static_cast<std::int64_t>(chain.size())};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<LoadArgs, LoadResult> c;
        c.name = "fx.chain.load";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Append or replace a track's FX chain from a preset in one undo step.";
        c.description =
            "Give exactly one of name (in the user's chains directory) or path (relative to the "
            "work directory). mode defaults to append; replace removes the target chain, its "
            "envelopes and key routes. Validates every plugin and state before changing the model.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<LoadArgs>("fx_chain_load_args")
                     .field("track", &LoadArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("name", &LoadArgs::name, schema::string().minLength(1))
                     .field("path", &LoadArgs::path, schema::string().minLength(1))
                     .field("chain", &LoadArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .defaultValue(std::string("main"))
                     .field("mode", &LoadArgs::mode,
                            schema::string().pattern("^(append|replace)$", "append or replace"))
                     .defaultValue(std::string("append"))
                     .build();
        c.result = schema::object<LoadResult>("fx_chain_load_result")
                       .field("effects", &LoadResult::effects, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"chain.source", "name/path missing or both given", "give exactly one of name or path"},
            {"chain.plugin_missing", "a saved plugin is not installed",
             "install the named plugin or choose another preset"}};
        c.examples = {{"Load a saved bass chain",
                       j(R"({"track":"tr_1","name":"Bass","mode":"replace"})"),
                       j(R"({"effects":["<any>","<any>"]})"), "song_with_saved_chain"}};
        c.run = [](const LoadArgs& a, cmd::Context& ctx) -> Result<LoadResult> {
            auto& app = ctx.service<EngineApp>();
            if (a.name.has_value() == a.path.has_value())
                return Error("chain.source", "give exactly one of name or path", "/name",
                             "use fx.chain.list to find a saved name");
            auto path = a.name ? namedPath(app, *a.name) : Result<fs::path>(app.resolve(*a.path));
            if (!path) return std::move(path).error();
            auto doc = readDocument(*path);
            if (!doc) return std::move(doc).error();
            const auto s = app.view();
            auto target = trackOf(s, a.track);
            if (!target) return std::move(target).error();
            auto ready = prepareEffects(app, doc->effects, "/effects");
            if (!ready) return std::move(ready).error();
            std::vector<Prepared>& prepared = *ready;
            const auto* strip = s.tracks.find(*target);
            auto chain = a.chain == "input" ? strip->inputChain : strip->chain;
            if (a.mode == "replace") {
                for (Id id : chain) {
                    auto args = json::Value::object();
                    args["fx"] = id.str();
                    auto removed = invoke("fx.remove", std::move(args), ctx);
                    if (!removed) return std::move(removed).error();
                }
                chain.clear();
            }
            LoadResult result;
            result.effects = installEffects(app, prepared, chain);
            app.txn().modify<model::Track>(*target, [&](model::Track& track) {
                (a.chain == "input" ? track.inputChain : track.chain) = chain;
            });
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NoArgs, ListResult> c;
        c.name = "fx.chain.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List saved FX chain presets by name, path and effect count.";
        c.description =
            "Lists valid supple.fx-chain/1.0 files, sorted by name and path. Invalid documents are "
            "skipped with warnings. See fx.chain.save and fx.chain.load.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<NoArgs>("fx_chain_list_args").build();
        const auto entrySchema =
            schema::object<ChainEntry>("fx_chain_entry")
                .field("name", &ChainEntry::name, schema::string())
                .field("path", &ChainEntry::path, schema::string())
                .field("effects", &ChainEntry::effects, schema::integer().min(0))
                .build();
        c.result = schema::object<ListResult>("fx_chain_list_result")
                       .field("chains", &ListResult::chains, schema::arrayOf(entrySchema))
                       .build();
        c.examples = {{"List presets", j("{}"), j(R"({"chains":[]})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<ListResult> {
            auto dir = chainDir(ctx.service<EngineApp>());
            if (!dir) return std::move(dir).error();
            ListResult result;
            std::error_code ec;
            if (fs::exists(*dir, ec)) {
                std::vector<ChainEntry> rows;
                fs::directory_iterator it(*dir, ec), end;
                while (!ec && it != end) {
                    const auto entry = *it;
                    it.increment(ec);
                    std::error_code kindError;
                    if (!entry.is_regular_file(kindError) || entry.path().extension() != ".json")
                        continue;
                    auto doc = readDocument(entry.path());
                    if (!doc) {
                        ctx.warn("chain.invalid",
                                 entry.path().string() + ": " + doc.error().message);
                        continue;
                    }
                    rows.push_back({doc->name, entry.path().string(),
                                    static_cast<std::int64_t>(doc->effects.size())});
                }
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                    return std::tie(a.name, a.path) < std::tie(b.name, b.path);
                });
                result.chains = std::move(rows);
            }
            if (ec)
                return Error("chain.directory",
                             "cannot list " + dir->string() + ": " + ec.message(), {},
                             "check the directory permissions");
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<NameArgs, PathResult> c;
        c.name = "fx.chain.delete";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a saved FX chain preset by name.";
        c.description =
            "Removes the preset file from the user's chains directory. Tracks that loaded it keep "
            "their effects.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<NameArgs>("fx_chain_delete_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<PathResult>("fx_chain_delete_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"chain.unknown", "no preset has that name", "use a name from fx.chain.list"}};
        c.examples = {{"Delete the bass chain", j(R"({"name":"Bass"})"), j(R"({"path":"<any>"})"),
                       "song_with_saved_chain"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto path = namedPath(ctx.service<EngineApp>(), a.name);
            if (!path) return std::move(path).error();
            SUPPLE_TRY(removeNamed(*path, "chain.unknown", "chain preset"));
            return PathResult{path->string()};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<CopyArgs, CopyResult> c;
        c.name = "fx.copy";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Copy a plugin and its current settings into a track's FX chain.";
        c.description =
            "Adds a new instance of fx's plugin to track's main (default) or input chain, at index "
            "(default: the end), with the source's options (label, bypass, offline, wet, "
            "isolation, oversampling, parallel, pdc) and its settings as they are now: the "
            "running plugin's own state when the engine has one (editor edits included), else the "
            "model's patch or state. Not copied: envelopes, key routes into the source, and, "
            "when the track differs, channel pins. The source stays where it is; "
            "use fx.move to move it. One undo step.";
        c.tags = {"fx"};
        c.args = schema::object<CopyArgs>("fx_copy_args")
                     .field("fx", &CopyArgs::fx, schema::string().pattern("^fx_[0-9]+$", "fx id"))
                     .field("track", &CopyArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("index", &CopyArgs::index, schema::integer().min(0))
                     .field("chain", &CopyArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .defaultValue(std::string("main"))
                     .build();
        c.result = schema::object<CopyResult>("fx_copy_result")
                       .field("fx", &CopyResult::fx, schema::string())
                       .field("chain", &CopyResult::chain, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"fx.unknown", "no such plugin instance",
             "use an fx id returned by fx.add, fx.copy or fx.chain.load"},
            {"fx.index", "index is past the end of the chain", "use 0 to the chain's size"}};
        c.examples = {{"Duplicate a plugin at the end of its own chain",
                       j(R"({"fx":"fx_1","track":"tr_1"})"),
                       j(R"({"fx":"<any>","chain":["fx_1","fx_2","<any>"]})"), "song_with_fx"}};
        c.run = [](const CopyArgs& a, cmd::Context& ctx) -> Result<CopyResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            auto source = Id::parse(a.fx);
            const model::FxInstance* original =
                source && source->kind == model::Kind::Fx ? s.fx.find(*source) : nullptr;
            if (!original)
                return Error("fx.unknown", "no plugin instance " + a.fx, "/fx",
                             "use an fx id returned by fx.add, fx.copy or fx.chain.load");
            auto target = trackOf(s, a.track);
            if (!target) return std::move(target).error();
            const model::Track* strip = s.tracks.find(*target);
            const bool input = a.chain == "input";
            std::vector<Id> chain = input ? strip->inputChain : strip->chain;
            if (a.index && static_cast<std::size_t>(*a.index) > chain.size())
                return Error("fx.index", "index is past the end of the destination chain", "/index",
                             "use an index from 0 to " + std::to_string(chain.size()));
            Prepared copy{*original, {}};
            // The pins name channels of the source's own track.
            const auto home = model::trackOfFx(s, *source);
            if (!home || *home != *target) copy.fx.pins = {};
            SUPPLE_TRY(captureLiveState(app, *source, copy.fx, copy.blob));
            if (copy.blob) app.txn().putBlob(*copy.fx.state, copy.blob);
            const Id id = app.txn().create(std::move(copy.fx));
            chain.insert(
                chain.begin() + static_cast<std::ptrdiff_t>(
                                    a.index ? *a.index : static_cast<std::int64_t>(chain.size())),
                id);
            app.txn().modify<model::Track>(*target, [&](model::Track& track) {
                (input ? track.inputChain : track.chain) = chain;
            });
            CopyResult result{id.str(), {}};
            for (Id f : chain) result.chain.push_back(f.str());
            return result;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<DuplicateArgs, DuplicateResult> c;
        c.name = "track.duplicate";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Duplicate channels or busses with their FX, items, sends and automation.";
        c.description =
            "Each copy goes right after its original, under the same parent, with the same "
            "settings and name; its main and input chains (each plugin's settings as it sounds "
            "now, as fx.copy), items and takes (unpooled MIDI copied, audio media shared), sends "
            "and record input, and the envelopes on the track, its plugins, sends and takes. Not "
            "copied: a folder's children, sends into the track, and item groups. Masters cannot "
            "be duplicated. One undo step; the copies' ids in the order given.";
        c.tags = {"track"};
        c.args = schema::object<DuplicateArgs>("track_duplicate_args")
                     .field("tracks", &DuplicateArgs::tracks,
                            schema::arrayOf(schema::string().pattern("^tr_[0-9]+$", "track id"))
                                .minItems(1))
                     .build();
        c.result = schema::object<DuplicateResult>("track_duplicate_result")
                       .field("tracks", &DuplicateResult::tracks, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"track.unknown", "no such track", "use a live id from track.list"},
            {"track.strip_type", "a master cannot be duplicated", "duplicate channels and busses"}};
        c.examples = {{"Duplicate a track with its plugins, item and automation",
                       j(R"({"tracks":["tr_1"]})"), j(R"({"tracks":["tr_2"]})"), "song_with_env"}};
        c.run = [](const DuplicateArgs& a, cmd::Context& ctx) -> Result<DuplicateResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            std::vector<Id> sources;
            for (std::size_t i = 0; i < a.tracks.size(); ++i) {
                auto id = trackOf(s, a.tracks[i]);
                if (!id) return std::move(id).error().at("/tracks/" + std::to_string(i));
                if (s.tracks.find(*id)->type == model::StripType::Master)
                    return Error("track.strip_type", "a master cannot be duplicated",
                                 "/tracks/" + std::to_string(i), "duplicate channels and busses");
                if (std::find(sources.begin(), sources.end(), *id) == sources.end())
                    sources.push_back(*id);
            }
            auto& txn = app.txn();
            DuplicateResult result;
            for (const Id source : sources) {
                const model::Track& original = *s.tracks.find(source);
                std::map<Id, Id> ids;  // the original's objects to their copies
                model::Track copy = original;
                copy.children.clear();
                auto chain = [&](const std::vector<Id>& from) -> Result<std::vector<Id>> {
                    std::vector<Id> out;
                    for (const Id fx : from) {
                        const model::FxInstance* f = s.fx.find(fx);
                        if (!f) continue;
                        Prepared p{*f, {}};
                        SUPPLE_TRY(captureLiveState(app, fx, p.fx, p.blob));
                        if (p.blob) txn.putBlob(*p.fx.state, p.blob);
                        ids[fx] = txn.create(std::move(p.fx));
                        out.push_back(ids[fx]);
                    }
                    return out;
                };
                auto main = chain(original.chain);
                if (!main) return std::move(main).error();
                auto input = chain(original.inputChain);
                if (!input) return std::move(input).error();
                copy.chain = std::move(*main);
                copy.inputChain = std::move(*input);
                copy.record.input.reset();
                copy.record.midiInput.reset();
                const Id fresh = txn.create(copy);
                ids[source] = fresh;
                txn.modify<model::Track>(*original.parent, [&](model::Track& parent) {
                    const auto at =
                        std::find(parent.children.begin(), parent.children.end(), source);
                    parent.children.insert(at == parent.children.end() ? at : at + 1, fresh);
                });
                // Its sends and outputs, and the input it records from; a send
                // into one of its own plugins goes to the copy's.
                for (const auto& r : s.routes) {
                    const bool out = r.value->from.track == source;
                    const bool recorded = original.record.input == r.id;
                    const bool midiInput = original.record.midiInput == r.id;
                    if (!out && !recorded && !midiInput) continue;
                    model::Route route = *r.value;
                    if (out) route.from.track = fresh;
                    if (route.to.track == source) route.to.track = fresh;
                    if (route.to.fx && ids.contains(*route.to.fx)) route.to.fx = ids[*route.to.fx];
                    ids[r.id] = txn.create(std::move(route));
                    if (recorded)
                        txn.modify<model::Track>(
                            fresh, [&](model::Track& t) { t.record.input = ids[r.id]; });
                    if (midiInput)
                        txn.modify<model::Track>(
                            fresh, [&](model::Track& t) { t.record.midiInput = ids[r.id]; });
                }
                for (const auto& e : s.items) {
                    if (e.value->track != source) continue;
                    model::Item item = *e.value;
                    item.track = fresh;
                    item.group.reset();  // a group would tie the copy to the original
                    item.takes.clear();
                    for (const Id take : e.value->takes) {
                        const model::Take* tk = s.takes.find(take);
                        if (!tk) continue;
                        model::Take t = *tk;
                        unpoolCopiedSource(t, s, txn);
                        auto cloned = copyTake(s, txn, take, std::move(t));
                        if (!cloned) return std::move(cloned).error();
                        item.takes.push_back(*cloned);
                        if (e.value->activeTake == take) item.activeTake = *cloned;
                    }
                    txn.create(std::move(item));
                }
                for (const auto& e : s.envelopes) {
                    const auto to = ids.find(e.value->target.object);
                    if (to == ids.end()) continue;
                    model::Envelope env = *e.value;
                    env.target.object = to->second;
                    const Id lane = txn.create(std::move(env));
                    for (const auto& ai : s.autoItems)
                        if (ai.value->envelope == e.id) {
                            model::AutomationItem item = *ai.value;
                            item.envelope = lane;
                            txn.create(std::move(item));
                        }
                }
                result.tracks.push_back(fresh.str());
            }
            return result;
        };
        addChecked(registry, std::move(c));
    }
    registerTemplateCommands(registry);
    registerProjectTemplateCommands(registry);
}
}  // namespace supple::engine
