// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/comp_view.h"

#include <algorithm>
#include <cstdint>
#include <set>

namespace supple::views {
namespace {

std::size_t heardIndex(const model::Item& item) {
    if (!item.activeTake) return 0;
    const auto at = std::find(item.takes.begin(), item.takes.end(), *item.activeTake);
    return at == item.takes.end() ? 0 : static_cast<std::size_t>(at - item.takes.begin());
}

double beatOfSample(const model::Session& s, std::int64_t sample) {
    return s.timeMap->beatAt(static_cast<double>(sample) / s.project->sampleRate);
}

}  // namespace

std::optional<CompView> compOf(const model::Session& s, model::Id item) {
    const model::Item* it = s.items.find(item);
    if (!it || it->takes.size() < 2 || it->loop) return std::nullopt;
    auto run = model::compRun(s, item, {});
    if (!run) return std::nullopt;
    CompView comp;
    comp.pieces = std::move(*run);
    for (const auto& p : comp.pieces) comp.index.push_back(heardIndex(*s.items.find(p.item)));
    for (std::size_t i = 0; i + 1 < comp.pieces.size(); ++i)
        if (comp.index[i] != comp.index[i + 1])
            comp.seams.push_back(
                {i, static_cast<double>(comp.pieces[i].coreEnd) / s.project->sampleRate});
    return comp;
}

std::vector<CompSeam> compSeams(const model::Session& s, const std::vector<model::Id>& items) {
    std::vector<CompSeam> out;
    std::set<model::Id> seen;
    for (model::Id id : items) {
        if (seen.contains(id)) continue;
        const auto comp = compOf(s, id);
        if (!comp) continue;
        for (const auto& p : comp->pieces) seen.insert(p.item);
        for (const auto& seam : comp->seams)
            out.push_back(
                {comp->pieces[seam.left].item, comp->pieces[seam.left + 1].item, seam.seconds});
    }
    return out;
}

std::vector<CompSpan> compSpans(const model::Session& s, const CompView& comp) {
    std::vector<CompSpan> out;
    for (std::size_t i = 0; i < comp.pieces.size(); ++i) {
        const double from = beatOfSample(s, comp.pieces[i].coreStart);
        const double to = beatOfSample(s, comp.pieces[i].coreEnd);
        if (!out.empty() && out.back().index == comp.index[i])
            out.back().to = to;
        else
            out.push_back({from, to, comp.index[i]});
    }
    return out;
}

std::optional<CompCrossfade> compCrossfade(const model::Session& s, const CompView& comp) {
    if (comp.seams.empty()) return std::nullopt;
    const model::Fade* longest = nullptr;
    for (const auto& seam : comp.seams) {
        const model::Item& l = *s.items.find(comp.pieces[seam.left].item);
        const model::Item& r = *s.items.find(comp.pieces[seam.left + 1].item);
        for (const model::Fade* fade : {&l.fadeOut, &r.fadeIn})
            if (!longest || fade->length > longest->length) longest = fade;
    }
    return CompCrossfade{longest->length,
                         longest->length > 0 && longest->shape == model::FadeShape::Linear};
}

json::Value compArgs(model::Id item, const std::vector<CompSpan>& spans,
                     const std::optional<CompCrossfade>& crossfade) {
    json::Value args = json::Value::object();
    args["item"] = item.str();
    json::Value list = json::Value::array();
    for (const CompSpan& span : spans) {
        json::Value o = json::Value::object();
        o["from"] = span.from;
        o["to"] = span.to;
        o["index"] = static_cast<std::int64_t>(span.index);
        list.push_back(std::move(o));
    }
    args["spans"] = std::move(list);
    if (crossfade) {
        args["crossfade"] = crossfade->seconds;
        args["crossfade_shape"] = crossfade->linear ? "linear" : "cos";
    }
    return args;
}

}  // namespace supple::views
