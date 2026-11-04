// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Row geometry shared by the arrange view's two columns (gui.md §4.3): one
// ordered row list per session version, one height per row and the same
// vertical scroll in the track control panel and the lanes. UI state per
// gui.md §1.2: the global row height and per-track overrides live here, never
// in the session.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "supple/model/session.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/tcp_geometry.h"

namespace supple::views {

// A visible row: a track and its indentation under the first root (the
// masters' children are at depth 0). An automation lane is a row too: it
// carries its track (and type/depth) plus the envelope it shows (P5.14).
struct TrackRow {
    model::Id track;
    model::StripType type = model::StripType::Channel;
    int depth = 0;
    std::optional<model::Id> env;  // set on an automation lane
};

// A parent's children in display order: channels first, then busses (masters
// last), each type in the parent's own child order. The track list shows this
// order and the mixer groups its strips from it, so a buss never sits between
// two channels (gui.md §4.2). The model's child order is untouched.
std::vector<model::Id> displayChildren(const model::Session& session, const model::Track& parent);

// The masters that have an arrange row (ADR-0057): their lane, for a master
// that is armed or holds items.
std::set<model::Id> masterLanes(const model::Session& session);

// Whether every take of `item` is audio (or empty).
inline bool audioItem(const model::Session& session, const model::Item& item) {
    for (model::Id id : item.takes)
        if (const model::Take* take = session.takes.find(id);
            take && take->source && take->source->kind != model::Kind::Media)
            return false;
    return true;
}

// Whether a row can hold `item` (any item when null; ADR-0057): a channel any
// item, a master's lane audio items only, busses and automation lanes none.
inline bool holdsItem(const model::Session& session, const TrackRow& row, const model::Item* item) {
    if (row.env || row.type == model::StripType::Buss) return false;
    return row.type == model::StripType::Channel || !item || audioItem(session, *item);
}

// Every track in the mixer's strip order: channels, then busses, then masters,
// each type in display order (displayChildren walked from the masters). The FX
// window's track arrows and menu follow it too.
std::vector<model::Id> stripOrder(const model::Session& session);

// Where `track.move {track, index}` puts `track` (a pure reorder under its own
// parent) so it lands in front of its sibling `before`, or after its last
// sibling of the same type when `before` is empty. Only siblings of the same
// type are valid targets; nullopt for anything else (a master, an unknown id, a
// `before` of another type or parent, or `before` the track itself) and for a
// drop that leaves the same-type siblings in the order they already have. The
// index is among the parent's children with the track already taken out, as the
// command reads it. Shared by every view that drags tracks (track list, mixer).
std::optional<std::size_t> reorderIndex(const model::Session& session, model::Id track,
                                        std::optional<model::Id> before);

// The same for several selected tracks dragged together: `track` (the dragged
// one) and the other selected tracks of its parent and type (`selected`), in
// their order, land together in front of `before` (else after the last sibling
// of that type). The track.move steps that do it, in order, each an index as
// the command reads it then; empty when the order would not change.
struct TrackMove {
    model::Id track;
    std::size_t index = 0;
};
std::vector<TrackMove> reorderMoves(const model::Session& session, model::Id track,
                                    const std::function<bool(model::Id)>& selected,
                                    std::optional<model::Id> before);
// The tracks reorderMoves would move: `track` and the selected siblings of its
// type, in their order (`track` alone when it is not selected itself).
std::vector<model::Id> reorderGroup(const model::Session& session, model::Id track,
                                    const std::function<bool(model::Id)>& selected);

// A select callback's flag for a plain press on a selected track: the
// selection stays (a drag from there moves all of it) and the press still
// focuses the tracks; a release without a drag selects the track alone.
inline constexpr unsigned kSelectKeep = 1u << 8;

// The arrange view's horizontal viewport and vertical scroll (gui.md §1.2:
// zoom and scroll are UI state).
struct Viewport {
    double startSeconds = 0;      // time at the left edge of the lanes
    double pixelsPerSecond = 40;  // design units per second
    float scrollY = 0;            // design units scrolled past the first row
};

class TrackLayout {
  public:
    static constexpr float kDefaultHeight = 72;
    static constexpr float kMinHeight = TcpRowGeometry::kMinHeight;
    static constexpr float kMaxHeight = 400;

    // Updates the snapshot; returns true only when row geometry changed. A track
    // whose ancestor is a collapsed folder (Track::folderCompact == 2) is
    // hidden; a "small" folder (== 1) gets half the global height.
    bool show(const model::Session& session, const std::vector<model::Change>* changes = nullptr,
              std::uint64_t generation = 0);

    std::uint64_t rebuilds() const { return rebuilds_; }
    const model::Session& session() const { return session_; }
    const std::vector<TrackRow>& rows() const { return rows_; }
    std::size_t rowCount() const { return rows_.size(); }
    std::optional<std::size_t> indexOf(model::Id track) const;   // the track's own row
    std::optional<std::size_t> indexOfEnv(model::Id env) const;  // an automation lane
    float envHeight() const;  // an automation lane's height (follows the global height)

    float globalHeight() const { return globalHeight_; }
    void setGlobalHeight(float h);  // absolute height, including saved row_height
    // Add design units to every track row, preserving mixed heights and
    // compact-folder sizes. Expanded-take rows share the delta among takes.
    // One geometry notification; no model/history edits.
    void adjustAllTrackHeights(float delta);
    // Per-track height in design units; <= 0 clears the override.
    void setTrackHeight(model::Id track, float h);
    void clearTrackHeights();  // every track back to the global height
    bool hasTrackHeight(model::Id track) const { return heights_.count(track) != 0; }
    // The explicit per-track overrides, by track (saving UI state).
    const std::map<model::Id, float>& heights() const { return heights_; }
    // "Show all takes" (gui.md §4.3): an item whose takes are spread out
    // vertically, each in a lane as tall as its track's row; the row grows to
    // hold them. An item that goes or keeps fewer than two takes drops out.
    static constexpr float kTakeBar = 17;  // the item's name bar above its take lanes
    void setTakesExpanded(model::Id item, bool on);
    bool takesExpanded(model::Id item) const { return expanded_.contains(item); }
    const std::set<model::Id>& expandedItems() const { return expanded_; }
    void clearTakesExpanded();
    float heightOf(std::size_t row) const { return rowHeights_[row]; }
    float yOf(std::size_t row) const { return yOffsets_[row]; }  // content space
    float contentHeight() const { return contentHeight_; }
    // A plugin parameter envelope's scale (the app reads fx.describe); empty
    // or nullopt: plugin lanes are not shown.
    std::function<std::optional<EnvScale>(const model::Envelope&)> pluginScales;
    // A lane's scale: a mixer parameter's, else the plugin's.
    std::optional<EnvScale> scaleOf(const model::Envelope& env) const;
    // Called when the rows or a height changed (the view re-clamps its scroll,
    // refreshes its scroll bars and repaints).
    std::function<void()> onChanged;

  private:
    float baseHeight(model::Id track) const;
    std::map<model::Id, std::size_t> expandedTakeCounts() const;
    void rebuildGeometry();

    std::uint64_t rebuilds_ = 0;
    model::Session session_;
    std::uint64_t generation_ = ~std::uint64_t{0};
    std::uint64_t indexed_ = ~std::uint64_t{0};
    std::vector<TrackRow> rows_;
    std::map<model::Id, std::size_t> trackRows_, envRows_;
    std::set<model::Id> scaledEnvs_;
    std::map<model::Id, float> heights_;
    std::set<model::Id> expanded_;
    std::set<model::Id> masterLanes_;  // the masters with a row (ADR-0057)
    bool pruneExpanded();              // true when one went
    float globalHeight_ = kDefaultHeight;
    std::vector<float> rowHeights_;
    std::vector<float> yOffsets_;
    float contentHeight_ = 0;
};

}  // namespace supple::views
