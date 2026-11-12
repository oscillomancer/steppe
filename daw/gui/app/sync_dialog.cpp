// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "sync_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "supple/json/write.h"
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
        ui::SizeRule::fixed(150);
    return r;
}

ui::Label& line(ui::Node& form, const std::string& id) {
    auto& label = form.emplace<ui::Label>(id, "", ui::Font::Ui, "small", "ink-quiet");
    label.layout.width = ui::SizeRule::fraction();
    label.layout.height = ui::SizeRule::fixed(18);
    return label;
}

// `missing`: how a channel the device lacks reads.
std::vector<ui::Choice> channels(std::uint32_t count, int current, const std::string& what,
                                 const std::string& missing = " (not on this device)") {
    std::vector<ui::Choice> out;
    const int last = std::max<int>(static_cast<int>(count), current);
    for (int c = 1; c <= std::max(last, 1); ++c)
        out.push_back({std::to_string(c), what + " " + std::to_string(c) +
                                              (c > static_cast<int>(count) ? missing : "")});
    return out;
}

std::string lossValue(const engine::SyncSettings& s) {
    return s.onLoss == "stop" ? "stop" : std::to_string(s.freewheelFrames);
}

}  // namespace

SyncDialog::SyncDialog(ui::Root& root, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
                       Save save)
    : root_(root),
      dispatcher_(dispatcher),
      app_(app),
      save_(std::move(save)),
      saved_(app.syncSettings()) {}

bool SyncDialog::isOpen() const { return root_.find("sync") != nullptr; }

void SyncDialog::open() { build(); }

Status SyncDialog::set(const json::Value& members) {
    const auto before = app_.syncSettings();
    auto r = dispatcher_.execute({"sync.set", members, kOrigin});
    if (!r) {
        if (onError) onError("sync: " + message(r.error()));
        if (auto* status = dynamic_cast<ui::Label*>(root_.find("sync.status.0")))
            status->setText(message(r.error()));
        return std::move(r).error();
    }
    watch();
    // Rows depend on the source; rebuild only when it changed.
    if (isOpen() && before.source != app_.syncSettings().source) build();
    refresh();
    return {};
}

void SyncDialog::watch() {
    if (app_.syncSettings() == saved_) return;
    if (save_)
        if (auto saved = save_(app_.syncSettings()); !saved && onError)
            onError("sync: " + message(saved.error()));
    saved_ = app_.syncSettings();
}

void SyncDialog::build() {
    if (root_.popupOpen()) root_.closePopups();
    const engine::SyncSettings& s = app_.syncSettings();
    auto form = std::make_unique<ui::Node>("sync");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(560);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    auto& header = form->emplace<ui::Node>("sync.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(28);
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("sync.title", "Sync settings", ui::Font::UiBold, "body")
        .layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("sync.close", "Close", [this] { root_.closePopups(); });

    auto dropdown = [&](const std::string& id, const std::string& label,
                        std::vector<ui::Choice> choices, const std::string& value,
                        std::function<void(const std::string&)> change) {
        auto& d =
            row(*form, id, label).emplace<ui::Dropdown>(id + ".set", std::move(choices), value);
        d.layout.width = ui::SizeRule::fraction();
        d.onChange = std::move(change);
    };
    auto member = [this](const char* key, json::Value value) {
        json::Value args = json::Value::object();
        args[key] = std::move(value);
        (void)set(args);
    };

    dropdown("sync.source", "Follow",
             {{"off", "Off (Steppe is the master)"},
              {"midi_clock", "MIDI clock"},
              {"mtc", "MIDI Time Code (MTC)"},
              {"ltc", "SMPTE LTC (audio)"}},
             s.source, [member](const std::string& v) { member("source", v); });
    const engine::Engine* engine = app_.engine();
    if (s.source == "midi_clock" || s.source == "mtc") {
        std::vector<ui::Choice> ports;
        const auto names = engine ? engine->midiPorts() : std::vector<std::string>{};
        for (const auto& name : names) ports.push_back({name, name});
        if (std::find(names.begin(), names.end(), s.midiInput) == names.end())
            ports.insert(ports.begin(),
                         {s.midiInput, s.midiInput.empty() ? std::string("Choose a MIDI input")
                                                           : s.midiInput + " (not connected)"});
        dropdown("sync.midi", "MIDI input", std::move(ports), s.midiInput,
                 [member](const std::string& v) { member("midi_input", v); });
    }
    if (s.source == "ltc")
        dropdown("sync.ltc_input", "LTC input",
                 channels(engine ? engine->config().inputs : 0, s.ltcInput, "Input"),
                 std::to_string(s.ltcInput),
                 [member](const std::string& v) { member("ltc_input", std::atoi(v.c_str())); });
    if (s.source == "midi_clock") {
        auto& box = row(*form, "sync.follow", "Tempo")
                        .emplace<ui::Checkbox>("sync.follow.set",
                                               "Follow the master's tempo (pitch preserved)",
                                               s.followTempo);
        box.onToggle = [member](bool on) { member("follow_tempo", on); };
    }
    if (s.source != "off") {
        char offset[32];
        std::snprintf(offset, sizeof offset, "%g", s.offsetMs);
        auto& field = row(*form, "sync.offset", "Offset (ms)")
                          .emplace<ui::TextField>("sync.offset.set", offset, "0");
        field.layout.width = ui::SizeRule::fraction();
        auto commit = [this, member](const std::string& text) {
            char* end = nullptr;
            const double ms = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0') {
                if (onError) onError("sync: the offset is a number of milliseconds");
                return;
            }
            if (ms != app_.syncSettings().offsetMs) member("offset_ms", ms);
        };
        field.onSubmit = commit;
        field.onBlur = commit;
        dropdown("sync.loss", "When the signal stops",
                 {{"stop", "Stop"},
                  {"10", "Keep playing for 10 frames"},
                  {"25", "Keep playing for 25 frames"},
                  {"50", "Keep playing for 50 frames"},
                  {"100", "Keep playing for 100 frames"}},
                 lossValue(s), [this](const std::string& v) {
                     json::Value args = json::Value::object();
                     args["on_loss"] = v == "stop" ? "stop" : "freewheel";
                     if (v != "stop") args["freewheel_frames"] = std::atoi(v.c_str());
                     (void)set(args);
                 });
    }
    auto& ltc = row(*form, "sync.ltc_out", "LTC output");
    auto& on = ltc.emplace<ui::Checkbox>("sync.ltc_out.set", "Send LTC", s.ltcOutput);
    on.onToggle = [member](bool v) { member("ltc_output", v); };
    // A channel the device lacks: LTC goes to its last output meanwhile.
    const std::uint32_t outputs = engine ? engine->config().outputs : 0;
    auto& channel = ltc.emplace<ui::Dropdown>(
        "sync.ltc_out.channel",
        channels(outputs, s.ltcOutputChannel, "Output",
                 outputs > 0
                     ? " (not on this device: LTC uses output " + std::to_string(outputs) + ")"
                     : std::string(" (not on this device)")),
        std::to_string(s.ltcOutputChannel));
    channel.layout.width = ui::SizeRule::fraction();
    channel.onChange = [member](const std::string& v) {
        member("ltc_output_channel", std::atoi(v.c_str()));
    };
    std::vector<ui::Choice> levels;
    for (int db : {-24, -18, -12, -6})
        levels.push_back({std::to_string(db), std::to_string(db) + " dBFS"});
    auto& level = ltc.emplace<ui::Dropdown>("sync.ltc_out.level", std::move(levels),
                                            std::to_string(static_cast<int>(s.ltcLevelDb)));
    level.onChange = [member](const std::string& v) {
        member("ltc_level_db", std::atof(v.c_str()));
    };

    auto& tc = row(*form, "sync.timecode", "Project timecode");
    tc.emplace<ui::Label>("sync.timecode.value", "", ui::Font::Mono, "small").layout.width =
        ui::SizeRule::fraction();
    tc.emplace<ui::Button>("sync.timecode.edit", "Change…", [this] {
        if (onTimecode) onTimecode();
    });
    for (int i = 0; i < 3; ++i) line(*form, "sync.status." + std::to_string(i));
    shown_.clear();
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
    refresh();
}

void SyncDialog::refresh() {
    watch();
    if (!isOpen()) return;
    // An agent's change while the dialog is open: rebuild the rows.
    const std::string now = json::writeCompact(engine::syncSettingsJson(app_.syncSettings()));
    if (!shown_.empty() && now != shown_) {
        shown_.clear();
        build();
        return;
    }
    shown_ = now;
    const auto& tc = app_.head().project->timecode;
    if (auto* value = dynamic_cast<ui::Label*>(root_.find("sync.timecode.value")))
        value->setText(std::string(sync::rateName(tc.rate)) + " fps from " +
                       sync::format(tc.start, tc.rate));
    const engine::SyncReport r = app_.syncReport();
    std::vector<std::string> lines;
    char text[160];
    if (r.source == "off") {
        lines.push_back("Steppe runs on its own clock.");
    } else if (!app_.engine()) {
        lines.push_back("Start the audio engine to follow a master.");
    } else if (!r.present) {
        lines.push_back("Waiting for " +
                        std::string(r.source == "midi_clock" ? "MIDI clock"
                                    : r.source == "mtc"      ? "MTC"
                                                             : "LTC") +
                        (r.input.empty() ? std::string() : " on " + r.input) + ".");
    } else {
        if (r.source == "midi_clock")
            std::snprintf(text, sizeof text, "%s: %.1f bpm, %s; chase rate %.4f, error %.1f ms.",
                          r.running ? "Running" : "Stopped", r.bpm,
                          r.locked ? "locked" : "not locked", r.chaseRate, r.errorMs);
        else
            std::snprintf(text, sizeof text, "%s at %s fps, %s; chase rate %.4f, error %.1f ms.",
                          r.timecode.c_str(), r.rate.c_str(), r.locked ? "locked" : "not locked",
                          r.chaseRate, r.errorMs);
        lines.push_back(text);
    }
    if (r.ltcOutput) lines.push_back("LTC output is on.");
    for (const auto& w : r.warnings) lines.push_back(w);
    for (int i = 0; i < 3; ++i)
        if (auto* label = dynamic_cast<ui::Label*>(root_.find("sync.status." + std::to_string(i))))
            label->setText(static_cast<std::size_t>(i) < lines.size() ? lines[i] : std::string());
}

}  // namespace supple::gui
