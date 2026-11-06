// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A compiled rack (ADR-0062 §5, docs/specs/rack.md §4.2–4.4): the modules in
// the order they run, feedback groups that run frame by frame, the cables'
// buffers and sums, and the cables into parameters. Built on the main
// thread from a Patch the host compiles from its session; immutable after,
// and processed by one audio thread at a time.
//
// process() splits the host's frames on a 32-frame grid of the timeline
// position, so a rack renders the same whatever the host's block: every
// value a module sees is sampled on that grid.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "supple/base/rt.h"
#include "supple/rack/instance.h"

namespace supple::rack {

// The host's automation of one parameter: its value (physical units) at a
// timeline position. Called once per grid block, positions rising except
// after a locate.
class ParamCurve {
  public:
    virtual double at(std::int64_t position) SUPPLE_RT = 0;

  protected:
    ~ParamCurve() = default;
};

// The host's timeline: the transport at a grid block's start.
class Clock {
  public:
    virtual script::Transport transport(std::int64_t position) SUPPLE_RT = 0;

  protected:
    ~Clock() = default;
};

// What a schedule is built from. Modules in the session's id order (the
// order the feedback rule and ties follow).
struct Patch {
    struct Module {
        std::uint32_t id = 0;          // the session's (mo_N: N)
        Instance* instance = nullptr;  // null: its type is missing (silent)
        bool bypass = false;
        std::vector<std::pair<int, ParamCurve*>> automation;  // (parameter, curve)
    };
    struct Cable {
        int from = 0, output = 0;  // module index, output index
        int to = 0;                // module index
        bool param = false;        // into a parameter, else an input
        int index = 0;             // the parameter's or the input's
        double amount = 1.0;       // a parameter's: at a value of 1, its modulation unit
    };
    std::vector<Module> modules;
    std::vector<Cable> cables;
};

class Schedule {
  public:
    // Checks indices; refuses a cable into a toggle or out of range.
    static std::unique_ptr<Schedule> build(Patch patch, std::string* error = nullptr);
    ~Schedule();

    // `frames` frames whose first is at timeline `position`.
    void process(std::int64_t position, int frames, Clock& clock) SUPPLE_RT;

    const Patch& patch() const { return patch_; }
    // Whether a cable carries the previous frame (it closes a loop).
    bool feedback(std::size_t cable) const { return feedback_[cable]; }
    // The order modules run in (indices), feedback groups bracketed by
    // group().
    struct Step {
        std::vector<int> modules;  // one, or a feedback group in id order
        bool group = false;
    };
    const std::vector<Step>& steps() const { return steps_; }
    // An output's buffer and channels after the last block (tests, probes):
    // channel c's frames at outputs(m, o) + c * kGrid.
    const double* output(int module, int output) const;
    int outputChannels(int module, int output) const;
    int lastFrames() const { return lastFrames_; }

  private:
    Schedule() = default;
    struct Source {
        int cable = 0;
        int module = 0, output = 0;
        bool feedback = false;
        double amount = 1.0;
    };
    struct InputPlan {
        std::vector<Source> sources;
        bool direct = false;  // one forward cable: read its buffer in place
        std::vector<double> normal, sum;
    };
    struct ParamPlan {
        std::vector<Source> sources;
        bool octaves = false;
        double scale = 1.0, low = 0.0, high = 1.0;
    };
    struct ModulePlan {
        Instance* instance = nullptr;
        bool bypass = false;
        std::vector<InputPlan> inputs;
        std::vector<ParamPlan> params;
        std::vector<std::vector<double>> outputs;  // kPortFrames each
        std::vector<int> channels;                 // per output
        std::vector<int> lastChannels;             // zeroed last block
        std::vector<std::pair<int, ParamCurve*>> automation;
        std::vector<double> automated;  // per parameter: this grid interval's, NaN for the slot's
        std::vector<char> passed;       // bypass: inputs already passed through
        int width = 1;
    };
    void beginBlock(ModulePlan& m, std::int64_t grid, int frames, bool sampleAutomation) SUPPLE_RT;
    void run(ModulePlan& m, int f0, int n, const HostContext& ctx) SUPPLE_RT;
    double value(const Source& s, int c, int f) const SUPPLE_RT;
    int sourceChannels(const Source& s) const SUPPLE_RT;

    Patch patch_;
    std::vector<ModulePlan> modules_;
    std::vector<Step> steps_;
    std::vector<bool> feedback_;
    // A feedback cable's source at the last frame of the previous block.
    std::vector<std::array<double, kChannels>> carry_;
    std::vector<int> carryChannels_;
    std::vector<double> zero_, spare_;
    std::int64_t expected_ = INT64_MIN;
    int lastFrames_ = 0;
};

}  // namespace supple::rack
