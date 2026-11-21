// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The arrange view (gui.md §4.3): the ruler (bars|beats, time,
// tempo/meter, markers/regions, the loop range) above the lanes, with the
// track control panel column to the left sharing the row geometry and the
// vertical scroll; the playhead lives on the root's overlay tree so a running
// transport never repaints the content.
//
// Edits go through command callbacks; drags preview locally and commit once.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include "supple/ui/root.h"

#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/edge_scroll.h"
#include "supple/ui/geometry.h"
#include "supple/ui/list.h"
#include "supple/ui/node.h"
#include "supple/views/item_gestures.h"
#include "supple/views/lanes.h"
#include "supple/views/launcher.h"
#include "supple/views/tcp.h"
#include "supple/views/track_layout.h"

namespace supple::ui {
class Root;
struct PointerEvent;
struct KeyEvent;
}  // namespace supple::ui

namespace supple::views {

class Ruler;
class TcpHeader;

// Transport state the arrange view reads (gui.md §3.3: telemetry, not
// commands). Wired by the app to `EngineApp`; tests inject it.
struct Transport {
    bool playing = false;
    double seconds = 0;
    bool loopEnabled = false;
    double loopStart = 0, loopEnd = 0;
    bool punchEnabled = false;
    double punchStart = 0, punchEnd = 0;
};

using TransportSource = std::function<Transport()>;

struct RulerCallbacks {
    // One command per completed gesture. Transport actions are not undoable.
    std::function<Result<json::Value>(const std::string&, json::Value)> run;
    std::function<void(const Error&)> onError;
};

// gui.md §4.3: the view follows the playhead off, by page (default: it jumps
// when the playhead leaves it) or continuously (while playing it scrolls with
// the playhead held in the middle; stopped, it pages).
enum class Follow { Off, Page, Continuous };

// The UI file stores the follow mode by name (gui.md §1.2).
std::string_view followName(Follow f);
std::optional<Follow> followFrom(std::string_view name);

// What the ruler's last paint produced (tests: tick density, label overlap).
// `minBarSlack` / `minTimeSlack` are the smallest gap between two labels minus
// the previous label's width and the 12-unit breathing space: < 0 would mean
// labels can touch.
struct RulerStats {
    int barLabels = 0, beatLabels = 0, subTicks = 0, timeLabels = 0;
    float minBarLabelGap = 1e9f, minTimeLabelGap = 1e9f;  // design units
    float minBarSlack = 1e9f, minTimeSlack = 1e9f;
    std::vector<int> labelledBars;         // the bars that carry a label, left to right
    std::vector<std::string> keyLabels;    // the key changes' labels, left to right
    std::vector<std::string> chordLabels;  // the chord cells' labels in view, left to right
    std::vector<std::pair<float, float>> chordSpans;  // those cells' left and right edges
    std::vector<float> chordLabelXs;                  // where each label's text starts
};

// The ruler's rows, top to bottom (gui.md §4.3). Its height is their sum, a
// run-time value: the header row, the ruler's painting and hit tests and the
// tracks' visible height all read it from here, so a row can be added or
// hidden (the context track's chord row) with nothing else to change.
struct RulerRows {
    static constexpr float kChordHeight = 18;  // the chord row, when shown
    float beat = 22;                           // bars | beats; the loop range
    float time = 15;                           // m:ss.mmm; the punch range
    float tempo = 20;                          // tempo and meter flags, key changes
    float region = 15;                         // regions
    float chord = 0;                           // chords (ADR-0095): kChordHeight when shown, else 0
    float marker = 16;                         // markers
    float tempoTop() const { return beat + time; }
    float regionTop() const { return beat + time + tempo; }
    float chordTop() const { return regionTop() + region; }
    float markerTop() const { return chordTop() + chord; }
    float height() const { return markerTop() + marker; }
    bool operator==(const RulerRows&) const = default;
};

class ArrangeView : public ui::Node {
  public:
    using Resolve = Lanes::Resolve;

    ArrangeView(peaks::PeakService& peaks, Resolve resolve, TransportSource transport = {});
    ~ArrangeView() override;

    // Takes the session to show (cheap: sessions share structure) and
    // updates only the affected rows/items; history hints are relative to the
    // currently displayed snapshot (forward or backward).
    void show(const model::Session& session, const std::vector<model::Change>* changes = nullptr,
              std::uint64_t generation = 0);
    // Reads the transport (at most once per 16 ms) and moves the playhead;
    // true when the playhead moved (the app loop keeps its wait short then).
    // force after input/model service, bypassing the throttle. `now` is the
    // pass's time, which the caller gives (the app the clock's, a test its
    // own); it also steps the edge scroll.
    bool poll(bool force, std::chrono::steady_clock::time_point now);

    // Edge scroll: a drag in the lanes or the ruler held within the zone at
    // an edge of the lanes, or past it, scrolls the view that way, faster
    // the further out, and the thing in hand goes with the pointer. The
    // lanes' moves and marquees also scroll the rows. noteDrag is told of
    // each pointer event of a drag (by the view and its ruler); autoScroll
    // advances the scroll by `seconds` and is true when the view moved
    // (poll calls it with the time since the last pass, at most
    // ui::EdgeScrollClock::kMaxStep of it).
    void noteDrag(const ui::PointerEvent& e, bool ruler);
    bool autoScroll(double seconds);
    bool autoScrolling() const { return edge_.vx != 0 || edge_.vy != 0; }

    const Viewport& viewport() const { return view_; }
    void setViewport(const Viewport& v);
    const TrackLayout& trackLayout() const { return layout_; }
    TrackLayout& trackLayout() { return layout_; }

    void setPlayhead(double seconds);
    double playhead() const { return playhead_; }
    bool playing() const { return transport_.playing; }
    void setFollow(Follow f) { follow_ = f; }
    Follow follow() const { return follow_; }
    // Per-project UI sidecar/screensets: viewport, row heights, follow and takes.
    json::Value saveLayout() const;
    void loadLayout(const json::Value& value);
    // Clamps the vertical scroll once the children have their bounds (a
    // viewport set before the first layout cannot know the visible height).
    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override;
    void setLoopRange(double start, double end, bool on);
    bool loopEnabled() const { return transport_.loopEnabled; }
    double loopStart() const { return transport_.loopStart; }
    double loopEnd() const { return transport_.loopEnd; }
    void setPunchRange(double start, double end, bool on);
    bool punchEnabled() const { return transport_.punchEnabled; }
    double punchStart() const { return transport_.punchStart; }
    double punchEnd() const { return transport_.punchEnd; }
    bool installOverlay(ui::Root& root);  // the playhead node; false if none
    // Modulation telemetry (ADR-0096, M2), each pass: the modulated
    // controls' live values and the lanes' live dot at the playhead follow.
    void setModulationOffsets(std::map<model::Id, double> offsets);

    Lanes& lanes() { return *lanes_; }
    ItemGestures& gestures() { return *gestures_; }  // P5.6: editing gestures
    ui::Node& tcp();
    ui::Node& ruler();
    void setRulerCallbacks(RulerCallbacks callbacks);
    const RulerRows& rulerRows() const { return rulerRows_; }
    void setRulerRows(const RulerRows& rows);  // resizes the header row and relays out
    // The chord row (gui.md §4.3): shown while the project has a chord, unless
    // View > Chords chose (`shown`, saved in the UI file); nullopt: that rule.
    void setChordRow(std::optional<bool> shown);
    std::optional<bool> chordRowChoice() const { return chordRowChoice_; }
    bool chordRowShown() const { return rulerRows_.chord > 0; }
    // The clip launcher pane (gui.md §4.15): shown while View > Clip launcher
    // chose it (`shown`, saved in the UI file) or, with no choice, while the
    // project has scenes.
    void setLauncherShown(std::optional<bool> shown);
    std::optional<bool> launcherChoice() const { return launcherChoice_; }
    bool launcherShown() const;
    // Its width as laid out; set by dragging its right edge (0: wide enough
    // for its columns). Dragged all the way it fills the view and the lanes
    // and ruler hide; dragged back, they return.
    float launcherWidth() const;
    bool launcherFull() const { return launcherFull_; }
    void dragLauncherWidth(float width);
    void setLauncherWidth(float width, bool full = false);  // 0: fit the columns
    // The scene columns scrolled left under the Arr column.
    float launcherScroll() const { return launcherScroll_; }
    void setLauncherCallbacks(LauncherCallbacks callbacks) {
        launcherCallbacks_ = std::move(callbacks);
    }
    const LauncherCallbacks& launcherCallbacks() const { return launcherCallbacks_; }
    // What each track with clips plays (EngineApp::launchState), each pass:
    // the pane's playing, queued and held cells and Arr dots, and the held
    // tracks' dimmed lanes. Redraws only when it changes.
    void setLaunchState(LaunchViews views);
    const LaunchViews& launchState() const { return launchViews_; }
    // The launcher header's Capture toggle, lit while the app has it armed
    // (set each pass).
    void setCaptureArmed(bool armed);
    bool captureArmed() const { return captureArmed_; }
    // The launcher's selection (gui.md §4.15): clips (a click on a body; Ctrl
    // adds), or one empty cell (a click beside its square) where a clip can
    // be made or pasted. A click on the lanes clears it.
    const std::vector<model::Id>& selectedClips() const { return selectedClips_; }
    void selectClips(std::vector<model::Id> clips);
    struct Cell {
        model::Id track, scene;
        bool operator==(const Cell&) const = default;
    };
    const std::optional<Cell>& selectedCell() const { return selectedCell_; }
    void selectCell(std::optional<Cell> cell);
    void clearLauncherSelection();
    // Runs a launcher command through its callbacks; a failure goes to onError.
    Result<json::Value> runLauncher(const std::string& command, json::Value args);
    LauncherPane& launcherPane() { return *launcherPane_; }
    LauncherHeader& launcherHeader() { return *launcherHeader_; }
    void addMarker();  // at the playhead, through marker.add
    void addRegion();  // over the loop range, through region.add
    // What the panel needs from the app (selection, commands, meters). Call
    // before build/insert; the panel keeps the callbacks.
    void setTcpCallbacks(TcpCallbacks callbacks) { tcpCallbacks_ = std::move(callbacks); }
    TcpPanel& tcpPanel() { return *tcp_; }
    // Row rectangles: the lanes' and the TCP's x, the same y and height
    // (the alignment guarantee of §4.3).
    ui::Rect rowRect(std::size_t row) const;
    ui::Rect tcpRowRect(std::size_t row) const;
    float rowHeight() const { return layout_.globalHeight(); }
    const RulerStats& rulerStats() const;

    std::uint64_t projectGeneration() const { return generation_; }
    const model::Session& session() const { return layout_.session(); }
    double timeAt(float x) const { return lanes_->timeAt(x); }
    float xAt(double seconds) const { return lanes_->xAt(seconds); }
    ui::Rect lanesRect() const { return lanes_->bounds(); }
    float playheadX() const { return lanes_->xAt(playhead_); }
    double viewWidthSeconds() const;
    // The session's length plus a margin (the horizontal scrollbar's extent).
    double totalSeconds() const;

    // The zoom keys (ui.arrange.zoom_in/out): one 1.25x step, holding the
    // playhead's time in place.
    void zoomStep(bool in);

    bool onPointer(const ui::PointerEvent& e) override;
    ui::Cursor cursorAt(ui::Point p) const override;

  private:
    void invalidateModulatedLanes();
    void damageViews();
    float scrollHeight() const;
    void clampScroll();
    void applyFollow();
    void syncScrollbars();
    void zoomBy(double factor, double seconds, float offset, bool cursor);

    peaks::PeakService& peaks_;
    Resolve resolve_;
    TransportSource transportSource_;
    std::uint64_t generation_ = ~std::uint64_t{0};
    TrackLayout layout_;
    Viewport view_;
    Transport transport_;
    double playhead_ = 0;
    // The last horizontal zoom step's anchor (zoomBy): the time held, where on
    // the lanes it was held, and the offset given for it (the pointer's, for a
    // pointer anchor). Any other move in time clears it.
    struct ZoomHold {
        double seconds = 0;
        float offset = 0;
        float given = 0;
        bool cursor = false;
    };
    std::optional<ZoomHold> zoomHold_;
    Follow follow_ = Follow::Page;
    Lanes* lanes_ = nullptr;
    TcpPanel* tcp_ = nullptr;
    Ruler* ruler_ = nullptr;
    TcpHeader* header_ = nullptr;
    ui::Node* headerRow_ = nullptr;  // the TCP header, the ruler and the corner
    RulerRows rulerRows_;
    std::optional<bool> chordRowChoice_;
    void syncChordRow();
    LauncherPane* launcherPane_ = nullptr;
    LauncherHeader* launcherHeader_ = nullptr;
    ui::Node* lanesColumn_ = nullptr;
    std::optional<bool> launcherChoice_;
    float launcherWidth_ = 0;  // 0: fit the columns
    bool launcherFull_ = false;
    float launcherScroll_ = 0;
    LauncherCallbacks launcherCallbacks_;
    LaunchViews launchViews_;
    bool captureArmed_ = false;
    std::vector<model::Id> selectedClips_;
    std::optional<Cell> selectedCell_;
    float launcherRoom() const;  // the widest the pane is before it fills the view
    void syncLauncher();
    std::unique_ptr<ItemGestures> gestures_;
    TcpCallbacks tcpCallbacks_;
    ui::ScrollBar* hbar_ = nullptr;
    ui::ScrollBar* vbar_ = nullptr;
    double lastPollMs_ = -1e9;
    struct EdgeScroll {
        float vx = 0, vy = 0;       // design units per second
        ui::PointerEvent last;      // the drag's latest event, delivered again after a scroll
        bool ruler = false;         // the drag is the ruler's, not the lanes'
        ui::EdgeScrollClock clock;  // the time poll steps it by
    } edge_;
    ui::Root::OverlayHandle overlay_;
    std::shared_ptr<ArrangeView*> alive_;
    bool overlayInstalled_ = false;
};

}  // namespace supple::views
