// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "supple/base/result.h"
namespace supple::host::platform {
// All paths crossing plugin ABIs are UTF-8.
std::filesystem::path pathFromText(std::string_view text);
std::string pathText(const std::filesystem::path& path);
std::filesystem::path executablePath();
// The executable image a plug-in path holds: the path itself, except a macOS bundle's
// Contents/MacOS executable (empty when it has none).
std::filesystem::path bundleExecutable(const std::filesystem::path& path);
Result<void*> openLibrary(const std::filesystem::path& path);
void* symbol(void* library, const char* name);
void closeLibrary(void* library);
std::vector<std::filesystem::path> pluginPaths(bool vst3);
bool executable(const std::filesystem::path& path);
std::string_view scanHelperName();
}  // namespace supple::host::platform
