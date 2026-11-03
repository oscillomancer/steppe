// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "render_chain.h"
#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/offline.h"
namespace supple::engine {
class EngineApp;
struct RenderTarget {
    std::uint64_t generation;
    std::string branch;
};
RenderTarget renderTarget(const EngineApp& app);
Status checkRenderTarget(const EngineApp& app, const RenderTarget& target);
// Final publication runs on the model thread. Stale jobs retain their files
// in the result with status/error render.stale_target and never import.
void completeRenderJob(EngineApp& app, cmd::Dispatcher& dispatcher, cmd::JobManager& jobs,
                       std::string id, RenderTarget target, json::Value result,
                       std::function<void()> publish);
struct RenderPatternContext {
    std::string project, track, folderPath, region, marker, item, take, format = "wav", branch;
    int trackNumber = 0, regionNumber = 0, sampleRate = 48000, bitDepth = 32, counter = 1;
    std::uint64_t version = 0;
    double startSeconds = 0, startBeats = 0;
};
Result<std::string> expandRenderPattern(std::string_view pattern,
                                        const RenderPatternContext& context);
Status checkRenderRange(const model::Session& session, const OfflineSettings& settings,
                        std::optional<std::int64_t> sampleRate = {});
Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity);
// The render commands (engine.md §3): render.mix, .stems, .regions,
// .dry-run, .settings, .preset.*, .queue.* and branch.render-ab, one
// implementation over the resolved render settings (ADR-0055).
// Adds rendered files (a render result's "files": path, start_beat) to the
// project as items on a new track each, at their start, as one undo step
// right after the render (posted to the model thread; from a job, when it
// completes). Used by add_to_project on the render commands.
void postAddToProject(EngineApp& app, RenderTarget target, cmd::Dispatcher& dispatcher,
                      const cmd::Origin& origin, const json::Value& files);
void registerRenderCommands(cmd::Registry& registry);
}  // namespace supple::engine
