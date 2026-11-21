// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Route E stage 5: the plan dump (plan_dump.h).
//
// Each struct the dump walks is first decomposed into all its fields, by
// name, in declaration order. A field added to one of them stops this file
// compiling ("decomposes into N elements"): dump the new field below, or, if
// it is runtime state, only name it in the decomposition. The fields named
// there and never dumped are the runtime state the dump leaves out.
#include "plan_dump.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <ranges>
#include <type_traits>

#include "hardware_midi.h"
#include "plan.h"
#include "rack_runtime.h"

namespace supple::engine::detail {
namespace {

template <class T>
struct IsVector : std::false_type {};
template <class T, class A>
struct IsVector<std::vector<T, A>> : std::true_type {};
template <class T>
struct IsArray : std::false_type {};
template <class T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};
template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};

class Dump {
  public:
    std::string text;

    // A scalar, an id, a string, an optional or a list of them.
    template <class T>
    void put(std::string_view at, std::string_view key, const T& value) {
        text += at;
        text += ' ';
        text += key;
        text += ' ';
        write(value);
        text += '\n';
    }
    // The number of elements of a buffer or list whose contents are runtime
    // state but whose size compile or prepare decides.
    template <class T>
    void size(std::string_view at, std::string_view key, const T& container) {
        put(at, key, static_cast<std::uint64_t>(container.size()));
    }

  private:
    template <class T>
    void write(const T& value) {
        using V = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<V, bool>) {
            text += value ? "true" : "false";
        } else if constexpr (std::is_enum_v<V>) {
            write(static_cast<std::int64_t>(value));
        } else if constexpr (std::is_integral_v<V>) {
            char buf[32];
            const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
            text.append(buf, end);
        } else if constexpr (std::is_floating_point_v<V>) {
            // Shortest round trip: equal text, equal bits (NaNs aside).
            char buf[64];
            const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
            text.append(buf, end);
        } else if constexpr (std::is_same_v<V, Id>) {
            text += value.str();
        } else if constexpr (std::is_same_v<V, std::string> ||
                             std::is_same_v<V, std::string_view>) {
            text += '"';
            text += value;
            text += '"';
        } else if constexpr (IsOptional<V>::value) {
            if (value)
                write(*value);
            else
                text += "none";
        } else if constexpr (IsVector<V>::value || IsArray<V>::value || std::is_array_v<V>) {
            text += '[';
            bool first = true;
            using E = std::ranges::range_value_t<V>;
            for (const auto& e : value) {
                if (!first) text += ' ';
                first = false;
                // An element seen through a proxy (a vector<bool>'s, which
                // libc++ gives as __bit_const_reference) is written as its
                // value type.
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(e)>, E>)
                    write(e);
                else
                    write(static_cast<E>(e));
            }
            text += ']';
        } else {
            static_assert(sizeof(V) == 0, "no canonical text for this type");
        }
    }
};

std::string at(std::string_view a, std::string_view b) {
    std::string s(a);
    s += ' ';
    s += b;
    return s;
}
std::string at(std::string_view a, std::string_view b, std::size_t i) {
    return at(a, std::string(b) + std::to_string(i));
}

// Instances are named by what they run: an FX or a rack module.
struct Names {
    std::map<const host::Instance*, Id> fx;
    std::map<const rack::Instance*, Id> modules;
    template <class P>
    std::vector<std::string> list(const std::vector<P*>& pointers) const {
        std::vector<std::string> out;
        for (const P* p : pointers) {
            if constexpr (std::is_same_v<P, host::Instance>) {
                const auto found = fx.find(p);
                out.push_back(found == fx.end() ? "?" : found->second.str());
            } else {
                const auto found = modules.find(p);
                out.push_back(found == modules.end() ? "?" : found->second.str());
            }
        }
        std::sort(out.begin(), out.end());  // the plans sort them by address
        return out;
    }
};

void dumpAutomation(Dump& d, std::string_view a, const Automation& x) {
    [[maybe_unused]] const auto& [points, cursor, timeline, deviceShift] = x;
    if (!x.active() && !x.timeline && x.deviceShift == 0) return;  // unused: one line less
    d.put(a, "points", static_cast<std::uint64_t>(x.points.size()));
    for (std::size_t i = 0; i < x.points.size(); ++i) {
        const auto& p = x.points[i];
        const auto pa = at(a, "p", i);
        d.put(pa, "sample", p.sample);
        d.put(pa, "value", p.value);
        d.put(pa, "shape", p.shape);
        d.put(pa, "tension", p.tension);
    }
    d.put(a, "timeline", x.timeline != nullptr);
    d.put(a, "deviceShift", x.deviceShift);
}

void dumpVcas(Dump& d, std::string_view a, const VcaControls& x) {
    for (std::size_t i = 0; i < x.values.size(); ++i) {
        const VcaControl& v = x.values[i];
        [[maybe_unused]] const auto& [volumeDb, mute, volume, gate, slot, hold] = v;
        const auto va = at(a, "vca", i);
        d.put(va, "volumeDb", v.volumeDb);
        d.put(va, "mute", v.mute);
        dumpAutomation(d, at(va, "volume"), v.volume);
        dumpAutomation(d, at(va, "gate"), v.gate);
        d.put(va, "slot", v.slot);
        d.put(va, "hold", v.hold);
    }
}

void dumpPan(Dump& d, std::string_view a, const PanSettings& x) {
    d.put(a, "mode", x.mode);
    d.put(a, "pan", x.pan);
    d.put(a, "width", x.width);
    d.put(a, "left", x.left);
    d.put(a, "right", x.right);
    d.put(a, "lawDb", x.law.db);
    d.put(a, "lawBoost", x.law.boost);
}

template <class S>
void dumpConsole(Dump& d, std::string_view a, const ConsoleStage<S>& x) {
    d.put(a, "seeds", std::array{x.seeds.left, x.seeds.right});
    d.put(a, "linear", x.linear);
}

void dumpEvent(Dump& d, std::string_view a, const host::Event& e) {
    d.put(a, "kind", e.kind);
    d.put(a, "time", e.time);
    d.put(a, "note", std::array<std::int64_t, 4>{e.port, e.channel, e.key, e.noteId});
    d.put(a, "velocity", e.velocity);
    d.put(a, "param", e.paramId);
    d.put(a, "value", e.value);
    d.put(a, "midi", std::array<int, 3>{e.midi[0], e.midi[1], e.midi[2]});
}

void dumpOrigin(Dump& d, std::string_view a, const NoteOrigin& o) {
    d.put(a, "origin",
          std::array<std::int64_t, 5>{static_cast<std::int64_t>(o.item),
                                      static_cast<std::int64_t>(o.take),
                                      static_cast<std::int64_t>(o.source), o.tick, o.pass});
    if (o.followed) d.put(a, "followed", true);  // a following take's (ADR-0098 X4c)
}

void dumpEvents(Dump& d, std::string_view a, const std::vector<TimedEvent>& events) {
    d.put(a, "events", static_cast<std::uint64_t>(events.size()));
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto ea = at(a, "e", i);
        d.put(ea, "sample", events[i].sample);
        dumpEvent(d, ea, events[i].event);
        dumpOrigin(d, ea, events[i].origin);
    }
}

void dumpStage(Dump& d, std::string_view a, const Stage& st) {
    [[maybe_unused]] const auto& [rackMidiBase, busPart, busMerge, busLive, midiOnly, busBase,
                                  busHeld, busControllers, instance, fx, lanes, laneModRoutes,
                                  laneSlot, mods, modRoutes, modEvents, modHeard, modEpoch, bypass,
                                  instrument, autoBypassOnSilence, sleeping, processed,
                                  inputLatency, rateLive, callbackBypass, wet, appliedWet,
                                  rampFromWet, rampToWet, wetRampLeft, preserveDryOnLocate, latency,
                                  dry, out, dryBuf, keyed, keyChannels, key, inPin, outPin, keyPins,
                                  pinned, chanDelay, pinBuf, auxOutPin, auxOut, auxOutPtr,
                                  auxWritten, blockEvents, voices, pendingVoices, noteSources,
                                  noteSourceIndex, sourceIds, nextNoteId, releases, voiceDropped,
                                  modDeferred, outStorage, outEvents, blocks, openGesture,
                                  openGestures, pdcIgnore, parallel, group, groupEnd, before, align,
                                  groupIn, groupOut, os, osFilters, osIn, osOut, osKey, osPad] = st;
    d.put(a, "fx", st.fx);
    d.put(a, "instance", st.instance != nullptr);
    d.put(a, "rackMidiBase", st.rackMidiBase);
    d.put(a, "busPart", st.busPart);
    d.put(a, "busMerge", st.busMerge);
    d.put(a, "midiOnly", st.midiOnly);
    d.put(a, "busBase", st.busBase);
    d.size(a, "busHeld", st.busHeld);
    d.put(a, "lanes", static_cast<std::uint64_t>(st.lanes.size()));
    for (std::size_t i = 0; i < st.lanes.size(); ++i) {
        const ParamLane& lane = st.lanes[i];
        const auto la = at(a, "lane", i);
        d.put(la, "clap", lane.clapId);
        d.put(la, "described", lane.spec != nullptr);
        dumpAutomation(d, at(la, "curve"), lane.curve);
        // Routes it carries (M4); the held sum is runtime state.
        if (lane.modCount) {
            d.put(la, "modRoutes", std::array{lane.modFirst, lane.modCount});
            d.put(la, "modRange", lane.modRange);
            d.put(la, "range", std::array{lane.lo, lane.hi});
            d.put(la, "telemetry", lane.telemetry);
        }
        // Its write hold's inputs (ADR-0061, amended); the hold is runtime state.
        d.put(la, "envelope", lane.envelope);
        d.put(la, "writes", std::array{lane.writes, lane.latches});
    }
    d.size(a, "laneModRoutes", st.laneModRoutes);
    d.put(a, "laneSlot", st.laneSlot);
    // Modulation slots; the memo in them is runtime state.
    d.put(a, "mods", static_cast<std::uint64_t>(st.mods.size()));
    for (std::size_t i = 0; i < st.mods.size(); ++i) {
        const ModParam& m = st.mods[i];
        const auto ma = at(a, "mod", i);
        d.put(ma, "clap", m.clapId);
        d.put(ma, "range", m.range);
        d.put(ma, "routes", std::array{m.first, m.count});
    }
    d.put(a, "modRoutes", st.modRoutes);
    d.put(a, "modEvents", static_cast<std::uint64_t>(st.modEvents));
    d.put(a, "bypass", st.bypass);
    d.put(a, "instrument", st.instrument);
    d.put(a, "autoBypassOnSilence", st.autoBypassOnSilence);
    d.put(a, "inputLatency", st.inputLatency);
    d.put(a, "rateLive", st.rateLive);
    d.put(a, "callbackBypass", st.callbackBypass);
    d.put(a, "wet", st.wet);
    d.put(a, "latency", st.latency);
    d.put(a, "dry", st.dry.length);
    d.size(a, "out", st.out.l);
    d.size(a, "dryBuf", st.dryBuf.l);
    d.put(a, "keyed", st.keyed);
    d.put(a, "keyChannels", st.keyChannels);
    d.size(a, "key", st.key.l);
    d.put(a, "inPin", st.inPin);
    d.put(a, "outPin", st.outPin);
    d.put(a, "keyPins", st.keyPins);
    d.put(a, "pinned", st.pinned);
    std::vector<std::int64_t> chanDelays;
    for (const MonoDelay& m : st.chanDelay) chanDelays.push_back(m.length);
    d.put(a, "chanDelay", chanDelays);
    d.size(a, "pinBuf", st.pinBuf.l);
    d.put(a, "auxOutPin", st.auxOutPin);
    std::vector<std::uint64_t> auxSizes;
    for (const auto& b : st.auxOut) auxSizes.push_back(b.size());
    d.put(a, "auxOut", auxSizes);
    d.size(a, "voices", st.voices);
    d.size(a, "pendingVoices", st.pendingVoices);
    const auto dumpSources = [&](std::string_view which, const std::vector<NoteSource>& sources) {
        d.put(a, which, static_cast<std::uint64_t>(sources.size()));
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const NoteSource& s = sources[i];
            const auto sa = at(a, which, i);
            dumpOrigin(d, sa, s.origin);
            d.put(sa, "span", std::array{s.start, s.end});
            d.put(sa, "note", std::array<std::int64_t, 3>{s.port, s.channel, s.key});
            d.put(sa, "ordinal", s.ordinal);
            d.put(sa, "id", s.id);
        }
    };
    dumpSources("noteSources", st.noteSources);
    dumpSources("noteSourceIndex", st.noteSourceIndex);
    d.size(a, "outStorage", st.outStorage);
    d.put(a, "pdcIgnore", st.pdcIgnore);
    d.put(a, "parallel", st.parallel);
    d.put(a, "group", st.group);
    d.put(a, "groupEnd", st.groupEnd);
    d.put(a, "before", st.before);
    d.put(a, "align", st.align.length);
    d.size(a, "groupIn", st.groupIn.l);
    d.size(a, "groupOut", st.groupOut.l);
    d.put(a, "os", st.os);
    d.size(a, "osFilters", st.osFilters);
    d.put(a, "osBuffers",
          std::array<std::uint64_t, 3>{st.osIn[0].size(), st.osOut[0].size(), st.osKey[0].size()});
    d.put(a, "osPad", st.osPad.length);
}

void dumpStages(Dump& d, std::string_view a, std::string_view name,
                const std::vector<Stage>& stages) {
    for (std::size_t i = 0; i < stages.size(); ++i) dumpStage(d, at(a, name, i), stages[i]);
}

void dumpTakeChain(Dump& d, std::string_view a, const TakeChain& tc) {
    [[maybe_unused]] const auto& [item, take, start, end, latency, clip, events, nextEvent,
                                  aheadFrom, aheadLoop, rateEventFresh, noteTarget, held,
                                  releaseHeld, chain, gainL, gainR, fadeIn, fadeOut, fadeInShape,
                                  fadeOutShape, fadeInCurve, fadeOutCurve, volume, pan, mute, buf,
                                  active, clipChain, player, delay, delayAt, inputPlayer,
                                  inputAnchor, clipReader] = tc;
    d.put(a, "item", tc.item);
    d.put(a, "take", tc.take);
    d.put(a, "span", std::array{tc.start, tc.end});
    d.put(a, "latency", tc.latency);
    d.put(a, "clip", tc.clip);
    dumpEvents(d, a, tc.events);
    d.put(a, "noteTarget", tc.noteTarget);
    d.put(a, "clipReader", tc.clipReader.state != nullptr);
    dumpStages(d, a, "chain", tc.chain);
    d.put(a, "gain", std::array{tc.gainL, tc.gainR});
    d.put(a, "fades", std::array{tc.fadeIn, tc.fadeOut});
    d.put(a, "fadeShapes", std::array{tc.fadeInShape, tc.fadeOutShape});
    d.put(a, "fadeCurves", std::array{tc.fadeInCurve, tc.fadeOutCurve});
    dumpAutomation(d, at(a, "volume"), tc.volume);
    dumpAutomation(d, at(a, "pan"), tc.pan);
    dumpAutomation(d, at(a, "mute"), tc.mute);
    d.size(a, "buf", tc.buf.l);
    // A clip's chain: its player and its delay back to the switch; what its
    // input last read is state.
    d.put(a, "clipChain", std::array{tc.clipChain ? 1 : 0, tc.player});
    d.size(a, "delay", tc.delay.l);
}

void dumpClip(Dump& d, std::string_view a, const Clip& c) {
    [[maybe_unused]] const auto& [item, take, rateState, data, start, length, deviceShift, offset,
                                  loop, mode, gainL, gainR, fadeIn, fadeOut, fadeInShape,
                                  fadeOutShape, fadeInCurve, fadeOutCurve, takeChain] = c;
    d.put(a, "item", c.item);
    d.put(a, "take", c.take);
    d.put(a, "rateState", c.rateState != nullptr);
    d.put(a, "data",
          c.data ? std::array<std::uint64_t, 2>{c.data->channels.size(), c.data->frames()}
                 : std::array<std::uint64_t, 2>{});
    d.put(a, "start", c.start);
    d.put(a, "length", c.length);
    d.put(a, "deviceShift", c.deviceShift);
    d.put(a, "offset", c.offset);
    d.put(a, "loop", c.loop);
    d.put(a, "mode", c.mode);
    d.put(a, "gain", std::array{c.gainL, c.gainR});
    d.put(a, "fades", std::array{c.fadeIn, c.fadeOut});
    d.put(a, "fadeShapes", std::array{c.fadeInShape, c.fadeOutShape});
    d.put(a, "fadeCurves", std::array{c.fadeInCurve, c.fadeOutCurve});
    d.put(a, "takeChain", c.takeChain);
}

void dumpRack(Dump& d, std::string_view a, const RackNode& r) {
    [[maybe_unused]] const auto& [rack, modules, instances, schedule, outs, outIds, outLatencies,
                                  ins, inIds, modOuts, modOutIds, modOutLatencies, modValues,
                                  modHeld, modStale, notes, notesOut, plugins, curves, clock,
                                  origin, midiEpoch, playing, started, own, stoppedAt, live,
                                  resetNotes, resetModules] = r;
    d.put(a, "rack", r.rack);
    d.put(a, "modules", r.modules);
    d.put(a, "instances", static_cast<std::uint64_t>(r.instances.size()));
    d.put(a, "schedule", r.schedule != nullptr);
    d.put(a, "outs", r.outIds);
    d.put(a, "outLatencies", r.outLatencies);
    d.put(a, "ins", r.inIds);
    // Mod Outs (ADR-0096); their values and held values are runtime state.
    d.put(a, "modOuts", r.modOutIds);
    d.put(a, "modOutLatencies", r.modOutLatencies);
    for (std::size_t i = 0; i < r.modValues.size(); ++i)
        if (!r.modValues[i].empty()) d.size(at(a, "modValues", i), "frames", r.modValues[i]);
    for (std::size_t i = 0; i < r.notes.size(); ++i) {
        const NotesFeed& f = r.notes[i];
        [[maybe_unused]] const auto& [fnotes, id, routes, source, events, next, midiStage,
                                      rateFresh, block, sortScratch, order, device, ports,
                                      sourceDevice, sourcePorts, fromChannel, toChannel, sourceAuto,
                                      liveGeneration, launch, players, clipEvents, follows,
                                      releaseHeld, launchFresh] = f;
        const auto fa = at(a, "notes", i);
        d.put(fa, "id", f.id);
        d.put(fa, "routes", f.routes);
        d.put(fa, "source", f.source);
        dumpEvents(d, fa, f.events);
        dumpStage(d, at(fa, "midiStage"), f.midiStage);
        d.size(fa, "block", f.block);
        d.put(fa, "device", f.device);
        d.put(fa, "ports", f.ports);
        d.put(fa, "sourceDevice", f.sourceDevice);
        d.put(fa, "sourcePorts", f.sourcePorts);
        d.put(fa, "channels", std::array{f.fromChannel, f.toChannel});
        d.put(fa, "sourceAuto", f.sourceAuto);
        d.put(fa, "liveGeneration", f.liveGeneration);
        // Its source track's launches (ADR-0097): the compiled MIDI clips;
        // what plays is state.
        d.put(fa, "follows", f.follows);
        for (std::size_t k = 0; k < f.players.size(); ++k) {
            const ClipPlayer& pl = f.players[k];
            const auto pa = at(fa, "player" + std::to_string(k));
            d.put(pa, "clip", pl.clip);
            d.put(pa, "flags", std::array{pl.once, pl.muted});
            d.put(pa, "beats", pl.periodBeats);
            d.put(pa, "events", static_cast<std::uint64_t>(pl.events.size()));
        }
        d.size(fa, "clipEvents", f.clipEvents);
    }
    for (std::size_t i = 0; i < r.notesOut.size(); ++i) {
        const NotesOutBinding& b = r.notesOut[i];
        [[maybe_unused]] const auto& [module, id, events, before, count, latency] = b;
        const auto ba = at(a, "notesOut", i);
        d.put(ba, "id", b.id);
        d.size(ba, "events", b.events);
        d.put(ba, "latency", b.latency);
    }
    for (std::size_t i = 0; i < r.plugins.size(); ++i) {
        const PluginBinding& b = r.plugins[i];
        [[maybe_unused]] const auto& [module, stage, io, mods, modCount] = b;
        const auto ba = at(a, "plugin", i);
        d.put(ba, "stage", b.stage);
        d.size(ba, "io", b.io.l);
        d.put(ba, "mods", b.modCount);
        for (int m = 0; m < b.modCount; ++m) {
            const auto& mod = b.mods[static_cast<std::size_t>(m)];
            d.put(at(ba, "mod", static_cast<std::size_t>(m)), "target",
                  std::array<double, 3>{double(mod.slot), double(mod.param), mod.scale});
        }
    }
    for (std::size_t i = 0; i < r.curves.size(); ++i)
        dumpAutomation(d, at(a, "curve", i), r.curves[i]->curve);
    d.put(a, "origin", r.origin);
    d.put(a, "live", r.live);
}

void dumpNode(Dump& d, std::string_view a, const Node& n) {
    [[maybe_unused]] const auto& [routedMidi, midiStage, midiPreFx, midiPostFx, midiSortScratch,
                                  midiPreTimeline, midiPostTimeline, midiInstrument, midiChain,
                                  midiBus, midiBusNext, midiBusGen, midiBusStages, midiBusOn,
                                  midiBusVoices, midiPostHeld, midiPostBefore, midiBusRemap,
                                  midiOldPool, midiPostRate, midiPostWindow, midiOrder, midiHeld,
                                  midiBefore, midiFree, midiInput, midiRouted, midiRemap,
                                  midiLiveBase, midiRoutedBase, midiTakeSources, midiEpoch,
                                  midiDropped, midiEventRate, midiWindow, midiReleaseTimeline,
                                  midiSends, hardwareMidiSource, id, inBySource, rack, rackOuts,
                                  rackIns, rackStages, freeze, frozenAudio, auxCount, aux, in, out,
                                  modIn, mixMods, modVolume, modPan, modWidth, chain, inputChain,
                                  recordInputs, recordRack, inputLatency, inputBuf, latIn, latChain,
                                  timelineShift, sum, preFx, preFader, post, postKey, events,
                                  nextEvent, rateEventFresh, held, releaseHeld, releaseLive, clips,
                                  takeChains, players, retiring, launch, launchLead, launchNext,
                                  launchNextValid, launchFresh, launchFadeFrames, arrangementGain,
                                  arrangementGains, launchBuf, launchReaders, clipVoices,
                                  clipWindow, postItems, itemShift, items, mix, silentFrom,
                                  silentTo, dropItems, noteTarget, volumeDb, pan, staticGain,
                                  panSettings, panCache, type, mute, master, decodeInput,
                                  phaseInvert, mono, utility, soloSilenced, postKeyed, stageSlot,
                                  decoder, encoder, fader, keyEncoder, keyFader, monitorInputs,
                                  tapeAuto, midiIn, liveHeld, live, autoVolume, autoPan, autoMute,
                                  autoWidth, autoPanLeft, autoPanRight, autoPanBoost, holdWidth,
                                  holdPanLeft, holdPanRight, holdPanBoost, holdVolume, holdPan,
                                  vcas, auxPostGain, meterSlot, meterGeneration, meterPeak,
                                  meterSumSq] = n;
    d.put(a, "id", n.id);
    d.put(a, "type", n.type);
    d.put(a, "master", n.master);
    // Track MIDI (ADR-0067, ADR-0068): the compiled sizes and bounds.
    d.size(a, "routedMidi", n.routedMidi);
    dumpStage(d, at(a, "midiStage"), n.midiStage);
    d.put(a, "midiBusStages", n.midiBusStages);
    d.put(a, "midiBusOn", n.midiBusOn);
    d.put(a, "midiBusVoices", n.midiBusVoices);
    d.size(a, "midiPostHeld", n.midiPostHeld);
    d.put(a, "midiPost", std::array<std::uint64_t, 2>{n.midiPostRate, n.midiPostWindow});
    d.size(a, "midiHeld", n.midiHeld);
    d.size(a, "midiInput", n.midiInput);
    d.size(a, "midiRouted", n.midiRouted);
    d.put(a, "midiBases", std::array{n.midiLiveBase, n.midiRoutedBase});
    // The clip launcher (ADR-0097): the compiled players; what plays is state.
    d.put(a, "players", static_cast<std::uint64_t>(n.players.size()));
    d.put(a, "retiring", static_cast<std::uint64_t>(n.retiring.size()));
    for (std::size_t k = 0; k < n.players.size(); ++k) {
        const ClipPlayer& pl = n.players[k];
        const auto pa = at(a, "player" + std::to_string(k));
        d.put(pa, "clip", pl.clip);
        d.put(pa, "flags", std::array{pl.midi, pl.once, pl.muted, pl.loopOn});
        d.put(pa, "frames",
              std::array{pl.periodFrames, pl.loopStartFrames, pl.loopFrames, pl.audio.offset});
        d.put(pa, "beats", pl.periodBeats);
        d.put(pa, "events", static_cast<std::uint64_t>(pl.events.size()));
        d.put(pa, "audio", pl.audio.data != nullptr);
        d.put(pa, "chain", pl.chain);
        // A following clip's tables (ADR-0098 X4): key only, regions, pairs.
        if (pl.follow)
            d.put(pa, "follow",
                  std::array<std::uint64_t, 3>{pl.follow->keyOnly, pl.follow->clip.place.size(),
                                               pl.follow->rows.size()});
    }
    // Its clip chains' lead (ADR-0097, amended 2026-11-20); launchNext is state.
    d.put(a, "launchLead", n.launchLead);
    // Its readers on the playrate path (S3): whether the plan made them.
    d.put(a, "launchReaders", n.launchReaders[0].state != nullptr);
    d.put(a, "clipSizing",
          std::array<std::uint64_t, 3>{static_cast<std::uint64_t>(n.launchFadeFrames), n.clipVoices,
                                       n.clipWindow});
    d.put(a, "midiTakeSources", n.midiTakeSources);
    d.put(a, "midiRate", std::array<std::uint64_t, 2>{n.midiEventRate, n.midiWindow});
    d.put(a, "midiSends", n.midiSends);
    d.put(a, "hardwareMidiSource", n.hardwareMidiSource);
    d.put(a, "inBySource", n.inBySource);
    if (n.rack) dumpRack(d, at(a, "rack"), *n.rack);
    d.size(a, "rackOuts", n.rackOuts);
    d.size(a, "rackIns", n.rackIns);
    dumpStages(d, a, "rackStage", n.rackStages);
    if (n.freeze) {
        d.put(a, "freeze", n.freeze->source);
        d.put(a, "freezeFrames",
              std::array{n.freeze->contentFrames, n.freeze->tailFrames, n.freeze->latencyFrames});
        d.put(a, "freezeHash", n.freeze->dependencyHash);
    }
    d.put(a, "frozenAudio", n.frozenAudio ? n.frozenAudio->frames() : 0);
    d.put(a, "auxCount", n.auxCount);
    d.size(a, "aux", n.aux);
    d.put(a, "in", n.in);
    d.put(a, "out", n.out);
    d.put(a, "modIn", n.modIn);
    // Mixer targets (ADR-0096, M-i): what each sums; its state is the block's.
    d.put(a, "modTargets", std::array{n.modVolume, n.modPan, n.modWidth});
    for (std::size_t i = 0; i < n.mixMods.size(); ++i) {
        const auto& mm = n.mixMods[i];
        const std::string m = at(a, "mixMod", i);
        d.put(m, "object", mm.object);
        d.put(m, "param", mm.param);
        d.put(m, "routes", mm.routes);
        d.size(m, "offsets", mm.offsets);
    }
    dumpStages(d, a, "chain", n.chain);
    dumpStages(d, a, "input", n.inputChain);
    d.put(a, "recordInputs", n.recordInputs);
    d.put(a, "recordRack", n.recordRack);
    d.put(a, "inputLatency", n.inputLatency);
    d.size(a, "inputBuf", n.inputBuf.l);
    d.put(a, "latIn", n.latIn);
    d.put(a, "latChain", n.latChain);
    d.put(a, "timelineShift", n.timelineShift);
    d.put(a, "buffers",
          std::array<std::uint64_t, 7>{n.sum.l.size(), n.preFx.l.size(), n.preFader.l.size(),
                                       n.post.l.size(), n.postKey.l.size(), n.items.l.size(),
                                       n.mix.l.size()});
    dumpEvents(d, a, n.events);
    for (std::size_t i = 0; i < n.clips.size(); ++i) dumpClip(d, at(a, "clip", i), n.clips[i]);
    for (std::size_t i = 0; i < n.takeChains.size(); ++i)
        dumpTakeChain(d, at(a, "take", i), n.takeChains[i]);
    d.put(a, "postItems", n.postItems);
    d.put(a, "itemShift", n.itemShift);
    d.put(a, "silent", std::array{n.silentFrom, n.silentTo});
    d.put(a, "dropItems", n.dropItems);
    d.put(a, "noteTarget", n.noteTarget);
    d.put(a, "volumeDb", n.volumeDb);
    d.put(a, "pan", n.pan);
    d.put(a, "staticGain", n.staticGain);
    dumpPan(d, at(a, "panSettings"), n.panSettings);
    d.put(a, "mute", n.mute);
    d.put(a, "decodeInput", n.decodeInput);
    d.put(a, "phaseInvert", n.phaseInvert);
    d.put(a, "mono", n.mono);
    d.put(a, "soloSilenced", n.soloSilenced);
    d.put(a, "postKeyed", n.postKeyed);
    d.put(a, "stageSlot", n.stageSlot);
    dumpConsole(d, at(a, "decoder"), n.decoder);
    dumpConsole(d, at(a, "encoder"), n.encoder);
    dumpConsole(d, at(a, "keyEncoder"), n.keyEncoder);
    d.put(a, "monitorInputs", n.monitorInputs);
    d.put(a, "tapeAuto", n.tapeAuto);
    if (n.midiIn) {
        d.put(a, "midiIn", n.midiIn->device);
        d.put(a, "midiInChannels", std::array{n.midiIn->fromChannel, n.midiIn->toChannel});
        d.put(a, "midiInPorts", n.midiIn->ports);
    }
    d.put(a, "live", n.live);
    dumpAutomation(d, at(a, "autoVolume"), n.autoVolume);
    dumpAutomation(d, at(a, "autoPan"), n.autoPan);
    dumpAutomation(d, at(a, "autoMute"), n.autoMute);
    dumpAutomation(d, at(a, "autoWidth"), n.autoWidth);
    dumpAutomation(d, at(a, "autoPanLeft"), n.autoPanLeft);
    dumpAutomation(d, at(a, "autoPanRight"), n.autoPanRight);
    dumpAutomation(d, at(a, "autoPanBoost"), n.autoPanBoost);
    d.put(a, "holds",
          std::array{n.holdVolume, n.holdPan, n.holdWidth, n.holdPanLeft, n.holdPanRight,
                     n.holdPanBoost});
    dumpVcas(d, a, n.vcas);
    d.size(a, "auxPostGain", n.auxPostGain);
    d.put(a, "meterSlot", n.meterSlot);
}

// A modulation edge (ADR-0096); its delay's contents are runtime state.
void dumpModEdge(Dump& d, std::string_view a, const ModEdge& e) {
    [[maybe_unused]] const auto& [route, from, to, fromId, toId, modOut, input, target, stage,
                                  inputStage, clapId, viaLane, depth, enabled, inert, tap, delay,
                                  preserveDelayOnLocate, values] = e;
    d.put(a, "route", e.route);
    d.put(a, "from", static_cast<std::uint64_t>(e.from));
    d.put(a, "to", static_cast<std::uint64_t>(e.to));
    d.put(a, "ids", std::array{e.fromId, e.toId});
    d.put(a, "source", std::array{e.modOut, e.input});
    d.put(a, "target", e.target.object.str() + " " + e.target.param);
    d.put(a, "stage", std::array{e.stage, e.inputStage});
    d.put(a, "depth", e.depth);
    d.put(a, "enabled", e.enabled);
    d.put(a, "inert", e.inert);
    d.put(a, "tap", e.tap);
    d.put(a, "delay", e.delay.length);
    d.put(a, "clap", e.clapId);
    d.put(a, "viaLane", e.viaLane);
    d.size(a, "values", e.values);
}

void dumpEdge(Dump& d, std::string_view a, const Edge& e) {
    [[maybe_unused]] const auto& [from, to, fromId, route, stage, tap, gainL, gainR, volumeDb, pan,
                                  staticGain, source, panCache, mono, invert, mapped, map,
                                  autoVolume, autoPan, autoMute, vcas, stripWidth, stripPanLeft,
                                  stripPanRight, stripPanBoost, stripVolume, stripPan, stripMute,
                                  encode, followPan, muted, holdVolume, holdPan, holdStripVolume,
                                  holdStripPan, holdStripWidth, holdStripPanLeft, holdStripPanRight,
                                  holdStripPanBoost, encoder, fader, encoded, delay,
                                  preserveDelayOnLocate, tmp, auxLinks, auxOnly, auxTmp, rackIn,
                                  rackOut, rackLatency, rackPadding, rackSend, homeMute,
                                  homeSilenced, rackHome, modVolume, midi, rackNotes, midiNotes,
                                  midiFrom, midiTo, midiSourceBase, midiQueue, midiHead, midiCount,
                                  midiVoices, midiAddresses, midiReconcile, midiNextVoice,
                                  midiEpoch, midiStarted, midiRelease, midiMuted, frontier, ring,
                                  frontierBuf] = e;
    d.put(a, "from", static_cast<std::uint64_t>(e.from));
    d.put(a, "to", static_cast<std::uint64_t>(e.to));
    d.put(a, "fromId", e.fromId);
    d.put(a, "route", e.route);
    d.put(a, "stage", e.stage);
    d.put(a, "tap", e.tap);
    d.put(a, "gain", std::array{e.gainL, e.gainR});
    d.put(a, "volumeDb", e.volumeDb);
    d.put(a, "pan", e.pan);
    d.put(a, "staticGain", e.staticGain);
    dumpPan(d, at(a, "source"), e.source);
    d.put(a, "mono", e.mono);
    d.put(a, "invert", e.invert);
    d.put(a, "mapped", e.mapped);
    d.put(a, "map", std::array{e.map[0][0], e.map[0][1], e.map[1][0], e.map[1][1]});
    dumpAutomation(d, at(a, "autoVolume"), e.autoVolume);
    dumpAutomation(d, at(a, "autoPan"), e.autoPan);
    dumpAutomation(d, at(a, "autoMute"), e.autoMute);
    dumpVcas(d, a, e.vcas);
    dumpAutomation(d, at(a, "stripWidth"), e.stripWidth);
    dumpAutomation(d, at(a, "stripPanLeft"), e.stripPanLeft);
    dumpAutomation(d, at(a, "stripPanRight"), e.stripPanRight);
    dumpAutomation(d, at(a, "stripPanBoost"), e.stripPanBoost);
    dumpAutomation(d, at(a, "stripVolume"), e.stripVolume);
    dumpAutomation(d, at(a, "stripPan"), e.stripPan);
    dumpAutomation(d, at(a, "stripMute"), e.stripMute);
    d.put(a, "encode", e.encode);
    d.put(a, "followPan", e.followPan);
    d.put(a, "muted", e.muted);
    d.put(a, "holds",
          std::array{e.holdVolume, e.holdPan, e.holdStripVolume, e.holdStripPan, e.holdStripWidth,
                     e.holdStripPanLeft, e.holdStripPanRight, e.holdStripPanBoost});
    dumpConsole(d, at(a, "encoder"), e.encoder);
    d.size(a, "encoded", e.encoded.l);
    d.put(a, "delay", e.delay.length);
    d.size(a, "tmp", e.tmp.l);
    for (std::size_t i = 0; i < e.auxLinks.size(); ++i) {
        const Edge::AuxLink& l = e.auxLinks[i];
        [[maybe_unused]] const auto& [lfrom, lto, ldelay, lpreserve] = l;
        d.put(at(a, "aux", i), "link", std::array<std::int64_t, 3>{l.from, l.to, l.delay.length});
    }
    d.put(a, "auxOnly", e.auxOnly);
    d.size(a, "auxTmp", e.auxTmp);
    d.put(a, "rack",
          std::array<std::int64_t, 4>{e.rackIn, e.rackOut, e.rackLatency, e.rackPadding});
    d.put(a, "rackSend", e.rackSend);
    d.put(a, "home", std::array{e.homeMute, e.homeSilenced});
    d.put(a, "rackHome", e.rackHome);
    d.put(a, "modVolume", e.modVolume);
    d.put(a, "midi", e.midi);
    d.put(a, "midiRoute",
          std::array{e.rackNotes, e.midiNotes, e.midiFrom, e.midiTo, e.midiSourceBase});
    d.size(a, "midiQueue", e.midiQueue);
    d.size(a, "midiVoices", e.midiVoices);
    d.put(a, "frontier", e.frontier);
    d.put(a, "ring", static_cast<std::uint64_t>(e.ring));
    d.size(a, "frontierBuf", e.frontierBuf.l);
}

void dumpGraph(Dump& d, std::string_view a, const TaskGraph* g) {
    if (!g) {
        d.put(a, "graph", false);
        return;
    }
    d.put(a, "preds", g->preds);
    for (std::size_t i = 0; i < g->succ.size(); ++i) d.put(at(a, "succ", i), "tasks", g->succ[i]);
    d.put(a, "parallelism", g->parallelism);
}

void dumpSlot(Dump& d, std::string_view a, const MixSlots::Value& v) {
    constexpr auto r = std::memory_order_relaxed;
    d.put(a, "levels",
          std::array{v.volumeDb.load(r), v.pan.load(r), v.width.load(r), v.panLeft.load(r),
                     v.panRight.load(r)});
    d.put(a, "flags",
          std::array{v.mute.load(r), v.soloSilenced.load(r), v.holdVolume.load(r),
                     v.holdPan.load(r), v.holdWidth.load(r), v.holdPanLeft.load(r),
                     v.holdPanRight.load(r), v.holdPanBoost.load(r)});
}

}  // namespace

std::string dumpPlan(const Plan& p) {
    [[maybe_unused]] const auto& [pool, buffers, sessionVersion, generation, nodes, mainMaster,
                                  outputs, edges, modEdges, modOffsets, modOffsetOf, nodeIndex,
                                  linearConsole, masterPlayrate, rateEnvelope, rateEnabled,
                                  rateStretch, syncPlan, syncRate, timelines, previousTimelines,
                                  rateControl, preparedRateBoundary, rateHandDevice,
                                  rateSeekSequence, rateEpochSeekSequence, rateFromGeneration,
                                  rateFromUnity, ratePrepared, rateOrigin, rateDeviceMargin,
                                  rateBackendLatency, rateSourceAhead, rateSourceBehind,
                                  rateSwitchDevice, rateSafeDevice, rateControlsChanged,
                                  scalarPublication, scalarHistory, requestedScalar, rate,
                                  smoothFrames, mix, startSeconds, outputLatency, block, timeMap,
                                  warnings, clipPlayers, follow, gestureHolds, projectDir,
                                  preparedSources, media, instances, graph, anyLive, heldLive,
                                  rendererNodes, liveNodes, rendererGraph, liveGraph,
                                  rendererInstances, liveInstances, modules, rendererModules,
                                  liveModules, rendererMeterNodes, liveMeterNodes, meterNodes,
                                  frontier, rateOutput, frontierRings, recordTaps, recordRings,
                                  handAt, handEpoch, handContinuable, split, deviceIn,
                                  deviceInChannels, deviceRecording, hardwareMidi,
                                  midiInputsSettings, midiOutputsSettings, hardwareLead,
                                  hardwareLeadBase, deviceMidi, deviceMidiOffset, processedInputs] =
        p;
    Dump d;
    Names names;
    for (const Node& n : p.nodes) {
        forEachStage(n, [&](const Stage& st) {
            if (st.instance) names.fx.emplace(st.instance, st.fx);
        });
        if (n.rack)
            for (std::size_t i = 0; i < n.rack->instances.size() && i < n.rack->modules.size(); ++i)
                names.modules.emplace(n.rack->instances[i], n.rack->modules[i]);
    }
    const std::string_view a = "plan";
    d.put(a, "sessionVersion", p.sessionVersion);
    d.put(a, "rate", p.rate);
    d.put(a, "block", p.block);
    d.put(a, "smoothFrames", p.smoothFrames);
    d.put(a, "startSeconds", p.startSeconds);
    d.put(a, "outputLatency", p.outputLatency);
    d.put(a, "linearConsole", p.linearConsole);
    d.put(a, "playrate", p.masterPlayrate.rate);
    d.put(a, "preservePitch", p.masterPlayrate.preservePitch);
    dumpAutomation(d, "plan rateEnvelope", p.rateEnvelope);
    d.put(a, "rateEnabled", std::array{p.rateEnabled, p.rateStretch});
    d.put(a, "sync", std::array{p.syncPlan, p.syncRate});
    d.put(a, "timelines", std::array{p.timelines[0] != nullptr, p.timelines[1] != nullptr});
    d.put(a, "rateMargins",
          std::array{p.rateDeviceMargin, p.rateBackendLatency, p.rateSourceAhead,
                     p.rateSourceBehind});
    d.put(a, "timeMap", p.timeMap != nullptr);
    d.put(a, "warnings", p.warnings);
    d.put(a, "clipPlayers", p.clipPlayers);
    // The project's side of following (ADR-0098 X4): its regions' sizes.
    d.put(a, "follow",
          p.follow ? std::array<std::uint64_t, 2>{p.follow->full.place.size(),
                                                  p.follow->keys.place.size()}
                   : std::array<std::uint64_t, 2>{0, 0});
    d.put(a, "gestureHolds", p.gestureHolds);
    d.put(a, "projectDir", p.projectDir.generic_string());
    d.put(a, "preparedSources", p.preparedSources != nullptr);
    d.put(a, "mainMaster", static_cast<std::uint64_t>(p.mainMaster));
    for (std::size_t i = 0; i < p.outputs.size(); ++i)
        d.put(at(a, "output", i), "tap",
              std::array<std::int64_t, 3>{static_cast<std::int64_t>(p.outputs[i].node),
                                          p.outputs[i].left, p.outputs[i].right});
    d.put(a, "nodes", static_cast<std::uint64_t>(p.nodes.size()));
    for (std::size_t i = 0; i < p.nodes.size(); ++i)
        dumpNode(d, "n" + std::to_string(i), p.nodes[i]);
    d.put(a, "edges", static_cast<std::uint64_t>(p.edges.size()));
    for (std::size_t i = 0; i < p.edges.size(); ++i)
        dumpEdge(d, "e" + std::to_string(i), p.edges[i]);
    d.put(a, "modEdges", static_cast<std::uint64_t>(p.modEdges.size()));
    for (std::size_t i = 0; i < p.modEdges.size(); ++i)
        dumpModEdge(d, "m" + std::to_string(i), p.modEdges[i]);
    d.put(a, "modOffsetOf", p.modOffsetOf);  // the values are telemetry, not the plan
    std::vector<std::string> index;
    for (const Plan::NodeKey& k : p.nodeIndex)
        index.push_back(k.id.str() + "=" + std::to_string(k.node));
    d.put(a, "nodeIndex", index);
    // The executors' shares (ADR-0014) and their task graphs.
    d.put(a, "live", std::array{p.anyLive, p.heldLive, p.split});
    d.put(a, "rendererNodes", p.rendererNodes);
    d.put(a, "liveNodes", p.liveNodes);
    d.put(a, "frontier", p.frontier);
    dumpGraph(d, "plan graph", p.graph.get());
    dumpGraph(d, "plan rendererGraph", p.rendererGraph.get());
    dumpGraph(d, "plan liveGraph", p.liveGraph.get());
    d.put(a, "instances", names.list(p.instances));
    d.put(a, "rendererInstances", names.list(p.rendererInstances));
    d.put(a, "liveInstances", names.list(p.liveInstances));
    d.put(a, "modules", names.list(p.modules));
    d.put(a, "rendererModules", names.list(p.rendererModules));
    d.put(a, "liveModules", names.list(p.liveModules));
    d.put(a, "meterNodes", p.meterNodes);
    d.put(a, "rendererMeterNodes", p.rendererMeterNodes);
    d.put(a, "liveMeterNodes", p.liveMeterNodes);
    d.put(a, "rateOutput", p.rateOutput != nullptr);
    d.put(a, "frontierRings", static_cast<std::uint64_t>(p.frontierRings.size()));
    for (std::size_t i = 0; i < p.recordTaps.size(); ++i)
        d.put(at(a, "record", i), "tap",
              std::array<std::int64_t, 2>{static_cast<std::int64_t>(p.recordTaps[i].node),
                                          p.recordTaps[i].ring});
    d.put(a, "recordRings", static_cast<std::uint64_t>(p.recordRings.size()));
    // Device MIDI (ADR-0016, ADR-0067).
    for (std::size_t i = 0; i < p.hardwareMidi.size(); ++i) {
        const HardwareRoute& h = *p.hardwareMidi[i];
        const auto ha = at(a, "hardwareMidi", i);
        d.put(ha, "ids", std::array{h.id, h.sourceId, h.noteOutId});
        d.put(ha, "source", static_cast<std::uint64_t>(h.source));
        d.put(ha, "device", h.device);
        d.put(ha, "tap", h.tap);
        d.put(ha, "route", std::array{h.rackNotes, h.fromChannel, h.toChannel, h.port});
        d.put(ha, "mute", h.mute);
        d.put(ha, "delay", h.delay);
        dumpAutomation(d, at(ha, "muteCurve"), h.muteCurve);
        dumpAutomation(d, at(ha, "sourceMute"), h.sourceMute);
        dumpVcas(d, ha, h.vcas);
    }
    for (std::size_t i = 0; i < 16; ++i)
        for (const auto* which : {&p.midiInputsSettings, &p.midiOutputsSettings}) {
            const MidiPortSettings& s = (*which)[i];
            d.put(at(a, which == &p.midiInputsSettings ? "midiIn" : "midiOut", i), "settings",
                  std::array<double, 8>{double(s.enabled), s.offsetMs, double(s.resetOnStop),
                                        double(s.clock), double(s.transport), double(s.spp),
                                        double(s.mtc), double(s.clockWhileStopped)});
        }
    d.put(a, "hardwareLead", p.hardwareLead);
    d.put(a, "hardwareLeadBase", p.hardwareLeadBase);
    std::vector<std::string> processed;
    for (const ProcessedInput& in : p.processedInputs)
        processed.push_back(in.track.str() + "+" + std::to_string(in.latency));
    d.put(a, "processedInputs", processed);
    // Mix values (ADR-0060, ADR-0093): the slots as published so far.
    if (p.mix) {
        const MixSlots& m = *p.mix;
        for (std::size_t i = 0; m.strips && i < p.nodes.size(); ++i)
            dumpSlot(d, at("slot", "strip", i), m.strips[i]);
        for (std::size_t i = 0; m.routes && i < p.edges.size(); ++i)
            dumpSlot(d, at("slot", "route", i), m.routes[i]);
        for (std::size_t i = 0; m.vcas && i < m.vcaIds.size(); ++i)
            dumpSlot(d, at("slot", "vca", i), m.vcas[i]);
        for (std::size_t i = 0; m.hardware && i < p.hardwareMidi.size(); ++i)
            dumpSlot(d, at("slot", "hardware", i), m.hardware[i]);
        d.put("slot", "vcaIds", m.vcaIds);
        d.put("slot", "stageFx", m.stageFx);
        for (std::size_t i = 0; m.stages && i < m.stageFx.size(); ++i)
            d.put(at("slot", "stage", i), "wet",
                  std::array{m.stages[i].wet.load(std::memory_order_relaxed),
                             double(m.stages[i].bypass.load(std::memory_order_relaxed))});
        d.put("slot", "laneEnvelopes", m.laneEnvelopes);
        for (std::size_t i = 0; m.lanes && i < m.laneEnvelopes.size(); ++i)
            d.put(at("slot", "lane", i), "held", m.lanes[i].load(std::memory_order_relaxed));
    }
    return std::move(d.text);
}

std::string firstDifference(std::string_view a, std::string_view b) {
    std::size_t line = 1;
    while (!a.empty() || !b.empty()) {
        const auto ea = a.find('\n'), eb = b.find('\n');
        const auto la = a.substr(0, ea), lb = b.substr(0, eb);
        if (la != lb || a.empty() != b.empty())
            return "line " + std::to_string(line) + ": " + std::string(la) + " | " +
                   std::string(lb);
        a.remove_prefix(ea == std::string_view::npos ? a.size() : ea + 1);
        b.remove_prefix(eb == std::string_view::npos ? b.size() : eb + 1);
        ++line;
    }
    return {};
}
}  // namespace supple::engine::detail
