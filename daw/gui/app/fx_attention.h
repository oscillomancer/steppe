// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Which plug-ins of a project need the user: the ones this run has disabled (`plugin.override`
// mode `disable`) and the ones that failed (`engine.status` failed_fx, a separate process that
// closed or stopped answering). The mixer's slots and the "fx" mark of both the mixer and the
// track list read it, so there is one rule for what a mark means (docs/specs/gui.md, "Disabled
// plug-ins are marked").
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "supple/engine/app.h"
#include "supple/model/session.h"

namespace supple::gui {

class FxAttention {
  public:
    explicit FxAttention(engine::EngineApp& app) : app_(app) {}

    // What a plug-in's slot is at: disabled wins over failed, since a disabled plug-in is not
    // loaded and cannot have failed since.
    enum class Mark { None, Disabled, Failed };
    Mark markOf(const model::Id& fx, const model::FxInstance& instance) const;

    // The chain (or the input chain) of `track` holds a plug-in that is disabled or failed: the
    // one rule for the warning colour of "fx", in the mixer and in the track list.
    bool chainNeeds(const model::Session& session, const model::Track& track) const;

    // Reads the engine's status at most twice a second (`now` is replaceable by tests) and the
    // overrides' version; true when what it shows has changed since the last call.
    bool poll();
    // The same from a status the caller has (tests): true when the failed set changed.
    bool apply(const engine::EngineStatus& status);

    // What a mixer folds into its structure so a change rebuilds the strips.
    std::string signature() const;

    std::function<std::chrono::steady_clock::time_point()> now = [] {
        return std::chrono::steady_clock::now();
    };

  private:
    engine::EngineApp& app_;
    std::set<std::string> failed_;
    std::uint64_t overrides_ = ~std::uint64_t{0};
    std::chrono::steady_clock::time_point polled_{};
    bool polledOnce_ = false;
    bool dirty_ = false;  // changed since poll() last said so
};

}  // namespace supple::gui
