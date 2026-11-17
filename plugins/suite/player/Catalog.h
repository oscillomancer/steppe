// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Where the player finds scripts, and how it makes plugins of them
// (Catalog.cpp, Layer.cpp).
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "supple/script_compiler.hpp"
#include "supple/suite/script_definition.h"

namespace supple::player {

using suite::ScriptDefinition;

// The longest script read: a page of C++ is a few kilobytes.
inline constexpr size_t kMaxScriptBytes = 1u << 20;

// The folders SUPPLE_SCRIPT_LIBRARY names, in order (':' between them; ';'
// on Windows), or none.
std::vector<std::filesystem::path> libraryFolders();
// <folder>/<name>/<name>.cpp for every <name>, sorted.
std::vector<std::filesystem::path> libraryScripts(const std::filesystem::path& folder);
// A regular script file's whole text; false for other file types, I/O errors
// or a file longer than kMaxScriptBytes.
bool readScript(const std::filesystem::path& file, std::string& text);
// A script compiled into a plugin; null with the reasons in `problems`. In a
// Debug build, SUPPLE_SCRIPT_INTERPRET in the environment selects MIR's
// interpreter instead of machine code: a diagnostic that allocates on the
// audio thread, so release builds ignore it (ADR-0032).
// `file` is where it came from, which the layer follows; empty for an
// embedded script.
std::shared_ptr<const ScriptDefinition> compileScript(const std::filesystem::path& file,
                                                      const std::string& source,
                                                      std::string* problems,
                                                      scriptc::Message* firstError = nullptr);

// The binary this code is in (inside Contents/MacOS on macOS), or empty.
std::filesystem::path ownModule();
// The script an export carries in its module trailer or macOS bundle resource
// (supple/script_export.hpp), or nothing: a player, not an export.
struct EmbeddedScript {
    std::string name, source;
};
std::optional<EmbeddedScript> embeddedScript(const std::filesystem::path& module);

}  // namespace supple::player
