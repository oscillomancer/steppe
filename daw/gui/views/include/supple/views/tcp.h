// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The track control panel (gui.md §4.2): one row per visible strip, sharing
// the arrange view's row geometry and vertical scroll. Each row carries the
// strip's identity (name, type badge, parentage), its state (mute, solo, arm,
// monitor for channels) and its controls (fader, pan, meter).
//
// Every change is a command through the injected `commit` callback (gui.md
// §1); a drag passes one gesture string for the whole drag so the bus
// coalesces it into one undo step (session-model §6.2).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "supple/ui/root.h"

#include "supple/json/value.h"
#include "supple/model/session.h"
#include "supple/ui/controls.h"
#include "supple/ui/geometry.h"
#include "supple/ui/node.h"
#include "supple/views/track_layout.h"

namespace supple::ui {
class ArmButton;
class Button;
class Meter;
class MonitorButton;
}  // namespace supple::ui

namespace supple::views {

struct TcpCallbacks {
    // Selection rules (click, Ctrl+click toggles, Shift+click extends).
    std::function<bool(const model::Id& track)> selected;
    std::function<void(const model::Id& track, unsigned mods)> select;
    // One command, with a gesture name for a drag (empty: a single edit).
    std::function<void(std::string_view command, json::Value args, std::string gesture)> commit;
    // Add a strip of a type ("channel", "buss", "master"): the app runs
    // track.add and selects what it created (the panel does not know the id).
    std::function<void(std::string_view type)> addStrip;
    // The panel pools its rows, so its meters come and go with them: the app
    // attaches a shown meter to its feed and detaches it before it is reused.
    std::function<void(const std::string& track, ui::Meter* meter)> meterShown;
    std::function<void(ui::Meter* meter)> meterHidden;
    // The strip's input button: what to write on it (the app knows the device's
    // port names; `channels` is the view's "1/2"), and the picker. The view
    // hands the button's window rect so the app can anchor its menu there.
    std::function<std::string(const model::Id& track, const std::string& channels)> inputText;
    std::function<void(const model::Id& track, const ui::Rect& anchor)> pickInput;
    // The strip's "env" button: the app's menu of its automation lanes
    // (show/hide volume, pan, mute; creating the envelope when missing).
    std::function<void(const model::Id& track, const ui::Rect& anchor)> envelopes;
    // The strip's "fx" button: its chain; "+" beside it: the plugin browser,
    // adding to the end of the chain.
    std::function<void(const model::Id& track)> fx;
    std::function<void(const model::Id& track)> addFx;
    // A right-click on the row: the track's menu (rename, colour, delete) at
    // the pointer, in window coordinates; `rename` starts the in-place rename.
    std::function<void(const model::Id& track, const ui::Rect& anchor,
                       std::function<void()> rename)>
        menu;
    // A right-click on a fader, pan or button: its MIDI learn menu (ADR-0058).
    // `param` is the control as track `learn` names it (volume, pan, mute,
    // solo, phase_invert, mono, armed, monitor, lane_mute).
    std::function<void(const model::Id& track, const std::string& param, const ui::Rect& anchor)>
        learn;
    // Output drags commit one send on release; a click opens the matrix.
    std::function<Result<json::Value>(const model::Id& from, const model::Id& to)> addSend;
    std::function<void(const Error&)> onError;
    std::function<void(const model::Id& track, const ui::Rect& anchor)> routing;
    // The "mixer" button at the panel's foot: show or hide the mixer.
    std::function<void()> mixer;
    // Dragging a row's bottom edge sets that track's height (UI state, not a
    // command; screensets keep it); <= 0 (a double-click there) clears it.
    std::function<void(const model::Id& track, float height)> resizeTrack;
    // The arrange lanes accept drops too, with the TCP's shared row geometry.
    std::function<ui::Rect()> lanesRect;
};

class TcpPanel : public ui::Node {
  public:
    TcpPanel(const TrackLayout& layout, const Viewport& view, TcpCallbacks callbacks);
    ~TcpPanel() override;

    // The band at the panel's foot ("mixer", "+ strip") that rows never cover;
    // the arrange view scrolls this much further so the last row clears it.
    static constexpr float kFootHeight = 26;

    // Values come from this session snapshot; the rows are rebound.
    void setSession(const model::Session& session);
    // Geometry stayed fixed: refresh only affected controls without rebinding.
    void updateSession(const model::Session& session, const std::set<model::Id>& tracks,
                       const std::set<model::Id>& envelopes, const std::set<model::Id>& routes);
    void refresh();  // values only (after a bus edit)

    bool onPointer(const ui::PointerEvent& e) override;
    ui::Rect rowRect(std::size_t row) const;  // same y as the lanes' rows
    std::size_t rowCount() const { return layout_.rowCount(); }

    // Test and app access: the pooled row currently showing `track`, and one
    // of its controls by name ("name", "mute", "fader", …).
    ui::Node* rowNode(const model::Id& track) const;
    ui::Node* controlNode(const model::Id& track, std::string_view what) const;
    // Starts the in-place rename of the row showing `track` (false: not shown).
    bool rename(const model::Id& track);
    // An automation lane's header control ("title", "mode", "active", "hide").
    ui::Node* envControl(const model::Id& env, std::string_view what) const;

    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override;
    void paint(ui::Canvas& canvas) override;
    // The shared viewport scrolled: the rows move with the lanes at the next
    // paint (one panel re-arranged, not the whole window).
    void scrolled();
    // The foot's buttons are the panel's own chrome: they win over the rows under them.
    ui::Node* hitTest(ui::Point p) override;

  private:
    class Row;
    class EnvRow;  // an automation lane's header (P5.14)
    class OutButton;
    class SendGhost;
    class ReorderGhost;
    friend class Row;
    friend class EnvRow;
    void bindRows();
    void openAddMenu(const ui::Rect& anchor, ui::Placement placement);
    ui::Meter* meterOf(Row* row) const;
    bool sendPointer(const ui::PointerEvent& e, const model::Id& from, const ui::Rect& anchor);
    void cancelSend();
    std::optional<std::size_t> sendRowAt(ui::Point p) const;
    std::optional<std::size_t> sendTargetAt(ui::Point p) const;
    // Dragging a row's empty part reorders it among its same-type siblings.
    struct Reorder {
        model::Id track;
        ui::Point press{};
        bool moved = false;
        // The track.move steps the drop would take (the dragged track and the
        // selected tracks of its parent and type), empty when it changes nothing.
        std::vector<TrackMove> moves;
        float markerY = 0;  // the insertion marker, in window coordinates
    };
    void beginReorder(const model::Id& track, ui::Point press);
    void moveReorder(ui::Point p);
    void endReorder(bool commit);
    void updateReorder(ui::Point p);

    const TrackLayout& layout_;
    const Viewport& view_;
    TcpCallbacks cb_;
    model::Session session_;
    ui::Node* rowsHost_ = nullptr;     // the rows, clipped above the foot
    ui::Node* foot_ = nullptr;         // the foot band with its two buttons
    ui::Button* add_ = nullptr;        // "+ strip", at the panel's foot
    ui::Button* mixer_ = nullptr;      // "mixer" beside it
    std::vector<Row*> rows_;           // a pool: only visible rows exist
    std::vector<EnvRow*> envRows_;     // the same for automation lanes
    std::uint64_t gestureSerial_ = 0;  // unique per drag (coalescing key)
    std::optional<model::Id> sendFrom_;
    ui::Point sendPress_{}, sendPointer_{};
    ui::Rect sendAnchor_{};
    bool sendMoved_ = false, sendOverlay_ = false;
    bool rearrange_ = false;  // scrolled() since the last arrange
    ui::Root::OverlayHandle sendOverlayHandle_;
    std::optional<Reorder> reorder_;
    ui::Root::OverlayHandle reorderOverlayHandle_;
    std::shared_ptr<TcpPanel*> alive_;
};

}  // namespace supple::views
