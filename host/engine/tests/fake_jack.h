// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The test controls of the stand-in libjack (fake_jack.cpp). The backend
// dlopens the same file, so both share one copy of its state.
#pragma once

#include <dlfcn.h>

#include <cstdint>
#include <stdexcept>
#include <string>

struct FakeJack {
    static constexpr const char* kPath = FAKE_JACK_PATH;

    void* handle = ::dlopen(kPath, RTLD_NOW | RTLD_LOCAL);

    template <class F>
    F fn(const char* name) const {
        auto f = reinterpret_cast<F>(::dlsym(handle, name));
        if (!f) throw std::runtime_error(std::string("fake libjack lacks ") + name);
        return f;
    }

    void (*setPeriod)(std::uint32_t) = fn<void (*)(std::uint32_t)>("fakejack_set_period");
    void (*setRate)(std::uint32_t) = fn<void (*)(std::uint32_t)>("fakejack_set_rate");
    // PipeWire's start-up: this rate until the first cycle (0: off).
    void (*setStaleRate)(std::uint32_t) = fn<void (*)(std::uint32_t)>("fakejack_set_stale_rate");
    void (*xrun)() = fn<void (*)()>("fakejack_xrun");
    void (*stall)(int) = fn<void (*)(int)>("fakejack_stall");
    void (*failOpen)(int) = fn<void (*)(int)>("fakejack_fail_open");
    void (*failCapture)(int) = fn<void (*)(int)>("fakejack_fail_capture");
    void (*setInput)(float) = fn<void (*)(float)>("fakejack_set_input");
    void (*shutdown)() = fn<void (*)()>("fakejack_shutdown");
    int (*open)() = fn<int (*)()>("fakejack_open");
    std::uint64_t (*cycles)() = fn<std::uint64_t (*)()>("fakejack_cycles");
    std::uint32_t (*bufferSizeRequest)() = fn<std::uint32_t (*)()>("fakejack_buffer_size_request");
    int (*connections)() = fn<int (*)()>("fakejack_connections");
    void (*midi)(int, std::uint32_t, int, int,
                 int) = fn<void (*)(int, std::uint32_t, int, int, int)>("fakejack_midi");
    const char* (*latencyEnv)() = fn<const char* (*)()>("fakejack_latency_env");
    const char* (*rateEnv)() = fn<const char* (*)()>("fakejack_rate_env");
    const char* (*propsEnv)() = fn<const char* (*)()>("fakejack_props_env");
    float (*outputPeak)(const char*) = fn<float (*)(const char*)>("fakejack_output_peak");

    struct WrittenMidi {
        std::uint32_t port = 0, frame = 0;
        std::uint64_t cycle = 0;
        std::uint8_t size = 0, bytes[10]{};
    };
    unsigned (*midiWritten)() = fn<unsigned (*)()>("fakejack_midi_written");
    unsigned (*midiClears)() = fn<unsigned (*)()>("fakejack_midi_clears");
    void (*midiRead)(unsigned, void*) = fn<void (*)(unsigned, void*)>("fakejack_midi_read");
    void (*topology)() = fn<void (*)()>("fakejack_topology");
    void (*midiFail)(int, int, int) = fn<void (*)(int, int, int)>("fakejack_midi_fail");

    // Back to a healthy server at 48 kHz with the given period.
    void reset(std::uint32_t period = 1024) {
        stall(0);
        failOpen(0);
        failCapture(0);
        midiFail(0, 0, 0);
        setInput(0.0f);
        setRate(48000);
        setStaleRate(0);
        setPeriod(period);
    }
};

inline FakeJack& fakeJack() {
    static FakeJack f;
    return f;
}
