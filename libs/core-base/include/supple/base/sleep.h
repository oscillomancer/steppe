// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <chrono>

namespace supple {

// A short pause for a thread that is not on the audio path: a control or worker
// thread polling for another thread's answer, or a fake device pacing blocks.
// Never call it from audio-path code.
//
// On Windows std::this_thread::sleep_for ends on the system timer tick (15.6 ms
// by default), so a poll that asks for 200 microseconds waits up to 78 times
// that. Here the wait ends on a high-resolution waitable timer (Windows 10
// 1803 and newer), owned by the calling thread, created on its first use and
// closed when the thread exits: process-owned, no global timer change, the
// same timer ADR-0102 gives the bridge waits. Where the timer cannot be
// created or the wait fails, and on every other platform, it is
// std::this_thread::sleep_for.
//
// The guarantee is "not before": both return only once std::chrono::steady_clock has
// reached the deadline (sleepFor's is its call time plus the duration), looking again
// after each wait and waiting the remainder, on the timer and the ordinary path alike,
// because a timer's due time is not steady_clock's and a wait can end a little early.
// Callers get never early and nothing about how late: a wait can end well after, as any
// sleep can, so a caller polls a condition and never decides by the time taken.
//
// The bridge's SharedSignals (host/clap-host/src/bridge_protocol.h) and
// midi_output_windows.cpp create the same timer themselves; they could move onto
// this later, and were left as they are.
void sleepFor(std::chrono::duration<double> duration);
// Until `deadline`; returns at once if it has passed.
void sleepUntil(std::chrono::steady_clock::time_point deadline);

// Whether the calling thread's most recent sleepFor/sleepUntil ended on the
// high-resolution timer (always false off Windows).
bool lastSleepUsedHighResolutionTimer();
// Tests: false makes sleeps take the ordinary path, as Windows before 10 1803 does
// when the timer cannot be created. A process-wide switch, true by default.
void setHighResolutionSleepEnabledForTests(bool enabled);

}  // namespace supple
