// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A rack as the rack window draws it (docs/specs/rack.md §8): its modules
// with their faces (the type's layout, rack.md §4.5), their knob values and
// telemetry, and its cables with the points they join, all in the rack's
// space (design units). Built from the session and the module catalog; no
// GL, so tests read it directly.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_engine.h"
#include "supple/model/session.h"
#include "supple/ui/geometry.h"
#include "supple/ui/theme.h"

namespace supple::gui {

using Signal = script::detail::PortModel::Signal;

// The theme token of a signal kind's colour ("signal-audio", …).
const char* signalToken(Signal s);
// Where a parameter's value sits on its knob, 0..1 (its curve applied), and
// back.
double paramPosition(const script::detail::ParamModel& p, double physical);
double paramFromPosition(const script::detail::ParamModel& p, double position);
// Where a knob's value arc starts: its zero when the range crosses it, else
// its minimum (a position).
double paramOrigin(const script::detail::ParamModel& p);
// A value as a knob says it: "-6.0 dB", "440 Hz", "Saw", "On".
std::string paramText(const script::detail::ParamModel& p, double physical);

struct RackScene {
    struct Module {
        model::Id id;
        std::shared_ptr<const rack::ModuleType> type;  // null: an unknown type
        std::string typeId;
        std::string title;     // the label, the type's name, a plug-in's name
        std::string subtitle;  // a plug-in's vendor; an Out module's channel
        double x = 0, y = 0, w = 0, h = 0;
        rack::PanelLayout face;      // the type's layout (a plugin module's own)
        std::vector<double> values;  // each parameter's knob, physical
        bool bypass = false, plugin = false, out = false;
        bool instrument = false;      // a plugin module's plug-in plays notes
        std::optional<model::Id> fx;  // a plugin module's
        // Telemetry, when the module runs and is watched.
        rack::Telemetry telemetry;
        bool live = false;
        ui::Rect rect() const {
            return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                    static_cast<float>(h)};
        }
    };
    struct Cable {
        model::Id id;
        model::CableEnd from, to;
        Signal signal = Signal::Audio;  // the output's
        std::string color;              // the cable's own ("" by signal)
        bool param = false;             // into a parameter
        bool feedback = false;          // closes a loop: carries the previous frame
        double amount = 0;
    };
    model::Id rack;
    std::vector<Module> modules;  // in id order
    std::vector<Cable> cables;

    // Rack `rack` of `s` (an empty scene when it is not there).
    // `instrument` says whether a plugin module's plug-in plays notes (its
    // face then shows the note inputs).
    static RackScene build(const model::Session& s, model::Id rack, const rack::Catalog& catalog,
                           const std::function<bool(const model::PluginRef&)>& instrument = {});

    const Module* module(model::Id id) const;
    Module* module(model::Id id);
    // Where a cable end sits in the space: a jack's centre, a knob's, or for
    // a plug-in's parameter the plugin module's edge.
    std::optional<ui::Point> endPoint(const model::CableEnd& end, bool output) const;
    // A port's signal by end (Audio when unknown).
    Signal signalOf(const model::CableEnd& end, bool output) const;
    // The cables into an input or parameter, or out of an output.
    std::vector<const Cable*> cablesAt(const model::CableEnd& end, bool output) const;
    // Everything's bounds (empty when there are no modules).
    ui::Rect bounds() const;

  private:
    std::map<model::Id, std::size_t> index_;
};

}  // namespace supple::gui
