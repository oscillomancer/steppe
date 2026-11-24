// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The audio device (Edit > Audio device…, and a row of Preferences): the
// backend, devices, sample rate and buffer size the engine starts with,
// saved as "audio" in preferences.json (supple.preferences/1.5). A change
// restarts a running engine at once. On Linux the rate and buffer are
// requests to PipeWire through JACK, or forced on the graph while the engine
// runs; on macOS and Windows they open the native device (ADR-0050).
#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/engine/portaudio.h"
#include "supple/json/value.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct AudioDeviceSettings {
    std::string backend;       // jack | portaudio | dummy; empty: the platform's default
    std::string hostApi;       // portaudio: exact name; empty: PortAudio's default
    std::string outputDevice;  // portaudio: exact name; empty: the host API's default
    std::string inputDevice;   // portaudio: exact name; empty: the host API's default
    bool record = true;        // portaudio: open the input device (off: playback only)
    double rate = 0;           // Hz; 0: the device's (or the graph's) own rate
    unsigned block = 256;      // frames, 16..4096
    bool forceGraph = false;   // JACK on PipeWire: force rate and block while running
    bool operator==(const AudioDeviceSettings&) const = default;
};

// The backend an interactive app starts with by default: JACK on Linux, the
// native device (PortAudio) on macOS and Windows.
std::string defaultAudioBackend();
std::string effectiveBackend(const AudioDeviceSettings& settings);

// The "audio" object of preferences.json. Errors are preferences.audio at
// /audio/<field>, with a hint.
Result<AudioDeviceSettings> parseAudioDevice(const json::Value& value);
json::Value audioDeviceJson(const AudioDeviceSettings& settings);

// engine.start's arguments for `settings`. For portaudio, `devices` (from
// engine.devices) gives the input's channel count and the automatic rate (the
// output device's own); an empty host API is WASAPI where it is listed, else
// the first listed (PortAudio's default).
json::Value engineStartArgs(const AudioDeviceSettings& settings,
                            std::span<const engine::AudioDevice> devices);

class AudioDeviceDialog {
  public:
    using Enumerate = std::function<Result<std::vector<engine::AudioDevice>>()>;
    using Save = std::function<Status(const AudioDeviceSettings&)>;
    // `save` persists a change (empty: not saved, as in tests and offscreen
    // runs).
    AudioDeviceDialog(ui::Root& root, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
                      AudioDeviceSettings settings, Save save);
    // Starts the engine from the settings. With `fallback` (app start), a
    // refused choice of devices or rate retries the defaults; the saved
    // choice is kept for when the device returns.
    Status start(bool fallback = false);
    // Saved, then a running engine (or one whose start failed) restarts with
    // it. A failed save changes nothing.
    Status apply(AudioDeviceSettings next);
    Status restart();  // stop if running, then start
    void stop();
    void open();  // lists the devices present now
    // Refresh: lists the devices again. PortAudio sees a device plugged in
    // since only when nothing holds it, so a running engine stops for the
    // listing and starts again the way it last started (with the defaults
    // as a fallback when that start had them; audio stops for a moment).
    // Refused while
    // recording (audio.recording), which it would put at risk, and while a
    // launcher capture is held (audio.capture): an engine stop discards it.
    // A failed start is said in the dialog's status and through onError; the
    // engine is left stopped, the fresh list shown.
    Status rescan();
    bool recording() const;
    // Why Refresh is off now ("" when it is not): recording or a capture.
    std::string refreshBlocked() const;
    bool isOpen() const;
    void refresh();  // open dialog only; also called by the GUI tick
    const AudioDeviceSettings& settings() const { return settings_; }
    // One line for Preferences: the backend and device, rate and buffer.
    std::string summary() const;
    const std::string& lastError() const { return lastError_; }
    Enumerate enumerate;  // default: engine.devices with backend portaudio
    // engine.start/engine.stop; tests replace them to avoid real devices.
    std::function<Result<json::Value>(const json::Value& args)> startEngine;
    std::function<void()> stopEngine;
    std::function<void(const std::string&)> onError;
    // A launcher capture armed, running or kept uncommitted. Default: the
    // engine's running capture; the app adds the Capture toggle's own.
    std::function<bool()> captureHeld;

  private:
    std::vector<engine::AudioDevice> devices(bool reload);
    std::string hostApiName() const;
    void openMenu(const std::string& id, std::vector<ui::MenuItem> items);
    void build(bool reload);  // the dialog's rows; `reload` enumerates again
    void choose(AudioDeviceSettings next);

    ui::Root& root_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    AudioDeviceSettings settings_;
    AudioDeviceSettings running_;  // what the engine last started with (a fallback differs)
    Save save_;
    std::vector<engine::AudioDevice> devices_;
    std::string enumerateError_, lastError_;
    bool wanted_ = false;               // a start was asked for: changes restart the engine
    bool startedWithFallback_ = false;  // the last start(): Refresh starts the same way
};

}  // namespace supple::gui
