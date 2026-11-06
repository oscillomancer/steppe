// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_runtime.h"

#include <algorithm>
#include <cmath>

#include "supple/rack/stock.h"

namespace supple::engine {

namespace sd = script::detail;

std::shared_ptr<const rack::ModuleType> pluginModuleType() {
    using Signal = sd::PortModel::Signal;
    static const auto t = [] {
        sd::ModuleInfoModel info;
        info.name = "Plugin";
        info.id = std::string(model::kPluginModule);
        info.vendor = "Supple Audio";
        info.description =
            "A plug-in in the patch: audio through it, notes from pitch and gate cables.";
        info.tags = {"io", "plugin"};
        std::vector<sd::PortModel> ports;
        auto port = [&](const char* key, const char* name, Signal signal, bool output,
                        const char* help, double normal = 0) {
            sd::PortModel p;
            p.key = key;
            p.member = key;
            p.name = name;
            p.signal = signal;
            p.output = output;
            p.description = help;
            p.normal = normal;
            ports.push_back(std::move(p));
        };
        port("in_l", "In L", Signal::Audio, false, "The plug-in's left input.");
        port("in_r", "In R", Signal::Audio, false,
             "Its right input; the left one while nothing is connected.");
        port("pitch", "Pitch", Signal::Pitch, false,
             "Each voice's note, in octaves from C4: a gate's rise plays it.");
        port("gate", "Gate", Signal::Gate, false, "A note per voice while high.");
        port("velocity", "Velocity", Signal::Unipolar, false,
             "Each voice's velocity at its gate's rise, 0 to 1.", 0.8);
        port("out_l", "Out L", Signal::Audio, true, "The plug-in's left output.");
        port("out_r", "Out R", Signal::Audio, true, "Its right output.");
        return rack::ModuleType::host(std::move(info), {}, std::move(ports));
    }();
    return t;
}

double moduleParamValue(const sd::ParamModel& param, const json::Value* value) {
    using K = sd::ParamModel::Kind;
    if (!value) return param.def;
    if (param.kind == K::Choice) {
        if (value->isString()) {
            for (std::size_t k = 0; k < param.choices.size(); ++k)
                if (sd::choiceKey(param, k) == value->asString() ||
                    param.choices[k] == value->asString())
                    return static_cast<double>(k);
            return param.def;
        }
        if (value->isNumber())
            return std::clamp(std::round(value->asDouble()), param.min, param.max);
        return param.def;
    }
    if (param.kind == K::Toggle) {
        if (value->isBool()) return value->asBool() ? 1.0 : 0.0;
        if (value->isNumber()) return value->asDouble() >= 0.5 ? 1.0 : 0.0;
        return param.def;
    }
    if (!value->isNumber() || !std::isfinite(value->asDouble())) return param.def;
    const double v = std::clamp(value->asDouble(), param.min, param.max);
    return param.kind == K::Integer ? std::round(v) : v;
}

json::Value moduleParamJson(const sd::ParamModel& param, double physical) {
    using K = sd::ParamModel::Kind;
    if (param.kind == K::Choice) {
        const auto k = static_cast<std::size_t>(
            std::clamp(std::lround(physical), 0L, static_cast<long>(param.choices.size()) - 1));
        return json::Value(sd::choiceKey(param, k));
    }
    if (param.kind == K::Toggle) return json::Value(physical >= 0.5);
    if (param.kind == K::Integer)
        return json::Value(static_cast<std::int64_t>(std::lround(physical)));
    return json::Value(physical);
}

}  // namespace supple::engine

namespace supple::engine::detail {

std::shared_ptr<const rack::ModuleType> probeModuleType() {
    static const auto t = [] {
        sd::ModuleInfoModel info;
        info.name = "Probe";
        info.id = std::string(kProbeModule);
        info.vendor = "Supple Audio";
        info.description = "rack.probe's listener.";
        sd::ParamModel seconds;
        seconds.key = seconds.member = "seconds";
        seconds.name = "Seconds";
        seconds.min = 0;
        seconds.max = 11;
        seconds.def = 2;
        sd::PortModel in;
        in.key = in.member = "in";
        in.name = "In";
        in.signal = sd::PortModel::Signal::Cv;
        return rack::ModuleType::host(std::move(info), {seconds}, {in});
    }();
    return t;
}

void ProbeModule::process(const rack::HostContext& ctx) SUPPLE_RT {
    const script::Input& in = ctx.instance->inputMember(0);
    width = std::max(width, in.channels());
    for (int c = 0; c < kProbeChannels; ++c) {
        std::vector<float>& to = recorded[static_cast<std::size_t>(c)];
        for (int i = 0; i < ctx.frames && to.size() < to.capacity(); ++i)
            to.push_back(static_cast<float>(in[c][i]));
    }
}

std::shared_ptr<const rack::Catalog> builtinModuleCatalog() {
    static const std::shared_ptr<const rack::Catalog> catalog = [] {
        auto c = std::make_shared<rack::Catalog>();
        for (auto type : {rack::host::outType(), rack::host::inType(), rack::host::notesType(),
                          rack::host::transportType(), pluginModuleType()})
            c->add(std::move(type));
        for (const rack::StockModule* m : rack::stockModules())
            c->add(
                rack::ModuleType::fromCode(m->code, rack::Origin::Stock, std::string(m->source)));
        return std::shared_ptr<const rack::Catalog>(std::move(c));
    }();
    return catalog;
}

// --- the pool ----------------------------------------------------------------------------

RackPool::RackPool() : catalog_(builtinModuleCatalog()) {}
RackPool::~RackPool() = default;

void RackPool::setCatalog(std::shared_ptr<const rack::Catalog> catalog) {
    catalog_ = catalog ? std::move(catalog) : builtinModuleCatalog();
}

namespace {
void setParams(rack::Instance& in, const model::RackModule& m) {
    const auto& params = in.type().params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        const json::Value* v = m.params.isObject() ? m.params.find(params[i].key) : nullptr;
        in.setParam(static_cast<int>(i), moduleParamValue(params[i], v));
    }
}
}  // namespace

rack::Instance* RackPool::get(Id id, const model::RackModule& module,
                              const rack::Instance::Setup& setup) {
    const rack::Catalog::Entry* type = catalog_->find(module.type);
    auto it = modules_.find(id);
    if (!type) {
        if (it != modules_.end()) {
            retired_.emplace_back(generation_, std::move(it->second.instance));
            modules_.erase(it);
        }
        return nullptr;
    }
    if (it != modules_.end() &&
        (it->second.type != module.type || it->second.code != type->generation ||
         it->second.rate != setup.sampleRate || it->second.seed != setup.seed ||
         &it->second.instance->type() != type->type.get())) {
        retired_.emplace_back(generation_, std::move(it->second.instance));
        modules_.erase(it);
        it = modules_.end();
    }
    if (it == modules_.end()) {
        std::unique_ptr<rack::Instance> made =
            type->type->code() ? rack::Instance::make(type->type, setup)
            : module.type == model::kPluginModule
                ? rack::Instance::make(type->type, std::make_unique<PluginModule>(), setup)
            : module.type == kProbeModule
                ? rack::Instance::make(type->type, std::make_unique<ProbeModule>(), setup)
                : rack::host::make(type->type, setup);
        if (!made) return nullptr;  // a host type this engine does not make here
        it = modules_
                 .emplace(id, Entry{std::move(made), module.type, type->generation,
                                    setup.sampleRate, setup.seed})
                 .first;
    }
    setParams(*it->second.instance, module);
    if (auto* probe = dynamic_cast<ProbeModule*>(it->second.instance->host()))
        probe->reserve(
            static_cast<std::size_t>(std::ceil(it->second.instance->param(0) * setup.sampleRate)) +
            rack::kGrid);
    return it->second.instance.get();
}

rack::Instance* RackPool::find(Id id) const {
    auto it = modules_.find(id);
    return it == modules_.end() ? nullptr : it->second.instance.get();
}

void RackPool::prune(const Session& session, std::uint64_t liveGeneration) {
    for (auto it = modules_.begin(); it != modules_.end();)
        it = session.modules.contains(it->first) ? std::next(it) : modules_.erase(it);
    std::erase_if(retired_, [&](const auto& r) { return r.first <= liveGeneration; });
}

void RackPool::applyParams(const Session& session) {
    for (auto& [id, e] : modules_)
        if (const model::RackModule* m = session.modules.find(id)) setParams(*e.instance, *m);
}

// --- curves and the clock ----------------------------------------------------------------

double RackCurve::at(std::int64_t position) SUPPLE_RT {
    const std::int64_t sample = rack->playing ? position - rack->origin : rack->stoppedAt;
    return curve.at(sample);
}

script::Transport RackClock::transport(std::int64_t position) SUPPLE_RT {
    const std::int64_t sample = rack->playing ? position - rack->origin : rack->stoppedAt;
    const host::Transport t = transportAt(*plan, sample, nullptr, rack->live);
    script::Transport out;
    out.playing = rack->playing;
    out.tempo = t.tempo;
    out.beats = t.beat;
    out.barStart = t.barStartBeat;
    out.timeSigNumerator = t.numerator;
    out.timeSigDenominator = t.denominator;
    return out;
}

// --- compile -----------------------------------------------------------------------------

Status compileRack(Plan& p, Node& n, const Session& s, Id rackId, InstancePool& plugins) {
    RackPool& pool = plugins.racks();
    auto r = std::make_shared<RackNode>();
    r->rack = rackId;
    r->origin = time::samplesAt(p.startSeconds, p.rate);
    r->clock.rack = r.get();  // its plan is set per block: plans move after compile
    rack::Patch patch;
    std::map<Id, int> index;
    for (const auto& e : s.modules) {
        const model::RackModule& m = *e.value;
        if (m.rack != rackId) continue;
        rack::Instance::Setup setup;
        setup.sampleRate = p.rate;
        // Deterministic per module and project (rack.md §4.2).
        const std::uint64_t mix =
            (s.project->seed ^ (std::uint64_t{e.id.n} << 20)) * 0x9E3779B97F4A7C15ULL;
        setup.seed = static_cast<int>((mix >> 33) & 0x7fffffff);
        rack::Instance* in = pool.get(e.id, m, setup);
        if (!in)
            p.warnings.push_back(e.id.str() + ": module type " + m.type +
                                 " is not installed; it is silent");
        index[e.id] = static_cast<int>(patch.modules.size());
        patch.modules.push_back({e.id.n, in, m.bypass, {}});
        r->modules.push_back(e.id);
        if (!in) continue;
        if (auto* out = dynamic_cast<rack::host::Out*>(in->host())) {
            r->outs.push_back(out);
            r->outIds.push_back(e.id);
        } else if (auto* inMod = dynamic_cast<rack::host::In*>(in->host())) {
            r->ins.push_back(inMod);
            r->inIds.push_back(e.id);
        } else if (auto* notes = dynamic_cast<rack::host::Notes*>(in->host())) {
            NotesFeed feed;
            feed.notes = notes;
            feed.device = m.device;
            r->notes.push_back(std::move(feed));
        } else if (auto* plugin = dynamic_cast<PluginModule*>(in->host())) {
            // Its plug-in: a stage of the rack node, from the instance pool.
            plugin->stage = -1;
            const model::FxInstance* fx = m.fx ? s.fx.find(*m.fx) : nullptr;
            if (fx) {
                Stage st;
                st.fx = *m.fx;
                st.bypass = fx->bypass || fx->offline;
                st.wet = fx->wet / 100.0;
                if (!fx->offline) {
                    auto instance = plugins.get(*m.fx, *fx, s);
                    if (!instance)
                        p.warnings.push_back(m.fx->str() + " (" + fx->plugin.id +
                                             "): " + instance.error().message + "; it is silent");
                    else
                        st.instance = *instance;
                }
                plugin->stage = static_cast<int>(n.rackStages.size());
                n.rackStages.push_back(std::move(st));
            }
            r->plugins.push_back(plugin);
        }
    }
    for (const auto& e : s.cables) {
        const model::Cable& c = *e.value;
        auto from = index.find(c.from.object);
        if (from == index.end()) continue;  // another rack's
        const rack::Instance* src = patch.modules[static_cast<std::size_t>(from->second)].instance;
        if (c.to.object.kind != Kind::Module) {
            p.warnings.push_back(e.id.str() + ": cables into a plug-in's parameters (" +
                                 c.to.str() + ") are not heard yet");
            continue;
        }
        auto to = index.find(c.to.object);
        if (to == index.end()) continue;
        const rack::Instance* dst = patch.modules[static_cast<std::size_t>(to->second)].instance;
        if (!src || !dst) continue;  // a missing type: its cables carry nothing
        const int output = src->type().output(c.from.key);
        if (output < 0) {
            p.warnings.push_back(e.id.str() + ": " + c.from.str() + " is not an output of " +
                                 src->type().id() + "; the cable is ignored");
            continue;
        }
        rack::Patch::Cable cable;
        cable.from = from->second;
        cable.output = output;
        cable.to = to->second;
        if (const int input = dst->type().input(c.to.key); input >= 0) {
            cable.index = input;
        } else if (const int param = dst->type().param(c.to.key); param >= 0) {
            const sd::ParamModel& pm = dst->type().params()[static_cast<std::size_t>(param)];
            if (pm.kind == sd::ParamModel::Kind::Toggle || pm.output) {
                p.warnings.push_back(e.id.str() + ": " + c.to.str() +
                                     " is a switch or a meter, which cables do not move");
                continue;
            }
            cable.param = true;
            cable.index = param;
            cable.amount = c.amount.value_or(1.0);
        } else {
            p.warnings.push_back(e.id.str() + ": " + c.to.str() + " is not an input or a knob of " +
                                 dst->type().id() + "; the cable is ignored");
            continue;
        }
        patch.cables.push_back(cable);
    }
    // Module knob envelopes (rack.md §6): compiled here, positioned in prepareRack.
    const std::int64_t origin = r->origin;
    for (const auto& e : s.envelopes) {
        const model::Envelope& env = *e.value;
        auto at = index.find(env.target.object);
        if (at == index.end() || !env.active) continue;
        rack::Patch::Module& pm = patch.modules[static_cast<std::size_t>(at->second)];
        if (!pm.instance) continue;
        const int param = pm.instance->type().param(env.target.param);
        if (param < 0) {
            p.warnings.push_back(e.id.str() + ": " + env.target.object.str() + " has no knob " +
                                 env.target.param + "; its automation is not heard");
            continue;
        }
        const auto points = model::effectivePoints(s, e.id);
        if (points.empty()) continue;
        auto curve = std::make_unique<RackCurve>();
        curve->rack = r.get();
        for (const model::EnvPoint& pt : points) {
            const double sec =
                env.timebase == model::Timebase::Time ? pt.pos : s.timeMap->secondsAt(pt.pos);
            curve->curve.points.push_back(
                {time::samplesAt(sec, p.rate) - origin, pt.value, pt.shape, pt.tension});
        }
        pm.automation.emplace_back(param, curve.get());
        r->curves.push_back(std::move(curve));
    }
    std::string error;
    r->schedule = rack::Schedule::build(std::move(patch), &error);
    if (!r->schedule)
        return Error("rack.compile", rackId.str() + ": " + error, "/racks/" + rackId.str(),
                     "remove the cable or module the message names");
    n.rackOuts.assign(r->outs.size(), {});
    n.rackIns.assign(r->ins.size(), {});
    n.rack = std::move(r);
    return {};
}

void prepareRack(Plan& p, Node& n, const Session& s) {
    RackNode& r = *n.rack;
    // Plugin modules' stages: their input is the rack's, their automation
    // lines up with it; every grid block's notes and parameter events fit.
    for (Stage& st : n.rackStages) {
        st.inputLatency = n.timelineShift;
        std::size_t need = 4 * static_cast<std::size_t>(rack::kChannels) * rack::kGrid + 1;
        for (ParamLane& ln : st.lanes) {
            ln.curve.shift(n.timelineShift);
            need += rack::kGrid / kAutomationStep + 2 + ln.curve.points.size();
        }
        st.blockEvents.reserve(need);
    }
    for (PluginModule* plugin : r.plugins) plugin->io.init(p.block);
    for (Stereo& out : n.rackOuts) out.init(p.block);
    for (Stereo& in : n.rackIns) in.init(p.block);
    for (std::size_t k = 0; k < r.ins.size(); ++k)
        r.ins[k]->bind(n.rackIns[k].l.data(), n.rackIns[k].r.data());
    r.live = n.live;
    // A Notes module plays its source track's MIDI (its rack's home track
    // when unset), as that track's instrument would hear it.
    for (std::size_t k = 0, f = 0; k < r.modules.size() && f < r.notes.size(); ++k) {
        const model::RackModule* m = s.modules.find(r.modules[k]);
        if (!m || m->type != model::kNotesModule) continue;
        NotesFeed& feed = r.notes[f++];
        const model::Rack* home = s.racks.find(r.rack);
        const Id source = m->source.value_or(home ? home->track : model::kMaster);
        feed.events = compileTrackEvents(s, source, p.startSeconds, p.rate);
        for (auto& ev : feed.events) ev.sample += n.timelineShift;
        std::size_t window = 0;
        for (std::size_t a = 0, b = 0; a < feed.events.size(); ++a) {
            if (b < a) b = a;
            while (b < feed.events.size() &&
                   feed.events[b].sample <
                       feed.events[a].sample + static_cast<std::int64_t>(p.block))
                ++b;
            window = std::max(window, b - a);
        }
        feed.block.reserve(window + (feed.device ? kMaxMidiInputEvents : 0) + 1);
    }
    for (auto& curve : r.curves) curve->curve.cursor = 0;
}

void locateRack(Node& n, std::int64_t pos, bool restart) SUPPLE_RT {
    RackNode& r = *n.rack;
    for (NotesFeed& feed : r.notes) {
        auto it =
            std::lower_bound(feed.events.begin(), feed.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        feed.next = static_cast<std::size_t>(it - feed.events.begin());
        if (restart) feed.notes->endAll();
    }
    for (auto& curve : r.curves) curve->curve.cursor = 0;
}

namespace {
// A host event as a MIDI short message at a frame.
bool toMidi(const host::Event& e, std::uint32_t frame, rack::host::MidiEvent& out) {
    out.frame = frame;
    const auto channel = static_cast<std::uint8_t>(std::clamp<int>(e.channel, 0, 15));
    switch (e.kind) {
        case host::Event::Kind::NoteOn:
            out.status = static_cast<std::uint8_t>(0x90 | channel);
            out.data1 = static_cast<std::uint8_t>(std::clamp<int>(e.key, 0, 127));
            out.data2 = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(std::lround(e.velocity * 127.0)), 1, 127));
            return true;
        case host::Event::Kind::NoteOff:
            out.status = static_cast<std::uint8_t>(0x80 | channel);
            out.data1 = static_cast<std::uint8_t>(std::clamp<int>(e.key, 0, 127));
            out.data2 = 0;
            return true;
        case host::Event::Kind::Midi:
            out.status = e.midi[0];
            out.data1 = e.midi[1];
            out.data2 = e.midi[2];
            return (out.status & 0x80) != 0;
        default:
            return false;
    }
}
}  // namespace

bool processRack(Plan& p, Node& n, std::int64_t pos, std::uint32_t frames,
                 const host::Transport& transport, const void* self) SUPPLE_RT {
    RackNode& r = *n.rack;
    r.clock.plan = &p;
    for (PluginModule* plugin : r.plugins) {
        plugin->plan = &p;
        plugin->node = &n;
        plugin->transport = &transport;
        plugin->self = self;
        plugin->pos = pos;
        plugin->failed = false;
    }
    // Sends into the In modules, each through its compensation delay.
    for (std::size_t k = 0; k < n.rackIns.size(); ++k) n.rackIns[k].clear(frames);
    for (std::size_t ei : n.in) {
        Edge& e = p.edges[ei];
        if (e.rackIn >= 0 && static_cast<std::size_t>(e.rackIn) < n.rackIns.size())
            addEdgeInto(p, e, n.rackIns[static_cast<std::size_t>(e.rackIn)], pos, frames);
    }
    // MIDI for the Notes modules: the timeline's while playing, a device's always.
    const bool playing = transport.playing;
    for (NotesFeed& feed : r.notes) {
        feed.block.clear();
        while (playing && feed.next < feed.events.size() &&
               feed.events[feed.next].sample < pos + static_cast<std::int64_t>(frames)) {
            const TimedEvent& te = feed.events[feed.next++];
            rack::host::MidiEvent m;
            const auto frame = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(te.sample - pos, 0, frames - 1));
            if (feed.block.size() < feed.block.capacity() && toMidi(te.event, frame, m))
                feed.block.push_back(m);
        }
        if (feed.device && !p.deviceMidi.empty()) {
            const std::size_t before = feed.block.size();
            for (const MidiInputEvent& ev : p.deviceMidi) {
                if (ev.frame < p.deviceMidiOffset) continue;
                if (ev.frame >= p.deviceMidiOffset + frames ||
                    feed.block.size() >= feed.block.capacity())
                    break;
                std::uint8_t m[3];
                if (!routeMidi(ev, feed.ports, 0, 0, m)) continue;
                feed.block.push_back({ev.frame - p.deviceMidiOffset, m[0], m[1], m[2]});
            }
            if (feed.block.size() > before)  // stable insertion sort by frame: no allocation
                for (std::size_t i = 1; i < feed.block.size(); ++i)
                    for (std::size_t j = i; j > 0 && feed.block[j - 1].frame > feed.block[j].frame;
                         --j)
                        std::swap(feed.block[j - 1], feed.block[j]);
        }
        feed.notes->bind(feed.block);
    }
    for (std::size_t k = 0; k < r.outs.size(); ++k) {
        n.rackOuts[k].clear(frames);
        r.outs[k]->bind(n.rackOuts[k].l.data(), n.rackOuts[k].r.data());
    }
    // The rack's grid follows the timeline at its input while playing; while
    // stopped it runs on from where it was, its clock and curves reading
    // where the transport stands.
    if (playing || !r.started) r.own = r.origin + pos - n.timelineShift;
    if (!playing) r.stoppedAt = pos - n.timelineShift;
    r.started = true;
    r.playing = playing;
    r.schedule->process(r.own, static_cast<int>(frames), r.clock);
    r.own += frames;
    for (const PluginModule* plugin : r.plugins)
        if (plugin->failed) return false;
    return true;
}

void PluginModule::process(const rack::HostContext& ctx) SUPPLE_RT {
    rack::Instance& in = *ctx.instance;
    script::Output& outL = in.outputMember(0);
    script::Output& outR = in.outputMember(1);
    outL.setChannels(1);
    outR.setChannels(1);
    if (stage < 0 || !plan || !node || failed) return;
    Stage& st = node->rackStages[static_cast<std::size_t>(stage)];
    const script::Input& l = in.inputMember(0);
    const script::Input& r = in.inputMember(1);
    const script::Input& pitch = in.inputMember(2);
    const script::Input& gate = in.inputMember(3);
    const script::Input& velocity = in.inputMember(4);
    // Audio: each input's channels summed; R is L while nothing is connected.
    for (int i = 0; i < ctx.frames; ++i) {
        double a = 0, b = 0;
        for (int c = 0; c < l.channels(); ++c) a += l[c][i];
        if (r.connected())
            for (int c = 0; c < r.channels(); ++c) b += r[c][i];
        else
            b = a;
        io.l[static_cast<std::size_t>(i)] = static_cast<float>(a);
        io.r[static_cast<std::size_t>(i)] = static_cast<float>(b);
    }
    // Notes: a voice's gate rising plays its pitch; falling (below 0.25) ends it.
    st.blockEvents.clear();
    const int voices = std::min(gate.channels(), rack::kChannels);
    for (int c = 0; c < voices; ++c) {
        Voice& v = voices_[static_cast<std::size_t>(c)];
        for (int i = 0; i < ctx.frames; ++i) {
            const double g = gate[c][i];
            const bool rise = !v.high && g >= 0.5, fall = v.high && g < 0.25;
            if (!rise && !fall) continue;
            if (st.blockEvents.size() + 2 > st.blockEvents.capacity()) break;
            v.high = rise;
            if (v.key >= 0) {
                host::Event off;
                off.kind = host::Event::Kind::NoteOff;
                off.time = static_cast<std::uint32_t>(i);
                off.key = static_cast<std::int16_t>(v.key);
                off.velocity = 0;
                st.blockEvents.push_back(off);
                v.key = -1;
            }
            if (rise) {
                host::Event on;
                on.kind = host::Event::Kind::NoteOn;
                on.time = static_cast<std::uint32_t>(i);
                on.key = static_cast<std::int16_t>(
                    std::clamp(static_cast<int>(std::lround(60.0 + 12.0 * pitch[c][i])), 0, 127));
                on.velocity = std::clamp(velocity[c][i], 0.0, 1.0);
                st.blockEvents.push_back(on);
                v.key = on.key;
            }
        }
    }
    // A gate cable gone: its notes end.
    for (int c = voices; c < rack::kChannels; ++c) {
        Voice& v = voices_[static_cast<std::size_t>(c)];
        v.high = false;
        if (v.key < 0 || st.blockEvents.size() + 1 > st.blockEvents.capacity()) continue;
        host::Event off;
        off.kind = host::Event::Kind::NoteOff;
        off.key = static_cast<std::int16_t>(v.key);
        off.velocity = 0;
        st.blockEvents.push_back(off);
        v.key = -1;
    }
    const auto frames = static_cast<std::uint32_t>(ctx.frames);
    if (!runModuleStage(*plan, st, io, frames, *transport, pos + ctx.offset, self)) {
        failed = true;
        return;
    }
    for (int i = 0; i < ctx.frames; ++i) {
        outL[0][i] = static_cast<double>(io.l[static_cast<std::size_t>(i)]);
        outR[0][i] = static_cast<double>(io.r[static_cast<std::size_t>(i)]);
    }
}

}  // namespace supple::engine::detail
