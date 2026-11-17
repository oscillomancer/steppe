// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// How this run loads a named plug-in, whatever the project says (topic 7d,
// ADR-0083 note; spec: docs/specs/engine.md "Load overrides").
//
// A value type: the app owns one, changes it on the main thread, and gives a
// copy to every InstancePool it makes (the app's own, a render's, a snapshot
// job's, a rack probe's), so no job thread reads the app's map. Nothing here is
// in the project, in history, or saved: it lives as long as the app does.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "supple/base/result.h"
#include "supple/host/clap_host.h"

namespace supple::engine {

enum class LoadMode {
    Normal,   // as the project says (no entry)
    Bridge,   // a dedicated bridge process, whatever the fx asked
    Disable,  // not loaded: a placeholder passes audio through, a rack module is silent
};

inline std::string_view loadModeName(LoadMode mode) {
    switch (mode) {
        case LoadMode::Bridge:
            return "bridge";
        case LoadMode::Disable:
            return "disable";
        case LoadMode::Normal:
            break;
    }
    return "normal";
}

inline std::optional<LoadMode> parseLoadMode(std::string_view text) {
    if (text == "normal") return LoadMode::Normal;
    if (text == "bridge") return LoadMode::Bridge;
    if (text == "disable") return LoadMode::Disable;
    return std::nullopt;
}

class LoadOverrides {
  public:
    using Key = std::pair<std::string, std::string>;  // (format, plug-in id)

    LoadMode mode(std::string_view format, std::string_view id) const {
        const auto it = modes_.find(Key(format, id));
        return it == modes_.end() ? LoadMode::Normal : it->second;
    }
    LoadMode mode(const host::PluginInfo& info) const { return mode(info.format, info.id); }

    // Normal removes the entry. True when something changed.
    bool set(std::string_view format, std::string_view id, LoadMode mode) {
        const Key key(format, id);
        if (mode == LoadMode::Normal) return modes_.erase(key) > 0;
        const auto [it, added] = modes_.emplace(key, mode);
        if (added) return true;
        if (it->second == mode) return false;
        it->second = mode;
        return true;
    }

    bool empty() const { return modes_.empty(); }
    const std::map<Key, LoadMode>& all() const { return modes_; }

    // The error a disabled plug-in answers with wherever it would be found or
    // made (findPlugin, a pool's get): one code, one wording.
    static Error disabled(const host::PluginInfo& info) {
        return Error("plugin.disabled", "plug-in \"" + info.name + "\" is disabled for this run",
                     {}, "plugin.override {plugin, mode: normal} loads it again");
    }

  private:
    std::map<Key, LoadMode> modes_;
};

}  // namespace supple::engine
