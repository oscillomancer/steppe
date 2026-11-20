// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Automation lanes (gui.md §4.3, P5.14): how an envelope's values map to a
// lane's height and read as text. A track's volume sits on the fader's taper
// (so the lane and the fader agree), pan is centred, mute is off/on. Plugin
// parameter lanes take their range and curve from the plugin's description
// (the app supplies it, see TrackLayout::pluginScales).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "supple/model/session.h"
#include "supple/params/params.h"
#include "supple/ui/controls.h"

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
    // Modulation (ADR-0096, "Depth units"): the domain a route's depth adds
    // in. A plug-in parameter's is its CLAP value (a stock plug-in's float
    // parameter: its position on the curve, 0..1); a mixer value's is its
    // own unit (dB, percent). modulationUnit() is depth 1 in that domain:
    // the CLAP range for a plug-in parameter, 1 for the rest.
    double toModulation(double value) const;
    double fromModulation(double m) const;  // a plug-in's clamped to its CLAP range
    double modulationUnit() const;
};

// The value `offset` (in the scale's modulation domain) makes of `base`:
// what the engine plays for a modulated parameter, before its range clamp.
double modulatedValue(const EnvScale& scale, double base, double offset);

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

// Modulation (ADR-0096). Whether a route's source input carries 0..1 (every
// cable into it from a unipolar, gate or phase output); the app supplies it
// from the rack catalog, and without it every source counts as -1..1.
using UnipolarSource = std::function<bool(const model::Session&, const model::ModSource&)>;
// Whether some route on `target` asks for its lane (`lane`, enabled or not).
bool modulationLane(const model::Session& session, const model::EnvTarget& target);
// The band the enabled routes on `target` draw on its lane: the offsets they
// can add, in the scale's modulation domain (dB for a volume, percent for a
// pan or width, a plug-in parameter's CLAP value: depth x its CLAP range),
// each route over its signal's range (ui::modulationSpan). The lane draws
// modulatedValue(scale, base, low .. high). nullopt when no enabled route
// moves it or the scale has no such unit (a switch, the playrate).
std::optional<ui::ModulationView> modulationBand(const model::Session& session,
                                                 const model::EnvTarget& target,
                                                 const EnvScale& scale,
                                                 const UnipolarSource& unipolar);
// Whether a strip's control takes modulation (ADR-0096, step M2): a
// track's volume and pan, and a channel's or buss's width in width mode,
// while it is not frozen. A dual pan's bars, switches and a VCA's controls
// do not.
bool stripModulatable(const model::Session& s, const model::Id& object, std::string_view param);

// A modulated control (gui.md §2): the span the enabled routes on `target`
// can add, in the route's depth unit (dB on a fader, percent on a pan bar, a
// fraction of the CLAP range, which is the travel, on a plug-in's slider),
// and the route a drag on the control's bar edits, the first enabled one,
// with its depth. nullopt when no enabled route moves it.
struct ControlModulation {
    ui::ModulationView view;
    model::Id route;
};
// `live`: the engine's telemetry (EngineApp::modulationOffsets, each route's
// target's offset now), which sets the view's live value; null or without
// the target's routes: none (not playing).
std::optional<ControlModulation> controlModulation(
    const model::Session& session, const model::EnvTarget& target, const UnipolarSource& unipolar,
    const std::map<model::Id, double>* live = nullptr);
// The target's live offset in `live` (any of its routes': each carries the
// sum over the target), or nullopt.
std::optional<double> liveOffset(const model::Session& session, const model::EnvTarget& target,
                                 const std::map<model::Id, double>& live);

// A parameter's value without automation: a strip's volume, pan, width or
// pan bar, a send's level; the scale's reference for the rest.
double targetValue(const model::Session& session, const model::EnvTarget& target,
                   const EnvScale& scale);

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
