// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: the module library (ADR-0062, rack.md §3): <dir>/<name>/<name>.cpp,
// each a module type compiled into the catalog racks use. The module.write,
// module.edit, module.check, module.get and module.fork commands work on it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"
#include "supple/rack/module_type.h"

namespace supple::engine::detail {

// The longest module source the library reads.
inline constexpr std::size_t kMaxModuleBytes = std::size_t(1) << 20;

struct ModuleDiagnostic {
    std::string severity, code, message, hint;  // severity: error or warning
    std::int64_t line = 0, column = 0;
};

struct CompiledModule {
    std::shared_ptr<const rack::ModuleType> type;  // null: it has errors
    std::vector<ModuleDiagnostic> diagnostics;
};

// Compiles a module's source (a struct deriving from Module) into a type of
// `origin`. A script that is not a module, or a description with problems
// (keys, limits, its id), gives an error diagnostic and no type.
CompiledModule compileModule(std::string_view source, rack::Origin origin,
                             const std::filesystem::path& path = {});

// A library module's file: <dir>/<name>/<name>.cpp.
std::filesystem::path modulePath(const std::filesystem::path& dir, std::string_view name);
// Its name, from a type's path (empty when it is not a library file).
std::string moduleName(const rack::ModuleType& type);
// A library name: 1-64 lower-case letters, digits, '_' or '-', from a letter.
Status validModuleName(std::string_view name, const std::string& path);

// `base` with the library's modules in place of its library types: each
// that compiles, by name; ids a host or stock type has, or an earlier name
// took, are left out. Problems are appended to `skipped` ("name: why").
std::shared_ptr<const rack::Catalog> loadModuleLibrary(
    const std::shared_ptr<const rack::Catalog>& base, const std::filesystem::path& dir,
    std::vector<std::string>* skipped = nullptr);

json::Value diagnosticsJson(const std::vector<ModuleDiagnostic>& diagnostics);

// A small module for examples and fixtures (library name "wobble").
inline constexpr const char* kExampleModule = R"(#include <supple/script.h>

// Tremolo: a sine LFO moves the input's level.
struct Wobble : Module {
    static constexpr ModuleInfo info = {.name = "Wobble",
                                        .id = "com.example.wobble",
                                        .vendor = "Example",
                                        .description = "A sine LFO moves the input's level.",
                                        .tags = {"amplifier", "modulation"}};

    Input in = audioIn("In").help("What it moves.");
    Param rate = frequency("Rate", 0.1, 20, 4).help("The LFO's speed.");
    Param depth = percent("Depth", 50).help("How far the level dips.");
    Output out = audioOut("Out").help("The input, wobbling.");

    double phase = 0;

    void process(int frames) {
        for (int i = 0; i < frames; ++i) {
            const double gain = 1.0 - depth.at(i) / 100.0 * (0.5 + 0.5 * sin(2 * pi * phase));
            for (int c = 0; c < channels(); ++c) out[c][i] = in[c][i] * gain;
            phase += rate.at(i) / sampleRate();
            phase -= floor(phase);
        }
    }
};
)";

}  // namespace supple::engine::detail
