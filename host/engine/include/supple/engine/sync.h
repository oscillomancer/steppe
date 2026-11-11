// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// External sync input and LTC output in the real-time engine (ADR-0071,
// engine.md § External sync). The audio callback feeds the chosen source's
// receiver (MIDI clock, MTC, or the LTC decoder on an input channel), runs
// the chase tracker against the transport it plays, and writes LTC to an
// output channel; the main thread (EngineApp) turns what it reports into
// transport changes and the live chase rate.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "supple/sync/chase.h"
#include "supple/sync/timecode.h"

namespace supple::engine {

enum class SyncSource : std::uint8_t { Off, MidiClock, Mtc, Ltc };
std::string_view syncSourceName(SyncSource source);  // off | midi_clock | mtc | ltc

// What the engine runs with, resolved by EngineApp from the settings: port
// and channel indices, the project's timecode.
struct SyncConfig {
    SyncSource source = SyncSource::Off;
    int midiPort = -1;   // index into Backend::midiPorts(); -1: none
    int ltcInput = -1;   // device input channel, 0-based; -1: none
    int ltcOutput = -1;  // device output channel, 0-based; -1: no LTC output
    double ltcLevelDb = -18.0;
    bool followTempo = true;     // MIDI clock: chase its tempo (else transport only)
    double offsetSeconds = 0.0;  // positive: Steppe plays that much behind the master
    // After the signal stops, the transport keeps going this long before a
    // loss is final (0: stop at once).
    double freewheelSeconds = 0.0;
    sync::TimecodeMapping timecode{sync::FrameRate::Fps30, {}};
    bool operator==(const SyncConfig&) const = default;
    // Whether `other` needs another runtime (receivers, LTC buffers); the
    // rest (follow tempo, offset, freewheel) changes in place, keeping what
    // the receivers know.
    bool sameRuntime(const SyncConfig& other) const {
        return source == other.source && midiPort == other.midiPort && ltcInput == other.ltcInput &&
               ltcOutput == other.ltcOutput && ltcLevelDb == other.ltcLevelDb &&
               timecode == other.timecode;
    }
    // Whether the engine runs a sync plan, every node live so transport
    // changes land with the device's latency: an input source. LTC output
    // alone needs none (it writes the timeline the callback plays).
    bool listens() const { return source != SyncSource::Off; }
    // Whether a chase can change the rate, so the sync plan has the rate path
    // on with stretch converters even at 1.0: MTC, LTC, or MIDI clock with
    // follow tempo.
    bool chases() const {
        return source == SyncSource::Mtc || source == SyncSource::Ltc ||
               (source == SyncSource::MidiClock && followTempo);
    }
};

// What the callback last saw, for engine.status and EngineApp's service.
struct SyncStatus {
    SyncSource source = SyncSource::Off;
    bool present = false;    // the signal arrives
    bool running = false;    // MIDI clock: after Start/Continue; timecode: moving
    bool locked = false;     // the receiver's lock
    std::uint8_t chase = 0;  // sync::ChaseState
    int direction = 0;       // timecode: +1 forward, -1 backward
    double bpm = 0.0;        // MIDI clock: the external tempo
    // The latest external position: beats (clock) or timecode seconds.
    double position = 0.0;
    // MIDI clock: where Continue resumes (beats); the Song Position while stopped.
    double songPosition = 0.0;
    sync::FrameRate rate = sync::FrameRate::Fps30;  // timecode: the detected rate
    bool rateKnown = false;
    sync::Timecode timecode{};  // timecode: the latest label
    double chaseRate = 1.0;     // the tracker's playrate (1 when not chasing)
    double externalRate = 1.0;  // the source's speed as a playrate
    double errorSeconds = 0.0;  // external minus heard local position
    // Where to relocate: the local timeline seconds the master will be at when
    // audio from a seek starting now is heard (EngineApp adds its seek time,
    // times `externalRate`).
    double relocateSeconds = 0.0;
    double heardLatency = 0.0;    // seconds from processing to the listener (plan + device)
    std::uint32_t starts = 0;     // MIDI clock: Start/Continue seen
    std::uint32_t stops = 0;      // MIDI clock: Stop seen
    std::uint32_t jumps = 0;      // the receiver's jumps (SPP, full frames, breaks)
    std::uint32_t relocates = 0;  // the tracker's relocate requests
    std::uint32_t losses = 0;     // signal losses
    std::int64_t device = 0;      // device frames since the engine started
    std::int64_t lostFrames = 0;  // device frames since the last observation
    bool ltcOutput = false;       // LTC output is being written
    std::uint64_t sequence = 0;   // bumps with every publication
    std::uint32_t runtime = 0;    // which runtime published it (a new one restarts the counts)
};

}  // namespace supple::engine
