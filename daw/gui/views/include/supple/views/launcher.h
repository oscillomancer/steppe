// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher pane (docs/specs/gui.md §4.15; ADR-0097, step L3):
// scenes as columns between the track panel and the lanes, on the arrange
// view's one row model, and its cell in the arrange header row. The pane
// draws what the session holds and what the engine plays (LaunchView, given
// each pass); every edit and launch is a command through its callbacks.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"
#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/geometry.h"
#include "supple/ui/gl.h"
#include "supple/ui/node.h"
#include "supple/ui/theme.h"
#include "supple/views/item_gestures.h"

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
    static ui::Rect bodyRect(const ui::Rect& cell);
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
    // An item dragged from the lanes over the pane (ItemGestures): the cell
    // under `at` is outlined as a drop that takes it, or refuses it.
    void showDrop(std::optional<ui::Point> at);
    ui::Rect arrRect(std::size_t row) const;
    LauncherColumns columns() const;
    // What the last paint drew (tests).
    struct Stats {
        std::size_t cells = 0, clips = 0, empty = 0;
        std::vector<std::string> names;  // the clips' names as drawn
        std::size_t playing = 0, held = 0, queued = 0, arrLit = 0, selected = 0, squares = 0,
                    dropShown = 0;
        std::size_t notes = 0, columns = 0, pending = 0;  // the last paintGl's
    } stats;

  private:
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
    // The scene column under x in the headers' row, and its ▶.
    std::optional<std::size_t> sceneAt(ui::Point p) const;
    ui::Rect sceneRect(std::size_t column) const;

  private:
    void openSceneMenu(model::Id scene, ui::Point at);
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
