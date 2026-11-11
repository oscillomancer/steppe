// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI learn on tracks', sends' and VCAs' own controls (ADR-0058): a strip's
// fader, pan and buttons, a send's level, pan and buttons, a VCA's fader, mute
// and solo. The bindings live on the track, the send or the VCA (`learn`);
// track.learn.*, route.learn.* and vca.learn.* edit and read them;
// EngineApp::pollControl moves the controls through moveControl, as
// track.set, route.set and vca.set edits.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "command_validation.h"
#include "learn_binding.h"
#include "supple/base/fader.h"
#include "supple/engine/app.h"
#include "supple/engine/midi_control.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

constexpr std::uint64_t kGestureMs = 1000;  // a pause this long starts a new undo step (learn.cpp)

EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
json::Value j(std::string_view s) { return json::parse(s).value(); }

// How a control moves: a level along the fader's travel, a pan, a switch
// (on/off), the monitor (off, on, auto in turn) or one of a send's taps.
enum class Shape { Level, Pan, Switch, Cycle, Choice };
struct ControlSpec {
    std::string_view param;
    Shape shape;
    std::string_view field;        // its track.set / route.set argument
    std::string_view choice = {};  // a tap's value
};
constexpr ControlSpec kTrackControls[] = {{"volume", Shape::Level, "volume_db"},
                                          {"pan", Shape::Pan, "pan"},
                                          {"width", Shape::Pan, "width"},
                                          {"pan_left", Shape::Pan, "pan_left"},
                                          {"pan_right", Shape::Pan, "pan_right"},
                                          {"pan_boost", Shape::Switch, "pan_law"},
                                          {"mute", Shape::Switch, "mute"},
                                          {"solo", Shape::Switch, "solo"},
                                          {"phase_invert", Shape::Switch, "phase_invert"},
                                          {"mono", Shape::Switch, "mono"},
                                          {"armed", Shape::Switch, "armed"},
                                          {"monitor", Shape::Cycle, "monitor"},
                                          {"lane_mute", Shape::Switch, "lane_mute"}};
constexpr ControlSpec kRouteControls[] = {{"volume", Shape::Level, "volume_db"},
                                          {"pan", Shape::Pan, "pan"},
                                          {"mute", Shape::Switch, "mute"},
                                          {"phase_invert", Shape::Switch, "phase_invert"},
                                          {"mono", Shape::Switch, "mono"},
                                          {"tap:post_fader", Shape::Choice, "tap", "post_fader"},
                                          {"tap:pre_fader", Shape::Choice, "tap", "pre_fader"},
                                          {"tap:pre_fx", Shape::Choice, "tap", "pre_fx"}};
constexpr ControlSpec kVcaControls[] = {{"volume", Shape::Level, "volume_db"},
                                        {"mute", Shape::Switch, "mute"},
                                        {"solo", Shape::Switch, "solo"}};

// How the commands name an object of each kind: the command prefix
// (track.learn.set), the argument naming it and its id pattern.
struct Owner {
    std::string_view prefix, key, noun, pattern, what;
};
Owner ownerOf(model::Kind kind) {
    if (kind == model::Kind::Route) return {"route", "route", "send", "^sd_[0-9]+$", "route id"};
    if (kind == model::Kind::Group) return {"vca", "group", "VCA", "^gr_[0-9]+$", "group id"};
    return {"track", "track", "track", "^tr_[0-9]+$", "track id"};
}

const ControlSpec* findSpec(model::Kind kind, std::string_view param) {
    const auto look = [&](const auto& list) -> const ControlSpec* {
        for (const ControlSpec& c : list)
            if (c.param == param) return &c;
        return nullptr;
    };
    if (kind == model::Kind::Track) return look(kTrackControls);
    if (kind == model::Kind::Route) return look(kRouteControls);
    if (kind == model::Kind::Group) return look(kVcaControls);
    return nullptr;
}

std::string controlNames(model::Kind kind) {
    std::string out;
    const auto add = [&](const auto& list) {
        for (const ControlSpec& c : list) out += (out.empty() ? "" : ", ") + std::string(c.param);
    };
    if (kind == model::Kind::Route)
        add(kRouteControls);
    else if (kind == model::Kind::Group)
        add(kVcaControls);
    else
        add(kTrackControls);
    return out;
}

std::string_view tapName(model::Tap tap) {
    switch (tap) {
        case model::Tap::PostFader:
            return "post_fader";
        case model::Tap::PreFader:
            return "pre_fader";
        case model::Tap::PreFx:
            return "pre_fx";
    }
    return "post_fader";
}

double panPosition(double pan) { return (std::clamp(pan, -100.0, 100.0) + 100.0) / 200.0; }
// A knob's centre (CC 64 of 0..127) is the centre exactly.
double panAt(double position) {
    if (std::abs(position - 0.5) <= 0.5 / 127 + 1e-9) return 0;
    return std::clamp(std::round(position * 200.0 - 100.0), -100.0, 100.0);
}

// Where the control stands, 0..1: a level along the fader's travel, a pan
// from hard left, a switch 0 or 1, the monitor 0, 0.5 or 1 (off, on, auto), a
// tap 1 while the send has it.
std::optional<double> standing(const model::Session& s, Id id, const ControlSpec& c) {
    const auto on = [](bool b) { return b ? 1.0 : 0.0; };
    if (id.kind == model::Kind::Track) {
        const model::Track* t = s.tracks.find(id);
        if (!t) return std::nullopt;
        if (c.param == "volume") return fader::position(t->volumeDb);
        if (c.param == "pan") return panPosition(t->pan);
        if (c.param == "width") return panPosition(t->width);
        if (c.param == "pan_left") return panPosition(t->panLeft);
        if (c.param == "pan_right") return panPosition(t->panRight);
        if (c.param == "pan_boost") return on(model::panLawOf(*s.project, *t).boost);
        if (c.param == "mute") return on(t->mute);
        if (c.param == "solo") return on(t->solo != model::Solo::Off);
        if (c.param == "phase_invert") return on(t->phaseInvert);
        if (c.param == "mono") return on(t->mono);
        if (c.param == "armed") return on(t->record.armed);
        if (c.param == "lane_mute") return on(t->laneMute);
        if (c.param == "monitor")
            return t->record.monitor == model::Monitor::Off  ? 0.0
                   : t->record.monitor == model::Monitor::On ? 0.5
                                                             : 1.0;
        return std::nullopt;
    }
    if (id.kind == model::Kind::Group) {
        const model::TrackGroup* g = s.groups.find(id);
        if (!g || !g->vca) return std::nullopt;
        if (c.param == "volume") return fader::position(g->vca->volumeDb);
        if (c.param == "mute") return on(g->vca->mute);
        if (c.param == "solo") return on(g->vca->solo);
        return std::nullopt;
    }
    const model::Route* r = s.routes.find(id);
    if (!r) return std::nullopt;
    if (c.param == "volume") return fader::position(r->volumeDb);
    if (c.param == "pan") {
        const model::Track* from = r->from.track ? s.tracks.find(*r->from.track) : nullptr;
        return panPosition(r->pan.resolve(from ? from->pan : 0.0));
    }
    if (c.param == "mute") return on(r->mute);
    if (c.param == "phase_invert") return on(r->phaseInvert);
    if (c.param == "mono") return on(r->mono);
    if (c.shape == Shape::Choice) return on(tapName(r->tap) == c.choice);
    return std::nullopt;
}

// One encoder step: 1/128 of the fader's travel, 1 % of pan, a whole switch.
double stepOf(const ControlSpec& c) {
    return c.shape == Shape::Level ? 1.0 / 128 : c.shape == Shape::Pan ? 1.0 / 200 : 1.0;
}

// The control's track.set / route.set / vca.set value at `to` (0..1).
json::Value argValue(model::Kind kind, const ControlSpec& c, double to) {
    switch (c.shape) {
        case Shape::Level:
            return std::clamp(fader::db(to), model::kSilenceDb, 12.0);
        case Shape::Pan:
            return panAt(to);
        case Shape::Switch:
            if (c.param == "solo" && kind == model::Kind::Track)  // a VCA's solo is a bool
                return std::string(to >= 0.5 ? "on" : "off");
            return to >= 0.5;
        case Shape::Cycle:
            return std::string(to < 0.25 ? "off" : to < 0.75 ? "on" : "auto");
        case Shape::Choice:
            return std::string(c.choice);
    }
    return json::Value();
}

// A switch from one message: a pad (a note or a program change) flips it on
// each press; a CC in toggle mode does too; an absolute CC turns it on in
// its upper half (a button's 127) and off below (its 0); an encoder turns it
// on stepping up and off stepping down.
std::optional<double> switchValue(double current, const ControlReading& r,
                                  const model::ControlLearn& learn) {
    const double flipped = current >= 0.5 ? 0.0 : 1.0;
    if (learn.control.kind == model::ControlKind::Note ||
        learn.control.kind == model::ControlKind::Program)
        return r.press ? std::optional<double>(flipped) : std::nullopt;
    if (r.relative) return r.steps == 0 ? std::nullopt : std::optional(r.steps > 0 ? 1.0 : 0.0);
    if (learn.mode == model::ControlMode::Toggle)
        return r.press ? std::optional<double>(flipped) : std::nullopt;
    return r.value >= 0.5 ? 1.0 : 0.0;
}

// What a frozen track's strip still changes (its sound is printed).
bool frozenAllows(std::string_view param) {
    return param == "volume" || param == "pan" || param == "width" || param == "pan_left" ||
           param == "pan_right" || param == "pan_boost" || param == "mute" || param == "solo";
}

struct LearnArgs : LearnBindingArgs {
    std::string target, param;
};
struct LearnRef {
    std::string target, param;
};
struct Empty {};
struct LearnListArgs {
    std::optional<std::string> target;
};
struct LearnList {
    json::Value learn = json::Value::array();
};

const std::vector<model::ControlLearn>* learnOf(const model::Session& s, Id id) {
    if (id.kind == model::Kind::Track) {
        const model::Track* t = s.tracks.find(id);
        return t ? &t->learn : nullptr;
    }
    if (id.kind == model::Kind::Group) {
        const model::TrackGroup* g = s.groups.find(id);
        return g && g->vca ? &g->vca->learn : nullptr;
    }
    const model::Route* r = s.routes.find(id);
    return r ? &r->learn : nullptr;
}

void editLearn(EngineApp& engine, Id id,
               const std::function<void(std::vector<model::ControlLearn>&)>& edit) {
    if (id.kind == model::Kind::Track)
        engine.txn().modify<model::Track>(id, [&](model::Track& t) { edit(t.learn); });
    else if (id.kind == model::Kind::Group)
        engine.txn().modify<model::TrackGroup>(id,
                                               [&](model::TrackGroup& g) { edit(g.vca->learn); });
    else
        engine.txn().modify<model::Route>(id, [&](model::Route& r) { edit(r.learn); });
}

// The object a command names, of its kind.
Result<Id> target(const model::Session& s, model::Kind kind, const std::string& text) {
    const auto id = Id::parse(text);
    const bool found = id && id->kind == kind && learnOf(s, *id);
    if (found) return *id;
    if (kind == model::Kind::Track)
        return Error("track.unknown", "no track " + text, "/track", "use an id from track.list");
    if (kind == model::Kind::Group) {
        if (id && id->kind == kind && s.groups.contains(*id))
            return Error("vca.missing", "group " + text + " has no VCA", "/group",
                         "add one with vca.add");
        return Error("group.unknown", "no group " + text, "/group", "use an id from group.list");
    }
    return Error("route.unknown", "no send " + text, "/route", "use an id from route.list");
}

// A control the strip has: monitoring is a channel's, arming a channel's or
// a master's, the lane mute a master's.
Status checkStrip(const model::Session& s, Id id, std::string_view param) {
    if (id.kind == model::Kind::Route && s.routes.find(id)->midi && param != "mute")
        return Error("route.midi", "MIDI sends can learn mute only", "/param", "use param: mute");
    if (id.kind != model::Kind::Track) return {};
    const model::Track& t = *s.tracks.find(id);
    const bool ok =
        param == "monitor"     ? t.type == model::StripType::Channel
        : param == "armed"     ? t.type != model::StripType::Buss
        : param == "lane_mute" ? t.type == model::StripType::Master
        : (param == "width" || param == "pan_left" || param == "pan_right" || param == "pan_boost")
            ? t.type != model::StripType::Master
            : true;
    if (ok) return {};
    return Error(
        "track.strip_type",
        std::string(model::stripTypeName(t.type)) + " has no " + std::string(param), "/param",
        param == "lane_mute" ? "lane_mute is a master's"
        : param == "armed"   ? "arm channels and masters"
        : param == "monitor" ? "monitoring is a channel's"
                             : "learn pan modes and gain compensation on a channel or buss");
}

json::Value learnJson(std::string_view key, Id id, const model::ControlLearn& l) {
    json::Value v = model::controlLearnSchema().encode(l);
    v[std::string(key)] = id.str();
    v["control"] = describe(l.control);
    return v;
}

void registerFor(cmd::Registry& registry, model::Kind kind) {
    const bool route = kind == model::Kind::Route;
    const bool vca = kind == model::Kind::Group;
    const Owner owner = ownerOf(kind);
    const std::string prefix(owner.prefix), key(owner.key), noun(owner.noun);
    auto idS = schema::string().pattern(std::string(owner.pattern), std::string(owner.what));
    const std::string fixture = route ? "song_with_route" : vca ? "song_with_vca" : "song";
    const std::string example = route ? "sd_1" : vca ? "gr_1" : "tr_1";
    {
        Command<LearnArgs, LearnSetResult> c;
        c.name = prefix + ".learn.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = route ? "Let a MIDI controller move a send's level, pan, mute or tap."
                    : vca ? "Let a MIDI controller move a VCA's fader, mute or solo."
                          : "Let a MIDI controller move a track's fader, pan, mute, solo or arm.";
        c.description =
            "One binding per control (a new one replaces it); saved with the project. param: " +
            controlNames(kind) +
            ". midi.device: a substring of the MIDI input's name (empty: any). A level or pan "
            "follows a knob (mode absolute, soft_takeover: once it reaches the value) or an "
            "encoder (relative1 127 = -1, relative2 63 = -1, relative3 65 = -1). A button: a "
            "note or program change flips it per press, as a CC in toggle mode does; an absolute "
            "CC is on above 63." +
            (route ? " A tap:… control sets that tap on each press."
             : vca ? " The bindings go with the VCA."
                   : " monitor goes off, on, auto in turn on each press.");
        c.tags = {prefix, "midi"};
        auto args = schema::object<LearnArgs>(prefix + "_learn_set_args");
        args.field(key, &LearnArgs::target, idS)
            .field("param", &LearnArgs::param,
                   schema::string().minLength(1).doc("The control: " + controlNames(kind)));
        c.args = learnBindingArgFields(args).build();
        c.result = schema::object<LearnSetResult>(prefix + "_learn_set_result")
                       .field("param", &LearnSetResult::param, schema::string())
                       .field("control", &LearnSetResult::control, schema::string())
                       .build();
        if (route)
            c.examples = {
                {"Mute the send from a pad",
                 j(R"({"route":"sd_1","param":"mute","midi":{"kind":"note","channel":10,"number":36}})"),
                 j(R"({"param":"mute","control":"Note C2 · ch 10"})"), fixture}};
        else if (vca)
            c.examples = {
                {"The VCA's fader on CC 9",
                 j(R"({"group":"gr_1","param":"volume","midi":{"kind":"cc","channel":1,"number":9}})"),
                 j(R"({"param":"volume","control":"CC 9 · ch 1"})"), fixture}};
        else
            c.examples = {
                {"The fader on CC 7",
                 j(R"({"track":"tr_1","param":"volume","midi":{"kind":"cc","channel":1,"number":7}})"),
                 j(R"({"param":"volume","control":"CC 7 · ch 1"})"), fixture}};
        c.errors = {
            {prefix + ".learn_param", "no such control", "use one of: " + controlNames(kind)},
            {"learn.mode", "toggle on a level or pan", "use absolute or a relative mode"}};
        c.run = [kind, prefix](const LearnArgs& a, Context& ctx) -> Result<LearnSetResult> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            auto id = target(s, kind, a.target);
            if (!id) return std::move(id).error();
            const ControlSpec* spec = findSpec(kind, a.param);
            if (!spec)
                return Error(prefix + ".learn_param", "no control " + a.param, "/param",
                             "use one of: " + controlNames(kind));
            SUPPLE_TRY(checkStrip(s, *id, a.param));
            model::ControlLearn learn{a.binding(), a.param};
            if (learn.mode == model::ControlMode::Toggle &&
                (spec->shape == Shape::Level || spec->shape == Shape::Pan))
                return Error("learn.mode", "toggle flips buttons; a level or pan follows a knob",
                             "/mode", "use absolute, or relative1..3 for an encoder");
            if (learn.control.kind != model::ControlKind::Cc)
                learn.mode = model::ControlMode::Absolute;
            editLearn(engine, *id, [&](std::vector<model::ControlLearn>& list) {
                std::erase_if(list, [&](const auto& l) { return l.param == learn.param; });
                list.push_back(learn);
                std::sort(list.begin(), list.end(),
                          [](const auto& x, const auto& y) { return x.param < y.param; });
            });
            return LearnSetResult{learn.param, describe(learn.control)};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<LearnRef, Empty> c;
        c.name = prefix + ".learn.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a " + noun + " control's MIDI controller binding.";
        c.tags = {prefix, "midi"};
        c.args = schema::object<LearnRef>(prefix + "_learn_remove_args")
                     .field(key, &LearnRef::target, idS)
                     .field("param", &LearnRef::param, schema::string().minLength(1))
                     .build();
        c.result = schema::object<Empty>("empty").build();
        c.examples = {{"Free the level",
                       j("{\"" + key + "\":\"" + example + "\",\"param\":\"volume\"}"), j("{}"),
                       "song_with_mix_learn"}};
        c.run = [kind, prefix](const LearnRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            auto id = target(s, kind, a.target);
            if (!id) return std::move(id).error();
            const auto& list = *learnOf(s, *id);
            if (std::none_of(list.begin(), list.end(),
                             [&](const auto& l) { return l.param == a.param; }))
                return Error(prefix + ".learn_unknown", a.param + " has no MIDI binding", "/param",
                             "use " + prefix + ".learn.list for the bound controls");
            editLearn(engine, *id, [&](std::vector<model::ControlLearn>& l) {
                std::erase_if(l, [&](const auto& x) { return x.param == a.param; });
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<LearnListArgs, LearnList> c;
        c.name = prefix + ".learn.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the MIDI controller bindings of " + noun + " controls.";
        c.description =
            "Every " + noun + "'s, or one's (" + key + "). control names the message in words.";
        c.tags = {prefix, "midi"};
        c.args = schema::object<LearnListArgs>(prefix + "_learn_list_args")
                     .field(key, &LearnListArgs::target, idS)
                     .build();
        c.result = schema::object<LearnList>(prefix + "_learn_list")
                       .field("learn", &LearnList::learn, schema::any())
                       .build();
        c.examples = {
            {"Every binding", json::Value::object(),
             j(route
                   ? R"({"learn":[{"route":"sd_1","param":"volume","midi":{"kind":"cc","channel":1,"number":12},"control":"CC 12 · ch 1"}]})"
               : vca
                   ? R"({"learn":[{"group":"gr_1","param":"volume","midi":{"kind":"cc","channel":1,"number":9},"control":"CC 9 · ch 1"}]})"
                   : R"({"learn":[{"track":"tr_1","param":"volume","midi":{"kind":"cc","channel":1,"number":7},"control":"CC 7 · ch 1"}]})"),
             "song_with_mix_learn"}};
        c.run = [kind, key](const LearnListArgs& a, Context& ctx) -> Result<LearnList> {
            const model::Session s = app(ctx).view();
            LearnList out;
            if (a.target) {
                auto id = target(s, kind, *a.target);
                if (!id) return std::move(id).error();
                for (const auto& l : *learnOf(s, *id)) out.learn.push_back(learnJson(key, *id, l));
                return out;
            }
            if (kind == model::Kind::Track) {
                for (const auto& e : s.tracks)
                    for (const auto& l : e.value->learn)
                        out.learn.push_back(learnJson(key, e.id, l));
            } else if (kind == model::Kind::Group) {
                for (const auto& e : s.groups)
                    if (e.value->vca)
                        for (const auto& l : e.value->vca->learn)
                            out.learn.push_back(learnJson(key, e.id, l));
            } else {
                for (const auto& e : s.routes)
                    for (const auto& l : e.value->learn)
                        out.learn.push_back(learnJson(key, e.id, l));
            }
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace

void EngineApp::applyControlLearn(const std::vector<ControlEvent>& events) {
    const model::Session s = head();
    const auto apply = [&](Id id, const model::ControlLearn& learn) {
        const ControlSpec* spec = findSpec(id.kind, learn.param);
        if (!spec) return;
        if (!heardControl(events, learn.control)) return;
        Takeover& takeover = learnState_[{id, learn.param}].takeover;
        driveLearned(
            events, learn, takeover,
            [&](const auto& next) { return moveControl(id, learn.param, next); },
            [&](double current, const ControlReading& r, double step) -> std::optional<double> {
                switch (spec->shape) {
                    case Shape::Level:
                    case Shape::Pan:
                        return nextValue(current, r, learn.mode, learn.softTakeover, step,
                                         takeover);
                    case Shape::Switch:
                        return switchValue(current, r, learn);
                    case Shape::Cycle:
                        if (r.press) return current < 0.25 ? 0.5 : current < 0.75 ? 1.0 : 0.0;
                        break;
                    case Shape::Choice:
                        if (r.press) return 1.0;
                        break;
                }
                return std::nullopt;
            });
    };
    for (const auto& e : s.tracks)
        for (const model::ControlLearn& learn : e.value->learn) apply(e.id, learn);
    for (const auto& e : s.routes)
        for (const model::ControlLearn& learn : e.value->learn) apply(e.id, learn);
    for (const auto& e : s.groups)
        if (e.value->vca)
            for (const model::ControlLearn& learn : e.value->vca->learn) apply(e.id, learn);
}

std::optional<double> EngineApp::moveControl(
    model::Id id, const std::string& param,
    const std::function<std::optional<double>(double current, double step)>& next) {
    const model::Session s = head();
    const ControlSpec* spec = findSpec(id.kind, param);
    if (!spec) return std::nullopt;
    const auto current = standing(s, id, *spec);
    if (!current) return std::nullopt;
    if (id.kind == model::Kind::Track && s.tracks.find(id)->freeze && !frozenAllows(param))
        return std::nullopt;
    auto moved = next(*current, stepOf(*spec));
    if (!moved) return std::nullopt;
    double to = std::clamp(*moved, 0.0, 1.0);
    if (spec->shape == Shape::Pan) to = panPosition(panAt(to));
    if (std::abs(to - *current) < 1e-12) return std::nullopt;
    if (spec->shape == Shape::Choice && to < 0.5) return std::nullopt;  // a tap is only set
    const std::uint64_t now = serviceClockMs();
    LearnState& state = learnState_[{id, param}];
    if (now > state.lastMs + kGestureMs) ++state.serial;
    state.lastMs = now;
    const Owner owner = ownerOf(id.kind);
    json::Value args = json::Value::object();
    args[std::string(owner.key)] = id.str();
    if (param == "pan_boost") {
        const auto* track = s.tracks.find(id);
        auto law = model::panLawOf(*s.project, *track);
        law.boost = to >= .5;
        args["pan_law"] = model::panLawSchema().encode(law);
    } else
        args[std::string(spec->field)] = argValue(id.kind, *spec, to);
    const cmd::Origin origin{cmd::Origin::Type::Internal, "midi", "",
                             "midi:" + id.str() + ":" + param + ":" + std::to_string(state.serial)};
    if (!commands_) return std::nullopt;
    if (auto set =
            commands_->execute({std::string(owner.prefix) + ".set", std::move(args), origin});
        !set) {
        emit("engine.error", pluginErrorJson(set.error()));
        return std::nullopt;
    }
    return to;
}

void registerControlLearnCommands(cmd::Registry& registry) {
    registerFor(registry, model::Kind::Track);
    registerFor(registry, model::Kind::Route);
    registerFor(registry, model::Kind::Group);
}

}  // namespace supple::engine
