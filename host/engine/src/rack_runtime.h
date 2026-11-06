// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: modular racks in the render plan (ADR-0062, rack.md §6). A rack
// is a node of its own: sends reach its In modules, its Out modules feed the
// channels whose record input they are, as edges, so compensation and the
// executors' split treat it as any strip. Its module instances live in the
// RackPool across plans, as plug-ins live in the InstancePool.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "plan.h"
#include "supple/engine/racks.h"
#include "supple/model/session.h"
#include "supple/rack/host_modules.h"
#include "supple/rack/schedule.h"

namespace supple::engine::detail {

// The module types every engine knows: the host modules (the rack runtime's
// and the plugin module) and the stock ones.
std::shared_ptr<const rack::Catalog> builtinModuleCatalog();
using engine::moduleParamJson;
using engine::moduleParamValue;
using engine::pluginModuleType;

// rack.probe's listener (never saved, in no catalog but a probe's): records
// the first kProbeChannels channels (voices) of its input, `seconds` (its
// knob) long.
inline constexpr std::string_view kProbeModule = "com.suppleaudio.rack.probe";
inline constexpr int kProbeChannels = 4;
std::shared_ptr<const rack::ModuleType> probeModuleType();
class ProbeModule final : public rack::HostModule {
  public:
    void process(const rack::HostContext& ctx) SUPPLE_RT override;
    void reserve(std::size_t frames) {
        for (auto& c : recorded) c.reserve(frames);
    }
    std::array<std::vector<float>, kProbeChannels> recorded;
    int width = 0;  // the most channels its input had
};

// rack.capture's listener (never saved, in no catalog but a capture's): the
// notes on its pitch, gate and velocity inputs, voice by voice (a gate's rise
// past 0.5 starts one, its fall below 0.25 ends it), as events in time order.
// Its knob `use_velocity` (0 or 1): whether its velocity input is used.
inline constexpr std::string_view kCaptureModule = "com.suppleaudio.rack.capture";
inline constexpr int kCaptureVoices = 16;
inline constexpr std::size_t kCaptureEvents = 1 << 16;
std::shared_ptr<const rack::ModuleType> captureModuleType();
class CaptureModule final : public rack::HostModule {
  public:
    struct Event {
        std::int64_t frame = 0;  // from the render's start
        int voice = 0;
        bool on = false;
        float pitch = 0, velocity = 0;
    };
    void process(const rack::HostContext& ctx) SUPPLE_RT override;
    void reserve() { events.reserve(kCaptureEvents); }
    std::vector<Event> events;
    std::int64_t frames = 0;  // processed so far
    bool full = false;        // events were dropped
  private:
    std::array<bool, kCaptureVoices> on_{};
};

// A plugin module (rack.md §5): its plug-in, a stage of the rack node, runs
// on the rack's grid. Audio in_l/in_r (R normalled to L) to out_l/out_r;
// poly pitch and gate cables play notes to an instrument (velocity 0..1).
// A plugin module's hidden knobs mod_1..mod_16: each cable into one of its
// plug-in's parameters moves one (rack.md §6).
inline constexpr int kPluginMods = 16;

// A cable into a plug-in's parameter: its slot (mod_<slot + 1>), the CLAP
// parameter, and the scale from the slot's -1..1 to the parameter's CLAP range.
struct PluginMod {
    int slot = 0;
    std::uint32_t param = 0;
    double scale = 0;
};

class PluginModule;

// What one plan gives a plugin module: module instances outlive plans (the
// rack pool keeps them), so a plan's stage, buffers and modulation live
// here, in the plan, and the audio thread attaches them every block.
struct PluginBinding {
    PluginModule* module = nullptr;
    int stage = -1;  // in Node::rackStages; -1: no plug-in (silent)
    Stereo io;
    std::array<PluginMod, kPluginMods> mods{};
    int modCount = 0;
};

class PluginModule final : public rack::HostModule {
  public:
    void process(const rack::HostContext& ctx) SUPPLE_RT override;
    // Set by processRack for each block (audio thread).
    Plan* plan = nullptr;
    Node* node = nullptr;
    PluginBinding* binding = nullptr;
    const host::Transport* transport = nullptr;
    const void* self = nullptr;
    std::int64_t pos = 0;  // the host block's plan sample
    bool failed = false;

  private:
    struct Voice {
        bool high = false;
        int key = -1;
    };
    std::array<Voice, rack::kChannels> voices_{};
    // The modulation last sent to the plug-in, by parameter: a parameter whose
    // cables go is set back to 0 once.
    struct Sent {
        std::uint32_t param = 0;
        double amount = 0;
    };
    std::array<Sent, kPluginMods> sent_{};
    int sentCount_ = 0;
    // The plug-in that heard the notes and modulation above, and its reset()
    // count: a replaced or reset plug-in has forgotten them.
    const host::Instance* heard_ = nullptr;
    std::uint32_t heardResets_ = 0;
};

// Module instances by module id, kept while the module's type, its code and
// the rate stay; replaced ones live until every plan that may use them is
// gone (setGeneration, prune), as InstancePool's plug-ins. Main thread.
class RackPool {
  public:
    RackPool();
    ~RackPool();
    void setCatalog(std::shared_ptr<const rack::Catalog> catalog);
    const std::shared_ptr<const rack::Catalog>& catalog() const { return catalog_; }
    // The instance for `id` of `type` (null: the catalog does not know the
    // type), its knobs set from the module's params.
    rack::Instance* get(Id id, const model::RackModule& module, const rack::Instance::Setup& setup);
    rack::Instance* find(Id id) const;
    void setGeneration(std::uint64_t generation) { generation_ = generation; }
    void prune(const Session& session, std::uint64_t liveGeneration = UINT64_MAX);
    // Knob values from the session's modules into their instances (a knob
    // edit needs no compile). Main thread.
    void applyParams(const Session& session);
    std::size_t size() const { return modules_.size(); }

  private:
    struct Entry {
        std::unique_ptr<rack::Instance> instance;
        std::string type;
        std::uint64_t code = 0;  // the catalog entry's generation
        double rate = 0;
        int seed = 0;
    };
    std::shared_ptr<const rack::Catalog> catalog_;
    std::map<Id, Entry> modules_;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<rack::Instance>>> retired_;
    std::uint64_t generation_ = 0;
};

// One Notes module's MIDI: its source track's compiled events and a device.
struct NotesFeed {
    rack::host::Notes* notes = nullptr;
    std::vector<TimedEvent> events;  // plan samples at the rack's input, sorted
    std::size_t next = 0;
    std::vector<rack::host::MidiEvent> block;  // this block's, reserved
    std::optional<std::string> device;         // live input, matched as a MIDI route's
    std::uint64_t ports = 0;
};

// A module parameter's envelope, read by the schedule once per grid block.
struct RackCurve final : rack::ParamCurve {
    Automation curve;  // plan samples, unshifted
    const struct RackNode* rack = nullptr;
    double at(std::int64_t position) SUPPLE_RT override;
};

// The transport at a grid block, from the plan's time map.
struct RackClock final : rack::Clock {
    const Plan* plan = nullptr;
    const struct RackNode* rack = nullptr;
    script::Transport transport(std::int64_t position) SUPPLE_RT override;
};

struct RackNode {
    Id rack;
    std::vector<Id> modules;  // the patch's, in id order
    std::unique_ptr<rack::Schedule> schedule;
    std::vector<rack::host::Out*> outs;  // Node::rackOuts, in module id order
    std::vector<Id> outIds;
    std::vector<rack::host::In*> ins;  // Node::rackIns
    std::vector<Id> inIds;
    std::vector<NotesFeed> notes;
    std::vector<PluginBinding> plugins;
    std::vector<std::unique_ptr<RackCurve>> curves;
    RackClock clock;
    std::int64_t origin = 0;  // the absolute timeline sample of plan sample 0
    // While stopped the rack keeps running on its own clock, from where the
    // transport stood (`stoppedAt`, plan samples at its input).
    bool playing = false, started = false;
    std::int64_t own = 0, stoppedAt = 0;
    bool live = false;
};

// Compiles rack `rack` into node `n` (main thread): instances from the pool,
// the schedule, its Out/In bindings and Notes feeds. Warnings for what it
// leaves out.
Status compileRack(Plan& p, Node& n, const Session& s, Id rack, InstancePool& pool);
// After the plan's timeline shifts are solved (prepare): events and curves.
void prepareRack(Plan& p, Node& n, const Session& s);
// One block of a rack node (processNode's for racks).
bool processRack(Plan& p, Node& n, std::int64_t pos, std::uint32_t frames,
                 const host::Transport& transport, const void* self) SUPPLE_RT;
// Cursors to `pos`; a seek (`restart`) or a loop wrap also ends held notes.
void locateRack(Node& n, std::int64_t pos, bool restart) SUPPLE_RT;

}  // namespace supple::engine::detail
