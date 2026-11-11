// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/engine/realtime.h"
namespace supple::engine {
namespace detail {
struct PortAudioApi;
class MidiSource;
class MidiSink;
}  // namespace detail
struct PortAudioOptions {
    // Exact names. Empty selects the platform's default host API/device.
    std::string hostApi, inputDevice, outputDevice;
    // The requested rate is the device's to run (CoreAudio switches the
    // device's nominal rate); otherwise the host API converts as needed.
    bool changeDeviceRate = false;
    double stopTimeoutSeconds = 2.0;
};
struct AudioDevice {
    std::string hostApi, name;
    std::uint32_t inputs = 0, outputs = 0;
    double defaultRate = 0;
    bool defaultInput = false, defaultOutput = false;
};
// CoreAudio (macOS), WASAPI/ASIO (Windows); Linux keeps JACK.
class PortAudioBackend final : public Backend {
  public:
    explicit PortAudioBackend(PortAudioOptions options = {});
    // Injectable driver seam. It never initializes the platform device library.
    PortAudioBackend(PortAudioOptions options, const detail::PortAudioApi& api);
    PortAudioBackend(PortAudioOptions options, const detail::PortAudioApi& api,
                     std::unique_ptr<detail::MidiSource> midi);
    PortAudioBackend(PortAudioOptions options, const detail::PortAudioApi& api,
                     std::unique_ptr<detail::MidiSource> midi,
                     std::unique_ptr<detail::MidiSink> sink);
    ~PortAudioBackend() override;
    Status start(const BackendConfig&, Callback) override;
    void stop() override;
    BackendConfig config() const override;
    std::string name() const override { return "portaudio"; }
    bool ownsThread() const override;
    void setMainThreadWake(MainThreadWake*) override;
    BackendStatus status() const override;
    std::vector<std::string> inputNames() const override;
    std::span<const MidiInputEvent> midiInput() const override;
    std::span<const SyncMidiEvent> syncMidi() const override;
    std::vector<std::string> midiPorts() const override;
    std::vector<MidiInputWarning> midiWarnings() const override;
    std::uint64_t midiDropped() const override;
    bool writeMidi(const MidiOutputEvent&) SUPPLE_RT override;
    std::vector<std::string> midiOutputPorts() const override;
    std::vector<std::int64_t> midiOutputLatency() const override;
    std::vector<MidiInputWarning> midiOutputWarnings() const override;
    std::uint64_t midiOutDropped() const override;
    bool nativeThreadPolicy() const override;
    Status prepareCurrentThread(bool live) override;
    void releaseCurrentThread() override;
    WorkgroupStatus workgroup() const override;
    Status prepareLiveThread(std::thread::native_handle_type) override;
    ThreadPolicy callbackPolicy() override;
    static Result<std::vector<AudioDevice>> devices();
    Result<std::vector<AudioDevice>> driverDevices() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace supple::engine
