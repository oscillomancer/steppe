// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// launch.commit's layout (ADR-0097 L4, "Scenes and the arrangement"): a
// captured performance written onto the timeline as ordinary items.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command_helpers.h"

namespace supple::engine {

// A captured performance as launch.commit records it in history: the song
// sample where the capture began, its running length and the transport
// loop it ran under, all at `sampleRate`, and the launches with their
// running frames from the start (frame 0: what each track played then).
struct CapturedLaunch {
    std::int64_t frame = 0;
    std::string track, action;  // clip | arrangement | stop
    std::optional<std::string> clip;
    std::optional<std::string> scene;  // launched as part of this scene (launch.scene)
    std::int64_t anchor = 0;           // song sample the clip's phase runs from
    double anchorBeat = 0;
};
struct CapturedLoop {
    std::int64_t start = 0, end = 0;
};
struct CapturedPerformance {
    double sampleRate = 48000;
    std::int64_t start = 0, frames = 0;
    std::optional<CapturedLoop> loop;
    std::vector<CapturedLaunch> launches;
};
schema::Schema<CapturedPerformance> capturedPerformanceSchema();
// The engine's capture as launch.commit records it, at the rate it ran at
// (launch.commit and a render's "captured").
CapturedPerformance performanceOf(const Engine::LaunchCapture& c, double rate);

// What `perf` lays down: of a track's launches at one frame the last given
// (a later one there takes it over), in track then frame order, less the
// arrangement a track begins in (a return at frame 0). A capture begun inside
// a crossfade lists what it fades from, then the switch, at frame 0 (fault
// B): only the switch is laid.
std::vector<const CapturedLaunch*> heldLaunches(const CapturedPerformance& perf);
// Whether `perf` holds anything launch.commit writes: a clip or a stop held
// on some track. The one test of whether a capture holds a performance: the
// uncommitted check (the project-switch prompt, the kept and replace
// warnings) asks it, and launch.commit lays out only the tracks it holds on
// (round 219 F2).
bool holdsLaunches(const CapturedPerformance& perf);

// A section written for a scene launched in the performance: every clip of the
// scene started on one frame, held until one of its tracks was launched again.
struct CommittedSection {
    std::string scene, region;
    double start = 0, end = 0;  // beats
};
// A launched stretch of a MIDI clip copied under a key that is not the
// clip's: what its context.mismatch warning says, for a caller to word.
struct CommittedMismatch {
    std::string clip;
    std::string kind;  // "key" | "chord", as followed.mismatch
    std::optional<std::string> clipKey, key, clipChord, chord;
    std::int64_t bar = 0;  // the first project bar where they differ
};
struct CommitDone {
    std::vector<std::string> tracks, items;
    std::vector<CommittedSection> sections;
    std::optional<std::vector<CommittedMismatch>> mismatches;  // with context.mismatch
    double from = 0, to = 0;                                   // beats
};
// Lays `perf` out linearly from where it began, by running time (L-f): on
// each track that left the arrangement, its clip, stopped and held
// stretches replace the arrangement there, each clip as copies of its
// takes (MIDI copied unless `pooled`) at the phase it was heard, and a
// section (region) for each scene launched whole. Runs in the caller's
// transaction; errors point under /performance.
Result<CommitDone> commitPerformance(cmd::Context& ctx, const CapturedPerformance& perf,
                                     bool pooled);

}  // namespace supple::engine
