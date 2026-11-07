// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "command_validation.h"
#include "supple/model/audio_warp.h"
#include "supple/model/numeric.h"
#include "supple/time/timeline.h"
// Additional session edits and queries from session-model §7.
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "supple/engine/app.h"
#include "supple/model/env_shape.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
json::Value j(std::string_view s) { return json::parse(s).value(); }

// Search the same immutable source samples that playback reads. No decode,
// stretching or cache mutation belongs in this edit handler.
Result<double> priorZeroCrossing(EngineApp& owner, const model::Session& s, const model::Item& item,
                                 double cursor) {
    const auto* take = item.activeTake ? s.takes.find(*item.activeTake) : nullptr;
    if (!take || !take->source || take->source->kind != model::Kind::Media)
        return Error("item.split_not_audio", "zero-crossing split needs an active audio take",
                     "/item", "use snap none to split MIDI or empty items at the cursor");
    const auto span = model::itemSpan(s, item);
    if (!(cursor > span.startSec && cursor < span.endSec))
        return Error("item.split_range", "split must be inside the item", "/at",
                     "choose a beat strictly after the start and before the end");
    const auto& prepared = owner.media().sources();
    const Error pending("item.split_source_pending", "current source audio is not prepared",
                        "/item",
                        "start the engine (engine.start with backend dummy is sufficient), "
                        "wait for source preparation and retry");
    if (!prepared || prepared->session.project->mediaDir != s.project->mediaDir ||
        (owner.engine() && prepared->sampleRate != owner.engine()->config().sampleRate))
        return pending;
    const auto found = prepared->takes.find(*item.activeTake);
    if (found == prepared->takes.end()) return pending;
    const auto& source = found->second;
    // Per-take provenance lets a batch split several items from one snapshot.
    // Scalar variants do not depend on item placement or source offset.
    auto matches = [&](Id id) {
        const auto* now = s.takes.find(id);
        const auto* was = prepared->session.takes.find(id);
        if (!now || !was || now->source != was->source || now->playrate != was->playrate ||
            now->pitch != was->pitch || now->preservePitch != was->preservePitch ||
            now->pitchMode != was->pitchMode || now->stretchMarkers != was->stretchMarkers ||
            now->warpPeers != was->warpPeers)
            return false;
        const auto* media = now->source ? s.media.find(*now->source) : nullptr;
        const auto* oldMedia = was->source ? prepared->session.media.find(*was->source) : nullptr;
        return media && oldMedia && model::toJson(*media) == model::toJson(*oldMedia) &&
               (!source.itemLocal || now->startOffset == was->startOffset);
    };
    if (!matches(*item.activeTake)) return pending;
    if (source.itemLocal) {
        const model::Item* original = nullptr;
        for (const auto& entry : prepared->session.items)
            if (entry.value->activeTake == item.activeTake) original = entry.value.get();
        if (!original || item.loop != original->loop) return pending;
        const auto previous = model::itemSpan(prepared->session, *original);
        if (span.endSec - span.startSec != previous.endSec - previous.startSec) return pending;
        for (auto peer : take->warpPeers)
            if (!matches(peer)) return pending;
    } else if (model::activeWarp(*take) && !item.loop) {
        return pending;
    }
    if (source.error)
        return Error("item.split_source", source.error->message, "/item",
                     "relink or repair the audio source, then retry after source preparation");
    if (!source.data || !(prepared->sampleRate > 0)) return pending;
    const auto& audio = *source.data;
    const double rate = prepared->sampleRate;
    const auto frames = static_cast<std::int64_t>(audio.frames());
    if (audio.channels.empty() || frames == 0) return cursor;
    const MediaVariant variant{take->playrate, take->pitch, take->preservePitch, take->pitchMode};
    const auto offset =
        source.itemLocal ? 0 : time::samplesAt(take->startOffset, rate / variant.speed());
    const auto start = time::samplesAt(span.startSec, rate);
    const auto end = time::samplesAt(span.endSec, rate);
    const auto first =
        std::max(start + 1, static_cast<std::int64_t>(std::ceil((cursor - .1) * rate)));
    const auto last = std::min(end - 1, static_cast<std::int64_t>(std::floor(cursor * rate)));
    auto sample = [&](std::int64_t frame, std::size_t channel) {
        auto at = offset + frame - start;
        if (item.loop) at = ((at % frames) + frames) % frames;
        if (at < 0 || at >= frames) return 0.f;
        const auto n = static_cast<std::size_t>(at);
        float l = audio.channels[0][n];
        float r = audio.channels[audio.channelCount() > 1 ? 1 : 0][n];
        switch (take->channelMode) {
            case model::ChannelMode::Normal:
                break;
            case model::ChannelMode::ReverseStereo:
                std::swap(l, r);
                break;
            case model::ChannelMode::MonoMix:
                l = r = .5f * (l + r);
                break;
            case model::ChannelMode::Left:
                r = l;
                break;
            case model::ChannelMode::Right:
                l = r;
                break;
        }
        return channel == 0 ? l : r;
    };
    double energy[2]{};
    for (auto n = first; n <= last; ++n)
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double x = static_cast<double>(sample(n, ch));
            if (std::isfinite(x)) energy[ch] += x * x;
        }
    const std::size_t channel = energy[1] > energy[0] ? 1 : 0;
    // A fractional cursor in silence is already at zero; keep it exact.
    if (first <= last && sample(last, channel) == 0 && sample(last + 1, channel) == 0)
        return cursor;
    for (auto n = last; n >= first; --n) {
        const float x = sample(n, channel), previous = sample(n - 1, channel);
        const double at = static_cast<double>(n) / rate;
        if (at > span.startSec && at < span.endSec && at <= cursor && std::isfinite(x) &&
            std::isfinite(previous) &&
            (x == 0 || (previous < 0 && x > 0) || (previous > 0 && x < 0)))
            return at;
    }
    return cursor;
}

template <class T>
Result<Id> require(const model::Session& s, const std::string& text, std::string path) {
    auto id = Id::parse(text);
    if (id && id->kind == model::Traits<T>::kind && model::Traits<T>::map(s).contains(*id))
        return *id;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown", "no " + text,
                 std::move(path), "use a live id from the corresponding list command");
}

auto idSchema(const char* prefix) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$", "object id");
}

// A model entry the session itself refers to (an item's takes, a take's chain):
// an id the model hands back must resolve, and a dangling one is a structured
// error rather than a crash.
template <class T>
Result<const T*> dangling(const model::Session& s, Id id, std::string path) {
    const T* found = model::Traits<T>::map(s).find(id);
    if (found) return found;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown",
                 id.str() + " is missing from the model", std::move(path),
                 "the session refers to it; reload the project or fix the file");
}

struct Empty {};
auto emptySchema() { return schema::object<Empty>("empty").build(); }
struct IdResult {
    std::string id;
};
auto idResultSchema() {
    return schema::object<IdResult>("created_id")
        .field("id", &IdResult::id, schema::string())
        .build();
}

struct RouteArgs {
    std::string from;
    std::optional<std::string> to, fx;
    std::optional<model::Tap> tap;
    std::optional<double> volumeDb;
    std::optional<model::RoutePan> pan;
    std::optional<bool> mute, mono, phaseInvert;
    std::optional<model::ChannelMap> channels;
    std::optional<model::MidiRouting> midi;
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
};
struct RouteRef {
    std::string route;
};
struct TrackMoveArgs {
    std::string track;
    std::optional<std::string> parent;
    std::optional<std::int64_t> index;
};
struct RouteEntry {
    std::string id, from, to, fx, tap;
    double volumeDb = 0;
    model::RoutePan pan;
    bool mute = false, mono = false, phaseInvert = false;
    model::ChannelMap channels;
    std::optional<model::MidiRouting> midi;
};
struct RouteList {
    std::vector<RouteEntry> routes;
};

struct RulerArgs {
    std::optional<double> pos, start, end;
    std::optional<std::string> name, color;
};
struct RulerSetArgs {
    std::string id;
    std::optional<double> pos, start, end;
    std::optional<std::string> name, color;
};
struct RulerRef {
    std::string id;
};
struct RulerEntry {
    model::Timebase timebase = model::Timebase::Beats;
    std::string id, name, color;
    std::optional<double> pos, start, end;
};
struct RulerList {
    std::vector<RulerEntry> entries;
};

struct PluginArgs {
    std::optional<std::string> filter;
};
struct PluginEntry {
    std::string id, name, vendor, format;
    bool instrument = false, semantic = false;
    std::string path;
    std::optional<bool> shadowed;  // an earlier location on the plugin path has this id
    std::vector<std::string> features;
};
struct ScanReason {
    std::string code, message, path, hint;
    std::optional<std::vector<json::Value>> details;
};
ScanReason scanReason(const Error& e) {
    ScanReason r{e.code, e.message, e.path, e.hint, {}};
    if (!e.details.empty()) {
        auto encode = [](auto&& self, const Error& error) -> json::Value {
            json::Value v = json::Value::object();
            v["code"] = error.code;
            v["message"] = error.message;
            v["path"] = error.path;
            v["hint"] = error.hint;
            if (!error.details.empty()) {
                v["details"] = json::Value::array();
                for (const auto& d : error.details) v["details"].push_back(self(self, d));
            }
            return v;
        };
        r.details = std::vector<json::Value>{};
        for (const auto& d : e.details) r.details->push_back(encode(encode, d));
    }
    return r;
}
struct ScanFailure {
    std::string module;
    ScanReason reason;
    bool cached = false;
};
struct ScanReport {
    std::vector<ScanFailure> failures;
    std::vector<ScanReason> warnings;
    std::uint64_t cachedModules = 0;
};
ScanReport scanReport(const host::ScanReport& source) {
    ScanReport r;
    for (const auto& f : source.failures)
        r.failures.push_back({f.module.string(), scanReason(f.reason), f.cached});
    for (const auto& w : source.warnings) r.warnings.push_back(scanReason(w));
    r.cachedModules = source.cachedModules;
    return r;
}
auto scanReportSchema() {
    auto reason = schema::object<ScanReason>("scan_reason")
                      .field("code", &ScanReason::code, schema::string())
                      .field("message", &ScanReason::message, schema::string())
                      .field("path", &ScanReason::path, schema::string())
                      .field("hint", &ScanReason::hint, schema::string())
                      .field("details", &ScanReason::details, schema::arrayOf(schema::any()))
                      .build();
    auto failure = schema::object<ScanFailure>("scan_failure")
                       .field("module", &ScanFailure::module, schema::string())
                       .field("reason", &ScanFailure::reason, reason)
                       .field("cached", &ScanFailure::cached, schema::boolean())
                       .build();
    return schema::object<ScanReport>("scan_report")
        .field("failures", &ScanReport::failures, schema::arrayOf(failure))
        .field("warnings", &ScanReport::warnings, schema::arrayOf(reason))
        .field("cached_modules", &ScanReport::cachedModules, schema::integerOf<std::uint64_t>())
        .build();
}
struct PluginList {
    std::vector<PluginEntry> plugins;
    ScanReport scan;
    std::optional<std::string> job;  // plugin.scan {wait: false}: the background scan
};
struct ScanArgs {
    std::optional<bool> wait;
};
struct TimeArgs {
    std::optional<double> seconds, beat;
    std::optional<std::int64_t> bar;
};
struct TimeResult {
    double seconds = 0, beat = 0;
    std::int64_t bar = 1, beatInBar = 1;
    double fraction = 0;
};

struct ItemSetArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> position, length, gainDb, fadeIn, fadeOut;
    std::optional<model::FadeShape> fadeInShape, fadeOutShape;
    std::optional<std::int64_t> bar;
    std::optional<bool> mute, loop, locked;
    std::optional<std::string> name, color;
    std::optional<model::ItemTimebase> timebase;
};
struct ItemDuplicateResult {
    std::string id;
    std::vector<std::string> linked;
};
struct ItemDuplicateArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> to;
};
struct PastePlacement {
    std::string item, track;
    double position = 0;
};
struct ItemPasteArgs {
    std::optional<std::int64_t> version;
    std::vector<PastePlacement> items;
};
struct ItemPasteResult {
    std::vector<std::string> items;
};
struct ItemSplitArgs {
    bool bypassGroups = false;
    std::string item;
    double at = 0;
    std::string snap = "none";
};
struct SplitResult {
    std::string left, right;
    std::vector<std::string> linkedLeft, linkedRight;
};
struct TakeActivateArgs {
    std::string item;
    std::optional<std::string> take;
    std::optional<std::int64_t> index;  // 0-based, in the item's take order
};
struct ItemQuery {
    std::string item;
};
struct TakeAddArgs {
    std::string item;
    std::optional<std::string> from;
};
struct TakeSetArgs {
    std::string take;
    std::optional<double> gainDb, pan, startOffset, playrate, pitch, rebaseSeconds;
    std::optional<model::ChannelMode> channelMode;
    std::optional<std::string> name, pitchMode;
    std::optional<bool> preservePitch;
};
struct TakeEntry {
    std::string id, name, source;
    bool active = false;
    double gainDb = 0, pan = 0, startOffset = 0;
    double playrate = 1, pitch = 0;
    bool preservePitch = true;
    std::string pitchMode;
    json::Value markers = json::Value::array(), hits = json::Value(), peers = json::Value::array();
    bool hitsStale = false;
};
struct TakeList {
    std::vector<TakeEntry> takes;
};
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
    if constexpr (std::is_same_v<T, RouteSetArgs>) b.field("route", &T::route, idSchema("sd"));
    b.field("from", &T::from, routeEndArg());
    b.field("to", &T::to, routeEndArg());
    b.field("midi", &T::midi,
            midiRouteArgs().doc("Note Out MIDI routing; {} means every channel unchanged. Channel "
                                "values 0..16: from 0 = all, to 0 = original."));
    b.field("fx", &T::fx,
            idSchema("fx").doc("Feed this plug-in's sidechain (key) input instead of a track's "
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

std::optional<std::vector<Id>> routeCycle(const model::Session& s, Id from, Id to,
                                          std::optional<Id> ignored = {}) {
    if (from == to) return std::vector<Id>{from, to};
    std::map<Id, std::vector<Id>> edges;
    for (const auto& e : s.tracks)
        if (e.value->parent && e.value->parentSend.enabled) edges[e.id].push_back(*e.value->parent);
    // Racks are nodes too (ADR-0062): a send reaches an In module's rack, an
    // Out module's rack feeds its channel.
    auto rackOf = [&](Id module) -> std::optional<Id> {
        const model::RackModule* m = s.modules.find(module);
        return m ? std::optional<Id>(m->rack) : std::nullopt;
    };
    for (const auto& e : s.routes) {
        if (e.id == ignored) continue;
        const auto source =
            e.value->from.module ? rackOf(*e.value->from.module) : e.value->from.track;
        const auto target = e.value->to.module ? rackOf(*e.value->to.module)
                            : e.value->to.fx   ? model::trackOfFx(s, *e.value->to.fx)
                                               : e.value->to.track;
        if (source && target) edges[*source].push_back(*target);
    }
    std::set<Id> seen;
    std::vector<Id> path{to};
    std::function<bool(Id)> walk = [&](Id cur) {
        if (cur == from) return true;
        if (!seen.insert(cur).second) return false;
        for (Id next : edges[cur]) {
            path.push_back(next);
            if (walk(next)) return true;
            path.pop_back();
        }
        return false;
    };
    if (!walk(to)) return {};
    path.push_back(to);
    return path;
}

// Is `track` a descendant of `ancestor` in the folder nesting (children lists)?
// Moving a track under one of its own descendants would make the tree cyclic.
bool nestedUnder(const model::Session& s, Id ancestor, Id track) {
    std::vector<Id> stack{track};
    std::set<Id> seen;
    while (!stack.empty()) {
        const Id cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        if (cur == ancestor) return true;
        const model::Track* found = s.tracks.find(cur);
        if (!found) continue;
        for (Id child : found->children) stack.push_back(child);
    }
    return false;
}

Result<model::Route> buildRoute(const RouteArgs& a, const model::Session& s, EngineApp& app,
                                const model::Route* old = nullptr, std::optional<Id> ignored = {}) {
    model::Route r = old ? *old : model::Route{};
    if (a.midi) r.midi = *a.midi;
    if (!a.from.empty()) {
        r.from = model::RouteEnd{};
        if (a.from.starts_with("mo_")) {
            auto from = require<model::RackModule>(s, a.from, "/from");
            if (!from) return std::move(from).error();
            r.from.module = *from;
        } else {
            auto from = require<model::Track>(s, a.from, "/from");
            if (!from) return std::move(from).error();
            r.from.track = *from;
        }
    }
    if (a.fx) {  // a key route (ADR-0018)
        auto fx = require<model::FxInstance>(s, *a.fx, "/fx");
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
        auto to = require<model::RackModule>(s, *a.to, "/to");
        if (!to) return std::move(to).error();
        if (s.modules.find(*to)->type != model::kInModule)
            return Error("route.endpoint", *a.to + " is not an In module", "/to",
                         "sends reach a rack through its In modules (rack.get)");
        r.to = model::RouteEnd{};
        r.to.module = *to;
    } else if (a.to) {
        auto to = require<model::Track>(s, *a.to, "/to");
        if (!to) return std::move(to).error();
        r.to = model::RouteEnd{};
        r.to.track = *to;
    }
    if (r.from.module) {
        const auto* m = s.modules.find(*r.from.module);
        const auto* to = r.to.track ? s.tracks.find(*r.to.track) : nullptr;
        if (!m || m->type != model::kNotesOutModule || !r.midi || !to ||
            to->type != model::StripType::Channel)
            return Error("route.endpoint", "a Note Out sends MIDI to a channel", "/from",
                         "use from: {module: mo_N}, to: {track: tr_N}, midi: {}");
        if (a.tap || a.volumeDb || a.pan || a.mono || a.phaseInvert || a.channels || a.fx)
            return Error("route.midi", "audio send controls do not apply to a Note Out", "/midi",
                         "use mute and MIDI channel routing; cable velocity to set note strength");
        if (auto cycle = routeCycle(s, m->rack, *r.to.track, ignored))
            return Error("route.cycle", "the MIDI route would feed back through a rack", "/to",
                         "choose a channel that does not feed this rack");
        if (a.mute) r.mute = *a.mute;
        return r;
    }
    if (r.midi)
        return Error("route.midi", "MIDI sends require a Note Out source", "/midi",
                     "add a notes_out module and route its module id");
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
        std::string names;
        for (Id id : *cycle) names += (names.empty() ? "" : " -> ") + id.str();
        return Error("route.cycle", "route creates a cycle: " + names, "/to",
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

schema::Schema<model::FadeShape> fadeShapeSchema() {
    return schema::enumeration<model::FadeShape>({{model::FadeShape::Linear, "linear"},
                                                  {model::FadeShape::Slow, "slow"},
                                                  {model::FadeShape::Fast, "fast"},
                                                  {model::FadeShape::SlowFast, "slow_fast"},
                                                  {model::FadeShape::FastSlow, "fast_slow"},
                                                  {model::FadeShape::Cos, "cos"},
                                                  {model::FadeShape::Exp, "exp"}});
}

template <class T, bool Region, bool WithId>
schema::Schema<T> rulerArgsSchema(const char* name) {
    auto b = schema::object<T>(name);
    if constexpr (WithId) b.field("id", &T::id, idSchema(Region ? "rg" : "mk"));
    if constexpr (Region) {
        b.field("start", &T::start, schema::number().min(0));
        b.field("end", &T::end, schema::number().min(0));
    } else {
        b.field("pos", &T::pos, schema::number().min(0));
    }
    b.field("name", &T::name, schema::string());
    b.field("color", &T::color, schema::string());
    return b.build();
}

auto rulerListSchema(bool region) {
    auto entry = schema::object<RulerEntry>("ruler_entry")
                     .field("id", &RulerEntry::id, schema::string())
                     .field("timebase", &RulerEntry::timebase, model::timebaseSchema())
                     .field("name", &RulerEntry::name, schema::string())
                     .field("color", &RulerEntry::color, schema::string())
                     .field("pos", &RulerEntry::pos, schema::number())
                     .field("start", &RulerEntry::start, schema::number())
                     .field("end", &RulerEntry::end, schema::number())
                     .build();
    return schema::object<RulerList>("ruler_list")
        .field(region ? "regions" : "markers", &RulerList::entries, schema::arrayOf(entry))
        .build();
}

}  // namespace

void registerExtraCommands(cmd::Registry& registry) {
    // Each command is built in its own immediately-invoked lambda, so an
    // unoptimized build reuses one frame instead of stacking every command's
    // locals (Windows' 1 MB main stack overflowed).
    [&] {
        Command<RouteArgs, IdResult> c;
        c.name = "route.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Send audio between strips, or a Note Out to a track's instrument.";
        c.description =
            "The destination must not feed the source. Master accepts routes. With fx instead of "
            "to, the send feeds that plug-in's sidechain (key) input — e.g. a kick keying a "
            "compressor on the bass; it is delay-compensated, and soloing the keyed track does not "
            "stop the key. A Note Out uses from: {module: mo_N}, to: {track: tr_N}, midi: {} "
            "(all channels unchanged); from_channel filters and to_channel remaps, 0..16 "
            "with 0 meaning all/original. Hardware routing is not supported yet.";
        c.tags = {"route", "mix"};
        c.args = routeArgsSchema<RouteArgs>("route_add_args");
        c.result = idResultSchema();
        c.errors = {{"route.cycle", "the send would feed back", "choose another destination"},
                    {"route.no_sidechain", "the plug-in has no key input", "pick another plug-in"},
                    {"route.endpoint", "a Note Out needs a channel destination and MIDI routing",
                     "use from: {module: mo_N}, to: {track: tr_N}, midi: {}"},
                    {"route.midi", "MIDI routing or audio controls do not apply to these endpoints",
                     "use a Note Out source for MIDI and omit audio send controls"}};
        c.examples = {
            {"Send a track to the master", j(R"({"from":"tr_1","to":"tr_0","volume_db":"-6 dB"})"),
             j(R"({"id":"sd_1"})"), "song"},
            {"Key a plug-in from a track, before its fader",
             j(R"({"from":"tr_1","fx":"fx_1","tap":"pre_fader"})"), j(R"({"id":"sd_1"})"),
             "song_with_keymix"},
            {"Play a channel's instrument from Note Out",
             j(R"({"from":{"module":"mo_4"},"to":{"track":"tr_2"},"midi":{}})"),
             j(R"({"id":"sd_2"})"), "song_with_note_out"}};
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
        c.exposure = cmd::Exposure::McpTool;
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
                    {"route.endpoint", "a Note Out needs a channel destination and MIDI routing",
                     "route Note Out MIDI to a channel"},
                    {"route.midi", "audio controls do not apply to MIDI routes",
                     "use mute and MIDI channel routing"}};
        c.run = [](const RouteSetArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Route>(s, a.route, "/route");
            if (!id) return std::move(id).error();
            RouteArgs edits{a.from.value_or(""), a.to,       a.fx,   a.tap,
                            a.volumeDb,          a.pan,      a.mute, a.mono,
                            a.phaseInvert,       a.channels, a.midi};
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
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a send.";
        c.tags = {"route"};
        c.args = schema::object<RouteRef>("route_ref")
                     .field("route", &RouteRef::route, idSchema("sd"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"route":"sd_1"})"), j("{}"), "song_with_route"}};
        c.run = [](const RouteRef& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Route>(s, a.route, "/route");
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
        c.exposure = cmd::Exposure::McpTool;
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
                                      r.midi});
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        struct LogArgs {
            std::optional<std::int64_t> limit, since;
            bool queries = false;
            std::optional<std::string> origin;
        };
        Command<LogArgs, json::Value> c;
        c.name = "log.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Recent commands with origin, arguments and outcome (the command log).";
        c.description =
            "Newest last. Who did what: origin is gui, agent:<client>/<model>, mcp:<client>, "
            "lua:<script>, rpc. queries: include read-only commands (left out by default). since: "
            "only entries after this seq (poll with the last seq seen). origin: entries whose "
            "origin starts with this. The log keeps the last 2000 commands.";
        c.tags = {"history"};
        c.args = schema::object<LogArgs>("log_list_args")
                     .field("limit", &LogArgs::limit, schema::integer().range(1, 2000))
                     .field("since", &LogArgs::since, schema::integer().min(0))
                     .field("queries", &LogArgs::queries, schema::boolean())
                     .defaultValue(false)
                     .field("origin", &LogArgs::origin, schema::string())
                     .build();
        c.result = schema::any();
        c.examples = {{"The last commands", j(R"({"limit": 5})"),
                       j(R"({"entries":"<any>","last_seq":"<any>"})"), "song"}};
        c.run = [](const LogArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& engine = app(ctx);
            const cmd::Registry& commands = ctx.service<const cmd::Registry>();
            static constexpr const char* kTypes[] = {"ui",   "lua",      "mcp",  "rpc",
                                                     "test", "internal", "agent"};
            auto originOf = [](const cmd::Origin& o) {
                std::string s = kTypes[static_cast<int>(o.type)];
                if (!o.client.empty()) s += ":" + o.client;
                if (!o.agent.empty()) s += "/" + o.agent;
                return s;
            };
            std::vector<const cmd::LogRecord*> picked;
            const auto& records = engine.records();
            for (auto it = records.rbegin(); it != records.rend(); ++it) {
                if (a.since && it->seq <= static_cast<std::uint64_t>(*a.since)) break;
                if (!a.queries) {
                    const cmd::CommandInfo* info = commands.find(it->command, it->version);
                    if (info && info->kind == Kind::Query) continue;
                }
                const std::string origin = originOf(it->origin);
                if (a.origin && origin.rfind(*a.origin, 0) != 0) continue;
                picked.push_back(&*it);
                if (picked.size() >= static_cast<std::size_t>(a.limit.value_or(50))) break;
            }
            json::Value entries = json::Value::array();
            for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
                const cmd::LogRecord& r = **it;
                json::Value e = json::Value::object();
                e["seq"] = static_cast<std::int64_t>(r.seq);
                e["time_ms"] = static_cast<std::int64_t>(r.time_ms);
                e["origin"] = originOf(r.origin);
                e["command"] = r.command;
                e["args"] = r.args;
                e["ok"] = r.ok;
                if (!r.ok)
                    e["error"] = r.errorCode;
                else if (!r.resultDigest.empty())
                    e["result"] = r.resultDigest;
                e["duration_ms"] = std::round(static_cast<double>(r.duration_us) / 10.0) / 100.0;
                entries.push_back(std::move(e));
            }
            json::Value out = json::Value::object();
            out["entries"] = std::move(entries);
            out["last_seq"] = static_cast<std::int64_t>(records.empty() ? 0 : records.back().seq);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<cmd::NoArgs, json::Value> c;
        c.name = "route.matrix";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "What can be routed where: strips, plug-in keys, sends and blocked pairs.";
        c.description =
            "sources: strips in tree order (depth, parent, parent_send). destinations: strips and "
            "plug-in keys (fx with a sidechain input). A pair is blocked when the source's rule "
            "applies (masters feed only keys), the destination's rule applies (channels take only "
            "hardware audio), it is the same strip, or it is listed in cycles (the destination "
            "already feeds the source). sends: existing routes per pair. Otherwise route.add "
            "accepts the pair. A source's channels past 2 are auxiliary (ADR-0020); "
            "channel_names names those a multi-out plug-in writes ({\"3\": \"Kick\"}), and "
            "route.add {channels: {from: [3, 4], to: [1, 2]}} sends that pair.";
        c.tags = {"route"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        c.result = schema::any();
        c.examples = {
            {"A song's routing", j("{}"),
             j(R"({"sources":"<any>","destinations":"<any>","sends":"<any>","cycles":"<any>"})"),
             "song"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<json::Value> {
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
                json::Value src = json::Value::object();
                src["id"] = id.str();
                src["name"] = model::trackLabel(s, id);
                src["type"] = std::string(model::stripTypeName(t.type));
                src["depth"] = depth;
                if (t.parent) {
                    src["parent"] = t.parent->str();
                    src["parent_send"] = t.parentSend.enabled;
                }
                src["channels"] = t.channels;
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
                json::Value dst = json::Value::object();
                dst["id"] = id.str();
                dst["name"] = model::trackLabel(s, id);
                dst["kind"] = "track";
                dst["type"] = std::string(model::stripTypeName(t.type));
                if (t.type == model::StripType::Channel)
                    dst["rule"] =
                        rule("route.strip_type", "a channel accepts only hardware audio",
                             "add a buss and route the channels into it; use fx for a key");
                destinations.push_back(std::move(dst));
                for (Id fx : t.chain) {
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
            }
            json::Value sends = json::Value::array();
            for (const auto& e : s.routes) {
                const model::Route& r = *e.value;
                if (!r.from.track) continue;
                json::Value v = json::Value::object();
                v["id"] = e.id.str();
                v["from"] = r.from.track->str();
                v["to"] = r.to.fx ? r.to.fx->str() : r.to.track ? r.to.track->str() : "";
                sends.push_back(std::move(v));
            }
            // A route from -> d feeds back when d (or the key's track) already
            // reaches from: listed per pair, with the path.
            json::Value cycles = json::Value::array();
            for (const json::Value& d : destinations.asArray()) {
                const std::string did = d.find("id")->asString();
                const bool key = d.find("kind")->asString() == "key";
                const Id target = *Id::parse(key ? d.find("track")->asString() : did);
                if (!key && s.tracks.find(target)->type == model::StripType::Channel)
                    continue;  // type rule
                for (const auto& [from, depth] : order) {
                    if (from == target) continue;  // the same strip
                    if (!key && s.tracks.find(from)->type == model::StripType::Master) continue;
                    if (auto path = routeCycle(s, from, target)) {
                        json::Value v = json::Value::object();
                        v["from"] = from.str();
                        v["to"] = did;
                        std::string names;
                        for (Id at : *path) names += (names.empty() ? "" : " -> ") + at.str();
                        v["path"] = names;
                        cycles.push_back(std::move(v));
                    }
                }
            }
            json::Value out = json::Value::object();
            out["sources"] = std::move(sources);
            out["destinations"] = std::move(destinations);
            out["sends"] = std::move(sends);
            out["cycles"] = std::move(cycles);
            return out;
        };
        addChecked(registry, std::move(c));
    }();

    [&] {
        Command<TrackMoveArgs, Empty> c;
        c.name = "track.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move a track under another track or reorder it among its siblings.";
        c.description =
            "parent defaults to the track's current parent (a pure reorder); index defaults to "
            "the end of the destination's children. A track cannot move under itself or one of "
            "its own descendants.";
        c.tags = {"track"};
        c.args = schema::object<TrackMoveArgs>("track_move_args")
                     .field("track", &TrackMoveArgs::track, idSchema("tr"))
                     .field("parent", &TrackMoveArgs::parent, idSchema("tr"))
                     .field("index", &TrackMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.errors = {{"tr.cycle", "the destination is inside the track", "choose another parent"}};
        c.examples = {{"Move a track to the master", j(R"({"track":"tr_1","parent":"tr_0"})"),
                       j("{}"), "song"}};
        c.run = [](const TrackMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Track>(s, a.track, "/track");
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
                auto parent = require<model::Track>(s, *a.parent, "/parent");
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
                    std::string names;
                    for (Id at : *cycle) names += (names.empty() ? "" : " -> ") + at.str();
                    return Error("tr.cycle", "the move creates a cycle: " + names, "/parent",
                                 "choose a destination that does not feed this track");
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

    auto rulerCommands = [&]<bool Region>() {
        using Object = std::conditional_t<Region, model::Region, model::Marker>;
        const std::string stem = Region ? "region" : "marker";
        const std::string fixture = Region ? "song_with_region" : "song_with_marker";
        const std::string exampleId = Region ? "rg_1" : "mk_1";
        {
            Command<RulerArgs, IdResult> c;
            c.name = stem + ".add";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = Region ? "Add a named region in beats." : "Add a named marker in beats.";
            c.tags = {"ruler"};
            c.args = rulerArgsSchema<RulerArgs, Region, false>((stem + "_add_args").c_str());
            c.result = idResultSchema();
            c.examples = {{"Add",
                           Region ? j(R"({"start":0,"end":4,"name":"Verse"})")
                                  : j(R"({"pos":4,"name":"Verse"})"),
                           j(std::string("{\"id\":\"") + exampleId + "\"}"), "empty"}};
            c.run = [](const RulerArgs& a, Context& ctx) -> Result<IdResult> {
                Object o;
                if constexpr (Region) {
                    if (!a.start || !a.end || *a.end <= *a.start)
                        return Error("region.range", "end must follow start", "/end",
                                     "give start and end in beats");
                    o.start = *a.start;
                    o.end = *a.end;
                } else {
                    if (!a.pos)
                        return Error("marker.pos", "marker position is required", "/pos",
                                     "provide pos as a nonnegative position in beats");
                    o.pos = *a.pos;
                }
                if (a.name) o.name = *a.name;
                if (a.color) o.color = *a.color;
                return IdResult{app(ctx).txn().create(std::move(o)).str()};
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<RulerSetArgs, Empty> c;
            c.name = stem + ".set";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Edit a ruler's position, name, or color.";
            c.tags = {"ruler"};
            c.args = rulerArgsSchema<RulerSetArgs, Region, true>((stem + "_set_args").c_str());
            c.result = emptySchema();
            c.examples = {{"Rename",
                           j(std::string("{\"id\":\"") + exampleId + "\",\"name\":\"Bridge\"}"),
                           j("{}"), fixture}};
            c.run = [](const RulerSetArgs& a, Context& ctx) -> Result<Empty> {
                const auto s = app(ctx).view();
                auto id = require<Object>(s, a.id, "/id");
                if (!id) return std::move(id).error();
                Object o = *model::Traits<Object>::map(s).find(*id);
                if constexpr (Region) {
                    if (a.start) o.start = *a.start;
                    if (a.end) o.end = *a.end;
                    if (!(o.end > o.start))
                        return Error("region.range", "end must follow start", "/end",
                                     "set end later than start in beats");
                } else {
                    if (a.pos) o.pos = *a.pos;
                }
                if (a.name) o.name = *a.name;
                if (a.color) o.color = *a.color;
                app(ctx).txn().put<Object>(*id, std::move(o));
                return Empty{};
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<RulerRef, Empty> c;
            c.name = stem + ".remove";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Remove a ruler.";
            c.tags = {"ruler"};
            c.args = schema::object<RulerRef>("ruler_ref")
                         .field("id", &RulerRef::id, idSchema(Region ? "rg" : "mk"))
                         .build();
            c.result = emptySchema();
            c.examples = {
                {"Remove", j(std::string("{\"id\":\"") + exampleId + "\"}"), j("{}"), fixture}};
            c.run = [](const RulerRef& a, Context& ctx) -> Result<Empty> {
                auto id = require<Object>(app(ctx).view(), a.id, "/id");
                if (!id) return std::move(id).error();
                app(ctx).txn().remove<Object>(*id);
                return Empty{};
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<cmd::NoArgs, RulerList> c;
            c.name = stem + ".list";
            c.kind = Kind::Query;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "List rulers in this project.";
            c.tags = {"ruler"};
            c.args = schema::object<cmd::NoArgs>("no_args").build();
            c.result = rulerListSchema(Region);
            c.examples = {{"List empty", j("{}"),
                           Region ? j(R"({"regions":[]})") : j(R"({"markers":[]})"), "empty"}};
            c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<RulerList> {
                RulerList out;
                const auto s = app(ctx).view();
                for (const auto& e : model::Traits<Object>::map(s)) {
                    RulerEntry row;
                    row.timebase = e.value->timebase;
                    row.id = e.id.str();
                    row.name = e.value->name;
                    row.color = e.value->color;
                    if constexpr (Region) {
                        row.start = e.value->start;
                        row.end = e.value->end;
                    } else {
                        row.pos = e.value->pos;
                    }
                    out.entries.push_back(std::move(row));
                }
                return out;
            };
            addChecked(registry, std::move(c));
        }
    };
    rulerCommands.template operator()<false>();
    rulerCommands.template operator()<true>();

    [&] {
        Command<PluginArgs, PluginList> c;
        c.name = "plugin.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List scanned plugins; filter matches id, name, or vendor.";
        c.tags = {"plugin"};
        c.args = schema::object<PluginArgs>("plugin_list_args")
                     .field("filter", &PluginArgs::filter, schema::string())
                     .build();
        auto entry =
            schema::object<PluginEntry>("plugin_entry")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("format", &PluginEntry::format, schema::string().doc("clap or vst3"))
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .field("features", &PluginEntry::features,
                       schema::arrayOf(schema::string())
                           .doc("CLAP features (a VST3's subcategories): the main category "
                                "first, then the kind of plugin, such as reverb"))
                .field("path", &PluginEntry::path, schema::string())
                .field("shadowed", &PluginEntry::shadowed,
                       schema::boolean().doc("Another location earlier on the plugin path has the "
                                             "same id; fx.add by id uses that one"))
                .build();
        c.result = schema::object<PluginList>("plugin_list")
                       .field("plugins", &PluginList::plugins, schema::arrayOf(entry))
                       .field("scan", &PluginList::scan, scanReportSchema())
                       .build();
        c.examples = {{"Find a test synth", j(R"({"filter":"test.sine"})"),
                       j(R"({"plugins":"<any>","scan":"<any>"})"), "empty"}};
        c.run = [](const PluginArgs& a, Context& ctx) -> Result<PluginList> {
            auto found = app(ctx).plugins();
            if (!found) return std::move(found).error();
            PluginList out;
            out.scan = scanReport(app(ctx).pluginScanReport());
            for (std::size_t i = 0; i < found->size(); ++i) {
                const auto& p = (*found)[i];
                if (a.filter && p.id.find(*a.filter) == std::string::npos &&
                    p.name.find(*a.filter) == std::string::npos &&
                    p.vendor.find(*a.filter) == std::string::npos)
                    continue;
                PluginEntry e{p.id,       p.name,          p.vendor, p.format,  p.instrument,
                              p.semantic, p.path.string(), {},       p.features};
                if (host::shadowed(*found, i)) e.shadowed = true;
                out.plugins.push_back(std::move(e));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ScanArgs, PluginList> c;
        c.name = "plugin.scan";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Rescan configured plugin paths and return the available plugins.";
        c.description =
            "wait: false scans in the background (every module through the supervised "
            "scanner) and returns at once with the current list and the scan's job; the "
            "catalogue changes when the job is done. A background scan already running is "
            "returned instead of a second one.";
        c.tags = {"plugin"};
        c.args = schema::object<ScanArgs>("plugin_scan_args")
                     .field("wait", &ScanArgs::wait,
                            schema::boolean().doc("false: scan in the background (default true)"))
                     .build();
        auto entry =
            schema::object<PluginEntry>("plugin_entry")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("format", &PluginEntry::format, schema::string().doc("clap or vst3"))
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .field("features", &PluginEntry::features,
                       schema::arrayOf(schema::string())
                           .doc("CLAP features (a VST3's subcategories): the main category "
                                "first, then the kind of plugin, such as reverb"))
                .field("path", &PluginEntry::path, schema::string())
                .field("shadowed", &PluginEntry::shadowed, schema::boolean())
                .build();
        c.result = schema::object<PluginList>("plugin_list")
                       .field("plugins", &PluginList::plugins, schema::arrayOf(entry))
                       .field("scan", &PluginList::scan, scanReportSchema())
                       .field("job", &PluginList::job,
                              schema::string().doc("the background scan (wait: false)"))
                       .build();
        c.examples = {{"Scan", j("{}"), j(R"({"plugins":"<any>","scan":"<any>"})"), "empty"}};
        c.run = [](const ScanArgs& a, Context& ctx) -> Result<PluginList> {
            EngineApp& engine = app(ctx);
            const bool wait = a.wait.value_or(true);
            std::optional<std::string> job;
            if (!wait) {
                auto& jobs = ctx.service<cmd::JobManager>();
                std::string& running = engine.pluginScanJob();
                const auto state =
                    running.empty() ? Result<json::Value>(json::Value()) : jobs.status(running);
                if (!state || !state->isObject() || !state->find("status") ||
                    state->find("status")->asString() != "running") {
                    running = jobs.create();
                    auto& dispatcher = ctx.service<cmd::Dispatcher>();
                    const auto& configured = engine.options().pluginPath;
                    auto paths = configured.empty() ? host::defaultSearchPath() : configured;
                    engine.startJob([&engine, &jobs, &dispatcher, id = running,
                                     paths = std::move(paths)](supple::StopToken) {
                        host::ScanReport report;
                        host::ScanOptions options;
                        options.superviseAll = true;  // nothing loads in this thread
                        auto found = host::scan(paths, options, &report);
                        if (!found) {
                            (void)jobs.fail(id, std::move(found).error());
                            return;
                        }
                        dispatcher.post(
                            "plugin.scan.done",
                            [&engine, &jobs, id, list = std::move(*found),
                             report = std::move(report)]() mutable -> Status {
                                json::Value result = json::Value::object();
                                result["plugins"] = static_cast<std::int64_t>(list.size());
                                engine.installPlugins(std::move(list), std::move(report));
                                return jobs.complete(id, std::move(result));
                            });
                    });
                }
                job = running;
            }
            auto found = engine.plugins(wait);
            if (!found) return std::move(found).error();
            PluginList out;
            out.scan = scanReport(engine.pluginScanReport());
            for (std::size_t i = 0; i < found->size(); ++i) {
                const auto& p = (*found)[i];
                PluginEntry e{p.id,       p.name,          p.vendor, p.format,  p.instrument,
                              p.semantic, p.path.string(), {},       p.features};
                if (host::shadowed(*found, i)) e.shadowed = true;
                out.plugins.push_back(std::move(e));
            }
            out.job = std::move(job);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TimeArgs, TimeResult> c;
        c.name = "time.convert";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Convert a seconds, beat, or bar position to all three time domains.";
        c.tags = {"time"};
        c.args = schema::object<TimeArgs>("time_convert_args")
                     .field("seconds", &TimeArgs::seconds, schema::number().min(0))
                     .field("beat", &TimeArgs::beat, schema::number().min(0))
                     .field("bar", &TimeArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .build();
        c.result = schema::object<TimeResult>("time_position")
                       .field("seconds", &TimeResult::seconds, schema::number())
                       .field("beat", &TimeResult::beat, schema::number())
                       .field("bar", &TimeResult::bar, schema::integer())
                       .field("beat_in_bar", &TimeResult::beatInBar, schema::integer())
                       .field("fraction", &TimeResult::fraction, schema::number())
                       .build();
        c.examples = {{"Bar two", j(R"({"bar":2})"),
                       j(R"({"seconds":2,"beat":4,"bar":2,"beat_in_bar":1,"fraction":0})"),
                       "empty"}};
        c.run = [](const TimeArgs& a, Context& ctx) -> Result<TimeResult> {
            if (a.seconds.has_value() + a.beat.has_value() + a.bar.has_value() != 1)
                return Error("time.input", "give exactly one of seconds, beat, or bar", "",
                             "remove the other time inputs");
            const auto s = app(ctx).view();
            const auto map = s.timeMap;
            const double beat = a.seconds ? map->beatAt(*a.seconds)
                                : a.bar   ? map->beatOfBar(static_cast<int>(*a.bar))
                                          : *a.beat;
            SUPPLE_TRY(model::checkedBeat(s, beat,
                                          a.seconds ? "/seconds"
                                          : a.bar   ? "/bar"
                                                    : "/beat"));
            const auto bb = map->barBeat(beat);
            return TimeResult{map->secondsAt(beat), beat, bb.bar, bb.beat, bb.fraction};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemSetArgs, Empty> c;
        c.name = "item.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set an item's position, length, playback, fades, name, color, or lock.";
        c.description =
            "Positions and lengths are in beats; fades are in seconds. Name is the active take's "
            "name. A locked item refuses every edit except {\"locked\": false}. timebase: what "
            "the item follows when the tempo changes — beats (moves and stretches with it), "
            "time (stays at its seconds), beats_position (its start follows the beat, its "
            "length stays in seconds) or track (inherit); the item keeps its place now.";
        c.tags = {"item"};
        c.args = schema::object<ItemSetArgs>("item_set_args")
                     .field("bypass_groups", &ItemSetArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemSetArgs::item, idSchema("it"))
                     .field("position", &ItemSetArgs::position, schema::number().min(0))
                     .field("bar", &ItemSetArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("length", &ItemSetArgs::length, schema::number().min(0))
                     .field("mute", &ItemSetArgs::mute, schema::boolean())
                     .field("loop", &ItemSetArgs::loop, schema::boolean())
                     .field("locked", &ItemSetArgs::locked,
                            schema::boolean().doc("The arrange view marks locked items."))
                     .field("gain_db", &ItemSetArgs::gainDb,
                            schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                     .field("fade_in", &ItemSetArgs::fadeIn, schema::number().min(0))
                     .field("fade_in_shape", &ItemSetArgs::fadeInShape, fadeShapeSchema())
                     .field("fade_out_shape", &ItemSetArgs::fadeOutShape, fadeShapeSchema())
                     .field("fade_out", &ItemSetArgs::fadeOut, schema::number().min(0))
                     .field("name", &ItemSetArgs::name, schema::string())
                     .field("color", &ItemSetArgs::color, schema::string())
                     .field("timebase", &ItemSetArgs::timebase, model::itemTimebaseSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Lower an item", j(R"({"item":"it_1","gain_db":"-3 dB"})"), j("{}"), "song_with_item"},
            {"Keep an item at its time when the tempo changes",
             j(R"({"item":"it_1","timebase":"time"})"), j("{}"), "song_with_item"}};
        c.run = [](const ItemSetArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const auto& original = *s.items.find(*id);
            // A locked item refuses edits; unlocking it is the one exception.
            const bool unlocking = a.locked && !*a.locked;
            if (!unlocking) SUPPLE_TRY(checkUnlocked(s, *id));
            if (a.position && a.bar)
                return Error("item.position", "give position or bar, not both", "/position",
                             "remove one of them");
            if (a.length && !(*a.length > 0))
                return Error("item.length", "length must be greater than 0", "/length",
                             "give a positive length, or remove the item");
            model::Item it = original;
            if (a.timebase) {  // first: the rest is placed in the new domain
                const model::Timebase from = model::itemTimebase(s, it);
                it.timebase = *a.timebase;
                model::retimeItem(s, it, from, model::itemTimebase(s, it));
            }
            if (a.position || a.bar || a.length) {
                const double start = a.position ? *a.position
                                     : a.bar    ? s.timeMap->beatOfBar(static_cast<int>(*a.bar))
                                                : model::itemSpan(s, it).startBeat;
                if (a.length)
                    model::placeItem(s, it, start, *a.length);
                else
                    model::moveItem(s, it, it.track, start);
            }
            if (a.mute) it.mute = *a.mute;
            if (a.loop) it.loop = *a.loop;
            if (a.locked) it.locked = *a.locked;
            if (a.gainDb) it.gainDb = *a.gainDb;
            if (a.fadeIn) it.fadeIn.length = *a.fadeIn;
            if (a.fadeOut) it.fadeOut.length = *a.fadeOut;
            if (a.fadeInShape) it.fadeIn.shape = *a.fadeInShape;
            if (a.fadeOutShape) it.fadeOut.shape = *a.fadeOutShape;
            if (a.color) it.color = *a.color;
            if (a.name && !it.activeTake)
                return Error("item.no_take", "item has no active take to name", "/name",
                             "select an existing take with take.activate before setting name, or "
                             "add a take to the item");
            auto& t = app(ctx).txn();
            if (a.name)
                t.modify<model::Take>(*it.activeTake, [&](model::Take& tk) { tk.name = *a.name; });
            t.put<model::Item>(*id, std::move(it));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemDuplicateArgs, ItemDuplicateResult> c;
        c.name = "item.duplicate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Copy an item and its takes to a new beat position (default: immediately after).";
        c.tags = {"item"};
        c.args = schema::object<ItemDuplicateArgs>("item_duplicate_args")
                     .field("bypass_groups", &ItemDuplicateArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemDuplicateArgs::item, idSchema("it"))
                     .field("to", &ItemDuplicateArgs::to, schema::number().min(0))
                     .build();
        c.result =
            schema::object<ItemDuplicateResult>("item_duplicate_result")
                .field("id", &ItemDuplicateResult::id, schema::string())
                .field("linked", &ItemDuplicateResult::linked, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .build();
        c.examples = {{"Repeat", j(R"({"item":"it_1"})"), j(R"({"id":"it_2"})"), "song_with_item"}};
        c.run = [](const ItemDuplicateArgs& a, Context& ctx) -> Result<ItemDuplicateResult> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            model::Item copy = *s.items.find(*id);
            const double end = model::itemSpan(s, copy).startBeat + model::itemLengthBeats(s, copy);
            model::moveItem(s, copy, copy.track, a.to.value_or(end));
            copy.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldId : s.items.find(*id)->takes) {
                auto old = dangling<model::Take>(s, oldId, "/item");
                if (!old) return std::move(old).error();
                model::Take tk = **old;
                unpoolCopiedSource(tk, s, txn);
                auto cloned = copyTake(s, txn, oldId, std::move(tk));
                if (!cloned) return std::move(cloned).error();
                const Id fresh = *cloned;
                copy.takes.push_back(fresh);
                if (s.items.find(*id)->activeTake == oldId) copy.activeTake = fresh;
            }
            return ItemDuplicateResult{txn.create(std::move(copy)).str(), {}};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemPasteArgs, ItemPasteResult> c;
        c.name = "item.paste";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Copy items as they were in a version (a clipboard) to tracks and beats.";
        c.description =
            "version: where the items are read (default: now) — a GUI copy or cut remembers the "
            "version, so a paste reproduces the items as copied even after they were removed or "
            "edited. items: [{item, track, position}] (beats; channels only). Takes, fades, "
            "take FX and MIDI are copied; an audio source the project no longer has is restored.";
        c.tags = {"item"};
        auto placement = schema::object<PastePlacement>("paste_placement")
                             .field("item", &PastePlacement::item, idSchema("it"))
                             .field("track", &PastePlacement::track, idSchema("tr"))
                             .field("position", &PastePlacement::position, schema::number().min(0))
                             .build();
        c.args = schema::object<ItemPasteArgs>("item_paste_args")
                     .field("version", &ItemPasteArgs::version, schema::integer().min(1))
                     .field("items", &ItemPasteArgs::items, schema::arrayOf(placement))
                     .build();
        c.result = schema::object<ItemPasteResult>("item_paste_result")
                       .field("items", &ItemPasteResult::items, schema::arrayOf(schema::string()))
                       .build();
        c.examples = {{"Paste after itself",
                       j(R"({"items":[{"item":"it_1","track":"tr_1","position":16}]})"),
                       j(R"({"items":["it_2"]})"), "song_with_item"}};
        c.run = [](const ItemPasteArgs& a, Context& ctx) -> Result<ItemPasteResult> {
            EngineApp& eng = app(ctx);
            const auto now = eng.view();
            std::shared_ptr<const model::Session> from;
            if (a.version) {
                const model::Version* v =
                    eng.versions().version(static_cast<std::uint64_t>(*a.version));
                if (!v || !v->session)
                    return Error("history.version", "no version " + std::to_string(*a.version),
                                 "/version", "history.list shows the versions");
                from = v->session;
            } else {
                from = std::make_shared<const model::Session>(now);
            }
            const model::Session& s = *from;
            auto& txn = eng.txn();
            std::map<Id, std::vector<Id>> copiedTakes;
            std::map<Id, Id> restored;  // audio sources brought back, by their old id
            ItemPasteResult out;
            for (std::size_t i = 0; i < a.items.size(); ++i) {
                const PastePlacement& p = a.items[i];
                const std::string at = "/items/" + std::to_string(i);
                auto item = require<model::Item>(s, p.item, at + "/item");
                if (!item) return std::move(item).error();
                auto track = require<model::Track>(now, p.track, at + "/track");
                if (!track) return std::move(track).error();
                const model::Item& original = *s.items.find(*item);
                SUPPLE_TRY(checkItemTrack(now, *track, original.takes, at + "/track", &s));
                model::Item copy = original;
                // Its length as its timebase kept it in the copied version (beats
                // for beats, seconds otherwise), placed on today's tempo map.
                const model::ItemSpan span = model::itemSpan(s, original);
                const bool beats = model::itemTimebase(s, original) == model::Timebase::Beats;
                copy.track = *track;
                const double lengthBeats =
                    beats ? s.timeMap->beatAt(span.endSec) - span.startBeat
                          : now.timeMap->beatAt(now.timeMap->secondsAt(p.position) + span.endSec -
                                                span.startSec) -
                                p.position;
                model::placeItem(now, copy, p.position, lengthBeats);
                copy.takes.clear();
                copy.activeTake.reset();
                for (Id oldId : original.takes) {
                    auto old = dangling<model::Take>(s, oldId, at + "/item");
                    if (!old) return std::move(old).error();
                    model::Take tk = **old;
                    if (tk.source && tk.source->kind == model::Kind::Media &&
                        !now.media.find(*tk.source)) {
                        auto it = restored.find(*tk.source);
                        if (it == restored.end()) {
                            const model::MediaSource* md = s.media.find(*tk.source);
                            if (!md)
                                return Error("item.source", "the copied audio is gone", at,
                                             "recopy from a version containing the audio source, "
                                             "or reimport its audio_file");
                            it = restored.emplace(*tk.source, txn.create(*md)).first;
                        }
                        tk.source = it->second;
                    } else if (tk.source && tk.source->kind == model::Kind::Midi &&
                               !now.midi.find(*tk.source)) {
                        const model::MidiSource* mi = s.midi.find(*tk.source);
                        if (!mi)
                            return Error("item.source", "the copied MIDI is gone", at,
                                         "recopy from a version containing the MIDI source, or "
                                         "recreate the item with notation");
                        model::MidiSource fresh = *mi;
                        fresh.pooled = false;
                        tk.source = txn.create(std::move(fresh));
                    } else {
                        unpoolCopiedSource(tk, s, txn);
                    }
                    auto cloned = copyTake(s, txn, oldId, std::move(tk));
                    if (!cloned) return std::move(cloned).error();
                    const Id fresh = *cloned;
                    copiedTakes[oldId].push_back(fresh);
                    copy.takes.push_back(fresh);
                    if (original.activeTake == oldId) copy.activeTake = fresh;
                }
                out.items.push_back(txn.create(std::move(copy)).str());
            }
            for (const auto& [oldId, copies] : copiedTakes) {
                const auto& original = *s.takes.find(oldId);
                if (copies.size() != 1 || original.warpPeers.empty()) continue;
                std::vector<Id> peers;
                for (auto peer : original.warpPeers) {
                    const auto found = copiedTakes.find(peer);
                    if (found == copiedTakes.end() || found->second.size() != 1) {
                        peers.clear();
                        break;
                    }
                    peers.push_back(found->second.front());
                }
                if (peers.size() != original.warpPeers.size()) continue;
                std::sort(peers.begin(), peers.end());
                txn.modify<model::Take>(copies.front(),
                                        [&](auto& take) { take.warpPeers = peers; });
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemSplitArgs, SplitResult> c;
        c.name = "item.split";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Split an item at a beat, optionally at the prior audio zero crossing.";
        c.description =
            "snap defaults to none. zero_crossing searches the active take's prepared playback "
            "source up to 100 ms before at, before FX/fades/gain. After channel mode, use the "
            "louder stereo channel by window energy (left on ties), never a stereo sum unless "
            "MonoMix is selected. Choose the latest exact zero or sign-change sample boundary; "
            "never search forward or outside the item. Without a crossing, split at at. MIDI "
            "and empty takes refuse zero_crossing; the GUI skips them. Current prepared audio "
            "must be available; start the engine and retry after preparation. Linked items use "
            "the initiating item's resolved split time. History records that time for replay.";
        c.tags = {"item"};
        c.args = schema::object<ItemSplitArgs>("item_split_args")
                     .field("bypass_groups", &ItemSplitArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemSplitArgs::item, idSchema("it"))
                     .field("at", &ItemSplitArgs::at, schema::number().min(0))
                     .field("snap", &ItemSplitArgs::snap,
                            schema::enumeration<std::string>(
                                {{"none", "none"}, {"zero_crossing", "zero_crossing"}}))
                     .defaultValue(std::string("none"))
                     .build();
        c.result =
            schema::object<SplitResult>("item_split_result")
                .field("left", &SplitResult::left, schema::string())
                .field("right", &SplitResult::right, schema::string())
                .field("linked_left", &SplitResult::linkedLeft, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .field("linked_right", &SplitResult::linkedRight, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .build();
        c.errors = {{"item.split_range", "the cursor is outside the item's interior",
                     "choose a beat strictly after the start and before the end"},
                    {"item.split_not_audio", "zero_crossing needs an active audio take",
                     "use snap none to split MIDI or empty items at the cursor"},
                    {"item.split_source_pending", "current source audio is not prepared",
                     "start the engine, wait for source preparation and retry"},
                    {"item.split_source", "the audio source could not be prepared",
                     "relink or repair the audio source and retry"}};
        c.examples = {{"Split halfway", j(R"({"item":"it_1","at":2})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_item"},
                      {"Split at a prior audio zero crossing",
                       j(R"({"item":"it_1","at":2,"snap":"zero_crossing"})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_prepared_audio"}};
        c.resolve = [](ItemSplitArgs& a, Context& ctx) -> Status {
            if (a.snap != "zero_crossing") return {};
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return id.error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            auto at =
                priorZeroCrossing(app(ctx), s, *s.items.find(*id), s.timeMap->secondsAt(a.at));
            if (!at) return at.error();
            a.at = s.timeMap->beatAt(*at);
            a.snap = "none";  // the bus logs the resolved beat for linked edits and replay
            return {};
        };
        c.run = [](const ItemSplitArgs& a, Context& ctx) -> Result<SplitResult> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& old = *s.items.find(*id);
            SUPPLE_TRY(checkUnlocked(s, *id));
            const auto* tr = s.tracks.find(old.track);
            model::Timebase tb = s.project->timebase;
            if (tr) {
                if (tr->timebase == model::TrackTimebase::Time) tb = model::Timebase::Time;
                if (tr->timebase == model::TrackTimebase::Beats) tb = model::Timebase::Beats;
                if (tr->timebase == model::TrackTimebase::BeatsPosition)
                    tb = model::Timebase::BeatsPosition;
            }
            if (old.timebase == model::ItemTimebase::Time) tb = model::Timebase::Time;
            if (old.timebase == model::ItemTimebase::Beats) tb = model::Timebase::Beats;
            if (old.timebase == model::ItemTimebase::BeatsPosition)
                tb = model::Timebase::BeatsPosition;
            const double splitSec = s.timeMap->secondsAt(a.at);
            const double startBeat =
                tb == model::Timebase::Time ? s.timeMap->beatAt(old.position) : old.position;
            const double startSec = s.timeMap->secondsAt(startBeat);
            const double endSec = tb == model::Timebase::Time ? old.position + old.length
                                  : tb == model::Timebase::Beats
                                      ? s.timeMap->secondsAt(old.position + old.length)
                                      : startSec + old.length;
            if (!(splitSec > startSec && splitSec < endSec))
                return Error("item.split_range", "split must be inside the item", "/at",
                             "choose a beat strictly after the start and before the end");
            model::Item left = old, right = old;
            if (tb == model::Timebase::Time) {
                left.length = splitSec - startSec;
                right.position = splitSec;
                right.length = endSec - splitSec;
            } else if (tb == model::Timebase::Beats) {
                left.length = a.at - old.position;
                right.position = a.at;
                right.length = old.position + old.length - a.at;
            } else {
                left.length = splitSec - startSec;
                right.position = a.at;
                right.length = endSec - splitSec;
            }
            left.fadeOut.length = 0;
            right.fadeIn.length = 0;
            right.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldTake : old.takes) {
                auto source = dangling<model::Take>(s, oldTake, "/item");
                if (!source) return std::move(source).error();
                model::Take tk = **source;
                const double delta = tk.source && tk.source->kind == model::Kind::Midi
                                         ? (a.at - startBeat) * tk.playrate
                                         : (splitSec - startSec) * tk.playrate;
                if (tk.source && tk.source->kind == model::Kind::Media)
                    model::rebaseAudioTake(tk, splitSec - startSec);
                else
                    tk.startOffset += delta;
                unpoolCopiedSource(tk, s, txn);
                auto cloned = copyTake(s, txn, oldTake, std::move(tk), a.at - startBeat);
                if (!cloned) return std::move(cloned).error();
                const Id fresh = *cloned;
                right.takes.push_back(fresh);
                if (old.activeTake == oldTake) right.activeTake = fresh;
            }
            txn.put<model::Item>(*id, std::move(left));
            Id fresh = txn.create(std::move(right));
            return SplitResult{id->str(), fresh.str(), {}, {}};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeAddArgs, IdResult> c;
        c.name = "take.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a take to an item, optionally copied from another take of that item.";
        c.tags = {"take"};
        c.args = schema::object<TakeAddArgs>("take_add_args")
                     .field("item", &TakeAddArgs::item, idSchema("it"))
                     .field("from", &TakeAddArgs::from, idSchema("tk"))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"Copy a take", j(R"({"item":"it_1","from":"tk_1"})"), j(R"({"id":"tk_2"})"),
                       "song_with_item"}};
        c.run = [](const TakeAddArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            model::Take copy;
            if (a.from) {
                auto source = require<model::Take>(s, *a.from, "/from");
                if (!source) return std::move(source).error();
                const auto& takes = s.items.find(*item)->takes;
                if (std::find(takes.begin(), takes.end(), *source) == takes.end())
                    return Error(
                        "take.not_in_item", *a.from + " does not belong to " + a.item, "/from",
                        "choose a from take in the destination item; take.list lists its takes");
                auto takeSource = dangling<model::Take>(s, *source, "/from");
                if (!takeSource) return std::move(takeSource).error();
                copy = **takeSource;
                unpoolCopiedSource(copy, s, app(ctx).txn());
                copy.chain.clear();
                for (Id fx : (*takeSource)->chain) {
                    auto chained = dangling<model::FxInstance>(s, fx, "/takes");
                    if (!chained) return std::move(chained).error();
                    copy.chain.push_back(app(ctx).txn().create(**chained));
                }
            }
            auto& txn = app(ctx).txn();
            Id fresh = txn.create(std::move(copy));
            txn.modify<model::Item>(*item, [&](model::Item& it) {
                it.takes.push_back(fresh);
                if (!it.activeTake) it.activeTake = fresh;
            });
            return IdResult{fresh.str()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeActivateArgs, Empty> c;
        c.name = "take.activate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Choose the take heard from an item.";
        c.description =
            "take (its id) or index (0-based in the item's take order — the same position "
            "after item.split, which copies the takes in order: comping splits an item and "
            "picks a take by index on the piece).";
        c.tags = {"take"};
        c.args = schema::object<TakeActivateArgs>("take_activate_args")
                     .field("item", &TakeActivateArgs::item, idSchema("it"))
                     .field("take", &TakeActivateArgs::take, idSchema("tk"))
                     .field("index", &TakeActivateArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Activate", j(R"({"item":"it_1","take":"tk_1"})"), j("{}"), "song_with_item"}};
        c.run = [](const TakeActivateArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            const auto& takes = s.items.find(*item)->takes;
            if (a.take.has_value() == a.index.has_value())
                return Error("take.choice", "give take or index", "", "take: \"tk_2\" or index: 1");
            std::optional<Id> take;
            if (a.index) {
                if (*a.index >= static_cast<std::int64_t>(takes.size()))
                    return Error("take.index",
                                 a.item + " has " + std::to_string(takes.size()) + " takes",
                                 "/index", "use take.list for this item");
                take = takes[static_cast<std::size_t>(*a.index)];
            } else {
                auto found = require<model::Take>(s, *a.take, "/take");
                if (!found) return std::move(found).error();
                take = *found;
                if (std::find(takes.begin(), takes.end(), *take) == takes.end())
                    return Error("take.not_in_item", *a.take + " does not belong to " + a.item,
                                 "/take", "use take.list for this item");
            }
            app(ctx).txn().modify<model::Item>(*item,
                                               [&](model::Item& it) { it.activeTake = *take; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeSetArgs, Empty> c;
        c.name = "take.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set take gain, pan, channel mode, name, source offset, or playrate and pitch.";
        c.tags = {"take"};
        c.args =
            schema::object<TakeSetArgs>("take_set_args")
                .field("take", &TakeSetArgs::take, idSchema("tk"))
                .field("gain_db", &TakeSetArgs::gainDb,
                       schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                .field("pan", &TakeSetArgs::pan,
                       schema::number().range(-100, 100).unit(units::Unit::percent))
                .field("channel_mode", &TakeSetArgs::channelMode,
                       schema::enumeration<model::ChannelMode>(
                           {{model::ChannelMode::Normal, "normal"},
                            {model::ChannelMode::ReverseStereo, "reverse_stereo"},
                            {model::ChannelMode::MonoMix, "mono_mix"},
                            {model::ChannelMode::Left, "left"},
                            {model::ChannelMode::Right, "right"}}))
                .field("name", &TakeSetArgs::name, schema::string())
                .field("rebase_seconds", &TakeSetArgs::rebaseSeconds,
                       schema::number().doc("Rebase an audio take after trimming its item start; "
                                            "preserves the warp map."))
                .field("start_offset", &TakeSetArgs::startOffset,
                       schema::number().doc(
                           "Source position at the item start; negative for a late first pass."))
                .field("playrate", &TakeSetArgs::playrate,
                       schema::number()
                           .range(0.01, 100)
                           .unit(units::Unit::x)
                           .doc("Playback speed: 2 plays the source twice as fast. Its length in "
                                "seconds is the item's, so the take runs out sooner or loops."))
                .field("preserve_pitch", &TakeSetArgs::preservePitch,
                       schema::boolean().doc(
                           "Keep the source's pitch when the playrate changes (the default). "
                           "With pitch_mode \"resample\" the pitch follows the speed."))
                .field("pitch", &TakeSetArgs::pitch,
                       schema::number()
                           .range(-48, 48)
                           .unit(units::Unit::st)
                           .doc("Pitch offset in semitones. Stretched when the pitch is preserved, "
                                "otherwise a further speed factor of 2^(pitch/12)."))
                .field("pitch_mode", &TakeSetArgs::pitchMode,
                       schema::enumeration<std::string>(
                           {{"resample", "resample"}, {"signalsmith", "signalsmith"}})
                           .doc("Time-stretch algorithm: \"signalsmith\" (pitch-preserving, "
                                "the default) or \"resample\" (varispeed)."))
                .build();
        c.result = emptySchema();
        c.examples = {
            {"Lower a take", j(R"({"take":"tk_1","gain_db":"-3 dB"})"), j("{}"), "song_with_item"},
            {"Twice as fast, pitch and all",
             j(R"({"take":"tk_1","playrate":2,"preserve_pitch":false})"), j("{}"),
             "song_with_item"},
            {"Half speed, pitch kept", j(R"({"take":"tk_1","playrate":0.5})"), j("{}"),
             "song_with_item"},
            {"An octave up, same tempo", j(R"({"take":"tk_1","pitch":12})"), j("{}"),
             "song_with_item"}};
        c.run = [](const TakeSetArgs& a, Context& ctx) -> Result<Empty> {
            auto id = require<model::Take>(app(ctx).view(), a.take, "/take");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlockedTake(app(ctx).view(), *id, "/take"));
            if (a.rebaseSeconds && a.startOffset)
                return Error("take.offset", "choose one source-position edit", "/rebase_seconds",
                             "supply rebase_seconds or start_offset, not both");
            const auto* original = app(ctx).view().takes.find(*id);
            const bool validWarp = !original->stretchMarkers.empty() &&
                                   static_cast<bool>(model::validateWarpMap(*original));
            app(ctx).txn().modify<model::Take>(*id, [&](model::Take& tk) {
                if (a.gainDb) tk.gainDb = *a.gainDb;
                if (a.pan) tk.pan = *a.pan;
                if (a.channelMode) tk.channelMode = *a.channelMode;
                if (a.name) tk.name = *a.name;
                if (a.rebaseSeconds) model::rebaseAudioTake(tk, *a.rebaseSeconds);
                if (a.startOffset) model::slipAudioTake(tk, *a.startOffset - tk.startOffset);
                if (a.playrate) model::rateAudioTake(tk, *a.playrate);
                if (a.preservePitch) tk.preservePitch = *a.preservePitch;
                if (a.pitch) tk.pitch = *a.pitch;
                if (a.pitchMode) tk.pitchMode = *a.pitchMode;
            });
            if (validWarp)
                SUPPLE_TRY(model::validateWarpMap(*app(ctx).view().takes.find(*id), "/take"));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemQuery, TakeList> c;
        c.name = "take.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the takes of an item and identify its active take.";
        c.tags = {"take"};
        c.args = schema::object<ItemQuery>("take_list_args")
                     .field("item", &ItemQuery::item, idSchema("it"))
                     .build();
        auto entry =
            schema::object<TakeEntry>("take_entry")
                .field("id", &TakeEntry::id, schema::string())
                .field("name", &TakeEntry::name, schema::string())
                .field("source", &TakeEntry::source, schema::string())
                .field("active", &TakeEntry::active, schema::boolean())
                .field("gain_db", &TakeEntry::gainDb, schema::number())
                .field("pan", &TakeEntry::pan, schema::number())
                .field("start_offset", &TakeEntry::startOffset, schema::number())
                .field("playrate", &TakeEntry::playrate, schema::number().unit(units::Unit::x))
                .field("preserve_pitch", &TakeEntry::preservePitch, schema::boolean())
                .field("pitch", &TakeEntry::pitch, schema::number().unit(units::Unit::st))
                .field("pitch_mode", &TakeEntry::pitchMode, schema::string())
                .field("stretch_markers", &TakeEntry::markers, schema::any())
                .field("transients", &TakeEntry::hits, schema::any())
                .field("warp_peers", &TakeEntry::peers, schema::any())
                .field("hits_stale", &TakeEntry::hitsStale, schema::boolean())
                .build();
        c.result = schema::object<TakeList>("take_list")
                       .field("takes", &TakeList::takes, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"List", j(R"({"item":"it_1"})"), j(R"({"takes":"<any>"})"), "song_with_item"}};
        c.run = [](const ItemQuery& a, Context& ctx) -> Result<TakeList> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            TakeList out;
            const auto& it = *s.items.find(*id);
            for (Id takeId : it.takes) {
                auto found = dangling<model::Take>(s, takeId, "/item");
                if (!found) return std::move(found).error();
                const auto& tk = **found;
                out.takes.push_back({takeId.str(), tk.name, tk.source ? tk.source->str() : "",
                                     it.activeTake == takeId, tk.gainDb, tk.pan, tk.startOffset,
                                     tk.playrate, tk.pitch, tk.preservePitch, tk.pitchMode});
                auto encoded = model::toJson(tk);
                auto& listedTake = out.takes.back();
                if (const auto* v = encoded.find("stretch_markers")) listedTake.markers = *v;
                if (const auto* v = encoded.find("transients")) listedTake.hits = *v;
                if (const auto* v = encoded.find("warp_peers")) listedTake.peers = *v;
                const auto* md = tk.source ? s.media.find(*tk.source) : nullptr;
                listedTake.hitsStale =
                    tk.transients && (!md || md->hash != tk.transients->sourceHash);
            }
            return out;
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
                     .field("fx", &FxMoveArgs::fx, idSchema("fx"))
                     .field("track", &FxMoveArgs::track, idSchema("tr"))
                     .field("index", &FxMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Move a gain first", j(R"({"fx":"fx_2","index":0})"), j("{}"), "song_with_fx"}};
        c.run = [](const FxMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
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
                auto tr = require<model::Track>(s, *a.track, "/track");
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
                     .field("fx", &FxPatchGetArgs::fx, idSchema("fx"))
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
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
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
                     .field("fx", &FxPatchListArgs::fx, idSchema("fx"))
                     .build();
        c.result = schema::object<FxPatchListResult>("fx_patch_list_result")
                       .field("patches", &FxPatchListResult::patches, schema::any())
                       .build();
        c.examples = {{"List factory patches", j(R"({"fx":"fx_2"})"), j(R"({"patches":[]})"),
                       "song_with_fx"}};
        c.run = [](const FxPatchListArgs& a, Context& ctx) -> Result<FxPatchListResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
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
                .field("fx", &FxPatchSetArgs::fx, idSchema("fx"))
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
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
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
                     .field("fx", &FxParamGetArgs::fx, idSchema("fx"))
                     .field("param", &FxParamGetArgs::param, schema::string().minLength(1))
                     .build();
        c.result = schema::object<FxParamResult>("fx_param_result")
                       .field("value", &FxParamResult::value, schema::any())
                       .build();
        c.examples = {{"Read gain", j(R"({"fx":"fx_2","param":"gain"})"), j(R"({"value":"<any>"})"),
                       "song_with_fx"}};
        c.run = [](const FxParamGetArgs& a, Context& ctx) -> Result<FxParamResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
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
                     .field("fx", &FxMetersArgs::fx, idSchema("fx"))
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
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
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
