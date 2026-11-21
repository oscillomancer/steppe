// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Event-list mode of the MIDI editor (gui.md §4.7). Rows are virtualized;
// cell edits and multi-row deletion each use one midi.edit command. Notes
// share the roll's selection; CC selection belongs to this panel.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "supple/ui/list.h"
#include "supple/ui/text_field.h"

namespace supple::engine {
class EngineApp;
}
namespace supple::ui {
class Root;
class Label;
}  // namespace supple::ui
namespace supple::gui {
class MidiEditor;

class MidiEventList {
  public:
    enum class Column { Position, Type, Length, Pitch, Value, Channel };
    using RowText = std::array<std::string, 6>;
    MidiEventList(ui::Root& root, MidiEditor& editor, engine::EngineApp& app);
    std::unique_ptr<ui::Node> build();
    void sync();
    void activate(bool shown);
    void cancel();
    RowText rowText(std::size_t row) const;
    void removeSelected();  // the selected rows, notes and controls, in one midi.edit
    ui::List* list() const { return list_; }

  private:
    struct Row {
        bool cc = false;
        std::size_t index = 0;  // pre-command note index, or control index
        double beat = 0, length = 0;
        int pitch = 60, value = 96, channel = 1;
    };
    void select(std::size_t row, unsigned mods);
    bool selected(std::size_t row) const;
    void begin(std::size_t row, Column column);
    void submit(const std::string& text);
    void add();
    void refreshSelection();
    void status(std::string text);
    ui::Root& root_;
    MidiEditor& editor_;
    engine::EngineApp& app_;
    ui::List* list_ = nullptr;
    ui::Node* columns_ = nullptr;  // the column header
    ui::TextField* field_ = nullptr;
    ui::Label* status_ = nullptr;
    std::vector<Row> rows_;
    std::set<std::size_t> selectedCc_;
    std::optional<std::size_t> anchor_;
    std::optional<std::pair<std::size_t, Column>> editing_;
    std::string item_;
    std::uint64_t version_ = 0;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    double tickBeat_ = 1.0 / 960;
};
}  // namespace supple::gui
