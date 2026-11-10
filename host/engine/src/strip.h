// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Implicit console stages. All state is owned by the source strip's executor.
#pragma once
#include <algorithm>
#include <cmath>
#include "pan.h"
#include "supple/console9/console9.h"

namespace supple::engine::detail {
// Gain and native pan ramps match the library's advance-before-sample rule.
// Evaluating controls separately lets mute ramp even under continuous automation.
struct ControlRamp {
    double value = 0, target = 0, step = 0;
    std::uint32_t left = 0;
    void set(double v, std::uint32_t n) SUPPLE_RT {
        if (v == target && (n || !left)) return;
        target = v;
        left = n;
        if (n)
            step = (v - value) / n;
        else
            value = v;
    }
    double next() SUPPLE_RT {
        if (left && --left)
            value += step;
        else if (!left)
            value = target;
        return value;
    }
};
// The fader, the pan matrix's four coefficients (ADR-0066) and the mute gate.
struct FaderControl {
    ControlRamp gain, gate;
    ControlRamp pan[4];  // ll, rl, lr, rr
    bool valid = false;
    void set(double g, const PanMatrix& m, bool muted, bool automated,
             std::uint32_t frames) SUPPLE_RT {
        const double c[4] = {m.ll, m.rl, m.lr, m.rr};
        if (!valid) {
            gain.value = gain.target = g;
            for (int i = 0; i < 4; ++i) pan[i].value = pan[i].target = c[i];
            gate.value = gate.target = muted ? 0 : 1;
            valid = true;
        }
        gain.set(g, automated ? 0 : frames);
        for (int i = 0; i < 4; ++i) pan[i].set(c[i], automated ? 0 : frames);
        gate.set(muted ? 0 : 1, frames);
    }
    PanMatrix nextPan() SUPPLE_RT {
        return {pan[0].next(), pan[1].next(), pan[2].next(), pan[3].next()};
    }
    void reset() SUPPLE_RT { *this = {}; }
};
// A stereo sample through a pan matrix.
inline void applyPan(const PanMatrix& m, float l, float r, double& ol, double& or_) SUPPLE_RT {
    const double a = std::isfinite(l) ? double(l) : 0, b = std::isfinite(r) ? double(r) : 0;
    ol = m.ll * a + m.rl * b;
    or_ = m.lr * a + m.rr * b;
}
// The strip's polarity and mono switches (ADR-0022). A switch ramps over the
// smoothing time like mute (polarity through silence), so it never clicks.
struct UtilityControl {
    ControlRamp polarity, mono;
    bool valid = false;
    void set(bool invert, bool sum, std::uint32_t frames) SUPPLE_RT {
        if (!valid) {
            polarity.value = polarity.target = invert ? -1 : 1;
            mono.value = mono.target = sum ? 1 : 0;
            valid = true;
        }
        polarity.set(invert ? -1 : 1, frames);
        mono.set(sum ? 1 : 0, frames);
    }
    bool transparent() const SUPPLE_RT {
        return !polarity.left && !mono.left && polarity.value == 1 && mono.value == 0;
    }
    void process(float* l, float* r, std::uint32_t n) SUPPLE_RT {
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto g = static_cast<float>(polarity.next());
            const auto m = static_cast<float>(mono.next());
            const float mid = .5f * (l[i] + r[i]);
            l[i] = (l[i] + (mid - l[i]) * m) * g;
            r[i] = (r[i] + (mid - r[i]) * m) * g;
        }
    }
    void reset() SUPPLE_RT { *this = {}; }
};

template <class Stage>
struct ConsoleStage {
    Stage current, previous;
    console9::Seeds seeds;
    bool linear = false;
    std::uint32_t left = 0, span = 1;
    double lastGain = 1, fadeFrom = 0;
    // Unity staging (ADR-0066): the strip pans before the encode.
    void init(console9::Seeds s, bool off) {
        seeds = s;
        linear = off;
        current.setStaging(console9::Staging::Unity);
        previous.setStaging(console9::Staging::Unity);
        current.set(1);
        current.setLinear(off);
        current.reset(s);
    }
    void reset() SUPPLE_RT {
        left = 0;
        current.reset(seeds);
        previous.reset(seeds);
    }
    void carry(const ConsoleStage& old, std::uint32_t frames) SUPPLE_RT {
        current = old.current;
        lastGain = old.lastGain;
        if (linear != old.linear) {
            previous = old.current;
            fadeFrom = 0;
            if (old.left) {
                // Reversing a two-mode fade starts from the mix actually heard.
                current = old.previous;
                fadeFrom = (1 - old.fadeFrom) * old.left / old.span;
            }
            left = span = frames;
        } else {
            previous = old.previous;
            left = old.left;
            span = old.span;
            fadeFrom = old.fadeFrom;
        }
        current.setLinear(linear);
    }
    void sample(float l, float r, float& ol, float& or_, double gain = 1) SUPPLE_RT {
        const float* in[] = {&l, &r};
        float* out[] = {&ol, &or_};
        // Sample-accurate controls, including the pre-encode mute ramp.
        const bool changed = gain != lastGain;
        if (changed) current.set(gain);
        current.process(in, out, 1);
        if (left) {
            float pl, pr;
            float* po[] = {&pl, &pr};
            if (changed) previous.set(gain);
            previous.process(in, po, 1);
            const float t = static_cast<float>(fadeFrom + (1 - fadeFrom) * (span - --left) / span);
            ol = pl + (ol - pl) * t;
            or_ = pr + (or_ - pr) * t;
        }
        lastGain = gain;
    }
};
}  // namespace supple::engine::detail
