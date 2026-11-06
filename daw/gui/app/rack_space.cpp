// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_space.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "supple/ui/root.h"

namespace supple::gui {
namespace {

namespace sd = script::detail;
using rack::PanelItem;
using Kind = PanelItem::Kind;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kRadius = 4;            // a module's corners
constexpr float kArc0 = 0.75f * kPi;    // a knob's travel: from bottom left...
constexpr float kArcSpan = 1.5f * kPi;  // ...270 degrees clockwise
constexpr float kJackR = static_cast<float>(rack::kJackSize) / 2;
constexpr float kLabel = 8.5f;  // labels under knobs and jacks
constexpr float kTitleFont = 11.0f;
constexpr int kManyCables = 8;  // past this, cables not in focus fade

constexpr double D(float x) { return static_cast<double>(x); }

NVGcolor nvg(ui::Color c) { return nvgRGBAf(c.r, c.g, c.b, c.a); }

ui::Color mix(ui::Color a, ui::Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}

// A label in its box: shrunk to fit, down to three quarters of its size,
// before the canvas cuts it with an ellipsis.
void fitText(ui::Canvas& c, const ui::Rect& box, std::string_view s, ui::Font font, float size,
             ui::Color col, int align) {
    const float w = c.textWidth(s, font, size);
    if (w > box.w && w > 0) size = std::max(size * 0.75f, size * box.w / w);
    c.text(box, s, font, size, col, align);
}

float dist(ui::Point a, ui::Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool isJack(Kind k) { return k == Kind::Input || k == Kind::Output; }
bool isParam(Kind k) { return !isJack(k) && k != Kind::Display; }

ui::Point centreOf(const RackScene::Module& m, const PanelItem& item) {
    const bool centred = item.kind == Kind::Knob || isJack(item.kind);
    return {static_cast<float>(m.x + (centred ? item.x : item.x + item.w / 2)),
            static_cast<float>(m.y + (centred ? item.y : item.y + item.h / 2))};
}

ui::Rect rectOf(const RackScene::Module& m, const PanelItem& item) {
    if (item.kind == Kind::Knob) {
        const float r = static_cast<float>(item.w) / 2;
        const ui::Point c = centreOf(m, item);
        return {c.x - r, c.y - r, 2 * r, 2 * r};
    }
    if (isJack(item.kind)) {
        const ui::Point c = centreOf(m, item);
        return {c.x - kJackR, c.y - kJackR, 2 * kJackR, 2 * kJackR};
    }
    return {static_cast<float>(m.x + item.x), static_cast<float>(m.y + item.y),
            static_cast<float>(item.w), static_cast<float>(item.h)};
}

// A cable's curve: hanging between its ends, deeper the longer it is.
struct Curve {
    ui::Point p0, c0, c1, p1;
};
Curve curveOf(ui::Point a, ui::Point b) {
    const float d = dist(a, b);
    const float sag = std::min(160.0f, 0.22f * d + 14.0f) / 0.75f;
    return {a,
            {a.x + (b.x - a.x) / 3, a.y + (b.y - a.y) / 3 + sag},
            {a.x + 2 * (b.x - a.x) / 3, a.y + 2 * (b.y - a.y) / 3 + sag},
            b};
}
ui::Point at(const Curve& k, float t) {
    const float u = 1 - t;
    const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return {a * k.p0.x + b * k.c0.x + c * k.c1.x + d * k.p1.x,
            a * k.p0.y + b * k.c0.y + c * k.c1.y + d * k.p1.y};
}

}  // namespace

RackSpace::RackSpace(Hooks hooks) : Node("rack.space", ui::Role::Custom), hooks_(std::move(hooks)) {
    interactive = true;
    focusable = true;
    clipsChildren = true;
    layout.width = ui::SizeRule::fraction();
    layout.height = ui::SizeRule::fraction();
}

void RackSpace::setScene(RackScene scene) {
    // A gesture on a module that stays carries on: the dragged modules stay
    // where the pointer has them, a knob keeps the value it set; one whose
    // module went ends.
    const bool onModule = drag_.kind != Drag::Kind::None && drag_.kind != Drag::Kind::Pan &&
                          drag_.kind != Drag::Kind::Marquee;
    if (onModule) {
        const auto& old = scene_.modules;
        auto indexIn = [&](model::Id id) -> std::optional<std::size_t> {
            for (std::size_t k = 0; k < scene.modules.size(); ++k)
                if (scene.modules[k].id == id) return k;
            return std::nullopt;
        };
        const auto moved = drag_.module < old.size() ? indexIn(old[drag_.module].id) : std::nullopt;
        if (!moved) {
            drag_ = {};
        } else {
            std::vector<std::pair<std::size_t, ui::Point>> origins;
            for (auto& [index, origin] : drag_.origins)
                if (index < old.size())
                    if (auto k = indexIn(old[index].id)) {
                        origins.emplace_back(*k, origin);
                        if (drag_.kind == Drag::Kind::Move && drag_.moved) {
                            scene.modules[*k].x = old[index].x;
                            scene.modules[*k].y = old[index].y;
                        }
                    }
            drag_.origins = std::move(origins);
            if (drag_.kind == Drag::Kind::Control &&
                scene.modules[*moved].values.size() == old[drag_.module].values.size())
                scene.modules[*moved].values = old[drag_.module].values;
            drag_.module = *moved;
        }
    }
    // Telemetry carries over until the next read.
    for (auto& m : scene.modules)
        if (const auto* before = scene_.module(m.id)) {
            m.telemetry = before->telemetry;
            m.live = before->live;
        }
    const bool otherRack = !(scene.rack == scene_.rack);
    scene_ = std::move(scene);
    std::erase_if(selection_, [&](model::Id id) { return !scene_.module(id); });
    if (otherRack) {
        selection_.clear();
        hot_.reset();
    }
    if (hot_ && !scene_.module(*hot_)) hot_.reset();
    hotCable_.reset();
    std::erase_if(faces_, [&](const auto& f) { return !scene_.module(f.first); });
    invalidate();
}

void RackSpace::arrange(const ui::Canvas& canvas, const ui::Rect& b) {
    Node::arrange(canvas, b);
    if (frameOnLayout_ && !b.empty()) {
        frameOnLayout_ = false;
        frameAll();
    }
}

void RackSpace::setView(View v) {
    v.zoom = std::clamp(v.zoom, kMinZoom, kMaxZoom);
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) v.x = v.y = 0;
    v.x = std::clamp(v.x, -model::kMaxRackCoordinate, model::kMaxRackCoordinate);
    v.y = std::clamp(v.y, -model::kMaxRackCoordinate, model::kMaxRackCoordinate);
    if (v == view_) return;
    view_ = v;
    invalidate();
    if (hooks_.viewChanged) hooks_.viewChanged(view_);
}

void RackSpace::zoomAt(ui::Point p, double factor) {
    const ui::Point before = toSpace(p);
    View v = view_;
    v.zoom = std::clamp(view_.zoom * factor, kMinZoom, kMaxZoom);
    // Keep the space point under the pointer where it is.
    v.x = D(before.x) - D(p.x - bounds().x) / v.zoom;
    v.y = D(before.y) - D(p.y - bounds().y) / v.zoom;
    setView(v);
}

void RackSpace::frameAll() {
    const ui::Rect r = scene_.bounds();
    const ui::Rect b = bounds();
    if (r.empty() || b.empty()) {
        setView({-40, -40, 1});
        return;
    }
    constexpr double kPad = 48;
    const double zoom = std::clamp(
        std::min((D(b.w) - 2 * kPad) / D(r.w), (D(b.h) - 2 * kPad) / D(r.h)), kMinZoom, 1.0);
    View v;
    v.zoom = zoom;
    v.x = D(r.x) + D(r.w) / 2 - D(b.w) / 2 / zoom;
    v.y = D(r.y) + D(r.h) / 2 - D(b.h) / 2 / zoom;
    setView(v);
}

void RackSpace::reveal(const std::set<model::Id>& ids) {
    ui::Rect r;
    bool any = false;
    for (const auto& m : scene_.modules)
        if (ids.contains(m.id)) {
            r = any ? r.unite(m.rect()) : m.rect();
            any = true;
        }
    const ui::Rect b = bounds();
    if (!any || b.empty()) return;
    const ui::Rect seen = visibleSpace();
    if (seen.contains({r.x, r.y}) && seen.contains({r.right(), r.bottom()})) return;
    // Centred, zoomed out only as far as they need.
    constexpr double kPad = 48;
    const double zoom = std::clamp(
        std::min({view_.zoom, (D(b.w) - 2 * kPad) / D(r.w), (D(b.h) - 2 * kPad) / D(r.h)}),
        kMinZoom, kMaxZoom);
    View v;
    v.zoom = zoom;
    v.x = D(r.x) + D(r.w) / 2 - D(b.w) / 2 / zoom;
    v.y = D(r.y) + D(r.h) / 2 - D(b.h) / 2 / zoom;
    setView(v);
}

ui::Point RackSpace::toSpace(ui::Point w) const {
    return {static_cast<float>(view_.x + D(w.x - bounds().x) / view_.zoom),
            static_cast<float>(view_.y + D(w.y - bounds().y) / view_.zoom)};
}

ui::Point RackSpace::toWindow(ui::Point s) const {
    return {static_cast<float>(D(bounds().x) + (D(s.x) - view_.x) * view_.zoom),
            static_cast<float>(D(bounds().y) + (D(s.y) - view_.y) * view_.zoom)};
}

ui::Rect RackSpace::visibleSpace() const {
    const ui::Point a = toSpace({bounds().x, bounds().y});
    return {a.x, a.y, static_cast<float>(D(bounds().w) / view_.zoom),
            static_cast<float>(D(bounds().h) / view_.zoom)};
}

void RackSpace::select(std::set<model::Id> ids) {
    std::erase_if(ids, [&](model::Id id) { return !scene_.module(id); });
    if (ids == selection_) return;
    selection_ = std::move(ids);
    invalidate();
    if (hooks_.selectionChanged) hooks_.selectionChanged(selection_);
}

void RackSpace::setEmptyText(std::string text) {
    if (text == emptyText_) return;
    emptyText_ = std::move(text);
    invalidate();
}

void RackSpace::dragTypeOver(const std::string& name, std::optional<ui::Point> window) {
    dropName_ = name;
    dropAt_ = window;
    invalidate();
}

// --- geometry ------------------------------------------------------------------------

model::CableEnd RackSpace::endOf(const RackScene::Module& m, const PanelItem& item) const {
    model::CableEnd end;
    end.object = m.id;
    if (!m.type) return end;
    if (item.kind == Kind::Input)
        end.key = m.type->input(item.index).key;
    else if (item.kind == Kind::Output)
        end.key = m.type->output(item.index).key;
    else if (item.index >= 0 && static_cast<std::size_t>(item.index) < m.type->params().size())
        end.key = m.type->params()[static_cast<std::size_t>(item.index)].key;
    return end;
}

bool RackSpace::connected(const RackScene::Module& m, const PanelItem& item) const {
    const model::CableEnd end = endOf(m, item);
    for (const auto& c : scene_.cables)
        if ((item.kind == Kind::Output ? c.from : c.to) == end) return true;
    return false;
}

// How lit a jack is, 0..1: an output's own level; an input's, its cables'.
double RackSpace::jackLevel(const RackScene::Module& m, const PanelItem& item) const {
    if (!m.type) return 0;
    auto levelOf = [&](const RackScene::Module& src, int out) -> double {
        if (!src.live || out < 0 || static_cast<std::size_t>(out) >= src.telemetry.peaks.size())
            return 0;
        const auto& port = src.type->output(out);
        const double peak = D(src.telemetry.peaks[static_cast<std::size_t>(out)]);
        const double value = src.telemetry.outputs[static_cast<std::size_t>(out)];
        switch (port.signal) {
            case Signal::Gate:
                return value >= 0.5 ? 1.0 : 0.15 * std::min(1.0, peak);
            case Signal::Pitch:
                return 0.55;  // a pitch is there or not, never loud
            case Signal::Audio:
                return std::min(1.0, std::sqrt(peak));
            default:
                return std::min(1.0, std::fabs(value) * 0.8 + 0.2 * peak);
        }
    };
    if (item.kind == Kind::Output) return levelOf(m, item.index);
    double level = 0;
    const model::CableEnd end = endOf(m, item);
    for (const auto& c : scene_.cables)
        if (c.to == end)
            if (const auto* src = scene_.module(c.from.object); src && src->type)
                level = std::max(level, levelOf(*src, src->type->output(c.from.key)));
    return level;
}

RackSpace::Hit RackSpace::hitAt(ui::Point p) const {
    // Topmost first: the reverse of the paint order (selected modules last).
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < scene_.modules.size(); ++i)
        if (!selection_.contains(scene_.modules[i].id)) order.push_back(i);
    for (std::size_t i = 0; i < scene_.modules.size(); ++i)
        if (selection_.contains(scene_.modules[i].id)) order.push_back(i);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const RackScene::Module& m = scene_.modules[*it];
        const ui::Rect r = m.rect();
        if (!ui::Rect{r.x - 8, r.y - 8, r.w + 16, r.h + 16}.contains(p)) continue;
        // A display hears the pointer where no control is, when the module
        // has pointer(); otherwise it is the module's body.
        const bool hears = m.type && m.type->code() && m.type->code()->pointer();
        std::optional<std::size_t> display;
        for (std::size_t k = 0; k < m.face.items.size(); ++k) {
            const PanelItem& item = m.face.items[k];
            const ui::Point c = centreOf(m, item);
            if (item.kind == Kind::Display) {
                if (hears && !display && rectOf(m, item).contains(p)) display = k;
            } else if (item.kind == Kind::Knob) {
                const float rad = static_cast<float>(item.w) / 2;
                const float d = dist(p, c);
                if (d <= rad + 1) return {Hit::Kind::Control, *it, k, {}};
                // The ring of a modulated knob sets the last cable's amount.
                if (d <= rad + 7) {
                    const auto cables = scene_.cablesAt(endOf(m, item), false);
                    if (!cables.empty()) return {Hit::Kind::Ring, *it, k, cables.back()->id};
                }
            } else if (isJack(item.kind)) {
                if (dist(p, c) <= kJackR + 2) return {Hit::Kind::Jack, *it, k, {}};
            } else if (rectOf(m, item).contains(p)) {
                return {Hit::Kind::Control, *it, k, {}};
            }
        }
        if (display) return {Hit::Kind::Display, *it, *display, {}};
        if (r.contains(p)) return {Hit::Kind::Body, *it, 0, {}};
    }
    return {};
}

std::optional<std::size_t> RackSpace::cableNear(ui::Point p) const {
    const float reach = static_cast<float>(5 / view_.zoom);
    std::optional<std::size_t> best;
    float bestD = reach;
    for (std::size_t i = 0; i < scene_.cables.size(); ++i) {
        const auto& c = scene_.cables[i];
        const auto a = scene_.endPoint(c.from, true), b = scene_.endPoint(c.to, false);
        if (!a || !b) continue;
        const Curve k = curveOf(*a, *b);
        ui::Point prev = k.p0;
        for (int s = 1; s <= 24; ++s) {
            const ui::Point q = at(k, static_cast<float>(s) / 24);
            // Distance to the segment prev..q.
            const float dx = q.x - prev.x, dy = q.y - prev.y;
            const float len2 = dx * dx + dy * dy;
            float t = len2 > 0 ? ((p.x - prev.x) * dx + (p.y - prev.y) * dy) / len2 : 0;
            t = std::clamp(t, 0.0f, 1.0f);
            const float d = dist(p, {prev.x + t * dx, prev.y + t * dy});
            if (d < bestD) {
                bestD = d;
                best = i;
            }
            prev = q;
        }
    }
    return best;
}

std::optional<ui::Point> RackSpace::jackAt(model::Id module, const std::string& key,
                                           bool output) const {
    const auto* m = scene_.module(module);
    if (!m || !m->type) return std::nullopt;
    const int port = output ? m->type->output(key) : m->type->input(key);
    for (const auto& item : m->face.items)
        if (item.index == port && item.kind == (output ? Kind::Output : Kind::Input))
            return toWindow(centreOf(*m, item));
    return std::nullopt;
}

std::optional<ui::Point> RackSpace::controlAt(model::Id module, const std::string& key) const {
    const auto* m = scene_.module(module);
    if (!m || !m->type) return std::nullopt;
    const int param = m->type->param(key);
    for (const auto& item : m->face.items)
        if (item.index == param && isParam(item.kind)) return toWindow(centreOf(*m, item));
    return std::nullopt;
}

std::optional<ui::Point> RackSpace::bodyAt(model::Id module) const {
    const auto* m = scene_.module(module);
    if (!m) return std::nullopt;
    // The title strip (a plugin module's name plate): never a control.
    return toWindow({static_cast<float>(m->x + m->w / 2), static_cast<float>(m->y + 6)});
}

std::string RackSpace::valueText() const {
    std::string out = std::to_string(scene_.modules.size()) + " modules, " +
                      std::to_string(scene_.cables.size()) + " cables, " +
                      std::to_string(static_cast<int>(std::lround(view_.zoom * 100))) + " %";
    if (!selection_.empty()) out += ", " + std::to_string(selection_.size()) + " selected";
    return out;
}

// --- painting ------------------------------------------------------------------------

void RackSpace::paintItem(ui::Canvas& c, const RackScene::Module& m, const PanelItem& item,
                          std::size_t index, bool hot) const {
    const ui::Theme& t = c.theme();
    const ui::Color face = t.color("module"), ink = t.color("ink"), quiet = t.color("ink-quiet");
    const ui::Color well = t.color("module-well");
    const ui::Point centre = centreOf(m, item);
    const bool dragging = drag_.kind == Drag::Kind::Control &&
                          &scene_.modules[drag_.module] == &m && drag_.item == index;
    if (isJack(item.kind)) {
        const bool output = item.kind == Kind::Output;
        const auto& port = output ? m.type->output(item.index) : m.type->input(item.index);
        const ui::Color sig = t.color(signalToken(port.signal));
        if (output)  // outputs sit on a plate, as on hardware
            c.fillRound({centre.x - 17, centre.y - kJackR - 4, 34, 2 * kJackR + 18}, 3,
                        t.color("module-title"));
        const double level = jackLevel(m, item);
        if (level > 0.02)
            c.fillCircle(centre, kJackR + 3.5f, sig.withAlpha(static_cast<float>(0.32 * level)));
        c.fillCircle(centre, kJackR, mix(sig, face, 0.25f));
        c.fillCircle(centre, kJackR - 2.5f, mix(face, ink, 0.16f));
        c.fillCircle(centre, kJackR - 5.5f, t.color("jack-hole"));
        if (level > 0.02)
            c.fillCircle(centre, kJackR - 7.5f, sig.withAlpha(static_cast<float>(0.9 * level)));
        // While a cable is drawn, the jacks it may end on answer.
        if (drag_.kind == Drag::Kind::Cable && output != drag_.fromOutput)
            c.strokeCircle(centre, kJackR + 2.5f, sig.withAlpha(0.75f), 1.5f);
        if (hot) c.strokeCircle(centre, kJackR + 1.5f, t.color("focus"), 1.5f);
        if (m.live && output &&
            static_cast<std::size_t>(item.index) < m.telemetry.channels.size() &&
            m.telemetry.channels[static_cast<std::size_t>(item.index)] > 1)
            c.text({centre.x + kJackR - 2, centre.y - kJackR - 3, 10, 8},
                   std::to_string(m.telemetry.channels[static_cast<std::size_t>(item.index)]),
                   ui::Font::MonoBold, 7.5f, quiet, ui::kLeft | ui::kMiddle);
        fitText(c, {centre.x - 18, centre.y + kJackR + 1, 36, 11}, port.name, ui::Font::Ui, kLabel,
                output ? ink : quiet, ui::kCenter | ui::kMiddle);
        return;
    }
    if (item.kind == Kind::Display) {
        // A module that draws paints its own display (under the controls).
        if (!(m.type && m.type->code() && m.type->code()->draw()))
            c.fillRound(rectOf(m, item), 2, well);
        return;
    }
    if (item.index < 0 || static_cast<std::size_t>(item.index) >= m.type->params().size()) return;
    const sd::ParamModel& p = m.type->params()[static_cast<std::size_t>(item.index)];
    const double v = static_cast<std::size_t>(item.index) < m.values.size()
                         ? m.values[static_cast<std::size_t>(item.index)]
                         : p.def;
    const float pos = static_cast<float>(rack::knobPosition(p, v));
    const ui::Color accent = t.color("accent");
    const bool showValue = hot || dragging;
    switch (item.kind) {
        case Kind::Knob: {
            const float r = static_cast<float>(item.w) / 2;
            const float a = kArc0 + pos * kArcSpan;
            const float o = kArc0 + static_cast<float>(paramOrigin(p)) * kArcSpan;
            const float track = std::max(1.5f, r * 0.11f);
            c.strokeArc(centre, r - track / 2, kArc0, kArc0 + kArcSpan,
                        mix(t.color("rule"), t.color("ink"), 0.12f), track);
            if (std::fabs(a - o) > 0.01f)
                c.strokeArc(centre, r - track / 2, std::min(a, o), std::max(a, o), accent, track);
            // Steps of a choice or a short integer range, as dots.
            const int steps =
                p.kind == sd::ParamModel::Kind::Choice    ? static_cast<int>(p.choices.size())
                : p.kind == sd::ParamModel::Kind::Integer ? static_cast<int>(p.max - p.min) + 1
                                                          : 0;
            if (steps >= 2 && steps <= 16)
                for (int s = 0; s < steps; ++s) {
                    const float as = kArc0 + kArcSpan * static_cast<float>(s) / (steps - 1);
                    c.fillCircle({centre.x + std::cos(as) * (r + 2.5f),
                                  centre.y + std::sin(as) * (r + 2.5f)},
                                 0.9f, quiet);
                }
            const float cap = r - track - 2.5f;
            c.fillCircle({centre.x, centre.y + 0.8f}, cap + 0.6f,
                         t.color("jack-hole").withAlpha(0.35f));
            c.fillCircle(centre, cap, mix(face, ink, 0.12f));
            c.strokeCircle(centre, cap, mix(face, ink, 0.22f), 1);
            c.line({centre.x + std::cos(a) * cap * 0.15f, centre.y + std::sin(a) * cap * 0.15f},
                   {centre.x + std::cos(a) * (cap - 1.0f), centre.y + std::sin(a) * (cap - 1.0f)},
                   ink, std::max(1.6f, r * 0.09f));
            // Modulation: the range the cables' amounts span, faint, and the
            // value they make now, bright.
            const auto cables = scene_.cablesAt(endOf(m, item), false);
            if (!cables.empty()) {
                const ui::Color mod = t.color("signal-cv");
                double span = 0;
                for (const auto* k : cables) span += k->amount;
                double reach = v + span;
                if (p.unit == sd::ParamModel::Unit::Hertz) reach = v * std::pow(2.0, span);
                const float ar =
                    kArc0 + static_cast<float>(rack::knobPosition(p, reach)) * kArcSpan;
                const float ring = r + 4.5f;
                if (std::fabs(ar - a) > 0.01f)
                    c.strokeArc(centre, ring, std::min(a, ar), std::max(a, ar),
                                mod.withAlpha(0.35f), 2);
                if (m.live && static_cast<std::size_t>(item.index) < m.telemetry.params.size()) {
                    const double now = m.telemetry.params[static_cast<std::size_t>(item.index)];
                    const float an =
                        kArc0 + static_cast<float>(rack::knobPosition(p, now)) * kArcSpan;
                    if (std::fabs(an - a) > 0.01f)
                        c.strokeArc(centre, ring, std::min(a, an), std::max(a, an), mod, 2);
                    c.fillCircle({centre.x + std::cos(an) * ring, centre.y + std::sin(an) * ring},
                                 2.2f, mod);
                }
            }
            const ui::Rect label{centre.x - 26, centre.y + r + 1.5f, 52, 11};
            if (showValue)
                fitText(c, label, paramText(p, v), ui::Font::Mono, kLabel, ink,
                        ui::kCenter | ui::kMiddle);
            else
                fitText(c, label, p.name, ui::Font::Ui, kLabel, quiet, ui::kCenter | ui::kMiddle);
            break;
        }
        case Kind::Toggle: {
            const ui::Rect r = rectOf(m, item);
            const bool on = v >= 0.5;
            c.fillRound(r, 3, on ? accent : t.color("module-title"));
            if (!on) c.strokeRound(r, 3, t.color("rule"));
            fitText(c, r.inset(2), p.name, ui::Font::UiBold, kLabel, on ? face : quiet,
                    ui::kCenter | ui::kMiddle);
            break;
        }
        case Kind::Choice: {
            const ui::Rect r = rectOf(m, item);
            c.fillRound(r, 3, well);
            fitText(c, {r.x + 4, r.y, r.w - 14, r.h}, paramText(p, v), ui::Font::Ui, 9.5f, ink,
                    ui::kLeft | ui::kMiddle);
            const float cx = r.right() - 7, cy = r.y + r.h / 2;
            c.fillPath({{cx - 3, cy - 1.5f}, {cx + 3, cy - 1.5f}, {cx, cy + 2}}, quiet);
            break;
        }
        case Kind::Slider: {
            const ui::Rect r = rectOf(m, item);
            const bool across = r.w >= r.h;
            c.fillRound(r, 2, well);
            if (across) {
                c.fillRound({r.x, r.y, r.w * pos, r.h}, 2, accent.withAlpha(0.8f));
                c.fillRect({r.x + r.w * pos - 1, r.y - 1, 2, r.h + 2}, ink);
            } else {
                c.fillRound({r.x, r.bottom() - r.h * pos, r.w, r.h * pos}, 2,
                            accent.withAlpha(0.8f));
                c.fillRect({r.x - 1, r.bottom() - r.h * pos - 1, r.w + 2, 2}, ink);
            }
            if (showValue)
                c.text(r, paramText(p, v), ui::Font::Mono, kLabel, ink, ui::kCenter | ui::kMiddle);
            break;
        }
        case Kind::Readout: {
            const ui::Rect r = rectOf(m, item);
            c.fillRound(r, 2, well);
            const double shown =
                m.live && static_cast<std::size_t>(item.index) < m.telemetry.params.size()
                    ? m.telemetry.params[static_cast<std::size_t>(item.index)]
                    : v;
            fitText(c, {r.x + 4, r.y, r.w / 2, r.h}, p.name, ui::Font::Ui, kLabel, quiet,
                    ui::kLeft | ui::kMiddle);
            c.text({r.x + r.w / 2, r.y, r.w / 2 - 4, r.h}, paramText(p, shown), ui::Font::Mono,
                   kLabel, ink, ui::kRight | ui::kMiddle);
            break;
        }
        default:
            break;
    }
    // A knob a MIDI controller turns: a small mark, in the knob's gap or at
    // the control's corner.
    if (m.learned.contains(p.key)) {
        const ui::Point at =
            item.kind == Kind::Knob
                ? ui::Point{centre.x, centre.y + static_cast<float>(item.w) / 2 - 1}
                : ui::Point{rectOf(m, item).right() - 1, rectOf(m, item).y + 1};
        c.fillCircle(at, 2.4f, face);
        c.fillCircle(at, 1.6f, accent);
    }
}

void RackSpace::paintModule(ui::Canvas& c, const RackScene::Module& m, bool selected,
                            bool hot) const {
    const ui::Theme& t = c.theme();
    NVGcontext* vg = c.vg();
    const ui::Rect r = m.rect();
    const bool dark = t.color("ground").r < 0.5f;
    // A soft shadow lifts the panel off the space.
    nvgBeginPath(vg);
    nvgRect(vg, r.x - 12, r.y - 8, r.w + 24, r.h + 24);
    nvgFillPaint(vg, nvgBoxGradient(vg, r.x, r.y + 3, r.w, r.h, kRadius + 2, 12,
                                    nvgRGBAf(0, 0, 0, dark ? 0.45f : 0.16f), nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);

    const ui::Color face = t.color("module"), plate = t.color("module-title");
    c.fillRound(r, kRadius, face);
    const float head = m.plugin ? 58.0f : static_cast<float>(rack::kTitleHeight);
    c.save();
    c.clip({r.x, r.y, r.w, head});
    c.fillRound(r, kRadius, plate);
    c.restore();
    c.fillRect({r.x, r.y + head - 0.5f, r.w, 1}, t.color("rule").withAlpha(0.7f));
    const ui::Color ink = m.bypass ? t.color("ink-faint") : t.color("ink");
    if (m.plugin) {
        // A plug-in shows its name, and opens on a click.
        fitText(c, {r.x + 10, r.y + 10, r.w - 20, 22}, m.title, ui::Font::UiBold, 13.5f, ink,
                ui::kCenter | ui::kMiddle);
        c.text({r.x + 10, r.y + 32, r.w - 20, 14}, m.subtitle.empty() ? "Plug-in" : m.subtitle,
               ui::Font::Ui, 9.5f, t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
        // The editor's mark: a small window, top right.
        const ui::Rect w{r.right() - 19, r.y + 7, 11, 9};
        const ui::Color mark = hot ? t.color("accent") : t.color("ink-faint");
        c.strokeRound(w, 1.5f, mark, 1.2f);
        c.fillRect({w.x, w.y, w.w, 2.2f}, mark);
    } else {
        std::string title = m.title;
        fitText(c, {r.x + 7, r.y, r.w - 14, head}, title, ui::Font::UiBold, kTitleFont, ink,
                ui::kCenter | ui::kMiddle);
    }
    // The module's own drawing, under its controls.
    if (m.type && m.type->code() && m.type->code()->draw()) {
        if (ModuleFace* drawn = faceOf(m)) {
            drawn->update(m);
            nvgSave(vg);
            nvgTranslate(vg, r.x, r.y);
            nvgIntersectScissor(vg, 0, 0, r.w, r.h);
            drawn->draw(vg, t, c.fonts(), m.w, m.h);
            nvgRestore(vg);
            if (drawn->stopped())
                c.text({r.x + 6, r.bottom() - 16, r.w - 12, 12}, "draw() ran too long",
                       ui::Font::Ui, 8.5f, t.color("clip"), ui::kCenter | ui::kMiddle);
        }
    }
    if (!m.type) {  // an unknown type: kept, silent, said
        c.text({r.x + 8, r.y + head + 8, r.w - 16, 14}, m.subtitle, ui::Font::UiBold, 9.5f,
               t.color("clip"), ui::kCenter | ui::kMiddle);
        c.text({r.x + 8, r.y + head + 24, r.w - 16, 24}, m.typeId, ui::Font::Mono, 7.5f,
               t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
    } else {
        std::optional<std::size_t> hotItem;
        if (pointer_ && hot && drag_.kind == Drag::Kind::None) {
            const Hit h = hitAt(toSpace(*pointer_));
            if (h.kind == Hit::Kind::Control || h.kind == Hit::Kind::Jack ||
                h.kind == Hit::Kind::Ring)
                hotItem = h.item;
        }
        for (std::size_t k = 0; k < m.face.items.size(); ++k)
            paintItem(c, m, m.face.items[k], k, hotItem == k);
    }
    if (m.bypass) {
        c.save();
        c.clip({r.x, r.y + head, r.w, r.h - head});
        c.fillRound(r, kRadius, face.withAlpha(0.6f));
        c.restore();
        c.text({r.x + 6, r.y + head + 2, r.w - 12, 12}, "Bypassed", ui::Font::UiBold, 8.5f,
               t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
    }
    c.strokeRound({r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, kRadius,
                  !m.type ? t.color("clip").withAlpha(0.7f) : t.color("rule"), 1);
    if (selected) {
        const float w = static_cast<float>(2.0 / view_.zoom);
        c.strokeRound({r.x - w, r.y - w, r.w + 2 * w, r.h + 2 * w}, kRadius + w, t.color("focus"),
                      w);
    }
    // An Out module's channel hangs under it, as a tag.
    if (m.out) {
        const bool fed = !m.subtitle.empty();
        const std::string tag = fed ? "→ " + m.subtitle : "→ no channel";
        c.text({r.x - 20, r.bottom() + 4, r.w + 40, 12}, tag, ui::Font::Ui, 9.5f,
               fed ? t.color("ink-quiet") : t.color("ink-faint"), ui::kCenter | ui::kMiddle);
    }
}

void RackSpace::paintCables(ui::Canvas& c) const {
    const ui::Theme& t = c.theme();
    NVGcontext* vg = c.vg();
    const bool many =
        static_cast<int>(scene_.cables.size()) >= kManyCables || drag_.kind == Drag::Kind::Cable;
    // A feedback cable: dashes over a dim cord.
    auto dashes = [&](const Curve& k, ui::Color col, float width, float alpha) {
        constexpr int n = 40;
        ui::Point prev = k.p0;
        for (int i = 1; i <= n; ++i) {
            const ui::Point q = at(k, static_cast<float>(i) / n);
            if (i % 2 == 1) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, prev.x, prev.y);
                nvgLineTo(vg, q.x, q.y);
                nvgLineCap(vg, NVG_ROUND);
                nvgStrokeWidth(vg, width);
                nvgStrokeColor(vg, nvg(col.withAlpha(alpha)));
                nvgStroke(vg);
            }
            prev = q;
        }
    };
    auto stroke = [&](const Curve& k, ui::Color col, float width, float alpha) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, k.p0.x, k.p0.y);
        nvgBezierTo(vg, k.c0.x, k.c0.y, k.c1.x, k.c1.y, k.p1.x, k.p1.y);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeWidth(vg, width + 1.6f);
        nvgStrokeColor(vg, nvg(mix(col, {0, 0, 0, 1}, 0.55f).withAlpha(0.85f * alpha)));
        nvgStroke(vg);
        nvgStrokeWidth(vg, width);
        nvgStrokeColor(vg, nvg(col.withAlpha(alpha)));
        nvgStroke(vg);
        // A highlight along the top makes it read as a cord.
        nvgStrokeWidth(vg, std::max(0.8f, width * 0.28f));
        nvgStrokeColor(vg, nvg(mix(col, {1, 1, 1, 1}, 0.45f).withAlpha(0.55f * alpha)));
        nvgStroke(vg);
    };
    auto plug = [&](ui::Point p, ui::Color col, float alpha, bool small) {
        const float r = small ? 3.5f : kJackR - 3.0f;
        c.fillCircle(p, r + 1.2f, mix(col, {0, 0, 0, 1}, 0.55f).withAlpha(alpha));
        c.fillCircle(p, r, col.withAlpha(alpha));
        c.fillCircle(p, r * 0.45f, mix(col, {0, 0, 0, 1}, 0.35f).withAlpha(alpha));
    };
    auto colorOf = [&](const RackScene::Cable& k) {
        if (!k.color.empty())
            if (auto col = t.trackColor(k.color)) return *col;
        return t.color(signalToken(k.signal));
    };
    for (std::size_t i = 0; i < scene_.cables.size(); ++i) {
        const auto& k = scene_.cables[i];
        if (drag_.kind == Drag::Kind::Cable && drag_.replace == k.id) continue;  // in the hand
        const auto a = scene_.endPoint(k.from, true), b = scene_.endPoint(k.to, false);
        if (!a || !b) continue;
        const bool focus = hotCable_ == i ||
                           (hot_ && (k.from.object == *hot_ || k.to.object == *hot_)) ||
                           selection_.contains(k.from.object) || selection_.contains(k.to.object);
        const float alpha = !many || focus ? 1.0f : 0.38f;
        const ui::Color col = colorOf(k);
        const auto* src = scene_.module(k.from.object);
        bool poly = false;
        if (src && src->live && src->type) {
            const int out = src->type->output(k.from.key);
            poly = out >= 0 && static_cast<std::size_t>(out) < src->telemetry.channels.size() &&
                   src->telemetry.channels[static_cast<std::size_t>(out)] > 1;
        }
        if (k.feedback) {
            stroke(curveOf(*a, *b), col, poly ? 4.6f : 3.0f, alpha * 0.35f);
            dashes(curveOf(*a, *b), col, poly ? 4.6f : 3.0f, alpha);
        } else {
            stroke(curveOf(*a, *b), col, poly ? 4.6f : 3.0f, alpha);
        }
        plug(*a, col, alpha, false);
        plug(*b, col, alpha, k.param);
    }
    if (drag_.kind == Drag::Kind::Cable && pointer_) {
        const auto start = scene_.endPoint(drag_.from, drag_.fromOutput);
        if (start) {
            const ui::Point end = toSpace(*pointer_);
            const ui::Color col =
                t.color(signalToken(drag_.fromOutput ? scene_.signalOf(drag_.from, true)
                                                     : scene_.signalOf(drag_.from, false)));
            const Curve k = drag_.fromOutput ? curveOf(*start, end) : curveOf(end, *start);
            stroke(k, col, 3.0f, 1.0f);
            plug(*start, col, 1.0f, false);
            plug(end, col, 1.0f, false);
        }
    }
}

void RackSpace::paint(ui::Canvas& c) {
    const ui::Theme& t = c.theme();
    const ui::Rect b = bounds();
    c.fillRect(b, t.color("ground"));
    c.save();
    c.clip(b);
    NVGcontext* vg = c.vg();
    nvgSave(vg);
    nvgTranslate(vg, static_cast<float>(D(b.x) - view_.x * view_.zoom),
                 static_cast<float>(D(b.y) - view_.y * view_.zoom));
    nvgScale(vg, static_cast<float>(view_.zoom), static_cast<float>(view_.zoom));
    const ui::Rect seen = visibleSpace();
    const ui::Rect reach{seen.x - 40, seen.y - 40, seen.w + 80, seen.h + 80};
    auto paintSome = [&](bool selected) {
        for (const auto& m : scene_.modules) {
            if (selection_.contains(m.id) != selected) continue;
            if (reach.intersect(m.rect()).empty()) continue;
            paintModule(c, m, selected, hot_ == m.id);
        }
    };
    paintSome(false);
    paintSome(true);
    paintCables(c);
    nvgRestore(vg);
    if (drag_.kind == Drag::Kind::Marquee && drag_.moved) {
        const ui::Rect r{
            std::min(drag_.start.x, drag_.last.x), std::min(drag_.start.y, drag_.last.y),
            std::fabs(drag_.last.x - drag_.start.x), std::fabs(drag_.last.y - drag_.start.y)};
        c.fillRect(r, t.color("selection").withAlpha(0.35f));
        c.strokeRect(r, t.color("focus").withAlpha(0.8f));
    }
    if (dropAt_ && !dropName_.empty()) {
        const ui::Rect g{dropAt_->x, dropAt_->y, static_cast<float>(110 * view_.zoom),
                         static_cast<float>(rack::kTitleHeight * view_.zoom)};
        c.fillRound(g, 4, t.color("module-title").withAlpha(0.85f));
        c.strokeRound(g, 4, t.color("focus"));
        c.text(g.inset(4), dropName_, ui::Font::UiBold, 11, t.color("ink"),
               ui::kCenter | ui::kMiddle);
    }
    if (scene_.modules.empty() && !emptyText_.empty())
        c.text({b.x, b.y + b.h / 2 - 10, b.w, 20}, emptyText_, ui::Font::Ui, t.type("small"),
               t.color("ink-faint"), ui::kCenter | ui::kMiddle);
    c.restore();
}

// --- input ---------------------------------------------------------------------------

void RackSpace::hover(std::optional<model::Id> module) {
    if (module == hot_) return;
    hot_ = module;
    invalidate();
}

ModuleFace* RackSpace::faceOf(const RackScene::Module& m) const {
    if (!m.type) return nullptr;
    auto& face = faces_[m.id];
    if (!face || face->type() != m.type.get()) face = ModuleFace::make(m.type);
    return face.get();
}

std::string RackSpace::nextDisplayGesture(model::Id module) {
    return "rack.display:" + module.str() + ":" + std::to_string(++displayGesture_);
}

void RackSpace::sendPointer(model::Id id, script::PointerAction action, ui::Point window,
                            ui::Point moved, double wheel, bool doubleClick, unsigned mods,
                            const std::string& gesture) {
    RackScene::Module* m = nullptr;
    for (auto& x : scene_.modules)
        if (x.id == id) m = &x;
    if (!m) return;
    ModuleFace* face = faceOf(*m);
    if (!face || !face->hearsPointer()) return;
    face->update(*m);  // the knobs as they are now
    const ui::Point sp = toSpace(window);
    ModuleFace::Pointer p;
    p.action = action;
    p.x = D(sp.x) - m->x;
    p.y = D(sp.y) - m->y;
    p.dx = D(moved.x) / view_.zoom;
    p.dy = D(moved.y) / view_.zoom;
    p.wheel = wheel;
    p.doubleClick = doubleClick;
    p.shift = mods & ui::kShift;
    p.ctrl = mods & ui::kCtrl;
    p.alt = mods & ui::kAlt;
    const std::vector<ModuleFace::Edit> edits = face->pointer(p);
    for (const auto& e : edits)
        if (e.param >= 0 && static_cast<std::size_t>(e.param) < m->values.size())
            m->values[static_cast<std::size_t>(e.param)] = e.value;
    invalidate();  // the display redraws after every call
    if (!edits.empty() && hooks_.displayEdit) hooks_.displayEdit(id, edits, gesture);
}

bool RackSpace::onPointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    const ui::Point sp = toSpace(e.pos);
    if (e.type == T::Leave) {
        if (const auto left = std::exchange(displayHot_, std::nullopt))
            sendPointer(*left, script::PointerAction::Leave, displayLast_, {}, 0, false, 0, {});
        pointer_.reset();
        hover(std::nullopt);
        hotCable_.reset();
        invalidate();
        return true;
    }
    if (e.type == T::Wheel) {
        // Over a display that hears the pointer, the wheel is the module's
        // (one undo step a burst); Ctrl+wheel still zooms.
        if (!(e.mods & ui::kCtrl) && drag_.kind == Drag::Kind::None)
            if (const Hit h = hitAt(sp); h.kind == Hit::Kind::Display) {
                const model::Id id = scene_.modules[h.module].id;
                const auto now = std::chrono::steady_clock::now();
                if (wheelModule_ != id || now - wheelAt_ > std::chrono::milliseconds(400)) {
                    wheelModule_ = id;
                    wheelGesture_ = nextDisplayGesture(id);
                }
                wheelAt_ = now;
                sendPointer(id, script::PointerAction::Wheel, e.pos, {}, D(e.wheelY), false, e.mods,
                            wheelGesture_);
                return true;
            }
        if (e.mods & ui::kCtrl) {
            zoomAt(e.pos, std::pow(1.15, static_cast<double>(e.wheelY)));
        } else {
            View v = view_;
            const double step = 40 / view_.zoom;
            if (e.mods & ui::kShift)
                v.x -= D(e.wheelY + e.wheelX) * step;
            else {
                v.y -= D(e.wheelY) * step;
                v.x += D(e.wheelX) * step;
            }
            setView(v);
        }
        return true;
    }
    if (e.type == T::Move || e.type == T::Enter) {
        pointer_ = e.pos;
        if (drag_.kind == Drag::Kind::None) {
            const Hit h = hitAt(sp);
            hover(h.kind == Hit::Kind::None ? std::nullopt
                                            : std::optional(scene_.modules[h.module].id));
            // A display that hears the pointer hears it move over it, and leave.
            const std::optional<model::Id> over = h.kind == Hit::Kind::Display
                                                      ? std::optional(scene_.modules[h.module].id)
                                                      : std::nullopt;
            if (displayHot_ && displayHot_ != over)
                sendPointer(*std::exchange(displayHot_, std::nullopt), script::PointerAction::Leave,
                            displayLast_, {}, 0, false, 0, {});
            if (over) {
                const ui::Point moved =
                    displayHot_ ? ui::Point{e.pos.x - displayLast_.x, e.pos.y - displayLast_.y}
                                : ui::Point{};
                displayHot_ = over;
                displayLast_ = e.pos;
                sendPointer(*over, script::PointerAction::Move, e.pos, moved, 0, false, e.mods, {});
            }
            const auto cable = h.kind == Hit::Kind::None ? cableNear(sp) : std::nullopt;
            if (cable != hotCable_) hotCable_ = cable;
            invalidate();
            return true;
        }
        const ui::Point d{e.pos.x - drag_.start.x, e.pos.y - drag_.start.y};
        if (!drag_.moved && std::hypot(d.x, d.y) >= 3) drag_.moved = true;
        if (drag_.kind == Drag::Kind::Display) {  // each move a Drag, wherever it goes
            const ui::Point moved{e.pos.x - drag_.last.x, e.pos.y - drag_.last.y};
            drag_.last = e.pos;
            displayLast_ = e.pos;
            sendPointer(drag_.display, script::PointerAction::Drag, e.pos, moved, 0, false, e.mods,
                        drag_.gesture);
            return true;
        }
        drag_.last = e.pos;
        switch (drag_.kind) {
            case Drag::Kind::Pan: {
                View v = drag_.view;
                v.x -= D(d.x) / v.zoom;
                v.y -= D(d.y) / v.zoom;
                setView(v);
                break;
            }
            case Drag::Kind::Marquee: {
                if (!drag_.moved) break;
                const ui::Point a = toSpace(drag_.start), z = toSpace(e.pos);
                const ui::Rect r{std::min(a.x, z.x), std::min(a.y, z.y), std::fabs(z.x - a.x),
                                 std::fabs(z.y - a.y)};
                std::set<model::Id> next = drag_.before;
                for (const auto& m : scene_.modules)
                    if (!r.intersect(m.rect()).empty()) next.insert(m.id);
                select(std::move(next));
                break;
            }
            case Drag::Kind::Move: {
                if (!drag_.moved) break;
                const double dx = D(d.x) / view_.zoom, dy = D(d.y) / view_.zoom;
                for (auto& [index, origin] : drag_.origins) {
                    auto& m = scene_.modules[index];
                    m.x = std::round(D(origin.x) + dx);
                    m.y = std::round(D(origin.y) + dy);
                }
                break;
            }
            case Drag::Kind::Control: {
                auto& m = scene_.modules[drag_.module];
                const PanelItem& item = m.face.items[drag_.item];
                const auto& p = m.type->params()[static_cast<std::size_t>(item.index)];
                const bool fine = e.mods & ui::kShift;
                // Vertical travel, whatever the zoom: 160 points end to end.
                const bool across = item.kind == Kind::Slider && item.w >= item.h;
                const float travel = across ? d.x : -d.y;
                const double pos =
                    std::clamp(drag_.value + D(travel) / (fine ? 1600.0 : 160.0), 0.0, 1.0);
                const double value = rack::knobValue(p, pos);
                if (static_cast<std::size_t>(item.index) < m.values.size() &&
                    m.values[static_cast<std::size_t>(item.index)] != value) {
                    m.values[static_cast<std::size_t>(item.index)] = value;
                    if (hooks_.param) hooks_.param(m.id, item.index, value, ui::Gesture::Move);
                }
                // Shift taken or released mid-drag: carry on from here.
                if ((e.mods & ui::kShift) != (drag_.mods & ui::kShift)) {
                    drag_.value = pos;
                    drag_.start = e.pos;
                    drag_.mods = e.mods;
                }
                break;
            }
            case Drag::Kind::Amount: {
                const double next = drag_.amount - D(d.y) / (e.mods & ui::kShift ? 400.0 : 40.0);
                for (auto& k : scene_.cables)
                    if (k.id == drag_.cable) k.amount = next;
                if (hooks_.amount && drag_.cable)
                    hooks_.amount(*drag_.cable, next, ui::Gesture::Move);
                break;
            }
            default:
                break;
        }
        invalidate();
        return true;
    }
    if (e.type == T::Down) {
        if (root()) root()->focus(this);
        pointer_ = e.pos;
        drag_ = {};
        drag_.start = drag_.last = e.pos;
        drag_.mods = e.mods;
        if (e.button == 2 || (e.button == 1 && spaceHeld_)) {
            drag_.kind = Drag::Kind::Pan;
            drag_.view = view_;
            spaceUsed_ = spaceHeld_;
            return true;
        }
        const Hit h = hitAt(sp);
        if (e.button == 3) {
            if (h.kind == Hit::Kind::Control && hooks_.paramMenu) {
                const auto& m = scene_.modules[h.module];
                hooks_.paramMenu(m.id, m.face.items[h.item].index, e.pos);
            } else if (h.kind != Hit::Kind::None) {
                const model::Id id = scene_.modules[h.module].id;
                if (!selection_.contains(id)) select({id});
                if (hooks_.moduleMenu) hooks_.moduleMenu(id, e.pos);
            } else if (hooks_.spaceMenu) {
                hooks_.spaceMenu(e.pos, sp);
            }
            return true;
        }
        if (e.button != 1) return true;
        switch (h.kind) {
            case Hit::Kind::None: {
                drag_.kind = Drag::Kind::Marquee;
                if (e.mods & (ui::kCtrl | ui::kShift))
                    drag_.before = selection_;
                else
                    select({});
                break;
            }
            case Hit::Kind::Body: {
                const model::Id id = scene_.modules[h.module].id;
                if (e.mods & ui::kCtrl) {
                    auto next = selection_;
                    if (!next.erase(id)) next.insert(id);
                    select(std::move(next));
                    if (!selection_.contains(id)) break;  // toggled off: no drag
                } else if (!selection_.contains(id)) {
                    select({id});
                }
                drag_.kind = Drag::Kind::Move;
                drag_.module = h.module;
                for (std::size_t i = 0; i < scene_.modules.size(); ++i)
                    if (selection_.contains(scene_.modules[i].id))
                        drag_.origins.emplace_back(
                            i, ui::Point{static_cast<float>(scene_.modules[i].x),
                                         static_cast<float>(scene_.modules[i].y)});
                break;
            }
            case Hit::Kind::Control: {
                auto& m = scene_.modules[h.module];
                const PanelItem& item = m.face.items[h.item];
                const auto& p = m.type->params()[static_cast<std::size_t>(item.index)];
                const auto index = static_cast<std::size_t>(item.index);
                const double v = index < m.values.size() ? m.values[index] : p.def;
                auto set = [&](double value) {
                    if (index < m.values.size()) m.values[index] = value;
                    if (hooks_.param) hooks_.param(m.id, item.index, value, ui::Gesture::Set);
                    invalidate();
                };
                if (item.kind == Kind::Toggle) {
                    set(v >= 0.5 ? 0.0 : 1.0);
                    break;
                }
                if (e.clicks >= 2) {  // back to its default
                    set(p.def);
                    break;
                }
                if (item.kind == Kind::Choice) {  // the next option, round
                    set(v + 1 > p.max ? p.min : v + 1);
                    break;
                }
                if (item.kind == Kind::Readout) break;
                drag_.kind = Drag::Kind::Control;
                drag_.module = h.module;
                drag_.item = h.item;
                drag_.value = rack::knobPosition(p, v);
                ++gesture_;
                if (hooks_.param) hooks_.param(m.id, item.index, v, ui::Gesture::Begin);
                break;
            }
            case Hit::Kind::Display: {
                drag_.kind = Drag::Kind::Display;
                drag_.module = h.module;
                drag_.display = scene_.modules[h.module].id;
                drag_.gesture = nextDisplayGesture(drag_.display);
                displayHot_ = drag_.display;
                displayLast_ = e.pos;
                sendPointer(drag_.display, script::PointerAction::Press, e.pos, {}, 0,
                            e.clicks >= 2, e.mods, drag_.gesture);
                break;
            }
            case Hit::Kind::Ring: {
                drag_.kind = Drag::Kind::Amount;
                drag_.module = h.module;
                drag_.item = h.item;
                drag_.cable = h.cable;
                for (const auto& k : scene_.cables)
                    if (k.id == h.cable) drag_.amount = k.amount;
                if (hooks_.amount && h.cable)
                    hooks_.amount(*h.cable, drag_.amount, ui::Gesture::Begin);
                break;
            }
            case Hit::Kind::Jack: {
                const auto& m = scene_.modules[h.module];
                const PanelItem& item = m.face.items[h.item];
                drag_.kind = Drag::Kind::Cable;
                drag_.module = h.module;
                drag_.item = h.item;
                const model::CableEnd end = endOf(m, item);
                if (item.kind == Kind::Output) {
                    drag_.from = end;
                    drag_.fromOutput = true;
                } else if (const auto cables = scene_.cablesAt(end, false); !cables.empty()) {
                    // Pick up the input's last cable by its plug: it moves, or goes.
                    drag_.from = cables.back()->from;
                    drag_.fromOutput = true;
                    drag_.replace = cables.back()->id;
                } else {
                    drag_.from = end;  // drawn backwards, to an output
                    drag_.fromOutput = false;
                }
                break;
            }
        }
        invalidate();
        return true;
    }
    if (e.type == T::Up || e.type == T::Cancel) {
        drag_.last = e.pos;
        endDrag(e.type == T::Up);
        if (spaceHeld_ && !SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_SPACE] &&
            SDL_GetKeyboardFocus())
            spaceHeld_ = false;
        return true;
    }
    return false;
}

void RackSpace::endDrag(bool commit) {
    Drag d = std::exchange(drag_, {});
    invalidate();
    switch (d.kind) {
        case Drag::Kind::Move: {
            if (!d.moved) {
                // A click: a plugin module opens its editor.
                if (commit && d.module < scene_.modules.size() && scene_.modules[d.module].plugin &&
                    !(d.mods & (ui::kCtrl | ui::kShift)) && hooks_.open)
                    hooks_.open(scene_.modules[d.module].id);
                break;
            }
            std::vector<Move> moves;
            for (auto& [index, origin] : d.origins) {
                auto& m = scene_.modules[index];
                if (!commit) {
                    m.x = D(origin.x);
                    m.y = D(origin.y);
                } else if (m.x != D(origin.x) || m.y != D(origin.y)) {
                    moves.push_back({m.id, m.x, m.y});
                }
            }
            if (!moves.empty() && hooks_.move) hooks_.move(moves);
            break;
        }
        case Drag::Kind::Control: {
            if (d.module >= scene_.modules.size()) break;
            const auto& m = scene_.modules[d.module];
            if (d.item >= m.face.items.size()) break;
            const int index = m.face.items[d.item].index;
            if (hooks_.param && index >= 0 && static_cast<std::size_t>(index) < m.values.size())
                hooks_.param(m.id, index, m.values[static_cast<std::size_t>(index)],
                             ui::Gesture::End);
            break;
        }
        case Drag::Kind::Display:
            sendPointer(d.display, script::PointerAction::Release, d.last, {}, 0, false, d.mods,
                        d.gesture);
            break;
        case Drag::Kind::Amount:
            if (hooks_.amount && d.cable)
                for (const auto& k : scene_.cables)
                    if (k.id == d.cable) hooks_.amount(k.id, k.amount, ui::Gesture::End);
            break;
        case Drag::Kind::Cable: {
            if (!commit || !d.moved) break;
            const Hit h = hitAt(toSpace(d.last));
            std::optional<model::CableEnd> target;
            if (h.kind == Hit::Kind::Jack || h.kind == Hit::Kind::Control ||
                h.kind == Hit::Kind::Ring) {
                const auto& m = scene_.modules[h.module];
                const PanelItem& item = m.face.items[h.item];
                const bool output = item.kind == Kind::Output;
                // An input or a parameter when the cable comes from an output;
                // an output when it was drawn backwards from an input.
                if (d.fromOutput ? !output && item.kind != Kind::Toggle : output)
                    target = endOf(m, item);
            }
            if (target && !target->key.empty()) {
                if (hooks_.connect) {
                    if (d.fromOutput)
                        hooks_.connect(d.from, *target, d.replace);
                    else
                        hooks_.connect(*target, d.from, std::nullopt);
                }
            } else if (d.replace && hooks_.disconnect) {
                hooks_.disconnect(*d.replace);  // pulled out and let go
            }
            break;
        }
        default:
            break;
    }
}

bool RackSpace::onKey(const ui::KeyEvent& e) {
    if (e.key == SDLK_SPACE && !e.mods) {
        if (!spaceHeld_) {
            spaceHeld_ = true;
            spaceUsed_ = false;
        }
        return true;  // the transport hears it on release, if no drag used it
    }
    if (e.key == SDLK_ESCAPE && drag_.kind != Drag::Kind::None) {
        endDrag(false);
        return true;
    }
    if ((e.key == SDLK_DELETE || e.key == SDLK_BACKSPACE) && !e.mods && !selection_.empty()) {
        if (hooks_.deleteSelected) hooks_.deleteSelected();
        return true;
    }
    if (e.key == SDLK_A && (e.mods & ui::kCtrl)) {
        std::set<model::Id> all;
        for (const auto& m : scene_.modules) all.insert(m.id);
        select(std::move(all));
        return true;
    }
    if (e.key == SDLK_HOME || (e.key == SDLK_0 && (e.mods & ui::kCtrl))) {
        if (e.key == SDLK_HOME)
            frameAll();
        else
            zoomAt({bounds().x + bounds().w / 2, bounds().y + bounds().h / 2}, 1.0 / view_.zoom);
        return true;
    }
    if ((e.key == SDLK_EQUALS || e.key == SDLK_PLUS || e.key == SDLK_KP_PLUS) &&
        (e.mods & ui::kCtrl)) {
        zoomAt(
            pointer_.value_or(ui::Point{bounds().x + bounds().w / 2, bounds().y + bounds().h / 2}),
            1.25);
        return true;
    }
    if ((e.key == SDLK_MINUS || e.key == SDLK_KP_MINUS) && (e.mods & ui::kCtrl)) {
        zoomAt(
            pointer_.value_or(ui::Point{bounds().x + bounds().w / 2, bounds().y + bounds().h / 2}),
            0.8);
        return true;
    }
    return false;
}

bool RackSpace::coalescesDrag() const { return drag_.kind != Drag::Kind::None; }

bool RackSpace::onKeyUp(const ui::KeyEvent& e) {
    if (e.key != SDLK_SPACE || !spaceHeld_) return false;
    spaceHeld_ = false;
    if (!spaceUsed_ && drag_.kind != Drag::Kind::Pan && hooks_.spaceTap) hooks_.spaceTap();
    return true;
}

}  // namespace supple::gui
