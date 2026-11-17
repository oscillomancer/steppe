// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Routing commands: route.add, route.set, route.remove, route.list and route.matrix.
#include "command_helpers.h"

namespace supple::engine {

namespace {

struct RouteArgs {
    std::string from;
    std::optional<std::string> to, fx;
    std::optional<model::Tap> tap;
    std::optional<double> volumeDb;
    std::optional<model::RoutePan> pan;
    std::optional<bool> mute, mono, phaseInvert;
    std::optional<model::ChannelMap> channels;
    std::optional<model::MidiRouting> midi;
    std::optional<std::string> toMidi;
};

struct RouteSetArgs {
    std::string route;
    std::optional<std::string> from, to, fx;
    std::optional<model::Tap> tap;
    std::optional<double> volumeDb;
    std::optional<model::RoutePan> pan;
    std::optional<bool> mute, mono, phaseInvert;
    std::optional<model::ChannelMap> channels;
    std::optional<model::MidiRouting> midi;
    std::optional<std::string> toMidi;
};

struct RouteRef {
    std::string route;
};

struct RouteEntry {
    std::string id, from, to, fx, tap;
    double volumeDb = 0;
    model::RoutePan pan;
    bool mute = false, mono = false, phaseInvert = false;
    model::ChannelMap channels;
    std::optional<model::MidiRouting> midi;
    std::optional<std::string> fromMidi, toMidi;
};

std::string midiModuleLabel(const model::Session& session, Id id) {
    const auto* module = session.modules.find(id);
    if (!module) return id.str();
    if (!module->label.empty()) return module->label;
    const std::string name = module->type == model::kNotesOutModule ? "Note Out" : "Notes";
    std::size_t count = 0, ordinal = 0;
    for (const auto& entry : session.modules)
        if (entry.value->rack == module->rack && entry.value->type == module->type) {
            ++count;
            if (entry.id == id) ordinal = count;
        }
    return count > 1 ? name + " " + std::to_string(ordinal) : name;
}

struct RouteMatrixArgs {
    std::string mode = "audio";
};

struct RouteList {
    std::vector<RouteEntry> routes;
};

auto tapSchema() {
    return schema::enumeration<model::Tap>({{model::Tap::PostFader, "post_fader"},
                                            {model::Tap::PreFader, "pre_fader"},
                                            {model::Tap::PreFx, "pre_fx"}});
}

schema::Schema<model::ChannelMap> channelMapSchema() {
    return schema::object<model::ChannelMap>("channel_map")
        .field("from", &model::ChannelMap::from,
               schema::arrayOf(schema::integerOf<int>().range(1, model::kMaxTrackChannels)))
        .field("to", &model::ChannelMap::to,
               schema::arrayOf(schema::integerOf<int>().range(1, model::kMaxTrackChannels)))
        .build();
}

// Canonical ids internally; the object form names a route end explicitly.
auto routeEndArg() {
    auto exported =
        json::parse(
            R"({"oneOf":[{"type":"string","pattern":"^(tr|mo)_[0-9]+$"},{"type":"object","properties":{"module":{"type":"string","pattern":"^mo_[0-9]+$"}},"required":["module"],"additionalProperties":false},{"type":"object","properties":{"track":{"type":"string","pattern":"^tr_[0-9]+$"}},"required":["track"],"additionalProperties":false}]})")
            .value();
    return schema::Schema<std::string>(
        "route_end",
        [](const json::Value& v, std::string& out, schema::DecodeCtx& ctx,
           const std::string& path) {
            const json::Value* id = &v;
            std::string_view kind;
            if (v.isObject() && v.size() == 1) {
                if ((id = v.find("module")))
                    kind = "mo";
                else if ((id = v.find("track")))
                    kind = "tr";
            }
            auto parsed = id && id->isString() ? model::Id::parse(id->asString()) : std::nullopt;
            if (!parsed ||
                (parsed->kind != model::Kind::Track && parsed->kind != model::Kind::Module) ||
                (!kind.empty() && id->asString().substr(0, 2) != kind)) {
                ctx.error("schema.type", "expected a track or module route end", path,
                          "use tr_N, mo_N, {track: tr_N} or {module: mo_N}");
                return false;
            }
            out = id->asString();
            return true;
        },
        [](const std::string& id) { return json::Value(id); }, exported);
}

auto midiRouteArgs() {
    auto b = schema::object<model::MidiRouting>("midi_routing");
    b.field("from_channel", &model::MidiRouting::fromChannel,
            schema::integerOf<int>().range(0, 16));
    b.defaultValue(0);
    b.field("to_channel", &model::MidiRouting::toChannel, schema::integerOf<int>().range(0, 16));
    b.defaultValue(0);
    return b.build();
}

template <class T>
schema::Schema<T> routeArgsSchema(const char* name) {
    auto b = schema::object<T>(name);
    if constexpr (std::is_same_v<T, RouteSetArgs>)
        b.field("route", &T::route, idArg("sd", "Route"));
    b.field("from", &T::from, routeEndArg());
    b.field("to", &T::to, routeEndArg());
    b.field("to_midi", &T::toMidi,
            schema::string().minLength(1).doc(
                "Exact hardware MIDI output name; requires midi: {} and replaces to/fx."));
    b.field("midi", &T::midi,
            midiRouteArgs().doc("MIDI send routing; {} means every channel unchanged. Channel "
                                "values 0..16: from 0 = all, to 0 = original."));
    b.field("fx", &T::fx,
            idArg("fx", "Plugin instance")
                .doc("Feed this plug-in's sidechain (key) input instead of a track's "
                     "input; `to` may be left out"));
    b.field("tap", &T::tap, tapSchema());
    b.field("volume_db", &T::volumeDb,
            schema::number().range(model::kSilenceDb, 12).unit(units::Unit::dB));
    b.field("pan", &T::pan, model::routePanSchema());
    b.field("mute", &T::mute, schema::boolean());
    b.field("mono", &T::mono, schema::boolean().doc("Sum the source to mono before the pan"));
    b.field("phase_invert", &T::phaseInvert, schema::boolean());
    b.field("channels", &T::channels,
            channelMapSchema().doc(
                "Which source channel feeds which destination channel, pair by pair: "
                "{from: [1, 2], to: [2, 1]} swaps, {from: [1, 1], to: [1, 2]} sends the left "
                "channel to both, {from: [], to: []} restores the identity"));
    return b.build();
}

Result<model::Route> buildRoute(const RouteArgs& a, const model::Session& s, EngineApp& app,
                                const model::Route* old = nullptr, std::optional<Id> ignored = {}) {
    model::Route r = old ? *old : model::Route{};
    if (a.midi) r.midi = *a.midi;
    if (!a.from.empty()) {
        r.from = model::RouteEnd{};
        if (a.from.starts_with("mo_")) {
            auto from = existing<model::RackModule>(s, a.from, "/from");
            if (!from) return std::move(from).error();
            r.from.module = *from;
        } else {
            auto from = existing<model::Track>(s, a.from, "/from");
            if (!from) return std::move(from).error();
            r.from.track = *from;
        }
    }
    if (a.toMidi) {
        if (a.to || a.fx)
            return Error("route.endpoint", "choose one MIDI output or track/module/key destination",
                         "/to_midi", "omit to and fx when using to_midi");
        r.to = {};
        r.to.midiDevice = *a.toMidi;
    } else if (a.fx) {  // a key route (ADR-0018)
        if (r.midi)
            return Error("route.endpoint", "a plug-in key is an audio-only destination", "/fx",
                         "send MIDI to a channel or a Notes module with to");
        auto fx = existing<model::FxInstance>(s, *a.fx, "/fx");
        if (!fx) return std::move(fx).error();
        const auto owner = model::trackOfFx(s, *fx);
        if (!owner)
            return Error("route.fx", *a.fx + " is not in a track's FX chain", "/fx",
                         "key routes feed plug-ins in track chains (not take or input FX)");
        if (a.to && *a.to != owner->str())
            return Error("route.endpoint", *a.fx + " is on " + owner->str() + ", not " + *a.to,
                         "/to", "leave out `to` or give " + owner->str());
        auto channels = app.fxSidechainChannels(*s.fx.find(*fx));
        if (!channels) return std::move(channels).error().at("/fx");
        if (*channels == 0)
            return Error("route.no_sidechain", *a.fx + " has no sidechain input", "/fx",
                         "choose a plug-in with a key input (fx.describe shows `sidechain`)");
        r.to = model::RouteEnd{};
        r.to.fx = *fx;
    } else if (a.to && a.to->starts_with("mo_")) {  // a rack's In module (ADR-0062)
        auto to = existing<model::RackModule>(s, *a.to, "/to");
        if (!to) return std::move(to).error();
        if (s.modules.find(*to)->type != (r.midi ? model::kNotesModule : model::kInModule))
            return Error("route.endpoint", *a.to + " is not a compatible module", "/to",
                         "audio sends reach In modules; MIDI sends reach Notes modules (rack.get)");
        r.to = model::RouteEnd{};
        r.to.module = *to;
    } else if (a.to) {
        auto to = existing<model::Track>(s, *a.to, "/to");
        if (!to) return std::move(to).error();
        r.to = model::RouteEnd{};
        r.to.track = *to;
    }
    if (r.from.midiDevice) {
        if (!r.to.track || r.to.midiDevice || r.to.fx || r.to.module)
            return Error("route.endpoint", "MIDI device inputs feed channels", "/to",
                         "use track.set record_midi to choose the channel's input");
        if (a.volumeDb || a.pan || a.mono || a.phaseInvert || a.channels || a.fx)
            return Error("route.midi", "audio controls do not apply to a MIDI input", "/midi",
                         "use mute and MIDI channel routing");
        if (a.mute) r.mute = *a.mute;
        return r;
    }
    if (!r.midi && r.from.module && !r.to.midiDevice) {
        // A rack send (ADR-0075): a rack's Out into a buss or master, after
        // its decode, at the route's level and mute. A channel's input route
        // stays its input: route.set does not turn it into a send.
        if (ignored)
            for (const auto& t : s.tracks)
                if (t.value->record.input == *ignored)
                    return Error("route.in_use",
                                 ignored->str() + " is " + t.id.str() + "'s input, not a send",
                                 a.to ? "/to" : "/route",
                                 "keep " + ignored->str() + " as " + t.id.str() +
                                     "'s input; add a rack send with route.add {from: {module: \"" +
                                     r.from.module->str() + "\"}, to}");
        const auto* module = s.modules.find(*r.from.module);
        if (module->type != model::kOutModule)
            return Error("route.endpoint", r.from.module->str() + " is not an Out module", "/from",
                         "audio leaves a rack from an Out module; Note Out sends MIDI (midi: {})");
        const auto* target = r.to.track ? s.tracks.find(*r.to.track) : nullptr;
        if (!target)
            return Error("route.endpoint", "a rack's Out sends to a buss or master", "/to",
                         "give to: a buss or master (track.list)");
        if (target->type == model::StripType::Channel)
            return Error("route.strip_type", "a rack's Out feeds a channel as its input", "/to",
                         "use track.set {track, record_rack: \"" + r.from.module->str() +
                             "\"}, or send to a buss or master");
        if (a.tap || a.pan || a.mono || a.phaseInvert || a.channels)
            return Error("route.rack_send", "a rack send has level and mute only",
                         a.tap    ? "/tap"
                         : a.pan  ? "/pan"
                         : a.mono ? "/mono"
                                  : (a.phaseInvert ? "/phase_invert" : "/channels"),
                         "pan, polarity and channel routing belong in the rack or on the buss");
        if (std::any_of(r.learn.begin(), r.learn.end(),
                        [](const auto& l) { return l.param != "volume" && l.param != "mute"; }))
            return Error("route.rack_send", "a rack send can learn level and mute only", "/from",
                         "remove the route's other route.learn bindings first");
        if (auto cycle = routeCycle(s, module->rack, *r.to.track, ignored))
            return Error("route.cycle", "route creates a cycle: " + cycleText(s, *cycle, true),
                         "/to", "choose a destination that does not feed the rack");
        // A route converted from a strip send keeps no strip controls.
        r.tap = model::Tap::PostFader;
        r.pan = {};
        r.mono = r.phaseInvert = false;
        r.channels = {};
        if (a.volumeDb) r.volumeDb = *a.volumeDb;
        if (a.mute) r.mute = *a.mute;
        return r;
    }
    if (r.midi || r.from.module || r.to.midiDevice) {
        const auto* module = r.from.module ? s.modules.find(*r.from.module) : nullptr;
        const auto* source = r.from.track ? s.tracks.find(*r.from.track) : nullptr;
        const auto* targetTrack = r.to.track ? s.tracks.find(*r.to.track) : nullptr;
        const auto* targetModule = r.to.module ? s.modules.find(*r.to.module) : nullptr;
        if (!r.midi ||
            !(module ? module->type == model::kNotesOutModule
                     : source && source->type == model::StripType::Channel) ||
            !(r.to.midiDevice ||
              (targetModule ? targetModule->type == model::kNotesModule
                            : targetTrack && targetTrack->type == model::StripType::Channel)))
            return Error("route.endpoint", "MIDI sends connect channels, Note Out and Notes", "/to",
                         "send from a channel or Note Out to a channel or Notes; set midi: {}");
        if (a.volumeDb || a.pan || a.mono || a.phaseInvert || a.channels || a.fx)
            return Error("route.midi", "audio controls do not apply to a MIDI send", "/midi",
                         "use tap, mute and MIDI channel routing");
        const Id from = module ? module->rack : *r.from.track;
        const Id to = targetModule ? targetModule->rack : r.to.track.value_or(from);
        if (!r.to.midiDevice)
            if (auto cycle = routeCycle(s, from, to, ignored))
                return Error("route.cycle", "route creates a cycle: " + cycleText(s, *cycle, true),
                             "/to", "choose a destination that does not feed the source");
        if (r.to.track)
            for (Id follower : model::midiFollowers(s, *r.to.track, from))
                if (auto cycle = routeCycle(s, from, s.modules.find(follower)->rack, ignored))
                    return Error("route.cycle",
                                 "route creates a cycle: " + cycleText(s, *cycle, true), "/to",
                                 "choose a channel whose Notes readers do not feed the source");
        if (a.tap) r.tap = *a.tap;
        if (a.mute) r.mute = *a.mute;
        // Converting an audio route clears controls that have no MIDI meaning.
        r.volumeDb = 0;
        r.pan = {};
        r.mono = r.phaseInvert = false;
        r.channels = {};
        if (std::any_of(r.learn.begin(), r.learn.end(),
                        [](const auto& l) { return l.param != "mute"; }))
            return Error("route.midi", "MIDI sends can learn mute only", "/midi",
                         "remove other route.learn bindings before converting the send");
        return r;
    }
    const std::optional<Id> target = r.to.module
                                         ? std::optional<Id>(s.modules.find(*r.to.module)->rack)
                                     : r.to.fx ? model::trackOfFx(s, *r.to.fx)
                                               : r.to.track;
    if (!r.from.track || !target)
        return Error("route.endpoint", "a source track and a destination are required", "",
                     "give from and to (a track) or fx (a plug-in's key)");
    if (!r.to.fx && !r.midi && s.tracks.find(*r.from.track)->type == model::StripType::Master)
        return Error("route.strip_type", "masters are terminal", "/from",
                     "masters feed hardware outputs and renders; use a buss to group");
    if (auto cycle = routeCycle(s, *r.from.track, *target, ignored)) {
        return Error("route.cycle", "route creates a cycle: " + cycleText(s, *cycle, true), "/to",
                     "choose a destination that does not feed the source");
    }
    if (!r.to.fx && !r.midi && !r.to.module) {
        if (s.tracks.find(*target)->type == model::StripType::Channel)
            return Error("route.strip_type", "a channel accepts only hardware audio", "/to",
                         "add a buss and route the channels into it; use fx for a key");
    }
    if (a.tap) r.tap = *a.tap;
    if (a.volumeDb) r.volumeDb = *a.volumeDb;
    if (a.pan) r.pan = *a.pan;
    if (a.mute) r.mute = *a.mute;
    if (a.mono) r.mono = *a.mono;
    if (a.phaseInvert) r.phaseInvert = *a.phaseInvert;
    if (a.channels) {
        const model::ChannelMap& m = *a.channels;
        if (m.from.size() != m.to.size())
            return Error("route.channels", "channels.from and channels.to differ in length",
                         "/channels", "pair them: {from: [1, 2], to: [2, 1]}");
        // The main pair, plus a strip's auxiliary channels (ADR-0020).
        const int sourceChannels = std::max(2, s.tracks.find(*r.from.track)->channels);
        const int targetChannels =
            r.to.fx || r.to.module ? 2 : std::max(2, s.tracks.find(*target)->channels);
        const std::string more = "track.set channels adds channels 3-128";
        for (std::size_t k = 0; k < m.from.size(); ++k) {
            if (m.from[k] > sourceChannels)
                return Error("route.channels",
                             "the source has " + std::to_string(sourceChannels) + " channels",
                             "/channels/from/" + std::to_string(k), more);
            if (m.to[k] > targetChannels)
                return Error("route.channels",
                             "the destination has " + std::to_string(targetChannels) + " channels",
                             "/channels/to/" + std::to_string(k),
                             r.to.fx ? "a key takes 1 and 2; pins choose the plug-in's" : more);
        }
        const bool identity =
            m.from.empty() || (m.from == std::vector<int>{1, 2} && m.to == std::vector<int>{1, 2});
        r.channels = identity ? model::ChannelMap{} : m;
    }
    return r;
}

}  // namespace

void registerRouteCommands(cmd::Registry& registry) {
    [&] {
        Command<RouteArgs, IdResult> c;
        c.name = "route.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Send audio between strips, or MIDI between channels and rack modules.";
        c.description =
            "The destination must not feed the source. Master accepts routes. With fx instead of "
            "to, the send feeds that plug-in's sidechain (key) input — e.g. a kick keying a "
            "compressor on the bass; it is delay-compensated, and soloing the keyed track does not "
            "stop the key. MIDI sends use channel or Note Out sources and channel or Notes "
            "destinations, midi: {} "
            "(all channels unchanged); from_channel filters and to_channel remaps, 0..16 "
            "with 0 meaning all/original. to_midi selects a hardware MIDI output by exact name "
            "instead of to. Missing outputs warn while audio keeps playing. A rack's Out module "
            "(from: mo_N) sends audio to a buss or master: a rack send, added after the "
            "destination's Console 9 decode, never encoded, with level and mute only; it "
            "follows the mute and solo of the rack's home channel (ADR-0075).";
        c.tags = {"route", "mix"};
        c.args = routeArgsSchema<RouteArgs>("route_add_args");
        c.result = idResultSchema();
        c.errors = {{"route.cycle", "the send would feed back", "choose another destination"},
                    {"route.no_sidechain", "the plug-in has no key input", "pick another plug-in"},
                    {"route.endpoint", "MIDI sends need channel or note-module endpoints",
                     "use from: {module: mo_N}, to: {track: tr_N}, midi: {}"},
                    {"route.midi", "MIDI routing or audio controls do not apply to these endpoints",
                     "use channel/Note Out to channel/Notes for MIDI; omit audio controls"},
                    {"route.rack_send", "a rack send has level and mute only",
                     "pan, polarity and channel routing belong in the rack or on the buss"}};
        c.examples = {
            {"Send a track to the master", j(R"({"from":"tr_1","to":"tr_0","volume_db":"-6 dB"})"),
             j(R"({"id":"sd_1"})"), "song"},
            {"Key a plug-in from a track, before its fader",
             j(R"({"from":"tr_1","fx":"fx_1","tap":"pre_fader"})"), j(R"({"id":"sd_1"})"),
             "song_with_keymix"},
            {"Play a channel's instrument from Note Out",
             j(R"({"from":{"module":"mo_4"},"to":{"track":"tr_2"},"midi":{}})"),
             j(R"({"id":"sd_2"})"), "song_with_note_out"},
            {"Send a rack's Out to a buss",
             j(R"({"from":{"module":"mo_1"},"to":"tr_2","volume_db":"-6 dB"})"),
             j(R"({"id":"sd_2"})"), "song_with_rack_buss"}};
        c.run = [](const RouteArgs& a, Context& ctx) -> Result<IdResult> {
            auto r = buildRoute(a, app(ctx).view(), app(ctx));
            if (!r) return std::move(r).error();
            return IdResult{app(ctx).txn().create(std::move(*r)).str()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<RouteSetArgs, Empty> c;
        c.name = "route.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Change a route's endpoints, tap, level, pan, mute, or channel map.";
        c.tags = {"route", "mix"};
        c.args = routeArgsSchema<RouteSetArgs>("route_set_args");
        c.result = emptySchema();
        c.examples = {
            {"Mute a route", j(R"({"route":"sd_1","mute":true})"), j("{}"), "song_with_route"},
            {"Swap left and right", j(R"({"route":"sd_1","channels":{"from":[1,2],"to":[2,1]}})"),
             j("{}"), "song_with_route"}};
        c.errors = {{"route.channels", "the map pairs channels the strips do not have",
                     "pair 1 and 2: {from: [1, 2], to: [2, 1]}"},
                    {"route.endpoint", "MIDI sends need channel or note-module endpoints",
                     "send channel/Note Out MIDI to a channel or Notes module"},
                    {"route.midi", "audio controls do not apply to MIDI routes",
                     "use mute and MIDI channel routing"},
                    {"route.rack_send", "a rack send has level and mute only",
                     "pan, polarity and channel routing belong in the rack or on the buss"},
                    {"route.in_use", "sd_1 is tr_1's input, not a send",
                     "keep sd_1 as tr_1's input; add a rack send with route.add"}};
        c.run = [](const RouteSetArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = existing<model::Route>(s, a.route, "/route");
            if (!id) return std::move(id).error();
            RouteArgs edits{a.from.value_or(""), a.to,       a.fx,   a.tap,
                            a.volumeDb,          a.pan,      a.mute, a.mono,
                            a.phaseInvert,       a.channels, a.midi, a.toMidi};
            auto r = buildRoute(edits, s, app(ctx), s.routes.find(*id), *id);
            if (!r) return std::move(r).error();
            app(ctx).txn().put<model::Route>(*id, std::move(*r));
            // Touch/latch/write envelopes record the change while playing.
            if (a.volumeDb) app(ctx).recordAutomation(*id, "volume", *a.volumeDb);
            if (a.pan && a.pan->percent) app(ctx).recordAutomation(*id, "pan", *a.pan->percent);
            if (a.mute) app(ctx).recordAutomation(*id, "mute", *a.mute ? 1.0 : 0.0);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<RouteRef, Empty> c;
        c.name = "route.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove a send.";
        c.tags = {"route"};
        c.args = schema::object<RouteRef>("route_ref")
                     .field("route", &RouteRef::route, idArg("sd", "Route"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"route":"sd_1"})"), j("{}"), "song_with_route"}};
        c.run = [](const RouteRef& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = existing<model::Route>(s, a.route, "/route");
            if (!id) return std::move(id).error();
            for (const auto& e : s.tracks)
                if (e.value->record.input == *id || e.value->record.midiInput == *id)
                    return Error("route.in_use", a.route + " is a record input on " + e.id.str(),
                                 "/route", "change that track's record input first");
            auto& t = app(ctx).txn();
            for (const auto& e : s.envelopes)
                if (e.value->target.object == *id) t.remove<model::Envelope>(e.id);
            t.remove<model::Route>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<cmd::NoArgs, RouteList> c;
        c.name = "route.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List sends with endpoints and mixer settings.";
        c.tags = {"route"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        auto entry = schema::object<RouteEntry>("route_entry")
                         .field("id", &RouteEntry::id, schema::string())
                         .field("from", &RouteEntry::from, schema::string())
                         .field("to", &RouteEntry::to, schema::string())
                         .field("fx", &RouteEntry::fx, schema::string())
                         .field("tap", &RouteEntry::tap, schema::string())
                         .field("volume_db", &RouteEntry::volumeDb, schema::number())
                         .field("pan", &RouteEntry::pan, model::routePanSchema())
                         .field("mute", &RouteEntry::mute, schema::boolean())
                         .field("mono", &RouteEntry::mono, schema::boolean())
                         .field("phase_invert", &RouteEntry::phaseInvert, schema::boolean())
                         .field("channels", &RouteEntry::channels, channelMapSchema())
                         .field("midi", &RouteEntry::midi, midiRouteArgs())
                         .field("from_midi", &RouteEntry::fromMidi, schema::string())
                         .field("to_midi", &RouteEntry::toMidi, schema::string())
                         .build();
        c.result = schema::object<RouteList>("route_list")
                       .field("routes", &RouteList::routes, schema::arrayOf(entry))
                       .build();
        c.examples = {{"No sends yet", j("{}"), j(R"({"routes":[]})"), "empty"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<RouteList> {
            RouteList out;
            const auto view = app(ctx).view();
            for (const auto& e : view.routes) {
                const auto& r = *e.value;
                const auto target = r.to.module ? r.to.module
                                    : r.to.fx   ? model::trackOfFx(view, *r.to.fx)
                                                : r.to.track;
                const auto source = r.from.module ? r.from.module : r.from.track;
                out.routes.push_back({e.id.str(), source ? source->str() : "",
                                      target ? target->str() : "", r.to.fx ? r.to.fx->str() : "",
                                      r.tap == model::Tap::PostFader  ? "post_fader"
                                      : r.tap == model::Tap::PreFader ? "pre_fader"
                                                                      : "pre_fx",
                                      r.volumeDb, r.pan, r.mute, r.mono, r.phaseInvert, r.channels,
                                      r.midi, r.from.midiDevice, r.to.midiDevice});
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<RouteMatrixArgs, json::Value> c;
        c.name = "route.matrix";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Read audio or MIDI endpoints, existing sends and blocked pairs.";
        c.description =
            "mode: audio (default), midi or hardware_midi (channels/Note Out and hardware inputs "
            "to channels/hardware outputs). MIDI sources are channels and Note Out; destinations "
            "are "
            "channels and Notes, with no audio controls. Audio sources: strips in tree order "
            "(depth, parent, parent_send). destinations: strips "
            "(kind track), plug-in keys (kind key: fx with a sidechain input) and racks' In "
            "modules (kind module: mo_N, named \"<rack> \u00bb <In>\", with rack and home "
            "track), each strip's keys and In modules after it. Each channel's rack Out "
            "modules follow it among sources (type rack_out, named \"<rack> \u00bb <Out>\", "
            "with rack and home track): they send only to busses and masters (their rule), "
            "added after the decode, never encoded (ADR-0075). A pair is blocked when the "
            "source's rule applies (masters feed only keys), the destination's rule applies "
            "(channels take only hardware audio), it is the same strip, or it is listed in "
            "cycles (the destination already feeds the source; for an In module, its rack's Outs "
            "reach the source; path names its ids, names the same by name). sends: existing routes "
            "per pair (to: a strip, an fx or an In "
            "module). Otherwise route.add accepts the pair (an In module: route.add {from, to: "
            "mo_N}). A source's channels past 2 are auxiliary (ADR-0020); "
            "channel_names names those a multi-out plug-in writes ({\"3\": \"Kick\"}), and "
            "route.add {channels: {from: [3, 4], to: [1, 2]}} sends that pair.";
        c.tags = {"route"};
        auto matrixArgs = schema::object<RouteMatrixArgs>("route_matrix_args");
        matrixArgs.field("mode", &RouteMatrixArgs::mode,
                         schema::string().pattern("^(audio|midi|hardware_midi)$",
                                                  "audio, midi or hardware_midi"));
        matrixArgs.defaultValue(std::string("audio"));
        c.args = matrixArgs.build();
        c.result = schema::any();
        c.examples = {
            {"A song's routing", j("{}"),
             j(R"({"sources":"<any>","destinations":"<any>","sends":"<any>","cycles":"<any>"})"),
             "song"}};
        c.run = [](const RouteMatrixArgs& args, Context& ctx) -> Result<json::Value> {
            const bool hardwareMode = args.mode == "hardware_midi";
            const bool midiMode = hardwareMode || args.mode == "midi";
            EngineApp& a = app(ctx);
            const model::Session s = a.view();
            auto rule = [](const char* code, const char* message, const char* hint) {
                json::Value r = json::Value::object();
                r["code"] = code;
                r["message"] = message;
                r["hint"] = hint;
                return r;
            };
            // Tracks in the order the views show them: each master's tree
            // (under a parent, its channels before its busses), then the
            // masters themselves.
            std::vector<std::pair<Id, int>> order;
            std::function<void(Id, int)> walk = [&](Id id, int depth) {
                const model::Track* t = s.tracks.find(id);
                if (!t) return;
                if (t->type != model::StripType::Master) order.emplace_back(id, depth);
                const int below = t->type == model::StripType::Master ? depth : depth + 1;
                for (model::StripType type : {model::StripType::Channel, model::StripType::Buss})
                    for (Id child : t->children)
                        if (const model::Track* kid = s.tracks.find(child);
                            kid && kid->type == type)
                            walk(child, below);
            };
            for (Id m : s.project->masters) walk(m, 0);
            for (Id m : s.project->masters) order.emplace_back(m, 0);
            json::Value sources = json::Value::array(), destinations = json::Value::array();
            for (const auto& [id, depth] : order) {
                const model::Track& t = *s.tracks.find(id);
                if (midiMode && t.type != model::StripType::Channel) continue;
                json::Value src = json::Value::object();
                src["id"] = id.str();
                src["name"] = model::trackLabel(s, id);
                src["type"] = std::string(model::stripTypeName(t.type));
                src["depth"] = depth;
                if (t.parent && !midiMode) {
                    src["parent"] = t.parent->str();
                    src["parent_send"] = t.parentSend.enabled;
                }
                src["channels"] = midiMode ? 2 : t.channels;
                // The auxiliary channels a multi-out plug-in writes, by its
                // ports' names (ADR-0021); the first plug-in to name one wins.
                json::Value names = json::Value::object();
                for (Id fx : t.chain) {
                    const model::FxInstance* inst = s.fx.find(fx);
                    if (!inst || inst->pins.out.size() <= 2) continue;
                    auto ports = a.fxAuxOutputs(*inst);
                    if (!ports) continue;
                    std::size_t k = 2;
                    for (const host::AudioPortInfo& port : *ports)
                        for (std::uint32_t ch = 0; ch < port.channels; ++ch, ++k) {
                            if (k >= inst->pins.out.size()) break;
                            const int channel = inst->pins.out[k];
                            if (channel < 3 || channel > t.channels) continue;
                            const std::string key = std::to_string(channel);
                            if (!names.find(key) && !port.name.empty()) names[key] = port.name;
                        }
                }
                if (!names.asObject().empty()) src["channel_names"] = std::move(names);
                if (t.type == model::StripType::Master)
                    src["rule"] = rule(
                        "route.strip_type", "masters are terminal (they may feed plug-in keys)",
                        "masters feed hardware outputs and renders; use a buss to group");
                sources.push_back(src);
                // A rack's Out modules under its home channel: rack sends to
                // busses and masters (ADR-0075).
                if (!midiMode)
                    for (const auto& module : s.modules) {
                        const auto* rack = s.racks.find(module.value->rack);
                        if (!rack || rack->track != id || module.value->type != model::kOutModule)
                            continue;
                        auto row = json::Value::object();
                        row["id"] = module.id.str();
                        row["name"] = model::rackLabel(s, module.value->rack) + " \u00bb " +
                                      model::moduleLabel(s, module.id);
                        row["type"] = "rack_out";
                        row["rack"] = module.value->rack.str();
                        row["track"] = id.str();
                        row["depth"] = depth + 1;
                        row["channels"] = 2;
                        row["rule"] =
                            rule("route.strip_type", "a rack's Out sends to busses and masters",
                                 "a channel takes it as its input (track.set record_rack)");
                        sources.push_back(std::move(row));
                    }
                if (midiMode)
                    for (const auto& module : s.modules) {
                        const auto* rack = s.racks.find(module.value->rack);
                        if (!rack || rack->track != id ||
                            module.value->type != model::kNotesOutModule)
                            continue;
                        auto row = json::Value::object();
                        row["id"] = module.id.str();
                        row["name"] = model::rackLabel(s, module.value->rack) + " » " +
                                      midiModuleLabel(s, module.id);
                        row["type"] = "note_out";
                        row["depth"] = depth + 1;
                        row["channels"] = 2;
                        sources.push_back(std::move(row));
                    }
                json::Value dst = json::Value::object();
                dst["id"] = id.str();
                dst["name"] = model::trackLabel(s, id);
                dst["kind"] = "track";
                dst["type"] = std::string(model::stripTypeName(t.type));
                if (!midiMode && t.type == model::StripType::Channel)
                    dst["rule"] =
                        rule("route.strip_type", "a channel accepts only hardware audio",
                             "add a buss and route the channels into it; use fx for a key");
                destinations.push_back(std::move(dst));
                for (Id fx : t.chain) {
                    if (midiMode) break;
                    const model::FxInstance* inst = s.fx.find(fx);
                    if (!inst) continue;
                    auto key = a.fxSidechainChannels(*inst);
                    if (!key || *key == 0) continue;
                    json::Value k = json::Value::object();
                    k["id"] = fx.str();
                    k["name"] = (inst->label.empty() ? inst->plugin.name : inst->label) + " (key)";
                    k["kind"] = "key";
                    k["track"] = id.str();
                    destinations.push_back(std::move(k));
                }
                // The In modules of the racks homed here, in id order: a send
                // reaches a rack through them (rack.md 2.4).
                for (const auto& rack : s.racks) {
                    if (rack.value->track != id || hardwareMode) continue;
                    for (const auto& m : s.modules) {
                        if (m.value->rack != rack.id ||
                            m.value->type != (midiMode ? model::kNotesModule : model::kInModule))
                            continue;
                        json::Value in = json::Value::object();
                        in["id"] = m.id.str();
                        in["name"] =
                            model::rackLabel(s, rack.id) + " \u00bb " +
                            (midiMode ? midiModuleLabel(s, m.id) : model::moduleLabel(s, m.id));
                        in["kind"] = "module";
                        in["rack"] = rack.id.str();
                        in["track"] = id.str();
                        destinations.push_back(std::move(in));
                    }
                }
            }
            json::Value sends = json::Value::array();
            for (const auto& e : s.routes) {
                const model::Route& r = *e.value;
                const bool rackSend = !midiMode && model::rackSendHome(s, r).has_value();
                if (bool(r.midi) != midiMode ||
                    (!r.from.track && !(midiMode && r.from.module) && !rackSend &&
                     !(hardwareMode && r.from.midiDevice)) ||
                    (!hardwareMode && r.to.midiDevice))
                    continue;
                json::Value v = json::Value::object();
                v["id"] = e.id.str();
                v["from"] = r.from.track    ? r.from.track->str()
                            : r.from.module ? r.from.module->str()
                                            : "midi_in:" + *r.from.midiDevice;
                v["to"] = r.to.fx           ? r.to.fx->str()
                          : r.to.module     ? r.to.module->str()
                          : r.to.track      ? r.to.track->str()
                          : r.to.midiDevice ? "midi_out:" + *r.to.midiDevice
                                            : "";
                sends.push_back(std::move(v));
            }
            // A route from -> d feeds back when d (the key's track, the In
            // module's rack) already reaches from: listed per pair, with the
            // path, as route.add finds it (routeCycle).
            json::Value cycles = json::Value::array();
            for (const json::Value& d : destinations.asArray()) {
                const std::string did = d.find("id")->asString();
                const std::string kind = d.find("kind")->asString();
                const bool key = kind == "key";
                const Id target = *Id::parse(key                ? d.find("track")->asString()
                                             : kind == "module" ? d.find("rack")->asString()
                                                                : did);
                if (!midiMode && kind == "track" &&
                    s.tracks.find(target)->type == model::StripType::Channel)
                    continue;
                for (const auto& row : sources.asArray()) {
                    const auto fromId = *Id::parse(row.find("id")->asString());
                    const Id from =
                        fromId.kind == model::Kind::Module ? s.modules.find(fromId)->rack : fromId;
                    if (!midiMode && from == target) continue;
                    if (!key && from.kind == model::Kind::Track &&
                        s.tracks.find(from)->type == model::StripType::Master)
                        continue;
                    auto path = routeCycle(s, from, target);
                    if (!path && midiMode && kind == "track")
                        for (Id follower : model::midiFollowers(s, target, from)) {
                            path = routeCycle(s, from, s.modules.find(follower)->rack);
                            if (path) break;
                        }
                    if (path) {
                        json::Value v = json::Value::object();
                        v["from"] = fromId.str();
                        v["to"] = did;
                        std::string ids;
                        for (Id at : *path) ids += (ids.empty() ? "" : " -> ") + at.str();
                        v["path"] = ids;
                        v["names"] = cycleText(s, *path, false);
                        cycles.push_back(std::move(v));
                    }
                }
            }
            json::Value out = json::Value::object();
            if (hardwareMode) {
                auto rows = json::Value::array();
                const auto inputs = a.midiPortsState(false), outputs = a.midiPortsState(true);
                for (const auto& port : inputs.asArray()) {
                    auto row = json::Value::object();
                    row["id"] = "midi_in:" + port.find("name")->asString();
                    row["name"] = port.find("name")->asString();
                    row["type"] = "midi_input";
                    row["depth"] = 0;
                    row["channels"] = 2;
                    rows.push_back(std::move(row));
                }
                for (const auto& row : sources.asArray()) rows.push_back(row);
                sources = std::move(rows);
                for (const auto& port : outputs.asArray()) {
                    auto col = json::Value::object();
                    col["id"] = "midi_out:" + port.find("name")->asString();
                    col["name"] = port.find("name")->asString();
                    col["kind"] = "midi_output";
                    // Sends clock now (ADR-0073): set, enabled, connected and not
                    // the sync master's own device.
                    bool clock = false;
                    if (const auto* kinds = port.find("sends"))
                        for (const auto& kind : kinds->asArray())
                            clock = clock || kind.asString() == "clock";
                    col["clock"] = clock;
                    destinations.push_back(std::move(col));
                }
            }
            if (hardwareMode) {
                auto resolved = json::Value::array();
                for (auto send : sends.asArray()) {
                    const auto* route = s.routes.find(*Id::parse(send.find("id")->asString()));
                    if (route->from.midiDevice) {
                        for (const auto& source : sources.asArray())
                            if (source.find("type")->asString() == "midi_input" &&
                                midiPortMask({source.find("name")->asString()},
                                             *route->from.midiDevice)) {
                                auto input = send;
                                input["from"] = *source.find("id");
                                resolved.push_back(std::move(input));
                            }
                        continue;
                    }
                    if (route->to.midiDevice)
                        for (const auto& dest : destinations.asArray())
                            if (dest.find("kind")->asString() == "midi_output") {
                                const auto& name = dest.find("name")->asString();
                                const auto& wanted = *route->to.midiDevice;
                                if (name.size() == wanted.size() &&
                                    std::equal(name.begin(), name.end(), wanted.begin(),
                                               [](unsigned char x, unsigned char y) {
                                                   return std::tolower(x) == std::tolower(y);
                                               }))
                                    send["to"] = *dest.find("id");
                            }
                    resolved.push_back(std::move(send));
                }
                sends = std::move(resolved);
            }
            out["sources"] = std::move(sources);
            out["destinations"] = std::move(destinations);
            out["sends"] = std::move(sends);
            out["cycles"] = std::move(cycles);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
