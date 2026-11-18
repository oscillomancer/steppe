// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "preferences.h"
#include <supple/base/paths.h>

#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/popup.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

constexpr const char* kSchema = engine::kPreferencesSchema;

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

std::string isolationLabel(model::Isolation i) {
    switch (i) {
        case model::Isolation::SharedBridge:
            return "In a shared bridge process";
        case model::Isolation::Dedicated:
            return "Each in its own process";
        case model::Isolation::InProcess:
        case model::Isolation::Inherit:
            break;
    }
    return "In the app";
}

const char* isolationName(model::Isolation i) {
    switch (i) {
        case model::Isolation::SharedBridge:
            return "shared_bridge";
        case model::Isolation::Dedicated:
            return "dedicated";
        case model::Isolation::InProcess:
        case model::Isolation::Inherit:
            break;
    }
    return "in_process";
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

const char* rememberedChoice(const std::string& mode) {
    return mode == "bridge" ? "In a separate process" : mode == "disable" ? "Disabled" : "Normally";
}

// A hairline under a section's heading.
class Hairline : public ui::Node {
  public:
    explicit Hairline(std::string id) : Node(std::move(id)) {
        layout.height = ui::SizeRule::fixed(1);
    }
    void paint(ui::Canvas& c) override {
        const ui::Rect b = bounds();
        c.fillRect({b.x, b.y, b.w, 1}, c.theme().color("rule"));
    }
};

// The list of remembered choices is as tall as its rows, up to six; more scroll.
class RememberedScroll : public ui::ScrollView {
  public:
    using ScrollView::ScrollView;
    float maxHeight = 6 * 24 + 5 * 4, contentWidth = 600;
    ui::Size measure(const ui::Canvas& c, ui::Size available) override {
        const auto size =
            content().measure(c, {contentWidth - ui::ScrollBar::kThickness, available.h});
        return {available.w, std::min(size.h, maxHeight)};
    }
};

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
         schema->asString() != "supple.preferences/1.5" &&
         schema->asString() != "supple.preferences/1.6" &&
         schema->asString() != "supple.preferences/1.7" &&
         schema->asString() != "supple.preferences/1.8" &&
         schema->asString() != "supple.preferences/1.9"))
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
    if (const auto* v = doc->find("plugin_isolation")) {  // 1.9, ADR-0081
        const std::string name = v->isString() ? v->asString() : std::string();
        if (name == "in_process")
            out.pluginIsolation = model::Isolation::InProcess;
        else if (name == "shared_bridge")
            out.pluginIsolation = model::Isolation::SharedBridge;
        else if (name == "dedicated")
            out.pluginIsolation = model::Isolation::Dedicated;
        else
            return Error("preferences.plugin_isolation",
                         "plugin_isolation must be in_process, shared_bridge or dedicated",
                         "/plugin_isolation", "use one of in_process, shared_bridge, dedicated");
    }
    if (const auto* v = doc->find("plugin_overrides")) {  // 1.10, topic 7d
        if (!v->isArray())
            return Error("preferences.plugin_overrides", "plugin_overrides must be an array",
                         "/plugin_overrides", "remove the field to forget every remembered choice");
        std::size_t index = 0;
        for (const auto& item : v->asArray()) {
            const std::string at = "/plugin_overrides/" + std::to_string(index++);
            auto text = [&](const char* key) {
                const auto* x = item.isObject() ? item.find(key) : nullptr;
                return x && x->isString() ? x->asString() : std::string();
            };
            RememberedLoad load;
            load.format = text("format");
            load.id = text("id");
            load.buildId = text("build_id");
            load.mode = text("mode");
            load.name = text("name");
            load.vendor = text("vendor");
            load.version = text("version");
            if (const auto* ms = item.isObject() ? item.find("remembered_ms") : nullptr;
                ms && ms->isInt())
                load.rememberedMs = ms->asInt();
            if (load.format.empty() || load.id.empty() || load.buildId.empty() ||
                (load.mode != "normal" && load.mode != "bridge" && load.mode != "disable"))
                return Error("preferences.plugin_overrides",
                             "a remembered choice needs a format, an id, a build_id and a mode "
                             "of normal, bridge or disable",
                             at, "remove the entry, or forget it in Preferences");
            out.rememberedLoads.push_back(std::move(load));
        }
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
    if (const auto* v = doc->find("sync")) {  // 1.7, ADR-0071
        auto sync = engine::parseSyncSettings(*v);
        if (!sync) {
            Error e = std::move(sync).error();
            e.code = "preferences.sync";
            e.path = "/sync" + e.path;
            return e;
        }
        out.sync = std::move(*sync);
    }
    // MIDI port records (1.8 adds the clock members, ADR-0073).
    for (bool output : {false, true}) {
        if (const auto* ports = doc->find(output ? "midi_outputs" : "midi_inputs")) {
            std::vector<engine::MidiPortPreferenceWarning> warnings;
            // EngineApp loads the same file and surfaces these startup warnings.
            (output ? out.midiOutputs : out.midiInputs) =
                engine::loadMidiPortPreferences(*ports, output, warnings);
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
    doc["plugin_isolation"] = isolationName(pluginIsolation);
    doc["render_open_folder"] = renderOpenFolder;
    if (!rememberedLoads.empty()) {
        json::Value loads = json::Value::array();
        for (const auto& load : rememberedLoads) {
            json::Value item = json::Value::object();
            item["format"] = load.format;
            item["id"] = load.id;
            item["build_id"] = load.buildId;
            item["mode"] = load.mode;
            if (!load.name.empty()) item["name"] = load.name;
            if (!load.vendor.empty()) item["vendor"] = load.vendor;
            if (!load.version.empty()) item["version"] = load.version;
            item["remembered_ms"] = load.rememberedMs;
            loads.push_back(std::move(item));
        }
        doc["plugin_overrides"] = std::move(loads);
    }
    doc["audio"] = audioDeviceJson(audio);
    doc["sync"] = engine::syncSettingsJson(sync);
    // These records belong to the engine; preserve the current disk value.
    std::error_code lookupError;
    if (std::filesystem::exists(file, lookupError)) {
        auto bytes = readFile(file);
        if (!bytes) return bytes.error();
        auto stored = json::parse(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        if (!stored) return stored.error();
        for (const auto* key : {"midi_inputs", "midi_outputs"})
            if (const auto* value = stored->find(key)) doc[key] = *value;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

PreferencesDialog::PreferencesDialog(ui::Root& root, engine::EngineApp& app, MouseDialog& mouse,
                                     Preferences prefs, std::filesystem::path file)
    : root_(root), app_(app), mouse_(mouse), prefs_(prefs), file_(std::move(file)) {
    app_.setDefaultMonitor(prefs_.defaultMonitor);
    // Loaded preferences are validated; callers constructing a struct directly
    // still cannot pass an invalid engine setting.
    if (!app_.setAudioThreadLimit(prefs_.audioThreads))
        prefs_.audioThreads = app_.audioThreadLimit();
    (void)app_.setMemoryLock(prefs_.lockMemory);
    (void)app_.setPreventDeepIdle(prefs_.preventDeepIdle);
    if (!prefs_.renderDirectory.empty()) app_.setRendersDir(prefs_.renderDirectory);
    app_.setCopyImportedMedia(prefs_.copyImports);
    if (!app_.setPluginIsolation(prefs_.pluginIsolation))
        prefs_.pluginIsolation = app_.pluginIsolation();
}

Status PreferencesDialog::setRememberedLoads(std::vector<RememberedLoad> loads) {
    auto next = prefs_;
    next.rememberedLoads = std::move(loads);
    if (!file_.empty()) SUPPLE_TRY(save(next));
    prefs_ = std::move(next);
    refresh();
    return {};
}

Status PreferencesDialog::save(Preferences& prefs) {
    SUPPLE_TRY(prefs.save(file_));
    return app_.writeMidiPreferences(file_);
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

    auto& isolation = row(*form, "prefs.isolation", "Third-party plug-ins run");
    isolation_ = &isolation.emplace<ui::Button>("prefs.isolation.set", "");
    isolation_->layout.width = ui::SizeRule::fraction();
    isolation_->layout.height = ui::SizeRule::fixed(26);
    isolation_->onClick = [this] {
        std::vector<ui::MenuItem> items;
        for (model::Isolation i : {model::Isolation::InProcess, model::Isolation::SharedBridge,
                                   model::Isolation::Dedicated}) {
            ui::MenuItem item;
            item.label = isolationLabel(i);
            item.checked = prefs_.pluginIsolation == i;
            item.run = [this, i] {
                if (auto result = setPluginIsolation(i); !result && onError)
                    onError("preferences: " + result.error().toString());
            };
            items.push_back(std::move(item));
        }
        root_.openPopup(
            ui::makeMenu("prefs.isolation.menu", std::move(items), [this] { root_.closePopup(); }),
            isolation_->bounds());
    };
    form->emplace<ui::Label>("prefs.isolation.note",
                             "Applies as plug-ins load. Stock plug-ins stay in the app; a "
                             "plug-in's own choice wins.",
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
    remembered_ = &form->emplace<ui::Node>("prefs.remembered");
    remembered_->layout.dir = ui::Layout::Dir::Column;
    remembered_->layout.height = ui::SizeRule::content();
    remembered_->layout.gap = 6;
    rememberedShown_ = "\x01";  // not built yet: an empty list still gets its sentence
    root_.openPopup(std::move(form), {}, ui::Placement::Center, [this] {
        remembered_ = nullptr;
        monitor_ = nullptr;
        threads_ = nullptr;
        memory_ = nullptr;
        memoryNote_.fill(nullptr);
        latency_ = nullptr;
        latencyNote_.fill(nullptr);
        imports_ = nullptr;
        isolation_ = nullptr;
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

// A section at the end of the form: its heading in bold over a rule, the count quiet after it and
// Forget all at the right; under it one row to a choice on the form's grid: the plug-in (bold) with
// its maker and version in quiet ink, what was chosen, and a quiet Forget at the right edge.
void PreferencesDialog::rebuildRemembered() {
    if (!remembered_) return;
    std::string signature;
    for (const auto& r : prefs_.rememberedLoads)
        signature += r.format + "|" + r.id + "|" + r.buildId + "|" + r.mode + "|" + r.name + "|" +
                     r.vendor + "|" + r.version + "\n";
    if (signature == rememberedShown_) return;
    rememberedShown_ = signature;
    const auto& loads = prefs_.rememberedLoads;
    root_.releaseWithin(remembered_);
    remembered_->clear();
    auto& head = remembered_->emplace<ui::Node>("prefs.remembered.head");
    head.layout.dir = ui::Layout::Dir::Row;
    head.layout.height = ui::SizeRule::fixed(24);
    head.layout.gap = 8;
    head.layout.align = ui::Layout::Align::Center;
    head.emplace<ui::Label>("prefs.remembered.title", "Plug-in choices", ui::Font::UiBold, "small",
                            "ink");
    head.emplace<ui::Label>("prefs.remembered.count",
                            loads.empty() ? "none"
                                          : std::to_string(loads.size()) +
                                                (loads.size() == 1 ? " choice" : " choices"),
                            ui::Font::Ui, "caption", "ink-quiet")
        .layout.width = ui::SizeRule::fraction();
    if (!loads.empty()) {
        auto& all = head.emplace<ui::Button>("prefs.remembered.forget_all", "Forget all", [this] {
            if (auto saved = setRememberedLoads({}); !saved && onError)
                onError("preferences: " + saved.error().toString());
        });
        all.quiet = true;
        all.layout.width = ui::SizeRule::fixed(90);
        all.layout.height = ui::SizeRule::fixed(22);
    }
    remembered_->emplace<Hairline>("prefs.remembered.rule");
    if (loads.empty()) {
        remembered_->emplace<ui::Label>("prefs.remembered.empty",
                                        "Nothing is remembered. A choice made when a project opens "
                                        "is kept here if you ask for it.",
                                        ui::Font::Ui, "caption", "ink-quiet");
        remembered_->relayout();
        return;
    }
    // Up to six rows stand as they are; more scroll (and the bar takes its own strip).
    ui::Node* rows = nullptr;
    if (loads.size() > 6) {
        auto& scroll = remembered_->emplace<RememberedScroll>("prefs.remembered.scroll");
        scroll.layout.width = ui::SizeRule::fraction();
        scroll.layout.height = ui::SizeRule::content();
        scroll.content().layout.dir = ui::Layout::Dir::Column;
        scroll.content().layout.gap = 4;
        rows = &scroll.content();
    } else {
        rows = &remembered_->emplace<ui::Node>("prefs.remembered.rows");
        rows->layout.dir = ui::Layout::Dir::Column;
        rows->layout.height = ui::SizeRule::content();
        rows->layout.gap = 4;
    }
    for (std::size_t i = 0; i < loads.size(); ++i) {
        const RememberedLoad& load = loads[i];
        const std::string at = "prefs.remembered.row." + std::to_string(i);
        auto& line = rows->emplace<ui::Node>(at);
        line.layout.dir = ui::Layout::Dir::Row;
        line.layout.height = ui::SizeRule::fixed(24);
        line.layout.gap = 8;
        line.layout.align = ui::Layout::Align::Center;
        std::string who = load.vendor;
        if (!load.version.empty()) who += (who.empty() ? "" : " \u00b7 ") + load.version;
        // The label column of the form: the plug-in and, quietly, who made it, on one line.
        std::vector<ui::TextRun> runs{
            {load.name.empty() ? "Unknown plug-in" : load.name, ui::Font::UiBold, "ink"}};
        if (!who.empty()) runs.push_back({"  " + who, ui::Font::Ui, "ink-quiet"});
        auto& name = line.emplace<ui::RichText>(at + ".name", std::move(runs), "small");
        name.layout.width = ui::SizeRule::fixed(200);
        line.emplace<ui::Label>(at + ".choice", rememberedChoice(load.mode), ui::Font::Ui, "small",
                                "ink")
            .layout.width = ui::SizeRule::fraction();
        auto& forget = line.emplace<ui::Button>(at + ".forget", "Forget", [this, i] {
            auto next = prefs_.rememberedLoads;
            if (i >= next.size()) return;
            next.erase(next.begin() + static_cast<std::ptrdiff_t>(i));
            if (auto saved = setRememberedLoads(std::move(next)); !saved && onError)
                onError("preferences: " + saved.error().toString());
        });
        forget.quiet = true;
        forget.layout.width = ui::SizeRule::fixed(60);
        forget.layout.height = ui::SizeRule::fixed(22);
    }
    remembered_->relayout();
}

void PreferencesDialog::refresh() {
    rebuildRemembered();
    if (latency_) latency_->setText(prefs_.preventDeepIdle ? "On" : "Off");
    if (imports_) imports_->setText(prefs_.copyImports ? "On" : "Off");
    if (isolation_) isolation_->setText(isolationLabel(prefs_.pluginIsolation));
    // Held without the option while a bridged plug-in plays live (ADR-0086).
    const auto& latency = app_.cpuIdleLatencyStatus();
    protectionNote(
        latencyNote_, latency,
        latency.requested ? "Requested: 20 µs" : "Held while a bridged plug-in plays live: 20 µs");
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

Status PreferencesDialog::setPluginIsolation(model::Isolation isolation) {
    auto next = prefs_;
    next.pluginIsolation = isolation;
    SUPPLE_TRY(app_.setPluginIsolation(isolation));  // inherit is refused before saving
    if (!file_.empty()) {
        if (auto saved = save(next); !saved) {
            (void)app_.setPluginIsolation(prefs_.pluginIsolation);
            return saved;
        }
    }
    prefs_ = next;
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

Status PreferencesDialog::setSync(const engine::SyncSettings& sync) {
    if (sync == prefs_.sync) return {};
    auto next = prefs_;
    next.sync = sync;
    if (!file_.empty()) SUPPLE_TRY(next.save(file_));
    prefs_ = next;
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
