// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/rack/schedule.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace supple::rack {

namespace {
constexpr std::int64_t kHookBudget = std::int64_t{1} << 22;
constexpr std::int64_t kFrameBudget = 65536;

double scaleOf(const sd::ParamModel& m) {
    return m.unit == sd::ParamModel::Unit::Percent ? 0.01 : 1.0;
}
}  // namespace

Schedule::~Schedule() = default;

std::unique_ptr<Schedule> Schedule::build(Patch patch, std::string* error) {
    auto fail = [&](std::string message) -> std::unique_ptr<Schedule> {
        if (error) *error = std::move(message);
        return nullptr;
    };
    auto s = std::unique_ptr<Schedule>(new Schedule());
    const int count = static_cast<int>(patch.modules.size());
    s->modules_.resize(patch.modules.size());
    for (int m = 0; m < count; ++m) {
        const Patch::Module& pm = patch.modules[static_cast<std::size_t>(m)];
        ModulePlan& plan = s->modules_[static_cast<std::size_t>(m)];
        plan.instance = pm.instance;
        plan.bypass = pm.bypass;
        plan.automation = pm.automation;
        if (!pm.instance) continue;
        const ModuleType& t = pm.instance->type();
        plan.inputs.resize(t.inputs().size());
        for (std::size_t k = 0; k < plan.inputs.size(); ++k)
            plan.inputs[k].normal.assign(kGrid, t.input(static_cast<int>(k)).normal);
        plan.params.resize(t.params().size());
        for (std::size_t i = 0; i < plan.params.size(); ++i) {
            const sd::ParamModel& p = t.params()[i];
            ParamPlan& pp = plan.params[i];
            pp.octaves = p.unit == sd::ParamModel::Unit::Hertz;
            pp.scale = scaleOf(p);
            pp.low = p.min * pp.scale;
            pp.high = p.max * pp.scale;
        }
        plan.outputs.assign(t.outputs().size(), std::vector<double>(kPortFrames, 0.0));
        plan.channels.assign(t.outputs().size(), 1);
        plan.lastChannels.assign(t.outputs().size(), kChannels);
        plan.automated.assign(t.params().size(), std::numeric_limits<double>::quiet_NaN());
        plan.passed.assign(t.inputs().size(), 0);
        for (const auto& [index, curve] : pm.automation)
            if (index < 0 || static_cast<std::size_t>(index) >= t.params().size() || !curve)
                return fail("automation of a parameter module " + std::to_string(pm.id) +
                            " does not have");
    }

    // Cables: into inputs and parameters; the graph they make.
    std::vector<std::vector<int>> next(patch.modules.size());
    std::vector<Patch::Cable> kept;
    for (const Patch::Cable& c : patch.cables) {
        if (c.from < 0 || c.from >= count || c.to < 0 || c.to >= count)
            return fail("a cable joins a module the rack does not have");
        const Instance* from = patch.modules[static_cast<std::size_t>(c.from)].instance;
        const Instance* to = patch.modules[static_cast<std::size_t>(c.to)].instance;
        if (!from || !to) continue;  // a missing type: its cables carry nothing
        if (c.output < 0 || static_cast<std::size_t>(c.output) >= from->type().outputs().size())
            return fail("a cable leaves an output " + from->type().id() + " does not have");
        if (c.param) {
            if (c.index < 0 || static_cast<std::size_t>(c.index) >= to->type().params().size())
                return fail("a cable reaches a parameter " + to->type().id() + " does not have");
            const sd::ParamModel& p = to->type().params()[static_cast<std::size_t>(c.index)];
            if (p.kind == sd::ParamModel::Kind::Toggle || p.output)
                return fail("a cable cannot move " + p.key + " (a toggle or a meter)");
        } else if (c.index < 0 || static_cast<std::size_t>(c.index) >= to->type().inputs().size()) {
            return fail("a cable reaches an input " + to->type().id() + " does not have");
        }
        kept.push_back(c);
        next[static_cast<std::size_t>(c.from)].push_back(c.to);
    }
    patch.cables = std::move(kept);

    // Strongly connected components (Tarjan), each in id (index) order.
    std::vector<int> index(patch.modules.size(), -1), low(patch.modules.size(), 0),
        component(patch.modules.size(), -1);
    std::vector<char> onStack(patch.modules.size(), 0);
    std::vector<int> stack;
    std::vector<std::vector<int>> components;
    int counter = 0;
    std::function<void(int)> visit = [&](int v) {
        index[static_cast<std::size_t>(v)] = low[static_cast<std::size_t>(v)] = counter++;
        stack.push_back(v);
        onStack[static_cast<std::size_t>(v)] = 1;
        for (int w : next[static_cast<std::size_t>(v)]) {
            if (index[static_cast<std::size_t>(w)] < 0) {
                visit(w);
                low[static_cast<std::size_t>(v)] =
                    std::min(low[static_cast<std::size_t>(v)], low[static_cast<std::size_t>(w)]);
            } else if (onStack[static_cast<std::size_t>(w)]) {
                low[static_cast<std::size_t>(v)] =
                    std::min(low[static_cast<std::size_t>(v)], index[static_cast<std::size_t>(w)]);
            }
        }
        if (low[static_cast<std::size_t>(v)] == index[static_cast<std::size_t>(v)]) {
            std::vector<int> c;
            int w = -1;
            do {
                w = stack.back();
                stack.pop_back();
                onStack[static_cast<std::size_t>(w)] = 0;
                component[static_cast<std::size_t>(w)] = static_cast<int>(components.size());
                c.push_back(w);
            } while (w != v);
            std::sort(c.begin(), c.end());
            components.push_back(std::move(c));
        }
    };
    for (int v = 0; v < count; ++v)
        if (index[static_cast<std::size_t>(v)] < 0) visit(v);

    // The components in topological order, the lowest id first among those
    // ready (Kahn's algorithm with a heap).
    const std::size_t nc = components.size();
    std::vector<std::vector<int>> cnext(nc);
    std::vector<int> indegree(nc, 0);
    std::vector<char> self(nc, 0);
    for (const Patch::Cable& c : patch.cables) {
        const int a = component[static_cast<std::size_t>(c.from)];
        const int b = component[static_cast<std::size_t>(c.to)];
        if (a == b) {
            self[static_cast<std::size_t>(a)] = 1;
            continue;
        }
        cnext[static_cast<std::size_t>(a)].push_back(b);
        ++indegree[static_cast<std::size_t>(b)];
    }
    using Ready = std::pair<int, int>;  // (lowest module index, component)
    std::priority_queue<Ready, std::vector<Ready>, std::greater<>> ready;
    for (std::size_t c = 0; c < nc; ++c)
        if (indegree[c] == 0) ready.push({components[c].front(), static_cast<int>(c)});
    while (!ready.empty()) {
        const int c = ready.top().second;
        ready.pop();
        Step step;
        step.modules = components[static_cast<std::size_t>(c)];
        step.group = step.modules.size() > 1 || self[static_cast<std::size_t>(c)];
        s->steps_.push_back(std::move(step));
        for (int d : cnext[static_cast<std::size_t>(c)])
            if (--indegree[static_cast<std::size_t>(d)] == 0)
                ready.push({components[static_cast<std::size_t>(d)].front(), d});
    }

    // Each cable's source; a cable inside a group that does not run forward
    // in id order carries the previous frame.
    s->feedback_.assign(patch.cables.size(), false);
    for (std::size_t k = 0; k < patch.cables.size(); ++k) {
        const Patch::Cable& c = patch.cables[k];
        const bool loop = component[static_cast<std::size_t>(c.from)] ==
                              component[static_cast<std::size_t>(c.to)] &&
                          s->steps_.size() > 0;
        const bool feedback = loop && c.from >= c.to;
        s->feedback_[k] = feedback;
        Source src{static_cast<int>(k), c.from, c.output, feedback, c.amount};
        ModulePlan& to = s->modules_[static_cast<std::size_t>(c.to)];
        if (c.param)
            to.params[static_cast<std::size_t>(c.index)].sources.push_back(src);
        else
            to.inputs[static_cast<std::size_t>(c.index)].sources.push_back(src);
    }
    for (ModulePlan& m : s->modules_)
        for (InputPlan& in : m.inputs) {
            in.direct = in.sources.size() == 1 && !in.sources.front().feedback;
            if (!in.sources.empty() && !in.direct) in.sum.assign(kPortFrames, 0.0);
        }
    s->carry_.assign(patch.cables.size(), {});
    s->carryChannels_.assign(patch.cables.size(), 0);
    s->zero_.assign(2 * kGrid, 0.0);
    s->spare_.assign(kGrid, 0.0);
    s->patch_ = std::move(patch);
    return s;
}

const double* Schedule::output(int module, int output) const {
    return modules_[static_cast<std::size_t>(module)]
        .outputs[static_cast<std::size_t>(output)]
        .data();
}
int Schedule::outputChannels(int module, int output) const {
    return modules_[static_cast<std::size_t>(module)].channels[static_cast<std::size_t>(output)];
}

int Schedule::sourceChannels(const Source& s) const SUPPLE_RT {
    return modules_[static_cast<std::size_t>(s.module)]
        .channels[static_cast<std::size_t>(s.output)];
}

double Schedule::value(const Source& s, int c, int f) const SUPPLE_RT {
    const ModulePlan& src = modules_[static_cast<std::size_t>(s.module)];
    if (s.feedback && f == 0) {
        const int ch = carryChannels_[static_cast<std::size_t>(s.cable)];
        const int k = ch == 1 ? 0 : c;
        return k < ch ? carry_[static_cast<std::size_t>(s.cable)][static_cast<std::size_t>(k)]
                      : 0.0;
    }
    const int ch = src.channels[static_cast<std::size_t>(s.output)];
    const int k = ch == 1 ? 0 : c;
    if (k >= ch) return 0.0;
    const int at = s.feedback ? f - 1 : f;
    return src
        .outputs[static_cast<std::size_t>(s.output)][static_cast<std::size_t>(k * kGrid + at)];
}

void Schedule::beginBlock(ModulePlan& m, std::int64_t grid, int frames,
                          bool sampleAutomation) SUPPLE_RT {
    if (!m.instance) return;
    Instance& in = *m.instance;
    if (sampleAutomation) {
        for (const auto& [index, curve] : m.automation)
            m.automated[static_cast<std::size_t>(index)] = curve->at(grid);
    }
    bool changed = false;
    for (std::size_t i = 0; i < m.params.size(); ++i)
        changed = in.beginParam(static_cast<int>(i), m.automated[i], frames) || changed;
    if (!changed || in.host() || m.bypass || in.status().silent.load(std::memory_order_relaxed))
        return;
    if (auto hook = in.type().code()->update()) {
        sd::CurrentScope scope(&in.record());
        in.record().budget = kHookBudget;
        hook(&in.record(), in.self());
    }
}

void Schedule::run(ModulePlan& m, int f0, int n, const HostContext& ctx) SUPPLE_RT {
    if (!m.instance) return;
    Instance& in = *m.instance;
    int width = 1;
    for (std::size_t k = 0; k < m.inputs.size(); ++k) {
        InputPlan& ip = m.inputs[k];
        script::Input& port = in.inputMember(static_cast<int>(k));
        if (ip.sources.empty()) {
            sd::PortBinding::bindInput(port, ip.normal.data() + f0, zero_.data(), 0, 0, n);
            continue;
        }
        if (ip.direct) {
            const Source& s = ip.sources.front();
            const ModulePlan& src = modules_[static_cast<std::size_t>(s.module)];
            const int ch = src.channels[static_cast<std::size_t>(s.output)];
            sd::PortBinding::bindInput(port,
                                       src.outputs[static_cast<std::size_t>(s.output)].data() + f0,
                                       zero_.data(), ch == 1 ? 0 : kGrid, ch, n);
            width = std::max(width, ch);
            continue;
        }
        int ch = 0;
        for (const Source& s : ip.sources) ch = std::max(ch, sourceChannels(s));
        ch = std::max(ch, 1);
        for (int c = 0; c < ch; ++c)
            for (int f = f0; f < f0 + n; ++f) {
                double x = 0.0;
                for (const Source& s : ip.sources) x += value(s, c, f);
                ip.sum[static_cast<std::size_t>(c * kGrid + f)] = x;
            }
        sd::PortBinding::bindInput(port, ip.sum.data() + f0, zero_.data(), ch == 1 ? 0 : kGrid, ch,
                                   n);
        width = std::max(width, ch);
    }
    for (std::size_t i = 0; i < m.params.size(); ++i) {
        ParamPlan& pp = m.params[i];
        script::Param& p = in.paramMember(static_cast<int>(i));
        const double* glide = in.glide(static_cast<int>(i));
        sd::ParamBinding::setGlide(p, glide + f0, n);
        if (pp.sources.empty()) {
            sd::ParamBinding::setModulation(p, nullptr, 0, 0);
            continue;
        }
        int ch = 0;
        for (const Source& s : pp.sources) ch = std::max(ch, sourceChannels(s));
        ch = std::max(ch, 1);
        double* mod = in.modBuffer(static_cast<int>(i));
        for (int c = 0; c < ch; ++c)
            for (int f = f0; f < f0 + n; ++f) {
                double x = 0.0;
                for (const Source& s : pp.sources) x += s.amount * value(s, c, f);
                const double base = glide[f];
                const double v = pp.octaves ? base * std::exp2(x) : base + x * pp.scale;
                mod[c * kGrid + f] = std::isfinite(v) ? std::clamp(v, pp.low, pp.high) : base;
            }
        sd::ParamBinding::setModulation(p, mod + f0, kGrid, ch);
        width = std::max(width, ch);
    }
    in.record().channels = width;
    for (std::size_t o = 0; o < m.outputs.size(); ++o) {
        double* buf = m.outputs[o].data();
        const int clear = std::min(kChannels, std::max(width, m.lastChannels[o]));
        for (int c = 0; c < clear; ++c) std::fill_n(buf + c * kGrid + f0, n, 0.0);
        sd::PortBinding::bindOutput(in.outputMember(static_cast<int>(o)), buf + f0, spare_.data(),
                                    kGrid, width, n);
    }
    if (m.bypass) {
        // Each output takes the first input of its kind not passed already.
        std::fill(m.passed.begin(), m.passed.end(), 0);
        const ModuleType& t = in.type();
        for (std::size_t o = 0; o < m.outputs.size(); ++o) {
            const sd::PortModel& out = t.output(static_cast<int>(o));
            for (std::size_t k = 0; k < m.inputs.size(); ++k) {
                if (m.passed[k] || t.input(static_cast<int>(k)).signal != out.signal) continue;
                m.passed[k] = 1;
                const script::Input& port = in.inputMember(static_cast<int>(k));
                const int ch = std::max(1, port.channels());
                script::Output& dst = in.outputMember(static_cast<int>(o));
                dst.setChannels(ch);
                for (int c = 0; c < ch; ++c)
                    for (int f = 0; f < n; ++f) dst[c][f] = port[c][f];
                break;
            }
        }
    } else if (HostModule* host = in.host()) {
        HostContext c = ctx;
        c.instance = &in;
        c.frames = n;
        c.offset = ctx.offset + f0;
        c.position = ctx.position + f0;
        c.channels = width;
        host->process(c);
    } else if (!in.status().silent.load(std::memory_order_relaxed)) {
        if (auto process = in.type().code()->process()) {
            sd::Instance& record = in.record();
            const std::uint32_t before =
                record.status.budgetExceeded.load(std::memory_order_relaxed);
            record.transport = ctx.transport;
            {
                sd::CurrentScope scope(&record);
                record.budget = kFrameBudget * n;
                process(&record, in.self(), n);
            }
            if (record.status.budgetExceeded.load(std::memory_order_relaxed) != before) {
                // Stopped by its budget: silent until reset or reloaded.
                record.status.silent.store(true, std::memory_order_relaxed);
                for (std::size_t o = 0; o < m.outputs.size(); ++o)
                    for (int c = 0; c < kChannels; ++c)
                        std::fill_n(m.outputs[o].data() + c * kGrid + f0, n, 0.0);
            }
        }
    }
    for (std::size_t o = 0; o < m.outputs.size(); ++o) {
        m.channels[o] = sd::PortBinding::channels(in.outputMember(static_cast<int>(o)));
        m.lastChannels[o] = std::max(m.channels[o], width);
    }
}

void Schedule::process(std::int64_t position, int frames, Clock& clock) SUPPLE_RT {
    if (frames <= 0) return;
    int done = 0;
    while (done < frames) {
        const std::int64_t pos = position + done;
        const int phase = static_cast<int>(((pos % kGrid) + kGrid) % kGrid);
        const int n = std::min(frames - done, kGrid - phase);
        HostContext ctx;
        ctx.frames = n;
        ctx.offset = done;
        ctx.position = pos;
        ctx.transport = clock.transport(pos);
        // Host blocks may split one grid interval into several calls. Keep
        // its sampled automation target while smoothing still advances every
        // frame. A first call or locate samples the grid containing its start.
        const bool sampleAutomation =
            phase == 0 || lastFrames_ == 0 || (done == 0 && position != expected_);
        for (ModulePlan& m : modules_) beginBlock(m, pos - phase, n, sampleAutomation);
        for (const Step& step : steps_) {
            if (!step.group) {
                run(modules_[static_cast<std::size_t>(step.modules.front())], 0, n, ctx);
                continue;
            }
            for (int f = 0; f < n; ++f)
                for (int mi : step.modules) run(modules_[static_cast<std::size_t>(mi)], f, 1, ctx);
        }
        for (std::size_t k = 0; k < feedback_.size(); ++k) {
            if (!feedback_[k]) continue;
            const Patch::Cable& c = patch_.cables[k];
            const ModulePlan& src = modules_[static_cast<std::size_t>(c.from)];
            const int ch = src.channels[static_cast<std::size_t>(c.output)];
            carryChannels_[k] = ch;
            for (int ci = 0; ci < ch; ++ci)
                carry_[k][static_cast<std::size_t>(ci)] =
                    src.outputs[static_cast<std::size_t>(c.output)]
                               [static_cast<std::size_t>(ci * kGrid + n - 1)];
        }
        for (ModulePlan& m : modules_) {
            if (!m.instance) continue;
            for (std::size_t o = 0; o < m.outputs.size(); ++o)
                sd::PortBinding::bindOutput(m.instance->outputMember(static_cast<int>(o)),
                                            m.outputs[o].data(), spare_.data(), kGrid,
                                            m.channels[o], n);
            for (std::size_t i = 0; i < m.params.size(); ++i)
                sd::ParamBinding::setGlide(m.instance->paramMember(static_cast<int>(i)),
                                           m.instance->glide(static_cast<int>(i)), n);
            m.instance->observe(n);
            m.instance->publish(n);
        }
        lastFrames_ = n;
        done += n;
    }
    expected_ = position + frames;
}

}  // namespace supple::rack
