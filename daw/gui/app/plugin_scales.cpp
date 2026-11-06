// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plugin_scales.h"

#include "supple/base/units.h"
#include "supple/rack/module_type.h"

namespace supple::gui {

namespace {

namespace sd = script::detail;

std::string_view unitOf(sd::ParamModel::Unit u) {
    using U = sd::ParamModel::Unit;
    using V = units::Unit;
    switch (u) {
        case U::Decibels:
            return units::unitName(V::dB);
        case U::Hertz:
            return units::unitName(V::Hz);
        case U::Milliseconds:
            return units::unitName(V::ms);
        case U::Percent:
            return units::unitName(V::percent);
        case U::Ratio:
            return units::unitName(V::ratio);
        case U::Semitones:
            return units::unitName(V::st);
        case U::Cents:
            return units::unitName(V::ct);
        case U::Octaves:
            return units::unitName(V::oct);
        case U::Degrees:
            return units::unitName(V::deg);
        case U::Bpm:
            return units::unitName(V::bpm);
        case U::Samples:
            return units::unitName(V::samples);
        case U::Meters:
            return units::unitName(V::m);
        default:
            return {};
    }
}

// A module knob as a described parameter: its range, curve, unit, choices.
std::shared_ptr<const params::DescribedParam> describe(const sd::ParamModel& p) {
    using K = sd::ParamModel::Kind;
    json::Value d = json::Value::object();
    d["id"] = static_cast<std::int64_t>(p.id);
    d["name"] = p.name;
    d["group"] = p.group;
    d["type"] = p.kind == K::Integer  ? "int"
                : p.kind == K::Toggle ? "bool"
                : p.kind == K::Choice ? "enum"
                                      : "float";
    if (const auto unit = unitOf(p.unit); !unit.empty() && p.kind == K::Continuous)
        d["unit"] = std::string(unit);
    if (p.kind == K::Choice) {
        json::Value keys = json::Value::array();
        for (std::size_t k = 0; k < p.choices.size(); ++k) keys.push_back(sd::choiceKey(p, k));
        d["choices"] = std::move(keys);
    }
    d["min"] = p.min;
    d["max"] = p.max;
    d["default"] = p.def;
    if (p.kind == K::Continuous && p.curve == sd::ParamModel::Curve::Log)
        d["curve"] = "log";
    else if (p.kind == K::Continuous && p.curve == sd::ParamModel::Curve::Pow)
        d["curve"] = "pow:" + std::to_string(p.curveK);
    auto spec = params::fromDescription(p.key, d);
    if (!spec) return nullptr;
    return std::shared_ptr<const params::DescribedParam>(std::move(*spec));
}

}  // namespace

std::optional<views::EnvScale> PluginScales::operator()(const model::Envelope& env) {
    if (env.target.object.kind == model::Kind::Module) {
        if (!app_) return std::nullopt;
        const model::RackModule* m = app_->head().modules.find(env.target.object);
        const rack::Catalog::Entry* t =
            m && app_->modules() ? app_->modules()->find(m->type) : nullptr;
        if (!t) return std::nullopt;
        const int index = t->type->param(env.target.param);
        if (index < 0) return std::nullopt;
        const auto key = std::make_pair(env.target.object, env.target.param);
        auto it = moduleCache_.find(key);
        if (it == moduleCache_.end() || it->second.first != t->generation)
            it = moduleCache_
                     .insert_or_assign(
                         key, std::make_pair(
                                  t->generation,
                                  describe(t->type->params()[static_cast<std::size_t>(index)])))
                     .first;
        if (!it->second.second) return std::nullopt;
        views::EnvScale scale;
        scale.kind = views::EnvScale::Kind::Plugin;
        scale.param = it->second.second;
        scale.normalizedInterpolation = false;  // module envelopes move in the knob's unit
        return scale;
    }
    if (env.target.object.kind != model::Kind::Fx) return std::nullopt;
    const auto key = std::make_pair(env.target.object, env.target.param);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        std::shared_ptr<const params::DescribedParam> found;
        json::Value args = json::Value::object();
        args["fx"] = env.target.object.str();
        args["full"] = true;
        auto described = dispatcher_.execute(
            {"fx.describe", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
        if (described)
            if (const json::Value* ps = described->find("params"); ps && ps->isObject())
                if (const json::Value* entry = ps->find(env.target.param))
                    if (auto spec = params::fromDescription(env.target.param, *entry); spec)
                        found = std::shared_ptr<const params::DescribedParam>(std::move(*spec));
        it = cache_.emplace(key, std::move(found)).first;
    }
    if (!it->second) return std::nullopt;
    views::EnvScale scale;
    scale.kind = views::EnvScale::Kind::Plugin;
    scale.param = it->second;
    if (app_)
        if (auto* instance = app_->instanceOf(env.target.object))
            scale.normalizedInterpolation =
                instance->info().semantic || instance->paramsNormalized();
    return scale;
}

}  // namespace supple::gui
