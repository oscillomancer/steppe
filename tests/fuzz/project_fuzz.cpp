// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/model/project_file.h>
#include <supple/model/schemas.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// The path project.open takes: the document, then its branches (ADR-0076).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > (1U << 20)) return 0;
    auto opened = supple::model::parseProjectFile(
        std::string_view(reinterpret_cast<const char*>(data), size));
    if (!opened) return 0;
    for (const auto& branch : opened->store.branches())
        if (!supple::model::checkInvariants(*opened->store.head(branch))) __builtin_trap();
    const std::string text = supple::model::serializeProjectFile(opened->store);
    auto again = supple::model::parseProjectFile(text);
    if (!again || again->branchesDropped ||
        supple::model::serializeProjectFile(again->store) != text)
        __builtin_trap();
    return 0;
}
