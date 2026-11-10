// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Pan laws and modes (ADR-0066): a strip's pan as a 2×2 matrix from its
// inputs to its outputs, applied before its Console 9 encode.
#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "supple/base/rt.h"
#include "supple/model/objects.h"

namespace supple::engine::detail {

// out left = ll·left + rl·right; out right = lr·left + rr·right.
struct PanMatrix {
    double ll = 1, rl = 0, lr = 0, rr = 1;
    bool operator==(const PanMatrix&) const = default;
};

// What a pan matrix is made from. Positions and width are in percent.
struct PanSettings {
    model::PanMode mode = model::PanMode::Pan;
    double pan = 0, width = 100, left = -100, right = 100;
    model::PanLaw law;
    bool operator==(const PanSettings&) const = default;
};

// The law's exponent on the constant-power curve: 0 dB is k = 0 (balance),
// -3 dB k = 1 (sin/cos), -6 dB k = 2 (sin²); others land on their label at
// the centre, where the sin/cos curve is 10·log10(1/2) dB.
inline double panLawExponent(double db) SUPPLE_RT {
    if (db == 0) return 0;
    if (db == -3) return 1;
    if (db == -6) return 2;
    return db / (10 * std::log10(0.5));
}

// The left and right gains of a mono source at `p` (-1..1) under `law`.
inline std::pair<double, double> panSides(double p, const model::PanLaw& law) SUPPLE_RT {
    p = std::isnan(p) ? 0 : std::clamp(p, -1.0, 1.0);
    constexpr double quarter = std::numbers::pi / 4;
    // sin of both halves, so a hard pan's far side is exactly zero.
    const double sl = std::sin((1 - p) * quarter), sr = std::sin((1 + p) * quarter);
    // Balance: the near side at unity, the far side cos(|p|·π/2), exactly 0 at the edge.
    const double far = std::fabs(p) >= 1 ? 0.0 : std::cos(p * 2 * quarter);
    const double bl = p <= 0 ? 1 : far, br = p >= 0 ? 1 : far;
    const double k = panLawExponent(law.db);
    double l, r;
    if (k == 0) {
        l = bl;
        r = br;
    } else if (k == 1) {
        l = sl;
        r = sr;
    } else if (k == 2) {
        l = sl * sl;
        r = sr * sr;
    } else if (k < 1) {
        l = std::pow(sl, k) * std::pow(bl, 1 - k);
        r = std::pow(sr, k) * std::pow(br, 1 - k);
    } else {
        l = std::pow(sl, k);
        r = std::pow(sr, k);
    }
    if (law.boost && k != 0) {
        // Gain compensation: the centre at 0 dB, the sides raised by the law.
        const double lift = k == 1 ? std::numbers::sqrt2 : k == 2 ? 2.0 : std::pow(2.0, k / 2);
        l *= lift;
        r *= lift;
    }
    return {l, r};
}

// A balance: the 0 dB law, which never raises either side (masters, takes).
inline std::pair<double, double> balanceSides(double p) SUPPLE_RT { return panSides(p, {}); }

inline PanMatrix panMatrix(const PanSettings& s) SUPPLE_RT {
    PanMatrix m;
    const double p = std::clamp(s.pan / 100, -1.0, 1.0);
    switch (s.mode) {
        case model::PanMode::Dual: {
            const auto [al, ar] = panSides(s.left / 100, s.law);
            const auto [bl, br] = panSides(s.right / 100, s.law);
            m = {al, bl, ar, br};
            break;
        }
        case model::PanMode::Width: {
            // Mid/side width, narrowed as the image moves out, then the pan.
            const double w = std::clamp(s.width / 100, -1.0, 1.0) * (1 - std::fabs(p));
            const auto [fl, fr] = panSides(p, s.law);
            const double same = (1 + w) / 2, cross = (1 - w) / 2;
            m = {fl * same, fl * cross, fr * cross, fr * same};
            break;
        }
        case model::PanMode::Pan:
        default: {
            const auto [fl, fr] = panSides(p, s.law);
            m = {fl, 0, 0, fr};
            break;
        }
    }
    return m;
}

// The matrix a strip's controls give, recomputed only when they change.
struct PanCache {
    PanSettings settings;
    PanMatrix matrix;
    bool valid = false;
    const PanMatrix& at(const PanSettings& s) SUPPLE_RT {
        if (!valid || !(s == settings)) {
            settings = s;
            matrix = panMatrix(s);
            valid = true;
        }
        return matrix;
    }
};

}  // namespace supple::engine::detail
