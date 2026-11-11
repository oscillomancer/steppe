// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <cmath>
#include <tuple>
#include "rack_runtime.h"

namespace supple::engine::detail {
namespace {
constexpr std::size_t kMaxMidiEvents = 1 << 20;
constexpr std::int32_t kLiveKeys = 16 * 128;
// Item notes held at once beyond the compiled overlap: releases that land
// late at a loop wrap or on a compressed playrate frame.
constexpr std::size_t kTimelineVoiceMargin = 16;

std::uint64_t routeNamespace(const Edge& edge) SUPPLE_RT {
    // Widen before adding the reserved timeline/device namespaces.
    return std::uint64_t{edge.route->n} + 2;
}
bool note(const host::Event& e) SUPPLE_RT {
    return e.kind == host::Event::Kind::NoteOn || e.kind == host::Event::Kind::NoteOff ||
           e.kind == host::Event::Kind::NoteChoke;
}
// A channel message (a note, or MIDI below 0xF0): system messages have no
// channel, so a send's channel filter and remap leave them alone.
bool channelMessage(const host::Event& e) SUPPLE_RT {
    return note(e) || (e.kind == host::Event::Kind::Midi && e.midi[0] < 0xf0);
}
host::Event message(std::uint8_t status, int key, int velocity, int voice,
                    std::uint32_t frame = 0) SUPPLE_RT {
    host::Event e;
    e.time = frame;
    e.channel = static_cast<std::int16_t>(status & 15);
    e.key = static_cast<std::int16_t>(key);
    e.velocity = velocity / 127.;
    e.noteId = voice;
    if ((status & 0xf0) == 0x90 && velocity)
        e.kind = host::Event::Kind::NoteOn;
    else if ((status & 0xf0) == 0x80 || (status & 0xf0) == 0x90)
        e.kind = host::Event::Kind::NoteOff;
    else {
        e.kind = host::Event::Kind::Midi;
        e.midi[0] = status;
        e.midi[1] = static_cast<std::uint8_t>(key);
        e.midi[2] = static_cast<std::uint8_t>(velocity);
    }
    return e;
}
int velocity7(double velocity) SUPPLE_RT {
    return std::clamp<int>(static_cast<int>(std::lround(velocity * 127)), 1, 127);
}
bool bytes(const host::Event& e, std::uint8_t* out) SUPPLE_RT {
    if (note(e)) {
        out[0] = static_cast<std::uint8_t>((e.kind == host::Event::Kind::NoteOn ? 0x90 : 0x80) |
                                           e.channel);
        out[1] = static_cast<std::uint8_t>(e.key);
        out[2] = e.kind == host::Event::Kind::NoteOn
                     ? static_cast<std::uint8_t>(velocity7(e.velocity))
                     : 0;
        return true;
    }
    if (e.kind != host::Event::Kind::Midi) return false;
    std::copy_n(e.midi, 3, out);
    return true;
}
template <class T>
bool put(std::vector<T>& into, const T& value, std::uint64_t& dropped) SUPPLE_RT {
    if (into.size() == into.capacity()) {
        ++dropped;
        return false;
    }
    into.push_back(value);
    return true;
}

MidiSlot::Kind inputKind(const Node& node, std::int32_t input) SUPPLE_RT {
    return input < node.midiLiveBase     ? MidiSlot::Kind::Timeline
           : input < node.midiRoutedBase ? MidiSlot::Kind::Live
                                         : MidiSlot::Kind::Routed;
}
// The slot an adopted voice moved to (-1: it did not survive).
int remapSlot(const Node& node, int slot) SUPPLE_RT {
    for (const auto& [from, to] : node.midiRemap)
        if (from == slot) return to;
    return -1;
}
void freeSlot(Node& node, int slot) SUPPLE_RT {
    auto& v = node.midiHeld[static_cast<std::size_t>(slot)];
    if (v.input >= 0 && static_cast<std::size_t>(v.input) < node.midiInput.size() &&
        node.midiInput[static_cast<std::size_t>(v.input)] == slot)
        node.midiInput[static_cast<std::size_t>(v.input)] = -1;
    v = {};
    node.midiFree.push_back(slot);  // capacity: the pool
}
void refillFree(Node& node) SUPPLE_RT {
    node.midiFree.clear();
    for (std::size_t s = node.midiHeld.size(); s-- > 0;)
        if (!node.midiHeld[s].high) node.midiFree.push_back(static_cast<std::int32_t>(s));
}

// Delivered: into a Notes module's block, or a track's stream as a routed
// input. Recording hears what a track is delivered.
void deliver(Node& target, Edge& edge, host::Event e, int voice) SUPPLE_RT {
    const int address = voice < 0 ? -1 : edge.midiVoices[static_cast<std::size_t>(voice)].address;
    if (target.rack) {
        auto& feed = target.rack->notes[static_cast<std::size_t>(edge.midiNotes)];
        std::uint8_t m[3];
        if (bytes(e, m))
            put(feed.block, {e.time, m[0], m[1], m[2], false, routeNamespace(edge), address},
                target.midiDropped);
        return;
    }
    e.noteId = -1;
    if (note(e)) {
        if (voice < 0) return;
        e.noteId = edge.midiSourceBase + voice;
        if (e.kind == host::Event::Kind::NoteOn)
            target.midiRouted[static_cast<std::size_t>(e.noteId - target.midiRoutedBase)] = {
                *edge.route, address};
    }
    put(target.midiStage.blockEvents, e, target.midiDropped);
    RoutedMidiEvent midi;
    midi.frame = e.time;
    midi.route = *edge.route;
    midi.voice = voice < 0 ? 0 : address;  // all delivered events use the internal input clock
    if (bytes(e, midi.bytes)) put(target.routedMidi, midi, target.midiDropped);
}
void emit(Node& target, Edge& edge, int voice, std::uint32_t frame, bool on) SUPPLE_RT {
    auto& v = edge.midiVoices[static_cast<std::size_t>(voice)];
    if (on) v.address = static_cast<int>(edge.midiNextVoice++ & 0x7fffffffu);
    deliver(target, edge,
            message(static_cast<std::uint8_t>((on ? 0x90 : 0x80) | v.channel), v.key,
                    on ? v.velocity : 0, voice, frame),
            voice);
    v.delivered = on;
}
void queue(Node& target, Edge& edge, std::int64_t sample, host::Event e, int voice) SUPPLE_RT {
    if (note(e) && voice < 0) return;  // an untracked note stays with its own instrument
    const bool channel = channelMessage(e);
    if (channel && edge.midiFrom &&
        (e.kind == host::Event::Kind::Midi ? e.midi[0] & 15 : e.channel) != edge.midiFrom - 1)
        return;
    if (edge.midiCount == edge.midiQueue.size()) {  // sized for the worst case in prepareRackMidi
        ++target.midiDropped;
        return;
    }
    if (channel && edge.midiTo) {
        e.channel = static_cast<std::int16_t>(edge.midiTo - 1);
        if (e.kind == host::Event::Kind::Midi)
            e.midi[0] = static_cast<std::uint8_t>((e.midi[0] & 0xf0) | (edge.midiTo - 1));
    }
    edge.midiQueue[(edge.midiHead + edge.midiCount++) % edge.midiQueue.size()] = {sample, e, voice};
}
void orderMidi(Node& node) SUPPLE_RT {
    node.midiOrder.sort(
        node.midiStage.blockEvents, node.midiSortScratch, [](const auto& e) { return e.time; },
        [](const auto& e) { return std::tuple{e.noteId, e.port, e.channel, e.key}; },
        [](const auto& e) {
            return e.kind == host::Event::Kind::NoteOff || e.kind == host::Event::Kind::NoteChoke
                       ? 0
                   : e.kind == host::Event::Kind::NoteOn ? 2
                                                         : 1;
        });
}

// The most item notes held at once: overlapping note-on/off pairs in
// compiled order (ties count high), plus a margin.
std::size_t timelineVoices(const std::vector<TimedEvent>& events) {
    std::size_t held = 0, most = 0;
    for (const auto& timed : events) {
        if (timed.event.kind == host::Event::Kind::NoteOn)
            most = std::max(most, ++held);
        else if ((timed.event.kind == host::Event::Kind::NoteOff ||
                  timed.event.kind == host::Event::Kind::NoteChoke) &&
                 held)
            --held;
    }
    return most + kTimelineVoiceMargin;
}

// An item note's compiled id in the new plan, by its musical identity.
std::int32_t timelineInput(const Node& old, const Node& node, std::int32_t input) SUPPLE_RT {
    const auto& sources = old.midiStage.noteSources;
    if (input < 0 || static_cast<std::size_t>(input) >= sources.size()) return -1;
    const auto& source = sources[static_cast<std::size_t>(input)];
    if (source.id < 0) return -1;
    const auto& index = node.midiStage.noteSourceIndex;
    const auto found =
        std::lower_bound(index.begin(), index.end(), source,
                         [](const auto& a, const auto& b) { return a.address() < b.address(); });
    return found != index.end() && found->address() == source.address() ? found->id : -1;
}
}  // namespace

std::size_t timelineVoiceBound(const std::vector<TimedEvent>& events) {
    return timelineVoices(events);
}

Status prepareRackMidi(Plan& p) {
    for (const auto& edge : p.edges)
        if (edge.midi) p.nodes[edge.from].midiSends = true;
    for (auto& node : p.nodes) {
        auto budget = [&](std::size_t count, const std::string& path) -> Status {
            if (count > kMaxMidiEvents)
                return Error("engine.midi_budget", "MIDI sends exceed the prepared event budget",
                             path, "reduce MIDI fan-in, simultaneous notes or plug-in latency");
            return {};
        };
        const bool track = !node.rack;
        // The pool: as many voices as can sound at once. Item notes by their
        // compiled overlap, every live key, and each send's source voices.
        std::size_t pool = 0, inputs = 0;
        if (track) {
            const auto timeline = node.midiStage.noteSources.size();
            if (timeline >= static_cast<std::size_t>(kRoutedNoteBase))
                return Error("engine.midi_budget", "the track has too many MIDI notes",
                             "/tracks/" + node.id.str(), "split the track's MIDI items");
            node.midiLiveBase = static_cast<std::int32_t>(timeline);
            node.midiRoutedBase = node.midiLiveBase + (node.midiIn ? kLiveKeys : 0);
            pool = timelineVoices(node.events) + (node.midiIn ? kLiveKeys : 0);
            inputs = static_cast<std::size_t>(node.midiRoutedBase);
        }
        const auto window = [&](std::int64_t frames) {
            std::size_t most = 0;
            for (std::size_t a = 0, b = 0; a < node.events.size(); ++a) {
                b = std::max(a, b);
                while (b < node.events.size() &&
                       node.events[b].sample < node.events[a].sample + frames)
                    ++b;
                most = std::max(most, b - a);
            }
            return most;
        };
        // Events in a block: the timeline's window, a release per held voice
        // (stop, loop wrap, all-notes-off, adoption), live input and sends.
        std::size_t capacity =
            window(static_cast<std::int64_t>(p.block) * (p.rateEnabled ? 4 : 1)) + pool + 1;
        std::size_t rate = window(p.rateEnabled ? 4 : 1) + (node.midiIn ? kMaxMidiInputEvents : 0);
        if (node.midiIn) capacity += kMaxMidiInputEvents;
        std::size_t routedVoices = 0;
        for (std::size_t index : node.in) {
            auto& edge = p.edges[index];
            if (!edge.midi) continue;
            const auto& source = p.nodes[edge.from];
            const bool notesOut = edge.rackNotes >= 0;
            const std::size_t voices = notesOut ? rack::kChannels : source.midiHeld.size();
            // What the source sends in `block` frames. A delay only shifts
            // the stream, so a block here takes one such window of a track's
            // (its midiWindow), whatever the PDC: a relay chain grows by each
            // hop's own events, never doubling per hop. A Note Out's bound
            // is per frame; two of its blocks keep the original margin.
            const std::size_t perBlock =
                notesOut ? static_cast<std::size_t>(rack::kChannels) * (p.block + 2)
                         : source.midiWindow;
            const std::size_t due = notesOut ? 2 * perBlock : perBlock;
            const std::size_t perFrame = notesOut ? rack::kChannels : source.midiEventRate;
            const std::uint64_t queued =
                2 * static_cast<std::uint64_t>(perBlock) +
                static_cast<std::uint64_t>(
                    edge.delay.length +
                    (edge.rackNotes < 0 && edge.tap != model::Tap::PreFx ? source.latChain : 0)) *
                    perFrame +
                voices;
            const auto path = "/routes/" + edge.route->str();
            SUPPLE_TRY(budget(static_cast<std::size_t>(queued), path));
            edge.midiQueue.resize(static_cast<std::size_t>(queued));
            edge.midiVoices.assign(voices, {});
            edge.midiAddresses.clear();
            edge.midiAddresses.reserve(voices);
            if (node.rack) {
                node.rack->notes[static_cast<std::size_t>(edge.midiNotes)].routes.push_back(
                    routeNamespace(edge));
            } else {
                edge.midiSourceBase = static_cast<int>(inputs);
                inputs += voices;
                pool += voices;
                routedVoices += voices;
            }
            auto points = edge.autoMute.points.size() +
                          (edge.tap == model::Tap::PostFader ? edge.stripMute.points.size() : 0);
            for (const auto& vca : edge.vcas.values) points += vca.gate.points.size();
            // Each mute transition ends or restarts at most every voice.
            const auto transitions = points ? std::min<std::size_t>(p.block, points + 2) : 1;
            capacity += due + (transitions + 2) * voices;
            rate += (notesOut ? 2 : 1) * perFrame + (points ? voices : 0);
            SUPPLE_TRY(budget(capacity, path));
        }
        node.midiEventRate = rate;
        // A window of `block` frames may straddle two blocks of live input;
        // releases come once, at the block start it holds.
        node.midiWindow = capacity + (node.midiIn ? kMaxMidiInputEvents : 0);
        SUPPLE_TRY(budget(capacity, "/tracks/" + node.id.str()));
        if (node.rack) {
            for (auto& feed : node.rack->notes) {
                feed.block.reserve(feed.block.capacity() + capacity);
                feed.sortScratch.reserve(feed.block.capacity());
                feed.order.prepare(feed.block.capacity(), p.block);
            }
            continue;
        }
        SUPPLE_TRY(budget(pool, "/tracks/" + node.id.str()));
        node.midiHeld.assign(pool, {});
        node.midiBefore.assign(node.midiSends ? pool : 0, {});
        node.midiFree.reserve(pool);
        refillFree(node);
        node.midiInput.assign(inputs, -1);
        node.midiRouted.assign(inputs - static_cast<std::size_t>(node.midiRoutedBase), {});
        node.midiRemap.reserve(pool);
        node.midiPreFx.reserve(capacity);
        node.midiPostFx.reserve(capacity);
        node.midiInstrument.reserve(capacity);
        node.midiSortScratch.reserve(capacity);
        node.midiOrder.prepare(capacity, p.block);
        node.routedMidi.reserve(capacity);
        auto& stream = node.midiStage;
        stream.blockEvents.reserve(capacity);
        stream.voices.reserve(pool);
        stream.pendingVoices.reserve(pool);
        if (node.noteTarget >= 0) {
            auto& stage = node.chain[static_cast<std::size_t>(node.noteTarget)];
            stage.rackMidiBase = kRoutedNoteBase;
            stage.blockEvents.reserve(stage.blockEvents.capacity() + capacity);
            stage.voices.reserve(stage.voices.capacity() + routedVoices);
            stage.pendingVoices.reserve(stage.voices.capacity());
        }
    }
    return {};
}

void beginTrackMidi(Node& node, bool playing, bool hearDevice) SUPPLE_RT {
    auto& events = node.midiStage.blockEvents;
    events.clear();
    node.midiStage.voices.clear();
    if (node.midiSends)
        std::copy(node.midiHeld.begin(), node.midiHeld.end(), node.midiBefore.begin());
    const bool endTimeline = !playing || node.midiReleaseTimeline;
    for (const auto& v : node.midiHeld) {
        if (!v.high || v.orphan) continue;
        const auto off = message(static_cast<std::uint8_t>(0x80 | v.channel), v.key, 0, v.input);
        if (v.kind == MidiSlot::Kind::Timeline) {
            if (endTimeline) {
                put(events, off, node.midiDropped);
            } else if (node.midiStage.voices.size() < node.midiStage.voices.capacity()) {
                // Held for rateEvents: a loop wrap ends it.
                HeldVoice voice;
                voice.sourceId = voice.noteId = v.input;
                voice.channel = v.channel;
                voice.key = v.key;
                node.midiStage.voices.push_back(voice);
            }
        } else if (v.kind == MidiSlot::Kind::Live && !hearDevice) {
            put(events, off, node.midiDropped);
        }
    }
    node.midiReleaseTimeline = false;
}

void finishTrackMidi(Node& node) SUPPLE_RT {
    auto& events = node.midiStage.blockEvents;
    for (auto& e : events)
        if (note(e) && e.noteId < 0 && node.midiIn)
            e.noteId = node.midiLiveBase + e.channel * 128 + e.key;
    orderMidi(node);
    auto& out = node.midiPreFx;
    auto& ids = node.midiInstrument;
    out.clear();
    ids.clear();
    auto emit = [&](const host::Event& e, std::int32_t instrument) {
        if (out.size() == out.capacity()) {
            ++node.midiDropped;
            return;
        }
        out.push_back(e);
        ids.push_back(instrument);
    };
    // The note target's id: an item note's compiled id (unless a take with
    // its own instrument plays it), tuple addressing for a key, a routed
    // voice's slot past kRoutedNoteBase.
    auto instrumentId = [&](MidiSlot::Kind kind, std::int32_t input, int slot) -> std::int32_t {
        switch (kind) {
            case MidiSlot::Kind::Timeline:
                return input >= 0 &&
                               !(static_cast<std::size_t>(input) < node.midiTakeSources.size() &&
                                 node.midiTakeSources[static_cast<std::size_t>(input)])
                           ? input
                           : kNotTargetNote;
            case MidiSlot::Kind::Live:
                return -1;
            case MidiSlot::Kind::Routed:
                return kRoutedNoteBase + slot;
        }
        return kNotTargetNote;
    };
    // A slot's note ends here: a release the stream did not get as an
    // event (an orphan, all-notes-off, an onset on an input still held).
    auto release = [&](int slot, std::uint32_t time) {
        const auto v = node.midiHeld[static_cast<std::size_t>(slot)];
        auto off = message(static_cast<std::uint8_t>(0x80 | v.channel), v.key, 0, slot, time);
        // An orphan item note or key is the note target's own to end (its
        // carry, releaseLive); a routed one it hears end here.
        emit(off, v.orphan && v.kind != MidiSlot::Kind::Routed
                      ? kNotTargetNote
                      : instrumentId(v.kind, v.input, slot));
        if (v.orphan && v.kind == MidiSlot::Kind::Routed) {
            RoutedMidiEvent midi;  // what recording heard begin ends with it
            midi.frame = time;
            midi.route = v.route;
            midi.voice = v.address;
            bytes(off, midi.bytes);
            put(node.routedMidi, midi, node.midiDropped);
        }
        freeSlot(node, slot);
    };
    for (std::size_t s = 0; s < node.midiHeld.size(); ++s)
        if (node.midiHeld[s].high && node.midiHeld[s].orphan) release(static_cast<int>(s), 0);
    for (const auto& e : events) {
        if (e.kind == host::Event::Kind::Midi && (e.midi[0] & 0xf0) == 0xb0 &&
            (e.midi[1] == 120 || e.midi[1] == 123)) {
            // All notes off: release at the controller, before a same-frame reattack.
            for (std::size_t s = 0; s < node.midiHeld.size(); ++s)
                if (node.midiHeld[s].high && node.midiHeld[s].channel == (e.midi[0] & 15))
                    release(static_cast<int>(s), e.time);
            emit(e, e.noteId);
            continue;
        }
        if (!note(e)) {
            emit(e, e.noteId);  // -2: a take's local controller, not the note target's
            continue;
        }
        const auto input = e.noteId;
        if (input < 0 || static_cast<std::size_t>(input) >= node.midiInput.size()) {
            // Untracked (a key without a stream input): the note target's alone.
            auto untracked = e;
            untracked.noteId = -1;
            emit(untracked, -1);
            continue;
        }
        const auto kind = inputKind(node, input);
        int slot = node.midiInput[static_cast<std::size_t>(input)];
        if (e.kind == host::Event::Kind::NoteOn) {
            // Safety net: an onset on an input still held ends the old note first.
            if (slot >= 0) release(slot, e.time);
            if (node.midiFree.empty()) {
                ++node.midiDropped;
                continue;
            }
            slot = node.midiFree.back();
            node.midiFree.pop_back();
            auto& v = node.midiHeld[static_cast<std::size_t>(slot)];
            v.high = true;
            v.kind = kind;
            v.channel = e.channel;
            v.key = e.key;
            v.velocity = static_cast<std::uint8_t>(velocity7(e.velocity));
            v.input = input;
            if (kind == MidiSlot::Kind::Routed) {
                const auto& routed =
                    node.midiRouted[static_cast<std::size_t>(input - node.midiRoutedBase)];
                v.route = routed.route;
                v.address = routed.address;
            }
            node.midiInput[static_cast<std::size_t>(input)] = slot;
            auto on = e;
            on.noteId = slot;
            emit(on, instrumentId(kind, input, slot));
            continue;
        }
        if (slot < 0) {
            // Nothing held: an item note's or a key's release may still end a
            // voice the note target holds, a routed one has nothing to end.
            if (kind != MidiSlot::Kind::Routed) {
                auto off = e;
                off.noteId = -1;
                emit(off, instrumentId(kind, input, -1));
            }
            continue;
        }
        auto off = e;
        off.noteId = slot;
        emit(off, instrumentId(kind, input, slot));
        freeSlot(node, slot);
    }
    node.midiPostFx.assign(out.begin(), out.end());
}

void resetTrackMidi(Node& node) SUPPLE_RT {
    ++node.midiEpoch;
    node.midiStage.voices.clear();
    for (auto& v : node.midiHeld) {
        if (v.input >= 0 && static_cast<std::size_t>(v.input) < node.midiInput.size())
            node.midiInput[static_cast<std::size_t>(v.input)] = -1;
        v = {};
    }
    refillFree(node);
}

void addRackMidi(Plan& p, Node& target, std::int64_t pos, std::uint32_t frames,
                 bool playing) SUPPLE_RT {
    target.routedMidi.clear();
    for (std::size_t index : target.in) {
        auto& edge = p.edges[index];
        if (!edge.midi) continue;
        if (edge.midiReconcile && target.rack) {
            target.rack->notes[static_cast<std::size_t>(edge.midiNotes)].notes->retainSource(
                routeNamespace(edge), edge.midiAddresses);
            edge.midiReconcile = false;
        }
        const auto& source = p.nodes[edge.from];
        const auto epoch = source.rack ? source.rack->midiEpoch : source.midiEpoch;
        if (edge.midiRelease || edge.midiEpoch != epoch || (!playing && source.rack)) {
            for (std::size_t c = 0; c < edge.midiVoices.size(); ++c) {
                auto& v = edge.midiVoices[c];
                if (v.delivered) emit(target, edge, static_cast<int>(c), 0, false);
                v = {};
            }
            if (target.rack)
                target.rack->notes[static_cast<std::size_t>(edge.midiNotes)].notes->endSource(
                    routeNamespace(edge));
            edge.midiCount = edge.midiHead = 0;
            edge.midiEpoch = epoch;
            edge.midiRelease = false;
            edge.midiStarted = false;
        }
        if (!playing && source.rack) continue;
        if (!edge.midiStarted) {
            // A new send: what its source holds starts now, unless the block
            // starts it anyway.
            edge.midiStarted = true;
            if (source.rack) {
                const auto& output =
                    source.rack->notesOut[static_cast<std::size_t>(edge.rackNotes)];
                for (int c = 0; c < rack::kChannels; ++c) {
                    const auto& v = output.before[static_cast<std::size_t>(c)];
                    const bool starts = std::any_of(
                        output.events.begin(),
                        output.events.begin() + static_cast<std::ptrdiff_t>(output.count),
                        [&](const auto& e) { return e.voice == c && e.midi.frame == 0; });
                    if (v.high && !starts)
                        queue(target, edge, pos + edge.delay.length,
                              message(static_cast<std::uint8_t>(0x90 | v.channel), v.key,
                                      v.velocity, c),
                              c);
                }
            } else {
                // Post-FX equals pre-FX: both taps start from the pre-FX voices.
                const auto& stream =
                    edge.tap == model::Tap::PreFx ? source.midiPreFx : source.midiPostFx;
                for (std::size_t c = 0; c < source.midiBefore.size(); ++c) {
                    const auto& v = source.midiBefore[c];
                    if (!v.high || v.orphan) continue;
                    const bool starts =
                        std::any_of(stream.begin(), stream.end(), [&](const auto& e) {
                            return e.noteId == static_cast<int>(c) && e.time == 0;
                        });
                    if (!starts)
                        queue(target, edge,
                              pos + edge.delay.length +
                                  (edge.tap == model::Tap::PreFx ? 0 : source.latChain),
                              message(static_cast<std::uint8_t>(0x90 | v.channel), v.key,
                                      v.velocity, static_cast<int>(c)),
                              static_cast<int>(c));
                }
            }
        }
        if (source.rack) {
            const auto& output = source.rack->notesOut[static_cast<std::size_t>(edge.rackNotes)];
            for (std::size_t i = 0; i < output.count; ++i) {
                const auto& e = output.events[i];
                queue(target, edge, pos + e.midi.frame + edge.delay.length,
                      message(e.midi.status, e.midi.data1, e.midi.data2, e.voice), e.voice);
            }
        } else {
            const auto& stream =
                edge.tap == model::Tap::PreFx ? source.midiPreFx : source.midiPostFx;
            for (const auto& e : stream)
                queue(target, edge,
                      pos + e.time + edge.delay.length +
                          (edge.tap == model::Tap::PreFx ? 0 : source.latChain),
                      e, note(e) ? e.noteId : -1);
        }
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            bool muted = edge.autoMute.active() ? edge.autoMute.at(pos + frame) >= .5 : edge.muted;
            if (!source.rack && edge.tap == model::Tap::PostFader) {
                muted = muted || (edge.stripMute.active() ? edge.stripMute.at(pos + frame) >= .5
                                                          : source.mute);
                double ignored = 0;
                edge.vcas.evaluate(pos + frame, ignored, muted);
            }
            while (edge.midiCount && edge.midiQueue[edge.midiHead].sample <= pos + frame) {
                auto pending = edge.midiQueue[edge.midiHead];
                edge.midiHead = (edge.midiHead + 1) % edge.midiQueue.size();
                --edge.midiCount;
                auto e = pending.event;
                e.time = frame;
                if (pending.voice < 0) {
                    if (!muted) deliver(target, edge, e, -1);
                    continue;
                }
                auto& v = edge.midiVoices[static_cast<std::size_t>(pending.voice)];
                if (v.delivered) emit(target, edge, pending.voice, frame, false);
                v.high = e.kind == host::Event::Kind::NoteOn;
                v.key = e.key;
                v.channel = e.channel;
                v.velocity = velocity7(e.velocity);
                if (v.high && !muted) emit(target, edge, pending.voice, frame, true);
            }
            if (muted != edge.midiMuted) {
                edge.midiMuted = muted;
                for (std::size_t c = 0; c < edge.midiVoices.size(); ++c) {
                    auto& v = edge.midiVoices[c];
                    if (muted && v.delivered)
                        emit(target, edge, static_cast<int>(c), frame, false);
                    else if (!muted && v.high && !v.delivered)
                        emit(target, edge, static_cast<int>(c), frame, true);
                }
            }
        }
    }
}

namespace {
// The old node a carried node continues (null: none).
const Node* continued(const Plan& from, const Node& node, int fromClass) SUPPLE_RT {
    for (const auto& old : from.nodes)
        if (old.id == node.id)
            return fromClass >= 0 && old.live != bool(fromClass) ? nullptr : &old;
    return nullptr;
}

// A track's pool: held voices keep their slots where they still fit. Item
// notes follow their musical identity, keys their key; routed voices wait
// for the send that delivered them (carryEdge), else end as orphans.
void carryPool(Node& node, const Node& old) SUPPLE_RT {
    node.midiEpoch = old.midiEpoch;
    auto& pool = node.midiHeld;
    auto place = [&](std::size_t from, std::size_t to) {
        auto v = old.midiHeld[from];
        std::int32_t input = -1;
        if (!v.orphan && v.kind == MidiSlot::Kind::Timeline)
            input = timelineInput(old, node, v.input);
        else if (!v.orphan && v.kind == MidiSlot::Kind::Live && node.midiIn)
            input = node.midiLiveBase + (v.input - old.midiLiveBase);
        v.orphan = input < 0;
        v.input = input;
        pool[to] = v;
        if (input >= 0)
            node.midiInput[static_cast<std::size_t>(input)] = static_cast<std::int32_t>(to);
        node.midiRemap.emplace_back(static_cast<std::int32_t>(from), static_cast<std::int32_t>(to));
    };
    for (std::size_t s = 0; s < old.midiHeld.size() && s < pool.size(); ++s)
        if (old.midiHeld[s].high) place(s, s);
    refillFree(node);
    for (std::size_t s = pool.size(); s < old.midiHeld.size(); ++s) {
        if (!old.midiHeld[s].high) continue;
        if (node.midiFree.empty()) {
            ++node.midiDropped;
            continue;
        }
        const auto to = static_cast<std::size_t>(node.midiFree.back());
        node.midiFree.pop_back();
        place(s, to);
    }
    // The note target's routed voices follow their slots.
    if (node.noteTarget >= 0) {
        auto& stage = node.chain[static_cast<std::size_t>(node.noteTarget)];
        for (auto& voice : stage.voices) {
            if (voice.origin.item != kRackNoteOrigin || voice.sourceId < kRoutedNoteBase) continue;
            const int slot = remapSlot(node, voice.sourceId - kRoutedNoteBase);
            if (slot < 0) {
                voice.release = true;
                voice.live = false;
            } else {
                voice.sourceId = kRoutedNoteBase + slot;
            }
        }
    }
}

// A send that continues unchanged (same route, tap, channels, delay and
// ends): its voices, queue and the target slots they hold.
bool sameSend(const Plan& to, const Edge& edge, const Plan& from, const Edge& old) SUPPLE_RT {
    if (!old.midi || edge.route != old.route || edge.tap != old.tap ||
        edge.midiFrom != old.midiFrom || edge.midiTo != old.midiTo ||
        edge.delay.length != old.delay.length)
        return false;
    const auto& source = to.nodes[edge.from];
    const auto& before = from.nodes[old.from];
    const auto& target = to.nodes[edge.to];
    const auto& oldTarget = from.nodes[old.to];
    const auto instrument = [](const Node& n) {
        return n.noteTarget < 0 ? nullptr
                                : n.chain[static_cast<std::size_t>(n.noteTarget)].instance;
    };
    if (source.id != before.id || target.id != oldTarget.id ||
        instrument(target) != instrument(oldTarget) || bool(source.rack) != bool(before.rack))
        return false;
    if (source.rack && (edge.rackNotes < 0 || old.rackNotes < 0 ||
                        source.rack->notesOut[static_cast<std::size_t>(edge.rackNotes)].id !=
                            before.rack->notesOut[static_cast<std::size_t>(old.rackNotes)].id))
        return false;
    if (target.rack && (edge.midiNotes < 0 || old.midiNotes < 0 ||
                        target.rack->notes[static_cast<std::size_t>(edge.midiNotes)].id !=
                            oldTarget.rack->notes[static_cast<std::size_t>(old.midiNotes)].id))
        return false;
    return true;
}

void carryEdge(Plan& to, Edge& edge, const Plan& from, const Edge& old) SUPPLE_RT {
    const auto& source = to.nodes[edge.from];
    auto& target = to.nodes[edge.to];
    const auto& oldTarget = from.nodes[old.to];
    const auto voice = [&](int v) { return v < 0 || source.rack ? v : remapSlot(source, v); };
    edge.midiNextVoice = old.midiNextVoice;
    edge.midiAddresses.clear();
    for (std::size_t i = 0; i < old.midiVoices.size(); ++i) {
        const auto& v = old.midiVoices[i];
        if (!v.high && !v.delivered) continue;
        const int at = voice(static_cast<int>(i));
        // Its source voice ended: what it delivered ends as an orphan.
        if (at < 0 || static_cast<std::size_t>(at) >= edge.midiVoices.size()) continue;
        auto& carried = edge.midiVoices[static_cast<std::size_t>(at)];
        carried = v;
        if (!v.delivered) continue;
        if (target.rack) {
            if (edge.midiAddresses.size() < edge.midiAddresses.capacity())
                edge.midiAddresses.push_back(v.address);
            continue;
        }
        const auto oldInput = static_cast<std::size_t>(old.midiSourceBase) + i;
        const int oldSlot =
            oldInput < oldTarget.midiInput.size() ? oldTarget.midiInput[oldInput] : -1;
        const int slot = oldSlot < 0 ? -1 : remapSlot(target, oldSlot);
        if (slot < 0) {
            carried.delivered = false;  // the target lost it: unmuting delivers it again
            continue;
        }
        const auto input = edge.midiSourceBase + at;
        auto& held = target.midiHeld[static_cast<std::size_t>(slot)];
        held.orphan = false;
        held.input = input;
        target.midiInput[static_cast<std::size_t>(input)] = slot;
        target.midiRouted[static_cast<std::size_t>(input - target.midiRoutedBase)] = {held.route,
                                                                                      held.address};
    }
    bool compatible = true;
    for (std::size_t i = 0; i < old.midiCount; ++i) {
        auto pending = old.midiQueue[(old.midiHead + i) % old.midiQueue.size()];
        if (pending.voice >= 0) {
            pending.voice = voice(pending.voice);
            if (pending.voice < 0 ||
                static_cast<std::size_t>(pending.voice) >= edge.midiVoices.size())
                continue;
        }
        if (edge.midiCount == edge.midiQueue.size()) {
            compatible = false;
            break;
        }
        edge.midiQueue[edge.midiCount++] = pending;
    }
    edge.midiReconcile = bool(target.rack);
    edge.midiStarted = old.midiStarted;
    edge.midiMuted = old.midiMuted;
    edge.midiEpoch = source.rack ? source.rack->midiEpoch : source.midiEpoch;
    edge.midiRelease = !compatible;
}
}  // namespace

// Adoption, on the audio thread: bounded by the voices held (the pools and
// send voices), never by the notes in the timeline. A send that changed
// (route, tap, channels, delay, ends) starts afresh; what the old one
// delivered ends as orphans downstream, then restarts: release and retrigger.
void carryRackMidi(Plan& to, Plan& from, int toClass, int fromClass) SUPPLE_RT {
    for (auto& node : to.nodes) {
        if (toClass >= 0 && node.live != bool(toClass)) continue;
        node.midiRemap.clear();
        if (node.rack) continue;
        if (const Node* old = continued(from, node, fromClass)) carryPool(node, *old);
    }
    for (auto& node : to.nodes) {
        if (toClass >= 0 && node.live != bool(toClass)) continue;
        const Node* old = continued(from, node, fromClass);
        if (!old) continue;
        for (std::size_t ei : node.in) {
            auto& edge = to.edges[ei];
            if (!edge.midi) continue;
            for (std::size_t oi : old->in)
                if (sameSend(to, edge, from, from.edges[oi])) {
                    carryEdge(to, edge, from, from.edges[oi]);
                    break;
                }
        }
    }
}
}  // namespace supple::engine::detail
