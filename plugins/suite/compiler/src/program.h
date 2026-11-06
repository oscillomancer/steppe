// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a Program holds (internal): its description, its instances' layout,
// its hooks, and the MIR context that owns its code.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "model.h"
#include "supple/script_program.hpp"

namespace supple::scriptc {

struct ProgramData {
    InfoModel info;
    ModuleInfoModel moduleInfo;
    std::vector<PortModel> ports;
    std::string typeName;
    PluginKind kind = PluginKind::None;
    std::vector<ParamModel> params;
    std::vector<FileModel> files;
    std::vector<SourceModel> sources;
    std::vector<FactoryPatchModel> factoryPatches;
    std::vector<SharedModel> shared;
    int size = 0, align = 8;
    std::vector<ObjectSlot> objects;
    std::vector<InitialValue> initial;
    std::vector<MemberSpare> memberSpares;
    std::vector<ParamSlot> paramSlots;
    std::vector<SourceSlot> sourceSlots;
    Program::Hook prepare = nullptr, update = nullptr;
    Program::ProcessHook process = nullptr;
    script::detail::ModuleCode::ProcessHook moduleProcess = nullptr;
    Program::VoiceHook startVoice = nullptr, releaseVoice = nullptr, stopVoice = nullptr;
    Program::StartHook startVoiceRetrigger = nullptr;
    Program::UiHook editor = nullptr, draw = nullptr, pointer = nullptr;
    // String literals the code points at.
    std::vector<std::unique_ptr<std::string>> strings;
    void* mir = nullptr;  // MIR_context_t: owns the code
    bool generator = false;
    bool interpreted = false;
    std::string listing;
    std::vector<std::unique_ptr<unsigned char[]>> prototypes;  // images of locals' starting state
};

struct CompileOptions {
    bool interpret = false;
    int optimize = 2;
    bool listing = false;
};

// Generates code for an analysed unit. A failure inside MIR (a compiler bug)
// becomes a diagnostic.
std::shared_ptr<const Program> generate(Unit& unit, Analysis& analysis,
                                        const CompileOptions& options, Diagnostics& diags);

}  // namespace supple::scriptc
