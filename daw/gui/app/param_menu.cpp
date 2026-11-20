// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "param_menu.h"

#include <cstdio>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "pan_menu.h"
#include "selection_edit.h"
#include "strip_learn.h"
#include "supple/ui/popup.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

void run(AppActions& actions, const std::string& command, json::Value args) {
    const auto r = actions.dispatcher().execute({command, std::move(args), kOrigin});
    if (!r && actions.onError) actions.onError(r.error().message);
}

// A strip control's value, as a double-click or the fader would set it: the
// whole selection when the track is selected, groups linked.
void setTrack(AppActions& actions, const model::Id& track, json::Value fields) {
    json::Value args = std::move(fields);
    args["track"] = track.str();
    const auto r =
        SelectionEdit(actions.dispatcher(), actions.app(), actions.ui()).set(std::move(args), {});
    if (!r && actions.onError) actions.onError(r.error().message);
}

std::string lawText(double db) {
    if (db == 0) return "0 dB";
    char buf[32];
    std::snprintf(buf, sizeof buf, "−%g dB", -db);
    return buf;
}

json::Value lawJson(const model::PanLaw& law) {
    json::Value v = json::Value::object();
    v["db"] = law.db;
    v["boost"] = law.boost;
    return v;
}

struct Builder {
    ui::Root& root;
    AppActions& actions;
    const model::Session& s;
    model::Id object;
    std::vector<ui::MenuItem> items;

    void add(std::string label, std::function<void()> action, bool checked = false,
             std::string shortcut = {}) {
        ui::MenuItem m;
        m.label = std::move(label);
        m.shortcut = std::move(shortcut);
        m.run = std::move(action);
        m.checked = checked;
        items.push_back(std::move(m));
    }
    void rule() { items.push_back({"", "", {}, true, true}); }

    void learn(const std::string& param) {
        ui::Root* r = &root;
        AppActions* a = &actions;
        add(stripLearnLabel(s, {{object}, param}),
            [r, a, o = object, param] { openStripLearn(*r, *a, {{o}, param}); });
    }
    // Show or hide the parameter's lane; a parameter without an envelope
    // gets one, flat at the control's value.
    void lane(const std::string& param) {
        std::optional<model::Id> env;
        bool shown = false;
        for (const auto& e : s.envelopes)
            if (e.value->target.object == object && e.value->target.param == param) {
                env = e.id;
                shown = e.value->visible;
            }
        AppActions* a = &actions;
        const std::string name = stripControlLabel(param);
        if (env) {
            add((shown ? "Hide lane: " : "Show lane: ") + name, [a, id = *env, shown] {
                json::Value args = json::Value::object();
                args["env"] = id.str();
                args["visible"] = !shown;
                run(*a, "env.set", std::move(args));
            });
            return;
        }
        add("Show lane: " + name,
            [a, o = object, param, value = envelopeStartValue(s, object, param)] {
                json::Value args = json::Value::object(), points = json::Value::array(),
                            first = json::Value::object();
                args[o.kind == model::Kind::Group ? "group" : "track"] = o.str();
                args["param"] = param;
                first["beat"] = 0.0;
                first["value"] = value;
                points.push_back(std::move(first));
                args["points"] = std::move(points);
                run(*a, "env.add", std::move(args));
            });
    }
};

// The pan's bars in the strip's mode, as their `learn` and lanes name them.
std::vector<std::string> panControls(const model::Track& t) {
    if (t.type == model::StripType::Master) return {"pan"};
    if (t.panMode == model::PanMode::Dual) return {"pan_left", "pan_right"};
    if (t.panMode == model::PanMode::Width) return {"pan", "width"};
    return {"pan"};
}

void panItems(Builder& b, const model::Track& t) {
    const model::Id track = b.object;
    AppActions* a = &b.actions;
    const bool master = t.type == model::StripType::Master;
    for (const std::string& param : panControls(t)) b.learn(param);
    if (!master) b.learn("pan_boost");
    for (const std::string& param : panControls(t)) b.lane(param);
    // Reset: every bar home, as a double-click on each would set it.
    json::Value home = json::Value::object();
    std::string label = "Reset to centre";
    if (!master && t.panMode == model::PanMode::Dual) {
        home["pan_left"] = -100.0;
        home["pan_right"] = 100.0;
        label = "Reset to full left and right";
    } else {
        home["pan"] = 0.0;
        if (!master && t.panMode == model::PanMode::Width) {
            home["width"] = 100.0;
            label = "Reset to centre, full width";
        }
    }
    b.add(label, [a, track, home] { setTrack(*a, track, home); }, false, "Double-click");
    if (master) return;  // a balance: no modes, no law
    b.rule();
    for (auto [mode, name, text] :
         {std::tuple{model::PanMode::Pan, "pan", "Pan mode: pan"},
          std::tuple{model::PanMode::Dual, "dual", "Pan mode: dual pan"},
          std::tuple{model::PanMode::Width, "width", "Pan mode: pan and width"}})
        b.add(
            text,
            [a, track, key = std::string(name)] {
                json::Value f = json::Value::object();
                f["pan_mode"] = key;
                setTrack(*a, track, std::move(f));
            },
            t.panMode == mode);
    b.rule();
    const model::PanLaw project = b.s.project->panLaw;
    b.add(
        "Pan law: project (" + lawText(project.db) + (project.boost ? ", boost" : "") + ")",
        [a, track] {
            json::Value f = json::Value::object();
            f["pan_law"] = json::Value();
            setTrack(*a, track, std::move(f));
        },
        !t.panLaw);
    const model::PanLaw effective = model::panLawOf(*b.s.project, t);
    for (const double db : model::kPanLawsDb)
        b.add(
            "Pan law: " + lawText(db),
            [a, track, db, boost = effective.boost] {
                json::Value f = json::Value::object();
                f["pan_law"] = lawJson({db, boost});
                setTrack(*a, track, std::move(f));
            },
            t.panLaw && t.panLaw->db == db);
    // On a strip that follows the project, the toggle gives it its own law.
    b.add(
        "Gain compensation (boost pans)",
        [a, track, effective] {
            json::Value f = json::Value::object();
            f["pan_law"] = lawJson({effective.db, !effective.boost});
            setTrack(*a, track, std::move(f));
        },
        effective.boost);
}

}  // namespace

double envelopeStartValue(const model::Session& s, const model::Id& object,
                          std::string_view param) {
    if (const model::TrackGroup* g = s.groups.find(object); g && g->vca)
        return param == "volume" ? g->vca->volumeDb : (g->vca->mute ? 1.0 : 0.0);
    if (param == "playrate") return s.project->playrate.rate;
    const model::Track* tr = s.tracks.find(object);
    if (!tr) return 0.0;
    return param == "volume"      ? tr->volumeDb
           : param == "pan"       ? tr->pan
           : param == "width"     ? tr->width
           : param == "pan_left"  ? tr->panLeft
           : param == "pan_right" ? tr->panRight
           : param == "pan_boost" ? (model::panLawOf(*s.project, *tr).boost ? 1.0 : 0.0)
                                  : (tr->mute ? 1.0 : 0.0);
}

void openParamMenu(ui::Root& root, AppActions& actions, const model::Id& object,
                   const std::string& param, const ui::Rect& anchor) {
    const model::Session s = actions.app().head();
    const model::Track* track = s.tracks.find(object);
    const model::TrackGroup* group = s.groups.find(object);
    if (!track && !(group && group->vca)) return;
    Builder b{root, actions, s, object, {}};
    AppActions* a = &actions;
    if (track && param == "pan") {
        panItems(b, *track);
    } else {
        b.learn(param);
        if (param == "volume" || param == "mute") b.lane(param);
        if (param == "volume") {
            const model::Id o = object;
            b.add(
                "Reset to 0 dB",
                [a, o, isGroup = group != nullptr] {
                    if (!isGroup) {
                        json::Value f = json::Value::object();
                        f["volume_db"] = 0.0;
                        setTrack(*a, o, std::move(f));
                        return;
                    }
                    json::Value args = json::Value::object();
                    args["group"] = o.str();
                    args["volume_db"] = 0.0;
                    run(*a, "vca.set", std::move(args));
                },
                false, "Double-click");
        }
    }
    ui::Root* r = &root;
    root.openPopup(ui::makeMenu("strip.param.menu", std::move(b.items), [r] { r->closePopup(); }),
                   anchor, ui::Placement::Below);
}

}  // namespace supple::gui
