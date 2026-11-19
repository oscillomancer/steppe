// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Track commands: track.add, track.remove, track.set, track.list and track.move.
#include "command_helpers.h"

namespace supple::engine {

namespace {

// Whether `track`'s signal reaches `rack` (through parent sends, routes and
// other racks): it may not then take that rack's Out as input (rack.cycle).
bool trackFeedsRack(const model::Session& s, Id track, Id rack) {
    std::set<Id> seen{track};
    std::vector<Id> todo{track};
    auto rackOf = [&](Id module) -> std::optional<Id> {
        const model::RackModule* m = s.modules.find(module);
        return m ? std::optional<Id>(m->rack) : std::nullopt;
    };
    while (!todo.empty()) {
        const Id at = todo.back();
        todo.pop_back();
        if (at == rack) return true;
        std::vector<Id> next;
        if (const model::Track* t = s.tracks.find(at); t && t->parent && t->parentSend.enabled)
            next.push_back(*t->parent);
        for (const auto& e : s.routes) {
            const model::Route& r = *e.value;
            const std::optional<Id> from = r.from.module ? rackOf(*r.from.module) : r.from.track;
            if (from != at) continue;
            const std::optional<Id> to = r.to.module ? rackOf(*r.to.module)
                                         : r.to.fx   ? model::trackOfFx(s, *r.to.fx)
                                                     : r.to.track;
            if (to) next.push_back(*to);
        }
        for (Id n : next)
            if (seen.insert(n).second) todo.push_back(n);
    }
    return false;
}

// --- tracks -----------------------------------------------------------------------------

struct TrackAddArgs {
    model::StripType type = model::StripType::Channel;
    std::optional<std::string> name, parent, color;
    std::optional<std::int64_t> index;
    std::optional<std::int64_t> channels;
};

struct TrackRef {
    std::string track;
};

struct TrackSetArgs {
    std::string track;
    bool bypassGroups = false;
    std::optional<std::string> name, color, notes;
    std::optional<double> volumeDb, pan;
    std::optional<model::PanMode> panMode;
    std::optional<double> width, panLeft, panRight;
    std::optional<json::Value> panLaw;  // {db, boost}, or null: the project's
    std::optional<model::StripType> type;
    std::optional<bool> mute, phaseInvert, mono;
    std::optional<std::string> solo;
    std::optional<bool> armed;
    std::optional<json::Value> recordInput;  // channels, or null: no hardware audio
    std::optional<json::Value> recordMidi;
    std::optional<json::Value> recordRack;  // an Out module (ADR-0062), or null: none
    std::optional<std::int64_t> midiChannel;
    std::optional<model::RecordMode> recordMode;
    std::optional<model::Monitor> monitor;
    std::optional<bool> parentSend;
    std::optional<model::TrackTimebase> timebase;
    std::optional<std::int64_t> channels;
    std::optional<std::vector<std::int64_t>> hwOutput;
    std::optional<bool> laneMute;
    std::optional<model::ChainMidi> chainMidi;
};

struct TrackEntry {
    bool frozen = false;
    std::optional<model::TrackFreeze> freeze;
    std::string id, name, parent;
    model::StripType type = model::StripType::Channel;
    std::int64_t depth = 0;
    double volumeDb = 0;
    bool mute = false;
    std::string solo;
    std::vector<std::string> fx, items;
    std::vector<Id> groups, vcas;
    std::optional<std::vector<std::int64_t>> hwOutput;  // masters
    std::optional<bool> armed, laneMute;                // masters (ADR-0057)
    double requestedEffective = 0, effective = 0, at = 0;
    bool effectiveMute = false, soloSilenced = false;
};

struct TrackListArgs {
    std::optional<double> at;
};

struct TrackList {
    std::vector<TrackEntry> tracks;
};

struct TrackMoveArgs {
    std::string track;
    std::optional<std::string> parent;
    std::optional<std::int64_t> index;
};

// Is `track` a descendant of `ancestor` in the folder nesting (children lists)?
// Moving a track under one of its own descendants would make the tree cyclic.
bool nestedUnder(const model::Session& s, Id ancestor, Id track) {
    std::vector<Id> stack{ancestor};
    std::set<Id> seen;
    while (!stack.empty()) {
        const Id cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        if (cur == track) return true;
        const model::Track* found = s.tracks.find(cur);
        if (!found) continue;
        for (Id child : found->children) stack.push_back(child);
    }
    return false;
}

}  // namespace

void registerTrackCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- track.add
    [&] {
        Command<TrackAddArgs, TrackRef> c;
        c.name = "track.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a track (a channel, buss or master) and return its id.";
        c.description =
            "parent: a buss/master id (default tr_0); masters have no parent; index: position "
            "among its siblings; name: default the first free ch_N, buss_N or mstr_N. A new "
            "channel gets the user's default monitoring (on unless set otherwise; monitoring "
            "needs the track armed). color: a track colour's name or \"#rrggbb\", \"\" for none; "
            "default for a channel or buss the palette colour the fewest tracks carry (red "
            "first), for a master none.";
        c.tags = {"track"};
        c.args = schema::object<TrackAddArgs>("track_add_args")
                     .field("type", &TrackAddArgs::type, model::stripTypeSchema())
                     .defaultValue(model::StripType::Channel)
                     .field("name", &TrackAddArgs::name, schema::string())
                     .field("color", &TrackAddArgs::color, schema::string())
                     .field("parent", &TrackAddArgs::parent, idArg("tr", "Parent track"))
                     .field("index", &TrackAddArgs::index, schema::integer().min(0))
                     .field("channels", &TrackAddArgs::channels,
                            schema::integer().range(1, model::kMaxTrackChannels))
                     .build();
        c.result = schema::object<TrackRef>("track_ref")
                       .field("track", &TrackRef::track, schema::string())
                       .build();
        c.examples = {
            {"Add a bass track", j(R"({"name": "Bass"})"), j(R"({"track": "tr_1"})"), "empty"},
            {"Add a green buss for the drums",
             j(R"({"name": "Drums", "type": "buss", "color": "green"})"), j(R"({"track": "tr_1"})"),
             "empty"}};
        c.run = [](const TrackAddArgs& a, Context& ctx) -> Result<TrackRef> {
            model::EditTxn& t = app(ctx).txn();
            Id parent = model::kMaster;
            if (a.parent) {
                auto p = Id::parse(*a.parent);
                if (!p || !t.exists<model::Track>(*p))
                    return Error("tr.unknown", "no track " + *a.parent, "/parent",
                                 "see track.list");
                parent = *p;
            }
            if (a.type == model::StripType::Master && a.parent)
                return Error("track.strip_type", "a master has no parent", "/parent",
                             "omit parent when adding a master");
            if (a.type != model::StripType::Master &&
                t.get<model::Track>(parent)->type == model::StripType::Channel)
                return Error("track.strip_type", "a channel cannot have children", "/parent",
                             "add a buss and place the channels under it");
            if (a.type != model::StripType::Channel && a.channels.value_or(2) < 2)
                return Error("track.channels", "busses and masters require at least two channels",
                             "/channels", "use 2..128 channels");
            if (a.color && !model::validTrackColor(*a.color))
                return Error("track.color", "unknown colour \"" + *a.color + "\"", "/color",
                             "red, orange, yellow, green, teal, blue, violet, grey or \"#rrggbb\"; "
                             "\"\" for none; omit it for the next palette colour");
            model::Track tr;
            tr.type = a.type;
            tr.name = a.name.value_or("");
            if (tr.name.empty()) tr.name = model::nextTrackName(t.preview(), a.type);
            // A colour of its own for every new channel and buss, so its items
            // and strip can be told from the others' at a glance (gui.md §7).
            if (a.color)
                tr.color = *a.color;
            else if (a.type != model::StripType::Master)
                tr.color = model::nextTrackColor(t.preview());
            if (a.type == model::StripType::Channel) tr.record.monitor = app(ctx).defaultMonitor();
            if (a.type != model::StripType::Master) tr.parent = parent;
            if (a.channels) tr.channels = static_cast<int>(*a.channels);
            const Id id = t.create(tr);
            if (tr.type == model::StripType::Master) {
                auto p = t.project();
                p.masters.push_back(id);
                t.setProject(std::move(p));
            } else
                t.modify<model::Track>(parent, [&](model::Track& p) {
                    const auto at = a.index
                                        ? std::min<std::size_t>(static_cast<std::size_t>(*a.index),
                                                                p.children.size())
                                        : p.children.size();
                    p.children.insert(p.children.begin() + static_cast<std::ptrdiff_t>(at), id);
                });
            return TrackRef{id.str()};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.remove
    [&] {
        Command<TrackRef, Empty> c;
        c.name = "track.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a track with its items, fx, routes and envelopes (not a folder).";
        c.description =
            "Removes the track's racks too, except a rack another channel hears (its Out "
            "as the channel's input, or its Note Out's MIDI), which moves there. A warning "
            "names each rack removed (track.rack_removed) or moved (track.rack_moved) and "
            "its rack sends: removed with it, or following the new home's mute and solo.";
        c.tags = {"track"};
        c.args = schema::object<TrackRef>("track_ref")
                     .field("track", &TrackRef::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"track": "tr_1"})"), j("{}"), "song"}};
        c.run = [](const TrackRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            model::EditTxn& t = app.txn();
            const model::Session s = app.view();
            auto idr = existing<model::Track>(s, a.track, "/track");
            if (!idr) return std::move(idr).error();
            const Id id = *idr;
            if (id == model::kMaster)
                return Error("track.strip_type", "tr_0 cannot be removed", "/track",
                             "keep the main master; remove another empty track instead");
            const model::Track& tr = *s.tracks.find(id);
            if (!tr.children.empty())
                return Error(
                    "tr.has_children",
                    a.track + " is a folder with " + std::to_string(tr.children.size()) + " tracks",
                    "/track", "move or remove its children first");
            std::set<Id> gone(tr.chain.begin(), tr.chain.end());
            gone.insert(tr.inputChain.begin(), tr.inputChain.end());
            gone.insert(id);
            for (const auto& e : s.items)
                if (e.value->track == id) {
                    for (Id tk : e.value->takes) {
                        const model::Take* take = s.takes.find(tk);
                        if (take)
                            for (Id f : take->chain) {
                                t.remove<model::FxInstance>(f);
                                gone.insert(f);
                            }
                        t.remove<model::Take>(tk);
                        gone.insert(tk);  // its envelopes go too
                    }
                    t.remove<model::Item>(e.id);
                }
            for (Id f : tr.chain) t.remove<model::FxInstance>(f);
            for (Id f : tr.inputChain) t.remove<model::FxInstance>(f);
            for (const auto& e : s.routes)
                if ((e.value->from.track && *e.value->from.track == id) ||
                    (e.value->to.track && *e.value->to.track == id) ||
                    (e.value->to.fx && std::count(tr.chain.begin(), tr.chain.end(),
                                                  *e.value->to.fx))) {  // keys into its plug-ins
                    gone.insert(e.id);
                    t.remove<model::Route>(e.id);
                }
            for (const auto& e : s.envelopes)
                if (gone.count(e.value->target.object)) t.remove<model::Envelope>(e.id);
            // Its racks (ADR-0062), and Notes modules elsewhere stop playing it.
            // Each rack it takes or moves is reported, with its sends (ADR-0075).
            for (const auto& w : removeRacksOn(t, s, id)) ctx.warn(w.code, w.message, w.path);
            for (const auto& e : s.modules)
                if (e.value->source == id)
                    t.modify<model::RackModule>(e.id,
                                                [](model::RackModule& m) { m.source.reset(); });
            if (!tr.parent) {
                auto p = t.project();
                std::erase(p.masters, id);
                t.setProject(std::move(p));
            } else
                t.modify<model::Track>(*tr.parent, [&](model::Track& p) {
                    p.children.erase(std::remove(p.children.begin(), p.children.end(), id),
                                     p.children.end());
                });
            for (const auto& g : s.groups)
                if (std::find(g.value->members.begin(), g.value->members.end(), id) !=
                    g.value->members.end())
                    t.modify<model::TrackGroup>(
                        g.id, [&](auto& group) { std::erase(group.members, id); });
            t.remove<model::Track>(id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.set
    [&] {
        Command<TrackSetArgs, Empty> c;
        c.name = "track.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set track fields; choosing audio, MIDI or rack input replaces the others.";
        c.description =
            "Only the given fields change. solo: off | on | in_place | defeat. armed + "
            "record_input (device input channels, 1-based) prepare a track for "
            "transport.record; record_midi records MIDI instead: hardware audio "
            "and MIDI or a rack Out (record_rack) replace each other: a track has one input. "
            "Choosing one clears the other kinds. null clears only the named kind; set all "
            "three to null for no input. Feed a rack's Notes through MIDI sends from another "
            "channel. A device name is a "
            "substring of "
            "the MIDI port (\"*\" = every device), with midi_channel 1-16 (0 = all). monitor: "
            "off | on | auto (auto: while armed) — a monitored track hears its input live (an "
            "instrument plays the keyboard), also while stopped. record_mode: input (new "
            "items), midi_overdub (merge into the MIDI item under the start, as a new take), "
            "midi_replace. color: red | orange | yellow | green | teal | blue | violet | grey | "
            "\"#rrggbb\" (\"\" clears it), shown as a stripe on the track. A master takes "
            "armed alone: transport.record then records its output (after its fader, before "
            "its own items) as an item on the master; lane_mute silences a master's items. "
            "chain_midi: bus (every plug-in with a note input hears the chain's MIDI, a "
            "note-output plug-in feeds those after it and the post-FX sends) or instrument "
            "(only the first such plug-in hears the track's MIDI, as before 2.10).";
        c.tags = {"track", "mixer"};
        c.args =
            schema::object<TrackSetArgs>("track_set_args")
                .field("bypass_groups", &TrackSetArgs::bypassGroups, schema::boolean())
                .defaultValue(false)
                .field("track", &TrackSetArgs::track, idArg("tr", "Track"))
                .field("type", &TrackSetArgs::type, model::stripTypeSchema())
                .field("name", &TrackSetArgs::name, schema::string())
                .field("color", &TrackSetArgs::color, schema::string())
                .field("notes", &TrackSetArgs::notes, schema::string())
                .field("volume_db", &TrackSetArgs::volumeDb,
                       schema::number().range(model::kSilenceDb, 12).unit(units::Unit::dB))
                .field("pan", &TrackSetArgs::pan,
                       schema::number().range(-100, 100).unit(units::Unit::percent))
                .field("pan_mode", &TrackSetArgs::panMode,
                       model::panModeSchema().doc(
                           "Channels and busses: pan (balance a stereo image, pan a mono one), "
                           "dual (pan_left and pan_right place each input on its own) or width "
                           "(width narrows, widens or swaps the image, pan places it)"))
                .field("width", &TrackSetArgs::width,
                       schema::number()
                           .range(-100, 100)
                           .unit(units::Unit::percent)
                           .doc("Width mode: 100 as recorded, 0 mono, -100 sides swapped"))
                .field("pan_left", &TrackSetArgs::panLeft,
                       schema::number()
                           .range(-100, 100)
                           .unit(units::Unit::percent)
                           .doc("Dual mode: where the left input goes (default -100)"))
                .field("pan_right", &TrackSetArgs::panRight,
                       schema::number()
                           .range(-100, 100)
                           .unit(units::Unit::percent)
                           .doc("Dual mode: where the right input goes (default 100)"))
                .field("pan_law", &TrackSetArgs::panLaw,
                       clearable("clearable_pan_law",
                                 model::panLawSchema().doc(
                                     "This track's pan law: db is the centre's cut (0, -2.5, "
                                     "-3, -4.5 or -6); boost raises the sides instead. null "
                                     "follows project.pan_law")))
                .field("mute", &TrackSetArgs::mute, schema::boolean())
                .field("phase_invert", &TrackSetArgs::phaseInvert,
                       schema::boolean().doc("Flip the polarity of the main pair (after the FX "
                                             "chain, before the fader)"))
                .field("mono", &TrackSetArgs::mono,
                       schema::boolean().doc("Sum the main pair to mono (after the FX chain, "
                                             "before the fader and pan): a mono check on the "
                                             "master"))
                .field("solo", &TrackSetArgs::solo,
                       schema::string().pattern("^(off|on|in_place|defeat)$",
                                                "off, on, in_place or defeat"))
                .field("armed", &TrackSetArgs::armed, schema::boolean())
                .field(
                    "record_input", &TrackSetArgs::recordInput,
                    clearable("clearable_channels", schema::arrayOf(schema::integer().range(1, 64)))
                        .doc("Hardware audio input; replaces MIDI or rack input. null clears "
                             "hardware audio only"))
                .field("record_midi", &TrackSetArgs::recordMidi,
                       clearableString(schema::string())
                           .doc("MIDI device; replaces hardware audio or rack input. null clears "
                                "MIDI only"))
                .field("record_rack", &TrackSetArgs::recordRack,
                       clearableString(
                           idArg("mo", "Out module")
                               .doc("A rack's Out module as the channel's input: the rack plays "
                                    "on the channel, replacing hardware audio or MIDI (rack.list "
                                    "lists them); "
                                    "null takes a rack's Out off the input")))
                .field("midi_channel", &TrackSetArgs::midiChannel, schema::integer().range(0, 16))
                .field("record_mode", &TrackSetArgs::recordMode,
                       schema::enumeration<model::RecordMode>(
                           {{model::RecordMode::Input, "input"},
                            {model::RecordMode::MidiOverdub, "midi_overdub"},
                            {model::RecordMode::MidiReplace, "midi_replace"}}))
                .field("monitor", &TrackSetArgs::monitor,
                       schema::enumeration<model::Monitor>({{model::Monitor::Off, "off"},
                                                            {model::Monitor::On, "on"},
                                                            {model::Monitor::Auto, "auto"}}))
                .field("parent_send", &TrackSetArgs::parentSend,
                       schema::boolean().doc("The implicit send to the parent buss/master "
                                             "(off: only explicit sends leave the track)"))
                .field("timebase", &TrackSetArgs::timebase,
                       model::trackTimebaseSchema().doc(
                           "What this channel's items follow when the tempo changes (those "
                           "that inherit it): project, beats, time or beats_position; they keep "
                           "their place now"))
                .field(
                    "channels", &TrackSetArgs::channels,
                    schema::integer()
                        .range(1, model::kMaxTrackChannels)
                        .doc("1-2: the track's main pair; up to 128 adds auxiliary channels 3-128 "
                             "for routes and plug-in pins (a detector on 3/4, a multi-out "
                             "instrument's outputs), outside the fader, pan and parent send"))
                .field("hw_output", &TrackSetArgs::hwOutput,
                       schema::arrayOf(schema::integer().range(1, model::kMaxDeviceOutputs))
                           .doc("Masters: the device outputs the main pair plays to: [l, r], "
                                "[mono] or [] for none (engine.inputs lists them). Default: "
                                "tr_0 [1, 2], other masters []"))
                .field("lane_mute", &TrackSetArgs::laneMute,
                       schema::boolean().doc("Masters: silence the master's own items (its "
                                             "prints) and nothing else"))
                .field("chain_midi", &TrackSetArgs::chainMidi,
                       model::chainMidiSchema().doc(
                           "What the FX chain does with MIDI: bus (default; plug-ins hear and "
                           "feed it in order) or instrument (only the first plug-in with a note "
                           "input hears the track's MIDI; plug-ins' note output goes nowhere)"))
                .build();
        c.result = emptySchema();
        c.examples = {
            {"Turn the bass down 4 dB and a little left",
             j(R"({"track": "tr_1", "volume_db": "-4 dB", "pan": -15})"), j("{}"), "song"},
            {"Arm for recording from inputs 1 and 2",
             j(R"({"track": "tr_1", "armed": true, "record_input": [1, 2]})"), j("{}"), "song"},
            {"Feed the parent only through explicit sends",
             j(R"({"track": "tr_1", "parent_send": false})"), j("{}"), "song"},
            {"Play and record a MIDI keyboard (any device, channel 1)",
             j(R"({"track": "tr_1", "armed": true, "record_midi": "*", "midi_channel": 1, "monitor": "auto"})"),
             j("{}"), "song"},
            {"Narrow a stereo track to half width, a little right",
             j(R"({"track": "tr_1", "pan_mode": "width", "width": 50, "pan": 20})"), j("{}"),
             "song"},
            {"Give a track the -3 dB pan law", j(R"({"track": "tr_1", "pan_law": {"db": -3}})"),
             j("{}"), "song"},
            {"Check the mix in mono", j(R"({"track": "tr_0", "mono": true})"), j("{}"), "song"},
            {"Four channels: a key signal can arrive on 3/4",
             j(R"({"track": "tr_1", "channels": 4})"), j("{}"), "song"},
            {"Play the main mix to outputs 3 and 4", j(R"({"track": "tr_0", "hw_output": [3, 4]})"),
             j("{}"), "song"},
            {"Arm the main master to print the mix", j(R"({"track": "tr_0", "armed": true})"),
             j("{}"), "song"},
            {"Hear the live mix without the print", j(R"({"track": "tr_0", "lane_mute": true})"),
             j("{}"), "song"},
            {"Only the instrument hears the track's MIDI, as before 2.10",
             j(R"({"track": "tr_1", "chain_midi": "instrument"})"), j("{}"), "song"}};
        c.errors = {{"track.record_input", "choose one input: audio, MIDI or rack",
                     "provide one non-null input field; it replaces the previous input"}};
        c.run = [](const TrackSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Track>(s, a.track, "/track");
            if (!id) return std::move(id).error();
            if (a.color && !model::validTrackColor(*a.color))
                return Error("track.color", "unknown colour \"" + *a.color + "\"", "/color",
                             "red, orange, yellow, green, teal, blue, violet, grey or \"#rrggbb\"; "
                             "\"\" clears it");
            const model::StripType newType = a.type.value_or(s.tracks.find(*id)->type);
            if ((a.recordInput || a.recordMidi || a.recordRack || a.midiChannel || a.recordMode ||
                 a.monitor) &&
                newType != model::StripType::Channel)
                return Error("track.strip_type", "only channels have record inputs or monitor",
                             "/track",
                             newType == model::StripType::Master
                                 ? "arm the master alone (armed: true): it records its output"
                                 : "record or monitor on a child channel instead");
            if (a.armed && *a.armed && newType == model::StripType::Buss)
                return Error("track.strip_type", "only channels and masters can be armed", "/armed",
                             "arm a child channel, or the master to print the mix");
            if (a.laneMute && newType != model::StripType::Master)
                return Error("track.strip_type", "only masters have a lane mute", "/lane_mute",
                             "mute the items themselves (item.set {mute}) or the track");
            if ((a.panMode || a.width || a.panLeft || a.panRight || a.panLaw) &&
                newType == model::StripType::Master)
                return Error("track.strip_type",
                             "a master's pan is a balance, with no pan mode or law",
                             a.panLaw    ? "/pan_law"
                             : a.panMode ? "/pan_mode"
                                         : "/width",
                             "use pan on the master; set pan modes and laws on channels and "
                             "busses");
            std::optional<model::PanLaw> panLaw;
            if (a.panLaw && !a.panLaw->isNull()) {
                schema::DecodeCtx decode;
                panLaw.emplace();
                if (!model::panLawSchema().decodeInto(*a.panLaw, *panLaw, decode, "/pan_law"))
                    return Error("pan_law.db", "invalid pan law", "/pan_law",
                                 "give {db: 0|-2.5|-3|-4.5|-6, boost: true|false} or null");
            }
            // null clears that input; a value sets it.
            const bool clearAudio = a.recordInput && a.recordInput->isNull();
            const bool clearRack = a.recordRack && a.recordRack->isNull();
            std::optional<std::vector<std::int64_t>> recordInput;
            if (a.recordInput && !clearAudio) {
                recordInput.emplace();
                for (const json::Value& ch : a.recordInput->asArray())
                    recordInput->push_back(ch.asInt());
            }
            const std::optional<std::string> recordRack =
                a.recordRack && !clearRack ? std::optional(a.recordRack->asString()) : std::nullopt;
            if (int(bool(recordRack)) + int(bool(recordInput)) +
                    int(bool(a.recordMidi && !a.recordMidi->isNull())) >
                1)
                return Error("track.record_input", "choose one input: audio, MIDI or rack",
                             recordInput && a.recordMidi && !a.recordMidi->isNull()
                                 ? "/record_midi"
                                 : "/record_rack",
                             "provide one of record_input, record_midi or record_rack; "
                             "it replaces the previous input");
            if (a.hwOutput && a.type.value_or(s.tracks.find(*id)->type) != model::StripType::Master)
                return Error("track.strip_type", "only masters play to hardware outputs",
                             "/hw_output",
                             "send this track to a master and set that master's hw_output");
            if (a.hwOutput && (a.hwOutput->size() > 2 ||
                               (a.hwOutput->size() == 2 && (*a.hwOutput)[0] == (*a.hwOutput)[1])))
                return Error("track.hw_output",
                             "hw_output is two different channels, one channel or none",
                             "/hw_output", "e.g. [3, 4] for a pair, [5] for mono, [] for none");
            if (a.parentSend && !s.tracks.find(*id)->parent)
                return Error("track.parent_send", "a master has no parent", "/parent_send",
                             "masters feed hardware outputs and renders");
            if (a.type && *a.type != s.tracks.find(*id)->type) {
                if (*id == model::kMaster)
                    return Error("track.strip_type", "tr_0 cannot be retyped", "/type",
                                 "keep the main master; add another track instead");
                // A channel cannot take a rack send (ADR-0075): name the route.
                if (*a.type == model::StripType::Channel)
                    for (const auto& r : s.routes)
                        if (r.value->to.track == *id && model::rackSendHome(s, *r.value))
                            return Error(
                                "track.strip_type",
                                "rack send " + r.id.str() + " reaches this " +
                                    std::string(model::stripTypeName(s.tracks.find(*id)->type)) +
                                    "; a channel cannot take one",
                                "/type",
                                "remove rack send " + r.id.str() +
                                    " (route.remove) before retyping");
                model::EditTxn& txn = app.txn();
                const auto& old = *s.tracks.find(*id);
                if (*a.type == model::StripType::Master) {
                    txn.modify<model::Track>(*old.parent,
                                             [&](auto& p) { std::erase(p.children, *id); });
                    auto p = txn.project();
                    p.masters.push_back(*id);
                    txn.setProject(std::move(p));
                    txn.modify<model::Track>(*id, [](auto& tr) { tr.parent.reset(); });
                } else if (old.type == model::StripType::Master) {
                    auto p = txn.project();
                    std::erase(p.masters, *id);
                    txn.setProject(std::move(p));
                    txn.modify<model::Track>(model::kMaster,
                                             [&](auto& tr) { tr.children.push_back(*id); });
                    txn.modify<model::Track>(*id, [](auto& tr) {
                        tr.parent = model::kMaster;
                        tr.hwOutput.reset();  // only masters play to hardware
                    });
                }
                txn.modify<model::Track>(*id, [&](auto& tr) {
                    tr.type = *a.type;
                    // A new channel's default monitoring goes with the channel.
                    if (*a.type != model::StripType::Channel)
                        tr.record.monitor = model::Monitor::Off;
                    // Only masters have a lane mute; busses are never armed (ADR-0057).
                    if (*a.type != model::StripType::Master) tr.laneMute = false;
                    if (*a.type == model::StripType::Buss) tr.record.armed = false;
                });
            }
            // The record input is a route from the device inputs (hw_input).
            std::optional<Id> input = s.tracks.find(*id)->record.input;
            std::optional<Id> midiInput = s.tracks.find(*id)->record.midiInput;
            if (input)
                if (const auto* old = s.routes.find(*input); old && old->from.midiDevice) {
                    if (!midiInput) midiInput = input;
                    input.reset();
                }
            // A route and the automation of its level and pan.
            auto removeRoute = [&](Id route) {
                for (const auto& env : s.envelopes)
                    if (env.value->target.object == route) {
                        for (const auto& item : s.autoItems)
                            if (item.value->envelope == env.id)
                                app.txn().remove<model::AutomationItem>(item.id);
                        app.txn().remove<model::Envelope>(env.id);
                    }
                app.txn().remove<model::Route>(route);
            };
            const model::Route* audioRoute = input ? s.routes.find(*input) : nullptr;
            const bool hardwareAudio = audioRoute && audioRoute->from.hwChannels;
            // No hardware audio input (record_input: null), or no rack Out
            // (record_rack: null): each takes off only an input of its kind.
            if ((clearAudio && hardwareAudio) ||
                (clearRack && audioRoute && audioRoute->from.module)) {
                removeRoute(*input);
                input.reset();
            }
            const bool clearMidi = a.recordMidi && a.recordMidi->isNull();
            if ((a.recordMidi && !clearMidi) || a.midiChannel)
                if (!a.recordMidi && !(midiInput && s.routes.find(*midiInput)))
                    return Error("track.record_midi", "give record_midi (the device) first",
                                 "/midi_channel",
                                 "set record_midi to a device from engine.inputs first");
            if (recordInput) {
                if (recordInput->empty())
                    return Error("track.record_input", "give one or more input channels",
                                 "/record_input", "e.g. [1, 2] for a stereo pair");
                // Reuse the selected input route when changing its kind.
                if (midiInput && !clearMidi) {
                    if (!input)
                        input = midiInput;
                    else
                        removeRoute(*midiInput);
                    midiInput.reset();
                }
                std::vector<int> channels(recordInput->begin(), recordInput->end());
                if (input && s.routes.find(*input)) {
                    app.txn().modify<model::Route>(*input, [&](model::Route& r) {
                        r.from.hwChannels = channels;
                        r.from.midiDevice.reset();  // audio replaces MIDI
                        r.from.module.reset();      // and a rack's Out
                        r.midi.reset();
                    });
                } else {
                    model::Route r;
                    r.from.hwChannels = channels;
                    r.to.track = *id;
                    input = app.txn().create(r);
                }
            }
            if (clearMidi) {
                if (midiInput) removeRoute(*midiInput);
                midiInput.reset();
            } else if (a.recordMidi || a.midiChannel) {
                // MIDI replaces either audio source, including a rack Out.
                if (input) {
                    if (!midiInput)
                        midiInput = input;
                    else
                        removeRoute(*input);
                    input.reset();
                }
                const model::Route* old = midiInput ? s.routes.find(*midiInput) : nullptr;
                auto setMidi = [&](model::Route& r) {
                    r.from = model::RouteEnd{};
                    r.from.midiDevice =
                        a.recordMidi ? a.recordMidi->asString() : *old->from.midiDevice;
                    model::MidiRouting m = r.midi.value_or(model::MidiRouting{});
                    if (a.midiChannel) m.fromChannel = static_cast<int>(*a.midiChannel);
                    r.midi = m;
                };
                if (old)
                    app.txn().modify<model::Route>(*midiInput, setMidi);
                else {
                    model::Route r;
                    r.to.track = *id;
                    setMidi(r);
                    midiInput = app.txn().create(r);
                }
            }
            if (recordRack) {  // a rack's Out module (ADR-0062)
                auto out = Id::parse(*recordRack);
                const model::RackModule* m = out ? s.modules.find(*out) : nullptr;
                if (!m || m->type != model::kOutModule)
                    return Error("rack.port", *recordRack + " is not an Out module", "/record_rack",
                                 "rack.list lists every rack's Out modules");
                if (trackFeedsRack(s, *id, m->rack))
                    return Error(
                        "rack.cycle",
                        a.track + " sends into " + m->rack.str() + ", so it cannot hear that rack",
                        "/record_rack",
                        "remove the send into the rack's In module, or pick another Out");
                if (midiInput) {
                    if (!input)
                        input = midiInput;
                    else
                        removeRoute(*midiInput);
                    midiInput.reset();
                }
                if (input && s.routes.find(*input)) {
                    app.txn().modify<model::Route>(*input, [&](model::Route& r) {
                        r.from = model::RouteEnd{};
                        r.from.module = *out;
                        r.midi.reset();
                    });
                } else {
                    model::Route r;
                    r.from.module = *out;
                    r.to.track = *id;
                    input = app.txn().create(r);
                }
            }
            app.txn().modify<model::Track>(*id, [&](model::Track& t) {
                if (a.armed) t.record.armed = *a.armed;
                if (a.recordMode) t.record.mode = *a.recordMode;
                if (a.recordInput || a.recordMidi || a.recordRack || a.midiChannel)
                    t.record.input =
                        input ? input : midiInput;  // retain the MIDI-only legacy alias
                if (a.recordInput || a.recordMidi || a.recordRack || a.midiChannel)
                    t.record.midiInput = midiInput;
                if (a.monitor) t.record.monitor = *a.monitor;
                if (a.name) t.name = *a.name;
                if (a.color) t.color = *a.color;
                if (a.notes) t.notes = *a.notes;
                if (a.volumeDb) t.volumeDb = *a.volumeDb;
                if (a.pan) t.pan = *a.pan;
                if (a.panMode) t.panMode = *a.panMode;
                if (a.width) t.width = *a.width;
                if (a.panLeft) t.panLeft = *a.panLeft;
                if (a.panRight) t.panRight = *a.panRight;
                if (a.panLaw) t.panLaw = panLaw;
                if (a.mute) t.mute = *a.mute;
                if (a.phaseInvert) t.phaseInvert = *a.phaseInvert;
                if (a.mono) t.mono = *a.mono;
                if (a.parentSend) t.parentSend.enabled = *a.parentSend;
                if (a.timebase) t.timebase = *a.timebase;
                if (a.channels) t.channels = static_cast<int>(*a.channels);
                if (a.hwOutput)
                    t.hwOutput = std::vector<int>(a.hwOutput->begin(), a.hwOutput->end());
                if (a.laneMute) t.laneMute = *a.laneMute;
                if (a.chainMidi) t.chainMidi = *a.chainMidi;
                if (a.solo)
                    t.solo = *a.solo == "on"         ? model::Solo::On
                             : *a.solo == "in_place" ? model::Solo::InPlace
                             : *a.solo == "defeat"   ? model::Solo::Defeat
                                                     : model::Solo::Off;
            });
            if (a.timebase && *a.timebase != s.tracks.find(*id)->timebase) {
                // Its items that inherit the track's timebase keep their place.
                const model::Timebase to =
                    *a.timebase == model::TrackTimebase::Time    ? model::Timebase::Time
                    : *a.timebase == model::TrackTimebase::Beats ? model::Timebase::Beats
                    : *a.timebase == model::TrackTimebase::BeatsPosition
                        ? model::Timebase::BeatsPosition
                        : s.project->timebase;
                for (const auto& e : s.items) {
                    if (e.value->track != *id || e.value->timebase != model::ItemTimebase::Track)
                        continue;
                    app.txn().modify<model::Item>(e.id, [&](model::Item& it) {
                        model::retimeItem(s, it, model::itemTimebase(s, it), to);
                    });
                }
            }
            // Expand each linked attribute once from the pre-edit snapshot. Selection
            // editing uses bypassed explicit targets, so overlaps never multiply a drag.
            if (!a.bypassGroups) {
                const auto& source = *s.tracks.find(*id);
                auto spread = [&](std::string_view attribute, auto change) {
                    for (Id member : model::linkedTracks(s, *id, attribute)) {
                        if (member == *id) continue;
                        const auto& old = *s.tracks.find(member);
                        // Arm spreads to channels and masters (ADR-0057), monitoring
                        // to channels.
                        if ((attribute == "armed" && old.type == model::StripType::Buss) ||
                            (attribute == "monitor" && old.type != model::StripType::Channel))
                            continue;
                        if (attribute == "pan" && !a.pan && old.type == model::StripType::Master)
                            continue;
                        app.txn().modify<model::Track>(member, [&](auto& tr) { change(tr, old); });
                        const auto tr = *app.view().tracks.find(member);
                        if (attribute == "volume")
                            app.recordAutomation(member, "volume", tr.volumeDb);
                        if (attribute == "pan") {
                            if (a.pan) app.recordAutomation(member, "pan", tr.pan);
                            if (a.width && old.type != model::StripType::Master)
                                app.recordAutomation(member, "width", tr.width);
                            if (a.panLeft && old.type != model::StripType::Master)
                                app.recordAutomation(member, "pan_left", tr.panLeft);
                            if (a.panRight && old.type != model::StripType::Master)
                                app.recordAutomation(member, "pan_right", tr.panRight);
                            if (a.panLaw && model::panLawOf(*s.project, old).boost !=
                                                model::panLawOf(*s.project, tr).boost)
                                app.recordAutomation(member, "pan_boost",
                                                     model::panLawOf(*s.project, tr).boost ? 1 : 0);
                        }
                        if (attribute == "mute")
                            app.recordAutomation(member, "mute", tr.mute ? 1 : 0);
                    }
                };
                if (a.volumeDb)
                    spread("volume", [&](auto& tr, const auto& old) {
                        if (*a.volumeDb <= model::kSilenceDb)
                            tr.volumeDb = model::kSilenceDb;
                        else if (source.volumeDb > model::kSilenceDb &&
                                 old.volumeDb > model::kSilenceDb)
                            tr.volumeDb = std::clamp(old.volumeDb + *a.volumeDb - source.volumeDb,
                                                     model::kSilenceDb, 12.0);
                    });
                if (a.pan || a.width || a.panLeft || a.panRight || a.panMode || a.panLaw)
                    spread("pan", [&](auto& tr, const auto& old) {
                        if (a.pan)
                            tr.pan = std::clamp(old.pan + *a.pan - source.pan, -100.0, 100.0);
                        if (old.type == model::StripType::Master) return;
                        if (a.width)
                            tr.width =
                                std::clamp(old.width + *a.width - source.width, -100.0, 100.0);
                        if (a.panLeft)
                            tr.panLeft = std::clamp(old.panLeft + *a.panLeft - source.panLeft,
                                                    -100.0, 100.0);
                        if (a.panRight)
                            tr.panRight = std::clamp(old.panRight + *a.panRight - source.panRight,
                                                     -100.0, 100.0);
                        if (a.panMode) tr.panMode = *a.panMode;
                        if (a.panLaw) tr.panLaw = panLaw;
                    });
                if (a.mute) spread("mute", [&](auto& tr, const auto&) { tr.mute = *a.mute; });
                if (a.solo) {
                    const auto value = app.view().tracks.find(*id)->solo;
                    spread("solo", [&](auto& tr, const auto&) { tr.solo = value; });
                }
                if (a.armed)
                    spread("armed", [&](auto& tr, const auto&) { tr.record.armed = *a.armed; });
                if (a.monitor)
                    spread("monitor",
                           [&](auto& tr, const auto&) { tr.record.monitor = *a.monitor; });
            }
            // Touch/latch/write envelopes record the change while the transport plays.
            if (a.volumeDb) app.recordAutomation(*id, "volume", *a.volumeDb);
            if (a.pan) app.recordAutomation(*id, "pan", *a.pan);
            if (a.width) app.recordAutomation(*id, "width", *a.width);
            if (a.panLeft) app.recordAutomation(*id, "pan_left", *a.panLeft);
            if (a.panRight) app.recordAutomation(*id, "pan_right", *a.panRight);
            if (a.panLaw) {
                const bool before = model::panLawOf(*s.project, *s.tracks.find(*id)).boost;
                const bool after = model::panLawOf(*s.project, *app.view().tracks.find(*id)).boost;
                if (before != after) app.recordAutomation(*id, "pan_boost", after ? 1 : 0);
            }
            if (a.mute) app.recordAutomation(*id, "mute", *a.mute ? 1.0 : 0.0);
            if (a.parentSend && *a.parentSend && !s.tracks.find(*id)->parentSend.enabled)
                if (auto valid = model::checkInvariants(app.view()); !valid)
                    return Error(
                        "route.cycle", "the parent send would feed back: " + valid.error().message,
                        "/parent_send", "remove the send from the parent to this track first");
            if (a.type && *a.type != s.tracks.find(*id)->type) {
                SUPPLE_TRY(model::validateStripRules(app.view()));
                auto valid = model::checkInvariants(app.view());
                if (!valid) {
                    auto error = std::move(valid).error();
                    error.code = "track.strip_type";
                    error.hint =
                        "keep a type and parent that preserve the routing graph; use a buss to "
                        "group";
                    return error;
                }
            }
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.list
    [&] {
        Command<TrackListArgs, TrackList> c;
        c.name = "track.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List tracks in tree order with their level, mute/solo, fx and items.";
        c.tags = {"track"};
        c.args = schema::object<TrackListArgs>("track_list_args")
                     .field("at", &TrackListArgs::at, schema::number().min(0))
                     .build();
        auto entry = schema::object<TrackEntry>("track_entry")
                         .field("frozen", &TrackEntry::frozen, schema::boolean())
                         .field("freeze", &TrackEntry::freeze, model::trackFreezeSchema())
                         .field("id", &TrackEntry::id, schema::string())
                         .field("name", &TrackEntry::name, schema::string())
                         .field("type", &TrackEntry::type, model::stripTypeSchema())
                         .field("parent", &TrackEntry::parent, schema::string())
                         .field("depth", &TrackEntry::depth, schema::integer())
                         .field("volume_db", &TrackEntry::volumeDb, schema::number())
                         .field("mute", &TrackEntry::mute, schema::boolean())
                         .field("solo", &TrackEntry::solo, schema::string())
                         .field("fx", &TrackEntry::fx, schema::arrayOf(schema::string()))
                         .field("items", &TrackEntry::items, schema::arrayOf(schema::string()))
                         .field("groups", &TrackEntry::groups, schema::arrayOf(model::idSchema()))
                         .field("vcas", &TrackEntry::vcas, schema::arrayOf(model::idSchema()))
                         .field("hw_output", &TrackEntry::hwOutput,
                                schema::arrayOf(schema::integer())
                                    .doc("Masters: the device outputs they play to"))
                         .field("armed", &TrackEntry::armed,
                                schema::boolean().doc("Masters: recording takes the output"))
                         .field("lane_mute", &TrackEntry::laneMute,
                                schema::boolean().doc("Masters: their own items are silent"))
                         .field("requested_effective_volume_db", &TrackEntry::requestedEffective,
                                schema::number())
                         .field("effective_volume_db", &TrackEntry::effective, schema::number())
                         .field("at", &TrackEntry::at, schema::number())
                         .field("effective_mute", &TrackEntry::effectiveMute, schema::boolean())
                         .field("solo_silenced", &TrackEntry::soloSilenced, schema::boolean())
                         .build();
        c.result = schema::object<TrackList>("track_list")
                       .field("tracks", &TrackList::tracks, schema::arrayOf(entry))
                       .build();
        c.examples = {{"List", j("{}"), j(R"({"tracks": "<any>"})"), "song"}};
        c.run = [](const TrackListArgs& args, Context& ctx) -> Result<TrackList> {
            const model::Session s = app(ctx).view();
            TrackList out;
            static constexpr const char* kSolo[] = {"off", "on", "in_place", "defeat"};
            std::function<void(Id, int)> walk = [&](Id id, int depth) {
                const model::Track& t = *s.tracks.find(id);
                TrackEntry e;
                e.frozen = t.freeze.has_value();
                e.freeze = t.freeze;
                e.id = id.str();
                e.name = t.name;
                e.type = t.type;
                e.parent = t.parent ? t.parent->str() : "";
                e.depth = depth;
                e.volumeDb = t.volumeDb;
                e.at = args.at.value_or(app(ctx).engine() ? app(ctx).engine()->transport().seconds
                                                          : 0);
                e.groups = model::trackGroups(s, id);
                e.vcas = model::vcaControllers(s, id);
                if (t.type == model::StripType::Master) {
                    const auto channels = model::hardwareOutputs(t, id);
                    e.hwOutput = std::vector<std::int64_t>(channels.begin(), channels.end());
                    e.armed = t.record.armed;
                    e.laneMute = t.laneMute;
                }
                const auto level = model::effectiveTrackLevel(s, id, e.at);
                e.requestedEffective = level.requestedDb;
                e.effective = level.appliedDb;
                e.effectiveMute = level.mute;
                e.soloSilenced = level.soloSilenced;
                e.mute = t.mute;
                e.solo = kSolo[static_cast<int>(t.solo)];
                for (Id f : t.chain) e.fx.push_back(f.str());
                for (const auto& it : s.items)
                    if (it.value->track == id) e.items.push_back(it.id.str());
                out.tracks.push_back(std::move(e));
                for (Id c2 : t.children) walk(c2, depth + 1);
            };
            for (Id master : s.project->masters) walk(master, 0);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TrackMoveArgs, Empty> c;
        c.name = "track.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Move a track under another track or reorder it among its siblings.";
        c.description =
            "parent defaults to the track's current parent (a pure reorder); index defaults to "
            "the end of the destination's children. A track cannot move under itself or one of "
            "its own descendants.";
        c.tags = {"track"};
        c.args = schema::object<TrackMoveArgs>("track_move_args")
                     .field("track", &TrackMoveArgs::track, idArg("tr", "Track"))
                     .field("parent", &TrackMoveArgs::parent, idArg("tr", "Track"))
                     .field("index", &TrackMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.errors = {{"tr.cycle", "the destination is inside the track", "choose another parent"}};
        c.examples = {{"Move a track to the master", j(R"({"track":"tr_1","parent":"tr_0"})"),
                       j("{}"), "song"}};
        c.run = [](const TrackMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = existing<model::Track>(s, a.track, "/track");
            if (!id) return std::move(id).error();
            if (*id == model::kMaster)
                return Error("track.strip_type", "tr_0 must remain the first root", "/track",
                             "reorder another master or move a channel/buss instead");
            const model::Track& track = *s.tracks.find(*id);
            if (track.type == model::StripType::Master) {
                if (a.parent)
                    return Error("track.strip_type", "masters have no parent", "/parent",
                                 "omit parent to reorder a master");
                auto p = app(ctx).txn().project();
                std::erase(p.masters, *id);
                auto at = a.index ? std::clamp<std::size_t>(*a.index, 1, p.masters.size())
                                  : p.masters.size();
                p.masters.insert(p.masters.begin() + static_cast<std::ptrdiff_t>(at), *id);
                app(ctx).txn().setProject(std::move(p));
                return Empty{};
            }
            const Id from = *track.parent;
            Id target = from;
            if (a.parent) {
                auto parent = existing<model::Track>(s, *a.parent, "/parent");
                if (!parent) return std::move(parent).error();
                target = *parent;
            }
            if (target == *id)
                return Error("tr.cycle", "a track cannot be moved under itself", "/parent",
                             "choose another parent");
            if (target != model::kMaster && nestedUnder(s, *id, target))
                return Error("tr.cycle", "a track cannot be moved under one of its own children",
                             "/parent", "choose a track outside it");
            // The send graph: a folder send or a route that already reaches the
            // track would close a feedback loop (the same check route.add uses).
            if (target != from)
                if (auto cycle = routeCycle(s, *id, target)) {
                    return Error("tr.cycle",
                                 "the move creates a cycle: " + cycleText(s, *cycle, true),
                                 "/parent", "choose a destination that does not feed this track");
                }
            if (s.tracks.find(target)->type == model::StripType::Channel)
                return Error("track.strip_type", "a channel cannot have children", "/parent",
                             "add a buss and place the channels under it");
            model::EditTxn& t = app(ctx).txn();
            t.modify<model::Track>(from, [&](model::Track& p) {
                p.children.erase(std::remove(p.children.begin(), p.children.end(), *id),
                                 p.children.end());
            });
            t.modify<model::Track>(target, [&](model::Track& p) {
                const auto at = a.index ? std::min<std::size_t>(static_cast<std::size_t>(*a.index),
                                                                p.children.size())
                                        : p.children.size();
                p.children.insert(p.children.begin() + static_cast<std::ptrdiff_t>(at), *id);
            });
            t.modify<model::Track>(*id, [&](model::Track& tr) { tr.parent = target; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
