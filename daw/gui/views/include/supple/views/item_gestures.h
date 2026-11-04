// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Editing gestures in the arrange view (docs/specs/gui.md §4.3, P5.6;
// reaper-ux.md 8-13): select (click, Shift add, Ctrl toggle, marquee on empty
// space or a right-drag anywhere: it takes the items and the automation points
// inside it, on any track), move in time and across channels (snapped
// to the grid at the current zoom; Shift moves freely; Ctrl-drag copies), trim either edge (the
// left edge moves the take's source offset so the audio stays put; Alt on the
// right edge stretches: the play rate follows the length), slip
// (Alt-drag: the source moves under the item), fade handles at the top
// corners (Ctrl+click cycles a fade's shape), the take badge cycles takes, double-click opens a
// MIDI item, and files dropped on the lanes. An item showing its take lanes: click a lane to hear
// that take, swipe across one to comp that range (split + take), drag the name bar to move it. On
// an automation lane: click selects a point (Ctrl toggles; a right-drag or a Ctrl-drag from empty
// space is the marquee) or adds one on empty space, drag moves the selected points, in every
// lane (snapped; Shift frees), Alt+click deletes one, a right-click opens the lane's menu (points,
// automation items, the envelope). Ctrl-drag below the name bar of an item with several takes takes
// that take (the one under the pointer, else the heard one) out into an item of its own, on any
// channel. A right-click on an item (without dragging) opens its menu: Cut, Copy, Paste (at the
// pointer), Duplicate, Delete, Consolidate items, Heal splits, Merge takes for
// one multi-take MIDI item, the splits, Export… (the item's raw audio or its MIDI
// to a file, through onExport), timebase, take FX
// and envelopes, and for several takes: show all takes, explode them onto tracks or along the
// track, delete the take under the pointer; and rendering the item through its FX (MIDI to audio)
// in place or as a new take; on empty lane space, a menu with Paste. Each gesture previews as a
// ghost on the root's overlay and commits ONE transaction on release.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "supple/ui/root.h"

#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/model/session.h"
#include "supple/ui/geometry.h"
#include "supple/views/envelope_edit.h"
#include "supple/views/mouse_map.h"

namespace supple::ui {
class Root;
class Node;
struct PointerEvent;
}  // namespace supple::ui

namespace supple::views {

class ArrangeView;

struct EditStep {
    std::string command;
    json::Value args;
};

// The item.add argument an imported or dropped file goes to: midi_file for a
// .mid/.midi file (any case), else audio_file.
const char* importArg(const std::filesystem::path& file);

class ItemGestures {
  public:
    // Runs the steps as one transaction (one undo step) labelled `label`; the
    // result is the last step's.
    using Runner = std::function<Result<json::Value>(const std::string& label,
                                                     const std::vector<EditStep>& steps)>;
    explicit ItemGestures(ArrangeView& view);
    ~ItemGestures();

    Runner run;
    std::function<void(const std::vector<model::Id>&, const std::string&, double)> onAudioEdit;
    std::function<void(const std::vector<model::Id>&)> onConsolidate;
    // The item menu's Export…: the app asks where, then runs item.export.
    std::function<void(model::Id item)> onExport;
    std::function<std::uint64_t()>
        projectGeneration;  // app identity, even before the next resnapshot
    // Keep the copied version available to item.paste until the clipboard changes.
    std::function<std::shared_ptr<const model::Session>(std::uint64_t)> pinVersion;
    std::function<void(const std::vector<model::Id>&)> onSelection;  // after every change
    std::function<void(model::Id item)> onOpenMidi;                  // double-click on a MIDI item
    std::function<void(model::Id take)> onTakeFx;  // the item menu's Take FX… (the app's FX window)
    std::function<void(double seconds)> onSeek;    // a click on empty space
    std::function<void(const Error&)> onError;     // a refused gesture (status line)
    // The keys bound to an app action ("Ctrl+C"), shown in the menus.
    std::function<std::string(std::string_view action)> keysFor;
    // REAPER's default: a move, copy or trim that makes two items overlap on a
    // track fades both over the overlap (in the same transaction).
    bool autoCrossfade = true;
    // What each modifier does where the press lands (gap log #37); the ruler
    // reads it too.
    MouseMap mouse;

    const std::vector<model::Id>& selection() const { return selection_; }
    void setSelection(std::vector<model::Id> items);  // no onSelection call
    bool isSelected(model::Id item) const;

    // From ArrangeView::onPointer (everything but the wheel); true when used.
    bool pointer(const ui::PointerEvent& e);
    void installOverlay(ui::Root& root);  // the ghost / selection layer
    // Files dropped at a window point: one transaction (an item per file, on
    // the channel under the point, further files on new tracks below).
    Status drop(const std::vector<std::filesystem::path>& files, ui::Point at);

    // The snap grid at the current zoom, in beats (1/16 .. 32).
    double gridBeats() const;
    // An empty MIDI item on a channel from the start of the bar holding
    // `fromBeat` to `toBeat` (at least that bar), selected and opened
    // (onOpenMidi): one transaction. Also a double-click on empty lane space.
    Result<model::Id> newMidiItem(model::Id track, double fromBeat,
                                  std::optional<double> toBeat = {});

    // The clipboard (Ctrl+C/X/V, Ctrl+D): the selected items with the version
    // they were copied at, so a paste reproduces them as copied (item.paste),
    // even after a cut. A paste puts the earliest item at `atBeat` and the
    // topmost one on `track` (default: where it was), keeping the others'
    // offsets in time and rows; the pasted items become the selection.
    void clearClipboard() { clipboard_ = {}; }
    void copySelection();
    Status cutSelection();
    Status paste(double atBeat, std::optional<model::Id> track = {});
    Status duplicateSelection();  // right after the selection, on the same tracks
    // Removes the selected items (one undo step): the item menu's Delete.
    Status deleteSelection();
    // MIDI items on one track (or one item's MIDI takes) into one item
    // (midi.merge, one undo step), which becomes the selection.
    bool canMerge() const;
    Status mergeSelection();
    bool canConsolidate() const;
    bool canHeal() const;
    void consolidateSelection();  // background action; completion is one engine edit
    Status healSelection();
    // Menu and S: split only items containing the edit cursor, one undo step.
    // Zero-crossing mode skips items without an active audio take.
    Status splitSelection(bool zeroCrossing = false);
    Status addStretchMarkerAtCursor();  // selected audio, one undo step; peers once
    bool canPaste() const {
        return !clipboard_.items.empty() &&
               (!projectGeneration || projectGeneration() == clipboard_.generation);
    }
    // Selected automation points, in any number of envelopes (a click selects
    // in one lane, a marquee in every lane it covers). The keyboard's Delete
    // and clipboard keys act on them while the lanes have the focus
    // (onEnvelopeFocus); the lane's menu offers the same.
    std::optional<model::Id> pointEnvelope() const { return pointEnv_; }  // the lane clicked last
    std::vector<model::Id> pointEnvelopes() const;  // the envelopes with selected points
    // The selected points that the envelopes still have (as they are now):
    // every envelope's, or one's.
    std::vector<model::EnvPoint> selectedPoints() const;
    std::vector<model::EnvPoint> selectedPoints(model::Id env) const;
    // `points` of `env` become the whole point selection.
    void selectPoints(model::Id env, std::vector<model::EnvPoint> points);
    void selectAllPoints(model::Id env);
    void clearPoints();
    // A marquee took items and points: Delete removes both (deleteAll), and
    // the clipboard keys take the items.
    bool mixedSelection() const { return mixed_ && !selection_.empty() && !pointSel_.empty(); }
    Status deletePoints();  // in every envelope, one undo step
    Status deleteAll();     // the selected items and points, one undo step
    void copyPoints();
    Status cutPoints();
    // Pastes the copied points at `atBeat`, replacing the points over their
    // span: one envelope's into `env` (default: the lane clicked last);
    // several envelopes' back into their own, keeping their offsets.
    Status pastePoints(double atBeat, std::optional<model::Id> env = {});
    bool canPastePoints() const { return !pointClip_.parts.empty(); }
    Status shapePoints(model::PointShape shape);
    // A press on an automation lane (true) or elsewhere in the lanes (false).
    std::function<void(bool)> onEnvelopeFocus;

    // The play rate that keeps a take's material filling the item when its
    // end moves by `deltaBeats` (the stretch gesture).
    double stretchRate(const model::Item& item, const model::Take& take, double deltaBeats) const;

    // What a drag is doing (the overlay and tests read it).
    enum class Drag {
        None,
        Warp,
        Move,
        TrimLeft,
        TrimRight,
        Slip,
        FadeIn,
        FadeOut,
        Marquee,  // items and automation points (on a lane, a right-click without a drag: its menu)
        EnvPoint,
        Comp,
        AutoItemMove,
        AutoItemLength,
        TakeOut  // Ctrl-drag on a take: it leaves its item for a place of its own
    };
    struct State {
        Drag drag = Drag::None;
        std::optional<std::size_t> warpIndex;
        double warpSource = 0, warpPosition = 0;
        std::uint64_t warpVersion = 0, warpGeneration = 0;
        model::Id anchor{};               // the item under the press
        double deltaBeats = 0;            // move / trim-left / trim-right / slip, snapped
        int deltaRows = 0;                // move
        double fadeSeconds = 0;           // fade in/out length being drawn
        bool stretch = false;             // Alt on the right edge: length and play rate together
        std::size_t takeLane = 0;         // comp / take out: the take (index) under the press
        double compFrom = 0, compTo = 0;  // comp: the range being swiped (beats)
        ui::Rect marquee{};
        std::vector<model::Id> marqueeBase;  // what a marquee adds to
        std::map<model::Id, std::vector<model::EnvPoint>> pointBase;
        // An automation lane's point being added or dragged (P5.14): a click
        // on the lane adds one, a drag moves it, Alt+click deletes one.
        std::optional<model::Id> env;
        std::vector<model::EnvPoint> envDraft;
        std::vector<model::EnvPoint> envBase;  // the model's points at the press
        std::size_t envIndex = 0;
        double envOrigPos = 0;
        bool envCreated = false;
        // An automation item (gap log #28): its points being edited (envDraft
        // holds them as played, in the loop pass `envOffset` beats in), or the
        // item moved by its header / resized by its right edge (deltaBeats).
        std::optional<model::Id> autoItem;
        // Retain the immutable press-time item without copying its point buffer.
        std::shared_ptr<const model::AutomationItem> autoBase;
        double envOffset = 0;
        // Several selected points dragged together: the selection as drafted.
        std::vector<model::EnvPoint> envMoved;
        std::vector<model::EnvPoint> envPicked;  // the pressed lane's selection at the press
        // Other envelopes' selected points moving along.
        struct Along {
            model::Id env;
            std::vector<model::EnvPoint> base, picked, draft, moved;
        };
        std::vector<Along> envAlong;
    };
    const State& state() const { return state_; }

    // Geometry helpers (also the overlay's).
    struct Hit {
        model::Id item{};
        std::size_t row = 0;
        ui::Rect rect{};
        Drag zone = Drag::None;
        bool takeBadge = false;
        std::optional<std::size_t> takeLane;  // a take lane under the point (comping)
    };
    std::optional<Hit> hit(ui::Point p) const;
    // A visible take envelope of the item's active take at `p` (on a point or
    // near its line), if any: a press there edits the envelope, not the item.
    std::optional<model::Id> takeEnvelopeAt(model::Id item, ui::Point p) const;
    std::optional<std::size_t> rowAt(float y) const;
    // The automation item of envelope `env` under `p`, the part pressed
    // (AutoItemMove: its header band, AutoItemLength: its right edge, EnvPoint:
    // its body) and the beat its loop pass there starts at.
    struct AutoHit {
        model::Id item{};
        Drag zone = Drag::None;
        double passStart = 0;
    };
    std::optional<AutoHit> autoItemAt(model::Id env, ui::Point p) const;
    ArrangeView& view() { return view_; }
    const ArrangeView& view() const { return view_; }

  private:
    double beatAtX(float x) const;
    // A right-click on an item (no drag): selection deletion/timebase and active take FX/envelopes.
    void openItemMenu(ui::Point at);
    void openSpaceMenu(ui::Point at);
    // The clipboard and merge entries both menus start with.
    void addEditItems(std::vector<ui::MenuItem>& items, ui::Point at, bool onItem);
    // A right-click on an automation lane: its points (insert, select, copy,
    // paste, delete, shape), automation items (insert, or edit the one under
    // the pointer) and the envelope (bypass, hide, remove).
    void openEnvMenu(model::Id env, ui::Point at);
    void openShapeMenu(ui::Point at);
    using EnvEdits = std::vector<std::pair<model::Id, EnvEdit>>;
    // One env.points.set per envelope; adoptPoints makes their selections the
    // point selection.
    std::vector<EditStep> pointSteps(const EnvEdits& edits) const;
    void adoptPoints(const EnvEdits& edits);
    Status setPoints(const EnvEdits& edits, const std::string& label);
    EnvEdits pointDeletions(std::size_t* count) const;
    // Lanes in row order, then visible take envelopes: what a marquee covers.
    std::vector<model::Id> shownEnvelopes() const;
    void marqueePoints();  // the points inside the marquee, added to its base
    std::optional<model::Id> pointEnv_;
    std::map<model::Id, std::vector<model::EnvPoint>> pointSel_;  // none empty
    bool mixed_ = false;
    struct PointClip {
        struct Part {
            model::Id env;
            EnvClip clip;
            double at = 0;  // beats after the earliest copied point
        };
        std::vector<Part> parts;
    } pointClip_;
    bool envPress(model::Id env, const ui::PointerEvent& e,
                  std::optional<AutoHit> autoItem = std::nullopt);
    void envDrag(const ui::PointerEvent& e);
    void commitEnv(const std::string& label);
    bool checkAutoItem() const;
    double snap(double beat, bool free) const;
    void commit(const ui::PointerEvent& e);
    void changed();
    ArrangeView& view_;
    std::vector<model::Id> selection_;
    State state_;
    ui::Point press_{};
    bool moved_ = false, free_ = false, copy_ = false, cycleFade_ = false, marqueeAdd_ = false;
    std::size_t pressRow_ = 0;
    ui::Root::OverlayHandle overlay_;
    std::shared_ptr<ItemGestures*> alive_;  // the overlay's ghost checks it
    struct Clip {
        std::uint64_t generation = 0, version = 0;
        std::shared_ptr<const model::Session> pin;
        std::optional<model::Id> anchorTrack;
        struct Entry {
            model::Id item;
            double position = 0, end = 0;
            model::Id track;
            std::ptrdiff_t channelOffset = 0;
            bool audio = true;  // a master's lane takes only audio items (ADR-0057)
        };
        std::vector<Entry> items;
    } clipboard_;
    Clip clip() const;  // the selection as a clip of the shown version
    Status pasteClip(const Clip& clip, double atBeat, std::optional<model::Id> track,
                     const std::string& label);
    ui::Root* root_ = nullptr;
};

}  // namespace supple::views
