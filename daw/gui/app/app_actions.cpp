// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "app_actions.h"

#include "playrate_range.h"
#include "supple/ui/shortcut.h"
#include "track_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "supple/engine/midi_control.h"
#include "supple/json/parse.h"
#include "supple/schema/schema.h"
#include "supple/ui/controls.h"
#include "supple/ui/node.h"
#include "supple/ui/root.h"
#include "supple/views/track_layout.h"

namespace supple::gui {

void selectTrack(UiState& ui, const std::string& track, unsigned mods,
                 const std::vector<std::string>& order) {
    auto& sel = ui.selectedTracks;
    ui.tracksFocused = true;
    ui.envelopeFocused = false;
    if ((mods & views::kSelectKeep) && std::find(sel.begin(), sel.end(), track) != sel.end()) {
        ui.trackAnchor = track;      // a press that may drag the selection: it stays
    } else if (mods & ui::kShift) {  // extend from the anchor, in the view's order
        const std::string from = ui.trackAnchor.value_or(track);
        auto a = std::find(order.begin(), order.end(), from);
        auto b = std::find(order.begin(), order.end(), track);
        if (a != order.end() && b != order.end()) {
            if (a > b) std::swap(a, b);
            sel.assign(a, b + 1);
        } else {
            sel = {track};
        }
        ui.trackAnchor = from;
    } else if (mods & ui::kCtrl) {  // toggle one
        const auto it = std::find(sel.begin(), sel.end(), track);
        if (it == sel.end())
            sel.push_back(track);
        else
            sel.erase(it);
        ui.trackAnchor = track;
    } else {
        sel = {track};
        ui.trackAnchor = track;
    }
}

std::vector<std::string> keySections(const ui::Root& root) {
    const bool typing = root.focused() && root.focused()->role() == ui::Role::TextField;
    std::vector<std::string> sections;
    if (typing) sections.push_back("text");
    const ui::Node* context = root.focused();
    if (const auto at = root.lastPointer(); at && !typing)
        for (const char* panel : {"midi.editor", "mixer", "fx", "arrange"})
            if (const ui::Node* n = root.find(panel);
                n && n->visible && n->bounds().contains(*at)) {
                context = n;
                break;
            }
    for (const ui::Node* n = context; n; n = n->parent())
        if (n->id() == "midi.editor" || n->id() == "mixer" || n->id() == "fx") {
            sections.push_back(n->id() == "midi.editor" ? "midi" : n->id());
            break;
        }
    sections.push_back("arrange");
    sections.push_back("global");
    return sections;
}

bool trackSelected(const UiState& ui, const std::string& track) {
    return std::find(ui.selectedTracks.begin(), ui.selectedTracks.end(), track) !=
           ui.selectedTracks.end();
}
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

actions::Action bound(std::string id, std::string name, std::string command, json::Value args) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.command = std::move(command);
    a.args = std::move(args);
    return a;
}

json::Value obj(std::initializer_list<std::pair<const char*, json::Value>> fields) {
    json::Value o = json::Value::object();
    for (const auto& [k, v] : fields) o[k] = v;
    return o;
}

struct Selection {
    std::vector<std::string> tracks, items;
};

}  // namespace

void registerGuiCommands(cmd::Registry& registry, const UiState& ui) {
    cmd::Command<cmd::NoArgs, Selection> c;
    c.name = "gui.selection";
    c.kind = cmd::Kind::Query;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "The GUI's selected tracks and items (context only).";
    c.description =
        "Useful context for requests like \"the selected items\"; never rely on it for "
        "correctness.";
    c.tags = {"gui"};
    c.args = schema::object<cmd::NoArgs>("no_args").build();
    c.result = schema::object<Selection>("selection")
                   .field("tracks", &Selection::tracks, schema::arrayOf(schema::string()))
                   .field("items", &Selection::items, schema::arrayOf(schema::string()))
                   .build();
    c.examples = {{"Read the selection", json::Value::object(),
                   *json::parse(R"({"tracks": [], "items": []})"), "empty"}};
    c.run = [&ui](const cmd::NoArgs&, cmd::Context&) -> Result<Selection> {
        return Selection{ui.selectedTracks, ui.selectedItems};
    };
    registry.add(std::move(c));
}

AppActions::AppActions(const cmd::Registry& registry, cmd::Dispatcher& dispatcher,
                       engine::EngineApp& app, UiState& ui, std::filesystem::path keymapFile,
                       std::filesystem::path controlsFile)
    : keymapFile_(std::move(keymapFile)),
      controlsFile_(std::move(controlsFile)),
      dispatcher_(dispatcher),
      app_(app),
      ui_(ui),
      runner_(registry_, dispatcher) {
    registry_.addCommands(registry);
    auto exec = [this](const char* command, json::Value args) {
        return dispatcher_.execute({command, std::move(args), kOrigin});
    };
    actions::Action toggle;
    toggle.id = "transport.toggle";
    toggle.name = "Transport: play/stop";
    toggle.description = "Start playback, or stop it when playing.";
    toggle.builtin = [exec](const actions::Context&) -> Result<json::Value> {
        auto t = exec("transport.get", json::Value::object());
        if (!t) return t;
        return exec((*t)["playing"].asBool() ? "transport.stop" : "transport.play",
                    json::Value::object());
    };
    registry_.add(toggle);
    registry_.add(bound("transport.playrate.reset", "Transport: reset playrate to 1.0",
                        "transport.playrate.set", obj({{"rate", 1.0}})));
    struct RateStep {
        const char* id;
        const char* name;
        double cents;
    };
    for (const RateStep step :
         {RateStep{"transport.playrate.semitone-up", "Transport: increase playrate one semitone",
                   100},
          {"transport.playrate.semitone-down", "Transport: decrease playrate one semitone", -100},
          {"transport.playrate.ten-cents-up", "Transport: increase playrate ten cents", 10},
          {"transport.playrate.ten-cents-down", "Transport: decrease playrate ten cents", -10}}) {
        actions::Action action;
        action.id = step.id;
        action.name = step.name;
        action.builtin = [this, exec, cents = step.cents](const actions::Context&) {
            const double rate = app_.head().project->playrate.rate * std::exp2(cents / 1200.0);
            return exec("transport.playrate.set", obj({{"rate", std::clamp(rate, 0.25, 4.0)}}));
        };
        registry_.add(std::move(action));
    }
    actions::Action preserve;
    preserve.id = "transport.playrate.preserve-pitch";
    preserve.name = "Transport: toggle playrate preserve pitch";
    preserve.builtin = [this, exec](const actions::Context&) {
        return exec("transport.playrate.set",
                    obj({{"preserve_pitch", !app_.head().project->playrate.preservePitch}}));
    };
    registry_.add(std::move(preserve));
    actions::Action loop;
    loop.id = "transport.loop.toggle";
    loop.name = "Transport: toggle loop";
    loop.builtin = [exec](const actions::Context&) -> Result<json::Value> {
        auto t = exec("transport.get", json::Value::object());
        if (!t) return t;
        return exec("transport.loop", obj({{"enabled", !(*t)["loop"]["enabled"].asBool()}}));
    };
    registry_.add(loop);
    registry_.add(bound("transport.seek:start", "Transport: go to start", "transport.seek",
                        obj({{"seconds", 0.0}})));
    actions::Action end;
    end.id = "transport.seek:end";
    end.name = "Transport: go to end";
    end.builtin = [this, exec](const actions::Context&) -> Result<json::Value> {
        double last = 0;
        const model::Session& s = app_.head();
        for (const auto& e : s.items) last = std::max(last, model::itemSpan(s, *e.value).endSec);
        return exec("transport.seek", obj({{"seconds", last}}));
    };
    registry_.add(end);
    for (const char* type : {"channel", "buss", "master"})
        registry_.add(bound(std::string("track.add:") + type, std::string("Track: add ") + type,
                            "track.add", obj({{"type", type}})));
    registry_.add(bound("item.split:playhead", "Item: split selected at playhead", "item.split",
                        obj({{"item", "$each.item"}, {"at", "$playhead.beats"}})));
    registry_.add(bound("item.remove:selected", "Item: remove selected", "item.remove",
                        obj({{"item", "$each.item"}})));
    // Takes of the selected items (the item menu has the same, gui.md §4.3).
    registry_.add(bound("item.explode-takes:tracks", "Item: explode takes onto new tracks",
                        "item.explode-takes", obj({{"item", "$each.item"}, {"mode", "tracks"}})));
    registry_.add(bound("item.explode-takes:serial", "Item: explode takes in sequence",
                        "item.explode-takes", obj({{"item", "$each.item"}, {"mode", "serial"}})));
    registry_.add(bound("item.apply-fx:take", "Item: render with FX as a new take", "item.apply-fx",
                        obj({{"item", "$each.item"}, {"mode", "take"}})));
    registry_.add(bound("item.apply-fx:replace", "Item: render with FX in place (MIDI to audio)",
                        "item.apply-fx", obj({{"item", "$each.item"}, {"mode", "replace"}})));
    actions::Action dropTake;
    dropTake.id = "take.remove:active";
    dropTake.name = "Item: delete the heard take";
    dropTake.description = "Delete each selected item's heard take (an item goes with its last).";
    dropTake.builtin = [this](const actions::Context&) -> Result<json::Value> {
        if (ui_.selectedItems.empty()) return Error("action.selection", "select items first");
        return dispatcher_.transact("Delete take", kOrigin, [&]() -> Result<json::Value> {
            const model::Session s = app_.head();
            for (const std::string& text : ui_.selectedItems) {
                const auto id = model::Id::parse(text);
                const model::Item* it = id ? s.items.find(*id) : nullptr;
                if (!it || !it->activeTake) continue;
                auto r = dispatcher_.execute(
                    {"take.remove", obj({{"take", it->activeTake->str()}}), kOrigin});
                if (!r) return r;
            }
            return json::Value::object();
        });
    };
    registry_.add(dropTake);
    actions::Action removeTracks;
    removeTracks.id = "track.remove:selected";
    removeTracks.name = "Track: delete selected";
    removeTracks.description = "Delete every selected track (not the main master), one undo step.";
    removeTracks.builtin = [this](const actions::Context&) -> Result<json::Value> {
        if (ui_.selectedTracks.empty()) return Error("action.selection", "select tracks first");
        SUPPLE_TRY(gui::removeTracks(dispatcher_, app_.head(), ui_.selectedTracks));
        return json::Value::object();
    };
    registry_.add(removeTracks);
    actions::Action duplicateTracks;
    duplicateTracks.id = "track.duplicate:selected";
    duplicateTracks.name = "Track: duplicate selected";
    duplicateTracks.description =
        "Duplicate every selected channel and buss with its FX, items, sends and automation, "
        "one undo step; the copies become the selection.";
    duplicateTracks.builtin = [this](const actions::Context&) -> Result<json::Value> {
        if (ui_.selectedTracks.empty()) return Error("action.selection", "select tracks first");
        auto copies = gui::duplicateTracks(dispatcher_, app_.head(), ui_.selectedTracks);
        if (!copies) return std::move(copies).error();
        if (!copies->empty()) ui_.selectedTracks = std::move(*copies);
        return json::Value::object();
    };
    registry_.add(duplicateTracks);
    // Delete: what was selected last, tracks (TCP, mixer) or items (arrange).
    actions::Action remove;
    remove.id = "edit.delete";
    remove.name = "Edit: delete the selection";
    remove.description =
        "Remove the selected automation points when an automation lane was "
        "clicked last, the selected tracks when tracks were selected last, else "
        "the selected items.";
    remove.builtin = [this](const actions::Context&) -> Result<json::Value> {
        if (ui_.envelopeFocused) return run("ui.env.points.delete");
        return run(ui_.tracksFocused && !ui_.selectedTracks.empty() ? "track.remove:selected"
                                                                    : "item.remove:selected");
    };
    registry_.add(remove);
    for (auto [name, label] : {std::pair{"beats", "Project: items follow the beat"},
                               {"time", "Project: items follow time"},
                               {"beats_position", "Project: items follow the beat (start only)"}})
        registry_.add(bound(std::string("project.timebase:") + name, label, "project.set",
                            obj({{"timebase", name}})));

    // Followed by a MIDI controller (ADR-0027).
    addControl("control.track.volume", "Track: volume of selected tracks (MIDI CC)",
               "A knob or fader sets the selected tracks' volume along the fader's travel; an "
               "encoder moves it.",
               [this](const actions::ControlValue& v) -> Status {
                   SUPPLE_TRY(controlTracks(v, false, false));
                   return {};
               });
    addControl("control.track.pan", "Track: pan of selected tracks (MIDI CC)",
               "A knob sets the selected tracks' pan, left to right; an encoder moves it.",
               [this](const actions::ControlValue& v) -> Status {
                   SUPPLE_TRY(controlTracks(v, true, false));
                   return {};
               });
    addControl("control.master.volume", "Master: volume (MIDI CC)",
               "A knob or fader sets the main master's volume along the fader's travel.",
               [this](const actions::ControlValue& v) -> Status {
                   SUPPLE_TRY(controlTracks(v, false, true));
                   return {};
               });
    addControl("control.transport.jog", "Transport: move the play cursor by beats (MIDI CC)",
               "An encoder (a relative mode) moves the play cursor a beat per step.",
               [this](const actions::ControlValue& v) -> Status {
                   if (!v.relative || v.steps == 0) return {};
                   const actions::Context c = context();
                   SUPPLE_TRY(dispatcher_.execute(
                       {"transport.seek", obj({{"beat", std::max(0.0, c.playheadBeats + v.steps)}}),
                        kOrigin}));
                   return {};
               });

    addControl("control.transport.playrate", "Transport: playrate (MIDI CC)",
               "A knob or fader sets the master playrate along the transport knob's travel "
               "(1.0 at the middle, its chosen range at the ends); an encoder moves it ten cents "
               "a step.",
               [this](const actions::ControlValue& v) -> Status {
                   const double now = app_.head().project->playrate.rate;
                   double rate;
                   if (v.relative) {
                       if (v.steps == 0) return {};
                       rate = now * std::exp2(v.steps * 10 / 1200.0);
                   } else {
                       // CC 64 of 0..127 is the middle: a centre detent gives 1.0 exactly.
                       const double p = std::abs(v.value - 0.5) <= 0.5 / 127 ? 0.5 : v.value;
                       rate = positionRate(p, playrateRange);
                   }
                   rate = std::clamp(rate, 0.25, 4.0);
                   if (rate == now) return {};
                   SUPPLE_TRY(dispatcher_.execute(
                       {"transport.playrate.set", obj({{"rate", rate}}),
                        cmd::Origin{cmd::Origin::Type::Ui, "midi", {}, v.gesture}}));
                   return {};
               });

    addControl(
        "control.fx.last", "FX: last touched parameter (MIDI CC)",
        "Moves the plug-in parameter edited last, in the FX window or a plug-in's own "
        "editor. A knob takes over once it reaches the parameter's value; an encoder "
        "moves it at once.",
        [this](const actions::ControlValue& v) -> Status {
            const auto t = app_.lastTouched();
            if (!t)
                return Error("fx.untouched", "no plug-in parameter was touched yet", {},
                             "move a parameter in the FX window or a plug-in's editor");
            // Soft takeover per parameter: a knob does not jump the new one.
            engine::Takeover& takeover = touchedTakeover_[t->fx.str() + ":" + t->param];
            const auto set = app_.moveParam(t->fx, t->param, [&](double current, double step) {
                if (v.relative)
                    return v.steps == 0 ? std::optional<double>()
                                        : std::optional<double>(current + v.steps * step);
                engine::ControlReading r;
                r.value = v.value;
                r.press = v.value > 0;
                return engine::nextValue(current, r, model::ControlMode::Absolute, true, step,
                                         takeover);
            });
            if (set && !v.relative) takeover.set = *set;  // the knob holds it now
            return {};
        });

    if (auto loadedControls = ControlMap::load(controlsFile_); loadedControls)
        controls_ = std::move(*loadedControls);
    else
        std::fprintf(stderr, "controls: %s (none bound)\n",
                     loadedControls.error().toString().c_str());

    auto loaded = actions::Keymap::load(keymapFile_);
    if (loaded) {
        keymap_ = std::move(*loaded);
    } else {
        std::fprintf(stderr, "keymap: %s (using the defaults)\n",
                     loaded.error().toString().c_str());
        keymap_ = actions::Keymap::defaults();
    }
}

AppActions::~AppActions() {
    if (auto close = std::exchange(closeLearn_, {})) close();
}

std::uint64_t AppActions::beginLearn(LearnSink sink, std::function<void()> close) {
    if (auto previous = std::exchange(closeLearn_, {})) previous();
    learnOwner_ = ++learnSerial_;
    learnSink = std::move(sink);
    closeLearn_ = std::move(close);
    app_.setLearning(true);
    return learnOwner_;
}

void AppActions::endLearn(std::uint64_t owner) {
    if (owner != learnOwner_) return;
    learnOwner_ = 0;
    learnSink = {};
    closeLearn_ = {};
    app_.setLearning(false);
}

void AppActions::addUi(std::string id, std::string name, std::function<void()> run) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.builtin = [run = std::move(run)](const actions::Context&) -> Result<json::Value> {
        run();
        return json::Value::object();
    };
    registry_.add(std::move(a));
}

void AppActions::addUi(std::string id, std::string name, std::string prompt,
                       std::function<Status(const json::Value& value)> run) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.prompts = {prompt};
    a.builtin = [prompt, run = std::move(run)](const actions::Context& c) -> Result<json::Value> {
        const json::Value* v = c.prompted.find(prompt);
        auto r = run(v ? *v : json::Value());
        if (!r) return std::move(r).error();
        return json::Value::object();
    };
    registry_.add(std::move(a));
}

void AppActions::addUiFor(std::string id, std::string name, std::string key,
                          std::function<Status(const json::Value& value)> run) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.builtin = [key, run = std::move(run)](const actions::Context& c) -> Result<json::Value> {
        const json::Value* v = c.prompted.isObject() ? c.prompted.find(key) : nullptr;
        auto r = run(v ? *v : json::Value());
        if (!r) return std::move(r).error();
        return json::Value::object();
    };
    registry_.add(std::move(a));
}

actions::Context AppActions::context(std::vector<std::string> sections) const {
    actions::Context c;
    c.selectedTracks = ui_.selectedTracks;
    c.selectedItems = ui_.selectedItems;
    c.sections = std::move(sections);
    if (auto t = app_.transport("get")) {
        c.playheadSeconds = t->seconds;
        c.playheadBeats = t->beat;
    }
    return c;
}

Result<json::Value> AppActions::run(const actions::Action& action, const json::Value& prompted) {
    auto r = runner_.run(action, context(), prompted);
    if (!r && onError) onError(action.name + ": " + r.error().message);
    return r;
}

Result<json::Value> AppActions::run(std::string_view id) {
    const actions::Action* a = registry_.find(id);
    if (!a) return Error("action.unknown", "no action " + std::string(id));
    return run(*a);
}

void AppActions::invoke(const actions::Action& action) {
    auto r = runner_.run(action, context());
    if (!r && r.error().code == "action.prompt" && onPrompt) {
        onPrompt(action);
        return;
    }
    if (!r && onError) onError(action.name + ": " + r.error().message);
}

void AppActions::invoke(std::string_view id) {
    if (const actions::Action* a = registry_.find(id))
        invoke(*a);
    else if (onError)
        onError("no action " + std::string(id));
}

bool AppActions::key(int keycode, unsigned mods, bool typing,
                     const std::vector<std::string>& sections) {
    if (typing && !(mods & (2u | 4u | 8u)) && !keymap_.pending()) return false;  // plain keys type
    const bool wasPending = keymap_.pending();
    const auto action = keymap_.press({keycode, mods}, sections);
    if (action) {
        if (const actions::Action* a = registry_.find(*action))
            invoke(*a);
        else if (onError)
            onError("no action " + *action + " (bound to a key)");
        return true;
    }
    return keymap_.pending() || wasPending;
}

Status AppActions::resetKeymap() {
    keymap_ = actions::Keymap::defaults();
    keymap_.clearPending();
    return keymapFile_.empty() ? Status{} : keymap_.save(keymapFile_);
}

void AppActions::addControl(std::string id, std::string name, std::string description,
                            std::function<Status(const actions::ControlValue&)> run) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.description = std::move(description);
    a.builtin = [name = a.name](const actions::Context&) -> Result<json::Value> {
        return Error("action.control", name + " follows a MIDI controller", {},
                     ui::shortcutLabel("bind a controller to it: Ctrl+L in the actions list"));
    };
    a.control = [run = std::move(run)](const actions::ControlValue& v,
                                       const actions::Context&) -> Result<json::Value> {
        SUPPLE_TRY(run(v));
        return json::Value::object();
    };
    registry_.add(std::move(a));
}

Result<json::Value> AppActions::controlTracks(const actions::ControlValue& v, bool pan,
                                              bool master) {
    const model::Session s = app_.head();
    std::vector<model::Id> tracks;
    if (master)
        tracks.push_back(model::kMaster);
    else
        for (const std::string& text : ui_.selectedTracks)
            if (auto id = model::Id::parse(text); id && s.tracks.find(*id)) tracks.push_back(*id);
    if (tracks.empty()) return json::Value::object();
    const cmd::Origin origin{cmd::Origin::Type::Ui, "midi", {}, v.gesture};
    return dispatcher_.transact(
        pan ? "Track pan" : "Track volume", origin, [&]() -> Result<json::Value> {
            for (const model::Id& id : tracks) {
                const model::Track& t = *s.tracks.find(id);
                json::Value args = json::Value::object();
                args["track"] = id.str();
                if (pan) {  // an encoder step is 1 %; a knob spans hard left to hard right
                    const double to =
                        v.relative ? t.pan + v.steps : std::round(v.value * 200.0 - 100.0);
                    args["pan"] = std::clamp(to, -100.0, 100.0);
                } else {
                    double p = ui::faderPosition(t.volumeDb);
                    p = std::clamp(v.relative ? p + v.steps / 128.0 : v.value, 0.0, 1.0);
                    args["volume_db"] = std::clamp(ui::faderDb(p), model::kSilenceDb, 12.0);
                }
                SUPPLE_TRY(dispatcher_.execute({"track.set", std::move(args), origin}));
            }
            return json::Value::object();
        });
}

Status AppActions::saveControls() const {
    if (controlsFile_.empty()) return {};
    return controls_.save(controlsFile_);
}

std::string AppActions::followingControl(std::string_view action) const {
    const actions::Action* a = registry_.find(action);
    if (!a || a->control) return {};
    if (action == "transport.playrate.set") return "control.transport.playrate";
    return {};
}

std::string AppActions::controlsFor(std::string_view action) const {
    const auto bound = controls_.bindingsFor(action);
    return bound.empty() ? std::string() : engine::describe(bound.front().control);
}

void AppActions::control(const std::vector<engine::EngineApp::ControlEvent>& events) {
    if (learnSink) {
        auto sink = learnSink;  // the dialog may close on what it hears
        sink(events);
        return;
    }
    if (controls_.empty()) return;
    const std::uint64_t now = app_.clockMs();
    const std::vector<ActionControl> bindings = controls_.bindings();  // actions may rebind
    for (const ActionControl& b : bindings) {
        const actions::Action* a = registry_.find(b.action);
        if (!a) continue;
        bool heard = false;
        int presses = 0;
        actions::ControlValue value;
        for (const auto& e : events) {
            if (!engine::matches(b.control, e.midi, e.device)) continue;
            heard = true;
            const engine::ControlReading r = engine::read(e.midi, b.mode);
            if (r.relative) {
                value.relative = true;
                value.steps += r.steps;
                if (r.steps != 0) ++presses;
            } else {
                value.value =
                    b.mode == model::ControlMode::Toggle ? (r.press ? 1.0 : 0.0) : r.value;
                if (r.press) ++presses;
            }
        }
        if (!heard) continue;
        if (a->control) {
            ControlState& st = controlState_[b.action];
            if (now > st.lastMs + 1000) ++st.serial;
            st.lastMs = now;
            value.gesture = "midi:" + b.action + ":" + std::to_string(st.serial);
            const actions::Action copy = *a;
            if (auto r = copy.control(value, context()); !r && onError)
                onError(copy.name + ": " + r.error().message);
            continue;
        }
        const actions::Action copy = *a;  // a run may replace the registry's entry
        for (int i = 0; i < presses; ++i) invoke(copy);
    }
}

std::string AppActions::keysFor(std::string_view action, bool all) const {
    std::vector<std::string> shown;
    for (const actions::Binding& b : keymap_.bindingsFor(action)) {
        std::string keys;
        for (const actions::Chord& c : b.keys)
            keys += (keys.empty() ? "" : " ") + actions::describe(c);
        // The same keys bound in another section show once.
        if (std::find(shown.begin(), shown.end(), keys) == shown.end()) shown.push_back(keys);
        if (!all) break;
    }
    std::string out;
    for (const std::string& keys : shown) out += (out.empty() ? "" : ", ") + keys;
    return out;
}

}  // namespace supple::gui
