// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A module made from a script (ADR-0062), as a rack runs it, however it was
// built: by the script compiler (scriptc::Program's module code) or by a C++
// compiler (NativeModuleCode<T> below, the stock modules). Its description
// -- identity, parameters, ports, shared state, presets -- and what an
// instance needs to be made, copied, destroyed and run, and its face laid
// out and drawn. Scripts never see it.
//
//   namespace supple::script::user {
//   #include "stock/fold.cpp"
//   }
//   using Fold = supple::script::user::Fold;
//   const NativeModuleCode<Fold> code({{&Fold::folds, "folds"}},
//                                     {{&Fold::in, "in"}, {&Fold::out, "out"}});
//
// As with NativeCode, what C++ cannot see is a member's name, which a key is
// made from: the keys come with the code, by member (what the script
// compiler worked out; for the stock modules, what modules/cmake/
// stock_module.cmake reads from the declarations). A
// member missing from the lists takes the key withKey() gives, or else its
// name's in snake_case.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "supple/script.h"
#include "supple/script_code.h"
#include "supple/script_native.h"
#include "supple/script_runtime.h"

namespace supple::script::detail {

// A port as the module declares it (script.h's PortSpec, evaluated).
struct PortModel {
    enum class Signal { Audio, Pitch, Gate, Cv, Unipolar, Phase };
    std::string member;  // the member's name (empty when compiled as C++)
    std::string key;     // its key (the member's name in snake_case unless given)
    std::string name, description, group;
    Signal signal = Signal::Audio;
    bool output = false;
    double normal = 0.0;  // an input's value while nothing is connected
    int index = 0;        // among the module's inputs, or among its outputs
    int offset = 0;       // of the Input or Output member in the module struct
    bool operator==(const PortModel&) const = default;
};
static_assert(static_cast<int>(SignalKind::Phase) == static_cast<int>(PortModel::Signal::Phase));

struct ModuleInfoModel {
    std::string name, id, vendor, version = "1.0.0", description, url;
    std::vector<std::string> tags;
    int width = 0, height = 0;  // design units; 0: the automatic layout's
    bool operator==(const ModuleInfoModel&) const = default;
};

// Immutable once made; any number of instances share it, on any thread.
class ModuleCode {
  public:
    virtual ~ModuleCode() = default;

    virtual const ModuleInfoModel& info() const = 0;
    // In declaration order.
    virtual const std::vector<ParamModel>& params() const = 0;
    // Inputs and outputs in declaration order (each with its index among
    // its own direction).
    virtual const std::vector<PortModel>& ports() const = 0;
    virtual const std::vector<SharedModel>& shared() const = 0;
    virtual const std::vector<FactoryPatchModel>& factoryPatches() const = 0;

    // An instance: the module struct, constructed (each Param at its
    // default). Main thread: it allocates.
    virtual void* create() const = 0;
    virtual void destroy(void* self) const = 0;
    // The whole struct, as its copy assignment does it (see Code::copy).
    virtual void copy(void* dst, const void* src) const = 0;

    // The hooks (ctx: the instance's runtime record, a script::detail::
    // Instance; self: the struct). Null when the module has none. The rack
    // sets the current instance (CurrentScope) around each call.
    using Hook = void (*)(void* ctx, void* self);
    using ProcessHook = void (*)(void* ctx, void* self, std::int64_t frames);
    // panel(Panel&), draw(Canvas&), pointer(const Pointer&): the object a
    // script::Panel, Canvas or Pointer.
    using UiHook = void (*)(void* ctx, void* self, void* object);
    virtual Hook prepare() const = 0;
    virtual Hook update() const = 0;
    virtual ProcessHook process() const = 0;
    virtual UiHook panel() const = 0;
    virtual UiHook draw() const = 0;
    virtual UiHook pointer() const = 0;
};

// A module compiled as C++.
template <class T>
class NativeModuleCode final : public ModuleCode {
  public:
    using Key = std::pair<Param T::*, const char*>;
    // A port's key: {&Fold::in, "in"} (an Input or an Output member).
    struct PortKey {
        template <class M>
        PortKey(M T::* m, const char* k)
            : of([m](const T& t) -> const void* { return &(t.*m); }), key(k) {}
        std::function<const void*(const T&)> of;
        const char* key;
    };

    explicit NativeModuleCode(std::vector<Key> keys = {}, std::vector<PortKey> portKeys = {}) {
        static_assert(std::is_base_of_v<Module, T>, "a module struct derives from Module");
        describe(T::info);
        native::Collector collector;
        setRegistrar(&collector);
        auto probe = std::make_unique<T>();
        setRegistrar(nullptr);
        const auto* base = reinterpret_cast<const unsigned char*>(probe.get());
        for (size_t i = 0; i < collector.params.size(); ++i) {
            ParamModel& m = collector.models[i];
            const Param* param = collector.params[i];
            m.offset = static_cast<int>(reinterpret_cast<const unsigned char*>(param) - base);
            for (const Key& k : keys)
                if (&((*probe).*(k.first)) == param && m.key.empty()) m.key = k.second;
            if (m.key.empty()) m.key = native::snakeCase(m.name);
            if (m.id == 0) m.id = keyId(m.key);
            params_.push_back(std::move(m));
        }
        int inputs = 0, outputs = 0;
        for (size_t i = 0; i < collector.ports.size(); ++i) {
            const PortSpec& spec = collector.portSpecs[i];
            const void* member = collector.ports[i];
            PortModel m;
            m.key = spec.key ? spec.key : "";
            m.name = spec.name ? spec.name : "";
            m.description = spec.description ? spec.description : "";
            m.group = spec.group ? spec.group : "";
            m.signal = static_cast<PortModel::Signal>(spec.signal);
            m.output = spec.output;
            m.normal = spec.normalValue;
            m.index = spec.output ? outputs++ : inputs++;
            m.offset = static_cast<int>(static_cast<const unsigned char*>(member) - base);
            for (const PortKey& k : portKeys)
                if (k.of(*probe) == member && m.key.empty()) m.key = k.key;
            if (m.key.empty()) m.key = native::snakeCase(m.name);
            ports_.push_back(std::move(m));
        }
        for (const auto& [member, bytes] : collector.shared)
            shared_.push_back(SharedModel{
                {}, static_cast<int>(static_cast<const unsigned char*>(member) - base), bytes});
        if constexpr (requires(T& t, Patches& p) { t.factoryPatches(p); }) {
            native::FactoryCollector builder(params_, collector.params, factoryPatches_);
            Patches patches(builder);
            probe->factoryPatches(patches);
        }
    }

    const ModuleInfoModel& info() const override { return info_; }
    const std::vector<ParamModel>& params() const override { return params_; }
    const std::vector<PortModel>& ports() const override { return ports_; }
    const std::vector<SharedModel>& shared() const override { return shared_; }
    const std::vector<FactoryPatchModel>& factoryPatches() const override {
        return factoryPatches_;
    }

    void* create() const override { return new T(); }
    void destroy(void* self) const override { delete static_cast<T*>(self); }
    void copy(void* dst, const void* src) const override {
        *static_cast<T*>(dst) = *static_cast<const T*>(src);
    }

    Hook prepare() const override {
        if constexpr (requires(T& t) { t.prepare(); })
            return [](void*, void* self) { static_cast<T*>(self)->prepare(); };
        return nullptr;
    }
    Hook update() const override {
        if constexpr (requires(T& t) { t.update(); })
            return [](void*, void* self) { static_cast<T*>(self)->update(); };
        return nullptr;
    }
    ProcessHook process() const override {
        if constexpr (requires(T& t) { t.process(0); })
            return [](void*, void* self, std::int64_t frames) {
                static_cast<T*>(self)->process(static_cast<int>(frames));
            };
        return nullptr;
    }
    UiHook panel() const override {
        if constexpr (requires(T& t, Panel& p) { t.panel(p); })
            return [](void*, void* self, void* p) {
                static_cast<T*>(self)->panel(*static_cast<Panel*>(p));
            };
        return nullptr;
    }
    UiHook draw() const override {
        if constexpr (requires(T& t, Canvas& g) { t.draw(g); })
            return [](void*, void* self, void* g) {
                static_cast<T*>(self)->draw(*static_cast<Canvas*>(g));
            };
        return nullptr;
    }
    UiHook pointer() const override {
        if constexpr (requires(T& t, Pointer& p) { t.pointer(p); })
            return [](void*, void* self, void* p) {
                static_cast<T*>(self)->pointer(*static_cast<Pointer*>(p));
            };
        return nullptr;
    }

  private:
    void describe(const ModuleInfo& i) {
        info_.name = i.name ? i.name : "";
        info_.id = i.id ? i.id : "";
        info_.vendor = i.vendor ? i.vendor : "";
        info_.version = i.version ? i.version : "";
        info_.description = i.description ? i.description : "";
        info_.url = i.url ? i.url : "";
        for (const char* t : i.tags)
            if (t && *t) info_.tags.emplace_back(t);
        info_.width = i.width;
        info_.height = i.height;
    }

    ModuleInfoModel info_;
    std::vector<ParamModel> params_;
    std::vector<PortModel> ports_;
    std::vector<SharedModel> shared_;
    std::vector<FactoryPatchModel> factoryPatches_;
};

}  // namespace supple::script::detail
