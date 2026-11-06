// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What semantic analysis learns about a script's plugin: its identity, its
// parameter table, its hooks and the layout of its struct. Code generation
// reads it, and the Program it makes describes the plugin to the glue.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast.h"
#include "supple/script_module.h"
#include "supple/script_program.hpp"

namespace supple::scriptc {

struct BuiltinClass;

// One runtime object inside the plugin struct (directly, or within arrays
// and structs): constructed, copied and destroyed through its thunks.
struct ObjectSlot {
    int offset = 0;
    const BuiltinClass* cls = nullptr;
};

// A member's starting value, written when an instance is constructed.
struct InitialValue {
    int offset = 0;
    TypeKind kind = TypeKind::Double;  // Bool, Int, Float, Double, Enum
    double value = 0.0;
};

// A source member's starting state: what kind of values it keeps.
struct SourceSlot {
    int offset = 0;
    bool perVoice = false, bipolar = true;
};

// A parameter member's own starting state.
struct ParamSlot {
    int offset = 0;
    int index = 0;
    double def = 0.0, min = 0.0;
    bool offAtMinimum = false;
};

struct MemberSpare {
    int offset = 0, first = 0;
    const BuiltinClass* cls = nullptr;
};

struct PluginModel {
    StructDecl* decl = nullptr;
    PluginKind kind = PluginKind::None;
    InfoModel info;
    ModuleInfoModel moduleInfo;
    std::vector<PortModel> ports;
    std::vector<ParamModel> params;
    std::vector<FieldDecl*> paramFields;  // params[i]'s member
    std::vector<FileModel> files;
    std::vector<FactoryPatchModel> factoryPatches;
    std::vector<FieldDecl*> fileFields;  // files[i]'s member
    std::vector<SourceModel> sources;
    std::vector<FieldDecl*> sourceFields;  // sources[i]'s member
    FunctionDecl* prepare = nullptr;
    FunctionDecl* update = nullptr;
    FunctionDecl* process = nullptr;
    // An instrument's voice hooks; startVoice takes (int v) or (int v, bool retrigger).
    FunctionDecl* startVoice = nullptr;
    FunctionDecl* startVoiceRetrigger = nullptr;
    FunctionDecl* releaseVoice = nullptr;
    FunctionDecl* stopVoice = nullptr;
    // The face: editor(Editor&) or panel(Panel&) for modules, then draw/pointer.
    FunctionDecl* editor = nullptr;
    FunctionDecl* draw = nullptr;
    FunctionDecl* pointer = nullptr;
    std::vector<SharedModel> shared;  // Shared and SharedArray members, in order
    int size = 0, align = 8;
    std::vector<ObjectSlot> objects;
    std::vector<InitialValue> initial;
    std::vector<MemberSpare> memberSpares;
    std::vector<ParamSlot> paramSlots;
    std::vector<SourceSlot> sourceSlots;
};

// The result of semantic analysis.
struct Analysis {
    PluginModel plugin;
    std::vector<FunctionDecl*> functions;  // every function with a body, methods included
    std::vector<VarDecl*> globals;
};

// Checks the parsed unit and fills in the tree and the model. Errors go to
// `diags`; the analysis is complete only when there are none.
void analyse(Unit& unit, Analysis& out, Diagnostics& diags);

// The layout helpers code generation shares with sema: every runtime object
// inside a value of `type` placed at `base`.
void collectObjects(const Type* type, int base, std::vector<ObjectSlot>& out);

}  // namespace supple::scriptc
