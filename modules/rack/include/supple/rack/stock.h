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

// A factory rack patch (modules/patches/<file>.json): a rack preset that
// ships with the app (docs/specs/rack.md §7.1), its text as written.
struct FactoryPatch {
    std::string_view file;  // its file name without .json
    std::string_view text;
};

// Every factory rack patch, by file name.
const std::vector<FactoryPatch>& factoryPatches();

}  // namespace supple::rack
