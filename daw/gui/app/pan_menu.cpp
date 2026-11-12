// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "pan_menu.h"

#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "selection_edit.h"
#include "strip_learn.h"
#include "supple/ui/popup.h"

namespace supple::gui {
namespace {

std::string lawText(double db) {
    if (db == 0) return "0 dB";
    char buf[32];
    std::snprintf(buf, sizeof buf, "−%g dB", -db);
    return buf;
}

void setTrack(AppActions& actions, const model::Id& track, const char* field, json::Value value) {
    json::Value args = json::Value::object();
    args["track"] = track.str();
    args[field] = std::move(value);
    const auto r =
        SelectionEdit(actions.dispatcher(), actions.app(), actions.ui()).set(std::move(args), {});
    if (!r && actions.onError) actions.onError(r.error().message);
}

json::Value lawJson(const model::PanLaw& law) {
    json::Value v = json::Value::object();
    v["db"] = law.db;
    v["boost"] = law.boost;
    return v;
}

}  // namespace

ui::PanControl::Split panSplit(const model::Track& track) {
    if (track.type == model::StripType::Master) return ui::PanControl::Split::None;
    switch (track.panMode) {
        case model::PanMode::Dual:
            return ui::PanControl::Split::Dual;
        case model::PanMode::Width:
            return ui::PanControl::Split::Width;
        case model::PanMode::Pan:
        default:
            return ui::PanControl::Split::None;
    }
}

void showPan(ui::PanControl& control, const model::Track& track) {
    const auto split = panSplit(track);
    control.setSplit(split);
    if (split == ui::PanControl::Split::Dual) {
        control.setValue(track.panLeft);
        control.setSecond(track.panRight);
    } else {
        control.setValue(track.pan);
        control.setSecond(track.width);
    }
}

const char* panField(const model::Track& track, int part) {
    const auto split = panSplit(track);
    if (split == ui::PanControl::Split::Dual) return part == 0 ? "pan_left" : "pan_right";
    if (split == ui::PanControl::Split::Width && part == 1) return "width";
    return "pan";
}

void openPanMenu(ui::Root& root, AppActions& actions, const model::Id& track,
                 const ui::Rect& anchor) {
    const model::Session s = actions.app().head();
    const model::Track* t = s.tracks.find(track);
    if (!t) return;
    if (t->type == model::StripType::Master) {
        openStripLearnMenu(root, actions, {{track}, "pan"}, anchor);
        return;
    }
    AppActions* a = &actions;
    std::vector<ui::MenuItem> items;
    auto add = [&](std::string label, bool checked, std::function<void()> run) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.checked = checked;
        m.run = std::move(run);
        items.push_back(std::move(m));
    };
    auto rule = [&] { items.push_back({"", "", {}, true, true}); };
    for (auto [mode, name, label] :
         {std::tuple{model::PanMode::Pan, "pan", "Pan mode: pan"},
          std::tuple{model::PanMode::Dual, "dual", "Pan mode: dual pan"},
          std::tuple{model::PanMode::Width, "width", "Pan mode: pan and width"}})
        add(label, t->panMode == mode, [a, track, key = std::string(name)] {
            setTrack(*a, track, "pan_mode", json::Value(key));
        });
    rule();
    const model::PanLaw project = s.project->panLaw;
    add("Pan law: project (" + lawText(project.db) + (project.boost ? ", boost" : "") + ")",
        !t->panLaw, [a, track] { setTrack(*a, track, "pan_law", json::Value()); });
    const model::PanLaw effective = model::panLawOf(*s.project, *t);
    for (const double db : model::kPanLawsDb)
        add("Pan law: " + lawText(db), t->panLaw && t->panLaw->db == db,
            [a, track, db, boost = effective.boost] {
                setTrack(*a, track, "pan_law", lawJson({db, boost}));
            });
    // On a strip that follows the project, the toggle gives it its own law.
    add("Gain compensation (boost pans)", effective.boost, [a, track, effective] {
        setTrack(*a, track, "pan_law", lawJson({effective.db, !effective.boost}));
    });
    rule();
    ui::Root* r = &root;
    std::vector<std::string> controls;
    if (t->panMode == model::PanMode::Dual)
        controls = {"pan_left", "pan_right"};
    else if (t->panMode == model::PanMode::Width)
        controls = {"pan", "width"};
    else
        controls = {"pan"};
    controls.push_back("pan_boost");
    for (const auto& param : controls)
        add(stripLearnLabel(s, {{track}, param}), false,
            [r, a, track, param] { openStripLearn(*r, *a, {{track}, param}); });
    root.openPopup(ui::makeMenu("strip.pan.menu", std::move(items), [r] { r->closePopup(); }),
                   anchor, ui::Placement::Below);
}

}  // namespace supple::gui
