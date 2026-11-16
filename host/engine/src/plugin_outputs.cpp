// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/app.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "supple/base/sha256.h"
#include "supple/model/freeze.h"
#include "supple/params/params.h"

namespace supple::engine {
namespace {
constexpr std::size_t kPendingOutputs = 8192, kGestures = 128, kGesturePoints = 4096;
using Kind = host::OutputEvent::Kind;

bool global(const host::OutputEvent& e) {
    return e.noteId == -1 && e.port == -1 && e.channel == -1 && e.key == -1;
}

}  // namespace

json::Value EngineApp::pluginErrorJson(const Error& e) {
    auto out = json::Value::object();
    out["code"] = e.code;
    out["message"] = e.message;
    out["path"] = e.path;
    out["hint"] = e.hint;
    if (!e.details.empty()) {
        out["details"] = json::Value::array();
        for (const auto& detail : e.details)
            out["details"].asArray().push_back(pluginErrorJson(detail));
    }
    return out;
}

Status EngineApp::commitOutputGesture(const OutputKey& key, const OutputGesture& gesture) {
    if (!gesture.latest || model::frozenOwner(head(), key.first)) return {};
    const auto* fx = head().fx.find(key.first);
    auto* instance = pool_->find(key.first);
    if (!fx || !instance || instance->failed()) return {};
    const auto info = instance->params();
    auto param =
        std::find_if(info.begin(), info.end(), [&](const auto& p) { return p.id == key.second; });
    // Read-only outputs (meters, readouts) are display values: they never
    // enter the model, history or automation.
    if (param == info.end() || (param->flags & CLAP_PARAM_IS_READONLY)) return {};
    auto plain = [&](double value) -> Result<double> {
        if (!std::isfinite(value))
            return Error("fx.output_value", "non-finite plug-in output", {},
                         "disable automation recording for this FX and correct or replace the "
                         "plugin emitting values outside its declared range");
        if (instance->paramsNormalized()) {
            if (value < 0 || value > 1)
                return Error("fx.output_value", "normalized output outside 0..1", {},
                             "disable automation recording for this FX and correct or replace the "
                             "plugin emitting values outside its declared range");
            auto converted = instance->plainParam(key.second, value);
            if (!converted) return converted.error();
            value = *converted;
        }
        if (!std::isfinite(value) || value < param->min || value > param->max)
            return Error("fx.output_value", "plug-in output outside the current parameter range",
                         {},
                         "disable automation recording for this FX and correct or replace the "
                         "plugin emitting values outside its declared range");
        return value;
    };
    auto value = plain(gesture.latest->event.value);
    if (!value) return value.error();
    std::unique_ptr<params::DescribedParam> semantic;
    if (instance->info().semantic) {
        auto schema = instance->semanticSchema();
        if (!schema) return schema.error();
        const auto* descriptions = schema->find("params");
        if (descriptions && descriptions->isObject())
            for (const auto& entry : descriptions->asObject()) {
                auto described = params::fromDescription(entry.key, entry.value);
                if (described && (*described)->spec.id == key.second) {
                    semantic = std::move(*described);
                    break;
                }
            }
        if (!semantic)
            return Error("fx.output_param", "output parameter is missing from the semantic schema",
                         {},
                         "choose an installed plugin version whose semantic schema describes the "
                         "output parameter, or remove that parameter's envelope");
    }
    auto next = *fx;
    next.params[key.second] = *value;
    std::string paramKey = semantic ? semantic->key : std::to_string(key.second);
    if (semantic) {
        if (!next.patch) {
            auto patch = instance->getPatch(false);
            if (!patch) return patch.error();
            auto parsed = json::parse(*patch);
            if (!parsed) return parsed.error();
            next.patch = std::move(*parsed);
        }
        (*next.patch)["params"][paramKey] =
            params::toJson(semantic->spec, params::fromClap(semantic->spec, *value));
    }
    const auto before = head();
    // A semantic FX's values live in its patch: raw params gain this id
    // whatever its value, so they alone would make every repeated value an
    // edit (each parameter, at each overflow recovery).
    bool changed =
        semantic ? fx->patch != next.patch : fx->params != next.params || fx->patch != next.patch;
    const bool records =
        std::any_of(before.envelopes.begin(), before.envelopes.end(), [&](const auto& env) {
            const auto& e = *env.value;
            return e.active && e.target.object == key.first &&
                   (e.target.param == paramKey || (!semantic && e.target.param == param->name)) &&
                   (e.mode == model::EnvMode::Touch || e.mode == model::EnvMode::Latch ||
                    e.mode == model::EnvMode::LatchPreview || e.mode == model::EnvMode::Write);
        });
    // A repeated processor output is not another user edit.
    if (!changed && (!records || gesture.recording.empty())) return {};
    if (!committingLearn_) touchParam(key.first, paramKey);
    std::optional<std::vector<std::uint8_t>> state;
    if (!semantic) {
        auto saved = instance->saveState();
        if (!saved) return saved.error();
        state = std::move(*saved);
        next.state = "sha256:" + sha256Hex(*state);
    }
    auto id =
        beginTransaction({cmd::Origin::Type::Internal, "plugin", ""}, "Plug-in parameter edit");
    if (!id) return id.error();
    const auto writing = autoWrite_;
    txn().put<model::FxInstance>(key.first, next);
    if (state)
        txn().putBlob(*next.state,
                      std::make_shared<const model::Blob>(model::Blob{std::move(*state)}));
    for (const auto& output : gesture.recording) {
        if (!records || !engine_) break;
        auto recorded = plain(output.event.value);
        if (!recorded) continue;
        const double physical = semantic ? params::fromClap(semantic->spec, *recorded) : *recorded;
        const double seconds =
            static_cast<double>(output.timelineSample) / engine_->config().sampleRate;
        if (!std::isfinite(seconds) || seconds < 0) continue;
        // Plain envelopes may have been addressed by the plug-in's display name.
        for (const auto& env : before.envelopes)
            if (env.value->target.object == key.first &&
                (env.value->target.param == paramKey ||
                 (!semantic && env.value->target.param == param->name)))
                recordAutomationAt(key.first, env.value->target.param, physical, seconds);
    }
    if (gesture.ended)
        for (const auto& env : before.envelopes)
            if (env.value->target.object == key.first && env.value->target.param == paramKey &&
                env.value->mode == model::EnvMode::Touch)
                autoWrite_.erase(env.id);
    // Replay metadata only: consuming an output never executes a setter.
    auto args = json::Value::object();
    args["fx"] = key.first.str();
    args["values"] = json::Value::object();
    args["values"][paramKey] = semantic ? (*next.patch)["params"][paramKey] : json::Value(*value);
    record_.invocations.push_back({"fx.param.set", std::move(args)});
    static constexpr const char* shapes[]{"linear",     "square",   "slow",
                                          "fast_start", "fast_end", "bezier"};
    const auto preview = txn().preview();
    for (const auto& env : preview.envelopes) {
        const auto* old = before.envelopes.find(env.id);
        if (!old || (old->points.size() == env.value->points.size() &&
                     std::equal(old->points.begin(), old->points.end(), env.value->points.begin(),
                                [](const auto& a, const auto& b) {
                                    return a.pos == b.pos && a.value == b.value &&
                                           a.shape == b.shape && a.tension == b.tension;
                                })))
            continue;
        auto envelope = json::Value::object();
        envelope["env"] = env.id.str();
        envelope["points"] = json::Value::array();
        for (const auto& point : env.value->points) {
            auto p = json::Value::object();
            p[env.value->timebase == model::Timebase::Time ? "seconds" : "beat"] = point.pos;
            p["value"] = point.value;
            p["shape"] = shapes[static_cast<unsigned>(point.shape)];
            p["tension"] = point.tension;
            envelope["points"].asArray().push_back(std::move(p));
        }
        record_.invocations.push_back({"env.points.set", std::move(envelope)});
    }
    if (semantic && next.patch)
        pool_->acknowledgeOutputPatch(key.first, *next.patch);
    else if (!semantic)
        pool_->acknowledgeOutputState(key.first, *next.state);
    auto committed = commitTransaction(*id);
    if (!committed) {
        autoWrite_ = writing;
        if (fx->patch)
            pool_->acknowledgeOutputPatch(key.first, *fx->patch);
        else
            pool_->acknowledgeOutputState(key.first, fx->state.value_or(""));
    }
    return committed;
}

void EngineApp::consumePluginOutputs() {
    PluginOutputs taken;
    if (engine_)
        taken = engine_->takePluginOutputs();
    else
        for (const auto& fx : head().fx)
            if (auto* instance = pool_->find(fx.id)) {
                auto idle = instance->takeIdleOutputs();
                taken.dropped += idle.dropped;
                if (idle.dropped != 0) taken.droppedFx.push_back(fx.id);
                for (const auto& event : idle.events) {
                    PluginOutput output;
                    output.fx = fx.id;
                    output.event = event;
                    output.idle = true;
                    taken.events.push_back(output);
                }
            }
    const auto generation = engine_ ? engine_->planGeneration() : 0;
    const auto epoch = engine_ ? engine_->meterClock().epoch : 0;
    // Re-arm after all exits, including a transaction deferral. No time poll is
    // used for rendered outputs; callback consumption makes them eligible.
    struct Arm {
        EngineApp& app;
        ~Arm() {
            if (!app.engine_) return;
            std::int64_t target = INT64_MAX;
            const auto epoch = app.engine_->meterClock().epoch;
            if (!app.txn_)
                for (const auto& output : app.pendingOutputs_)
                    if (output.anticipative && !output.idle && output.epoch == epoch)
                        target = std::min(target, output.streamSample);
            app.engine_->armOutputWake(epoch, target);
        }
    } arm{*this};
    if (generation != outputGeneration_ || epoch != outputEpoch_) {
        outputGestures_.clear();
        pendingOutputs_.clear();
        outputGeneration_ = generation;
        outputEpoch_ = epoch;
    }
    bool overflow =
        outputOverflow_ || taken.dropped != outputDrops_ || taken.voiceDropped != voiceDrops_;
    // Voice tracking names no FX; the engine's table may have overflowed.
    overflowAll_ = overflowAll_ || taken.droppedAll || taken.voiceDropped != voiceDrops_;
    for (const auto& fx : taken.droppedFx)
        if (std::find(overflowFx_.begin(), overflowFx_.end(), fx) == overflowFx_.end())
            overflowFx_.push_back(fx);
    outputDrops_ = taken.dropped;
    voiceDrops_ = taken.voiceDropped;
    if (engine_ && loadedHead_ && loadedHead_->version != head().version) {
        // A failed head rebuild leaves an older plan playing. Its outputs must
        // not edit the newer model merely because the published generation stayed.
        outputGestures_.clear();
        pendingOutputs_.clear();
        outputOverflow_ = false;
        return;
    }
    for (const auto& output : taken.events) {
        if (output.planGeneration != generation || output.epoch != epoch ||
            !head().fx.contains(output.fx))
            continue;
        if (pendingOutputs_.size() == kPendingOutputs) {
            ++consumerDrops_;
            overflow = true;
            if (std::find(overflowFx_.begin(), overflowFx_.end(), output.fx) == overflowFx_.end())
                overflowFx_.push_back(output.fx);
        } else
            pendingOutputs_.push_back(output);
    }
    // Do not nest a model transaction inside a caller's batch.
    outputOverflow_ = overflow;
    if (txn_) return;
    auto commit = [&](const OutputKey& key, const OutputGesture& gesture) {
        auto result = commitOutputGesture(key, gesture);
        if (!result) emit("engine.error", pluginErrorJson(result.error()));
        // Our own rebuild does not make the rest of this drain stale. External
        // changes are detected at the next entry, before another drain is used.
        outputGeneration_ = engine_ ? engine_->planGeneration() : 0;
    };
    if (overflow) {
        outputOverflow_ = false;
        pendingOutputs_.clear();
        outputGestures_.clear();
        std::erase_if(autoWrite_, [&](const auto& entry) {
            const auto* env = head().envelopes.find(entry.first);
            return env && env->target.object.kind == model::Kind::Fx;
        });
        // Read current values rather than guessing missing events or points.
        const auto session = head();
        // Only the FX whose outputs were dropped, and only their writable
        // parameters: read-only outputs (meters) never enter the model.
        const auto dropped = [&](model::Id id) {
            return overflowAll_ ||
                   std::find(overflowFx_.begin(), overflowFx_.end(), id) != overflowFx_.end();
        };
        for (const auto& fx : session.fx) {
            if (!dropped(fx.id)) continue;
            auto* instance = pool_->find(fx.id);
            if (!instance || instance->failed()) continue;
            for (const auto& param : instance->params()) {
                if (param.flags & CLAP_PARAM_IS_READONLY) continue;
                auto value = instance->paramValue(param.id);
                if (!value) continue;
                PluginOutput output;
                output.fx = fx.id;
                output.event.paramId = param.id;
                output.event.value = *value;
                if (instance->paramsNormalized()) {
                    auto normalized = instance->normalizedParam(param.id, *value);
                    if (!normalized) continue;
                    output.event.value = *normalized;
                }
                output.idle = true;
                OutputGesture recovered;
                recovered.latest = output;
                commit({fx.id, param.id}, recovered);
            }
        }
        overflowFx_.clear();
        overflowAll_ = false;
        return;
    }
    for (auto it = pendingOutputs_.begin(); it != pendingOutputs_.end();) {
        if (engine_ && it->anticipative && !it->idle &&
            it->streamSample > engine_->playedStream(epoch)) {
            ++it;
            continue;
        }
        const auto output = *it;
        it = pendingOutputs_.erase(it);
        const auto& event = output.event;
        const OutputKey key{output.fx, event.paramId};
        if (event.kind == Kind::NoteEnd || (event.kind == Kind::ParamValue && !global(event)))
            continue;
        auto found = outputGestures_.find(key);
        if (event.kind == Kind::GestureBegin) {
            // A knob grabbed in the plug-in's editor is touched now, not at release.
            if (auto* instance = pool_->find(output.fx)) {
                const auto info = instance->params();
                const auto param = std::find_if(
                    info.begin(), info.end(), [&](const auto& p) { return p.id == event.paramId; });
                if (param != info.end() && !(param->flags & CLAP_PARAM_IS_READONLY)) {
                    std::string name = std::to_string(event.paramId);
                    if (instance->info().semantic)
                        if (auto schema = instance->semanticSchema())
                            if (const json::Value* ps = schema->find("params");
                                ps && ps->isObject())
                                for (const auto& entry : ps->asObject())
                                    if (const json::Value* id = entry.value.find("id");
                                        id && id->isNumber() && id->asDouble() == event.paramId)
                                        name = entry.key;
                    touchParam(output.fx, std::move(name));
                }
            }
            if (found != outputGestures_.end()) {
                commit(key, found->second);
                outputGestures_.erase(found);
            }
            if (outputGestures_.size() < kGestures)
                outputGestures_.emplace(key, OutputGesture{});
            else {
                ++consumerDrops_;
                outputOverflow_ = true;
            }
        } else if (event.kind == Kind::ParamValue) {
            const bool single = found == outputGestures_.end();
            if (single && outputGestures_.size() >= kGestures) {
                ++consumerDrops_;
                outputOverflow_ = true;
                continue;
            }
            auto& gesture = outputGestures_[key];
            gesture.latest = output;
            if (engine_ && !output.idle && engine_->transport().playing &&
                !(event.flags & host::OutputEvent::DontRecord)) {
                if (gesture.recording.size() < kGesturePoints)
                    gesture.recording.push_back(output);
                else {
                    ++consumerDrops_;
                    gesture.recording.clear();
                    gesture.ended = true;
                    outputOverflow_ = true;
                }
            }
            if (single || gesture.ended) {
                commit(key, gesture);
                outputGestures_.erase(key);
            }
        } else if (event.kind == Kind::GestureEnd && found != outputGestures_.end()) {
            found->second.ended = true;
            commit(key, found->second);
            outputGestures_.erase(found);
        }
    }
}

void EngineApp::checkFailures() {
    for (const auto& [id, error] : pool_->failedErrors()) {
        if (!reportedFailures_.insert(id).second) continue;
        auto payload = json::Value::object();
        payload["fx"] = id.str();
        payload["reason"] = error.message;
        payload["error"] = pluginErrorJson(error);
        emit("fx.failed", std::move(payload));
    }
}

}  // namespace supple::engine
