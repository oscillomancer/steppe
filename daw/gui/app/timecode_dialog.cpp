// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "timecode_dialog.h"

#include "supple/sync/timecode.h"
#include "supple/ui/forms.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::string message(const Error& e) {
    return e.hint.empty() ? e.message : e.message + "; " + e.hint;
}

ui::Node& row(ui::Node& form, const std::string& id, const std::string& label) {
    auto& r = form.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(26);
    r.layout.gap = 8;
    r.layout.align = ui::Layout::Align::Center;
    r.emplace<ui::Label>(id + ".label", label, ui::Font::Ui, "small", "ink-quiet").layout.width =
        ui::SizeRule::fixed(120);
    return r;
}

std::vector<ui::Choice> rateChoices() {
    using sync::FrameRate;
    std::vector<ui::Choice> out;
    for (FrameRate r : {FrameRate::Fps23976, FrameRate::Fps24, FrameRate::Fps25,
                        FrameRate::Fps2997Drop, FrameRate::Fps2997, FrameRate::Fps30}) {
        const std::string name(sync::rateName(r));
        const std::string label = r == FrameRate::Fps2997Drop ? "29.97 fps drop-frame"
                                  : r == FrameRate::Fps2997   ? "29.97 fps non-drop"
                                                              : name + " fps";
        out.push_back({name, label});
    }
    return out;
}

}  // namespace

TimecodeDialog::TimecodeDialog(ui::Root& root, cmd::Dispatcher& dispatcher, engine::EngineApp& app)
    : root_(root), dispatcher_(dispatcher), app_(app) {}

bool TimecodeDialog::isOpen() const { return root_.find("timecode") != nullptr; }

void TimecodeDialog::open() { build(); }

Status TimecodeDialog::setRate(const std::string& rate) {
    json::Value args = json::Value::object();
    args["timecode"] = json::Value::object();
    args["timecode"]["rate"] = rate;
    auto r = dispatcher_.execute({"project.set", args, kOrigin});
    if (!r) {
        show(message(r.error()));
        return std::move(r).error();
    }
    show({});
    return {};
}

Status TimecodeDialog::setStart(const std::string& start) {
    json::Value args = json::Value::object();
    args["timecode"] = json::Value::object();
    args["timecode"]["start"] = start;
    auto r = dispatcher_.execute({"project.set", args, kOrigin});
    if (!r) {
        show(message(r.error()));
        return std::move(r).error();
    }
    show({});
    return {};
}

void TimecodeDialog::show(const std::string& error) {
    if (!error.empty() && onError) onError("timecode: " + error);
    if (!isOpen()) return;
    if (auto* status = dynamic_cast<ui::Label*>(root_.find("timecode.status")))
        status->setText(error);
    shown_.clear();  // re-read the project
    refresh();
}

void TimecodeDialog::build() {
    if (root_.popupOpen()) root_.closePopups();
    auto form = std::make_unique<ui::Node>("timecode");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(440);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    auto& header = form->emplace<ui::Node>("timecode.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(28);
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("timecode.title", "Project timecode", ui::Font::UiBold, "body")
        .layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("timecode.close", "Close", [this] { root_.closePopups(); });

    auto& rate = row(*form, "timecode.rate", "Frame rate")
                     .emplace<ui::Dropdown>("timecode.rate.set", rateChoices());
    rate.layout.width = ui::SizeRule::fraction();
    rate.onChange = [this](const std::string& value) { (void)setRate(value); };
    auto& start = row(*form, "timecode.start", "Start")
                      .emplace<ui::TextField>("timecode.start.set", std::string(), "HH:MM:SS:FF");
    start.layout.width = ui::SizeRule::fraction();
    start.onSubmit = [this](const std::string& text) { (void)setStart(text); };
    start.onBlur = [this](const std::string& text) {
        const auto& tc = app_.head().project->timecode;
        if (text != sync::format(tc.start, tc.rate)) (void)setStart(text);
    };
    auto& note = form->emplace<ui::Label>(
        "timecode.note", "The timecode at project time 0. MTC and LTC sync and output use it.",
        ui::Font::Ui, "small", "ink-quiet");
    note.layout.height = ui::SizeRule::fixed(18);
    auto& status = form->emplace<ui::Label>("timecode.status", "", ui::Font::Ui, "small", "ink");
    status.layout.height = ui::SizeRule::fixed(18);
    shown_.clear();
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    refresh();
}

void TimecodeDialog::refresh() {
    if (!isOpen()) return;
    const auto& tc = app_.head().project->timecode;
    const std::string start = sync::format(tc.start, tc.rate);
    const std::string now = std::string(sync::rateName(tc.rate)) + " " + start;
    if (now == shown_) return;
    shown_ = now;
    if (auto* rate = dynamic_cast<ui::Dropdown*>(root_.find("timecode.rate.set")))
        rate->setValue(std::string(sync::rateName(tc.rate)));
    if (auto* field = dynamic_cast<ui::TextField*>(root_.find("timecode.start.set")))
        if (!field->focused()) field->setText(start);
}

}  // namespace supple::gui
