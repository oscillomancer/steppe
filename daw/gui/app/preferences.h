// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Preferences (Edit > Preferences…): app-wide settings, never project state.
// The monitoring mode, audio thread limit, opt-in process controls, render
// folder, import copying, the audio device and external sync are saved to
// <config>/<slug>/preferences.json (supple.preferences/1.11). Buttons open the
// audio device and mouse modifiers dialogs.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "audio_device.h"
#include "mouse_dialog.h"
#include "supple/engine/app.h"
#include "supple/ui/forms.h"
#include "supple/ui/root.h"

namespace supple::gui {

// A choice kept for one exact build of a plug-in (a preference, opt-in, managed in Settings).
struct RememberedLoad {
    std::string format, id, buildId;    // the key: the build is the plug-in's binary identity
    std::string mode;                   // "normal", "bridge" or "disable"
    std::string name, vendor, version;  // for the Settings list only
    std::int64_t rememberedMs = 0;
    bool operator==(const RememberedLoad&) const = default;
};

struct Preferences {
    model::Monitor defaultMonitor = model::Monitor::On;
    unsigned audioThreads = 0;  // Automatic; otherwise limit 1..64, including each executor
    bool lockMemory = false;
    bool preventDeepIdle = false;
    std::filesystem::path renderDirectory{};  // empty: the app's default (mediaHome())
    bool copyImports = true;  // imported audio is copied into the project's media folder
    // A recording pass over an item is a new take of it; off, it is laid on top as an item
    // (1.11, ADR-0103). The engine keeps it only while it runs: this is its keeper.
    bool stackRecordedTakes = true;
    AudioDeviceSettings audio;      // the device the engine starts with (1.5)
    bool renderOpenFolder = false;  // open the render folder when a render finishes (1.6)
    engine::SyncSettings sync;      // external sync and LTC output (1.7, ADR-0071)
    // Where third-party plug-ins left on inherit run (1.9, ADR-0081).
    model::Isolation pluginIsolation = model::Isolation::InProcess;
    // The MIDI port records (routing, ADR-0072; clock output, 1.8, ADR-0073),
    // read for display. The engine is their only writer (midi.*.set).
    engine::MidiPortPreferences midiInputs, midiOutputs;
    // Load choices kept for an exact build of a plug-in (1.10, topic 7d), applied without asking
    // when a crash report points at that build; listed in Settings, where each is forgotten.
    std::vector<RememberedLoad> rememberedLoads;
    static std::filesystem::path defaultFile(std::string_view slug);
    static Result<Preferences> load(const std::filesystem::path& file);  // absent: defaults
    Status save(const std::filesystem::path& file) const;
};

class PreferencesDialog {
  public:
    // `file` empty: not saved (tests, offscreen runs). The app's default
    // monitoring follows `prefs` from construction on.
    PreferencesDialog(ui::Root& root, engine::EngineApp& app, MouseDialog& mouse, Preferences prefs,
                      std::filesystem::path file);
    void open();
    bool isOpen() const;
    // The lines under Lock memory and under Keep CPUs out of deep idle: on
    // Linux the cautions and how to allow each; elsewhere that it is
    // Linux's alone.
    static std::vector<std::string> memoryNotes(bool onLinux);
    static std::vector<std::string> idleNotes(bool onLinux);
    // Whether one of those two rows is shown: always on Linux; elsewhere
    // only while its setting is on (it can then be turned off).
    static bool showsLinuxOnly(bool onLinux, bool on);
    // Whether this is Linux, for those two rows (tests set it to see the
    // dialog either way on any platform).
#if defined(__linux__)
    bool onLinux = true;
#else
    bool onLinux = false;
#endif
    // The most room the settings under the header may take before they
    // scroll, in design units. 0: what the window leaves (tests set it).
    float bodyLimit = 0;
    // Shown above the settings while non-empty: the app sets it when the
    // preferences file could not be used (a newer version's: preferences.newer).
    std::string fileNotice;
    void setDefaultMonitor(model::Monitor m);  // applied at once and saved
    Status setMemoryLock(bool requested);
    Status setPreventDeepIdle(bool requested);
    Status setCopyImports(bool on);
    Status setStackRecordedTakes(bool on);  // saved, and applied to the engine at once
    Status setPluginIsolation(model::Isolation isolation);  // saved; as plug-ins load
    // The remembered load choices (safe-load, topic 7d): saved. Forgetting one is a removal.
    Status setRememberedLoads(std::vector<RememberedLoad> loads);
    void refresh();  // open dialog only; also called by the existing GUI tick
    Status setAudioThreads(unsigned limit);  // saved; next engine start
    // The render dialog's folder choice: saved, and renders go there at once.
    Status setRenderDirectory(const std::filesystem::path& dir);
    // The render dialog's "Open folder when done": saved.
    Status setRenderOpenFolder(bool on);
    // The audio device dialog's settings: saved (the dialog applies them).
    Status setAudioDevice(const AudioDeviceSettings& audio);
    // The sync settings (from the sync dialog, or an agent's sync.set): saved.
    Status setSync(const engine::SyncSettings& sync);
    // Opens the audio device dialog (the Audio device row's button).
    std::function<void()> onAudioDevice;
    std::function<std::string()> audioSummary;  // the row's text
    const Preferences& preferences() const { return prefs_; }
    std::function<void(const std::string&)> onError;

  private:
    Status save(Preferences& prefs);
    ui::Root& root_;
    engine::EngineApp& app_;
    MouseDialog& mouse_;
    Preferences prefs_;
    std::filesystem::path file_;
    ui::Button* monitor_ = nullptr;
    ui::Button* threads_ = nullptr;
    ui::Button* memory_ = nullptr;
    std::array<ui::Label*, 4> memoryNote_{};
    ui::Button* latency_ = nullptr;
    ui::Button* imports_ = nullptr;
    ui::Button* stack_ = nullptr;
    ui::Button* isolation_ = nullptr;
    ui::Button* audio_ = nullptr;
    ui::Node* advanced_ = nullptr;  // the low-level settings, under their heading
    ui::Button* advancedToggle_ = nullptr;
    bool advancedOpen_ = false;  // kept while the app runs
    std::array<ui::Label*, 4> latencyNote_{};
    // The remembered plug-in choices (safe-load, topic 7d): a section of its own at the end.
    void rebuildRemembered();
    ui::Node* remembered_ = nullptr;
    std::string rememberedShown_;  // what the section was built from: it is rebuilt on a change
};

}  // namespace supple::gui
