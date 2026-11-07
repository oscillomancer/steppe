// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_plugin_params.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <utility>

#include <clap/clap.h>

namespace supple::gui::racks {
namespace {

bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

const PluginParam* PluginParams::find(const std::string& key) const {
    for (const PluginParam& p : params)
        if (p.key == key) return &p;
    std::uint32_t number = 0;
    const auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), number);
    if (ec == std::errc{} && end == key.data() + key.size()) {
        for (const PluginParam& p : params)
            if (p.id == number) return &p;
        return nullptr;
    }
    for (const PluginParam& p : params)
        if (sameName(p.name, key)) return &p;
    return nullptr;
}

std::string PluginParams::nameOf(const std::string& key) const {
    const PluginParam* p = find(key);
    return p && !p->name.empty() ? p->name : key;
}

PluginParams pluginParams(engine::EngineApp& app, const model::FxInstance& fx) {
    PluginParams out;
    const bool vst3 = fx.plugin.format == "vst3";
    auto list = app.fxParams(fx);
    if (!list) {
        out.note = vst3 ? "VST3 plug-ins take no modulation."
                        : "The plug-in could not be asked: " + list.error().message;
        return out;
    }
    // A semantic plug-in's keys and groups, by CLAP id.
    std::map<std::uint32_t, std::pair<std::string, std::string>> semantic;
    if (auto schema = app.fxSchema(fx); schema && schema->isObject())
        if (const json::Value* ps = schema->find("params"); ps && ps->isObject())
            for (const auto& member : ps->asObject()) {
                const json::Value* id = member.value.find("id");
                if (!id || !id->isNumber()) continue;
                const json::Value* group = member.value.find("group");
                semantic[static_cast<std::uint32_t>(id->asInt())] = {
                    member.key, group && group->isString() ? group->asString() : ""};
            }
    bool any = false;
    for (const host::ParamInfo& info : *list) {
        if (info.flags & CLAP_PARAM_IS_HIDDEN) continue;
        PluginParam p;
        p.id = info.id;
        p.name = info.name;
        p.modulatable = !vst3 && (info.flags & CLAP_PARAM_IS_MODULATABLE) &&
                        !(info.flags & CLAP_PARAM_IS_READONLY);
        if (const auto s = semantic.find(info.id); s != semantic.end()) {
            p.key = s->second.first;
            p.group = s->second.second;
        } else {
            p.key = std::to_string(info.id);
            p.group = info.module;
        }
        any = any || p.modulatable;
        out.params.push_back(std::move(p));
    }
    if (vst3)
        out.note = "VST3 plug-ins take no modulation.";
    else if (!any)
        out.note = "None of its parameters take modulation.";
    return out;
}

// --- the shared cache ---------------------------------------------------------------

namespace {

// What an fx holds: "format:id@version" ("" when it is not there).
std::string pluginOf(const model::Session& s, model::Id fx) {
    const model::FxInstance* f = s.fx.find(fx);
    return f ? f->plugin.format + ":" + f->plugin.id + "@" + f->plugin.version : std::string();
}

}  // namespace

struct PluginParamCache::Store {
    struct Asked {
        std::string plugin;  // pluginOf when asked
        PluginParams params;
        std::map<std::string, std::string> names;  // by key
    };
    std::map<model::Id, Asked> byFx;
    std::size_t asked = 0;
};

namespace {
// The stores by app (tests run several apps), alive while a view holds one.
std::map<const engine::EngineApp*, std::weak_ptr<PluginParamCache::Store>>& stores() {
    static std::map<const engine::EngineApp*, std::weak_ptr<PluginParamCache::Store>> all;
    return all;
}
}  // namespace

PluginParamCache::PluginParamCache(engine::EngineApp& app) : app_(app) {
    auto& slot = stores()[&app_];
    store_ = slot.lock();
    if (!store_) {
        store_ = std::make_shared<Store>();
        slot = store_;
    }
}

PluginParamCache::~PluginParamCache() {
    const bool last = store_.use_count() == 1;
    store_.reset();
    if (last) stores().erase(&app_);
}

const PluginParams& PluginParamCache::of(model::Id fx) {
    const model::Session& s = app_.head();
    const std::string plugin = pluginOf(s, fx);
    Store::Asked& a = store_->byFx[fx];
    if (a.plugin != plugin || plugin.empty()) {
        a.plugin = plugin;
        a.names.clear();
        const model::FxInstance* f = s.fx.find(fx);
        a.params = f ? pluginParams(app_, *f) : PluginParams{};
        if (f) ++store_->asked;
    }
    return a.params;
}

const std::string& PluginParamCache::name(model::Id fx, const std::string& key) {
    const PluginParams& params = of(fx);
    Store::Asked& a = store_->byFx[fx];
    auto it = a.names.find(key);
    if (it == a.names.end()) it = a.names.emplace(key, params.nameOf(key)).first;
    return it->second;
}

std::size_t PluginParamCache::asked() const { return store_->asked; }

}  // namespace supple::gui::racks
