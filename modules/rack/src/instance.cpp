// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/rack/instance.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace supple::rack {

namespace {
constexpr std::int64_t kHookBudget = std::int64_t{1} << 22;

double scaleOf(const sd::ParamModel& m) {
    return m.unit == sd::ParamModel::Unit::Percent ? 0.01 : 1.0;
}
}  // namespace

std::unique_ptr<Instance> Instance::make(std::shared_ptr<const ModuleType> type,
                                         const Setup& setup) {
    if (!type || !type->code()) return nullptr;
    auto in = std::unique_ptr<Instance>(new Instance());
    in->type_ = std::move(type);
    in->setup_ = setup;
    in->self_ = in->type_->code()->create();
    sd::Instance::bind(*reinterpret_cast<script::Plugin*>(in->self_), &in->record_);
    in->bindMembers();
    in->prepare();
    return in;
}

std::unique_ptr<Instance> Instance::make(std::shared_ptr<const ModuleType> type,
                                         std::unique_ptr<HostModule> host, const Setup& setup) {
    if (!type || !host) return nullptr;
    auto in = std::unique_ptr<Instance>(new Instance());
    in->type_ = std::move(type);
    in->setup_ = setup;
    in->host_ = std::move(host);
    // A host module's members are its own objects, bound like a script's.
    for (const sd::ParamModel& m : in->type_->params()) {
        script::ParamSpec spec;
        const double s = scaleOf(m);
        spec.min = m.min * s;
        spec.max = m.max * s;
        spec.def = m.def * s;
        spec.offAtMinimum = m.offAtMinimum;
        in->hostParams_.push_back(std::make_unique<script::Param>(spec));
    }
    for (const sd::PortModel& p : in->type_->ports()) {
        if (p.output)
            in->hostOutputs_.push_back(std::make_unique<script::Output>(script::PortSpec{}));
        else
            in->hostInputs_.push_back(std::make_unique<script::Input>(script::PortSpec{}));
    }
    in->bindMembers();
    in->prepare();
    return in;
}

Instance::~Instance() {
    host_.reset();
    if (self_ && type_ && type_->code()) type_->code()->destroy(self_);
}

void Instance::bindMembers() {
    const ModuleType& t = *type_;
    const auto& params = t.params();
    unsigned char* base = static_cast<unsigned char*>(self_);
    for (std::size_t i = 0; i < params.size(); ++i) {
        script::Param* p =
            base ? reinterpret_cast<script::Param*>(base + params[i].offset) : hostParams_[i].get();
        sd::ParamBinding::setSlot(*p, static_cast<int>(i));
        sd::ParamBinding::setRange(*p, params[i].min * scaleOf(params[i]), params[i].offAtMinimum);
        params_.push_back(p);
    }
    for (const sd::PortModel& p : t.ports()) {
        if (p.output) {
            script::Output* o = base ? reinterpret_cast<script::Output*>(base + p.offset)
                                     : hostOutputs_[static_cast<std::size_t>(p.index)].get();
            sd::PortBinding::setSlot(*o, p.index);
            outputs_.push_back(o);
        } else {
            script::Input* in = base ? reinterpret_cast<script::Input*>(base + p.offset)
                                     : hostInputs_[static_cast<std::size_t>(p.index)].get();
            sd::PortBinding::setSlot(*in, p.index);
            inputs_.push_back(in);
        }
    }
    const std::size_t n = params.size();
    targets_ = std::make_unique<std::atomic<double>[]>(n);
    scale_.resize(n);
    value_.resize(n);
    smoothed_.resize(n);
    coefficient_.resize(n);
    glide_.resize(n);
    mod_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const sd::ParamModel& m = params[i];
        scale_[i] = scaleOf(m);
        targets_[i].store(m.def, std::memory_order_relaxed);
        value_[i] = smoothed_[i] = m.def * scale_[i];
        const double samples = m.smoothingMs * 0.001 * setup_.sampleRate;
        coefficient_[i] = m.smoothingMs > 0 ? std::exp(-1.0 / std::max(1.0, samples)) : 0.0;
        glide_[i].fill(value_[i]);
        mod_[i].fill(0.0);
    }
    // Telemetry slots, sized once.
    std::size_t sharedBytes = 0;
    if (t.code())
        for (const sd::SharedModel& s : t.code()->shared())
            sharedBytes += static_cast<std::size_t>(s.bytes);
    for (Telemetry& slot : slots_) {
        slot.outputs.assign(outputs_.size(), 0.0);
        slot.peaks.assign(outputs_.size(), 0.0f);
        slot.channels.assign(outputs_.size(), 0);
        slot.params.assign(n, 0.0);
        slot.shared.assign(sharedBytes, 0);
    }
    peaks_.assign(outputs_.size(), 0.0f);
    static_assert(std::atomic<float>::is_always_lock_free);
    outputLevels_ = std::make_unique<std::atomic<float>[]>(outputs_.size());
    for (std::size_t o = 0; o < outputs_.size(); ++o)
        outputLevels_[o].store(0.0f, std::memory_order_relaxed);
}

void Instance::prepare() {
    record_.sampleRate = setup_.sampleRate;
    record_.maxBlockSize = kGrid;
    record_.channels = 1;
    record_.offline = setup_.offline;
    record_.seed = setup_.seed;
    if (host_) {
        host_->prepare(*this);
        return;
    }
    if (auto hook = type_->code()->prepare()) {
        sd::CurrentScope scope(&record_);
        record_.budget = kHookBudget;
        record_.memory = 0;
        hook(&record_, self_);
        record_.memory = -1;
    }
    first_ = true;
}

float Instance::outputLevel(int output) const {
    if (output < 0 || static_cast<std::size_t>(output) >= outputs_.size()) return 0.0f;
    return outputLevels_[static_cast<std::size_t>(output)].load(std::memory_order_relaxed);
}

void Instance::reset() {
    for (std::size_t o = 0; o < outputs_.size(); ++o)
        outputLevels_[o].store(0.0f, std::memory_order_relaxed);
    if (host_) {
        host_->prepare(*this);
        first_ = true;
        return;
    }
    const sd::ModuleCode& code = *type_->code();
    code.destroy(self_);
    self_ = code.create();
    sd::Instance::bind(*reinterpret_cast<script::Plugin*>(self_), &record_);
    // Rebind the members the new struct holds, keeping the knobs.
    unsigned char* base = static_cast<unsigned char*>(self_);
    const auto& params = type_->params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        params_[i] = reinterpret_cast<script::Param*>(base + params[i].offset);
        sd::ParamBinding::setSlot(*params_[i], static_cast<int>(i));
        sd::ParamBinding::setRange(*params_[i], params[i].min * scale_[i], params[i].offAtMinimum);
    }
    std::size_t k = 0, o = 0;
    for (const sd::PortModel& p : type_->ports()) {
        if (p.output) {
            outputs_[o] = reinterpret_cast<script::Output*>(base + p.offset);
            sd::PortBinding::setSlot(*outputs_[o++], p.index);
        } else {
            inputs_[k] = reinterpret_cast<script::Input*>(base + p.offset);
            sd::PortBinding::setSlot(*inputs_[k++], p.index);
        }
    }
    record_.status.clear();
    record_.status.memoryRefused.store(false, std::memory_order_relaxed);
    prepare();
}

void Instance::setParam(int index, double physical) {
    if (index < 0 || static_cast<std::size_t>(index) >= type_->params().size()) return;
    const sd::ParamModel& m = type_->params()[static_cast<std::size_t>(index)];
    if (!std::isfinite(physical)) return;
    targets_[static_cast<std::size_t>(index)].store(std::clamp(physical, m.min, m.max),
                                                    std::memory_order_relaxed);
}

double Instance::param(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= type_->params().size()) return 0.0;
    return targets_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
}

bool Instance::beginParam(int i, double automation, int frames) SUPPLE_RT {
    const auto u = static_cast<std::size_t>(i);
    const sd::ParamModel& m = type_->params()[u];
    double physical =
        std::isnan(automation) ? targets_[u].load(std::memory_order_relaxed) : automation;
    physical = std::isfinite(physical) ? std::clamp(physical, m.min, m.max) : m.def;
    if (m.kind != sd::ParamModel::Kind::Continuous) physical = std::round(physical);
    const double v = physical * scale_[u];
    const bool changed = first_ || v != value_[u];
    value_[u] = v;
    double s = first_ ? v : smoothed_[u];
    const double k = coefficient_[u];
    if (k > 0.0 && s != v) {
        const double epsilon = std::max(1e-12, 1e-9 * (m.max - m.min) * scale_[u]);
        for (int f = 0; f < frames; ++f) {
            s = v + k * (s - v);
            if (std::abs(s - v) < epsilon) s = v;
            glide_[u][static_cast<std::size_t>(f)] = s;
        }
    } else {
        s = v;
        for (int f = 0; f < frames; ++f) glide_[u][static_cast<std::size_t>(f)] = v;
    }
    smoothed_[u] = s;
    script::Param& p = *params_[u];
    sd::ParamBinding::setValue(p, v, changed);
    sd::ParamBinding::setGlide(p, glide_[u].data(), frames);
    return changed;
}

void Instance::observe(int frames) SUPPLE_RT {
    first_ = false;
    for (std::size_t o = 0; o < outputs_.size(); ++o) {
        const script::Output& out = *outputs_[o];
        double peak = 0.0;
        for (int c = 0; c < sd::PortBinding::channels(out); ++c) {
            const script::Channel ch = out[c];
            for (int f = 0; f < frames && f < ch.size(); ++f)
                peak = std::max(peak, std::abs(ch.data_[f]));
        }
        // Keep the public reading finite even if a faulty module emits Inf.
        outputLevels_[o].store(static_cast<float>(std::min(
                                   peak, static_cast<double>(std::numeric_limits<float>::max()))),
                               std::memory_order_relaxed);
    }
    if (!watched_.load(std::memory_order_relaxed)) return;
    for (std::size_t o = 0; o < outputs_.size(); ++o) {
        const script::Output& out = *outputs_[o];
        const script::Channel ch = out[0];
        float peak = peaks_[o];
        for (int f = 0; f < frames && f < ch.size(); ++f)
            peak = std::max(peak, static_cast<float>(std::abs(ch.data_[f])));
        peaks_[o] = peak;
    }
    sinceFrames_ += frames;
}

void Instance::publish(int frames) SUPPLE_RT {
    if (!watched_.load(std::memory_order_relaxed)) return;
    if (sinceFrames_ < static_cast<std::int64_t>(setup_.sampleRate / 60.0)) return;
    sinceFrames_ = 0;
    Telemetry& t = slots_[static_cast<std::size_t>(back_)];
    for (std::size_t o = 0; o < outputs_.size(); ++o) {
        const script::Output& out = *outputs_[o];
        const script::Channel ch = out[0];
        t.outputs[o] = ch.size() > 0 ? ch.data_[ch.size() - 1] : 0.0;
        t.peaks[o] = peaks_[o];
        t.channels[o] = static_cast<std::uint8_t>(sd::PortBinding::channels(out));
        peaks_[o] = 0.0f;
    }
    const auto& params = type_->params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        const script::Param& p = *params_[i];
        t.params[i] = p.at(std::max(frames, 1) - 1, 0) / scale_[i];
    }
    if (self_ && type_->code()) {
        std::size_t at = 0;
        for (const sd::SharedModel& s : type_->code()->shared()) {
            std::memcpy(t.shared.data() + at, static_cast<const unsigned char*>(self_) + s.offset,
                        static_cast<std::size_t>(s.bytes));
            at += static_cast<std::size_t>(s.bytes);
        }
    }
    t.serial = ++serial_;
    back_ = middle_.exchange(back_ | 4, std::memory_order_acq_rel) & 3;
}

bool Instance::readTelemetry(Telemetry& out) const {
    if (middle_.load(std::memory_order_acquire) & 4)
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3;
    const Telemetry& t = slots_[static_cast<std::size_t>(front_)];
    if (t.serial == 0 || t.serial == out.serial) return false;
    out = t;
    return true;
}

}  // namespace supple::rack
