// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Preferences (Edit > Preferences…): app-wide settings, never project state.
// The monitoring mode, audio thread limit, opt-in process controls, render
// folder, import copying and the audio device are saved to
// <config>/<slug>/preferences.json (supple.preferences/1.6). Buttons open the
// audio device and mouse modifiers dialogs.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "audio_device.h"
#include "mouse_dialog.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct Preferences {
    model::Monitor defaultMonitor = model::Monitor::On;
    unsigned audioThreads = 0;  // Automatic; otherwise limit 1..64, including each executor
    bool lockMemory = false;
    bool preventDeepIdle = false;
    std::filesystem::path renderDirectory{};  // empty: the app's default (mediaHome())
    bool copyImports = true;        // imported audio is copied into the project's media folder
    AudioDeviceSettings audio;      // the device the engine starts with (1.5)
    bool renderOpenFolder = false;  // open the render folder when a render finishes (1.6)
    engine::MidiPortPreferences midiInputs, midiOutputs;
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
    void setDefaultMonitor(model::Monitor m);  // applied at once and saved
    Status setMemoryLock(bool requested);
    Status setPreventDeepIdle(bool requested);
    Status setCopyImports(bool on);
    void refresh();  // open dialog only; also called by the existing GUI tick
    Status setAudioThreads(unsigned limit);  // saved; next engine start
    // The render dialog's folder choice: saved, and renders go there at once.
    Status setRenderDirectory(const std::filesystem::path& dir);
    // The render dialog's "Open folder when done": saved.
    Status setRenderOpenFolder(bool on);
    // The audio device dialog's settings: saved (the dialog applies them).
    Status setAudioDevice(const AudioDeviceSettings& audio);
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
    ui::Button* audio_ = nullptr;
    std::array<ui::Label*, 4> latencyNote_{};
};

}  // namespace supple::gui
