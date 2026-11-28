// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "param_menu.h"

#include <cstdio>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "mod_source.h"
#include "modulation_status.h"
#include "pan_menu.h"
#include "rack_engine.h"
#include "selection_edit.h"
#include "strip_learn.h"
#include "supple/ui/popup.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/object_name.h"
#include "supple/views/track_layout.h"

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
    ui::Rect anchor;
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
    // Modulate: <name>… (ADR-0096, M-k).
    void modulate(const std::string& param) {
        json::Value target = json::Value::object();
        target["track"] = object.str();
        target["param"] = param;
        AppActions* a = &actions;
        items.push_back(modulateItem(
            root, actions.dispatcher(), [a] { return a->app().head(); }, stripControlLabel(param),
            std::move(target),
            object.kind == model::Kind::Track ? std::optional(object) : std::nullopt,
            views::stripModulatable(s, object, param), anchor,
            [a](const std::string& message) {
                if (a->onError) a->onError(message);
            }));
        if (auto idle = inertItem(actions.dispatcher(), s, {object, param}))
            items.push_back(std::move(*idle));
        const auto catalog = racks::catalog(actions.app());
        for (auto& item : modRouteItems(actions, s, catalog.get(), {object, param}))
            items.push_back(std::move(item));
    }
    // Show or hide the parameter's lane, as the lanes decide it: through
    // its envelope and its modulation routes (laneShown, setLaneShown).
    void lane(const std::string& param) {
        const model::EnvTarget target{object, param};
        const bool shown = laneShown(s, target);
        AppActions* a = &actions;
        json::Value envArgs = json::Value::object();
        envArgs[object.kind == model::Kind::Group ? "group" : "track"] = object.str();
        add((shown ? "Hide lane: " : "Show lane: ") + stripControlLabel(param),
            [a, target, shown, envArgs, value = envelopeStartValue(s, object, param)] {
                const model::Session now = a->app().head();
                const auto r = setLaneShown(a->dispatcher(), now, target, !shown, envArgs, value);
                if (!r && a->onError) a->onError(r.error().message);
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
    for (const std::string& param : panControls(t)) b.modulate(param);
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

void openModulateMenu(ui::Root& root, cmd::Dispatcher& dispatcher, const model::Session& s,
                      json::Value target, std::optional<model::Id> ownTrack, const ui::Rect& anchor,
                      std::function<void(const std::string&)> onError) {
    cmd::Dispatcher* d = &dispatcher;
    auto make = [d, target, onError](json::Value source, std::optional<model::Id> from) {
        return [d, target, onError, source = std::move(source), from] {
            json::Value args = json::Value::object();
            args["target"] = target;
            args["source"] = source;
            if (from) args["from"] = from->str();
            const auto r = d->execute({"modulate", std::move(args), kOrigin});
            if (!r && onError) onError(r.error().message);
        };
    };
    std::vector<ui::MenuItem> items;
    for (auto [label, key] :
         {std::pair{"LFO", "lfo"}, std::pair{"Random", "random"}, std::pair{"Macro", "macro"}})
        items.push_back({label, "", make(json::Value(key), std::nullopt)});
    bool first = true;
    for (model::Id id : views::stripOrder(s)) {
        const model::Track* t = s.tracks.find(id);
        if (!t || t->type == model::StripType::Master || id == ownTrack) continue;
        if (first) items.push_back({"", "", {}, true, true});
        first = false;
        const std::string name = views::objectName(s, id);
        items.push_back({"Follower: " + name, "", make(json::Value("follower"), id)});
    }
    ui::Root* r = &root;
    root.openPopup(ui::makeMenu("param.modulate.menu", std::move(items), [r] { r->closePopup(); }),
                   anchor, ui::Placement::Below);
}

ui::MenuItem modulateItem(ui::Root& root, cmd::Dispatcher& dispatcher,
                          std::function<model::Session()> head, const std::string& name,
                          json::Value target, std::optional<model::Id> ownTrack, bool modulatable,
                          const ui::Rect& anchor, std::function<void(const std::string&)> onError) {
    ui::MenuItem m;
    m.label = "Modulate: " + name + "\u2026";  // a second menu follows, as Groups… does
    m.enabled = modulatable;
    // The menu closes before its row runs; the sources open in its place,
    // from the session as it is then.
    ui::Root* r = &root;
    cmd::Dispatcher* d = &dispatcher;
    m.run = [r, d, head = std::move(head), target = std::move(target), ownTrack, anchor,
             onError = std::move(onError)] {
        openModulateMenu(*r, *d, head(), target, ownTrack, anchor, onError);
    };
    return m;
}

bool laneShown(const model::Session& s, const model::EnvTarget& target) {
    for (const auto& e : s.envelopes)
        if (e.value->target == target && e.value->visible) return true;
    return views::modulationLane(s, target);
}

Status setLaneShown(cmd::Dispatcher& dispatcher, const model::Session& s,
                    const model::EnvTarget& target, bool shown, json::Value envArgs,
                    double startValue) {
    std::vector<std::pair<std::string, json::Value>> edits;
    for (const auto& e : s.envelopes) {
        if (!(e.value->target == target) || e.value->visible == shown) continue;
        json::Value args = json::Value::object();
        args["env"] = e.id.str();
        args["visible"] = shown;
        edits.emplace_back("env.set", std::move(args));
    }
    bool held = false;  // an envelope or a route can show it
    for (const auto& e : s.envelopes) held |= e.value->target == target;
    for (const auto& m : s.modulations) {
        if (!(m.value->target == target)) continue;
        held = true;
        if (m.value->lane == shown) continue;
        json::Value args = json::Value::object();
        args["modulation"] = m.id.str();
        args["lane"] = shown;
        edits.emplace_back("modulation.set", std::move(args));
    }
    if (shown && !held) {
        json::Value args = std::move(envArgs), points = json::Value::array(),
                    first = json::Value::object();
        args["param"] = target.param;
        first["beat"] = 0.0;
        first["value"] = startValue;
        points.push_back(std::move(first));
        args["points"] = std::move(points);
        edits.emplace_back("env.add", std::move(args));
    }
    if (edits.empty()) return {};
    auto r = dispatcher.transact(
        shown ? "Show lane" : "Hide lane", kOrigin, [&]() -> Result<json::Value> {
            for (auto& [command, args] : edits)
                SUPPLE_TRY(dispatcher.execute({command, std::move(args), kOrigin}));
            return json::Value::object();
        });
    if (!r) return std::move(r).error();
    return {};
}

void openParamMenu(ui::Root& root, AppActions& actions, const model::Id& object,
                   const std::string& param, const ui::Rect& anchor) {
    const model::Session s = actions.app().head();
    const model::Track* track = s.tracks.find(object);
    const model::TrackGroup* group = s.groups.find(object);
    if (!track && !(group && group->vca)) return;
    Builder b{root, actions, s, object, anchor, {}};
    AppActions* a = &actions;
    if (track && param == "pan") {
        panItems(b, *track);
    } else {
        b.learn(param);
        if (param == "volume" || param == "mute") {
            b.modulate(param);
            b.lane(param);
        }
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
