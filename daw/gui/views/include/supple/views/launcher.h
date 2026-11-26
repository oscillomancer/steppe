// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher pane (docs/specs/gui.md §4.15; ADR-0097, step L3):
// scenes as columns between the track panel and the lanes, on the arrange
// view's one row model, and its cell in the arrange header row. The pane
// draws what the session holds and what the engine plays (LaunchView, given
// each pass); every edit and launch is a command through its callbacks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"
#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/time/timeline.h"
#include "supple/ui/geometry.h"
#include "supple/ui/gl.h"
#include "supple/ui/node.h"
#include "supple/ui/theme.h"
#include "supple/views/item_gestures.h"
#include "supple/views/object_name.h"

namespace supple::views {

class ArrangeView;

// What one track with clips plays, as the engine hears it (from
// EngineApp::launchState; the views do not see the engine).
struct LaunchView {
    enum class Plays { Arrangement, Clip, Held, Stopped };
    Plays plays = Plays::Arrangement;
    std::optional<model::Id> clip;   // Clip and Held
    std::optional<double> position;  // Clip: in its period, in the clip's unit
    // A queued launch: a clip, a stop, or the arrangement.
    enum class Queued { None, Clip, Stop, Arrangement };
    Queued queued = Queued::None;
    std::optional<model::Id> queuedClip;
    bool operator==(const LaunchView&) const = default;
};
using LaunchViews = std::map<model::Id, LaunchView>;  // by track

// What a press would be refused for right now, by clip and by scene, with
// the refusal's code (EngineApp::launchRefusal and sceneLaunchRefusal; the
// views do not see the engine). A clip: track.frozen, launch.playrate (an
// audio clip with take FX or envelopes while the plan keeps the pitch),
// launch.memory, launch.clip. A scene: launch.scene (nothing it holds can
// play) or the first refusal among the clips it would launch together.
// Transport-state refusals (no engine, a relocation, a full queue) are not
// here. The pane draws these clips, and these scenes' ▶, dimmed as a muted
// clip is; a click still asks, and its refusal reaches the status line.
struct LaunchRefusals {
    std::map<model::Id, std::string> clips, scenes;
    bool operator==(const LaunchRefusals&) const = default;
};

struct LauncherCallbacks {
    // One command per gesture (launches are actions, edits one undo step).
    std::function<Result<json::Value>(const std::string&, json::Value)> run;
    std::function<void(const Error&)> onError;
    // A double-click on a clip: open its editor (MIDI: the piano roll; audio:
    // the inspector).
    std::function<void(model::Id clip)> open;
    // The selected clips changed, or a cell was selected (selectedCell set
    // then): the app's selection and Delete follow.
    std::function<void(const std::vector<model::Id>& clips)> onSelection;
    // Removes clips in one undo step (the menu's Delete); unset: one
    // clip.remove each.
    std::function<void(const std::vector<model::Id>& clips)> remove;
    // The header's Capture toggle pressed: arm (true) or end and commit
    // (false) the capture of launches (launch.capture, launch.commit).
    std::function<void(bool on)> capture;
    // The clip menu's Take FX…: open the chain of the clip's active take.
    std::function<void(model::Id take)> takeFx;
    // What laying clips on the timeline did with their keys (followNotice),
    // for the status line; not called when there is nothing to say.
    std::function<void(const std::string&)> onNotice;
};

// How a scene is laid on the timeline (gui.md §4.15): the scene menu's two
// checks, which a scene dropped on the lanes and Lay out scenes… use too.
// One state per project view, saved in the UI sidecar.
struct SceneLaying {
    // follow: chords, the song's key and chords under each item; unset, the
    // command is not told, so each clip keeps its own Follow.
    bool followSong = false;
    // write_context: the clips' own key and chords onto the context track
    // under each section that has none.
    bool writeContext = false;
    bool operator==(const SceneLaying&) const = default;
};

// The pane's geometry, shared by its body and its header cell: one column
// per scene in `order`, then the Arr column, which stays at the pane's right
// edge while the scene columns scroll under it.
struct LauncherColumns {
    static constexpr float kColumn = 64;  // a scene's column
    static constexpr float kArr = 22;     // the Arr column, and the header's + cell
    static constexpr float kMinWidth = 2 * kColumn + kArr;
    std::vector<model::Id> scenes;  // in order
    float x = 0, w = 0;             // the pane's
    float scroll = 0;               // the scene columns scrolled left
    // The x of a scene column's left edge, and the column under an x.
    float columnX(std::size_t column) const {
        return x + static_cast<float>(column) * kColumn - scroll;
    }
    std::optional<std::size_t> columnAt(float px) const;
    float arrX() const { return x + w - kArr; }
    // The right edge the scene columns are drawn up to (the Arr column's left).
    float scenesRight() const { return arrX(); }
    // Width that shows every column, the + cell and the Arr column.
    float contentWidth() const { return static_cast<float>(scenes.size()) * kColumn + 2 * kArr; }
};
LauncherColumns launcherColumns(const model::Session& session);

// Whether every clip of a scene plays now (its header is outlined).
bool scenePlaying(const model::Session& session, const LaunchViews& views, model::Id scene);

// Whether a track has any clip (an empty cell's stop square shows only then).
bool hasClips(const model::Session& session, model::Id track);

// The whole bars a scene laid on the timeline from `bar` (1-based) lasts:
// its longest clip's length rounded up to whole bars there, at least one
// (scene.insert's bars).
std::int64_t sceneBars(const model::Session& session, model::Id scene, int bar);
// The first bar line at or after a beat, as a 1-based bar.
int barAtOrAfter(const time::TimeMap& map, double beat);
// The bar after the arrangement: the first bar line at or after the end of
// its last item or section (bar 1 when it is empty).
int barAfterArrangement(const model::Session& session);

// What a command that laid clips on the timeline did with their keys, in a
// few words for the status line (gui.md §4.15), read from the answer's
// `followed` blocks. The notes are laid as written and sound as the place
// says (ADR-0098, following is playback): "Riff follows: sounds in E minor,
// by degree", "Stabs follows the chords: sounds in E minor", "Keys is laid as
// written: its key C minor differs from D minor at bar 21", "Beat is laid as
// written: drums don't follow"; for scene.insert the scene and its bar, then
// each clip, or counts when they would not fit; for launch.commit
// "Committed: " and each launched clip laid as written under another key
// (`mismatches`). `command` and `args` are what ran (item.add {clip},
// scene.insert or launch.commit); nothing when no clip sounds moved or was
// laid as written in another key.
std::optional<std::string> followNotice(const model::Session& session, const std::string& command,
                                        const json::Value& args, const json::Value& answer);

// What a context edit made in the GUI (key.set, key.remove, chord.set,
// chord.remove) did to the items that follow, for the status line (gui.md
// §4.15): the items whose notes now sound otherwise, found by comparing
// model::soundingNotes before and after. "Riff follows the new chord at bar
// 6", "Bass and Keys follow the new key at bar 9: G major", "3 items follow
// the new chord at bar 6"; where a following note found no key to read,
// "Riff sounds as written from bar 17: no key there". Nothing when no
// following item sounds otherwise.
std::optional<std::string> contextFollowNotice(const model::Session& before,
                                               const model::Session& after,
                                               const std::string& command);

// What take.bake did, for the status line: "Riff baked: 6 notes written as
// they sound", or, when the answer says `unrolled`, "Riff baked: its 4
// passes written out, each as it sounds". `notes` and `passes` are the
// item's heard notes and passes before the bake.
std::string bakeNotice(const std::string& name, std::size_t notes, int passes, bool unrolled);

// The mark before a following clip's key (gui.md §4.15): the one place it
// is spelled. An arrow: "~" is the modulation mark.
inline constexpr std::string_view kFollowMark = "\u2192";

// A clip's key at its start, short, for its launcher cell (gui.md §4.15):
// "Dm", "G", "D dor", "A hm"; with kFollowMark before it when the clip
// follows the key or the chords. "" when the clip has no readable key.
std::string clipKeyMark(const model::Clip& clip);
// A following take's mark for its item's name bar (gui.md §4.3): kFollowMark
// and the take's own key where its content starts, short, as a cell's ("→D");
// the arrow alone when that key is unreadable. "" for a take that does not
// follow.
std::string takeFollowMark(const model::Take& take);

// The clip in a channel's cell of a scene.
std::optional<model::Id> clipIn(const model::Session& session, model::Id track, model::Id scene);

// A clip's colour: its own when the theme knows it, else its track's, else the
// channel colour.
ui::Color clipColor(const ui::Theme& theme, const model::Session& session, const model::Clip& clip);

class LauncherPane : public ui::Node {
  public:
    static constexpr float kEdge = 4;  // the resize zone at the pane's right edge
    // `peaks` and `resolve` draw audio clips' waveforms, as the lanes do.
    using Resolve = std::function<std::filesystem::path(const model::MediaSource&)>;
    LauncherPane(ArrangeView& view, peaks::PeakService& peaks, Resolve resolve);
    void paint(ui::Canvas& canvas) override;
    // The clips' content (notes or waveform, over one period) by the shared
    // item-body functions (item_body.h).
    void paintGl(const ui::GlFrame& frame) override;
    bool onPointer(const ui::PointerEvent& e) override;
    ui::Cursor cursorAt(ui::Point p) const override;
    // A row's cell in a scene column (window coordinates).
    ui::Rect cellRect(std::size_t row, std::size_t column) const;
    // A clip's launch zone (the ▶ in its name bar) and an empty cell's stop
    // square, as a press finds them.
    static ui::Rect playZone(const ui::Rect& cell);
    static ui::Rect stopZone(const ui::Rect& cell);
    // A clip's content area: its cell under the name bar.
    // The strip at the foot of a clip's body that holds its key mark
    // (clipKeyMark), under the preview: its height, 0 for no mark or a body
    // too low for one.
    static constexpr float kKeyStrip = 12;
    static float keyStrip(const ui::Rect& body, const std::string& keyMark);
    static ui::Rect bodyRect(const ui::Rect& cell);
    // The lowest body (above its key strip) that shows an audio clip's fades.
    static constexpr float kFadeRoom = 8;
    // What a press at `p` lands on: a clip's ▶ or body, an empty cell's stop
    // square or the rest of it, a row's Arr dot.
    struct Hit {
        enum class Kind { None, Play, Body, Stop, Cell, Arr };
        Kind kind = Kind::None;
        std::size_t row = 0;
        model::Id track;
        std::optional<model::Id> scene, clip;
    };
    Hit hitAt(ui::Point p) const;
    // Where one dragged item would become a clip, from the cell under the
    // pointer: rows down (a move's row offset) and scenes along (its place in
    // its track's run).
    struct DropCell {
        std::ptrdiff_t row = 0, scene = 0;
    };
    // Items dragged from the lanes over the pane (ItemGestures): each item's
    // cell (`cells`, from the cell under `at`) outlined as it would take its
    // clip (empty, on a channel) or not. A cell past the rows or the scenes,
    // or on an automation lane, or `refused` (an item on a hidden track),
    // refuses the whole drop: the cell under `at` is outlined so.
    void showDrop(std::optional<ui::Point> at, std::vector<DropCell> cells = {},
                  bool refused = false);
    ui::Rect arrRect(std::size_t row) const;
    LauncherColumns columns() const;
    // What the last paint drew (tests).
    struct Stats {
        std::size_t cells = 0, clips = 0, empty = 0;
        std::vector<std::string> names;  // the clips' names as drawn
        std::vector<std::string> keys;   // their key marks as drawn ("Dm"; "" for none)
        // Their name marks' colour tokens as drawn (views::takeNameMark; ""
        // for none).
        std::vector<std::string> marks;
        std::size_t playing = 0, held = 0, queued = 0, arrLit = 0, selected = 0, squares = 0,
                    dropShown = 0;
        bool dropTakes = false;      // the cell under a drop outlined as taking it (focus)
        std::size_t dropHidden = 0;  // a drop's cells out of view, said as "+N" at the edge
        int refused = 0;  // clips drawn dimmed: a press is refused (ArrangeView::launchRefusals)
        std::vector<model::Id> refusedClips;  // those clips, in drawing order
        std::size_t notes = 0, columns = 0, pending = 0, fades = 0;  // the last paintGl's
    } stats;

  private:
    // An audio clip's fades over its body, one pass across `full`; the fades drawn.
    std::size_t drawClipFades(const ui::Rect& body, const ui::Rect& full, const model::Clip& clip,
                              std::uint32_t rgba, std::uint32_t shade);
    ArrangeView& view_;
    peaks::PeakService& peaks_;
    Resolve resolve_;
    std::optional<ui::RectRenderer> notes_;
    std::optional<ui::StripRenderer> strips_;
    bool glFailed_ = false;
    const ui::Theme* theme_ = nullptr;  // from the last paint (paintGl has no canvas)
    bool resizing_ = false;
    float pressX_ = 0, pressWidth_ = 0;
    // A clip held by the pointer: dragged to another cell it moves there
    // (with Ctrl a copy); an occupied cell refuses it.
    struct Drag {
        model::Id clip;
        ui::Point press;
        bool moving = false;
        Hit over;
    };
    std::optional<Drag> drag_;
    std::optional<ui::Point> dropAt_;
    std::vector<DropCell> dropCells_;
    bool dropRefused_ = false;
    bool dropAllowed(const Hit& over) const;
    // The clip's copy on the lanes under `p` (a channel's own row), if any.
    std::optional<ItemGestures::ClipGhost> timelineGhost(model::Id clip, ui::Point p,
                                                         bool free) const;
    void openClipMenu(model::Id clip, ui::Point at);
    void renameClip(model::Id clip, ui::Point at);
};

// The pane's cell in the arrange header row: stop and arrangement for every
// track, and the scene headers in the ruler band's lowest row.
class LauncherHeader : public ui::Node {
  public:
    explicit LauncherHeader(ArrangeView& view);
    void paint(ui::Canvas& canvas) override;
    bool onPointer(const ui::PointerEvent& e) override;
    // The two buttons, and the scene headers' row (window coordinates).
    ui::Rect stopRect() const;
    ui::Rect arrangementRect() const;
    ui::Rect scenesRow() const;
    ui::Rect addRect() const;  // the + cell after the last scene
    // The Capture toggle, after Arrangement: lit while armed
    // (ArrangeView::captureArmed).
    ui::Rect captureRect() const;
    // Stop all, Arrangement and Capture sit in one row. Where their words
    // don't fit (a pane one or two scenes wide), all three are glyph cells
    // (kGlyphCell wide) whose names are their hints, so none overlaps at
    // any width the pane allows.
    static constexpr float kGlyphCell = 22;
    bool glyphCells() const;
    ui::Hint hintAt(ui::Point p) const override;
    // What the last paint drew (tests).
    struct Stats {
        bool captureLit = false;
        bool captureEnding = false;  // lit, outlined dashed (ArrangeView::captureEnding)
        bool captureKept = false;    // dark, its dot in outline (ArrangeView::captureKept)
        bool glyphCells = false;     // Stop all, Arrangement and Capture drawn as glyphs
        int refused = 0;             // scenes whose ▶ is dimmed (ArrangeView::launchRefusals)
        std::vector<model::Id> refusedScenes;  // those scenes, in column order
        std::vector<std::string> names;        // the scenes' names as drawn
    } stats;
    // The scene column under x in the headers' row, and its ▶.
    std::optional<std::size_t> sceneAt(ui::Point p) const;
    ui::Rect sceneRect(std::size_t column) const;
    // A scene laid on the timeline (scene.insert) from `bar`, for its
    // longest clip's whole bars, as the view's SceneLaying says; a refusal
    // goes to onError.
    bool insertScene(model::Id scene, int bar);
    // Scenes laid out one after the other from `bar` (arrangement.build),
    // each for its bars, as the view's SceneLaying says; a refusal goes to
    // onError and is returned.
    Result<json::Value> layOut(const std::vector<std::pair<model::Id, std::int64_t>>& sequence,
                               int bar);
    // The scene menu's Lay out scenes…: the scenes in pane order, each with
    // a check and its bars (its longest clip's whole bars), a start bar (the
    // first whole bar after the arrangement) and the laying's two checks.
    // Lay out runs layOut; a refusal shows in the dialog, which stays open.
    void openLayOut();

  private:
    void openSceneMenu(model::Id scene, ui::Point at);
    // The bar a scene dragged onto the lanes lands on: the bar under `p`.
    std::optional<int> dropBar(ui::Point p) const;
    void renameScene(model::Id scene, ui::Point at);
    ArrangeView& view_;
    // A scene header held by the pointer: dragged sideways it moves its column.
    struct Drag {
        model::Id scene;
        ui::Point press;
        bool moving = false;
    };
    std::optional<Drag> drag_;
};

}  // namespace supple::views
