// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The runtime as the compiler sees it: every function, class, method and
// field a script can name, with its C++ signature, where it may run, what it
// changes, and how code reaches it (a thunk into the runtime library, or an
// intrinsic the code generator expands inline exactly as script.h defines
// it). Thunk signatures are deduced from the thunks themselves, and the
// thunks call the runtime's C++ API, so a mismatch with script.h does not
// compile.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast.h"

namespace supple::scriptc {

// How an argument or result crosses into a thunk.
enum class Abi : std::uint8_t { Void, I32, F32, F64, Ptr, I64 };

// Operations the code generator expands inline. Each replicates script.h's
// definition step for step.
enum class Intrinsic : std::uint8_t {
    None,
    Abs,
    Min,
    Max,
    Clamp,
    Lerp,
    Sign,
    IsFinite,
    // Param
    ParamValue,  // operator double, value()
    ParamAt,
    ParamIndex,
    ParamOn,
    ParamChanged,
    ParamSet,
    // Plugin base: the instance's fields
    SampleRate,
    MaxBlockSize,
    Channels,
    Offline,
    ModuleTransport,
    // Containers
    ArraySize,
    ArrayFill,
    BufferSize,
    BufferFill,
    // Block
    BlockSidechain,
    BlockOutput,
    // Shared<T>
    SharedGet,
    SharedSet,
};

// What a call may change, for the evaluation-order rule.
enum class Writes : std::uint8_t { Nothing, Self };

struct BuiltinFunction {
    std::string name;
    std::vector<QualType> params;
    QualType result;
    Intrinsic intrinsic = Intrinsic::None;
    void* thunk = nullptr;
    std::vector<Abi> abi;  // result first, then the parameters
    std::uint8_t contexts = kInit | kAudio | kUi;
};

struct BuiltinClass;

struct BuiltinMethod {
    const BuiltinClass* owner = nullptr;
    std::string name;
    std::vector<QualType> params;
    QualType result;
    bool isConst = false;  // callable on a const object
    Writes writes = Writes::Self;
    Intrinsic intrinsic = Intrinsic::None;
    void* thunk = nullptr;
    std::vector<Abi> abi;  // result, self, parameters
    std::uint8_t contexts = kInit | kAudio | kUi;
    std::string doc;          // for errors: "prepare(int maxDelaySamples)"
    bool module = false;      // a base method only a module has
    bool instrument = false;  // a plugin base method only an instrument has
};

struct BuiltinField {
    std::string name;
    const Type* type = nullptr;
    int offset = 0;
};

struct BuiltinClass {
    std::string name;
    Type* type = nullptr;
    // Owns memory: only a member (never a local, never copied), and its
    // allocating methods only in prepare().
    bool resource = false;
    bool triviallyCopyable = false;  // may reset an invalid Array access from a prototype
    // Scripts may declare one (a local or a member). Block, Param and the
    // channel views are the runtime's to make.
    bool declarable = true;
    void (*construct)(void*) = nullptr;
    void (*destroy)(void*) = nullptr;
    void (*copy)(void*, const void*) = nullptr;
    std::vector<BuiltinMethod> methods;
    std::vector<BuiltinField> fields;
    std::map<std::string, const Type*> nestedTypes;  // Oscillator::Shape
    // Shared<T>: T. A member of the plugin struct the audio code sends the
    // editor (with SharedArray<T, N>, an Array type marked shared).
    const Type* sharedValue = nullptr;
};

// A runtime enum: its type and enumerators.
struct BuiltinEnum {
    Type* type = nullptr;
    std::vector<std::pair<std::string, int>> values;
};

class Builtins {
  public:
    static const Builtins& get();

    const Type* voidType() const { return void_; }
    const Type* boolType() const { return bool_; }
    const Type* intType() const { return int_; }
    const Type* floatType() const { return float_; }
    const Type* doubleType() const { return double_; }
    const Type* stringType() const { return string_; }
    const Type* initListType() const { return initList_; }
    const Type* nullType() const { return null_; }

    const BuiltinClass* cls(const std::string& name) const;
    const BuiltinEnum* enumOf(const Type* type) const;
    // A runtime enum declared at the top level (VoiceMode), or null.
    const Type* enumNamed(const std::string& name) const;
    std::vector<const BuiltinFunction*> functions(const std::string& name) const;
    // Every name the runtime declares, which a script may not declare again.
    bool reserved(const std::string& name) const;

    const BuiltinClass* param() const { return param_; }
    const BuiltinClass* block() const { return block_; }
    const BuiltinClass* channel() const { return channel_; }
    const BuiltinClass* inputChannel() const { return inputChannel_; }
    const BuiltinClass* transport() const { return transport_; }
    const BuiltinClass* paramSpec() const { return paramSpec_; }
    const BuiltinClass* info() const { return info_; }
    const BuiltinClass* pluginBase() const { return plugin_; }
    const BuiltinClass* audioFile() const { return audioFile_; }
    const BuiltinClass* convolver() const { return convolver_; }
    const BuiltinClass* canvas() const { return canvas_; }
    const BuiltinClass* editor() const { return editor_; }
    const BuiltinClass* pointer() const { return pointer_; }
    // An AudioFile or Convolver: a file member, declared from a FileSpec.
    bool isFile(const Type* t) const;
    // A Source: a modulation source member, declared from a SourceSpec.
    const BuiltinClass* source() const { return source_; }
    bool isSource(const Type* t) const {
        return t && t->kind == TypeKind::Object && t->builtin == source_;
    }
    bool isPort(const Type* t) const {
        return t && t->kind == TypeKind::Object &&
               (t->builtin == cls("Input") || t->builtin == cls("Output"));
    }
    // Shared<T> for T (double, float, int, bool), or null.
    const BuiltinClass* shared(const Type* value) const;
    // A Shared<T> or a SharedArray<T, N>: state the audio code sends the editor.
    static bool isShared(const Type* t);
    // A runtime group of named int constants (Theme::accent), or null.
    const std::map<std::string, int>* constants(const std::string& group) const;

    // The plugin base's size: where a plugin struct's own members start.
    int pluginBaseSize() const;

  private:
    Builtins();
    Builtins(const Builtins&) = delete;

    Type* scalar(TypeKind kind, const char* name, int size);
    BuiltinClass* addClass(const std::string& name, int size, int align);
    const Type* addEnum(const std::string& name, std::vector<std::pair<std::string, int>> values);
    void addEditor();
    void addFunctions();
    void addClasses();

    std::vector<std::unique_ptr<Type>> types_;
    std::vector<std::unique_ptr<BuiltinClass>> classes_;
    std::vector<std::unique_ptr<BuiltinFunction>> functions_;
    std::vector<std::unique_ptr<BuiltinEnum>> enums_;
    std::map<std::string, std::vector<const BuiltinFunction*>> byName_;
    std::map<std::string, const BuiltinClass*> classByName_;
    std::map<std::string, const Type*> enumByName_;
    std::map<std::string, std::map<std::string, int>> constants_;
    std::map<const Type*, const BuiltinClass*> sharedByValue_;

    Type *void_, *bool_, *int_, *float_, *double_, *string_, *initList_, *null_;
    const BuiltinClass *param_ = nullptr, *block_ = nullptr, *channel_ = nullptr,
                       *inputChannel_ = nullptr, *transport_ = nullptr, *paramSpec_ = nullptr,
                       *info_ = nullptr, *plugin_ = nullptr, *audioFile_ = nullptr,
                       *convolver_ = nullptr, *canvas_ = nullptr, *editor_ = nullptr,
                       *pointer_ = nullptr, *source_ = nullptr;

    friend struct Registrar;
};

}  // namespace supple::scriptc
