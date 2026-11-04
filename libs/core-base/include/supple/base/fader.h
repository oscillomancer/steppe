// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// A level fader's travel: dB <-> 0..1 position, a square-root taper over
// -60..+12 dB with -inf below. Unity sits near 70 % of the travel; the bottom
// 2 % is silence. The GUI's faders and the controllers that move a level
// (MIDI learn, "(MIDI CC)" actions) share it, so a knob and the fader agree.
#pragma once

#include <algorithm>
#include <cmath>

namespace supple::fader {

inline constexpr double kTopDb = 12.0, kFloorDb = -60.0;
inline constexpr double kSilenceDb = -150.0;  // -inf: at or below this
inline constexpr double kSilence = 0.02;      // the travel's bottom: silence

inline double position(double db) {
    if (db <= kFloorDb) return db <= kSilenceDb + 1e-9 ? 0.0 : kSilence;
    const double x = (std::min(db, kTopDb) - kFloorDb) / (kTopDb - kFloorDb);  // 0..1
    return kSilence + (1.0 - kSilence) * x * x;
}

inline double db(double position) {
    if (position <= kSilence * 0.5) return kSilenceDb;
    if (position <= kSilence) return kFloorDb;
    const double x = std::sqrt((std::min(position, 1.0) - kSilence) / (1.0 - kSilence));
    return kFloorDb + x * (kTopDb - kFloorDb);
}

}  // namespace supple::fader
