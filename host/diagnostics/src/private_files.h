// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include "supple/base/result.h"
namespace supple::diagnostics::detail {
Status privatePublish(const std::filesystem::path& path, const std::string& text);
Result<std::string> privateRead(const std::filesystem::path& path);
// Removal is idempotent for absent files and for files another remover has already
// condemned; ownership/link and other I/O errors fail.
bool privateRemove(const std::filesystem::path& path);
// Classifies a failed open or delete of the file being removed: true when the file is
// already gone or on its way out, so removal counts as done. Windows reports a file whose
// deletion is pending (condemned by another handle that is still open) as
// ERROR_ACCESS_DENIED, the same Win32 code as a real permission refusal, so that code is
// accepted only together with the NT status STATUS_DELETE_PENDING. `ntStatus` is the
// status of the same failed call; without it ERROR_ACCESS_DENIED stays an error.
// Platform independent so every platform tests it; only Windows calls it.
bool removalTargetGone(std::uint32_t win32Error, std::optional<std::uint32_t> ntStatus);
#if defined(_WIN32)
bool privateDirectoryAcl(const std::filesystem::path& path);
#endif
}  // namespace supple::diagnostics::detail
