// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Automation lanes (gui.md §4.3, P5.14): how an envelope's values map to a
// lane's height and read as text. A track's volume sits on the fader's taper
// (so the lane and the fader agree), pan is centred, mute is off/on. Plugin
// parameter lanes take their range and curve from the plugin's description
// (the app supplies it, see TrackLayout::pluginScales).
#pragma once

#include <optional>
#include <string>

#include <memory>

#include "supple/model/session.h"
#include "supple/params/params.h"

namespace supple::views {

struct EnvScale {
    enum class Kind { Volume, Pan, Percent, Mute, Boost, Playrate, Plugin };
    Kind kind = Kind::Volume;
    std::shared_ptr<const params::DescribedParam> param;  // Plugin: range, curve, units
    bool normalizedInterpolation = true;                  // semantic and normalized plug-in lanes
    double toNorm(double value) const;                    // 0 = the lane's bottom, 1 = its top
    double fromNorm(double norm) const;                   // the inverse, clamped to the range
    std::string text(double value) const;                 // "-6.0 dB", "L 30", "muted", "1.20 kHz"
    double reference() const;  // the lane's guide line: unity, centre, default
};

// The scale of a mixer envelope or the main master's playrate; nullopt for others.
std::optional<EnvScale> envScale(const model::Envelope& env);
// "Volume", "Pan", "Mute", "Playrate" (or the parameter key).
std::string envTitle(const model::Envelope& env);
// With a send's destination: "Send → Reverb · Volume" (others as above).
std::string envTitle(const model::Envelope& env, const model::Session& session);
// The envelope's value at a beat (points are in beats), with the segment
// shapes the engine plays; `fallback` when it has no points.
double envValueAt(const model::Envelope& env, double beat, double fallback);
// The order lanes appear under their track: volume, pan, mute, then others,
// then its sends' (volume, pan, mute).
int envOrder(const model::Envelope& env);

// Samples visible automation directly, without expanding repeated source points.
class EnvelopeCurve {
  public:
    EnvelopeCurve(const model::Session& session, model::Id envelope, EnvScale scale, double from,
                  double to);
    double valueAt(double beat, double fallback) const;

  private:
    struct Segment {
        double start, end, sourceLength, baseline;
        bool loop;
        std::vector<model::EnvPoint> points;
    };
    EnvScale scale_;
    bool normalized_ = false;
    std::vector<model::EnvPoint> points_;
    std::vector<Segment> segments_;
};

}  // namespace supple::views
