// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "inventory.h"
#include <supple/json/utf8.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include "supple/diagctx/commands.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#endif
namespace supple::diagnostics::detail {
namespace {
// ---- worker side ------------------------------------------------------------------------
Native gNative;
Packet gBoot;
std::mutex gSendMutex;  // one inventory send at a time (the Windows ring has one producer)
std::atomic<bool> gInstalled{false}, gClosed{false};
std::atomic<std::uint32_t> gSenders{0}, gDropped{0};
std::atomic<std::uint64_t> gCutoffNs{0};  // closeInventory's deadline for the senders in flight
bool (*gPublish)(Native&, const Packet&) noexcept = &publish;  // a test replaces it
// The command names go out in one burst when the registry is built; a small socket buffer
// (macOS) can fill before the supervisor wakes, so a name chunk waits for room, but the
// whole table gets one budget in all and then the rest is dropped.
constexpr auto kNamesBudget = std::chrono::milliseconds(250);
std::chrono::steady_clock::time_point gNamesDeadline{};
bool gNamesStarted = false;  // guarded by gSendMutex
std::atomic<bool> gNamesGivenUp{false};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free);

void yieldThread() noexcept {
#if defined(_WIN32)
    SwitchToThread();
#else
    (void)::sched_yield();
#endif
}

// The sink diagctx calls. Cold, any thread, never on an audio path; it never waits for the
// supervisor (publish() does not block) and it drops, counted, rather than stall the caller.
void send(const diagctx::Event& event) noexcept {
    gSenders.fetch_add(1);  // seq_cst: pairs with closeInventory's store and load
    struct Leave {
        ~Leave() { gSenders.fetch_sub(1); }
    } leave;
    if (gClosed.load()) {
        gDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Packet packet = gBoot;
    packet.kind = Message::Instance;
    packet.dropped = gDropped.load(std::memory_order_relaxed);
    auto& record = packet.instance;
    record.cell = event.cell;
    record.generation = event.generation;
    record.event = static_cast<std::uint8_t>(event.event);
    record.kind = static_cast<std::uint8_t>(event.kind);
    record.origin = static_cast<std::uint8_t>(event.origin);
    record.mode = static_cast<std::uint8_t>(event.mode);
    record.state = static_cast<std::uint8_t>(event.state);
    record.image = event.image;
    record.bridge = event.bridge;
    record.how = static_cast<std::uint8_t>(event.how);
    record.status = event.status;
    record.childCell = event.childCell;
    record.childGeneration = event.childGeneration;
    const std::string_view fields[diagctx::kTextFields]{event.identity.name, event.identity.vendor,
                                                        event.identity.version, event.identity.id};
    for (std::size_t i = 0; i < diagctx::kTextFields; ++i) {
        const auto n = std::min(fields[i].size(), diagctx::kMaxText);
        if (n != 0) std::memcpy(record.text[i], fields[i].data(), n);  // data() may be null
        record.length[i] = static_cast<std::uint8_t>(n);
    }
    std::lock_guard lock(gSendMutex);
    // The gate may have closed while this call waited for the lock (the send ahead of it
    // timed out finishing a part-sent packet): it must not wait that long again.
    if (gClosed.load()) {
        gDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool names = event.event == diagctx::EventKind::Names;
    if (names && gNamesGivenUp.load()) {
        gDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool sent = gPublish(gNative, packet);
    if (!sent && names && !gNative.torn) {
        if (!gNamesStarted) {
            gNamesStarted = true;
            gNamesDeadline = std::chrono::steady_clock::now() + kNamesBudget;
        }
        while (!sent && !gNative.torn && !gClosed.load() &&
               std::chrono::steady_clock::now() < gNamesDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sent = gPublish(gNative, packet);
        }
        if (!sent && !gNative.torn) gNamesGivenUp.store(true);
    }
    if (!sent) {
        gDropped.fetch_add(1, std::memory_order_relaxed);
        // A packet part sent and not finished leaves the stream torn: close the sink for good,
        // so the process waits for the supervisor at most once, not once per announcement.
        if (gNative.torn) gClosed.store(true);
    }
}

// ---- supervisor side --------------------------------------------------------------------
bool controlFree(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) < 0xa0 &&
            static_cast<unsigned char>(text[i + 1]) >= 0x80)
            return false;  // C1 control characters
    }
    return true;
}
// Whether a text field of a report is one cleanText could have produced.
bool reportText(const json::Value* v) {
    if (!v || !v->isString()) return false;
    const auto& text = v->asString();
    return text.size() <= diagctx::kMaxText && json::firstInvalidUtf8(text) == std::string::npos &&
           controlFree(text);
}
bool validEnum(std::uint8_t value, std::uint8_t count) { return value >= 1 && value < count; }
}  // namespace

void installInventory(const Native& native, const Packet& boot) {
    if (gInstalled.exchange(true)) return;
    gNative = native;
    gBoot = boot;
    gClosed.store(false);
    diagctx::setInventorySink(&send);
}

void resetInventoryForTests(bool (*publishFn)(Native&, const Packet&) noexcept) {
    diagctx::setInventorySink(nullptr);
    gPublish = publishFn ? publishFn : &publish;
    gInstalled.store(false);
    gClosed.store(false);
    gCutoffNs.store(0);
    gDropped.store(0);
    gNamesStarted = false;
    gNamesGivenUp.store(false);
}

std::uint64_t partSendCutoffNs() noexcept { return gCutoffNs.load(); }

void setPartSendCutoffForTests(std::uint64_t nanoseconds) noexcept { gCutoffNs.store(nanoseconds); }

std::uint64_t monotonicNs() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#else
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
#endif
}

bool closeInventory() noexcept {
    // One deadline for everyone: a sender finishing a part-sent packet (publish) stops at it, and
    // this call waits for it, and a little more for the thread to be scheduled and leave, so a
    // shorter wait could write this process's terminal packet into the middle of that packet
    // (macOS takes part of a packet). The deadline is published before the gate closes, so a
    // sender that has passed the gate and is preempted before it starts its send finds it set.
    constexpr std::uint64_t kGraceNs = 50'000'000ULL;
    const std::uint64_t begin = monotonicNs();
    const std::uint64_t cutoff = begin + static_cast<std::uint64_t>(kPartSendPolls) * 1'000'000ULL;
    gCutoffNs.store(begin == 0 ? 1 : cutoff);  // without a clock: 1, already past, to stop at once
    gClosed.store(true);                       // seq_cst
    if (begin == 0) {  // no clock to wait by (cannot happen on a working system): a count
        for (int i = 0; i < 1'000'000 && gSenders.load() != 0; ++i) yieldThread();
    } else {
        while (gSenders.load() != 0 && monotonicNs() < cutoff + kGraceNs) yieldThread();
    }
    // Every sender has left, so what it wrote to gNative is visible (its decrement released it).
    return gSenders.load() != 0 || !gNative.torn;
}

std::uint32_t inventoryDropped() noexcept { return gDropped.load(std::memory_order_relaxed); }

std::string cleanText(const char* bytes, std::size_t length) {
    const std::string_view text(bytes, std::min(length, diagctx::kMaxText));
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t n = json::utf8SequenceLength(text, i);
        const bool c1 = n == 2 && c == 0xc2 && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
                        static_cast<unsigned char>(text[i + 1]) < 0xa0;
        if (n == 0 || c < 0x20 || c == 0x7f || c1) {
            out += '?';
            i += (n == 0 || c < 0x20 || c == 0x7f) ? 1 : n;
        } else {
            out.append(text.substr(i, n));
            i += n;
        }
    }
    return out;  // never longer than the input, so within kMaxText
}

Inventory::Inventory() : live_(kMaxPlugins) {}

void Inventory::noteDropped(std::uint32_t workerCount) noexcept {
    workerDropped_ = std::max<std::size_t>(workerDropped_, workerCount);
}

void Inventory::retire(PluginEntry entry) {
    entry.state = diagctx::InstanceState::Destroyed;
    retired_.push_back(std::move(entry));
    if (retired_.size() > kRetiredPlugins) {
        retired_.pop_front();
        ++retiredLost_;
    }
}

// Four command names, starting at an id that is a multiple of four. A name that is not a
// valid command name is left unknown; a slot with no name is skipped; the rest of the record
// must be zero, which only this build's sender makes it.
bool Inventory::applyNames(const InstanceRecord& r) {
    if (r.cell % diagctx::kNamesPerChunk != 0 || r.cell >= diagctx::kMaxCommandNames ||
        r.generation != 0 || r.kind != 0 || r.origin != 0 || r.mode != 0 || r.state != 0 ||
        r.image != 0 || r.bridge != 0 || r.how != 0 || r.status != 0 || r.childCell != 0 ||
        r.childGeneration != 0)
        return false;
    bool any = false;
    for (std::size_t i = 0; i < diagctx::kNamesPerChunk; ++i) {
        if (r.length[i] > diagctx::kMaxCommandName) return false;
        // The table's own text slots are 64 bytes: the unused two stay empty.
        if (r.length[i] != 0) any = true;
    }
    for (std::size_t i = diagctx::kNamesPerChunk; i < diagctx::kTextFields; ++i)
        if (r.length[i] != 0) return false;
    if (!any) return false;
    if (names_.size() < r.cell + diagctx::kNamesPerChunk)
        names_.resize(r.cell + diagctx::kNamesPerChunk);
    for (std::size_t i = 0; i < diagctx::kNamesPerChunk; ++i) {
        if (r.length[i] == 0) continue;
        const std::string_view name(r.text[i], r.length[i]);
        if (diagctx::validCommandName(name)) {
            names_[r.cell + i] = std::string(name);
        } else {
            names_[r.cell + i].clear();
            ++rejected_;
        }
    }
    return true;
}

// The entry of bridge `id`, made when it is not there yet (an instance or an end named a bridge
// whose start was lost); null when the list is full of bridges that are still running. Keeps at
// most kMaxBridges: the oldest ended one falls out, counted.
BridgeEntry* Inventory::touchBridge(std::uint16_t id) {
    if (const auto found = bridges_.find(id); found != bridges_.end()) return &found->second;
    if (bridges_.size() >= kMaxBridges) {
        const auto ended = std::find_if(bridges_.begin(), bridges_.end(),
                                        [](const auto& item) { return item.second.ended; });
        if (ended == bridges_.end()) {
            ++bridgesLost_;
            return nullptr;
        }
        // A newer bridge never pushes out an even newer one that has ended.
        if (ended->first > id) {
            ++bridgesLost_;
            return nullptr;
        }
        bridges_.erase(ended);
        ++bridgesLost_;
    }
    auto& entry = bridges_[id];
    entry.id = id;
    return &entry;
}

// A Bridge event: ordinal 1 or more, nothing else in the record but how and status, which only
// an end has. A bridge never started is made by an end that names it (the start was lost); an
// end after an end changes nothing.
bool Inventory::applyBridge(const InstanceRecord& r) {
    if (r.bridge == 0 || r.cell != 0 || r.generation != 0 || r.kind != 0 || r.origin != 0 ||
        r.mode != 0 || r.state != 0 || r.image != 0 || r.how >= diagctx::kBridgeHowCount ||
        (r.how == 0 && r.status != 0) || r.childCell != 0 || r.childGeneration != 0)
        return false;
    // The only text an event of a bridge may carry is the incident id the worker minted for an end
    // that may be reported: a signal or an exception, 32 lowercase hex characters.
    for (std::size_t i = 0; i < diagctx::kTextFields; ++i)
        if (r.length[i] != 0 && i != 3) return false;
    std::string incident;
    if (r.length[3] != 0) {
        if (r.length[3] != 32 ||
            (r.how != static_cast<std::uint8_t>(diagctx::BridgeHow::Signal) &&
             r.how != static_cast<std::uint8_t>(diagctx::BridgeHow::Exception)))
            return false;
        incident.assign(r.text[3], 32);
        if (!std::all_of(incident.begin(), incident.end(), [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            }))
            return false;
    }
    BridgeEntry* entry = touchBridge(r.bridge);
    if (!entry) {
        // Counted in bridgesLost_: valid, but there is no room for it, so it is never considered
        // for a report. An end that would have had one says so in `dropped.bridge_reports`.
        if (r.how == static_cast<std::uint8_t>(diagctx::BridgeHow::Signal) ||
            r.how == static_cast<std::uint8_t>(diagctx::BridgeHow::Exception))
            ++bridgeReportsSuppressed_;
        return true;
    }
    if (r.how != 0 && !entry->ended) {
        entry->ended = true;
        entry->how = static_cast<diagctx::BridgeHow>(r.how);
        entry->status = r.status;
        entry->incident = std::move(incident);
    }
    return true;
}

bool Inventory::apply(const Packet& packet) {
    noteDropped(packet.dropped);
    const auto& r = packet.instance;
    const auto reject = [&] {
        ++rejected_;
        return false;
    };
    if (packet.kind == Message::Instance &&
        r.event == static_cast<std::uint8_t>(diagctx::EventKind::Names))
        return applyNames(r) || reject();
    if (packet.kind == Message::Instance &&
        r.event == static_cast<std::uint8_t>(diagctx::EventKind::Bridge))
        return applyBridge(r) || reject();
    if (packet.kind != Message::Instance || r.cell >= kMaxPlugins ||
        r.generation > diagctx::kGenerationMask)
        return reject();
    const auto event = static_cast<diagctx::EventKind>(r.event);
    auto& slot = live_[r.cell];
    switch (event) {
        case diagctx::EventKind::Add: {
            if (!validEnum(r.kind, diagctx::kInstanceKindCount) ||
                !validEnum(r.origin, diagctx::kInstanceOriginCount) ||
                !validEnum(r.mode, diagctx::kInstanceModeCount) ||
                !validEnum(r.state, diagctx::kInstanceStateCount) ||
                static_cast<diagctx::InstanceState>(r.state) == diagctx::InstanceState::Destroyed)
                return reject();
            for (const auto length : r.length)
                if (length > diagctx::kMaxText) return reject();
            if (r.how != 0 || r.status != 0) return reject();  // only a Bridge event has them
            if (r.childCell != 0 || r.childGeneration != 0) return reject();  // nor a Child's
            PluginEntry entry;
            entry.cell = r.cell;
            entry.generation = r.generation;
            entry.kind = static_cast<diagctx::InstanceKind>(r.kind);
            entry.origin = static_cast<diagctx::InstanceOrigin>(r.origin);
            entry.mode = static_cast<diagctx::InstanceMode>(r.mode);
            entry.state = static_cast<diagctx::InstanceState>(r.state);
            entry.image = r.image;
            entry.bridge = r.bridge;
            if (r.bridge != 0) (void)touchBridge(r.bridge);  // it exists: an instance runs in it
            for (std::size_t i = 0; i < diagctx::kTextFields; ++i)
                entry.text[i] = cleanText(r.text[i], r.length[i]);
            if (slot && slot->generation == r.generation) {
                entry.ordinal = slot->ordinal;  // the same instance, described again
                entry.child = slot->child;      // where it is in its bridge is not part of that
            } else {
                // The cell was reused: whoever held it is gone, whether or not it said so.
                if (slot) retire(std::move(*slot));
                entry.ordinal = nextOrdinal_++;
                ++announced_;
            }
            slot = std::move(entry);
            return true;
        }
        case diagctx::EventKind::State:
        case diagctx::EventKind::Remove: {
            const auto state = event == diagctx::EventKind::Remove
                                   ? diagctx::InstanceState::Destroyed
                                   : static_cast<diagctx::InstanceState>(r.state);
            if (!validEnum(static_cast<std::uint8_t>(state), diagctx::kInstanceStateCount))
                return reject();
            // An event for an instance the supervisor never heard of (its Add was lost) is
            // counted and ignored: nothing is made up from half a description.
            if (!slot || slot->generation != r.generation) return reject();
            if (state == diagctx::InstanceState::Destroyed) {
                retire(std::move(*slot));
                slot.reset();
            } else {
                slot->state = state;
            }
            return true;
        }
        case diagctx::EventKind::Child: {
            // Where a bridged instance is inside its bridge process (topic 6b). It describes an
            // instance that was announced as bridged, in a bridge, with its cell and generation
            // in that process's table; nothing else in the record may be set.
            if (!slot || slot->generation != r.generation ||
                slot->mode != diagctx::InstanceMode::Bridged || slot->bridge == 0 ||
                r.childCell >= kMaxPlugins || r.childGeneration > diagctx::kGenerationMask ||
                r.kind != 0 || r.origin != 0 || r.mode != 0 || r.state != 0 || r.bridge != 0 ||
                r.how != 0 || r.status != 0)
                return reject();
            for (const auto length : r.length)
                if (length != 0) return reject();
            slot->child = {r.childCell, r.childGeneration, r.image};
            return true;
        }
        case diagctx::EventKind::Names:
        case diagctx::EventKind::Bridge:
            break;  // handled above, before the cell checks
    }
    return reject();
}

const PluginEntry* Inventory::find(std::uint32_t cell, std::uint16_t generation) const {
    if (cell >= kMaxPlugins) return nullptr;
    if (const auto& slot = live_[cell]; slot && slot->generation == generation) return &*slot;
    for (auto it = retired_.rbegin(); it != retired_.rend(); ++it)
        if (it->cell == cell && it->generation == generation) return &*it;
    return nullptr;
}

const PluginEntry* Inventory::findInBridge(std::uint16_t bridge, std::uint32_t cell,
                                           std::uint16_t generation) const {
    if (bridge == 0) return nullptr;
    const PluginEntry* found = nullptr;
    for (const auto& slot : live_)
        if (slot && slot->bridge == bridge && slot->child.set() && slot->child.cell == cell &&
            slot->child.generation == generation && (!found || slot->ordinal > found->ordinal))
            found = &*slot;
    if (found) return found;
    for (auto it = retired_.rbegin(); it != retired_.rend(); ++it)
        if (it->bridge == bridge && it->child.set() && it->child.cell == cell &&
            it->child.generation == generation)
            return &*it;
    return nullptr;
}

std::vector<const PluginEntry*> Inventory::entries() const {
    std::vector<const PluginEntry*> all;
    for (const auto& slot : live_)
        if (slot) all.push_back(&*slot);
    for (const auto& entry : retired_) all.push_back(&entry);
    std::sort(all.begin(), all.end(),
              [](const PluginEntry* a, const PluginEntry* b) { return a->ordinal < b->ordinal; });
    return all;
}

const BridgeEntry* Inventory::bridge(std::uint16_t id) const {
    const auto found = bridges_.find(id);
    return found == bridges_.end() ? nullptr : &found->second;
}

std::vector<const BridgeEntry*> Inventory::bridges() const {
    std::vector<const BridgeEntry*> all;
    for (const auto& item : bridges_) all.push_back(&item.second);
    return all;  // a std::map: ascending ids
}

bool Inventory::admitBridgeReport(std::uint16_t bridge) {
    const auto found = bridges_.find(bridge);
    if (found == bridges_.end()) return false;
    BridgeEntry& entry = found->second;
    if (!entry.ended || entry.considered) return false;
    entry.considered = true;
    if (entry.how != diagctx::BridgeHow::Signal && entry.how != diagctx::BridgeHow::Exception)
        return false;  // the host's own kill, or an exit: no report, and nothing to count
    std::vector<std::string> members;
    for (const PluginEntry* plugin : entries())
        if (plugin->bridge == bridge && plugin->state != diagctx::InstanceState::Destroyed)
            members.push_back(plugin->text[3] + '\x1f' + plugin->text[2] + '\x1f' +
                              (plugin->text[3].empty() ? plugin->text[0] : std::string()));
    std::sort(members.begin(), members.end());
    std::string key;
    for (const auto& member : members) key += member + '\x1e';
    key += std::string(diagctx::bridgeHowName(entry.how)) + ':' + std::to_string(entry.status);
    if (bridgeReports_ >= kMaxBridgeReports ||
        std::find(bridgeReportKeys_.begin(), bridgeReportKeys_.end(), key) !=
            bridgeReportKeys_.end()) {
        ++bridgeReportsSuppressed_;
        return false;
    }
    bridgeReportKeys_.push_back(std::move(key));
    ++bridgeReports_;
    return true;
}

json::Value Inventory::bridgeExitAttributionJson(std::uint16_t bridge) const {
    std::optional<std::uint32_t> only;
    std::size_t live = 0;
    for (const PluginEntry* plugin : entries())
        if (plugin->bridge == bridge && plugin->state != diagctx::InstanceState::Destroyed) {
            ++live;
            only = plugin->ordinal;
        }
    auto result = json::Value::object();
    result["confidence"] = "bridge_exit";
    result["plugin"] =
        live == 1 && only ? json::Value(static_cast<long long>(*only)) : json::Value();
    return result;
}

json::Value Inventory::bridgesJson() const {
    auto list = json::Value::array();
    const auto plugins = entries();
    for (const BridgeEntry* entry : bridges()) {
        auto item = json::Value::object();
        item["id"] = static_cast<long long>(entry->id);
        item["state"] = entry->ended ? "ended" : "running";
        if (entry->ended) {
            auto ended = json::Value::object();
            ended["how"] = diagctx::bridgeHowName(entry->how);
            ended["code"] = static_cast<long long>(entry->status);
            item["ended"] = std::move(ended);
        }
        auto instances = json::Value::array();
        for (const PluginEntry* plugin : plugins)
            if (plugin->bridge == entry->id)
                instances.push_back(static_cast<long long>(plugin->ordinal));
        item["instances"] = std::move(instances);
        list.push_back(std::move(item));
    }
    return list;
}

namespace {
const char* moduleFormat() {
#if defined(_WIN32)
    return "pe";
#elif defined(__APPLE__)
    return "macho";
#else
    return "elf";
#endif
}
// The module of the snapshot that holds `address`, or -1.
int moduleOfImage(const TraceSnapshot& trace, std::uint64_t address) {
    if (address == 0) return -1;
    const auto it = std::upper_bound(trace.regions.begin(), trace.regions.end(), address,
                                     [](std::uint64_t a, const Region& r) { return a < r.start; });
    if (it == trace.regions.begin()) return -1;
    const Region& region = *(it - 1);
    return address < region.end ? region.module : -1;
}
// The id of the module that holds an entry's image, when the snapshot has it and it has one.
const std::string* buildIdOf(const TraceSnapshot& trace, const PluginEntry& entry) {
    const int module = moduleOfImage(trace, entry.image);
    if (module < 0 || static_cast<std::size_t>(module) >= trace.modules.size()) return nullptr;
    const auto& id = trace.modules[static_cast<std::size_t>(module)].id;
    return id ? &*id : nullptr;
}
bool isLibrary(const PluginEntry& entry) {
    return entry.kind == diagctx::InstanceKind::ClapLibrary ||
           entry.kind == diagctx::InstanceKind::Vst3Library;
}
}  // namespace

json::Value Inventory::pluginsJson(const TraceSnapshot* trace) const {
    auto plugins = json::Value::array();
    for (const PluginEntry* entry : entries()) {
        auto item = json::Value::object();
        item["id"] = static_cast<long long>(entry->ordinal);
        item["kind"] = diagctx::instanceKindName(entry->kind);
        item["origin"] = diagctx::instanceOriginName(entry->origin);
        item["mode"] = diagctx::instanceModeName(entry->mode);
        item["state"] = diagctx::instanceStateName(entry->state);
        if (entry->bridge != 0) item["bridge"] = static_cast<long long>(entry->bridge);
        if (trace)
            if (const std::string* id = buildIdOf(*trace, *entry)) {
                auto build = json::Value::object();
                build["format"] = moduleFormat();
                build["id"] = *id;
                item["build"] = std::move(build);
            }
        const char* const keys[diagctx::kTextFields]{"name", "vendor", "version", "plugin_id"};
        for (std::size_t i = 0; i < diagctx::kTextFields; ++i)
            if (!entry->text[i].empty()) item[keys[i]] = entry->text[i];
        auto executing = json::Value::array();
        if (trace) {
            long long ordinal = 0;
            for (const auto& thread : trace->threads) {
                const auto at = ordinal++;
                if (thread.entry == diagctx::Entry::None || thread.entryCell != entry->cell ||
                    thread.entryGeneration != entry->generation)
                    continue;
                auto active = json::Value::object();
                active["entry"] = diagctx::entryName(thread.entry);
                active["thread"] = at;
                active["match"] = thread.role == Role::Fault ? "thread" : "other_thread";
                executing.push_back(std::move(active));
            }
        }
        if (executing.size() != 0) item["executing"] = std::move(executing);
        plugins.push_back(std::move(item));
    }
    return plugins;
}

json::Value Inventory::attributionJson(const TraceSnapshot* trace) const {
    const char* confidence = "unknown";
    std::optional<std::uint32_t> plugin;
    const auto thread = [&](std::size_t i) -> const PluginEntry* {
        const auto& t = trace->threads[i];
        return t.entry == diagctx::Entry::None ? nullptr : find(t.entryCell, t.entryGeneration);
    };
    if (trace && !trace->threads.empty() && trace->threads[0].role == Role::Fault) {
        const auto& fault = trace->threads[0];
        const PluginEntry* byToken = thread(0);
        // 1. The faulting instruction is in a module that is the build of a listed instance:
        // plug-in code was running, whatever the table says. Several instances of one binary
        // are told apart by the token, then by there being only one that is still alive.
        const std::string* faultModule = nullptr;
        if (!fault.walk.frames.empty() && fault.walk.frames[0].module >= 0 &&
            static_cast<std::size_t>(fault.walk.frames[0].module) < trace->modules.size() &&
            trace->modules[static_cast<std::size_t>(fault.walk.frames[0].module)].id)
            faultModule =
                &*trace->modules[static_cast<std::size_t>(fault.walk.frames[0].module)].id;
        std::vector<const PluginEntry*> inImage;
        if (faultModule)
            for (const PluginEntry* entry : entries())
                if (const std::string* id = buildIdOf(*trace, *entry); id && *id == *faultModule)
                    inImage.push_back(entry);
        if (!inImage.empty()) {
            confidence = "fault_in_image";
            std::vector<const PluginEntry*> live, instances;
            for (const PluginEntry* entry : inImage) {
                if (entry == byToken) plugin = entry->ordinal;
                if (entry->state != diagctx::InstanceState::Destroyed && !isLibrary(*entry))
                    live.push_back(entry);
                if (!isLibrary(*entry)) instances.push_back(entry);
            }
            if (!plugin) {
                if (live.size() == 1)
                    plugin = live[0]->ordinal;
                else if (live.empty() && instances.size() == 1)
                    plugin = instances[0]->ordinal;
                else if (live.empty() && instances.empty() && inImage.size() == 1)
                    plugin = inImage[0]->ordinal;
            }
        } else if (byToken) {
            // 2. The host says this thread was inside the entry of that instance.
            confidence = "active_token";
            plugin = byToken->ordinal;
        } else {
            // 3. Some other thread was inside an entry of an instance: activity, not blame.
            std::vector<std::uint32_t> others;
            for (std::size_t i = 1; i < trace->threads.size(); ++i)
                if (const PluginEntry* entry = thread(i))
                    if (std::find(others.begin(), others.end(), entry->ordinal) == others.end())
                        others.push_back(entry->ordinal);
            if (!others.empty()) {
                confidence = "concurrent_activity";
                if (others.size() == 1) plugin = others[0];
            } else if (!trace->commands.empty()) {
                confidence = "recent_command";  // 4. the model had been running commands
            }
        }
    }
    auto result = json::Value::object();
    result["confidence"] = confidence;
    result["plugin"] = plugin ? json::Value(static_cast<long long>(*plugin)) : json::Value();
    return result;
}

json::Value Inventory::droppedJson(const TraceSnapshot* trace) const {
    const auto count = [](std::size_t n) { return static_cast<long long>(n); };
    auto dropped = json::Value::object();
    dropped["threads"] = count(trace ? trace->dropped : 0);
    dropped["unregistered_threads"] = count(trace ? trace->tableThreadsDropped : 0);
    dropped["plugins"] = count(retiredLost_);
    dropped["commands"] =
        count(trace ? std::size_t{trace->commandsDamaged} + trace->commandsRefused : 0);
    dropped["cells"] =
        count(trace ? std::size_t{trace->unplacedEntries} + trace->tableScopeOverflow : 0);
    dropped["messages"] = count(workerDropped_ + rejected_);
    dropped["untracked"] = count(trace ? trace->tableUntracked : 0);
    dropped["bridges"] = count(bridgesLost_);
    dropped["bridge_reports"] = count(bridgeReportsSuppressed_);
    return dropped;
}

std::vector<std::string> Inventory::limits() const {
    std::vector<std::string> limits;
    if (rejected_ != 0)
        limits.push_back(
            "some plug-in inventory messages were damaged or named an unknown instance and were "
            "ignored");
    if (workerDropped_ != 0)
        limits.push_back(
            "some plug-in inventory messages could not be sent, so instances may be missing or "
            "show an old state");
    if (retiredLost_ != 0)
        limits.push_back("destroyed plug-in instances beyond the last 64 are not listed");
    if (bridgesLost_ != 0) limits.push_back("bridge processes beyond the last 256 are not listed");
    if (bridgeReportsSuppressed_ != 0)
        limits.push_back(
            "some bridge processes that died were not reported separately: a run writes at most 3, "
            "and none for a bridge that repeats one already reported");
    return limits;
}

const std::string* Inventory::commandName(std::uint32_t id) const noexcept {
    return id < names_.size() && !names_[id].empty() ? &names_[id] : nullptr;
}

json::Value Inventory::commandsJson(const TraceSnapshot* trace) const {
    auto commands = json::Value::array();
    if (!trace) return commands;
    for (const auto& crumb : trace->commands) {
        auto item = json::Value::object();
        item["seq"] = static_cast<long long>(crumb.seq);
        const std::string* name = commandName(crumb.nameId);
        item["name"] = name ? *name : std::string("unknown");
        item["origin"] = diagctx::commandOriginName(crumb.origin);
        item["state"] = diagctx::commandStateName(crumb.state);
        commands.push_back(std::move(item));
    }
    return commands;
}

std::vector<std::string> Inventory::commandLimits(const TraceSnapshot* trace) const {
    std::vector<std::string> limits;
    if (!trace || trace->table != TableState::Read) {
        limits.push_back("the execution table could not be read, so no recent commands are listed");
        return limits;
    }
    limits.push_back(
        "recent commands are the last 64 the model thread began; a command begun on another "
        "thread, or whose name was not announced, is not named");
    if (trace->commandsDamaged != 0)
        limits.push_back(
            "some command entries in the execution table were damaged and are left out");
    if (trace->commandsRefused != 0)
        limits.push_back(
            "some commands began on a thread other than the model thread and are not listed");
    for (const auto& crumb : trace->commands)
        if (!commandName(crumb.nameId)) {
            limits.push_back(
                "some listed commands have no name because the name table was not received");
            break;
        }
    return limits;
}

bool commandsShape(const json::Value& commands) {
    if (!commands.isArray() || commands.size() > diagctx::kMaxCommandEntries) return false;
    std::int64_t previous = 0;
    for (const auto& item : commands.asArray()) {
        if (!item.isObject() || item.size() != 4) return false;
        const auto* seq = item.find("seq");
        const auto* name = item.find("name");
        const auto* origin = item.find("origin");
        const auto* state = item.find("state");
        if (!seq || !seq->isInt() || seq->asInt() <= previous) return false;
        previous = seq->asInt();
        if (!name || !name->isString() ||
            (name->asString() != "unknown" && !diagctx::validCommandName(name->asString())))
            return false;
        bool known = false;
        for (std::uint8_t i = 1; i < diagctx::kCommandOriginCount && !known; ++i)
            known = origin && origin->isString() &&
                    origin->asString() ==
                        diagctx::commandOriginName(static_cast<diagctx::CommandOrigin>(i));
        if (!known) return false;
        known = false;
        for (std::uint8_t i = 1; i < diagctx::kCommandStateCount && !known; ++i)
            known = state && state->isString() &&
                    state->asString() ==
                        diagctx::commandStateName(static_cast<diagctx::CommandState>(i));
        if (!known) return false;
    }
    return true;
}

bool droppedShape(const json::Value& dropped) {
    if (dropped.isInt()) return dropped.asInt() >= 0;  // reports of older builds
    if (!dropped.isObject()) return false;
    // The seven counts of topic 5 are always there; the bridge counts (topic 6) are there from
    // the build that wrote them on.
    for (const char* key : {"threads", "unregistered_threads", "plugins", "commands", "cells",
                            "messages", "untracked"}) {
        const auto* v = dropped.find(key);
        if (!v || !v->isInt() || v->asInt() < 0) return false;
    }
    for (const auto& member : dropped.asObject()) {
        const std::string_view known[]{"threads",       "unregistered_threads",
                                       "plugins",       "commands",
                                       "cells",         "messages",
                                       "untracked",     "bridges",
                                       "bridge_reports"};
        if (std::find(std::begin(known), std::end(known), member.key) == std::end(known))
            return false;
        if (!member.value.isInt() || member.value.asInt() < 0) return false;
    }
    return true;
}

// The id of the module the fault thread's first frame is in, as the report's own threads and
// modules say, or null when the report does not say.
static const std::string* faultModuleId(const json::Value& threads, const json::Value* modules) {
    if (!threads.isArray() || threads.size() == 0 || !modules || !modules->isArray())
        return nullptr;
    const auto& fault = threads.asArray()[0];
    const auto* role = fault.find("role");
    const auto* frames = fault.find("frames");
    if (!role || !role->isString() || role->asString() != "fault" || !frames ||
        !frames->isArray() || frames->size() == 0)
        return nullptr;
    const auto* index = frames->asArray()[0].find("module");
    if (!index || !index->isInt() || index->asInt() < 0 ||
        static_cast<std::uint64_t>(index->asInt()) >= modules->size())
        return nullptr;
    const auto* id = modules->asArray()[static_cast<std::size_t>(index->asInt())].find("id");
    return id && id->isString() ? &id->asString() : nullptr;
}

bool attributionShape(const json::Value& attribution, const json::Value& plugins,
                      const json::Value& commands, const json::Value& threads,
                      const json::Value* modules, std::int64_t bridge) {
    if (!attribution.isObject() || attribution.size() != 2) return false;
    const auto* confidence = attribution.find("confidence");
    const auto* plugin = attribution.find("plugin");
    if (!confidence || !confidence->isString() || !plugin) return false;
    const std::string_view how = confidence->asString();
    if (how != "fault_in_image" && how != "active_token" && how != "concurrent_activity" &&
        how != "recent_command" && how != "unknown" && how != "bridge_exit")
        return false;
    // A bridge's end names no instruction: it belongs to a bridge_exit report and only to one.
    if ((how == "bridge_exit") != (bridge != 0)) return false;
    // A fault in a plug-in's binary needs the fault thread's first frame in a module whose id
    // is the build of a plug-in the report lists; the plug-in named, if any, is held to it below.
    const std::string* faultId = nullptr;
    if (how == "fault_in_image") {
        faultId = faultModuleId(threads, modules);
        if (!faultId || !plugins.isArray()) return false;
        bool listed = false;
        for (const auto& item : plugins.asArray()) {
            const auto* build = item.find("build");
            const auto* id = build ? build->find("id") : nullptr;
            if (id && id->isString() && id->asString() == *faultId) listed = true;
        }
        if (!listed) return false;
    }
    if (plugin->isNull()) {
        // A token names an instance by definition.
        if (how == "active_token") return false;
        if (how == "recent_command") return commands.isArray() && commands.size() != 0;
        return true;
    }
    if (!plugin->isInt() || how == "recent_command" || how == "unknown") return false;
    // The plug-in is one the report lists, and its own entry supports what is claimed.
    if (!plugins.isArray()) return false;
    for (const auto& item : plugins.asArray()) {
        const auto* id = item.find("id");
        if (!id || !id->isInt() || id->asInt() != plugin->asInt()) continue;
        const auto* executing = item.find("executing");
        const auto matched = [&](std::string_view match) {
            if (executing && executing->isArray())
                for (const auto& active : executing->asArray())
                    if (const auto* m = active.find("match");
                        m && m->isString() && m->asString() == match)
                        return true;
            return false;
        };
        if (how == "bridge_exit") {
            // A member of that bridge that was still there when it ended.
            const auto* member = item.find("bridge");
            const auto* state = item.find("state");
            return member && member->isInt() && member->asInt() == bridge && state &&
                   state->isString() && state->asString() != "destroyed";
        }
        if (how == "active_token") return matched("thread");
        if (how == "concurrent_activity") return matched("other_thread");
        const auto* build = item.find("build");
        const auto* buildId = build ? build->find("id") : nullptr;
        return buildId && buildId->isString() && buildId->asString() == *faultId;
    }
    return false;
}

bool pluginsShape(const json::Value& plugins, std::size_t threadCount) {
    if (!plugins.isArray() || plugins.size() > kMaxPlugins + kRetiredPlugins) return false;
    const auto oneOf = [](const json::Value* v, std::uint8_t count, auto name, auto cast) {
        if (!v || !v->isString()) return false;
        for (std::uint8_t i = 1; i < count; ++i)
            if (v->asString() == name(cast(i))) return true;
        return false;
    };
    std::int64_t previous = -1;
    for (const auto& item : plugins.asArray()) {
        if (!item.isObject()) return false;
        for (const auto& member : item.asObject()) {
            const std::string_view allowed[]{"id",      "kind",      "origin",    "mode",
                                             "state",   "build",     "name",      "vendor",
                                             "version", "plugin_id", "executing", "bridge"};
            if (std::find(std::begin(allowed), std::end(allowed), member.key) == std::end(allowed))
                return false;
        }
        const auto* id = item.find("id");
        if (!id || !id->isInt() || id->asInt() <= previous || id->asInt() > INT32_MAX) return false;
        previous = id->asInt();
        if (!oneOf(item.find("kind"), diagctx::kInstanceKindCount, diagctx::instanceKindName,
                   [](auto i) { return static_cast<diagctx::InstanceKind>(i); }) ||
            !oneOf(item.find("origin"), diagctx::kInstanceOriginCount, diagctx::instanceOriginName,
                   [](auto i) { return static_cast<diagctx::InstanceOrigin>(i); }) ||
            !oneOf(item.find("mode"), diagctx::kInstanceModeCount, diagctx::instanceModeName,
                   [](auto i) { return static_cast<diagctx::InstanceMode>(i); }) ||
            !oneOf(item.find("state"), diagctx::kInstanceStateCount, diagctx::instanceStateName,
                   [](auto i) { return static_cast<diagctx::InstanceState>(i); }))
            return false;
        for (const char* key : {"name", "vendor", "version", "plugin_id"})
            if (const auto* text = item.find(key); text && !reportText(text)) return false;
        if (const auto* bridge = item.find("bridge");
            bridge && (!bridge->isInt() || bridge->asInt() < 1 || bridge->asInt() > UINT16_MAX))
            return false;
        if (const auto* build = item.find("build")) {
            if (!build->isObject() || build->size() != 2) return false;
            const auto* format = build->find("format");
            const auto* buildId = build->find("id");
            if (!format || !format->isString() ||
                (format->asString() != "elf" && format->asString() != "macho" &&
                 format->asString() != "pe"))
                return false;
            if (!buildId || !buildId->isString() || buildId->asString().empty() ||
                buildId->asString().size() > 128 ||
                !std::all_of(buildId->asString().begin(), buildId->asString().end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                }))
                return false;
        }
        if (const auto* executing = item.find("executing")) {
            if (!executing->isArray() || executing->size() == 0 || executing->size() > threadCount)
                return false;
            for (const auto& active : executing->asArray()) {
                if (!active.isObject() || active.size() != 3) return false;
                const auto* thread = active.find("thread");
                const auto* match = active.find("match");
                const auto* entryName = active.find("entry");
                if (!thread || !thread->isInt() || thread->asInt() < 0 ||
                    static_cast<std::uint64_t>(thread->asInt()) >= threadCount)
                    return false;
                if (!match || !match->isString() ||
                    (match->asString() != "thread" && match->asString() != "other_thread"))
                    return false;
                bool known = false;
                if (entryName && entryName->isString())
                    for (std::uint8_t k = 1; k < diagctx::kEntryCount && !known; ++k)
                        known = entryName->asString() ==
                                diagctx::entryName(static_cast<diagctx::Entry>(k));
                if (!known) return false;
            }
        }
    }
    return true;
}

bool bridgesShape(const json::Value& bridges, const json::Value& plugins,
                  std::int64_t droppedBridges) {
    if (!bridges.isArray() || bridges.size() > kMaxBridges || !plugins.isArray()) return false;
    std::int64_t previous = 0;
    std::vector<std::int64_t> listed;
    for (const auto& item : bridges.asArray()) {
        if (!item.isObject()) return false;
        for (const auto& member : item.asObject()) {
            const std::string_view allowed[]{"id", "state", "ended", "instances"};
            if (std::find(std::begin(allowed), std::end(allowed), member.key) == std::end(allowed))
                return false;
        }
        const auto* id = item.find("id");
        const auto* state = item.find("state");
        const auto* ended = item.find("ended");
        const auto* instances = item.find("instances");
        if (!id || !id->isInt() || id->asInt() <= previous || id->asInt() > UINT16_MAX)
            return false;
        previous = id->asInt();
        listed.push_back(previous);
        if (!state || !state->isString() ||
            (state->asString() != "running" && state->asString() != "ended"))
            return false;
        const bool isEnded = state->asString() == "ended";
        if (isEnded != (ended != nullptr)) return false;
        if (ended) {
            if (!ended->isObject() || ended->size() != 2) return false;
            const auto* how = ended->find("how");
            const auto* code = ended->find("code");
            if (!how || !how->isString() || !code || !code->isInt() || code->asInt() < INT32_MIN ||
                code->asInt() > INT32_MAX)
                return false;
            bool known = false;
            for (std::uint8_t i = 1; i < diagctx::kBridgeHowCount && !known; ++i)
                known =
                    how->asString() == diagctx::bridgeHowName(static_cast<diagctx::BridgeHow>(i));
            if (!known) return false;
        }
        if (!instances || !instances->isArray() || instances->size() > plugins.size()) return false;
        std::int64_t last = -1;
        for (const auto& instance : instances->asArray()) {
            if (!instance.isInt() || instance.asInt() <= last) return false;
            last = instance.asInt();
            bool found = false;
            for (const auto& plugin : plugins.asArray()) {
                const auto* pluginId = plugin.find("id");
                if (!pluginId || !pluginId->isInt() || pluginId->asInt() != last) continue;
                const auto* bridge = plugin.find("bridge");
                found = bridge && bridge->isInt() && bridge->asInt() == previous;
                break;
            }
            if (!found) return false;
        }
    }
    // Every plug-in that says it runs in a bridge is listed by one, unless bridges were dropped.
    for (const auto& plugin : plugins.asArray()) {
        const auto* bridge = plugin.find("bridge");
        if (!bridge || !bridge->isInt()) continue;
        if (std::find(listed.begin(), listed.end(), bridge->asInt()) != listed.end()) {
            const auto* id = plugin.find("id");
            bool inList = false;
            for (const auto& item : bridges.asArray())
                if (item.find("id")->asInt() == bridge->asInt())
                    for (const auto& instance : item.find("instances")->asArray())
                        inList = inList || (id && id->isInt() && instance.asInt() == id->asInt());
            if (!inList) return false;
        } else if (droppedBridges <= 0) {
            return false;
        }
    }
    return true;
}

bool bridgeExitShape(const json::Value& bridges, std::int64_t bridge, std::int64_t code) {
    if (!bridges.isArray()) return false;
    for (const auto& item : bridges.asArray()) {
        const auto* id = item.find("id");
        if (!id || !id->isInt() || id->asInt() != bridge) continue;
        const auto* ended = item.find("ended");
        const auto* how = ended ? ended->find("how") : nullptr;
        const auto* endCode = ended ? ended->find("code") : nullptr;
        return how && how->isString() &&
               (how->asString() == "signal" || how->asString() == "exception") && endCode &&
               endCode->isInt() && endCode->asInt() == code;
    }
    return false;
}

}  // namespace supple::diagnostics::detail
