// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include "rack_runtime.h"

namespace supple::engine::detail {
namespace {
constexpr std::size_t kMaxMidiEvents = 1 << 20;
void emit(Stage& stage, Edge& edge, int voice, std::uint32_t frame, bool on) SUPPLE_RT {
    auto& v = edge.midiVoices[static_cast<std::size_t>(voice)];
    host::Event event;
    event.kind = on ? host::Event::Kind::NoteOn : host::Event::Kind::NoteOff;
    event.time = frame;
    event.channel = static_cast<std::int16_t>(v.channel);
    event.key = static_cast<std::int16_t>(v.key);
    event.velocity = on ? v.velocity / 127.0 : 0.0;
    event.noteId = edge.midiSourceBase + voice;
    if (stage.blockEvents.size() < stage.blockEvents.capacity()) stage.blockEvents.push_back(event);
    v.delivered = on;
}
void queue(Edge& edge, std::int64_t sample, std::uint8_t status, int key, int velocity,
           int voice) SUPPLE_RT {
    if (edge.midiFrom && (status & 15) != edge.midiFrom - 1) return;
    if (edge.midiCount >= edge.midiQueue.size()) return;  // capacity proved in prepareRackMidi
    auto& event = edge.midiQueue[(edge.midiHead + edge.midiCount++) % edge.midiQueue.size()];
    event = {sample, status, static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity),
             static_cast<std::uint8_t>(voice)};
    if (edge.midiTo) event.status = static_cast<std::uint8_t>((status & 0xf0) | (edge.midiTo - 1));
}
}  // namespace

Status prepareRackMidi(Plan& p) {
    for (auto& node : p.nodes) {
        if (node.noteTarget < 0) continue;
        auto& stage = node.chain[static_cast<std::size_t>(node.noteTarget)];
        // A nonzero render start removes old events without renumbering the
        // remaining item notes. Event count is therefore not an ID bound.
        stage.rackMidiBase = 0;
        for (const auto& timed : node.events)
            stage.rackMidiBase = std::max(stage.rackMidiBase, timed.event.noteId + 1);
        std::size_t extra = 0;
        for (std::size_t index : node.in) {
            auto& edge = p.edges[index];
            if (edge.rackNotes < 0) continue;
            const auto& output =
                p.nodes[edge.from].rack->notesOut[static_cast<std::size_t>(edge.rackNotes)];
            const auto capacity = static_cast<std::uint64_t>(rack::kChannels) *
                                  (static_cast<std::uint64_t>(edge.delay.length) + 2 * p.block + 2);
            extra += static_cast<std::size_t>(rack::kChannels) * (p.block + 2);
            if (capacity > kMaxMidiEvents || extra > kMaxMidiEvents)
                return Error("engine.midi_budget", "Note Out routes exceed the MIDI event budget",
                             "/routes/" + edge.route->str(),
                             "reduce plug-in latency or route fewer Note Outs to this instrument");
            edge.midiQueue.resize(static_cast<std::size_t>(capacity));
            edge.midiSourceBase =
                stage.rackMidiBase + static_cast<int>(stage.rackNoteSources.size());
            for (int voice = 0; voice < rack::kChannels; ++voice)
                stage.rackNoteSources.push_back(
                    {output.id, *edge.route, voice, edge.midiFrom, edge.midiTo, edge.delay.length});
        }
        stage.blockEvents.reserve(stage.blockEvents.capacity() + extra);
        stage.voices.reserve(stage.voices.capacity() + stage.rackNoteSources.size());
        stage.pendingVoices.reserve(stage.voices.capacity());
    }
    return {};
}

void addRackMidi(Plan& p, Node& target, std::int64_t pos, std::uint32_t frames,
                 bool playing) SUPPLE_RT {
    if (target.noteTarget < 0) return;
    auto& stage = target.chain[static_cast<std::size_t>(target.noteTarget)];
    bool appended = false;
    for (std::size_t index : target.in) {
        auto& edge = p.edges[index];
        if (edge.rackNotes < 0) continue;
        const auto& rack = *p.nodes[edge.from].rack;
        const auto& output = rack.notesOut[static_cast<std::size_t>(edge.rackNotes)];
        if (!playing || edge.midiRelease || edge.midiEpoch != rack.midiEpoch) {
            for (int c = 0; c < rack::kChannels; ++c) {
                auto& voice = edge.midiVoices[static_cast<std::size_t>(c)];
                if (voice.delivered) emit(stage, edge, c, 0, false);
                voice = {};
            }
            edge.midiCount = edge.midiHead = 0;
            edge.midiEpoch = rack.midiEpoch;
            edge.midiRelease = false;
            appended = true;
        }
        if (!playing) {
            edge.midiStarted = false;
            continue;
        }
        if (!edge.midiStarted) {
            edge.midiStarted = true;
            for (int c = 0; c < rack::kChannels; ++c) {
                const auto& v = output.before[static_cast<std::size_t>(c)];
                const bool startsWithEvent =
                    std::any_of(output.events.begin(),
                                output.events.begin() + static_cast<std::ptrdiff_t>(output.count),
                                [&](const auto& e) { return e.voice == c && e.midi.frame == 0; });
                if (v.high && !startsWithEvent)
                    queue(edge, pos + edge.delay.length,
                          static_cast<std::uint8_t>(0x90 | v.channel), v.key, v.velocity, c);
            }
        }
        for (std::size_t i = 0; i < output.count; ++i) {
            const auto& e = output.events[i];
            queue(edge, pos + e.midi.frame + edge.delay.length, e.midi.status, e.midi.data1,
                  e.midi.data2, e.voice);
        }
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const bool muted =
                edge.autoMute.active() ? edge.autoMute.at(pos + frame) >= 0.5 : edge.muted;
            while (edge.midiCount && edge.midiQueue[edge.midiHead].sample <= pos + frame) {
                const auto e = edge.midiQueue[edge.midiHead];
                edge.midiHead = (edge.midiHead + 1) % edge.midiQueue.size();
                --edge.midiCount;
                auto& voice = edge.midiVoices[e.voice];
                if (voice.delivered) emit(stage, edge, e.voice, frame, false);
                voice.high = (e.status & 0xf0) == 0x90 && e.velocity != 0;
                voice.key = e.key;
                voice.channel = e.status & 15;
                voice.velocity = e.velocity;
                if (voice.high && !muted) emit(stage, edge, e.voice, frame, true);
                appended = true;
            }
            if (muted != edge.midiMuted) {
                edge.midiMuted = muted;
                for (int c = 0; c < rack::kChannels; ++c) {
                    auto& voice = edge.midiVoices[static_cast<std::size_t>(c)];
                    if (muted && voice.delivered)
                        emit(stage, edge, c, frame, false);
                    else if (!muted && voice.high && !voice.delivered)
                        emit(stage, edge, c, frame, true);
                }
                appended = true;
            }
        }
    }
    if (appended) {
        // A total ordering at ties is independent of the host's block splits.
        // Keep explicit releases first: prepareNoteIds recognizes that prefix.
        auto rank = [](const host::Event& e) {
            return e.kind == host::Event::Kind::NoteOff  ? 0
                   : e.kind == host::Event::Kind::NoteOn ? 2
                                                         : 1;
        };
        std::sort(stage.blockEvents.begin() + static_cast<std::ptrdiff_t>(stage.releases),
                  stage.blockEvents.end(), [&](const auto& a, const auto& b) {
                      if (a.time != b.time) return a.time < b.time;
                      if (rank(a) != rank(b)) return rank(a) < rank(b);
                      return std::tuple{a.noteId, a.port, a.channel, a.key} <
                             std::tuple{b.noteId, b.port, b.channel, b.key};
                  });
    }
}

void carryRackMidi(Plan& to, Plan& from, int liveClass) SUPPLE_RT {
    for (auto& edge : to.edges) {
        if (edge.rackNotes < 0 || (liveClass >= 0 && to.nodes[edge.to].live != bool(liveClass)))
            continue;
        const auto& target = to.nodes[edge.to];
        if (target.noteTarget < 0) continue;
        for (auto& old : from.edges) {
            const auto& oldTarget = from.nodes[old.to];
            if (oldTarget.noteTarget < 0 ||
                target.chain[static_cast<std::size_t>(target.noteTarget)].instance !=
                    oldTarget.chain[static_cast<std::size_t>(oldTarget.noteTarget)].instance)
                continue;
            if ((liveClass >= 0 && from.nodes[old.to].live != bool(liveClass)) ||
                old.rackNotes < 0 || edge.route != old.route || edge.midiFrom != old.midiFrom ||
                edge.midiTo != old.midiTo || edge.delay.length != old.delay.length ||
                to.nodes[edge.to].id != from.nodes[old.to].id ||
                to.nodes[edge.from]
                        .rack->notesOut[static_cast<std::size_t>(edge.rackNotes)]
                        .module != from.nodes[old.from]
                                       .rack->notesOut[static_cast<std::size_t>(old.rackNotes)]
                                       .module)
                continue;
            if (edge.midiQueue.size() != old.midiQueue.size()) continue;
            std::swap(edge.midiQueue, old.midiQueue);
            edge.midiHead = old.midiHead;
            edge.midiCount = old.midiCount;
            edge.midiVoices = old.midiVoices;
            edge.midiStarted = old.midiStarted;
            edge.midiMuted = old.midiMuted;
            edge.midiEpoch = to.nodes[edge.from].rack->midiEpoch;
            break;
        }
    }
}
}  // namespace supple::engine::detail
