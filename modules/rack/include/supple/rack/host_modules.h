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
inline constexpr std::string_view kTransport = "com.suppleaudio.rack.transport";
inline constexpr std::string_view kPlugin = "com.suppleaudio.rack.plugin";

// The four types, described (each made once).
std::shared_ptr<const ModuleType> outType();
std::shared_ptr<const ModuleType> inType();
std::shared_ptr<const ModuleType> notesType();
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
};

// Notes: MIDI to poly pitch, gate, velocity and pressure. The host gives it
// each block's events in frame order; endAll() releases every note (a stop,
// a locate, a loop wrap).
class Notes final : public HostModule {
  public:
    void bind(std::span<const MidiEvent> events) SUPPLE_RT { events_ = events; }
    void endAll() SUPPLE_RT { endAll_ = true; }
    void prepare(Instance& instance) override;
    void process(const HostContext& ctx) SUPPLE_RT override;

  private:
    struct Voice {
        int key = -1;
        bool held = false, sustained = false;
        double velocity = 0, pressure = 0;
        std::uint64_t age = 0;
        int gap = 0;      // frames the gate drops for a retrigger
        int trigger = 0;  // frames left of the retrigger output
    };
    void on(int key, double velocity, int voices, int mode);
    void off(int key, int mode);
    void release(Voice& v);
    void handle(const MidiEvent& e, int voices, int mode);
    std::array<Voice, kChannels> voice_{};
    std::array<int, 128> stack_{};  // mono modes: keys held, in order
    int stacked_ = 0;
    std::array<double, 128> keyVelocity_{};
    std::span<const MidiEvent> events_;
    std::uint64_t clock_ = 0;
    int rotate_ = 0;
    double bend_ = 0, modwheel_ = 0, channelPressure_ = 0;
    bool sustain_ = false, endAll_ = false;
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
