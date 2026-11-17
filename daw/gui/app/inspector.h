// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The inspector (docs/specs/gui.md §4.14): the side panel's tab that says
// what is selected. It reads the selection and the session each pass and
// executes nothing by itself.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_actions.h"
#include "supple/base/result.h"
#include "supple/engine/app.h"
#include "supple/model/session.h"
#include "supple/time/timeline.h"
#include "supple/ui/canvas.h"
#include "supple/ui/node.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Dropdown;
class Label;
class List;
class TextBlock;
class TextField;
class Toggle;
}  // namespace supple::ui

namespace supple::gui {

// What the inspector's header says about a selection: a title, a line under
// it, and the colour the thing is drawn in elsewhere (an item's or a track's;
// none for several things or nothing).
struct InspectorSubject {
    enum class Kind { Nothing, Item, Items, Track, Tracks } kind = Kind::Nothing;
    std::string title, detail;
    std::optional<ui::Color> color;
    std::optional<model::Id> item;  // the one item (Kind::Item)
};
// Tracks win when they were selected last (UiState::tracksFocused), else
// items; ids the session no longer holds are left out.
InspectorSubject inspectorSubject(const model::Session& session, const UiState& selection,
                                  const ui::Theme& theme);

// A position as the inspector writes it, "3.1.000": bar, beat of the bar's
// meter and thousandths of that beat, each from 1 (as the transport shows
// it). And back: "3", "3.2" or "3.2.500"; an error says what to write.
std::string inspectorBarsText(double beat, const time::TimeMap& map);
Result<double> inspectorParseBars(std::string_view text, const time::TimeMap& map);
// A number with a unit a person may or may not type: "-3", "-3 dB", "0.4s".
Result<double> inspectorParseNumber(std::string_view text, std::string_view unit);

class Inspector {
  public:
    Inspector(ui::Root& root, engine::EngineApp& app, const UiState& selection,
              cmd::Dispatcher& dispatcher, AppActions& actions);
    std::unique_ptr<ui::Node> build();
    void setVisible(bool visible);  // the side panel tells it; it reads nothing while hidden
    void sync();                    // every loop pass

  private:
    // A text field and the text it last took from the session.
    struct Field {
        ui::TextField* node = nullptr;
        std::string shown;
    };
    ui::TextField& field(ui::Node& parent, const std::string& key, const std::string& label);
    ui::Toggle& state(ui::Node& parent, const std::string& key, const std::string& label,
                      const std::string& color);
    void show(const std::string& key, std::string text, bool force);
    void commit(const std::string& key, const std::string& text);  // a field's text, as typed
    void set(const std::string& key, json::Value value);           // one item.set
    void setTake(const std::string& key, json::Value value);       // one take.set, the heard take
    void run(const char* command, json::Value args);
    void fail(const std::string& message);
    void readItem(bool force);  // the fields from the session; a focused field keeps its text
    ui::Root& root_;
    engine::EngineApp& app_;
    const UiState& selection_;
    cmd::Dispatcher& dispatcher_;
    AppActions& actions_;
    ui::Node* itemBox_ = nullptr;  // the one-item fields; hidden otherwise
    std::map<std::string, Field> fields_;
    std::map<std::string, ui::Toggle*> states_;
    ui::Dropdown* timebase_ = nullptr;
    ui::Node* takeBox_ = nullptr;  // speed and pitch: an audio take's
    ui::Toggle* keepPitch_ = nullptr;
    ui::Node* takesBox_ = nullptr;  // the take list: an item with several
    ui::List* takes_ = nullptr;
    std::vector<model::Id> takeIds_;
    std::vector<std::string> takeNames_;
    std::size_t heard_ = 0;  // the heard take's row
    ui::TextBlock* locked_ = nullptr;
    ui::TextBlock* file_ = nullptr;
    std::optional<model::Id> item_;  // the item the fields show
    ui::Node* panel_ = nullptr;
    ui::Node* swatch_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::TextBlock* detail_ = nullptr;
    bool visible_ = false;
    // What the header was last built from.
    std::uint64_t version_ = ~std::uint64_t{0}, generation_ = ~std::uint64_t{0};
    std::vector<std::string> tracks_, items_;
    bool tracksFocused_ = false;
};

}  // namespace supple::gui
