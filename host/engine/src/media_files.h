// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Copies of audio files into a project's or a template's own media folder
// (imports, Save as into another folder, project templates), so a project
// never depends on a file somewhere else. Main/I-O threads only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
// `created`, if given, gets each file this copy created (not those it kept).
Status copyFolder(const std::filesystem::path& from, const std::filesystem::path& to,
                  std::vector<std::filesystem::path>* created = nullptr);

// The one test of where a path the engine reads or writes on a file's say
// leads (ADR-0076, ADR-0078): inside `root` when the deepest part of `path`
// that exists resolves, every link on the way followed (canonical, so its
// ancestors too), inside the resolved `root`, and the part that does not
// exist yet names no ".." (where that leads is unknown until it is made).
// `missing` (optional) receives the parts that do not exist, deepest first.
bool resolvedInside(const std::filesystem::path& root, const std::filesystem::path& path,
                    std::vector<std::filesystem::path>* missing = nullptr);

// Every file crash recovery opens goes through these (ADR-0078): autosaves,
// the copies kept for the first save, recording sidecars and WAVs, MIDI
// journals. A file that does not resolve inside `root` (the folder recovery
// may use) is not opened: recover.outside. An empty `root` confines nothing.
// Only a regular file is read (a link to one is followed): a named pipe, a
// device or a folder is refused without being waited on, recover.not_file.
Status recoveryFileInside(const std::filesystem::path& root, const std::filesystem::path& file);
Result<std::vector<std::uint8_t>> readRecoveryFile(const std::filesystem::path& root,
                                                   const std::filesystem::path& file,
                                                   std::size_t limit);
Result<std::string> readRecoveryText(const std::filesystem::path& root,
                                     const std::filesystem::path& file, std::size_t limit);

}  // namespace supple::engine::detail
