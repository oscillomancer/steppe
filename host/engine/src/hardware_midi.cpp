// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "hardware_midi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include "rack_runtime.h"
namespace supple::engine::detail {
namespace {
bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}
bool later(const auto& a, const auto& b) {
    return a.at > b.at || (a.at == b.at && a.order > b.order);
}
MidiOutputEvent convert(const host::Event& event) SUPPLE_RT {
    MidiOutputEvent result;
    result.frame = event.time;
    if (event.kind == host::Event::Kind::NoteOn || event.kind == host::Event::Kind::NoteOff ||
        event.kind == host::Event::Kind::NoteChoke) {
        result.size = 3;
        result.bytes[0] = static_cast<std::uint8_t>(
            (event.kind == host::Event::Kind::NoteOn ? 0x90 : 0x80) | (event.channel & 15));
        result.bytes[1] = static_cast<std::uint8_t>(event.key);
        result.bytes[2] = event.kind == host::Event::Kind::NoteOn
                              ? static_cast<std::uint8_t>(
                                    std::clamp<long>(std::lround(event.velocity * 127), 1, 127))
                              : 0;
    } else if (event.kind == host::Event::Kind::Midi) {
        std::copy_n(event.midi, 3, result.bytes);
        const auto status = result.bytes[0];
        result.size = status < 0xf0                      ? ((status & 0xe0) == 0xc0 ? 2 : 3)
                      : status == 0xf2                   ? 3
                      : status == 0xf1 || status == 0xf3 ? 2
                      : status >= 0xf6                   ? 1
                                                         : 0;
    }
    return result;
}
}  // namespace
Status prepareHardwareMidi(Plan& plan, Backend& backend, const MidiPortPreferences& inputs,
                           const MidiPortPreferences& outputs) {
    const auto ports = backend.midiOutputPorts(), sources = backend.midiPorts();
    const auto latency = backend.midiOutputLatency();
    for (std::size_t p = 0; p < 16; ++p) {
        plan.midiInputsSettings[p] = midiPortSettings(inputs, p < sources.size() ? sources[p] : "");
        plan.midiOutputsSettings[p] = midiPortSettings(outputs, p < ports.size() ? ports[p] : "");
    }
    for (auto& node : plan.nodes)
        for (std::size_t p = 0; p < 16; ++p)
            if (!plan.midiInputsSettings[p].enabled) {
                if (node.midiIn) node.midiIn->ports &= ~(std::uint64_t{1} << p);
                if (node.rack)
                    for (auto& feed : node.rack->notes) {
                        feed.ports &= ~(std::uint64_t{1} << p);
                        feed.sourcePorts &= ~(std::uint64_t{1} << p);
                    }
            }
    if (plan.hardwareMidi.size() > kHardwareMidiRoutes)
        return Error("engine.midi_budget", "too many hardware MIDI routes", "/routes",
                     "use at most 256 hardware MIDI routes");
    std::int64_t lead = 0;
    for (auto& pointer : plan.hardwareMidi) {
        auto& route = *pointer;
        const auto& node = plan.nodes[route.source];
        route.sourceId = node.id;
        if (route.rackNotes >= 0)
            route.noteOutId = node.rack->notesOut[static_cast<std::size_t>(route.rackNotes)].id;
        for (std::size_t p = 0; p < ports.size() && p < 16; ++p)
            if (sameName(ports[p], route.device)) {
                if (route.port >= 0)
                    return Error("engine.midi_port", "MIDI output name is ambiguous",
                                 "/routes/" + route.id.str() + "/to/midi_device",
                                 "rename duplicate MIDI destinations before starting the engine");
                route.port = static_cast<int>(p);
            }
        if (route.port < 0) {
            plan.warnings.push_back(route.id.str() + ": MIDI output " + route.device +
                                    " is absent; connect it and restart the engine");
            continue;
        }
        const auto p = static_cast<std::size_t>(route.port);
        if (!plan.midiOutputsSettings[p].enabled) {
            route.port = -1;
            continue;
        }
        const auto tapped =
            route.rackNotes >= 0
                ? node.rack->notesOut[static_cast<std::size_t>(route.rackNotes)].latency
            : route.tap == model::Tap::PreFx ? 0
                                             : node.latChain;
        // The channel's post-FX bus retains the chain's input clock (ADR-0068).
        // Its tapped latency is added at delivery, cancelling its subtraction
        // here. Rack Note Out timestamps already include its prepared latency.
        const auto eventClockLatency = route.rackNotes >= 0 ? tapped : 0;
        route.delay = plan.outputLatency - node.timelineShift - eventClockLatency +
                      (p < latency.size() ? latency[p] : 0) +
                      time::samplesAt(plan.midiOutputsSettings[p].offsetMs / 1000., plan.rate);
        lead = std::max(lead, -route.delay);
        // Evaluate gates on the stream's clock, before its device delay. Node
        // curves already carry the fader shift; channel streams precede it.
        route.muteCurve.shift(node.timelineShift + eventClockLatency);
        route.muteCurve.timeline =
            plan.rateEnabled ? plan.timelines[node.live ? 1 : 0].get() : nullptr;
        route.sourceMute = node.autoMute;
        route.sourceMute.shift(eventClockLatency - node.latChain);
        route.vcas = node.vcas;
        route.vcas.shift(eventClockLatency - node.latChain);
        for (auto& v : route.vcas.values) {
            v.volume.points.clear();
            v.volumeDb = 0;
        }
    }
    if (lead > static_cast<std::int64_t>(plan.rate * kHardwareMidiLeadSeconds))
        return Error("engine.midi_lead", "MIDI advance exceeds one second of prepared audio lead",
                     "/midi_outputs",
                     "increase the MIDI output timing offset or reduce the port's advance/latency");
    plan.hardwareLead = static_cast<std::uint32_t>(lead);
    for (auto& route : plan.hardwareMidi) {
        route->delay += lead;
        if (route->delay > static_cast<std::int64_t>(plan.rate * 10))
            return Error("engine.midi_delay", "hardware MIDI delay exceeds ten seconds", "/routes",
                         "reduce plugin delay or the output timing offset");
    }
    if (lead)
        plan.warnings.push_back("Hardware MIDI alignment adds " + std::to_string(lead) +
                                " frames of prepared audio lead");
    return {};
}
HardwareMidiRuntime::HardwareMidiRuntime() = default;
void HardwareMidiRuntime::prepare(double rate, std::uint32_t channels) {
    historyRate_ = rate;
    historyChannels_ = channels;
    fadeFrames_ = static_cast<std::uint32_t>(std::max(1., std::round(rate * .005)));
}
void HardwareMidiRuntime::ensureHistory() {
    if (historyReady_.load(std::memory_order_acquire)) return;
    audioDelay_.assign(
        historyChannels_,
        std::vector<float>(
            static_cast<std::size_t>(std::ceil(historyRate_ * kHardwareMidiLeadSeconds)) + 1));
    historyReady_.store(true, std::memory_order_release);
}
bool HardwareMidiRuntime::releaseHistory(bool stopped) {
    if (!stopped && historyBusy_.load(std::memory_order_acquire)) return false;
    historyReady_.store(false, std::memory_order_release);
    // No retained plan can reactivate the buffer, and audio has acknowledged
    // its zero-lead state. Allocation and reclamation stay on the owner thread.
    std::vector<std::vector<float>>().swap(audioDelay_);
    return true;
}
std::size_t HardwareMidiRuntime::historySamples() const {
    return audioDelay_.empty() ? 0 : audioDelay_.size() * audioDelay_.front().size();
}
std::uint64_t HardwareMidiRuntime::dropped(bool output, std::size_t port) const {
    return port < 16
               ? (output ? outputDrops_[port] : inputDrops_[port]).load(std::memory_order_relaxed)
               : 0;
}
void HardwareMidiRuntime::begin(Backend& backend, std::uint32_t frames) SUPPLE_RT {
    backend_ = &backend;
    frames_ = frames;
    wrote_ = 0;
}
bool HardwareMidiRuntime::write(RouteState& state, const MidiOutputEvent& value) SUPPLE_RT {
    auto event = value;
    event.port = static_cast<std::uint16_t>(state.port);
    if (!backend_->writeMidi(event)) {
        outputDrops_[event.port].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ++wrote_;
    if (event.bytes[0] < 0xf0)
        state.channels |= static_cast<std::uint16_t>(1u << (event.bytes[0] & 15));
    const auto key = std::size_t(event.bytes[0] & 15) * 128 + event.bytes[1];
    if ((event.bytes[0] & 0xf0) == 0x90 && event.bytes[2]) {
        if (state.held[key] < 65535) ++state.held[key];
    } else if ((event.bytes[0] & 0xf0) == 0x80 ||
               ((event.bytes[0] & 0xf0) == 0x90 && !event.bytes[2])) {
        if (state.held[key]) --state.held[key];
    }
    return true;
}
bool HardwareMidiRuntime::release(RouteState& state, std::uint32_t at) SUPPLE_RT {
    for (std::size_t key = 0; key < state.held.size(); ++key)
        while (state.held[key]) {
            if (wrote_ >= kMaxMidiOutputEvents - 512)
                return false;  // reserve bounded whole-port panic capacity
            if (!write(state, {at,
                               0,
                               3,
                               {static_cast<std::uint8_t>(0x80 | (key / 128)),
                                static_cast<std::uint8_t>(key % 128), 0}}))
                return false;
        }
    for (unsigned channel = 0; channel < 16; ++channel)
        if (state.channels & (1u << channel)) {
            if (wrote_ >= kMaxMidiOutputEvents - 512) return false;
            if (!write(state, {at, 0, 3, {static_cast<std::uint8_t>(0xb0 | channel), 64, 0}}))
                return false;
        }
    return true;
}
void HardwareMidiRuntime::adopt(Plan& plan, std::uint64_t epoch, bool playing) SUPPLE_RT {
    const bool boundary = epoch != epoch_ || (playing_ && !playing);
    const bool changed = plan_ != &plan || planGeneration_ != plan.generation;
    if (boundary) {
        // Reset-off ports still owe their delayed releases and mute markers.
        // Stale timeline onsets must not start after a stop or locate. Live
        // onsets keep their device clock. Reset-on routes
        // discard the rest through their generation change below.
        std::size_t keep = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& event = queue_[i].event;
            const bool onset = event.size && (event.bytes[0] & 0xf0) == 0x90 && event.bytes[2];
            if (!onset || !queue_[i].timeline) queue_[keep++] = queue_[i];
        }
        count_ = keep;
        std::make_heap(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(count_),
                       [](const auto& a, const auto& b) { return later(a, b); });
    }
    if (changed) {
        if (!plan_ || (!lead_ && plan.hardwareLead && !fadeLeft_)) {
            // The first lead after an inactive history primes once. No audio
            // work or pre-roll allocation is needed for zero-lead projects.
            readLead_ = plan.hardwareLead;
            audioCursor_ = 0;
            audioFilled_ = 0;
        } else if (readLead_ != plan.hardwareLead && !fadeLeft_) {
            fadeTarget_ = plan.hardwareLead;
            fadeLeft_ = fadeFrames_;
        }
        lead_ = plan.hardwareLead;
        leadReported_.store(lead_, std::memory_order_relaxed);
        historyBusy_.store(lead_ || fadeLeft_, std::memory_order_release);
        for (auto& state : routes_)
            if (state.active) {
                auto found = std::find_if(
                    plan.hardwareMidi.begin(), plan.hardwareMidi.end(), [&](const auto& r) {
                        return r->id.n == state.id && r->sourceId == state.sourceId &&
                               r->noteOutId == state.noteOutId && r->port == state.port &&
                               r->delay == state.delay && r->tap == state.tap &&
                               r->fromChannel == state.from && r->toChannel == state.to &&
                               r->port >= 0;
                    });
                if (found == plan.hardwareMidi.end()) {
                    state.release = true;
                    ++state.generation;
                }
            }
        plan_ = &plan;
        planGeneration_ = plan.generation;
    }
    std::array<std::uint16_t, 16> resetChannels{};
    for (auto& state : routes_)
        if (state.active && state.port >= 0 &&
            !outputControls_[static_cast<std::size_t>(state.port)].enabled) {
            resetChannels[static_cast<std::size_t>(state.port)] |= state.channels;
            if (!state.release) {
                state.release = true;
                ++state.generation;
            }
        }
    for (auto& state : routes_)
        if (state.active)
            state.reset = outputControls_[static_cast<std::size_t>(state.port)].resetOnStop;
    for (auto& state : routes_)
        if (state.active && boundary && state.reset) {
            state.release = true;
            state.started = false;
            ++state.generation;
            resetChannels[static_cast<std::size_t>(state.port)] |= state.channels;
        }
    for (auto& state : routes_)
        if (state.active && state.release && release(state)) {
            state.active = false;
            state.release = false;
        }
    for (std::size_t p = 0; p < 16; ++p)
        for (unsigned channel = 0; channel < 16; ++channel)
            if (resetChannels[p] & (1u << channel)) {
                for (const auto cc : {64, 123})
                    if (!backend_->writeMidi({0,
                                              static_cast<std::uint16_t>(p),
                                              3,
                                              {static_cast<std::uint8_t>(0xb0 | channel),
                                               static_cast<std::uint8_t>(cc), 0}}))
                        outputDrops_[p].fetch_add(1, std::memory_order_relaxed);
            }
    for (const auto& r : plan.hardwareMidi) {
        if (r->port < 0 || !outputControls_[static_cast<std::size_t>(r->port)].enabled) continue;
        auto found = std::find_if(routes_.begin(), routes_.end(), [&](const auto& s) {
            return s.active && !s.release && s.id == r->id.n;
        });
        if (found == routes_.end()) {
            found = std::find_if(routes_.begin(), routes_.end(),
                                 [](const auto& s) { return !s.active; });
            if (found == routes_.end()) {
                outputDrops_[static_cast<std::size_t>(r->port)].fetch_add(
                    1, std::memory_order_relaxed);
                continue;
            }
            const auto generation = found->generation + 1;
            *found = {};
            found->generation = generation;
            found->active = true;
            found->id = r->id.n;
            found->sourceId = r->sourceId;
            found->noteOutId = r->noteOutId;
            found->port = r->port;
            found->delay = r->delay;
            found->tap = r->tap;
            found->from = r->fromChannel;
            found->to = r->toChannel;
            found->started =
                boundary && outputControls_[static_cast<std::size_t>(r->port)].resetOnStop;
        }
        found->reset = outputControls_[static_cast<std::size_t>(r->port)].resetOnStop;
    }
    if (changed) {
        std::size_t keep = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& pending = queue_[i];
            const auto& state = routes_[pending.slot];
            if (state.active && !state.release && state.generation == pending.generation)
                queue_[keep++] = pending;
        }
        count_ = keep;
        std::make_heap(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(count_),
                       [](const auto& a, const auto& b) { return later(a, b); });
    }
    epoch_ = epoch;
    playing_ = playing;
}
void HardwareMidiRuntime::schedule(std::size_t slot, MidiOutputEvent event, std::int64_t delay,
                                   std::uint32_t offset, bool timeline) SUPPLE_RT {
    auto& state = routes_[slot];
    if (count_ == queue_.size()) {
        outputDrops_[static_cast<std::size_t>(state.port)].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_[count_++] = {frame_ + offset + event.frame + delay, order_++, state.generation,
                        static_cast<std::uint16_t>(slot),      event,    timeline};
    std::push_heap(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(count_),
                   [](const auto& a, const auto& b) { return later(a, b); });
}
void HardwareMidiRuntime::collect(Plan& plan, std::int64_t pos, std::uint32_t frames,
                                  std::uint32_t offset, bool playing,
                                  std::uint64_t epoch) SUPPLE_RT {
    adopt(plan, epoch, playing);
    if (!frames) return;
    for (auto& pointer : plan.hardwareMidi) {
        auto& route = *pointer;
        if (route.port < 0 || !outputControls_[static_cast<std::size_t>(route.port)].enabled)
            continue;
        auto state = std::find_if(routes_.begin(), routes_.end(), [&](const auto& s) {
            return s.active && !s.release && s.id == route.id.n;
        });
        if (state == routes_.end()) continue;
        const auto slot = static_cast<std::size_t>(state - routes_.begin());
        auto& source = plan.nodes[route.source];
        auto filter = [&](MidiOutputEvent event) {
            if (event.size && event.bytes[0] < 0xf0) {
                if (state->from && (event.bytes[0] & 15) != state->from - 1) event.size = 0;
                if (state->to)
                    event.bytes[0] =
                        static_cast<std::uint8_t>((event.bytes[0] & 0xf0) | (state->to - 1));
            }
            return event;
        };
        auto logical = [&](const MidiOutputEvent& event, bool timeline = false) {
            if (!event.size) return;
            const auto key = std::size_t(event.bytes[0] & 15) * 128 + event.bytes[1];
            if ((event.bytes[0] & 0xf0) == 0x90 && event.bytes[2]) {
                if (state->logical[key] < 65535) {
                    ++state->logical[key];
                    if (timeline) ++state->logicalTimeline[key];
                }
                state->velocity[key] = event.bytes[2];
            } else if ((event.bytes[0] & 0xf0) == 0x80 ||
                       ((event.bytes[0] & 0xf0) == 0x90 && !event.bytes[2])) {
                if (state->logical[key]) --state->logical[key];
                if (timeline && state->logicalTimeline[key]) --state->logicalTimeline[key];
            }
        };
        const auto& stream = route.tap == model::Tap::PreFx ? source.midiPreFx : source.midiPostFx;
        const auto& origins =
            route.tap == model::Tap::PreFx ? source.midiPreTimeline : source.midiPostTimeline;
        const auto* rack = source.rack && route.rackNotes >= 0
                               ? &source.rack->notesOut[static_cast<std::size_t>(route.rackNotes)]
                               : nullptr;
        if (!state->started) {
            if (rack) {
                if (playing)
                    for (const auto& held : rack->before)
                        if (held.high)
                            logical(filter({0,
                                            0,
                                            3,
                                            {static_cast<std::uint8_t>(0x90 | held.channel),
                                             static_cast<std::uint8_t>(held.key),
                                             static_cast<std::uint8_t>(held.velocity)}}));
            } else {
                auto restart = [&](const auto& held, std::size_t voice, bool timeline) {
                    if (std::any_of(stream.begin(), stream.end(), [&](const auto& e) {
                            return e.time == 0 && e.noteId == static_cast<int>(voice);
                        }))
                        return;
                    logical(filter({0,
                                    0,
                                    3,
                                    {static_cast<std::uint8_t>(0x90 | held.channel),
                                     static_cast<std::uint8_t>(held.key),
                                     static_cast<std::uint8_t>(held.velocity)}}),
                            timeline);
                };
                if (route.tap == model::Tap::PreFx) {
                    for (std::size_t voice = 0; voice < source.midiBefore.size(); ++voice) {
                        const auto& held = source.midiBefore[voice];
                        if (held.high && !held.orphan &&
                            (playing || held.kind != MidiSlot::Kind::Timeline))
                            restart(held, voice, held.kind == MidiSlot::Kind::Timeline);
                    }
                } else {
                    // Post-FX taps recover the bus's held voices, including
                    // plug-in output; pre-FX slots may have been replaced.
                    for (std::size_t voice = 0; voice < source.midiPostBefore.size(); ++voice) {
                        const auto& held = source.midiPostBefore[voice];
                        const bool timeline =
                            voice < source.midiBefore.size() &&
                            source.midiBefore[voice].kind == MidiSlot::Kind::Timeline;
                        if (held.high && (playing || !timeline)) restart(held, voice, timeline);
                    }
                }
            }
        }
        std::size_t next = 0;
        for (std::uint32_t f = 0; f < frames; ++f) {
            bool silent = route.muteCurve.active() ? route.muteCurve.at(pos + f) >= .5 : route.mute;
            if (route.tap == model::Tap::PostFader && !source.rack) {
                silent |=
                    route.sourceMute.active() ? route.sourceMute.at(pos + f) >= .5 : source.mute;
                double ignored = 0;
                route.vcas.evaluate(pos + f, ignored, silent);
            }
            if (silent && !state->muted) schedule(slot, {f, 0, 0, {}}, route.delay, offset);
            if (!silent && (!state->started || state->muted))
                for (std::size_t key = 0; key < state->logical.size(); ++key)
                    for (unsigned n = 0; n < state->logical[key]; ++n)
                        schedule(slot,
                                 {f,
                                  0,
                                  3,
                                  {static_cast<std::uint8_t>(0x90 | (key / 128)),
                                   static_cast<std::uint8_t>(key % 128), state->velocity[key]}},
                                 route.delay, offset, n < state->logicalTimeline[key]);
            state->started = true;
            state->muted = silent;
            if (rack) {
                while (next < rack->count && rack->events[next].midi.frame == f) {
                    const auto& e = rack->events[next++].midi;
                    if (!playing) continue;
                    auto event = filter({f, 0, 3, {e.status, e.data1, e.data2}});
                    logical(event);
                    if (!silent && event.size) schedule(slot, event, route.delay, offset);
                }
            } else
                while (next < stream.size() && stream[next].time == f) {
                    const bool timeline = next < origins.size() && origins[next] != 0;
                    const auto& e = stream[next++];
                    auto event = filter(convert(e));
                    const auto status = event.bytes[0] & 0xf0;
                    const bool release =
                        event.size && (status == 0x80 || (status == 0x90 && !event.bytes[2]));
                    if (!playing && !state->reset && release && timeline && e.noteId >= 0 &&
                        static_cast<std::size_t>(e.noteId) < source.midiBefore.size()) {
                        const auto& before = source.midiBefore[static_cast<std::size_t>(e.noteId)];
                        if (before.high && before.kind == MidiSlot::Kind::Timeline) continue;
                    }
                    logical(event, timeline);
                    if (!silent && event.size) schedule(slot, event, route.delay, offset, timeline);
                }
        }
    }
}
void HardwareMidiRuntime::finish(float* const* out, std::uint32_t channels, std::uint32_t frames,
                                 bool closing) SUPPLE_RT {
    // A failed audio block still reaches finish: disabling a MIDI port must
    // not depend on successful plug-in processing or a plan handoff.
    for (auto& state : routes_)
        if (state.active && !outputControls_[static_cast<std::size_t>(state.port)].enabled) {
            if (!state.release) {
                state.release = true;
                ++state.generation;
            }
            if (release(state)) {
                state.active = false;
                state.release = false;
            }
        }
    if (closing) count_ = 0;
    while (count_ && queue_[0].at < frame_ + frames) {
        std::pop_heap(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(count_),
                      [](const auto& a, const auto& b) { return later(a, b); });
        auto pending = queue_[--count_];
        auto& state = routes_[pending.slot];
        if (!state.active || state.release || state.generation != pending.generation) continue;
        pending.event.frame =
            static_cast<std::uint32_t>(std::max<std::int64_t>(0, pending.at - frame_));
        if (pending.event.size)
            (void)write(state, pending.event);
        else if (!release(state, pending.event.frame))
            state.release = true;
    }
    if (closing) {
        std::array<std::uint16_t, 16> used{};
        for (auto& state : routes_)
            if (state.active) {
                (void)release(state);
                used[static_cast<std::size_t>(state.port)] |= state.channels;
            }
        for (std::size_t p = 0; p < 16; ++p)
            for (unsigned c = 0; c < 16; ++c)
                if (used[p] & (1u << c)) {
                    for (const auto cc : {64, 123})
                        if (!backend_->writeMidi({0,
                                                  static_cast<std::uint16_t>(p),
                                                  3,
                                                  {static_cast<std::uint8_t>(0xb0 | c),
                                                   static_cast<std::uint8_t>(cc), 0}}))
                            outputDrops_[p].fetch_add(1, std::memory_order_relaxed);
                }
        count_ = 0;
    }
    if ((lead_ || fadeLeft_) && historyReady_.load(std::memory_order_acquire) && out &&
        !audioDelay_.empty()) {
        const auto capacity = audioDelay_.front().size();
        for (std::uint32_t f = 0; f < frames; ++f) {
            const double mix = fadeLeft_ ? double(fadeFrames_ - fadeLeft_ + 1) / fadeFrames_ : 0;
            for (std::uint32_t c = 0; c < channels && c < audioDelay_.size(); ++c) {
                auto& history = audioDelay_[c];
                history[audioCursor_] = out[c][f];
                auto tap = [&](std::uint32_t delay) {
                    return delay <= audioFilled_
                               ? history[(audioCursor_ + capacity - delay) % capacity]
                               : 0.0f;
                };
                const auto a = tap(readLead_);
                const auto b = fadeLeft_ ? tap(fadeTarget_) : a;
                out[c][f] = mix == 0 ? a
                            : mix == 1
                                ? b
                                : static_cast<float>((1 - mix) * double(a) + mix * double(b));
            }
            audioCursor_ = (audioCursor_ + 1) % capacity;
            if (audioFilled_ < capacity - 1) ++audioFilled_;
            if (fadeLeft_ && !--fadeLeft_) {
                readLead_ = fadeTarget_;
                // Finish an in-flight fade before taking the latest target:
                // rapid edits never move either tap or cut a fade midway.
                if (readLead_ != lead_) {
                    fadeTarget_ = lead_;
                    fadeLeft_ = fadeFrames_;
                }
            }
        }
    }
    historyBusy_.store(lead_ || fadeLeft_, std::memory_order_release);
    frame_ += frames;
}
std::span<const MidiInputEvent> HardwareMidiRuntime::input(std::span<const MidiInputEvent> raw,
                                                           const Plan* plan) SUPPLE_RT {
    if (plan) outputControls_ = plan->midiOutputsSettings;
    for (const auto& e : raw) {
        const auto settings =
            plan && e.port < 16 ? plan->midiInputsSettings[e.port] : MidiPortSettings{};
        if (!settings.enabled) continue;
        const auto delay = plan ? time::samplesAt(settings.offsetMs / 1000., plan->rate) : 0;
        if (inputCount_ == inputQueue_.size()) {
            if (e.port < 16) inputDrops_[e.port].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        inputQueue_[inputCount_++] = {
            frame_ + std::max<std::int64_t>(0, std::int64_t(e.frame) + delay), order_++, e};
        std::push_heap(inputQueue_.begin(),
                       inputQueue_.begin() + static_cast<std::ptrdiff_t>(inputCount_),
                       [](const auto& a, const auto& b) { return later(a, b); });
    }
    std::size_t count = 0;
    while (inputCount_ && inputQueue_[0].at < frame_ + frames_) {
        std::pop_heap(inputQueue_.begin(),
                      inputQueue_.begin() + static_cast<std::ptrdiff_t>(inputCount_),
                      [](const auto& a, const auto& b) { return later(a, b); });
        auto event = inputQueue_[--inputCount_];
        if (plan && event.event.port < 16 && !plan->midiInputsSettings[event.event.port].enabled)
            continue;
        if (count < inputBlock_.size()) {
            event.event.frame =
                static_cast<std::uint32_t>(std::max<std::int64_t>(0, event.at - frame_));
            inputBlock_[count++] = event.event;
        } else if (event.event.port < 16)
            inputDrops_[event.event.port].fetch_add(1, std::memory_order_relaxed);
    }
    return {inputBlock_.data(), count};
}
}  // namespace supple::engine::detail
