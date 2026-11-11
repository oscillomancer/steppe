// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "preferences.h"
#include <supple/base/paths.h>

#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/popup.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

constexpr const char* kSchema = "supple.preferences/1.6";

Error threadLimitError() {
    return Error("preferences.audio_threads", "audio processing thread limit is out of range",
                 "/audio_threads", "use 0 for Automatic or an integer from 1 to 64");
}

std::string threadLabel(unsigned limit) {
    return limit == 0 ? "Automatic" : "Limit: " + std::to_string(limit);
}

const char* monitorName(model::Monitor m) {
    return m == model::Monitor::Off ? "off" : m == model::Monitor::On ? "on" : "auto";
}

std::string monitorLabel(model::Monitor m) {
    switch (m) {
        case model::Monitor::Off:
            return "Off";
        case model::Monitor::On:
            return "On (while armed)";
        case model::Monitor::Auto:
            return "Auto (armed: stopped or recording)";
    }
    return "On";
}

ui::Node& row(ui::Node& form, const std::string& id, const std::string& label) {
    auto& r = form.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(26);
    r.layout.gap = 8;
    r.layout.align = ui::Layout::Align::Center;
    r.emplace<ui::Label>(id + ".label", label, ui::Font::Ui, "small", "ink-quiet").layout.width =
        ui::SizeRule::fixed(200);
    return r;
}

}  // namespace

std::filesystem::path Preferences::defaultFile(std::string_view slug) {
    if (const auto root = paths::config(); !root.empty()) return root / slug / "preferences.json";
    return {};
}

Result<Preferences> Preferences::load(const std::filesystem::path& file) {
    Preferences out;
    if (file.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec) return out;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() ||
        (schema->asString() != kSchema && schema->asString() != "supple.preferences/1.0" &&
         schema->asString() != "supple.preferences/1.1" &&
         schema->asString() != "supple.preferences/1.2" &&
         schema->asString() != "supple.preferences/1.3" &&
         schema->asString() != "supple.preferences/1.4" &&
         schema->asString() != "supple.preferences/1.5"))
        return Error("preferences.schema", file.string() + " is not a " + kSchema + " document",
                     "/schema");
    if (const json::Value* m = doc->find("default_monitor")) {
        const std::string v = m->isString() ? m->asString() : std::string();
        if (v == "off")
            out.defaultMonitor = model::Monitor::Off;
        else if (v == "on")
            out.defaultMonitor = model::Monitor::On;
        else if (v == "auto")
            out.defaultMonitor = model::Monitor::Auto;
        else
            return Error("preferences.default_monitor", "default_monitor is off, on or auto",
                         "/default_monitor");
    }
    if (const auto* v = doc->find("audio_threads")) {
        if (!v->isInt() || v->asInt() < 0 || v->asInt() > 64) return threadLimitError();
        out.audioThreads = static_cast<unsigned>(v->asInt());
    }
    if (const auto* v = doc->find("lock_memory")) {
        if (!v->isBool())
            return Error("preferences.lock_memory", "lock_memory must be a boolean", "/lock_memory",
                         "use true or false");
        out.lockMemory = v->asBool();
    }
    if (const auto* v = doc->find("prevent_deep_idle")) {
        if (!v->isBool())
            return Error("preferences.prevent_deep_idle", "prevent_deep_idle must be a boolean",
                         "/prevent_deep_idle", "use true or false");
        out.preventDeepIdle = v->asBool();
    }
    if (const auto* v = doc->find("copy_imported_media")) {
        if (!v->isBool())
            return Error("preferences.copy_imported_media", "copy_imported_media must be a boolean",
                         "/copy_imported_media", "use true or false");
        out.copyImports = v->asBool();
    }
    if (const auto* v = doc->find("render_open_folder")) {
        if (!v->isBool())
            return Error("preferences.render_open_folder", "render_open_folder must be a boolean",
                         "/render_open_folder", "use true or false");
        out.renderOpenFolder = v->asBool();
    }
    if (const auto* v = doc->find("render_directory")) {
        if (!v->isString() || !std::filesystem::path(v->asString()).is_absolute())
            return Error("preferences.render_directory",
                         "render_directory must be an absolute path", "/render_directory",
                         "choose the folder again in the render dialog, or remove the field");
        out.renderDirectory = v->asString();
    }
    if (const auto* v = doc->find("audio")) {
        auto audio = parseAudioDevice(*v);
        if (!audio) return std::move(audio).error();
        out.audio = std::move(*audio);
    }
    for (bool output : {false, true}) {
        if (const auto* ports = doc->find(output ? "midi_outputs" : "midi_inputs")) {
            auto parsed = engine::parseMidiPortPreferences(*ports, output);
            if (!parsed) return parsed.error();
            (output ? out.midiOutputs : out.midiInputs) = std::move(*parsed);
        }
    }
    return out;
}

Status Preferences::save(const std::filesystem::path& file) const {
    if (audioThreads > 64) return threadLimitError();
    json::Value doc = json::Value::object();
    doc["schema"] = kSchema;
    doc["default_monitor"] = monitorName(defaultMonitor);
    doc["lock_memory"] = lockMemory;
    doc["prevent_deep_idle"] = preventDeepIdle;
    doc["audio_threads"] = static_cast<std::int64_t>(audioThreads);
    if (!renderDirectory.empty()) doc["render_directory"] = renderDirectory.string();
    doc["copy_imported_media"] = copyImports;
    doc["render_open_folder"] = renderOpenFolder;
    doc["audio"] = audioDeviceJson(audio);
    doc["midi_inputs"] = engine::midiPortPreferencesJson(midiInputs);
    doc["midi_outputs"] = engine::midiPortPreferencesJson(midiOutputs);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

PreferencesDialog::PreferencesDialog(ui::Root& root, engine::EngineApp& app, MouseDialog& mouse,
                                     Preferences prefs, std::filesystem::path file)
    : root_(root), app_(app), mouse_(mouse), prefs_(prefs), file_(std::move(file)) {
    (void)app_.setMidiPreferences(prefs_.midiInputs, prefs_.midiOutputs, false);
    app_.setDefaultMonitor(prefs_.defaultMonitor);
    // Loaded preferences are validated; callers constructing a struct directly
    // still cannot pass an invalid engine setting.
    if (!app_.setAudioThreadLimit(prefs_.audioThreads))
        prefs_.audioThreads = app_.audioThreadLimit();
    (void)app_.setMemoryLock(prefs_.lockMemory);
    (void)app_.setPreventDeepIdle(prefs_.preventDeepIdle);
    if (!prefs_.renderDirectory.empty()) app_.setRendersDir(prefs_.renderDirectory);
    app_.setCopyImportedMedia(prefs_.copyImports);
}

Status PreferencesDialog::save(Preferences& prefs) {
    prefs.midiInputs = app_.midiInputsPreferences();
    prefs.midiOutputs = app_.midiOutputsPreferences();
    return prefs.save(file_);
}

bool PreferencesDialog::isOpen() const { return root_.find("prefs") != nullptr; }

void PreferencesDialog::open() {
    if (root_.popupOpen()) root_.closePopups();
    auto form = std::make_unique<ui::Node>("prefs");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(640);
    form->layout.height = ui::SizeRule::content();
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    auto& header = form->emplace<ui::Node>("prefs.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(28);
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("prefs.title", "Preferences", ui::Font::UiBold, "body").layout.width =
        ui::SizeRule::fraction();
    header.emplace<ui::Button>("prefs.close", "Close", [this] { root_.closePopups(); });

    auto& monitor = row(*form, "prefs.monitor", "New channels' monitoring");
    monitor_ = &monitor.emplace<ui::Button>("prefs.monitor.set", "");
    monitor_->layout.width = ui::SizeRule::fraction();
    monitor_->layout.height = ui::SizeRule::fixed(26);
    monitor_->onClick = [this] {
        std::vector<ui::MenuItem> items;
        for (model::Monitor m : {model::Monitor::Off, model::Monitor::On, model::Monitor::Auto}) {
            ui::MenuItem item;
            item.label = monitorLabel(m);
            item.checked = prefs_.defaultMonitor == m;
            item.run = [this, m] { setDefaultMonitor(m); };
            items.push_back(std::move(item));
        }
        root_.openPopup(
            ui::makeMenu("prefs.monitor.menu", std::move(items), [this] { root_.closePopup(); }),
            monitor_->bounds());
    };

    auto& threads = row(*form, "prefs.threads", "Audio processing threads");
    threads_ = &threads.emplace<ui::Button>("prefs.threads.set", "");
    threads_->layout.width = ui::SizeRule::fraction();
    threads_->layout.height = ui::SizeRule::fixed(26);
    threads_->onClick = [this] {
        std::vector<ui::MenuItem> items;
        for (unsigned limit : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
            ui::MenuItem item;
            item.label = threadLabel(limit);
            item.checked = prefs_.audioThreads == limit;
            item.run = [this, limit] {
                if (auto result = setAudioThreads(limit); !result && onError)
                    onError("preferences: " + result.error().toString());
            };
            items.push_back(std::move(item));
        }
        root_.openPopup(
            ui::makeMenu("prefs.threads.menu", std::move(items), [this] { root_.closePopup(); }),
            threads_->bounds());
    };
    form->emplace<ui::Label>("prefs.threads.note",
                             "Live and background; applies on next engine start.", ui::Font::Ui,
                             "small", "ink-quiet")
        .layout.height = ui::SizeRule::fixed(20);

    auto& memory = row(*form, "prefs.memory", "Lock process memory (mlockall)");
    memory_ = &memory.emplace<ui::Button>("prefs.memory.set", "Off", [this] {
        if (auto result = setMemoryLock(!prefs_.lockMemory); !result && onError)
            onError("preferences: " + result.error().toString());
    });
    memory_->layout.width = ui::SizeRule::fraction();
    memory_->layout.height = ui::SizeRule::fixed(26);
    for (const auto& text :
         {"Uses RAM; large sample libraries or stack growth can fail.",
          "Host process only; turning it off also unlocks plug-in locks.",
          "To allow: add \"@audio - memlock unlimited\" to /etc/security/limits.d/, join audio."}) {
        auto& label = form->emplace<ui::Label>(
            "prefs.memory.caution." + std::to_string(form->children().size()), text, ui::Font::Ui,
            "small", "ink-quiet");
        label.layout.height = ui::SizeRule::fixed(20);
    }
    for (std::size_t i = 0; i < memoryNote_.size(); ++i) {
        auto& label = form->emplace<ui::Label>("prefs.memory.note." + std::to_string(i), "",
                                               ui::Font::Ui, "small", "ink-quiet");
        label.layout.width = ui::SizeRule::fraction();
        label.layout.height = ui::SizeRule::fixed(18);
        memoryNote_[i] = &label;
    }

    auto& latency = row(*form, "prefs.latency", "Keep CPUs out of deep idle");
    latency_ = &latency.emplace<ui::Button>("prefs.latency.set", "Off", [this] {
        if (auto result = setPreventDeepIdle(!prefs_.preventDeepIdle); !result && onError)
            onError("preferences: " + result.error().toString());
    });
    latency_->layout.width = ui::SizeRule::fraction();
    latency_->layout.height = ui::SizeRule::fixed(26);
    form->emplace<ui::Label>(
            "prefs.latency.caution",
            "While the engine runs, affects system idle; may increase power, heat and fan noise.",
            ui::Font::Ui, "small", "ink-quiet")
        .layout.height = ui::SizeRule::fixed(20);
    form->emplace<ui::Label>(
            "prefs.latency.permission",
            "To allow: a udev rule giving group audio rw on /dev/cpu_dma_latency; join audio.",
            ui::Font::Ui, "small", "ink-quiet")
        .layout.height = ui::SizeRule::fixed(20);
    for (std::size_t i = 0; i < latencyNote_.size(); ++i) {
        auto& label = form->emplace<ui::Label>("prefs.latency.note." + std::to_string(i), "",
                                               ui::Font::Ui, "small", "ink-quiet");
        label.layout.width = ui::SizeRule::fraction();
        label.layout.height = ui::SizeRule::fixed(18);
        latencyNote_[i] = &label;
    }

    auto& imports = row(*form, "prefs.imports", "Copy imported audio into project");
    imports_ = &imports.emplace<ui::Button>("prefs.imports.set", "On", [this] {
        if (auto result = setCopyImports(!prefs_.copyImports); !result && onError)
            onError("preferences: " + result.error().toString());
    });
    imports_->layout.width = ui::SizeRule::fraction();
    imports_->layout.height = ui::SizeRule::fixed(26);
    form->emplace<ui::Label>("prefs.imports.note",
                             "On: the project keeps working if the original file moves or goes.",
                             ui::Font::Ui, "small", "ink-quiet")
        .layout.height = ui::SizeRule::fixed(20);

    auto& audio = row(*form, "prefs.audio", "Audio device");
    audio_ = &audio.emplace<ui::Button>("prefs.audio.open", "", [this] {
        if (onAudioDevice) onAudioDevice();
    });
    audio_->layout.width = ui::SizeRule::fraction();
    audio_->layout.height = ui::SizeRule::fixed(26);

    auto& mouse = row(*form, "prefs.mouse", "Mouse modifiers");
    mouse
        .emplace<ui::Button>("prefs.mouse.open", "Edit mouse modifiers…", [this] { mouse_.open(); })
        .layout.height = ui::SizeRule::fixed(26);
    root_.openPopup(std::move(form), {}, ui::Placement::Center, [this] {
        monitor_ = nullptr;
        threads_ = nullptr;
        memory_ = nullptr;
        memoryNote_.fill(nullptr);
        latency_ = nullptr;
        latencyNote_.fill(nullptr);
        imports_ = nullptr;
        audio_ = nullptr;
    });
    refresh();
}

namespace {
void protectionNote(const std::array<ui::Label*, 4>& labels,
                    const engine::RuntimeProtectionStatus& state, const char* activeText) {
    if (!labels[0]) return;
    std::string text = state.error       ? state.error->message + "; " + state.error->hint
                       : state.active    ? activeText
                       : state.requested ? "Applies when the engine starts"
                                         : "";
    // Labels are single-line; split the short diagnostic into readable lines.
    for (auto* label : labels) {
        std::size_t end = text.size();
        if (end > 80) {
            end = text.rfind(' ', 80);
            if (end == std::string::npos || end == 0) end = 80;
        }
        const bool visible = !text.empty();
        if (label->visible != visible) {
            label->visible = visible;
            label->relayout();
        }
        label->setText(text.substr(0, end));
        text.erase(0, end);
        if (!text.empty() && text.front() == ' ') text.erase(0, 1);
    }
}
}  // namespace

void PreferencesDialog::refresh() {
    if (latency_) latency_->setText(prefs_.preventDeepIdle ? "On" : "Off");
    if (imports_) imports_->setText(prefs_.copyImports ? "On" : "Off");
    protectionNote(latencyNote_, app_.cpuIdleLatencyStatus(), "Requested: 20 µs");
    if (memory_) memory_->setText(prefs_.lockMemory ? "On" : "Off");
    protectionNote(memoryNote_, app_.memoryLockStatus(), "Locked");
    if (threads_) threads_->setText(threadLabel(prefs_.audioThreads));
    if (monitor_) monitor_->setText(monitorLabel(prefs_.defaultMonitor));
    if (audio_)
        audio_->setText((audioSummary ? audioSummary() : std::string("Audio device")) + "…");
}

void PreferencesDialog::setDefaultMonitor(model::Monitor m) {
    prefs_.defaultMonitor = m;
    app_.setDefaultMonitor(m);
    if (!file_.empty())
        if (auto saved = save(prefs_); !saved && onError)
            onError("preferences: " + saved.error().toString());
    refresh();
}

Status PreferencesDialog::setAudioThreads(unsigned limit) {
    if (limit > 64) return threadLimitError();
    auto next = prefs_;
    next.audioThreads = limit;
    // A failed save leaves both the displayed and effective preference intact.
    if (!file_.empty()) SUPPLE_TRY(save(next));
    SUPPLE_TRY(app_.setAudioThreadLimit(limit));
    prefs_ = next;
    refresh();
    return {};
}

Status PreferencesDialog::setMemoryLock(bool requested) {
    auto next = prefs_;
    next.lockMemory = requested;
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = next;
    auto applied = app_.setMemoryLock(requested);
    refresh();
    return applied;
}

Status PreferencesDialog::setPreventDeepIdle(bool requested) {
    auto next = prefs_;
    next.preventDeepIdle = requested;
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = next;
    auto applied = app_.setPreventDeepIdle(requested);
    refresh();
    return applied;
}

Status PreferencesDialog::setCopyImports(bool on) {
    auto next = prefs_;
    next.copyImports = on;
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = next;
    app_.setCopyImportedMedia(on);
    refresh();
    return {};
}

Status PreferencesDialog::setAudioDevice(const AudioDeviceSettings& audio) {
    auto next = prefs_;
    next.audio = audio;
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = next;
    refresh();
    return {};
}

Status PreferencesDialog::setRenderOpenFolder(bool on) {
    auto next = prefs_;
    next.renderOpenFolder = on;
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = next;
    return {};
}

Status PreferencesDialog::setRenderDirectory(const std::filesystem::path& dir) {
    if (!dir.is_absolute())
        return Error("preferences.render_directory", "render_directory must be an absolute path",
                     "/render_directory", "choose a folder in the render dialog");
    auto next = prefs_;
    next.renderDirectory = dir;
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = next;
    app_.setRendersDir(dir);
    return {};
}

}  // namespace supple::gui
