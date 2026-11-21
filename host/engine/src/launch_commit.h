// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// launch.commit's layout (ADR-0097 L4, "Scenes and the arrangement"): a
// captured performance written onto the timeline as ordinary items.
#pragma once

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

// A section written for a scene launched in the performance: every clip of the
// scene started on one frame, held until one of its tracks was launched again.
struct CommittedSection {
    std::string scene, region;
    double start = 0, end = 0;  // beats
};
struct CommitDone {
    std::vector<std::string> tracks, items;
    std::vector<CommittedSection> sections;
    double from = 0, to = 0;  // beats
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
