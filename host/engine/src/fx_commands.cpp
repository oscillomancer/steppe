// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// FX commands: fx.add/remove/set/restart/move, patches, parameters, meters and fx.describe.
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- fx ------------------------------------------------------------------------------------

struct FxAddArgs {
    std::optional<std::string> track, take;
    std::string plugin;
    std::optional<std::int64_t> index;
    std::optional<json::Value> patch;
    std::optional<std::string> chain;  // "main" (default) or "input"
};

struct FxAddResult {
    std::string fx, name;
    bool semantic = false;
    std::vector<std::string> chain;
};

struct FxRef {
    std::string fx;
};

struct FxSetArgs {
    std::string fx;
    std::optional<bool> bypass, offline;
    std::optional<double> wet;
    std::optional<std::string> label;
    std::optional<model::Isolation> isolation;
    std::optional<model::PinMap> pins;
    std::optional<model::MidiOutputMode> midiOutput;
};

struct FxPatchArgs {
    std::string fx;
    json::Value patch;
};

struct FxParamSetArgs {
    std::string fx;
    json::Value values;
};

struct MergeResult {
    json::Value changed;
    std::string description;
    std::optional<std::vector<std::string>> inactive;
};

struct FxDescribeArgs {
    std::string fx;
    bool full = false;
    std::optional<std::string> filter;
};

struct FxDescription {
    std::string plugin, name, description;
    std::optional<json::Value> params, patch;
    bool statusAvailable = false;
    std::optional<json::Value> problems;
    std::optional<std::int64_t> paramsTotal;
    std::optional<std::int64_t> sidechainChannels;  // a key input (ADR-0018)
    std::optional<std::int64_t> latencySamples;     // the instance's reported latency
    std::optional<bool> outputsNotes;               // note output ports (ADR-0068)
    std::optional<std::string> isolationEffective;  // where it runs (ADR-0081)
};

constexpr std::size_t kMaxListedParams = 512;

struct FxMoveArgs {
    std::string fx;
    std::optional<std::string> track;
    std::int64_t index = 0;
};

struct FxPatchGetArgs {
    std::string fx;
    bool changedOnly = false;
};

struct FxPatchResult {
    json::Value patch;
};

struct FxParamGetArgs {
    std::string fx, param;
};

struct FxParamResult {
    json::Value value;
};

enum class MetersFrom : std::uint8_t { Live, Render };

struct FxMetersArgs {
    std::string fx;
    MetersFrom from = MetersFrom::Live;
};

struct FxMetersResult {
    std::string source;
    json::Value meters;
};

struct FxPatchSetArgs {
    std::string fx;
    std::optional<json::Value> patch;
    std::optional<std::string> name;
};

struct FxPatchListArgs {
    std::string fx;
};

struct FxPatchListResult {
    json::Value patches;
};

}  // namespace

void registerFxCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- fx.state.capture
    // A plug-in's own state change (its editor, a preset loaded inside it) as
    // a replayable edit: captureFxState logs it, and merges replay it on
    // another branch (session-model §6.4). The state is found by its hash in
    // any kept version, so the log holds no copy of the bytes.
    [&] {
        struct StateArgs {
            std::string fx, state;
        };
        Command<StateArgs, Empty> c;
        c.name = "fx.state.capture";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::Internal;
        c.summary = "Record a plug-in's own state change as a replayable edit.";
        c.args =
            schema::object<StateArgs>("fx_state_capture_args")
                .field("fx", &StateArgs::fx, idArg("fx", "Plug-in"))
                .field("state", &StateArgs::state,
                       schema::string()
                           .pattern("^sha256:[0-9a-f]{64}$", "a state hash")
                           .doc("The state's blob hash, as the plug-in's state field holds it"))
                .build();
        c.result = emptySchema();
        c.run = [](const StateArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            std::shared_ptr<const model::Blob> blob;
            if (const auto found = s.blobs.find(a.state); found != s.blobs.end())
                blob = found->second;
            engine.versions().forEachVersion([&](const model::Version& v) {
                if (blob) return;
                if (const auto found = v.session->blobs.find(a.state);
                    found != v.session->blobs.end())
                    blob = found->second;
            });
            if (!blob)
                return Error("fx.state_missing", "no kept version holds the state " + a.state,
                             "/state", "capture the plug-in's state again on this branch");
            model::FxInstance next = *s.fx.find(*id);
            next.state = a.state;
            engine.txn().putBlob(a.state, std::move(blob));
            engine.txn().put<model::FxInstance>(*id, std::move(next));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.add
    [&] {
        Command<FxAddArgs, FxAddResult> c;
        c.name = "fx.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Insert a plugin into a track's (or a take's) FX chain and return its id.";
        c.description =
            "plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic "
            "plugins. chain: main (default; the track's effects) or input (the recorded-input "
            "chain). take instead of track: the take's own chain (take FX) — the item plays "
            "through it before the track's input, cut to the item; an instrument there plays a "
            "MIDI take on its own. An instrument with outputs past its main pair (a multi-out "
            "sampler's Kick, Snare...) gets that many more channels on the track, and its "
            "pins.out sends each output there; route.matrix lists them and route.add "
            "{channels: {from: [3, 4]}} sends one to a buss. Without index, a note effect "
            "(an arpeggiator) goes to the front of the track's chain, ahead of what it plays, and "
            "an instrument after the note effects there.";
        c.tags = {"fx"};
        c.args = schema::object<FxAddArgs>("fx_add_args")
                     .field("track", &FxAddArgs::track, idArg("tr", "Track"))
                     .field("take", &FxAddArgs::take, idArg("tk", "Take"))
                     .field("plugin", &FxAddArgs::plugin, schema::string().minLength(1))
                     .field("index", &FxAddArgs::index, schema::integer().min(0))
                     .field("patch", &FxAddArgs::patch, schema::any())
                     .field("chain", &FxAddArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .build();
        c.result = schema::object<FxAddResult>("fx_add")
                       .field("fx", &FxAddResult::fx, schema::string())
                       .field("name", &FxAddResult::name, schema::string())
                       .field("semantic", &FxAddResult::semantic, schema::boolean())
                       .field("chain", &FxAddResult::chain, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"plugin.unknown", "no such plugin", "closest ids in the hint; see plugin.list"}};
        c.examples = {
            {"An instrument on the bass track",
             j(R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})"),
             j(R"({"fx": "fx_1", "name": "Test Sine", "semantic": false, "chain": ["fx_1"]})"),
             "song"},
            {"An effect in the track's input chain",
             j(R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain", "chain": "input"})"),
             j(R"({"fx": "fx_1", "name": "<any>", "semantic": "<any>", "chain": ["fx_1"]})"),
             "song"},
            {"Take FX: a gain on the take of an item",
             j(R"({"take": "tk_1", "plugin": "com.suppleaudio.test.gain"})"),
             j(R"({"fx": "fx_1", "name": "<any>", "semantic": "<any>", "chain": ["fx_1"]})"),
             "song_with_item"}};
        c.run = [](const FxAddArgs& a, Context& ctx) -> Result<FxAddResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            if (a.track.has_value() == a.take.has_value())
                return Error("fx.target", "give track or take", "/track",
                             "track: a track's chain; take: a take's own chain (take FX)");
            std::optional<Id> track, take;
            if (a.track) {
                auto t = existing<model::Track>(s, *a.track, "/track");
                if (!t) return std::move(t).error();
                track = *t;
            } else {
                auto t = existing<model::Take>(s, *a.take, "/take");
                if (!t) return std::move(t).error();
                take = *t;
                if (a.chain && *a.chain == "input")
                    return Error("fx.chain", "a take has one chain", "/chain", "leave chain out");
            }
            auto info = app.findPlugin(a.plugin);
            if (!info) return std::move(info).error().at("/plugin");
            model::FxInstance fx;
            fx.plugin = {info->format, info->id, info->name, info->vendor, info->version};
            if (a.patch) {
                auto merged = app.mergedPatch(fx, *a.patch, false);
                if (!merged) {
                    Error e = std::move(merged).error();
                    e.path = "/patch" + e.path;
                    return e;
                }
                fx.patch = std::move(*merged);
            }
            const bool input = a.chain && *a.chain == "input";
            // A multi-out instrument in a track's chain (ADR-0021): its
            // auxiliary outputs land on new channels after the track's own, as
            // far as the track can grow; out names them after the main pair.
            // An effect's extra outputs stay unconnected until pinned.
            int grownChannels = 0;
            if (track && !input && info->instrument) {
                if (auto ports = app.fxAuxOutputs(fx); ports && !ports->empty()) {
                    int channels = std::max(s.tracks.find(*track)->channels, 2);
                    std::vector<int> out{1, 2};
                    bool connected = false;
                    for (const host::AudioPortInfo& port : *ports)
                        for (std::uint32_t ch = 0; ch < port.channels; ++ch) {
                            if (channels < model::kMaxTrackChannels) {
                                out.push_back(++channels);
                                connected = true;
                            } else {
                                out.push_back(0);
                            }
                        }
                    if (connected) {
                        fx.pins.out = std::move(out);
                        grownChannels = channels;
                    }
                }
            }
            model::EditTxn& t = app.txn();
            const Id id = t.create(std::move(fx));
            if (take) {  // take FX keep the caller's order
                std::vector<Id> chain = s.takes.find(*take)->chain;
                const std::size_t at =
                    a.index
                        ? std::min<std::size_t>(static_cast<std::size_t>(*a.index), chain.size())
                        : chain.size();
                chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), id);
                t.modify<model::Take>(*take, [&](model::Take& tk) { tk.chain = chain; });
                FxAddResult r{id.str(), info->name, info->semantic, {}};
                for (Id f : chain) r.chain.push_back(f.str());
                return r;
            }
            const model::Track* strip = s.tracks.find(*track);
            std::vector<Id> chain = input ? strip->inputChain : strip->chain;
            std::size_t at =
                a.index ? std::min<std::size_t>(static_cast<std::size_t>(*a.index), chain.size())
                        : chain.size();
            // Unless the caller said where (the input chain keeps the
            // caller's order): a note effect goes to the front of the track's
            // chain, ahead of what plays its notes (ADR-0068), and an
            // instrument after the note effects there.
            const auto noteEffect = [](const host::PluginInfo& p) {
                return std::find(p.features.begin(), p.features.end(),
                                 CLAP_PLUGIN_FEATURE_NOTE_EFFECT) != p.features.end();
            };
            if (!input && !a.index && noteEffect(*info)) {
                at = 0;
            } else if (!input && !a.index && info->instrument) {
                at = 0;
                while (at < chain.size()) {
                    const model::FxInstance* existing = s.fx.find(chain[at]);
                    auto known = existing ? app.findPlugin(existing->plugin.id)
                                          : Result<host::PluginInfo>(Error("", ""));
                    if (!known || !noteEffect(*known)) break;
                    ++at;
                }
            }
            chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), id);
            t.modify<model::Track>(*track, [&](model::Track& modified) {
                (input ? modified.inputChain : modified.chain) = chain;
                if (grownChannels) modified.channels = grownChannels;
            });
            FxAddResult r{id.str(), info->name, info->semantic, {}};
            for (Id f : chain) r.chain.push_back(f.str());
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.remove / fx.set
    [&] {
        Command<FxRef, Empty> c;
        c.name = "fx.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a plugin instance (and its envelopes).";
        c.tags = {"fx"};
        c.args = schema::object<FxRef>("fx_ref")
                     .field("fx", &FxRef::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"fx": "fx_1"})"), j("{}"), "song_with_fx"}};
        c.run = [](const FxRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            model::EditTxn& t = app.txn();
            auto drop = [&](std::vector<Id>& chain) {
                chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
            };
            for (const auto& e : s.tracks)
                if (std::count(e.value->chain.begin(), e.value->chain.end(), *id) ||
                    std::count(e.value->inputChain.begin(), e.value->inputChain.end(), *id))
                    t.modify<model::Track>(e.id, [&](model::Track& tr) {
                        drop(tr.chain);
                        drop(tr.inputChain);
                    });
            for (const auto& e : s.takes)
                if (std::count(e.value->chain.begin(), e.value->chain.end(), *id))
                    t.modify<model::Take>(e.id, [&](model::Take& tk) { drop(tk.chain); });
            std::set<Id> gone{*id};
            for (const auto& e : s.routes)  // key routes into it (ADR-0018)
                if (e.value->to.fx && *e.value->to.fx == *id) {
                    gone.insert(e.id);
                    t.remove<model::Route>(e.id);
                }
            for (const auto& e : s.envelopes)
                if (gone.count(e.value->target.object)) t.remove<model::Envelope>(e.id);
            t.remove<model::FxInstance>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxSetArgs, Empty> c;
        c.name = "fx.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set instance options: bypass, offline, wet, label, isolation, pins, MIDI.";
        c.description =
            "isolation: in_process, dedicated (the plugin runs in a process of its own: a "
            "crash silences only it), shared_bridge (one process for all such plugins), "
            "inherit (the project default: in_process). pins: which of the track's channels "
            "(1-based; track.set channels adds 3-128) the plugin reads ({in: [l, r]}), writes "
            "({out: [l, r]}) and keys from ({key: [3, 4]}: a detector on 3/4, for a plugin "
            "with a sidechain input); {} restores 1/2 in and out. out past its first two "
            "entries: a track channel for each of the plugin's auxiliary output channels in "
            "order (0: unconnected); fx.add sets them for a multi-out instrument. midi_output, "
            "for a plug-in with note output (fx.describe outputs_notes): replace (default) "
            "gives the plug-ins after it, and the track's post-FX MIDI sends, only its output; "
            "merge adds its output to the MIDI that reached it.";
        c.tags = {"fx"};
        c.args = schema::object<FxSetArgs>("fx_set_args")
                     .field("fx", &FxSetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("bypass", &FxSetArgs::bypass, schema::boolean())
                     .field("offline", &FxSetArgs::offline, schema::boolean())
                     .field("wet", &FxSetArgs::wet,
                            schema::number().range(0, 100).unit(units::Unit::percent))
                     .field("label", &FxSetArgs::label, schema::string())
                     .field("isolation", &FxSetArgs::isolation,
                            schema::enumeration<model::Isolation>(
                                {{model::Isolation::Inherit, "inherit"},
                                 {model::Isolation::InProcess, "in_process"},
                                 {model::Isolation::SharedBridge, "shared_bridge"},
                                 {model::Isolation::Dedicated, "dedicated"}}))
                     .field("pins", &FxSetArgs::pins, model::pinMapSchema())
                     .field("midi_output", &FxSetArgs::midiOutput,
                            schema::enumeration<model::MidiOutputMode>(
                                {{model::MidiOutputMode::Replace, "replace"},
                                 {model::MidiOutputMode::Merge, "merge"}}))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Half wet", j(R"({"fx": "fx_1", "wet": "50%"})"), j("{}"), "song_with_fx"},
            {"Key the plugin from channels 3/4", j(R"({"fx": "fx_1", "pins": {"key": [3, 4]}})"),
             j("{}"), "song_with_keymix_4ch"}};
        c.run = [](const FxSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            auto id = existing<model::FxInstance>(app.view(), a.fx, "/fx");
            if (!id) return std::move(id).error();
            if (a.pins && !a.pins->empty()) {
                const model::Session s = app.view();
                const auto track = model::trackOfFx(s, *id);
                if (!track)
                    return Error("fx.pins", "pins are for a track's FX chain", "/pins",
                                 "take FX and input FX are stereo");
                const int channels = s.tracks.find(*track)->channels;
                for (auto [list, name] :
                     {std::pair{&a.pins->in, "in"}, {&a.pins->out, "out"}, {&a.pins->key, "key"}}) {
                    // out may go on with the auxiliary outputs' channels (ADR-0021).
                    const bool out = list == &a.pins->out;
                    if (out ? list->size() > static_cast<std::size_t>(model::kMaxTrackChannels)
                            : list->size() > 2)
                        return Error("fx.pins", std::string(name) + " names one or two channels",
                                     std::string("/pins/") + name, "e.g. [3, 4]");
                    for (std::size_t k = 0; k < list->size(); ++k) {
                        if ((*list)[k] == 0 && !(out && k >= 2))
                            return Error("fx.pins", "channels count from 1",
                                         std::string("/pins/") + name + "/" + std::to_string(k),
                                         "0 leaves only an auxiliary output unconnected");
                        if ((*list)[k] > std::max(channels, 2))
                            return Error("fx.pins",
                                         "the track has " + std::to_string(std::max(channels, 2)) +
                                             " channels",
                                         std::string("/pins/") + name + "/" + std::to_string(k),
                                         "track.set channels adds channels 3-128");
                    }
                }
            }
            app.txn().modify<model::FxInstance>(*id, [&](model::FxInstance& f) {
                if (a.bypass) f.bypass = *a.bypass;
                if (a.offline) f.offline = *a.offline;
                if (a.wet) f.wet = *a.wet;
                if (a.label) f.label = *a.label;
                if (a.isolation) f.isolation = *a.isolation;
                if (a.pins) f.pins = *a.pins;
                if (a.midiOutput) f.midiOutput = *a.midiOutput;
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxRef, Empty> c;
        c.name = "fx.restart";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Re-create a plugin instance from its saved patch/state (after a crash).";
        c.description =
            "engine.status lists failed_fx: isolated plugins whose process crashed or hung. "
            "The new instance starts from the model's patch or state.";
        c.tags = {"fx"};
        c.args = schema::object<FxRef>("fx_ref")
                     .field("fx", &FxRef::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Restart", j(R"({"fx": "fx_1"})"), j("{}"), "song_with_fx"}};
        c.run = [](const FxRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            auto id = existing<model::FxInstance>(app.view(), a.fx, "/fx");
            if (!id) return std::move(id).error();
            app.restartFx(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.patch.merge /
    // fx.param.set
    auto mergeRun = [](EngineApp& app, const std::string& fxText, const json::Value& patch,
                       const char* patchPath) -> Result<MergeResult> {
        const model::Session s = app.view();
        auto id = existing<model::FxInstance>(s, fxText, "/fx");
        if (!id) return std::move(id).error();
        const model::FxInstance& fx = *s.fx.find(*id);
        auto merged = app.mergedPatch(fx, patch, false);
        if (!merged) {
            Error e = std::move(merged).error();
            auto fix = [&](Error& x) {
                if (std::string_view(patchPath) == "/values" && x.path.rfind("/params", 0) == 0)
                    x.path = "/values" + x.path.substr(7);
                else if (!x.path.empty() || x.details.empty())
                    x.path = std::string(patchPath) + x.path;
            };
            fix(e);
            for (auto& d : e.details) fix(d);
            return e;
        }
        MergeResult r;
        auto before = fx.patch ? *fx.patch : json::Value::object();
        auto changed = json::createMergePatch(before, *merged);
        r.changed = changed ? std::move(*changed) : json::Value::object();
        model::FxInstance next = fx;
        next.patch = std::move(*merged);
        auto d = app.describeFx(next, true);
        r.description = d ? *d : "";
        // The scratch instance holds the merged patch now: its schema says
        // which of the values set do not apply with it.
        if (const json::Value* set = patch.find("params"); set && set->isObject())
            if (auto schema = app.fxSchema(next); schema && schema->find("params")) {
                std::vector<std::string> inactive;
                for (const auto& m : set->asObject()) {
                    const json::Value* e = schema->find("params")->find(m.key);
                    const json::Value* active = e ? e->find("active") : nullptr;
                    if (active && active->isBool() && !active->asBool()) inactive.push_back(m.key);
                }
                if (!inactive.empty()) r.inactive = std::move(inactive);
            }
        app.txn().put<model::FxInstance>(*id, std::move(next));
        return r;
    };
    auto mergeResultSchema = [] {
        return schema::object<MergeResult>("merge_result")
            .field("changed", &MergeResult::changed, schema::any())
            .field("description", &MergeResult::description, schema::string())
            .field("inactive", &MergeResult::inactive,
                   schema::arrayOf(schema::string())
                       .doc("Params set that do not apply with the new values: kept, and heard "
                            "once what fx.describe full's active_when names allows it"))
            .build();
    };
    [&] {
        Command<FxPatchArgs, MergeResult> c;
        c.name = "fx.patch.merge";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Merge a partial patch into a plugin instance; returns the effective change.";
        c.description =
            "Semantic plugins only. Values in physical units: {\"params\": {\"gain\": \"-6 dB\"}}.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchArgs>("fx_patch_args")
                     .field("fx", &FxPatchArgs::fx, idArg("fx", "Plugin instance"))
                     .field("patch", &FxPatchArgs::patch, schema::any())
                     .build();
        c.result = mergeResultSchema();
        c.examples = {{"Turn the gain down",
                       j(R"({"fx": "fx_2", "patch": {"params": {"gain": "-6 dB"}}})"),
                       j(R"({"changed": "<any>", "description": "<any>"})"), "song_with_fx"}};
        c.run = [mergeRun](const FxPatchArgs& a, Context& ctx) {
            return mergeRun(app(ctx), a.fx, a.patch, "/patch");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxParamSetArgs, MergeResult> c;
        c.name = "fx.param.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set semantic parameters in units, or plain plugin parameters by name/id.";
        c.description =
            "Plain CLAP/VST3 values use the declared range or plugin text; read-only parameters "
            "refuse edits.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxParamSetArgs>("fx_param_set_args")
                     .field("fx", &FxParamSetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("values", &FxParamSetArgs::values, schema::any())
                     .build();
        c.result = mergeResultSchema();
        c.examples = {{"Set the gain", j(R"({"fx": "fx_2", "values": {"gain": -3}})"),
                       j(R"({"changed": "<any>", "description": "<any>"})"), "song_with_fx"}};
        c.run = [mergeRun](const FxParamSetArgs& a, Context& ctx) -> Result<MergeResult> {
            auto id = existing<model::FxInstance>(app(ctx).view(), a.fx, "/fx");
            if (!id) return id.error();
            auto schema = app(ctx).fxSchema(*app(ctx).view().fx.find(*id));
            if (!schema) return schema.error();
            // The window's edits are the "last touched parameter" (ADR-0027).
            const bool touches = ctx.origin().type == cmd::Origin::Type::Ui;
            if (schema->isNull()) {
                auto changed = app(ctx).setPlainParams(*id, a.values);
                if (!changed) return changed.error();
                if (touches && changed->isObject())
                    if (auto list = app(ctx).fxParams(*app(ctx).view().fx.find(*id)))
                        for (const auto& m : changed->asObject())
                            for (const host::ParamInfo& p : *list)
                                if (std::to_string(p.id) == m.key ||
                                    std::ranges::equal(p.name, m.key, [](char x, char y) {
                                        return std::tolower(static_cast<unsigned char>(x)) ==
                                               std::tolower(static_cast<unsigned char>(y));
                                    })) {
                                    app(ctx).touchParam(*id, std::to_string(p.id));
                                    break;
                                }
                return MergeResult{std::move(*changed), "Plugin parameters updated", std::nullopt};
            }
            json::Value patch = json::Value::object();
            patch["params"] = a.values;
            auto merged = mergeRun(app(ctx), a.fx, patch, "/values");
            if (!merged || !a.values.isObject()) return merged;
            if (touches)
                for (const auto& m : a.values.asObject()) app(ctx).touchParam(*id, m.key);
            // Touch/latch/write envelopes of these parameters record the change
            // while the transport plays (values in the parameter's unit).
            const auto fx = model::Id::parse(a.fx);
            if (!fx) return merged;
            for (const auto& m : a.values.asObject()) {
                if (!app(ctx).writesAutomation(*fx, m.key)) continue;
                const model::EnvTarget target{*fx, m.key};
                auto domain = envDomain(app(ctx), app(ctx).view(), target, "/values");
                if (!domain) continue;
                if (auto v = envValue(*domain, m.value, "/values/" + m.key); v)
                    app(ctx).recordAutomation(*fx, m.key, *v);
            }
            return merged;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.describe
    [&] {
        Command<FxDescribeArgs, FxDescription> c;
        c.name = "fx.describe";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Describe an instance and its live problems; full adds parameters and patch.";
        c.tags = {"fx"};
        c.args = schema::object<FxDescribeArgs>("fx_describe_args")
                     .field("fx", &FxDescribeArgs::fx, idArg("fx", "Plugin instance"))
                     .field("full", &FxDescribeArgs::full, schema::boolean())
                     .defaultValue(false)
                     .field("filter", &FxDescribeArgs::filter,
                            schema::string().doc(
                                "Plain parameters only: list those whose name contains this "
                                "text (case-insensitive)."))
                     .build();
        c.result =
            schema::object<FxDescription>("fx_description")
                .field("plugin", &FxDescription::plugin, schema::string())
                .field("name", &FxDescription::name, schema::string())
                .field("description", &FxDescription::description, schema::string())
                .field("params", &FxDescription::params, schema::any())
                .field("patch", &FxDescription::patch, schema::any())
                .field("status_available", &FxDescription::statusAvailable,
                       schema::boolean().doc("A live instance supplied its current problems"))
                .field("problems", &FxDescription::problems,
                       schema::any().doc("Current plugin problems: code, message, scope, line when "
                                         "known, hint, silent; scope host when unreadable"))
                .field("sidechain_channels", &FxDescription::sidechainChannels,
                       schema::integer().doc("The plug-in's key input: route.add {fx} feeds it"))
                .field("latency_samples", &FxDescription::latencySamples,
                       schema::integer().doc("The instance's reported latency, in samples"))
                .field("isolation_effective", &FxDescription::isolationEffective,
                       schema::string().doc(
                           "Where the instance runs: in_process, shared_bridge or dedicated; "
                           "inherit resolves to the plug-in isolation preference (stock plug-ins "
                           "in process), as the instance is made"))
                .field("outputs_notes", &FxDescription::outputsNotes,
                       schema::boolean().doc("The loaded plug-in sends notes or MIDI: fx.set "
                                             "midi_output chooses replace or merge"))
                .field("params_total", &FxDescription::paramsTotal,
                       schema::integer().doc("Plain parameters matching; at most " +
                                             std::to_string(kMaxListedParams) + " are listed."))
                .build();
        c.examples = {
            {"Describe", j(R"({"fx": "fx_2"})"),
             j(R"({"plugin": "com.suppleaudio.test.gain", "name": "<any>", "description": "<any>", "latency_samples": "<any>", "isolation_effective": "in_process", "status_available": false})"),
             "song_with_fx"}};
        c.run = [](const FxDescribeArgs& a, Context& ctx) -> Result<FxDescription> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const model::FxInstance& fx = *s.fx.find(*id);
            FxDescription r;
            r.plugin = fx.plugin.id;
            r.name = fx.plugin.name;
            if (auto owner = model::frozenOwner(s, *id)) {
                r.description = "Frozen on " + owner->str() +
                                "; stored state retained. Use track.unfreeze to edit.";
                if (fx.patch) r.patch = *fx.patch;
                r.params = model::toJson(fx);
                return r;
            }
            // A scratch instance cannot know what went wrong in the one playing.
            host::Instance* live = app.instanceOf(*id);
            if (live) {
                // A failed query (a crashed bridge) is reported, not fatal: the
                // description is what an agent needs to decide what to do next.
                auto status = live->pluginStatus();
                if (!status)
                    r.problems = host::statusFailure(status.error());
                else if (!status->isNull()) {
                    r.statusAvailable = true;
                    r.problems = *status->find("problems");
                }
            }
            auto d = app.describeFx(fx, !a.full);
            // A bad saved revision may prevent creating a scratch instance.
            // Still inspect the older version that is actually loaded.
            if (!d && live)
                d = live->info().semantic ? live->describe(!a.full)
                                          : Result<std::string>(live->info().name);
            if (!d) return std::move(d).error();
            r.description = *d;
            if (auto key = app.fxSidechainChannels(fx); key && *key > 0) r.sidechainChannels = *key;
            if (auto latency = app.fxLatencySamples(*id, fx)) r.latencySamples = *latency;
            if (auto notes = app.fxOutputsNotes(*id, fx); notes && *notes) r.outputsNotes = true;
            switch (app.effectiveIsolation(*id)) {
                case model::Isolation::SharedBridge:
                    r.isolationEffective = "shared_bridge";
                    break;
                case model::Isolation::Dedicated:
                    r.isolationEffective = "dedicated";
                    break;
                case model::Isolation::InProcess:
                case model::Isolation::Inherit:
                    r.isolationEffective = "in_process";
                    break;
            }
            if (a.full) {
                auto schema = app.fxSchema(fx);
                if (!schema && live) schema = live->semanticSchema();
                if (schema && schema->isObject() && schema->find("params"))
                    r.params = *schema->find("params");
                else if (auto list = app.fxParams(fx)) {
                    // Not semantic (a plain CLAP or a VST3): its parameters as the
                    // plugin declares them, for fx.param.set by id or name.
                    auto lower = [](std::string t) {
                        for (char& ch : t)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        return t;
                    };
                    const std::string filter = lower(a.filter.value_or(""));
                    json::Value ps = json::Value::array();
                    std::int64_t total = 0;
                    for (const host::ParamInfo& p : *list) {
                        if (p.flags & CLAP_PARAM_IS_HIDDEN) continue;
                        if (!filter.empty() && lower(p.name).find(filter) == std::string::npos)
                            continue;
                        if (static_cast<std::size_t>(total++) >= kMaxListedParams) continue;
                        json::Value e = json::Value::object();
                        e["id"] = static_cast<double>(p.id);
                        e["name"] = p.name;
                        e["min"] = p.min;
                        e["max"] = p.max;
                        e["default"] = p.def;
                        if (auto saved = fx.params.find(p.id); saved != fx.params.end())
                            e["value"] = saved->second;
                        if (p.flags & (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM))
                            e["stepped"] = true;
                        if (p.flags & CLAP_PARAM_IS_READONLY) e["read_only"] = true;
                        ps.push_back(std::move(e));
                    }
                    r.params = std::move(ps);
                    r.paramsTotal = total;
                }
                if (fx.patch) r.patch = *fx.patch;
            }
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxMoveArgs, Empty> c;
        c.name = "fx.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move a plugin within a track chain or to another track.";
        c.description =
            "Works on the track's chain, on its input chain (an input-chain effect stays on its "
            "track) and on a take's chain (take FX stay on their take: index only).";
        c.tags = {"fx"};
        c.args = schema::object<FxMoveArgs>("fx_move_args")
                     .field("fx", &FxMoveArgs::fx, idArg("fx", "Plugin instance"))
                     .field("track", &FxMoveArgs::track, idArg("tr", "Track"))
                     .field("index", &FxMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Move a gain first", j(R"({"fx":"fx_2","index":0})"), j("{}"), "song_with_fx"}};
        c.run = [](const FxMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            std::optional<Id> source;
            bool input = false;
            // The first chain that holds it: a track owns its FX, so a second
            // match would be a broken model, not a second place to remove from.
            for (const auto& e : s.tracks) {
                if (std::find(e.value->chain.begin(), e.value->chain.end(), *id) !=
                    e.value->chain.end()) {
                    source = e.id;
                    break;
                }
                if (std::find(e.value->inputChain.begin(), e.value->inputChain.end(), *id) !=
                    e.value->inputChain.end()) {
                    source = e.id;
                    input = true;
                    break;
                }
            }
            if (!source) {  // a take's chain: reordered in place, never moved out
                for (const auto& e : s.takes) {
                    auto chain = e.value->chain;
                    const auto at = std::find(chain.begin(), chain.end(), *id);
                    if (at == chain.end()) continue;
                    if (a.track)
                        return Error("fx.chain", "a take FX stays on its take", "/track",
                                     "give only index to reorder the take's chain");
                    chain.erase(at);
                    const std::size_t to =
                        std::min<std::size_t>(static_cast<std::size_t>(a.index), chain.size());
                    chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(to), *id);
                    app(ctx).txn().modify<model::Take>(e.id,
                                                       [&](model::Take& tk) { tk.chain = chain; });
                    return Empty{};
                }
                return Error("fx.chain", "this plugin is in no chain", "/fx",
                             "use an fx id returned by fx.add, fx.copy or fx.chain.load");
            }
            Id target = *source;
            if (a.track) {
                auto tr = existing<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                target = *tr;
            }
            auto sourceChain = dangling<model::Track>(s, *source, "/fx");
            if (!sourceChain) return std::move(sourceChain).error();
            if (input) {
                if (target != *source)
                    return Error("fx.chain", "an input-chain effect stays on its track", "/track",
                                 "remove it and add it to the other track's input chain");
                std::vector<Id> chain = (*sourceChain)->inputChain;
                chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
                if (static_cast<std::size_t>(a.index) > chain.size())
                    return Error("fx.index", "index is past the end of the input chain", "/index",
                                 "use an index from 0 to " + std::to_string(chain.size()));
                chain.insert(chain.begin() + a.index, *id);
                app(ctx).txn().modify<model::Track>(
                    *source, [&](model::Track& tr) { tr.inputChain = chain; });
                return Empty{};
            }
            auto chain = (*sourceChain)->chain;
            chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
            std::vector<Id> targetChain = chain;
            if (target != *source) {
                auto destination = dangling<model::Track>(s, target, "/track");
                if (!destination) return std::move(destination).error();
                targetChain = (*destination)->chain;
            }
            if (static_cast<std::size_t>(a.index) > targetChain.size())
                return Error("fx.index", "index is past the end of the destination chain", "/index",
                             "use an index from 0 to " + std::to_string(targetChain.size()));
            targetChain.insert(targetChain.begin() + a.index, *id);
            auto& txn = app(ctx).txn();
            if (target != *source)
                txn.modify<model::Track>(*source, [&](model::Track& tr) { tr.chain = chain; });
            txn.modify<model::Track>(target, [&](model::Track& tr) { tr.chain = targetChain; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxPatchGetArgs, FxPatchResult> c;
        c.name = "fx.patch.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a semantic plugin's saved patch.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchGetArgs>("fx_patch_get_args")
                     .field("fx", &FxPatchGetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("changed_only", &FxPatchGetArgs::changedOnly, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<FxPatchResult>("fx_patch_result")
                       .field("patch", &FxPatchResult::patch, schema::any())
                       .build();
        c.examples = {
            {"Read gain", j(R"({"fx":"fx_2"})"), j(R"({"patch":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxPatchGetArgs& a, Context& ctx) -> Result<FxPatchResult> {
            const auto s = app(ctx).view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const auto& fx = *s.fx.find(*id);
            auto effective = app(ctx).mergedPatch(fx, json::Value::object(), false);
            if (!effective) return std::move(effective).error();
            if (a.changedOnly) {
                model::FxInstance defaults = fx;
                defaults.patch.reset();
                auto baseline = app(ctx).mergedPatch(defaults, json::Value::object(), false);
                if (!baseline) return std::move(baseline).error();
                auto diff = json::createMergePatch(*baseline, *effective);
                if (!diff) return std::move(diff).error();
                return FxPatchResult{std::move(*diff)};
            }
            return FxPatchResult{std::move(*effective)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxPatchListArgs, FxPatchListResult> c;
        c.name = "fx.patch.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List an instance's read-only factory patches, by name.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchListArgs>("fx_patch_list_args")
                     .field("fx", &FxPatchListArgs::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = schema::object<FxPatchListResult>("fx_patch_list_result")
                       .field("patches", &FxPatchListResult::patches, schema::any())
                       .build();
        c.examples = {{"List factory patches", j(R"({"fx":"fx_2"})"), j(R"({"patches":[]})"),
                       "song_with_fx"}};
        c.run = [](const FxPatchListArgs& a, Context& ctx) -> Result<FxPatchListResult> {
            const auto s = app(ctx).view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            auto patches = app(ctx).fxFactoryPatches(*id, *s.fx.find(*id));
            if (!patches) return std::move(patches).error();
            return FxPatchListResult{std::move(*patches)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxPatchSetArgs, FxPatchResult> c;
        c.name = "fx.patch.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace a semantic patch, or apply a factory patch by name.";
        c.description =
            "Give exactly one of patch or name. Factory names come from fx.patch.list; omitted "
            "parameters take defaults.";
        c.tags = {"fx", "patch"};
        c.args =
            schema::object<FxPatchSetArgs>("fx_patch_set_args")
                .field("fx", &FxPatchSetArgs::fx, idArg("fx", "Plugin instance"))
                .field("patch", &FxPatchSetArgs::patch, schema::any())
                .field("name", &FxPatchSetArgs::name, schema::string().minLength(1).maxLength(64))
                .build();
        c.result = schema::object<FxPatchResult>("fx_patch_result")
                       .field("patch", &FxPatchResult::patch, schema::any())
                       .build();
        c.examples = {
            {"Replace gain",
             j(R"({"fx":"fx_2","patch":{"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}}})"),
             j(R"({"patch":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxPatchSetArgs& a, Context& ctx) -> Result<FxPatchResult> {
            if (a.patch.has_value() == a.name.has_value())
                return Error("fx.patch_args", "give exactly one of patch or name", "/patch",
                             "provide a complete patch, or a factory name from fx.patch.list");
            const auto s = app(ctx).view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            json::Value document;
            if (a.patch)
                document = *a.patch;
            else {
                auto patches = app(ctx).fxFactoryPatches(*id, *s.fx.find(*id));
                if (!patches) return std::move(patches).error();
                std::string names;
                for (const auto& candidate : patches->asArray()) {
                    const auto& name = candidate.find("name")->asString();
                    if (name == *a.name) document = candidate;
                    names += (names.empty() ? "" : ", ") + name;
                }
                if (document.isNull())
                    return Error("fx.unknown_factory_patch",
                                 "no factory patch named '" + *a.name + "'", "/name",
                                 names.empty()
                                     ? "this plugin has no factory patches; provide patch instead"
                                     : "choose a name from fx.patch.list: " + names);
            }
            auto patch = app(ctx).mergedPatch(*s.fx.find(*id), document, true);
            if (!patch) {
                Error e = std::move(patch).error();
                e.path = "/patch" + e.path;
                return e;
            }
            app(ctx).txn().modify<model::FxInstance>(
                *id, [&](model::FxInstance& fx) { fx.patch = *patch; });
            return FxPatchResult{std::move(*patch)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxParamGetArgs, FxParamResult> c;
        c.name = "fx.param.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read one saved plugin parameter by semantic key or numeric CLAP id.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxParamGetArgs>("fx_param_get_args")
                     .field("fx", &FxParamGetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("param", &FxParamGetArgs::param, schema::string().minLength(1))
                     .build();
        c.result = schema::object<FxParamResult>("fx_param_result")
                       .field("value", &FxParamResult::value, schema::any())
                       .build();
        c.examples = {{"Read gain", j(R"({"fx":"fx_2","param":"gain"})"), j(R"({"value":"<any>"})"),
                       "song_with_fx"}};
        c.run = [](const FxParamGetArgs& a, Context& ctx) -> Result<FxParamResult> {
            const auto s = app(ctx).view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const auto& fx = *s.fx.find(*id);
            auto effective = app(ctx).mergedPatch(fx, json::Value::object(), false);
            if (!effective && effective.error().code != "fx.not_semantic")
                return std::move(effective).error();  // a broken patch is the cause
            const auto* params = effective ? effective->find("params") : nullptr;
            if (params && params->isObject())
                if (const auto* value = params->find(a.param)) return FxParamResult{*value};
            for (const auto& [key, value] : fx.params)
                if (std::to_string(key) == a.param) return FxParamResult{json::Value(value)};
            return Error("fx.param", "no saved parameter " + a.param, "/param",
                         "use fx.describe {full:true} for parameter names");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxMetersArgs, FxMetersResult> c;
        c.name = "fx.meters";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a plugin's meters and readouts, playing now or as a render ended.";
        c.description =
            "The read-only parameters of a plugin: a semantic plugin's by patch key in physical "
            "units (an analyzer's loudness, a reverb's RT60, a gate's suggested threshold), any "
            "other plugin's by name as CLAP values. from live (the default): the playing "
            "instance's, which move while it processes (playback, monitoring); source none "
            "while no instance runs (start the engine). from render: as the last render.mix "
            "ended, which renders through instances of its own; source none until a render.mix "
            "has included this plugin.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxMetersArgs>("fx_meters_args")
                     .field("fx", &FxMetersArgs::fx, idArg("fx", "Plugin instance"))
                     .field("from", &FxMetersArgs::from,
                            schema::enumeration<MetersFrom>(
                                {{MetersFrom::Live, "live"}, {MetersFrom::Render, "render"}})
                                .doc("live: the playing instance; render: the last render.mix"))
                     .defaultValue(MetersFrom::Live)
                     .build();
        c.result = schema::object<FxMetersResult>("fx_meters")
                       .field("source", &FxMetersResult::source,
                              schema::string().doc("live, render, or none (nothing to read yet)"))
                       .field("meters", &FxMetersResult::meters,
                              schema::any().doc("Read-only values by patch key (physical) or name"))
                       .build();
        c.examples = {{"Read meters", j(R"({"fx":"fx_2"})"),
                       j(R"({"source":"<any>","meters":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxMetersArgs& a, Context& ctx) -> Result<FxMetersResult> {
            const auto s = app(ctx).view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            FxMetersResult r;
            r.source = "none";
            r.meters = json::Value::object();
            if (a.from == MetersFrom::Render) {
                if (const json::Value* rendered = app(ctx).renderMeters(*id)) {
                    r.source = "render";
                    r.meters = *rendered;
                }
                return r;
            }
            host::Instance* live = app(ctx).instanceOf(*id);
            if (!live || live->failed()) return r;
            auto readouts = instanceReadouts(*live);
            if (!readouts) return std::move(readouts).error();
            r.source = "live";
            r.meters = std::move(*readouts);
            return r;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
