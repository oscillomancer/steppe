// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Contextual mouse modifiers (gap log #37): what a press or the wheel does in
// each context (the arrange view's, the mixer's, the MIDI editor's) for each
// modifier combination. The defaults are the
// built-in gestures; <config>/<slug>/mouse.json (supple.mouse/1.0) overrides
// them per context and combination:
//   {"schema": "supple.mouse/1.0",
//    "contexts": {"item.drag": {"alt": "copy", "ctrl": "slip"}}}
// A combination a context does not name does what its unmodified press does.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"

namespace supple::views {

class MouseMap {
  public:
    // A context: where the press lands, and the behaviours it can have.
    struct Context {
        std::string_view id, label;
        std::vector<std::string_view> behaviours;  // the first is the unmodified default
    };
    static std::span<const Context> contexts();
    static const Context* context(std::string_view id);
    // The modifier combinations a map can name, as written in the file
    // ("" is no modifier; "ctrl+shift", "alt+shift", ...).
    static std::span<const unsigned> combos();
    static std::string comboName(unsigned mods);
    static std::optional<unsigned> comboFrom(std::string_view name);

    MouseMap();  // the defaults
    // What a press with `mods` (ui::kShift | kCtrl | kAlt; others ignored)
    // does in `context`.
    std::string_view behaviour(std::string_view context, unsigned mods) const;
    // Sets (or with an empty behaviour, clears to the unmodified default) a
    // combination. Unknown contexts or behaviours are refused.
    Status set(std::string_view context, unsigned mods, std::string_view behaviour);
    // Whether the combination is bound explicitly (defaults count).
    bool bound(std::string_view context, unsigned mods) const;

    json::Value toJson() const;  // only what differs from the defaults
    static Result<MouseMap> fromJson(const json::Value& doc);
    static Result<MouseMap> load(const std::filesystem::path& file);  // defaults when absent
    Status save(const std::filesystem::path& file) const;
    static std::filesystem::path defaultFile(std::string_view slug);

    bool operator==(const MouseMap&) const = default;

  private:
    std::map<std::string, std::map<unsigned, std::string>, std::less<>> map_;
};

}  // namespace supple::views
