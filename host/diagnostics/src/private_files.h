// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include "supple/base/result.h"
namespace supple::diagnostics::detail {
Status privatePublish(const std::filesystem::path& path, const std::string& text);
Result<std::string> privateRead(const std::filesystem::path& path);
bool privateRemove(const std::filesystem::path& path);
#if defined(_WIN32)
bool privateDirectoryAcl(const std::filesystem::path& path);
#endif
}  // namespace supple::diagnostics::detail
