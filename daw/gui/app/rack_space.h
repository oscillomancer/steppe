// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The rack window's space (docs/specs/rack.md §8): a plain background where
// modules sit anywhere, drawn from a RackScene. Middle-drag or Space-drag
// pans, Ctrl+wheel zooms at the pointer (10-400 %), the wheel scrolls.
// Modules are dragged by their body, selected by a click, Ctrl+click or a
// marquee, and move together. Jacks are circles in their signal's colour,
// lit by level; a drag from one draws a cable to a jack of the other
// direction or a knob. Cables sag and sit above the modules. Knobs drag
// vertically (Shift: fine) and show their modulated value as an arc.
//
// The node only draws and gestures; what an edit means is the window's
// (Hooks), which dispatches commands.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "rack_face.h"
#include "rack_scene.h"
#include "supple/ui/controls.h"
#include "supple/ui/node.h"

namespace supple::gui {

class RackSpace : public ui::Node {
  public:
    // What the space shows: the space point at its top-left, and the zoom.
    struct View {
        double x = 0, y = 0, zoom = 1;
        bool operator==(const View&) const = default;
    };
    static constexpr double kMinZoom = 0.1, kMaxZoom = 4.0;
    // Below this zoom the faces' small words go, and each module's name is
    // drawn at a readable size over it instead.
    static constexpr double kWordsZoom = 0.55;

    struct Move {
        model::Id module;
        double x = 0, y = 0;  // its new top-left
    };
    struct Hooks {
        // Modules dragged to new places (one undo step).
        std::function<void(const std::vector<Move>&)> move;
        // A plugin module's body clicked (not dragged): its editor.
        std::function<void(model::Id module)> open;
        // A right-click on a module (selected first), or on the space.
        std::function<void(model::Id module, ui::Point at)> moduleMenu;
        std::function<void(ui::Point at, ui::Point space)> spaceMenu;
        // A knob or other control moved: parameter `param` of `module`, physical.
        std::function<void(model::Id module, int param, double value, ui::Gesture g)> param;
        // A right-click on a control: its MIDI learn and automation menu.
        std::function<void(model::Id module, int param, ui::Point at)> paramMenu;
        // A cable drawn from `from` (an output) to `to` (an input or a
        // parameter); `replace`: the cable it moves, if any.
        std::function<void(const model::CableEnd& from, const model::CableEnd& to,
                           std::optional<model::Id> replace)>
            connect;
        // A cable dragged off its input and dropped on nothing.
        std::function<void(model::Id cable)> disconnect;
        // A cable from `from` dropped on a plugin module's body (at a window
        // point): which of its plug-in's parameters it modulates is asked.
        std::function<void(const model::CableEnd& from, model::Id module,
                           std::optional<model::Id> replace, ui::Point at)>
            pickParam;
        // A right-click on a cable, or on a plug-in parameter's socket.
        std::function<void(model::Id cable, ui::Point at)> cableMenu;
        // A module's pointer() changed knobs on its display: one undo step
        // per `gesture` (a press to its release, or a burst of the wheel).
        std::function<void(model::Id module, const std::vector<ModuleFace::Edit>& edits,
                           const std::string& gesture)>
            displayEdit;
        // A modulation cable's amount dragged on the ring around its knob, or
        // on a plug-in parameter's socket.
        std::function<void(model::Id cable, double amount, ui::Gesture g)> amount;
        std::function<void(const std::set<model::Id>&)> selectionChanged;
        std::function<void(const View&)> viewChanged;
        std::function<void()> deleteSelected;
        std::function<void()> spaceTap;  // Space pressed and released without a drag
        // Something dropped on the space from the browser (a type id).
        std::function<void(const std::string& type, ui::Point space)> dropType;
        // Add a module where the pointer is (window and space points): typed
        // into the space (`text`, the first letters), a double-click on it,
        // or a new cable let go on nothing (`from`: its end, an output when
        // `fromOutput`), which the module added then takes.
        std::function<void(ui::Point at, ui::Point space, const std::string& text,
                           std::optional<model::CableEnd> from, bool fromOutput)>
            quickAdd;
        // Ctrl+C, Ctrl+X, Ctrl+D: the selection; Ctrl+V at a space point.
        std::function<void()> copy, cut, duplicate;
        std::function<void(ui::Point space)> paste;
    };

    explicit RackSpace(Hooks hooks);

    // The rack to draw. Selection and gestures on modules that stay are kept.
    void setScene(RackScene scene);
    const RackScene& scene() const { return scene_; }
    RackScene& scene() { return scene_; }
    // New telemetry was read into the scene's modules: repaint.
    void telemetryChanged() { invalidate(); }

    const View& view() const { return view_; }
    void setView(View v);
    // Zoom `factor` around a window point (the pointer).
    void zoomAt(ui::Point at, double factor);
    // Everything in view, at most 100 % (an empty rack: the origin at 100 %).
    void frameAll();
    // `ids` filling the view, at most 150 % (F: the selection).
    void frame(const std::set<model::Id>& ids);
    // Where a paste goes: the pointer's space point when it is over the
    // space, else the middle of the view.
    ui::Point pastePoint() const;
    // `ids` in view: centred (zoomed out as far as they need) unless they are.
    void reveal(const std::set<model::Id>& ids);
    // frameAll() once the space has its size (before its first layout).
    void frameAllWhenLaidOut() {
        if (bounds().empty())
            frameOnLayout_ = true;
        else
            frameAll();
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& bounds) override;
    // The modules' space rectangle that is visible (for telemetry watching).
    ui::Rect visibleSpace() const;
    ui::Point toSpace(ui::Point window) const;
    ui::Point toWindow(ui::Point space) const;

    const std::set<model::Id>& selection() const { return selection_; }
    void select(std::set<model::Id> ids);
    // An empty-state line shown in the middle ("" for none: a blank space).
    void setEmptyText(std::string text);

    // A browser drag that is over the space (a type id), for the drop's ghost.
    void dragTypeOver(const std::string& name, std::optional<ui::Point> window);

    // --- for tests: a module's control or jack in window coordinates.
    std::optional<ui::Point> jackAt(model::Id module, const std::string& key, bool output) const;
    std::optional<ui::Point> controlAt(model::Id module, const std::string& key) const;
    std::optional<ui::Point> bodyAt(model::Id module) const;  // a point on its body
    // A plugin module's socket for its plug-in's parameter `key`.
    std::optional<ui::Point> socketAt(model::Id module, const std::string& key) const;
    // A point in view on a cable, clear of the modules, as near halfway as it can.
    std::optional<ui::Point> cableAt(model::Id cable) const;
    // The label a hovered socket or parameter cable shows ("" for none).
    std::string hoverText() const;
    std::string valueText() const override;

    void paint(ui::Canvas& c) override;
    bool onPointer(const ui::PointerEvent& e) override;
    bool onKey(const ui::KeyEvent& e) override;
    bool onKeyUp(const ui::KeyEvent& e) override;
    bool onText(std::string_view text) override;
    bool coalescesDrag() const override;

  private:
    struct Hit {
        enum class Kind { None, Body, Control, Jack, Ring, Display, Socket } kind = Kind::None;
        std::size_t module = 0;
        std::size_t item = 0;            // in the module's face; Socket: in its mods
        std::optional<model::Id> cable;  // Ring, Socket: the modulation cable
    };
    Hit hitAt(ui::Point space) const;
    bool words() const { return view_.zoom >= kWordsZoom; }
    void frameRect(const ui::Rect& r, double maxZoom);
    void paintNames(ui::Canvas& c) const;  // below kWordsZoom
    void nudge(double dx, double dy);
    std::optional<std::size_t> cableNear(ui::Point space) const;
    void paintModule(ui::Canvas& c, const RackScene::Module& m, bool selected, bool hot) const;
    void paintLabels(ui::Canvas& c, const RackScene::Module& m,
                     std::optional<std::size_t> hotItem) const;
    void paintItem(ui::Canvas& c, const RackScene::Module& m, const rack::PanelItem& item,
                   std::size_t index, bool hot) const;
    void paintCables(ui::Canvas& c) const;
    void paintSockets(ui::Canvas& c, const RackScene::Module& m) const;
    void paintHover(ui::Canvas& c) const;
    ui::Color cableColor(const ui::Theme& t, const RackScene::Cable& k) const;
    // The parameter cable a hover label is about, and where (space).
    std::optional<std::pair<const RackScene::Cable*, ui::Point>> hovered() const;
    double jackLevel(const RackScene::Module& m, const rack::PanelItem& item) const;
    bool connected(const RackScene::Module& m, const rack::PanelItem& item) const;
    model::CableEnd endOf(const RackScene::Module& m, const rack::PanelItem& item) const;
    void endDrag(bool commit);
    void hover(std::optional<model::Id> module);
    // The module's display copy (made on first use; null without draw() or
    // pointer()), and pointer() run on it with a window point.
    ModuleFace* faceOf(const RackScene::Module& m) const;
    void sendPointer(model::Id module, script::PointerAction action, ui::Point window,
                     ui::Point moved, double wheel, bool doubleClick, unsigned mods,
                     const std::string& gesture);
    std::string nextDisplayGesture(model::Id module);

    struct Drag {
        enum class Kind {
            None,
            Pan,
            Marquee,
            Move,
            Control,
            Cable,
            Amount,
            Display
        } kind = Kind::None;
        ui::Point start;  // window
        ui::Point last;   // window
        View view;        // at the start (Pan)
        bool moved = false;
        std::size_t module = 0, item = 0;
        double value = 0;  // a control's at the start (a position 0..1)
        unsigned mods = 0;
        // Move: each module's place at the start.
        std::vector<std::pair<std::size_t, ui::Point>> origins;
        std::set<model::Id> before;  // Marquee with Ctrl: the selection it adds to
        // Cable: the output it starts from, and the cable it moves (from an
        // input), with that cable's end left behind.
        model::CableEnd from;
        bool fromOutput = true;
        std::optional<model::Id> replace;
        std::optional<model::Id> cable;  // Amount
        double amount = 0;
        bool fraction = false;  // Amount: a plug-in parameter's (a fraction of its range)
        model::Id display;      // Display: the module pressed
        std::string gesture;    // Display: the press's undo step
    };

    Hooks hooks_;
    RackScene scene_;
    // Displays: each drawing module's copy, made when first painted.
    mutable std::map<model::Id, std::unique_ptr<ModuleFace>> faces_;
    View view_;
    std::set<model::Id> selection_;
    std::optional<model::Id> hot_;  // the module under the pointer
    std::optional<std::size_t> hotCable_;
    std::optional<ui::Point> pointer_;  // window
    std::string emptyText_;
    std::string dropName_;
    std::optional<ui::Point> dropAt_;
    Drag drag_;
    bool spaceHeld_ = false, spaceUsed_ = false, frameOnLayout_ = false;
    int gesture_ = 0;
    // pointer(): the display under the pointer (it hears Leave when it goes),
    // and the wheel's burst.
    std::optional<model::Id> displayHot_;
    ui::Point displayLast_;
    int displayGesture_ = 0;
    std::optional<model::Id> wheelModule_;
    std::chrono::steady_clock::time_point wheelAt_;
    std::string wheelGesture_;
};

}  // namespace supple::gui
