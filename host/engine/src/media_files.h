// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Copies of audio files into a project's or a template's own media folder
// (imports, Save as into another folder, project templates), so a project
// never depends on a file somewhere else. Main/I-O threads only.
#pragma once

#include <filesystem>
#include <string>

#include "supple/base/result.h"

namespace supple::engine::detail {

// Copies `file` into `dir` under its own name and returns the name used. A
// file of that name with the same bytes is reused; one with other bytes
// leaves it alone and the copy gets "-2", "-3"… before the extension. The
// copy is a clone where the filesystem supports one, and appears whole.
// `created` (optional) is set when a new file was written.
Result<std::string> copyIntoFolder(const std::filesystem::path& file,
                                   const std::filesystem::path& dir, bool* created = nullptr);

// Every file under `from` to the same place under `to` (an absent `from`
// copies nothing). A file already there with the same bytes is kept; one
// with other bytes is an error, and nothing is overwritten. A destination
// strictly within `from` is excluded from traversal, including resolved symlink
// aliases. An ancestor destination receives the source files normally.
Status copyFolder(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace supple::engine::detail
