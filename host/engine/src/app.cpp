// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/app.h"
#include "media_files.h"
#include "module_library.h"
#include "rack_runtime.h"
#include "runtime_protection.h"
#include "source_preparation.h"
#include "supple/model/freeze.h"
#include "supple/model/numeric.h"

#if !defined(_WIN32)
#include <poll.h>
#endif
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <set>
#include <string_view>
#include <thread>

#include "supple/audio/audio_file.h"
#include "supple/audio/wav_stream.h"
#include "supple/base/closest.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/clap/semantic_params.h"
#include "supple/engine/jack.h"
#include "supple/model/env_shape.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

#include "../../dsp-commands/script_library.h"
#include "meters.h"
#include "recording.h"

namespace supple::engine {

namespace fs = std::filesystem;
using model::Id;

namespace {
bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}
std::string originText(const cmd::Origin& o) {
    static constexpr const char* kTypes[] = {"ui",   "lua",      "mcp",  "rpc",
                                             "test", "internal", "agent"};
    std::string s = kTypes[static_cast<int>(o.type)];
    if (!o.client.empty()) s += ":" + o.client;
    if (!o.agent.empty()) s += "/" + o.agent;
    return s;
}
}  // namespace

EngineApp::EngineApp(AppOptions options) : options_(std::move(options)) {
    // The scripting runtime exists from the start (its commands are attached
    // later, when the dispatcher does); a script that only computes runs either
    // way, and the registry's examples exercise exactly that.
    scripts_ = std::make_unique<lua::Runtime>();
    protections_ = std::make_unique<detail::RuntimeProtection>(options_.runtimeProtectionOps);
}

EngineApp::~EngineApp() {
    stopEngine();
    stopJobs();
    scratch_.clear();
}

Result<std::unique_ptr<EngineApp>> EngineApp::create(AppOptions options, model::Project project) {
    if (options.workdir.empty())
        return Error("engine.workdir", "no work directory given", {}, "pass a writable directory");
    std::error_code ec;
    fs::create_directories(options.workdir / "renders", ec);
    if (ec)
        return Error(
            "engine.workdir", "cannot create " + options.workdir.string(), {},
            "pass a writable workdir and ensure its parent directory exists and has space");
    auto app = std::unique_ptr<EngineApp>(new EngineApp(std::move(options)));
    dsp_commands::nameScriptLibrary(app->scriptDir());
    if (!app->serviceWaitable().valid())
        return Error("engine.service_wake", "cannot create the main-thread service descriptor", {},
                     "close unused files/descriptors and retry creating the engine app");
    EngineApp* self = app.get();
    app->pool_ = std::make_unique<InstancePool>(
        [self](const model::PluginRef& ref) { return self->findPlugin(ref.id); },
        app->options_.identity);
    app->pool_->setMainThreadWake(app->mainWake_);
    app->modules_ = detail::loadModuleLibrary(detail::builtinModuleCatalog(), app->moduleDir());
    app->pool_->racks().setCatalog(app->modules_);
    SUPPLE_TRY(app->newProject(std::move(project)));
    return app;
}

const model::Session& EngineApp::head() const { return *versions_->head(); }

model::Session EngineApp::view() const { return txn_ ? txn_->preview() : head(); }

model::EditTxn& EngineApp::txn() {
    assert(txn_ && "edit command outside a transaction");
    return *txn_;
}

// --- plugins -------------------------------------------------------------------------

Result<std::span<const host::PluginInfo>> EngineApp::plugins(bool rescan) {
    if (!catalogue_ || rescan) {
        const auto path =
            options_.pluginPath.empty() ? host::defaultSearchPath() : options_.pluginPath;
        auto list = host::scan(path, host::ScanOptions{}, &scanReport_);
        if (!list) return std::move(list).error();
        host::orderCatalogue(*list);  // the first location on the path wins an id
        catalogue_ = std::move(*list);
    }
    return std::span<const host::PluginInfo>(*catalogue_);
}

void EngineApp::installPlugins(std::vector<host::PluginInfo> list, host::ScanReport report) {
    host::orderCatalogue(list);  // the first location on the path wins an id
    catalogue_ = std::move(list);
    scanReport_ = std::move(report);
}

Result<host::PluginInfo> EngineApp::findPlugin(std::string_view idOrName) {
    auto all = plugins();
    if (!all) return std::move(all).error();
    for (const auto& p : *all)
        if (p.id == idOrName) return p;
    for (const auto& p : *all)
        if (iequals(p.id, idOrName) || iequals(p.name, idOrName)) return p;
    std::vector<std::string_view> ids;
    for (const auto& p : *all) ids.push_back(p.id);
    const std::string best = closestMatch(idOrName, ids);
    return Error(
        "plugin.unknown", "no plugin \"" + std::string(idOrName) + "\"", {},
        (best.empty() ? std::string() : "did you mean \"" + best + "\"? ") + "see plugin.list");
}

Result<host::Instance*> EngineApp::scratch(const model::FxInstance& fx) {
    const auto s = view();
    for (const auto& e : s.fx)
        if (e.value.get() == &fx)
            if (const auto owner = model::frozenOwner(s, e.id))
                return model::frozenError(s, *owner, "/fx");
    auto& slot = scratch_[fx.plugin.id];
    if (!slot) {
        auto info = findPlugin(fx.plugin.id);
        if (!info) {
            scratch_.erase(fx.plugin.id);
            return std::move(info).error();
        }
        auto made = host::Instance::create(*info, options_.identity);
        if (!made) {
            scratch_.erase(fx.plugin.id);
            return std::move(made).error();
        }
        slot = std::move(*made);
        slot->setMainThreadWake(mainWake_);
    }
    return slot.get();
}

Result<json::Value> EngineApp::fxSchema(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    if (!(*in)->info().semantic) return json::Value();
    return (*in)->semanticSchema();
}

Result<json::Value> EngineApp::fxFactoryPatches(model::Id id, const model::FxInstance& fx) {
    if (auto* live = instanceOf(id)) return live->factoryPatches();
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->factoryPatches();
}

Result<std::vector<host::ParamInfo>> EngineApp::fxParams(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->params();
}

Result<std::uint32_t> EngineApp::fxSidechainChannels(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->sidechainChannels();
}

Result<std::vector<host::AudioPortInfo>> EngineApp::fxAuxOutputs(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->auxOutputPorts();
}

namespace {
Status restoreFx(host::Instance& instance, const model::FxInstance& fx, const model::Session& s) {
    if (fx.patch)
        SUPPLE_TRY(instance.setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
    else if (fx.state) {
        const auto blob = s.blobs.find(*fx.state);
        if (blob == s.blobs.end())
            return Error("fx.state", "the state blob is missing", "/fx/state",
                         "load a project or preset containing this state blob, or capture the FX "
                         "state again");
        SUPPLE_TRY(instance.loadState(blob->second->data));
    }
    for (const auto& [id, value] : fx.params) SUPPLE_TRY(instance.setParam(id, value));
    return {};
}
}  // namespace

Result<json::Value> EngineApp::setPlainParams(model::Id id, const json::Value& values) {
    if (const auto owner = model::frozenOwner(view(), id))
        return model::frozenError(view(), *owner, "/fx");
    if (!values.isObject())
        return Error("fx.params", "values must be an object", "/values",
                     "map parameter names or ids to numbers or plugin text");
    const auto s = view();
    const auto* fx = s.fx.find(id);
    if (!fx)
        return Error("fx.unknown", "the plugin instance is missing", "/fx",
                     "use an fx id returned by fx.add, fx.copy or fx.chain.load");
    auto info = findPlugin(fx->plugin.id);
    if (!info) return info.error();
    auto made = host::Instance::create(*info, options_.identity);
    if (!made) return made.error();
    auto& instance = **made;
    instance.setMainThreadWake(mainWake_);
    SUPPLE_TRY(restoreFx(instance, *fx, s));
    const auto params = instance.params();
    std::map<std::uint32_t, double> pending;
    json::Value changed = json::Value::object();
    for (const auto& entry : values.asObject()) {
        std::uint32_t numeric = 0;
        const auto parsed =
            std::from_chars(entry.key.data(), entry.key.data() + entry.key.size(), numeric);
        const bool byId =
            parsed.ec == std::errc{} && parsed.ptr == entry.key.data() + entry.key.size();
        const host::ParamInfo* found = nullptr;
        for (const auto& param : params)
            if ((byId && param.id == numeric) || (!byId && iequals(param.name, entry.key))) {
                if (found)
                    return Error("fx.param_ambiguous", "parameter name is ambiguous",
                                 "/values/" + entry.key,
                                 "use the numeric id from fx.describe full");
                found = &param;
            }
        const auto path = "/values/" + entry.key;
        if (!found)
            return Error("fx.param_unknown", "unknown parameter " + entry.key, path,
                         "use a parameter name or id from fx.describe full");
        if (found->flags & CLAP_PARAM_IS_READONLY)
            return Error("fx.param_readonly", "parameter is read-only", path,
                         "choose a writable parameter");
        double value = 0;
        if (entry.value.isNumber())
            value = entry.value.asDouble();
        else if (entry.value.isString()) {
            auto parsedText = instance.paramFromText(found->id, entry.value.asString());
            if (!parsedText) return std::move(parsedText).error().at(path);
            value = *parsedText;
        } else
            return Error("fx.param_value", "value must be a number or plugin text", path,
                         "provide a number or parameter text such as \"-6 dB\", using the unit in "
                         "fx.describe");
        if (!std::isfinite(value) || value < found->min || value > found->max)
            return Error("fx.param_range", "parameter value is outside its declared range", path,
                         "use " + std::to_string(found->min) + ".." + std::to_string(found->max));
        if (found->flags & CLAP_PARAM_IS_STEPPED) value = std::round(value);
        if (pending.contains(found->id))
            return Error("fx.param_duplicate", "parameter appears twice", path,
                         "use one name or id per parameter");
        pending[found->id] = value;
        changed[entry.key] = value;
    }
    for (const auto& [param, value] : pending) SUPPLE_TRY(instance.setParam(param, value));
    // VST3 controllers enqueue processor changes: consume them on this private
    // instance before saving the component state. No device is opened.
    SUPPLE_TRY(instance.activate(s.project->sampleRate, 1, 1));
    if (!instance.startProcessing())
        return Error("fx.process", "plugin refused the state probe", {},
                     "use fx.restart for the affected FX, or replace it with a plugin that accepts "
                     "the saved state and sample rate");
    float left = 0, right = 0;
    const float* inputs[]{&left, &right};
    float* outputs[]{&left, &right};
    const bool processed = instance.process({.inputs = inputs,
                                             .inputChannels = 2,
                                             .outputs = outputs,
                                             .outputChannels = 2,
                                             .frames = 1});
    instance.stopProcessing();
    instance.deactivate();
    if (!processed)
        return Error("fx.process", "plugin failed the state probe", {},
                     "use fx.restart for the affected FX, or replace it with a plugin that accepts "
                     "the saved state and sample rate");
    auto state = instance.saveState();
    if (!state) return state.error();
    const auto hash = "sha256:" + sha256Hex(*state);
    txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{std::move(*state)}));
    auto next = *fx;
    next.state = hash;
    for (const auto& [param, value] : pending) next.params[param] = value;
    txn().put<model::FxInstance>(id, std::move(next));
    return changed;
}

Result<std::uint32_t> EngineApp::fxLatencySamples(model::Id id, const model::FxInstance& fx) {
    if (auto owner = model::frozenOwner(view(), id))
        return model::frozenError(view(), *owner, "/fx");
    // A running instance already knows the plan's rate and block. Otherwise
    // activate a throwaway instance at the project rate: a plugin reports its
    // latency in samples, so a rate is needed even when nothing is playing.
    if (host::Instance* live = instanceOf(id); live && live->active()) return live->latency();
    auto info = findPlugin(fx.plugin.id);
    if (!info) return std::move(info).error();
    auto made = host::Instance::create(*info, options_.identity);
    if (!made) return std::move(made).error();
    host::Instance& probe = **made;
    probe.setMainThreadWake(mainWake_);
    SUPPLE_TRY(restoreFx(probe, fx, view()));
    constexpr std::uint32_t kProbeBlock = 512;  // OfflineSettings' default block
    SUPPLE_TRY(probe.activate(head().project->sampleRate, 1, kProbeBlock));
    if (probe.paramsNormalized() && !fx.params.empty()) {
        if (!probe.startProcessing())
            return Error("fx.process", "plugin refused the latency probe", {},
                         "use fx.restart for the affected FX, or replace it with a plugin that "
                         "accepts the saved state and sample rate");
        float l = 0, r = 0;
        const float* inputs[]{&l, &r};
        float* outputs[]{&l, &r};
        const bool processed = probe.process({.inputs = inputs,
                                              .inputChannels = 2,
                                              .outputs = outputs,
                                              .outputChannels = 2,
                                              .frames = 1});
        probe.stopProcessing();
        if (!processed)
            return Error("fx.process", "plugin failed the latency probe", {},
                         "use fx.restart for the affected FX, or replace it with a plugin that "
                         "accepts the saved state and sample rate");
    }
    const std::uint32_t latency = probe.latency();
    probe.deactivate();
    return latency;
}

Result<json::Value> EngineApp::mergedPatch(const model::FxInstance& fx, const json::Value& patch,
                                           bool replace) {
    auto inr = scratch(fx);
    if (!inr) return std::move(inr).error();
    host::Instance& in = **inr;
    if (!in.info().semantic)
        return Error("fx.not_semantic", fx.plugin.id + " has no semantic patch", {},
                     "use fx.param.set with CLAP ids or names");
    // Start from the model's patch (or the plugin's defaults).
    if (fx.patch) {
        SUPPLE_TRY(in.setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
    } else {
        auto schema = in.semanticSchema();
        if (!schema) return std::move(schema).error();
        json::Value defaults = json::Value::object();
        defaults["schema"] =
            schema->find("patch_type") ? *schema->find("patch_type") : json::Value("");
        SUPPLE_TRY(in.setPatch(json::writeCompact(defaults), CLAP_SUPPLE_SET_REPLACE));
    }
    SUPPLE_TRY(in.setPatch(json::writeCompact(patch),
                           replace ? CLAP_SUPPLE_SET_REPLACE : CLAP_SUPPLE_SET_MERGE));
    auto text = in.getPatch(false);
    if (!text) return std::move(text).error();
    return json::parse(*text);
}

Result<std::string> EngineApp::describeFx(const model::FxInstance& fx, bool changedOnly) {
    auto inr = scratch(fx);
    if (!inr) return std::move(inr).error();
    host::Instance& in = **inr;
    if (!in.info().semantic) return in.info().name;
    auto patch = mergedPatch(fx, json::Value::object(), false);
    if (!patch) return std::move(patch).error();
    return in.describe(changedOnly);
}

void unpoolCopiedSource(model::Take& take, const model::Session& s, model::EditTxn& txn) {
    if (!take.source || take.source->kind != model::Kind::Midi) return;
    const model::MidiSource* source = s.midi.find(*take.source);
    if (source && !source->pooled) take.source = txn.create(*source);
}

Result<model::Id> copyTake(const model::Session& source, model::EditTxn& txn, model::Id original,
                           model::Take copy, double shift) {
    std::map<model::Id, model::Id> ids;
    copy.chain.clear();
    copy.warpPeers.clear();
    const auto* old = source.takes.find(original);
    if (!old)
        return Error(
            "item.take", "the copied take is missing", "/take",
            "use a current take id from take.list; recopy the item after undoing its deletion");
    for (model::Id id : old->chain) {
        const auto* fx = source.fx.find(id);
        if (!fx)
            return Error("item.fx", "the copied take FX is missing", "/take",
                         "recopy the item from a version that still contains its take FX, or undo "
                         "their removal");
        // The source may be a retained clipboard version whose opaque state
        // does not exist in the destination after undo or a branch switch.
        if (fx->state)
            if (const auto blob = source.blobs.find(*fx->state); blob != source.blobs.end())
                txn.putBlob(blob->first, blob->second);
        const auto fresh = txn.create(*fx);
        ids[id] = fresh;
        copy.chain.push_back(fresh);
    }
    const auto fresh = txn.create(std::move(copy));
    ids[original] = fresh;
    for (const auto& e : source.envelopes) {
        const auto to = ids.find(e.value->target.object);
        if (to == ids.end()) continue;
        model::Envelope env = *e.value;
        env.target.object = to->second;
        const bool relative = to->first == original;
        if (relative && shift != 0 && !env.points.empty()) {
            const auto& points = e.value->points;
            auto boundary = points.front();
            for (std::size_t i = 0; i + 1 < points.size(); ++i)
                if (points[i].pos <= shift && shift < points[i + 1].pos) {
                    boundary = points[i];
                    const double t = (shift - points[i].pos) / (points[i + 1].pos - points[i].pos);
                    boundary.value += (points[i + 1].value - points[i].value) *
                                      model::shapeCurve(boundary.shape, boundary.tension, t);
                }
            if (shift >= points.back().pos) boundary = points.back();
            boundary.pos = 0;
            env.points = {boundary};
            for (const auto& point : points)
                if (point.pos > shift) {
                    auto right = point;
                    right.pos -= shift;
                    env.points.push_back(right);
                }
            auto left = points;
            boundary.pos = shift;
            std::erase_if(left, [&](const auto& point) { return point.pos >= shift; });
            left.push_back(boundary);
            txn.modify<model::Envelope>(e.id, [&](auto& lane) { lane.points = left; });
        }
        const auto lane = txn.create(std::move(env));
        for (const auto& ai : source.autoItems)
            if (ai.value->envelope == e.id) {
                auto item = *ai.value;
                item.envelope = lane;
                if (relative) item.position -= shift;
                txn.create(std::move(item));
            }
    }
    return fresh;
}

// --- project files ----------------------------------------------------------------------

Status EngineApp::replaceProject(model::Session session, fs::path path,
                                 const std::function<Status()>& prepare,
                                 std::vector<schema::Warning>* warnings) {
    Backend* running = engine_ ? backend_ : nullptr;
    const auto config = engine_ ? engine_->config() : BackendConfig{};
    // Preserve the caller's Automatic/explicit choice, not the resolved count:
    // a project replacement also starts a new engine with the current preference.
    const auto settings = engine_ ? restartOptions_ : EngineOptions{};
    if (engine_) {
        auto stopped = transport("stop");
        if (!stopped) return stopped.error();
    }
    projectGeneration_.fetch_add(1, std::memory_order_acq_rel);
    // A control still moving belongs to the old project's effects.
    learnCommits_.clear();
    learnState_.clear();
    touched_.reset();
    if (commands_) {
        const auto jobs = commands_->jobs().list();
        for (const auto& job : jobs.find("jobs")->asArray())
            if (*job.find("status") == "running")
                SUPPLE_TRY(commands_->jobs().cancel(job.find("job")->asString()));
    }
    stopJobs();
    if (engine_) {
        engine_->stop();
        engine_.reset();
        protections_->stop();
        backend_ = nullptr;
    }
    if (versions_ && projectPath_.empty()) clearScratch();
    loadedHead_.reset();
    media_ = MediaCache{};
    scratch_.clear();
    reportedFailures_.clear();
    recordingFiles_.clear();
    endMasterRecording();
    autoWrite_.clear();
    renderQueue_.clear();
    {
        std::lock_guard lock(rendersMutex_);
        renders_.clear();
    }
    // The project's copies of the user's modules (ADR-0062), or none.
    const fs::path moduleFolder = projectModuleDir(path);
    std::vector<std::string> skipped;
    modules_ = detail::loadProjectModules(modules_, moduleFolder, &skipped);
    if (warnings && !skipped.empty()) {
        std::string message = "project module copies could not be loaded: ";
        for (std::size_t i = 0; i < skipped.size(); ++i) {
            if (i) message += "; ";
            message += skipped[i];
        }
        // An unreadable or broken source may not reveal its id. Name every
        // used type left without a project copy, and the version that will play.
        std::set<std::string> seen;
        for (const auto& m : session.modules) {
            const std::string& id = m.value->type;
            if (!seen.insert(id).second) continue;
            const auto* entry = modules_->find(id);
            if (!entry)
                message += "; " + id + ": unavailable";
            else if (entry->type->origin() == rack::Origin::Library)
                message += "; " + id + ": using the library version instead of a project copy";
        }
        warnings->push_back({"project.modules", std::move(message), moduleFolder.string()});
    }
    pool_ = std::make_unique<InstancePool>(
        [this](const model::PluginRef& ref) { return findPlugin(ref.id); }, options_.identity);
    pool_->setMainThreadWake(mainWake_);
    pool_->racks().setCatalog(modules_);
    pendingDirtyStates_.clear();
    if (scripts_) {
        clearScripts_ = scripts_->active();
        if (!clearScripts_) scripts_->clear();
    }
    versions_ = std::make_unique<model::VersionStore>(std::move(session));
    projectPath_ = std::move(path);
    autosaveMarked_ = !projectPath_.empty();
    autosaveMs_ = serviceClockMs();
    autosaveVersion_ = head().version;
    const Status prepared = prepare ? prepare() : Status{};
    if (running) SUPPLE_TRY(startEngine(*running, config, settings));
    headChanged();
    return prepared;
}

Status EngineApp::newProject(model::Project project) {
    return replaceProject(model::Session::create(std::move(project)), {});
}

fs::path EngineApp::projectModuleDir(const fs::path& project) const {
    if (project.empty()) return {};
    fs::path dir = project.parent_path() / "modules";
    std::error_code ec;
    if (fs::equivalent(dir, moduleDir(), ec)) return {};  // the library itself
    return dir;
}

fs::path EngineApp::resolve(const fs::path& path) const {
    return path.is_relative() ? options_.workdir / path : path;
}

Status EngineApp::save(const fs::path& where, std::vector<schema::Warning>* warnings) {
    const fs::path path = resolve(where);
    if (!txn_) commitLearned(true);  // a control just moved: its values in the file
    // What running plug-ins hold now (a sampler's loaded files), as hosts
    // save it, whether or not they said it changed.
    std::vector<model::Id> running;
    for (const auto& e : head().fx) running.push_back(e.id);
    captureFxStates(running, warnings);
    SUPPLE_TRY(model::checkInvariants(head()));
    const bool fromScratch = projectPath_.empty() && !options_.scratchDir.empty();
    if (fromScratch && engine_ && engine_->recording())
        return Error("project.recording", "a recording is running", {},
                     "stop it before the project's first save (its files move then)");
    auto publish = [&] { return writeFileAtomic(path, model::serializeProject(head())); };
    const fs::path media = path.parent_path() / head().project->mediaDir;
    if (fromScratch) {
        SUPPLE_TRY(moveScratchMedia(media, publish));
    } else {
        // Saved as into another folder: that folder gets its own copy of the
        // media (the old project keeps its files).
        if (!projectPath_.empty()) SUPPLE_TRY(detail::copyFolder(mediaDir(), media));
        SUPPLE_TRY(publish());
    }
    // The user's modules it plays go with it, as its media do (ADR-0062).
    if (const fs::path dir = projectModuleDir(path); !dir.empty()) {
        std::vector<std::string> types;
        for (const auto& e : head().modules) types.push_back(e.value->type);
        if (Status copied = detail::saveProjectModules(*modules_, types, dir); !copied && warnings)
            warnings->push_back(
                {"project.modules",
                 "the project's module copies were not written: " + copied.error().message,
                 dir.string()});
    }
    projectPath_ = path;
    autosaveMarked_ = true;  // the file itself is the newest copy
    autosaveMs_ = serviceClockMs();
    autosaveVersion_ = head().version;
    return okStatus();
}

Status EngineApp::open(const fs::path& where, std::vector<schema::Warning>* warnings) {
    const fs::path path = resolve(where);
    auto text = readTextFile(path, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    auto s = model::parseProject(*text, warnings);
    if (!s) return std::move(s).error();
    return replaceProject(std::move(*s), path, {}, warnings);
}

Status EngineApp::writeTemplate(const fs::path& folder, std::vector<schema::Warning>* warnings) {
    std::vector<model::Id> running;
    for (const auto& e : head().fx) running.push_back(e.id);
    captureFxStates(running, warnings);
    SUPPLE_TRY(model::checkInvariants(head()));
    const model::Session& s = head();
    model::EditTxn txn(s, s.version);
    for (const auto& e : s.media) {
        fs::path file = e.value->path;
        if (file.is_relative()) {  // as plans find it
            const fs::path inMedia = mediaDir() / file;
            file = fs::exists(inMedia) ? inMedia : projectDir() / file;
        }
        auto name = detail::copyIntoFolder(file, folder / "media");
        if (!name) return std::move(name).error();
        txn.modify<model::MediaSource>(e.id, [&](model::MediaSource& m) { m.path = *name; });
    }
    model::Project project = txn.project();
    project.mediaDir = "media";
    txn.setProject(std::move(project));
    auto copy = txn.commit();
    if (!copy) return std::move(copy).error();
    return writeFileAtomic(folder / "project.json", model::serializeProject(*copy));
}

Status EngineApp::openTemplate(const fs::path& where, std::vector<schema::Warning>* warnings) {
    const fs::path path = resolve(where);
    std::error_code ec;
    const bool folder = fs::is_directory(path, ec);
    auto text = readTextFile(folder ? path / "project.json" : path, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    auto s = model::parseProject(*text, warnings);
    if (!s) return std::move(s).error();
    if (!folder) return replaceProject(std::move(*s), {});  // an older template: absolute paths
    const fs::path from = path / s->project->mediaDir;
    if (options_.scratchDir.empty()) {
        // No folder for an unsaved project (a headless engine): the copies go
        // to <workdir>/media, named absolutely.
        const fs::path to = options_.workdir / "media";
        model::EditTxn txn(*s, s->version);
        for (const auto& e : s->media) {
            if (fs::path(e.value->path).is_absolute()) continue;
            auto name = detail::copyIntoFolder(from / e.value->path, to);
            if (!name) return std::move(name).error();
            txn.modify<model::MediaSource>(
                e.id, [&](model::MediaSource& m) { m.path = (to / *name).string(); });
        }
        auto copy = txn.commit();
        if (!copy) return std::move(copy).error();
        return replaceProject(std::move(*copy), {});
    }
    // The new project's folder is emptied when the old project goes, so the
    // copies are made after that, with the names the template gives them.
    return replaceProject(std::move(*s), {},
                          [this, from] { return detail::copyFolder(from, mediaDir()); });
}

// --- branches ----------------------------------------------------------------------------

Result<model::Replayed> EngineApp::replay(const cmd::Registry& registry, const model::Session& base,
                                          const std::vector<model::Invocation>& invocations,
                                          std::uint64_t version) {
    std::unique_ptr<model::EditTxn> saved = std::move(txn_);
    model::TxnRecord savedRecord = std::move(record_);
    txn_ = std::make_unique<model::EditTxn>(base, version);
    auto restore = [&] {
        txn_ = std::move(saved);
        record_ = std::move(savedRecord);
    };
    for (std::size_t i = 0; i < invocations.size(); ++i) {
        const model::Invocation& inv = invocations[i];
        const cmd::CommandInfo* info = registry.find(inv.command);
        if (!info) {
            restore();
            return Error("history.replay", "unknown command " + inv.command + " in history", {},
                         "use history undo or a branch predating the unsupported command, or open "
                         "this history with an engine that provides that command");
        }
        cmd::Context ctx = makeContext({cmd::Origin::Type::Internal, "replay", ""});
        ctx.provide<const cmd::Registry>(registry);
        auto r = info->invoke(inv.args, ctx);
        if (!r) {
            Error e = std::move(r).error();
            restore();
            e.message = inv.command + ": " + e.message;
            return e;
        }
    }
    model::Replayed out;
    out.changes = txn_->changes();
    out.created = txn_->created();
    auto validation = model::validateStripRules(txn_->preview());
    if (!validation) {
        restore();
        return std::move(validation).error();
    }
    auto s = txn_->commit();
    restore();
    if (!s) return std::move(s).error();
    out.session = std::move(*s);
    return out;
}

void EngineApp::restartAutomationPass() {
    // Moving through history mid-pass (ADR-0061): the writes so far belong to
    // the version left behind, so nothing they wrote stays held or gets a
    // latch tail at stop; the pass starts again here.
    if (engine_ && engine_->transport().playing)
        beginAutomationPass(engine_->transport().seconds);
    else
        autoWrite_.clear();
}

Status EngineApp::undo() {
    SUPPLE_TRY(versions_->undo());
    if (engine_ && engine_->recording())
        if (auto valid = model::validateRecordingPlayrate(head()); !valid) {
            // Restore history before publishing any audio/model notification.
            SUPPLE_TRY(versions_->redo());
            return valid;
        }
    restartAutomationPass();
    headChanged();
    json::Value ev = json::Value::object();
    ev["version"] = head().version;
    ev["branch"] = versions_->branch();
    emit("session.changed", std::move(ev));
    return {};
}

Status EngineApp::redo() {
    SUPPLE_TRY(versions_->redo());
    if (engine_ && engine_->recording())
        if (auto valid = model::validateRecordingPlayrate(head()); !valid) {
            SUPPLE_TRY(versions_->undo());
            return valid;
        }
    restartAutomationPass();
    headChanged();
    json::Value ev = json::Value::object();
    ev["version"] = head().version;
    ev["branch"] = versions_->branch();
    emit("session.changed", std::move(ev));
    return {};
}

Status EngineApp::switchBranch(std::string_view name) {
    if (engine_ && engine_->recording())
        if (const auto target = versions_->head(name))
            SUPPLE_TRY(model::validateRecordingPlayrate(*target));
    SUPPLE_TRY(versions_->switchBranch(name));
    restartAutomationPass();
    headChanged();
    return okStatus();
}

model::MergeOutcome EngineApp::mergeBranch(const cmd::Registry& registry, std::string_view from,
                                           std::string_view into) {
    const bool recordingTarget = engine_ && engine_->recording() && versions_->branch() == into;
    // Fast-forwards bypass the replayer. Only preflight those: an unsafe
    // source snapshot need not make a replay unsafe (its rate may be inherited
    // from the fork and already reset on the recording branch).
    if (recordingTarget)
        if (const auto source = versions_->head(from)) {
            bool fastForward = false;
            std::vector<std::uint64_t> pending{source->version};
            std::set<std::uint64_t> seen;
            const auto target = versions_->headVersion(into);
            while (!pending.empty() && !fastForward) {
                const auto id = pending.back();
                pending.pop_back();
                if (!seen.insert(id).second) continue;
                fastForward = id == target;
                if (const auto* version = versions_->version(id))
                    pending.insert(pending.end(), version->parents.begin(), version->parents.end());
            }
            if (auto valid = model::validateRecordingPlayrate(*source); fastForward && !valid) {
                model::MergeOutcome refused;
                refused.head = target;
                refused.conflicts.push_back(
                    {model::Conflict::Kind::Invalid, 0, 0, {}, valid.error().toString()});
                return refused;
            }
        }
    auto outcome = versions_->merge(
        from, into,
        [&](const model::Session& base, const std::vector<model::Invocation>& inv,
            std::uint64_t version) -> Result<model::Replayed> {
            auto next = replay(registry, base, inv, version);
            if (!next) return std::move(next).error();
            if (recordingTarget) SUPPLE_TRY(model::validateRecordingPlayrate(next->session));
            return next;
        },
        [&](std::string_view command) -> const json::Value* {
            const cmd::CommandInfo* info = registry.find(command);
            return info ? &info->argsSchema : nullptr;
        },
        clockMs());
    if (outcome.conflicts.empty() && !outcome.upToDate && versions_->branch() == into)
        headChanged();
    return outcome;
}

// --- rendering ----------------------------------------------------------------------------

Result<OfflineResult> EngineApp::renderMix(const OfflineSettings& settings) {
    OfflineSettings s = settings;
    if (!s.media) s.media = &media_;
    return renderSession(view(), s);
}

Result<OfflineResult> EngineApp::renderSession(const model::Session& session,
                                               const OfflineSettings& settings) {
    OfflineSettings s = settings;
    if (s.projectDir.empty()) s.projectDir = projectDir();
    MediaCache localMedia;
    if (!s.media) s.media = &localMedia;
    // Offline renders use their own plugin instances (engine.md §3): playback
    // is never disturbed and state is deterministic.
    InstancePool offlinePool([this](const model::PluginRef& ref) { return findPlugin(ref.id); },
                             options_.identity);
    if (!s.modules) s.modules = modules_;
    return renderOffline(session, offlinePool, s);
}

std::string EngineApp::rememberRender(const fs::path& path) {
    const std::string id = reserveRenderId();
    publishRender(id, path);
    return id;
}

std::string EngineApp::reserveRenderId() {
    std::lock_guard lock(rendersMutex_);
    return "rd_" + std::to_string(nextRender_++);
}

void EngineApp::publishRender(std::string id, const fs::path& path) {
    std::lock_guard lock(rendersMutex_);
    renders_[std::move(id)] = path;
}

Result<fs::path> EngineApp::renderPath(std::string_view reference) const {
    if (reference.starts_with("rd_")) {
        std::lock_guard lock(rendersMutex_);
        auto it = renders_.find(std::string(reference));
        if (it == renders_.end())
            return Error("engine.unknown_render", "no such render " + std::string(reference), {},
                         "render.mix returns a render id; use it or a WAV path");
        return it->second;
    }
    return resolve(fs::path(reference));
}

void EngineApp::startJob(std::function<void(supple::StopToken)> work) {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->finished->load(std::memory_order_acquire))
            it = jobs_.erase(it);
        else
            ++it;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    jobs_.push_back(
        {supple::JoiningThread([hook = options_.jobStartedHook, done = options_.jobFinishedHook,
                                work = std::move(work), finished, this](supple::StopToken stop) {
             if (hook) hook();
             work(stop);
             if (done) done();
             finished->store(true, std::memory_order_release);
             mainWake_.notify();
         }),
         finished});
}

void EngineApp::stopJobs() {
    for (auto& job : jobs_) job.thread.request_stop();
    jobs_.clear();  // jthread joins before the command bus destroys JobManager
}

// --- real-time engine -----------------------------------------------------------------------

Status EngineApp::setAudioThreadLimit(unsigned limit) {
    if (limit > 64)
        return Error("engine.audio_threads", "audio processing thread limit is out of range",
                     "/audio_threads", "use 0 for Automatic or an integer from 1 to 64");
    audioThreadLimit_ = limit;
    return {};
}

Status EngineApp::setMemoryLock(bool requested) {
    return protections_->setMemoryLock(requested, engine_ && backend_);
}

const RuntimeProtectionStatus& EngineApp::memoryLockStatus() const {
    return protections_->memoryLock();
}

Status EngineApp::setPreventDeepIdle(bool requested) {
    return protections_->setPreventDeepIdle(requested, engine_ && backend_);
}

const RuntimeProtectionStatus& EngineApp::cpuIdleLatencyStatus() const {
    return protections_->cpuIdleLatency();
}

Status EngineApp::startEngine(Backend& backend, const BackendConfig& config) {
    return startEngine(backend, config, EngineOptions{});
}

Status EngineApp::startEngine(Backend& backend, const BackendConfig& config,
                              const EngineOptions& options) {
    if (engine_)
        return Error("engine.running", "the engine is already running", {},
                     "use engine.stop before changing the backend or starting another engine");
    auto effective = options;
    if (effective.workers < 0 && audioThreadLimit_ > 0)
        effective.workers = static_cast<int>(audioThreadLimit_ - 1);
    engine_ = std::make_unique<Engine>(*pool_, media_, effective);
    engine_->setMainThreadWake(mainWake_);
    backend.setMainThreadWake(&mainWake_);
    auto st = engine_->start(backend, config);
    if (!st) {
        engine_.reset();
        backend.setMainThreadWake(nullptr);
        return st;
    }
    model::Session atRate = head();
    auto project = *atRate.project;
    project.sampleRate = engine_->config().sampleRate;
    atRate.project = std::make_shared<const model::Project>(std::move(project));
    auto numeric = model::validateNumeric(atRate);
    if (numeric)
        for (const auto& range : {loop_, punch_})
            if (range.end > range.start) {
                numeric = model::checkedSeconds(atRate, range.end, "/end_seconds");
                if (!numeric) break;
            }
    if (!numeric) {
        engine_->stop();
        engine_.reset();
        backend.setMainThreadWake(nullptr);
        return numeric.error();
    }
    backend_ = &backend;
    restartBackend_ = &backend;  // a caller-supplied backend: reuse it
    restartConfig_ = engine_->config();
    restartOptions_ = options;  // preserve Automatic for a later device/project restart
    restarting_ = false;
    playStart_ = 0.0;
    if (loop_.enabled || loop_.end > loop_.start)
        engine_->setLoop(loop_.start, loop_.end, loop_.enabled);
    engine_->setMetronome(metronome_.enabled, metronome_.volumeDb);
    auto loaded = engine_->load(head(), projectDir());
    if (!loaded) {
        haltEngine();
        return loaded;
    }
    loadedHead_ = versions_->head();
    protections_->start();
    return {};
}

Status EngineApp::startEngine(std::string_view backend, std::uint32_t block, bool anticipative,
                              const EngineDeviceOptions& device) {
    if (engine_)
        return Error("engine.running", "the engine is already running", {},
                     "engine.stop first to change the backend or block size");
    BackendConfig config;
    config.sampleRate = device.rate > 0 ? device.rate : view().project->sampleRate;
    config.blockSize = block;
    config.inputs = 2;         // at least the device's first stereo pair, for recording
    config.midiInputs = true;  // its MIDI inputs, for MIDI tracks (ADR-0016)
    if (backend == "jack") {
#if defined(__linux__)
        JackOptions o;
        o.clientName = std::string(product::kSlug);
        o.library = options_.jackLibrary;
        o.requestRate = device.rate;  // an explicit rate is asked of the graph
        o.forceGraph = device.forceGraph;
        // One input per physical capture port (at least the first pair, at
        // most 32), so any device input can feed a track.
        if (auto names = JackBackend::captureNames(o); names && !names->empty())
            config.inputs =
                static_cast<std::uint32_t>(std::clamp<std::size_t>(names->size(), 2, 32));
        // One output per physical playback port (2 to 64): any master can
        // play to any of them (ADR-0056).
        if (device.outputs > 0)
            config.outputs = device.outputs;
        else if (auto names = JackBackend::playbackNames(o); names && !names->empty())
            config.outputs =
                static_cast<std::uint32_t>(std::clamp<std::size_t>(names->size(), 2, 64));
        ownedBackend_ = std::make_unique<JackBackend>(o);
#else
        return Error("backend.unsupported", "the JACK backend is unavailable on this platform",
                     "/backend", "select portaudio for a native device, or dummy without a device");
#endif
    } else if (backend == "portaudio") {
        config.inputs = device.inputs;
        config.outputs = device.outputs > 0 ? device.outputs : 2;
        PortAudioOptions native = device.native;
        native.changeDeviceRate = device.rate > 0;  // an explicit rate is asked of the device
        ownedBackend_ = std::make_unique<PortAudioBackend>(native);
    } else if (backend == "dummy") {
        config.outputs = device.outputs > 0 ? device.outputs : 2;
        ownedBackend_ = std::make_unique<DummyBackend>(
            DummyBackend::Options{.mode = DummyBackend::Mode::Thread, .paced = true});
    } else {
        return Error("engine.backend", "unknown backend " + std::string(backend), {},
                     "select jack, portaudio or dummy");
    }
    auto st = startEngine(*ownedBackend_, config, EngineOptions{.anticipative = anticipative});
    if (!st) {
        haltEngine();  // e.g. the project's rate is not the device's
        return st;
    }
    // The app owns this backend: a restart recreates it (the instance may be
    // dead with the device).
    engineDevice_ = device;
    engineKind_ = std::string(backend);
    engineBlock_ = block;
    engineAnticipative_ = anticipative;
    restartBackend_ = nullptr;
    return st;
}

void EngineApp::haltEngine() {
    // A pass the halt interrupts is never finished (ADR-0061): its writes
    // must not come back as holds or a latch tail when a successor plays or
    // records (recording, unlike play, starts no new pass).
    autoWrite_.clear();
    sourcePreparation_.reset();
    pendingSourceKey_.clear();
    preparationError_.clear();
    if (engine_) {
        engine_->stop();
        auto takes = engine_->stopRecording();
        engine_->service();
        engine_.reset();
        recordingFiles_.clear();
        recordingMasters_ = false;  // the engine that held them is gone
        if (takes) {
            if (auto committed = commitTakes(*takes); !committed) {
                const auto& error = committed.error();
                auto event = json::Value::object();
                event["code"] = error.code;
                event["message"] = error.message;
                event["path"] = error.path;
                event["hint"] = error.hint;
                emit("record.error", std::move(event));
            }
        }
        pool_->prune(head());  // nothing processes now: replaced instances can go
    }
    protections_->stop();
    if (backend_) backend_->setMainThreadWake(nullptr);
    backend_ = nullptr;
    ownedBackend_.reset();
    outputGestures_.clear();
    pendingOutputs_.clear();
    outputGeneration_ = outputEpoch_ = 0;
    outputDrops_ = voiceDrops_ = consumerDrops_ = 0;
    outputOverflow_ = false;
}

void EngineApp::stopEngine() {
    restarting_ = false;  // an explicit stop cancels a pending restart
    haltEngine();
}

// --- main-thread service ------------------------------------------------------------

namespace {
constexpr std::uint64_t kAutosaveIntervalMs = 60'000;
constexpr int kAutosaveKeep = 5;
constexpr std::uint64_t kRestartGiveUpMs = 30'000;
// A touch envelope's hold ends this long after its last write (ADR-0061):
// nothing reports a strip control's release.
constexpr std::uint64_t kTouchReleaseMs = 500;
}  // namespace

void EngineApp::tick() {
    mainWake_.drain();  // re-arm before inspecting producer queues
    std::erase_if(jobs_,
                  [](const auto& job) { return job.finished->load(std::memory_order_acquire); });
    if (sourcePreparation_)
        if (auto ready = sourcePreparation_->take()) {
            pendingSourceKey_.clear();
            if (*ready) {
                media_.adoptSources(**ready);
                preparationError_.clear();
                headChanged();
            } else {
                preparationError_ = ready->error().toString();
                emit("engine.error", preparationError_);
            }
        }
    if (clearScripts_ && scripts_ && !scripts_->active()) {
        scripts_->clear();
        clearScripts_ = false;
    }
    if (engine_) {
        engine_->service();
        if (!txn_) updateWriteHolds();  // a touch hold's release time passed
        consumePluginOutputs();
        pollControl();
        // Controller actions may stop or replace the engine on this thread.
        auto dirty = engine_ ? engine_->takeDirtyStates() : std::vector<model::Id>{};
        for (const auto id : dirty)
            if (std::find(pendingDirtyStates_.begin(), pendingDirtyStates_.end(), id) ==
                pendingDirtyStates_.end())
                pendingDirtyStates_.push_back(id);
        if (!txn_) captureFxStates(std::exchange(pendingDirtyStates_, {}));
        // Failures reach subscribers without a client asking.
        checkFailures();
        if (backend_ && engine_) {
            const BackendStatus b = backend_->status();
            if (b.restartNeeded)
                beginRestart(b.restartReason.empty() ? std::string("the device changed")
                                                     : std::string(b.restartReason),
                             engine_->transport().seconds);
        }
    } else if (restarting_) {
        retryRestart();
    }
    // Probe instances also own callback, FD and timer subscriptions. Their
    // idle outputs describe the probe only and never edit an arbitrary FX.
    for (const auto& [id, instance] : scratch_) {
        instance->serviceMainThread();
        instance->takeNotifications();
        instance->takeIdleOutputs();
    }
    if (!engine_) {
        for (const auto& fx : head().fx) {
            auto notes = pool_->service(fx.id);
            if (notes.stateDirty &&
                std::find(pendingDirtyStates_.begin(), pendingDirtyStates_.end(), fx.id) ==
                    pendingDirtyStates_.end())
                pendingDirtyStates_.push_back(fx.id);
        }
        consumePluginOutputs();
        pollControl();
        checkFailures();
    }
    if (!txn_) {
        auto dirty = std::exchange(pendingDirtyStates_, {});
        captureFxStates(dirty);
    }
    maybeAutosave();
    if (commands_) commands_->flushJobEvents();
    // What the tick emitted (fx.failed, engine.restarting, …) reaches scripts
    // now, not after some later command.
    if (scripts_ && !clearScripts_) scripts_->deliver();
}

std::uint64_t EngineApp::serviceClockMs() const {
    if (fixedClock_) return fixedClock_;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
std::vector<host::Instance*> EngineApp::serviceInstances() const {
    auto out = pool_->instances();
    for (const auto& [id, instance] : scratch_)
        if (instance) out.push_back(instance.get());
    return out;
}
std::vector<EngineApp::ServiceFd> EngineApp::serviceFds() const {
    std::vector<ServiceFd> out;
#if !defined(_WIN32)
    for (auto* instance : serviceInstances()) {
        if (instance->isolated()) continue;  // bridge owns its subscriptions
        for (const auto& [fd, flags] : instance->watchedFds()) {
            short events = 0;
            if (flags & CLAP_POSIX_FD_READ) events |= POLLIN;
            if (flags & CLAP_POSIX_FD_WRITE) events |= POLLOUT;
            if (flags & CLAP_POSIX_FD_ERROR) events |= POLLERR;
            out.push_back({fd, events, instance});
        }
    }
#endif
    return out;
}
void EngineApp::serviceFdReady(const ServiceFd& watch, short ready) {
#if defined(_WIN32)
    (void)watch;
    (void)ready;
#else
    // A preceding ready callback/command may replace an instance or unregister
    // an fd. Never dereference a stale owner from a poll snapshot.
    for (const auto& current : serviceFds()) {
        if (current.owner != watch.owner || current.fd != watch.fd) continue;
        std::uint32_t flags = 0;
        if (ready & POLLIN) flags |= CLAP_POSIX_FD_READ;
        if (ready & POLLOUT) flags |= CLAP_POSIX_FD_WRITE;
        if (ready & (POLLERR | POLLHUP | POLLNVAL)) flags |= CLAP_POSIX_FD_ERROR;
        current.owner->onFd(current.fd, flags);
        return;
    }
#endif
}
std::optional<std::uint64_t> EngineApp::nextServiceDelayMs() const {
    std::optional<std::uint64_t> delay;
    const auto now = serviceClockMs();
    auto at = [&](std::uint64_t deadline) {
        const auto ms = deadline <= now ? 0 : deadline - now;
        if (!delay || ms < *delay) delay = ms;
    };
    if (!projectPath_.empty() && head().version != autosaveVersion_)
        at(autosaveMs_ + kAutosaveIntervalMs);
    if (restarting_) at(std::min(restartRetryAt_, restartUntil_));
    if (!txn_)
        for (const auto& [key, commit] : learnCommits_) at(commit.lastMs + 300);
    if (!txn_ && engine_) {
        const auto& holds = engine_->writeHolds();
        for (const auto& [id, w] : autoWrite_)  // touch holds end (ADR-0061)
            if (const auto* env = head().envelopes.find(id);
                w.touched && env && env->mode == model::EnvMode::Touch &&
                std::find(holds.begin(), holds.end(), env->target) != holds.end())
                at(w.lastMs + kTouchReleaseMs);
    }
    for (auto* instance : serviceInstances()) {
        if (instance->isolated()) continue;
        const int timer = instance->nextTimerMs();
        if (timer >= 0) at(now + static_cast<std::uint64_t>(timer));
    }
    return delay;
}
std::optional<std::chrono::steady_clock::time_point> EngineApp::nextServiceDeadline() const {
    const auto delay = nextServiceDelayMs();
    if (!delay) return {};
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(*delay);
}

void EngineApp::maybeAutosave() {
    if (projectPath_.empty()) return;
    const std::uint64_t now = serviceClockMs();
    if (!autosaveMarked_) {
        autosaveMarked_ = true;
        autosaveMs_ = now;
        autosaveVersion_ = head().version;
        return;
    }
    // Autosave when the head changed and an interval passed since the last one.
    if (head().version == autosaveVersion_ || now < autosaveMs_ + kAutosaveIntervalMs) return;
    autosave();
}

void EngineApp::beginRestart(std::string reason, double seconds) {
    restartReason_ = std::move(reason);
    restartError_.clear();
    restartSeconds_ = seconds;
    restartBackoff_ = 0;
    restartUntil_ = serviceClockMs() + kRestartGiveUpMs;
    restartRetryAt_ = serviceClockMs();
    json::Value ev = json::Value::object();
    ev["reason"] = restartReason_;
    emit("engine.restarting", std::move(ev));
    // Playback stays stopped; the position and the loop are restored.
    haltEngine();
    restarting_ = true;
}

void EngineApp::retryRestart() {
    const std::uint64_t now = serviceClockMs();
    if (now >= restartUntil_) {
        restarting_ = false;
        auto ev = json::Value::object();
        ev["reason"] = restartReason_;
        ev["error"] = restartError_.empty() ? "device restart deadline expired" : restartError_;
        emit("engine.error", std::move(ev));
        return;
    }
    if (now < restartRetryAt_) return;
    Status st = okStatus();
    if (!engineKind_.empty())
        st = startEngine(engineKind_, engineBlock_, engineAnticipative_, engineDevice_);
    else if (restartBackend_)
        st = startEngine(*restartBackend_, restartConfig_, restartOptions_);
    else
        st = Error("engine.restart", "there is no backend to restart", {},
                   "start a backend with engine.start before requesting a restart");
    if (st) {
        restarting_ = false;
        if (engine_) {
            engine_->seek(restartSeconds_);  // the transport position is restored
            engine_->awaitTransport();       // ... before the tick returns
        }
        return;
    }
    restartError_ = st.error().toString();
    const auto finishedAt = serviceClockMs();
    if (finishedAt >= restartUntil_) {
        restarting_ = false;
        json::Value ev = json::Value::object();
        ev["reason"] = restartReason_;
        ev["error"] = restartError_;
        emit("engine.error", std::move(ev));
        return;
    }
    restartBackoff_ =
        restartBackoff_ == 0 ? 250 : std::min<std::uint64_t>(restartBackoff_ * 2, 4000);
    restartRetryAt_ = finishedAt + restartBackoff_;
}

fs::path EngineApp::autosaveDir() const {
    if (projectPath_.empty()) return {};
    return projectPath_.parent_path() / (projectPath_.filename().string() + "-data");
}

namespace {
std::optional<std::uint64_t> autosaveIndex(std::string_view name) {
    constexpr std::string_view prefix = "autosave-", suffix = ".json";
    if (!name.starts_with(prefix) || !name.ends_with(suffix)) return std::nullopt;
    name.remove_prefix(prefix.size());
    name.remove_suffix(suffix.size());
    std::uint64_t index = 0;
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || index == 0)
        return std::nullopt;
    return index;
}
}  // namespace

fs::path EngineApp::newerAutosave(const fs::path& project) const {
    if (project.empty()) return {};
    std::error_code ec;
    const fs::path dir = project.parent_path() / (project.filename().string() + "-data");
    if (!fs::is_directory(dir, ec)) return {};
    const auto written = fs::last_write_time(project, ec);
    if (ec) return {};
    fs::path best;
    fs::file_time_type bestTime{};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& candidate = it->path();
        const std::string name = candidate.filename().string();
        if (!autosaveIndex(name)) continue;
        if (!fs::is_regular_file(candidate, ec)) continue;
        const auto time = fs::last_write_time(candidate, ec);
        if (ec) break;
        if (time > written && (best.empty() || time > bestTime)) {
            best = candidate;
            bestTime = time;
        }
    }
    return best;
}

bool EngineApp::captureFxState(model::Id id, Error* failure) {
    if (txn_ || !pool_ || model::frozenOwner(head(), id)) return false;
    const model::FxInstance* fx = head().fx.find(id);
    host::Instance* instance = pool_->find(id);
    // Semantic plug-ins keep their patch in the model already.
    if (!fx || fx->patch || !instance || instance->info().semantic || pool_->failure(id))
        return false;
    auto saved = instance->saveState();
    if (!saved) {
        if (failure) *failure = saved.error();
        return false;
    }
    if (saved->empty()) return false;
    const std::string hash = "sha256:" + sha256Hex(*saved);
    if (fx->state == hash) return false;
    cmd::Origin origin{cmd::Origin::Type::Internal, "plugin", ""};
    origin.gesture = "state:" + id.str();  // later changes to it join this step
    auto txid = beginTransaction(origin, "Plug-in state");
    if (!txid) return false;
    txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{std::move(*saved)}));
    model::FxInstance next = *fx;
    next.state = hash;
    const std::string previous = fx->state.value_or("");
    txn().put<model::FxInstance>(id, std::move(next));
    pool_->acknowledgeOutputState(id, hash);  // the instance holds it: no reload
    if (!commitTransaction(*txid)) {
        (void)rollbackTransaction(*txid);
        pool_->acknowledgeOutputState(id, previous);
        return false;
    }
    return true;
}

void EngineApp::captureFxStates(const std::vector<model::Id>& fx,
                                std::vector<schema::Warning>* failures) {
    for (model::Id id : fx) {
        Error failure;
        if (captureFxState(id, failures ? &failure : nullptr) || failure.code.empty()) continue;
        const model::Session& s = head();
        const model::FxInstance* instance = s.fx.find(id);
        std::string where = instance ? instance->plugin.name : id.str();
        if (const auto track = model::trackOfFx(s, id))
            if (const model::Track* t = s.tracks.find(*track)) where += " on " + t->name;
        failures->push_back({"fx.state",
                             where + ": its current state could not be read (" + failure.message +
                                 "); the state saved before is kept",
                             "/fx/" + id.str()});
    }
}

void EngineApp::autosave() {
    const fs::path dir = autosaveDir();
    if (dir.empty()) return;
    // Rotate: the next free index, then the oldest files over the keep count go.
    std::uint64_t next = 1;
    std::vector<std::pair<std::uint64_t, fs::path>> existing;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const auto n = autosaveIndex(name);
        if (!n) continue;
        if (*n == UINT64_MAX) {
            emit(
                "engine.error",
                json::Value(Error("autosave.index", "autosave numbering is exhausted", dir.string(),
                                  "move the highest numbered autosave to a backup name and retry")
                                .toString()));
            autosaveMs_ = serviceClockMs();
            return;
        }
        next = std::max(next, *n + 1);
        existing.push_back({*n, it->path()});
    }
    const fs::path file = dir / ("autosave-" + std::to_string(next) + ".json");
    if (auto st = writeFileAtomic(file, model::serializeProject(head())); !st) {
        emit("engine.error", json::Value(st.error().toString()));
        autosaveMs_ = serviceClockMs();  // try again after an interval, not every tick
        return;
    }
    existing.push_back({next, file});
    std::sort(existing.begin(), existing.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    while (static_cast<int>(existing.size()) > kAutosaveKeep) {
        std::error_code ignored;
        fs::remove(existing.front().second, ignored);
        existing.erase(existing.begin());
    }
    autosaveVersion_ = head().version;
    autosaveMs_ = serviceClockMs();
    autosaveMarked_ = true;
}

void EngineApp::restartFx(model::Id fx) {
    pool_->restart(fx);
    reportedFailures_.erase(fx);
    headChanged();  // a running engine gets the new instance in its next plan
}

EngineStatus EngineApp::engineStatus() {
    checkFailures();
    EngineStatus st;
    st.preparingAudio = !pendingSourceKey_.empty();
    st.preparationVersion = st.preparingAudio ? head().version : 0;
    st.preparedVersion = loadedHead_ ? loadedHead_->version : 0;
    st.preparationError = preparationError_;
    st.memoryLock = protections_->memoryLock();
    st.cpuIdleLatency = protections_->cpuIdleLatency();
    if (st.cpuIdleLatency.error)
        st.warnings.push_back(st.cpuIdleLatency.error->message + "; " +
                              st.cpuIdleLatency.error->hint);
    if (st.memoryLock.error)
        st.warnings.push_back(st.memoryLock.error->message + "; " + st.memoryLock.error->hint);
    st.dropped = static_cast<std::int64_t>(outputDrops_ + consumerDrops_);
    st.voiceDropped = static_cast<std::int64_t>(voiceDrops_);
    for (const auto& [id, error] : pool_->failedErrors()) {
        st.failedFx.push_back(id.str());
        auto failure = json::Value::object();
        failure["fx"] = id.str();
        failure["reason"] = error.message;
        failure["error"] = pluginErrorJson(error);
        st.failedErrors.asArray().push_back(std::move(failure));
    }
    if (!engine_ || !backend_) return st;
    const BackendStatus b = backend_->status();
    st.running = true;
    st.backend = backend_->name();
    st.sampleRate = engine_->config().sampleRate;
    st.block = engine_->config().blockSize;
    st.period = b.period;
    st.planLatency = engine_->outputLatency();
    st.deviceLatency = b.outputLatency;
    st.xruns = static_cast<std::int64_t>(b.xruns);
    st.midiDropped = static_cast<std::int64_t>(backend_->midiDropped());
    st.midiWarnings = backend_->midiWarnings();
    for (const auto& warning : st.midiWarnings)
        st.warnings.push_back(warning.device + ": " + warning.message + " (" +
                              std::to_string(warning.osError) + "); " + warning.hint);
    st.cycles = static_cast<std::int64_t>(engine_->cycles());
    st.processErrors = static_cast<std::int64_t>(engine_->processErrors());
    st.restartNeeded = b.restartNeeded;
    st.midiDevicesChanged = b.midiDevicesChanged;
    if (st.midiDevicesChanged)
        st.midiDevicesMessage = "MIDI devices changed; restart the engine to pick up the new set";
    st.anticipative = engine_->options().anticipative;
    st.liveHelpers = engine_->liveHelpers();
    const auto threadClass = [](ThreadPolicy p) { return ThreadClass{p.kind, p.priority}; };
    const Engine::Scheduling& sched = engine_->scheduling();
    st.scheduling = {threadClass(sched.callback), threadClass(sched.live),
                     threadClass(sched.renderer), threadClass(sched.background)};
    const WorkgroupStatus group = backend_->workgroup();
    st.workgroup = {group.state,   group.joined,  group.failed,
                    group.osError, group.message, group.hint};
    const auto warnings = engine_->warnings();
    st.warnings.insert(st.warnings.end(), warnings.begin(), warnings.end());
    st.anticipationMisses = static_cast<std::int64_t>(engine_->anticipationMisses());
    if (const std::shared_ptr<MeterBank> bank = engine_->meterBank()) {
        st.meterDrops.renderer = static_cast<std::int64_t>(bank->droppedRenderer());
        st.meterDrops.callback = static_cast<std::int64_t>(bank->droppedCallback());
    }
    return st;
}

std::vector<MeterReading> EngineApp::meters() {
    if (!engine_) return {};
    const std::shared_ptr<MeterBank> bank = engine_->meterBank();
    if (!bank) return {};
    const Engine::MeterClock clock = engine_->meterClock();
    return bank->read(clock.epoch, clock.heardStream);
}

std::uint64_t EngineApp::meterActivitySequence() const {
    const auto bank = engine_ ? engine_->meterBank() : nullptr;
    return bank ? bank->activitySequence() : 0;
}

void EngineApp::armMeterActivity(std::uint64_t observed, float minimumPeak) {
    if (engine_)
        if (const auto bank = engine_->meterBank()) bank->armActivity(observed, minimumPeak);
}

Result<TransportInfo> EngineApp::transport(std::string_view action, std::optional<double> seconds) {
    if (!engine_)
        return Error("engine.stopped", "the real-time engine is not running", {},
                     "engine.start first");
    std::optional<std::vector<RecordedInfo>> recorded;
    if (action == "record") {
        const model::Session s = view();
        SUPPLE_TRY(model::validateRecordingPlayrate(s));  // before creating a record folder/file
        const fs::path dir = recordDir();
        std::vector<RecordTarget> targets;
        for (const auto& e : s.tracks) {
            const model::Track& tr = *e.value;
            if (tr.record.armed && tr.record.mode == model::RecordMode::Output)
                return Error("record.output_unsupported", "output recording is not supported yet",
                             "/tracks/" + e.id.str() + "/record/mode",
                             "output recording is not supported yet; use track.set {track: \"" +
                                 e.id.str() + "\", record_mode: \"input\"}");
            if (!tr.record.armed) continue;
            const bool output = tr.type == model::StripType::Master;  // ADR-0057
            if (!output && !tr.record.input) continue;
            const model::Route* in = output ? nullptr : s.routes.find(*tr.record.input);
            if (in && in->from.midiDevice) {  // MIDI (ADR-0016): events, no file
                RecordTarget t;
                t.track = e.id;
                t.midi = RecordTarget::Midi{
                    midiPortMask(engine_->midiPorts(), *in->from.midiDevice),
                    in->midi ? in->midi->fromChannel : 0, in->midi ? in->midi->toChannel : 0};
                targets.push_back(std::move(t));
                continue;
            }
            if (!output && (!in || !in->from.hwChannels)) continue;
            RecordTarget t;
            t.track = e.id;
            t.output = output;
            if (in)
                for (int ch : *in->from.hwChannels) t.inputs.push_back(ch - 1);
            std::string base;
            for (char c : tr.name.empty() ? e.id.str() : tr.name)
                base += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
            for (int n = 1;; ++n) {  // (the writer also creates it exclusively)
                t.file = dir / (base + "-" + std::to_string(n) + ".wav");
                std::error_code ec;
                const bool taken =
                    std::any_of(targets.begin(), targets.end(),
                                [&](const RecordTarget& o) { return o.file == t.file; });
                if (!taken && !fs::exists(t.file, ec) &&
                    !fs::exists(detail::sidecarPath(t.file), ec))
                    break;
            }
            targets.push_back(std::move(t));
        }
        std::vector<fs::path> files;
        for (const RecordTarget& t : targets)
            if (!t.midi) files.push_back(t.file);
        // A recording master's own items are silent where it records: in the
        // punch range, else everywhere (ADR-0057). The plan changes first.
        std::vector<RecordingMaster> masters;
        for (const RecordTarget& t : targets)
            if (t.output) masters.push_back({t.track, !punch_.enabled, punch_.start, punch_.end});
        if (!masters.empty()) {
            engine_->setRecordingMasters(std::move(masters));
            recordingMasters_ = true;
            headChanged();
        }
        if (auto started = engine_->startRecording(std::move(targets)); !started) {
            endMasterRecording();
            return std::move(started).error();
        }
        recordingFiles_ = std::move(files);
        playStart_ = engine_->transport().seconds;
        startPlaying(metronome_.countInBars);
    } else if (action == "play") {
        playStart_ = engine_->transport().seconds;
        beginAutomationPass(playStart_);
        startPlaying(metronome_.countInPlayback ? metronome_.countInBars : 0);
    } else if (action == "pause" || action == "stop") {
        const double stopSeconds = engine_->transport().seconds;
        const bool wasPlaying = engine_->transport().playing;
        engine_->pause();
        if (action == "stop") SUPPLE_TRY(engine_->prepareSeek(playStart_));
        if (engine_->recording()) {
            auto takes = engine_->stopRecording();
            if (!takes) return std::move(takes).error();  // record.busy: still recording
            recordingFiles_.clear();
            const auto before = head().version;
            const bool masters = recordingMasters_;
            if (masters) {
                engine_->setRecordingMasters({});
                recordingMasters_ = false;
            }
            auto committed = commitTakes(*takes);
            // The masters' items play everywhere again (a commit reloads anyway).
            if (masters && head().version == before) headChanged();
            if (!committed) return std::move(committed).error();
            recorded = std::move(*committed);
        }
        if (wasPlaying) SUPPLE_TRY(finishAutomationPass(stopSeconds));
        updateWriteHolds();  // the pass is over: its curves play again
    } else if (action == "seek") {
        SUPPLE_TRY(engine_->prepareSeek(seconds.value_or(0.0)));
        if (!engine_->transport().playing) playStart_ = seconds.value_or(0.0);
    }
    if (action != "get") engine_->awaitTransport();
    engine_->service();
    checkFailures();
    const TransportState t = engine_->transport();
    TransportInfo info;
    info.playing = t.playing;
    info.recording = engine_->recording();
    info.countingIn = t.countingIn;
    info.seconds = t.seconds;
    info.beat = view().timeMap->beatAt(t.seconds);
    info.sample = t.sample;
    info.loop = loop_;
    info.punch = punch_;
    info.recorded = std::move(recorded);
    return info;
}

namespace {

double envelopePosition(const model::Session& s, const model::Envelope& env, double seconds) {
    return env.timebase == model::Timebase::Time ? seconds : s.timeMap->beatAt(seconds);
}

bool writingMode(model::EnvMode m) {
    return m == model::EnvMode::Touch || m == model::EnvMode::Latch ||
           m == model::EnvMode::LatchPreview || m == model::EnvMode::Write;
}

// All point arithmetic uses the envelope's own coordinate domain.
double envelopeAt(const std::vector<model::EnvPoint>& pts, double position, double fallback) {
    if (pts.empty()) return fallback;
    if (position <= pts.front().pos) return pts.front().value;
    if (position >= pts.back().pos) return pts.back().value;
    const auto next =
        std::upper_bound(pts.begin(), pts.end(), position,
                         [](double b, const model::EnvPoint& p) { return b < p.pos; });
    const model::EnvPoint& b = *next;
    const model::EnvPoint& a = *(next - 1);
    if (b.pos <= a.pos) return b.value;
    return a.value + (b.value - a.value) * model::shapeCurve(a.shape, a.tension,
                                                             (position - a.pos) / (b.pos - a.pos));
}

void insertPoint(std::vector<model::EnvPoint>& pts, double position, double value) {
    model::EnvPoint p;
    p.pos = position;
    p.value = value;
    const auto at = std::upper_bound(pts.begin(), pts.end(), position,
                                     [](double b, const model::EnvPoint& q) { return b < q.pos; });
    pts.insert(at, p);
}

// A strip's static value for an envelope's parameter (a write pass that never
// moved the control flattens to it).
double staticValue(const model::Session& s, const model::EnvTarget& t) {
    if (t.object == model::kMaster && t.param == "playrate") return s.project->playrate.rate;
    if (const model::Track* tr = s.tracks.find(t.object))
        return t.param == "volume" ? tr->volumeDb
               : t.param == "pan"  ? tr->pan
                                   : (tr->mute ? 1.0 : 0.0);
    if (const auto* g = s.groups.find(t.object); g && g->vca)
        return t.param == "volume" ? g->vca->volumeDb : (g->vca->mute ? 1.0 : 0.0);
    if (const model::Route* r = s.routes.find(t.object))
        return t.param == "volume" ? r->volumeDb
               : t.param == "pan"  ? r->pan.percent.value_or(0.0)
                                   : (r->mute ? 1.0 : 0.0);
    return 0.0;
}

}  // namespace

void EngineApp::beginAutomationPass(double seconds) {
    autoWrite_.clear();
    const model::Session s = view();
    for (const auto& e : s.envelopes)
        if (e.value->active && e.value->mode == model::EnvMode::Write &&
            e.value->target.object.kind != model::Kind::Fx) {
            AutoWrite w;
            w.startSeconds = w.lastSeconds = seconds;
            w.lastValue = staticValue(s, e.value->target);
            autoWrite_[e.id] = w;
        }
}

bool EngineApp::writesAutomation(model::Id object, const std::string& param) const {
    if (!engine_ || !txn_ || !engine_->transport().playing) return false;
    const model::Session s = view();
    for (const auto& e : s.envelopes)
        if (e.value->target.object == object && e.value->target.param == param && e.value->active &&
            writingMode(e.value->mode))
            return true;
    return false;
}

void EngineApp::recordAutomation(model::Id object, const std::string& param, double value) {
    if (!engine_ || !txn_ || !engine_->transport().playing) return;
    recordAutomationAt(object, param, value, engine_->transport().seconds);
}

void EngineApp::recordAutomationAt(model::Id object, const std::string& param, double value,
                                   double seconds) {
    const model::Session s = view();
    std::optional<model::Id> envId;
    for (const auto& e : s.envelopes)
        if (e.value->target.object == object && e.value->target.param == param && e.value->active &&
            writingMode(e.value->mode))
            envId = e.id;
    if (!envId) return;
    auto it = autoWrite_.find(*envId);
    const bool first = it == autoWrite_.end() || !it->second.touched;
    // Keep capture times in seconds, even if the tempo changes during a pass.
    // A loop wrap restarts the erase window at the current write.
    const double fromSeconds =
        !first && it->second.lastSeconds < seconds ? it->second.lastSeconds : seconds;
    const double startSeconds = it == autoWrite_.end() ? seconds : it->second.startSeconds;
    txn().modify<model::Envelope>(*envId, [&](model::Envelope& env) {
        const double pos = envelopePosition(s, env, seconds);
        const double from = envelopePosition(s, env, fromSeconds);
        const double before = envelopeAt(env.points, pos, staticValue(s, env.target));
        std::erase_if(env.points, [&](const model::EnvPoint& p) {
            return first ? std::abs(p.pos - pos) < 1e-9 : p.pos > from && p.pos <= pos + 1e-9;
        });
        // Preserve the existing 0.001-beat entry guard, converted into the
        // envelope's domain just like the write position and erase window.
        const double beat = s.timeMap->beatAt(seconds);
        if (first && beat > 1e-3)
            insertPoint(env.points, envelopePosition(s, env, s.timeMap->secondsAt(beat - 1e-3)),
                        before);
        insertPoint(env.points, pos, value);
    });
    AutoWrite w;
    w.startSeconds = startSeconds;
    w.lastSeconds = seconds;
    w.lastValue = value;
    w.touched = true;
    w.lastMs = serviceClockMs();
    autoWrite_[*envId] = w;
}

std::vector<model::EnvTarget> EngineApp::writeHolds() const {
    std::vector<model::EnvTarget> holds;
    // Holds belong to a playing pass: a stopped engine holds nothing (a halt
    // also ends the pass that was playing).
    if (!engine_ || !engine_->transport().playing) return holds;
    const std::uint64_t now = serviceClockMs();
    for (const auto& [id, w] : autoWrite_) {
        const model::Envelope* env = head().envelopes.find(id);
        if (!w.touched || !env || !env->active || !writingMode(env->mode)) continue;
        if (env->mode == model::EnvMode::Touch && now >= w.lastMs + kTouchReleaseMs) continue;
        const auto kind = env->target.object.kind;
        const std::string& param = env->target.param;
        if ((kind == model::Kind::Track || kind == model::Kind::Route)
                ? param == "volume" || param == "pan"
                : kind == model::Kind::Group && param == "volume")
            holds.push_back(env->target);
    }
    return holds;
}

void EngineApp::updateWriteHolds() {
    if (engine_ && engine_->setWriteHolds(writeHolds())) headChanged();
}

Status EngineApp::finishAutomationPass(double stopSeconds) {
    std::map<model::Id, AutoWrite> pass;
    pass.swap(autoWrite_);
    const model::Session s = head();
    std::vector<std::pair<model::Id, AutoWrite>> work;
    for (const auto& [id, w] : pass) {
        const model::Envelope* env = s.envelopes.find(id);
        if (!env || stopSeconds <= w.lastSeconds) continue;
        const bool holds = env->mode == model::EnvMode::Latch ||
                           env->mode == model::EnvMode::LatchPreview ||
                           env->mode == model::EnvMode::Write;
        if (holds) work.emplace_back(id, w);
    }
    if (work.empty()) return {};
    auto txnId =
        beginTransaction({cmd::Origin::Type::Internal, "engine", ""}, "Automation written");
    if (!txnId) return std::move(txnId).error();
    for (const auto& [id, w] : work)
        txn().modify<model::Envelope>(id, [&](model::Envelope& env) {
            const double start = envelopePosition(s, env, w.startSeconds);
            const double stop = envelopePosition(s, env, stopSeconds);
            const double from = w.touched ? envelopePosition(s, env, w.lastSeconds) : start - 1e-9;
            std::erase_if(env.points, [&](const model::EnvPoint& p) {
                return p.pos > from && p.pos <= stop + 1e-9;
            });
            if (!w.touched) insertPoint(env.points, start, w.lastValue);
            insertPoint(env.points, stop, w.lastValue);
        });
    return commitTransaction(*txnId);
}

void EngineApp::startPlaying(int countInBars) {
    if (countInBars <= 0) {
        engine_->play();
        return;
    }
    // Whole bars of the meter at the start position, at its tempo.
    const time::TimeMap& tm = *view().timeMap;
    const double beat = tm.beatAt(engine_->transport().seconds);
    const time::MeterChange m = tm.meterAt(tm.barBeat(beat).bar);
    const auto state = playrateState();
    const double rate = state.envelopeValue.value_or(state.requestedRate);
    const double beatSeconds =
        4.0 / static_cast<double>(m.denominator) * 60.0 / (tm.bpmAt(beat) * rate);
    const auto beatFrames =
        static_cast<std::int64_t>(std::llround(beatSeconds * engine_->config().sampleRate));
    engine_->playCountIn(beatFrames * m.numerator * countInBars, beatFrames, m.numerator);
}

void EngineApp::setMetronome(const MetronomeSettings& m) {
    metronome_ = m;
    if (engine_) engine_->setMetronome(m.enabled, m.volumeDb);
}

void EngineApp::setLoop(const TransportRange& loop) {
    loop_ = loop;
    if (engine_) engine_->setLoop(loop.start, loop.end, loop.enabled);
}

namespace {

// A recording's runs on the timeline, and the items and takes they become
// (ADR-0015, ADR-0016).
struct Run {
    std::int64_t from, to, t0;  // file frames [from, to) heard from timeline t0
    std::size_t index;          // into the segments (a MIDI event's run)
};
struct TakePlan {
    std::int64_t fileFrame;  // at the item's start (audio)
    bool whole;              // the run covers the whole item
    std::size_t run;         // into the runs
};
struct ItemPlan {
    std::int64_t start, end;
    std::vector<TakePlan> takes;
};

// Adds run `run`'s recorded MIDI heard in [from, to) to `seq`: notes paired
// from note-on/off (a retrigger ends the previous one; still held at the
// run's end: ended there, or at `to`), other channel messages as controls.
template <class TickOf>
void appendRecorded(midi::Sequence& seq, const RecordedTake& r, std::size_t run, std::int64_t from,
                    std::int64_t to, std::int64_t runEnd, TickOf tick) {
    std::map<std::pair<int, int>, std::pair<std::int64_t, int>> open;
    auto close = [&](std::pair<int, int> key, std::int64_t at, int offVelocity) {
        auto it = open.find(key);
        if (it == open.end()) return;
        const auto [on, velocity] = it->second;
        open.erase(it);
        const auto first = std::max(on, from);
        const auto last = std::min(at, to);
        if (last <= first) return;
        midi::Note n;
        n.start = tick(first);
        n.length = std::max<std::int64_t>(1, tick(last) - n.start);
        n.channel = static_cast<std::uint8_t>(key.first);
        n.pitch = static_cast<std::uint8_t>(key.second);
        n.velocity = static_cast<std::uint8_t>(velocity);
        n.offVelocity = static_cast<std::uint8_t>(offVelocity);
        seq.notes.push_back(n);
    };
    for (const RecordedMidiEvent& ev : r.midi) {
        if (ev.run != run) continue;
        const int status = ev.bytes[0] & 0xf0, ch = ev.bytes[0] & 0x0f;
        const int b1 = ev.bytes[1], b2 = ev.bytes[2];
        if (status == 0x90 && b2 > 0) {
            close({ch, b1}, ev.timeline, 0);
            open[{ch, b1}] = {ev.timeline, b2};
        } else if (status == 0x80 || status == 0x90) {
            close({ch, b1}, ev.timeline, status == 0x80 ? b2 : 0);
        } else if (ev.timeline >= from && ev.timeline < to) {
            midi::Control c;
            c.tick = tick(ev.timeline);
            c.channel = static_cast<std::uint8_t>(ch);
            switch (status) {
                case 0xb0:
                    c.kind = midi::ControlKind::CC;
                    c.number = static_cast<std::uint8_t>(b1);
                    c.value = b2;
                    break;
                case 0xc0:
                    c.kind = midi::ControlKind::ProgramChange;
                    c.value = b1;
                    break;
                case 0xd0:
                    c.kind = midi::ControlKind::ChannelPressure;
                    c.value = b1;
                    break;
                case 0xa0:
                    c.kind = midi::ControlKind::PolyPressure;
                    c.number = static_cast<std::uint8_t>(b1);
                    c.value = b2;
                    break;
                default:  // 0xe0
                    c.kind = midi::ControlKind::PitchBend;
                    c.value = ((b2 << 7) | b1) - 8192;
                    break;
            }
            seq.controls.push_back(c);
        }
    }
    while (!open.empty()) close(open.begin()->first, runEnd, 0);  // still held
}

Status recordingBounds(const model::Session& s, const detail::RecordingInfo& side,
                       std::uint64_t frames, double fileRate = 0) {
    auto invalid = [](std::string path) {
        return Error("record.sidecar", "recording positions exceed the finite timeline range",
                     std::move(path), "restore valid recording positions or skip this file");
    };
    if (!std::isfinite(side.sampleRate) || side.sampleRate < 8000 || side.sampleRate > 384000 ||
        (fileRate > 0 && side.sampleRate != fileRate) || frames > INT64_MAX)
        return invalid("/sample_rate");
    auto run = [&](std::int64_t start, std::uint64_t count, const std::string& path) -> Status {
        const long double end = static_cast<long double>(start) + static_cast<long double>(count);
        if (std::abs(end) > model::kSampleLimit ||
            !model::checkedSamples(
                static_cast<long double>(start) / static_cast<long double>(side.sampleRate),
                side.sampleRate, path))
            return invalid(path);
        // A negative start is valid latency preroll; only its positive end enters the session.
        if (end > 0 && !model::checkedSeconds(
                           s, static_cast<double>(end / static_cast<long double>(side.sampleRate)),
                           path, side.sampleRate))
            return invalid(path);
        return {};
    };
    if (side.segments.empty()) return run(side.startSample, frames, "/start_sample");
    if (!model::checkedSeconds(s, static_cast<double>(side.loopStart) / side.sampleRate, "/loop/0",
                               side.sampleRate) ||
        !model::checkedSeconds(s, static_cast<double>(side.loopEnd) / side.sampleRate, "/loop/1",
                               side.sampleRate))
        return invalid("/loop");
    for (std::size_t i = 0; i < side.segments.size(); ++i) {
        const auto& seg = side.segments[i];
        const auto from = static_cast<std::uint64_t>(seg.fileFrame);
        const auto to =
            i + 1 < side.segments.size()
                ? std::min(frames, static_cast<std::uint64_t>(side.segments[i + 1].fileFrame))
                : frames;
        if (from < to) SUPPLE_TRY(run(seg.timeline, to - from, "/segments/" + std::to_string(i)));
    }
    return {};
}

std::optional<std::vector<RecordingWarning>> recordingWarnings(const RecordedTake& r) {
    if (r.warnings.empty()) return {};
    std::vector<RecordingWarning> out;
    for (const auto& e : r.warnings)
        out.push_back({e.code, e.message, e.path, e.hint, r.track.str(), r.file.string()});
    return out;
}

// midi_overdub (ADR-0016): the recording merges into the MIDI item under its
// first heard position, as a new take (the old one stays for undo and take
// switching). False when there is no such item (a new item is made then).
Result<bool> overdubMidi(model::EditTxn& t, const model::Session& s, const RecordedTake& r,
                         const std::vector<Run>& runs, double rate,
                         std::vector<RecordedInfo>& out) {
    if (runs.empty()) return false;
    const double atBeat = s.timeMap->beatAt(static_cast<double>(runs.front().t0) / rate);
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != r.track || !it.activeTake) continue;
        const model::Take* old = s.takes.find(*it.activeTake);
        if (!old || !old->source || old->source->kind != model::Kind::Midi) continue;
        const model::ItemSpan span = model::itemSpan(s, it);
        const double itemEndBeat = s.timeMap->beatAt(span.endSec);
        if (atBeat < span.startBeat || atBeat >= itemEndBeat) continue;
        midi::Sequence seq = s.midi.find(*old->source)->sequence;
        const double ppq = seq.ppq;
        auto tick = [&](std::int64_t sample) {
            const double beat = s.timeMap->beatAt(static_cast<double>(sample) / rate);
            return std::llround(((beat - span.startBeat) * old->playrate + old->startOffset) * ppq);
        };
        const auto from = static_cast<std::int64_t>(std::llround(span.startSec * rate));
        const auto to = static_cast<std::int64_t>(std::llround(span.endSec * rate));
        for (const Run& run : runs)  // every pass layers onto the same take
            appendRecorded(seq, r, run.index, from, to, run.t0 + (run.to - run.from), tick);
        seq.canonicalize();
        model::MidiSource src;
        src.sequence = std::move(seq);
        const Id mi = t.create(std::move(src));
        model::Take take = *old;
        take.source = mi;
        take.name = (old->name.empty() ? std::string("MIDI") : old->name) + " overdub";
        auto copied = copyTake(s, t, *it.activeTake, std::move(take));
        if (!copied) return copied.error();
        const Id tk = *copied;
        t.modify<model::Item>(e.id, [&](model::Item& x) {
            x.takes.push_back(tk);
            x.activeTake = tk;
        });
        RecordedInfo info;
        info.warnings = recordingWarnings(r);
        info.track = r.track.str();
        info.item = e.id.str();
        info.take = tk.str();
        info.takes = {tk.str()};
        info.seconds = static_cast<double>(r.frames) / rate;
        info.lostFrames = static_cast<std::int64_t>(r.overflowFrames);
        out.push_back(std::move(info));
        return true;
    }
    return false;
}

// MIDI (ADR-0016): a source per take in the project's PPQ from the item start.

Status commitMidiTake(model::EditTxn& t, const model::Session& s, const RecordedTake& r,
                      const std::vector<Run>& runs, const std::vector<ItemPlan>& items, double rate,
                      std::vector<RecordedInfo>& out) {
    const model::Track* tr = s.tracks.find(r.track);
    if (tr && tr->record.mode == model::RecordMode::MidiOverdub) {
        auto overdub = overdubMidi(t, s, r, runs, rate, out);
        if (!overdub) return overdub.error();
        if (*overdub) return {};
    }
    const int ppq = s.project->ppq;
    for (const ItemPlan& ip : items) {
        model::Item item;
        item.track = r.track;
        const double start = static_cast<double>(ip.start) / rate;
        const double end = static_cast<double>(ip.end) / rate;
        model::placeItemSeconds(s, item, start, end);  // in the track's timebase
        const double startBeat = s.timeMap->beatAt(start);
        auto tick = [&](std::int64_t sample) {
            return static_cast<std::int64_t>(std::llround(
                (s.timeMap->beatAt(static_cast<double>(sample) / rate) - startBeat) * ppq));
        };
        RecordedInfo info;
        info.warnings = recordingWarnings(r);
        std::optional<Id> active;
        for (std::size_t k = 0; k < ip.takes.size(); ++k) {
            const Run& run = runs[ip.takes[k].run];
            midi::Sequence seq;
            seq.ppq = ppq;
            seq.length = tick(ip.end);
            appendRecorded(seq, r, run.index, ip.start, ip.end, run.t0 + (run.to - run.from), tick);
            seq.canonicalize();
            model::MidiSource src;
            src.sequence = std::move(seq);
            const Id mi = t.create(std::move(src));
            model::Take take;
            take.source = mi;
            take.name = (tr && !tr->name.empty() ? tr->name : r.track.str()) + " MIDI";
            if (ip.takes.size() > 1) take.name += " pass " + std::to_string(k + 1);
            const Id tk = t.create(std::move(take));
            item.takes.push_back(tk);
            info.takes.push_back(tk.str());
            if (ip.takes[k].whole) active = tk;  // the newest whole pass...
        }
        if (!active) active = item.takes.back();  // ...or the last one
        item.activeTake = active;
        const Id it = t.create(std::move(item));
        info.track = r.track.str();
        info.item = it.str();
        info.take = active->str();
        info.seconds = end - start;
        info.lostFrames = static_cast<std::int64_t>(r.overflowFrames);  // events, for MIDI
        out.push_back(std::move(info));
    }
    return {};
}

}  // namespace

Result<std::vector<RecordedInfo>> EngineApp::commitTakes(const std::vector<RecordedTake>& takes,
                                                         std::string_view label) {
    std::vector<RecordedInfo> out;
    std::vector<std::pair<Id, RecordedTake>> pendingHashes;
    const model::Session initial = view();
    for (const auto& r : takes) {
        if (r.frames <= 0) continue;
        const double rate = r.sampleRate == 0 ? initial.project->sampleRate : r.sampleRate;
        detail::RecordingInfo side{r.track,    r.startSample, rate,
                                   r.segments, r.loop.start,  r.loop.end};
        SUPPLE_TRY(recordingBounds(initial, side, static_cast<std::uint64_t>(r.frames)));
    }
    auto txnId = beginTransaction({cmd::Origin::Type::Internal, "engine", ""}, label);
    if (!txnId) return std::move(txnId).error();
    const model::Session s = view();
    // Relative to the media folder when there is one (a saved project's or the
    // scratch folder), so a first save can move the files and keep the paths.
    const fs::path media = projectDir().empty() ? fs::path() : projectDir() / s.project->mediaDir;
    for (const RecordedTake& r : takes) {
        for (const Error& e : r.warnings) {
            json::Value ev = json::Value::object();
            ev["code"] = e.code;
            ev["message"] = e.message;
            ev["path"] = e.path;
            ev["hint"] = e.hint;
            ev["track"] = r.track.str();
            ev["file"] = r.file.string();
            emit("record.warning", std::move(ev));
        }
        if (!r.error.empty()) {
            json::Value ev = json::Value::object();
            ev["track"] = r.track.str();
            ev["file"] = r.file.string();
            ev["error"] = r.error;
            emit("record.error", std::move(ev));
        }
        const double rate = r.sampleRate > 0 ? r.sampleRate : s.project->sampleRate;
        if (r.frames <= 0 || !s.tracks.find(r.track)) continue;
        // The runs on the timeline (ADR-0015): one per loop pass, or one.
        std::vector<RecordedSegment> segs = r.segments;
        if (segs.empty()) segs.push_back({0, r.startSample});
        std::vector<Run> runs;
        for (std::size_t k = 0; k < segs.size(); ++k) {
            const std::int64_t to =
                std::min(r.frames, k + 1 < segs.size() ? segs[k + 1].fileFrame : r.frames);
            if (to > segs[k].fileFrame)
                runs.push_back({segs[k].fileFrame, to, segs[k].timeline, k});
        }
        const bool passes = runs.size() > 1 && r.loop.end > r.loop.start;
        // Items: the loop range for passes (one take per pass), else one per
        // run; within the punch range and never before the timeline's start
        // (material outside stays in the file, skipped by the take).
        const auto punchFrom = static_cast<std::int64_t>(std::llround(punch_.start * rate));
        const auto punchTo = static_cast<std::int64_t>(std::llround(punch_.end * rate));
        auto clip = [&](std::int64_t& from, std::int64_t& to) {
            if (punch_.enabled) {
                from = std::max(from, punchFrom);
                to = std::min(to, punchTo);
            }
            from = std::max<std::int64_t>(from, 0);
            return to > from;
        };
        std::vector<ItemPlan> items;
        if (passes) {
            ItemPlan ip{r.loop.start, r.loop.end, {}};
            if (clip(ip.start, ip.end)) {
                for (std::size_t k = 0; k < runs.size(); ++k) {
                    const Run& run = runs[k];
                    const std::int64_t t1 = run.t0 + (run.to - run.from);
                    if (t1 <= ip.start || run.t0 >= ip.end) continue;
                    ip.takes.push_back(
                        {run.from + (ip.start - run.t0), run.t0 <= ip.start && t1 >= ip.end, k});
                }
                if (!ip.takes.empty()) items.push_back(std::move(ip));
            }
        } else {
            for (std::size_t k = 0; k < runs.size(); ++k) {
                const Run& run = runs[k];
                ItemPlan ip{run.t0, run.t0 + (run.to - run.from), {}};
                if (!clip(ip.start, ip.end)) continue;
                ip.takes.push_back({run.from + (ip.start - run.t0), true, k});
                items.push_back(std::move(ip));
            }
        }
        if (items.empty()) continue;
        if (r.isMidi) {
            if (!r.midi.empty()) SUPPLE_TRY(commitMidiTake(txn(), s, r, runs, items, rate, out));
            continue;
        }
        // Known hashes may reuse a source; freshly closed files commit now
        // with an empty hash and complete their metadata in the background.
        model::EditTxn& t = txn();
        Id source;
        if (const auto known = model::mediaSourceWithHash(s, r.hash)) {
            source = *known;
        } else {
            model::MediaSource md;
            std::error_code ec;
            const fs::path rel = media.empty() ? fs::path() : fs::relative(r.file, media, ec);
            md.path = !ec && !rel.empty() && *rel.begin() != ".." ? rel.string() : r.file.string();
            md.hash = r.hash;
            md.format = "wav";
            md.sampleRate = rate;  // the device's (resampled to the plan's rate on playback)
            md.channels = 0;
            if (auto head = audio::inspectWavStream(r.file))
                md.channels = static_cast<int>(head->channels);
            md.frames = r.frames;
            source = t.create(std::move(md));
            if (r.hash.empty() && commands_) pendingHashes.emplace_back(source, r);
        }
        for (const ItemPlan& ip : items) {
            model::Item item;
            item.track = r.track;
            const double start = static_cast<double>(ip.start) / rate;
            const double end = static_cast<double>(ip.end) / rate;
            model::placeItemSeconds(s, item, start, end);  // in the track's timebase
            RecordedInfo info;
            info.warnings = recordingWarnings(r);
            std::optional<Id> active;
            for (std::size_t k = 0; k < ip.takes.size(); ++k) {
                model::Take take;
                take.source = source;
                take.startOffset =
                    static_cast<double>(ip.takes[k].fileFrame) / rate;  // < 0: silence first
                take.name = r.file.stem().string();
                if (ip.takes.size() > 1) take.name += " pass " + std::to_string(k + 1);
                const Id tk = t.create(std::move(take));
                item.takes.push_back(tk);
                info.takes.push_back(tk.str());
                if (ip.takes[k].whole) active = tk;  // the newest whole pass...
            }
            if (!active) active = item.takes.back();  // ...or the last one
            item.activeTake = active;
            const Id it = t.create(std::move(item));
            info.track = r.track.str();
            info.item = it.str();
            info.take = active->str();
            info.file = r.file.string();
            info.seconds = end - start;
            info.lostFrames = static_cast<std::int64_t>(r.overflowFrames);
            out.push_back(std::move(info));
        }
    }
    SUPPLE_TRY(commitTransaction(*txnId));
    for (auto& [source, pendingTake] : pendingHashes) {
        // Open only; all reads happen on the worker. The handle survives a
        // first save moving the scratch file before the job gets to run.
        auto input = std::make_shared<std::ifstream>(pendingTake.file, std::ios::binary);
        const model::MediaSource expected = *head().media.find(source);
        pendingTake.sampleRate = expected.sampleRate;
        const auto generation = projectGeneration();
        const auto branch = versions_->branch();
        auto* dispatcher = commands_;
        startJob([this, dispatcher, source, expected, take = std::move(pendingTake), input,
                  generation, branch](supple::StopToken stop) mutable {
            Result<std::string> result =
                Error("file.open", "cannot open recording for hashing", take.file.string(),
                      "restore a readable file and reimport it to retry hashing");
            if (*input) {
                Sha256 digest;
                std::array<std::uint8_t, 65536> buffer{};
                while (!stop.stop_requested() && *input) {
                    input->read(reinterpret_cast<char*>(buffer.data()), buffer.size());
                    digest.update(
                        std::span(buffer.data(), static_cast<std::size_t>(input->gcount())));
                }
                if (!input->eof())
                    result =
                        Error("file.read", "cannot read recording for hashing", take.file.string(),
                              "restore a readable file and reimport it to retry hashing");
                else
                    result = "sha256:" + sha256Hex(digest.finish());
            }
            if (stop.stop_requested()) return;
            dispatcher->post("record.hash",
                             [this, source, expected, take = std::move(take),
                              result = std::move(result), generation, branch]() mutable -> Status {
                                 if (projectGeneration() != generation) return {};
                                 if (!result) return std::move(result).error();
                                 finalizeRecordingHash(source, expected, std::move(take),
                                                       std::move(*result), branch, generation);
                                 return {};
                             });
        });
    }

    return out;
}

// On the model thread: resolve the current location before the worker
// finalizes the sidecar. A save can relocate scratch files while hashing.
void EngineApp::finalizeRecordingHash(Id source, model::MediaSource expected, RecordedTake take,
                                      std::string hash, std::string branch,
                                      std::uint64_t generation) {
    const auto current = versions_->head(branch);
    if (!current || !current->media.find(source)) return;
    const fs::path base = projectDir().empty() ? options_.workdir : projectDir();
    const fs::path path = fs::path(expected.path).is_absolute()
                              ? fs::path(expected.path)
                              : base / current->project->mediaDir / expected.path;
    auto* dispatcher = commands_;
    startJob([this, dispatcher, source, expected, take = std::move(take), hash = std::move(hash),
              branch = std::move(branch), generation, path](supple::StopToken stop) mutable {
        if (stop.stop_requested()) return;
        detail::RecordingInfo side{take.track,    take.startSample, take.sampleRate,
                                   take.segments, take.loop.start,  take.loop.end,
                                   hash};
        Status finalized;
        {
            std::lock_guard lock(recordingFinalizeMutex_);
            std::error_code ec;
            finalized =
                fs::is_regular_file(path, ec)
                    ? detail::writeSidecar(path, side)
                    : Status{Error("file.open", "recording moved or disappeared", path.string(),
                                   "restore the recording WAV at the reported path before "
                                   "finalizing its hash or recovering it")};
        }
        if (stop.stop_requested()) return;
        dispatcher->post(
            "record.finalize",
            [this, source, expected, take = std::move(take), hash = std::move(hash),
             branch = std::move(branch), generation, path,
             finalized = std::move(finalized)]() mutable -> Status {
                if (projectGeneration() != generation) return {};
                const auto latest = versions_->head(branch);
                if (!latest || !latest->media.find(source)) return {};
                const fs::path folder = projectDir().empty() ? options_.workdir : projectDir();
                const fs::path now = fs::path(expected.path).is_absolute()
                                         ? fs::path(expected.path)
                                         : folder / latest->project->mediaDir / expected.path;
                if (now != path) {
                    finalizeRecordingHash(source, expected, std::move(take), std::move(hash),
                                          branch, generation);
                    return {};
                }
                SUPPLE_TRY(finalized);
                if (versions_->updateMediaHash(source, expected, hash, branch)) {
                    json::Value event = json::Value::object();
                    event["version"] = versions_->headVersion(branch);
                    event["branch"] = branch;
                    emit("session.changed", std::move(event));
                }
                return {};
            });
    });
}

// --- recordings (crash recovery) ------------------------------------------------------------

fs::path EngineApp::mediaDir() const {
    return projectDir().empty() ? options_.workdir / "media"
                                : projectDir() / head().project->mediaDir;
}

fs::path EngineApp::projectDir() const {
    return projectPath_.empty() ? options_.scratchDir : projectPath_.parent_path();
}

namespace {
bool within(const fs::path& inner, const fs::path& outer) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(inner, ec), b = fs::weakly_canonical(outer, ec);
    const auto rel = a.lexically_relative(b);
    return !rel.empty() && *rel.begin() != "..";
}
}  // namespace

void EngineApp::clearScratch() {
    const fs::path& scratch = options_.scratchDir;
    // Only an unsaved project's: once saved, its recordings moved beside the
    // file (or, if that failed, are still here and must stay).
    if (!projectPath_.empty() || scratch.empty() || scratch == scratch.root_path()) return;
    if (within(options_.workdir, scratch)) return;
    std::error_code ec;
    fs::remove_all(scratch, ec);
}

// First save: stage and verify copies while the scratch originals remain.
// Failed relocation/publication removes only files this operation created.
Status EngineApp::moveScratchMedia(const fs::path& dest, const std::function<Status()>& publish) {
    std::lock_guard lock(recordingFinalizeMutex_);
    const fs::path from = options_.scratchDir / head().project->mediaDir;
    std::error_code ec;
    if (!fs::exists(from, ec))
        return ec ? Status{Error("project.media_move", ec.message(), {},
                                 "save to a writable project directory with sufficient space and "
                                 "keep all scratch media readable during the save")}
                  : publish();
    if (fs::equivalent(from, dest, ec)) return publish();
    ec.clear();
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status))
            return Error("project.media_move", "scratch media contains a symlink",
                         it->path().string(),
                         "replace the scratch-media symlink with its actual media file, or save "
                         "into another project directory");
        if (fs::is_regular_file(status)) files.push_back(it->path().lexically_relative(from));
    }
    if (ec)
        return Error("project.media_move", ec.message(), from.string(),
                     "save to a writable project directory with sufficient space and keep all "
                     "scratch media readable during the save");
    std::sort(files.begin(), files.end());
    for (const auto& rel : files) {
        const auto status = fs::symlink_status(dest / rel, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return Error("project.media_move", ec.message(), {},
                         "save to a writable project directory with sufficient space and keep all "
                         "scratch media readable during the save");
        if (fs::exists(status))
            return Error("project.media_exists", (dest / rel).string() + " already exists", {},
                         "save into another folder; scratch media is unchanged");
        ec.clear();
    }
    fs::create_directories(dest, ec);
    if (ec)
        return Error("project.media_move", ec.message(), dest.string(),
                     "save to a writable project directory with sufficient space and keep all "
                     "scratch media readable during the save");
    static std::atomic<std::uint64_t> nextStage{0};
    fs::path stage;
    for (;;) {
        stage = dest / (".save-stage-" + std::to_string(clockMs()) + "-" +
                        std::to_string(nextStage.fetch_add(1)));
        if (fs::create_directory(stage, ec)) break;
        if (ec)
            return Error("project.media_move", ec.message(), stage.string(),
                         "save to a writable project directory with sufficient space and keep all "
                         "scratch media readable during the save");
    }
    std::vector<fs::path> placed;
    bool published = false;
    struct Cleanup {
        fs::path stage;
        std::vector<fs::path>& placed;
        bool& published;
        ~Cleanup() {
            std::error_code ignored;
            if (!published)
                for (const auto& path : placed) fs::remove(path, ignored);
            fs::remove_all(stage, ignored);
        }
    } cleanup{stage, placed, published};
    for (const auto& rel : files) {
        const auto staged = stage / rel;
        fs::create_directories(staged.parent_path(), ec);
        if (!ec) fs::copy_file(from / rel, staged, fs::copy_options::none, ec);
        if (ec)
            return Error("project.media_move", ec.message(), (from / rel).string(),
                         "save to a writable project directory with sufficient space and keep all "
                         "scratch media readable during the save");
        auto original = sha256File(from / rel), copied = sha256File(staged);
        if (!original || !copied || *original != *copied)
            return Error("project.media_move", "staged media verification failed", rel.string(),
                         "keep source media unchanged during the save and save into a directory "
                         "with reliable writable storage");
    }
    for (const auto& rel : files) {
        const auto target = dest / rel;
        fs::create_directories(target.parent_path(), ec);
        if (!ec) fs::create_hard_link(stage / rel, target, ec);
        if (ec)
            return Error("project.media_move", ec.message(), target.string(),
                         "save to a writable project directory with sufficient space and keep all "
                         "scratch media readable during the save");
        placed.push_back(target);
    }
    SUPPLE_TRY(publish());
    published = true;
    std::vector<fs::path> emptyDirs;
    for (const auto& rel : files) {
        ec.clear();
        fs::remove(from / rel, ec);
        for (auto parent = (from / rel).parent_path(); parent != from && within(parent, from);
             parent = parent.parent_path())
            emptyDirs.push_back(parent);
    }
    std::sort(emptyDirs.begin(), emptyDirs.end(),
              [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
    for (const auto& dir : emptyDirs) {
        ec.clear();
        fs::remove(dir, ec);
    }
    ec.clear();
    fs::remove(from, ec);
    ec.clear();
    fs::remove(options_.scratchDir, ec);  // only when empty
    return {};
}

namespace {

fs::path comparable(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

}  // namespace

// Only regular files directly in a real rec/ directory: recovery rewrites
// headers, so nothing reached through a link is touched.
bool EngineApp::inRecordDir(const fs::path& file) const {
    std::error_code ec;
    const fs::path dir = recordDir();
    return fs::is_directory(fs::symlink_status(dir, ec)) &&
           fs::is_regular_file(fs::symlink_status(file, ec)) &&
           fs::equivalent(file.parent_path(), dir, ec) && !ec;
}

std::vector<RecoverableInfo> EngineApp::recoverable(std::vector<RecordingWarning>* warnings) const {
    std::vector<RecoverableInfo> out;
    const fs::path dir = recordDir();
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return out;
    // Every file a version uses, resolved the way plans resolve media.
    std::set<fs::path> used;
    const fs::path projectDir = this->projectDir();
    versions_->forEachVersion([&](const model::Version& v) {
        for (const auto& e : v.session->media) {
            const fs::path p = e.value->path;
            if (p.is_absolute()) {
                used.insert(comparable(p));
            } else {
                used.insert(comparable(projectDir / v.session->project->mediaDir / p));
                used.insert(comparable(projectDir / p));
            }
        }
    });
    for (const fs::path& f : recordingFiles_) used.insert(comparable(f));
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".wav") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    const fs::path media = mediaDir();
    for (const fs::path& f : files) {
        if (!inRecordDir(f) || used.count(comparable(f))) continue;
        auto side = detail::readSidecar(f);  // none: not a recording, or capture never began
        if (!side) {
            if (warnings && fs::exists(detail::sidecarPath(f), ec)) {
                const auto& e = side.error();
                warnings->push_back({"record.sidecar", e.message, e.path.empty() ? "/" : e.path,
                                     e.hint.empty()
                                         ? "restore a valid recording sidecar or skip this file"
                                         : e.hint,
                                     "", f.lexically_relative(media).string()});
            }
            continue;
        }
        auto info = audio::inspectWavStream(f);
        if (!info) {
            const auto& e = info.error();
            if (warnings)
                warnings->push_back(
                    {e.code, e.message, e.path.empty() ? "/file" : e.path,
                     e.hint.empty() ? "restore the recording audio or skip this file" : e.hint,
                     side->track.str(), f.lexically_relative(media).string()});
            continue;
        }
        if (info->fileFrames == 0) continue;
        if (auto valid = recordingBounds(head(), *side, info->fileFrames, info->sampleRate);
            !valid) {
            const auto& e = valid.error();
            if (warnings)
                warnings->push_back({e.code, e.message, e.path, e.hint, side->track.str(),
                                     f.lexically_relative(media).string()});
            continue;
        }
        RecoverableInfo r;
        r.file = f.lexically_relative(media).string();
        r.track = side->track.str();
        r.startSeconds = static_cast<double>(side->startSample) / side->sampleRate;
        r.seconds = static_cast<double>(info->fileFrames) / info->sampleRate;
        r.channels = info->channels;
        out.push_back(std::move(r));
    }
    return out;
}

Result<std::vector<RecordedInfo>> EngineApp::recover(const std::vector<std::string>& files,
                                                     std::optional<Id> track,
                                                     std::vector<RecordingWarning>* warnings) {
    std::vector<RecordingWarning> skipped;
    const std::vector<RecoverableInfo> all = recoverable(&skipped);
    if (warnings) warnings->insert(warnings->end(), skipped.begin(), skipped.end());
    auto warn = [&](const Error& e, const fs::path& file, std::string trackName = {}) {
        skipped.push_back({e.code, e.message, e.path, e.hint, std::move(trackName), file.string()});
        if (warnings) warnings->push_back(skipped.back());
    };
    std::vector<const RecoverableInfo*> chosen;
    if (files.empty()) {
        for (const RecoverableInfo& r : all) chosen.push_back(&r);
        if (chosen.empty() && skipped.empty())
            return Error(
                "recover.nothing", "there are no recordings to recover", {},
                "use project.recoverable to list recordings before calling project.recover");
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const fs::path want = fs::path(files[i]).lexically_normal();
        auto it = std::find_if(all.begin(), all.end(),
                               [&](const RecoverableInfo& r) { return fs::path(r.file) == want; });
        if (it == all.end() &&
            std::any_of(skipped.begin(), skipped.end(),
                        [&](const RecordingWarning& w) { return fs::path(w.file) == want; }))
            continue;
        if (it == all.end())
            return Error("recover.unknown_file", files[i] + " is not a recording to recover",
                         "/files/" + std::to_string(i), "project.recoverable lists them");
        if (std::find(chosen.begin(), chosen.end(), &*it) != chosen.end())
            return Error("recover.duplicate_file", files[i] + " is listed twice",
                         "/files/" + std::to_string(i),
                         "remove the duplicate entry from files; include each project.recoverable "
                         "path only once");
        chosen.push_back(&*it);
    }
    const model::Session& s = head();
    std::vector<RecordedTake> takes;
    for (const RecoverableInfo* r : chosen) {
        RecordedTake t;
        t.file = mediaDir() / r->file;
        auto side = detail::readSidecar(t.file);
        if (!side) {
            Error e = side.error();
            e.code = "record.sidecar";
            warn(e, r->file, r->track);
            continue;
        }
        auto info = audio::inspectWavStream(t.file);
        if (!info) {
            warn(info.error(), r->file, r->track);
            continue;
        }
        if (auto valid = recordingBounds(s, *side, info->fileFrames, info->sampleRate); !valid) {
            warn(valid.error(), r->file, r->track);
            continue;
        }
        t.track = track.value_or(side->track);
        if (!s.tracks.find(t.track))
            return Error("recover.no_track",
                         r->file + " was recorded on " + t.track.str() + ", which is gone",
                         "/track", "give a track to put it on");
        // A master's own recordings recover onto it (ADR-0057: audio items).
        if (s.tracks.find(t.track)->type == model::StripType::Buss)
            return Error("track.strip_type", "recover recordings onto a channel or master",
                         "/track", "choose a child channel of the buss instead");
        t.startSample = side->startSample;
        t.sampleRate = side->sampleRate;
        t.segments = side->segments;  // loop passes, when the recording ended normally
        t.loop = {side->loopStart, side->loopEnd};
        takes.push_back(std::move(t));
    }
    for (auto it = takes.begin(); it != takes.end();) {
        RecordedTake& t = *it;
        if (!inRecordDir(t.file))  // replaced since it was listed
            return Error("recover.unknown_file", t.file.string() + " is not a recording to recover",
                         {}, "choose a file listed by project.recoverable for the current project");
        auto frames = audio::repairWavStream(t.file);
        if (!frames) {
            warn(frames.error(), t.file.lexically_relative(mediaDir()), t.track.str());
            it = takes.erase(it);
            continue;
        }
        t.frames = static_cast<std::int64_t>(*frames);
        ++it;
    }
    return commitTakes(takes, "recover");
}

PlayrateState EngineApp::playrateState() const {
    PlayrateState state;
    state.requestedRate = head().project->playrate.rate;
    state.preservePitch = head().project->playrate.preservePitch;
    state.effectiveRate = engine_ ? engine_->effectivePlayrate() : 1.;
    state.envelope = playrateEnvelope_;
    if (state.envelope) {
        const double seconds = engine_ ? engine_->transport().seconds : playStart_;
        const double pos =
            playrateTimebase_ == model::Timebase::Time ? seconds : head().timeMap->beatAt(seconds);
        state.envelopeValue = model::pointsValueAt(playratePoints_, pos, state.requestedRate);
    }
    return state;
}

void EngineApp::endMasterRecording() {
    if (!recordingMasters_) return;
    recordingMasters_ = false;
    if (!engine_) return;
    engine_->setRecordingMasters({});
    headChanged();
}

rack::Instance* EngineApp::moduleInstance(model::Id module) const {
    return pool_->racks().find(module);
}

void EngineApp::setModules(std::shared_ptr<const rack::Catalog> catalog) {
    modules_ = std::move(catalog);
    pool_->racks().setCatalog(modules_);
    headChanged();  // the running engine compiles its racks against it
}

void EngineApp::headChanged() {
    for (const auto& e : head().fx)
        if (model::frozenOwner(head(), e.id)) scratch_.erase(e.value->plugin.id);
    playrateEnvelope_ = model::activePlayrateEnvelope(head());
    playratePoints_.clear();
    if (playrateEnvelope_) {
        playrateTimebase_ = head().envelopes.find(*playrateEnvelope_)->timebase;
        playratePoints_ = model::effectivePoints(head(), *playrateEnvelope_);
    }
    mainWake_.notify();
    if (!engine_) return;
    const model::Session& s = head();
    const double rate = engine_->config().sampleRate;
    const auto& memo = sourceKeyMemo_;
    if (!memo || !memo->items.sameStorage(s.items) || !memo->takes.sameStorage(s.takes) ||
        !memo->media.sameStorage(s.media) || memo->project != s.project ||
        memo->timeMap != s.timeMap || memo->dir != projectDir() || memo->rate != rate) {
        auto key = sourcePreparationKey(s, projectDir(), rate);
        if (!key) {
            preparationError_ = key.error().toString();
            emit("engine.error", preparationError_);
            return;
        }
        sourceKeyMemo_ = SourceKeyMemo{s.items,   s.takes,      s.media, s.project,
                                       s.timeMap, projectDir(), rate,    *key};
    }
    const std::string* sourceKey = &sourceKeyMemo_->key;
    if (!media_.sources() || media_.sources()->key != *sourceKey || !pendingSourceKey_.empty()) {
        if (pendingSourceKey_ != *sourceKey) {
            if (!sourcePreparation_)
                sourcePreparation_ =
                    std::make_unique<SourcePreparation>([this] { mainWake_.notify(); });
            pendingSourceKey_ = *sourceKey;
            preparationError_.clear();
            sourcePreparation_->request(head(), projectDir(), engine_->config().sampleRate);
        }
        return;
    }
    // Returning to a version before the first patch/state edit means defaults,
    // rather than retaining the previous version's live plugin state.
    if (loadedHead_)
        for (const auto& e : head().fx)
            if (!e.value->patch && !e.value->state)
                if (const auto* before = loadedHead_->fx.find(e.id);
                    before && (before->patch || before->state))
                    pool_->restart(e.id);
    (void)engine_->setWriteHolds(writeHolds(), false);  // the load takes them
    auto st = engine_->load(head(), projectDir());
    if (st) loadedHead_ = versions_->head();
    if (!st) emit("engine.error", json::Value(st.error().toString()));
    engine_->service();
}

// --- cmd::Host ------------------------------------------------------------------------------

void EngineApp::attachCommands(cmd::Dispatcher& dispatcher) {
    if (!scripts_) scripts_ = std::make_unique<lua::Runtime>();
    scripts_->setCommands(dispatcher.registry(), dispatcher);
}

cmd::Context EngineApp::makeContext(const cmd::Origin& origin) {
    cmd::Context ctx(origin);
    ctx.provide<EngineApp>(*this);
    if (scripts_) ctx.provide<lua::Runtime>(*scripts_);
    return ctx;
}

Result<std::uint64_t> EngineApp::beginTransaction(const cmd::Origin& origin,
                                                  std::string_view label) {
    if (txn_)
        return Error(
            "history.nested", "a transaction is already open", {},
            "commit or roll back the open transaction before beginning another transaction");
    txn_ = std::make_unique<model::EditTxn>(head(), versions_->nextVersion());
    autoWriteRollback_ = autoWrite_;
    record_ = model::TxnRecord{std::string(label), originText(origin), {}, {}};
    // Scoped by origin, so two clients' gestures never merge into one step.
    if (!origin.gesture.empty()) record_.coalesceKey = record_.origin + "|" + origin.gesture;
    return ++txnId_;
}

Status EngineApp::commitTransaction(std::uint64_t id) {
    if (!txn_ || id != txnId_)
        return Error(
            "history.transaction", "no such open transaction", {},
            "use the id returned by beginTransaction while that transaction is still open");
    // Automation has transient latch/write state as well as model points.
    // Restore both on failure, including internal callers without a dispatcher.
    struct WriterRollback {
        std::map<model::Id, AutoWrite>& writer;
        std::optional<std::map<model::Id, AutoWrite>> before;
        bool committed = false;
        ~WriterRollback() {
            if (!committed && before) writer = std::move(*before);
        }
    } writing{autoWrite_, std::exchange(autoWriteRollback_, std::nullopt)};
    std::unique_ptr<model::EditTxn> t = std::move(txn_);
    mainWake_.notify();  // resume work deferred by this transaction
    auto changes = t->changes();
    if (changes.empty() && !t->projectChanged() && t->created().empty()) {
        writing.committed = true;
        return okStatus();  // nothing to record
    }
    auto created = t->created();
    SUPPLE_TRY(model::validateStripRules(t->preview()));
    auto s = t->commit();
    if (!s) return std::move(s).error();
    if (engine_ && engine_->recording()) SUPPLE_TRY(model::validateRecordingPlayrate(*s));
    auto v = versions_->commit(std::move(*s), std::move(record_), std::move(changes),
                               std::move(created), clockMs());
    if (!v) return std::move(v).error();
    writing.committed = true;
    json::Value ev = json::Value::object();
    ev["version"] = *v;
    ev["branch"] = versions_->branch();
    emit("session.changed", std::move(ev));
    headChanged();
    return okStatus();
}

Status EngineApp::rollbackTransaction(std::uint64_t id) {
    if (id == txnId_) {
        txn_.reset();
        if (autoWriteRollback_) autoWrite_ = std::move(*autoWriteRollback_);
        autoWriteRollback_.reset();
        mainWake_.notify();
    }
    return okStatus();
}

Result<std::function<Status()>> EngineApp::invocationSavepoint() {
    if (!txn_) return std::function<Status()>{};
    auto scriptEvents = scripts_ ? scripts_->eventSavepoint() : std::function<void()>{};
    return std::function<Status()>([this, saved = *txn_, record = record_, writing = autoWrite_,
                                    events = events_,
                                    scriptEvents = std::move(scriptEvents)]() mutable {
        *txn_ = std::move(saved);
        record_ = std::move(record);
        autoWrite_ = std::move(writing);
        events_ = std::move(events);
        if (scriptEvents) scriptEvents();
        return okStatus();
    });
}

void EngineApp::log(const cmd::LogRecord& record) {
    if (record.command == "log.list") return;  // reading the log is not news
    records_.push_back(record);
    while (records_.size() > kMaxRecords) records_.pop_front();
}

void EngineApp::emit(std::string_view topic, json::Value payload) {
    // Scripts see the event once the emitting command has finished
    // (commandFinished), like the clients on the control socket.
    if (scripts_) scripts_->event(topic, payload);
    events_.emplace_back(std::string(topic), std::move(payload));
}

void EngineApp::commandFinished() {
    mainWake_.notify();  // deadlines/FD subscriptions may have changed
    if (clearScripts_ && scripts_ && !scripts_->active()) {
        scripts_->clear();
        clearScripts_ = false;
    }
    if (scripts_ && !clearScripts_) scripts_->deliver();
}

std::vector<std::pair<std::string, json::Value>> EngineApp::takeEvents() {
    return std::exchange(events_, {});
}

bool EngineApp::actionTransactional(std::string_view command) const {
    return command == "render.mix" || command == "render.stems" || command == "render.regions" ||
           command == "branch.render-ab";
}

std::uint64_t EngineApp::clockMs() const {
    if (fixedClock_) return fixedClock_;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void EngineApp::invoked(std::string_view command, int, const json::Value& args, cmd::Kind kind) {
    if (txn_ && kind == cmd::Kind::Edit)
        record_.invocations.push_back({std::string(command), args});
}

void EngineApp::setupFixture(cmd::Dispatcher& d, std::string_view fixture) {
    if (fixture == "empty") return;
    if (fixture == "engine_running") {  // the dummy backend: no device
        auto st = startEngine("dummy", 256);
        assert(st && "fixture engine failed");
        (void)st;
        return;
    }
    if (fixture == "job_running") {
        (void)d.jobs().create();
        return;
    }
    auto run = [&](const char* command, std::string_view args) {
        auto r = d.execute(
            {command, json::parse(args).value(), {cmd::Origin::Type::Test, "fixture", ""}});
        assert(r && "fixture command failed");
        (void)r;
    };
    if (fixture == "engine_armed") {  // + tr_1 armed on inputs 1-2
        run("track.add", R"({"name": "Vocal"})");
        run("track.set", R"({"track": "tr_1", "armed": true, "record_input": [1, 2]})");
        auto st = startEngine("dummy", 256);
        assert(st && "fixture engine failed");
        (void)st;
        return;
    }
    if (fixture == "recording_interrupted") {  // tr_1 "Vocal" + rec/Vocal-1.wav, never committed
        run("track.add", R"({"name": "Vocal"})");
        // One second of stereo from 1 s on; the recording stopped (crashed)
        // half a second after its last header rewrite.
        const fs::path file = recordDir() / "Vocal-1.wav";
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        const double rate = head().project->sampleRate;
        const auto half = static_cast<std::size_t>(rate / 2);
        std::vector<float> frames(2 * half, 0.25f);
        audio::WavStreamWriter w;
        auto st = w.open(file, rate, 2);
        st = st ? w.append(frames.data(), half) : st;
        st = st ? w.updateHeader() : st;
        st = st ? w.append(frames.data(), half) : st;
        st = st ? w.flush() : st;
        w.abandon();
        st = st ? detail::writeSidecar(
                      file,
                      {Id{model::Kind::Track, 1}, static_cast<std::int64_t>(rate), rate, {}, 0, 0})
                : st;
        assert(st && "fixture recording failed");
        (void)st;
        return;
    }
    if (fixture == "song_with_audio" || fixture == "song_split_audio" ||
        fixture == "song_with_prepared_audio" || fixture == "song_with_hits" ||
        fixture == "song_with_warp") {  // tr_1 "Audio" + it_1/tk_1: four beats of a 0.5 tone
        run("track.add", R"({"name": "Audio"})");
        const fs::path file = mediaDir() / "tone.wav";
        audio::AudioData tone;
        tone.sampleRate = head().project->sampleRate;
        tone.channels.assign(2, std::vector<float>(static_cast<std::size_t>(2 * tone.sampleRate)));
        for (std::size_t i = 0; i < tone.frames(); ++i)
            tone.channels[0][i] = tone.channels[1][i] =
                static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 440.0 *
                                                  static_cast<double>(i) / tone.sampleRate));
        const Status st = audio::writeWav(file, tone);
        assert(st && "fixture audio failed");
        (void)st;
        auto add = *json::parse(R"({"track": "tr_1", "position": 0})");
        add["audio_file"] = file.string();  // a Windows path's backslashes need JSON escapes
        run("item.add", json::writeCompact(add));
        if (fixture == "song_with_hits")
            run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
        if (fixture == "song_split_audio") run("item.split", R"({"item":"it_1","at":2})");
        if (fixture == "song_with_warp")
            run("take.stretch.add", R"({"take":"tk_1","source_pos":0.3,"item_pos":0.25})");
        if (fixture == "song_with_prepared_audio") {
            auto prepared = media_.prepareSources(head(), projectDir(), head().project->sampleRate);
            assert(prepared && "fixture source preparation failed");
            (void)prepared;
        }
        return;
    }
    // gain/gain.cpp in the DSP script library; dsp_player: and the script
    // player on the plugin path (the harness gives it); dsp_player_editor:
    // a player built with its editor.
    if (fixture == "dsp_script" || fixture.starts_with("dsp_player")) {
        const Status st =
            writeFileAtomic(scriptDir() / "gain" / "gain.cpp", R"(#include <supple/script.h>

struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain"};
    Param gain = decibels("Gain", -24, 24, 0);

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= gain.gain();
    }
};
)");
        assert(st && "fixture script failed");
        (void)st;
        return;
    }
    if (fixture == "meter_change") {  // 3/4 from bar 9
        run("timesig.set", R"({"bar": 9, "meter": "3/4"})");
        return;
    }
    if (fixture == "render_preset") {  // a user render preset, "Archive"
        run("render.preset.save",
            R"({"name": "Archive", "settings": {"outputs": [{"format": "flac"}]}})");
        return;
    }
    if (fixture == "saved") {
        run("project.set", R"({"name": "Saved"})");
        (void)save("example-project.json");
        return;
    }
    run("track.add", R"({"name": "Bass"})");
    if (fixture == "song") return;
    if (fixture == "song_with_rack" || fixture == "song_with_rack_preset" ||
        fixture == "song_with_rack_learn" || fixture == "song_with_rack_clip") {
        // + rk_1 on tr_1: mo_1 Out (tr_1's input), mo_2 Notes, mo_3 VCA; cb_1 mo_3.out -> mo_1.l
        run("rack.add", R"({"track": "tr_1"})");
        run("module.add", R"({"rack": "rk_1", "type": "notes", "x": 40, "y": 40})");
        run("module.add", R"({"rack": "rk_1", "type": "vca", "x": 300, "y": 40})");
        run("cable.add", R"({"from": "mo_3.out", "to": "mo_1.l"})");
        if (fixture == "song_with_rack") return;
        if (fixture == "song_with_rack_clip") {
            run("module.copy", R"({"modules": ["mo_3", "mo_1"]})");
            return;
        }
        if (fixture == "song_with_rack_learn") {  // + mo_3's level on CC 21, channel 1
            run("module.learn.set",
                R"({"module": "mo_3", "param": "level", "midi": {"kind": "cc", "channel": 1, "number": 21}})");
            return;
        }
        // + the rack saved as the preset "Gated VCA"; tr_2 "Lead" with rk_2 (mo_4 its Out)
        run("rack.preset.save", R"({"rack": "rk_1", "name": "Gated VCA"})");
        run("track.add", R"({"name": "Lead"})");
        run("rack.add", R"({"track": "tr_2"})");
        return;
    }
    if (fixture == "song_with_module") {  // + wobble in the module library
        run("rack.add", R"({"track": "tr_1"})");
        json::Value args = json::Value::object();
        args["name"] = "wobble";
        args["source"] = std::string(detail::kExampleModule);
        auto r =
            d.execute({"module.write", std::move(args), {cmd::Origin::Type::Test, "fixture", ""}});
        assert(r && "fixture command failed");
        (void)r;
        return;
    }
    if (fixture == "song_with_group" || fixture == "song_with_vca") {
        run("group.add", R"({"name":"Drums","members":["tr_1"]})");
        if (fixture == "song_with_vca") run("vca.add", R"({"group":"gr_1"})");
        return;
    }
    if (fixture == "song_with_project_template") {
        run("project.template.save", R"({"name":"Band"})");
        return;
    }
    if (fixture == "song_with_chain" || fixture == "song_with_saved_chain" ||
        fixture == "song_with_saved_template") {
        run("fx.add",
            R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-6}}})");
        run("fx.add",
            R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-12}}})");
        if (fixture == "song_with_saved_chain")
            run("fx.chain.save", R"({"track":"tr_1","name":"Bass"})");
        if (fixture == "song_with_saved_template")
            run("track.template.save", R"({"track":"tr_1","name":"Bass"})");
        return;
    }
    if (fixture == "song_with_keymix" || fixture == "song_with_keymix_4ch") {
        // + tr_2 "Keyed" with fx_1, a key mix (ADR-0018); 4ch: tr_2 has channels 3/4 (ADR-0020)
        run("track.add", R"({"name": "Keyed"})");
        run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.keymix"})");
        if (fixture == "song_with_keymix_4ch")
            run("track.set", R"({"track": "tr_2", "channels": 4})");
        return;
    }
    if (fixture == "song_with_route" || fixture == "song_with_mix_learn") {
        run("route.add", R"({"from":"tr_1","to":"tr_0"})");
        // + tr_1 volume on CC 7, sd_1 volume on CC 12, gr_1's VCA volume on CC 9
        if (fixture == "song_with_mix_learn") {
            run("track.learn.set",
                R"({"track": "tr_1", "param": "volume", "midi": {"kind": "cc", "channel": 1, "number": 7}})");
            run("route.learn.set",
                R"({"route": "sd_1", "param": "volume", "midi": {"kind": "cc", "channel": 1, "number": 12}})");
            run("group.add", R"({"name": "Drums", "members": ["tr_1"]})");
            run("vca.add", R"({"group": "gr_1"})");
            run("vca.learn.set",
                R"({"group": "gr_1", "param": "volume", "midi": {"kind": "cc", "channel": 1, "number": 9}})");
        }
        return;
    }
    if (fixture == "song_with_marker") {
        run("marker.add", R"({"pos":4,"name":"Verse"})");
        return;
    }
    if (fixture == "branch_ahead") {  // an agent branch one edit ahead of main, main current
        run("branch.create", R"({"name": "agent/claude/darker"})");
        run("track.set", R"({"track": "tr_1", "volume_db": -3})");
        run("branch.switch", R"({"name": "main"})");
        return;
    }
    if (fixture == "song_undone") {
        run("track.set", R"({"track": "tr_1", "volume_db": -3})");
        (void)versions_->undo();
        return;
    }
    run("item.add",
        R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q r:q C2:q r:q"})");
    if (fixture == "song_with_item") return;
    if (fixture == "song_with_takes") {  // it_1 with three takes, tk_1..tk_3 (tk_3 heard)
        run("take.add", R"({"item":"it_1","from":"tk_1"})");
        run("take.add", R"({"item":"it_1","from":"tk_1"})");
        run("take.activate", R"({"item":"it_1","take":"tk_3"})");
        return;
    }
    if (fixture == "song_with_region") {
        run("region.add", R"({"start":0,"end":4,"name":"Verse"})");
        return;
    }
    run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain"})");
    if (fixture == "song_with_learn")  // + fx_2 gain on CC 21, channel 1
        run("fx.learn.set",
            R"({"fx": "fx_2", "param": "gain", "midi": {"kind": "cc", "channel": 1, "number": 21}})");
    if (fixture == "song_with_env" || fixture == "song_with_autoitem")  // + en_1: fx_2 gain
        run("env.add",
            R"({"fx": "fx_2", "param": "gain", "points": [{"beat": 0, "value": -12}, {"beat": 4, "value": 0}]})");
    if (fixture == "song_with_autoitem")  // + ai_1 on en_1: beats 4-8, a two-beat ramp, looped
        run("autoitem.add",
            R"({"env": "en_1", "position": 4, "length": 4, "source_length": 2, "points": [{"beat": 0, "value": -6}, {"beat": 2, "value": 0}]})");
    if (fixture == "song_with_preset") {  // + the user preset "Quiet" for the gain, at -12 dB
        run("fx.patch.merge", R"({"fx": "fx_2", "patch": {"params": {"gain": -12}}})");
        run("fx.preset.save", R"({"fx": "fx_2", "name": "Quiet"})");
    }
    if (fixture == "song_frozen") run("track.freeze", R"({"track":"tr_1"})");
    if (fixture == "song_with_render") run("render.mix", R"({"name":"example"})");
    if (fixture == "render_queued")  // + q1: the mix of the first beat
        run("render.queue.add",
            R"({"command": "render.mix", "args": {"end_beat": 1, "name": "queued"}})");
}

}  // namespace supple::engine
