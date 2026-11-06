// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace supple::gui {

namespace sd = script::detail;
using rack::PanelItem;

const char* signalToken(Signal s) {
    switch (s) {
        case Signal::Pitch:
            return "signal-pitch";
        case Signal::Gate:
            return "signal-gate";
        case Signal::Cv:
            return "signal-cv";
        case Signal::Unipolar:
            return "signal-unipolar";
        case Signal::Phase:
            return "signal-phase";
        default:
            return "signal-audio";
    }
}

double paramOrigin(const sd::ParamModel& p) {
    if (p.kind == sd::ParamModel::Kind::Continuous && p.min < 0 && p.max > 0)
        return rack::knobPosition(p, 0.0);
    return 0;
}

std::string paramText(const sd::ParamModel& p, double v) {
    using K = sd::ParamModel::Kind;
    using U = sd::ParamModel::Unit;
    if (p.kind == K::Toggle) return v >= 0.5 ? "On" : "Off";
    if (p.kind == K::Choice) {
        const auto k = static_cast<std::size_t>(
            std::clamp(std::lround(v), 0L, std::max(0L, static_cast<long>(p.choices.size()) - 1)));
        return k < p.choices.size() ? p.choices[k] : std::to_string(std::lround(v));
    }
    char buf[48];
    if (p.kind == K::Integer) {
        std::snprintf(buf, sizeof buf, "%ld", std::lround(v));
        return buf;
    }
    auto fixed = [&](double x, const char* unit) {
        const double a = std::fabs(x);
        const int decimals = a >= 100 ? 0 : a >= 10 ? 1 : 2;
        std::snprintf(buf, sizeof buf, "%.*f%s", decimals, x, unit);
        return std::string(buf);
    };
    switch (p.unit) {
        case U::Decibels:
            if (p.offAtMinimum && v <= p.min) return "-inf dB";
            if (v <= -60 && p.min <= -60) return "-inf dB";
            std::snprintf(buf, sizeof buf, "%.1f dB", v);
            return buf;
        case U::Hertz:
            if (v >= 1000) return fixed(v / 1000, " kHz");
            return fixed(v, " Hz");
        case U::Milliseconds:
            if (v >= 1000) return fixed(v / 1000, " s");
            return fixed(v, " ms");
        case U::Percent:
            std::snprintf(buf, sizeof buf, "%.0f %%", v);
            return buf;
        case U::Ratio:
            return fixed(v, ":1");
        case U::Semitones:
            std::snprintf(buf, sizeof buf, "%+.2f st", v);
            return buf;
        case U::Cents:
            std::snprintf(buf, sizeof buf, "%+.0f ct", v);
            return buf;
        case U::Octaves:
            std::snprintf(buf, sizeof buf, "%+.2f oct", v);
            return buf;
        case U::Degrees:
            std::snprintf(buf, sizeof buf, "%.0f°", v);
            return buf;
        case U::Bpm:
            return fixed(v, " BPM");
        case U::Samples:
            std::snprintf(buf, sizeof buf, "%.0f smp", v);
            return buf;
        case U::Meters:
            return fixed(v, " m");
        default:
            return fixed(v, "");
    }
}

namespace {

// A plugin module's face: its name over its jacks, inputs then outputs on
// one row (audio), notes above them for an instrument.
rack::PanelLayout pluginFace(const rack::ModuleType& t, bool instrument) {
    using rack::kJackPitch, rack::kJackRow, rack::kMargin, rack::kJackSize;
    rack::PanelLayout l;
    const bool notes = instrument && t.inputs().size() > 2;
    const double w = 4 * kJackPitch + 2 * kMargin;
    l.width = w;
    double y = 64;  // the name's area
    auto jack = [&](bool output, int index, int col) {
        PanelItem item;
        item.kind = output ? PanelItem::Kind::Output : PanelItem::Kind::Input;
        item.index = index;
        item.x = kMargin + col * kJackPitch + kJackPitch / 2;
        item.y = y + kJackSize / 2 + 4;
        l.items.push_back(item);
    };
    if (notes) {
        for (int k = 2; k < static_cast<int>(t.inputs().size()); ++k) jack(false, k, k - 2);
        y += kJackRow;
    }
    jack(false, 0, 0);
    jack(false, 1, 1);
    jack(true, 0, 2);
    jack(true, 1, 3);
    y += kJackRow;
    l.height = y + kMargin / 2;
    l.custom = true;
    return l;
}

}  // namespace

RackScene RackScene::build(const model::Session& s, model::Id rackId, const rack::Catalog& catalog,
                           const std::function<bool(const model::PluginRef&)>& instrument) {
    RackScene scene;
    scene.rack = rackId;
    if (!s.racks.find(rackId)) return scene;
    for (const auto& e : s.modules) {
        const model::RackModule& m = *e.value;
        if (m.rack != rackId) continue;
        Module v;
        v.id = e.id;
        v.typeId = m.type;
        v.x = m.x;
        v.y = m.y;
        v.bypass = m.bypass;
        for (const auto& l : m.learn) v.learned.insert(l.param);
        v.fx = m.fx;
        v.plugin = m.type == model::kPluginModule;
        v.out = m.type == model::kOutModule;
        if (v.plugin) {
            const model::FxInstance* fx = m.fx ? s.fx.find(*m.fx) : nullptr;
            v.type = racks::pluginType();
            v.instrument = fx && instrument && instrument(fx->plugin);
            v.title = !m.label.empty()                 ? m.label
                      : fx && !fx->label.empty()       ? fx->label
                      : fx && !fx->plugin.name.empty() ? fx->plugin.name
                                                       : std::string("Plugin");
            if (fx) v.subtitle = fx->plugin.vendor;
        } else if (const rack::Catalog::Entry* t = catalog.find(m.type)) {
            v.type = t->type;
            v.title = model::moduleLabel(s, e.id);
            if (v.title.empty()) v.title = t->type->info().name;
        } else {
            const auto dot = m.type.rfind('.');
            v.title =
                m.label.empty() ? m.type.substr(dot == std::string::npos ? 0 : dot + 1) : m.label;
            v.subtitle = "Missing module";
        }
        if (v.out)
            for (const auto& r : s.routes)
                if (r.value->from.module == e.id && r.value->to.track) {
                    v.subtitle = model::trackLabel(s, *r.value->to.track);
                    break;
                }
        if (v.type) {
            const auto& params = v.type->params();
            v.values.reserve(params.size());
            for (const auto& p : params)
                v.values.push_back(racks::paramValue(p, m.params.find(p.key)));
            v.face = v.plugin ? pluginFace(*v.type, v.instrument) : v.type->layout();
            v.w = v.face.width;
            v.h = v.face.height;
        } else {
            v.w = 132;
            v.h = 84;
        }
        scene.index_[v.id] = scene.modules.size();
        scene.modules.push_back(std::move(v));
    }
    for (const auto& e : s.cables) {
        const model::Cable& c = *e.value;
        const Module* from = scene.module(c.from.object);
        if (!from) continue;
        Cable v;
        v.id = e.id;
        v.from = c.from;
        v.to = c.to;
        v.color = c.color;
        v.param = c.amount.has_value();
        v.amount = c.amount.value_or(0);
        v.signal = scene.signalOf(c.from, true);
        if (c.to.object.kind == model::Kind::Fx)
            for (Module& m : scene.modules)
                if (m.fx == c.to.object &&
                    std::find(m.mods.begin(), m.mods.end(), c.to.key) == m.mods.end())
                    m.mods.push_back(c.to.key);
        scene.cables.push_back(std::move(v));
    }
    return scene;
}

const RackScene::Module* RackScene::module(model::Id id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &modules[it->second];
}

RackScene::Module* RackScene::module(model::Id id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &modules[it->second];
}

std::optional<ui::Point> RackScene::endPoint(const model::CableEnd& end, bool output) const {
    if (end.object.kind == model::Kind::Fx) {
        const Module* m = moduleOfFx(end.object);
        if (!m) return std::nullopt;
        const auto it = std::find(m->mods.begin(), m->mods.end(), end.key);
        return modSocket(*m,
                         it == m->mods.end() ? 0 : static_cast<std::size_t>(it - m->mods.begin()));
    }
    const Module* m = module(end.object);
    if (!m) return std::nullopt;
    const rack::PanelLayout* l = m->type ? &m->face : nullptr;
    if (!l)
        return ui::Point{static_cast<float>(m->x + (output ? m->w : 0)),
                         static_cast<float>(m->y + m->h / 2)};
    const rack::ModuleType& t = *m->type;
    const int port = output ? t.output(end.key) : t.input(end.key);
    const int param = port < 0 && !output ? t.param(end.key) : -1;
    for (const PanelItem& item : l->items) {
        const bool hit =
            port >= 0 ? item.index == port &&
                            item.kind == (output ? PanelItem::Kind::Output : PanelItem::Kind::Input)
            : param >= 0
                ? item.index == param && item.kind != PanelItem::Kind::Input &&
                      item.kind != PanelItem::Kind::Output && item.kind != PanelItem::Kind::Display
                : false;
        if (!hit) continue;
        const bool centred = item.kind == PanelItem::Kind::Knob ||
                             item.kind == PanelItem::Kind::Input ||
                             item.kind == PanelItem::Kind::Output;
        double cx = centred ? item.x : item.x + item.w / 2;
        double cy = centred ? item.y : item.y + item.h / 2;
        if (item.kind == PanelItem::Kind::Knob) {  // a modulation cable plugs into its rim
            cx += item.w / 2 * 0.72;
            cy -= item.w / 2 * 0.72;
        }
        return ui::Point{static_cast<float>(m->x + cx), static_cast<float>(m->y + cy)};
    }
    // Not on the face (a hidden parameter): the module's edge.
    return ui::Point{static_cast<float>(m->x + (output ? m->w : 0)),
                     static_cast<float>(m->y + rack::kTitleHeight / 2)};
}

const RackScene::Module* RackScene::moduleOfFx(model::Id fx) const {
    for (const Module& m : modules)
        if (m.fx == fx) return &m;
    return nullptr;
}

// A plug-in parameter's socket: on the top edge, from the left, 16 apart
// (closer when many, so that 16 fit; the editor's mark keeps the right).
ui::Point RackScene::modSocket(const Module& m, std::size_t i) {
    const double room = std::max(0.0, m.w - 46);
    const double step =
        m.mods.size() > 1 ? std::min(16.0, room / static_cast<double>(m.mods.size() - 1)) : 0;
    return {static_cast<float>(m.x + 14 + step * static_cast<double>(i)), static_cast<float>(m.y)};
}

Signal RackScene::signalOf(const model::CableEnd& end, bool output) const {
    const Module* m = module(end.object);
    if (!m || !m->type) return Signal::Audio;
    const int port = output ? m->type->output(end.key) : m->type->input(end.key);
    if (port < 0) return Signal::Cv;  // a parameter
    return (output ? m->type->output(port) : m->type->input(port)).signal;
}

std::vector<const RackScene::Cable*> RackScene::cablesAt(const model::CableEnd& end,
                                                         bool output) const {
    std::vector<const Cable*> out;
    for (const Cable& c : cables)
        if ((output ? c.from : c.to) == end) out.push_back(&c);
    return out;
}

ui::Rect RackScene::bounds() const {
    ui::Rect r;
    for (const Module& m : modules) r = r.unite(m.rect());
    return r;
}

}  // namespace supple::gui
