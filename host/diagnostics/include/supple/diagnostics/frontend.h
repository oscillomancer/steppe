// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <string_view>
namespace supple::diagnostics {
enum class Role : unsigned { Gui = 1, Engine = 2, Fixture = 3 };
using Main = int (*)(int, char**);
struct Options {
    // Test/embedder override; ordinary frontends use localData/slug/diagnostics.
    std::filesystem::path directory;
};
// Cold entry, before SDL, model, plugins or device threads. Never audio.
int runFrontend(int argc, char** argv, Role role, Main main, Options options = {});
}  // namespace supple::diagnostics
