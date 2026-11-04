// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "learn_dialog.h"

#include <memory>
#include <set>
#include <utility>

#include "supple/engine/midi_control.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

constexpr model::ControlMode kModes[] = {
    model::ControlMode::Absolute, model::ControlMode::Relative1, model::ControlMode::Relative2,
    model::ControlMode::Relative3, model::ControlMode::Toggle};

class LearnPanel : public ui::Node {
  public:
    LearnPanel(ui::Root& root, AppActions& actions, engine::EngineApp& app, LearnRequest request)
        : Node("learn"), root_(root), actions_(actions), app_(app), request_(std::move(request)) {
        if (request_.current) {
            choice_ = *request_.current;
            heard_ = true;
            anyDevice_ = choice_.control.device.empty();
        } else {
            choice_.mode = request_.mode;
        }
        layout.dir = ui::Layout::Dir::Column;
        layout.width = ui::SizeRule::fixed(520);
        layout.height = ui::SizeRule::content();
        layout.padding = ui::Insets::all(14);
        layout.gap = 8;
        emplace<ui::Label>("learn.title", "MIDI learn: " + request_.title, ui::Font::UiBold,
                           "body");
        emplace<ui::Label>("learn.prompt", "Move the MIDI controller you wish to assign.",
                           ui::Font::Ui, "small");
        emplace<ui::Label>("learn.note",
                           app_.engine() ? "Every MIDI input the audio engine has open is heard."
                                         : "The audio engine is not running: start it to hear "
                                           "controllers.",
                           ui::Font::Ui, "caption", "ink-quiet");
        control_ = &row("learn.control", "Control")
                        .emplace<ui::Label>("learn.control.value", "", ui::Font::Ui, "small");
        control_->layout.width = ui::SizeRule::fraction();
        warning_ = &emplace<ui::TextBlock>("learn.warning", "", ui::Font::Ui, "small", "ink");
        warning_->visible = false;
        mode_ = &row("learn.mode", "CC mode").emplace<ui::Button>("learn.mode.set", "");
        mode_->layout.width = ui::SizeRule::fraction();
        mode_->onClick = [this] { openModes(); };
        device_ = &row("learn.device", "MIDI input").emplace<ui::Button>("learn.device.set", "");
        device_->layout.width = ui::SizeRule::fraction();
        device_->onClick = [this] {
            anyDevice_ = !anyDevice_;
            refresh();
        };
        if (request_.param) {
            option("learn.selected", "Enable only when its track is selected",
                   &LearnChoice::whenSelected);
            option("learn.focused", "Enable only when its FX window or editor has the focus",
                   &LearnChoice::whenFocused);
            option("learn.visible", "Enable only when its FX window or editor shows",
                   &LearnChoice::whenVisible);
            option("learn.soft", "Soft takeover (absolute mode only)", &LearnChoice::softTakeover);
        } else if (request_.takeover) {
            option("learn.soft", "Soft takeover (absolute mode only)", &LearnChoice::softTakeover);
        }
        auto& buttons = emplace<ui::Node>("learn.buttons");
        buttons.layout.dir = ui::Layout::Dir::Row;
        buttons.layout.height = ui::SizeRule::fixed(28);
        buttons.layout.gap = 8;
        ok_ = &buttons.emplace<ui::Button>("learn.ok", "OK", [this] { finish(true); });
        buttons.emplace<ui::Button>("learn.cancel", "Cancel", [this] { root_.closePopup(); });
        auto& remove = buttons.emplace<ui::Button>("learn.remove", "Remove", [this] {
            if (request_.current) finish(false);
        });
        remove.enabled = request_.current.has_value();
        owner_ = actions_.beginLearn(
            [this](const std::vector<engine::EngineApp::ControlEvent>& events) { heard(events); },
            [root = &root_, panel = this] {
                // Close this dialog and its menus. Capture the root separately:
                // removing the dialog may destroy it within this callback.
                while (root->find("learn") == panel && root->popupOpen()) root->closePopup();
            });
        refresh();
    }
    ~LearnPanel() override { actions_.endLearn(owner_); }
    void paint(ui::Canvas&) override {}

  private:
    ui::Node& row(const std::string& id, const std::string& label) {
        auto& r = emplace<ui::Node>(id);
        r.layout.dir = ui::Layout::Dir::Row;
        r.layout.height = ui::SizeRule::fixed(26);
        r.layout.gap = 8;
        r.layout.align = ui::Layout::Align::Center;
        r.emplace<ui::Label>(id + ".label", label, ui::Font::Ui, "small", "ink-quiet")
            .layout.width = ui::SizeRule::fixed(110);
        return r;
    }
    void option(const std::string& id, const std::string& label, bool LearnChoice::* field) {
        auto& r = emplace<ui::Node>(id);
        r.layout.dir = ui::Layout::Dir::Row;
        r.layout.height = ui::SizeRule::fixed(24);
        r.layout.gap = 8;
        r.layout.align = ui::Layout::Align::Center;
        auto* button = &r.emplace<ui::Button>(id + ".set", "");
        button->layout.width = ui::SizeRule::fixed(46);
        r.emplace<ui::Label>(id + ".label", label, ui::Font::Ui, "small").layout.width =
            ui::SizeRule::fraction();
        button->onClick = [this, field] {
            choice_.*field = !(choice_.*field);
            refresh();
        };
        options_.push_back({button, field});
    }

    // The control moved last is the one to bind.
    void heard(const std::vector<engine::EngineApp::ControlEvent>& events) {
        for (const auto& e : events)
            if (auto c = engine::controlOf(e.midi, e.device)) {
                if (!heard_ ||
                    !(c->kind == choice_.control.kind && c->channel == choice_.control.channel &&
                      c->number == choice_.control.number))
                    values_.clear();
                values_.insert((e.midi.bytes[1] & 0x7f) | ((e.midi.bytes[2] & 0x7f) << 7));
                choice_.control = *c;
                heardDevice_ = e.device;
                heard_ = true;
            }
        refresh();
        root_.damageAll();
    }

    void openModes() {
        std::vector<ui::MenuItem> items;
        for (model::ControlMode m : kModes) {
            ui::MenuItem item;
            item.label = std::string(engine::modeLabel(m));
            item.checked = choice_.mode == m;
            item.run = [this, m] {
                choice_.mode = m;
                refresh();
            };
            items.push_back(std::move(item));
        }
        root_.openPopup(
            ui::makeMenu("learn.mode.menu", std::move(items), [this] { root_.closePopup(); }),
            mode_->bounds());
    }

    void refresh() {
        const bool cc = choice_.control.kind == model::ControlKind::Cc;
        std::string text = heard_ ? engine::describe(choice_.control) : "(waiting for a control)";
        const std::string device = heardDevice_.empty() ? choice_.control.device : heardDevice_;
        if (heard_ && !device.empty()) text += " · " + device;
        control_->setText(text);
        mode_->setText(cc ? std::string(engine::modeLabel(choice_.mode))
                          : "Absolute (a CC chooses others)");
        mode_->enabled = cc;
        device_->setText(anyDevice_ || device.empty() ? "Any MIDI input" : "Only " + device);
        for (const auto& [button, field] : options_) button->setText(choice_.*field ? "On" : "Off");
        ok_->enabled = heard_;
        // A button sends one or two values; a knob or fader sends many.
        const bool knob = heard_ && values_.size() > 2 &&
                          ((cc && choice_.mode == model::ControlMode::Absolute) ||
                           choice_.control.kind == model::ControlKind::PitchBend);
        warning_->visible = request_.pressOnly && knob;
        if (warning_->visible) {
            std::string warning =
                "This action runs once per movement; the knob's position is not "
                "passed on.";
            if (!request_.alternative.empty())
                warning += " Bind \"" + request_.alternative + "\" to follow it.";
            warning_->setText(warning);
        }
    }

    void finish(bool ok) {
        LearnChoice choice = choice_;
        if (choice.control.kind != model::ControlKind::Cc)
            choice.mode = model::ControlMode::Absolute;
        const std::string device = heardDevice_.empty() ? choice_.control.device : heardDevice_;
        choice.control.device = anyDevice_ ? std::string() : device;
        // closePopup destroys this panel (and the request with it): copy first.
        auto onOk = request_.onOk;
        auto onRemove = request_.onRemove;
        root_.closePopup();
        if (ok && onOk) onOk(choice);
        if (!ok && onRemove) onRemove();
    }

    ui::Root& root_;
    AppActions& actions_;
    engine::EngineApp& app_;
    LearnRequest request_;
    std::uint64_t owner_ = 0;
    LearnChoice choice_;
    bool heard_ = false, anyDevice_ = true;
    std::string heardDevice_;
    std::set<int> values_;  // of the control heard last: more than two is a knob
    ui::Label* control_ = nullptr;
    ui::TextBlock* warning_ = nullptr;
    ui::Button *mode_ = nullptr, *device_ = nullptr, *ok_ = nullptr;
    std::vector<std::pair<ui::Button*, bool LearnChoice::*>> options_;
};

}  // namespace

void openLearnDialog(ui::Root& root, AppActions& actions, engine::EngineApp& app,
                     LearnRequest request) {
    root.openPopup(std::make_unique<LearnPanel>(root, actions, app, std::move(request)), {},
                   ui::Placement::Center);
}

}  // namespace supple::gui
