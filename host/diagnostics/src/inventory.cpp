// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "inventory.h"
#include <supple/json/utf8.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
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
bool (*gPublish)(Native&, const Packet&) noexcept = &publish;  // a test replaces it
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
    if (!gPublish(gNative, packet)) {
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
    gDropped.store(0);
}

void closeInventory() noexcept {
    gClosed.store(true);  // seq_cst
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (gSenders.load() != 0 && std::chrono::steady_clock::now() < deadline) yieldThread();
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

bool Inventory::apply(const Packet& packet) {
    noteDropped(packet.dropped);
    const auto& r = packet.instance;
    const auto reject = [&] {
        ++rejected_;
        return false;
    };
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
            PluginEntry entry;
            entry.cell = r.cell;
            entry.generation = r.generation;
            entry.kind = static_cast<diagctx::InstanceKind>(r.kind);
            entry.origin = static_cast<diagctx::InstanceOrigin>(r.origin);
            entry.mode = static_cast<diagctx::InstanceMode>(r.mode);
            entry.state = static_cast<diagctx::InstanceState>(r.state);
            entry.image = r.image;
            for (std::size_t i = 0; i < diagctx::kTextFields; ++i)
                entry.text[i] = cleanText(r.text[i], r.length[i]);
            if (slot && slot->generation == r.generation) {
                entry.ordinal = slot->ordinal;  // the same instance, described again
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

std::vector<const PluginEntry*> Inventory::entries() const {
    std::vector<const PluginEntry*> all;
    for (const auto& slot : live_)
        if (slot) all.push_back(&*slot);
    for (const auto& entry : retired_) all.push_back(&entry);
    std::sort(all.begin(), all.end(),
              [](const PluginEntry* a, const PluginEntry* b) { return a->ordinal < b->ordinal; });
    return all;
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
        if (trace) {
            const int module = moduleOfImage(*trace, entry->image);
            if (module >= 0 && static_cast<std::size_t>(module) < trace->modules.size() &&
                trace->modules[static_cast<std::size_t>(module)].id) {
                auto build = json::Value::object();
                build["format"] = moduleFormat();
                build["id"] = *trace->modules[static_cast<std::size_t>(module)].id;
                item["build"] = std::move(build);
            }
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
    return limits;
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
            const std::string_view allowed[]{"id",      "kind",      "origin",   "mode",
                                             "state",   "build",     "name",     "vendor",
                                             "version", "plugin_id", "executing"};
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

}  // namespace supple::diagnostics::detail
