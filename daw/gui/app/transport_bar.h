// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The transport bar (gui.md §4.1): play/stop, record, loop, punch, metronome,
// count-in and follow buttons drawn as symbols (the same actions the keymap
// runs), the position in bars|beats and time, the session tempo and meter
// (click to edit them), and the engine's readouts.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "app_actions.h"
#include "supple/engine/app.h"
#include "supple/ui/node.h"
#include "supple/views/arrange.h"

namespace supple::ui {
class Button;
class Label;
class Knob;
class Root;
}  // namespace supple::ui

namespace supple::gui {

class TransportButton;

class TransportBar {
  public:
    TransportBar(engine::EngineApp& app, AppActions& actions, cmd::Dispatcher& dispatcher,
                 ui::Root& root);
    std::unique_ptr<ui::Node> build();
    // Reads transport each pass. Counters have a 4 Hz deadline only during
    // playback/recording; stopped readouts refresh on model/input notifications.
    void sync(bool event = true,
              std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;
    json::Value saveLayout() const;
    void loadLayout(const json::Value& value);
    // Where the playrate's menu opens (under its readout); the master's
    // envelope menu, which that menu leads to, opens there too.
    ui::Rect rateAnchor() const;
    // The arrange view's follow mode, which the follow button shows (it runs
    // ui.follow.cycle). Unset: there is no follow button.
    void setFollow(std::function<views::Follow()> follow) { follow_ = std::move(follow); }

  private:
    void openTempoForm();
    void openMeterForm();
    void openRateMenu();
    void openSyncMenu();  // the tempo button's right click (ADR-0071)

    engine::EngineApp& app_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    TransportButton* play_ = nullptr;
    TransportButton* record_ = nullptr;
    TransportButton* loop_ = nullptr;
    TransportButton* punch_ = nullptr;
    TransportButton* click_ = nullptr;  // the metronome
    TransportButton* count_ = nullptr;  // count-in bars before recording
    TransportButton* followButton_ = nullptr;
    std::function<views::Follow()> follow_;
    ui::Button* tempo_ = nullptr;
    ui::Button* meter_ = nullptr;
    ui::Label* position_ = nullptr;
    ui::Label* time_ = nullptr;
    ui::Label* engine_ = nullptr;
    ui::Knob* rate_ = nullptr;
    ui::Label* rateValue_ = nullptr;
    ui::Node* rateReadout_ = nullptr;
    std::uint64_t rateGesture_ = 0;
    bool moving_ = false;
    bool syncOn_ = false;  // external sync is on: the tempo button refreshes at 4 Hz
    std::optional<std::chrono::steady_clock::time_point> lastStatus_;
};

}  // namespace supple::gui
