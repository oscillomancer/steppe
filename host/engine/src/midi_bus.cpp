// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
//
// The FX chain as a MIDI bus (ADR-0068). The bus starts as a track's pre-FX
// MIDI stream. Each plug-in with an input note port hears the bus at its
// place in the chain; a plug-in with output note ports, while it runs and is
// not bypassed, replaces the bus with its output for the plug-ins after it,
// or merges its output in. The post-FX stream (midiPostFx) is the bus after
// the chain. Notes travel by bus id: a pre-FX slot, or a note-output stage's
// busBase + one of its kBusVoices slots. Whatever a stage, or the post-FX
// stream, holds of a part that no longer reaches it (a note-output plug-in
// bypassed, removed or switched to replace ahead of it) ends at the start of
// the block. A track whose chain_midi is instrument has no bus: only the note
// target hears its MIDI, as before 2.10, and post-FX equals pre-FX. Nothing
// here allocates: every buffer is sized in prepareRackMidi.
#include <algorithm>

#include "rack_runtime.h"

namespace supple::engine::detail {
namespace {
// The part a bus id belongs to: -1 the pre-FX stream, else a note-output
// stage's ordinal.
int partOf(const Node& node, std::int32_t id) SUPPLE_RT {
    const auto pool = static_cast<std::int32_t>(node.midiHeld.size());
    return id < pool ? -1 : static_cast<int>((id - pool) / static_cast<std::int32_t>(kBusVoices));
}

bool accepts(const Stage& st) SUPPLE_RT { return st.instance && st.instance->acceptsNotes(); }

void sortByTime(std::vector<host::Event>& events) SUPPLE_RT {
    // Stable insertion sort: no allocation, linear when nearly sorted.
    for (std::size_t i = 1; i < events.size(); ++i)
        for (std::size_t j = i; j > 0 && events[j - 1].time > events[j].time; --j)
            std::swap(events[j - 1], events[j]);
}

host::Event noteEvent(host::Event::Kind kind, std::uint32_t time, std::int16_t port,
                      std::int16_t channel, std::int16_t key, std::int32_t id,
                      double velocity) SUPPLE_RT {
    host::Event e;
    e.kind = kind;
    e.time = time;
    e.port = port;
    e.channel = channel;
    e.key = key;
    e.noteId = id;
    e.velocity = velocity;
    return e;
}
}  // namespace

bool midiBusReaches(const Node& node, int part, std::size_t at) SUPPLE_RT {
    // The last running, replacing note-output stage ahead of `at`.
    int from = -1;
    for (std::size_t q = 0; q < node.midiBusStages.size(); ++q) {
        const auto index = static_cast<std::size_t>(node.midiBusStages[q]);
        if (index >= at) break;
        const Stage& st = node.chain[index];
        if (st.busLive && !st.busMerge) from = static_cast<int>(q);
    }
    if (part < 0) return from < 0;
    if (static_cast<std::size_t>(part) >= node.midiBusStages.size()) return false;
    const auto index = static_cast<std::size_t>(node.midiBusStages[static_cast<std::size_t>(part)]);
    return index < at && node.chain[index].busLive && part >= from;
}

bool beginMidiBus(Node& node, const void* self) SUPPLE_RT {
    for (const int index : node.midiBusStages) {
        Stage& st = node.chain[static_cast<std::size_t>(index)];
        // On the bus: it will run (a bypassed plug-in passes the bus as it
        // passes audio). A plug-in that fails mid-block outputs nothing.
        const bool live = !st.bypass && !st.callbackBypass && st.instance &&
                          st.instance->processing() && (!self || st.instance->owner() == self);
        if (!live)
            for (auto& v : st.busHeld) v.high = false;
        st.busLive = live;
    }
    const bool target =
        node.noteTarget < 0 || midiBusReaches(node, -1, static_cast<std::size_t>(node.noteTarget));
    for (std::size_t k = 0; k < node.chain.size(); ++k) {
        Stage& st = node.chain[k];
        if (!accepts(st)) continue;
        const bool isTarget = static_cast<int>(k) == node.noteTarget;
        // A voice already flagged (a plan swap's carry could not follow it)
        // still needs its release.
        bool any = false;
        for (auto& voice : st.voices) {
            if (voice.release) {
                any = true;
                continue;
            }
            if (voice.origin.item == kBusNoteOrigin) {
                voice.release =
                    !node.midiBusOn ||
                    !midiBusReaches(node, partOf(node, voice.sourceId - kBusNoteBase), k);
            } else if (isTarget && !target) {
                // The pre-FX stream no longer reaches the note target: what it
                // played of it ends (a replacing plug-in now plays it).
                voice.release = true;
            }
            any |= voice.release;
        }
        // The note target's releases go out with its own (processNode step
        // 2); any other stage's lead its block.
        if (!any || isTarget) continue;
        for (const auto& voice : st.voices) {
            if (!voice.release) continue;
            if (st.blockEvents.size() == st.blockEvents.capacity()) {
                ++st.voiceDropped;
                break;
            }
            st.blockEvents.push_back(noteEvent(host::Event::Kind::NoteOff, 0, voice.port,
                                               voice.channel, voice.key, voice.noteId, 0.0));
        }
        st.releases = st.blockEvents.size();
    }
    return target;
}

void fillMidiBus(Node& node) SUPPLE_RT {
    node.midiBus.clear();
    for (std::size_t i = 0; i < node.midiPreFx.size(); ++i) {
        if (node.midiBus.size() == node.midiBus.capacity()) {
            ++node.midiDropped;
            break;
        }
        BusEvent b;
        b.event = node.midiPreFx[i];
        b.part = -1;
        b.stages = i < node.midiChain.size() && node.midiChain[i] != 0;
        node.midiBus.push_back(b);
    }
}

void feedMidiBus(Node& node, Stage& st, std::size_t k) SUPPLE_RT {
    if (!node.midiBusOn || !accepts(st)) return;  // instrument mode: the target alone
    const bool isTarget = static_cast<int>(k) == node.noteTarget;
    const std::size_t before = st.blockEvents.size();
    for (const BusEvent& b : node.midiBus) {
        // A take's own note or controller is its instrument's; the note
        // target already hears the pre-FX stream under its own ids.
        if (!b.stages || (isTarget && b.part < 0)) continue;
        if (st.blockEvents.size() == st.blockEvents.capacity()) {
            ++st.voiceDropped;
            break;
        }
        host::Event e = b.event;
        const bool note = e.kind == host::Event::Kind::NoteOn ||
                          e.kind == host::Event::Kind::NoteOff ||
                          e.kind == host::Event::Kind::NoteChoke;
        if (note) e.noteId = e.noteId >= 0 ? kBusNoteBase + e.noteId : -1;
        st.blockEvents.push_back(e);
    }
    if (st.blockEvents.size() > before && before > 0) sortByTime(st.blockEvents);
}

void stageMidiBus(Node& node, Stage& st) SUPPLE_RT {
    if (st.busPart < 0 || !st.busLive) return;  // not on the bus: it passes
    // Its output as bus events, in time order (the host sorts each block's).
    auto emit = [&](const host::Event& e) {
        if (node.midiBusGen.size() == node.midiBusGen.capacity()) {
            ++st.voiceDropped;
            return;
        }
        BusEvent b;
        b.event = e;
        b.part = static_cast<std::int16_t>(st.busPart);
        node.midiBusGen.push_back(b);
    };
    node.midiBusGen.clear();
    auto end = [&](std::size_t slot, host::Event::Kind kind, std::uint32_t time) {
        auto& v = st.busHeld[slot];
        emit(noteEvent(kind, time, v.port, v.channel, v.key,
                       st.busBase + static_cast<std::int32_t>(slot), 0.0));
        v.high = false;
    };
    auto endMatching = [&](host::Event::Kind kind, std::uint32_t time, std::int16_t port,
                           std::int16_t channel, std::int16_t key, std::int32_t noteId) {
        for (std::size_t s = 0; s < st.busHeld.size(); ++s) {
            const auto& v = st.busHeld[s];
            if (v.high && (port < 0 || v.port == port) && (channel < 0 || v.channel == channel) &&
                (key < 0 || v.key == key) && (noteId < 0 || v.noteId == noteId))
                end(s, kind, time);
        }
    };
    auto start = [&](std::uint32_t time, std::int16_t port, std::int16_t channel, std::int16_t key,
                     std::int32_t noteId, double velocity) {
        if (channel < 0 || channel > 15 || key < 0 || key > 127) return;
        // Safety net: the same note again ends the one held first.
        for (std::size_t s = 0; s < st.busHeld.size(); ++s) {
            const auto& v = st.busHeld[s];
            if (v.high && v.port == port && v.channel == channel && v.key == key &&
                v.noteId == noteId)
                end(s, host::Event::Kind::NoteOff, time);
        }
        const auto free = std::find_if(st.busHeld.begin(), st.busHeld.end(),
                                       [](const BusVoice& v) { return !v.high; });
        if (free == st.busHeld.end()) {
            ++st.voiceDropped;
            return;
        }
        *free = {true, port, channel, key, noteId};
        emit(noteEvent(host::Event::Kind::NoteOn, time, port, channel, key,
                       st.busBase + static_cast<std::int32_t>(free - st.busHeld.begin()),
                       velocity));
    };
    if (st.processed)
        for (std::size_t i = 0; i < st.outEvents.count; ++i) {
            const host::OutputEvent& o = st.outEvents.storage[i];
            if (!o.midiOutput()) continue;
            switch (o.kind) {
                case host::OutputEvent::Kind::NoteOn:
                    start(o.time, o.port, o.channel, o.key, o.noteId, o.value);
                    break;
                case host::OutputEvent::Kind::NoteOff:
                    endMatching(host::Event::Kind::NoteOff, o.time, o.port, o.channel, o.key,
                                o.noteId);
                    break;
                case host::OutputEvent::Kind::NoteChoke:
                    endMatching(host::Event::Kind::NoteChoke, o.time, o.port, o.channel, o.key,
                                o.noteId);
                    break;
                default: {
                    // MIDI 1.0. Its notes travel as notes, so they are held,
                    // carried and released like any other.
                    const int status = o.midi[0] & 0xf0;
                    const auto channel = static_cast<std::int16_t>(o.midi[0] & 0x0f);
                    const auto key = static_cast<std::int16_t>(o.midi[1]);
                    if (status == 0x90 && o.midi[2] > 0) {
                        start(o.time, o.port, channel, key, -1, o.midi[2] / 127.0);
                        break;
                    }
                    if (status == 0x80 || status == 0x90) {
                        endMatching(host::Event::Kind::NoteOff, o.time, o.port, channel, key, -1);
                        break;
                    }
                    if (status == 0xb0 && (o.midi[1] == 120 || o.midi[1] == 123))
                        endMatching(host::Event::Kind::NoteOff, o.time, o.port, channel, -1, -1);
                    host::Event e;
                    e.kind = host::Event::Kind::Midi;
                    e.time = o.time;
                    e.port = o.port;
                    e.channel = o.midi[0] < 0xf0 ? channel : static_cast<std::int16_t>(-1);
                    std::copy_n(o.midi, 3, e.midi);
                    emit(e);
                }
            }
        }
    // Replace: its output alone. Merge: both, by time, the bus first at a frame.
    auto& out = node.midiBusNext;
    out.clear();
    auto add = [&](const BusEvent& b) {
        if (out.size() < out.capacity())
            out.push_back(b);
        else
            ++node.midiDropped;
    };
    const auto& in = node.midiBus;
    std::size_t a = 0;
    for (const BusEvent& made : node.midiBusGen) {
        while (st.busMerge && a < in.size() && in[a].event.time <= made.event.time) add(in[a++]);
        add(made);
    }
    while (st.busMerge && a < in.size()) add(in[a++]);
    std::swap(node.midiBus, node.midiBusNext);
}

void finishMidiBus(Node& node) SUPPLE_RT {
    auto& post = node.midiPostFx;
    post.clear();
    const std::size_t end = node.chain.size();
    auto put = [&](const host::Event& e) {
        if (post.size() < post.capacity())
            post.push_back(e);
        else
            ++node.midiDropped;
    };
    // What it held of a part that no longer reaches it ends first.
    for (std::size_t id = 0; id < node.midiPostHeld.size(); ++id) {
        auto& v = node.midiPostHeld[id];
        if (!v.high || midiBusReaches(node, partOf(node, static_cast<std::int32_t>(id)), end))
            continue;
        put(noteEvent(host::Event::Kind::NoteOff, 0, 0, v.channel, v.key,
                      static_cast<std::int32_t>(id), 0.0));
        v.high = false;
    }
    for (const BusEvent& b : node.midiBus) {
        const auto& e = b.event;
        put(e);
        if (e.noteId < 0 || static_cast<std::size_t>(e.noteId) >= node.midiPostHeld.size())
            continue;
        auto& v = node.midiPostHeld[static_cast<std::size_t>(e.noteId)];
        if (e.kind == host::Event::Kind::NoteOn) {
            v.high = true;
            v.channel = e.channel;
            v.key = e.key;
            v.velocity = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(e.velocity * 127.0 + 0.5), 1, 127));
        } else if (e.kind == host::Event::Kind::NoteOff || e.kind == host::Event::Kind::NoteChoke) {
            v.high = false;
        }
    }
}
}  // namespace supple::engine::detail
