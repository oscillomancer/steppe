// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The stock modules (ADR-0062, modules/stock): each script compiled into the
// DAW as C++, and its text, which module.get shows and module.fork copies.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "supple/script_module.h"

namespace supple::rack {

struct StockModule {
    std::string_view name;  // its file's: modules/stock/<name>.cpp
    std::shared_ptr<const script::detail::ModuleCode> code;
    std::string_view source;
};

// Every stock module, by name.
const std::vector<const StockModule*>& stockModules();

}  // namespace supple::rack
