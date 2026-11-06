// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A script compiled as C++ (a source export), as the glue's Code
// (supple/script_code.h): the glue runs it as it runs the scripts the player
// compiles, and describes the same plugin.
//
//   namespace supple::script::user {
//   #include "script/tilt.cpp"
//   }
//   using Tilt = supple::script::user::Tilt;
//   const NativeCode<Tilt> code({{&Tilt::tilt, "tilt"}, {&Tilt::pivot, "pivot"}});
//
// The description comes from the script itself: Info from `info`, each
// Param's spec from its constructor, in declaration order. What C++ cannot
// see is a member's name, which a parameter's patch key is made from: the
// keys come with the code, by member (what the script compiler worked out).
// A Param with no key there takes the one withKey() gives, or else its
// name's in snake_case; file members (AudioFile, Convolver) and Source
// members the same, from a second list. Shared members report where they
// are as they are constructed, as Params do. Scripts never see this header.
#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "supple/script.h"
#include "supple/script_code.h"
#include "supple/script_runtime.h"

namespace supple::script::detail {

// What a script compiled as C++ declares, collected while one instance is
// made (a plugin's, NativeCode; a module's, NativeModuleCode).
namespace native {

struct FactoryCollector final : PatchBuilder {
    const std::vector<ParamModel>& params;
    const std::vector<const Param*>& members;
    std::vector<FactoryPatchModel>& patches;
    FactoryCollector(const std::vector<ParamModel>& p, const std::vector<const Param*>& m,
                     std::vector<FactoryPatchModel>& out)
        : params(p), members(m), patches(out) {}
    void add(const char* name) override { patches.push_back({name ? name : "", {}, {}, {}}); }
    size_t index(const Param& p) const {
        for (size_t i = 0; i < members.size(); ++i)
            if (members[i] == &p) return i;
        return members.size();
    }
    void set(const Param& param, double physical) override {
        if (patches.empty()) add("");
        const size_t i = index(param);
        patches.back().values.emplace_back(i < params.size() ? params[i].id : 0, physical);
    }
    void set(const Param& param, const char* key) override {
        const size_t i = index(param);
        double value = std::numeric_limits<double>::quiet_NaN();
        if (i < params.size() && params[i].kind == ParamModel::Kind::Choice && key)
            for (size_t j = 0; j < params[i].choices.size(); ++j)
                if (choiceKey(params[i], j) == key) value = static_cast<double>(j);
        set(param, value);
    }
    void route(const char* route, double amount, const char* via) override {
        if (patches.empty()) add("");
        patches.back().routes.push_back({route ? route : "", via ? via : "", amount});
    }
    void file(const void* member, const char* reference) override {
        if (patches.empty()) add("");
        uint32_t id = 0;
        for (size_t i = 0; i < fileMembers.size(); ++i)
            if (fileMembers[i] == member) id = fileModels[i].id;
        patches.back().files.emplace_back(id, reference ? reference : "");
    }
    std::vector<const void*> fileMembers;
    std::vector<FileModel> fileModels;
};

static_assert(static_cast<int>(FileSpec::Kind::Impulse) ==
              static_cast<int>(FileModel::Kind::Impulse));

// What the script compiler makes of the same spec. The enums match
// script.h's, value for value.
static_assert(static_cast<int>(ParamSpec::Kind::Toggle) ==
              static_cast<int>(ParamModel::Kind::Toggle));
static_assert(static_cast<int>(Unit::Meters) == static_cast<int>(ParamModel::Unit::Meters));
static_assert(static_cast<int>(Unit::Bpm) == static_cast<int>(ParamModel::Unit::Bpm));
static_assert(static_cast<int>(Curve::Pow) == static_cast<int>(ParamModel::Curve::Pow));
inline ParamModel model(const ParamSpec& s) {
    ParamModel m;
    m.key = s.key;
    m.id = s.id;
    m.kind = static_cast<ParamModel::Kind>(s.kind);
    m.name = s.name;
    m.min = s.min;
    m.max = s.max;
    m.def = s.def;
    m.unit = static_cast<ParamModel::Unit>(s.unit);
    m.curve = static_cast<ParamModel::Curve>(s.curve);
    m.curveK = s.curveK;
    for (const char* c : s.choices) m.choices.emplace_back(c);
    for (const char* k : s.choiceKeys) m.choiceKeys.emplace_back(k);
    m.group = s.group;
    m.description = s.description;
    m.smoothingMs = s.smoothingMs;
    m.automatable = s.automatable;
    m.modulatable = s.modulatable;
    m.polyModulatable = s.polyModulatable;
    m.restarts = s.restart;
    m.offAtMinimum = s.offAtMinimum;
    m.output = s.output;
    m.bypass = s.bypass;
    m.hidden = s.hidden;
    for (const char* r : s.roles) m.roles.emplace_back(r);
    m.routeMin = s.routeMin;
    m.routeMax = s.routeMax;
    return m;
}

struct Collector final : ParamRegistrar {
    std::vector<const Param*> params;
    std::vector<ParamModel> models;
    std::vector<const void*> files;
    std::vector<FileModel> fileModels;
    std::vector<std::pair<const void*, int>> shared;
    void declareShared(void* member, int bytes) override { shared.emplace_back(member, bytes); }
    // A module's ports (ADR-0062), in declaration order.
    std::vector<const void*> ports;
    std::vector<PortSpec> portSpecs;
    void declarePort(void* member, const PortSpec& spec) override {
        ports.push_back(member);
        portSpecs.push_back(spec);
    }
    void declare(Param& param, const ParamSpec& spec) override {
        ParamModel m = model(spec);
        for (int c = 0; c < spec.conditionCount && c < ParamSpec::kConditions; ++c)
            m.activeWhen.push_back(condition(spec.conditions[c]));
        params.push_back(&param);
        models.push_back(std::move(m));
    }
    // A condition on a parameter declared above, its options as indices
    // (what the script compiler makes of it). One the definition must
    // refuse -- on no parameter so far, an option unknown -- names no
    // parameter or holds -1.
    ParamModel::Condition condition(const ParamSpec::Condition& c) const {
        using Kind = ParamSpec::Condition::Kind;
        ParamModel::Condition out;
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i] == c.param) out.param = static_cast<int>(i);
        if (out.param < 0) return out;
        const ParamModel& on = models[static_cast<size_t>(out.param)];
        if (c.kind == Kind::Above || c.kind == Kind::Below) {
            (c.kind == Kind::Above ? out.above : out.below) = c.bound;
            return out;
        }
        const bool unless = c.kind == Kind::Unless;
        const bool toggle = on.kind == ParamModel::Kind::Toggle;
        std::vector<bool> named(toggle ? 2 : on.choices.size(), false);
        if (toggle && c.options.size() == 0) named[1] = true;
        for (const char* option : c.options) {
            size_t k = 0;
            while (k < on.choices.size() && on.choices[k] != option && choiceKey(on, k) != option)
                ++k;
            if (toggle || k == on.choices.size()) {
                out.values.push_back(-1);
                return out;
            }
            named[k] = true;
        }
        for (size_t k = 0; k < named.size(); ++k)
            if (named[k] != unless) out.values.push_back(static_cast<int>(k));
        return out;
    }
    std::vector<const void*> sources;
    std::vector<SourceModel> sourceModels;
    void declareSource(Source& source, const SourceSpec& spec) override {
        sources.push_back(&source);
        SourceModel m;
        m.key = spec.key;
        m.name = spec.name;
        m.description = spec.description;
        m.perVoice = spec.perVoice;
        m.bipolar = spec.bipolar;
        sourceModels.push_back(std::move(m));
    }
    void declareFile(void* member, const FileSpec& spec) override {
        files.push_back(member);
        FileModel m;
        m.key = spec.key;
        m.kind = static_cast<FileModel::Kind>(spec.kind);
        m.name = spec.name;
        m.description = spec.description;
        m.def = spec.def ? spec.def : "";
        fileModels.push_back(std::move(m));
    }
};
// A parameter's name as a key: "Low Cut" -> low_cut.
inline std::string snakeCase(const std::string& name) {
    std::string out;
    bool gap = false;
    for (char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            if (gap && !out.empty()) out += '_';
            gap = false;
            out += static_cast<char>(std::tolower(c));
        } else {
            gap = true;
        }
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) out = "p" + out;
    return out;
}

}  // namespace native

template <class T>
class NativeCode final : public Code {
  public:
    using Key = std::pair<Param T::*, const char*>;
    // A file or source member's key: {&Sampler::sample, "sample"} (an
    // AudioFile, a Convolver or a Source).
    struct FileKey {
        template <class M>
        FileKey(M T::* m, const char* k)
            : of([m](const T& t) -> const void* { return &(t.*m); }), key(k) {}
        std::function<const void*(const T&)> of;
        const char* key;
    };

    explicit NativeCode(std::vector<Key> keys = {}, std::vector<FileKey> fileKeys = {}) {
        static_assert(std::is_base_of_v<Plugin, T>, "a plugin struct derives from Effect");
        describe(T::info);
        kind_ = std::is_base_of_v<Instrument, T>   ? PluginKind::Instrument
                : std::is_base_of_v<NoteEffect, T> ? PluginKind::NoteEffect
                                                   : PluginKind::Effect;
        // One instance, made to watch its parameters declare themselves.
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
        for (size_t i = 0; i < collector.files.size(); ++i) {
            FileModel& m = collector.fileModels[i];
            const void* member = collector.files[i];
            m.offset = static_cast<int>(static_cast<const unsigned char*>(member) - base);
            for (const FileKey& k : fileKeys)
                if (k.of(*probe) == member && m.key.empty()) m.key = k.key;
            if (m.key.empty()) m.key = native::snakeCase(m.name);
            m.id = keyId(m.key);
            files_.push_back(std::move(m));
        }
        for (size_t i = 0; i < collector.sources.size(); ++i) {
            SourceModel& m = collector.sourceModels[i];
            const void* member = collector.sources[i];
            m.offset = static_cast<int>(static_cast<const unsigned char*>(member) - base);
            for (const FileKey& k : fileKeys)
                if (k.of(*probe) == member && m.key.empty()) m.key = k.key;
            if (m.key.empty()) m.key = native::snakeCase(m.name);
            sources_.push_back(std::move(m));
        }
        for (const auto& [member, bytes] : collector.shared)
            shared_.push_back(SharedModel{
                {}, static_cast<int>(static_cast<const unsigned char*>(member) - base), bytes});
        if constexpr (requires(T& t, Patches& p) { t.factoryPatches(p); }) {
            native::FactoryCollector builder(params_, collector.params, factoryPatches_);
            builder.fileMembers = collector.files;
            builder.fileModels = files_;
            Patches patches(builder);
            probe->factoryPatches(patches);
        }
    }

    const InfoModel& info() const override { return info_; }
    PluginKind kind() const override { return kind_; }
    const std::vector<ParamModel>& params() const override { return params_; }
    const std::vector<FileModel>& files() const override { return files_; }
    const std::vector<SourceModel>& sources() const override { return sources_; }
    const std::vector<FactoryPatchModel>& factoryPatches() const override {
        return factoryPatches_;
    }
    const std::vector<SharedModel>& shared() const override { return shared_; }

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
        if constexpr (requires(T& t, Block& b) { t.process(b); })
            return [](void*, void* self, void* block) {
                static_cast<T*>(self)->process(*static_cast<Block*>(block));
            };
        return nullptr;
    }
    VoiceHook startVoice() const override {
        if constexpr (requires(T& t) { t.startVoice(0); })
            return [](void*, void* self, std::int64_t v) {
                static_cast<T*>(self)->startVoice(static_cast<int>(v));
            };
        return nullptr;
    }
    StartHook startVoiceRetrigger() const override {
        if constexpr (requires(T& t) { t.startVoice(0, true); })
            return [](void*, void* self, std::int64_t v, std::int64_t retrigger) {
                static_cast<T*>(self)->startVoice(static_cast<int>(v), retrigger != 0);
            };
        return nullptr;
    }
    VoiceHook releaseVoice() const override {
        if constexpr (requires(T& t) { t.releaseVoice(0); })
            return [](void*, void* self, std::int64_t v) {
                static_cast<T*>(self)->releaseVoice(static_cast<int>(v));
            };
        return nullptr;
    }
    VoiceHook stopVoice() const override {
        if constexpr (requires(T& t) { t.stopVoice(0); })
            return [](void*, void* self, std::int64_t v) {
                static_cast<T*>(self)->stopVoice(static_cast<int>(v));
            };
        return nullptr;
    }
    UiHook editor() const override {
        if constexpr (requires(T& t, Editor& e) { t.editor(e); })
            return [](void*, void* self, void* e) {
                static_cast<T*>(self)->editor(*static_cast<Editor*>(e));
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
    void describe(const Info& i) {
        info_.name = i.name;
        info_.id = i.id;
        info_.vendor = i.vendor;
        info_.version = i.version;
        info_.description = i.description;
        info_.url = i.url;
        for (const char* f : i.features)
            if (f && *f) info_.features.emplace_back(f);
        for (int w : i.channels)
            if (w > 0) info_.channels.push_back(w);
        info_.sidechain = i.sidechain;
        for (const char* o : i.outputs)
            if (o && *o) info_.outputs.emplace_back(o);
        info_.voices = i.voices;
        info_.latency = i.latency;
        info_.tail = i.tail;
    }

    InfoModel info_;
    PluginKind kind_ = PluginKind::Effect;
    std::vector<ParamModel> params_;
    std::vector<FileModel> files_;
    std::vector<SourceModel> sources_;
    std::vector<FactoryPatchModel> factoryPatches_;
    std::vector<SharedModel> shared_;
};

}  // namespace supple::script::detail
