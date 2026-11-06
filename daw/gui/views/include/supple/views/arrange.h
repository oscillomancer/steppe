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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include "supple/ui/root.h"

#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/geometry.h"
#include "supple/ui/list.h"
#include "supple/ui/node.h"
#include "supple/views/item_gestures.h"
#include "supple/views/lanes.h"
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
    bool poll(bool force = false);  // force after input/model service, bypassing the throttle

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

    Lanes& lanes() { return *lanes_; }
    ItemGestures& gestures() { return *gestures_; }  // P5.6: editing gestures
    ui::Node& tcp();
    ui::Node& ruler();
    void setRulerCallbacks(RulerCallbacks callbacks);
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

  private:
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
    std::unique_ptr<ItemGestures> gestures_;
    TcpCallbacks tcpCallbacks_;
    ui::ScrollBar* hbar_ = nullptr;
    ui::ScrollBar* vbar_ = nullptr;
    double lastPollMs_ = -1e9;
    ui::Root::OverlayHandle overlay_;
    std::shared_ptr<ArrangeView*> alive_;
    bool overlayInstalled_ = false;
};

}  // namespace supple::views
