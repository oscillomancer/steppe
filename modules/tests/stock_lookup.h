// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// A stock module's code by its file name, for tests.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "supple/rack/stock.h"

namespace supple::rack::test {

inline const script::detail::ModuleCode& stock(std::string_view name) {
    for (const StockModule* m : stockModules())
        if (m->name == name) return *m->code;
    throw std::out_of_range("no stock module " + std::string(name));
}

}  // namespace supple::rack::test
