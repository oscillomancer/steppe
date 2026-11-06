// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// One module running in a rack (ADR-0062 §5): its state, which lives across
// schedule rebuilds, so adding a cable keeps an LFO's phase and a reverb's
// tail. A script module's struct, or a host module's object, with the
// parameters, inputs and outputs a schedule binds, the knob values the main
// thread sets, and the telemetry the window reads.
//
// Threads: made and destroyed on the main thread (they allocate); processed
// by one audio thread at a time (the host's executors hand a rack over
// between them); setParam() and readTelemetry() from the main thread at any
// time.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "supple/base/rt.h"
#include "supple/rack/module_type.h"
#include "supple/script_runtime.h"

namespace supple::rack {

inline constexpr int kGrid = 32;  // the rack's block: frames, on the timeline's grid
inline constexpr int kChannels = script::kMaxChannels;
inline constexpr int kPortFrames = kChannels * kGrid;  // a port's buffer

class Instance;

// What a host module's process() is given for one grid block.
struct HostContext {
    Instance* instance = nullptr;
    int frames = 0;               // 1..32
    int offset = 0;               // of this block in the host's block
    std::int64_t position = 0;    // its first frame's timeline position
    script::Transport transport;  // at its start
    int channels = 1;             // the module's (its widest input or cable)
};

// A module the host implements in C++ (Out, In, Notes, Transport, Plugin):
// its ports and parameters are bound exactly as a script's, so it reads
// `in(k)[c][i]` and `param(i).at(i, c)` as a script would.
class HostModule {
  public:
    virtual ~HostModule() = default;
    // Main thread, once the instance exists.
    virtual void prepare(Instance& instance) { (void)instance; }
    virtual void process(const HostContext& ctx) SUPPLE_RT = 0;
};

// What the window shows of a module, as of one moment.
struct Telemetry {
    std::vector<double> outputs;         // each output's last value, channel 0
    std::vector<float> peaks;            // its peak |x|, channel 0, since the last
    std::vector<std::uint8_t> channels;  // its channels
    std::vector<double> params;          // each parameter's value with its cables, physical
    std::vector<std::uint8_t> shared;    // the Shared members' bytes, in declaration order
    std::uint64_t serial = 0;            // grows with each publication
};

class Instance {
  public:
    struct Setup {
        double sampleRate = 48000.0;
        int seed = 0;
        bool offline = false;
    };
    // A script module's instance: made, its parameters at their defaults,
    // prepared. Main thread.
    static std::unique_ptr<Instance> make(std::shared_ptr<const ModuleType> type,
                                          const Setup& setup);
    // A host module's.
    static std::unique_ptr<Instance> make(std::shared_ptr<const ModuleType> type,
                                          std::unique_ptr<HostModule> host, const Setup& setup);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    const ModuleType& type() const { return *type_; }
    const std::shared_ptr<const ModuleType>& typePtr() const { return type_; }
    HostModule* host() const { return host_.get(); }
    const Setup& setup() const { return setup_; }

    // A knob's value in physical units (a percent as 30, a choice its
    // index), kept to its range. Main thread; heard from the next block.
    void setParam(int index, double physical);
    double param(int index) const;  // what setParam() last gave

    // The newest telemetry, if newer than `out` (by serial). Main thread.
    bool readTelemetry(Telemetry& out) const;
    // Whether anyone reads telemetry: off, the audio side publishes nothing.
    void watch(bool on) { watched_.store(on, std::memory_order_relaxed); }

    // What the module's code found wrong while it ran (budget, indices).
    const sd::Status& status() const { return record_.status; }
    // Back to how prepare() left it: a fresh start. Main thread, while no
    // executor processes it.
    void reset();

    // --- the schedule's (audio thread) ----------------------------------------
    script::Param& paramMember(int i) const { return *params_[static_cast<std::size_t>(i)]; }
    script::Input& inputMember(int k) const { return *inputs_[static_cast<std::size_t>(k)]; }
    script::Output& outputMember(int k) const { return *outputs_[static_cast<std::size_t>(k)]; }
    sd::Instance& record() { return record_; }
    void* self() const { return self_; }

    // A parameter's value this block, in the script's units, given the
    // target from the slot or the host's automation (physical; NaN: the
    // slot's). Fills the glide for `frames` and returns whether it changed.
    bool beginParam(int i, double automation, int frames) SUPPLE_RT;
    const double* glide(int i) const { return glide_[static_cast<std::size_t>(i)].data(); }
    // Modulation buffers, one per parameter (kPortFrames).
    double* modBuffer(int i) { return mod_[static_cast<std::size_t>(i)].data(); }
    // Telemetry at the end of a block.
    void observe(int frames) SUPPLE_RT;
    void publish(int frames) SUPPLE_RT;
    bool watched() const { return watched_.load(std::memory_order_relaxed); }

  private:
    Instance() = default;
    void bindMembers();
    void prepare();

    std::shared_ptr<const ModuleType> type_;
    std::unique_ptr<HostModule> host_;
    Setup setup_;
    sd::Instance record_;
    void* self_ = nullptr;  // a script's struct
    std::vector<script::Param*> params_;
    std::vector<script::Input*> inputs_;
    std::vector<script::Output*> outputs_;
    // A host module's own members.
    std::vector<std::unique_ptr<script::Param>> hostParams_;
    std::vector<std::unique_ptr<script::Input>> hostInputs_;
    std::vector<std::unique_ptr<script::Output>> hostOutputs_;

    std::unique_ptr<std::atomic<double>[]> targets_;  // physical
    std::vector<double> scale_;                       // physical to script units
    std::vector<double> value_, smoothed_, coefficient_;
    std::vector<std::array<double, kGrid>> glide_;
    std::vector<std::array<double, kPortFrames>> mod_;
    bool first_ = true;

    // Telemetry: a triple buffer, the audio side writing `back`.
    std::atomic<bool> watched_{false};
    std::array<Telemetry, 3> slots_;
    mutable std::atomic<int> middle_{1};  // index | 4 when fresh
    int back_ = 0;
    mutable int front_ = 2;
    std::vector<float> peaks_;
    std::uint64_t serial_ = 0;
    std::int64_t sinceFrames_ = 0;
};

}  // namespace supple::rack
