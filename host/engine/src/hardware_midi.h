// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "plan.h"
namespace supple::engine::detail {
inline constexpr std::size_t kHardwareMidiRoutes = 256, kHardwareMidiQueue = 65536;
inline constexpr double kHardwareMidiLeadSeconds = 1.0;
struct HardwareRoute {
    Id id, sourceId, noteOutId;
    std::size_t source = 0;
    std::string device;
    model::Tap tap = model::Tap::PostFader;
    int rackNotes = -1, fromChannel = 0, toChannel = 0, port = -1;
    bool mute = false;
    std::int64_t delay = 0;
    Automation muteCurve, sourceMute;
    VcaControls vcas;
};
class HardwareMidiRuntime {
  public:
    HardwareMidiRuntime();
    void prepare(double rate, std::uint32_t channels);
    void begin(Backend&, std::uint32_t frames) SUPPLE_RT;
    void collect(Plan&, std::int64_t pos, std::uint32_t frames, std::uint32_t offset, bool playing,
                 std::uint64_t epoch) SUPPLE_RT;
    void finish(float* const* out, std::uint32_t channels, std::uint32_t frames,
                bool closing) SUPPLE_RT;
    std::span<const MidiInputEvent> input(std::span<const MidiInputEvent>, const Plan*) SUPPLE_RT;
    std::uint64_t dropped(bool output, std::size_t port) const;
    std::uint32_t lead() const { return leadReported_.load(std::memory_order_relaxed); }

  private:
    struct RouteState {
        std::uint32_t id = 0, generation = 0;
        Id sourceId, noteOutId;
        bool active = false, reset = true, muted = false, started = false, release = false;
        int port = -1;
        std::int64_t delay = 0;
        model::Tap tap = model::Tap::PostFader;
        int from = 0, to = 0;
        std::uint16_t channels = 0;
        std::array<std::uint16_t, 2048> held{}, logical{};
        std::array<std::uint8_t, 2048> velocity{};
    };
    struct Pending {
        std::int64_t at = 0;
        std::uint64_t order = 0;
        std::uint32_t generation = 0;
        std::uint16_t slot = 0;
        MidiOutputEvent event;
    };
    struct InputPending {
        std::int64_t at = 0;
        std::uint64_t order = 0;
        MidiInputEvent event;
    };
    std::array<RouteState, kHardwareMidiRoutes> routes_{};
    std::array<MidiPortSettings, 16>
        outputControls_{};  // newest publication; disable bypasses render horizon
    std::array<Pending, kHardwareMidiQueue> queue_{};
    std::size_t count_ = 0;
    std::array<InputPending, kHardwareMidiQueue> inputQueue_{};
    std::size_t inputCount_ = 0;
    std::array<MidiInputEvent, kMaxMidiInputEvents> inputBlock_{};
    std::array<std::atomic<std::uint64_t>, 16> outputDrops_{}, inputDrops_{};
    std::vector<std::vector<float>> audioDelay_;
    std::atomic<bool> historyReady_{false};
    std::uint32_t readLead_ = 0, fadeTarget_ = 0;
    std::uint32_t fadeFrames_ = 1, fadeLeft_ = 0;
    std::size_t audioCursor_ = 0;
    std::uint32_t audioFilled_ = 0;
    std::size_t wrote_ = 0;
    std::uint32_t lead_ = 0, frames_ = 0;
    std::atomic<std::uint32_t> leadReported_{0};
    std::int64_t frame_ = 0;
    std::uint64_t order_ = 0, epoch_ = 0;
    bool playing_ = false;
    Backend* backend_ = nullptr;
    Plan* plan_ = nullptr;  // identity only; a failed block may outlive this plan
    std::uint64_t planGeneration_ = 0;
    void adopt(Plan&, std::uint64_t epoch, bool playing) SUPPLE_RT;
    void schedule(std::size_t slot, MidiOutputEvent event, std::int64_t delay,
                  std::uint32_t offset) SUPPLE_RT;
    bool release(RouteState&, std::uint32_t at = 0) SUPPLE_RT;
    bool write(RouteState&, const MidiOutputEvent&) SUPPLE_RT;
};
Status prepareHardwareMidi(Plan&, Backend&, const MidiPortPreferences&, const MidiPortPreferences&);
}  // namespace supple::engine::detail
