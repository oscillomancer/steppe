// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "strip_learn.h"

#include <memory>
#include <utility>

#include "learn_dialog.h"
#include "supple/engine/midi_control.h"
#include "supple/model/schemas.h"
#include "supple/ui/popup.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

bool isRoute(const model::Id& id) { return id.kind == model::Kind::Route; }
bool isVca(const model::Id& id) { return id.kind == model::Kind::Group; }

// The commands' prefix (track.learn.set) and the argument naming the object.
std::string prefixOf(const model::Id& id) {
    return isRoute(id) ? "route" : isVca(id) ? "vca" : "track";
}
std::string keyOf(const model::Id& id) {
    return isRoute(id) ? "route" : isVca(id) ? "group" : "track";
}

const std::vector<model::ControlLearn>* learnOf(const model::Session& s, const model::Id& id) {
    if (isRoute(id)) {
        const model::Route* r = s.routes.find(id);
        return r ? &r->learn : nullptr;
    }
    if (isVca(id)) {
        const model::TrackGroup* g = s.groups.find(id);
        return g && g->vca ? &g->vca->learn : nullptr;
    }
    const model::Track* t = s.tracks.find(id);
    return t ? &t->learn : nullptr;
}

const model::ControlLearn* bindingOf(const model::Session& s, const model::Id& id,
                                     const std::string& param) {
    if (const auto* list = learnOf(s, id))
        for (const model::ControlLearn& l : *list)
            if (l.param == param) return &l;
    return nullptr;
}

std::string trackName(const model::Session& s, const std::optional<model::Id>& id) {
    const model::Track* t = id ? s.tracks.find(*id) : nullptr;
    if (!t) return "?";
    return t->name.empty() ? std::string(model::stripTypeName(t->type)) : t->name;
}

// "Bass", "Bass → Verb", "Drums VCA", or "3 sends".
std::string ownerName(const model::Session& s, const std::vector<model::Id>& objects) {
    if (objects.size() > 1)
        return std::to_string(objects.size()) + (isRoute(objects.front()) ? " sends" : " tracks");
    const model::Id& id = objects.front();
    if (isVca(id)) {
        const model::TrackGroup* g = s.groups.find(id);
        return (g && !g->name.empty() ? g->name + " " : std::string()) + "VCA";
    }
    if (!isRoute(id)) return trackName(s, id);
    const model::Route* r = s.routes.find(id);
    if (!r) return id.str();
    std::string to = r->to.track ? trackName(s, r->to.track) : r->to.fx ? "key" : "out";
    return trackName(s, r->from.track) + " → " + to;
}

// A level or pan follows a knob; the rest are buttons, toggled by a press.
bool continuous(std::string_view param) { return param == "volume" || param == "pan"; }

}  // namespace

std::string stripControlLabel(std::string_view param) {
    if (param == "volume") return "Volume";
    if (param == "pan") return "Pan";
    if (param == "mute") return "Mute";
    if (param == "solo") return "Solo";
    if (param == "phase_invert") return "Polarity";
    if (param == "mono") return "Mono";
    if (param == "armed") return "Record arm";
    if (param == "monitor") return "Monitoring";
    if (param == "lane_mute") return "Lane mute";
    if (param == "tap:post_fader") return "Post-fader tap";
    if (param == "tap:pre_fader") return "Pre-fader tap";
    if (param == "tap:pre_fx") return "Pre-FX tap";
    return std::string(param);
}

void openStripLearn(ui::Root& root, AppActions& actions, StripControl control) {
    if (control.objects.empty()) return;
    const model::Session s = actions.app().head();
    LearnRequest request;
    request.title = ownerName(s, control.objects) + " · " + stripControlLabel(control.param);
    request.takeover = continuous(control.param);
    request.mode =
        continuous(control.param) ? model::ControlMode::Absolute : model::ControlMode::Toggle;
    for (const model::Id& id : control.objects)
        if (const model::ControlLearn* bound = bindingOf(s, id, control.param)) {
            LearnChoice choice;
            choice.control = bound->control;
            choice.mode = bound->mode;
            choice.softTakeover = bound->softTakeover;
            request.current = choice;
            break;
        }
    AppActions* a = &actions;
    request.onOk = [a, control](const LearnChoice& choice) {
        const auto r =
            a->dispatcher().transact("MIDI learn", kOrigin, [&]() -> Result<json::Value> {
                for (const model::Id& id : control.objects) {
                    json::Value args = json::Value::object();
                    args[keyOf(id)] = id.str();
                    args["param"] = control.param;
                    args["midi"] = model::midiControlSchema().encode(choice.control);
                    args["mode"] = std::string(engine::modeName(choice.mode));
                    if (continuous(control.param)) args["soft_takeover"] = choice.softTakeover;
                    SUPPLE_TRY(a->dispatcher().execute(
                        {prefixOf(id) + ".learn.set", std::move(args), kOrigin}));
                }
                return json::Value::object();
            });
        if (!r && a->onError) a->onError(r.error().message);
    };
    request.onRemove = [a, control] {
        const model::Session now = a->app().head();
        const auto r =
            a->dispatcher().transact("Remove MIDI learn", kOrigin, [&]() -> Result<json::Value> {
                for (const model::Id& id : control.objects) {
                    if (!bindingOf(now, id, control.param)) continue;
                    json::Value args = json::Value::object();
                    args[keyOf(id)] = id.str();
                    args["param"] = control.param;
                    SUPPLE_TRY(a->dispatcher().execute(
                        {prefixOf(id) + ".learn.remove", std::move(args), kOrigin}));
                }
                return json::Value::object();
            });
        if (!r && a->onError) a->onError(r.error().message);
    };
    openLearnDialog(root, actions, actions.app(), std::move(request));
}

void openStripLearnMenu(ui::Root& root, AppActions& actions, StripControl control,
                        const ui::Rect& anchor) {
    if (control.objects.empty()) return;
    const model::Session s = actions.app().head();
    std::string label = "MIDI learn: " + stripControlLabel(control.param) + "…";
    for (const model::Id& id : control.objects)
        if (const model::ControlLearn* bound = bindingOf(s, id, control.param)) {
            label += " (" + engine::describe(bound->control) + ")";
            break;
        }
    std::vector<ui::MenuItem> items;
    ui::MenuItem learn;
    learn.label = std::move(label);
    ui::Root* r = &root;
    AppActions* a = &actions;
    learn.run = [r, a, control] { openStripLearn(*r, *a, control); };
    items.push_back(std::move(learn));
    root.openPopup(ui::makeMenu("strip.learn.menu", std::move(items), [r] { r->closePopup(); }),
                   anchor, ui::Placement::Below);
}

}  // namespace supple::gui
