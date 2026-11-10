// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The JACK backend (ADR-0004). libjack is loaded at run time through a
// symbol table of our own, so the engine starts on systems without JACK and
// reports a clean error there. On PipeWire, libjack is pipewire-jack.
//
// The graph's period may differ from the engine block, and may change while
// running: the callback is called in chunks of at most `blockSize` frames, so
// a larger period costs nothing but extra calls. The block size is requested
// from PipeWire as the client's latency (PIPEWIRE_LATENCY, a per-client
// request the graph honours if no other client needs a larger period);
// `forceBufferSize` sets the graph period for everyone (jack_set_buffer_size)
// and is for benchmarks. `requestRate` suggests a graph rate (PIPEWIRE_RATE,
// node.rate: applied only while the devices are idle and the rate is one the
// server allows); `forceGraph` instead forces the block as the quantum, and
// the requested rate, for as long as this client is active (node.force-*
// through PIPEWIRE_PROPS). Variables the user set win over all three
// requests. Plain JACK servers ignore them. Rate changes and server shutdown are reported in
// stats() for the owner to restart the engine; nothing is handled on the
// audio thread beyond counting.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "supple/engine/realtime.h"

namespace supple::engine {

struct JackOptions {
    std::string clientName = "supple";
    std::string library = "libjack.so.0";
    bool connectOutputs = true;    // connect our outputs to the physical playback ports
    bool requestLatency = true;    // ask PipeWire for a period of blockSize
    bool forceBufferSize = false;  // jack_set_buffer_size: the whole graph's period
    double requestRate = 0;        // > 0: suggest this graph rate to PipeWire
    bool forceGraph = false;       // force the block (and requestRate) while active
    double stopTimeoutSeconds = 2.0;
    BenchProbe* benchProbe = nullptr;  // benchmark only; whole JACK period, outlives stop()
};

struct JackStats {
    std::uint32_t period = 0;         // the graph's current period, in frames
    std::uint64_t xruns = 0;          // reported by the server
    std::uint64_t cycles = 0;         // process callbacks since start
    std::uint32_t outputLatency = 0;  // frames from our output ports to the device
    bool rateChanged = false;         // the graph left the started rate: restart
    bool serverGone = false;          // the server closed the client: restart
};

class JackBackend : public Backend {
  public:
    using Options = JackOptions;
    using Stats = JackStats;

    explicit JackBackend(Options options = {});
    ~JackBackend() override;
    JackBackend(const JackBackend&) = delete;
    JackBackend& operator=(const JackBackend&) = delete;

    // Opens a client, registers `config.inputs`/`config.outputs` ports and
    // activates it. The granted rate is in config() (the requested rate is
    // not enforced: the graph owns it).
    void setMainThreadWake(MainThreadWake* wake) override { mainWake_ = wake; }
    Status start(const BackendConfig& config, Callback callback) override;
    // The next process cycle makes the final zero-frame callback; then the
    // client is deactivated and closed. If the server is gone or no cycle
    // comes within stopTimeoutSeconds, the client is closed without it.
    void stop() override;
    BackendConfig config() const override { return config_; }
    std::string name() const override { return "jack"; }
    bool ownsThread() const override { return true; }
    unsigned liveThreadCapacity() const override;
    Status prepareLiveThread(std::thread::native_handle_type thread) override;
    ThreadPolicy callbackPolicy() override;  // the client thread's (jack_client_thread_id)
    Stats stats() const;
    BackendStatus status() const override;
    std::span<const MidiInputEvent> midiInput() const override;
    std::vector<std::string> midiPorts() const override;
    bool writeMidi(const MidiOutputEvent&) SUPPLE_RT override;
    std::vector<std::string> midiOutputPorts() const override;
    std::vector<std::int64_t> midiOutputLatency() const override;
    std::vector<MidiInputWarning> midiOutputWarnings() const override;
    std::uint64_t midiOutDropped() const override;
    std::vector<std::string> inputNames() const override;
    std::vector<std::string> outputNames() const override;
    // The device's physical capture ports, in order, from a short-lived
    // client (no activation): how many inputs start() should open.
    static Result<std::vector<std::string>> captureNames(const Options& options = {});
    // The physical playback ports (ADR-0056: one engine output each), likewise.
    static Result<std::vector<std::string>> playbackNames(const Options& options = {});

    // Whether libjack can be loaded and has every symbol we use. Does not
    // contact a server.
    static Status available(const std::string& library = "libjack.so.0");

  private:
    static Result<std::vector<std::string>> physicalPorts(const Options& options, bool capture);
    struct Impl;
    MainThreadWake* mainWake_ = nullptr;
    Options options_;
    BackendConfig config_;
    std::uint64_t stoppedMidiOutDropped_ = 0;
    std::unique_ptr<Impl> impl_;
    std::atomic<Impl*> active_{nullptr};  // set before activation (audio-thread reads)
};

}  // namespace supple::engine
