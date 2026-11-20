// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/app.h"
#include "audition_loader.h"
#include "file_platform.h"
#include "media_files.h"
#include "module_library.h"
#include "rack_runtime.h"
#include "runtime_protection.h"
#include "session_lock.h"
#include "source_preparation.h"
#include "supple/model/freeze.h"
#include "supple/model/numeric.h"
#include "suspects.h"

#if !defined(_WIN32)
#include <poll.h>
#endif
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
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
#include "supple/diagnostics/reports.h"
#include "supple/dsp/oversampler.h"
#include "supple/engine/jack.h"
#include "supple/model/env_shape.h"
#include "supple/model/project_file.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

#include "../../dsp-commands/script_library.h"
#include "meters.h"
#include "recording.h"

namespace supple::engine {

namespace fs = std::filesystem;
using model::Id;

namespace {
// The engine's own commands, for checking a file's logged steps before any
// dispatcher exists (open() may run without one).
const cmd::Registry& engineCommands() {
    static const cmd::Registry* registry = [] {
        auto* r = new cmd::Registry;  // never destroyed: a static's exit order is unknown
        registerEngineCommands(*r);
        return r;
    }();
    return *registry;
}

// Every project file the engine reads (opened, autosaves listed or
// recovered) checks its saved steps with the same rule (ADR-0076).
Status replayableStep(const model::Invocation& step) {
    return EngineApp::replayableFromFile(engineCommands(), step);
}

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
    if (const auto& fixed = app->options_.syncLocateSeconds)  // ADR-0071 §4
        app->syncSeekSeconds_ = std::clamp(*fixed, 0.0, 0.5);
    if (!app->options_.midiPreferencesFile.empty()) {
        std::error_code existsError;
        if (fs::exists(app->options_.midiPreferencesFile, existsError)) {
            auto bytes = readFile(app->options_.midiPreferencesFile);
            if (!bytes) return bytes.error();
            auto doc = json::parse(
                std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            if (!doc) return doc.error();
            for (bool output : {false, true})
                if (const auto* value = doc->find(output ? "midi_outputs" : "midi_inputs")) {
                    (output ? app->midiOutputsPreferences_ : app->midiInputsPreferences_) =
                        loadMidiPortPreferences(*value, output,
                                                app->midiPreferencesWarnings_[output ? 1 : 0]);
                }
        }
    }
    dsp_commands::nameScriptLibrary(app->scriptDir());
    if (!app->serviceWaitable().valid())
        return Error("engine.service_wake", "cannot create the main-thread service descriptor", {},
                     "close unused files/descriptors and retry creating the engine app");
    EngineApp* self = app.get();
    app->pool_ = std::make_unique<InstancePool>(
        [self](const model::PluginRef& ref) { return self->findPlugin(ref.id); },
        app->options_.identity);
    app->pool_->setMainThreadWake(app->mainWake_);
    app->pool_->setDefaultIsolation(app->pluginIsolation_);
    app->pool_->setOverrides(app->overrides_);
    app->modules_ = detail::loadModuleLibrary(detail::builtinModuleCatalog(), app->moduleDir());
    app->pool_->racks().setCatalog(app->modules_);
    SUPPLE_TRY(app->newProject(std::move(project)));
    app->pruneEmptySessions();
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

Result<host::PluginInfo> EngineApp::lookupPlugin(std::string_view idOrName) {
    auto all = plugins();
    if (!all) return std::move(all).error();
    return findPlugin(*all, idOrName);
}

Result<host::PluginInfo> EngineApp::findPlugin(std::string_view idOrName) {
    auto info = lookupPlugin(idOrName);
    if (info && overrides_.mode(*info) == LoadMode::Disable) return LoadOverrides::disabled(*info);
    return info;
}

LoadMode EngineApp::loadModeOf(const model::PluginRef& ref) const {
    if (overrides_.empty()) return LoadMode::Normal;
    if (const auto direct = overrides_.mode(ref.format, ref.id); direct != LoadMode::Normal)
        return direct;
    if (catalogue_)
        if (auto info = findPlugin(std::span<const host::PluginInfo>(*catalogue_), ref.id))
            return overrides_.mode(*info);
    return LoadMode::Normal;
}

Result<std::unique_ptr<host::Instance>> EngineApp::makeInstance(const host::PluginInfo& info) {
    const LoadMode mode = overrides_.mode(info);
    if (mode == LoadMode::Disable) return LoadOverrides::disabled(info);
    auto made = host::Instance::create(info, options_.identity, mode == LoadMode::Bridge);
    if (made) ++instancesMade_;
    return made;
}

Result<bool> EngineApp::setLoadOverride(const host::PluginInfo& info, LoadMode mode) {
    if (mode == LoadMode::Bridge) {
        if (host::bridgeExecutable().empty())
            return Error("plugin.override_unavailable",
                         "no plug-in bridge executable was found, so \"" + info.name +
                             "\" cannot be bridged",
                         "/mode",
                         "reinstall so that the bridge sits beside the app, or use mode "
                         "\"disable\" or \"normal\"");
        if (pool_->isStock(info))
            return Error("plugin.override_unavailable",
                         "\"" + info.name + "\" is a stock plug-in; stock plug-ins are not bridged",
                         "/mode", "use mode \"disable\" or \"normal\"");
    }
    if (!overrides_.set(info.format, info.id, mode)) return false;
    ++overridesVersion_;
    pool_->setOverrides(overrides_);
    // A probe made before is of the wrong kind now (in process, or bridged).
    std::erase_if(scratch_, [&](const auto& entry) {
        return entry.second && entry.second->info().format == info.format &&
               entry.second->info().id == info.id;
    });
    pool_->restartPlugin(info.format, info.id);
    for (const auto& entry : head().fx) {
        const auto& ref = entry.value->plugin;
        const bool same =
            ref.id == info.id || (catalogue_ && [&] {
                auto found = findPlugin(std::span<const host::PluginInfo>(*catalogue_), ref.id);
                return found && found->id == info.id && found->format == info.format;
            }());
        if (same) reportedFailures_.erase(entry.id);
    }
    headChanged();  // a running engine plays fresh instances from its next plan
    return true;
}

Result<host::PluginInfo> EngineApp::findPlugin(std::span<const host::PluginInfo> catalogue,
                                               std::string_view idOrName) {
    for (const auto& p : catalogue)
        if (p.id == idOrName) return p;
    for (const auto& p : catalogue)
        if (iequals(p.id, idOrName) || iequals(p.name, idOrName)) return p;
    std::vector<std::string_view> ids;
    for (const auto& p : catalogue) ids.push_back(p.id);
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
        return scratch(*info, fx.plugin.id);
    }
    return slot.get();
}

Result<host::Instance*> EngineApp::scratch(const host::PluginInfo& info, std::string key) {
    auto& slot = scratch_[key];
    if (!slot) {
        auto made = makeInstance(info);
        if (!made) {
            scratch_.erase(key);
            return std::move(made).error();
        }
        slot = std::move(*made);
        slot->setMainThreadWake(mainWake_);
    }
    return slot.get();
}

Result<EngineApp::Probe> EngineApp::probeFor(const host::PluginInfo& info,
                                             const model::FxInstance& fx) {
    Probe probe;
    if (overrides_.mode(info) == LoadMode::Bridge && (fx.patch || fx.state)) {
        auto borrowed = scratch(info, fx.plugin.id);
        if (!borrowed) return std::move(borrowed).error();
        probe.use = *borrowed;
        return probe;
    }
    auto made = makeInstance(info);
    if (!made) return std::move(made).error();
    probe.own = std::move(*made);
    probe.use = probe.own.get();
    probe.use->setMainThreadWake(mainWake_);
    return probe;
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

Result<bool> EngineApp::fxOutputsNotes(model::Id id, const model::FxInstance& fx) {
    if (const host::Instance* live = instanceOf(id)) return live->outputsNotes();
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->outputsNotes();
}

Result<bool> EngineApp::fxAcceptsNotes(model::Id id, const model::FxInstance& fx) {
    if (const host::Instance* live = instanceOf(id)) return live->acceptsNotes();
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->acceptsNotes();
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
    if (const auto home = model::frozenRackHome(view(), id))  // a frozen home's rack (ADR-0075)
        return model::frozenError(view(), *home, "/fx");
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
    auto probe = probeFor(*info, *fx);
    if (!probe) return probe.error();
    auto& instance = *probe->use;
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

Status EngineApp::validatePluginMod(model::Id id, const model::FxInstance& fx,
                                    const std::string& key, const std::string& path,
                                    std::shared_ptr<const model::Blob> blob,
                                    PluginModValidation* validation) {
    auto check = [&](host::Instance& instance) -> Status {
        json::Value schema;
        if (instance.info().semantic) {
            auto described = instance.semanticSchema();
            if (!described) return std::move(described).error().at(path);
            schema = std::move(*described);
        }
        auto target = detail::resolvePluginMod(instance.params(), &schema, key,
                                               instance.info().format == "vst3");
        if (!target) return std::move(target).error().at(path);
        return {};
    };
    if (!validation)
        if (auto* live = instanceOf(id)) return check(*live);
    // The fx's own state and parameters, put on `probe` (which must then be checked at once).
    auto prepare = [&](host::Instance& probe) -> Status {
        auto state = validation ? validation->session : view();
        if (blob && fx.state) state.blobs[*fx.state] = blob;
        SUPPLE_TRY(restoreFx(probe, fx, state));
        if (!fx.params.empty()) {
            if (probe.active()) probe.deactivate();  // a borrowed probe may have been used
            SUPPLE_TRY(probe.activate(state.project->sampleRate, 1, 1));
            probe.flushParams();
        }
        return {};
    };
    auto makeProbe = [&]() -> Result<std::unique_ptr<host::Instance>> {
        auto info = findPlugin(fx.plugin.id);
        if (!info) return std::move(info).error();
        auto probe = makeInstance(*info);
        if (!probe) return std::move(probe).error();
        SUPPLE_TRY(prepare(**probe));
        return std::move(*probe);
    };
    if (validation) {
        // Under a bridge override a probe is a process: a load with many cables to the same
        // plug-in must not start one per fx. A fx with state borrows the plug-in's one cached
        // scratch probe and restores its own state onto it for each check; one with none (and
        // no parameters) shares a pristine probe of the load.
        if (auto info = findPlugin(fx.plugin.id);
            info && overrides_.mode(*info) == LoadMode::Bridge) {
            const std::string pluginKey = fx.plugin.id;
            if (const auto failed = validation->failures.find(&fx);
                failed != validation->failures.end())
                return Error(failed->second).at(path);
            const bool stateful = fx.patch || fx.state;
            if (stateful || fx.params.empty()) {
                host::Instance* use = nullptr;
                if (stateful) {
                    auto borrowed = scratch(*info, pluginKey);
                    if (!borrowed) return std::move(borrowed).error().at(path);
                    use = *borrowed;
                    if (validation->restoredFor[pluginKey] != &fx) {
                        validation->restoredFor[pluginKey] =
                            nullptr;  // unknown while it is half done
                        if (Status st = prepare(*use); !st) {
                            validation->failures.emplace(&fx, st.error());
                            return std::move(st).error().at(path);
                        }
                        validation->restoredFor[pluginKey] = &fx;
                    }
                } else {
                    auto& slot = validation->pristine[pluginKey];
                    if (!slot) {
                        auto made = makeInstance(*info);
                        if (!made) {
                            validation->failures.emplace(&fx, made.error());
                            return std::move(made).error().at(path);
                        }
                        slot = std::move(*made);
                    }
                    use = slot.get();
                }
                return check(*use);
            }
        }
        auto found = validation->probes.find(&fx);
        if (found == validation->probes.end())
            found = validation->probes.emplace(&fx, makeProbe()).first;
        if (!found->second) return Error(found->second.error()).at(path);
        return check(**found->second);
    }
    auto probe = makeProbe();
    if (!probe) return std::move(probe).error().at(path);
    return check(**probe);
}

Result<std::uint32_t> EngineApp::fxLatencySamples(model::Id id, const model::FxInstance& fx,
                                                  bool compensated) {
    if (auto owner = model::frozenOwner(view(), id))
        return model::frozenError(view(), *owner, "/fx");
    // A running instance already knows the plan's rate and block. Otherwise
    // activate a throwaway instance at the project rate: a plugin reports its
    // latency in samples, so a rate is needed even when nothing is playing.
    if (compensated && fx.offline) return 0;
    const int os =
        compensated && (fx.oversampling == 2 || fx.oversampling == 4 || fx.oversampling == 8)
            ? fx.oversampling
            : 1;
    const auto measured = [&](const host::Instance& instance) -> std::uint32_t {
        if (os == 1) return instance.latency();
        dsp::HalfbandOversampler filters;
        filters.prepare(os);
        const auto trip = filters.latency() * static_cast<std::uint32_t>(os);
        const std::uint64_t high = std::uint64_t(instance.latency()) +
                                   (instance.audioInputChannels() == 0 ? trip / 2 : trip);
        return static_cast<std::uint32_t>((high + os - 1) / os);
    };
    if (host::Instance* live = instanceOf(id); live && live->active()) return measured(*live);
    auto info = findPlugin(fx.plugin.id);
    if (!info) return std::move(info).error();
    auto lease = probeFor(*info, fx);
    if (!lease) return std::move(lease).error();
    host::Instance& probe = *lease->use;
    SUPPLE_TRY(restoreFx(probe, fx, view()));
    constexpr std::uint32_t kProbeBlock = 512;  // OfflineSettings' default block
    const auto rate =
        compensated && engine_ ? engine_->config().sampleRate : head().project->sampleRate;
    SUPPLE_TRY(probe.activate(rate * os, 1, kProbeBlock * os));
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
    const std::uint32_t latency = measured(probe);
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
    return replaceProject(model::VersionStore(std::move(session)), std::move(path), prepare,
                          warnings);
}

Status EngineApp::replaceProject(model::VersionStore store, fs::path path,
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
    stopAudition();  // a file audition belongs to the old project's media folder (ADR-0094)
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
        processFaultBase_.reset();
        faultWatch_.clear();
        backend_ = nullptr;
    }
    if (versions_ && projectPath_.empty()) clearScratch();
    loadedHead_.reset();
    media_ = MediaCache{};
    scratch_.clear();
    reportedFailures_.clear();
    recordingFiles_.clear();
    recordingErrorsReported_.clear();
    // Not saved: a saved project's journals stay, offered on its next open;
    // an unsaved one's went with its folder above (ADR-0078, ADR-0080).
    recordingJournalsPending_.clear();
    unpublishedCopies_.clear();
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
        for (const auto& m : store.head()->modules) {
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
    harvestRan();  // what this project ran in process counts for the run
    pool_ = std::make_unique<InstancePool>(
        [this](const model::PluginRef& ref) { return findPlugin(ref.id); }, options_.identity);
    pool_->setMainThreadWake(mainWake_);
    pool_->setDefaultIsolation(pluginIsolation_);
    pool_->setOverrides(overrides_);
    pool_->racks().setCatalog(modules_);
    pendingDirtyStates_.clear();
    if (scripts_) {
        clearScripts_ = scripts_->active();
        if (!clearScripts_) scripts_->clear();
    }
    versions_ = std::make_unique<model::VersionStore>(std::move(store));
    projectPath_ = std::move(path);
    openedFile_.reset();
    autosavedThrough_.reset();  // open() sets the file's
    if (std::error_code ec; !projectPath_.empty())
        if (const auto time = fs::last_write_time(projectPath_, ec); !ec)
            openedFile_.emplace(OpenedFile{projectPath_, time, std::nullopt});
    branchesBackupFor_.clear();
    recoveryKeep_.clear();
    autosaveMarked_ = !autosaveDir().empty();
    autosaveMs_ = serviceClockMs();
    autosaveHeads_ = versions_->heads();
    const Status prepared = prepare ? prepare() : Status{};
    if (projectPath_.empty()) holdScratch();  // an unsaved project's folder is this run's
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

namespace {
// autosave-<n>.json's n (defined with the autosave code below).
std::optional<std::uint64_t> autosaveIndex(std::string_view name);

// Whether two spellings name one file, as the file system says: through
// `.`, links or a case-insensitive file system. Spelling alone decides
// nothing: a/link/../b is not a/b when link points into another folder.
// A path where no file is names the same file as no other path:
// equivalent() is then false, or reports an error, which is taken as no.
bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

bool holds(const std::vector<fs::path>& files, const fs::path& file) {
    return std::any_of(files.begin(), files.end(),
                       [&](const fs::path& f) { return sameFile(f, file); });
}
}  // namespace

Status EngineApp::save(const fs::path& where, std::vector<schema::Warning>* warnings,
                       bool history) {
    const fs::path path = resolve(where);
    if (!txn_) commitLearned(true);  // a control just moved: its values in the file
    // What running plug-ins hold now (a sampler's loaded files), as hosts
    // save it, whether or not they said it changed.
    std::vector<model::Id> running;
    for (const auto& e : head().fx) running.push_back(e.id);
    captureFxStates(running, warnings);
    SUPPLE_TRY(model::checkInvariants(head()));
    SUPPLE_TRY(checkpointRecordingJournals());
    const bool relocated = !projectPath_.empty() || !options_.scratchDir.empty();
    const bool fromScratch = projectPath_.empty() && !options_.scratchDir.empty();
    if (fromScratch && engine_ && engine_->recording())
        return Error("project.recording", "a recording is running", {},
                     "stop it before the project's first save (its files move then)");
    // The file holds the work of every autosave already beside it (and of
    // those this project numbered before them): one numbered above that is
    // newer than it (ADR-0078). File times decide nothing here.
    std::uint64_t through = 0;
    {
        std::error_code ec;
        const fs::path data = path.parent_path() / (path.filename().string() + "-data");
        for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec))
            if (const auto n = autosaveIndex(it->path().filename().string()))
                through = std::max(through, *n);
        // A folder it cannot read may hold autosaves above what it read: a
        // count short of them would offer them over this file once the folder
        // reads again. No folder (or a file in its place) holds none.
        if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory)
            return Error("project.autosave_folder",
                         "could not read the autosave folder beside the file: " + ec.message(),
                         data.string(),
                         "make the folder readable (or move it aside), then save again; or save "
                         "to another file");
        if (path == projectPath_ && autosavedThrough_)
            through = std::max(through, *autosavedThrough_);
    }
    auto publish = [&]() -> Status {
        // This file's branches could not be restored when it was opened:
        // keep it before writing main alone over it.
        std::error_code ec;
        if (!branchesBackupFor_.empty() && fs::exists(path, ec) &&
            fs::equivalent(path, branchesBackupFor_, ec)) {
            const std::string name = path.filename().string();
            // The only copy of those branches: written like a save (temporary
            // file, fsync, then published) before the file is replaced, so a
            // power loss leaves it whole or absent. Published only under a
            // free name, atomically: another app that opened the same file
            // may be keeping its own copy under the same name right now.
            auto bytes = readFile(path, std::size_t(1) << 30);
            fs::path backup;
            Status kept = bytes ? Status(Error("file.exists", "")) : Status(bytes.error());
            for (int n = 1; bytes && !kept && kept.error().code == "file.exists" && n < 10000;
                 ++n) {
                backup = path.parent_path() /
                         (n == 1 ? name + ".pre-branches.bak"
                                 : name + ".pre-branches-" + std::to_string(n) + ".bak");
                if (fs::exists(backup, ec)) continue;
                kept = writeFileNew(backup, *bytes);
            }
            if (!kept)
                return Error("project.history_backup",
                             "could not keep a copy of the file before saving over its unreadable "
                             "branches: " +
                                 kept.error().message,
                             backup.string(),
                             "make the project folder writable, or save to another file");
            if (warnings)
                warnings->push_back(
                    {"project.history_backup",
                     "kept the file with its unreadable branches as " + backup.filename().string(),
                     backup.string()});
            branchesBackupFor_.clear();
        }
        SUPPLE_TRY(publishRecoveryKeeps(path, warnings));
        auto written =
            writeFileAtomic(path, model::serializeProjectFile(*versions_, history, through));
        if (!written && written.error().code != "file.sync_published") return written;
        auto confirmed = written ? syncFile(path) : written;
        if (confirmed) confirmed = syncParentDirectories(path);
        if (!confirmed)
            return Status{
                Error("project.sync", confirmed.error().message, path.string(),
                      "the project may already exist; originals and copied media are kept; "
                      "check storage before retrying the save")};
        return okStatus();
    };
    const fs::path media = path.parent_path() / head().project->mediaDir;
    const fs::path oldMedia = mediaDir();
    std::error_code sameError;
    const bool sameMedia =
        media == oldMedia || (fs::exists(media, sameError) && fs::exists(oldMedia, sameError) &&
                              fs::equivalent(media, oldMedia, sameError));
    // A recording writes into the project's media folder until it stops: a
    // save as into another folder would copy its first part while the rest
    // goes on into the old one. As a first save, it waits for the stop.
    if (!fromScratch && !projectPath_.empty() && !sameMedia && engine_ && engine_->recording())
        return Error("project.recording", "a recording is running", {},
                     "stop it before saving as into another folder (its files are copied then), "
                     "or save in place");
    std::vector<fs::path> createdMedia;  // what this save created in the new media folder
    if (fromScratch) {
        SUPPLE_TRY(moveScratchMedia(media, publish));
    } else {
        // Saved as into another folder: that folder gets its own copy of the
        // media (the old project keeps its files).
        if (!projectPath_.empty()) {
            Status copied = detail::copyFolder(mediaDir(), media, &createdMedia);
            // Until a file holds them, a failure keeps them ours: a retry
            // would take them for files that were there before it.
            for (const fs::path& file : createdMedia)
                if (!holds(unpublishedCopies_, file)) unpublishedCopies_.push_back(file);
            SUPPLE_TRY(std::move(copied));
        }
        SUPPLE_TRY(publish());
        createdMedia = std::move(unpublishedCopies_);
        unpublishedCopies_.clear();
    }
    // The user's modules it plays go with it, as its media do (ADR-0062):
    // those of every version the store keeps, not only the current head's,
    // since the file keeps branches and may keep undo (ADR-0076). A version
    // the save squashes away costs at most an unused copy.
    if (const fs::path dir = projectModuleDir(path); !dir.empty()) {
        std::set<std::string> seen;
        std::vector<std::string> types;
        versions_->forEachVersion([&](const model::Version& v) {
            for (const auto& e : v.session->modules)
                if (seen.insert(e.value->type).second) types.push_back(e.value->type);
        });
        if (Status copied = detail::saveProjectModules(*modules_, types, dir); !copied && warnings)
            warnings->push_back(
                {"project.modules",
                 "the project's module copies were not written: " + copied.error().message,
                 dir.string()});
    }
    projectPath_ = path;
    // A first save or a save as gives the project a file: an autosave
    // offered from now on is offered against the file as it is now, and a
    // later save to the same path keeps that (recoverAutosave).
    autosavedThrough_ = through;
    if (std::error_code ec; !openedFile_ || openedFile_->path != path)
        if (const auto time = fs::last_write_time(path, ec); !ec)
            openedFile_.emplace(OpenedFile{path, time, through});
    // The journals of the takes this file holds are retired where the save
    // put them: moved with a first save, copied by a save as into another
    // folder (whose old copy stays the old project's, as its WAVs do). A
    // file the copy found already there is not this save's: it stays.
    std::vector<fs::path> retire;
    for (const fs::path& journal : recordingJournalsPending_) {
        const fs::path rel = journal.lexically_relative(oldMedia);
        if (!relocated || sameMedia || rel.empty() || *rel.begin() == "..") {
            retire.push_back(journal);
            continue;
        }
        const fs::path moved = media / rel;
        if (fromScratch || holds(createdMedia, moved)) retire.push_back(moved);
    }
    recordingJournalsPending_ = std::move(retire);
    dropSavedRecordingJournals();
    if (fromScratch) releaseScratch();  // its media moved; its autosaves are older than the file
    autosaveMarked_ = true;             // the file itself is the newest copy
    autosaveMs_ = serviceClockMs();
    autosaveHeads_ = versions_->heads();
    return okStatus();
}

// --- crash safe-load (topic 7d) ---------------------------------------------------------

std::vector<EngineApp::ProjectPlugin> EngineApp::gatherPlugins(const std::vector<NamedHead>& heads,
                                                               const std::string& current) const {
    using Key = std::pair<std::string, std::string>;
    std::map<Key, ProjectPlugin> byKey;
    for (const auto& [branch, session] : heads)
        for (const auto& entry : session->fx) {
            const model::PluginRef& ref = entry.value->plugin;
            std::optional<host::PluginInfo> resolved;
            if (catalogue_)
                if (auto found = findPlugin(std::span<const host::PluginInfo>(*catalogue_), ref.id))
                    resolved = std::move(*found);
            const Key key =
                resolved ? Key(resolved->format, resolved->id) : Key(ref.format, ref.id);
            auto [it, added] = byKey.try_emplace(key);
            ProjectPlugin& p = it->second;
            if (added) {
                p.format = key.first;
                p.id = key.second;
                p.name = ref.name.empty() && resolved ? resolved->name : ref.name;
                p.vendor = ref.vendor.empty() && resolved ? resolved->vendor : ref.vendor;
                p.version = ref.version.empty() && resolved ? resolved->version : ref.version;
                p.resolved = std::move(resolved);
                p.mode = overrides_.mode(key.first, key.second);
                p.stock = p.resolved && pool_ && pool_->isStock(*p.resolved);
                p.bridgeable = p.resolved && !p.stock && !host::bridgeExecutable().empty();
            }
            if (branch == current) ++p.uses;
            if (std::ranges::find(p.branches, branch) == p.branches.end())
                p.branches.push_back(branch);
        }
    std::vector<ProjectPlugin> out;
    for (auto& [key, p] : byKey) out.push_back(std::move(p));
    return out;
}

void EngineApp::annotateSuspects(std::vector<ProjectPlugin>& plugins) {
    auto found = diagnostics::suspects(diagnosticsDir());
    if (!found || found->empty()) return;
    detail::SuspectCheck check(std::move(*found));
    for (auto& p : plugins) {
        if (!p.resolved) continue;  // not installed: nothing to load, nothing to protect
        const auto verdict = check.check(*p.resolved);
        if (!verdict.applies()) continue;
        p.suspectBuild = std::string(detail::buildName(verdict.build));
        p.attribution = verdict.attribution;
        p.incidents = verdict.incidents;
    }
}

void EngineApp::warnSuspects(const std::vector<ProjectPlugin>& plugins,
                             std::vector<schema::Warning>* warnings) {
    for (const auto& p : plugins) {
        // A bridge or a disable already protects this run; a settled one is not a suspect.
        if (p.suspectBuild.empty() || p.mode != LoadMode::Normal) continue;
        const std::string who = (p.name.empty() ? p.id : p.name) + " (" + p.id + ")";
        const std::string how = p.suspectBuild == "unreadable"
                                    ? ", and the installed file could not be checked against it"
                                : p.suspectBuild == "version" ? ", for the version installed"
                                                              : ", for the build installed";
        const std::string remedy =
            p.stock ? "it looks like a product fault, so only disabling it is offered: "
                      "plugin.override {plugin, mode: \"disable\"}"
                    : (p.bridgeable ? "plugin.override {plugin, mode: \"bridge\" or \"disable\"}"
                                    : "plugin.override {plugin, mode: \"disable\"}") +
                          std::string(" before opening protects this run");
        std::string message = who + " may crash: crash report " + p.incidents.front() +
                              " points at it (" + p.attribution + how +
                              "); it loads normally here. " + remedy +
                              "; crash.settle the report once the plug-in is trusted.";
        if (options_.warnSuspectsOnStderr && suspectsWarned_.insert(p.format + "/" + p.id).second)
            std::fprintf(stderr, "%s\n", message.c_str());
        if (warnings)
            warnings->push_back({"project.suspect_plugin", std::move(message), "/plugins/" + p.id});
    }
}

void EngineApp::warnStoreSuspects(const model::VersionStore& store,
                                  std::vector<schema::Warning>* out) {
    auto suspected = diagnostics::suspects(diagnosticsDir());
    if (!suspected || suspected->empty()) return;
    std::vector<NamedHead> heads;
    bool holdsPlugins = false;
    for (const auto& name : store.branches())
        if (auto head = store.head(name)) {
            holdsPlugins = holdsPlugins || !head->fx.empty();
            heads.emplace_back(name, std::move(head));
        }
    if (!holdsPlugins) return;
    // The load scans the catalogue for these plug-ins anyway; the check needs it first.
    if (!catalogue_) (void)plugins();
    auto list = gatherPlugins(heads, store.branch());
    annotateSuspects(list);
    warnSuspects(list, out);
}

Result<EngineApp::ProjectPlugins> EngineApp::projectPlugins(
    const fs::path& where, bool asTemplate, std::vector<schema::Warning>* warnings) {
    const fs::path path = resolve(where);
    std::error_code ec;
    const fs::path file = asTemplate && fs::is_directory(path, ec) ? path / "project.json" : path;
    auto text = readTextFile(file, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    ProjectPlugins out;
    out.path = file.string();
    out.hash = "sha256:" + sha256Hex(*text);
    out.source = asTemplate ? "template" : "file";
    std::vector<NamedHead> heads;
    std::string current = "main";
    if (asTemplate) {
        auto session = model::parseProject(*text, warnings);
        if (!session) return std::move(session).error();
        heads.emplace_back(current, std::make_shared<const model::Session>(std::move(*session)));
    } else {
        auto opened = model::parseProjectFile(*text, warnings, replayableStep);
        if (!opened) return std::move(opened).error();
        current = opened->store.branch();
        for (const auto& name : opened->store.branches())
            if (auto head = opened->store.head(name)) heads.emplace_back(name, std::move(head));
    }
    out.catalogueEmpty = !catalogue_ || catalogue_->empty();
    out.plugins = gatherPlugins(heads, current);
    annotateSuspects(out.plugins);
    return out;
}

void EngineApp::harvestRan() {
    if (!pool_) return;
    for (auto& ran : pool_->ranInProcess()) {
        // An earlier project of the run may have run it already: the first sighting stands.
        auto known = std::ranges::find_if(ranBefore_, [&](const InstancePool::Ran& seen) {
            return seen.info.format == ran.info.format && seen.info.id == ran.info.id;
        });
        if (known == ranBefore_.end())
            ranBefore_.push_back(std::move(ran));
        else if (ran.since < known->since)
            known->since = ran.since;
    }
}

void EngineApp::settleCleanSuspects() {
    harvestRan();
    if (ranBefore_.empty()) return;
    auto found = diagnostics::suspects(diagnosticsDir());
    if (!found || found->empty()) return;
    detail::SuspectCheck check(std::move(*found));
    for (const auto& ran : ranBefore_) {
        // Only the build the report named, or the version when it named none: a plug-in whose
        // file could not be checked ran fine, but is not known to be the one that crashed.
        const auto verdict = check.check(ran.info);
        if (verdict.build != detail::SuspicionVerdict::Build::Same &&
            verdict.build != detail::SuspicionVerdict::Build::Version)
            continue;
        // Only a report older than this run's first sight of the plug-in active: another process
        // sharing the folder may have crashed in it since, and a clean run here cannot clear that.
        for (std::size_t i = 0; i < verdict.incidents.size(); ++i)
            if (verdict.incidentTimes[i] < ran.since)
                (void)diagnostics::settle(verdict.incidents[i], diagnosticsDir());
    }
    ranBefore_.clear();
}

Status EngineApp::open(const fs::path& where, std::vector<schema::Warning>* warnings,
                       std::string_view expectHash) {
    const fs::path path = resolve(where);
    auto text = readTextFile(path, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    if (!expectHash.empty() && "sha256:" + sha256Hex(*text) != expectHash)
        return Error("project.changed",
                     path.filename().string() + " changed since its plug-ins were checked", "/path",
                     "run project.plugins again and decide again, or open without expect_hash");
    auto opened = model::parseProjectFile(*text, warnings, replayableStep);
    if (!opened) return std::move(opened).error();
    // A plug-in an earlier crash report points at is warned of, never refused or changed here:
    // the engine does not prompt and does not override itself (the frontend or the agent asked
    // first with project.plugins, and plugin.override).
    warnStoreSuspects(opened->store, warnings);
    if (opened->branchesDropped && warnings) {
        const Error& why = *opened->branchesDropped;
        warnings->push_back({"project.history",
                             "opened main alone: the saved branches could not be restored (" +
                                 why.message + (why.path.empty() ? "" : " at " + why.path) +
                                 "). Saving over this file first keeps a copy of it as " +
                                 path.filename().string() + ".pre-branches.bak",
                             why.path.empty() ? "/branches" : why.path});
    }
    const auto current = opened->store.head();
    const model::Session* s = current.get();
    if (warnings) {
        // Scoped so that its probes are released before the engine creates
        // the real instances (no probe sits beside its instance during open).
        PluginModValidation validation{*s, {}};
        for (const auto& cable : s->cables) {
            const auto& target = cable.value->to;
            if (target.object.kind != model::Kind::Fx) continue;
            const auto* fx = s->fx.find(target.object);
            // Missing plug-ins remain reopenable, as with strip FX.
            if (!fx || !findPlugin(fx->plugin.id)) continue;
            auto checked = validatePluginMod({}, *fx, target.key,
                                             "/cables/" + cable.id.str() + "/to", {}, &validation);
            if (!checked)
                warnings->push_back({checked.error().code,
                                     checked.error().message + "; cable retained but not heard. " +
                                         checked.error().hint,
                                     checked.error().path});
        }
    }
    const bool dropped = opened->branchesDropped.has_value();
    const auto through = opened->autosavedThrough;
    SUPPLE_TRY(replaceProject(std::move(opened->store), path, {}, warnings));
    autosavedThrough_ = through;
    if (openedFile_) openedFile_->through = through;
    if (dropped) branchesBackupFor_ = path;
    return okStatus();
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

Status EngineApp::replayableFromFile(const cmd::Registry& registry,
                                     const model::Invocation& invocation) {
    auto refuse = [&](std::string why) {
        return Error("history.command", invocation.command + ": " + why, "/command",
                     "a branch may replay only edits of the session");
    };
    const cmd::CommandInfo* info = registry.find(invocation.command);
    if (!info) return refuse("not a command of this program");
    if (info->kind != cmd::Kind::Edit || invocation.command == "batch")
        return refuse("not an edit of the session, so a saved step may not run it");
    if (info->validate)
        if (Status fits = info->validate(invocation.args); !fits)
            return refuse("its arguments do not fit the command: " + fits.error().message);
    return okStatus();
}

Result<model::Replayed> EngineApp::replay(const cmd::Registry& registry, const model::Session& base,
                                          const std::vector<model::Invocation>& invocations,
                                          std::uint64_t version) {
    // Before any step runs, and before the live transaction is set aside: a
    // replay writes files only inside the project's own folder (a file's
    // media_dir may name any folder, or a link out).
    SUPPLE_TRY(replayMayWriteMedia());
    std::unique_ptr<model::EditTxn> saved = std::move(txn_);
    model::TxnRecord savedRecord = std::move(record_);
    txn_ = std::make_unique<model::EditTxn>(base, version);
    const bool wasReplaying = std::exchange(replaying_, true);
    auto restore = [&] {
        txn_ = std::move(saved);
        record_ = std::move(savedRecord);
        replaying_ = wasReplaying;
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
        if (Status allowed = replayableFromFile(registry, inv); !allowed) {
            restore();
            return std::move(allowed).error();
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
    // Files and folders the replays create are removed if nothing is merged.
    std::vector<fs::path> created;
    std::error_code ec;
    mergeCreated_ = &created;
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
    mergeCreated_ = nullptr;
    if (!outcome.conflicts.empty())  // files first, then the folders deepest first
        for (auto it = created.rbegin(); it != created.rend(); ++it) fs::remove(*it, ec);
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
    offlinePool.setOverrides(overrides_);
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

namespace {
Result<RecordedPunch> punchFrames(const TransportRange& punch, double rate) {
    const long double start =
        static_cast<long double>(punch.start) * static_cast<long double>(rate);
    const long double end = static_cast<long double>(punch.end) * static_cast<long double>(rate);
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end < 0 ||
        start >= static_cast<long double>(detail::kRecordingClockLimit) + .5L ||
        end >= static_cast<long double>(detail::kRecordingClockLimit) + .5L)
        return Error("record.punch", "the punch is outside the recording clock range", "/punch",
                     "clear the punch or use a nonnegative range within 2^48 recording frames");
    RecordedPunch frames{punch.enabled, static_cast<std::int64_t>(std::llround(start)),
                         static_cast<std::int64_t>(std::llround(end))};
    SUPPLE_TRY(detail::validateRecordingPunch(frames));
    return frames;
}
}  // namespace
Status EngineApp::setPunch(const TransportRange& punch) {
    if (engine_ && engine_->recording()) {
        auto frames = punchFrames(punch, engine_->config().sampleRate);
        if (!frames) return std::move(frames).error();
        SUPPLE_TRY(engine_->setRecordingPunch(*frames));
    }
    punch_ = punch;
    return okStatus();
}

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
    engine_->setMidiPreferences(midiInputsPreferences_, midiOutputsPreferences_);
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
    applySync();  // before the first plan: a sync plan from the start (ADR-0071)
    auto loaded = engine_->load(head(), projectDir());
    if (!loaded) {
        haltEngine();
        return loaded;
    }
    loadedHead_ = versions_->head();
    protections_->start();
    startFaultWatch();
    return {};
}

void EngineApp::startFaultWatch() {
    faultWatch_.clear();
    processFaultBase_ = processPageFaults();
    watchFaults();
}

void EngineApp::watchFaults() {
    if (!engine_ || !processFaultBase_) return;
    for (const auto thread : engine_->deadlineThreads()) {
        const std::int64_t id = kernelThreadId(thread);
        if (id == 0 || std::any_of(faultWatch_.begin(), faultWatch_.end(),
                                   [&](const FaultWatch& w) { return w.id == id; }))
            continue;
        if (const auto now = threadPageFaults(id)) faultWatch_.push_back({id, *now, *now});
    }
}

Status EngineApp::startEngine(std::string_view backend, std::uint32_t block, bool anticipative,
                              const EngineDeviceOptions& device) {
    if (engine_)
        return Error("engine.running", "the engine is already running", {},
                     "engine.stop first to change the backend or block size");
    BackendConfig config;
    config.sampleRate = device.rate > 0 ? device.rate : view().project->sampleRate;
    config.blockSize = block;
    config.inputs = 2;          // at least the device's first stereo pair, for recording
    config.midiOutputs = true;  // device output API (ADR-0069); routing is separate
    config.midiInputs = true;   // its MIDI inputs, for MIDI tracks (ADR-0016)
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
            DummyBackend::Options{.mode = DummyBackend::Mode::Thread,
                                  .paced = true,
                                  .midiOutputPorts = options_.dummyMidiOutputs,
                                  .midiPorts = options_.dummyMidiInputs});
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
    stopAudition();  // the engine stopping drops it (ADR-0094)
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
    processFaultBase_.reset();
    faultWatch_.clear();
    if (backend_) backend_->setMainThreadWake(nullptr);
    backend_ = nullptr;
    ownedBackend_.reset();
    outputGestures_.clear();
    pendingOutputs_.clear();
    outputGeneration_ = outputPatches_ = outputEpoch_ = 0;
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

void EngineApp::emitRecordingError(const RecordingError& failure) {
    if (!recordingErrorsReported_.insert(failure.file).second) return;
    const auto& error = failure.error;
    auto event = json::Value::object();
    event["track"] = failure.track.str();
    event["file"] = failure.file.string();
    event["error"] = error.toString();
    event["code"] = error.code;
    event["message"] = error.message;
    event["path"] = error.path.empty() ? "/recording/" + failure.track.str() : error.path;
    event["hint"] =
        error.hint.empty()
            ? "save the retained audio; check free space and storage before recording again"
            : error.hint;
    emit("record.error", std::move(event));
}

void EngineApp::tick() {
    mainWake_.drain();  // re-arm before inspecting producer queues
    serviceAudition();  // a decoded file audition to the engine (ADR-0094)
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
        noteUnplayedEdit();
        watchFaults();  // a callback thread seen for the first time
        // ADR-0086: the idle-latency request is held while a bridged plug-in
        // plays live (acts on a change only).
        (void)protections_->setBridgedLive(backend_ && engine_->bridgedLive(), backend_ != nullptr);
        for (const auto& error : engine_->takeRecordingErrors()) emitRecordingError(error);
        serviceSync();                  // what the sync master did, to the transport (ADR-0071)
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
    if (!autosaveDir().empty() && versions_->heads() != autosaveHeads_)
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
    if (autosaveDir().empty()) return;
    const std::uint64_t now = serviceClockMs();
    if (!autosaveMarked_) {
        autosaveMarked_ = true;
        autosaveMs_ = now;
        autosaveHeads_ = versions_->heads();
        return;
    }
    // Autosave when the project (any branch) changed and an interval passed
    // since the last one.
    if (versions_->heads() == autosaveHeads_ || now < autosaveMs_ + kAutosaveIntervalMs) return;
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

Status EngineApp::setPluginIsolation(model::Isolation isolation) {
    if (isolation == model::Isolation::Inherit)
        return Error("preferences.plugin_isolation", "inherit is not a default isolation",
                     "/plugin_isolation", "use in_process, shared_bridge or dedicated");
    pluginIsolation_ = isolation;
    if (pool_) pool_->setDefaultIsolation(isolation);
    return okStatus();
}

model::Isolation EngineApp::effectiveIsolation(model::Id fx) const {
    const model::FxInstance* instance = head().fx.find(fx);
    if (!instance) return model::Isolation::InProcess;
    return pool_ ? pool_->effectiveIsolation(fx, *instance) : instance->isolation;
}

fs::path EngineApp::autosaveDir() const {
    if (projectPath_.empty()) return options_.scratchDir;  // an unsaved project's (ADR-0078)
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
    // Above the largest count a project file holds, it is not an autosave.
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || index == 0 ||
        index > model::kAutosaveNumberMax)
        return std::nullopt;
    return index;
}

}  // namespace

fs::path EngineApp::newerAutosave(const fs::path& project) const {
    if (project.empty()) return {};
    std::error_code ec;
    const fs::path dir = project.parent_path() / (project.filename().string() + "-data");
    if (!fs::is_directory(dir, ec)) return {};
    // The open project's file says which autosaves it holds; another file,
    // or one written before 2.15, goes by file time.
    const std::optional<std::uint64_t> through =
        project == projectPath_ ? autosavedThrough_ : std::nullopt;
    fs::file_time_type written{};
    if (!through) {
        written = fs::last_write_time(project, ec);
        if (ec) return {};
    }
    fs::path best;
    std::uint64_t bestIndex = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& candidate = it->path();
        const auto index = autosaveIndex(candidate.filename().string());
        if (!index || *index <= bestIndex) continue;
        std::error_code fileError;
        if (!fs::is_regular_file(fs::symlink_status(candidate, fileError))) continue;
        if (through ? *index <= *through
                    : fs::last_write_time(candidate, fileError) <= written || fileError)
            continue;
        best = candidate;
        bestIndex = *index;
    }
    return best;
}

bool EngineApp::captureFxState(model::Id id, Error* failure) {
    // A frozen home locks its racks' plug-ins (ADR-0075): their state stays.
    if (txn_ || !pool_ || model::frozenOwner(head(), id) || model::frozenRackHome(head(), id))
        return false;
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
    // Logged as fx.state.capture, so a merge replays it on another branch.
    json::Value args = json::Value::object();
    args["fx"] = id.str();
    args["state"] = hash;
    record_.invocations.push_back({"fx.state.capture", std::move(args)});
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
    if (projectPath_.empty()) holdScratch();
    // Rotate: the next free index, then the oldest files over the keep count go.
    std::uint64_t next = 1;
    std::vector<std::pair<std::uint64_t, fs::path>> existing;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const auto n = autosaveIndex(name);
        if (!n) continue;
        if (*n == model::kAutosaveNumberMax) {
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
    // Past what the project file holds, even when its autosaves are gone.
    if (!projectPath_.empty() && autosavedThrough_ && *autosavedThrough_ >= next) {
        if (*autosavedThrough_ >= model::kAutosaveNumberMax) {
            emit(
                "engine.error",
                json::Value(Error("autosave.index", "autosave numbering is exhausted", dir.string(),
                                  "save the project under a new name to number afresh")
                                .toString()));
            autosaveMs_ = serviceClockMs();
            return;
        }
        next = *autosavedThrough_ + 1;
    }
    const fs::path file = dir / ("autosave-" + std::to_string(next) + ".json");
    auto st = checkpointRecordingJournals();
    if (st) st = writeFileAtomic(file, model::serializeProjectFile(*versions_));
    if (!st) {
        emit("engine.error", json::Value(st.error().toString()));
        autosaveMs_ = serviceClockMs();  // try again after an interval, not every tick
        return;
    }
    dropSavedRecordingJournals();
    existing.push_back({next, file});
    std::sort(existing.begin(), existing.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    while (static_cast<int>(existing.size()) > kAutosaveKeep) {
        std::error_code ignored;
        fs::remove(existing.front().second, ignored);
        existing.erase(existing.begin());
    }
    autosaveHeads_ = versions_->heads();
    autosaveMs_ = serviceClockMs();
    autosaveMarked_ = true;
}

namespace {
fs::path audioJournalWave(const fs::path& journal) {
    auto stem = journal.filename().string();
    stem.resize(stem.size() - std::string_view(".audio.jsonl").size());
    return journal.parent_path() / (stem + ".wav");
}
}  // namespace
Status EngineApp::checkpointRecordingJournals() {
    for (const auto& journal : recordingJournalsPending_) {
        if (!detail::isAudioJournal(journal)) continue;
        const auto wav = audioJournalWave(journal);
        // A prior writer sync may have failed; never retire its recovery
        // metadata on the strength of a durable project but volatile audio.
        SUPPLE_TRY(syncFile(wav));
        SUPPLE_TRY(syncParentDirectories(wav));
    }
    return okStatus();
}
void EngineApp::dropSavedRecordingJournals() {
    std::vector<fs::path> kept;
    for (const fs::path& journal : recordingJournalsPending_) {
        if (detail::isAudioJournal(journal)) {
            const auto wav = audioJournalWave(journal);
            auto side = detail::readSidecar(wav, wav.parent_path());
            // Metadata failure leaves the journal, including across retry.
            if (!side || !side->frames) {
                const auto inspected = audio::inspectWavStream(wav);
                if (!inspected || inspected->fileFrames != 0) {
                    kept.push_back(journal);
                    continue;
                }
            }
        }
        std::error_code ec;
        fs::remove(journal, ec);
        if (ec) kept.push_back(journal);
    }
    recordingJournalsPending_ = std::move(kept);
}

void EngineApp::restartFx(model::Id fx) {
    pool_->restart(fx);
    reportedFailures_.erase(fx);
    headChanged();  // a running engine gets the new instance in its next plan
}

EngineBridgeTiming bridgeTimingStatus(const host::BridgeTiming::Calls& live,
                                      const Engine::BridgeOverruns& overruns) {
    EngineBridgeTiming t;
    const double calls =
        live.calls + live.settles ? static_cast<double>(live.calls + live.settles) : 1.0;
    const double answered =
        live.calls > live.late ? static_cast<double>(live.calls - live.late) : 1.0;
    t.calls = static_cast<std::int64_t>(live.calls);
    t.late = static_cast<std::int64_t>(live.late + live.lateSettles);
    t.settles = static_cast<std::int64_t>(live.settles);
    t.pastDeadline = static_cast<std::int64_t>(live.pastDeadline);
    t.spun = static_cast<std::int64_t>(live.spun);
    t.waitMaxUs = static_cast<double>(live.waitMaxNs) / 1000.0;
    t.waitMeanUs = static_cast<double>(live.waitSumNs) / calls / 1000.0;
    t.wakeMaxUs = static_cast<double>(live.wakeMaxNs) / 1000.0;
    t.wakeMeanUs = static_cast<double>(live.wakeSumNs) / answered / 1000.0;
    t.workMaxUs = static_cast<double>(live.workMaxNs) / 1000.0;
    t.returnMaxUs = static_cast<double>(live.returnMaxNs) / 1000.0;
    t.longestCycleUs = static_cast<double>(overruns.longestCycleNs) / 1000.0;
    t.wakesOver200us = static_cast<std::int64_t>(live.wakesOver200us);
    t.wakesOver1ms = static_cast<std::int64_t>(live.wakesOver1ms);
    for (int c = 0; c < 64; ++c)
        if (live.bridgeCpus >> c & 1) t.bridgeCpus.push_back(c);
    t.migrations = live.migrations;
    t.classMismatches = static_cast<std::int64_t>(live.classMismatches);
    t.overrunsLateWait = static_cast<std::int64_t>(overruns.lateWait);
    t.overrunsSlowBridge = static_cast<std::int64_t>(overruns.slowBridge);
    t.overrunsNotWaiting = static_cast<std::int64_t>(overruns.notWaiting);
    return t;
}

EngineStatus EngineApp::engineStatus() {
    checkFailures();
    EngineStatus st;
    st.midiInputs = midiPortsState(false);
    st.midiOutputs = midiPortsState(true);
    st.midiLeadFrames = engine_ ? engine_->midiLeadFrames() : 0;
    st.preparingAudio = !pendingSourceKey_.empty();
    st.preparationVersion = st.preparingAudio ? head().version : 0;
    st.preparedVersion = loadedHead_ ? loadedHead_->version : 0;
    st.preparationError = preparationError_;
    st.memoryLock = protections_->memoryLock();
    st.cpuIdleLatency = protections_->cpuIdleLatency();
    st.thpOptOut = protections_->thpOptOut();
    if (processFaultBase_) {
        watchFaults();
        auto& faults = st.audioPageFaults;
#if defined(__linux__)
        faults.scope = "audio_threads";
        for (FaultWatch& w : faultWatch_) {
            // Gone keeps its last count; an id reused by a later thread, whose
            // count starts again, is not taken for it.
            if (const auto now = threadPageFaults(w.id); now && now->minor >= w.last.minor)
                w.last = *now;
            faults.minor += w.last.minor - w.base.minor;
            *faults.major += w.last.major - w.base.major;
        }
        faults.threads = static_cast<std::int64_t>(faultWatch_.size());
#else
        faults.scope = "process";
        const PageFaults now = processPageFaults();
        faults.minor = now.minor - processFaultBase_->minor;
        if (now.saturated || processFaultBase_->saturated) {
            // A counter stopped at its limit: no delta is a count (round 23).
            faults.saturated = true;
            faults.minor = 0;
            faults.major.reset();
        } else if (now.major < 0)
            faults.major.reset();
        else
            faults.major = now.major - processFaultBase_->major;
#endif
    }
    if (st.thpOptOut.error)
        st.warnings.push_back(st.thpOptOut.error->message + "; " + st.thpOptOut.error->hint);
    // An automatic hold that failed is no warning: audio plays without it, and
    // on many systems only root may make the request (ADR-0086).
    if (st.cpuIdleLatency.error && st.cpuIdleLatency.requested)
        st.warnings.push_back(st.cpuIdleLatency.error->message + "; " +
                              st.cpuIdleLatency.error->hint);
    if (st.memoryLock.error)
        st.warnings.push_back(st.memoryLock.error->message + "; " + st.memoryLock.error->hint);
    for (const auto& entry : head().fx)
        if (loadModeOf(entry.value->plugin) == LoadMode::Disable)
            st.disabledFx.push_back(entry.id.str());
    for (model::Id id : pool_->isolationFallbacks()) {
        const model::FxInstance* fx = head().fx.find(id);
        st.warnings.push_back((fx ? fx->plugin.name : id.str()) + " (" + id.str() +
                              ") runs in process: no plug-in bridge was found for the plug-in "
                              "isolation preference; reinstall, or choose In the app");
    }
    st.dropped = static_cast<std::int64_t>(outputDrops_ + consumerDrops_);
    st.voiceDropped = static_cast<std::int64_t>(voiceDrops_);
    for (const model::Id& id : pool_->bridgeThreadsRefused())
        st.bridgeThreadsRefused.push_back(id.str());
    for (const auto& [id, error] : pool_->failedErrors()) {
        st.failedFx.push_back(id.str());
        auto failure = json::Value::object();
        failure["fx"] = id.str();
        failure["reason"] = error.message;
        failure["error"] = pluginErrorJson(error);
        if (auto incident = pool_->failureIncident(id); !incident.empty())
            failure["incident"] = std::move(incident);
        st.failedErrors.asArray().push_back(std::move(failure));
    }
    st.sync = syncReport();
    if (!engine_ || !backend_) return st;
    const BackendStatus b = backend_->status();
    st.running = true;
    st.backend = backend_->name();
    st.sampleRate = engine_->config().sampleRate;
    st.block = engine_->config().blockSize;
    st.period = b.period;
    st.planLatency = engine_->outputLatency();
    st.deviceLatency = b.outputLatency;
    // A cycle a bridged plug-in made late is an xrun too (ADR-0081).
    st.bridgeLate = static_cast<std::int64_t>(engine_->bridgeLateCycles());
    st.xruns = static_cast<std::int64_t>(b.xruns) + st.bridgeLate;
    st.bridgeTiming = bridgeTimingStatus(host::bridgeTiming().live, engine_->bridgeOverruns());
    st.midiDropped = static_cast<std::int64_t>(backend_->midiDropped());
    st.midiWarnings = backend_->midiWarnings();
    st.midiOutDropped = static_cast<std::int64_t>(backend_->midiOutDropped());
    st.midiOutputWarnings = backend_->midiOutputWarnings();
    st.midiOutputLatency = backend_->midiOutputLatency();
    for (const auto& warning : st.midiOutputWarnings)
        st.warnings.push_back(warning.device + ": " + warning.message + " (" +
                              std::to_string(warning.osError) + "); " + warning.hint);
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
        std::set<std::string> reservedStems;
        for (const auto& e : s.tracks) {
            const model::Track& tr = *e.value;
            if (tr.record.armed && tr.record.mode == model::RecordMode::Output)
                return Error("record.output_unsupported", "output recording is not supported yet",
                             "/tracks/" + e.id.str() + "/record/mode",
                             "output recording is not supported yet; use track.set {track: \"" +
                                 e.id.str() + "\", record_mode: \"input\"}");
            if (!tr.record.armed) continue;
            const bool output = tr.type == model::StripType::Master;  // ADR-0057
            const bool routed =
                !output &&
                (tr.record.mode == model::RecordMode::MidiOverdub ||
                 tr.record.mode == model::RecordMode::MidiReplace) &&
                std::any_of(s.routes.begin(), s.routes.end(), [&](const auto& route) {
                    return route.value->to.track == e.id && route.value->midi &&
                           (route.value->from.module || route.value->from.track);
                });
            const model::Route* in =
                output || !tr.record.input ? nullptr : s.routes.find(*tr.record.input);
            const auto* midi = output ? nullptr : model::midiInputRoute(s, e.id);
            // Explicit MIDI record modes capture sends independently of the selected input.
            std::string base;
            for (char c : tr.name.empty() ? e.id.str() : tr.name)
                base += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
            // <base>-<n> free for a WAV, its sidecar and a MIDI journal alike
            // (the writer also creates each exclusively).
            // Any entry takes a name, a dangling link too (fs::exists
            // follows it and says no).
            auto occupied = [](const fs::path& p) {
                std::error_code ec;
                return fs::exists(fs::symlink_status(p, ec));
            };
            auto freeName = [&](const char* extension) {
                for (int n = 1;; ++n) {
                    const std::string stem = base + "-" + std::to_string(n);
                    auto folded = stem;
                    for (char& c : folded)
                        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                    const fs::path file = dir / (stem + extension);
                    const bool taken = reservedStems.contains(folded) ||
                                       occupied(dir / (stem + ".wav")) ||
                                       occupied(dir / (stem + ".rec.json")) ||
                                       occupied(dir / (stem + ".midi.jsonl")) ||
                                       occupied(dir / (stem + ".audio.jsonl"));
                    if (!taken) {
                        reservedStems.insert(std::move(folded));
                        return file;
                    }
                }
            };
            if (routed || midi) {  // MIDI (ADR-0016): events; a journal for recovery (ADR-0080)
                RecordTarget t;
                t.track = e.id;
                t.midi = RecordTarget::Midi{
                    midi ? midiPortMask(engine_->midiPorts(), *midi->from.midiDevice) : 0,
                    midi && midi->midi ? midi->midi->fromChannel : 0,
                    midi && midi->midi ? midi->midi->toChannel : 0, routed};
                t.file = freeName(".midi.jsonl");
                targets.push_back(std::move(t));
                continue;
            }
            if (!output && (!in || (!in->from.hwChannels && !in->from.module))) continue;
            RecordTarget t;
            t.track = e.id;
            t.output = output;
            t.rackInput = in && in->from.module.has_value();
            if (in && in->from.hwChannels)
                for (int ch : *in->from.hwChannels) t.inputs.push_back(ch - 1);
            t.file = freeName(".wav");
            targets.push_back(std::move(t));
        }
        auto recordPunch = punchFrames(punch_, engine_->config().sampleRate);
        if (!recordPunch) return std::move(recordPunch).error();
        std::vector<fs::path> files;  // WAVs and MIDI journals: not leftovers while recording
        for (const RecordTarget& t : targets)
            if (!t.file.empty()) files.push_back(t.file);
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
        if (auto started = engine_->startRecording(std::move(targets), *recordPunch); !started) {
            endMasterRecording();
            return std::move(started).error();
        }
        recordingFiles_ = std::move(files);
        recordingErrorsReported_.clear();
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
        return t.param == "volume"      ? tr->volumeDb
               : t.param == "pan"       ? tr->pan
               : t.param == "width"     ? tr->width
               : t.param == "pan_left"  ? tr->panLeft
               : t.param == "pan_right" ? tr->panRight
               : t.param == "pan_boost" ? (model::panLawOf(*s.project, *tr).boost ? 1.0 : 0.0)
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
    // A replayed step's writes come from its logged env.write.commit, not
    // from where the live transport is now (nor into the live pass).
    if (replaying_ || !engine_ || !txn_ || !engine_->transport().playing) return;
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
    {
        const model::Envelope& env = *s.envelopes.find(*envId);
        const double pos = envelopePosition(s, env, seconds);
        const double from = envelopePosition(s, env, fromSeconds);
        const double before = envelopeAt(env.points, pos, staticValue(s, env.target));
        std::vector<model::EnvPoint> points;
        // Preserve the existing 0.001-beat entry guard, converted into the
        // envelope's domain just like the write position and erase window.
        const double beat = s.timeMap->beatAt(seconds);
        if (first && beat > 1e-3)
            points.push_back({envelopePosition(s, env, s.timeMap->secondsAt(beat - 1e-3)), before});
        points.push_back({pos, value});
        // The first write replaces only a point at its own position.
        writeEnvelope(*envId, first ? pos - 1e-9 : from, pos + 1e-9, points, true);
    }
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
                ? param == "volume" || param == "pan" ||
                      (kind == model::Kind::Track && (param == "width" || param == "pan_left" ||
                                                      param == "pan_right" || param == "pan_boost"))
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
    for (const auto& [id, w] : work) {
        const model::Envelope& env = *s.envelopes.find(id);
        const double start = envelopePosition(s, env, w.startSeconds);
        const double stop = envelopePosition(s, env, stopSeconds);
        const double from = w.touched ? envelopePosition(s, env, w.lastSeconds) : start - 1e-9;
        std::vector<model::EnvPoint> points;
        if (!w.touched) points.push_back({start, w.lastValue});
        points.push_back({stop, w.lastValue});
        writeEnvelope(id, from, stop + 1e-9, points, true);
    }
    return commitTransaction(*txnId);
}

void EngineApp::writeEnvelope(model::Id env, double after, double through,
                              const std::vector<model::EnvPoint>& points, bool log) {
    txn().modify<model::Envelope>(env, [&](model::Envelope& e) {
        std::erase_if(e.points,
                      [&](const model::EnvPoint& p) { return p.pos > after && p.pos <= through; });
        for (const auto& p : points) insertPoint(e.points, p.pos, p.value);
    });
    if (!log) return;
    json::Value args = json::Value::object();
    args["env"] = env.str();
    args["after"] = after;
    args["through"] = through;
    json::Value list = json::Value::array();
    for (const auto& p : points) {
        json::Value point = json::Value::object();
        point["pos"] = p.pos;
        point["value"] = p.value;
        list.push_back(std::move(point));
    }
    args["points"] = std::move(list);
    record_.invocations.push_back({"env.write.commit", std::move(args)});
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

// --- file audition (ADR-0094) ---------------------------------------------------------------

Result<AuditionInfo> EngineApp::playAudition(const fs::path& given) {
    if (!engine_)
        return Error("engine.stopped", "the engine is not running", {}, "engine.start first");
    const fs::path path = given.is_relative() ? mediaDir() / given : given;
    auto inspected = audio::inspect(path);
    if (!inspected)
        return Error("audition.file", path.string() + ": " + inspected.error().message, "/path",
                     "give a WAV, FLAC or MP3 file: an absolute path, or a name in the "
                     "project's media folder");
    stopAudition();
    // The cap bounds the decoded file and the resampled voice alike, so it
    // counts at the higher of the file's and the device's rates (the
    // resampler's round(n * device / file) frames stay within it).
    const double deviceRate = engine_->config().sampleRate;
    const auto cap = static_cast<std::int64_t>(
        std::floor(static_cast<double>(std::max<std::int64_t>(0, options_.auditionMaxFrames)) *
                   std::min(1.0, inspected->sampleRate / deviceRate)));
    AuditionInfo info;
    info.path = path.string();
    info.sampleRate = inspected->sampleRate;
    info.channels = inspected->channels;
    info.truncated = inspected->frames > cap;
    info.seconds = static_cast<double>(std::min(inspected->frames, cap)) / inspected->sampleRate;
    if (!auditionLoader_)
        auditionLoader_ = std::make_unique<AuditionLoader>([this] { mainWake_.notify(); });
    auditionRate_ = deviceRate;
    auditionRequest_ = auditionLoader_->request({path, cap, auditionRate_});
    auditionDecoding_ = true;
    auditionInfo_ = info;
    return info;
}

void EngineApp::stopAudition() {
    if (auditionLoader_) auditionLoader_->cancel();
    auditionRequest_ = 0;
    auditionDecoding_ = false;
    auditionInfo_ = {};
    if (engine_ && auditionVoice_ != 0) engine_->audition(nullptr);
    auditionVoice_ = 0;
}

void EngineApp::serviceAudition() {
    if (!auditionLoader_) return;
    auto done = auditionLoader_->take();
    if (!done || !auditionDecoding_ || done->generation != auditionRequest_) return;
    auditionDecoding_ = false;
    if (!done->voice) {
        auto event = json::Value::object();
        event["path"] = auditionInfo_.path;
        event["code"] = done->voice.error().code;
        event["message"] = done->voice.error().message;
        emit("audition.error", std::move(event));
        auditionInfo_ = {};
        return;
    }
    // The device it was resampled for (a restart in between drops it).
    if (!engine_ || engine_->config().sampleRate != auditionRate_) {
        auditionInfo_ = {};
        return;
    }
    auditionVoice_ = engine_->audition(std::move(*done->voice));
    if (auditionVoice_ == 0) auditionInfo_ = {};  // an empty file: nothing plays
}

AuditionStatus EngineApp::auditionStatus() const {
    AuditionStatus s;
    s.decoding = auditionDecoding_;
    bool playing = false;
    std::int64_t position = 0;
    if (engine_ && auditionVoice_ != 0) {
        const AuditionState state = engine_->auditionState();
        // Published and not yet taken by the callback, or playing.
        playing = state.taken < auditionVoice_ || state.serial == auditionVoice_;
        if (state.serial == auditionVoice_) position = state.position;
    }
    s.playing = s.decoding || playing;
    if (!s.playing) return s;
    s.path = auditionInfo_.path;
    s.seconds = auditionInfo_.seconds;
    if (playing && auditionRate_ > 0) s.position = static_cast<double>(position) / auditionRate_;
    return s;
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
    using Address = std::tuple<Id, int, int, int>;  // route, voice, channel, key
    std::map<Address, std::pair<std::int64_t, int>> open;
    auto close = [&](Address key, std::int64_t at, int offVelocity) {
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
        n.channel = static_cast<std::uint8_t>(std::get<2>(key));
        n.pitch = static_cast<std::uint8_t>(std::get<3>(key));
        n.velocity = static_cast<std::uint8_t>(velocity);
        n.offVelocity = static_cast<std::uint8_t>(offVelocity);
        seq.notes.push_back(n);
    };
    for (const RecordedMidiEvent& ev : r.midi) {
        if (ev.run != run) continue;
        const int status = ev.bytes[0] & 0xf0, ch = ev.bytes[0] & 0x0f;
        const int b1 = ev.bytes[1], b2 = ev.bytes[2];
        const Address key{ev.route, ev.voice, ch, b1};
        if (status == 0x90 && b2 > 0) {
            close(key, ev.timeline, 0);
            open[key] = {ev.timeline, b2};
        } else if (status == 0x80 || status == 0x90) {
            close(key, ev.timeline, status == 0x80 ? b2 : 0);
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
    if (side.punch) {
        const auto& punch = *side.punch;
        if (punch.start < 0 || punch.end < 0 || punch.start > detail::kRecordingClockLimit ||
            punch.end > detail::kRecordingClockLimit || (punch.enabled && punch.end <= punch.start))
            return invalid("/punch");
    }
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
    std::vector<std::pair<Id, RecordedTake>> pendingHashes;
    const model::Session initial = view();
    for (const auto& r : takes) {
        if (r.frames <= 0) continue;
        const double rate = r.sampleRate == 0 ? initial.project->sampleRate : r.sampleRate;
        detail::RecordingInfo side{r.track,    r.startSample, rate,
                                   r.segments, r.loop.start,  r.loop.end};
        side.punch = r.punch;
        SUPPLE_TRY(recordingBounds(initial, side, static_cast<std::uint64_t>(r.frames)));
    }
    auto txnId = beginTransaction({cmd::Origin::Type::Internal, "engine", ""}, label);
    if (!txnId) return std::move(txnId).error();
    std::vector<RecordedInfo> out;
    // One undo step, with contiguous groups retaining each recording's punch.
    // record.commit already has a top-level punch: no argument/schema change.
    const fs::path media =
        projectDir().empty() ? fs::path() : projectDir() / head().project->mediaDir;
    auto punchFor = [&](const RecordedTake& take) {
        if (!take.punch) return punch_;
        const auto& p = *take.punch;
        const double rate = take.sampleRate > 0 ? take.sampleRate : initial.project->sampleRate;
        return TransportRange{p.enabled, static_cast<double>(p.start) / rate,
                              static_cast<double>(p.end) / rate};
    };
    for (std::size_t first = 0; first < takes.size();) {
        const auto punch = punchFor(takes[first]);
        auto last = first + 1;
        while (last < takes.size()) {
            const auto other = punchFor(takes[last]);
            if (other.enabled != punch.enabled || other.start != punch.start ||
                other.end != punch.end)
                break;
            ++last;
        }
        const std::vector<RecordedTake> group(takes.begin() + static_cast<std::ptrdiff_t>(first),
                                              takes.begin() + static_cast<std::ptrdiff_t>(last));
        auto placed = placeTakes(group, punch, &pendingHashes);
        if (!placed) return std::move(placed).error();
        out.insert(out.end(), std::make_move_iterator(placed->begin()),
                   std::make_move_iterator(placed->end()));
        record_.invocations.push_back(
            {"record.commit", detail::recordCommitArgs(group, punch, media)});
        first = last;
    }
    if (takes.empty())
        record_.invocations.push_back(
            {"record.commit", detail::recordCommitArgs(takes, punch_, media)});
    SUPPLE_TRY(commitTransaction(*txnId));
    // The takes are in the project now; owned journals go once a saved
    // file or autosave holds them (ADR-0080/0082).
    for (const RecordedTake& r : takes)
        if (r.isMidi && !r.file.empty())
            recordingJournalsPending_.push_back(r.file);
        else if (!r.isMidi && !r.journal.empty())
            recordingJournalsPending_.push_back(r.journal);
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

Result<std::vector<RecordedInfo>> EngineApp::placeTakes(
    const std::vector<RecordedTake>& takes, const TransportRange& punch,
    std::vector<std::pair<Id, RecordedTake>>* pendingHashes) {
    std::vector<RecordedInfo> out;
    const bool live = pendingHashes != nullptr;  // else record.commit replays them
    const model::Session s = view();
    if (!live)  // commitTakes checked them before its transaction
        for (const auto& r : takes) {
            if (r.frames <= 0) continue;
            const double rate = r.sampleRate == 0 ? s.project->sampleRate : r.sampleRate;
            detail::RecordingInfo side{r.track,    r.startSample, rate,
                                       r.segments, r.loop.start,  r.loop.end};
            SUPPLE_TRY(recordingBounds(s, side, static_cast<std::uint64_t>(r.frames)));
        }
    // Relative to the media folder when there is one (a saved project's or the
    // scratch folder), so a first save can move the files and keep the paths.
    const fs::path media = projectDir().empty() ? fs::path() : projectDir() / s.project->mediaDir;
    for (const RecordedTake& r : takes) {
        if (live)
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
        if (live && !r.error.empty())
            emitRecordingError(
                {r.track, r.file, r.failure.value_or(Error("record.write", r.error))});
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
        const auto punchFrom = static_cast<std::int64_t>(std::llround(punch.start * rate));
        const auto punchTo = static_cast<std::int64_t>(std::llround(punch.end * rate));
        auto clip = [&](std::int64_t& from, std::int64_t& to) {
            if (punch.enabled) {
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
        // Replayed before its hash was known here: the branch's own source
        // for the file may have it by now.
        std::string hash = r.hash;
        std::error_code relError;
        const fs::path relative =
            media.empty() ? fs::path() : fs::relative(r.file, media, relError);
        const std::string mediaPath = !relError && !relative.empty() && *relative.begin() != ".."
                                          ? relative.generic_string()
                                          : r.file.string();
        if (!live && hash.empty())
            versions_->forEachVersion([&](const model::Version& v) {
                for (const auto& e : v.session->media)
                    if (hash.empty() && e.value->path == mediaPath) hash = e.value->hash;
            });
        Id source;
        if (const auto known = model::mediaSourceWithHash(s, hash)) {
            source = *known;
        } else {
            model::MediaSource md;
            md.path = mediaPath;
            md.hash = hash;
            md.format = "wav";
            md.sampleRate = rate;  // the device's (resampled to the plan's rate on playback)
            md.channels = 0;
            if (auto head = audio::inspectWavStream(r.file))
                md.channels = static_cast<int>(head->channels);
            md.frames = r.frames;
            source = t.create(std::move(md));
            if (hash.empty() && commands_ && live) pendingHashes->emplace_back(source, r);
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
        side.frames = take.frames;
        side.punch = take.punch;
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

namespace {
// Why the media folder `setting` (a project's media_dir) names from `folder`
// may not hold files a replayed step writes: it names ".." anywhere (a step
// may make folders, so where one leads must not depend on what exists), or
// it does not resolve inside `folder`. Empty when it may. `missing`
// (optional) receives the folders that do not exist yet, deepest first.
std::string mediaOutside(const fs::path& folder, const std::string& setting,
                         std::vector<fs::path>* missing = nullptr) {
    for (const auto& part : fs::path(setting))
        if (part == "..") return "names '..'";
    if (!detail::resolvedInside(folder, folder / setting, missing))
        return "is outside the project folder";
    return {};
}
}  // namespace

Status EngineApp::replayMayWriteMedia() {
    // A project with no folder (a headless engine's unsaved one) keeps its
    // media in the engine's own <workdir>/media, which no file names. An
    // unsaved project with a session folder is held to that folder: its
    // state may have come from a recovered autosave (ADR-0078).
    const fs::path project = projectDir();
    if (project.empty()) return okStatus();
    auto outside = [&](const std::string& why) {
        return Error("history.media_outside",
                     "the media folder " + mediaDir().string() + " " + why +
                         "; steps replayed in a merge may create files only inside the project "
                         "folder " +
                         project.string(),
                     "/project/media_dir",
                     "set media_dir to a folder inside the project without '..', or merge into "
                     "a branch that has not moved since the fork");
    };
    // Nothing is made anywhere until the deepest folder that exists (a link
    // counts, and is followed) resolves inside the project.
    std::vector<fs::path> made;
    if (std::string why = mediaOutside(project, head().project->mediaDir, &made); !why.empty())
        return outside(why);
    std::reverse(made.begin(), made.end());  // shallowest first
    // Then make the rest and resolve it for real once more: the folders this
    // made are removed again if it is outside after all, and by a merge that
    // fails.
    const fs::path media = mediaDir();
    std::error_code ec;
    fs::create_directories(media, ec);
    if (ec) {
        for (auto it = made.rbegin(); it != made.rend(); ++it) fs::remove(*it, ec);
        return outside("cannot be made (" + ec.message() + ")");
    }
    if (!detail::resolvedInside(project, media)) {
        for (auto it = made.rbegin(); it != made.rend(); ++it) fs::remove(*it, ec);
        return outside("is outside the project folder");
    }
    if (mergeCreated_)  // a failed merge removes them (after its files)
        mergeCreated_->insert(mergeCreated_->begin(), made.begin(), made.end());
    return okStatus();
}

fs::path EngineApp::recoveryRoot() const {
    return projectPath_.empty() ? options_.scratchDir : fs::path();
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
    if (scratchLock_) {
        scratchLock_->releaseAndRemove();
        scratchLock_.reset();
    }
    std::error_code ec;
    fs::remove_all(scratch, ec);
}

// --- unsaved projects left behind (ADR-0078) --------------------------------------------------

namespace {
// The numbered autosaves in `folder`, newest first.
std::vector<fs::path> autosavesIn(const fs::path& folder) {
    std::vector<std::pair<std::uint64_t, fs::path>> found;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (const auto n = autosaveIndex(it->path().filename().string());
            n && it->is_regular_file(ec))
            found.push_back({*n, it->path()});
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

// Whether `folder` holds anything but its lock file (empty folders do not
// count), and the size of its files.
bool sessionContent(const fs::path& folder, std::int64_t* bytes = nullptr) {
    bool content = false;
    std::error_code ec;
    for (fs::recursive_directory_iterator
             it(folder, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec || fs::is_directory(status)) continue;
        if (it.depth() == 0 && it->path().filename() == detail::kSessionLockName) continue;
        content = true;
        if (!bytes) return true;
        if (fs::is_regular_file(status)) {
            std::error_code sizeError;
            const auto size = it->file_size(sizeError);
            if (!sizeError) *bytes += static_cast<std::int64_t>(size);
        }
    }
    return content;
}

// When the run that named the folder <YYYYmmdd-HHMMSS>-<pid> started, in Unix
// milliseconds (the name is local time).
std::optional<std::int64_t> sessionStartedMs(const std::string& name) {
    std::tm t{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (name.size() < 15 || name[8] != '-') return std::nullopt;
    for (std::size_t i = 0; i < 15; ++i)
        if (i != 8 && !std::isdigit(static_cast<unsigned char>(name[i]))) return std::nullopt;
    auto number = [&](std::size_t at, std::size_t n) {
        int v = 0;
        std::from_chars(name.data() + at, name.data() + at + n, v);
        return v;
    };
    year = number(0, 4), month = number(4, 2), day = number(6, 2);
    hour = number(9, 2), minute = number(11, 2), second = number(13, 2);
    t.tm_year = year - 1900, t.tm_mon = month - 1, t.tm_mday = day;
    t.tm_hour = hour, t.tm_min = minute, t.tm_sec = second, t.tm_isdst = -1;
    const std::time_t when = std::mktime(&t);
    if (when == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<std::int64_t>(when) * 1000;
}

std::optional<std::int64_t> fileTimeMs(const fs::path& file) {
    std::error_code ec;
    const auto written = fs::last_write_time(file, ec);
    if (ec) return std::nullopt;
    // The file clock's epoch is unspecified: measure from now on both clocks.
    const auto age = fs::file_time_type::clock::now() - written;
    const auto at = std::chrono::system_clock::now() -
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(age);
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

// Recordings with a sidecar in <folder>/<mediaDir>/rec that no path in
// `used` (relative to the media folder) names.
std::int64_t leftoverRecordings(const fs::path& folder, const fs::path& rec,
                                const std::set<std::string>& used) {
    std::int64_t n = 0;
    std::error_code ec;
    if (!detail::resolvedInside(folder, rec)) return 0;  // not the session's: never looked at
    for (fs::directory_iterator it(rec, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& f = it->path();
        if (detail::isMidiJournal(f)) {  // ADR-0080: no sidecar; the journal says it all
            if (!fs::is_regular_file(fs::symlink_status(f, ec))) continue;
            // Counted as recovery would place it: only with something played.
            if (auto c = detail::readMidiJournal(f, rec); c && !c->events.empty()) ++n;
            continue;
        }
        if (f.extension() != ".wav" || !it->is_regular_file(ec)) continue;
        if (used.count(("rec" / f.filename()).generic_string())) continue;
        const auto journal = detail::audioJournalPath(f);
        if (fs::symlink_status(journal, ec).type() != fs::file_type::not_found) {
            auto side = detail::readAudioJournal(f, rec);
            auto wav = audio::inspectWavStream(f);
            if (side && side->frames && *side->frames > 0 && wav && wav->fileFrames > 0) ++n;
            continue;
        }
        std::error_code sideError;  // a sidecar recovery would read: regular, inside
        if (detail::recoveryFileInside(rec, f) &&
            detail::recoveryFileInside(rec, detail::sidecarPath(f)) &&
            fs::is_regular_file(fs::status(detail::sidecarPath(f), sideError)))
            ++n;
    }
    return n;
}

// What recover() can put back from one WAV in `rec` (ADR-0014): a regular
// file directly in that real folder (recovery rewrites headers, so nothing
// reached through a link), a sidecar that reads, frames, and a placement
// inside the timeline of `s`. With `root` (an unsaved project's session
// folder), the file must resolve inside it too: nothing reached through a
// link out of the session is read or repaired. The listing, the recovery
// itself and a session's preflight all ask this, so none accepts what
// another refuses.
// A MIDI journal (ADR-0080) is checked the same way: it reads (inside its
// folder), it holds at least one event, and the take it places is inside
// the timeline; `midi` is then that take, as stop would have committed it
// up to the crash.
struct RecordingCheck {
    std::optional<detail::RecordingInfo> side;
    std::optional<audio::WavStreamInfo> wav;  // set when it can be put back
    std::optional<RecordedTake> midi;         // or this, for a MIDI journal
    std::optional<Error> problem;             // why not, when it is worth saying
    bool usable() const { return wav.has_value() || midi.has_value(); }
};

RecordingCheck checkRecording(const model::Session& s, const fs::path& rec, const fs::path& f,
                              const fs::path& root) {
    RecordingCheck out;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(rec, ec)) ||
        !fs::is_regular_file(fs::symlink_status(f, ec)) ||
        !fs::equivalent(f.parent_path(), rec, ec) || ec || !detail::recoveryFileInside(root, f))
        return out;
    if (detail::isMidiJournal(f)) {
        auto capture = detail::readMidiJournal(f, rec);
        if (!capture) {
            Error e = std::move(capture).error();
            e.code = "record.journal";
            if (e.path.empty()) e.path = "/";
            if (e.hint.empty()) e.hint = "skip this file; it is not a readable MIDI journal";
            out.problem = std::move(e);
            return out;
        }
        RecordedTake take = detail::placeMidi(*capture);
        out.side = detail::RecordingInfo{take.track,    take.startSample, take.sampleRate,
                                         take.segments, take.loop.start,  take.loop.end};
        out.side->punch = take.punch;
        if (take.midi.empty() || take.frames <= 0) return out;  // nothing was played
        if (auto valid = recordingBounds(s, *out.side, static_cast<std::uint64_t>(take.frames));
            !valid) {
            out.problem = valid.error();
            return out;
        }
        take.file = f;
        out.midi = std::move(take);
        return out;
    }
    // The sidecar is opened only inside the WAV's own folder (itself inside
    // `root`): a link to placement metadata elsewhere is not followed.
    const auto journal = detail::audioJournalPath(f);
    const bool journalPresent = fs::symlink_status(journal, ec).type() != fs::file_type::not_found;
    auto side = journalPresent ? detail::readAudioJournal(f, rec) : detail::readSidecar(f, rec);
    // A journal is authoritative while present: never flatten an unreadable one.
    if (!side) {
        if (journalPresent || fs::exists(detail::sidecarPath(f), ec)) {
            Error e = std::move(side).error();
            e.code = journalPresent ? "record.journal" : "record.sidecar";
            if (e.path.empty()) e.path = "/";
            if (e.hint.empty()) e.hint = "restore a valid recording sidecar or skip this file";
            out.problem = std::move(e);
        }
        return out;
    }
    out.side = *side;
    auto wav = audio::inspectWavStream(f);
    if (!wav) {
        Error e = std::move(wav).error();
        if (e.path.empty()) e.path = "/file";
        if (e.hint.empty()) e.hint = "restore the recording audio or skip this file";
        out.problem = std::move(e);
        return out;
    }
    if (side->frames)
        wav->fileFrames = std::min(wav->fileFrames, static_cast<std::uint64_t>(*side->frames));
    if (wav->fileFrames == 0) return out;
    if (auto valid = recordingBounds(s, *side, wav->fileFrames, wav->sampleRate); !valid) {
        out.problem = valid.error();
        return out;
    }
    out.wav = *wav;
    return out;
}

// Recordings in `rec` that recover() could put back into `s`.
std::int64_t usableRecordings(const model::Session& s, const fs::path& rec, const fs::path& root) {
    std::int64_t n = 0;
    std::error_code ec;
    if (!detail::resolvedInside(root, rec)) return 0;
    for (fs::directory_iterator it(rec, ec), end; !ec && it != end; it.increment(ec))
        if ((it->path().extension() == ".wav" || detail::isMidiJournal(it->path())) &&
            checkRecording(s, rec, it->path(), root).usable())
            ++n;
    return n;
}

// A recovered session's media live in its folder (ADR-0078): on each branch
// whose media_dir does not resolve inside it (resolvedInside, as it is on
// disk now: "a/../media" that does is kept), one more version puts it back
// to the default. Only a file not written by this program names such a
// folder. If the default does not resolve inside either (a link out), the
// recordings there are not recovered: recover() asks the same test.
Status confineMedia(model::VersionStore& store, const fs::path& folder,
                    std::vector<RecordingWarning>& warnings) {
    const std::string current = store.branch();
    const std::string fallback = model::Project{}.mediaDir;
    for (const std::string& name : store.branches()) {
        SUPPLE_TRY(store.switchBranch(name));
        const auto head = store.head();
        const std::string was = head->project->mediaDir;
        if (detail::resolvedInside(folder, folder / was)) continue;
        // The default itself leads out (a link): nothing better to point at;
        // recoverSession says its recordings are not recovered.
        if (was == fallback) continue;
        model::EditTxn t(*head, store.nextVersion());
        model::Project project = t.project();
        project.mediaDir = fallback;
        t.setProject(std::move(project));
        auto changes = t.changes();
        auto created = t.created();
        auto confined = t.commit();
        if (!confined) return std::move(confined).error();
        model::TxnRecord record;
        record.label = "Keep media in the session folder";
        record.origin = "engine";
        auto committed = store.commit(std::move(*confined), std::move(record), std::move(changes),
                                      std::move(created));
        if (!committed) return std::move(committed).error();
        warnings.push_back({"recover.media_dir",
                            "the media folder " + was + " of branch " + name +
                                " is outside the session folder; recovered, it uses " + fallback +
                                " in the session folder, so media only in " + was + " are missing",
                            "/project/media_dir",
                            "copy the media into the session's " + fallback + " folder", "", ""});
    }
    return store.switchBranch(current);
}

constexpr std::chrono::hours kPruneAfter{1};
}  // namespace

void EngineApp::holdScratch() {
    const fs::path& scratch = options_.scratchDir;
    if (scratch.empty() || scratchLock_) return;
    // Another run may hold the lock for a moment while it looks at this
    // folder (recoverableSessions()): try again briefly.
    for (int attempt = 0; attempt < 40 && !scratchLock_; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::error_code ec;
        fs::create_directories(scratch, ec);
        if (auto lock = detail::SessionLock::tryAcquire(scratch))
            scratchLock_ = std::make_unique<detail::SessionLock>(std::move(*lock));
    }
}

void EngineApp::releaseScratch() {
    const fs::path& scratch = options_.scratchDir;
    if (scratch.empty()) return;
    std::error_code ec;
    for (const fs::path& f : autosavesIn(scratch)) fs::remove(f, ec);
    if (scratchLock_) {
        scratchLock_->releaseAndRemove();
        scratchLock_.reset();
    }
    fs::remove(scratch, ec);  // only when empty: media whose move failed stay
}

Result<fs::path> EngineApp::sessionFolder(const std::string& session) const {
    const fs::path& own = options_.scratchDir;
    auto unknown = [&](std::string why) {
        return Error("recover.unknown_session", std::move(why), "/session",
                     "use a session project.recoverable lists");
    };
    if (own.empty()) return unknown("this app keeps no unsaved projects in folders");
    const fs::path name(session);
    if (session.empty() || name.filename() != name || session == "." || session == ".." ||
        session.find_first_of("/\\") != std::string::npos)
        return unknown("\"" + session + "\" is not a session name");
    const fs::path folder = own.parent_path() / name;
    std::error_code ec;
    if (name == own.filename() || !fs::is_directory(fs::symlink_status(folder, ec)))
        return unknown("there is no unsaved project " + session + " left behind");
    return folder;
}

std::vector<RecoverableSession> EngineApp::recoverableSessions(
    std::vector<RecordingWarning>* warnings) const {
    std::vector<RecoverableSession> out;
    const fs::path& own = options_.scratchDir;
    if (own.empty()) return out;
    std::vector<fs::path> folders;
    std::error_code ec;
    for (fs::directory_iterator it(own.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() != own.filename() && fs::is_directory(it->symlink_status(ec)))
            folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders) {
        const auto lock = detail::SessionLock::tryAcquire(folder);
        if (!lock) continue;  // a running app's
        RecoverableSession s;
        s.session = folder.filename().string();
        if (!sessionContent(folder, &s.bytes)) continue;
        s.startedMs = sessionStartedMs(s.session);
        std::string media = model::Project{}.mediaDir;
        std::set<std::string> used;
        for (const fs::path& file : autosavesIn(folder)) {
            auto text = detail::readRecoveryText(folder, file, std::size_t(1) << 30);
            auto opened = text ? model::parseProjectFile(*text, nullptr, replayableStep)
                               : Result<model::OpenedProject>(std::move(text).error());
            if (!opened) {
                if (warnings)
                    warnings->push_back({"recover.autosave",
                                         "an autosave of " + s.session +
                                             " cannot be read: " + opened.error().message,
                                         "/sessions", "an older autosave is used instead", "",
                                         s.session + "/" + file.filename().string()});
                continue;
            }
            const auto head = opened->store.head();
            s.autosavedMs = fileTimeMs(file);
            s.name = head->project->name;
            for (const auto& t : head->tracks)
                if (t.value->parent) ++s.tracks;  // masters are not counted
            s.branches = opened->store.branches();
            media = head->project->mediaDir;
            opened->store.forEachVersion([&](const model::Version& v) {
                for (const auto& e : v.session->media)
                    if (const fs::path p = e.value->path; p.is_relative())
                        used.insert(p.lexically_normal().generic_string());
            });
            break;
        }
        s.recordings = leftoverRecordings(folder, folder / media / "rec", used);
        out.push_back(std::move(s));
    }
    return out;
}

std::int64_t EngineApp::recoverableSessionCount() const {
    const fs::path& own = options_.scratchDir;
    if (own.empty()) return 0;
    std::int64_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(own.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().filename() == own.filename() || !fs::is_directory(it->symlink_status(ec)))
            continue;
        if (detail::SessionLock::tryAcquire(it->path()) && sessionContent(it->path())) ++n;
    }
    return n;
}

void EngineApp::pruneEmptySessions() {
    const fs::path& own = options_.scratchDir;
    if (own.empty()) return;
    std::vector<fs::path> empty;
    std::error_code ec;
    for (fs::directory_iterator it(own.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path& folder = it->path();
        if (folder.filename() == own.filename() || !fs::is_directory(it->symlink_status(ec)))
            continue;
        // A run starting now may not hold its new folder's lock yet: only
        // folders untouched for a while go.
        std::error_code timeError;
        const auto written = fs::last_write_time(folder, timeError);
        if (timeError || fs::file_time_type::clock::now() - written < kPruneAfter) continue;
        auto lock = detail::SessionLock::tryAcquire(folder);
        if (!lock || sessionContent(folder)) continue;
        for (fs::directory_iterator child(folder, ec), last; !ec && child != last;
             child.increment(ec))
            if (child->path().filename() != detail::kSessionLockName) {
                std::error_code ignored;
                fs::remove_all(child->path(), ignored);
            }
        lock->releaseAndRemove();
        empty.push_back(folder);
    }
    for (const fs::path& folder : empty) fs::remove(folder, ec);
}

Result<std::vector<EngineApp::RecoveryKeep>> EngineApp::keepForFirstSave(
    const fs::path& folder, const std::vector<RawAutosave>& raw, const std::string& path) {
    std::vector<RecoveryKeep> kept;
    auto undo = [&] {
        std::error_code ec;
        for (const RecoveryKeep& k : kept) fs::remove(k.copy, ec);
    };
    for (const auto& [name, bytes, suffix] : raw) {
        const auto data =
            std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        Status written = Error("file.exists", "too many kept copies of " + name);
        for (int n = 1; n < 1000; ++n) {
            const fs::path copy =
                folder / (name + suffix + (n == 1 ? "" : "-" + std::to_string(n)) + ".keep");
            written = writeFileNew(copy, data);
            if (written) {
                kept.push_back({copy, suffix});
                break;
            }
            if (written.error().code != "file.exists") break;
        }
        if (!written) {
            undo();
            return Error(
                "recover.keep",
                "could not keep a copy of " + name +
                    ", which saving the recovered project would delete: " + written.error().message,
                path,
                "make " + folder.string() +
                    " writable (or free space) and retry; nothing was changed");
        }
    }
    return kept;
}

Status EngineApp::publishRecoveryKeeps(const fs::path& file,
                                       std::vector<schema::Warning>* warnings) {
    for (auto it = recoveryKeep_.begin(); it != recoveryKeep_.end();) {
        auto bytes =
            detail::readRecoveryFile(it->copy.parent_path(), it->copy, std::size_t(1) << 30);
        const std::string name = file.filename().string() + it->suffix;
        fs::path backup;
        Status kept = bytes ? Status(Error("file.exists", "")) : Status(bytes.error());
        for (int n = 1; bytes && !kept && kept.error().code == "file.exists" && n < 10000; ++n) {
            backup = file.parent_path() /
                     (n == 1 ? name + ".bak" : name + "-" + std::to_string(n) + ".bak");
            kept = writeFileNew(backup, *bytes);
        }
        if (!kept)
            return Error("project.history_backup",
                         "could not keep a copy of the recovered autosave before its folder is "
                         "released: " +
                             kept.error().message,
                         backup.string(),
                         "make the project folder writable, or save to another file");
        if (warnings)
            warnings->push_back({"project.history_backup",
                                 "kept the recovered session's " +
                                     std::string(it->suffix == ".pre-branches"
                                                     ? "autosave with its unrestored branches"
                                                     : "unreadable autosave") +
                                     " as " + backup.filename().string(),
                                 backup.string()});
        std::error_code ec;
        fs::remove(it->copy, ec);
        it = recoveryKeep_.erase(it);
    }
    return okStatus();
}

Result<std::vector<fs::path>> EngineApp::autosaveCandidates(const std::string& name) const {
    auto none = [] {
        return Error("recover.no_autosave", "there is no autosave newer than the saved file",
                     "/autosave", "open the project as saved; nothing was lost");
    };
    if (projectPath_.empty()) return none();
    std::error_code ec;
    // Newer than the file: numbered above its autosaved_through, or, for a
    // file without one (before 2.15), written after it.
    fs::file_time_type written{};
    if (!autosavedThrough_) {
        written = fs::last_write_time(projectPath_, ec);
        if (ec) return none();
    }
    auto newer = [&](const fs::path& file, std::optional<std::uint64_t> through,
                     fs::file_time_type time) {
        if (through) return autosaveIndex(file.filename().string()).value_or(0) > *through;
        std::error_code timeError;
        const auto t = fs::last_write_time(file, timeError);
        return !timeError && t > time;
    };
    const fs::path folder = autosaveDir();
    // A named one is the one that was offered: it must be newer than the
    // file as this project was opened, so a save since (the question before
    // opening it offers Save) does not make it unopenable, and a name offered
    // for another project opens nothing older in this one.
    std::vector<fs::path> candidates;
    if (!name.empty()) {
        const fs::path file = folder / name;
        if (!autosaveIndex(name) || fs::path(name).filename() != fs::path(name) ||
            !fs::is_regular_file(fs::symlink_status(file, ec)))
            return Error("recover.no_autosave", name + " is not an autosave of this project",
                         "/autosave_file",
                         "omit autosave_file to open the newest autosave newer than the file");
        const bool asOpened = openedFile_ && openedFile_->path == projectPath_;
        const auto openedThrough = asOpened ? openedFile_->through : autosavedThrough_;
        const auto openedTime = asOpened ? openedFile_->time : written;
        if (!newer(file, openedThrough, openedTime))
            return Error("recover.no_autosave",
                         name + " is not newer than " + projectPath_.filename().string() +
                             " as it was opened",
                         "/autosave_file",
                         "open the project as saved, or omit autosave_file to open the newest "
                         "autosave newer than the file");
        candidates.push_back(file);
    } else {
        for (const fs::path& file : autosavesIn(folder))  // newest first
            if (newer(file, autosavedThrough_, written)) candidates.push_back(file);
    }
    return candidates;
}

Result<SessionRecovery> EngineApp::recoverAutosave(const std::string& name) {
    auto none = [] {
        return Error("recover.no_autosave", "there is no autosave newer than the saved file",
                     "/autosave", "open the project as saved; nothing was lost");
    };
    auto listed = autosaveCandidates(name);
    if (!listed) return std::move(listed).error();
    const std::vector<fs::path> candidates = std::move(*listed);
    const fs::path folder = autosaveDir();
    SessionRecovery out;
    // The same rule as a session's (ADR-0078): what this recovery cannot
    // fully restore is kept for the first save to publish beside the file.
    std::vector<RawAutosave> keep;
    for (const fs::path& file : candidates) {
        std::vector<schema::Warning> parsed;
        auto text = detail::readRecoveryText(folder, file, std::size_t(1) << 30);
        auto opened = text ? model::parseProjectFile(*text, &parsed, replayableStep)
                           : Result<model::OpenedProject>(text.error());
        if (!opened && !name.empty())
            return Error("recover.no_autosave", name + " cannot be read: " + opened.error().message,
                         "/autosave_file",
                         "open the project as saved, or omit autosave_file to try the others");
        if (!opened && text)
            keep.emplace_back(file.filename().string(), *text, ".unreadable-autosave");
        if (!opened) {
            out.warnings.push_back(
                {"recover.autosave",
                 file.filename().string() +
                     " cannot be read, an older autosave was tried: " + opened.error().message,
                 "/autosave", "", "", file.filename().string()});
            continue;
        }
        for (auto& w : parsed)
            out.warnings.push_back({w.code, w.message, w.path, "", "", file.filename().string()});
        if (opened->branchesDropped)
            out.warnings.push_back({"project.history",
                                    "opened main alone: the autosave's branches could not be "
                                    "restored (" +
                                        opened->branchesDropped->message + ")",
                                    "/branches", "", "", file.filename().string()});
        if (opened->branchesDropped)
            keep.emplace_back(file.filename().string(), *text, ".pre-branches");
        {  // a plug-in a crash report points at, before anything of it loads
            std::vector<schema::Warning> suspect;
            warnStoreSuspects(opened->store, &suspect);
            for (auto& w : suspect)
                out.warnings.push_back(
                    {w.code, w.message, w.path, "", "", file.filename().string()});
        }
        // Kept before anything is replaced: without the copies, the first
        // save would delete the only source of what this cannot restore.
        auto kept = keepForFirstSave(folder, keep, "/autosave");
        if (!kept) return std::move(kept).error();
        std::vector<schema::Warning> replaced;
        // The file is unchanged: what it holds, and how it was opened, stay.
        const auto fileThrough = autosavedThrough_;
        const auto fileOpened = openedFile_;
        if (Status st = replaceProject(std::move(opened->store), projectPath_, {}, &replaced);
            !st) {
            std::error_code removeError;
            for (const RecoveryKeep& k : *kept) fs::remove(k.copy, removeError);
            return std::move(st).error();
        }
        autosavedThrough_ = fileThrough;
        openedFile_ = fileOpened;
        recoveryKeep_ = std::move(*kept);
        for (auto& w : replaced) out.warnings.push_back({w.code, w.message, w.path, "", "", ""});
        out.autosave = file.filename().string();
        return out;
    }
    return none();
}

Result<SessionRecovery> EngineApp::recoverSession(const std::string& session) {
    auto folder = sessionFolder(session);
    if (!folder) return std::move(folder).error();
    auto lock = detail::SessionLock::tryAcquire(*folder);
    if (!lock)
        return Error("recover.busy", session + " belongs to an app that is running", "/session",
                     "recover it in that app, or quit that app and retry");
    if (!sessionContent(*folder))
        return Error("recover.unknown_session", "there is nothing in " + session + " to recover",
                     "/session", "use a session project.recoverable lists");
    SessionRecovery out;
    std::optional<model::VersionStore> store;
    // Raw autosaves that must outlive the first save.
    std::vector<RawAutosave> keep;
    for (const fs::path& file : autosavesIn(*folder)) {
        std::vector<schema::Warning> parsed;
        auto text = detail::readRecoveryText(*folder, file, std::size_t(1) << 30);
        auto opened = text ? model::parseProjectFile(*text, &parsed, replayableStep)
                           : Result<model::OpenedProject>(text.error());
        if (!opened && text)
            keep.emplace_back(file.filename().string(), *text, ".unreadable-autosave");
        if (!opened) {
            out.warnings.push_back(
                {"recover.autosave",
                 file.filename().string() +
                     " cannot be read, an older autosave was tried: " + opened.error().message,
                 "/session", "", "", file.filename().string()});
            continue;
        }
        for (auto& w : parsed)
            out.warnings.push_back({w.code, w.message, w.path, "", "", file.filename().string()});
        if (opened->branchesDropped)
            out.warnings.push_back({"project.history",
                                    "opened main alone: the autosave's branches could not be "
                                    "restored (" +
                                        opened->branchesDropped->message + ")",
                                    "/branches", "", "", file.filename().string()});
        if (opened->branchesDropped)
            keep.emplace_back(file.filename().string(), *text, ".pre-branches");
        {
            std::vector<schema::Warning> suspect;
            warnStoreSuspects(opened->store, &suspect);
            for (auto& w : suspect)
                out.warnings.push_back(
                    {w.code, w.message, w.path, "", "", file.filename().string()});
        }
        store = std::move(opened->store);
        out.autosave = file.filename().string();
        break;
    }
    // Without a readable autosave, only recordings that can really be put
    // back make a recovery: a blank project in place of the user's work, and
    // the unreadable autosaves deleted by its first save, would be worse.
    std::optional<Id> recoveredTrack;
    if (store) {
        // An unsaved project's media live in its session folder: a media_dir
        // that leads elsewhere was not written by this program, and nothing
        // recorded, rendered or replayed may follow it out (ADR-0078).
        SUPPLE_TRY(confineMedia(*store, *folder, out.warnings));
    } else {
        // The tracks they were recorded on are in the unreadable autosave:
        // they go on one new channel.
        model::Session blank = model::Session::create(model::Project{});
        model::EditTxn t(blank, blank.version);
        model::Track tr;
        tr.type = model::StripType::Channel;
        tr.name = "Recovered";
        tr.parent = model::kMaster;
        const Id id = t.create(tr);
        t.modify<model::Track>(model::kMaster, [&](model::Track& m) { m.children.push_back(id); });
        auto made = t.commit();
        if (!made) return std::move(made).error();
        // Placed by the same rules recover() uses, against this project.
        if (usableRecordings(*made, *folder / made->project->mediaDir / "rec", *folder) == 0)
            return Error("recover.damaged", "nothing in " + session + " can be opened", "/session",
                         "delete it with project.recoverable.delete, or copy what you need out "
                         "of " +
                             folder->string() + " first");
        store.emplace(std::move(*made));
        recoveredTrack = id;
    }
    // Kept before anything is replaced: without the copies, the first save
    // would delete the only source of what this cannot restore.
    auto kept = keepForFirstSave(*folder, keep, "/session");
    if (!kept) return std::move(kept).error();
    auto held = std::make_unique<detail::SessionLock>(std::move(*lock));
    std::vector<schema::Warning> replaced;
    Status replacing = (replaceProject(
        std::move(*store), {},
        [&]() -> Status {
            // The left-behind folder becomes this run's: its media resolve
            // there, autosaves continue its numbering.
            options_.scratchDir = *folder;
            scratchLock_ = std::move(held);
            return okStatus();
        },
        &replaced));
    if (!replacing) {
        std::error_code removeError;
        for (const RecoveryKeep& k : *kept) fs::remove(k.copy, removeError);
        return std::move(replacing).error();
    }
    recoveryKeep_ = std::move(*kept);
    for (auto& w : replaced) out.warnings.push_back({w.code, w.message, w.path, "", "", ""});
    if (std::error_code ec;
        fs::exists(recordDir(), ec) && !detail::resolvedInside(*folder, recordDir()))
        out.warnings.push_back(
            {"recover.media_outside",
             "the recordings folder " + recordDir().string() +
                 " leads outside the session folder: its recordings are not recovered, and "
                 "nothing there was read or changed",
             "/project/media_dir", "copy the recordings into the session folder, then recover them",
             "", ""});
    if (!recoverable().empty()) {
        auto r = recover({}, recoveredTrack, &out.warnings);
        if (r)
            out.recovered = std::move(*r);
        else
            out.warnings.push_back(
                {r.error().code, r.error().message + "; the recordings stay recoverable",
                 r.error().path, "use project.recover with files and track to place them", "", ""});
    }
    return out;
}

Result<EngineApp::ProjectPlugins> EngineApp::recoverPlugins(
    const std::string& session, const std::string& autosaveFile,
    std::vector<schema::Warning>* warnings) {
    fs::path folder;
    std::vector<fs::path> candidates;
    ProjectPlugins out;
    if (!session.empty()) {
        // What recoverSession reads: the folder's autosaves, newest first, the first that opens.
        auto found = sessionFolder(session);
        if (!found) return std::move(found).error();
        folder = *found;
        if (!sessionContent(folder))
            return Error("recover.unknown_session",
                         "there is nothing in " + session + " to recover", "/session",
                         "use a session project.recoverable lists");
        candidates = autosavesIn(folder);
        out.source = "session";
    } else {
        auto listed = autosaveCandidates(autosaveFile);
        if (!listed) return std::move(listed).error();
        folder = autosaveDir();
        candidates = std::move(*listed);
        if (candidates.empty())
            return Error("recover.no_autosave", "there is no autosave newer than the saved file",
                         "/autosave", "open the project as saved; nothing was lost");
        out.source = "autosave";
    }
    out.catalogueEmpty = !catalogue_ || catalogue_->empty();
    for (const fs::path& file : candidates) {
        auto text = detail::readRecoveryText(folder, file, std::size_t(1) << 30);
        if (!text) continue;
        std::vector<schema::Warning> parsed;
        auto opened = model::parseProjectFile(*text, &parsed, replayableStep);
        if (!opened) continue;  // recover tries the next older one, so does this
        if (warnings)
            for (auto& w : parsed) warnings->push_back(std::move(w));
        out.path = file.string();
        out.autosave = file.filename().string();
        out.hash = "sha256:" + sha256Hex(*text);
        std::vector<NamedHead> heads;
        for (const auto& name : opened->store.branches())
            if (auto head = opened->store.head(name)) heads.emplace_back(name, std::move(head));
        out.plugins = gatherPlugins(heads, opened->store.branch());
        annotateSuspects(out.plugins);
        return out;
    }
    // As project.recover answers: an autosave nothing could read is "no autosave"; a session
    // with nothing readable puts back recordings only, which name no plug-in.
    if (out.source == "autosave") {
        if (!autosaveFile.empty())
            return Error("recover.no_autosave", autosaveFile + " cannot be read", "/autosave_file",
                         "open the project as saved, or omit autosave_file to try the others");
        return Error("recover.no_autosave", "there is no autosave newer than the saved file",
                     "/autosave", "open the project as saved; nothing was lost");
    }
    return out;
}

Status EngineApp::deleteSession(const std::string& session) {
    auto folder = sessionFolder(session);
    if (!folder) return std::move(folder).error();
    auto lock = detail::SessionLock::tryAcquire(*folder);
    if (!lock)
        return Error("recover.busy", session + " belongs to an app that is running", "/session",
                     "quit the app using it and retry");
    std::error_code ec;
    for (fs::directory_iterator child(*folder, ec), last; !ec && child != last; child.increment(ec))
        if (child->path().filename() != detail::kSessionLockName) {
            std::error_code removeError;
            fs::remove_all(child->path(), removeError);
            if (removeError)
                return Error("recover.delete", removeError.message(), "/session",
                             "make " + child->path().string() + " writable and retry");
        }
    lock->releaseAndRemove();
    fs::remove(*folder, ec);
    if (ec)
        return Error("recover.delete", ec.message(), "/session",
                     "make " + folder->string() + " writable and retry");
    return okStatus();
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
#if defined(_WIN32)
        if (!ec) fs::permissions(staged, fs::perms::owner_write, fs::perm_options::add, ec);
#endif
        if (ec)
            return Error("project.media_move", ec.message(), (from / rel).string(),
                         "save to a writable project directory with sufficient space and keep all "
                         "scratch media readable during the save");
        auto original = sha256File(from / rel), copied = sha256File(staged);
        if (!original || !copied || *original != *copied)
            return Error("project.media_move", "staged media verification failed", rel.string(),
                         "keep source media unchanged during the save and save into a directory "
                         "with reliable writable storage");
        SUPPLE_TRY(syncFile(staged));
    }
    for (const auto& rel : files) {
        const auto target = dest / rel;
        fs::create_directories(target.parent_path(), ec);
        if (!ec && detail::files::publishNew(stage / rel, target) != 0)
            ec = std::error_code(errno, std::generic_category());
        if (ec)
            return Error("project.media_move", ec.message(), target.string(),
                         "save to a writable project directory with sufficient space and keep all "
                         "scratch media readable during the save");
        placed.push_back(target);
        SUPPLE_TRY(syncParentDirectories(target));
    }
    auto saved = publish();
    if (!saved) {
        // Publication may already have replaced the file. Keep its audio
        // as well as the originals; rollback must never strand that project.
        if (saved.error().code == "project.sync") published = true;
        return saved;
    }
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
    for (const fs::path& f : recordingJournalsPending_) used.insert(comparable(f));
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".wav" || detail::isMidiJournal(it->path()))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    const fs::path media = mediaDir();
    for (const fs::path& f : files) {
        if (used.count(comparable(f))) continue;
        const RecordingCheck check = checkRecording(head(), dir, f, recoveryRoot());
        if (!check.usable()) {
            if (warnings && check.problem) {
                const Error& e = *check.problem;
                warnings->push_back({e.code, e.message, e.path, e.hint,
                                     check.side ? check.side->track.str() : std::string(),
                                     f.lexically_relative(media).generic_string()});
            }
            continue;
        }
        const detail::RecordingInfo* side = &*check.side;
        RecoverableInfo r;
        r.file = f.lexically_relative(media).generic_string();
        r.track = side->track.str();
        r.startSeconds = static_cast<double>(side->startSample) / side->sampleRate;
        if (check.midi) {
            r.seconds = static_cast<double>(check.midi->frames) / check.midi->sampleRate;
            r.midiEvents = static_cast<std::int64_t>(check.midi->midi.size());
        } else {
            r.seconds = static_cast<double>(check.wav->fileFrames) / check.wav->sampleRate;
            r.channels = check.wav->channels;
        }
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
        // Again, as the preflight and the listing ask: it may have changed.
        const RecordingCheck check = checkRecording(s, recordDir(), t.file, recoveryRoot());
        if (!check.usable() && !check.problem)  // gone or replaced since it was listed
            return Error("recover.unknown_file", r->file + " is not a recording to recover", {},
                         "choose a file listed by project.recoverable for the current project");
        if (!check.usable()) {
            warn(*check.problem, r->file, r->track);
            continue;
        }
        const detail::RecordingInfo* side = &*check.side;
        if (check.midi) {  // placed as stop places it (ADR-0080)
            t = *check.midi;
            // Note Out addresses of routes this project no longer has: the
            // events stay, unaddressed. Each voice keeps its own identity
            // (numbered below a device's -1), so overlapping notes of one
            // pitch still pair as stop paired them.
            std::map<std::pair<model::Id, int>, int> unaddressed;
            for (RecordedMidiEvent& e : t.midi)
                if (e.route != model::Id{} && !s.routes.find(e.route)) {
                    const auto [it, added] = unaddressed.try_emplace(
                        {e.route, e.voice}, -2 - static_cast<int>(unaddressed.size()));
                    (void)added;
                    e.route = {};
                    e.voice = it->second;
                }
        }
        t.punch = side->punch.value_or(RecordedPunch{});
        t.track = track.value_or(side->track);
        if (!s.tracks.find(t.track))
            return Error("recover.no_track",
                         r->file + " was recorded on " + t.track.str() + ", which is gone",
                         "/track", "give a track to put it on");
        // A master's own recordings recover onto it (ADR-0057: audio items).
        if (s.tracks.find(t.track)->type == model::StripType::Buss)
            return Error("track.strip_type", "recover recordings onto a channel or master",
                         "/track", "choose a child channel of the buss instead");
        if (!check.midi) {
            t.frames = static_cast<std::int64_t>(check.wav->fileFrames);
            t.startSample = side->startSample;
            t.sampleRate = side->sampleRate;
            t.segments = side->segments;  // loop passes, when the recording ended normally
            t.loop = {side->loopStart, side->loopEnd};
        }
        takes.push_back(std::move(t));
    }
    for (auto it = takes.begin(); it != takes.end();) {
        RecordedTake& t = *it;
        if (t.isMidi) {  // nothing to repair; checked just above
            ++it;
            continue;
        }
        if (!inRecordDir(t.file))  // replaced since it was listed
            return Error("recover.unknown_file", t.file.string() + " is not a recording to recover",
                         {}, "choose a file listed by project.recoverable for the current project");
        // Checked again right before the header is rewritten.
        Status inside = detail::recoveryFileInside(recoveryRoot(), t.file);
        auto frames = inside ? audio::repairWavStream(t.file)
                             : Result<std::uint64_t>(std::move(inside).error());
        if (!frames) {
            warn(frames.error(), t.file.lexically_relative(mediaDir()), t.track.str());
            it = takes.erase(it);
            continue;
        }
        t.frames = std::min(t.frames, static_cast<std::int64_t>(*frames));
        const auto journal = detail::audioJournalPath(t.file);
        std::error_code ec;
        if (fs::is_regular_file(fs::symlink_status(journal, ec))) t.journal = journal;
        // Keep trusted placement after the next save retires the journal.
        detail::RecordingInfo side{t.track,    t.startSample, t.sampleRate,
                                   t.segments, t.loop.start,  t.loop.end};
        side.frames = t.frames;
        side.punch = t.punch;
        auto metadata = detail::writeSidecar(t.file, side);
        if (!metadata) {
            warn(metadata.error(), t.file.lexically_relative(mediaDir()), t.track.str());
            it = takes.erase(it);
            continue;
        }
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

float EngineApp::moduleOutputLevel(model::Id module, std::string_view port) const {
    if (!engine_ || !engine_->running() || !engine_->transport().playing ||
        !head().modules.contains(module))
        return 0.0f;
    const auto* instance = moduleInstance(module);
    if (!instance) return 0.0f;
    return instance->outputLevel(instance->type().output(port));
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
    // An edit that a reload since the last tick found would never play (a
    // seek's, say) is reported first: a failure of this load would take it.
    noteUnplayedEdit();
    auto st = engine_->load(head(), projectDir());
    if (st) loadedHead_ = versions_->head();
    if (!st) {
        // This failure also ends an edit deferred before it: neither played.
        if (engine_->takeUnplayedEdit()) loadedHead_ = engine_->latestSession();
        emit("engine.error", json::Value(st.error().toString()));
    }
    engine_->service();
    noteUnplayedEdit();
}

void EngineApp::noteUnplayedEdit() {
    // An edit that playrate scheduling deferred (load() returned success, so
    // loadedHead_ took it) and that then failed to compile never played: say
    // so as a failed load does, and report the version that plays.
    auto unplayed = engine_ ? engine_->takeUnplayedEdit() : std::nullopt;
    if (!unplayed) return;
    loadedHead_ = engine_->latestSession();
    emit("engine.error", json::Value("an edit deferred by playrate scheduling never played: " +
                                     unplayed->toString() + "; the engine plays version " +
                                     std::to_string(loadedHead_ ? loadedHead_->version : 0) +
                                     " (undo or change the edit)"));
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
           command == "branch.render-ab" || command == "rack.capture";
}

std::uint64_t EngineApp::clockMs() const {
    if (fixedClock_) return fixedClock_;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void EngineApp::invoked(std::string_view command, int, const json::Value& args, cmd::Kind kind) {
    // context.write records the points it resolved to (recordInvocation), not its text.
    if (txn_ && kind == cmd::Kind::Edit && command != "context.write")
        record_.invocations.push_back({std::string(command), args});
}

void EngineApp::recordInvocation(std::string command, json::Value args) {
    if (txn_) record_.invocations.push_back({std::move(command), std::move(args)});
}

void EngineApp::setupFixture(cmd::Dispatcher& d, std::string_view fixture) {
    if (fixture == "empty") return;
    if (fixture == "midi_ports") {
        (void)setMidiPreferences({{"Test port", json::Value::object()}},
                                 {{"Test port", json::Value::object()}}, false);
        return;
    }
    if (fixture == "engine_running") {  // the dummy backend: no device
        auto st = startEngine("dummy", 256);
        assert(st && "fixture engine failed");
        (void)st;
        return;
    }
    if (fixture == "audition_file") {  // + media/tone.wav: two seconds of stereo at 48 kHz
        auto st = startEngine("dummy", 256);
        assert(st && "fixture engine failed");
        (void)st;
        audio::AudioData tone;
        tone.sampleRate = 48000;
        tone.channels.assign(2, std::vector<float>(96000));
        for (std::size_t i = 0; i < tone.frames(); ++i)
            tone.channels[0][i] = tone.channels[1][i] =
                static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * 440.0 *
                                                   static_cast<double>(i) / tone.sampleRate));
        std::error_code ec;
        fs::create_directories(mediaDir(), ec);
        const Status written = audio::writeWav(mediaDir() / "tone.wav", tone);
        assert(written && "fixture audio failed");
        (void)written;
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
    // Beside this run's unsaved folder, one a crashed run left: project
    // "Sketch" autosaved with tr_1 "Vocal", then rec/Vocal-1.wav recorded and
    // never committed (ADR-0078).
    if (fixture == "session_left_behind") {
        const fs::path unsaved = options_.workdir / "unsaved";
        options_.scratchDir = unsaved / "20261114-090000-4242";
        model::Project sketch;
        sketch.name = "Sketch";
        auto st = newProject(std::move(sketch));
        assert(st && "fixture project failed");
        run("track.add", R"({"name": "Vocal"})");
        autosave();
        setupFixture(d, "recording_take_file");
        scratchLock_.reset();  // the run ends without discarding its folder
        options_.scratchDir = unsaved / "20261114-100000-1";
        st = newProject(model::Project{});
        assert(st && "fixture project failed");
        (void)st;
        return;
    }
    if (fixture == "recording_interrupted" || fixture == "recording_take_file") {
        // tr_1 "Vocal" + rec/Vocal-1.wav, never committed
        if (fixture == "recording_interrupted") run("track.add", R"({"name": "Vocal"})");
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
        fixture == "song_with_rack_learn" || fixture == "song_with_rack_clip" ||
        fixture == "song_with_note_out" || fixture == "song_with_rack_buss") {
        // + rk_1 on tr_1: mo_1 Out (tr_1's input), mo_2 Notes, mo_3 VCA; cb_1 mo_3.out -> mo_1.l
        run("rack.add", R"({"track": "tr_1"})");
        run("module.add", R"({"rack": "rk_1", "type": "notes", "x": 40, "y": 40})");
        run("module.add", R"({"rack": "rk_1", "type": "vca", "x": 300, "y": 40})");
        run("cable.add", R"({"from": "mo_3.out", "to": "mo_1.l"})");
        if (fixture == "song_with_rack") return;
        if (fixture == "song_with_rack_buss") {  // + tr_2 "Verb", a buss (ADR-0075)
            run("track.add", R"({"name":"Verb","type":"buss"})");
            return;
        }
        if (fixture == "song_with_note_out") {
            run("module.add", R"({"rack":"rk_1","type":"notes_out"})");
            run("track.add", R"({"name":"Note destination"})");
            return;
        }
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
    if (fixture == "song_with_crash_report") {  // + one earlier run's report, 0123…cdef
        const auto st =
            diagnostics::writeSampleReport("0123456789abcdef0123456789abcdef", diagnosticsDir());
        assert(st && "fixture crash report failed");
        (void)st;
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
    if (fixture ==
        "song_with_context") {  // cx_1 Eb major, cx_2 Ebmaj7 and cx_3 Cm7 (bar 2), rg_1 Verse
        run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
        run("chord.set", R"({"at":0,"symbol":"Ebmaj7"})");
        run("chord.set", R"({"at":4,"symbol":"Cm7"})");
        run("region.add", R"({"start":0,"end":8,"name":"Verse"})");
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
