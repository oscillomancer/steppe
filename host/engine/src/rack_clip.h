// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include "supple/model/session.h"

namespace supple::engine {

// Main-thread clipboard, owned by one EngineApp. Effect entries carry patches
// or inline state bytes, so no project blob or live instance has to survive.
struct RackClip {
    struct Module {
        model::Id original;
        model::RackModule module;
    };
    std::vector<Module> modules;
    std::vector<json::Value> effects;
    std::vector<model::Cable> cables;
};

}  // namespace supple::engine
