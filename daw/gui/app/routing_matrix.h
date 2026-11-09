// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The routing matrix (docs/specs/gui.md §4.6): strips (in hierarchy) ×
// strips, plug-in keys and racks' In modules, from route.matrix. A strip with auxiliary channels
// (a multi-out instrument's outputs, ADR-0021) has a row per channel pair
// under its own, named by the outputs. A click selects a cell (Ctrl toggles,
// Shift extends; a row's or column's name takes all of it, the corner every
// cell); a double click sends from that pair. Every control below acts on the
// selection: Add and Remove, the tap, level and pan (a drag moves each send by
// the same amount), channels, polarity and mute. Cells the routing rules
// reject are drawn disabled and say why on hover.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app_actions.h"
#include "floating_panel.h"
#include "supple/ui/controls.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class Button;
}  // namespace supple::ui

namespace supple::gui {

struct MatrixModel {
    struct Source {
        std::string id, name, type, parent;
        int depth = 0;
        bool parentSend = true;
        std::string rule;  // why it cannot feed strips (masters)
        // The channels the row sends (ADR-0021): 1 is the strip's main pair;
        // 3, 5... its auxiliary pairs (width 1: a last odd channel), whose
        // rows follow the strip's with the outputs' names.
        int pair = 1, width = 2;
        std::string strip;  // the strip's name, for an auxiliary row
    };
    struct Destination {
        // kind: track | key | module (a rack's In); track: a key's strip, an
        // In module's home channel.
        std::string id, name, kind, type, track;
        std::string rule;  // why no strip can feed it (channels)
    };
    struct Send {
        std::string id, from, to, tap;
        double volumeDb = 0, pan = 0;
        bool mute = false, mono = false, phaseInvert = false;
        std::vector<int> mapFrom, mapTo;  // the channel map (empty: identity)
    };
    std::vector<Source> sources;
    std::vector<Destination> destinations;
    std::vector<Send> sends;
    std::vector<std::pair<std::string, std::string>> cyclePairs;  // (from, to)
    std::vector<std::string> cyclePaths;                          // by name: "Synth -> FX"
    void index();
    std::map<std::pair<std::string, std::string>, std::string> cycleIndex;
    std::map<std::pair<std::string, std::string>, std::vector<std::size_t>> sendIndex;
    // Why from -> to is refused (empty: allowed).
    std::string blocked(std::size_t row, std::size_t col) const;
    std::vector<const Send*> sendsAt(std::size_t row, std::size_t col) const;
    // The source pair a send leaves from: 1 (the main pair), 3, 5...
    static int sourcePair(const Send& x);
    static std::string pairText(int pair, int width);  // "3/4", "5"
};

class RoutingMatrix {
  public:
    RoutingMatrix(FloatingPanel& panel, AppActions& actions, cmd::Dispatcher& dispatcher,
                  engine::EngineApp& app);
    ~RoutingMatrix();
    void close() { panel_.close(); }
    void open(std::optional<ui::Rect> anchor = std::nullopt);
    bool isOpen() const { return grid_ != nullptr; }
    void sync();  // reload when the session changed while open

    // A cell, by what it joins (row and column numbers move as strips do).
    struct Cell {
        std::string source;
        int pair = 1;
        std::string destination;
        auto operator<=>(const Cell&) const = default;
    };
    // Selection, as the grid's clicks make it (mods: ui::kCtrl, ui::kShift).
    Status clickCell(std::size_t row, std::size_t col, unsigned mods = 0);
    void clickRow(std::size_t row, unsigned mods = 0);
    void clickColumn(std::size_t col, unsigned mods = 0);
    void selectAll();
    // A send there (the parent's cell: the parent send on or off).
    Status doubleClickCell(std::size_t row, std::size_t col);
    // The buttons: a send at each selected cell that has none (the parent's
    // cell: the parent send on), or none (off); one undo step each.
    Status addSelected();
    Status removeSelected();
    bool isSelected(std::size_t row, std::size_t col) const;
    std::size_t selectedCells() const { return selection_.size(); }
    // The sends at the selected cells, and the one the controls show (the
    // anchor's, else the first).
    std::vector<std::string> selectedSends() const;
    std::optional<std::string> selectedSend() const;

    // Tests.
    const MatrixModel& model() const { return model_; }
    ui::Node* grid() const { return grid_; }
    ui::Fader* volumeControl() const { return volume_; }
    ui::PanControl* panControl() const { return pan_; }
    static std::string channelsText(const MatrixModel::Send& x);
    std::string status() const;

  private:
    Status reload();
    void refresh();  // the controls, from the selection
    void select(std::set<Cell> cells, std::optional<Cell> anchor);
    Cell cellAt(std::size_t row, std::size_t col) const;
    std::optional<std::pair<std::size_t, std::size_t>> locate(const Cell& c) const;
    bool parentCell(std::size_t row, std::size_t col) const;
    const MatrixModel::Send* sendById(const std::string& id) const;
    Result<json::Value> exec(const std::string& command, json::Value args,
                             const cmd::Origin* origin = nullptr);
    // route.set on every selected send, one undo step; `field` from `each`.
    Status setSends(const std::string& label,
                    const std::function<std::optional<json::Value>(const MatrixModel::Send&)>& each,
                    const std::string& gesture = {});
    void level(double db, ui::Gesture g);
    // A right-click at `p` on a send control: its MIDI learn menu for the
    // selected sends (false: no control there, or no send selected).
    bool learnAt(ui::Point p);
    void panTo(double pan, ui::Gesture g);
    ui::Root& root() const { return panel_.root(); }
    FloatingPanel& panel_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    MatrixModel model_;
    std::uint64_t version_ = 0;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::set<Cell> selection_;
    std::optional<Cell> anchor_;
    std::optional<std::size_t> anchorRow_, anchorCol_;  // Shift on a row's / column's name
    // A drag of the level or pan: each send's value when it began.
    int gesture_ = 0;
    std::map<std::string, std::pair<double, double>> dragStart_;
    ui::Node* grid_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::Label* sendTitle_ = nullptr;
    ui::Node* details_ = nullptr;
    ui::Fader* volume_ = nullptr;
    ui::Label* volumeText_ = nullptr;
    ui::PanControl* pan_ = nullptr;
    ui::Label* panText_ = nullptr;
    ui::Button* add_ = nullptr;
    ui::Button* remove_ = nullptr;
    ui::Button* mute_ = nullptr;
    ui::Button* channels_ = nullptr;  // the channel map (a menu)
    ui::Button* phase_ = nullptr;
    std::vector<ui::Button*> taps_;
};

}  // namespace supple::gui
