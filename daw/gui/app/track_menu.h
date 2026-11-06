// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A track's right-click menu, shared by the TCP and the mixer: Rename, Color…
// (the swatch menu), Timebase… (channels), Channels… (ADR-0020: the main pair
// and auxiliary channels 3-8), a channel's racks (ADR-0062: Show rack, Add
// rack), Duplicate and Delete. Each pick is one undo step.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::gui {

// Removes the tracks (a selection) as one undo step: the main master stays,
// children go before their folders, and any refusal rolls the whole step back.
Status removeTracks(cmd::Dispatcher& dispatcher, const model::Session& session,
                    const std::vector<std::string>& tracks);

// Duplicates the tracks (a selection; masters are left out) as one undo step
// (track.duplicate); the copies' ids, each right after its original.
Result<std::vector<std::string>> duplicateTracks(cmd::Dispatcher& dispatcher,
                                                 const model::Session& session,
                                                 const std::vector<std::string>& tracks);

struct TrackMenu {
    ui::Root& root;
    cmd::Dispatcher& dispatcher;
    engine::EngineApp& app;
    std::function<void(const std::string&)> onError;  // a refused command (status line)
    // The selected tracks: Delete on one of them removes them all, Duplicate
    // duplicates them all, and the copies become the selection.
    std::function<std::vector<std::string>()> selection;
    std::function<void(std::vector<std::string>)> select;
    // Runs a UI action with {"track": id} (a channel's Show rack, Add rack).
    std::function<void(const char* action, const model::Id& track)> action;

    // `rename` starts the view's own rename (the TCP edits in place); empty:
    // a small rename field opens at the anchor (the mixer).
    void open(const model::Id& track, const ui::Rect& anchor, std::function<void()> rename) const;
    // The colour swatches (model::kTrackColors and None); an empty anchor
    // centres the menu (the ui.track.color action). A pick colours the whole
    // selection when the track is part of it.
    Status openColors(const model::Id& track, const ui::Rect& anchor) const;
    void openGroups(std::vector<std::string> tracks, const ui::Rect& anchor) const;
    void openGroup(model::Id group, std::vector<std::string> tracks, const ui::Rect& anchor) const;
    void openRename(const model::Id& track, const ui::Rect& anchor) const;
    // What a channel's items follow when the tempo changes (track.set timebase).
    void openTimebase(const model::Id& track, const ui::Rect& anchor) const;
    // How many channels the strip has (track.set channels).
    void openChannels(const model::Id& track, const ui::Rect& anchor) const;

  private:
    void commit(std::string_view command, json::Value args) const;
};

}  // namespace supple::gui
