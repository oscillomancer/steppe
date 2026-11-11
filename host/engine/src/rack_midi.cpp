// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <cmath>
#include <tuple>
#include "rack_runtime.h"

namespace supple::engine::detail {
namespace {
constexpr std::size_t kMaxMidiEvents = 1 << 20;
std::uint64_t routeNamespace(const Edge& edge) SUPPLE_RT {
    // Widen before adding the reserved timeline/device namespaces.
    return std::uint64_t{edge.route->n} + 2;
}
bool note(const host::Event& e) SUPPLE_RT {
    return e.kind == host::Event::Kind::NoteOn || e.kind == host::Event::Kind::NoteOff ||
           e.kind == host::Event::Kind::NoteChoke;
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
bool bytes(const host::Event& e, std::uint8_t* out) SUPPLE_RT {
    if (note(e)) {
        out[0] = static_cast<std::uint8_t>((e.kind == host::Event::Kind::NoteOn ? 0x90 : 0x80) |
                                           e.channel);
        out[1] = static_cast<std::uint8_t>(e.key);
        out[2] = e.kind == host::Event::Kind::NoteOn
                     ? static_cast<std::uint8_t>(
                           std::clamp<int>(static_cast<int>(std::lround(e.velocity * 127)), 1, 127))
                     : 0;
        return true;
    }
    if (e.kind != host::Event::Kind::Midi) return false;
    std::copy_n(e.midi, 3, out);
    return true;
}
void deliver(Node& target, Edge& edge, host::Event e, int voice) SUPPLE_RT {
    e.noteId = voice < 0 ? -1 : edge.midiSourceBase + voice;
    if (target.rack) {
        auto& feed = target.rack->notes[static_cast<std::size_t>(edge.midiNotes)];
        std::uint8_t m[3];
        if (bytes(e, m) && feed.block.size() < feed.block.capacity())
            feed.block.push_back(
                {e.time, m[0], m[1], m[2], false, routeNamespace(edge),
                 voice < 0 ? -1 : edge.midiVoices[static_cast<std::size_t>(voice)].address});
    } else if (target.midiStage.blockEvents.size() < target.midiStage.blockEvents.capacity()) {
        target.midiStage.blockEvents.push_back(e);
    }
    if (!target.rack && target.routedMidi.size() < target.routedMidi.capacity()) {
        RoutedMidiEvent midi;
        midi.frame = e.time;
        midi.route = *edge.route;
        midi.voice = voice < 0 ? 0
                               : edge.midiVoices[static_cast<std::size_t>(voice)]
                                     .address;  // all delivered events use the internal input clock
        if (bytes(e, midi.bytes)) target.routedMidi.push_back(midi);
    }
}
void emit(Node& target, Edge& edge, int voice, std::uint32_t frame, bool on) SUPPLE_RT {
    auto& v = edge.midiVoices[static_cast<std::size_t>(voice)];
    deliver(target, edge,
            message(static_cast<std::uint8_t>((on ? 0x90 : 0x80) | v.channel), v.key,
                    on ? v.velocity : 0, voice, frame),
            voice);
    v.delivered = on;
}
void queue(Edge& edge, std::int64_t sample, host::Event e, int voice) SUPPLE_RT {
    const int channel = e.kind == host::Event::Kind::Midi ? e.midi[0] & 15 : e.channel;
    if (edge.midiFrom && channel != edge.midiFrom - 1) return;
    if (edge.midiCount == edge.midiQueue.size()) return;  // proved in prepareRackMidi
    if (edge.midiTo) {
        e.channel = static_cast<std::int16_t>(edge.midiTo - 1);
        if (e.kind == host::Event::Kind::Midi && e.midi[0] < 0xf0)
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
}  // namespace

Status prepareRackMidi(Plan& p) {
    for (auto& node : p.nodes) {
        auto budget = [&](std::size_t count, const std::string& path) -> Status {
            if (count > kMaxMidiEvents)
                return Error("engine.midi_budget", "MIDI sends exceed the prepared event budget",
                             path, "reduce MIDI fan-in, note density or plug-in latency");
            return {};
        };
        // Source identities use musical origins, so inserting earlier notes or
        // an unrelated route cannot renumber a held voice's address.
        for (const auto& source : node.midiStage.noteSources) {
            RackNoteSource identity;
            identity.module = node.id;
            identity.origin = source.origin;
            identity.voice = static_cast<int>(source.ordinal);
            identity.port = source.port;
            identity.channel = source.channel;
            identity.key = source.key;
            node.midiSources.push_back(std::move(identity));
        }
        node.midiLiveBase = static_cast<int>(node.midiSources.size());
        if (node.midiIn) {
            for (int i = 0; i < 16 * 128; ++i) {
                RackNoteSource source;
                source.module = node.id;
                source.voice = i;
                source.origin.item = UINT64_MAX - 1;
                node.midiSources.push_back(std::move(source));
            }
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
        std::size_t capacity =
            window(static_cast<std::int64_t>(p.block) * (p.rateEnabled ? 4 : 1)) +
            node.midiSources.size() + 1;
        std::size_t rate = window(p.rateEnabled ? 4 : 1) + (node.midiIn ? kMaxMidiInputEvents : 0);
        if (node.midiIn) capacity += kMaxMidiInputEvents;
        for (std::size_t index : node.in) {
            auto& edge = p.edges[index];
            if (!edge.midi) continue;
            const auto& source = p.nodes[edge.from];
            const std::size_t voices =
                edge.rackNotes >= 0 ? rack::kChannels : source.midiSources.size();
            const std::size_t perBlock =
                edge.rackNotes >= 0 ? static_cast<std::size_t>(rack::kChannels) * (p.block + 2)
                                    : source.midiPreFx.capacity();
            const std::size_t perFrame =
                edge.rackNotes >= 0 ? rack::kChannels : source.midiEventRate;
            const std::uint64_t queued =
                2 * static_cast<std::uint64_t>(perBlock) +
                static_cast<std::uint64_t>(
                    edge.delay.length +
                    (edge.rackNotes < 0 && edge.tap != model::Tap::PreFx ? source.latChain : 0)) *
                    perFrame +
                voices;
            SUPPLE_TRY(budget(static_cast<std::size_t>(queued), "/routes/" + edge.route->str()));
            SUPPLE_TRY(budget(node.midiSources.size() + voices, "/routes/" + edge.route->str()));
            edge.midiQueue.resize(static_cast<std::size_t>(queued));
            edge.midiVoices.resize(voices);
            for (auto& voice : edge.midiVoices)
                voice.address = static_cast<int>(edge.midiNextVoice++);
            edge.midiAddresses.resize(voices);
            for (std::size_t voice = 0; voice < voices; ++voice)
                edge.midiAddresses[voice] = edge.midiVoices[voice].address;
            if (node.rack)
                node.rack->notes[static_cast<std::size_t>(edge.midiNotes)].routes.push_back(
                    routeNamespace(edge));
            edge.midiSourceBase = static_cast<int>(node.midiSources.size());
            for (std::size_t voice = 0; voice < voices; ++voice) {
                RackNoteSource identity;
                if (edge.rackNotes >= 0) {
                    identity.module =
                        source.rack->notesOut[static_cast<std::size_t>(edge.rackNotes)].id;
                    identity.voice = static_cast<int>(voice);
                } else
                    identity = source.midiSources[voice];
                identity.route = *edge.route;
                identity.path.push_back(*edge.route);
                identity.from = edge.midiFrom;
                identity.to = edge.midiTo;
                identity.delay = edge.delay.length;
                identity.tap = edge.tap;
                node.midiSources.push_back(identity);
            }
            auto points = edge.autoMute.points.size() +
                          (edge.tap == model::Tap::PostFader ? edge.stripMute.points.size() : 0);
            for (const auto& vca : edge.vcas.values) points += vca.gate.points.size();
            const auto transitions = points ? std::min<std::size_t>(p.block, points + 2) : 1;
            capacity += 2 * perBlock + (transitions + 2) * voices;
            rate += 2 * perFrame + (points ? voices : 0);
        }
        node.midiEventRate = rate;
        SUPPLE_TRY(budget(capacity, "/tracks/" + node.id.str()));
        node.midiPreFx.reserve(capacity);
        node.midiPostFx.reserve(capacity);
        node.midiSortScratch.reserve(capacity);
        node.midiOrder.prepare(capacity, p.block);
        node.midiHeld.resize(node.midiSources.size());
        node.midiBefore.resize(node.midiSources.size());
        node.routedMidi.reserve(capacity);
        auto& stream = node.midiStage;
        stream.blockEvents.reserve(capacity);
        stream.voices.reserve(node.midiSources.size());
        stream.pendingVoices.reserve(node.midiSources.size());
        if (node.noteTarget >= 0) {
            auto& stage = node.chain[static_cast<std::size_t>(node.noteTarget)];
            stage.rackMidiBase = node.midiLiveBase;
            stage.rackNoteSources.assign(node.midiSources.begin() + node.midiLiveBase,
                                         node.midiSources.end());
            // Devices retain tuple addressing at the instrument; sends have ids.
            stage.blockEvents.reserve(stage.blockEvents.capacity() + capacity);
            const auto routedSources = stage.rackNoteSources.size() - (node.midiIn ? 16 * 128 : 0);
            stage.voices.reserve(stage.voices.capacity() + routedSources);
            stage.pendingVoices.reserve(stage.voices.capacity());
        }
        if (node.rack) {
            for (auto& feed : node.rack->notes) {
                feed.block.reserve(feed.block.capacity() + capacity);
                feed.sortScratch.reserve(feed.block.capacity());
                feed.order.prepare(feed.block.capacity(), p.block);
            }
        }
    }
    return {};
}

void beginTrackMidi(Node& node, bool playing, bool hearDevice) SUPPLE_RT {
    node.midiStage.blockEvents.clear();
    node.midiStage.voices.clear();
    std::copy(node.midiHeld.begin(), node.midiHeld.end(), node.midiBefore.begin());
    for (int i = 0; i < node.midiLiveBase; ++i) {
        const auto& v = node.midiHeld[static_cast<std::size_t>(i)];
        if (!v.high) continue;
        if (!playing || node.midiReleaseTimeline) {
            node.midiStage.blockEvents.push_back(
                message(static_cast<std::uint8_t>(0x80 | v.channel), v.key, 0, i));
        } else {
            HeldVoice voice;
            voice.sourceId = voice.noteId = i;
            voice.channel = static_cast<std::int16_t>(v.channel);
            voice.key = static_cast<std::int16_t>(v.key);
            node.midiStage.voices.push_back(voice);
        }
    }
    if (node.midiIn && !hearDevice)
        for (int i = node.midiLiveBase; i < node.midiLiveBase + 16 * 128; ++i) {
            const auto& v = node.midiHeld[static_cast<std::size_t>(i)];
            if (v.high)
                node.midiStage.blockEvents.push_back(
                    message(static_cast<std::uint8_t>(0x80 | v.channel), v.key, 0, i));
        }
    node.midiReleaseTimeline = false;
}
void finishTrackMidi(Node& node) SUPPLE_RT {
    auto& events = node.midiStage.blockEvents;
    for (auto& e : events) {
        if (note(e) && e.noteId < 0 && node.midiIn)
            e.noteId = node.midiLiveBase + e.channel * 128 + e.key;
    }
    orderMidi(node);
    const auto count = events.size();
    auto& expanded = node.midiSortScratch;
    expanded.clear();
    for (std::size_t index = 0; index < count; ++index) {
        const auto e = events[index];
        if (e.kind == host::Event::Kind::Midi && (e.midi[0] & 0xf0) == 0xb0 &&
            (e.midi[1] == 120 || e.midi[1] == 123)) {
            for (std::size_t voice = 0; voice < node.midiHeld.size(); ++voice) {
                auto& held = node.midiHeld[voice];
                if (!held.high || held.channel != (e.midi[0] & 15)) continue;
                // Insert releases at their controller, before a same-frame
                // reattack. Keep room for every remaining original event.
                if (expanded.size() + count - index < expanded.capacity())
                    expanded.push_back(message(static_cast<std::uint8_t>(0x80 | held.channel),
                                               held.key, 0, static_cast<int>(voice), e.time));
                held.high = false;
            }
        }
        expanded.push_back(e);
        if (!note(e) || e.noteId < 0 || static_cast<std::size_t>(e.noteId) >= node.midiHeld.size())
            continue;
        auto& v = node.midiHeld[static_cast<std::size_t>(e.noteId)];
        v.high = e.kind == host::Event::Kind::NoteOn;
        v.key = e.key;
        v.channel = e.channel;
        v.velocity = std::clamp<int>(static_cast<int>(std::lround(e.velocity * 127)), 1, 127);
    }
    events.swap(expanded);
    orderMidi(node);
    node.midiPreFx.assign(events.begin(), events.end());
    node.midiPostFx.assign(node.midiPreFx.begin(), node.midiPreFx.end());
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
                const int address = v.address;
                v = {};
                v.address = address;
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
                        queue(edge, pos + edge.delay.length,
                              message(static_cast<std::uint8_t>(0x90 | v.channel), v.key,
                                      v.velocity, c),
                              c);
                }
            } else {
                for (std::size_t c = 0; c < source.midiBefore.size(); ++c) {
                    const auto& v = source.midiBefore[c];
                    const auto& stream =
                        edge.tap == model::Tap::PreFx ? source.midiPreFx : source.midiPostFx;
                    const bool starts =
                        std::any_of(stream.begin(), stream.end(), [&](const auto& e) {
                            return e.noteId == static_cast<int>(c) && e.time == 0;
                        });
                    if (v.high && !starts)
                        queue(edge,
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
                queue(edge, pos + e.midi.frame + edge.delay.length,
                      message(e.midi.status, e.midi.data1, e.midi.data2, e.voice), e.voice);
            }
        } else {
            const auto& stream =
                edge.tap == model::Tap::PreFx ? source.midiPreFx : source.midiPostFx;
            for (const auto& e : stream)
                queue(edge,
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
                v.velocity =
                    std::clamp<int>(static_cast<int>(std::lround(e.velocity * 127)), 1, 127);
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

void carryRackMidi(Plan& to, Plan& from, int toClass, int fromClass) SUPPLE_RT {
    for (auto& node : to.nodes) {
        if (toClass >= 0 && node.live != bool(toClass)) continue;
        for (const auto& old : from.nodes) {
            if (old.id != node.id || (fromClass >= 0 && old.live != bool(fromClass))) continue;
            node.midiEpoch = old.midiEpoch;
            for (std::size_t i = 0; i < node.midiSources.size(); ++i) {
                const auto at =
                    std::find(old.midiSources.begin(), old.midiSources.end(), node.midiSources[i]);
                if (at != old.midiSources.end())
                    node.midiHeld[i] =
                        old.midiHeld[static_cast<std::size_t>(at - old.midiSources.begin())];
            }
            break;
        }
    }
    for (auto& edge : to.edges) {
        if (!edge.midi || (toClass >= 0 && to.nodes[edge.to].live != bool(toClass))) continue;
        for (const auto& old : from.edges) {
            if (!old.midi || edge.route != old.route || edge.tap != old.tap ||
                edge.midiFrom != old.midiFrom || edge.midiTo != old.midiTo ||
                edge.delay.length != old.delay.length ||
                (fromClass >= 0 && from.nodes[old.to].live != bool(fromClass)))
                continue;
            const auto& source = to.nodes[edge.from];
            const auto& before = from.nodes[old.from];
            const auto& target = to.nodes[edge.to];
            const auto& oldTarget = from.nodes[old.to];
            const auto instrument = [](const Node& n) {
                return n.noteTarget < 0 ? nullptr
                                        : n.chain[static_cast<std::size_t>(n.noteTarget)].instance;
            };
            if (source.id != before.id || target.id != oldTarget.id ||
                instrument(target) != instrument(oldTarget) ||
                bool(source.rack) != bool(before.rack))
                continue;
            if (source.rack &&
                (edge.rackNotes < 0 || old.rackNotes < 0 ||
                 source.rack->notesOut[static_cast<std::size_t>(edge.rackNotes)].id !=
                     before.rack->notesOut[static_cast<std::size_t>(old.rackNotes)].id))
                continue;
            if (target.rack &&
                (edge.midiNotes < 0 || old.midiNotes < 0 ||
                 target.rack->notes[static_cast<std::size_t>(edge.midiNotes)].id !=
                     oldTarget.rack->notes[static_cast<std::size_t>(old.midiNotes)].id))
                continue;
            const auto mapVoice = [&](int voice) {
                if (voice < 0 || source.rack) return voice;
                if (static_cast<std::size_t>(voice) >= before.midiSources.size()) return -2;
                const auto found = std::find(source.midiSources.begin(), source.midiSources.end(),
                                             before.midiSources[static_cast<std::size_t>(voice)]);
                return found == source.midiSources.end()
                           ? -2
                           : static_cast<int>(found - source.midiSources.begin());
            };
            bool compatible = true;
            edge.midiNextVoice = old.midiNextVoice;
            for (std::size_t i = 0; i < edge.midiVoices.size(); ++i) {
                int previous = static_cast<int>(i);
                if (!source.rack) {
                    const auto found = std::find(before.midiSources.begin(),
                                                 before.midiSources.end(), source.midiSources[i]);
                    previous = found == before.midiSources.end()
                                   ? -1
                                   : static_cast<int>(found - before.midiSources.begin());
                }
                if (previous >= 0 && static_cast<std::size_t>(previous) < old.midiVoices.size())
                    edge.midiVoices[i] = old.midiVoices[static_cast<std::size_t>(previous)];
                else
                    edge.midiVoices[i].address =
                        static_cast<int>(edge.midiNextVoice++ & 0x7fffffffu);
            }
            for (std::size_t i = 0; i < edge.midiVoices.size(); ++i)
                edge.midiAddresses[i] = edge.midiVoices[i].address;
            edge.midiReconcile = bool(target.rack);
            for (std::size_t i = 0; i < old.midiCount; ++i) {
                auto pending = old.midiQueue[(old.midiHead + i) % old.midiQueue.size()];
                pending.voice = mapVoice(pending.voice);
                if (pending.voice == -2) continue;
                if (edge.midiCount == edge.midiQueue.size()) {
                    compatible = false;
                    break;
                }
                edge.midiQueue[edge.midiCount++] = pending;
            }
            edge.midiStarted = old.midiStarted;
            edge.midiMuted = old.midiMuted;
            edge.midiEpoch = source.rack ? source.rack->midiEpoch : source.midiEpoch;
            edge.midiRelease = !compatible;
            break;
        }
    }
}
}  // namespace supple::engine::detail
