// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Effects as preset entries, shared by FX chain presets, track templates and
// rack presets (rack.md §7): an entry is a model FxInstance with the plug-in's
// name and its settings as a semantic patch or base64 state, independent of
// session ids.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"
#include "supple/model/session.h"

namespace supple::engine {
class EngineApp;
}

namespace supple::engine::detail {

// The effects, in order, as preset entries: the semantic patch when the
// plug-in has one, else its state as base64 (from the model, or from a probe
// instance when the model holds only parameter values).
Result<std::vector<json::Value>> encodeEffects(EngineApp& app, const model::Session& s,
                                               const std::vector<model::Id>& chain);

// A preset entry checked and ready to add: its plug-in installed, its patch
// merged or its state loaded once (nothing changes the model yet).
struct PreparedFx {
    model::FxInstance fx;
    std::shared_ptr<const model::Blob> blob;
};

// Checks entries (their JSON paths under `base`): errors chain.invalid and
// chain.plugin_missing, or the plug-in's own when its state does not load.
Result<std::vector<PreparedFx>> prepareEffects(EngineApp& app,
                                               const std::vector<json::Value>& entries,
                                               const std::string& base);

// Captures the current live settings without changing the session or its history.
Status captureLiveState(EngineApp& app, model::Id id, model::FxInstance& fx,
                        std::shared_ptr<const model::Blob>& blob);

// Creates a prepared effect, in no chain, in the open transaction; its id.
model::Id installEffect(EngineApp& app, PreparedFx& prepared);

// Writes a preset file; without `replace` an existing file is refused
// (chain.exists), atomically even against another process saving the name.
Status writePreset(const std::filesystem::path& path, std::string_view text, bool replace);

}  // namespace supple::engine::detail
