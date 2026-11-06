// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// One module, run by itself for tests (ADR-0062): signals into its inputs and
// into its parameters as cables would carry them, its knobs set, its outputs
// recorded, block by block on the rack's 32-frame grid. It binds the module as
// the rack does (supple/script_runtime.h's ParamBinding and PortBinding), so a
// module that works here works in a rack; it is not the rack runtime
// (modules/rack), which also orders modules, sums cables and keeps feedback.
//
//   ModuleHarness h(code);                       // a ModuleCode: native or JIT
//   h.set("cutoff", 800);                        // physical: Hz, a percent as 30
//   h.input("in", 1, [](int, std::int64_t n) { return std::sin(n * 0.05); });
//   h.modulate("cutoff", 1, lfo, 2.0);           // +-2 octaves
//   h.run(48000);
//   const std::vector<double>& y = h.output("out", 0);
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "supple/script_module.h"
#include "supple/script_runtime.h"

namespace supple::rack::test {

namespace sd = supple::script::detail;
namespace ss = supple::script;

class ModuleHarness {
  public:
    static constexpr int kGrid = 32;
    static constexpr int kChannels = ss::kMaxChannels;
    // A signal: its value at channel c, frame n (from the first frame run).
    using Signal = std::function<double(int c, std::int64_t n)>;
    using Transport = ss::Transport;

    explicit ModuleHarness(const sd::ModuleCode& code, double sampleRate = 48000.0,
                           int seed = 12345)
        : code_(code) {
        record_.sampleRate = sampleRate;
        record_.maxBlockSize = kGrid;
        record_.seed = seed;
        record_.channels = 1;
        self_ = code.create();
        sd::Instance::bind(*reinterpret_cast<ss::Plugin*>(self_), &record_);
        const auto& params = code.params();
        params_.resize(params.size());
        for (std::size_t i = 0; i < params.size(); ++i) {
            ParamState& p = params_[i];
            p.model = &params[i];
            p.target = scaled(params[i], params[i].def);
            p.value = p.target;
            ss::Param& member = param(i);
            sd::ParamBinding::setSlot(member, static_cast<int>(i));
            sd::ParamBinding::setRange(member, scaled(params[i], params[i].min),
                                       params[i].offAtMinimum);
        }
        for (const sd::PortModel& m : code.ports()) {
            if (m.output) {
                outputs_.push_back(OutputState{&m, {}, {}, {}});
                sd::PortBinding::setSlot(outputPort(m), m.index);
            } else {
                InputState in{&m, {}, 0, {}, {}};
                in.normal.fill(m.normal);
                inputs_.push_back(std::move(in));
                sd::PortBinding::setSlot(inputPort(m), m.index);
            }
        }
        zero_.fill(0.0);
        call(code.prepare(), kBudget);
    }
    ~ModuleHarness() { code_.destroy(self_); }
    ModuleHarness(const ModuleHarness&) = delete;
    ModuleHarness& operator=(const ModuleHarness&) = delete;

    // A knob, in physical units (a percent as 30, a choice as its index, a
    // toggle as 0 or 1).
    void set(std::string_view key, double physical) {
        ParamState& p = params_.at(paramIndex(key));
        p.target = scaled(*p.model, std::clamp(physical, p.model->min, p.model->max));
    }
    // A signal into an input, `channels` wide (a mono cable reaches every
    // channel); none() disconnects it.
    void input(std::string_view key, int channels, Signal signal) {
        InputState& in = inputs_.at(inputIndex(key));
        in.channels = std::clamp(channels, 0, kChannels);
        in.signal = std::move(signal);
    }
    void disconnect(std::string_view key) { input(key, 0, nullptr); }
    // A cable into a parameter: `amount` at a value of 1, in the parameter's
    // modulation unit (octaves for a frequency, else its own: a percent in
    // percent).
    void modulate(std::string_view key, int channels, Signal signal, double amount) {
        ParamState& p = params_.at(paramIndex(key));
        p.modChannels = std::clamp(channels, 0, kChannels);
        p.modSignal = std::move(signal);
        p.amount = amount;
    }
    void transport(const ss::Transport& t) { transport_ = t; }

    // Runs `frames` frames in grid blocks, recording every output.
    void run(std::int64_t frames) {
        while (frames > 0) {
            const int n = static_cast<int>(std::min<std::int64_t>(frames, kGrid));
            block(n);
            frames -= n;
        }
    }

    // What an output carried, channel c, every frame run (0 where it had
    // fewer channels).
    const std::vector<double>& output(std::string_view key, int c = 0) const {
        return outputs_.at(outputIndex(key)).recorded.at(static_cast<std::size_t>(c));
    }
    // An output's channels in the last block.
    int channels(std::string_view key) const { return outputs_.at(outputIndex(key)).lastChannels; }
    std::int64_t frames() const { return frame_; }
    const sd::Status& status() const { return record_.status; }
    template <class T>
    T& as() {
        return *static_cast<T*>(self_);
    }

  private:
    static constexpr std::int64_t kBudget = std::int64_t{1} << 31;

    struct ParamState {
        const sd::ParamModel* model = nullptr;
        double target = 0, value = 0, smoothed = 0;
        bool first = true;
        std::array<double, kGrid> glide{};
        int modChannels = 0;
        Signal modSignal;
        double amount = 0;
        std::array<double, kChannels * kGrid> mod{};
    };
    struct InputState {
        const sd::PortModel* model = nullptr;
        Signal signal;
        int channels = 0;
        std::array<double, kChannels * kGrid> buffer{};
        std::array<double, kGrid> normal{};
    };
    struct OutputState {
        const sd::PortModel* model = nullptr;
        std::array<double, kChannels * kGrid> buffer{};
        std::array<double, kGrid> spare{};
        std::array<std::vector<double>, kChannels> recorded;
        int lastChannels = 0;
    };

    static double scaled(const sd::ParamModel& m, double physical) {
        return physical * (m.unit == sd::ParamModel::Unit::Percent ? 0.01 : 1.0);
    }
    ss::Param& param(std::size_t i) {
        return *reinterpret_cast<ss::Param*>(static_cast<unsigned char*>(self_) +
                                             code_.params()[i].offset);
    }
    ss::Input& inputPort(const sd::PortModel& m) {
        return *reinterpret_cast<ss::Input*>(static_cast<unsigned char*>(self_) + m.offset);
    }
    ss::Output& outputPort(const sd::PortModel& m) {
        return *reinterpret_cast<ss::Output*>(static_cast<unsigned char*>(self_) + m.offset);
    }
    std::size_t paramIndex(std::string_view key) const {
        for (std::size_t i = 0; i < code_.params().size(); ++i)
            if (code_.params()[i].key == key) return i;
        throw std::out_of_range("no parameter " + std::string(key));
    }
    std::size_t inputIndex(std::string_view key) const {
        for (std::size_t i = 0; i < inputs_.size(); ++i)
            if (inputs_[i].model->key == key) return i;
        throw std::out_of_range("no input " + std::string(key));
    }
    std::size_t outputIndex(std::string_view key) const {
        for (std::size_t i = 0; i < outputs_.size(); ++i)
            if (outputs_[i].model->key == key) return i;
        throw std::out_of_range("no output " + std::string(key));
    }
    void call(sd::ModuleCode::Hook hook, std::int64_t budget) {
        if (!hook) return;
        sd::CurrentScope scope(&record_);
        record_.budget = budget;
        hook(&record_, self_);
    }

    void block(int n) {
        // Knobs: the value at the block's start, glided per frame when
        // smoothed (one pole over the smoothing time, as a plugin's).
        bool changed = false;
        for (std::size_t i = 0; i < params_.size(); ++i) {
            ParamState& p = params_[i];
            const bool moved = p.first || p.target != p.value;
            p.value = p.target;
            if (p.first) p.smoothed = p.target;
            const double ms = p.model->smoothingMs;
            const double k =
                ms > 0 ? std::exp(-1.0 / std::max(1.0, ms * 0.001 * record_.sampleRate)) : 0.0;
            for (int f = 0; f < n; ++f) {
                p.smoothed = ms > 0 ? p.value + k * (p.smoothed - p.value) : p.value;
                p.glide[static_cast<std::size_t>(f)] = p.smoothed;
            }
            sd::ParamBinding::setValue(param(i), p.value, moved);
            sd::ParamBinding::setGlide(param(i), p.glide.data(), n);
            if (p.modChannels > 0 && p.modSignal) {
                const bool octaves = p.model->unit == sd::ParamModel::Unit::Hertz;
                const double lo = scaled(*p.model, p.model->min),
                             hi = scaled(*p.model, p.model->max);
                for (int c = 0; c < p.modChannels; ++c)
                    for (int f = 0; f < n; ++f) {
                        const double x = p.amount * p.modSignal(c, frame_ + f);
                        const double base = p.glide[static_cast<std::size_t>(f)];
                        const double v = octaves ? base * std::exp2(x) : base + scaled(*p.model, x);
                        p.mod[static_cast<std::size_t>(c * kGrid + f)] =
                            std::isfinite(v) ? std::clamp(v, lo, hi) : base;
                    }
                sd::ParamBinding::setModulation(param(i), p.mod.data(), kGrid, p.modChannels);
            } else {
                sd::ParamBinding::setModulation(param(i), nullptr, 0, 0);
            }
            changed = changed || moved;
            p.first = false;
        }
        // The module's channels: its widest input or modulating cable.
        int width = 1;
        for (const InputState& in : inputs_) width = std::max(width, in.channels);
        for (const ParamState& p : params_) width = std::max(width, p.modChannels);
        record_.channels = width;
        record_.transport = transport_;
        if (changed) call(code_.update(), kBudget);
        for (InputState& in : inputs_) {
            ss::Input& port = inputPort(*in.model);
            if (in.channels > 0 && in.signal) {
                for (int c = 0; c < in.channels; ++c)
                    for (int f = 0; f < n; ++f)
                        in.buffer[static_cast<std::size_t>(c * kGrid + f)] =
                            in.signal(c, frame_ + f);
                sd::PortBinding::bindInput(port, in.buffer.data(), zero_.data(),
                                           in.channels == 1 ? 0 : kGrid, in.channels, n);
            } else {
                sd::PortBinding::bindInput(port, in.normal.data(), zero_.data(), 0, 0, n);
            }
        }
        for (OutputState& out : outputs_) {
            out.buffer.fill(0.0);
            sd::PortBinding::bindOutput(outputPort(*out.model), out.buffer.data(), out.spare.data(),
                                        kGrid, width, n);
        }
        if (auto process = code_.process()) {
            sd::CurrentScope scope(&record_);
            record_.budget = std::int64_t{65536} * n;
            process(&record_, self_, n);
        }
        for (OutputState& out : outputs_) {
            out.lastChannels = sd::PortBinding::channels(outputPort(*out.model));
            for (int c = 0; c < kChannels; ++c)
                for (int f = 0; f < n; ++f)
                    out.recorded[static_cast<std::size_t>(c)].push_back(
                        c < out.lastChannels ? out.buffer[static_cast<std::size_t>(c * kGrid + f)]
                                             : 0.0);
        }
        if (transport_.playing)
            transport_.beats += n * transport_.tempo / 60.0 / record_.sampleRate;
        frame_ += n;
    }

    const sd::ModuleCode& code_;
    sd::Instance record_;
    void* self_ = nullptr;
    std::vector<ParamState> params_;
    std::vector<InputState> inputs_;
    std::vector<OutputState> outputs_;
    std::array<double, kGrid> zero_{};
    ss::Transport transport_;
    std::int64_t frame_ = 0;
};

}  // namespace supple::rack::test
