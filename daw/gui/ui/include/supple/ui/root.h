// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// One window's UI (docs/specs/gui.md §2): the content tree, an overlay tree
// (popups, drag ghosts, playhead-like layers painted every frame), focus,
// hover, pointer capture and damage. Content repaints only damaged regions
// into a cached framebuffer; each frame blits the cache, paints overlays
// and presents.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/gui/window.h"
#include "supple/ui/node.h"
#include "supple/ui/popup.h"

union SDL_Event;

namespace supple::ui {

// The shortest time between two presented frames on a display of `refreshHz`
// (60 when unknown): three quarters of the frame period, the frame rate being
// the refresh rate divided by the smallest whole number that brings it to
// 120 Hz or below. Vsync still paces frames; this bounds them where vsync is
// off, and keeps high-refresh displays at <= 120 fps with even frame times.
std::chrono::steady_clock::duration minFrameInterval(float refreshHz);

class Root {
  public:
    Root(gui::Window& window, const gui::Fonts& fonts, Theme theme);
    ~Root();
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;

    void setContent(std::unique_ptr<Node> content);
    Node* content() const { return content_.get(); }
    Node& overlay() { return *overlay_; }
    class OverlayHandle {
      public:
        OverlayHandle() = default;
        ~OverlayHandle() { reset(); }
        OverlayHandle(OverlayHandle&& other) noexcept;
        OverlayHandle& operator=(OverlayHandle&& other) noexcept;
        OverlayHandle(const OverlayHandle&) = delete;
        OverlayHandle& operator=(const OverlayHandle&) = delete;
        void reset();
        explicit operator bool() const { return node_ != nullptr; }

      private:
        friend class Root;
        OverlayHandle(std::weak_ptr<Root*> root, Node* node)
            : root_(std::move(root)), node_(node) {}
        std::weak_ptr<Root*> root_;
        Node* node_ = nullptr;
    };
    OverlayHandle addOverlay(std::unique_ptr<Node> node);
    Node* find(std::string_view id) const;
    const Theme& theme() const { return theme_; }
    // Device pixels per design unit, as of the last frame (1 before the first).
    float pixelRatio() const { return ratio_; }
    void setTheme(Theme theme);

    // --- window integration
    // Motion while a coalescesDrag() control holds the capture is kept, the
    // latest only, until flushMotion(): by frame(), or before any other input.
    void handle(const SDL_Event& e);
    bool motionPending() const { return pendingMotion_.has_value(); }
    void flushMotion();
    bool wantsFrame() const;
    // Lays out if needed, repaints damage into the cache, blits it, paints the
    // overlays. present=false composes a snapshot for readBack().
    void frame(bool present = true);
    // wantsFrame(), and minFrameInterval() has passed since the last presented
    // frame. frameDeadline() is when a wanted frame may draw, for the wait.
    bool frameReady(std::chrono::steady_clock::time_point now) const;
    std::optional<std::chrono::steady_clock::time_point> frameDeadline() const;
    gui::Image readBack() const;

    // --- damage and layout
    void damage(const Rect& area);
    void damageAll();
    void relayout() { layoutPending_ = true; }
    void requestAnimationFrame() { animate_ = true; }

    // --- focus, hover, capture
    void focus(Node* node);
    Node* focused() const { return focused_; }
    Node* hovered() const { return hovered_; }
    // Where the pointer was last seen over this root (keys act on the panel
    // under it); empty before any pointer event.
    std::optional<Point> lastPointer() const { return lastPointer_; }
    void capture(Node* node);
    // Release focus and cancel a captured gesture inside a hidden subtree.
    void releaseWithin(const Node* subtree);
    void forget(Node* node);  // a node is going away

    // --- popups (popup.h): returns the content node, now owned by the overlay
    Node& openPopup(std::unique_ptr<Node> content, const Rect& anchor,
                    Placement placement = Placement::Below, std::function<void()> onClose = {});
    void closePopup();  // the topmost
    void closePopups();
    bool popupOpen() const { return !popups_.empty(); }

    // --- driving the UI from tests and scripts (same paths as SDL input)
    void pointer(PointerEvent e);
    bool click(std::string_view id, int button = 1);
    void key(int keycode, unsigned mods = 0, bool repeat = false);
    void keyUp(int keycode, unsigned mods = 0);
    // Keys no focused node used, while no popup is open (shortcuts, §6);
    // true when consumed.
    std::function<bool(const KeyEvent&)> onUnhandledKey;
    void text(std::string_view utf8);
    void composition(std::string_view utf8, int cursor);
    // Where the focused text field's caret is (design units), for the IME
    // candidate window.
    void setTextInputArea(const Rect& caret);
    // Clipboard (the system's; tests may run without one).
    std::string clipboard() const;
    void setClipboard(const std::string& text);

    struct Stats {
        std::uint64_t frames = 0, repaints = 0;
        double repaintedArea = 0;  // design units², content cache only
    };
    const Stats& stats() const { return stats_; }

  private:
    friend class Node;
    class Dispatch {
      public:
        explicit Dispatch(Root& root) : root_(root) { ++root_.dispatchDepth_; }
        ~Dispatch() {
            if (--root_.dispatchDepth_ == 0) root_.retired_.clear();
        }

      private:
        Root& root_;
    };
    void retire(std::shared_ptr<Node> node);
    bool attached(const Node* node) const;
    Node* pointerTarget(Point point);
    void layoutNow(const Canvas& canvas);
    void paintTree(Node& node, Canvas& canvas, const Rect& clip);
    void paintGl(Node& node, const Rect& clip);
    void ensureCache();
    gui::Window& window_;
    gui::Fonts fonts_;
    Theme theme_;
    std::shared_ptr<Node> content_, overlay_;
    std::shared_ptr<Root*> alive_;
    unsigned dispatchDepth_ = 0;
    std::vector<std::shared_ptr<Node>> retired_;
    Node* focused_ = nullptr;
    Node* hovered_ = nullptr;
    std::optional<Point> lastPointer_;
    Node* captured_ = nullptr;
    std::optional<PointerEvent> pendingMotion_;  // for captured_ (handle)
    std::vector<Rect> damage_;
    bool layoutPending_ = true, animate_ = false;
    std::optional<std::chrono::steady_clock::time_point> presented_;
    std::chrono::steady_clock::duration frameInterval_{};
    Size laidOut_{};
    float ratio_ = 1;  // of the frame being painted
    struct Cache;
    std::unique_ptr<Cache> cache_;
    Stats stats_;
    std::vector<Node*> popups_;  // PopupFrames owned by overlay_, topmost last
};

}  // namespace supple::ui
