// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A plugin dragged out of the side panel's browser onto a track
// (docs/specs/gui.md §4.14): a chip with its name follows the pointer, the
// track it would go on is outlined (a row of the arrange view, a strip of
// the mixer), and the release runs one `fx.add` there.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/model/session.h"
#include "supple/ui/list.h"
#include "supple/ui/root.h"

namespace supple::gui {

// A track under the pointer and the part of the window that stands for it.
struct TrackTarget {
    model::Id track;
    ui::Rect rect;
};

class PluginDrag {
  public:
    PluginDrag(ui::Root& root, cmd::Dispatcher& dispatcher);
    // The places a drop may land, asked in the order added; the first that
    // answers wins (the mixer's strips, then the arrange view's rows).
    using Finder = std::function<std::optional<TrackTarget>(ui::Point)>;
    void addTargets(Finder finder);
    // The browser's drag (PluginBrowser::onDrag): the release runs `fx.add`
    // and tells onDrop.
    void drag(const std::string& plugin, const std::string& name, ui::DragPhase phase,
              ui::Point at);
    // Anything else that goes on a track (an FX chain preset): `label` is on
    // the chip, and `drop` runs with the track it was released on.
    using Drop = std::function<void(const model::Id& track)>;
    void drag(const std::string& label, ui::DragPhase phase, ui::Point at, Drop drop);
    // What a drop did or why it did nothing, for the browser's status line:
    // the track it went on, or the command's error.
    std::function<void(const model::Id& track, const Status& result)> onDrop;

    // The drag in progress, for the feedback layer and tests.
    struct View {
        bool active = false;
        std::string plugin, label;
        ui::Point pos;
        std::optional<TrackTarget> target;
    };
    const View& view() const { return *view_; }

  private:
    void end();
    ui::Root& root_;
    cmd::Dispatcher& dispatcher_;
    std::vector<Finder> finders_;
    std::shared_ptr<View> view_;
    ui::Root::OverlayHandle ghost_;
};

}  // namespace supple::gui
