// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/engine/realtime.h"
namespace supple::engine::detail {
inline constexpr std::size_t kMidiOutputPorts = 16;
inline constexpr std::size_t kMidiOutputQueueSize = 2048;
// Shared by ordered closing notes and a future final transport Stop. Keep the
// native port/client alive for the remaining scheduled delay, at most 250 ms.
inline constexpr double kMidiOutputCloseWaitSeconds = .250;
double midiOutputCloseDeadline(double now, double lastScheduled);
struct TimedMidiOutput {
    double seconds = 0;  // sink clock; first byte's requested departure time
    MidiOutputEvent event;
    bool closing = false;  // final callback: ordered native releases; WinMM sends now
};
struct MidiOutputUmp {
    std::array<std::uint32_t, 4> words{};
    std::uint32_t count = 0;
};
// MIDI 1.0 UMP conversion, including up to two SysEx7 packets. Input validated
// by MidiOutputCycle; shared here so native wire encoding is tested on Linux.
MidiOutputUmp midiOutputUmp(const MidiOutputEvent&) SUPPLE_RT;
class MidiSink {
  public:
    virtual ~MidiSink() = default;
    // Called before the sender/audio threads. Best effort per destination.
    virtual Status start() = 0;
    virtual void stop() = 0;  // after sender joined; release the owned port/client
    std::vector<std::string> names;
    std::vector<MidiInputWarning> warnings;
    std::vector<double> latencySeconds;  // unknown: zero; fixed while running
    virtual double now() const SUPPLE_RT = 0;
    virtual bool timestamped() const { return false; }
    virtual bool send(const TimedMidiOutput&) = 0;  // sender only, never audio
    virtual void prepareThread() {}
    virtual void releaseThread() {}
    // Sender sleeps at most 1ms, so stop and newly enqueued events are noticed.
    virtual void waitUntil(double seconds);
    virtual bool changed() = 0;  // monitor only
};
// Shared production/fake queue and scheduling path. One producer (audio),
// one consumer (sender). stop only after the audio producer is joined.
class MidiOutputSender {
  public:
    explicit MidiOutputSender(MidiSink& sink) : sink_(sink) {}
    ~MidiOutputSender() { stop(); }
    Status start();
    void stop();
    bool push(const TimedMidiOutput&) SUPPLE_RT;
    // A deterministic sender step for fake clocks (same path as run()).
    double dispatch();
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t dropped(std::size_t port) const {
        return port < portDrops_.size() ? portDrops_[port].load(std::memory_order_relaxed) : 0;
    }

  private:
    MidiSink& sink_;
    std::array<SpscRing<TimedMidiOutput, kMidiOutputQueueSize>, kMidiOutputPorts> queues_;
    std::array<double, kMidiOutputPorts> lastDue_{};  // producer only
    double lastSent_ = 0;                             // consumer only; stop reads after joining
    void drop(std::size_t port) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if (port < portDrops_.size()) portDrops_[port].fetch_add(1, std::memory_order_relaxed);
    }
    std::array<std::atomic<std::uint64_t>, kMidiOutputPorts> portDrops_{};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> quit_{false};
    std::thread thread_;
};
std::unique_ptr<MidiSink> nativeMidiSink(class MidiSource*);
}  // namespace supple::engine::detail
