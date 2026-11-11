// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The host modules the rack runtime implements itself (docs/specs/rack.md
// §5): Out (audio to the host), In (audio from it), Notes (MIDI to poly
// pitch and gates) and Transport (the timeline as clocks and phases). The
// host binds each to its block's buffers or events before Schedule::process.
// Plugin modules are the engine's (they need its plugin hosting).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "supple/base/rt.h"
#include "supple/rack/instance.h"

namespace supple::rack::host {

inline constexpr std::string_view kOut = "com.suppleaudio.rack.out";
inline constexpr std::string_view kIn = "com.suppleaudio.rack.in";
inline constexpr std::string_view kNotes = "com.suppleaudio.rack.notes";
inline constexpr std::string_view kNotesOut = "com.suppleaudio.rack.notes-out";
inline constexpr std::string_view kTransport = "com.suppleaudio.rack.transport";
inline constexpr std::string_view kPlugin = "com.suppleaudio.rack.plugin";

// The four types, described (each made once).
std::shared_ptr<const ModuleType> outType();
std::shared_ptr<const ModuleType> inType();
std::shared_ptr<const ModuleType> notesType();
std::shared_ptr<const ModuleType> notesOutType();
std::shared_ptr<const ModuleType> transportType();
// Their instances (main thread).
std::unique_ptr<Instance> make(std::shared_ptr<const ModuleType> type,
                               const Instance::Setup& setup);

// Out: its inputs (poly cables summed, R normalled to L) times its level,
// added into the host's stereo pair from the block's first frame.
class Out final : public HostModule {
  public:
    void bind(float* left, float* right) SUPPLE_RT {
        left_ = left;
        right_ = right;
    }
    void process(const HostContext& ctx) SUPPLE_RT override;

  private:
    float* left_ = nullptr;
    float* right_ = nullptr;
};

// In: the host's stereo pair at its level.
class In final : public HostModule {
  public:
    void bind(const float* left, const float* right) SUPPLE_RT {
        left_ = left;
        right_ = right;
    }
    void process(const HostContext& ctx) SUPPLE_RT override;

  private:
    const float* left_ = nullptr;
    const float* right_ = nullptr;
};

// A MIDI short message at a frame of the host's block.
struct MidiEvent {
    std::uint32_t frame = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
    bool live = false;         // from a device, not the timeline
    std::uint64_t source = 0;  // 0: timeline, 1: device, route id + 2: MIDI send
    int noteId = -1;           // addressed notes remain distinct at equal pitches
};

// Shared with rack.capture: hysteretic gates and MIDI quantization.
struct NoteGate {
    bool high = false;
    bool step(double value) SUPPLE_RT {
        const bool next = high ? !(value < 0.25) : value >= 0.5;
        const bool changed = next != high;
        high = next;
        return changed;
    }
};
int noteKey(double pitch) SUPPLE_RT;
int noteVelocity(double value, bool connected) SUPPLE_RT;

// Poly pitch/gate to MIDI. The host binds a preallocated event span and count
// for each block. No notes start while transport is stopped. endAll() ends
// held notes at the next processed frame (a locate or loop wrap).
class NotesOut final : public HostModule {
  public:
    struct Event {
        MidiEvent midi;
        int voice = 0;
    };
    struct State {
        bool high = false;
        int key = 60, channel = 0, velocity = 96;
    };
    std::array<State, kChannels> held() const SUPPLE_RT;
    void bind(std::span<Event> events, std::size_t& count) SUPPLE_RT {
        events_ = events;
        count_ = &count;
        count = 0;
    }
    void endAll() SUPPLE_RT { endAll_ = true; }
    void prepare(Instance&) override;
    void process(const HostContext& ctx) SUPPLE_RT override;

  private:
    struct Voice {
        NoteGate gate;
        int key = 60, channel = 0, velocity = 96;
    };
    void send(std::uint32_t frame, int channel, int key, int velocity, bool on,
              int voice) SUPPLE_RT;
    std::array<Voice, kChannels> voices_{};
    std::span<Event> events_;
    std::size_t* count_ = nullptr;
    bool endAll_ = false;
};

// Notes: MIDI to poly pitch, gate, velocity and pressure. The host gives it
// each block's events in frame order; endAll() releases every note (a stop,
// a locate, a loop wrap).
class Notes final : public HostModule {
  public:
    void bind(std::span<const MidiEvent> events) SUPPLE_RT { events_ = events; }
    void endAll() SUPPLE_RT { endAll_ = true; }
    // The live input changed: a newer generation releases what live events
    // opened (voices, held keys, bend, mod wheel, pressure, sustain) and
    // leaves the timeline's notes playing. Older generations are ignored.
    void liveInput(std::uint64_t generation) SUPPLE_RT {
        if (generation > liveGeneration_) {
            liveGeneration_ = generation;
            endLive_ = true;
        }
    }
    void trackInput(std::uint64_t source) SUPPLE_RT {
        if (trackSource_ != source) {
            trackSource_ = source;
            endTrack_ = true;
        }
    }
    void endSource(std::uint64_t source) SUPPLE_RT;
    void retainSource(std::uint64_t source, std::span<const int> notes) SUPPLE_RT;
    void retainRoutes(std::span<const std::uint64_t> routes) SUPPLE_RT;
    void prepare(Instance& instance) override;
    void process(const HostContext& ctx) SUPPLE_RT override;

  private:
    struct Voice {
        int key = -1;
        bool held = false, sustained = false;
        std::uint64_t source = 0;
        int noteId = -1;
        double velocity = 0, pressure = 0;
        std::uint64_t age = 0;
        int gap = 0;        // frames the gate drops for a retrigger
        int trigger = 0;    // frames left of the retrigger output
        bool live = false;  // opened by a live event
    };
    void on(int key, double velocity, int voices, int mode, bool live, std::uint64_t source,
            int noteId);
    void off(int key, int mode, std::uint64_t source, int noteId);
    void release(Voice& v);
    void handle(const MidiEvent& e, int voices, int mode);
    void releaseLive(int mode);
    std::array<Voice, kChannels> voice_{};
    int monoChoice(int mode) const SUPPLE_RT;
    void prune(std::uint64_t source, std::span<const int> notes,
               std::span<const std::uint64_t> routes) SUPPLE_RT;
    std::array<Voice, 128> stack_{};  // mono modes: independently addressed held keys
    int stacked_ = 0;
    int lastMode_ = 0;
    std::uint64_t trackSource_ = 0;
    bool endTrack_ = false;
    std::span<const MidiEvent> events_;
    std::uint64_t clock_ = 0;
    int rotate_ = 0;
    double bend_ = 0, modwheel_ = 0, channelPressure_ = 0;
    bool sustain_ = false, endAll_ = false, endLive_ = false;
    // Which source set each controller last: live ones reset on endLive_.
    bool bendLive_ = false, modwheelLive_ = false, pressureLive_ = false, sustainLive_ = false;
    std::uint64_t liveGeneration_ = 0;
    int triggerFrames_ = 48;
};

// Transport: the timeline as gates, clocks and phases.
class Transport final : public HostModule {
  public:
    void prepare(Instance& instance) override;
    void process(const HostContext& ctx) SUPPLE_RT override;

  private:
    double expected_ = 0;  // the song position the next block should start at
    bool wasPlaying_ = false, started_ = false;
    double lastTicks_ = -1, lastBar_ = -1, lastBeat_ = -1;
    int reset_ = 0, bar_ = 0, beat_ = 0, clock_ = 0;
    int triggerFrames_ = 48;
};

}  // namespace supple::rack::host
