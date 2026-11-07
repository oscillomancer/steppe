// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A channel's Rack device (gui.md §4.5, §4.12): a rack that feeds the
// channel, shown in the FX window's chain with its name, Open (the rack
// window on that rack) and its Macro modules' cabled knobs, named after what
// they drive. The knobs turn module.set (a drag is one undo step), offer a
// rack knob's right-click menu, and follow playback through the Macro
// modules' telemetry.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "learn_dialog.h"
#include "rack_controls.h"
#include "supple/ui/controls.h"
#include "supple/ui/widgets.h"

namespace supple::gui {

namespace sd = script::detail;

class RackDevice : public ui::Node {
  public:
    struct Hooks {
        std::function<void(model::Id rack)> open;
        // A command; a drag's edits share `gesture` (one undo step).
        std::function<void(const std::string& command, json::Value args,
                           const std::string& gesture)>
            exec;
        std::function<void(LearnRequest)> learn;
        // A knob's right-click menu, opened at a window point.
        std::function<void(std::vector<ui::MenuItem> items, ui::Point at)> menu;
        // A plug-in parameter's name (fx, key), for a knob cabled into one;
        // else the cable's key.
        std::function<std::string(model::Id fx, const std::string& key)> paramName;
    };
    RackDevice(engine::EngineApp& app, model::Id rack, Hooks hooks);

    model::Id rack() const { return rack_; }
    // The Macro modules whose knobs it shows (their telemetry is read).
    const std::vector<model::Id>& macroModules() const { return macros_; }
    // The knobs from the session, or from a running Macro's telemetry (its
    // automation, as it plays). Returns whether one moved.
    bool follow();
    // Tests: the knobs as shown, and where one is (window).
    struct Shown {
        std::string group, name, value;
        model::Id module;
        std::string key;
    };
    std::vector<Shown> shown() const;
    std::optional<ui::Point> knobAt(const std::string& name) const;

    ui::Size measure(const ui::Canvas& c, ui::Size available) override;
    void arrange(const ui::Canvas& c, const ui::Rect& bounds) override;
    void paint(ui::Canvas& c) override;
    std::string valueText() const override;

  private:
    class MacroKnob;
    struct Knob {
        model::Id module;
        int param = -1;
        std::string key, group, name, value;
        MacroKnob* knob = nullptr;
        double physical = 0;  // as shown
    };
    struct Group {
        std::string title;
        std::size_t first = 0, count = 0;  // in knobs_
    };
    const sd::ParamModel* paramOf(const Knob& k) const;
    void show(Knob& k, double physical);
    void turn(Knob& k, double position, ui::Gesture g);
    void openMenu(const Knob& k, ui::Point at);
    int columns(float width) const;

    engine::EngineApp& app_;
    model::Id rack_;
    Hooks hooks_;
    std::shared_ptr<const rack::Catalog> catalog_;
    std::shared_ptr<const rack::ModuleType> macroType_;
    std::string title_;
    ui::Button* open_ = nullptr;
    std::vector<Knob> knobs_;
    std::vector<Group> groups_;
    std::vector<model::Id> macros_;
    std::vector<rack::Telemetry> telemetry_;  // per macros_
    std::vector<ui::Rect> cells_;             // per knobs_, as arranged
    std::vector<float> captions_;             // per groups_: the caption's top
    std::uint64_t gesture_ = 0;
};

}  // namespace supple::gui
