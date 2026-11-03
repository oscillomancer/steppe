// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The mixer (docs/specs/gui.md §4.4), docked under the arrange view: strips
// in track order grouped by type — channels, busses, masters — each with its
// type colour, its name (a double-click renames it), "fx" (the chain) and "+"
// (the plugin browser) at its top right, the plug-in chain (click: the
// plug-in's editor; drag: reorder it, or copy it onto another strip; a
// double-click on its empty part: the browser), sends, the input and "out"
// (click: the routing matrix; drag: a send), monitoring and arm on channels,
// pan, fader and meter, mute and solo; Console 9 as a project toggle in the
// header. The panel's top edge resizes it (only the chain section grows).
// Dragging a strip's empty part reorders it among its own kind. Every change
// is a command, and a control on a selected strip acts on the whole selection
// (SelectionEdit); a fader or pan drag, a reorder or a copy is one undo step.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app_actions.h"
#include "meter_feed.h"
#include "selection_edit.h"
#include "supple/ui/root.h"
#include "supple/views/track_layout.h"
#include "track_menu.h"

namespace supple::ui {
class Label;
class Fader;
class PanControl;
class Toggle;
class Button;
class Meter;
class MonitorButton;
}  // namespace supple::ui

namespace supple::gui {

class Mixer {
  public:
    Mixer(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
          MeterFeed& meters);
    ~Mixer();
    std::unique_ptr<ui::Node> build();  // the docked panel (hidden until shown)
    void toggle();
    void setVisible(bool visible);
    bool visible() const;
    void sync();  // every loop pass

    // The panel's height (UI state, persisted with the panels): clamped to what
    // the window can give it; the default fits about five plug-ins.
    float height() const { return height_; }
    void setHeight(float height);
    static float defaultHeight();
    json::Value saveLayout() const;  // the "mixer" section of the UI sidecar
    void loadLayout(const json::Value& v);
    // A double-click on the strips' empty space adds a channel, a right-click
    // there offers channel, buss or master: the app hands the shell's addStrip
    // (which selects it); unset, the mixer runs track.add.
    std::function<void(const std::string& type)> onAddStrip;
    // The channels' input button: what it says and its picker (the shell's,
    // shared with the TCP).
    std::function<std::string(const model::Id& track)> inputText;
    std::function<void(const model::Id& track, const ui::Rect& anchor)> pickInput;
    // The masters' output button (ADR-0056): what it says ("out 1/2") and the
    // hardware outputs menu (the shell's, shared with the TCP).
    std::function<std::string(const model::Id& track)> outputText;
    std::function<void(const model::Id& track, const ui::Rect& anchor)> pickOutput;
    std::function<void(const model::Id&, const ui::Rect&)> envelopes;

    // Tests.
    struct Strip {
        std::string track, type;
        ui::Node* node = nullptr;
        ui::Label* name = nullptr;
        ui::Fader* fader = nullptr;
        ui::PanControl* pan = nullptr;
        ui::Toggle *mute = nullptr, *solo = nullptr, *arm = nullptr;
        ui::Toggle *polarity = nullptr, *mono = nullptr;  // ADR-0022
        ui::MonitorButton* monitor = nullptr;
        ui::Label* value = nullptr;
        ui::Meter* meter = nullptr;
        ui::Button* fx = nullptr;     // "fx" at the strip's top right: the chain
        ui::Button* addFx = nullptr;  // "+" beside it: the plugin browser
        ui::Button* input = nullptr;  // channels: the record input
        ui::Button* out = nullptr;    // the output (click: matrix; drag: a send)
        ui::Node* rename = nullptr;   // the name's in-place editor (a double-click)
        ui::Node* chain = nullptr;    // the plug-in list
    };
    const std::vector<Strip>& strips() const { return strips_; }

    enum class DragPhase { Begin, Move, End, Cancel };
    // A drag in progress, for the feedback layer (and tests).
    struct DragView {
        enum class Kind { None, Strip, Fx, Send } kind = Kind::None;
        enum class Drop { None, Reorder, Copy } drop = Drop::None;
        bool active = false;  // past the movement threshold
        std::string track;    // the dragged strip, or the plug-in's own strip
        std::string fx;       // the dragged plug-in
        std::string label;    // chip text (plug-ins)
        ui::Point pos;
        ui::Rect source, marker;  // the dragged item; the insertion bar or target strip
        std::string target;       // Copy: the strip dropped on
        bool chain = false;       // Copy: the plug-in's whole chain (Ctrl held)
        std::size_t index = 0;    // Reorder: the track.move (masters) / fx.move index
        // Reorder of channels or busses: the track.move steps (the dragged strip
        // and the selected strips of its parent and type move together).
        std::vector<views::TrackMove> moves;
    };
    const DragView& drag() const { return *drag_; }

  private:
    void run(std::string_view action, json::Value args);  // an app action; errors to onError
    void addStrip(const std::string& type);
    float maxHeight() const;
    void applyHeight();
    void rebuild();
    void update();
    void dragStrip(const std::string& track, DragPhase phase, ui::Point p);
    void dragFx(const std::string& track, model::Id fx, DragPhase phase, ui::Point p,
                unsigned mods);
    void openFxMenu(model::Id fx, const ui::Rect& slot, ui::Point at);
    void openFxRename(model::Id fx, const ui::Rect& slot);
    void duplicateFx(model::Id fx);
    void removeFx(model::Id fx);
    void dragSend(const std::string& track, DragPhase phase, ui::Point p);
    void endDrag(bool commit);
    void resolveStripDrop();
    void resolveFxDrop();
    const Strip* stripAt(ui::Point p) const;
    std::string structure() const;
    Result<json::Value> set(const std::string& track, const char* field, json::Value value,
                            const std::string& gesture = {}, bool bypassGroups = false);
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    MeterFeed& meters_;
    SelectionEdit selectionEdit_;              // a strip control on a selected track: all of them
    TrackMenu trackMenu_;                      // a strip's right-click menu
    std::vector<std::string> shownSelection_;  // the track selection the strips show
    void showSelection();
    ui::Node* panel_ = nullptr;
    ui::Node* row_ = nullptr;  // the strips' container
    ui::Toggle* console9_ = nullptr;
    std::vector<Strip> strips_, vcaStrips_;
    float height_ = defaultHeight(), shown_ = 0;  // asked for; on screen
    std::shared_ptr<DragView> drag_ = std::make_shared<DragView>();
    ui::Root::OverlayHandle ghost_;
    std::string structure_;
    std::uint64_t version_ = ~0ull;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::uint64_t gesture_ = 0;  // a serial per drag: separate drags are separate undo steps
};

}  // namespace supple::gui
