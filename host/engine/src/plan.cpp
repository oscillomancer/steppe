// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plan.h"
#include "live_workers.h"
#include "rack_runtime.h"
#include "supple/model/fade_shape.h"
#include "supple/model/freeze.h"
#include "supple/model/groups.h"
#include "supple/model/source_mapping.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <map>
#include <set>

#include "supple/clap/semantic_params.h"
#include "supple/dsp/dsp.h"

namespace supple::engine::detail {

namespace {

std::int64_t tapOffset(const Node& n, model::Tap tap) {
    return tap == model::Tap::PreFx ? 0 : n.latChain;
}

double fadeCurve(model::FadeShape shape, double curve, double t) {
    return model::fadeGain(shape, curve, t);
}

// Note and control events of the MIDI items on `track`, in samples from the
// render start (before PDC shifting).
// `include`: which of the track's items to take (empty: all).
std::vector<TimedEvent> trackEvents(const Session& s, Id track, double startSeconds, double rate,
                                    const std::function<bool(Id item)>& include = {}) {
    std::vector<std::pair<int, TimedEvent>> items;  // rank (0 off, 1 ctl, 2 on), event
    const time::TimeMap& tm = *s.timeMap;
    const std::int64_t origin = time::samplesAt(startSeconds, rate);
    std::int32_t noteId = 0;
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != track || it.mute || !it.activeTake) continue;
        if (include && !include(e.id)) continue;
        const model::Take* take = s.takes.find(*it.activeTake);
        if (!take || !take->source || take->source->kind != Kind::Midi) continue;
        const model::MidiSource* src = s.midi.find(*take->source);
        if (!src) continue;
        const midi::Sequence& seq = src->sequence;
        const model::ItemSpan span = model::itemSpan(s, it);
        const double startBeat = span.startBeat, endSec = span.endSec;
        const double rateScale = take->playrate > 0 ? take->playrate : 1.0;
        const double loopBeats = seq.ppq > 0 ? static_cast<double>(seq.length) / seq.ppq : 0.0;
        const bool looping = it.loop && loopBeats > 0;
        const model::SourceBeatMapping mapping{take->startOffset, rateScale, loopBeats};
        const double sourceOffset = mapping.sourceOffset(looping);
        auto toSample = [&](double contentBeat, double pass) -> std::optional<std::int64_t> {
            const double itemBeat = mapping.itemBeat(contentBeat, pass, looping);
            if (itemBeat < 0) return std::nullopt;
            const double sec = tm.secondsAt(startBeat + itemBeat);
            return time::samplesAt(std::min(sec, endSec), rate) - origin;
        };
        const std::int64_t endSample = time::samplesAt(endSec, rate) - origin;
        const int reps =
            looping
                ? 1 + static_cast<int>(std::floor(
                          ((tm.beatAt(endSec) - startBeat) * rateScale + sourceOffset) / loopBeats))
                : 1;
        // Include the previous cycle so a note-off that spans the source
        // boundary can end the voice retriggered at the item's start.
        for (int k = looping ? -1 : 0; k < reps; ++k) {
            const double offset = k * loopBeats;
            for (const midi::Note& n : seq.notes) {
                if (looping && n.start >= seq.length) continue;
                const double b0 = static_cast<double>(n.start) / seq.ppq;
                auto on = toSample(b0, k);
                // A take can start in the middle of a note (for example after
                // item.split). Retrigger the held note at the item's boundary.
                if (!on) {
                    if (static_cast<double>(n.start + n.length) / seq.ppq + offset <= sourceOffset)
                        continue;
                    on = time::samplesAt(tm.secondsAt(startBeat), rate) - origin;
                }
                if (*on >= endSample) continue;
                auto off = toSample(static_cast<double>(n.start + n.length) / seq.ppq, k);
                const std::int64_t offS = std::max(*on + 1, off ? *off : *on + 1);
                TimedEvent a;
                a.sample = *on;
                a.origin = {e.id.n, it.activeTake->n, take->source->n, n.start, k};
                a.event.kind = host::Event::Kind::NoteOn;
                a.event.channel = n.channel;
                a.event.key = n.pitch;
                a.event.noteId = noteId;
                a.event.velocity = n.velocity / 127.0;
                items.push_back({2, a});
                TimedEvent b = a;
                b.sample = offS;
                b.event.kind = host::Event::Kind::NoteOff;
                b.event.velocity = n.offVelocity / 127.0;
                items.push_back({0, b});
                ++noteId;
            }
            for (const midi::Control& c : seq.controls) {
                auto at = toSample(static_cast<double>(c.tick) / seq.ppq, k);
                if (!at || *at >= endSample) continue;
                TimedEvent ev;
                ev.sample = *at;
                ev.event.kind = host::Event::Kind::Midi;
                const auto ch = static_cast<std::uint8_t>(c.channel & 0x0f);
                auto b7 = [](std::int32_t v) {
                    return static_cast<std::uint8_t>(std::clamp(v, 0, 127));
                };
                switch (c.kind) {
                    case midi::ControlKind::CC:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xb0 | ch);
                        ev.event.midi[1] = b7(c.number);
                        ev.event.midi[2] = b7(c.value);
                        break;
                    case midi::ControlKind::ProgramChange:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xc0 | ch);
                        ev.event.midi[1] = b7(c.value);
                        break;
                    case midi::ControlKind::ChannelPressure:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xd0 | ch);
                        ev.event.midi[1] = b7(c.value);
                        break;
                    case midi::ControlKind::PolyPressure:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xa0 | ch);
                        ev.event.midi[1] = b7(c.number);
                        ev.event.midi[2] = b7(c.value);
                        break;
                    case midi::ControlKind::PitchBend: {
                        const std::int32_t v = std::clamp(c.value, -8192, 8191) + 8192;
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xe0 | ch);
                        ev.event.midi[1] = static_cast<std::uint8_t>(v & 0x7f);
                        ev.event.midi[2] = static_cast<std::uint8_t>((v >> 7) & 0x7f);
                        break;
                    }
                }
                items.push_back({1, ev});
            }
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second.sample != b.second.sample) return a.second.sample < b.second.sample;
        return a.first < b.first;
    });
    std::vector<TimedEvent> out;
    out.reserve(items.size());
    for (auto& [rank, ev] : items)
        if (ev.sample >= 0) out.push_back(ev);  // no chasing of notes before the start (yet)
    return out;
}

// Topological order: every track after the tracks feeding it; ties by ID.
Result<std::vector<Id>> topoOrder(const Session& s) {
    std::map<Id, std::set<Id>> feeds;  // source -> targets
    std::map<Id, int> indeg;
    for (const auto& e : s.tracks) indeg[e.id];
    for (const auto& e : s.racks) indeg[e.id];  // racks are nodes too (ADR-0062)
    auto link = [&](Id a, Id b) {
        if (feeds[a].insert(b).second) ++indeg[b];
    };
    for (const auto& e : s.tracks)
        if (e.value->parent && e.value->parentSend.enabled) link(e.id, *e.value->parent);
    for (const auto& e : s.routes) {
        const model::Route& r = *e.value;
        std::optional<Id> from = r.from.track;
        std::optional<Id> to = r.to.fx ? model::trackOfFx(s, *r.to.fx) : r.to.track;
        if (r.from.module)
            if (const model::RackModule* m = s.modules.find(*r.from.module)) from = m->rack;
        if (r.to.module)
            if (const model::RackModule* m = s.modules.find(*r.to.module)) to = m->rack;
        if (from && to) link(*from, *to);
    }
    std::set<Id> ready;
    for (const auto& [id, d] : indeg)
        if (d == 0) ready.insert(id);
    std::vector<Id> order;
    while (!ready.empty()) {
        const Id id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        for (Id t : feeds[id])
            if (--indeg[t] == 0) ready.insert(t);
    }
    if (order.size() != s.tracks.size() + s.racks.size())
        return Error("engine.cycle", "routing cycle", {},
                     "remove a route that feeds back to its source, or change its destination to "
                     "break the cycle");
    return order;
}

}  // namespace

std::vector<TimedEvent> compileTrackEvents(const Session& s, Id track, double startSeconds,
                                           double rate) {
    return trackEvents(s, track, startSeconds, rate);
}

// Resolves an envelope's plugin parameter: a semantic key (physical values)
// or, for any plugin, a CLAP id ("12") or parameter name (CLAP values).
struct ResolvedLane {
    ParamLane lane;
    double lo = 0.0, hi = 1.0;
    double toLaneDomain(double v) const {
        if (lane.spec)
            return params::toNormalized(lane.spec->spec, params::clampPhysical(lane.spec->spec, v));
        return std::clamp(v, lo, hi);
    }
};

// A plain lane whose instance takes normalized parameter values (VST3): the
// envelope points move into the plugin's normalized domain, so the events the
// audio thread sends need no conversion there. An edit controller is called
// here, on the compile (main) thread, never on the audio thread.
bool toNormalizedDomain(host::Instance& in, ParamLane& lane) {
    for (auto& pt : lane.curve.points) {
        auto n = in.normalizedParam(lane.clapId, pt.value);
        if (!n) return false;
        pt.value = *n;
    }
    return true;
}

Result<ResolvedLane> paramLane(host::Instance& in, const std::string& param,
                               std::map<host::Instance*, json::Value>& schemas) {
    ResolvedLane r;
    const auto list = in.params();
    if (in.info().semantic) {
        auto it = schemas.find(&in);
        if (it == schemas.end()) {
            auto schema = in.semanticSchema();
            it = schemas.emplace(&in, schema ? std::move(*schema) : json::Value()).first;
        }
        const json::Value* entry = nullptr;
        if (const json::Value* ps = it->second.isObject() ? it->second.find("params") : nullptr)
            entry = ps->find(param);
        if (entry) {
            auto d = params::fromDescription(param, *entry);
            if (!d) return std::move(d).error();
            if ((*d)->spec.flags & params::kReadonly)
                return Error("engine.automation", param + " is read-only", {},
                             "choose a writable parameter from fx.describe or remove the obsolete "
                             "envelope with env.remove");
            if (std::none_of(list.begin(), list.end(),
                             [&](const auto& info) { return info.id == (*d)->spec.id; }))
                return Error("engine.automation",
                             param + " is absent from the current parameter list", {},
                             "choose a writable parameter from fx.describe or remove the obsolete "
                             "envelope with env.remove");
            r.lane.clapId = (*d)->spec.id;
            r.lane.spec = std::shared_ptr<const params::DescribedParam>(std::move(*d));
            return r;
        }
    }
    const bool numeric = !param.empty() && std::all_of(param.begin(), param.end(),
                                                       [](char c) { return c >= '0' && c <= '9'; });
    for (const host::ParamInfo& pi : list) {
        const bool match = numeric ? std::to_string(pi.id) == param : pi.name == param;
        if (!match) continue;
        if (pi.flags & CLAP_PARAM_IS_READONLY)
            return Error("engine.automation", param + " is read-only", {},
                         "choose a writable parameter from fx.describe or remove the obsolete "
                         "envelope with env.remove");
        r.lane.clapId = pi.id;
        r.lo = pi.min;
        r.hi = pi.max;
        return r;
    }
    return Error("engine.automation", in.info().name + " has no parameter \"" + param + "\"", {},
                 "choose a writable parameter from fx.describe or remove the obsolete envelope "
                 "with env.remove");
}

Result<Plan> compile(const Session& s, InstancePool& pool, double startSeconds, std::uint32_t block,
                     double rate, const std::vector<model::Id>* keepLive,
                     std::span<const RecordingMaster> recording) {
    SUPPLE_TRY(model::validateFrozenState(s));
    Plan p;
    p.pool = &pool;
    p.linearConsole = !s.project->console9;
    p.rate = rate > 0 ? rate : s.project->sampleRate;
    p.smoothFrames = static_cast<std::uint32_t>(std::max(1.0, std::round(0.010 * p.rate)));
    p.block = block;
    p.startSeconds = startSeconds;
    p.timeMap = s.timeMap;
    auto order = topoOrder(s);
    if (!order) return std::move(order).error();
    std::map<Id, std::size_t> index;
    for (std::size_t i = 0; i < order->size(); ++i) index[(*order)[i]] = i;
    p.mainMaster = index.at(model::kMaster);
    for (Id m : s.project->masters) {  // ADR-0056
        const auto channels = model::hardwareOutputs(*s.tracks.find(m), m);
        if (channels.empty()) continue;
        p.outputs.push_back(
            {index.at(m), channels[0] - 1, channels.size() > 1 ? channels[1] - 1 : -1});
    }
    p.nodes.resize(order->size());
    for (std::size_t i = 0; i < order->size(); ++i) {
        Node& n = p.nodes[i];
        n.id = (*order)[i];
        if (n.id.kind == Kind::Rack) {
            SUPPLE_TRY(compileRack(p, n, s, n.id, pool));
            continue;
        }
        const model::Track& t = *s.tracks.find(n.id);
        n.volumeDb = t.volumeDb;
        n.pan = t.pan;
        n.type = t.type;
        n.master = t.type == model::StripType::Master;
        n.decodeInput = t.type != model::StripType::Channel;
        n.postItems = n.master;  // a master's items join after its plain stage (ADR-0057)
        // A recorded master (armed, or still recording) and its items while it
        // records: silent in the punch range, else everywhere (ADR-0057).
        const auto recorded =
            std::find_if(recording.begin(), recording.end(),
                         [&](const RecordingMaster& r) { return r.master == n.id; });
        if (n.master && (t.record.armed || recorded != recording.end()))
            p.recordTaps.push_back({i, -1, {}});
        n.dropItems =
            n.master && (t.laneMute || (recorded != recording.end() && recorded->everywhere));
        if (n.master && recorded != recording.end() && !recorded->everywhere) {
            const auto origin = time::samplesAt(startSeconds, p.rate);
            n.silentFrom = time::samplesAt(recorded->startSeconds, p.rate) - origin;
            n.silentTo = time::samplesAt(recorded->endSeconds, p.rate) - origin;
        }
        n.staticGain = stripGain(n.master, t.volumeDb);
        n.staticPan = console9::nativePan(t.pan);
        n.encoder.init(console9::seedsFor(s.project->seed, (n.id.n << 3) | 1), p.linearConsole);
        n.keyEncoder.init(console9::seedsFor(s.project->seed, (n.id.n << 3) | 1), p.linearConsole);
        n.decoder.init(console9::seedsFor(s.project->seed, (n.id.n << 3) | 2), p.linearConsole);
        n.mute = t.mute;
        n.phaseInvert = t.phaseInvert;
        n.mono = t.mono;
        auto makeStage = [&](Id fxId) {
            const model::FxInstance& fx = *s.fx.find(fxId);
            Stage st;
            st.fx = fxId;
            st.bypass = fx.bypass || fx.offline;
            st.wet = fx.wet / 100.0;
            st.autoBypassOnSilence = fx.autoBypassOnSilence;
            st.pdcIgnore = fx.pdc == model::PdcMode::Ignore;
            st.parallel = fx.parallel;
            st.os = fx.oversampling == 2 || fx.oversampling == 4 || fx.oversampling == 8
                        ? fx.oversampling
                        : 1;
            if (!fx.offline) {
                auto inst = pool.get(fxId, fx, s);
                if (!inst) {
                    p.warnings.push_back(fxId.str() + " (" + fx.plugin.id +
                                         "): " + inst.error().message + "; passing audio through");
                } else {
                    st.instance = *inst;
                }
            }
            return st;
        };
        n.auxCount = std::clamp(t.channels - 2, 0, model::kMaxTrackChannels - 2);
        n.freeze = t.freeze;
        if (n.freeze) continue;  // Original objects remain in the model, not this plan.
        const int channels = 2 + n.auxCount;
        for (Id fxId : t.chain) {
            Stage st = makeStage(fxId);
            // Pins (ADR-0020): which track channels the plug-in reads, writes
            // and keys from.
            const model::PinMap& pins = s.fx.find(fxId)->pins;
            auto valid = [&](const std::vector<int>& list, const char* what) {
                for (int c : list)
                    if (c < 1 || c > channels) {
                        p.warnings.push_back(fxId.str() + ": " + what + " pin " +
                                             std::to_string(c) + " is beyond the track's " +
                                             std::to_string(channels) + " channels; ignored");
                        return false;
                    }
                return !list.empty();
            };
            if (valid(pins.in, "input")) {
                st.inPin[0] = pins.in[0] - 1;
                st.inPin[1] = pins.in[pins.in.size() > 1 ? 1 : 0] - 1;
            }
            // out: the main pair, then the auxiliary outputs' channels (ADR-0021).
            const std::vector<int> mainOut(
                pins.out.begin(),
                pins.out.begin() +
                    static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, pins.out.size())));
            if (valid(mainOut, "output")) {
                st.outPin[0] = mainOut[0] - 1;
                st.outPin[1] = mainOut.size() > 1 ? mainOut[1] - 1 : -1;
            }
            for (std::size_t k = 2; k < pins.out.size(); ++k) {
                int c = pins.out[k];
                if (c < 0 || c > channels) {
                    p.warnings.push_back(fxId.str() + ": output pin " + std::to_string(c) +
                                         " is beyond the track's " + std::to_string(channels) +
                                         " channels; ignored");
                    c = 0;
                }
                st.auxOutPin.push_back(c - 1);
            }
            st.pinned =
                st.inPin[0] != 0 || st.inPin[1] != 1 || st.outPin[0] != 0 || st.outPin[1] != 1;
            if (valid(pins.key, "key")) {
                if (!st.instance || st.instance->sidechainChannels() == 0) {
                    p.warnings.push_back(fxId.str() +
                                         " has no sidechain input; its key pins are ignored");
                } else {
                    for (int c : pins.key) st.keyPins.push_back(c - 1);
                    st.keyed = true;
                }
            }
            if (st.os > 1 && !st.auxOutPin.empty()) {
                p.warnings.push_back(fxId.str() +
                                     ": oversampling is not available with auxiliary outputs; "
                                     "it runs at the project rate");
                st.os = 1;
            }
            if (st.parallel && !n.chain.empty() && (st.pinned || n.auxCount > 0))
                p.warnings.push_back(fxId.str() +
                                     ": parallel is not available with pins or auxiliary "
                                     "channels; it runs in series");
            n.chain.push_back(std::move(st));
        }
        // Input FX (channels only): run on the record input, see processNode.
        if (t.type == model::StripType::Channel)
            for (Id fxId : t.inputChain) n.inputChain.push_back(makeStage(fxId));
        // Takes with FX or envelopes play through a chain of their own (gap log #22).
        for (const auto& e : s.items) {
            const model::Item& it = *e.value;
            if (it.track != n.id || it.mute || !it.activeTake || n.dropItems) continue;
            const model::Take* take = s.takes.find(*it.activeTake);
            if (!take) continue;
            const bool enveloped =
                std::any_of(s.envelopes.begin(), s.envelopes.end(), [&](auto& en) {
                    return en.value->target.object == *it.activeTake && en.value->active &&
                           !en.value->points.empty();
                });
            if (take->chain.empty() && !enveloped) continue;
            TakeChain tc;
            tc.item = e.id;
            tc.take = *it.activeTake;
            for (Id fxId : take->chain) tc.chain.push_back(makeStage(fxId));
            n.takeChains.push_back(std::move(tc));
        }
    }
    for (const Node& n : p.nodes)
        forEachStage(n, [&](const Stage& st) {
            if (st.instance) p.instances.push_back(st.instance);
        });
    std::sort(p.instances.begin(), p.instances.end());
    p.instances.erase(std::unique(p.instances.begin(), p.instances.end()), p.instances.end());
    // Edges: children's parent sends, then routes; each target sums its
    // inputs in (source order, edge id) order.
    struct Pending {
        std::size_t from, to;
        Edge e;
        std::uint64_t key;
    };
    std::vector<Pending> pending;
    for (const auto& e : s.tracks) {
        const model::Track& t = *e.value;
        if (!t.parent || !t.parentSend.enabled) continue;
        Edge ed;
        ed.from = index.at(e.id);
        ed.tap = model::Tap::PostFader;
        pending.push_back({index.at(e.id), index.at(*t.parent), std::move(ed), 0});
    }
    for (const auto& e : s.routes) {
        const model::Route& r = *e.value;
        // Racks (ADR-0062): an Out module into its channel, unity; a send
        // into an In module, at the route's tap, level and pan.
        if (r.from.module && r.to.track) {
            const model::RackModule* m = s.modules.find(*r.from.module);
            if (!m || !index.count(m->rack) || !index.count(*r.to.track)) continue;
            const Node& rack = p.nodes[index.at(m->rack)];
            if (!rack.rack) continue;
            const auto& outs = rack.rack->outIds;
            const auto at = std::find(outs.begin(), outs.end(), *r.from.module);
            if (at == outs.end()) continue;  // a missing Out (its type is not known)
            Edge ed;
            ed.from = index.at(m->rack);
            ed.route = e.id;
            ed.rackOut = static_cast<int>(at - outs.begin());
            pending.push_back({ed.from, index.at(*r.to.track), std::move(ed), 1 + e.id.n});
            continue;
        }
        int rackIn = -1;
        std::optional<Id> toTrack = r.to.fx ? model::trackOfFx(s, *r.to.fx) : r.to.track;
        if (r.to.module) {
            const model::RackModule* m = s.modules.find(*r.to.module);
            if (!m || !index.count(m->rack) || !p.nodes[index.at(m->rack)].rack) continue;
            const auto& ins = p.nodes[index.at(m->rack)].rack->inIds;
            const auto at = std::find(ins.begin(), ins.end(), *r.to.module);
            if (at == ins.end()) continue;
            rackIn = static_cast<int>(at - ins.begin());
            toTrack = m->rack;
        }
        if (!r.from.track || !toTrack) continue;
        Edge ed;
        ed.rackIn = rackIn;
        ed.from = index.at(*r.from.track);
        ed.route = e.id;
        if (r.to.fx) {  // a key route: into that stage's sidechain input (ADR-0018)
            const model::Track& owner = *s.tracks.find(*toTrack);
            ed.stage = static_cast<int>(
                std::find(owner.chain.begin(), owner.chain.end(), *r.to.fx) - owner.chain.begin());
            Stage& st = p.nodes[index.at(*toTrack)].chain[static_cast<std::size_t>(ed.stage)];
            if (!st.instance || st.instance->sidechainChannels() == 0) {
                p.warnings.push_back(e.id.str() + ": " + r.to.fx->str() +
                                     " has no sidechain input; the key route is ignored");
                continue;
            }
            st.keyed = true;
        }
        ed.tap = r.tap;
        ed.volumeDb = r.volumeDb;
        ed.followPan = !r.pan.percent;
        ed.muted = r.mute;
        ed.pan = r.pan.resolve(s.tracks.find(*r.from.track)->pan);
        const double g = dbToGain(r.volumeDb);
        auto [pl, pr] = panGains(ed.pan / 100.0, -3.0);
        ed.gainL = g * pl;
        ed.gainR = g * pr;
        ed.mono = r.mono;
        ed.invert = r.phaseInvert;
        if (!r.channels.from.empty()) {
            // Main pairs make the matrix; pairs that touch auxiliary channels
            // become links (ADR-0020).
            const int fromChannels = 2 + p.nodes[ed.from].auxCount;
            const int toChannels =
                r.to.fx || r.to.module ? 2 : 2 + p.nodes[index.at(*toTrack)].auxCount;
            float m[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
            bool anyMain = false;
            for (std::size_t k = 0; k < r.channels.from.size() && k < r.channels.to.size(); ++k) {
                const int from = r.channels.from[k], to = r.channels.to[k];
                if (from < 1 || from > fromChannels || to < 1 || to > toChannels) {
                    p.warnings.push_back(e.id.str() + ": channel " +
                                         std::to_string(std::max(from, to)) +
                                         " is beyond its strip's channels; that pair is ignored");
                    continue;
                }
                if (from > 2 || to > 2) {
                    Edge::AuxLink link;
                    link.from = from - 1;
                    link.to = to - 1;
                    ed.auxLinks.push_back(std::move(link));
                    continue;
                }
                m[to - 1][from - 1] += 1.0f;
                anyMain = true;
            }
            ed.auxOnly = !anyMain && !ed.auxLinks.empty();
            ed.mapped = !(m[0][0] == 1 && m[0][1] == 0 && m[1][0] == 0 && m[1][1] == 1);
            std::copy(&m[0][0], &m[0][0] + 4, &ed.map[0][0]);
        }
        pending.push_back({ed.from, index.at(*toTrack), std::move(ed), 1 + e.id.n});
    }
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        return a.to != b.to ? a.to < b.to : a.from != b.from ? a.from < b.from : a.key < b.key;
    });
    for (auto& pe : pending) {
        p.nodes[pe.to].in.push_back(p.edges.size());
        pe.e.to = pe.to;
        p.edges.push_back(std::move(pe.e));
    }
    // Solo: with any track soloed, a track stays audible only if it is soloed,
    // solo-defeated, the master, or on a soloed track's signal path: what
    // feeds it (soloing a bus or folder keeps its sources and children) and
    // where it goes (soloing a source keeps its folders, buses and the master).
    // Siblings that merely share a folder with a soloed track are silenced.
    {
        std::set<Id> soloed;
        for (const auto& e : s.tracks)
            if (e.value->solo == model::Solo::On || e.value->solo == model::Solo::InPlace)
                soloed.insert(e.id);
        for (const auto& e : s.tracks)
            for (Id controller : model::vcaControllers(s, e.id))
                if (s.groups.find(controller)->vca->solo) soloed.insert(e.id);
        if (!soloed.empty()) {
            std::map<Id, std::vector<Id>> down, up;  // signal flow
            for (const auto& e : s.tracks)
                if (e.value->parent && e.value->parentSend.enabled) {
                    down[e.id].push_back(*e.value->parent);
                    up[*e.value->parent].push_back(e.id);
                }
            for (const auto& e : s.routes)
                if (e.value->from.track && e.value->to.track) {
                    down[*e.value->from.track].push_back(*e.value->to.track);
                    up[*e.value->to.track].push_back(*e.value->from.track);
                }
            std::set<Id> audible = soloed;
            auto flood = [&](std::map<Id, std::vector<Id>>& g) {
                std::vector<Id> stack(soloed.begin(), soloed.end());
                std::set<Id> seen;
                while (!stack.empty()) {
                    const Id id = stack.back();
                    stack.pop_back();
                    for (Id nxt : g[id])
                        if (seen.insert(nxt).second) {
                            audible.insert(nxt);
                            stack.push_back(nxt);
                        }
                }
            };
            flood(down);
            flood(up);
            for (Node& n : p.nodes) {
                if (n.rack) continue;  // a rack is heard through its channels
                const model::Track& t = *s.tracks.find(n.id);
                n.soloSilenced = !audible.count(n.id) && t.solo != model::Solo::Defeat && !n.master;
            }
        }
    }
    p.masterPlayrate = s.project->playrate;
    p.rateEnabled = p.masterPlayrate.rate != 1 || model::activePlayrateEnvelope(s).has_value();
    // Envelopes on mixer targets and plugin parameters.
    const std::int64_t origin = time::samplesAt(startSeconds, p.rate);
    for (const auto& e : s.envelopes) {
        const model::Envelope& env = *e.value;
        // What plays: the envelope with its automation items (gap log #28).
        const std::vector<model::EnvPoint> played =
            env.target.object.kind == Kind::Take ? env.points : model::effectivePoints(s, e.id);
        if (!env.active || played.empty()) continue;
        Automation a;
        for (const model::EnvPoint& pt : played) {
            const double sec =
                env.timebase == model::Timebase::Time ? pt.pos : s.timeMap->secondsAt(pt.pos);
            a.points.push_back(
                {time::samplesAt(sec, p.rate) - origin, pt.value, pt.shape, pt.tension});
        }
        const Id o = env.target.object;
        const std::string& param = env.target.param;
        Automation* slot = nullptr;
        if (o.kind == Kind::Track && index.count(o)) {
            Node& n = p.nodes[index.at(o)];
            slot = param == "playrate" && o == model::kMaster ? &p.rateEnvelope
                   : param == "volume"                        ? &n.autoVolume
                   : param == "pan"                           ? &n.autoPan
                   : param == "mute"                          ? &n.autoMute
                                                              : nullptr;
        } else if (o.kind == Kind::Route) {
            for (Edge& ed : p.edges)
                if (ed.route == o)
                    slot = param == "volume" ? &ed.autoVolume
                           : param == "pan"  ? &ed.autoPan
                           : param == "mute" ? &ed.autoMute
                                             : nullptr;
        } else if (o.kind == Kind::Take) {
            // Positions are item-relative beats: they move with the item.
            for (Node& n : p.nodes)
                for (TakeChain& tc : n.takeChains) {
                    if (tc.take != o) continue;
                    const model::Item* item = s.items.find(tc.item);
                    if (!item) continue;
                    const double startBeat = model::itemSpan(s, *item).startBeat;
                    a.points.clear();
                    for (const model::EnvPoint& pt : env.points)
                        a.points.push_back(
                            {time::samplesAt(s.timeMap->secondsAt(startBeat + pt.pos), p.rate) -
                                 origin,
                             pt.value, pt.shape, pt.tension});
                    slot = param == "volume" ? &tc.volume
                           : param == "pan"  ? &tc.pan
                           : param == "mute" ? &tc.mute
                                             : nullptr;
                }
            if (!slot) continue;  // an inactive take's, or not one the engine plays
        } else if (o.kind == Kind::Group) {
            continue;  // compiled below as per-consumer VCA controls
        } else if (o.kind == Kind::Fx) {
            continue;  // resolved after servicing/activation in prepare()
        } else if (o.kind == Kind::Module) {
            continue;  // compiled with its rack (compileRack)
        }
        if (slot)
            *slot = std::move(a);
        else
            p.warnings.push_back(e.id.str() + ": automation of " + o.str() + " " + param +
                                 " is not rendered yet");
    }
    // Input monitoring and the live region (ADR-0014): an armed track monitors
    // its record input when monitor is on or auto (auto, tape style, only while
    // stopped or recording: processNode decides per block); whatever it feeds
    // is live too (nodes are in topological order, so one pass propagates).
    for (Node& n : p.nodes) {
        if (n.rack) {
            // A rack playing a device's MIDI is live, as a monitored track.
            n.live = std::any_of(n.rack->notes.begin(), n.rack->notes.end(),
                                 [](const NotesFeed& f) { return f.device.has_value(); });
            for (std::size_t e : n.in) n.live = n.live || p.nodes[p.edges[e].from].live;
            if (!n.live && keepLive && std::binary_search(keepLive->begin(), keepLive->end(), n.id))
                n.live = p.heldLive = true;
            p.anyLive = p.anyLive || n.live;
            continue;
        }
        const model::Track& t = *s.tracks.find(n.id);
        const bool monitoring = t.record.armed && t.record.monitor != model::Monitor::Off;
        n.tapeAuto = t.record.monitor == model::Monitor::Auto;
        if (t.record.armed && t.record.input && !n.inputChain.empty())
            if (const model::Route* in = s.routes.find(*t.record.input); in && in->from.hwChannels)
                for (int ch : *in->from.hwChannels) n.recordInputs.push_back(ch - 1);
        if (monitoring && t.record.input)
            if (const model::Route* in = s.routes.find(*t.record.input)) {
                if (in->from.hwChannels)
                    for (int ch : *in->from.hwChannels) n.monitorInputs.push_back(ch - 1);
                if (in->from.midiDevice)
                    n.midiIn =
                        Node::MidiIn{*in->from.midiDevice, in->midi ? in->midi->fromChannel : 0,
                                     in->midi ? in->midi->toChannel : 0, 0};
            }
        n.live = !n.monitorInputs.empty() || n.midiIn.has_value() || !n.recordInputs.empty();
        for (std::size_t e : n.in) n.live = n.live || p.nodes[p.edges[e].from].live;
        if (!n.live && keepLive && std::binary_search(keepLive->begin(), keepLive->end(), n.id))
            n.live = p.heldLive = true;
        p.anyLive = p.anyLive || n.live;
    }
    // The master is the output: live whenever anything is (even a monitored
    // track that feeds nothing).
    if (p.anyLive) {
        p.nodes[p.mainMaster].live = true;
        // Every master the device plays: the callback writes their output.
        for (const OutputTap& o : p.outputs) p.nodes[o.node].live = true;
        // A main-master key may feed later strips, which must stay with the live executor.
        for (Node& n : p.nodes)
            for (std::size_t edge : n.in) n.live = n.live || p.nodes[p.edges[edge].from].live;
        // Auxiliary links do not cross the frontier (its rings carry the main
        // pair): a strip that feeds a live strip's aux channels, or is fed
        // from aux into a live one, plays live too, and what it feeds.
        for (bool changed = true; changed;) {
            changed = false;
            for (const Edge& e : p.edges)
                if (!e.auxLinks.empty() && !p.nodes[e.from].live) {
                    const auto to = static_cast<std::size_t>(
                        std::find_if(p.nodes.begin(), p.nodes.end(),
                                     [&](const Node& n) {
                                         return std::find(n.in.begin(), n.in.end(),
                                                          static_cast<std::size_t>(
                                                              &e - p.edges.data())) != n.in.end();
                                     }) -
                        p.nodes.begin());
                    if (to < p.nodes.size() && p.nodes[to].live) {
                        p.nodes[e.from].live = true;
                        changed = true;
                    }
                }
            for (Node& n : p.nodes)
                for (std::size_t edge : n.in)
                    if (!n.live && p.nodes[p.edges[edge].from].live) {
                        n.live = true;
                        changed = true;
                    }
        }
    }
    // Compile each control once, then copy its curves to independent consumers.
    std::map<Id, VcaControl> controls;
    for (const auto& e : s.groups) {
        if (!e.value->vca) continue;
        VcaControl v;
        v.volumeDb = e.value->vca->volumeDb;
        v.mute = e.value->vca->mute;
        v.slot = static_cast<std::uint32_t>(p.mix->vcaIds.size());
        p.mix->vcaIds.push_back(e.id);
        for (const auto& lane : s.envelopes) {
            if (lane.value->target.object != e.id || !lane.value->active) continue;
            const auto points = model::effectivePoints(s, lane.id);
            if (points.empty()) continue;
            Automation& curve = lane.value->target.param == "volume" ? v.volume : v.gate;
            curve.points.clear();
            for (const auto& pt : points) {
                const double sec = lane.value->timebase == model::Timebase::Time
                                       ? pt.pos
                                       : s.timeMap->secondsAt(pt.pos);
                curve.points.push_back(
                    {time::samplesAt(sec, p.rate) - origin, pt.value, pt.shape, pt.tension});
            }
        }
        controls.emplace(e.id, std::move(v));
    }
    std::size_t controlPoints = 0;
    for (Node& n : p.nodes)
        for (Id id : model::vcaControllers(s, n.id)) {
            const auto& v = controls.at(id);
            controlPoints += v.volume.points.size() + v.gate.points.size();
            if (controlPoints > 1000000)
                return Error("automation.budget", "VCA consumers exceed one million curve points",
                             "/groups", "reduce VCA automation points or membership");
            n.vcas.values.push_back(v);
        }
    for (Edge& e : p.edges) {
        if (e.stage >= 0) {
            e.stripPan = p.nodes[e.from].autoPan;
            continue;
        }
        if (e.auxOnly) continue;       // nothing reaches the main pair
        if (e.rackOut >= 0) continue;  // a rack's Out: its audio as is
        e.encode = e.rackIn >= 0 || e.tap != model::Tap::PostFader || e.volumeDb != 0 ||
                   !e.followPan || e.mono || e.invert || e.mapped || e.muted ||
                   e.autoVolume.active() || e.autoPan.active() || e.autoMute.active();
        if (!e.encode) continue;
        Node& source = p.nodes[e.from];
        source.out.push_back(static_cast<std::size_t>(&e - p.edges.data()));
        if (e.tap == model::Tap::PostFader) {
            for (const auto& v : source.vcas.values)
                controlPoints += v.volume.points.size() + v.gate.points.size();
            if (controlPoints > 1000000)
                return Error("automation.budget", "VCA consumers exceed one million curve points",
                             "/groups", "reduce VCA automation points or sends");
            e.vcas = source.vcas;
        }
        e.stripVolume = source.autoVolume;
        e.stripPan = source.autoPan;
        e.stripMute = source.autoMute;
        e.staticGain = sendGain(e.volumeDb, e.tap, source.volumeDb);
        e.staticPan = console9::nativePan(e.pan);
        const auto salt = std::rotl(source.id.n, 32) ^ ((e.route->n << 3) | 3);
        // A send into a rack is never Console 9 encoded (rack.md §2.4).
        e.encoder.init(console9::seedsFor(s.project->seed, salt), p.linearConsole || e.rackIn >= 0);
    }
    for (const Edge& edge : p.edges)
        if (edge.stage >= 0 && edge.tap == model::Tap::PostFader)
            p.nodes[edge.from].postKeyed = true;
    SUPPLE_TRY(checkStrips(p));
    // Mix-value slots (ADR-0060), holding what was compiled until a publication.
    MixSlots& mix = *p.mix;
    mix.strips = std::make_unique<MixSlots::Value[]>(p.nodes.size());
    mix.routes = std::make_unique<MixSlots::Value[]>(p.edges.size());
    mix.vcas = std::make_unique<MixSlots::Value[]>(mix.vcaIds.size());
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        mix.strips[i].volumeDb.store(p.nodes[i].volumeDb, std::memory_order_relaxed);
        mix.strips[i].pan.store(p.nodes[i].pan, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < p.edges.size(); ++i) {
        mix.routes[i].volumeDb.store(p.edges[i].volumeDb, std::memory_order_relaxed);
        mix.routes[i].pan.store(p.edges[i].pan, std::memory_order_relaxed);
    }
    for (const auto& [id, v] : controls)
        mix.vcas[v.slot].volumeDb.store(v.volumeDb, std::memory_order_relaxed);
    return p;
}

namespace {
// Parallel groups and compensated latency of one chain (stages prepared): a
// group's latency is its slowest branch's, the others delayed to match.
std::int64_t layoutChain(std::vector<Stage>& chain, bool allowParallel, std::size_t block) {
    std::int64_t total = 0;
    for (std::size_t k = 0; k < chain.size();) {
        std::size_t end = k + 1;
        if (allowParallel && !chain[k].pinned)
            while (end < chain.size() && chain[end].parallel && !chain[end].pinned) ++end;
        std::int64_t latency = 0;
        for (std::size_t j = k; j < end; ++j) latency = std::max(latency, compLatency(chain[j]));
        const bool grouped = end - k > 1;
        for (std::size_t j = k; j < end; ++j) {
            Stage& st = chain[j];
            st.before = total;
            st.group = grouped ? static_cast<int>(k) : -1;
            st.groupEnd = grouped && j + 1 == end;
            st.align.init(grouped ? latency - compLatency(st) : 0);
        }
        chain[k].groupIn.init(grouped ? block : 0);
        chain[k].groupOut.init(grouped ? block : 0);
        total += latency;
        k = end;
    }
    return total;
}
}  // namespace

// Activates instances, measures latencies, sizes buffers and delays.
Status prepare(Plan& p, const Session& s) {
    if (!p.preparedSources && p.media) {
        auto prepared = p.media->prepareSources(s, p.projectDir, p.rate);
        if (!prepared) return prepared.error();
        p.preparedSources = *prepared;
    }
    const std::size_t block = p.block;
    // One stage: activated at the plan's rate and block, buffers sized.
    auto prepareStage = [&](Stage& st) -> Status {
        if (st.instance) {
            host::Instance& in = *st.instance;
            p.pool->service(st.fx);
            const auto os = static_cast<std::uint32_t>(st.os);
            const bool keyChanged = in.sidechainActive() != (st.keyed && in.sidechainChannels());
            if (in.active() && (in.sampleRate() != p.rate * os || in.maxFrames() < block * os ||
                                (keyChanged && !in.processing())))
                in.deactivate();
            if (!in.active() && !p.pool->failure(st.fx)) {
                SUPPLE_TRY(in.setSidechainActive(st.keyed));
                SUPPLE_TRY(in.activate(p.rate * os, 1, p.block * os));
            }
            st.keyChannels = std::min<std::uint32_t>(in.sidechainChannels(), 2);
            st.instrument = in.audioInputChannels() == 0;
            if (p.pool->failure(st.fx)) {
                st.latency = p.pool->rememberedLatency(st.fx);
            } else if (os > 1) {
                // The resamplers' round trip (an instrument's output only its
                // downward half) plus the plug-in's own latency, at the high
                // rate, padded to a whole number of plan samples.
                st.osFilters.assign(4, {});
                for (auto& f : st.osFilters) f.prepare(st.os);
                const std::int64_t trip = st.osFilters[0].latency() * os;
                const std::int64_t high = in.latency() + (st.instrument ? trip / 2 : trip);
                const std::int64_t pad = (os - high % os) % os;
                st.osPad.init(pad);
                st.latency = (high + pad) / os;
                for (int c = 0; c < 2; ++c) {
                    st.osIn[c].assign(block * os, 0.0f);
                    st.osOut[c].assign(block * os, 0.0f);
                    st.osKey[c].assign(st.keyed ? block * os : 0, 0.0f);
                }
            } else {
                st.latency = in.latency();
            }
            if (in.active()) p.pool->rememberLatency(st.fx, static_cast<std::uint32_t>(st.latency));
            if (in.audioOutputChannels() != 2)
                return Error(
                    "engine.layout", in.info().name + " does not have a stereo output", {},
                    "choose a plugin output layout with a stereo main pair, or replace the plugin");
        }
        if (const auto* failure = p.pool->failure(st.fx))
            p.warnings.push_back(st.fx.str() + ": " + failure->code + ": " + failure->message +
                                 "; compensated pass-through; use fx.restart");
        st.dry.init(st.latency);
        st.out.init(block);
        st.dryBuf.init(block);
        st.key.init(st.keyed ? block : 0);
        if (st.instance && st.instance->acceptsNotes()) {
            st.voices.reserve(4096);
            st.pendingVoices.reserve(4096);
        }
        st.outStorage.resize(256);
        st.outEvents.storage = st.outStorage;
        return {};
    };
    p.processedInputs.clear();
    for (Node& n : p.nodes) {
        for (Stage& st : n.rackStages) SUPPLE_TRY(prepareStage(st));
        for (Stage& st : n.inputChain) SUPPLE_TRY(prepareStage(st));
        n.inputLatency = layoutChain(n.inputChain, true, block);
        n.inputBuf.init(n.inputChain.empty() ? 0 : block);
        if (!n.inputChain.empty() && !n.recordInputs.empty())
            p.processedInputs.push_back(
                {n.id, {n.inputBuf.l.data(), n.inputBuf.r.data()}, n.inputLatency});
        for (TakeChain& tc : n.takeChains) {
            int stageIndex = 0;
            for (Stage& st : tc.chain) {
                SUPPLE_TRY(prepareStage(st));
                if (tc.noteTarget < 0 && st.instance && st.instance->acceptsNotes())
                    tc.noteTarget = stageIndex;
                ++stageIndex;
            }
            tc.latency = layoutChain(tc.chain, true, block);
            tc.buf.init(block);
        }
        int stageIndex = 0;
        for (Stage& st : n.chain) {
            SUPPLE_TRY(prepareStage(st));
            if (n.noteTarget < 0 && st.instance && st.instance->acceptsNotes())
                n.noteTarget = stageIndex;
            // Auxiliary outputs as far as the plug-in has them (ADR-0021).
            const std::size_t auxOuts = st.instance ? st.instance->auxOutputChannels() : 0;
            if (st.auxOutPin.size() > auxOuts) st.auxOutPin.resize(auxOuts);
            while (!st.auxOutPin.empty() && st.auxOutPin.back() < 0) st.auxOutPin.pop_back();
            st.auxOut.assign(st.auxOutPin.size(), std::vector<float>(block, 0.0f));
            st.auxOutPtr.clear();
            for (auto& buffer : st.auxOut) st.auxOutPtr.push_back(buffer.data());
            st.chanDelay.clear();
            if (st.pinned || n.auxCount > 0) {
                st.chanDelay.resize(static_cast<std::size_t>(2 + n.auxCount));
                for (MonoDelay& d : st.chanDelay) d.init(st.latency);
                st.pinBuf.init(block);
            }
            ++stageIndex;
        }
        n.latChain = n.freeze ? time::samplesAt(
                                    double(n.freeze->latencyFrames) / s.project->sampleRate, p.rate)
                              : layoutChain(n.chain, n.auxCount == 0, block);
        n.aux.assign(static_cast<std::size_t>(n.auxCount), std::vector<float>(block, 0.0f));
        n.sum.init(block);
        n.preFx.init(block);
        n.preFader.init(block);
        n.post.init(block);
        n.items.init(n.postItems ? block : 0);
        n.mix.init(n.postItems ? block : 0);
        if (!n.vcas.empty()) n.auxPostGain.resize(block);
        if (n.postKeyed) n.postKey.init(block);
    }
    for (RecordTap& tap : p.recordTaps) tap.pulled.init(block);
    // Parameter topology may have changed during main-thread servicing or
    // activation. Resolve from model points now, without touching a live plan.
    std::map<host::Instance*, json::Value> schemas;
    const auto laneOrigin = time::samplesAt(p.startSeconds, p.rate);
    for (Node& node : p.nodes)
        forEachStage(node, [&](Stage& st) {
            st.lanes.clear();
            if (!st.instance) return;
            for (const auto& entry : s.envelopes) {
                const auto& env = *entry.value;
                if (env.target.object != st.fx || !env.active) continue;
                auto points = model::effectivePoints(s, entry.id);
                if (points.empty()) continue;
                auto resolved = paramLane(*st.instance, env.target.param, schemas);
                if (!resolved) {
                    p.warnings.push_back(st.fx.str() + ": orphaned automation parameter " +
                                         env.target.param + ": " + resolved.error().message);
                    continue;
                }
                if (p.pool->automationCleared(st.fx, resolved->lane.clapId)) {
                    p.warnings.push_back(st.fx.str() + ": automation cleared for parameter " +
                                         env.target.param +
                                         "; envelope retained until rescan/reload");
                    continue;
                }
                for (const auto& pt : points) {
                    const double sec = env.timebase == model::Timebase::Time
                                           ? pt.pos
                                           : s.timeMap->secondsAt(pt.pos);
                    resolved->lane.curve.points.push_back(
                        {time::samplesAt(sec, p.rate) - laneOrigin,
                         resolved->toLaneDomain(pt.value), pt.shape, pt.tension});
                }
                if (!resolved->lane.spec && st.instance->paramsNormalized() &&
                    !toNormalizedDomain(*st.instance, resolved->lane)) {
                    p.warnings.push_back(st.fx.str() + ": " + env.target.param +
                                         " cannot be automated by this plugin");
                    continue;
                }
                st.lanes.push_back(std::move(resolved->lane));
            }
        });
    // Inactive, or waiting for a restart it asked for: no executor runs it (the
    // executors stop it on adopting this plan), so the restart can happen.
    std::erase_if(p.instances, [&](auto* in) { return !in->active() || p.pool->restarting(in); });
    // Each input constraint joins a source tap to a destination stage. Aux
    // channels leave after the chain even when the main pair uses a pre-FX tap.
    auto inputs = [&](std::size_t target, auto&& visit) {
        Node& n = p.nodes[target];
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            const Node& source = p.nodes[e.from];
            const auto before = e.stage < 0 ? 0 : n.chain[static_cast<std::size_t>(e.stage)].before;
            if (!e.auxOnly) visit(e.from, tapOffset(source, e.tap), before, e.delay);
            if (e.stage < 0)
                for (auto& link : e.auxLinks)
                    visit(e.from, tapOffset(source, link.from >= 2 ? model::Tap::PostFader : e.tap),
                          0, link.delay);
        }
    };
    // Retain the forward arrivals as diagnostics and the main output's startup
    // anchor. Actual source timing and edge padding come from the reverse solve.
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        Node& n = p.nodes[i];
        n.latIn = 0;
        inputs(i, [&](std::size_t from, std::int64_t tap, std::int64_t before, auto&) {
            n.latIn = std::max(n.latIn, p.nodes[from].latIn + tap - before);
        });
        n.timelineShift = INT64_MAX;
    }
    // The anchors: the main master and every master the device plays
    // (ADR-0056). They share one output latency, the largest of theirs, so
    // their outputs reach the device in step; with only the main master this
    // is its own latency, as before.
    std::vector<std::size_t> anchors{p.mainMaster};
    for (const OutputTap& o : p.outputs)
        if (std::find(anchors.begin(), anchors.end(), o.node) == anchors.end())
            anchors.push_back(o.node);
    p.outputLatency = 0;
    for (std::size_t a : anchors)
        p.outputLatency = std::max(p.outputLatency, p.nodes[a].latIn + p.nodes[a].latChain);
    for (std::size_t a : anchors) p.nodes[a].timelineShift = p.outputLatency - p.nodes[a].latChain;
    // ADR-0051: S = outputLatency - downstreamLatency. Schedule every source
    // as late as its downstream paths permit, leaving padding only at splits.
    // Classification/monitoring never enters this solve. The main master is
    // the same anchor in offline and realtime plans (also for selected taps).
    for (std::size_t i = p.nodes.size(); i-- > 0;) {
        const Node& n = p.nodes[i];
        if (n.timelineShift == INT64_MAX) continue;
        inputs(i, [&](std::size_t from, std::int64_t tap, std::int64_t before, auto&) {
            auto& shift = p.nodes[from].timelineShift;
            shift = std::min(shift, n.timelineShift + before - tap);
        });
    }
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        Node& n = p.nodes[i];
        // Nodes not upstream of main follow their already scheduled sources;
        // an unused master must not constrain the audible monitoring path.
        if (n.timelineShift == INT64_MAX) {
            n.timelineShift = n.latIn;
            inputs(i, [&](std::size_t from, std::int64_t tap, std::int64_t before, auto&) {
                n.timelineShift =
                    std::max(n.timelineShift, p.nodes[from].timelineShift + tap - before);
            });
        }
        assert(n.timelineShift >= n.latIn);
        inputs(i, [&](std::size_t from, std::int64_t tap, std::int64_t before, auto& line) {
            const auto delay = n.timelineShift + before - p.nodes[from].timelineShift - tap;
            assert(delay >= 0);
            line.init(delay);
        });
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            e.tmp.init(block);
            if (e.encode) e.encoded.init(block);
            e.auxTmp.assign(e.auxLinks.empty() ? 0 : block, 0.0f);
        }
    }
    // A key route from one output master into another's path can pull the
    // first one earlier than the others (ADR-0056).
    for (const OutputTap& o : p.outputs) {
        const Node& m = p.nodes[o.node];
        if (const auto early = p.outputLatency - m.timelineShift - m.latChain; early > 0)
            p.warnings.push_back(m.id.str() + " reaches the device " + std::to_string(early) +
                                 " samples early: a key route from it feeds another master's "
                                 "path");
    }
    // Automation lines up with the signal where it is applied: the fader sees
    // the track's input and chain latency, a route's gain its target's input.
    for (Node& n : p.nodes) {
        const std::int64_t at = n.timelineShift + n.latChain;
        // Items play at the input on channels, after the chain on masters.
        n.itemShift = n.postItems ? at : n.timelineShift;
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->shift(at);
        n.vcas.shift(at);
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            auto shift = e.encode
                             ? (p.nodes[e.from].timelineShift + tapOffset(p.nodes[e.from], e.tap))
                             : n.timelineShift;
            if (e.stage >= 0) shift += n.chain[static_cast<std::size_t>(e.stage)].before;
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->shift(shift);
            e.vcas.shift(shift);
        }
        // A plugin's parameters: its input is the track's, delayed by the
        // stages before it.
        for (Stage& st : n.chain) {
            st.inputLatency = n.timelineShift + st.before;
            for (ParamLane& ln : st.lanes) ln.curve.shift(st.inputLatency);
        }
        // A take chain's output lines up with the track's input; its stages
        // run its latency ahead of it.
        for (TakeChain& tc : n.takeChains) {
            for (Automation* a : {&tc.volume, &tc.pan, &tc.mute}) a->shift(n.itemShift);
            const std::int64_t ahead = n.itemShift - tc.latency;
            for (Stage& st : tc.chain) {
                st.inputLatency = ahead + st.before;
                for (ParamLane& ln : st.lanes) ln.curve.shift(st.inputLatency);
            }
        }
    }
    // Take chains: the item's span, gain, pan and fades (applied after the chain).
    for (Node& n : p.nodes)
        for (TakeChain& tc : n.takeChains) {
            const model::Item& it = *s.items.find(tc.item);
            const model::Take& take = *s.takes.find(tc.take);
            const model::ItemSpan span = model::itemSpan(s, it);
            const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
            tc.start = time::samplesAt(span.startSec, p.rate) - origin + n.itemShift;
            tc.end = time::samplesAt(span.endSec, p.rate) - origin + n.itemShift;
            const double g =
                dbToGain(it.gainDb) * dbToGain(take.gainDb) * (take.phaseInvert ? -1.0 : 1.0);
            const auto [pl, pr] =
                take.pan == 0.0 ? std::pair{1.0, 1.0} : panGains(take.pan / 100.0, -3.0);
            tc.gainL = g * pl;
            tc.gainR = g * pr;
            tc.fadeIn = time::samplesAt(it.fadeIn.length, p.rate);
            tc.fadeOut = time::samplesAt(it.fadeOut.length, p.rate);
            tc.fadeInShape = it.fadeIn.shape;
            tc.fadeOutShape = it.fadeOut.shape;
            tc.fadeInCurve = it.fadeIn.curve;
            tc.fadeOutCurve = it.fadeOut.curve;
        }
    // Frozen preparation is localized here so a prepared-source service can own it later.
    // Audio items and events share the track input's solved timeline shift.
    for (Node& n : p.nodes) {
        if (n.freeze) {
            const auto* source = s.media.find(n.freeze->source);
            if (!source)
                return Error("freeze.media_missing", "frozen source is missing", "/track",
                             "restore the frozen media or use track.unfreeze");
            std::filesystem::path path = source->path;
            if (path.is_relative()) {
                const auto inMedia = p.projectDir / s.project->mediaDir / path;
                path = std::filesystem::exists(inMedia) ? inMedia : p.projectDir / path;
            }
            auto data = p.media->get(path, p.rate, {}, source->hash);
            if (!data) return data.error();
            if ((*data)->truncated || (*data)->channelCount() != std::size_t(2 + n.auxCount))
                return Error("freeze.media_missing",
                             "frozen media is truncated or has wrong channels", "/track",
                             "restore the frozen media or use track.unfreeze");
            n.frozenAudio = *data;
            continue;
        }
        for (const auto& e : s.items) {
            const model::Item& it = *e.value;
            if (it.track != n.id || it.mute || !it.activeTake || n.dropItems) continue;
            const model::Take* take = s.takes.find(*it.activeTake);
            if (!take || !take->source || take->source->kind != Kind::Media) continue;
            const model::MediaSource* src = s.media.find(*take->source);
            if (!src) continue;
            std::filesystem::path path = src->path;
            if (path.is_relative()) {
                const auto inMedia = p.projectDir / s.project->mediaDir / path;
                path = std::filesystem::exists(inMedia) ? inMedia : p.projectDir / path;
            }
            // The take's playback variant (session-model §1.5): resample
            // (varispeed, the pitch follows the rate) or a pitch-preserving
            // stretch. Built and kept once per combination (engine.md §4).
            MediaVariant variant;
            variant.playrate = take->playrate > 0.0 ? take->playrate : 1.0;
            variant.pitch = take->pitch;
            variant.preservePitch = take->preservePitch;
            variant.pitchMode = take->pitchMode;
            const auto& prepared = p.preparedSources;
            const auto found = prepared ? prepared->takes.find(*it.activeTake)
                                        : std::map<Id, PreparedSource>::const_iterator{};
            if (!prepared || found == prepared->takes.end() || !found->second.data) {
                p.warnings.push_back(
                    e.id.str() + ": " +
                    (prepared && found != prepared->takes.end() && found->second.error
                         ? found->second.error->message
                         : "source audio is not prepared"));
                continue;
            }
            const auto& data = found->second;
            // A resample cannot shift the pitch on its own; the stretch mode
            // can (the pitch follows the rate instead without one).
            if (!variant.stretch() && take->preservePitch && take->pitch != 0.0)
                p.warnings.push_back(e.id.str() + ": pitch " + std::to_string(take->pitch) +
                                     " st needs pitch_mode \"signalsmith\"; it is ignored and "
                                     "the take is resampled");
            if (!take->stretchMarkers.empty() && !data.itemLocal)
                p.warnings.push_back(e.id.str() +
                                     ": unsupported or invalid warp; scalar playback is active");
            const model::ItemSpan span = model::itemSpan(s, it);
            const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
            Clip c;
            c.item = e.id;
            c.take = *it.activeTake;
            c.deviceShift = n.itemShift;
            c.data = data.data;
            c.start = time::samplesAt(span.startSec, p.rate) - origin + n.itemShift;
            c.length =
                time::samplesAt(span.endSec, p.rate) - time::samplesAt(span.startSec, p.rate);
            // The item's source offset is in source seconds; the variant is
            // read one frame per timeline sample, so `speed()` source seconds
            // pass per timeline second.
            c.offset =
                data.itemLocal ? 0 : time::samplesAt(take->startOffset, p.rate / variant.speed());
            c.loop = it.loop;
            c.mode = take->channelMode;
            const double g =
                dbToGain(it.gainDb) * dbToGain(take->gainDb) * (take->phaseInvert ? -1.0 : 1.0);
            const auto [pl, pr] =
                take->pan == 0.0 ? std::pair{1.0, 1.0} : panGains(take->pan / 100.0, -3.0);
            c.gainL = g * pl;
            c.gainR = g * pr;
            c.fadeIn = time::samplesAt(it.fadeIn.length, p.rate);
            c.fadeOut = time::samplesAt(it.fadeOut.length, p.rate);
            c.fadeInShape = it.fadeIn.shape;
            c.fadeOutShape = it.fadeOut.shape;
            c.fadeInCurve = it.fadeIn.curve;
            c.fadeOutCurve = it.fadeOut.curve;
            for (std::size_t k = 0; k < n.takeChains.size(); ++k) {
                if (n.takeChains[k].item != e.id) continue;
                // Through its take chain: the raw source; gain, pan and fades after it.
                c.takeChain = static_cast<int>(k);
                c.gainL = c.gainR = 1.0;
                c.fadeIn = c.fadeOut = 0;
                n.takeChains[k].clip = static_cast<int>(n.clips.size());
            }
            n.clips.push_back(std::move(c));
        }
    }
    // Events, shifted by each track's solved timeline shift so its own notes stay
    // aligned with what arrives from upstream.
    for (Node& n : p.nodes) {
        if (n.rack) {
            prepareRack(p, n, s);
            continue;
        }
        // A MIDI take whose chain holds an instrument plays it there, not on the track's.
        std::vector<Id> ownInstrument;
        for (TakeChain& tc : n.takeChains) {
            if (tc.noteTarget >= 0) {
                ownInstrument.push_back(tc.item);
                tc.events = trackEvents(s, n.id, p.startSeconds, p.rate,
                                        [&](Id item) { return item == tc.item; });
                for (auto& ev : tc.events)  // its input runs the chain's latency ahead
                    ev.sample = p.rateEnabled ? ev.sample + n.itemShift - tc.latency
                                              : std::max<std::int64_t>(
                                                    0, ev.sample + n.itemShift - tc.latency);
            }
            // Reserve every block's events (processing never allocates): notes
            // and their releases for its instrument, parameter automation.
            for (std::size_t k = 0; k < tc.chain.size(); ++k) {
                Stage& st = tc.chain[k];
                std::size_t need = 1;
                if (static_cast<int>(k) == tc.noteTarget)
                    need +=
                        std::min<std::size_t>(tc.events.size(), 4 * block) + st.voices.capacity();
                for (const ParamLane& ln : st.lanes)
                    need += block / kAutomationStep + 2 + ln.curve.points.size();
                st.blockEvents.reserve(need);
            }
        }
        n.events = trackEvents(s, n.id, p.startSeconds, p.rate, [&](Id item) {
            return std::find(ownInstrument.begin(), ownInstrument.end(), item) ==
                   ownInstrument.end();
        });
        for (auto& ev : n.events) ev.sample += n.timelineShift;
        // Reserve the most events any block-long window holds (events are
        // sorted; after a seek, blocks start anywhere), so processBlock never
        // allocates.
        std::size_t windowMax = 0;
        for (std::size_t a = 0, b = 0; a < n.events.size(); ++a) {
            if (b < a) b = a;
            while (b < n.events.size() &&
                   n.events[b].sample < n.events[a].sample + static_cast<std::int64_t>(block) *
                                                                 (p.rateEnabled ? 4 : 1))
                ++b;
            windowMax = std::max(windowMax, b - a);
        }
        for (Stage& st : n.chain) {
            // Automation events: the grid plus the most points in a block window.
            std::size_t lanes = 0;
            for (const ParamLane& ln : st.lanes) {
                std::size_t pts = 0;
                for (std::size_t a = 0, b = 0; a < ln.curve.points.size(); ++a) {
                    if (b < a) b = a;
                    while (b < ln.curve.points.size() &&
                           ln.curve.points[b].sample <
                               ln.curve.points[a].sample + static_cast<std::int64_t>(block))
                        ++b;
                    pts = std::max(pts, b - a);
                }
                lanes += block / kAutomationStep + 2 + pts;
            }
            const bool noteTarget =
                n.noteTarget >= 0 && &st == &n.chain[static_cast<std::size_t>(n.noteTarget)];
            const std::size_t notes =
                noteTarget ? st.voices.capacity() : 0;  // releases at a wrap or route edit
            const std::size_t live = noteTarget && n.midiIn ? kMaxMidiInputEvents : 0;
            st.blockEvents.reserve(windowMax + 1 + lanes + notes + live);
        }
    }
    for (Node& node : p.nodes)
        forEachStage(node, [](Stage& st) { st.sourceIds.reserve(st.blockEvents.capacity()); });
    // Immutable musical origins let adoption rebind compiled ids after a
    // MIDI edit or time-map change. Build the lookup here, never scan a model on audio.
    auto sources = [](Stage& st, const std::vector<TimedEvent>& events) {
        std::map<decltype(NoteSource{}.addressWithoutOrdinal()), std::uint32_t> counts;
        for (const auto& timed : events) {
            const auto& event = timed.event;
            if (event.kind != host::Event::Kind::NoteOn || event.noteId < 0) continue;
            const auto id = static_cast<std::size_t>(event.noteId);
            if (st.noteSources.size() <= id) st.noteSources.resize(id + 1);
            NoteSource source;
            source.origin = timed.origin;
            source.start = timed.sample;
            source.port = event.port;
            source.channel = event.channel;
            source.key = event.key;
            source.ordinal = counts[source.addressWithoutOrdinal()]++;
            source.id = event.noteId;
            st.noteSources[id] = source;
        }
        for (const auto& timed : events)
            if (timed.event.kind == host::Event::Kind::NoteOff && timed.event.noteId >= 0 &&
                static_cast<std::size_t>(timed.event.noteId) < st.noteSources.size())
                st.noteSources[static_cast<std::size_t>(timed.event.noteId)].end = timed.sample;
        for (const auto& source : st.noteSources)
            if (source.id >= 0) st.noteSourceIndex.push_back(source);
        std::sort(st.noteSourceIndex.begin(), st.noteSourceIndex.end(),
                  [](const auto& a, const auto& b) { return a.address() < b.address(); });
    };
    for (Node& node : p.nodes) {
        if (node.noteTarget >= 0)
            sources(node.chain[static_cast<std::size_t>(node.noteTarget)], node.events);
        for (TakeChain& tc : node.takeChains)
            if (tc.noteTarget >= 0)
                sources(tc.chain[static_cast<std::size_t>(tc.noteTarget)], tc.events);
    }
    // The node graph for the worker pool: a node waits for its edges' sources.
    std::vector<std::vector<std::uint32_t>> deps(p.nodes.size());
    for (std::size_t i = 0; i < p.nodes.size(); ++i)
        for (std::size_t e : p.nodes[i].in)
            deps[i].push_back(static_cast<std::uint32_t>(p.edges[e].from));
    p.graph = std::make_unique<TaskGraph>();
    p.graph->build(deps);
    // The executors' shares (ADR-0014). Live propagates downstream, so a
    // renderer node never depends on a live one.
    std::vector<std::uint32_t> task(p.nodes.size(), 0);
    // As p.instances: an instance waiting for its restart runs on neither.
    auto runs = [&](const host::Instance* in) {
        return in && in->active() && !(p.pool && p.pool->restarting(in));
    };
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        (p.nodes[i].live ? p.liveNodes : p.rendererNodes).push_back(i);
        task[i] = static_cast<std::uint32_t>(
            (p.nodes[i].live ? p.liveNodes : p.rendererNodes).size() - 1);
        forEachStage(p.nodes[i], [&](const Stage& st) {
            if (runs(st.instance))
                (p.nodes[i].live ? p.liveInstances : p.rendererInstances).push_back(st.instance);
        });
    }
    std::vector<std::vector<std::uint32_t>> rdeps;
    for (std::size_t i : p.rendererNodes) {
        rdeps.emplace_back();
        for (std::uint32_t d : deps[i]) rdeps.back().push_back(task[d]);
    }
    p.rendererGraph = std::make_unique<TaskGraph>();
    p.rendererGraph->build(rdeps);
    std::vector<std::vector<std::uint32_t>> ldeps;
    for (std::size_t i : p.liveNodes) {
        ldeps.emplace_back();
        for (std::uint32_t d : deps[i])
            if (p.nodes[d].live) ldeps.back().push_back(task[d]);
    }
    p.liveGraph = std::make_unique<TaskGraph>();
    p.liveGraph->build(ldeps);
    for (auto* list : {&p.rendererInstances, &p.liveInstances}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    for (std::size_t i : p.liveNodes)
        for (std::size_t e : p.nodes[i].in)
            if (!p.nodes[p.edges[e].from].live) {
                p.edges[e].frontier = true;
                p.edges[e].ring = p.frontier.size();
                p.edges[e].frontierBuf.init(block);
                p.frontier.push_back(e);
            }
    if (p.rateEnabled) {
        std::vector<RatePoint> points;
        for (const auto& pt : p.rateEnvelope.points)
            points.push_back({double(pt.sample), pt.value, pt.shape, pt.tension});
        p.rateDeviceMargin = p.outputLatency + p.rateBackendLatency;
        for (const auto& node : p.nodes) {
            p.rateDeviceMargin = std::max(p.rateDeviceMargin, node.timelineShift + node.latChain);
            p.rateSourceBehind =
                std::max(p.rateSourceBehind, node.hasItems() ? node.itemShift : node.timelineShift);
            for (const auto& take : node.takeChains) {
                p.rateDeviceMargin = std::max(p.rateDeviceMargin, take.latency + node.itemShift);
                p.rateSourceAhead = std::max(p.rateSourceAhead, take.latency - node.itemShift);
                p.rateSourceBehind = std::max(p.rateSourceBehind, node.itemShift - take.latency);
            }
            forEachStage(node, [&](const Stage& stage) {
                p.rateDeviceMargin = std::max(p.rateDeviceMargin, std::abs(stage.inputLatency));
            });
        }
        // Keep four seconds of additional past/future headroom even in a
        // zero-latency graph, so inserting ordinary latent FX can inherit the
        // existing trajectory. Larger jumps use a prepared seek instead.
        const auto capacity = static_cast<std::size_t>(
            2 * (2 * p.rateDeviceMargin + p.block + static_cast<std::int64_t>(p.rate * 4.5) + 256));
        if (capacity > (1u << 24))
            return Error("playrate.budget", "source clock history exceeds the render budget", {},
                         "reduce plugin latency or render high-latency chains to audio");
        for (auto& timeline : p.timelines)
            timeline =
                std::make_unique<PlayrateTimeline>(capacity, p.masterPlayrate.rate, points,
                                                   double(time::samplesAt(p.startSeconds, p.rate)));
        static const auto table = std::make_shared<const TapeTable>();
        for (auto& node : p.nodes) {
            auto* timeline = p.timelines[node.live ? 1 : 0].get();
            for (Automation* lane : {&node.autoVolume, &node.autoPan, &node.autoMute})
                lane->timeline = timeline;
            node.vcas.timeline(timeline);
            forEachStage(node, [&](Stage& st) {
                st.rateLive = node.live;
                for (auto& lane : st.lanes) lane.curve.timeline = timeline;
            });
            for (auto& take : node.takeChains)
                for (Automation* lane : {&take.volume, &take.pan, &take.mute})
                    lane->timeline = timeline;
            for (auto& clip : node.clips)
                clip.rateState = std::make_unique<RateClipState>(
                    table, clip.data->channelCount(), p.rate,
                    p.masterPlayrate.preservePitch &&
                        (p.masterPlayrate.rate != 1 || !points.empty()));
        }
        for (auto& edge : p.edges) {
            // Source-owned send envelopes use the source's executor clock;
            // unencoded key edges use their target's (same trajectory).
            auto* timeline = p.timelines[p.nodes[edge.from].live ? 1 : 0].get();
            for (Automation* lane : {&edge.autoVolume, &edge.autoPan, &edge.autoMute,
                                     &edge.stripVolume, &edge.stripPan, &edge.stripMute})
                lane->timeline = timeline;
            edge.vcas.timeline(timeline);
        }
    }
    return okStatus();
}

host::Transport transportAt(const Plan& p, std::int64_t sample, const LoopRange* loop,
                            bool live) SUPPLE_RT {
    host::Transport t;
    const double seconds = p.rateSeconds(sample, live);
    const double beat = p.timeMap->beatAt(seconds);
    const auto bb = p.timeMap->barBeat(beat);
    const auto meter = p.timeMap->meterAt(bb.bar);
    t.playing = true;
    t.tempo = p.timeMap->bpmAt(beat);
    if (p.rateEnabled) {
        const auto control = sample - static_cast<std::int64_t>(
                                          static_cast<std::uint64_t>(sample - p.rateOrigin) & 63);
        const auto controlBeat = p.timeMap->beatAt(p.rateSeconds(control, live));
        t.tempo = p.timeMap->bpmAt(controlBeat) * p.timelines[live ? 1 : 0]->rate(control);
    }
    t.beat = beat;
    t.barStartBeat = p.timeMap->beatOfBar(bb.bar);
    t.bar = bb.bar == std::numeric_limits<int>::min() ? bb.bar : bb.bar - 1;
    t.numerator = static_cast<std::uint16_t>(meter.numerator);
    t.denominator = static_cast<std::uint16_t>(meter.denominator);
    t.seconds = seconds;
    if (loop && loop->active()) {
        t.loopActive = true;
        t.loopStartSeconds = p.startSeconds + static_cast<double>(loop->start) / p.rate;
        t.loopEndSeconds = p.startSeconds + static_cast<double>(loop->end) / p.rate;
        t.loopStartBeat = p.timeMap->beatAt(t.loopStartSeconds);
        t.loopEndBeat = p.timeMap->beatAt(t.loopEndSeconds);
    }
    return t;
}

Status checkStrips(const Plan& p) {
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const Node& n = p.nodes[i];
        if (n.decodeInput != (n.type != model::StripType::Channel) ||
            n.master != (n.type == model::StripType::Master))
            return Error("engine.strip_order", "implicit strip stages disagree with its type", {},
                         "report this strip-type compiler inconsistency with the project and "
                         "session.describe; a valid strip should not fail this check");
        // Items play on channels before the chain and on masters after the
        // plain stage (ADR-0057): never into a summing input, never on a buss.
        if (n.postItems != n.master ||
            (n.type == model::StripType::Buss && (n.hasItems() || !n.events.empty())))
            return Error("engine.strip_order", "items are placed where the strip type forbids", {},
                         "report this strip-type compiler inconsistency with the project and "
                         "session.describe; a valid strip should not fail this check");
        for (std::size_t ei : n.in) {
            if (ei >= p.edges.size())
                return Error("engine.strip_edge", "missing input edge", {},
                             "report this routing compiler inconsistency with the project and "
                             "session.describe, including the affected routes from route.list");
            const Edge& e = p.edges[ei];
            if (e.from >= i)
                return Error("engine.strip_edge", "input is not topologically ordered", {},
                             "report this routing compiler inconsistency with the project and "
                             "session.describe, including the affected routes from route.list");
            // Racks (ADR-0062): an Out module's audio enters its channel as is;
            // a send into an In module is linear, through its own encoder.
            if (e.rackOut >= 0 || e.rackIn >= 0) {
                if ((e.rackOut >= 0) == e.encode || (e.rackIn >= 0) != bool(n.rack) ||
                    (e.rackOut >= 0 && !p.nodes[e.from].rack))
                    return Error("engine.strip_edge", "a rack's edges are its In and Out modules'",
                                 {},
                                 "report this routing compiler inconsistency with the project and "
                                 "session.describe, including the affected routes from route.list");
                continue;
            }
            if (e.stage >= 0) {
                if (static_cast<std::size_t>(e.stage) >= n.chain.size() || e.encode)
                    return Error("engine.strip_edge", "key routes must bypass send encoders", {},
                                 "report this routing compiler inconsistency with the project and "
                                 "session.describe, including the affected routes from route.list");
                continue;
            }
            if (e.auxOnly) {  // linear links into auxiliary channels (ADR-0020)
                if (e.encode)
                    return Error("engine.strip_edge", "aux-only routes are not encoded", {},
                                 "report this routing compiler inconsistency with the project and "
                                 "session.describe, including the affected routes from route.list");
                continue;
            }
            if (e.encode &&
                std::count(p.nodes[e.from].out.begin(), p.nodes[e.from].out.end(), ei) != 1)
                return Error("engine.strip_edge", "send encoder is not owned by its source", {},
                             "report this routing compiler inconsistency with the project and "
                             "session.describe, including the affected routes from route.list");
            if (!n.decodeInput || p.nodes[e.from].master ||
                (!e.encode && (e.tap != model::Tap::PostFader || e.volumeDb != 0 || !e.followPan ||
                               e.mono || e.invert || e.mapped || e.muted || e.autoVolume.active() ||
                               e.autoPan.active() || e.autoMute.active())))
                return Error("engine.strip_edge",
                             "summing inputs require an unmodified Channel output", {},
                             "report this routing compiler inconsistency with the project and "
                             "session.describe, including the affected routes from route.list");
        }
    }
    return okStatus();
}

const Stereo& edgeSignal(const Plan& p, const Edge& e) SUPPLE_RT {
    if (e.rackOut >= 0) return p.nodes[e.from].rackOuts[static_cast<std::size_t>(e.rackOut)];
    if (e.encode) return e.encoded;
    const Node& src = p.nodes[e.from];
    if (e.stage >= 0 && e.tap == model::Tap::PostFader) return src.postKey;
    return e.tap == model::Tap::PreFx      ? src.preFx
           : e.tap == model::Tap::PreFader ? src.preFader
                                           : src.post;
}

namespace {
bool runStage(Plan& p, Stage& st, Stereo& sum, std::uint32_t n, const host::Transport& transport,
              std::int64_t pos, const void* self) SUPPLE_RT;
void automateStage(Stage& st, std::int64_t pos, std::uint32_t n, std::int64_t origin) SUPPLE_RT;
// Audio edges carry only the encoded signal and PDC; key controls are conventional.
void addEdge(Plan& p, Edge& e, double panLawDb, Stereo& into, std::int64_t pos,
             std::uint32_t n) SUPPLE_RT {
    const Stereo& tap = p.split && e.frontier ? e.frontierBuf : edgeSignal(p, e);
    e.tmp.copyFrom(tap, n);
    e.delay.process(e.tmp.l.data(), e.tmp.r.data(), n);
    for (std::uint32_t i = 0; i < n; ++i) {
        float l = e.tmp.l[i], r = e.tmp.r[i];
        if (e.stage >= 0) {
            const auto at = pos + i;
            const bool muted = e.autoMute.active() ? e.autoMute.at(at) >= .5 : e.muted;
            const double g =
                muted ? 0 : dbToGain(e.volumeCurve() ? e.autoVolume.at(at) : e.volumeDb);
            const auto [pl, pr] = panGains((e.panCurve()                       ? e.autoPan.at(at)
                                            : e.followPan && e.stripPanCurve() ? e.stripPan.at(at)
                                                                               : e.pan) /
                                               100,
                                           panLawDb);
            if (e.mono) l = r = .5f * (l + r);
            const double sign = e.invert ? -1 : 1;
            l *= static_cast<float>(sign * g * pl);
            r *= static_cast<float>(sign * g * pr);
            e.applyMap(l, r);  // on the send's output, after its pan
        }
        into.l[i] += l;
        into.r[i] += r;
    }
}
}  // namespace

void addEdgeInto(Plan& p, Edge& e, Stereo& into, std::int64_t pos, std::uint32_t n) SUPPLE_RT {
    addEdge(p, e, -3.0, into, pos, n);
}

bool runModuleStage(Plan& p, Stage& st, Stereo& io, std::uint32_t n,
                    const host::Transport& transport, std::int64_t pos,
                    const void* self) SUPPLE_RT {
    automateStage(st, pos, n, time::samplesAt(p.startSeconds, p.rate));
    // Events in time order (notes from the patch, then automation at each frame).
    for (std::size_t i = 1; i < st.blockEvents.size(); ++i)
        for (std::size_t k = i; k > 0 && st.blockEvents[k - 1].time > st.blockEvents[k].time; --k)
            std::swap(st.blockEvents[k - 1], st.blockEvents[k]);
    st.outEvents.clear();
    st.sourceIds.clear();
    st.releases = 0;
    st.processed = false;
    return runStage(p, st, io, n, transport, pos, self);
}

namespace {
// An audio item's samples for [pos, pos + n), added into `into`.
void playClip(Plan& p, Clip& c, Stereo& into, std::int64_t pos, std::uint32_t n, std::int64_t shift,
              bool live) SUPPLE_RT {
    if (p.rateEnabled) {
        playRateClip(p, c, into, pos, n, shift, live);
        return;
    }
    // Timeline sample t plays the clip at t + shift (a take chain's input
    // runs its latency ahead).
    const std::int64_t from = std::max<std::int64_t>(pos, c.start - shift);
    const std::int64_t to = std::min<std::int64_t>(pos + n, c.start + c.length - shift);
    const auto& ch = c.data->channels;
    const auto frames = static_cast<std::int64_t>(c.data->frames());
    if (ch.empty() || frames == 0) return;
    const std::size_t right = ch.size() > 1 ? 1 : 0;
    for (std::int64_t t = from; t < to; ++t) {
        const std::int64_t k = t + shift - c.start;  // sample within the item
        std::int64_t f = c.offset + k;
        if (c.loop) f = ((f % frames) + frames) % frames;
        if (f < 0 || f >= frames) continue;
        const auto fi = static_cast<std::size_t>(f);
        float l = ch[0][fi], r = ch[right][fi];
        switch (c.mode) {
            case model::ChannelMode::Normal:
                break;
            case model::ChannelMode::ReverseStereo:
                std::swap(l, r);
                break;
            case model::ChannelMode::MonoMix:
                l = r = 0.5f * (l + r);
                break;
            case model::ChannelMode::Left:
                r = l;
                break;
            case model::ChannelMode::Right:
                l = r;
                break;
        }
        double fade = 1.0;
        if (c.fadeIn > 0 && k < c.fadeIn)
            fade *= fadeCurve(c.fadeInShape, c.fadeInCurve,
                              static_cast<double>(k) / static_cast<double>(c.fadeIn));
        if (c.fadeOut > 0 && k >= c.length - c.fadeOut)
            fade *= fadeCurve(c.fadeOutShape, c.fadeOutCurve,
                              static_cast<double>(c.length - k) / static_cast<double>(c.fadeOut));
        const auto i = static_cast<std::size_t>(t - pos);
        into.l[i] += static_cast<float>(static_cast<double>(l) * c.gainL * fade);
        into.r[i] += static_cast<float>(static_cast<double>(r) * c.gainR * fade);
    }
}

// Plugin parameter automation: an initial value, the absolute timeline
// kAutomationStep grid and each point, in time order (the lane's
// cursor only moves forward), merged into the block's sorted events.
void automateStage(Stage& st, std::int64_t pos, std::uint32_t n, std::int64_t origin) SUPPLE_RT {
    if (st.lanes.empty()) return;
    const std::size_t before = st.blockEvents.size();
    for (ParamLane& ln : st.lanes) {
        if (ln.curve.timeline) {
            const auto& pts = ln.curve.points;
            const auto& timeline = *ln.curve.timeline;
            const auto shift = ln.curve.deviceShift;
            const double projectOrigin = timeline.origin();
            double previous = timeline.absolutePosition(pos - 1 - shift) + double(shift);
            auto point = std::upper_bound(pts.begin(), pts.end(), previous,
                                          [projectOrigin](double sample, const Automation::Pt& pt) {
                                              return sample < double(pt.sample) + projectOrigin;
                                          });
            for (std::uint32_t i = 0; i < n; ++i) {
                const double at = timeline.absolutePosition(pos + i - shift) + double(shift);
                if (at < previous) point = pts.begin();
                const bool crossed =
                    point != pts.end() && double(point->sample) + projectOrigin <= at;
                while (point != pts.end() && double(point->sample) + projectOrigin <= at) ++point;
                if (std::isnan(ln.lastSent) || crossed ||
                    (pos + i + origin) % kAutomationStep == 0) {
                    const double value = ln.clapAt(pos + i);
                    if (value != ln.lastSent && st.blockEvents.size() < st.blockEvents.capacity()) {
                        host::Event event;
                        event.kind = host::Event::Kind::Param;
                        event.time = i;
                        event.paramId = ln.clapId;
                        event.value = value;
                        st.blockEvents.push_back(event);
                        ln.lastSent = value;
                    }
                }
                previous = at;
            }
            continue;
        }
        const auto& pts = ln.curve.points;
        auto pi =
            static_cast<std::size_t>(std::lower_bound(pts.begin(), pts.end(), pos,
                                                      [](const Automation::Pt& pt, std::int64_t x) {
                                                          return pt.sample < x;
                                                      }) -
                                     pts.begin());
        const auto phase = ((pos % kAutomationStep + origin % kAutomationStep) % kAutomationStep +
                            kAutomationStep) %
                           kAutomationStep;
        auto grid = static_cast<std::uint32_t>((kAutomationStep - phase) % kAutomationStep);
        bool initial = std::isnan(ln.lastSent);
        for (;;) {
            const std::uint32_t atPoint = pi < pts.size() && pts[pi].sample < pos + n
                                              ? static_cast<std::uint32_t>(pts[pi].sample - pos)
                                              : n;
            const std::uint32_t at = std::min({initial ? 0u : n, grid, atPoint});
            if (at >= n) break;
            const double v = ln.clapAt(pos + at);
            if (v != ln.lastSent && st.blockEvents.size() < st.blockEvents.capacity()) {
                host::Event e;
                e.kind = host::Event::Kind::Param;
                e.time = at;
                e.paramId = ln.clapId;
                e.value = v;
                st.blockEvents.push_back(e);
                ln.lastSent = v;
            }
            if (at == grid) grid += kAutomationStep;
            if (at == atPoint) ++pi;
            initial = false;
        }
    }
    if (st.blockEvents.size() > before)  // stable insertion sort by time: no allocation
        for (std::size_t i = 1; i < st.blockEvents.size(); ++i)
            for (std::size_t j = i; j > 0 && st.blockEvents[j - 1].time > st.blockEvents[j].time;
                 --j)
                std::swap(st.blockEvents[j - 1], st.blockEvents[j]);
}

// One stage of a chain, in place on `sum` (the track's or a take's). False on
// a plugin error.
// Note ids must not be reused at a loop wrap until the old voice has ended.
// Assign running ids and resolve compiled note-offs against held voices or
// earlier note-ons in this block. Live MIDI retains its -1 tuple addressing.
void prepareNoteIds(Stage& st) SUPPLE_RT {
    st.sourceIds.resize(st.blockEvents.size(), -1);  // capacity reserved by prepare
    st.pendingVoices.clear();
    for (const auto& voice : st.voices) st.pendingVoices.push_back(voice);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < st.blockEvents.size(); ++i) {
        auto event = st.blockEvents[i];
        const auto source = event.noteId;
        if (event.kind == host::Event::Kind::NoteOn) {
            // Never deliver a note the engine cannot later address/release.
            if (st.pendingVoices.size() == st.pendingVoices.capacity()) {
                ++st.voiceDropped;
                continue;
            }
            if (event.noteId >= 0)
                event.noteId = static_cast<std::int32_t>(st.nextNoteId++ & 0x7fffffffu);
            HeldVoice pending{event.noteId, event.port, event.channel, event.key,
                              event.noteId == -1};
            pending.sourceId = source;
            st.pendingVoices.push_back(pending);
        } else if (event.kind == host::Event::Kind::NoteOff ||
                   event.kind == host::Event::Kind::NoteChoke) {
            if (event.noteId >= 0 && i >= st.releases) {
                const auto voice = std::find_if(
                    st.pendingVoices.begin(), st.pendingVoices.end(), [&](const HeldVoice& v) {
                        return v.sourceId == source &&
                               matchesVoice(v, event.port, event.channel, event.key, -1);
                    });
                // Its onset was dropped or NoteEnd already finished it.
                if (voice == st.pendingVoices.end()) continue;
                event.noteId = voice->noteId;
            }
            std::erase_if(st.pendingVoices, [&](const HeldVoice& v) {
                return matchesVoice(v, event.port, event.channel, event.key, event.noteId);
            });
        }
        st.blockEvents[kept] = event;
        st.sourceIds[kept++] = source;
    }
    st.blockEvents.resize(kept);
    st.sourceIds.resize(kept);
}

// Merge input-note and output-end clocks before updating the held counters.
// The plug-in may finish one overlapping voice while another remains held.
void trackVoices(Stage& st, std::array<std::uint8_t, 16 * 128>& held,
                 std::array<std::uint8_t, 16 * 128>* liveHeld = nullptr) SUPPLE_RT {
    if (!st.processed) return;  // input events not delivered during a compensated bypass
    auto remove = [&](std::int16_t port, std::int16_t channel, std::int16_t key, std::int32_t id) {
        std::erase_if(st.voices,
                      [&](const HeldVoice& v) { return matchesVoice(v, port, channel, key, id); });
    };
    std::size_t input = 0;
    for (std::size_t output = 0; output <= st.outEvents.count; ++output) {
        const bool end = output < st.outEvents.count &&
                         st.outEvents.storage[output].kind == host::OutputEvent::Kind::NoteEnd;
        if (output < st.outEvents.count && !end) continue;
        const auto at = end ? st.outEvents.storage[output].time : UINT32_MAX;
        while (input < st.blockEvents.size() && st.blockEvents[input].time <= at) {
            const auto sourceIndex = input;
            const auto& event = st.blockEvents[input++];
            if (event.kind == host::Event::Kind::NoteOn) {
                if (st.voices.size() < st.voices.capacity()) {
                    HeldVoice voice{event.noteId, event.port, event.channel, event.key,
                                    liveHeld && event.noteId == -1};
                    voice.sourceId = sourceIndex < st.sourceIds.size() ? st.sourceIds[sourceIndex]
                                                                       : event.noteId;
                    if (voice.sourceId >= 0 &&
                        static_cast<std::size_t>(voice.sourceId) < st.noteSources.size()) {
                        const auto& source =
                            st.noteSources[static_cast<std::size_t>(voice.sourceId)];
                        voice.origin = source.origin;
                        voice.sourceOrdinal = source.ordinal;
                    }
                    st.voices.push_back(voice);
                } else
                    ++st.voiceDropped;
            } else if (event.kind == host::Event::Kind::NoteOff ||
                       event.kind == host::Event::Kind::NoteChoke) {
                remove(event.port, event.channel, event.key, event.noteId);
            }
        }
        if (end) {
            const auto& event = st.outEvents.storage[output];
            remove(event.port, event.channel, event.key, event.noteId);
        }
    }
    held.fill(0);
    if (liveHeld) liveHeld->fill(0);
    for (const auto& voice : st.voices) {
        if (voice.channel < 0 || voice.channel > 15 || voice.key < 0 || voice.key > 127) continue;
        auto& counts = voice.live && liveHeld ? *liveHeld : held;
        auto& count = counts[static_cast<std::size_t>(voice.channel * 128 + voice.key)];
        if (count < 255) ++count;
    }
}

// Releases preserve note ids so an end for an old loop does not terminate
// its newly retriggered overlapping voice. Live notes survive a transport wrap.
void releaseVoices(Stage& st, bool includeLive, bool onlyPending = false,
                   std::int64_t pos = 0) SUPPLE_RT {
    for (const auto& voice : st.voices) {
        bool pending = voice.release;
        if (onlyPending && voice.sourceId >= 0 &&
            static_cast<std::size_t>(voice.sourceId) < st.noteSources.size()) {
            const auto& source = st.noteSources[static_cast<std::size_t>(voice.sourceId)];
            pending |= source.end < pos || source.start > pos;
        }
        if ((voice.live && !includeLive) || (onlyPending && !pending)) continue;
        if (st.blockEvents.size() == st.blockEvents.capacity()) break;
        host::Event off;
        off.kind = host::Event::Kind::NoteOff;
        off.port = voice.port;
        off.channel = voice.channel;
        off.key = voice.key;
        off.noteId = voice.noteId;
        st.blockEvents.push_back(off);
    }
    st.releases = st.blockEvents.size();
}

// The half-band resamplers of an oversampled stage (dsp::HalfbandOversampler: one
// sample in, `os` out, and back).
void upsample(dsp::HalfbandOversampler& f, const float* x, float* y, std::uint32_t n,
              std::uint32_t os) SUPPLE_RT {
    std::array<double, 8> tmp{};
    for (std::uint32_t i = 0; i < n; ++i) {
        f.upsample(static_cast<double>(x[i]), std::span<double>(tmp.data(), os));
        for (std::uint32_t j = 0; j < os; ++j) y[i * os + j] = static_cast<float>(tmp[j]);
    }
}
void downsample(dsp::HalfbandOversampler& f, const float* x, float* y, std::uint32_t n,
                std::uint32_t os) SUPPLE_RT {
    std::array<double, 8> tmp{};
    for (std::uint32_t i = 0; i < n; ++i) {
        for (std::uint32_t j = 0; j < os; ++j) tmp[j] = static_cast<double>(x[i * os + j]);
        y[i] = static_cast<float>(f.downsample(std::span<const double>(tmp.data(), os)));
    }
}

// Parallel groups: before a stage runs, the group's first stage keeps its
// input and the later branches start from it; after it, each branch is
// aligned to the group's latency and summed, the sum leaving the last one.
void enterGroup(std::vector<Stage>& chain, Stage& st, Stereo& sum, std::uint32_t n) SUPPLE_RT {
    if (st.group < 0) return;
    Stage& first = chain[static_cast<std::size_t>(st.group)];
    if (&first == &st) {
        first.groupIn.copyFrom(sum, n);
        first.groupOut.clear(n);
    } else {
        sum.copyFrom(first.groupIn, n);
    }
}
void leaveGroup(std::vector<Stage>& chain, Stage& st, Stereo& sum, std::uint32_t n) SUPPLE_RT {
    if (st.group < 0) return;
    Stage& first = chain[static_cast<std::size_t>(st.group)];
    st.align.process(sum.l.data(), sum.r.data(), n);
    for (std::uint32_t i = 0; i < n; ++i) {
        first.groupOut.l[i] += sum.l[i];
        first.groupOut.r[i] += sum.r[i];
    }
    if (st.groupEnd) sum.copyFrom(first.groupOut, n);
}

// Clears what a stage carries from block to block outside its plug-in.
void clearStageDelays(Stage& st) SUPPLE_RT {
    st.dry.clear();
    st.align.clear();
    st.osPad.clear();
    for (auto& f : st.osFilters) f.reset();
    for (MonoDelay& d : st.chanDelay) d.clear();
}

bool runStage(Plan& p, Stage& st, Stereo& sum, std::uint32_t n, const host::Transport& transport,
              std::int64_t pos, const void* self) SUPPLE_RT {
    st.outEvents.clear();
    st.auxWritten = false;
    // A stage whose plugin is not processing (start_processing failed)
    // passes audio through, delay-compensated, like a bypass.
    if (st.callbackBypass || !st.instance || !st.instance->processing() ||
        (self && st.instance->owner() != self)) {
        st.dry.process(sum.l.data(), sum.r.data(), n);  // compensated bypass
        st.appliedWet = -1.0f;
        st.wetRampLeft = 0;  // no ramp to carry: `appliedWet` holds nothing
        return true;
    }
    // The wet level to reach (0 bypassed); a change ramps from the level
    // last applied, the plugin running until the ramp is done.
    const auto target = static_cast<float>(st.bypass ? 0.0 : st.instrument ? 1.0 : st.wet);
    if (st.appliedWet < 0.0f) st.appliedWet = target;  // new here: no ramp
    if (st.wetRampLeft ? target != st.rampToWet : target != st.appliedWet) {
        st.rampFromWet = st.appliedWet;
        st.rampToWet = target;
        st.wetRampLeft = p.smoothFrames;
    }
    const bool requested = st.instance->takeProcessRequest();
    if (st.bypass && st.wetRampLeft == 0 && st.blockEvents.empty() && !requested &&
        !st.instance->needsAudioFlush()) {  // bypassed and faded out
        st.dry.process(sum.l.data(), sum.r.data(), n);
        st.appliedWet = 0.0f;
        return true;
    }
    bool silent = true;
    if (st.autoBypassOnSilence) {
        for (std::uint32_t i = 0; i < n && silent; ++i)
            silent = sum.l[i] == 0 && sum.r[i] == 0 &&
                     (!st.keyed || (st.key.l[i] == 0 && st.key.r[i] == 0));
        if (st.sleeping && silent && st.blockEvents.empty() && !requested &&
            !st.instance->needsAudioFlush()) {
            st.dry.process(sum.l.data(), sum.r.data(), n);
            return true;
        }
    }
    const auto os = static_cast<std::uint32_t>(st.os);
    const float* in[2] = {sum.l.data(), sum.r.data()};
    float* out[2] = {st.out.l.data(), st.out.r.data()};
    const float* key[2] = {st.key.l.data(), st.key.r.data()};
    prepareNoteIds(st);
    if (os > 1) {
        if (!st.instrument)
            for (int c = 0; c < 2; ++c)
                upsample(st.osFilters[static_cast<std::size_t>(c)], in[c], st.osIn[c].data(), n,
                         os);
        if (st.keyed)
            for (int c = 0; c < 2; ++c)
                upsample(st.osFilters[static_cast<std::size_t>(2 + c)], key[c], st.osKey[c].data(),
                         n, os);
        for (int c = 0; c < 2; ++c) {
            in[c] = st.osIn[c].data();
            out[c] = st.osOut[c].data();
            key[c] = st.osKey[c].data();
        }
        for (host::Event& e : st.blockEvents) e.time *= os;
    }
    host::ProcessBlock b;
    b.inputs = st.instrument ? nullptr : in;
    b.inputChannels = st.instrument ? 0 : 2;
    b.outputs = out;
    b.outputChannels = 2;
    if (st.keyed) {
        b.sidechain = key;
        b.sidechainChannels = st.keyChannels;
    }
    if (!st.auxOutPtr.empty()) {
        b.auxOutputs = st.auxOutPtr.data();
        b.auxOutputChannels = static_cast<std::uint32_t>(st.auxOutPtr.size());
    }
    b.frames = n * os;
    b.events = st.blockEvents;
    host::Transport mappedTransport = transportAt(p, pos - st.inputLatency, nullptr, st.rateLive);
    mappedTransport.playing = transport.playing;
    mappedTransport.loopActive = transport.loopActive;
    mappedTransport.loopStartSeconds = transport.loopStartSeconds;
    mappedTransport.loopEndSeconds = transport.loopEndSeconds;
    mappedTransport.loopStartBeat = transport.loopStartBeat;
    mappedTransport.loopEndBeat = transport.loopEndBeat;
    if (p.rateEnabled) {
        const auto control =
            pos - static_cast<std::int64_t>(static_cast<std::uint64_t>(pos - p.rateOrigin) & 63) -
            st.inputLatency;
        const auto beat = p.timeMap->beatAt(p.rateSeconds(control, st.rateLive));
        mappedTransport.tempo =
            p.timeMap->bpmAt(beat) * p.timelines[st.rateLive ? 1 : 0]->rate(control);
    }
    b.transport = &mappedTransport;
    b.steadyTime = pos * os;
    const bool processed = st.instance->process(b, &st.outEvents);
    if (os > 1) {
        for (host::Event& e : st.blockEvents) e.time /= os;
        for (std::size_t k = 0; k < st.outEvents.count; ++k) st.outEvents.storage[k].time /= os;
        if (processed) {
            st.osPad.process(st.osOut[0].data(), st.osOut[1].data(), n * os);
            for (int c = 0; c < 2; ++c)
                downsample(st.osFilters[static_cast<std::size_t>(c)], st.osOut[c].data(),
                           c == 0 ? st.out.l.data() : st.out.r.data(), n, os);
        }
    }
    if (!processed) return false;
    st.processed = true;
    st.auxWritten = true;
    st.sleeping = st.autoBypassOnSilence && st.instance->sleeping();
    // Dry signal delayed by the stage's latency: an instrument adds its
    // output to what came in; an effect mixes wet and dry.
    st.dryBuf.copyFrom(sum, n);
    st.dry.process(st.dryBuf.l.data(), st.dryBuf.r.data(), n);
    if (st.wetRampLeft == 0) {
        const float wet = target, dryGain = st.instrument ? 1.0f : 1.0f - wet;
        for (std::uint32_t i = 0; i < n; ++i) {
            sum.l[i] = st.out.l[i] * wet + st.dryBuf.l[i] * dryGain;
            sum.r[i] = st.out.r[i] * wet + st.dryBuf.r[i] * dryGain;
        }
        st.appliedWet = target;
    } else {
        // Crossfading (an instrument's "dry" is what came in: it stays).
        const float span = static_cast<float>(p.smoothFrames);
        for (std::uint32_t i = 0; i < n; ++i) {
            float wet = target;
            if (st.wetRampLeft > 0) {
                const float t = 1.0f - static_cast<float>(st.wetRampLeft) / span;
                wet = st.rampFromWet + (st.rampToWet - st.rampFromWet) * t;
                --st.wetRampLeft;
                if (st.wetRampLeft == 0) wet = st.rampToWet;
            }
            const float dryGain = st.instrument ? 1.0f : 1.0f - wet;
            sum.l[i] = st.out.l[i] * wet + st.dryBuf.l[i] * dryGain;
            sum.r[i] = st.out.r[i] * wet + st.dryBuf.r[i] * dryGain;
            st.appliedWet = wet;
        }
    }
    return true;
}

// A multi-out plug-in's auxiliary outputs replace the track channels they
// are pinned to (ADR-0021), unless it is bypassed or did not run.
void writeAuxOutputs(Node& node, const Stage& st, std::uint32_t n) SUPPLE_RT {
    if (!st.auxWritten || st.bypass) return;
    for (std::size_t k = 0; k < st.auxOutPin.size(); ++k)
        if (st.auxOutPin[k] >= 0)
            std::copy_n(st.auxOut[k].data(), n, node.channel(st.auxOutPin[k]));
}

// A stage of a strip with auxiliary channels, or with pins (ADR-0020): the
// plug-in reads its input pins and writes its output pins (its dry signal is
// its input's); the channels it does not write are delayed by its latency. A
// bypassed or idle pinned stage passes every channel through, delayed.
bool runPinned(Plan& p, Node& node, Stage& st, std::uint32_t n, const host::Transport& transport,
               std::int64_t pos, const void* self) SUPPLE_RT {
    st.outEvents.clear();
    st.auxWritten = false;
    const int channels = 2 + node.auxCount;
    if (!st.pinned) {  // the main pair as usual; the aux channels keep time
        for (int c = 2; c < channels; ++c)
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
        if (!runStage(p, st, node.sum, n, transport, pos, self)) return false;
        writeAuxOutputs(node, st, n);
        return true;
    }
    if (st.callbackBypass || !st.instance || !st.instance->processing() ||
        (self && st.instance->owner() != self) ||
        (st.bypass && st.blockEvents.empty() && !st.instance->processRequested() &&
         !st.instance->needsAudioFlush())) {
        for (int c = 0; c < channels; ++c)
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
        if (st.bypass && !st.callbackBypass && st.instance && st.instance->processing() &&
            (!self || st.instance->owner() == self))
            st.instance->takeProcessRequest();
        st.appliedWet = st.bypass ? 0.0f : -1.0f;
        st.wetRampLeft = 0;
        return true;
    }
    std::copy_n(node.channel(st.inPin[0]), n, st.pinBuf.l.begin());
    std::copy_n(node.channel(st.inPin[1]), n, st.pinBuf.r.begin());
    if (!runStage(p, st, st.pinBuf, n, transport, pos, self)) return false;
    if (st.bypass) {  // process queued note/param events without changing the pin routing
        for (int c = 0; c < channels; ++c)
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
        return true;
    }
    for (int c = 0; c < channels; ++c)
        if (c != st.outPin[0] && c != st.outPin[1])
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
    std::copy_n(st.pinBuf.l.begin(), n, node.channel(st.outPin[0]));
    if (st.outPin[1] >= 0) std::copy_n(st.pinBuf.r.begin(), n, node.channel(st.outPin[1]));
    writeAuxOutputs(node, st, n);
    return true;
}

double mappedAt(const Plan& p, bool live, std::int64_t device, std::int64_t shift) SUPPLE_RT {
    return p.rateSample(device - shift, live) + double(shift);
}
std::uint32_t eventOffset(const Plan& p, bool live, std::int64_t pos, std::uint32_t frames,
                          std::int64_t shift, std::int64_t event) SUPPLE_RT {
    std::uint32_t lo = 0, hi = frames;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (mappedAt(p, live, pos + mid, shift) < double(event))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Emit timeline events at their first device boundary. Unwrapped phase
// permits a compensated instrument to cross a loop before the output does.
void rateEvents(Plan& p, bool live, std::span<const TimedEvent> events, Stage& stage,
                std::int64_t pos, std::uint32_t frames, std::int64_t shift, bool& fresh) SUPPLE_RT {
    auto& clock = *p.timelines[live ? 1 : 0];
    auto& held = stage.pendingVoices;
    held.clear();
    for (const auto& voice : stage.voices)
        if (!voice.live && voice.sourceId >= 0 && held.size() < held.capacity())
            held.push_back(voice);
    auto emit = [&](host::Event event) {
        if (stage.blockEvents.size() == stage.blockEvents.capacity()) {
            ++stage.voiceDropped;
            return;
        }
        stage.blockEvents.push_back(event);
        if (event.kind == host::Event::Kind::NoteOn) {
            if (held.size() < held.capacity()) {
                HeldVoice voice;
                voice.sourceId = event.noteId;
                voice.port = event.port;
                voice.channel = event.channel;
                voice.key = event.key;
                held.push_back(voice);
            }
        } else if (event.kind == host::Event::Kind::NoteOff ||
                   event.kind == host::Event::Kind::NoteChoke) {
            std::erase_if(held,
                          [&](const HeldVoice& voice) { return voice.sourceId == event.noteId; });
        }
    };
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double here = clock.phase(pos + i - shift);
        double before = clock.phase(pos + i - 1 - shift);
        if (fresh) {
            before = std::nextafter(here, -std::numeric_limits<double>::infinity());
            fresh = false;
        }
        const auto firstCycle = clock.cycle(before), lastCycle = clock.cycle(here);
        for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            if (cycle != firstCycle) {
                // Compiled source ids are resolved to running ids by prepareNoteIds.
                while (!held.empty()) {
                    const auto voice = held.back();
                    held.pop_back();
                    host::Event off;
                    off.kind = host::Event::Kind::NoteOff;
                    off.time = i;
                    off.noteId = voice.sourceId;
                    off.port = voice.port;
                    off.channel = voice.channel;
                    off.key = voice.key;
                    if (stage.blockEvents.size() < stage.blockEvents.capacity())
                        stage.blockEvents.push_back(off);
                    else
                        ++stage.voiceDropped;
                }
            }
            const double offset = double(cycle) * (clock.looping() ? clock.cycleLength() : 0.);
            const double lower = before + double(shift) - offset;
            const double upper = here + double(shift) - offset;
            auto event = std::upper_bound(
                events.begin(), events.end(), lower,
                [](double sample, const TimedEvent& e) { return sample < double(e.sample); });
            for (; event != events.end() && double(event->sample) <= upper; ++event) {
                const double original = double(event->sample - shift);
                if (clock.looping() &&
                    (original >= clock.loopEnd() || (cycle > 0 && original < clock.loopStart())))
                    continue;
                auto e = event->event;
                e.time = i;
                emit(e);
            }
        }
    }
}

// A take chain's block (gap log #22): its source (the clip, or its MIDI for
// its instrument) runs `latency` ahead through its stages; the output, cut to
// the item's span with the item and take gain, pan, fades and the take's
// envelopes, is added to the track's sum. Outside its span it does not run;
// it starts fresh (plugins reset) each time it is entered.
bool processTake(Plan& p, Node& node, TakeChain& tc, std::int64_t pos, std::uint32_t n,
                 const host::Transport& transport, const void* self) SUPPLE_RT {
    for (Stage& st : tc.chain) {
        st.blockEvents.clear();
        st.outEvents.clear();
    }
    tc.buf.clear(n);
    if (p.rateEnabled && transport.playing && tc.clip >= 0)
        playClip(p, node.clips[static_cast<std::size_t>(tc.clip)], tc.buf, pos, n, tc.latency,
                 node.live);
    const double inputEnd = mappedAt(p, node.live, pos + n + tc.latency, node.itemShift);
    const double outputStart = mappedAt(p, node.live, pos, node.itemShift);
    bool intersects = inputEnd > double(tc.start) && outputStart < double(tc.end);
    if (p.rateEnabled) {
        const auto& clock = *p.timelines[node.live ? 1 : 0];
        const double from = clock.phase(pos - node.itemShift);
        const double to = clock.phase(pos + n + tc.latency - node.itemShift);
        const double start = double(tc.start - node.itemShift),
                     end = double(tc.end - node.itemShift);
        const double firstEnd = clock.looping() ? std::min(end, clock.loopEnd()) : end;
        intersects = start < firstEnd && to > start && from < firstEnd;
        if (!intersects && clock.looping() && end > clock.loopStart() && start < clock.loopEnd()) {
            const double a = std::max(start, clock.loopStart()), b = std::min(end, clock.loopEnd());
            const double cycle = std::max(1., std::floor((from - b) / clock.cycleLength()) + 1);
            intersects =
                to > a + cycle * clock.cycleLength() && from < b + cycle * clock.cycleLength();
        }
    }
    if (!transport.playing || !intersects) {
        tc.active = false;
        return true;
    }
    if (!tc.active) {
        tc.active = true;
        for (Stage& st : tc.chain) {
            st.voices.clear();
            st.sleeping = false;
            clearStageDelays(st);
            if (!st.callbackBypass && st.instance && st.instance->processing() &&
                (!self || st.instance->owner() == self))
                st.instance->reset();
        }
    }
    if (!p.rateEnabled) tc.buf.clear(n);
    if (!p.rateEnabled && tc.clip >= 0)
        playClip(p, node.clips[static_cast<std::size_t>(tc.clip)], tc.buf, pos, n, tc.latency,
                 node.live);
    if (tc.noteTarget >= 0) {
        auto& target = tc.chain[static_cast<std::size_t>(tc.noteTarget)].blockEvents;
        if (tc.releaseHeld) {  // a loop wrap: what is held ends first
            tc.releaseHeld = false;
            releaseVoices(tc.chain[static_cast<std::size_t>(tc.noteTarget)], true);
            tc.held.fill(0);
        } else
            releaseVoices(tc.chain[static_cast<std::size_t>(tc.noteTarget)], true, true,
                          static_cast<std::int64_t>(
                              mappedAt(p, node.live, pos, node.itemShift - tc.latency)));
        if (p.rateEnabled)
            rateEvents(p, node.live, tc.events, tc.chain[static_cast<std::size_t>(tc.noteTarget)],
                       pos, n, node.itemShift - tc.latency, tc.rateEventFresh);
        while (!p.rateEnabled && tc.nextEvent < tc.events.size() &&
               double(tc.events[tc.nextEvent].sample) <
                   mappedAt(p, node.live, pos + n, node.itemShift - tc.latency)) {
            const TimedEvent& te = tc.events[tc.nextEvent++];
            host::Event e = te.event;
            e.time = eventOffset(p, node.live, pos, n, node.itemShift - tc.latency, te.sample);
            if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
        }
    }
    for (Stage& st : tc.chain) automateStage(st, pos, n, time::samplesAt(p.startSeconds, p.rate));
    for (Stage& st : tc.chain) {
        enterGroup(tc.chain, st, tc.buf, n);
        if (!runStage(p, st, tc.buf, n, transport, pos, self)) return false;
        leaveGroup(tc.chain, st, tc.buf, n);
    }
    if (tc.noteTarget >= 0) trackVoices(tc.chain[static_cast<std::size_t>(tc.noteTarget)], tc.held);
    const std::int64_t length = tc.end - tc.start;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::int64_t t = pos + i;
        const auto* clock = p.rateEnabled ? p.timelines[node.live ? 1 : 0].get() : nullptr;
        const double origin = clock ? clock->origin() : 0;
        const double mapped =
            clock ? clock->absolutePosition(t - node.itemShift) + double(node.itemShift)
                  : double(t);
        const double start = double(tc.start) + origin, end = double(tc.end) + origin;
        if (mapped < start || mapped >= end) continue;
        const double k = mapped - start;
        double g = 1.0;
        if (tc.fadeIn > 0 && k < tc.fadeIn)
            g *= fadeCurve(tc.fadeInShape, tc.fadeInCurve,
                           static_cast<double>(k) / static_cast<double>(tc.fadeIn));
        if (tc.fadeOut > 0 && k >= length - tc.fadeOut)
            g *= fadeCurve(tc.fadeOutShape, tc.fadeOutCurve,
                           static_cast<double>(length - k) / static_cast<double>(tc.fadeOut));
        if (tc.volume.active()) g *= dbToGain(tc.volume.at(t));
        if (tc.mute.active() && tc.mute.at(t) >= .5) g = 0.0;
        double l = tc.gainL * g, r = tc.gainR * g;
        if (tc.pan.active()) {
            const auto [pl, pr] = panGains(tc.pan.at(t) / 100.0, -3.0);
            l *= pl;
            r *= pr;
        }
        Stereo& into = node.postItems ? node.items : node.sum;  // ADR-0057
        into.l[i] += static_cast<float>(static_cast<double>(tc.buf.l[i]) * l);
        into.r[i] += static_cast<float>(static_cast<double>(tc.buf.r[i]) * r);
    }
    return true;
}

}  // namespace

// One block of every node, in order. Returns false on a plugin error.
bool processNode(Plan& p, std::size_t index, std::int64_t pos, std::uint32_t n,
                 const host::Transport& transport, const void* self) SUPPLE_RT {
    // ADR-0025: FTZ/DAZ for the whole node, on whichever thread runs it; the
    // thread's own MXCSR is back when the node returns.
    dsp::DenormalGuard denormals;
    Node& node = p.nodes[index];
    if (node.rack) return processRack(p, node, pos, n, transport, self);
    forEachStage(node, [](Stage& st) {
        st.outEvents.clear();
        st.sourceIds.clear();
        st.releases = 0;
        st.processed = false;
    });
    // 1. Sum the inputs (fixed order), each through its compensation delay.
    node.sum.clear(n);
    for (std::size_t ei : node.in)
        if (p.edges[ei].stage < 0 && !p.edges[ei].auxOnly)
            addEdge(p, p.edges[ei], -3.0, node.sum, pos, n);
    // Monitored device input enters with the inputs (spec §1.1); auto
    // monitoring plays the timeline instead while playing without recording.
    // Device state belongs to the callback; renderer-only nodes have no
    // monitored inputs and must not read it concurrently with liveRender().
    const bool hearInput = (!node.monitorInputs.empty() || node.midiIn.has_value()) &&
                           (!node.tapeAuto || !transport.playing || p.deviceRecording);
    const auto deviceChannel = [&](const std::vector<int>& inputs, std::size_t k) -> const float* {
        const int c = inputs[std::min(k, inputs.size() - 1)];
        return c >= 0 && static_cast<std::uint32_t>(c) < p.deviceInChannels ? p.deviceIn[c]
                                                                            : nullptr;
    };
    // Input FX: the armed record input through the input chain, every block
    // (recorded whether or not it is heard; ADR-0014).
    const bool inputFx = !node.recordInputs.empty() && p.deviceIn;
    if (inputFx) {
        for (int c = 0; c < 2; ++c) {
            const float* x = deviceChannel(node.recordInputs, static_cast<std::size_t>(c));
            float* y = c == 0 ? node.inputBuf.l.data() : node.inputBuf.r.data();
            if (x)
                std::copy_n(x, n, y);
            else
                std::fill_n(y, n, 0.0f);
        }
        for (Stage& st : node.inputChain) {
            st.blockEvents.clear();
            automateStage(st, pos, n, time::samplesAt(p.startSeconds, p.rate));
            enterGroup(node.inputChain, st, node.inputBuf, n);
            if (!runStage(p, st, node.inputBuf, n, transport, pos, self)) return false;
            leaveGroup(node.inputChain, st, node.inputBuf, n);
        }
    }
    if (hearInput && !node.monitorInputs.empty() && p.deviceIn) {
        const float* l = inputFx ? node.inputBuf.l.data() : deviceChannel(node.monitorInputs, 0);
        const float* r = inputFx ? node.inputBuf.r.data() : deviceChannel(node.monitorInputs, 1);
        if (l)
            for (std::uint32_t i = 0; i < n; ++i) node.sum.l[i] += l[i];
        if (r)
            for (std::uint32_t i = 0; i < n; ++i) node.sum.r[i] += r[i];
    }
    if (node.decodeInput)
        for (std::uint32_t i = 0; i < n; ++i)
            node.decoder.sample(node.sum.l[i], node.sum.r[i], node.sum.l[i], node.sum.r[i]);
    // Auxiliary links (ADR-0020), linear, after the decode.
    for (auto& a : node.aux) std::fill_n(a.begin(), n, 0.0f);
    for (std::size_t ei : node.in) {
        Edge& e = p.edges[ei];
        if (e.stage >= 0 || e.auxLinks.empty() || (p.split && e.frontier)) continue;
        Node& src = p.nodes[e.from];
        const bool post = e.tap == model::Tap::PostFader;
        const double routeGain = e.muted ? 0.0 : dbToGain(e.volumeDb);
        for (Edge::AuxLink& link : e.auxLinks) {
            const float* from = nullptr;
            double g = routeGain * (e.invert ? -1.0 : 1.0);
            if (link.from >= 2) {
                from = src.aux[static_cast<std::size_t>(link.from - 2)].data();
            } else {
                const Stereo& tap = e.tap == model::Tap::PreFx ? src.preFx : src.preFader;
                from = link.from == 0 ? tap.l.data() : tap.r.data();
                if (post && src.vcas.empty())
                    g *= src.mute || src.soloSilenced ? 0.0 : dbToGain(src.volumeDb);
            }
            std::copy_n(from, n, e.auxTmp.begin());
            if (post && link.from < 2 && !src.vcas.empty())
                for (std::uint32_t i = 0; i < n; ++i) e.auxTmp[i] *= src.auxPostGain[i];
            link.delay.process(e.auxTmp.data(), n);
            float* to = node.channel(link.to);
            const auto gain = static_cast<float>(g);
            for (std::uint32_t i = 0; i < n; ++i) to[i] += e.auxTmp[i] * gain;
        }
    }
    node.preFx.copyFrom(node.sum, n);
    // Audio items play after the pre-FX tap, before the chain (not when
    // stopped); a master's into its own buffer, joined after its plain stage
    // (ADR-0057).
    const bool postItems = node.postItems && node.hasItems();
    if (postItems) node.items.clear(n);
    Stereo& itemsInto = postItems ? node.items : node.sum;
    for (Clip& c : node.clips)
        if (transport.playing && c.takeChain < 0) playClip(p, c, itemsInto, pos, n, 0, node.live);
    // Takes with chains or envelopes: through their chains, into the sum.
    for (TakeChain& tc : node.takeChains)
        if (!processTake(p, node, tc, pos, n, transport, self)) return false;
    // A recording master's items are silent in the punch range (ADR-0057).
    if (postItems && node.silentTo > node.silentFrom)
        for (std::uint32_t i = 0; i < n; ++i) {
            const double at =
                mappedAt(p, node.live, pos + i, node.itemShift) - double(node.itemShift);
            if (at >= double(node.silentFrom) && at < double(node.silentTo))
                node.items.l[i] = node.items.r[i] = 0.0f;
        }
    // 2. Route this block's events to the note target. Notes still held at
    // a loop wrap end first, at the start of the block after it.
    for (Stage& st : node.chain) st.blockEvents.clear();
    if (node.releaseHeld) {
        node.releaseHeld = false;
        if (node.noteTarget >= 0) {
            const bool routeChanged = std::all_of(node.liveHeld.begin(), node.liveHeld.end(),
                                                  [](auto count) { return count == 0; });
            releaseVoices(node.chain[static_cast<std::size_t>(node.noteTarget)], routeChanged);
        }
        node.held.fill(0);
    } else if (node.noteTarget >= 0) {
        releaseVoices(node.chain[static_cast<std::size_t>(node.noteTarget)], true, true,
                      static_cast<std::int64_t>(mappedAt(p, node.live, pos, node.timelineShift)));
    }
    if (p.rateEnabled && transport.playing && node.noteTarget >= 0)
        rateEvents(p, node.live, node.events, node.chain[static_cast<std::size_t>(node.noteTarget)],
                   pos, n, node.timelineShift, node.rateEventFresh);
    while (!p.rateEnabled && transport.playing && node.nextEvent < node.events.size() &&
           double(node.events[node.nextEvent].sample) <
               mappedAt(p, node.live, pos + n, node.timelineShift)) {
        const TimedEvent& te = node.events[node.nextEvent++];
        if (node.noteTarget < 0) continue;
        host::Event e = te.event;
        e.time = eventOffset(p, node.live, pos, n, node.timelineShift, te.sample);
        auto& target = node.chain[static_cast<std::size_t>(node.noteTarget)].blockEvents;
        if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
    }
    // Live MIDI from the device (ADR-0016), playing or not, after timeline MIDI
    // at the same frame; not counted as held (a loop wrap does not cut what
    // the performer holds).
    if (hearInput && node.midiIn && node.noteTarget >= 0 && !p.deviceMidi.empty()) {
        auto& target = node.chain[static_cast<std::size_t>(node.noteTarget)].blockEvents;
        const std::size_t before = target.size();
        for (const MidiInputEvent& ev : p.deviceMidi) {
            if (ev.frame < p.deviceMidiOffset) continue;
            if (ev.frame >= p.deviceMidiOffset + n || target.size() >= target.capacity()) break;
            std::uint8_t m[3];
            if (!routeMidi(ev, node.midiIn->ports, node.midiIn->fromChannel, node.midiIn->toChannel,
                           m))
                continue;
            host::Event e;
            e.time = ev.frame - p.deviceMidiOffset;
            e.channel = static_cast<std::int16_t>(m[0] & 0x0f);
            const int kind = m[0] & 0xf0;
            if (kind == 0x90 && m[2] > 0) {
                e.kind = host::Event::Kind::NoteOn;
                e.key = m[1];
                e.velocity = m[2] / 127.0;
            } else if (kind == 0x80 || kind == 0x90) {
                e.kind = host::Event::Kind::NoteOff;
                e.key = m[1];
                e.velocity = m[2] / 127.0;
            } else {
                e.kind = host::Event::Kind::Midi;
                std::copy_n(m, 3, e.midi);
            }
            target.push_back(e);
        }
        if (target.size() > before)  // stable insertion sort by time: no allocation
            for (std::size_t i = 1; i < target.size(); ++i)
                for (std::size_t j = i; j > 0 && target[j - 1].time > target[j].time; --j)
                    std::swap(target[j - 1], target[j]);
    }
    // 2b. Plugin parameter automation (automateStage).
    for (Stage& st : node.chain) automateStage(st, pos, n, time::samplesAt(p.startSeconds, p.rate));
    // 3. The FX chain, in place on `sum`.
    for (Stage& st : node.chain) {
        // Its key first, every block (the key edges' delay lines and
        // automation keep time even while the stage is bypassed).
        if (st.keyed) {
            const int stageIndex = static_cast<int>(&st - node.chain.data());
            st.key.clear(n);
            for (std::size_t ei : node.in)
                if (p.edges[ei].stage == stageIndex) addEdge(p, p.edges[ei], -3.0, st.key, pos, n);
            // Key pins: track channels into the detector (a key on 3/4).
            if (!st.keyPins.empty()) {
                const float* kl = node.channel(st.keyPins[0]);
                const float* kr = node.channel(st.keyPins[st.keyPins.size() > 1 ? 1 : 0]);
                for (std::uint32_t i = 0; i < n; ++i) {
                    st.key.l[i] += kl[i];
                    st.key.r[i] += kr[i];
                }
            }
            if (st.keyChannels == 1)
                for (std::uint32_t i = 0; i < n; ++i)
                    st.key.l[i] = 0.5f * (st.key.l[i] + st.key.r[i]);
        }
        if (st.chanDelay.empty()) {
            enterGroup(node.chain, st, node.sum, n);
            if (!runStage(p, st, node.sum, n, transport, pos, self)) return false;
            leaveGroup(node.chain, st, node.sum, n);
        } else if (!runPinned(p, node, st, n, transport, pos, self)) {
            return false;
        }
    }
    if (node.noteTarget >= 0)
        trackVoices(node.chain[static_cast<std::size_t>(node.noteTarget)], node.held,
                    &node.liveHeld);
    // 3b. Polarity and mono (ADR-0022), on the main pair after the chain:
    // before the fader and the Console 9 encode, as the pre-fader taps hear it.
    if (node.frozenAudio) {
        const auto origin = time::samplesAt(p.startSeconds, p.rate);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto frame = origin + pos + i - node.timelineShift - node.latChain;
            for (std::size_t c = 0; c < node.frozenAudio->channels.size(); ++c)
                node.channel(static_cast<int>(c))[i] =
                    transport.playing && frame >= 0 &&
                            std::uint64_t(frame) < node.frozenAudio->frames()
                        ? node.frozenAudio->channels[c][static_cast<std::size_t>(frame)]
                        : 0.f;
        }
    } else {
        node.utility.set(node.phaseInvert, node.mono, p.smoothFrames);
        if (!node.utility.transparent())
            node.utility.process(node.sum.l.data(), node.sum.r.data(), n);
    }
    node.preFader.copyFrom(node.sum, n);
    // Metering (P5.3c): a strip's meter shows the signal before the Console 9
    // encode — after the fader and after the pan law (one multiply per channel
    // per sample, with the pan gains computed from libs/console9 when the pan
    // moves); a master's meter is its output, the signal the device is given.
    const bool metered = node.meterSlot != 0xffff;
    double meterPeakL = 0.0, meterPeakR = 0.0, meterSumL = 0.0, meterSumR = 0.0;
    double meterLeft = 0.0, meterRight = 0.0, meterPan = -1.0;
    // Fader controls and mute act before the channel encode; masters balance decoded audio.
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto at = pos + i;
        const bool volumeCurve = node.volumeCurve(), panCurve = node.panCurve();
        double db = volumeCurve ? node.autoVolume.at(at) : node.volumeDb;
        const double pan = panCurve ? node.autoPan.at(at) : node.pan;
        bool ownMuted = node.autoMute.active() ? node.autoMute.at(at) >= .5 : node.mute;
        node.vcas.evaluate(at, db, ownMuted);
        if (!node.vcas.empty()) db = std::min(db, model::kFaderCeilingDb);
        const bool muted = node.soloSilenced || ownMuted;
        const double g =
            volumeCurve || !node.vcas.empty() ? stripGain(node.master, db) : node.staticGain;
        const bool automated = volumeCurve || node.vcas.automated() || panCurve;
        node.fader.set(g, panCurve ? console9::nativePan(pan) : node.staticPan, muted, automated,
                       p.smoothFrames);
        const double gate = node.fader.gate.next();
        const double gain = node.fader.gain.next() * gate;
        const double nativePan = node.fader.pan.next();
        if (!node.vcas.empty()) node.auxPostGain[i] = static_cast<float>(gain);
        double keyGain = 0, keyGate = 0, keyPan = nativePan;
        if (node.postKeyed) {
            node.keyFader.set(g, panCurve ? console9::nativePan(pan) : node.staticPan, ownMuted,
                              automated, p.smoothFrames);
            keyGate = node.keyFader.gate.next();
            keyGain = node.keyFader.gain.next() * keyGate;
            keyPan = node.keyFader.pan.next();
        }
        if (node.master) {
            const double balance = nativePan * 2 - 1;
            const double l = balance <= 0 ? 1 : std::cos(balance * std::numbers::pi / 2);
            const double r = balance >= 0 ? 1 : std::cos(balance * std::numbers::pi / 2);
            node.post.l[i] = node.sum.l[i] * static_cast<float>(gain * l);
            node.post.r[i] = node.sum.r[i] * static_cast<float>(gain * r);
            if (node.postKeyed) {
                node.postKey.l[i] = node.sum.l[i] * static_cast<float>(keyGain * l);
                node.postKey.r[i] = node.sum.r[i] * static_cast<float>(keyGain * r);
            }
            // Its own items (ADR-0057): already mixed, so only the mute ramps them.
            if (postItems) {
                node.mix.l[i] = node.post.l[i];
                node.mix.r[i] = node.post.r[i];
                node.post.l[i] += node.items.l[i] * static_cast<float>(gate);
                node.post.r[i] += node.items.r[i] * static_cast<float>(gate);
                if (node.postKeyed) {
                    node.postKey.l[i] += node.items.l[i] * static_cast<float>(keyGate);
                    node.postKey.r[i] += node.items.r[i] * static_cast<float>(keyGate);
                }
            }
            if (metered) {
                meterPeakL = std::max(meterPeakL, std::fabs(double(node.post.l[i])));
                meterPeakR = std::max(meterPeakR, std::fabs(double(node.post.r[i])));
                meterSumL += double(node.post.l[i]) * double(node.post.l[i]);
                meterSumR += double(node.post.r[i]) * double(node.post.r[i]);
            }
        } else {
            node.encoder.sample(node.sum.l[i], node.sum.r[i], node.post.l[i], node.post.r[i], gain,
                                nativePan);
            if (node.postKeyed)
                node.keyEncoder.sample(node.sum.l[i], node.sum.r[i], node.postKey.l[i],
                                       node.postKey.r[i], keyGain, keyPan);
            if (metered) {
                if (nativePan != meterPan) {
                    console9::panGains(console9::panAngle(nativePan), meterLeft, meterRight);
                    meterPan = nativePan;
                }
                const double l = double(node.sum.l[i]) * gain * meterLeft;
                const double r = double(node.sum.r[i]) * gain * meterRight;
                meterPeakL = std::max(meterPeakL, std::fabs(l));
                meterPeakR = std::max(meterPeakR, std::fabs(r));
                meterSumL += l * l;
                meterSumR += r * r;
            }
        }
    }
    if (metered) {
        node.meterPeak[0] = static_cast<float>(meterPeakL);
        node.meterPeak[1] = static_cast<float>(meterPeakR);
        node.meterSumSq[0] = static_cast<float>(meterSumL);
        node.meterSumSq[1] = static_cast<float>(meterSumR);
    }
    // Independent encoders run at the source, before PDC and frontier publication.
    for (std::size_t ei : node.out) {
        Edge& e = p.edges[ei];
        const Stereo& input = e.tap == model::Tap::PreFx ? node.preFx : node.preFader;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto at = pos + i;
            const bool post = e.tap == model::Tap::PostFader;
            const bool volumeCurve = e.volumeCurve(), panCurve = e.panCurve();
            const bool stripVolumeCurve = e.stripVolumeCurve(), stripPanCurve = e.stripPanCurve();
            double sourceDb = stripVolumeCurve ? e.stripVolume.at(at) : node.volumeDb;
            bool sourceMute = e.stripMute.active() ? e.stripMute.at(at) >= .5 : node.mute;
            if (post) {
                e.vcas.evaluate(at, sourceDb, sourceMute);
                // A post-fader send starts from the member's applied level;
                // attenuation cannot recover gain beyond the member's ceiling.
                if (!e.vcas.empty()) sourceDb = std::min(sourceDb, model::kFaderCeilingDb);
            }
            const double routeDb = volumeCurve ? e.autoVolume.at(at) : e.volumeDb;
            const double sourcePan = stripPanCurve ? e.stripPan.at(at) : node.pan;
            const double pan = panCurve ? e.autoPan.at(at) : e.followPan ? sourcePan : e.pan;
            const bool muted = node.soloSilenced || (post && sourceMute) ||
                               (e.autoMute.active() ? e.autoMute.at(at) >= .5 : e.muted);
            // -inf plus a send boost remains -inf.
            const double db =
                (routeDb <= model::kSilenceDb || (post && sourceDb <= model::kSilenceDb))
                    ? model::kSilenceDb
                    : routeDb + (post ? sourceDb : 0);
            const double g = volumeCurve || (post && (stripVolumeCurve || !e.vcas.empty()))
                                 ? db <= model::kSilenceDb ? 0.0 : console9::faderGain(db)
                                 : e.staticGain;
            const double nativePan =
                panCurve || (e.followPan && stripPanCurve) ? console9::nativePan(pan) : e.staticPan;
            e.fader.set(g, nativePan, muted,
                        volumeCurve || panCurve ||
                            (post && (stripVolumeCurve || e.vcas.automated())) ||
                            (e.followPan && stripPanCurve),
                        p.smoothFrames);
            float l = input.l[i], r = input.r[i];
            if (e.mono) l = r = .5f * (l + r);
            if (e.invert) {
                l = -l;
                r = -r;
            }
            e.encoder.sample(l, r, e.encoded.l[i], e.encoded.r[i],
                             e.fader.gain.next() * e.fader.gate.next(), e.fader.pan.next());
            // The channel map acts on the send's output (after its pan): the
            // encode is per channel, so moving encoded channels is exact.
            e.applyMap(e.encoded.l[i], e.encoded.r[i]);
        }
    }
    if (p.rateEnabled)
        for (const auto& clip : node.clips)
            if (clip.rateState->failed ||
                (clip.rateState->previous && clip.rateState->previous->failed))
                return false;
    return true;
}

bool processBlock(Plan& p, std::int64_t pos, std::uint32_t n, bool playing,
                  const LoopRange* loop) SUPPLE_RT {
    prepareRateWindow(p, pos, n, -1, loop);
    syncMix(p, -1);
    host::Transport transport = transportAt(p, pos, loop);
    transport.playing = playing;
    for (std::size_t i = 0; i < p.nodes.size(); ++i)
        if (!processNode(p, i, pos, n, transport)) return false;
    return true;
}

namespace {
struct BlockRun {
    Plan* plan;
    std::int64_t pos;
    std::uint32_t frames;
    const host::Transport* transport;
    const std::vector<std::size_t>* nodes = nullptr;  // task -> node; null: identity
    const void* self = nullptr;
    std::atomic<bool> ok{true};
};
void runNode(void* ctx, std::uint32_t task) SUPPLE_RT {
    auto* b = static_cast<BlockRun*>(ctx);
    const std::size_t index = b->nodes ? (*b->nodes)[task] : task;
    if (!processNode(*b->plan, index, b->pos, b->frames, *b->transport, b->self))
        b->ok.store(false, std::memory_order_relaxed);
}
}  // namespace

bool processCallbackBlock(LiveWorkers* workers, Plan& p, std::int64_t pos, std::uint32_t n,
                          bool playing, bool split, const void* self,
                          const LoopRange* loop) SUPPLE_RT {
    prepareRateWindow(p, pos, n, split ? 1 : -1, loop);
    syncMix(p, split ? 1 : -1);
    host::Transport transport = transportAt(p, pos, loop, split);
    transport.playing = playing;
    BlockRun run{&p, pos, n, &transport, split ? &p.liveNodes : nullptr, self};
    TaskGraph& graph = split ? *p.liveGraph : *p.graph;
    if (workers)
        workers->run(graph, &runNode, &run);
    else
        for (std::uint32_t i = 0; i < graph.size(); ++i) runNode(&run, i);
    return run.ok.load(std::memory_order_relaxed);
}

bool processRendererBlock(Workers* workers, Plan& p, std::int64_t pos, std::uint32_t n,
                          const void* self, const LoopRange* loop, bool playing) {
    prepareRateWindow(p, pos, n, 0, loop);
    syncMix(p, 0);
    host::Transport transport = transportAt(p, pos, loop);
    transport.playing = playing;
    if (!workers || workers->helpers() == 0) {
        bool ok = true;
        for (std::size_t i : p.rendererNodes) ok = processNode(p, i, pos, n, transport, self) && ok;
        return ok;
    }
    BlockRun run{&p, pos, n, &transport, &p.rendererNodes, self};
    workers->run(*p.rendererGraph, &runNode, &run);
    return run.ok.load(std::memory_order_relaxed);
}

namespace {
void locateEdge(Edge& edge, bool restart) SUPPLE_RT {
    if (restart || !edge.preserveDelayOnLocate) edge.delay.clear();
    edge.preserveDelayOnLocate = false;
    for (auto& link : edge.auxLinks) {
        if (restart || !link.preserveDelayOnLocate) link.delay.clear();
        link.preserveDelayOnLocate = false;
    }
}
// Take chains (gap log #22) after a seek (`wrap` false: fresh at the next
// entry) or a loop wrap (held notes end; delay lines and tails kept).
void locateTakes(Node& n, std::int64_t pos, bool wrap, bool keepPlaying = false) SUPPLE_RT {
    for (TakeChain& tc : n.takeChains) {
        const auto it =
            std::lower_bound(tc.events.begin(), tc.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        tc.nextEvent = static_cast<std::size_t>(it - tc.events.begin());
        if (!wrap) tc.rateEventFresh = !keepPlaying;
        for (Automation* a : {&tc.volume, &tc.pan, &tc.mute}) a->cursor = 0;
        for (Stage& st : tc.chain) {
            if (!wrap && !keepPlaying) {
                st.voices.clear();
                st.sleeping = false;
            }
            if (!wrap && !st.preserveDryOnLocate) clearStageDelays(st);
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();
            }
        }
        if (wrap) {
            tc.releaseHeld = true;
        } else if (!keepPlaying) {  // a seek: fresh at the next entry
            tc.held.fill(0);
            tc.active = false;
        }
    }
}
}  // namespace

void locateClass(Plan& p, std::int64_t pos, bool live, bool restartConsole) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (n.live != live) continue;
        if (n.rack) locateRack(n, pos, restartConsole);
        locateTakes(n, pos, false, !restartConsole);
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        n.rateEventFresh = restartConsole;
        if (restartConsole) {
            n.encoder.reset();
            n.keyEncoder.reset();
            n.keyFader.reset();
            n.decoder.reset();
            n.fader.reset();
            n.utility.reset();
        }
        for (std::size_t ei : n.out) {
            Edge& e = p.edges[ei];
            if (restartConsole) {
                e.encoder.reset();
                e.fader.reset();
            }
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->cursor = 0;
            e.vcas.reset();
        }
        if (restartConsole && !n.releaseHeld) n.held.fill(0);
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->cursor = 0;
        n.vcas.reset();
        for (Stage& st : n.chain) {
            if (restartConsole && !n.releaseHeld) {
                st.voices.clear();
                st.sleeping = false;
            }
            if (!st.preserveDryOnLocate) clearStageDelays(st);
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();
            }
        }
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            locateEdge(e, restartConsole);
            if (!e.encode)
                for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripPan})
                    a->cursor = 0;
        }
    }
}

bool processBlockOn(Workers& workers, Plan& p, std::int64_t pos, std::uint32_t n) {
    prepareRateWindow(p, pos, n, -1);
    syncMix(p, -1);
    const host::Transport transport = transportAt(p, pos);
    BlockRun run{&p, pos, n, &transport};
    workers.run(*p.graph, &runNode, &run);
    return run.ok.load(std::memory_order_relaxed);
}

void locate(Plan& p, std::int64_t pos, bool restartConsole) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (n.rack) locateRack(n, pos, restartConsole);
        locateTakes(n, pos, false, !restartConsole);  // adoption keeps takes playing
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        n.rateEventFresh = restartConsole;
        if (restartConsole) {
            n.encoder.reset();
            n.keyEncoder.reset();
            n.keyFader.reset();
            n.decoder.reset();
            n.fader.reset();
            n.utility.reset();
        }
        for (std::size_t ei : n.out) {
            Edge& e = p.edges[ei];
            if (restartConsole) {
                e.encoder.reset();
                e.fader.reset();
            }
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->cursor = 0;
            e.vcas.reset();
        }
        if (restartConsole && !n.releaseHeld) n.held.fill(0);
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->cursor = 0;
        n.vcas.reset();
        for (Stage& st : n.chain) {
            if (restartConsole && !n.releaseHeld) {
                st.voices.clear();
                st.sleeping = false;
            }
            if (!st.preserveDryOnLocate) clearStageDelays(st);
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();  // resend at once
            }
        }
    }
    for (Edge& e : p.edges) {
        locateEdge(e, restartConsole);
        for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripPan}) a->cursor = 0;
    }
}

void wrapTo(Plan& p, std::int64_t pos, int liveClass) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        if (n.rack) locateRack(n, pos, true);  // notes held at the wrap end there
        if (!p.rateEnabled) locateTakes(n, pos, true);
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        n.encoder.reset();
        n.keyEncoder.reset();
        n.decoder.reset();
        for (std::size_t ei : n.out) {
            Edge& e = p.edges[ei];
            e.encoder.reset();
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->cursor = 0;
            e.vcas.reset();
        }
        if (!p.rateEnabled) n.releaseHeld = true;
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->cursor = 0;
        n.vcas.reset();
        for (Stage& st : n.chain)
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();  // the loop start's value
            }
        for (std::size_t ei : n.in)
            if (!p.edges[ei].encode)
                for (Automation* a : {&p.edges[ei].autoVolume, &p.edges[ei].autoPan,
                                      &p.edges[ei].autoMute, &p.edges[ei].stripPan})
                    a->cursor = 0;
    }
}

void carryLiveNotes(Plan& from, Plan& to, bool liveOnly) SUPPLE_RT {
    for (Node& old : from.nodes) {
        if (!old.midiIn) continue;
        const bool sounding = std::any_of(old.liveHeld.begin(), old.liveHeld.end(),
                                          [](std::uint8_t c) { return c != 0; });
        if (!sounding) continue;
        for (Node& n : to.nodes) {
            if (n.id != old.id) continue;
            if (liveOnly && !n.live) break;
            const bool sameRoute = n.midiIn && n.midiIn->ports == old.midiIn->ports &&
                                   n.midiIn->fromChannel == old.midiIn->fromChannel &&
                                   n.midiIn->toChannel == old.midiIn->toChannel;
            if (sameRoute) {
                n.liveHeld = old.liveHeld;  // still live: its note-offs will come
            } else {
                for (std::size_t k = 0; k < n.held.size(); ++k)
                    n.held[k] =
                        static_cast<std::uint8_t>(std::min(255, n.held[k] + old.liveHeld[k]));
                n.releaseHeld = true;  // ended at the start of the next block
            }
            break;
        }
    }
}

void writeOutputs(const Plan& p, float* const* out, std::uint32_t width, std::uint32_t offset,
                  std::uint32_t k) SUPPLE_RT {
    std::array<bool, 64> written{};
    auto put = [&](int c, const float* from, bool half, const float* other) {
        if (c < 0 || static_cast<std::uint32_t>(c) >= width || c >= 64) return;
        float* to = out[c] + offset;
        const bool first = !written[static_cast<std::size_t>(c)];
        written[static_cast<std::size_t>(c)] = true;
        if (half) {
            for (std::uint32_t i = 0; i < k; ++i) {
                const float v = 0.5f * (from[i] + other[i]);
                to[i] = first ? v : to[i] + v;
            }
        } else if (first) {
            std::copy_n(from, k, to);
        } else {
            for (std::uint32_t i = 0; i < k; ++i) to[i] += from[i];
        }
    };
    for (const OutputTap& o : p.outputs) {
        const Stereo& post = p.nodes[o.node].post;
        if (o.right < 0) {
            put(o.left, post.l.data(), true, post.r.data());
        } else {
            put(o.left, post.l.data(), false, nullptr);
            put(o.right, post.r.data(), false, nullptr);
        }
    }
    for (std::uint32_t c = 0; c < width && c < 64; ++c)
        if (!written[c]) std::fill_n(out[c] + offset, k, 0.0f);
}

void carryMix(Plan& from, Plan& to, int liveClass) SUPPLE_RT {
    for (Node& n : to.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        for (Node& old : from.nodes) {  // (plans are small; a swap is rare)
            if (old.id != n.id) continue;
            // Only what this executor processed (the other one writes the rest).
            if (liveClass >= 0 && old.live != (liveClass == 1)) break;
            // Match inside the destination's incoming edges: scanning every
            // node to rediscover each edge's destination would be cubic.
            for (std::size_t ei : n.in) {
                Edge& edge = to.edges[ei];
                for (std::size_t oi : old.in) {
                    Edge& previous = from.edges[oi];
                    if (edge.route != previous.route || edge.tap != previous.tap ||
                        (edge.stage >= 0) != (previous.stage >= 0) ||
                        to.nodes[edge.from].id != from.nodes[previous.from].id)
                        continue;
                    if (edge.stage >= 0 &&
                        n.chain[static_cast<std::size_t>(edge.stage)].fx !=
                            old.chain[static_cast<std::size_t>(previous.stage)].fx)
                        continue;
                    if (edge.delay.length == previous.delay.length) {
                        for (int c = 0; c < 2; ++c)
                            std::swap(edge.delay.buf[c], previous.delay.buf[c]);
                        edge.delay.pos = previous.delay.pos;
                        edge.preserveDelayOnLocate = true;
                    }
                    for (auto& link : edge.auxLinks)
                        for (auto& oldLink : previous.auxLinks)
                            if (link.from == oldLink.from && link.to == oldLink.to &&
                                link.delay.length == oldLink.delay.length) {
                                std::swap(link.delay.buf, oldLink.delay.buf);
                                link.delay.pos = oldLink.delay.pos;
                                link.preserveDelayOnLocate = true;
                                break;
                            }
                    break;
                }
            }
            if (n.type == old.type) {
                n.fader = old.fader;
                n.utility = old.utility;
                n.encoder.carry(old.encoder, to.smoothFrames);
                n.keyEncoder.carry(old.keyEncoder, to.smoothFrames);
                n.keyFader = old.keyFader;
                n.decoder.carry(old.decoder, to.smoothFrames);
                for (std::size_t ei : n.out)
                    for (std::size_t oi : old.out) {
                        Edge& e = to.edges[ei];
                        const Edge& oe = from.edges[oi];
                        if (e.route == oe.route && e.tap == oe.tap) {
                            e.fader = oe.fader;
                            e.encoder.carry(oe.encoder, to.smoothFrames);
                            break;
                        }
                    }
            }
            if (n.noteTarget >= 0 && old.noteTarget >= 0 &&
                n.chain[static_cast<std::size_t>(n.noteTarget)].instance &&
                n.chain[static_cast<std::size_t>(n.noteTarget)].instance ==
                    old.chain[static_cast<std::size_t>(old.noteTarget)].instance) {
                for (std::size_t k = 0; k < n.held.size(); ++k)
                    n.held[k] = static_cast<std::uint8_t>(std::min(255, n.held[k] + old.held[k]));
                n.releaseHeld = n.releaseHeld || old.releaseHeld;
            }
            auto carryStages = [&](std::vector<Stage>& chain, std::vector<Stage>& oldChain) {
                for (Stage& st : chain)
                    for (Stage& os : oldChain)
                        if (st.instance && os.instance == st.instance) {
                            st.nextNoteId = os.nextNoteId;
                            st.sleeping = os.sleeping;
                            for (const auto& voice : os.voices) {
                                if (st.voices.size() == st.voices.capacity()) {
                                    ++st.voiceDropped;
                                    break;
                                }
                                auto carried = voice;
                                if (carried.sourceId >= 0) {
                                    NoteSource source;
                                    source.origin = carried.origin;
                                    source.port = carried.port;
                                    source.channel = carried.channel;
                                    source.key = carried.key;
                                    source.ordinal = carried.sourceOrdinal;
                                    const auto found = std::lower_bound(
                                        st.noteSourceIndex.begin(), st.noteSourceIndex.end(),
                                        source, [](const auto& a, const auto& b) {
                                            return a.address() < b.address();
                                        });
                                    if (found != st.noteSourceIndex.end() &&
                                        found->address() == source.address())
                                        carried.sourceId = found->id;
                                    else
                                        carried.release = true;
                                }
                                if (carried.live && n.releaseHeld && !n.midiIn)
                                    carried.live = false;
                                st.voices.push_back(carried);
                            }
                            st.appliedWet = os.appliedWet;
                            st.rampFromWet = os.rampFromWet;
                            st.rampToWet = os.rampToWet;
                            st.wetRampLeft = os.wetRampLeft;
                            auto carryDelay = [](Delay& dst, Delay& src) {
                                if (dst.length != src.length) return false;
                                std::swap(dst.buf[0], src.buf[0]);
                                std::swap(dst.buf[1], src.buf[1]);
                                dst.pos = src.pos;
                                return true;
                            };
                            if (carryDelay(st.dry, os.dry)) {
                                st.preserveDryOnLocate = true;
                                carryDelay(st.align, os.align);
                                if (st.os == os.os && carryDelay(st.osPad, os.osPad))
                                    std::swap(st.osFilters, os.osFilters);
                            }
                            if (st.chanDelay.size() == os.chanDelay.size() &&
                                std::equal(std::begin(st.outPin), std::end(st.outPin),
                                           std::begin(os.outPin))) {
                                for (std::size_t c = 0; c < st.chanDelay.size(); ++c) {
                                    MonoDelay& delay = st.chanDelay[c];
                                    MonoDelay& oldDelay = os.chanDelay[c];
                                    if (delay.length != oldDelay.length) continue;
                                    std::swap(delay.buf, oldDelay.buf);
                                    delay.pos = oldDelay.pos;
                                }
                            }
                            break;
                        }
            };
            carryStages(n.inputChain, old.inputChain);
            carryStages(n.chain, old.chain);
            // A take chain playing on keeps playing (no reset at the swap).
            for (TakeChain& tc : n.takeChains)
                for (TakeChain& otc : old.takeChains)
                    if (otc.take == tc.take) {
                        tc.active = otc.active;
                        tc.held = otc.held;
                        carryStages(tc.chain, otc.chain);
                        break;
                    }
            break;
        }
    }
}

double stripGain(bool master, double db) SUPPLE_RT {
    return master ? dbToGain(db) : db <= model::kSilenceDb ? 0.0 : console9::faderGain(db);
}

double sendGain(double routeDb, model::Tap tap, double sourceDb) SUPPLE_RT {
    const bool post = tap == model::Tap::PostFader;
    // -inf plus a send boost remains -inf.
    const double db = routeDb <= model::kSilenceDb || (post && sourceDb <= model::kSilenceDb)
                          ? model::kSilenceDb
                          : routeDb + (post ? sourceDb : 0);
    return db <= model::kSilenceDb ? 0.0 : console9::faderGain(db);
}

namespace {
bool held(std::span<const model::EnvTarget> holds, Id object, std::string_view param) {
    return std::any_of(holds.begin(), holds.end(), [&](const model::EnvTarget& t) {
        return t.object == object && t.param == param;
    });
}
}  // namespace

void publishMix(Plan& p, const model::Session& s, std::span<const model::EnvTarget> holds) {
    MixSlots& mix = *p.mix;
    constexpr auto relaxed = std::memory_order_relaxed;
    const std::uint64_t sequence = mix.sequence.load(relaxed);
    mix.sequence.store(sequence + 1, relaxed);  // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    auto store = [&](MixSlots::Value& slot, double db, double pan, bool holdVolume, bool holdPan) {
        slot.volumeDb.store(db, relaxed);
        slot.pan.store(pan, relaxed);
        slot.holdVolume.store(holdVolume, relaxed);
        slot.holdPan.store(holdPan, relaxed);
    };
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const Id id = p.nodes[i].id;
        const model::Track* t = s.tracks.find(id);
        if (!t) continue;
        store(mix.strips[i], t->volumeDb, t->pan, held(holds, id, "volume"),
              held(holds, id, "pan"));
    }
    for (std::size_t i = 0; i < p.edges.size(); ++i) {
        const Edge& e = p.edges[i];
        const model::Route* r = e.route ? s.routes.find(*e.route) : nullptr;
        const model::Track* source = r && r->from.track ? s.tracks.find(*r->from.track) : nullptr;
        if (!r || !source) continue;  // a parent send: nothing to publish
        store(mix.routes[i], r->volumeDb, r->pan.resolve(source->pan),
              held(holds, *e.route, "volume"), held(holds, *e.route, "pan"));
    }
    for (std::size_t i = 0; i < mix.vcaIds.size(); ++i) {
        const model::TrackGroup* g = s.groups.find(mix.vcaIds[i]);
        if (!g || !g->vca) continue;
        store(mix.vcas[i], g->vca->volumeDb, 0, held(holds, mix.vcaIds[i], "volume"), false);
    }
    mix.sequence.store(sequence + 2, std::memory_order_release);
}

void syncMix(Plan& p, int liveClass) SUPPLE_RT {
    MixSlots& mix = *p.mix;
    std::uint64_t& seen = mix.seen[liveClass == 1 ? 1 : 0];
    const std::uint64_t sequence = mix.sequence.load(std::memory_order_acquire);
    if (sequence == seen || (sequence & 1)) return;
    constexpr auto relaxed = std::memory_order_relaxed;
    auto ours = [&](const Node& n) { return liveClass < 0 || n.live == (liveClass == 1); };
    auto syncVcas = [&](VcaControls& vcas) {
        for (VcaControl& v : vcas.values) {
            v.volumeDb = mix.vcas[v.slot].volumeDb.load(relaxed);
            v.hold = mix.vcas[v.slot].holdVolume.load(relaxed);
        }
    };
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        Node& n = p.nodes[i];
        if (!ours(n)) continue;
        const MixSlots::Value& slot = mix.strips[i];
        n.volumeDb = slot.volumeDb.load(relaxed);
        n.pan = slot.pan.load(relaxed);
        n.holdVolume = slot.holdVolume.load(relaxed);
        n.holdPan = slot.holdPan.load(relaxed);
        n.staticGain = stripGain(n.master, n.volumeDb);
        n.staticPan = console9::nativePan(n.pan);
        syncVcas(n.vcas);
    }
    // An edge belongs to whoever reads it: a key route to its destination
    // (addEdge), any other to its source (the send's encode). The source's
    // level comes from its slot: the source node can belong to the other
    // executor.
    for (std::size_t i = 0; i < p.edges.size(); ++i) {
        Edge& e = p.edges[i];
        if (!e.route || !ours(p.nodes[e.stage >= 0 ? e.to : e.from])) continue;
        const MixSlots::Value& slot = mix.routes[i];
        const MixSlots::Value& source = mix.strips[e.from];
        e.volumeDb = slot.volumeDb.load(relaxed);
        e.pan = slot.pan.load(relaxed);
        e.holdVolume = slot.holdVolume.load(relaxed);
        e.holdPan = slot.holdPan.load(relaxed);
        e.holdStripVolume = source.holdVolume.load(relaxed);
        e.holdStripPan = source.holdPan.load(relaxed);
        e.staticGain = sendGain(e.volumeDb, e.tap, source.volumeDb.load(relaxed));
        e.staticPan = console9::nativePan(e.pan);
        syncVcas(e.vcas);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (mix.sequence.load(relaxed) == sequence) seen = sequence;
}

}  // namespace supple::engine::detail
