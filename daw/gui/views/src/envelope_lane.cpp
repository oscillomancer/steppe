// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/envelope_lane.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "supple/model/env_shape.h"
#include "supple/ui/controls.h"

namespace supple::views {

double EnvScale::toNorm(double value) const {
    switch (kind) {
        case Kind::Volume:
            return ui::faderPosition(value);
        case Kind::Percent:
        case Kind::Pan:
            return std::clamp((value + 100.0) / 200.0, 0.0, 1.0);
        case Kind::Boost:
        case Kind::Mute:
            return value >= 0.5 ? 1.0 : 0.0;
        case Kind::Playrate:
            return (std::log2(std::clamp(value, 0.25, 4.0)) + 2.0) / 4.0;
        case Kind::Plugin:
            return param ? params::toNormalized(param->spec, value) : 0.0;
    }
    return 0.0;
}

double EnvScale::reference() const {
    if (kind == Kind::Playrate) return 1.0;
    return kind == Kind::Plugin && param ? param->spec.def : 0.0;
}

double EnvScale::fromNorm(double norm) const {
    const double n = std::clamp(norm, 0.0, 1.0);
    switch (kind) {
        case Kind::Volume:
            return std::max(-150.0, ui::faderDb(n));  // the floor stands for -inf
        case Kind::Percent:
        case Kind::Pan:
            return std::round(n * 200.0 - 100.0);
        case Kind::Boost:
        case Kind::Mute:
            return n >= 0.5 ? 1.0 : 0.0;
        case Kind::Playrate:
            return std::exp2(4.0 * n - 2.0);
        case Kind::Plugin:
            return param
                       ? params::clampPhysical(param->spec, params::fromNormalized(param->spec, n))
                       : 0.0;
    }
    return 0.0;
}

std::string EnvScale::text(double value) const {
    char buf[32];
    switch (kind) {
        case Kind::Volume:
            if (value <= -150.0 || !std::isfinite(value)) return "-inf dB";
            std::snprintf(buf, sizeof buf, "%+.1f dB", value);
            return buf;
        case Kind::Percent:
            std::snprintf(buf, sizeof buf, "%+.0f %%", value);
            return buf;
        case Kind::Boost:
            return value >= .5 ? "on" : "off";
        case Kind::Pan: {
            const long p = std::lround(value);
            if (p == 0) return "C";
            std::snprintf(buf, sizeof buf, "%s %ld", p < 0 ? "L" : "R", std::labs(p));
            return buf;
        }
        case Kind::Mute:
            return value >= 0.5 ? "muted" : "on";
        case Kind::Playrate:
            std::snprintf(buf, sizeof buf, "%.3fx", value);
            return buf;
        case Kind::Plugin:
            return param ? params::format(param->spec, value) : std::string{};
    }
    return {};
}

std::optional<EnvScale> envScale(const model::Envelope& env) {
    const auto kind = env.target.object.kind;
    if (kind != model::Kind::Track && kind != model::Kind::Route && kind != model::Kind::Take &&
        kind != model::Kind::Group)
        return std::nullopt;
    EnvScale s;
    if (env.target.param == "volume")
        s.kind = EnvScale::Kind::Volume;
    else if (env.target.param == "pan" || env.target.param == "pan_left" ||
             env.target.param == "pan_right")
        s.kind = EnvScale::Kind::Pan;
    else if (env.target.param == "width")
        s.kind = EnvScale::Kind::Percent;
    else if (env.target.param == "pan_boost")
        s.kind = EnvScale::Kind::Boost;
    else if (env.target.param == "mute")
        s.kind = EnvScale::Kind::Mute;
    else if (env.target.object == model::kMaster && env.target.param == "playrate")
        s.kind = EnvScale::Kind::Playrate;
    else
        return std::nullopt;
    return s;
}

std::string envTitle(const model::Envelope& env) {
    const std::string& p = env.target.param;
    if (p == "volume") return "Volume";
    if (p == "pan") return "Pan";
    if (p == "width") return "Width";
    if (p == "pan_left") return "Pan left";
    if (p == "pan_right") return "Pan right";
    if (p == "pan_boost") return "Gain compensation";
    if (p == "mute") return "Mute";
    if (p == "playrate") return "Playrate";
    return p;
}

double envValueAt(const model::Envelope& env, double beat, double fallback) {
    const auto& pts = env.points;
    if (pts.empty()) return fallback;
    if (beat <= pts.front().pos) return pts.front().value;
    if (beat >= pts.back().pos) return pts.back().value;
    const auto next = std::upper_bound(
        pts.begin(), pts.end(), beat, [](double b, const model::EnvPoint& p) { return b < p.pos; });
    const model::EnvPoint& b = *next;
    const model::EnvPoint& a = *(next - 1);
    if (b.pos <= a.pos) return b.value;
    const double t = (beat - a.pos) / (b.pos - a.pos);
    return a.value + (b.value - a.value) * model::shapeCurve(a.shape, a.tension, t);
}

EnvelopeCurve::EnvelopeCurve(const model::Session& session, model::Id envelope, EnvScale scale,
                             double from, double to)
    : scale_(std::move(scale)) {
    // Index source definitions and boundary points, never expanded loop passes.
    (void)from;
    (void)to;
    normalized_ = scale_.kind == EnvScale::Kind::Plugin && scale_.normalizedInterpolation;
    const auto* env = session.envelopes.find(envelope);
    if (!env) return;
    std::vector<const model::AutomationItem*> items;
    for (const auto& entry : session.autoItems)
        if (entry.value->envelope == envelope) items.push_back(entry.value.get());
    std::stable_sort(items.begin(), items.end(),
                     [](auto* a, auto* b) { return a->position < b->position; });
    double previousEnd = -1e300;
    auto add = [&](double at, double value) { points_.push_back({at, value}); };
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = *items[i];
        const double start = std::max(item.position, previousEnd);
        const double end = i + 1 < items.size()
                               ? std::min(item.position + item.length, items[i + 1]->position)
                               : item.position + item.length;
        if (!(end > start) || !(item.sourceLength > 0)) continue;
        for (const auto& point : env->points)
            if (point.pos > previousEnd && point.pos < start) points_.push_back(point);
        if (!env->points.empty()) add(start, model::pointsValueAt(env->points, start, 0));
        Segment segment{start, end, item.sourceLength, item.baseline, item.loop, item.points};
        for (auto& point : segment.points)
            point.value = item.baseline + item.amplitude * (point.value - item.baseline);
        auto local = [&](double at) {
            return model::pointsValueAt(segment.points, std::min(at, item.sourceLength),
                                        item.baseline);
        };
        add(start, local(0));
        double last = end - start;
        if (item.loop) {
            const double pass = std::max(0.0, std::ceil(last / item.sourceLength - 1e-9) - 1);
            if (std::isfinite(pass))
                last -= pass * item.sourceLength;
            else
                last = item.sourceLength;
        }
        add(end, local(last));
        if (!env->points.empty()) add(end, model::pointsValueAt(env->points, end, 0));
        segments_.push_back(std::move(segment));
        previousEnd = end;
    }
    for (const auto& point : env->points)
        if (point.pos > previousEnd) points_.push_back(point);
    if (normalized_)
        for (auto& point : points_) point.value = scale_.toNorm(point.value);
}

double EnvelopeCurve::valueAt(double beat, double fallback) const {
    auto domain = [&](double value) { return normalized_ ? scale_.toNorm(value) : value; };
    double value = model::pointsValueAt(points_, beat, domain(fallback));
    const auto next =
        std::upper_bound(segments_.begin(), segments_.end(), beat,
                         [](double at, const Segment& segment) { return at < segment.start; });
    if (next != segments_.begin()) {
        const auto& item = *std::prev(next);
        if (beat >= item.start && beat < item.end) {
            const double local = beat - item.start;
            const double at = item.loop ? std::fmod(local, item.sourceLength) : local;
            const double endLocal = item.loop ? std::min(item.sourceLength, item.end - beat + at)
                                              : item.end - item.start;
            auto sourceValue = [&](double x) {
                return model::pointsValueAt(item.points, std::min(x, item.sourceLength),
                                            item.baseline);
            };
            model::EnvPoint a{0, domain(sourceValue(0))},
                b{endLocal, domain(sourceValue(endLocal))};
            const auto upper = std::upper_bound(
                item.points.begin(), item.points.end(), at,
                [](double x, const model::EnvPoint& point) { return x < point.pos; });
            auto lower = upper;
            while (lower != item.points.begin() && std::prev(lower)->pos >= item.sourceLength)
                --lower;
            if (lower != item.points.begin()) {
                const auto& point = *std::prev(lower);
                if (point.pos < item.sourceLength) {
                    a = point;
                    a.value = domain(a.value);
                }
            }
            if (upper != item.points.end() && upper->pos < item.sourceLength &&
                upper->pos < b.pos) {
                b = *upper;
                b.value = domain(b.value);
            }
            value = b.pos > a.pos ? a.value + (b.value - a.value) *
                                                  model::shapeCurve(a.shape, a.tension,
                                                                    (at - a.pos) / (b.pos - a.pos))
                                  : b.value;
        }
    }
    return normalized_ ? scale_.fromNorm(value) : value;
}

int envOrder(const model::Envelope& env) {
    const std::string& p = env.target.param;
    const int order = p == "volume" ? 0 : p == "pan" ? 1 : p == "mute" ? 2 : 3;
    return env.target.object.kind == model::Kind::Route ? 4 + order : order;
}

std::string envTitle(const model::Envelope& env, const model::Session& session) {
    // A rack module's knob: "Bells » Osc · fine".
    if (env.target.object.kind == model::Kind::Module)
        if (const model::RackModule* m = session.modules.find(env.target.object)) {
            std::string name = model::moduleLabel(session, env.target.object);
            if (name.empty()) {
                const auto dot = m->type.rfind('.');
                name = m->type.substr(dot == std::string::npos ? 0 : dot + 1);
                if (!name.empty())
                    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            }
            return model::rackLabel(session, m->rack) + " \u00bb " + name + " \u00b7 " +
                   envTitle(env);
        }
    const model::Route* r = env.target.object.kind == model::Kind::Route
                                ? session.routes.find(env.target.object)
                                : nullptr;
    if (!r) return envTitle(env);
    std::string to;
    if (r->to.fx) {
        if (const model::FxInstance* fx = session.fx.find(*r->to.fx))
            to = "key " + (fx->label.empty() ? fx->plugin.name : fx->label);
    } else if (r->to.track) {
        if (const model::Track* t = session.tracks.find(*r->to.track)) to = t->name;
    } else if (r->to.hwChannels) {
        to = "out";
        for (std::size_t i = 0; i < r->to.hwChannels->size(); ++i)
            to += (i ? "/" : " ") + std::to_string((*r->to.hwChannels)[i]);
    }
    return "Send \u2192 " + to + " \u00b7 " + envTitle(env);
}

}  // namespace supple::views
