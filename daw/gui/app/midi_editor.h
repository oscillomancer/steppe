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
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
#include "supple/views/envelope_edit.h"
#include "supple/views/envelope_lane.h"

namespace supple::ui {
class Button;
class Toggle;
class Label;
class TextBlock;
}  // namespace supple::ui

namespace supple::views {
class MouseMap;
}  // namespace supple::views

namespace supple::peaks {
class PeakService;
}  // namespace supple::peaks

namespace supple::gui {

class MidiEventList;
class PluginScales;

// The MIDI take an editor opens (ADR-0097 "The take owner"): an item's active
// take, or a launcher clip's. A clip has no place in time: its beats count
// from its own start.
struct MidiOwner {
    const model::Take* take = nullptr;
    const model::MidiSource* source = nullptr;
    const model::Item* item = nullptr;  // set for an item
    const model::Clip* clip = nullptr;  // set for a clip
    model::Id track;
    // An audio clip's media (its take's source), when `source` is null: the
    // pane shows its waveform and its clip-time lanes (gui.md §4.7).
    const model::MediaSource* audio = nullptr;
};
std::optional<MidiOwner> midiOwner(const model::Session& session, const std::string& id);
// The roll's frame shift for an owner (RollState::frameShift): the whole
// loop lengths in a looped item's start_offset, over the playrate; 0 for an
// unlooped item, a clip or audio. The event list shares it, so both show an
// event where it plays.
double rollFrameShift(const MidiOwner& owner);

struct RollNote {
    int pitch = 60;
    double start = 0, length = 1;  // beats from the item's start
    int velocity = 96, channel = 1;
};

// A clip-time lane (gui.md §4.7): one of a MIDI clip take's envelopes
// (volume, pan, mute) or one on its take FX, in clip beats from the clip's
// start, shown while the envelope is visible.
struct RollEnvLane {
    model::Id env;
    model::Envelope envelope;  // as the session had it at the last reload
    views::EnvScale scale;
    double fallback = 0;  // the value without points
    std::string title;    // "Volume (take)", "Teasel · Drive (take)": the lane's one name
    // A take FX parameter a modulation route shows (`lane`, ADR-0096 M5)
    // without any envelope on it: its value and the band, no points, no
    // press but hide; `env` and `envelope` name nothing.
    bool modOnly = false;
    model::EnvTarget target;
    // The lane's node id: "midi.env.<env>", or "midi.mod.<fx>.<param>".
    std::string key() const {
        return modOnly ? "midi.mod." + target.object.str() + "." + target.param
                       : "midi.env." + env.str();
    }
};

// What the roll, keyboard and velocity lane share.
struct CcPoint {
    double beat = 0;  // from the item's start
    int value = 0;    // 0..127
};

struct RollState {
    std::uint64_t gestureVersion = 0;
    std::string item;         // "it_3", or a launcher clip "cl_2"; empty: closed
    bool clip = false;        // the owner is a clip: it sits at beat 0, in its own context
    double itemPosition = 0;  // project beats
    double itemLength = 4;    // beats
    // A looped item plays its content from start_offset wrapped into one loop
    // length (model::SourceBeatMapping::sourceOffset), and the roll's axis
    // wraps the same way, so a split loop's right part draws its notes where
    // they play. The engine's item beats (midi.get, midi.edit) don't wrap:
    // roll beat = engine beat + frameShift (the whole loop lengths in
    // start_offset, over the playrate; 0 without a loop or for a clip).
    double frameShift = 0;
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
    // A MIDI clip's take envelopes and take FX envelopes, in lane order
    // (volume, pan, mute, then the take FX in chain order); none for an item,
    // whose take lanes are on the timeline.
    std::vector<RollEnvLane> envLanes;
    // The lanes (keys) whose last paint drew a modulation band, and a live
    // value at the playhead (ADR-0096 M5; for tests).
    std::set<std::string> lanesBanded, lanesLive;
    // The clip-time lanes' point selection (gui.md §4.7, as the timeline's):
    // one lane's at a time, its points named by position and value, so
    // points an edit removed simply leave it.
    model::Id envSelLane;
    std::vector<std::pair<double, double>> envSel;
    // The lane pressed last (none after a press on the roll): the editor's
    // copy and paste keys take its points.
    std::optional<model::Id> envLastLane;
    // Where the roll's last paint drew the end mark ("end"), if in view.
    std::optional<float> endMarkX;
    bool envSelected(model::Id env, const model::EnvPoint& p) const {
        if (env != envSelLane) return false;
        for (const auto& [pos, value] : envSel)
            if (std::abs(pos - p.pos) < 1e-9 && std::abs(value - p.value) < 1e-9) return true;
        return false;
    }
    // An audio clip (gui.md §4.7, mock 93): the roll draws its active take's
    // waveform and the axis is in seconds of played time from the clip's
    // start (the clip's own unit), not beats: itemLength, scrollBeat,
    // pixelsPerBeat and the lanes' positions are seconds then.
    bool audio = false;
    struct Wave {
        model::Take take;            // the active take: offset, rate, loop
        std::string hash;            // the media's, for the peak cache
        std::filesystem::path path;  // the media file, resolved
        float gain = 1;              // the clip's and the take's gain
        bool loop = false;           // the clip loops its take
        std::string label;           // the take's name, at the left
    };
    std::optional<Wave> wave;
    std::size_t waveColumns = 0;  // the last paint's waveform columns
    bool wavePending = false;     // its peaks were not ready
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
        // A take row's cell that only names where the take's key changes
        // ("A minor: Am" over the chord holding there): no point to edit.
        bool keyOnly = false;
        // The length of a key change's name before the chord in `text`
        // ("A minor: " in "A minor: Am"), so a drag shows the chord alone.
        std::size_t keyPrefix = 0;
    };
    std::vector<ChordMark> chords;
    // A clip's key points (text: "D dorian"), drawn and dragged in the
    // strip's key row (ADR-0098 X3); none for an item.
    std::vector<ChordMark> keyMarks;
    // A clip's chords and keyMarks show at their clip beat + contextShift
    // (a looped clip's loop start: the roll's axis is the take's content,
    // gui::ClipContextAxis), its first pass only, the last chord ending at
    // contextEnd; `keys` are already in content beats (gui::clipKeySpans).
    double contextShift = 0, contextEnd = 0;
    // A following take (ADR-0098 amended; gui.md §4.7 "A following take"): an
    // item's heard take whose follow is key or chords. The roll draws each note
    // where it sounds and outlines its written pitch, or, with `written` (the
    // Written cell), the written pitches with the sounding ones outlined.
    bool following = false;
    bool written = false;
    bool soundingView() const { return following && !written; }
    // Whether note `i`, as `n`, lies outside the item or clip (drawn dim, as
    // nothing there is heard): a following take's note not heard in the pass
    // shown, else one wholly before the start or from the end on.
    bool outsideItem(std::size_t i, const RollNote& n) const;
    // What a drag of the take row's chord at project beat `beat` shows: the
    // chord alone, without a key change named on its cell ("Am" from
    // "A minor: Am"), as the drag moves only the chord; "r" for none.
    std::string heldTakeChord(double beat) const;
    // What each note sounds as in the pass shown, index for index with
    // `notes` (model::soundingNotes): its pitch (-1: not heard in that pass,
    // drawn as written) and that occurrence's song beat less the roll's
    // (itemPosition + start), so a preview at a moved place reads the same
    // pass.
    struct Sounds {
        int pitch = -1;
        double shift = 0;
        // Not heard in the pass shown (outside the item): `pitch` is what it
        // would sound as by the same place rule past the item's edges, so
        // trimming the edge out over it shows it where it was drawn.
        bool heard = true;
    };
    std::vector<Sounds> sounds;
    int pass = 0, passes = 1;   // the pass shown, of the passes the item plays
    double passShift = 0;       // its song beats less the roll's, for a new note
    bool passesDiffer = false;  // some note sounds otherwise in another pass
    // What a note written as `pitch` on `channel` (1..16) sounds as at song
    // beat `beat` (model::placeAt and soundsAt); set for a following take.
    std::function<int(int pitch, int channel, double beat)> soundsAt;
    // The take's own key at song beat `beat` (model::placeAt), which
    // midi.edit's degrees read on a following take; set with soundsAt.
    std::function<std::optional<harmony::Key>(double beat)> writtenKeyAt;
    // The place a note is named by (ADR-0100), as midi.get names it, so the
    // two spell it alike: `writtenPlace` at a written note's start in roll
    // beats (model::writtenNamePlace for an item's take, at its unwrapped
    // source beat; model::clipNamePlace for a clip, where the note first
    // sounds), `songPlace` at a song beat (model::songNamePlace). Set at
    // reload; unset, notes are named by the default table.
    std::function<harmony::Place(double rollBeat)> writtenPlace;
    std::function<harmony::Place(double songBeat)> songPlace;
    // Note `i`, as `n`, in the view shown: the place it is named by. In the
    // sounding view the song's where it sounds in the pass shown, else
    // writtenPlace at its start.
    harmony::Place namePlace(std::size_t i, const RollNote& n) const;
    // A sounding pitch at roll beat `rollBeat` in the pass shown: the song's
    // place there.
    harmony::Place heardPlace(double rollBeat, double shift) const;
    // The key a degree step of note `i` counts in, at roll-project beat
    // `projectBeat` (itemPosition + start): a following take's own at the
    // note's place (writtenKeyAt, in the pass shown), else the view's.
    std::optional<harmony::Key> degreeKeyAt(std::size_t i, double projectBeat) const;
    // What note `i` sounds as, as `n` (the note or its drag preview) has it;
    // none when the take does not follow or the note is not heard.
    std::optional<int> soundingPitch(std::size_t i, const RollNote& n) const;
    // The pitch note `i` is drawn at (the view's), and its outline's: none
    // where the two agree or the note is not heard.
    int drawnPitch(std::size_t i, const RollNote& n) const;
    std::optional<int> outlinePitch(std::size_t i, const RollNote& n) const;
    // The strip's rows for a take with a context (gui.md §4.7): `chords` is
    // the context the view reads, `otherChords` the other one, dim and
    // read-only, each labelled in the keyboard column ("song", "take"). A
    // take's chords end with the item (`chordsEnd`, `otherEnd`, roll beats);
    // the song's run on.
    std::vector<ChordMark> otherChords;
    std::string rowLabel, otherLabel;
    std::optional<double> chordsEnd, otherEnd;
    std::optional<harmony::Key> keyAt(double projectBeat) const;
    // A note off channel 10 whose pitch is not in the key holding at its
    // start (none holding: false). The roll outlines it, and the title
    // counts them: a clip laid in another key than the place's (ADR-0098
    // context.mismatch) shows on the item's roll, and a clip's notes outside
    // its own key show on its.
    bool outsideKey(const RollNote& note) const;
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
    // An audio clip's snap grid in seconds: the finest of 0.01, 0.02, 0.05,
    // 0.1, 0.2, 0.5, 1 s... at least 20 px wide at the zoom.
    double audioGrid() const;
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
    // {"height", "velocity", "cc", "written"}: the UI sidecar's "midi" section
    // ("cc" 0: the CC lane hidden; "written": a following take's Written cell).
    json::Value saveLayout() const;
    void loadLayout(const json::Value& v);
    // Row (key) height zoom, anchored at the window y whose pitch stays put.
    static constexpr float kMinRowHeight = 4, kMaxRowHeight = 48;
    void zoomRows(float wheel, float anchorY);
    // The mouse modifiers (Edit > Mouse modifiers): what the wheel does here
    // for `mods`, from its midi.wheel bindings (the defaults when unset).
    void setMouseMap(const views::MouseMap* map) { mouse_ = map; }
    // The peak service an audio clip's waveform reads (the app's); none: no
    // waveform is drawn. peaksReady() repaints it when a pyramid arrives.
    void setPeaks(peaks::PeakService* peaks) { peaks_ = peaks; }
    // Modulation on the clip-time lanes (ADR-0096 M5, gui.md §4.7): which
    // sources are 0..1, for the band, and the engine's live offsets (the
    // arrange's, each pass), for the live dot. Unset: every source counts
    // as -1..1 and no live value is drawn.
    void setModulation(const views::UnipolarSource* unipolar,
                       const std::map<model::Id, double>* live) {
        unipolar_ = unipolar;
        live_ = live;
    }
    const views::UnipolarSource& unipolarSource() const;
    const model::Session& head() const;
    const std::map<model::Id, double>* liveOffsets() const { return live_; }
    peaks::PeakService* peaks() const { return peaks_; }
    void peaksReady();
    std::string_view wheelBehaviour(unsigned mods) const;

    // Tests and the actions.
    RollState& state() { return state_; }
    ui::Node* roll() const { return roll_; }
    // The bars the strip above the roll numbered in its last paint, left to
    // right, and the chords its chord row named (tests).
    const std::vector<int>& barLabels() const { return barLabels_; }
    const std::vector<std::string>& chordLabels() const { return chordLabels_; }
    // The strip's second row's names (a take's context, or the song's in the
    // written view), as last painted (tests).
    const std::vector<std::string>& otherChordLabels() const;
    // Edge scroll in the piano roll (gui.md §4.7): true while a drag is held
    // at an edge of the roll; autoScroll advances it by `seconds` and is
    // true when the view moved (sync calls it with the frame's time).
    bool autoScrolling() const;
    bool autoScroll(double seconds);
    ui::TextArea* notation() const { return text_; }
    MidiEventList* eventList() const { return eventList_.get(); }
    std::uint64_t beginGesture();
    // A refusal the roll gives itself (the pencil in the sounding view), to
    // the actions' error hook, so the status line says it.
    void refuse(const std::string& message);
    // News an edit made (a sounding-view drag that lands on a pitch another
    // written pitch also sounds as), to the actions' notice hook.
    void notice(const std::string& message);
    Status edit(json::Value ops, std::optional<std::uint64_t> expectedVersion =
                                     {});  // midi.edit on the open item, keeping the selection
    void selectAll();
    void selectNone();
    void quantize();
    void transpose(int semitones);
    void transposeDegrees(int degrees);  // midi.edit transpose_degrees on the selection
    ui::Toggle* keyButton() const { return keyButton_; }
    ui::Toggle* writtenButton() const { return writtenButton_; }
    ui::Button* passButton() const { return passButton_; }
    const std::string& keyText() const { return keyText_; }  // the key button's words
    // The chord field (views/chord_field.h) for the chord point at the project
    // beat `beat`, opened at the window point `at` (the strip's double-click).
    // On a clip `beat` is a roll beat and it edits the clip's own chord at
    // the clip beat there (ADR-0098; clipBeatAt).
    void editChord(double beat, ui::Point at);
    // A clip's own context (ADR-0098 X3): the key field for the key at clip
    // beat `beat`; the strip's right-click menu at roll beat `rollBeat`
    // (`rollChord`: the roll beat of a chord point under the pointer); the
    // context replaced (clip.set {context}, printed as the lead sheet in clip
    // bars).
    void editClipKey(double beat, ui::Point at);
    void openContextMenu(double rollBeat, std::optional<double> rollChord, ui::Point at);
    // The clip's point of `type` at roll beat `rollFrom` moved to `rollTo`
    // (`copy`: a second one there, the first kept), as a strip drag does.
    void moveClipPoint(model::ContextType type, double rollFrom, double rollTo, bool copy = false);
    // A timeline item's take's own context (ADR-0098 amended, gui.md §4.7),
    // each edit one take.set {context} with the lead sheet in bars of the
    // meter at the item (takeLeadSheet). Beats here are project beats on the
    // strip; a point goes at the context beat of the place (model::placeAt),
    // so a looped context's repeats edit its one period. The chord field for
    // the take's chord there; its key field; the strip's right-click menu
    // (`chord`: a take chord's project beat under the pointer); a point
    // moved, as a strip drag does; the context replaced.
    void editTakeChord(double projectBeat, ui::Point at);
    void editTakeKey(double projectBeat, ui::Point at);
    void openTakeContextMenu(double projectBeat, std::optional<double> chord, ui::Point at);
    void moveTakePoint(model::ContextType type, double fromProject, double toProject,
                       bool copy = false);
    Status setTakeContext(const std::vector<model::ContextPoint>& points);
    // The context beat of a project beat in the open item's heard take, on
    // the 1/960 grid and inside its period; nothing without a MIDI take.
    std::optional<double> takeBeatAt(double projectBeat) const;
    // The clip beat of a roll beat on the clip's strip, or nothing (with the
    // reason in the status line) when a looped clip's point would fall
    // outside the first pass the strip shows.
    std::optional<double> clipBeatAt(double rollBeat);
    // An audio clip's key button: its menu (Key…, Remove key, Take from the
    // project at the playhead).
    void openAudioKeyMenu();
    // A clip-time lane's menu at window point `at` (`pos`, `value`: the
    // lane's position and value there): Insert point, Select all points,
    // Delete points, Point shape…, Hide.
    void openEnvLaneMenu(model::Id env, ui::Point at, double pos, double value);
    // The selected points of state_.envSelLane removed, or given `shape`.
    void deleteEnvSelection();
    // The selected points of state_.envSelLane copied (positions and values,
    // as the timeline copies them); pasted into lane `env` at `pos` (its own
    // clip time: beats, or an audio clip's seconds), replacing the points over
    // the pasted span, or at the place they were copied from. False when
    // there is nothing to paste or no such lane.
    void copyEnvSelection();
    bool pasteEnvPoints(model::Id env, std::optional<double> pos = {});
    bool canPasteEnv() const { return !envClip_.empty(); }
    void repaintLanes();  // the clip-time lanes, after their selection changed
    void shapeEnvSelection(model::PointShape shape);
    Status setClipContext(const std::vector<model::ContextPoint>& points);
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
    // A clip-time lane's edits (gui.md §4.7): its points replaced
    // (env.points.set, clip beats), and an envelope on the clip's take or
    // its take FX shown (made with one point at the parameter's value when it
    // has none) or hidden. Refusals go to the actions' error hook.
    Status setEnvPoints(model::Id env, const std::vector<model::EnvPoint>& points,
                        std::optional<std::uint64_t> expectedVersion = {});
    Status showEnvelope(const model::EnvTarget& target, bool shown);
    // The Lanes menu's parameter list for one take effect, at a window point.
    void openFxParamsMenu(model::Id fx, ui::Point at);

  private:
    // sync's work apart from the edge scroll. `pass`: the pass's time; none
    // when a gesture calls it (the pause after typing is not counted then).
    void follow(std::optional<std::chrono::steady_clock::time_point> pass);
    Status reload();
    void reloadText();
    // `submit`: the user pressed Ctrl+Enter, which also overwrites MIDI that
    // changed elsewhere while the draft was being typed.
    void compileText(bool write, bool submit = false);
    // The item's MIDI, decompiled: by the default table, or `named` by the
    // places the roll's labels read (RollState::writtenPlace).
    std::optional<std::string> modelText(bool named = false) const;
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
    views::EnvClip envClip_;           // the clip-time lanes' copied points
    double envClipAt_ = 0;             // where the first of them was
    std::vector<RollNote> clipboard_;  // starts from the earliest copied note
    float height_ = kDefaultHeight;
    const views::MouseMap* mouse_ = nullptr;
    peaks::PeakService* peaks_ = nullptr;
    ui::Node* panel_ = nullptr;
    // The header's MIDI controls (grid, Snap, Quantize, Notation, List),
    // hidden for an audio clip; applyMode() shows the parts of the pane the
    // open take has.
    std::vector<ui::Node*> midiOnly_;
    bool audioShown_ = false;       // the pane is in its audio form
    bool notationWasShown_ = true;  // the notation pane, before an audio clip hid it
    void applyMode();
    Status reloadAudio();                          // reload() for an audio clip
    void collectEnvLanes(const MidiOwner& owner);  // state_.envLanes from the clip's take
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
    void applyLanes();       // the lanes' visibility from the state
    void refreshEnvLanes();  // the clip-time lanes' nodes from state_.envLanes
    // A command on the open item or clip, then a reload; a refusal goes to
    // the error hook after `what` ("Lanes: …").
    Status command(std::string_view name, json::Value args,
                   std::optional<std::uint64_t> expectedVersion = {},
                   std::string_view what = "Lanes");
    std::unique_ptr<PluginScales> scales_;
    ui::Node* envColumn_ = nullptr;      // the clip-time lanes, under the CC lane
    std::vector<std::string> envNodes_;  // the lanes envColumn_ has nodes for (keys)
    const views::UnipolarSource* unipolar_ = nullptr;
    const std::map<model::Id, double>* live_ = nullptr;
    std::map<model::Id, double> liveDrawn_;  // the offsets the lanes last drew
    ui::Toggle* writtenButton_ = nullptr;    // a following take's Written cell
    ui::Button* passButton_ = nullptr;       // its Pass cell, while passes differ
    std::vector<double> passStarts_;         // each pass's first heard song beat
    void openPassMenu();
    ui::Toggle* keyButton_ = nullptr;
    std::string keyText_ = "No key";  // the key button's words (its width follows them)
    void refreshKeyButton();
    ui::Node* notationPane_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Label* notationTitle_ = nullptr;  // "Notation", "· written" while the take follows
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
    bool textConflict_ = false;  // the MIDI changed under a draft: only Ctrl+Enter writes it
    std::string baseText_;  // the item's MIDI, decompiled by the default table, when the text last
                            // matched it
    std::uint64_t typingSession_ = 0;  // one undo step per focus of the notation panel
    // The first pass after the notation was last typed in: the pause before
    // it compiles counts from there. None: typed in since the last pass.
    std::optional<std::chrono::steady_clock::time_point> typedAt_;
};

}  // namespace supple::gui
