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
#include "supple/model/session.h"
#include "supple/rack/host_modules.h"
#include "supple/rack/schedule.h"

namespace supple::engine::detail {

// The module types every engine knows: the host modules (the rack runtime's
// and the plugin module) and the stock ones.
std::shared_ptr<const rack::Catalog> builtinModuleCatalog();
std::shared_ptr<const rack::ModuleType> pluginModuleType();

// A plugin module (rack.md §5): its plug-in, a stage of the rack node, runs
// on the rack's grid. Audio in_l/in_r (R normalled to L) to out_l/out_r;
// poly pitch and gate cables play notes to an instrument (velocity 0..1).
class PluginModule final : public rack::HostModule {
  public:
    void process(const rack::HostContext& ctx) SUPPLE_RT override;
    // Set by processRack for each block.
    Plan* plan = nullptr;
    Node* node = nullptr;
    const host::Transport* transport = nullptr;
    const void* self = nullptr;
    std::int64_t pos = 0;  // the host block's plan sample
    bool failed = false;
    int stage = -1;  // in Node::rackStages; -1: no plug-in (silent)
    Stereo io;

  private:
    struct Voice {
        bool high = false;
        int key = -1;
    };
    std::array<Voice, rack::kChannels> voices_{};
};

// A module's knob from its JSON value (a number, a choice's key, a bool), in
// the physical units Instance::setParam takes; the default when absent or
// not one of its values.
double moduleParamValue(const script::detail::ParamModel& param, const json::Value* value);
// The JSON value a physical value is written as (rack.md §2.2).
json::Value moduleParamJson(const script::detail::ParamModel& param, double physical);

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
    std::vector<PluginModule*> plugins;
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
