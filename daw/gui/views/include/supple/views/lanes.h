// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The track lanes: the arrange view's item area (gui.md §4.3). Items are
// drawn with the bulk GL layers (§3): bodies, fades and MIDI notes as
// instanced rectangles, audio as min/max bands sampled from peak pyramids,
// fades as polyline bands. Rows and items outside the painted region are
// skipped (virtualized); the row geometry comes from the shared TrackLayout.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "supple/model/follow.h"
#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/gl.h"
#include "supple/ui/node.h"
#include "supple/views/comp_view.h"
#include "supple/views/track_layout.h"

namespace supple::views {

class Lanes : public ui::Node {
  public:
    using Resolve = std::function<std::filesystem::path(const model::MediaSource&)>;
    Lanes(peaks::PeakService& peaks, Resolve resolve, const TrackLayout& rowLayout,
          const Viewport& view);

    // Rebuilds the per-row item index from the layout's session, then
    // invalidates (the controller calls this after TrackLayout::show).
    void rebuild();
    // Adjust only changed items and rows whose timebase changed. Geometry must
    // be unchanged; visual-only edits do not touch the position index.
    void updateIndex(const model::Session& before, const std::set<model::Id>& items,
                     const std::set<model::Id>& tracks);
    const std::vector<model::Id>& itemsInRow(std::size_t row) const { return rowItems_[row]; }

    // Time <-> x inside the lanes (the arrange controller and the ruler share
    // this mapping, so the ruler lines up with the items).
    double timeAt(float x) const {
        return view_.startSeconds + static_cast<double>(x - bounds().x) / view_.pixelsPerSecond;
    }
    float xAt(double seconds) const {
        return bounds().x +
               static_cast<float>((seconds - view_.startSeconds) * view_.pixelsPerSecond);
    }
    float contentHeight() const { return layout_.contentHeight(); }
    std::size_t rowCount() const { return layout_.rowCount(); }
    ui::Rect rowRect(std::size_t row) const;
    ui::Rect itemRect(std::size_t row, const model::Item& item) const;
    // An automation lane's value area inside its row (gestures map y with it).
    static ui::Rect envArea(const ui::Rect& row);
    // Where a take's envelopes (gap log #22) are drawn over its item: under its label.
    static ui::Rect takeEnvArea(const ui::Rect& item);
    // An automation lane's items (gap log #28): the header band that moves
    // one, and the screen value of an item point (values scale about the
    // item's baseline).
    static constexpr float kAutoHeader = 10;
    static double autoItemValue(const model::AutomationItem& ai, double value) {
        return ai.baseline + ai.amplitude * (value - ai.baseline);
    }
    // An item's name bar (gui.md §4.3): the band across its top, in the item's
    // colour, that carries its label and marks. An item too short for one
    // keeps its label over the body.
    static constexpr float kNameBar = 15;
    static bool hasNameBar(const ui::Rect& item) { return item.h >= 2 * kNameBar; }
    // Where an item's fades are drawn: its body, under the name bar.
    static ui::Rect fadeArea(const ui::Rect& item);
    // The colour an item is drawn in: its own (item.set {color}) when it has
    // one the theme knows, else its track's (§7), else its strip type's.
    static ui::Color itemColor(const ui::Theme& theme, const model::Item& item,
                               const model::Track* track, model::StripType type);
    // What an item is called: its heard take's name, or an unnamed audio
    // take's media file. Empty when it has neither (the view then shows the
    // item's id).
    // The colour of a MIDI item's notes: the item's, drawn towards the ink.
    // On the body, which is a wash of the same hue, the pure colour is hard
    // to read (yellow on yellow in the light theme); towards the ink it is
    // lighter on a dark ground and darker on a light one.
    static ui::Color noteColor(const ui::Theme& theme, ui::Color item);
    static std::string itemLabel(const model::Session& session, const model::Item& item);
    // Take lanes (comping): an item with several takes, tall enough, shows
    // each take in its own lane under its label; the heard one is lit.
    static bool showsTakeLanes(const ui::Rect& item, std::size_t takes);
    static ui::Rect takeLaneRect(const ui::Rect& item, std::size_t takes, std::size_t index);
    // The comp seams of a row (between pieces that play different takes),
    // for the session version they were found at. Each is drawn as a line
    // across the pieces with a notch at the foot; a drag there moves it.
    const std::vector<CompSeam>& compSeams(std::size_t row) const;
    // take.comp.get, for a comp on an edit-grouped track: whether it differs
    // from its group (CompSeam::differs). Set by the app; unset, none differs.
    CompQuery compQuery;

    void arrange(const ui::Canvas& canvas, const ui::Rect& bounds) override;
    void paint(ui::Canvas& canvas) override;
    void paintGl(const ui::GlFrame& frame) override;

    // What the last paint drew (benchmarks, tests).
    struct Stats {
        std::size_t rows = 0, items = 0, columns = 0, notes = 0, pending = 0, fades = 0;
        std::vector<std::string> labels;  // each item's name bar text, as drawn ("": none)
    };
    const Stats& stats() const { return stats_; }
    // The parts of automation lanes the last paint dimmed: where an item's
    // take effect does not play (outside the item, or all of it while
    // another take is active), as a held track's items are dimmed.
    const std::vector<ui::Rect>& dimmedLaneSpans() const { return dimmedLaneSpans_; }
    struct IndexStats {
        std::uint64_t rebuilds = 0, items = 0;
    };
    const IndexStats& indexStats() const { return indexStats_; }
    // Where a modulated lane draws its live value (the layout's
    // liveOffsets): the playhead, in seconds.
    double livePlayhead = 0;

  private:
    friend class LaneLabels;
    void drawAudio(const ui::GlFrame& frame, const ui::Rect& r, const model::Item& item,
                   const model::Take& take, const model::MediaSource& source, std::uint32_t rgba);
    // `heard`: the item's id when `take` is its active take, which a
    // following take draws where it sounds (model::soundingNotes).
    void drawMidi(const ui::Rect& r, const model::Item& item, const model::Take& take,
                  const model::MidiSource& source, std::uint32_t rgba, std::uint32_t border,
                  std::optional<model::Id> heard = std::nullopt);
    // A following take's heard notes, by item, for the session version they
    // were read at (a context edit moves them without touching the item).
    const std::vector<model::SoundingNote>& soundingOf(model::Id item);
    std::uint64_t soundingVersion_ = ~std::uint64_t{0};
    std::map<model::Id, std::vector<model::SoundingNote>> sounding_;
    mutable std::uint64_t seamsVersion_ = ~std::uint64_t{0};
    mutable std::map<std::size_t, std::vector<CompSeam>> seams_;
    void drawFades(const ui::Rect& r, const model::Item& item, std::uint32_t rgba,
                   std::uint32_t shade);
    void drawEnvelope(ui::Canvas& c, std::size_t row, const ui::Rect& r);

    peaks::PeakService& peaks_;
    Resolve resolve_;
    const TrackLayout& layout_;
    const Viewport& view_;
    std::vector<std::vector<model::Id>> rowItems_;  // by position within a row
    std::optional<ui::RectRenderer> bodies_, notes_;
    std::optional<ui::StripRenderer> strips_;
    bool glFailed_ = false;
    const ui::Theme* theme_ = nullptr;  // from the last paint (paintGl has no canvas)
    Stats stats_;
    IndexStats indexStats_;
    std::vector<ui::Rect> dimmedLaneSpans_;
};

}  // namespace supple::views
