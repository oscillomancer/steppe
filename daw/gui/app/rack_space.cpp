// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_space.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <nanovg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "rack_labels.h"
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
constexpr float kLabel = 8.5f;    // labels under knobs and jacks
constexpr float kSocketR = 5.5f;  // a plug-in parameter's socket on a plugin module's edge
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
    if (w > box.w && w > 0) {
        // Widths are measured in whole pixels: step down until it does fit.
        const float floor = size * 0.75f;
        size = std::max(floor, size * box.w / w);
        while (size > floor && c.textWidth(s, font, size) > box.w)
            size = std::max(floor, size - 0.25f);
    }
    c.text(box, s, font, size, col, align);
}

// A name inside its control's box (a toggle's): as fitText, then on two
// lines when the box is tall enough, else cut with an ellipsis.
void fitLines(ui::Canvas& c, const ui::Rect& box, const std::string& s, ui::Font font, float size,
              ui::Color col) {
    if (c.textWidth(s, font, size) <= box.w || c.textWidth(s, font, racks::kLabelMin) <= box.w)
        return fitText(c, box, s, font, size, col, ui::kCenter | ui::kMiddle);
    for (float z = size; z >= racks::kLabelMin - 0.01f; z -= 0.25f) {
        const float line = z * 1.15f;
        if (2 * line > box.h + 2) continue;
        const auto lines = c.wrap(s, font, z, box.w);
        if (lines.size() != 2) continue;
        const float y = box.y + box.h / 2 - line;
        for (std::size_t k = 0; k < 2; ++k)
            c.text({box.x - 1, y + line * static_cast<float>(k), box.w + 2, line}, lines[k], font,
                   z, col, ui::kCenter | ui::kMiddle);
        return;
    }
    c.text(box, s, font, racks::kLabelMin, col, ui::kCenter | ui::kMiddle);
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
            m.levels = before->levels;
            m.measured = before->measured;
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
    if (r.empty() || bounds().empty()) {
        setView({-40, -40, 1});
        return;
    }
    frameRect(r, 1.0);
}

void RackSpace::frame(const std::set<model::Id>& ids) {
    ui::Rect r;
    bool any = false;
    for (const auto& m : scene_.modules)
        if (ids.contains(m.id)) {
            r = any ? r.unite(m.rect()) : m.rect();
            any = true;
        }
    if (any) frameRect(r, 1.5);
}

ui::Point RackSpace::pastePoint() const {
    if (pointer_ && bounds().contains(*pointer_)) return toSpace(*pointer_);
    const ui::Rect v = visibleSpace();
    return {v.x + v.w / 2 - 60, v.y + v.h / 2 - 60};
}

void RackSpace::frameRect(const ui::Rect& r, double maxZoom) {
    const ui::Rect b = bounds();
    if (b.empty()) return;
    constexpr double kPad = 48;
    const double zoom = std::clamp(std::min((D(b.w) - 2 * kPad) / std::max(1.0, D(r.w)),
                                            (D(b.h) - 2 * kPad) / std::max(1.0, D(r.h))),
                                   kMinZoom, maxZoom);
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

void RackSpace::setStartHint(bool on) {
    if (on == startHint_) return;
    startHint_ = on;
    if (!on) {
        startLinks_[0] = startLinks_[1] = {};
        startHot_.reset();
    }
    invalidate();
}

std::optional<int> RackSpace::startLinkAt(ui::Point window) const {
    if (!startHint_) return std::nullopt;
    for (int i = 0; i < 2; ++i)
        if (startLinks_[i].w > 0 && startLinks_[i].inset(-3).contains(window)) return i;
    return std::nullopt;
}

std::optional<ui::Point> RackSpace::startLinkAt(bool patch) const {
    const ui::Rect& r = startLinks_[patch ? 1 : 0];
    if (!startHint_ || r.w <= 0) return std::nullopt;
    return ui::Point{r.x + r.w / 2, r.y + r.h / 2};
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

double RackSpace::outputLevel(const RackScene::Module& src, int out) const {
    if (!src.type || out < 0) return 0;
    const auto i = static_cast<std::size_t>(out);
    double peak = 0, value = 0;
    if (src.live && i < src.telemetry.peaks.size()) {
        peak = D(src.telemetry.peaks[i]);
        value = src.telemetry.outputs[i];
    } else if (src.measured && i < src.levels.size()) {
        // Only a peak: a gate that rose in the block is on.
        peak = value = D(src.levels[i]);
    } else {
        return 0;
    }
    const auto& port = src.type->output(out);
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
}

// How lit a jack is, 0..1: an output's own level; an input's, its cables'.
double RackSpace::jackLevel(const RackScene::Module& m, const PanelItem& item) const {
    if (!m.type) return 0;
    if (item.kind == Kind::Output) return outputLevel(m, item.index);
    double level = 0;
    const model::CableEnd end = endOf(m, item);
    for (const auto& c : scene_.cables)
        if (c.to == end)
            if (const auto* src = scene_.module(c.from.object); src && src->type)
                level = std::max(level, outputLevel(*src, src->type->output(c.from.key)));
    return level;
}

std::vector<model::Id> RackSpace::cableSources(const ui::Rect& seen) const {
    std::vector<model::Id> out;
    for (const auto& k : scene_.cables) {
        const auto* src = scene_.module(k.from.object);
        if (!src || std::find(out.begin(), out.end(), src->id) != out.end()) continue;
        const auto a = scene_.endPoint(k.from, true), b = scene_.endPoint(k.to, false);
        if (!a || !b) continue;
        // The curve lies inside its points' hull.
        const Curve c = curveOf(*a, *b);
        const float x0 = std::min({c.p0.x, c.c0.x, c.c1.x, c.p1.x});
        const float y0 = std::min({c.p0.y, c.c0.y, c.c1.y, c.p1.y});
        const float x1 = std::max({c.p0.x, c.c0.x, c.c1.x, c.p1.x});
        const float y1 = std::max({c.p0.y, c.c0.y, c.c1.y, c.p1.y});
        if (!seen.intersect({x0 - 4, y0 - 4, x1 - x0 + 8, y1 - y0 + 8}).empty())
            out.push_back(src->id);
    }
    return out;
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
        // A plug-in parameter's socket sets its last cable's amount.
        if (m.fx)
            for (std::size_t i = 0; i < m.mods.size(); ++i)
                if (dist(p, RackScene::modSocket(m, i)) <= kSocketR + 2.5f) {
                    const auto cables = scene_.cablesAt({*m.fx, m.mods[i]}, false);
                    if (!cables.empty()) return {Hit::Kind::Socket, *it, i, cables.back()->id};
                }
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

std::optional<ui::Point> RackSpace::socketAt(model::Id module, const std::string& key) const {
    const auto* m = scene_.module(module);
    if (!m) return std::nullopt;
    const auto it = std::find(m->mods.begin(), m->mods.end(), key);
    if (it == m->mods.end()) return std::nullopt;
    return toWindow(RackScene::modSocket(*m, static_cast<std::size_t>(it - m->mods.begin())));
}

std::optional<ui::Point> RackSpace::cableAt(model::Id cable) const {
    for (std::size_t i = 0; i < scene_.cables.size(); ++i) {
        const auto& k = scene_.cables[i];
        if (k.id != cable) continue;
        const auto a = scene_.endPoint(k.from, true), b = scene_.endPoint(k.to, false);
        if (!a || !b) return std::nullopt;
        // From the middle outwards: the first point clear of every module.
        for (int s = 0; s <= 40; ++s) {
            const float t = 0.5f + (s % 2 ? 1.0f : -1.0f) * static_cast<float>((s + 1) / 2) / 90;
            const ui::Point q = at(curveOf(*a, *b), t);
            if (hitAt(q).kind == Hit::Kind::None && cableNear(q) == i &&
                bounds().contains(toWindow(q)))
                return toWindow(q);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::pair<const RackScene::Cable*, ui::Point>> RackSpace::hovered() const {
    std::optional<model::Id> id;
    if (drag_.kind == Drag::Kind::Amount && drag_.fraction)
        id = drag_.cable;
    else if (drag_.kind == Drag::Kind::None && pointer_) {
        if (const Hit h = hitAt(toSpace(*pointer_)); h.kind == Hit::Kind::Socket)
            id = h.cable;
        else if (hotCable_ && *hotCable_ < scene_.cables.size() &&
                 scene_.cables[*hotCable_].to.object.kind == model::Kind::Fx)
            id = scene_.cables[*hotCable_].id;
    }
    if (!id) return std::nullopt;
    for (const auto& k : scene_.cables)
        if (k.id == *id)
            if (const auto b = scene_.endPoint(k.to, false)) return std::pair{&k, *b};
    return std::nullopt;
}

std::string RackSpace::hoverText() const {
    const auto h = hovered();
    if (!h) return {};
    const RackScene::Cable& k = *h->first;
    const RackScene::Module* m = scene_.moduleOfFx(k.to.object);
    char amount[32];
    std::snprintf(amount, sizeof amount, "%+.0f %%", k.amount * 100);
    return (m ? m->modName(k.to.key) : k.to.key) + " \u00b7 " + amount;
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
            if (words())
                c.text({centre.x + kJackR - 2, centre.y - kJackR - 3, 10, 8},
                       std::to_string(m.telemetry.channels[static_cast<std::size_t>(item.index)]),
                       ui::Font::MonoBold, 7.5f, quiet, ui::kLeft | ui::kMiddle);
        return;  // its label comes with the others, over every control
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
            break;  // its label comes with the others
        }
        case Kind::Toggle: {
            const ui::Rect r = rectOf(m, item);
            const bool on = v >= 0.5;
            c.fillRound(r, 3, on ? accent : t.color("module-title"));
            if (!on) c.strokeRound(r, 3, t.color("rule"));
            if (words())
                fitLines(c, r.inset(2), p.name, ui::Font::UiBold, kLabel, on ? face : quiet);
            break;
        }
        case Kind::Choice: {
            const ui::Rect r = rectOf(m, item);
            c.fillRound(r, 3, well);
            if (words())
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
            if (showValue && words())
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
            if (words())
                fitText(c, {r.x + 4, r.y, r.w / 2, r.h}, p.name, ui::Font::Ui, kLabel, quiet,
                        ui::kLeft | ui::kMiddle);
            if (words())
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

// The knobs' and jacks' labels, each in its own room (rack_labels.h); a
// knob under the pointer or in the hand shows its value there instead.
void RackSpace::paintLabels(ui::Canvas& c, const RackScene::Module& m,
                            std::optional<std::size_t> hotItem) const {
    const ui::Theme& t = c.theme();
    const ui::Color ink = t.color("ink"), quiet = t.color("ink-quiet");
    const auto labels = racks::faceLabels(*m.type, m.face, [&](std::string_view s, float size) {
        return c.textWidth(s, ui::Font::Ui, size);
    });
    const auto x0 = static_cast<float>(m.x), y0 = static_cast<float>(m.y);
    for (const auto& l : labels) {
        const PanelItem& item = m.face.items[l.item];
        const bool dragging = drag_.kind == Drag::Kind::Control &&
                              &scene_.modules[drag_.module] == &m && drag_.item == l.item;
        if (item.kind == Kind::Knob && (hotItem == l.item || dragging) && item.index >= 0 &&
            static_cast<std::size_t>(item.index) < m.type->params().size()) {
            const auto i = static_cast<std::size_t>(item.index);
            const sd::ParamModel& p = m.type->params()[i];
            const double v = i < m.values.size() ? m.values[i] : p.def;
            const std::string text = paramText(p, v);
            // Over the neighbours' labels when it needs the room, on the face's colour.
            const float w =
                std::max(l.cell.w, std::min(64.0f, c.textWidth(text, ui::Font::Mono, kLabel) + 6));
            const float cx = x0 + static_cast<float>(item.x);
            const ui::Rect box{cx - w / 2, y0 + l.cell.y, w, l.cell.h};
            if (w > l.cell.w) c.fillRound(box, 2, t.color("module"));
            fitText(c, box, text, ui::Font::Mono, kLabel, ink, ui::kCenter | ui::kMiddle);
            continue;
        }
        const ui::Color col = item.kind == Kind::Output ? ink : quiet;
        float y = y0 + l.box.y;
        for (std::size_t k = 0; k < l.lines.size(); ++k) {
            const float h = k == 0 ? l.cell.h : racks::kLabelLine2;
            c.text({x0 + l.box.x - 1, y, l.box.w + 2, h}, l.lines[k], ui::Font::Ui, l.size, col,
                   ui::kCenter | ui::kMiddle);
            y += h;
        }
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
        if (words())
            fitText(c, {r.x + 10, r.y + 10, r.w - 20, 22}, m.title, ui::Font::UiBold, 13.5f, ink,
                    ui::kCenter | ui::kMiddle);
        if (words())
            c.text({r.x + 10, r.y + 32, r.w - 20, 14}, m.subtitle.empty() ? "Plug-in" : m.subtitle,
                   ui::Font::Ui, 9.5f, t.color("ink-quiet"), ui::kCenter | ui::kMiddle);
        // A cable over it: a drop picks one of its plug-in's parameters.
        if (drag_.kind == Drag::Kind::Cable && drag_.fromOutput && pointer_ &&
            r.contains(toSpace(*pointer_))) {
            c.strokeRound({r.x - 1.5f, r.y - 1.5f, r.w + 3, r.h + 3}, kRadius + 1.5f,
                          t.color("accent"), 1.5f);
            c.fillRect({r.x + 1, r.y + 31, r.w - 2, 16}, plate);
            c.text({r.x + 6, r.y + 32, r.w - 12, 14}, "Drop to modulate a parameter", ui::Font::Ui,
                   9.0f, t.color("accent"), ui::kCenter | ui::kMiddle);
        }
        // The editor's mark: a small window, top right.
        const ui::Rect w{r.right() - 19, r.y + 7, 11, 9};
        const ui::Color mark = hot ? t.color("accent") : t.color("ink-faint");
        c.strokeRound(w, 1.5f, mark, 1.2f);
        c.fillRect({w.x, w.y, w.w, 2.2f}, mark);
    } else {
        std::string title = m.title;
        if (words())
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
        // Plates first, so a stack of outputs keeps every label; then the
        // controls; then the labels over them all.
        for (const auto& item : m.face.items)
            if (item.kind == Kind::Output) {
                const ui::Point centre = centreOf(m, item);
                c.fillRound({centre.x - 17, centre.y - kJackR - 4, 34, 2 * kJackR + 18}, 3, plate);
            }
        for (std::size_t k = 0; k < m.face.items.size(); ++k)
            paintItem(c, m, m.face.items[k], k, hotItem == k);
        if (words()) paintLabels(c, m, hotItem);
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
        if (words())
            c.text({r.x - 20, r.bottom() + 4, r.w + 40, 12}, tag, ui::Font::Ui, 9.5f,
                   fed ? t.color("ink-quiet") : t.color("ink-faint"), ui::kCenter | ui::kMiddle);
    }
}

ui::Color RackSpace::cableColor(const ui::Theme& t, const RackScene::Cable& k) const {
    if (!k.color.empty())
        if (auto col = t.trackColor(k.color)) return *col;
    return t.color(signalToken(k.signal));
}

// A plugin module's plug-in parameter sockets, over the cables into them:
// each shows its cables' amount as an arc around it (270 degrees at 100 %).
void RackSpace::paintSockets(ui::Canvas& c, const RackScene::Module& m) const {
    if (!m.fx) return;
    const ui::Theme& t = c.theme();
    std::optional<std::size_t> hot;
    if (drag_.kind == Drag::Kind::Amount && drag_.fraction &&
        drag_.module < scene_.modules.size() && &scene_.modules[drag_.module] == &m)
        hot = drag_.item;
    else if (drag_.kind == Drag::Kind::None && pointer_)
        if (const Hit h = hitAt(toSpace(*pointer_));
            h.kind == Hit::Kind::Socket && &scene_.modules[h.module] == &m)
            hot = h.item;
    for (std::size_t i = 0; i < m.mods.size(); ++i) {
        const auto cables = scene_.cablesAt({*m.fx, m.mods[i]}, false);
        if (cables.empty()) continue;
        const ui::Point at = RackScene::modSocket(m, i);
        const ui::Color col = cableColor(t, *cables.back());
        double sum = 0;
        for (const auto* k : cables) sum += k->amount;
        c.fillCircle(at, kSocketR + 1.2f, t.color("rule"));
        c.fillCircle(at, kSocketR, t.color("module-title"));
        c.fillCircle(at, kSocketR - 2, t.color("jack-hole"));
        c.fillCircle(at, 2.2f, col);
        const float ring = kSocketR + 3.5f;
        if (hot == i) c.strokeCircle(at, ring, t.color("ink-faint").withAlpha(0.5f), 1.5f);
        const float a0 = -kPi / 2;
        const float a1 = a0 + static_cast<float>(std::clamp(sum, -1.0, 1.0)) * kArcSpan;
        if (std::fabs(a1 - a0) > 0.01f)
            c.strokeArc(at, ring, std::min(a0, a1), std::max(a0, a1), col, 2);
        c.fillCircle({at.x + std::cos(a1) * ring, at.y + std::sin(a1) * ring}, 2.0f, col);
        if (hot == i) c.strokeCircle(at, kSocketR + 1.2f, t.color("focus"), 1.5f);
    }
}

// Zoomed out: each module's name at a readable size, across its top when it
// fits, else down its length, on a plate.
void RackSpace::paintNames(ui::Canvas& c) const {
    const ui::Theme& t = c.theme();
    NVGcontext* vg = c.vg();
    const ui::Rect b = bounds();
    constexpr float kSize = 10.5f;
    for (const auto& m : scene_.modules) {
        const ui::Point a = toWindow({static_cast<float>(m.x), static_cast<float>(m.y)});
        const ui::Rect r{a.x, a.y, static_cast<float>(m.w * view_.zoom),
                         static_cast<float>(m.h * view_.zoom)};
        if (r.intersect(b).empty() || r.w < 10) continue;
        const bool selected = selection_.contains(m.id), hot = hot_ == m.id;
        const ui::Color ink = m.bypass ? t.color("ink-faint") : t.color("ink");
        const ui::Color plate = t.color("module-title").withAlpha(0.92f);
        const float w = c.textWidth(m.title, ui::Font::UiBold, kSize) + 8;
        if (w <= r.w + 6 || r.h < w + 8) {
            const float bw = std::min(w, r.w + 6);
            const ui::Rect box{r.x + r.w / 2 - bw / 2, r.y + 2, bw, 15};
            c.fillRound(box, 3, plate);
            if (selected || hot) c.strokeRound(box, 3, t.color("focus"));
            c.save();
            c.clip(box);
            fitText(c, box, m.title, ui::Font::UiBold, kSize, ink, ui::kCenter | ui::kMiddle);
            c.restore();
            continue;
        }
        // Narrow and tall: the name reads down the module, as on hardware.
        nvgSave(vg);
        nvgTranslate(vg, r.x + r.w / 2 + 7.5f, r.y + 3);
        nvgRotate(vg, kPi / 2);
        const ui::Rect box{0, 0, w, 15};
        c.fillRound(box, 3, plate);
        if (selected || hot) c.strokeRound(box, 3, t.color("focus"));
        c.text(box, m.title, ui::Font::UiBold, kSize, ink, ui::kCenter | ui::kMiddle);
        nvgRestore(vg);
    }
}

// The hovered plug-in parameter's name and amount, over its socket.
void RackSpace::paintHover(ui::Canvas& c) const {
    const auto h = hovered();
    if (!h) return;
    const std::string text = hoverText();
    const ui::Theme& t = c.theme();
    const float w = c.textWidth(text, ui::Font::Ui, 10.5f) + 14;
    const ui::Point at = toWindow(h->second);
    const float top = at.y - static_cast<float>(D(kSocketR + 6) * view_.zoom) - 20;
    const ui::Rect box{at.x - w / 2, top, w, 18};
    c.fillRound(box, 4, t.color("panel"));
    c.strokeRound(box, 4, t.color("rule"));
    c.text(box, text, ui::Font::Ui, 10.5f, t.color("ink"), ui::kCenter | ui::kMiddle);
}

void RackSpace::paintCables(ui::Canvas& c) const {
    const ui::Theme& t = c.theme();
    NVGcontext* vg = c.vg();
    const bool many = static_cast<int>(scene_.cables.size()) >= kManyCables;
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
    // A cable carrying signal glows around its cord, as strong as its
    // source's output (a pitch, always there, only faintly).
    auto glow = [&](const Curve& k, ui::Color col, float width, float amount) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, k.p0.x, k.p0.y);
        nvgBezierTo(vg, k.c0.x, k.c0.y, k.c1.x, k.c1.y, k.p1.x, k.p1.y);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeWidth(vg, width + 3 + 6 * amount);
        nvgStrokeColor(vg, nvg(col.withAlpha(0.16f * amount)));
        nvgStroke(vg);
        nvgStrokeWidth(vg, width + 1.6f + 2.4f * amount);
        nvgStrokeColor(vg, nvg(col.withAlpha(0.3f * amount)));
        nvgStroke(vg);
    };
    auto glowOf = [&](const RackScene::Cable& k) -> float {
        const auto* src = scene_.module(k.from.object);
        if (!src || !src->type) return 0;
        const int out = src->type->output(k.from.key);
        if (out < 0) return 0;
        const double level = outputLevel(*src, out);
        if (src->type->output(out).signal == Signal::Pitch) return level > 0 ? 0.35f : 0.0f;
        return static_cast<float>(level);
    };
    auto colorOf = [&](const RackScene::Cable& k) { return cableColor(t, k); };
    // What the pointer or the selection is on keeps its cables bright and
    // on top; the rest step back, far when many cross.
    auto owner = [&](const model::CableEnd& end) {
        if (end.object.kind != model::Kind::Fx) return end.object;
        const auto* m = scene_.moduleOfFx(end.object);
        return m ? m->id : end.object;
    };
    auto focused = [&](std::size_t i) {
        const auto& k = scene_.cables[i];
        const model::Id from = owner(k.from), to = owner(k.to);
        return hotCable_ == i || (hot_ && (from == *hot_ || to == *hot_)) ||
               selection_.contains(from) || selection_.contains(to);
    };
    const bool anyFocus = hotCable_ || hot_ || !selection_.empty();
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < scene_.cables.size(); ++i)
        if (!anyFocus || !focused(i)) order.push_back(i);
    if (anyFocus)
        for (std::size_t i = 0; i < scene_.cables.size(); ++i)
            if (focused(i)) order.push_back(i);
    for (const std::size_t i : order) {
        const auto& k = scene_.cables[i];
        if (drag_.kind == Drag::Kind::Cable && drag_.replace == k.id) continue;  // in the hand
        const auto a = scene_.endPoint(k.from, true), b = scene_.endPoint(k.to, false);
        if (!a || !b) continue;
        const bool focus = anyFocus && focused(i);
        const float alpha = drag_.kind == Drag::Kind::Cable ? (many ? 0.3f : 0.6f)
                            : focus                         ? 1.0f
                            : anyFocus                      ? (many ? 0.12f : 0.45f)
                            : many                          ? 0.7f
                                                            : 1.0f;
        ui::Color col = colorOf(k);
        const auto* src = scene_.module(k.from.object);
        bool poly = false;
        if (src && src->live && src->type) {
            const int out = src->type->output(k.from.key);
            poly = out >= 0 && static_cast<std::size_t>(out) < src->telemetry.channels.size() &&
                   src->telemetry.channels[static_cast<std::size_t>(out)] > 1;
        }
        const float width = poly ? 4.6f : 3.0f;
        const Curve curve = curveOf(*a, *b);
        if (const float g = glowOf(k); g > 0.02f) {
            glow(curve, col, width, g * alpha);
            col = mix(col, {1, 1, 1, 1}, 0.3f * g);
        }
        if (k.feedback) {
            stroke(curve, col, width, alpha * 0.35f);
            dashes(curve, col, width, alpha);
        } else {
            stroke(curve, col, width, alpha);
        }
        plug(*a, col, alpha, false);
        if (k.to.object.kind != model::Kind::Fx) plug(*b, col, alpha, k.param);  // else a socket
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
    for (const auto& m : scene_.modules)
        if (!m.mods.empty() && !reach.intersect(m.rect()).empty()) paintSockets(c, m);
    nvgRestore(vg);
    if (!words()) paintNames(c);
    paintHover(c);
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
    startLinks_[0] = startLinks_[1] = {};
    if (startHint_ && drag_.kind == Drag::Kind::None) {
        // Under the rack's Outs and Ins in view, else in the middle.
        float y = b.y + b.h / 2;
        float under = -1;
        for (const auto& m : scene_.modules) {
            const ui::Rect r = m.rect();
            if (seen.intersect(r).empty()) continue;
            under = std::max(under, toWindow({r.x, r.bottom()}).y);
        }
        if (under >= 0) y = under + 40;
        const float size = 12.5f;
        const std::string a = "Add a module", mid = ", or ", z = "start from a patch";
        const float wa = c.textWidth(a, ui::Font::Ui, size),
                    wm = c.textWidth(mid, ui::Font::Ui, size),
                    wz = c.textWidth(z, ui::Font::Ui, size);
        float x = b.x + (b.w - wa - wm - wz) / 2;
        if (y + 20 < b.bottom()) {
            auto link = [&](int i, const std::string& text, float w) {
                const ui::Rect r{x, y, w, 20};
                const bool hot = startHot_ == i;
                const ui::Color ink = hot ? t.color("ink") : t.color("ink-quiet");
                c.text(r, text, ui::Font::Ui, size, ink, ui::kLeft | ui::kMiddle);
                c.line({r.x, r.y + 16}, {r.right(), r.y + 16}, ink.withAlpha(hot ? 0.8f : 0.35f),
                       1);
                startLinks_[i] = r;
                x += w;
            };
            link(0, a, wa);
            c.text({x, y, wm, 20}, mid, ui::Font::Ui, size, t.color("ink-faint"),
                   ui::kLeft | ui::kMiddle);
            x += wm;
            link(1, z, wz);
        }
    }
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
        startHot_.reset();
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
        // (one undo step a burst); Ctrl+wheel scrolls the space.
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
        if (!(e.mods & (ui::kCtrl | ui::kShift))) {
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
        if (const auto link = startLinkAt(e.pos); link != startHot_) {
            startHot_ = link;
            invalidate();
        }
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
                // A knob's in its unit, 40 points a unit; a plug-in parameter's a
                // fraction of its range, 160 points end to end. Shift: fine.
                const double scale =
                    (drag_.fraction ? 160.0 : 40.0) * (e.mods & ui::kShift ? 10 : 1);
                double next = drag_.amount - D(d.y) / scale;
                if (drag_.fraction) next = std::clamp(next, -1.0, 1.0);
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
    if (e.type == T::Down && e.button == 1 && !spaceHeld_)
        if (const auto link = startLinkAt(e.pos); link && hooks_.start) {
            hooks_.start(*link == 1);
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
            const auto cable = h.kind == Hit::Kind::None ? cableNear(sp) : std::nullopt;
            if (h.kind == Hit::Kind::Control && hooks_.paramMenu) {
                const auto& m = scene_.modules[h.module];
                hooks_.paramMenu(m.id, m.face.items[h.item].index, e.pos);
            } else if (h.kind == Hit::Kind::Socket && h.cable && hooks_.cableMenu) {
                hooks_.cableMenu(*h.cable, e.pos);
            } else if (cable && hooks_.cableMenu) {
                hooks_.cableMenu(scene_.cables[*cable].id, e.pos);
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
                if (e.clicks >= 2 && !(e.mods & (ui::kCtrl | ui::kShift)) && hooks_.quickAdd) {
                    hooks_.quickAdd(e.pos, sp, {}, std::nullopt, true);
                    return true;
                }
                drag_.kind = Drag::Kind::Marquee;
                if (e.mods & (ui::kCtrl | ui::kShift))
                    drag_.before = selection_;
                else
                    select({});
                break;
            }
            case Hit::Kind::Body: {
                const model::Id id = scene_.modules[h.module].id;
                if (e.mods & (ui::kCtrl | ui::kShift)) {
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
            case Hit::Kind::Socket:
            case Hit::Kind::Ring: {
                drag_.kind = Drag::Kind::Amount;
                drag_.fraction = h.kind == Hit::Kind::Socket;
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
            if (h.kind == Hit::Kind::Socket) {  // another cable to the same parameter
                const auto& m = scene_.modules[h.module];
                if (d.fromOutput && m.fx) target = model::CableEnd{*m.fx, m.mods[h.item]};
            } else if (d.fromOutput &&
                       (h.kind == Hit::Kind::Body || h.kind == Hit::Kind::Display) &&
                       scene_.modules[h.module].plugin) {
                // On a plugin module's body: which of its plug-in's parameters.
                if (hooks_.pickParam)
                    hooks_.pickParam(d.from, scene_.modules[h.module].id, d.replace, d.last);
                break;
            } else if (h.kind == Hit::Kind::Jack || h.kind == Hit::Kind::Control ||
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
            } else if (!d.replace && h.kind == Hit::Kind::None && hooks_.quickAdd) {
                // A new cable let go on nothing: a module for it to plug into.
                hooks_.quickAdd(d.last, toSpace(d.last), {}, d.from, d.fromOutput);
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
    // Delete, Backspace and Alt+C (the arrange view's and MIDI editor's keys)
    // delete the selection. They stop here even with nothing selected, so the
    // window's unhandled keys never delete the arrange view's selection.
    if (((e.key == SDLK_DELETE || e.key == SDLK_BACKSPACE) && !e.mods) ||
        (e.key == SDLK_C && e.mods == ui::kAlt)) {
        if (!selection_.empty() && hooks_.deleteSelected) hooks_.deleteSelected();
        return true;
    }
    if (e.mods & ui::kCtrl) {
        const auto run = [](const std::function<void()>& f) {
            if (f) f();
            return true;
        };
        if (e.key == SDLK_C && !selection_.empty()) return run(hooks_.copy);
        if (e.key == SDLK_X && !selection_.empty()) return run(hooks_.cut);
        if (e.key == SDLK_D && !selection_.empty()) return run(hooks_.duplicate);
        if (e.key == SDLK_V && hooks_.paste) {
            hooks_.paste(pastePoint());
            return true;
        }
    }
    // Arrows nudge the selection: 10 units, Shift 1.
    if (!selection_.empty() && !(e.mods & ui::kCtrl)) {
        const double step = e.mods & ui::kShift ? 1 : 10;
        const double dx = e.key == SDLK_LEFT ? -step : e.key == SDLK_RIGHT ? step : 0;
        const double dy = e.key == SDLK_UP ? -step : e.key == SDLK_DOWN ? step : 0;
        if (dx != 0 || dy != 0) {
            nudge(dx, dy);
            return true;
        }
    }
    if (e.key == SDLK_HOME && (e.mods & ui::kShift) && !selection_.empty()) {
        frame(selection_);
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

void RackSpace::nudge(double dx, double dy) {
    std::vector<Move> moves;
    for (auto& m : scene_.modules)
        if (selection_.contains(m.id)) {
            m.x += dx;
            m.y += dy;
            moves.push_back({m.id, m.x, m.y});
        }
    invalidate();
    if (!moves.empty() && hooks_.move) hooks_.move(moves);
}

// Typing a letter or digit into the space adds a module there by name.
bool RackSpace::onText(std::string_view text) {
    if (text.empty() || drag_.kind != Drag::Kind::None || !hooks_.quickAdd) return false;
    const auto ch = static_cast<unsigned char>(text.front());
    if (!std::isalnum(ch)) return false;
    const ui::Point at = pointer_ && bounds().contains(*pointer_)
                             ? *pointer_
                             : ui::Point{bounds().x + bounds().w / 2, bounds().y + bounds().h / 2};
    hooks_.quickAdd(at, toSpace(at), std::string(text), std::nullopt, true);
    return true;
}

bool RackSpace::coalescesDrag() const { return drag_.kind != Drag::Kind::None; }

bool RackSpace::onKeyUp(const ui::KeyEvent& e) {
    if (e.key != SDLK_SPACE || !spaceHeld_) return false;
    spaceHeld_ = false;
    if (!spaceUsed_ && drag_.kind != Drag::Kind::Pan && hooks_.spaceTap) hooks_.spaceTap();
    return true;
}

}  // namespace supple::gui
