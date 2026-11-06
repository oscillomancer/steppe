// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A module's display (rack.md §3.5): its draw() hook, run on the window's
// own copy of the module struct, which takes the module's knob values and
// the Shared bytes its telemetry carries, and draws through NanoVG in the
// module's coordinates, under its controls. A hook that spends its budget is
// stopped and says so; the rest of the window draws on.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rack_scene.h"
#include "supple/gui/window.h"

struct NVGcontext;

namespace supple::gui {

class ModuleFace {
  public:
    // Null when the type has no draw() (or is a host module).
    static std::unique_ptr<ModuleFace> make(std::shared_ptr<const rack::ModuleType> type);
    ~ModuleFace();
    ModuleFace(const ModuleFace&) = delete;
    ModuleFace& operator=(const ModuleFace&) = delete;

    const rack::ModuleType* type() const { return type_.get(); }
    // The module's knobs and its latest Shared state into the copy.
    void update(const RackScene::Module& m);
    // Runs draw() at the current transform (the module's top-left at 0, 0).
    void draw(NVGcontext* vg, const ui::Theme& theme, const gui::Fonts& fonts, double width,
              double height);
    bool stopped() const { return stopped_; }

  private:
    class Services;
    ModuleFace() = default;
    std::shared_ptr<const rack::ModuleType> type_;
    void* self_ = nullptr;
    std::unique_ptr<script::detail::Instance> record_;
    std::unique_ptr<Services> services_;
    std::uint64_t shown_ = ~std::uint64_t{0};
    bool stopped_ = false;
};

}  // namespace supple::gui
