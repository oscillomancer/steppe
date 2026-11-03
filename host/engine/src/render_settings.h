// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Render settings resolution and presets (ADR-0055, engine.md §3 Settings):
// command arguments, then project.render, then the user's default, then the
// built-in defaults, per top-level member. Presets and the default are
// per-user files, `supple.render-preset/1.0`, in the render-presets folder.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/render_settings.h"
#include "supple/model/session.h"

namespace supple::engine {
class EngineApp;
namespace rs = model::render;

// Each top-level member's layer: "args", "project", "default" or "built-in".
using RenderOrigins = std::map<std::string, std::string>;

// The project's settings as stored (project.render, already validated).
rs::Settings projectRenderSettings(const model::Project& project);
// The built-in layer for a session (the project's rate, WAV 32-bit float, …).
rs::Settings builtInRenderSettings(const model::Session& session);
// Per member, the first layer that has it.
rs::Settings resolveRenderSettings(const rs::Settings& args, const rs::Settings& project,
                                   const rs::Settings& user, const rs::Settings& builtIn,
                                   RenderOrigins* from = nullptr);
// Fills the optional members of the objects with their defaults (targets,
// bit depths, dither for 16/24 bits, MP3 and Vorbis options, patterns per
// source), so the result shows what applies. Only defaults that depend on
// nothing else are filled: an output's sample_rate stays absent unless given
// (outputSampleRate resolves it against the chain rate where it is used), so
// settings written back keep following the chain rate.
void completeRenderSettings(rs::Settings& settings);
// The default bit depth for a format (0: none, for MP3 and Vorbis).
int defaultBitDepth(rs::Format format);
// An output's effective rate: its own, else the chain's, adjusted to what
// the format accepts (session-model §1.10).
int outputSampleRate(const rs::Output& output, int chainRate);

// The render-presets folder: AppOptions::renderPresetDir, else
// <workdir>/render-presets.
std::filesystem::path renderPresetDir(const EngineApp& app);
// The user's default (default.json), if there is one.
Result<std::optional<rs::Settings>> loadRenderDefault(const EngineApp& app);
struct RenderPreset {
    std::string name;
    bool factory = false;
    rs::Settings settings;
};
// Factory presets (compiled in, read-only), then the user's, by name.
Result<std::vector<RenderPreset>> listRenderPresets(const EngineApp& app);
std::vector<RenderPreset> factoryRenderPresets();
// Removes what never belongs in a preset: stems.tracks, matrix, custom bounds.
rs::Settings presetSettings(rs::Settings settings);
Status validPresetName(const std::string& name);
Status saveRenderPreset(const EngineApp& app, const std::optional<std::string>& name,
                        const rs::Settings& settings);  // nullopt: the default
Status deleteRenderPreset(const EngineApp& app, const std::optional<std::string>& name);
Result<rs::Settings> findRenderPreset(const EngineApp& app, const std::string& name);

}  // namespace supple::engine
