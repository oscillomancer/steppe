// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <supple/base/result.h>
#include <supple/json/value.h>
namespace supple::json {
struct ParseOptions {
    std::size_t maxDepth = 256;
    std::size_t maxBytes = 256u << 20;
    std::size_t maxValues = 0;  // containers and primitives; zero keeps trusted parsing unlimited
};
Result<Value> parse(std::string_view text, const ParseOptions& options = {});
}  // namespace supple::json
