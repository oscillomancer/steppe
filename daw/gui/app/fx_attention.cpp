// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "fx_attention.h"

#include <utility>

namespace supple::gui {

FxAttention::Mark FxAttention::markOf(const model::Id& fx,
                                      const model::FxInstance& instance) const {
    if (app_.loadModeOf(instance.plugin) == engine::LoadMode::Disable) return Mark::Disabled;
    if (failed_.contains(fx.str())) return Mark::Failed;
    return Mark::None;
}

bool FxAttention::chainNeeds(const model::Session& session, const model::Track& track) const {
    for (const auto* chain : {&track.chain, &track.inputChain})
        for (const model::Id& id : *chain)
            if (const model::FxInstance* f = session.fx.find(id); f && markOf(id, *f) != Mark::None)
                return true;
    return false;
}

bool FxAttention::apply(const engine::EngineStatus& status) {
    std::set<std::string> failed(status.failedFx.begin(), status.failedFx.end());
    if (failed == failed_) return false;
    failed_ = std::move(failed);
    dirty_ = true;  // poll() tells its caller, whoever applied it
    return true;
}

bool FxAttention::poll() {
    if (const auto version = app_.loadOverridesVersion(); version != overrides_) {
        overrides_ = version;
        dirty_ = true;
    }
    const auto at = now();
    if (!polledOnce_ || at - polled_ >= std::chrono::milliseconds(500)) {
        polledOnce_ = true;
        polled_ = at;
        (void)apply(app_.engineStatus());
    }
    return std::exchange(dirty_, false);
}

std::string FxAttention::signature() const {
    std::string out = "F";
    for (const auto& id : failed_) out += id + ",";
    return out;
}

}  // namespace supple::gui
