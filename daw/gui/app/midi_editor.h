// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The MIDI editor (docs/specs/gui.md §4.7), docked under the arrange view: a
// piano roll (draw, select, move, resize, velocity, quantize, transpose) and
// a velocity lane over midi.notes / midi.edit, and the notation panel — the
// item's notation text beside the roll, editable, compiled live with located
// diagnostics, written back with midi.write. Either side's edits reach the
// other through the session: one model, two views.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app_actions.h"
#include "supple/harmony/harmony.h"
#include "supple/ui/edge_scroll.h"
#include "supple/ui/root.h"
#include "supple/ui/text_area.h"

namespace supple::ui {
class Button;
class Toggle;
class Label;
class TextBlock;
}  // namespace supple::ui

namespace supple::views {
class MouseMap;
}  // namespace supple::views

namespace supple::gui {

class MidiEventList;

// The MIDI take an editor opens (ADR-0097 "The take owner"): an item's active
// take, or a launcher clip's. A clip has no place in time: its beats count
// from its own start.
struct MidiOwner {
    const model::Take* take = nullptr;
    const model::MidiSource* source = nullptr;
    const model::Item* item = nullptr;  // set for an item
    const model::Clip* clip = nullptr;  // set for a clip
    model::Id track;
};
std::optional<MidiOwner> midiOwner(const model::Session& session, const std::string& id);

struct RollNote {
    int pitch = 60;
    double start = 0, length = 1;  // beats from the item's start
    int velocity = 96, channel = 1;
};

// What the roll, keyboard and velocity lane share.
struct CcPoint {
    double beat = 0;  // from the item's start
    int value = 0;    // 0..127
};

struct RollState {
    std::uint64_t gestureVersion = 0;
    std::string item;         // "it_3", or a launcher clip "cl_2"; empty: closed
    bool clip = false;        // the owner is a clip: it sits at beat 0, no chords
    double itemPosition = 0;  // project beats
    double itemLength = 4;    // beats
    std::vector<RollNote> notes;
    std::set<std::size_t> selected;
    // The colour the notes are drawn in: the item's in the arrangement (its
    // own, else its track's, else its strip type's), so the roll and the
    // item it edits look alike. Unset: the accent.
    std::function<ui::Color(const ui::Theme&)> color;
    // The CC lane: one controller at a time (channel 1). The Lanes menu
    // shows or hides it and the velocity lane.
    int ccNumber = 1;
    bool ccShown = true, velocityShown = true;
    std::vector<CcPoint> cc;
    std::vector<CcPoint> ccDraft;  // a drag in progress (drawn, not yet committed)
    bool ccErasing = false;
    double ccFrom = 0, ccTo = 0;  // the drag's span (beats)
    // View: beats from the item start at the left edge, the pitch at the top
    // edge, zoom.
    double scrollBeat = -0.5, pixelsPerBeat = 80;
    double topPitch = 84;
    float rowHeight = 12;
    double grid = 0.25;  // beats
    bool snap = true;
    double lastLength = 0.25;
    int lastVelocity = 96;
    // A drag in progress, previewed locally and committed once on release.
    enum class Drag { None, Move, ResizeEnd, ResizeStart, Marquee, Velocity } drag = Drag::None;
    double dBeats = 0, dLength = 0, dStart = 0;
    int dPitch = 0, dVelocity = 0;
    std::optional<std::size_t> velocityNote;  // the note a velocity drag picked
    ui::Rect marquee{};
    // Project grid for bars/beats (from the time map).
    std::vector<double> barLines;  // project beats of bar starts in view
    int firstBar = 1;              // the project bar barLines starts with
    double playheadBeat = -1;      // project beats; < 0: hidden
    // The context track over the roll (ADR-0095), in project beats, in a
    // window around the item: the key holding at its start and each change
    // after (none: no key holds; an unknown scale reads as none), and the
    // chord points the same way ("" for no chord, "?" after a symbol this
    // build cannot read).
    struct KeySpan {
        double beat = 0;
        std::optional<harmony::Key> key;
    };
    std::vector<KeySpan> keys;
    struct ChordMark {
        double beat = 0;
        std::string text;
        bool known = true;
    };
    std::vector<ChordMark> chords;
    std::optional<harmony::Key> keyAt(double projectBeat) const;
    // The key button (gui.md §4.7): a pitch drag and the arrow keys move the
    // selected notes by scale degrees of the key at each note's beat
    // (harmony::transposeDegrees, as midi.edit's transpose_degrees does).
    bool degrees = false;
    int dDegrees = 0;  // a degree drag's preview
    void cancelGesture() {
        drag = Drag::None;
        dBeats = dLength = dStart = 0;
        dPitch = dVelocity = 0;
        dDegrees = 0;
        velocityNote.reset();
        ccDraft.clear();
        ccErasing = false;
    }
    double snapBeat(double itemBeat) const;  // to the project grid when snapping
};

class MidiEditor {
  public:
    MidiEditor(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
               engine::EngineApp& app, UiState& ui);
    ~MidiEditor();

    std::unique_ptr<ui::Node> build();  // the docked panel (hidden until opened)
    Status open(const std::string& item);
    void close();
    bool isOpen() const { return !state_.item.empty(); }
    // Every loop pass: reload when the session changed, compile the notation
    // after a pause in typing, follow the playhead. `now` is the pass's time,
    // which the caller gives (the app the clock's, a test its own); it steps
    // the edge scroll and counts the pause after typing.
    void sync(std::chrono::steady_clock::time_point now);
    // When the next pass is due for the notation's compile: the pause's end,
    // or at once (a time in the past) when it was typed in since the last pass.
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;

    // The panel's height, set by dragging its top border (double-click resets
    // to kDefaultHeight). The stored height is the preference; sync() shrinks
    // what is laid out when the window cannot fit it.
    static constexpr float kDefaultHeight = 420, kMinHeight = 160;
    float height() const { return height_; }
    void setHeight(float height);
    // {"height", "velocity", "cc"}: the UI sidecar's "midi" section ("cc" 0:
    // the CC lane hidden).
    json::Value saveLayout() const;
    void loadLayout(const json::Value& v);
    // Row (key) height zoom, anchored at the window y whose pitch stays put.
    static constexpr float kMinRowHeight = 4, kMaxRowHeight = 48;
    void zoomRows(float wheel, float anchorY);
    // The mouse modifiers (Edit > Mouse modifiers): what the wheel does here
    // for `mods`, from its midi.wheel bindings (the defaults when unset).
    void setMouseMap(const views::MouseMap* map) { mouse_ = map; }
    std::string_view wheelBehaviour(unsigned mods) const;

    // Tests and the actions.
    RollState& state() { return state_; }
    ui::Node* roll() const { return roll_; }
    // The bars the strip above the roll numbered in its last paint, left to
    // right, and the chords its chord row named (tests).
    const std::vector<int>& barLabels() const { return barLabels_; }
    const std::vector<std::string>& chordLabels() const { return chordLabels_; }
    // Edge scroll in the piano roll (gui.md §4.7): true while a drag is held
    // at an edge of the roll; autoScroll advances it by `seconds` and is
    // true when the view moved (sync calls it with the frame's time).
    bool autoScrolling() const;
    bool autoScroll(double seconds);
    ui::TextArea* notation() const { return text_; }
    MidiEventList* eventList() const { return eventList_.get(); }
    std::uint64_t beginGesture();
    Status edit(json::Value ops, std::optional<std::uint64_t> expectedVersion =
                                     {});  // midi.edit on the open item, keeping the selection
    void selectAll();
    void selectNone();
    void quantize();
    void transpose(int semitones);
    void transposeDegrees(int degrees);  // midi.edit transpose_degrees on the selection
    ui::Toggle* keyButton() const { return keyButton_; }
    // The chord field (views/chord_field.h) for the chord point at the project
    // beat `beat`, opened at the window point `at` (the strip's double-click).
    void editChord(double beat, ui::Point at);
    void nudge(int steps);  // the selected notes, by grid steps
    // The selected notes; with the event list shown, its selected rows.
    void removeSelected();
    void duplicate();
    // The note clipboard: copies keep the notes' pitches and their places
    // from the earliest; a paste puts the earliest at `itemBeat` (default: the
    // playhead, in the item) and selects what it added.
    void copySelected();
    void cutSelected();
    void paste(std::optional<double> itemBeat = {});
    bool canPaste() const { return !clipboard_.empty(); }
    // The grid's right-click menu at a window point (`itemBeat` under it):
    // Delete, Copy, Cut, Paste (there), Duplicate.
    void openNoteMenu(ui::Point at, double itemBeat);

  private:
    // sync's work apart from the edge scroll. `pass`: the pass's time; none
    // when a gesture calls it (the pause after typing is not counted then).
    void follow(std::optional<std::chrono::steady_clock::time_point> pass);
    Status reload();
    void reloadText();
    // `submit`: the user pressed Ctrl+Enter, which also overwrites MIDI that
    // changed elsewhere while the draft was being typed.
    void compileText(bool write, bool submit = false);
    std::optional<std::string> modelText() const;  // the item's MIDI, decompiled
    void refreshTitle();
    float maxHeight() const;
    void applyHeight();
    void showDiagnostics(const std::vector<ui::TextArea::Mark>& marks, const std::string& summary);
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    RollState state_;
    std::string lastItem_;             // the item opened last (ui.midi.toggle reopens it)
    std::vector<RollNote> clipboard_;  // starts from the earliest copied note
    float height_ = kDefaultHeight;
    const views::MouseMap* mouse_ = nullptr;
    ui::Node* panel_ = nullptr;
    std::unique_ptr<MidiEventList> eventList_;
    ui::Node* listPane_ = nullptr;
    ui::Node* rollPane_ = nullptr;
    ui::Node* roll_ = nullptr;
    ui::EdgeScrollClock edgeClock_;  // the time sync steps the edge scroll by
    ui::Node* keys_ = nullptr;
    ui::Node* bars_ = nullptr;
    std::vector<int> barLabels_;
    std::vector<std::string> chordLabels_;
    ui::Node* velocity_ = nullptr;
    ui::Node* ccLane_ = nullptr;
    ui::Button* lanesButton_ = nullptr;
    void openLanesMenu();
    void applyLanes();  // the lanes' visibility from the state
    ui::Toggle* keyButton_ = nullptr;
    std::string keyText_ = "No key";  // the key button's words (its width follows them)
    void refreshKeyButton();
    ui::Node* notationPane_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::TextArea* text_ = nullptr;
    ui::TextBlock* diagnostics_ = nullptr;
    ui::Root::OverlayHandle playheadHandle_;
    ui::Node* playhead_ = nullptr;  // in the root's overlay
    std::uint64_t version_ = 0;
    std::uint64_t gestureGeneration_ = 0;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::string appliedText_;  // the text last compiled into the item
    std::string titleBase_;
    bool textDirty_ = false, compilePending_ = false;
    bool textConflict_ = false;        // the MIDI changed under a draft: only Ctrl+Enter writes it
    std::string baseText_;             // the item's MIDI, decompiled, when the text last matched it
    std::uint64_t typingSession_ = 0;  // one undo step per focus of the notation panel
    // The first pass after the notation was last typed in: the pause before
    // it compiles counts from there. None: typed in since the last pass.
    std::optional<std::chrono::steady_clock::time_point> typedAt_;
};

}  // namespace supple::gui
