// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "vst3_host.h"
#include "editor_platform.h"
#include "main_thread_wake.h"
#include "pending_edits.h"
#include "platform.h"
#include "unicode.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "supple/base/spsc.h"
#include "supple/json/json.h"

namespace supple::host {
namespace fs = std::filesystem;
namespace vst = Steinberg::Vst;
using Steinberg::FUnknownPtr;
using Steinberg::IPtr;
using Steinberg::kInvalidArgument;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;

namespace {
constexpr std::size_t kMaxEvents = 2048;
constexpr std::size_t kMaxParams = 256;
// The SDK's ParameterChanges grows vectors during addPoint. These bounded
// queues allocate once at construction, before any audio-thread call.
class FixedQueue : public Steinberg::U::Implements<Steinberg::U::Directly<vst::IParamValueQueue>> {
  public:
    FixedQueue() { points.reserve(kMaxEvents); }
    void clear(vst::ParamID id) {
        id_ = id;
        points.clear();
    }
    vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API getPointCount() override {
        return static_cast<Steinberg::int32>(points.size());
    }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 i, Steinberg::int32& offset,
                                           vst::ParamValue& value) override {
        if (i < 0 || static_cast<std::size_t>(i) >= points.size()) return Steinberg::kResultFalse;
        offset = points[i].first;
        value = points[i].second;
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 offset, vst::ParamValue value,
                                           Steinberg::int32& index) override {
        if (points.size() == points.capacity()) return Steinberg::kResultFalse;
        index = static_cast<Steinberg::int32>(points.size());
        points.emplace_back(offset, value);
        return kResultOk;
    }

  private:
    vst::ParamID id_ = vst::kNoParamId;
    std::vector<std::pair<Steinberg::int32, vst::ParamValue>> points;
};

class FixedChanges
    : public Steinberg::U::Implements<Steinberg::U::Directly<vst::IParameterChanges>> {
  public:
    void prepare(std::size_t count) {
        queues_.reserve(std::min(count, kMaxParams));
        for (std::size_t i = 0; i < std::min(count, kMaxParams); ++i)
            queues_.push_back(Steinberg::owned(new FixedQueue));
    }
    void clear() { used_ = 0; }
    Steinberg::int32 PLUGIN_API getParameterCount() override { return used_; }
    vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 i) override {
        return i >= 0 && i < used_ ? queues_[i].get() : nullptr;
    }
    vst::IParamValueQueue* PLUGIN_API addParameterData(const vst::ParamID& id,
                                                       Steinberg::int32& index) override {
        for (Steinberg::int32 i = 0; i < used_; ++i)
            if (queues_[i]->getParameterId() == id) {
                index = i;
                return queues_[i].get();
            }
        if (used_ == static_cast<Steinberg::int32>(queues_.size())) return nullptr;
        index = used_++;
        queues_[index]->clear(id);
        return queues_[index].get();
    }

  private:
    std::vector<IPtr<FixedQueue>> queues_;
    Steinberg::int32 used_ = 0;
};

class Handler : public Steinberg::U::Implements<Steinberg::U::Directly<vst::IComponentHandler>> {
  public:
    std::atomic<MainThreadWake*> mainWake{nullptr};
    void notifyMain() SUPPLE_RT {
        if (auto* wake = mainWake.load(std::memory_order_acquire)) wake->notify();
    }
    std::atomic<Steinberg::int32> restart{0};
    std::atomic<bool> edited{false};
    // An edit from the plug-in's editor (main thread): forwarded to the processor.
    std::function<bool(vst::ParamID, vst::ParamValue)> edit;
    Steinberg::tresult PLUGIN_API beginEdit(vst::ParamID) override { return kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit(vst::ParamID id, vst::ParamValue value) override {
        const bool accepted = edit && edit(id, value);
        edited = true;
        notifyMain();
        return accepted ? kResultOk : kResultFalse;
    }
    Steinberg::tresult PLUGIN_API endEdit(vst::ParamID) override { return kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        restart.fetch_or(flags, std::memory_order_release);
        notifyMain();
        return kResultOk;
    }
};

// The editor's frame and, on Linux, its run loop: file descriptors and timers
// the plug-in registers are serviced from the host's main thread (the UI loop
// polls the descriptors and calls onFd; timers run from serviceMainThread).
#if defined(__linux__)
using PlugFrameInterfaces =
    Steinberg::U::Directly<Steinberg::IPlugFrame, Steinberg::Linux::IRunLoop>;
#define SUPPLE_RUNLOOP_OVERRIDE override
#else
using PlugFrameInterfaces = Steinberg::U::Directly<Steinberg::IPlugFrame>;
#define SUPPLE_RUNLOOP_OVERRIDE
#endif
class PlugFrame : public Steinberg::U::Implements<PlugFrameInterfaces> {
  public:
    struct Timer {
        Steinberg::Linux::ITimerHandler* handler;
        std::uint32_t periodMs;
        std::chrono::steady_clock::time_point next;
    };
    std::vector<std::pair<Steinberg::Linux::IEventHandler*, int>> handlers;
    std::vector<Timer> timers;
    std::atomic<MainThreadWake*> mainWake{nullptr};
    std::atomic<bool> resize{false};
    std::atomic<std::uint64_t> size{0};

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view,
                                             Steinberg::ViewRect* r) override {
        if (!view || !r) return kInvalidArgument;
        size.store((std::uint64_t(std::max(0, r->getWidth())) << 32) |
                       static_cast<std::uint32_t>(std::max(0, r->getHeight())),
                   std::memory_order_release);
        resize = true;
        if (auto* wake = mainWake.load(std::memory_order_acquire)) wake->notify();
        // The window follows on the UI loop's next pass; the view takes the
        // size now (the common host behaviour).
        return view->onSize(r);
    }
    Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* h,
                                                       Steinberg::Linux::FileDescriptor fd)
        SUPPLE_RUNLOOP_OVERRIDE {
        if (!h) return kInvalidArgument;
        handlers.emplace_back(h, fd);
        if (auto* wake = mainWake.load(std::memory_order_acquire)) wake->notify();
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler* h)
        SUPPLE_RUNLOOP_OVERRIDE {
        std::erase_if(handlers, [&](const auto& x) { return x.first == h; });
        if (auto* wake = mainWake.load(std::memory_order_acquire)) wake->notify();
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler* h,
                                                Steinberg::Linux::TimerInterval ms)
        SUPPLE_RUNLOOP_OVERRIDE {
        if (!h) return kInvalidArgument;
        const auto period = static_cast<std::uint32_t>(std::max<Steinberg::uint64>(ms, 1));
        timers.push_back(
            {h, period, std::chrono::steady_clock::now() + std::chrono::milliseconds(period)});
        if (auto* wake = mainWake.load(std::memory_order_acquire)) wake->notify();
        return kResultOk;
    }
    Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* h)
        SUPPLE_RUNLOOP_OVERRIDE {
        std::erase_if(timers, [&](const Timer& t) { return t.handler == h; });
        if (auto* wake = mainWake.load(std::memory_order_acquire)) wake->notify();
        return kResultOk;
    }
};

#undef SUPPLE_RUNLOOP_OVERRIDE

// A module stays loaded for the life of the process once opened (scan or
// instance), and is never unloaded, not even at exit: modules can bind to
// each other's GNU-unique symbols (two plugins built with the same fmt), so
// unloading one leaves the other calling unmapped code — a crash at exit
// seen with two locally built plugins. Hosts commonly keep VST3 modules
// loaded for the same reason.
std::shared_ptr<VST3::Hosting::Module> moduleFor(const fs::path& path) {
    static auto& mutex = *new std::mutex;
    static auto& cache = *new std::map<std::string, std::shared_ptr<VST3::Hosting::Module>>;
    std::lock_guard lock(mutex);
    const auto key = platform::pathText(path.lexically_normal());
    if (auto found = cache.find(key); found != cache.end()) return found->second;
    std::string error;
    auto module = VST3::Hosting::Module::create(key, error);
    if (module) cache[key] = module;
    return module;
}

// The host context plugins get in initialize(): our name, and IMessage /
// IAttributeList creation for component-controller messages.
class HostContext : public vst::HostApplication {
  public:
    explicit HostContext(std::string name) : name_(std::move(name)) {}
    Steinberg::tresult PLUGIN_API getName(vst::String128 name) override {
        return unicode::toUtf16(name_, std::span(name, 128)) ? Steinberg::kResultTrue
                                                             : Steinberg::kInternalError;
    }

  private:
    std::string name_;
};

std::string classId(const VST3::Hosting::ClassInfo& info) { return "vst3:" + info.ID().toString(); }

// Steinberg's moduleinfo writer emits JSON5 comments and trailing commas.
// Normalize those two forms before handing the metadata to core-json.
std::string moduleJson(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool quoted = false, escaped = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (quoted) {
            out.push_back(c);
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            out.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i += 2;
            while (i < input.size() && input[i] != '\n') ++i;
            out.push_back('\n');
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
            ++i;
            out.push_back(' ');
            continue;
        }
        if (c == ',') {
            std::size_t next = i + 1;
            while (next < input.size() && std::isspace(static_cast<unsigned char>(input[next])))
                ++next;
            if (next < input.size() && (input[next] == '}' || input[next] == ']')) continue;
        }
        out.push_back(c);
    }
    return out;
}

std::optional<std::vector<PluginInfo>> scanModuleInfo(const fs::path& path) {
    std::ifstream file(path / "Contents/Resources/moduleinfo.json");
    if (!file) return std::nullopt;
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    auto parsed = json::parse(moduleJson(source));
    if (!parsed || !parsed->isObject()) return std::nullopt;
    const auto* classes = parsed->find("Classes");
    if (!classes || !classes->isArray()) return std::nullopt;
    std::string factoryVendor;
    if (const auto* factory = parsed->find("Factory Info"); factory && factory->isObject()) {
        if (const auto* vendor = factory->find("Vendor"); vendor && vendor->isString())
            factoryVendor = vendor->asString();
    }
    std::vector<PluginInfo> found;
    for (const auto& cls : classes->asArray()) {
        if (!cls.isObject()) return std::nullopt;
        const auto* category = cls.find("Category");
        if (!category || !category->isString() || category->asString() != "Audio Module Class")
            continue;
        const auto* cid = cls.find("CID");
        const auto* name = cls.find("Name");
        if (!cid || !cid->isString() || !name || !name->isString()) return std::nullopt;
        auto uid = VST3::UID::fromString(cid->asString());
        if (!uid) return std::nullopt;
        PluginInfo info;
        info.format = "vst3";
        info.id = "vst3:" + uid->toString();
        info.path = path;
        info.name = name->asString();
        info.vendor = factoryVendor;
        if (const auto* vendor = cls.find("Vendor"); vendor && vendor->isString())
            info.vendor = vendor->asString();
        if (const auto* version = cls.find("Version"); version && version->isString())
            info.version = version->asString();
        if (const auto* categories = cls.find("Sub Categories");
            categories && categories->isArray()) {
            for (const auto& value : categories->asArray()) {
                if (!value.isString()) return std::nullopt;
                info.features.push_back(value.asString());
            }
        }
        info.instrument = std::find(info.features.begin(), info.features.end(), "Instrument") !=
                          info.features.end();
        found.push_back(std::move(info));
    }
    return found;
}

std::vector<std::uint8_t> encodeState(const std::vector<std::uint8_t>& component,
                                      const std::vector<std::uint8_t>& controller) {
    std::vector<std::uint8_t> out;
    out.reserve(16 + component.size() + controller.size());
    auto u32 = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    u32(0x33545356);
    u32(1);
    u32(static_cast<std::uint32_t>(component.size()));
    u32(static_cast<std::uint32_t>(controller.size()));
    out.insert(out.end(), component.begin(), component.end());
    out.insert(out.end(), controller.begin(), controller.end());
    return out;
}
std::uint32_t read32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) |
           (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}
}  // namespace

struct Vst3Instance::State {
    std::shared_ptr<VST3::Hosting::Module> module;
    // Not reference counted (the SDK's HostApplication ignores addRef/release):
    // owned here and declared before the plug-in objects, so it outlives them.
    std::unique_ptr<HostContext> context;
    IPtr<vst::IComponent> component;
    FUnknownPtr<vst::IAudioProcessor> processor;
    IPtr<vst::IEditController> controller;
    IPtr<Handler> handler = Steinberg::owned(new Handler);
    IPtr<PlugFrame> frame = Steinberg::owned(new PlugFrame);
    IPtr<Steinberg::IPlugView> view;                                     // the open editor
    std::optional<bool> hasEditor;                                       // probed once
    FUnknownPtr<vst::IConnectionPoint> componentPoint, controllerPoint;  // when connected
    std::vector<ParamInfo> parameters;
    std::uint32_t inputs = 0, outputs = 0;  // channels of the main buses
    // Every audio bus gets buffers (the VST3 contract): the main buses carry
    // the track's audio, auxiliary ones (sidechains, extra outputs) silence
    // and scratch space, preallocated in activate().
    std::vector<std::uint32_t> inBusChannels, outBusChannels;
    std::vector<std::string> outBusNames;
    // The auxiliary output buses' channel pointers for one block (the block's
    // aux outputs, else scratch) and their scratch (ADR-0021).
    std::vector<float*> outAuxPointers, outAuxScratch;
    std::vector<vst::AudioBusBuffers> inBuses, outBuses;
    std::vector<std::vector<float>> auxStorage;
    std::vector<float*> auxPointers;
    std::vector<float**> silentIn;  // each auxiliary input bus's silent channels
    int keyBus = -1;                // the first auxiliary input bus (the key), ADR-0018
    bool keyActive = false;
    std::vector<float*> keyPointers;  // the block's key, padded with silence
    // Main bus 0's channel pointers for one block (the host's channels, then
    // silence/scratch), so the bus never reaches past the host's array.
    std::vector<float*> mainIn, mainOut;
    std::vector<float> padIn, padOut;
    // Notes delivered and not yet ended, per channel and key: a reset() ends
    // them at the next process() (a VST3 has no reset of its own).
    struct Note {
        std::int16_t channel = 0, key = 0;
        std::int32_t id = -1;
        std::uint32_t count = 0;
    };
    std::array<Note, kMaxEvents> sounding{};
    bool releaseAll = false;
    bool notes = false, active = false;
    bool separateController = false;
    std::array<std::array<vst::ParamID, 130>, 16> midiParams{};
    double rate = 0;
    FixedChanges inputChanges, outputChanges;
    vst::EventList inputEvents{static_cast<Steinberg::int32>(kMaxEvents)};
    vst::EventList outputEvents{static_cast<Steinberg::int32>(kMaxEvents)};
    PendingEdits changes;
    std::atomic<std::uint32_t> dropped{0};
    std::atomic<bool> latencyChanged{false};
    std::atomic<bool> restartWanted{false};  // kLatencyChanged: reactivate to apply
};

Vst3Instance::Vst3Instance() : state_(std::make_unique<State>()) {}
Vst3Instance::~Vst3Instance() {
    auto& s = *state_;
    closeEditor();  // the view goes before the controller
    if (s.active) deactivate();
    if (s.componentPoint && s.controllerPoint) {
        s.componentPoint->disconnect(s.controllerPoint);
        s.controllerPoint->disconnect(s.componentPoint);
    }
    if (s.controller) {
        s.controller->setComponentHandler(nullptr);
        if (s.separateController) s.controller->terminate();
    }
    if (s.component) s.component->terminate();
}

Result<std::unique_ptr<Vst3Instance>> Vst3Instance::create(const PluginInfo& info,
                                                           const HostIdentity& host) {
    auto module = moduleFor(info.path);
    if (!module) return Error("host.load", "cannot load VST3 module " + info.path.string());
    if (info.id.rfind("vst3:", 0) != 0 || info.id.size() != 37)
        return Error("host.create", "invalid VST3 class id " + info.id);
    auto id = VST3::UID::fromString(info.id.substr(5));
    if (!id) return Error("host.create", "invalid VST3 class id " + info.id);
    std::unique_ptr<Vst3Instance> self(new Vst3Instance);
    auto& s = *self->state_;
    s.module = std::move(module);
    s.context = std::make_unique<HostContext>(host.name);
    s.component = s.module->getFactory().createInstance<vst::IComponent>(*id);
    if (!s.component || s.component->initialize(s.context.get()) != kResultOk)
        return Error("host.create", "VST3 component could not initialize");
    s.processor = FUnknownPtr<vst::IAudioProcessor>(s.component);
    if (!s.processor) return Error("host.create", "VST3 class has no audio processor");
    Steinberg::TUID controllerId{};
    if (s.component->getControllerClassId(controllerId) == kResultOk)
        s.controller =
            s.module->getFactory().createInstance<vst::IEditController>(VST3::UID(controllerId));
    s.separateController = static_cast<bool>(s.controller);
    if (!s.controller) s.controller = FUnknownPtr<vst::IEditController>(s.component);
    if (s.separateController) {
        if (s.controller->initialize(s.context.get()) != kResultOk)
            return Error("host.create", "VST3 controller could not initialize");
        // The SDK's host sequence: connect the two halves, then give the
        // controller the component's state (its parameter values).
        s.componentPoint = FUnknownPtr<vst::IConnectionPoint>(s.component);
        s.controllerPoint = FUnknownPtr<vst::IConnectionPoint>(s.controller);
        if (s.componentPoint && s.controllerPoint) {
            s.componentPoint->connect(s.controllerPoint);
            s.controllerPoint->connect(s.componentPoint);
        } else {
            s.componentPoint = nullptr;
            s.controllerPoint = nullptr;
        }
        Steinberg::ResizableMemoryIBStream state;
        if (s.component->getState(&state) == kResultOk) {
            state.rewind();
            s.controller->setComponentState(&state);
        }
    }
    if (s.controller) s.controller->setComponentHandler(s.handler);
    // Editor edits reach the processor through the same queue as setParam().
    s.handler->edit = [state = &s](vst::ParamID param, vst::ParamValue normalized) {
        return state->changes.push(param, normalized);
    };
    for (auto& channel : s.midiParams) channel.fill(vst::kNoParamId);
    if (s.controller) {
        FUnknownPtr<vst::IMidiMapping> mapping(s.controller);
        if (mapping) {
            for (Steinberg::int16 channel = 0; channel < 16; ++channel)
                for (Steinberg::int16 control = 0; control < 130; ++control) {
                    vst::ParamID target = vst::kNoParamId;
                    if (mapping->getMidiControllerAssignment(0, channel, control, target) ==
                        kResultOk)
                        s.midiParams[channel][control] = target;
                }
        }
    }
    for (auto [dir, channels] :
         {std::pair{vst::kInput, &s.inBusChannels}, std::pair{vst::kOutput, &s.outBusChannels}}) {
        const auto count = s.component->getBusCount(vst::kAudio, dir);
        if (count < 0 || count > 128)
            return Error("host.layout", "VST3 layout exceeds 128 audio buses");
        for (Steinberg::int32 b = 0; b < count; ++b) {
            vst::BusInfo bus{};
            const bool ok = s.component->getBusInfo(vst::kAudio, dir, b, bus) == kResultOk;
            bus.name[std::size(bus.name) - 1] = 0;
            if (ok && (bus.channelCount < 0 || bus.channelCount > 64))
                return Error("host.layout", "VST3 audio bus exceeds 64 channels");
            channels->push_back(ok ? static_cast<std::uint32_t>(std::max(bus.channelCount, 0)) : 0);
            if (dir == vst::kOutput)
                s.outBusNames.push_back(ok ? unicode::toUtf8(bus.name) : std::string());
            if (ok && dir == vst::kInput && b > 0 && bus.busType == vst::kAux && s.keyBus < 0 &&
                bus.channelCount > 0)
                s.keyBus = b;
        }
    }
    s.inputs = s.inBusChannels.empty() ? 0 : s.inBusChannels[0];
    s.outputs = s.outBusChannels.empty() ? 0 : s.outBusChannels[0];
    s.notes = s.component->getBusCount(vst::kEvent, vst::kInput) > 0;
    if (s.controller) {
        const auto count = s.controller->getParameterCount();
        for (Steinberg::int32 i = 0; i < count; ++i) {
            vst::ParameterInfo p{};
            if (s.controller->getParameterInfo(i, p) != kResultOk) continue;
            p.title[std::size(p.title) - 1] = 0;
            std::uint32_t flags = 0;
            if (p.stepCount > 0) flags |= CLAP_PARAM_IS_STEPPED;
            if (p.flags & vst::ParameterInfo::kCanAutomate) flags |= CLAP_PARAM_IS_AUTOMATABLE;
            if (p.flags & vst::ParameterInfo::kIsList) flags |= CLAP_PARAM_IS_ENUM;
            if (p.flags & vst::ParameterInfo::kIsReadOnly) flags |= CLAP_PARAM_IS_READONLY;
            if (p.flags & vst::ParameterInfo::kIsHidden) flags |= CLAP_PARAM_IS_HIDDEN;
            if (p.flags & vst::ParameterInfo::kIsBypass) flags |= CLAP_PARAM_IS_BYPASS;
            s.parameters.push_back(
                {p.id, unicode::toUtf8(p.title), "", s.controller->normalizedParamToPlain(p.id, 0),
                 s.controller->normalizedParamToPlain(p.id, 1),
                 s.controller->normalizedParamToPlain(p.id, p.defaultNormalizedValue), flags});
        }
    }
    s.inputChanges.prepare(std::max<std::size_t>(1, s.parameters.size()));
    s.outputChanges.prepare(std::max<std::size_t>(1, s.parameters.size()));
    return self;
}

std::uint32_t Vst3Instance::inputChannels() const { return state_->inputs; }
std::uint32_t Vst3Instance::outputChannels() const { return state_->outputs; }
bool Vst3Instance::acceptsNotes() const { return state_->notes; }
std::vector<AudioPortInfo> Vst3Instance::auxOutputPorts() const {
    const auto& s = *state_;
    std::vector<AudioPortInfo> out;
    for (std::size_t b = 1; b < s.outBusChannels.size(); ++b)
        out.push_back(
            {b < s.outBusNames.size() ? s.outBusNames[b] : std::string(), s.outBusChannels[b]});
    return out;
}

std::uint32_t Vst3Instance::sidechainChannels() const {
    const auto& s = *state_;
    return s.keyBus < 0 ? 0 : s.inBusChannels[static_cast<std::size_t>(s.keyBus)];
}

Status Vst3Instance::activate(double rate, std::uint32_t maxFrames, bool sidechainActive) {
    if (maxFrames > static_cast<std::uint32_t>(std::numeric_limits<Steinberg::int32>::max()))
        return Error("host.activate", "maximum block size exceeds the VST3 limit");
    auto& s = *state_;
    if (s.active) return okStatus();
    // One arrangement per bus: the main buses stereo or mono, auxiliary buses
    // as the plugin has them (a partial list is refused, and the plugin then
    // outputs silence). Main and auxiliary outputs are activated below;
    // the key input is activated only while connected.
    auto arrangements = [&](vst::BusDirection dir, const std::vector<std::uint32_t>& channels,
                            std::uint32_t main) {
        std::vector<vst::SpeakerArrangement> out;
        for (std::size_t b = 0; b < channels.size(); ++b) {
            vst::SpeakerArrangement a =
                channels[b] >= 2 ? vst::SpeakerArr::kStereo : vst::SpeakerArr::kMono;
            if (b == 0)
                a = main == 2 ? vst::SpeakerArr::kStereo : vst::SpeakerArr::kMono;
            else
                s.processor->getBusArrangement(dir, static_cast<Steinberg::int32>(b), a);
            out.push_back(a);
        }
        return out;
    };
    auto ins = arrangements(vst::kInput, s.inBusChannels, s.inputs);
    auto outs = arrangements(vst::kOutput, s.outBusChannels, s.outputs);
    s.processor->setBusArrangements(ins.data(), static_cast<Steinberg::int32>(ins.size()),
                                    outs.data(), static_cast<Steinberg::int32>(outs.size()));
    // What the plugin settled on (it may adapt a refused layout).
    for (auto [dir, channels] :
         {std::pair{vst::kInput, &s.inBusChannels}, std::pair{vst::kOutput, &s.outBusChannels}})
        for (std::size_t b = 0; b < channels->size(); ++b) {
            vst::SpeakerArrangement a = 0;
            if (s.processor->getBusArrangement(dir, static_cast<Steinberg::int32>(b), a) ==
                kResultOk)
                (*channels)[b] = static_cast<std::uint32_t>(vst::SpeakerArr::getChannelCount(a));
        }
    for (const auto* channels : {&s.inBusChannels, &s.outBusChannels})
        if (std::any_of(channels->begin(), channels->end(), [](auto n) { return n > 64; }))
            return Error("host.layout", "negotiated VST3 audio bus exceeds 64 channels");
    if (s.inputs) s.component->activateBus(vst::kAudio, vst::kInput, 0, true);
    if (s.outputs) s.component->activateBus(vst::kAudio, vst::kOutput, 0, true);
    // Every auxiliary output bus: a multi-out instrument's outputs reach the
    // track (ADR-0021).
    for (std::size_t b = 1; b < s.outBusChannels.size(); ++b)
        if (s.outBusChannels[b])
            s.component->activateBus(vst::kAudio, vst::kOutput, static_cast<Steinberg::int32>(b),
                                     true);
    // The main buses are reported (and declared to the plug-in) as the plug-in
    // settled on them, not as they were offered: a plug-in that adapted the
    // layout is served its own width, and the engine's stereo check refuses a
    // plan whose main bus is not what the host's buffers provide, instead of
    // handing it a layout it did not ask for.
    s.inputs = s.inBusChannels.empty() ? 0 : s.inBusChannels[0];
    s.outputs = s.outBusChannels.empty() ? 0 : s.outBusChannels[0];
    s.mainIn.assign(std::max<std::uint32_t>(s.inputs, 1), nullptr);
    s.mainOut.assign(std::max<std::uint32_t>(s.outputs, 1), nullptr);
    s.padIn.assign(maxFrames, 0.0f);
    s.padOut.assign(maxFrames, 0.0f);
    s.inBuses.assign(s.inBusChannels.size(), {});
    s.outBuses.assign(s.outBusChannels.size(), {});
    s.auxStorage.clear();
    s.auxPointers.clear();
    std::size_t auxChannels = 0, auxOutChannels = 0;
    for (std::size_t b = 1; b < s.inBusChannels.size(); ++b) auxChannels += s.inBusChannels[b];
    for (std::size_t b = 1; b < s.outBusChannels.size(); ++b) auxOutChannels += s.outBusChannels[b];
    s.auxPointers.reserve(auxChannels);
    s.outAuxPointers.assign(auxOutChannels, nullptr);
    s.outAuxScratch.clear();
    for (std::size_t b = 1; b < s.inBusChannels.size(); ++b) {
        s.inBuses[b].numChannels = static_cast<Steinberg::int32>(s.inBusChannels[b]);
        s.inBuses[b].channelBuffers32 = s.auxPointers.data() + s.auxPointers.size();
        for (std::uint32_t c = 0; c < s.inBusChannels[b]; ++c) {
            s.auxStorage.emplace_back(maxFrames, 0.0f);
            s.auxPointers.push_back(s.auxStorage.back().data());
        }
    }
    for (std::size_t b = 1; b < s.outBusChannels.size(); ++b) {
        s.outBuses[b].numChannels = static_cast<Steinberg::int32>(s.outBusChannels[b]);
        s.outBuses[b].channelBuffers32 = s.outAuxPointers.data() + s.outAuxScratch.size();
        for (std::uint32_t c = 0; c < s.outBusChannels[b]; ++c) {
            s.auxStorage.emplace_back(maxFrames, 0.0f);
            s.outAuxScratch.push_back(s.auxStorage.back().data());
        }
    }
    s.silentIn.assign(s.inBuses.size(), nullptr);
    for (std::size_t b = 1; b < s.inBuses.size(); ++b)
        s.silentIn[b] = s.inBuses[b].channelBuffers32;
    // The key bus only while a route feeds it: some plug-ins take an active
    // auxiliary bus as "key from it" and would otherwise key from silence.
    s.keyActive = sidechainActive && s.keyBus >= 0;
    if (s.keyBus >= 0) s.component->activateBus(vst::kAudio, vst::kInput, s.keyBus, s.keyActive);
    s.keyPointers.assign(s.keyBus >= 0 ? s.inBusChannels[static_cast<std::size_t>(s.keyBus)] : 0,
                         nullptr);
    if (s.notes) s.component->activateBus(vst::kEvent, vst::kInput, 0, true);
    vst::ProcessSetup setup{vst::kRealtime, vst::kSample32,
                            static_cast<Steinberg::int32>(maxFrames), rate};
    if (s.processor->setupProcessing(setup) != kResultOk ||
        s.component->setActive(true) != kResultOk)
        return Error("host.activate", "VST3 component refused processing setup");
    s.active = true;
    s.rate = rate;
    // A latency the plug-in reports while being activated is already current:
    // a notification, not a request to activate it again.
    if (s.handler->restart.fetch_and(~vst::kLatencyChanged) & vst::kLatencyChanged)
        s.latencyChanged = true;
    return okStatus();
}
void Vst3Instance::deactivate() {
    state_->component->setActive(false);
    state_->active = false;
}
std::uint32_t Vst3Instance::latency() const {
    return state_->active ? state_->processor->getLatencySamples() : 0;
}
std::uint32_t Vst3Instance::tail() const {
    const auto samples = state_->processor->getTailSamples();
    return samples == vst::kInfiniteTail ? UINT32_MAX : samples;
}
std::vector<ParamInfo> Vst3Instance::params() const { return state_->parameters; }

Result<double> Vst3Instance::paramValue(std::uint32_t id) const {
    if (!state_->controller) return Error("host.param", "VST3 has no controller");
    for (const auto& p : state_->parameters)
        if (p.id == id)
            return state_->controller->normalizedParamToPlain(
                id, state_->controller->getParamNormalized(id));
    return Error("host.param", "no parameter with id " + std::to_string(id));
}
Result<std::string> Vst3Instance::paramText(std::uint32_t id, double plain) const {
    if (!state_->controller) return Error("host.param", "VST3 has no controller");
    vst::String128 text{};
    const double normalized = state_->controller->plainParamToNormalized(id, plain);
    if (state_->controller->getParamStringByValue(id, normalized, text) != kResultOk)
        return Error("host.param", "VST3 has no text for parameter " + std::to_string(id));
    text[std::size(text) - 1] = 0;
    return unicode::toUtf8(text);
}
Result<double> Vst3Instance::paramFromText(std::uint32_t id, std::string_view text) const {
    if (!state_->controller) return Error("host.param_text", "VST3 has no controller");
    vst::String128 utf16{};
    if (!unicode::toUtf16(text, utf16)) return Error("host.param_text", "invalid parameter text");
    vst::ParamValue normalized = 0;
    if (state_->controller->getParamValueByString(id, utf16, normalized) != kResultOk)
        return Error("host.param_text", "VST3 could not read parameter text");
    return state_->controller->normalizedParamToPlain(id, normalized);
}
Result<double> Vst3Instance::normalizedParam(std::uint32_t id, double plain) const {
    if (!state_->controller) return Error("host.param", "VST3 has no controller");
    return state_->controller->plainParamToNormalized(id, plain);
}
Result<double> Vst3Instance::plainParam(std::uint32_t id, double normalized) const {
    if (!state_->controller) return Error("host.param", "VST3 has no controller");
    return state_->controller->normalizedParamToPlain(id, normalized);
}
Status Vst3Instance::setParam(std::uint32_t id, double plain) {
    auto& s = *state_;
    const auto found = std::find_if(s.parameters.begin(), s.parameters.end(),
                                    [&](const ParamInfo& p) { return p.id == id; });
    if (found == s.parameters.end())
        return Error("host.param", "no parameter with id " + std::to_string(id));
    if (!std::isfinite(plain) || plain < found->min || plain > found->max)
        return Error("host.param", "parameter value is outside its range");
    // The conversion is a controller call: main thread, here, not in process().
    const double normalized = s.controller->plainParamToNormalized(id, plain);
    if (!s.changes.push(id, normalized))
        return Error("host.param_queue", "too many queued parameter changes");
    s.controller->setParamNormalized(id, normalized);
    return okStatus();
}

Result<std::vector<std::uint8_t>> Vst3Instance::saveState() {
    auto& s = *state_;
    if (!s.active && !s.changes.pending().empty()) {
        s.inputChanges.clear();
        for (const auto& edit : s.changes.pending()) {
            Steinberg::int32 queueIndex = 0, pointIndex = 0;
            auto* queue = s.inputChanges.addParameterData(edit.id, queueIndex);
            if (!queue || queue->addPoint(0, edit.value, pointIndex) != kResultOk)
                return Error("host.state", "too many pending VST3 parameters to flush");
        }
        vst::ProcessData data{};
        data.processMode = vst::kRealtime;
        data.symbolicSampleSize = vst::kSample32;
        data.inputParameterChanges = &s.inputChanges;
        if (s.processor->process(data) != kResultOk)
            return Error("host.state", "VST3 idle parameter flush failed");
        s.changes.barrier();
    }
    Steinberg::ResizableMemoryIBStream component, controller;
    if (state_->component->getState(&component) != kResultOk)
        return Error("host.state", "VST3 component could not save state");
    // A controller with no state of its own may answer kNotImplemented (the
    // SDK's default; sfizz keeps it): its state is then empty, and the
    // component's alone is saved.
    if (state_->separateController) {
        const auto result = state_->controller->getState(&controller);
        if (result != kResultOk && result != Steinberg::kNotImplemented)
            return Error("host.state", "VST3 controller could not save state");
    }
    return encodeState(component.take(), controller.take());
}
Status Vst3Instance::loadState(std::span<const std::uint8_t> bytes) {
    // Framing first; then the component, the controller's copy of it and the
    // controller's own state. A rejection at any step restores the state the
    // plug-in had before, so a failed load leaves it as it was.
    auto check = [](std::span<const std::uint8_t> b) -> Status {
        if (b.size() < 16 || read32(b, 0) != 0x33545356 || read32(b, 4) != 1)
            return Error("host.state", "invalid VST3 state header");
        const auto c = read32(b, 8), e = read32(b, 12);
        if (std::uint64_t(c) + e != b.size() - 16)
            return Error("host.state", "invalid VST3 state lengths");
        if (c > static_cast<std::uint32_t>(std::numeric_limits<Steinberg::int32>::max()) ||
            e > static_cast<std::uint32_t>(std::numeric_limits<Steinberg::int32>::max()))
            return Error("host.state", "VST3 state is too large");
        return okStatus();
    };
    auto apply = [&](std::span<const std::uint8_t> b) -> Status {
        const auto c = read32(b, 8), e = read32(b, 12);
        Steinberg::ResizableMemoryIBStream component, controller;
        component.write(const_cast<std::uint8_t*>(b.data() + 16), static_cast<Steinberg::int32>(c),
                        nullptr);
        component.rewind();
        if (state_->component->setState(&component) != kResultOk)
            return Error("host.state", "VST3 component rejected state");
        if (state_->separateController) {
            component.rewind();
            if (state_->controller->setComponentState(&component) != kResultOk)
                return Error("host.state", "VST3 controller rejected component state");
            if (e) {
                controller.write(const_cast<std::uint8_t*>(b.data() + 16 + c),
                                 static_cast<Steinberg::int32>(e), nullptr);
                controller.rewind();
                if (state_->controller->setState(&controller) != kResultOk)
                    return Error("host.state", "VST3 controller rejected state");
            }
        }
        return okStatus();
    };
    SUPPLE_TRY(check(bytes));
    auto previous = saveState();
    state_->changes.barrier();
    auto status = apply(bytes);
    if (status) return status;
    Error error = status.error();
    if (!previous || !check(*previous) || !apply(*previous))
        error.message += "; restoring the previous state also failed";
    error.hint = "check that the state was saved by this plug-in and version";
    return error;
}

bool Vst3Instance::startProcessing() SUPPLE_RT {
    // kNotImplemented means "nothing to do" (the SDK documents it as OK; sfizz returns it).
    const auto result = state_->processor->setProcessing(true);
    return result == kResultOk || result == Steinberg::kNotImplemented;
}
void Vst3Instance::stopProcessing() SUPPLE_RT { state_->processor->setProcessing(false); }
void Vst3Instance::reset() SUPPLE_RT {
    // A VST3 has no reset: the notes the host delivered and did not end are
    // remembered, and the next process() prepends their note-offs.
    state_->releaseAll = true;
}
void Vst3Instance::flushParams() SUPPLE_RT {}

bool Vst3Instance::process(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT {
    auto& s = *state_;
    s.inputChanges.clear();
    s.outputChanges.clear();
    s.inputEvents.clear();
    s.outputEvents.clear();
    auto addNormalized = [&](std::uint32_t id, double value, std::uint32_t time) {
        Steinberg::int32 queueIndex = 0;
        auto* q = s.inputChanges.addParameterData(id, queueIndex);
        if (!q) {
            s.dropped.fetch_add(1);
            return;
        }
        Steinberg::int32 index = 0;
        if (q->addPoint(static_cast<Steinberg::int32>(time), value, index) != kResultOk)
            s.dropped.fetch_add(1);
    };
    // Everything here is already normalized: setParam() converted on the main
    // thread and the engine's param events arrive normalized for a VST3 (its
    // automation lanes compile into the normalized domain, Instance::
    // paramsNormalized()). No edit-controller call happens on this thread: the
    // controller is not the processor, is not real-time safe, and the host
    // calls it from the main thread only.
    PendingEdits::Change change{};
    while (s.changes.pop(change)) addNormalized(change.id, change.value, 0);
    auto endNotes = [&](std::int16_t port, std::int16_t channel, std::int16_t key, std::int32_t id,
                        std::uint32_t time, float velocity) {
        if (port != -1 && port != 0) return true;
        for (auto& note : s.sounding) {
            if (!note.count || (channel != -1 && channel != note.channel) ||
                (key != -1 && key != note.key) || (id != -1 && id != note.id))
                continue;
            while (note.count) {
                vst::Event off{};
                off.busIndex = 0;
                off.sampleOffset = static_cast<Steinberg::int32>(time);
                off.type = vst::Event::kNoteOffEvent;
                off.noteOff = {note.channel, note.key, velocity, note.id, 0};
                if (s.inputEvents.addEvent(off) != kResultOk) {
                    s.dropped.fetch_add(1);
                    return false;
                }
                --note.count;
            }
        }
        return true;
    };
    if (s.releaseAll) s.releaseAll = !endNotes(-1, -1, -1, -1, 0, 0);
    for (const auto& incoming : block.events) {
        Event event = incoming;
        if (event.kind == Event::Kind::Midi && event.midi[1] < 128 && event.midi[2] < 128) {
            const auto status = event.midi[0] & 0xf0;
            if (status == 0x80 || status == 0x90) {
                event.kind =
                    status == 0x90 && event.midi[2] ? Event::Kind::NoteOn : Event::Kind::NoteOff;
                event.port = 0;
                event.channel = event.midi[0] & 15;
                event.key = event.midi[1];
                event.noteId = -1;
                event.velocity = event.midi[2] / 127.0;
            }
        }
        const auto time = std::min(event.time, block.frames ? block.frames - 1 : 0u);
        if (event.kind == Event::Kind::ParamMod) continue;  // VST3 has no modulation
        if (event.kind == Event::Kind::Param) {
            addNormalized(event.paramId, event.value, time);
            continue;
        }
        if (event.kind == Event::Kind::Midi) {
            if (event.midi[1] >= 128 || event.midi[2] >= 128) {
                s.dropped.fetch_add(1);
                continue;
            }
            const auto channel = event.midi[0] & 15;
            const auto type = event.midi[0] & 0xf0;
            int control = -1;
            double normalized = 0;
            if (type == 0xb0) {
                control = event.midi[1];
                normalized = event.midi[2] / 127.0;
            } else if (type == 0xe0) {
                control = vst::kPitchBend;
                normalized = (event.midi[1] | (event.midi[2] << 7)) / 16383.0;
            } else if (type == 0xd0) {
                control = vst::kAfterTouch;
                normalized = event.midi[1] / 127.0;
            }
            if (control >= 0) {
                const auto target = s.midiParams[channel][static_cast<std::size_t>(control)];
                if (target != vst::kNoParamId) addNormalized(target, normalized, time);
            }
            continue;
        }
        if (event.channel < -1 || event.channel >= 16 || event.key < -1 || event.key >= 128 ||
            event.port < -1 || event.noteId < -1 || !std::isfinite(event.velocity)) {
            s.dropped.fetch_add(1);
            continue;
        }
        if (event.kind != Event::Kind::NoteOn) {
            (void)endNotes(event.port, event.channel, event.key, event.noteId, time,
                           static_cast<float>(event.velocity));
            continue;
        }
        if (event.port != 0 || event.channel < 0 || event.key < 0) {
            s.dropped.fetch_add(1);
            continue;
        }
        auto note = std::find_if(s.sounding.begin(), s.sounding.end(), [&](const auto& n) {
            return n.count && n.channel == event.channel && n.key == event.key &&
                   n.id == event.noteId;
        });
        if (note == s.sounding.end())
            note = std::find_if(s.sounding.begin(), s.sounding.end(),
                                [](const auto& n) { return n.count == 0; });
        if (note == s.sounding.end() || note->count == UINT32_MAX) {
            s.dropped.fetch_add(1);
            continue;
        }
        vst::Event e{};
        e.busIndex = 0;
        e.sampleOffset = static_cast<Steinberg::int32>(time);
        e.type = vst::Event::kNoteOnEvent;
        e.noteOn = {event.channel, event.key, 0, static_cast<float>(event.velocity), 0,
                    event.noteId};
        if (s.inputEvents.addEvent(e) != kResultOk) {
            s.dropped.fetch_add(1);
        } else {
            note->channel = event.channel;
            note->key = event.key;
            note->id = event.noteId;
            ++note->count;
        }
    }
    // Main bus 0: the host's channels, then silence (input) or scratch (output)
    // up to the width the plug-in settled on, so the bus never reaches past the
    // host's channel array.
    if (!s.inBuses.empty()) {
        const std::uint32_t have = std::min(block.inputChannels, s.inputs);
        for (std::uint32_t c = 0; c < have; ++c) s.mainIn[c] = const_cast<float*>(block.inputs[c]);
        for (std::uint32_t c = have; c < s.inputs; ++c) s.mainIn[c] = s.padIn.data();
        s.inBuses[0].numChannels = static_cast<Steinberg::int32>(s.inputs);
        s.inBuses[0].channelBuffers32 = s.mainIn.data();
        s.inBuses[0].silenceFlags = 0;
    }
    if (!s.outBuses.empty()) {
        const std::uint32_t have = std::min(block.outputChannels, s.outputs);
        for (std::uint32_t c = 0; c < have; ++c) s.mainOut[c] = block.outputs[c];
        for (std::uint32_t c = have; c < s.outputs; ++c) s.mainOut[c] = s.padOut.data();
        s.outBuses[0].numChannels = static_cast<Steinberg::int32>(s.outputs);
        s.outBuses[0].channelBuffers32 = s.mainOut.data();
    }
    // Auxiliary outputs: the block's, then scratch (ADR-0021).
    for (std::size_t c = 0; c < s.outAuxPointers.size(); ++c)
        s.outAuxPointers[c] = block.auxOutputs && c < block.auxOutputChannels ? block.auxOutputs[c]
                                                                              : s.outAuxScratch[c];
    for (std::size_t b = 1; b < s.inBuses.size(); ++b) {  // silent auxiliary inputs
        s.inBuses[b].channelBuffers32 = s.silentIn[b];
        s.inBuses[b].silenceFlags = ~0ull;
    }
    if (s.keyActive && block.sidechain && block.sidechainChannels > 0) {
        auto& key = s.inBuses[static_cast<std::size_t>(s.keyBus)];
        for (std::size_t c = 0; c < s.keyPointers.size(); ++c)
            s.keyPointers[c] = c < block.sidechainChannels ? const_cast<float*>(block.sidechain[c])
                                                           : key.channelBuffers32[c];
        key.channelBuffers32 = s.keyPointers.data();
        key.silenceFlags = 0;
    }
    vst::ProcessContext context{};
    context.sampleRate = s.rate;
    if (block.transport) {
        const auto& t = *block.transport;
        context.state = vst::ProcessContext::kTempoValid | vst::ProcessContext::kTimeSigValid |
                        vst::ProcessContext::kProjectTimeMusicValid |
                        vst::ProcessContext::kBarPositionValid;
        if (t.playing) context.state |= vst::ProcessContext::kPlaying;
        if (t.loopActive)
            context.state |= vst::ProcessContext::kCycleActive | vst::ProcessContext::kCycleValid;
        context.tempo = t.tempo;
        context.timeSigNumerator = t.numerator;
        context.timeSigDenominator = t.denominator;
        context.projectTimeMusic = t.beat;
        context.barPositionMusic = t.barStartBeat;
        context.cycleStartMusic = t.loopStartBeat;
        context.cycleEndMusic = t.loopEndBeat;
        context.projectTimeSamples = static_cast<Steinberg::int64>(t.seconds * s.rate);
    } else
        context.projectTimeSamples = block.steadyTime;
    vst::ProcessData data;
    data.processMode = vst::kRealtime;
    data.numSamples = static_cast<Steinberg::int32>(block.frames);
    data.numInputs = static_cast<Steinberg::int32>(s.inBuses.size());
    data.numOutputs = static_cast<Steinberg::int32>(s.outBuses.size());
    data.inputs = s.inBuses.empty() ? nullptr : s.inBuses.data();
    data.outputs = s.outBuses.empty() ? nullptr : s.outBuses.data();
    data.inputParameterChanges = &s.inputChanges;
    data.outputParameterChanges = &s.outputChanges;
    data.inputEvents = &s.inputEvents;
    data.outputEvents = &s.outputEvents;
    data.processContext = &context;
    const bool ok = s.processor->process(data) == kResultOk;
    // A plug-in that settled on a narrower output bus than the host passes
    // writes only its own channels: the host clears the rest, so a persistent
    // buffer cannot repeat the previous block.
    for (std::uint32_t c = std::min(block.outputChannels, s.outputs); c < block.outputChannels; ++c)
        std::fill_n(block.outputs[c], block.frames, 0.0f);
    if (out) {
        for (Steinberg::int32 i = 0; i < s.outputChanges.getParameterCount(); ++i) {
            auto* q = s.outputChanges.getParameterData(i);
            for (Steinberg::int32 j = 0; j < q->getPointCount(); ++j) {
                Steinberg::int32 offset;
                double value;
                if (q->getPoint(j, offset, value) != kResultOk) continue;
                if (out->count == out->storage.size()) {
                    ++out->dropped;
                    continue;
                }
                // The normalized value, as the plug-in reported it: a
                // main-thread consumer converts with Instance::plainParam()
                // when paramsNormalized() (nothing consumes these yet).
                out->storage[out->count++] = {OutputEvent::Kind::ParamValue,
                                              static_cast<std::uint32_t>(offset),
                                              q->getParameterId(), value, -1};
            }
        }
    }
    return ok;
}

void Vst3Instance::setMainThreadWake(MainThreadWake& wake) {
    state_->handler->mainWake.store(&wake, std::memory_order_release);
    state_->frame->mainWake.store(&wake, std::memory_order_release);
}

void Vst3Instance::notifyMain() SUPPLE_RT {
    if (state_->restartWanted.load()) state_->handler->notifyMain();
}

bool Vst3Instance::serviceMainThread() {
    const auto flags = state_->handler->restart.exchange(0);
    if (flags & vst::kLatencyChanged) {
        state_->latencyChanged = true;
        state_->restartWanted = true;  // "the host has to deactivate and reactivate"
    }
    bool did = flags != 0;
    const auto now = std::chrono::steady_clock::now();
    // Copy first: a timer callback may (un)register timers.
    const auto timers = state_->frame->timers;
    for (const auto& t : timers)
        if (now >= t.next) {
            auto& liveTimers = state_->frame->timers;
            const auto live =
                std::find_if(liveTimers.begin(), liveTimers.end(),
                             [&](const auto& timer) { return timer.handler == t.handler; });
            if (live == liveTimers.end()) continue;
            live->next = now + std::chrono::milliseconds(live->periodMs);
            t.handler->onTimer();
            did = true;
        }
    return did;
}

bool Vst3Instance::hasEditor() {
    auto& s = *state_;
    if (!s.hasEditor) {
        s.hasEditor = false;
        if (s.controller)
            if (IPtr<Steinberg::IPlugView> v =
                    Steinberg::owned(s.controller->createView(vst::ViewType::kEditor)))
                s.hasEditor = v->isPlatformTypeSupported(editorVst3Api()) == kResultTrue;
    }
    return *s.hasEditor;
}

Result<Instance::EditorSize> Vst3Instance::openEditor(NativeWindow parent, double scale) {
    auto& s = *state_;
    if (s.view) closeEditor();
    if (!hasEditor())
        return Error("host.editor.unsupported", "this plug-in has no native editor", "/editor",
                     "use the parameter panel or a plugin with a native editor");
    s.view = Steinberg::owned(s.controller->createView(vst::ViewType::kEditor));
    if (!s.view) return Error("host.editor", "the plug-in could not create its editor");
    s.frame->resize = false;
    s.view->setFrame(s.frame);
#if defined(__APPLE__)
    (void)scale;  // NSView uses points; the view observes its backing scale.
#else
    if (FUnknownPtr<Steinberg::IPlugViewContentScaleSupport> scaling(s.view); scaling)
        scaling->setContentScaleFactor(static_cast<float>(scale));
#endif
    // The view's size is asked before it is attached, as VST3 hosts do: some
    // views (clap-wrapper's) size a resizable editor in attached() from the
    // last size reported, and would otherwise open at their minimum.
    Steinberg::ViewRect r;
    (void)s.view->getSize(&r);
    if (s.view->attached(reinterpret_cast<void*>(parent.handle), editorVst3Api()) != kResultOk) {
        s.view->setFrame(nullptr);
        s.view = nullptr;
        return Error("host.editor", "the plug-in refused the parent window");
    }
    Instance::EditorSize size;
    if (s.view->getSize(&r) == kResultOk) {
        size.width = static_cast<std::uint32_t>(std::max(0, r.getWidth()));
        size.height = static_cast<std::uint32_t>(std::max(0, r.getHeight()));
    }
    size.resizable = s.view->canResize() == kResultTrue;
    return size;
}

Result<Instance::EditorSize> Vst3Instance::resizeEditor(std::uint32_t width, std::uint32_t height) {
    auto& s = *state_;
    if (!s.view) return Error("host.editor", "no editor is open");
    Steinberg::ViewRect r(0, 0, static_cast<Steinberg::int32>(width),
                          static_cast<Steinberg::int32>(height));
    Instance::EditorSize size{width, height, s.view->canResize() == kResultTrue};
    if (size.resizable) {
        (void)s.view->checkSizeConstraint(&r);
        (void)s.view->onSize(&r);
    }
    if (s.view->getSize(&r) == kResultOk) {
        size.width = static_cast<std::uint32_t>(std::max(0, r.getWidth()));
        size.height = static_cast<std::uint32_t>(std::max(0, r.getHeight()));
    }
    return size;
}

Result<Instance::EditorSize> Vst3Instance::setEditorScale(double scale, double from) {
    auto& s = *state_;
    if (!s.view) return Error("host.editor", "no editor is open");
    const auto read = [&](Steinberg::ViewRect& r, Instance::EditorSize& size) {
        if (s.view->getSize(&r) != kResultOk) return;
        size.width = static_cast<std::uint32_t>(std::max(0, r.getWidth()));
        size.height = static_cast<std::uint32_t>(std::max(0, r.getHeight()));
    };
    Steinberg::ViewRect r;
    Instance::EditorSize before;
    before.resizable = s.view->canResize() == kResultTrue;
    read(r, before);
    Instance::EditorSize size = before;
    if (!editorTakesScale(nativeWindowKind())) return size;
    if (FUnknownPtr<Steinberg::IPlugViewContentScaleSupport> scaling(s.view); scaling)
        scaling->setContentScaleFactor(static_cast<float>(scale));
    read(r, size);
    // A view that rescaled itself has answered; otherwise a resizable editor
    // keeps the logical size it had.
    if (before.resizable && size.width == before.width && size.height == before.height) {
        rescaleEditorSize(size.width, size.height, from, scale);
        r = Steinberg::ViewRect(0, 0, static_cast<Steinberg::int32>(size.width),
                                static_cast<Steinberg::int32>(size.height));
        (void)s.view->checkSizeConstraint(&r);
        (void)s.view->onSize(&r);
        read(r, size);
    }
    return size;
}

void Vst3Instance::closeEditor() {
    auto& s = *state_;
    if (!s.view) return;
    s.view->removed();
    s.view->setFrame(nullptr);
    s.view = nullptr;
}

bool Vst3Instance::editorOpen() const { return state_->view != nullptr; }

Instance::EditorRequests Vst3Instance::takeEditorRequests() {
    Instance::EditorRequests r;
    if (state_->frame->resize.exchange(false)) {
        r.resize = true;
        const auto size = state_->frame->size.load(std::memory_order_acquire);
        r.width = static_cast<std::uint32_t>(size >> 32);
        r.height = static_cast<std::uint32_t>(size);
    }
    return r;
}

std::vector<std::pair<int, std::uint32_t>> Vst3Instance::watchedFds() const {
    std::vector<std::pair<int, std::uint32_t>> out;
    for (const auto& [h, fd] : state_->frame->handlers) out.emplace_back(fd, CLAP_POSIX_FD_READ);
    return out;
}

void Vst3Instance::onFd(int fd) {
    const auto handlers = state_->frame->handlers;  // a callback may unregister
    for (const auto& [h, f] : handlers)
        if (f == fd && std::find(state_->frame->handlers.begin(), state_->frame->handlers.end(),
                                 std::pair{h, f}) != state_->frame->handlers.end())
            h->onFDIsSet(fd);
}

int Vst3Instance::nextTimerMs() const {
    const auto& timers = state_->frame->timers;
    if (timers.empty()) return -1;
    auto next = timers.front().next;
    for (const auto& t : timers) next = std::min(next, t.next);
    return timerTimeout(next, std::chrono::steady_clock::now());
}

bool Vst3Instance::takeEdited() { return state_->handler->edited.exchange(false); }
bool Vst3Instance::restartWanted() const { return state_->restartWanted.load(); }
void Vst3Instance::clearRestart() { state_->restartWanted = false; }

Instance::Notifications Vst3Instance::takeNotifications() {
    Instance::Notifications n;
    n.latencyChanged = state_->latencyChanged.exchange(false);
    n.droppedEvents = state_->dropped.exchange(0);
    n.stateDirty = takeEdited();  // the editor changed a parameter
    return n;
}

Result<std::vector<PluginInfo>> scanVst3Module(const fs::path& path) {
    if (auto listed = scanModuleInfo(path)) return std::move(*listed);
    std::vector<PluginInfo> result;
    auto module = moduleFor(path);
    if (!module)
        return Error("host.scan.load", "cannot load VST3 bundle " + path.string(), "/path",
                     "check that the bundle is built for this platform");
    auto factory = module->getFactory();
    const auto vendor = factory.info().vendor();
    for (const auto& cls : factory.classInfos()) {
        if (cls.category() != "Audio Module Class") continue;
        PluginInfo info;
        info.format = "vst3";
        info.id = classId(cls);
        info.path = path;
        info.name = cls.name();
        info.vendor = cls.vendor().empty() ? vendor : cls.vendor();
        info.version = cls.version();
        info.features = cls.subCategories();
        info.instrument = std::find(info.features.begin(), info.features.end(), "Instrument") !=
                          info.features.end();
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<fs::path> defaultVst3SearchPath() { return platform::pluginPaths(true); }
}  // namespace supple::host
