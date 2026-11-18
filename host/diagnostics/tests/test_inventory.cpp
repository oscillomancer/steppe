// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>
#include "../src/inventory.h"
#include "supple/diagcap/native.h"
#include "supple/diagcap/selfcapture.h"
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace supple::diagnostics::detail;
namespace diagctx = supple::diagctx;
namespace json = supple::json;
namespace {
Packet base() {
    Packet p;
    p.role = 3;
    std::fill(p.incident.begin(), p.incident.begin() + 32, '0');
    p.kind = Message::Instance;
    return p;
}
void setText(Packet& p, std::size_t field, std::string_view text) {
    const auto n = std::min<std::size_t>(text.size(), sizeof(p.instance.text[field]));
    std::memcpy(p.instance.text[field], text.data(), n);
    p.instance.length[field] = static_cast<std::uint8_t>(n);
}
Packet add(std::uint32_t cell, std::uint16_t generation, diagctx::InstanceState state,
           std::string_view name = "Gain", std::string_view vendor = "Acme") {
    Packet p = base();
    auto& r = p.instance;
    r.cell = cell;
    r.generation = generation;
    r.event = static_cast<std::uint8_t>(diagctx::EventKind::Add);
    r.kind = static_cast<std::uint8_t>(diagctx::InstanceKind::Clap);
    r.origin = static_cast<std::uint8_t>(diagctx::InstanceOrigin::Foreign);
    r.mode = static_cast<std::uint8_t>(diagctx::InstanceMode::InProcess);
    r.state = static_cast<std::uint8_t>(state);
    r.image = 0x7000;
    setText(p, 0, name);
    setText(p, 1, vendor);
    setText(p, 2, "1.2");
    setText(p, 3, "com.acme.gain");
    return p;
}
Packet change(std::uint32_t cell, std::uint16_t generation, diagctx::EventKind event,
              diagctx::InstanceState state) {
    Packet p = base();
    p.instance.cell = cell;
    p.instance.generation = generation;
    p.instance.event = static_cast<std::uint8_t>(event);
    p.instance.state = static_cast<std::uint8_t>(state);
    return p;
}
Packet addIn(std::uint32_t cell, std::uint16_t generation, std::uint16_t bridge,
             std::string_view name = "Gain") {
    Packet p = add(cell, generation, diagctx::InstanceState::Active, name);
    p.instance.bridge = bridge;
    p.instance.mode = static_cast<std::uint8_t>(diagctx::InstanceMode::Bridged);
    return p;
}
Packet bridgeEvent(std::uint16_t bridge, diagctx::BridgeHow how = diagctx::BridgeHow::None,
                   std::int32_t status = 0) {
    Packet p = base();
    p.instance.event = static_cast<std::uint8_t>(diagctx::EventKind::Bridge);
    p.instance.bridge = bridge;
    p.instance.how = static_cast<std::uint8_t>(how);
    p.instance.status = status;
    return p;
}
Packet childEvent(std::uint32_t cell, std::uint16_t generation, std::uint32_t childCell,
                  std::uint16_t childGeneration, std::uint64_t image) {
    Packet p = base();
    p.instance.event = static_cast<std::uint8_t>(diagctx::EventKind::Child);
    p.instance.cell = cell;
    p.instance.generation = generation;
    p.instance.childCell = childCell;
    p.instance.childGeneration = childGeneration;
    p.instance.image = image;
    return p;
}
using diagctx::EventKind;
using diagctx::InstanceState;
}  // namespace

TEST_CASE("inventory text is bounded, valid UTF-8 and free of control characters") {
    CHECK(cleanText("Plain name", 10) == "Plain name");
    CHECK(cleanText("a\x01"
                    "b\x7f"
                    "c\nd",
                    7) == "a?b?c?d");
    CHECK(cleanText("caf\xc3\xa9", 5) == "caf\xc3\xa9");  // valid multibyte text stays
    CHECK(cleanText("\xc2\x85x", 3) == "?x");             // a C1 control character
    CHECK(cleanText("a\xff"
                    "b\xc3",
                    4) == "a?b?");  // invalid bytes, one '?' each
    CHECK(cleanText("a\0b", 3) == "a?b");
    const std::string longText(200, 'x');
    CHECK(cleanText(longText.data(), longText.size()).size() == diagctx::kMaxText);
    CHECK(cleanText("abc", 0).empty());
}

TEST_CASE("inventory follows an instance from loading to destroyed") {
    Inventory inventory;
    CHECK(inventory.empty());
    REQUIRE(inventory.apply(add(7, 3, InstanceState::Loading)));
    REQUIRE_FALSE(inventory.empty());
    const auto* found = inventory.find(7, 3);
    REQUIRE(found);
    CHECK(found->state == InstanceState::Loading);
    CHECK(found->text[0] == "Gain");
    CHECK(found->text[3] == "com.acme.gain");
    CHECK(inventory.find(7, 4) == nullptr);  // another generation of the cell
    REQUIRE(inventory.apply(change(7, 3, EventKind::State, InstanceState::Active)));
    CHECK(inventory.find(7, 3)->state == InstanceState::Active);
    // Described again (the library opened): the same instance, the same id.
    const auto ordinal = inventory.find(7, 3)->ordinal;
    auto again = add(7, 3, InstanceState::Loading);
    again.instance.image = 0x9000;
    REQUIRE(inventory.apply(again));
    CHECK(inventory.find(7, 3)->ordinal == ordinal);
    CHECK(inventory.find(7, 3)->image == 0x9000);
    REQUIRE(inventory.apply(change(7, 3, EventKind::Remove, InstanceState::Destroyed)));
    const auto* gone = inventory.find(7, 3);
    REQUIRE(gone);
    CHECK(gone->state == InstanceState::Destroyed);
    CHECK(inventory.entries().size() == 1);
    CHECK(inventory.rejected() == 0);
}

TEST_CASE("a reused cell retires whoever held it, and ids follow the order of announcement") {
    Inventory inventory;
    REQUIRE(inventory.apply(add(1, 0, InstanceState::Active, "First")));
    REQUIRE(inventory.apply(add(2, 0, InstanceState::Active, "Second")));
    // Cell 1 comes back in the next generation without a Remove: the first is gone.
    REQUIRE(inventory.apply(add(1, 1, InstanceState::Loading, "Third")));
    const auto all = inventory.entries();
    REQUIRE(all.size() == 3);
    CHECK(all[0]->text[0] == "First");
    CHECK(all[0]->state == InstanceState::Destroyed);
    CHECK(all[1]->text[0] == "Second");
    CHECK(all[2]->text[0] == "Third");
    CHECK(all[0]->ordinal < all[1]->ordinal);
    CHECK(all[1]->ordinal < all[2]->ordinal);
    CHECK(inventory.find(1, 0)->text[0] == "First");  // still found, by its generation
}

TEST_CASE("events for an instance the inventory never heard of invent nothing") {
    Inventory inventory;
    CHECK_FALSE(inventory.apply(change(5, 0, EventKind::State, InstanceState::Active)));
    CHECK_FALSE(inventory.apply(change(5, 0, EventKind::Remove, InstanceState::Destroyed)));
    REQUIRE(inventory.apply(add(5, 2, InstanceState::Loading)));
    // Right cell, wrong generation: not the same instance.
    CHECK_FALSE(inventory.apply(change(5, 1, EventKind::State, InstanceState::Active)));
    CHECK(inventory.find(5, 2)->state == InstanceState::Loading);
    CHECK(inventory.rejected() == 3);
    CHECK(inventory.entries().size() == 1);
    CHECK_FALSE(inventory.limits().empty());
}

TEST_CASE("every damaged record is refused or leaves a well-formed entry, never an invented one") {
    // Each byte of the record is replaced, one at a time, by several values. The result is
    // either a refusal that changes nothing, or an entry whose every field is in range and
    // whose strings are clean, and it never makes a second entry out of one packet.
    const Packet good = add(9, 1, InstanceState::Loading);
    const std::uint8_t values[]{0x00, 0x01, 0x07, 0x7f, 0x80, 0xc2, 0xff};
    const auto bytes = sizeof(InstanceRecord);
    for (std::size_t at = 0; at < bytes; ++at) {
        for (const auto value : values) {
            Packet damaged = good;
            reinterpret_cast<std::uint8_t*>(&damaged.instance)[at] = value;
            Inventory inventory;
            const bool applied = inventory.apply(damaged);
            const auto entries = inventory.entries();
            if (!applied) {
                CHECK(entries.empty());
                CHECK(inventory.rejected() == 1);
                continue;
            }
            REQUIRE(entries.size() == 1);
            const auto& entry = *entries[0];
            CHECK(entry.cell < kMaxPlugins);
            CHECK(entry.generation <= diagctx::kGenerationMask);
            CHECK(diagctx::instanceKindName(entry.kind) != nullptr);
            CHECK(diagctx::instanceOriginName(entry.origin) != nullptr);
            CHECK(diagctx::instanceModeName(entry.mode) != nullptr);
            CHECK(diagctx::instanceStateName(entry.state) != nullptr);
            for (const auto& text : entry.text) {
                CHECK(text.size() <= diagctx::kMaxText);
                CHECK(json::firstInvalidUtf8(text) == std::string::npos);
                CHECK(std::none_of(text.begin(), text.end(), [](char c) {
                    return static_cast<unsigned char>(c) < 0x20 ||
                           static_cast<unsigned char>(c) == 0x7f;
                }));
            }
            CHECK(pluginsShape(inventory.pluginsJson(nullptr), 0));
        }
    }
    // A packet of another kind or an event number outside the set is never applied.
    Packet other = good;
    other.kind = Message::Terminal;
    Inventory inventory;
    CHECK_FALSE(inventory.apply(other));
    other = good;
    other.instance.event = 0;
    CHECK_FALSE(inventory.apply(other));
    other.instance.event = 4;
    CHECK_FALSE(inventory.apply(other));
    CHECK(inventory.empty());
}

TEST_CASE("the inventory has one entry per cell, a ring of destroyed ones and no more") {
    Inventory inventory;
    for (std::uint32_t cell = 0; cell < kMaxPlugins; ++cell)
        REQUIRE(inventory.apply(add(cell, 0, InstanceState::Active)));
    CHECK(inventory.entries().size() == kMaxPlugins);
    CHECK_FALSE(
        inventory.apply(add(static_cast<std::uint32_t>(kMaxPlugins), 0, InstanceState::Active)));
    CHECK_FALSE(inventory.apply(add(0xffffffffU, 0, InstanceState::Active)));
    CHECK_FALSE(inventory.apply(add(0, 0x1000, InstanceState::Active)));  // 12-bit generations
    CHECK(inventory.entries().size() == kMaxPlugins);
    // 70 instances destroyed: the last 64 are kept, six fell out.
    for (std::uint32_t cell = 0; cell < kRetiredPlugins + 6; ++cell)
        REQUIRE(inventory.apply(change(cell, 0, EventKind::Remove, InstanceState::Destroyed)));
    CHECK(inventory.entries().size() == kMaxPlugins - 70 + kRetiredPlugins);
    CHECK(inventory.retiredLost() == 6);
    CHECK(inventory.find(0, 0) == nullptr);
    CHECK(inventory.find(69, 0) != nullptr);
    CHECK(pluginsShape(inventory.pluginsJson(nullptr), 0));
    bool lost = false;
    for (const auto& limit : inventory.limits())
        lost = lost || limit.find("last 64") != std::string::npos;
    CHECK(lost);
}

TEST_CASE("the worker's count of unsent events is kept, and said in the report") {
    Inventory inventory;
    CHECK(inventory.limits().empty());
    inventory.noteDropped(3);
    inventory.noteDropped(2);  // an older packet: the largest stays
    CHECK(inventory.workerDropped() == 3);
    REQUIRE(inventory.apply([] {
        auto p = add(1, 0, InstanceState::Active);
        p.dropped = 9;
        return p;
    }()));
    CHECK(inventory.workerDropped() == 9);
    CHECK(inventory.limits().size() == 1);
}

TEST_CASE("the report's plugins array names the module, the thread and the entry") {
    Inventory inventory;
    auto described = add(4, 2, InstanceState::Active, "Reverb", "Acme Audio");
    described.instance.image = 0x5100;  // inside the second module
    REQUIRE(inventory.apply(described));
    REQUIRE(inventory.apply(add(5, 0, InstanceState::Inactive, "Delay")));

    TraceSnapshot trace;
    trace.modules.resize(2);
    trace.modules[0].id = "aaaa";
    trace.modules[1].id = "0123456789abcdef";
    trace.regions = {{0x1000, 0x2000, 0}, {0x5000, 0x6000, 1}};
    trace.threads.resize(3);
    trace.threads[0].role = Role::Fault;
    trace.threads[1].role = Role::Main;
    trace.threads[2].role = Role::Other;
    trace.threads[0].entry = diagctx::Entry::Process;
    trace.threads[0].entryCell = 4;
    trace.threads[0].entryGeneration = 2;
    trace.threads[2].entry = diagctx::Entry::Reset;  // another thread, in the same instance
    trace.threads[2].entryCell = 4;
    trace.threads[2].entryGeneration = 2;
    trace.threads[1].entry = diagctx::Entry::State;  // a generation the inventory has not got
    trace.threads[1].entryCell = 4;
    trace.threads[1].entryGeneration = 3;

    const auto plugins = inventory.pluginsJson(&trace);
    REQUIRE(plugins.size() == 2);
    const auto& reverb = plugins.asArray()[0];
    CHECK(reverb.find("name")->asString() == "Reverb");
    CHECK(reverb.find("vendor")->asString() == "Acme Audio");
    CHECK(reverb.find("kind")->asString() == "clap");
    CHECK(reverb.find("state")->asString() == "active");
    REQUIRE(reverb.find("build"));
    CHECK(reverb.find("build")->find("id")->asString() == "0123456789abcdef");
    const auto* executing = reverb.find("executing");
    REQUIRE(executing);
    REQUIRE(executing->size() == 2);
    CHECK(executing->asArray()[0].find("thread")->asInt() == 0);
    CHECK(executing->asArray()[0].find("match")->asString() == "thread");
    CHECK(executing->asArray()[0].find("entry")->asString() == "process");
    CHECK(executing->asArray()[1].find("thread")->asInt() == 2);
    CHECK(executing->asArray()[1].find("match")->asString() == "other_thread");
    const auto& delay = plugins.asArray()[1];
    CHECK(delay.find("build") == nullptr);  // its image is in no module
    CHECK(delay.find("executing") == nullptr);
    CHECK(pluginsShape(plugins, trace.threads.size()));
    // Without a trace (no terminal capsule) there is no module and no thread to name.
    const auto bare = inventory.pluginsJson(nullptr);
    CHECK(bare.asArray()[0].find("build") == nullptr);
    CHECK(bare.asArray()[0].find("executing") == nullptr);
    CHECK(pluginsShape(bare, 0));
    // The private image address is never written.
    CHECK(json::writeCanonical(plugins).find("5100") == std::string::npos);
}

TEST_CASE("plug-in strings reach a report only bounded and clean, and nothing else of them does") {
    Inventory inventory;
    // A hostile description: control characters, an over-long vendor, a path-shaped id, a
    // canary in every field and text that is not UTF-8.
    auto hostile = add(1, 0, InstanceState::Active, std::string("CANARY\x1b[31m name\n"),
                       std::string(100, 'v'));
    setText(hostile, 2, std::string("1.0\xff\xfe"));
    setText(hostile, 3, std::string("../../CANARY/id"));
    REQUIRE(inventory.apply(hostile));
    const auto plugins = inventory.pluginsJson(nullptr);
    CHECK(pluginsShape(plugins, 0));
    const auto& entry = plugins.asArray()[0];
    CHECK(entry.find("name")->asString() == "CANARY?[31m name?");
    CHECK(entry.find("vendor")->asString().size() == diagctx::kMaxText);
    CHECK(entry.find("version")->asString() == "1.0??");
    // None of the hostile bytes reaches the file: the writer lays the document out with line
    // breaks of its own, so the check is for the bytes the plug-in sent.
    const auto text = json::writeCanonical(plugins);
    for (const char hostileByte : {'\x1b', '\xff', '\xfe'})
        CHECK(text.find(hostileByte) == std::string::npos);
    CHECK(text.find("../../CANARY/id") != std::string::npos);  // the id is text, not a path field
    // Only the stated keys exist.
    for (const auto& member : entry.asObject()) {
        const std::string_view allowed[]{"id",   "kind",   "origin",  "mode",     "state",
                                         "name", "vendor", "version", "plugin_id"};
        CHECK(std::find(std::begin(allowed), std::end(allowed), member.key) != std::end(allowed));
    }
}

TEST_CASE("the report validator accepts what the writer makes and refuses every other shape") {
    Inventory inventory;
    REQUIRE(inventory.apply(add(1, 0, InstanceState::Active)));
    REQUIRE(inventory.apply(add(2, 0, InstanceState::Loading)));
    const auto good = inventory.pluginsJson(nullptr);
    REQUIRE(pluginsShape(good, 0));
    REQUIRE(pluginsShape(json::Value::array(), 0));
    const auto mutated = [&](auto&& change) {
        auto value = good;
        change(value.asArray()[0]);
        return pluginsShape(value, 4);
    };
    CHECK_FALSE(mutated([](json::Value& v) { v["extra"] = 1; }));
    CHECK_FALSE(mutated([](json::Value& v) { v["kind"] = "rack_module"; }));
    CHECK_FALSE(mutated([](json::Value& v) { v["state"] = "weird"; }));
    CHECK_FALSE(mutated([](json::Value& v) { v["mode"] = 3; }));
    CHECK_FALSE(mutated([](json::Value& v) { v["id"] = -1; }));
    CHECK_FALSE(mutated([](json::Value& v) { v["id"] = 5; }));  // not increasing: the next is 1
    CHECK_FALSE(mutated([](json::Value& v) { v["name"] = std::string(65, 'x'); }));
    CHECK_FALSE(mutated([](json::Value& v) { v["name"] = std::string("a\x01"); }));
    CHECK_FALSE(mutated([](json::Value& v) { v["name"] = std::string("a\xc2\x85"); }));
    CHECK_FALSE(mutated([](json::Value& v) { v["vendor"] = 7; }));
    const auto withBuild = [&](const char* format, const char* id) {
        return mutated([&](json::Value& v) {
            auto build = json::Value::object();
            build["format"] = format;
            build["id"] = id;
            v["build"] = std::move(build);
        });
    };
    CHECK(withBuild("elf", "0123abcd"));
    CHECK_FALSE(withBuild("coff", "0123abcd"));
    CHECK_FALSE(withBuild("elf", ""));
    CHECK_FALSE(withBuild("elf", "XYZ"));
    CHECK_FALSE(withBuild("elf", "ABCD"));  // upper case: the writer never makes it
    const auto withExecuting = [&](long long thread, const char* match, const char* entry) {
        return mutated([&](json::Value& v) {
            auto active = json::Value::object();
            active["entry"] = entry;
            active["thread"] = thread;
            active["match"] = match;
            auto list = json::Value::array();
            list.push_back(std::move(active));
            v["executing"] = std::move(list);
        });
    };
    CHECK(withExecuting(3, "thread", "process"));
    CHECK_FALSE(withExecuting(4, "thread", "process"));  // there are four threads, 0 to 3
    CHECK_FALSE(withExecuting(-1, "thread", "process"));
    CHECK_FALSE(withExecuting(0, "fault", "process"));
    CHECK_FALSE(withExecuting(0, "thread", "mystery"));
    CHECK_FALSE(pluginsShape(json::Value("x"), 0));
    auto not_object = json::Value::array();
    not_object.push_back(1);
    CHECK_FALSE(pluginsShape(not_object, 0));
    auto many = json::Value::array();
    for (std::size_t i = 0; i <= kMaxPlugins + kRetiredPlugins; ++i)
        many.push_back(good.asArray()[0]);
    CHECK_FALSE(pluginsShape(many, 0));
}

TEST_CASE("a bridge's start, its instances and its end are kept, and the report lists them") {
    Inventory inventory;
    REQUIRE(inventory.apply(bridgeEvent(1)));
    REQUIRE(inventory.apply(addIn(1, 0, 1, "Reverb")));
    REQUIRE(inventory.apply(add(2, 0, InstanceState::Active, "Local")));  // in this process
    REQUIRE(inventory.apply(bridgeEvent(2)));
    REQUIRE(inventory.apply(addIn(3, 0, 2, "Delay")));
    REQUIRE(inventory.apply(addIn(4, 0, 1, "Chorus")));
    REQUIRE(inventory.apply(bridgeEvent(1, diagctx::BridgeHow::Signal, 11)));
    CHECK(inventory.rejected() == 0);
    const auto first = inventory.bridge(1);
    REQUIRE(first);
    CHECK(first->ended);
    CHECK(first->how == diagctx::BridgeHow::Signal);
    CHECK(first->status == 11);
    CHECK_FALSE(inventory.bridge(2)->ended);
    const auto plugins = inventory.pluginsJson(nullptr);
    CHECK(plugins.asArray()[0].find("bridge")->asInt() == 1);
    CHECK(plugins.asArray()[1].find("bridge") == nullptr);  // this process: no key
    CHECK(plugins.asArray()[2].find("bridge")->asInt() == 2);
    const auto bridges = inventory.bridgesJson();
    REQUIRE(bridges.size() == 2);
    const auto& one = bridges.asArray()[0];
    CHECK(one.find("id")->asInt() == 1);
    CHECK(one.find("state")->asString() == "ended");
    CHECK(one.find("ended")->find("how")->asString() == "signal");
    CHECK(one.find("ended")->find("code")->asInt() == 11);
    REQUIRE(one.find("instances")->size() == 2);  // Reverb (0) and Chorus (3), by id
    CHECK(one.find("instances")->asArray()[0].asInt() == 0);
    CHECK(one.find("instances")->asArray()[1].asInt() == 3);
    const auto& two = bridges.asArray()[1];
    CHECK(two.find("state")->asString() == "running");
    CHECK(two.find("ended") == nullptr);
    CHECK(pluginsShape(plugins, 0));
    CHECK(bridgesShape(bridges, plugins, 0));
    CHECK(inventory.droppedJson(nullptr).find("bridges")->asInt() == 0);
}

TEST_CASE("a bridge that was never announced is made by what names it, and an end is final") {
    Inventory inventory;
    // The start was lost: an instance in bridge 5, and an end for bridge 7, still say so.
    REQUIRE(inventory.apply(addIn(1, 0, 5)));
    REQUIRE(inventory.bridge(5));
    CHECK_FALSE(inventory.bridge(5)->ended);
    REQUIRE(inventory.apply(bridgeEvent(7, diagctx::BridgeHow::Exception, -1073741819)));
    REQUIRE(inventory.bridge(7));
    CHECK(inventory.bridge(7)->how == diagctx::BridgeHow::Exception);
    CHECK(inventory.bridge(7)->status == -1073741819);
    // A second start changes nothing; a second end does not overwrite the first.
    REQUIRE(inventory.apply(bridgeEvent(5)));
    CHECK_FALSE(inventory.bridge(5)->ended);
    REQUIRE(inventory.apply(bridgeEvent(7, diagctx::BridgeHow::Host, 0)));
    CHECK(inventory.bridge(7)->how == diagctx::BridgeHow::Exception);
    const auto ids = inventory.bridges();
    REQUIRE(ids.size() == 2);
    CHECK(ids[0]->id == 5);  // ascending, however they arrived
    CHECK(ids[1]->id == 7);
    CHECK(bridgesShape(inventory.bridgesJson(), inventory.pluginsJson(nullptr), 0));
}

TEST_CASE("a damaged bridge record is refused and changes nothing") {
    const Packet good = bridgeEvent(3, diagctx::BridgeHow::Exit, 1);
    {
        Inventory inventory;
        REQUIRE(inventory.apply(good));
    }
    const auto refused = [&](auto&& change) {
        Packet damaged = good;
        change(damaged.instance);
        Inventory inventory;
        const bool applied = inventory.apply(damaged);
        return !applied && inventory.bridges().empty() && inventory.rejected() == 1;
    };
    CHECK(refused([](InstanceRecord& r) { r.bridge = 0; }));
    CHECK(refused([](InstanceRecord& r) { r.cell = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.generation = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.kind = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.state = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.image = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.length[2] = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.how = diagctx::kBridgeHowCount; }));
    CHECK(refused([](InstanceRecord& r) { r.how = 0xff; }));
    CHECK(refused([](InstanceRecord& r) {  // a start has no status
        r.how = 0;
        r.status = 5;
    }));
    // The bridge fields have no meaning in the other events: an Add or a name chunk carrying
    // them is damaged, a bridge ordinal on an Add is not.
    Inventory inventory;
    auto withHow = add(1, 0, InstanceState::Active);
    withHow.instance.how = 2;
    CHECK_FALSE(inventory.apply(withHow));
    auto withStatus = add(1, 0, InstanceState::Active);
    withStatus.instance.status = 2;
    CHECK_FALSE(inventory.apply(withStatus));
    CHECK(inventory.apply(addIn(1, 0, 65535)));
    CHECK(inventory.bridge(65535));
}

TEST_CASE("a report lists at most 256 bridges: the oldest ended one falls out, counted") {
    Inventory inventory;
    for (std::uint16_t id = 1; id <= kMaxBridges; ++id) {
        REQUIRE(inventory.apply(bridgeEvent(id)));
        REQUIRE(inventory.apply(bridgeEvent(
            id, id == 1 || id == 2 ? diagctx::BridgeHow::Exit : diagctx::BridgeHow::None, 0)));
    }
    CHECK(inventory.bridges().size() == kMaxBridges);
    CHECK(inventory.bridgesLost() == 0);
    // Bridge 257 pushes out bridge 1 (the oldest that has ended); bridge 2 stays.
    REQUIRE(inventory.apply(bridgeEvent(kMaxBridges + 1)));
    CHECK(inventory.bridges().size() == kMaxBridges);
    CHECK(inventory.bridge(1) == nullptr);
    CHECK(inventory.bridge(2) != nullptr);
    CHECK(inventory.bridgesLost() == 1);
    CHECK(inventory.droppedJson(nullptr).find("bridges")->asInt() == 1);
    CHECK_FALSE(inventory.limits().empty());
    // An instance that names the lost bridge is still listed, and the report is still valid.
    REQUIRE(inventory.apply(addIn(1, 0, 1)));
    const auto plugins = inventory.pluginsJson(nullptr);
    CHECK(plugins.asArray()[0].find("bridge")->asInt() == 1);
    CHECK(bridgesShape(inventory.bridgesJson(), plugins, 0) == false);
    CHECK(bridgesShape(inventory.bridgesJson(), plugins,
                       inventory.droppedJson(nullptr).find("bridges")->asInt()));
}

TEST_CASE("an end that has no room among 256 running bridges is counted as a report not written") {
    Inventory inventory;
    for (std::uint16_t id = 1; id <= kMaxBridges; ++id) REQUIRE(inventory.apply(bridgeEvent(id)));
    // Every bridge is running, so there is nowhere to put another: the event is valid, counted in
    // `dropped.bridges`, and the bridge is never considered. An end that would have been reported
    // (a signal or an exception) is counted as the report it did not get; the others are not.
    REQUIRE(inventory.apply(bridgeEvent(kMaxBridges + 1)));  // a start
    CHECK(inventory.bridgeReportsSuppressed() == 0);
    REQUIRE(inventory.apply(bridgeEvent(kMaxBridges + 2, diagctx::BridgeHow::Exit, 1)));
    REQUIRE(inventory.apply(bridgeEvent(kMaxBridges + 3, diagctx::BridgeHow::Host, 0)));
    CHECK(inventory.bridgeReportsSuppressed() == 0);
    REQUIRE(inventory.apply(bridgeEvent(kMaxBridges + 4, diagctx::BridgeHow::Signal, 11)));
    CHECK(inventory.bridgeReportsSuppressed() == 1);
    REQUIRE(inventory.apply(bridgeEvent(kMaxBridges + 5, diagctx::BridgeHow::Exception, -1)));
    CHECK(inventory.bridgeReportsSuppressed() == 2);
    CHECK(inventory.bridgesLost() == 5);
    CHECK(inventory.droppedJson(nullptr).find("bridge_reports")->asInt() == 2);
    CHECK_FALSE(inventory.admitBridgeReport(kMaxBridges + 4));  // not there: nothing to write
}

TEST_CASE("the bridges validator accepts what the writer makes and refuses every other shape") {
    Inventory inventory;
    REQUIRE(inventory.apply(bridgeEvent(1)));
    REQUIRE(inventory.apply(addIn(1, 0, 1)));
    REQUIRE(inventory.apply(bridgeEvent(2, diagctx::BridgeHow::Host, 9)));
    const auto plugins = inventory.pluginsJson(nullptr);
    const auto bridges = inventory.bridgesJson();
    REQUIRE(bridgesShape(bridges, plugins, 0));
    REQUIRE(bridgesShape(json::Value::array(), json::Value::array(), 0));
    const auto mutated = [&](auto&& change) {
        auto value = bridges;
        change(value.asArray());
        return bridgesShape(value, plugins, 0);
    };
    using Items = json::Array;
    CHECK_FALSE(mutated([](Items& v) { v[0]["extra"] = 1; }));
    CHECK_FALSE(mutated([](Items& v) { v[0]["id"] = 0; }));
    CHECK_FALSE(mutated([](Items& v) { v[1]["id"] = 1; }));  // not rising
    CHECK_FALSE(mutated([](Items& v) { v[0]["id"] = 70000; }));
    CHECK_FALSE(mutated([](Items& v) { v[0]["state"] = "dead"; }));
    CHECK_FALSE(mutated([](Items& v) { v[0]["state"] = "ended"; }));    // ended needs `ended`
    CHECK_FALSE(mutated([](Items& v) { v[1]["state"] = "running"; }));  // running has none
    CHECK_FALSE(mutated([](Items& v) { v[1]["ended"]["how"] = "crashed"; }));
    CHECK_FALSE(mutated([](Items& v) { v[1]["ended"]["how"] = 3; }));
    CHECK_FALSE(mutated([](Items& v) { v[1]["ended"]["code"] = "9"; }));
    CHECK_FALSE(mutated([](Items& v) { v[1]["ended"]["code"] = 5000000000LL; }));
    CHECK_FALSE(mutated([](Items& v) { v[1]["ended"]["extra"] = 1; }));
    CHECK_FALSE(mutated([](Items& v) { v[0]["instances"] = json::Value::array(); }));  // unlisted
    CHECK_FALSE(mutated([](Items& v) { v[1]["instances"].asArray().push_back(0); }));  // not its
    CHECK_FALSE(mutated([](Items& v) { v[0]["instances"].asArray().push_back(9); }));  // absent
    CHECK_FALSE(mutated([](Items& v) { v[0]["instances"] = 4; }));
    CHECK_FALSE(mutated([](Items& v) { v[0].asObject().erase("instances"); }));
    CHECK_FALSE(mutated([](Items& v) { v.erase(v.begin()); }));  // the plug-in's bridge unlisted
    CHECK_FALSE(bridgesShape(json::Value("x"), plugins, 0));
    auto badPlugins = plugins;
    badPlugins.asArray()[0]["bridge"] = 0;
    CHECK_FALSE(pluginsShape(badPlugins, 0));
    badPlugins.asArray()[0]["bridge"] = 70000;
    CHECK_FALSE(pluginsShape(badPlugins, 0));
    badPlugins.asArray()[0]["bridge"] = "1";
    CHECK_FALSE(pluginsShape(badPlugins, 0));
}

TEST_CASE("the dropped counts accept an older report's seven and refuse anything unknown") {
    Inventory inventory;
    auto dropped = inventory.droppedJson(nullptr);
    CHECK(dropped.size() == 9);
    CHECK(droppedShape(dropped));
    dropped.asObject().erase("bridge_reports");
    CHECK(droppedShape(dropped));  // the commit that added only `bridges`
    dropped.asObject().erase("bridges");
    CHECK(droppedShape(dropped));  // what builds before topic 6 wrote
    dropped["bridges"] = -1;
    CHECK_FALSE(droppedShape(dropped));
    dropped["bridges"] = 0;
    dropped["mystery"] = 0;
    CHECK_FALSE(droppedShape(dropped));
    auto missing = inventory.droppedJson(nullptr);
    missing.asObject().erase("cells");
    CHECK_FALSE(droppedShape(missing));
}

namespace {
// A bridge with one member that has ended the way `how` says.
Inventory withEnded(std::uint16_t bridge, diagctx::BridgeHow how, std::int32_t status,
                    std::string_view name = "Reverb") {
    Inventory inventory;
    REQUIRE(inventory.apply(bridgeEvent(bridge)));
    REQUIRE(inventory.apply(addIn(bridge, 0, bridge, name)));
    REQUIRE(inventory.apply(bridgeEvent(bridge, how, status)));
    return inventory;
}
}  // namespace

TEST_CASE("only a bridge that died of a signal or an exception is reported, and only once") {
    using diagctx::BridgeHow;
    for (const auto how : {BridgeHow::Exit, BridgeHow::Host}) {
        auto inventory = withEnded(1, how, 9);
        CHECK_FALSE(inventory.admitBridgeReport(1));
        CHECK(inventory.bridgeReportsSuppressed() == 0);  // not turned away: never a candidate
    }
    auto inventory = withEnded(1, BridgeHow::Signal, 11);
    CHECK(inventory.admitBridgeReport(1));
    CHECK_FALSE(inventory.admitBridgeReport(1));  // decided on already
    CHECK(inventory.bridgeReportsSuppressed() == 0);
    CHECK_FALSE(inventory.admitBridgeReport(2));  // no such bridge
    // A bridge that is still running is not a candidate, and stays one for later.
    Inventory running;
    REQUIRE(running.apply(bridgeEvent(1)));
    CHECK_FALSE(running.admitBridgeReport(1));
    REQUIRE(running.apply(bridgeEvent(1, BridgeHow::Exception, -1073741819)));
    CHECK(running.admitBridgeReport(1));
}

TEST_CASE("a run writes at most three bridge reports, none that repeats one, and counts the rest") {
    using diagctx::BridgeHow;
    Inventory inventory;
    // The same plug-in dying the same way, over and over, is one report.
    for (std::uint16_t id = 1; id <= 4; ++id) {
        REQUIRE(inventory.apply(bridgeEvent(id)));
        REQUIRE(inventory.apply(addIn(id, 0, id, "Reverb")));
        REQUIRE(inventory.apply(bridgeEvent(id, BridgeHow::Signal, 11)));
        CHECK(inventory.admitBridgeReport(id) == (id == 1));
    }
    CHECK(inventory.bridgeReportsSuppressed() == 3);
    // Another status, or another plug-in, is a different death.
    REQUIRE(inventory.apply(bridgeEvent(5)));
    REQUIRE(inventory.apply(addIn(5, 0, 5, "Reverb")));
    REQUIRE(inventory.apply(bridgeEvent(5, BridgeHow::Signal, 6)));
    CHECK(inventory.admitBridgeReport(5));
    REQUIRE(inventory.apply(bridgeEvent(6)));
    auto delay = addIn(6, 0, 6, "Delay");
    setText(delay, 3, "com.acme.delay");
    REQUIRE(inventory.apply(delay));
    REQUIRE(inventory.apply(bridgeEvent(6, BridgeHow::Signal, 11)));
    CHECK(inventory.admitBridgeReport(6));
    CHECK(inventory.bridgeReportsSuppressed() == 3);
    // The limit: three were written, so every further kind of death is only counted.
    REQUIRE(inventory.apply(bridgeEvent(7)));
    auto chorus = addIn(7, 0, 7, "Chorus");
    setText(chorus, 3, "com.acme.chorus");
    REQUIRE(inventory.apply(chorus));
    REQUIRE(inventory.apply(bridgeEvent(7, BridgeHow::Exception, 3)));
    CHECK_FALSE(inventory.admitBridgeReport(7));
    CHECK(inventory.bridgeReportsSuppressed() == 4);
    CHECK(inventory.droppedJson(nullptr).find("bridge_reports")->asInt() == 4);
    CHECK_FALSE(inventory.limits().empty());
}

TEST_CASE("a bridge_exit attribution names the one member that was still there, or nobody") {
    using diagctx::BridgeHow;
    auto inventory = withEnded(1, BridgeHow::Signal, 11);
    auto one = inventory.bridgeExitAttributionJson(1);
    CHECK(one.find("confidence")->asString() == "bridge_exit");
    CHECK(one.find("plugin")->asInt() == 0);
    // A second live member: several remain, none is named.
    REQUIRE(inventory.apply(addIn(2, 0, 1, "Delay")));
    CHECK(inventory.bridgeExitAttributionJson(1).find("plugin")->isNull());
    // One of the two destroyed before the end: the other is the one.
    REQUIRE(inventory.apply(change(1, 0, EventKind::Remove, InstanceState::Destroyed)));
    CHECK(inventory.bridgeExitAttributionJson(1).find("plugin")->asInt() == 1);
    // A bridge with no member names nobody.
    Inventory empty;
    REQUIRE(empty.apply(bridgeEvent(3, BridgeHow::Signal, 9)));
    CHECK(empty.bridgeExitAttributionJson(3).find("plugin")->isNull());
}

TEST_CASE("the attribution validator holds bridge_exit to the report's own bridge") {
    using diagctx::BridgeHow;
    auto inventory = withEnded(2, BridgeHow::Signal, 11);
    REQUIRE(inventory.apply(addIn(5, 0, 2, "Delay")));  // two live members: nobody is named
    const auto plugins = inventory.pluginsJson(nullptr);
    const auto none = json::Value::array();
    const auto attribution = [](const json::Value& plugin) {
        auto a = json::Value::object();
        a["confidence"] = "bridge_exit";
        a["plugin"] = plugin;
        return a;
    };
    const auto valid = [&](const json::Value& a, std::int64_t bridge) {
        return attributionShape(a, plugins, none, none, nullptr, bridge);
    };
    CHECK(valid(attribution(json::Value()), 2));
    CHECK(valid(attribution(json::Value(0LL)), 2));        // a member of bridge 2
    CHECK_FALSE(valid(attribution(json::Value(0LL)), 3));  // not of bridge 3
    CHECK_FALSE(valid(attribution(json::Value(9LL)), 2));  // not listed
    CHECK_FALSE(valid(attribution(json::Value()), 0));     // only a bridge_exit report has it
    // And no other level belongs to a bridge_exit report.
    auto token = json::Value::object();
    token["confidence"] = "unknown";
    token["plugin"] = json::Value();
    CHECK_FALSE(valid(token, 2));
    CHECK(valid(token, 0));
    // A destroyed member is not one that was there when it ended.
    REQUIRE(inventory.apply(change(2, 0, EventKind::Remove, InstanceState::Destroyed)));
    CHECK_FALSE(attributionShape(attribution(json::Value(0LL)), inventory.pluginsJson(nullptr),
                                 none, none, nullptr, 2));
}

TEST_CASE("a bridge_exit report's bridge is one listed as ended by a signal or an exception") {
    using diagctx::BridgeHow;
    for (const auto how : {BridgeHow::Signal, BridgeHow::Exception}) {
        const auto inventory = withEnded(4, how, -7);
        CHECK(bridgeExitShape(inventory.bridgesJson(), 4, -7));
        CHECK_FALSE(bridgeExitShape(inventory.bridgesJson(), 4, 7));   // not the code it says
        CHECK_FALSE(bridgeExitShape(inventory.bridgesJson(), 5, -7));  // not listed
    }
    CHECK_FALSE(bridgeExitShape(withEnded(4, BridgeHow::Host, 9).bridgesJson(), 4, 9));
    CHECK_FALSE(bridgeExitShape(withEnded(4, BridgeHow::Exit, 0).bridgesJson(), 4, 0));
    Inventory running;
    REQUIRE(running.apply(bridgeEvent(4)));
    CHECK_FALSE(bridgeExitShape(running.bridgesJson(), 4, 0));
    CHECK_FALSE(bridgeExitShape(json::Value("x"), 4, 0));
}

TEST_CASE("the channel accepts inventory packets and refuses what it did not write") {
    Packet packet = base();
    packet.incident[32] = 0;
    packet.role = 3;
    CHECK(valid(packet));
    packet.kind = static_cast<Message>(7);
    CHECK_FALSE(valid(packet));
    packet.kind = Message::Instance;
    packet.version = 1;  // the previous protocol
    CHECK_FALSE(valid(packet));
    packet.version = kProtocolVersion + 1;
    CHECK_FALSE(valid(packet));
    CHECK(sizeof(Packet) <= 512);
}

TEST_CASE("the Windows inventory ring counts full and empty across the counter's wrap") {
    CHECK(ringHasRoom(0, 0));
    CHECK_FALSE(ringHasData(0, 0));
    CHECK(ringHasData(1, 0));
    CHECK(ringHasRoom(kInventoryRing - 1, 0));
    CHECK_FALSE(ringHasRoom(kInventoryRing, 0));
    const std::uint32_t nearEnd = 0xffffffffU - 3;  // head wrapped past zero, tail has not
    const std::uint32_t head = nearEnd + 10;
    CHECK(head < nearEnd);
    CHECK(ringHasData(head, nearEnd));
    CHECK(ringHasRoom(head, nearEnd));
    CHECK_FALSE(ringHasRoom(nearEnd + kInventoryRing, nearEnd));
    CHECK(ringSlot(nearEnd) < kInventoryRing);
    CHECK(ringSlot(7) == 7);
    CHECK(ringSlot(kInventoryRing + 7) == 7);
}

#if !defined(_WIN32)
TEST_CASE("a full channel drops whole packets and never tears one") {
    int pair[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Native sender{pair[0]};
    Native receiver{pair[1]};
    receiver.exited = true;  // receive() then drains what is queued and ends, without waitpid
    // The worker's descriptor is blocking, as launch() leaves it. publish() must still refuse
    // a full channel instead of waiting (macOS decides from the socket's own state, not from
    // MSG_DONTWAIT; a hang here is that), and must hand the descriptor back as it found it.
    const int before = ::fcntl(pair[0], F_GETFL);
    REQUIRE(before >= 0);
    REQUIRE((before & O_NONBLOCK) == 0);
    std::uint32_t sent = 0;
    for (; sent < 100000; ++sent) {
        auto packet = add(sent % kMaxPlugins, 0, InstanceState::Active);
        packet.instance.image = sent;
        if (!publish(sender, packet)) break;
    }
    REQUIRE(sent > 10);      // something fit
    REQUIRE(sent < 100000);  // and the buffer did fill: the send refused instead of waiting
    CHECK(::fcntl(pair[0], F_GETFL) == before);
    std::uint32_t received = 0;
    Packet packet;
    while (receive(receiver, packet)) {
        CHECK(packet.kind == Message::Instance);
        CHECK(packet.instance.image == received);  // in order, none torn or lost in between
        ++received;
    }
    CHECK(received == sent);
    ::close(pair[0]);
    ::close(pair[1]);
}
#endif

namespace {
int gHookCalls = 0;
bool gHookTears = false;
bool hookPublish(Native& native, const Packet&) noexcept {
    ++gHookCalls;
    if (gHookTears) native.torn = true;  // what publish() says when a part-sent packet is stuck
    return false;
}
}  // namespace

TEST_CASE("a torn stream closes the sink for good; a merely full one does not") {
    Native native;
    Packet boot = base();
    resetInventoryForTests(&hookPublish);
    installInventory(native, boot);
    gHookCalls = 0;

    gHookTears = false;  // a full channel: this event is lost, the next may still go
    diagctx::announceState(1, diagctx::InstanceState::Active);
    diagctx::announceState(1, diagctx::InstanceState::Inactive);
    CHECK(gHookCalls == 2);
    CHECK(inventoryDropped() == 2);

    gHookTears = true;  // a packet stuck half sent: the process waited once, never again
    diagctx::announceState(1, diagctx::InstanceState::Active);
    CHECK(gHookCalls == 3);
    for (int i = 0; i < 5; ++i) diagctx::announceState(1, diagctx::InstanceState::Inactive);
    diagctx::announceRemove(1);
    CHECK(gHookCalls == 3);  // nothing was sent, nothing waited
    CHECK(inventoryDropped() == 2 + 1 + 5 + 1);
    resetInventoryForTests(nullptr);
}

namespace {
std::atomic<bool> gSlowStarted{false}, gSlowFinished{false};
// A send that takes a while, as one finishing a part-sent packet does on macOS (up to 250 ms).
bool slowPublish(Native&, const Packet&) noexcept {
    gSlowStarted.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    gSlowFinished.store(true);
    return true;
}
}  // namespace

TEST_CASE("closing the inventory waits out a send as long as a part-sent packet can take") {
    Native native;
    Packet boot = base();
    resetInventoryForTests(&slowPublish);
    installInventory(native, boot);
    gSlowStarted.store(false);
    gSlowFinished.store(false);
    std::thread announcer([] { diagctx::announceState(1, diagctx::InstanceState::Active); });
    while (!gSlowStarted.load()) std::this_thread::yield();
    // The terminal packet goes out right after this returns: the send in progress must be done
    // by then, or the two packets interleave in the stream (the wait used to give up at 100 ms).
    // The wait is bounded (250 ms for the send and 50 ms for scheduling it), so a send that a
    // loaded machine delays past that bound is correctly given up on: the check is that the close
    // returned either with the send done or not before the whole bound had passed, whatever the
    // machine's load did to the 160 ms the send takes.
    const std::uint64_t before = monotonicNs();
    REQUIRE(before != 0);
    CHECK(closeInventory());
    const std::uint64_t waitedNs = monotonicNs() - before;
    const bool finished = gSlowFinished.load();
    CHECK((finished || waitedNs >= 300ULL * 1'000'000ULL));
    announcer.join();
    CHECK(gSlowFinished.load());
    resetInventoryForTests(nullptr);
}

namespace {
std::atomic<bool> gCutStarted{false};
std::atomic<std::uint64_t> gCutSeen{0};
// A send in progress that waits to be told when it must stop, as publish() does while it
// finishes a part-sent packet: it reads the deadline the close publishes.
bool cutoffPublish(Native&, const Packet&) noexcept {
    gCutStarted.store(true);
    for (int i = 0; i < 5000 && partSendCutoffNs() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    gCutSeen.store(partSendCutoffNs());
    return true;
}
}  // namespace

TEST_CASE("closing the inventory publishes one deadline for the sends it waits out") {
    Native native;
    Packet boot = base();
    resetInventoryForTests(&cutoffPublish);
    installInventory(native, boot);
    CHECK(partSendCutoffNs() == 0);  // nothing is closing
    gCutStarted.store(false);
    gCutSeen.store(0);
    std::thread announcer([] { diagctx::announceState(1, diagctx::InstanceState::Active); });
    while (!gCutStarted.load()) std::this_thread::yield();
    const std::uint64_t before = monotonicNs();
    REQUIRE(before != 0);
    CHECK(closeInventory());
    announcer.join();
    // The send saw the deadline while the close was waiting for it: kPartSendPolls ms after the
    // close began, by the clock publish() reads, and the close did not return before the send did.
    const std::uint64_t seen = gCutSeen.load();
    CHECK(seen >= before + static_cast<std::uint64_t>(kPartSendPolls) * 1'000'000ULL);
    CHECK(seen <= monotonicNs() + static_cast<std::uint64_t>(kPartSendPolls) * 1'000'000ULL);
    resetInventoryForTests(nullptr);
    CHECK(partSendCutoffNs() == 0);  // a reset forgets it
}
#if !defined(_WIN32)
// The real part-send loop of publish(), driven on a socket pair (round 6a, F4). The test above
// swaps publish out, so it tests only that the close publishes its deadline; this one tests that
// the loop that finishes a part-sent packet obeys it. The packet's first bytes are "already
// sent"; the rest could go at once (the buffer is empty), so the only thing that can stop it is
// the deadline, and it is already past when the loop starts: an ordering check, not a timing one.
namespace {
struct SocketPair {
    int fd[2]{-1, -1};
    SocketPair() { REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0); }
    ~SocketPair() {
        for (const int one : fd)
            if (one >= 0) ::close(one);
    }
    // What is waiting to be read on the far end, without blocking.
    std::vector<char> pending() const {
        std::vector<char> got(sizeof(Packet) + 16);
        const auto n = ::recv(fd[1], got.data(), got.size(), MSG_DONTWAIT);
        got.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        return got;
    }
};
Packet patterned() {
    Packet packet = base();
    for (std::size_t i = 0; i < sizeof(packet.instance.text[0]); ++i)
        packet.instance.text[0][i] = static_cast<char>('a' + i % 26);
    return packet;
}
}  // namespace

TEST_CASE(
    "a part-sent packet is finished by the real loop unless the close's deadline has passed") {
    constexpr std::size_t kSent = 10;  // as if the first send had taken only this much
    const Packet packet = patterned();
    const auto* bytes = reinterpret_cast<const char*>(&packet);
    const std::uint64_t now = monotonicNs();
    REQUIRE(now > 10'000'000'000ULL);
    resetInventoryForTests(nullptr);
    {
        // No close in progress: the rest goes out, whole and in order.
        SocketPair pair;
        Native native;
        native.channel = pair.fd[0];
        CHECK(finishPartSend(native, packet, kSent));
        CHECK_FALSE(native.torn);
        const auto got = pair.pending();
        REQUIRE(got.size() == sizeof(Packet) - kSent);
        CHECK(std::equal(got.begin(), got.end(), bytes + kSent));
    }
    {
        // A deadline still ahead does not stop it either.
        SocketPair pair;
        Native native;
        native.channel = pair.fd[0];
        setPartSendCutoffForTests(now + 10'000'000'000ULL);
        CHECK(finishPartSend(native, packet, kSent));
        CHECK_FALSE(native.torn);
        CHECK(pair.pending().size() == sizeof(Packet) - kSent);
    }
    {
        // A deadline already past stops it before it sends anything more: the stream is torn, and
        // nothing follows the part already sent (the terminal packet must not land inside this one,
        // and the close has given up waiting for it).
        SocketPair pair;
        Native native;
        native.channel = pair.fd[0];
        setPartSendCutoffForTests(now - 1);
        CHECK_FALSE(finishPartSend(native, packet, kSent));
        CHECK(native.torn);
        CHECK(pair.pending().empty());
    }
    resetInventoryForTests(nullptr);
}
#endif

TEST_CASE("closing the inventory says when a send left the stream torn") {
    Native native;
    Packet boot = base();
    resetInventoryForTests(&hookPublish);
    installInventory(native, boot);
    gHookTears = false;
    diagctx::announceState(1, diagctx::InstanceState::Active);  // lost, but the stream is whole
    CHECK(closeInventory());
    resetInventoryForTests(&hookPublish);
    installInventory(native, boot);
    gHookTears = true;
    diagctx::announceState(1, diagctx::InstanceState::Active);
    CHECK_FALSE(closeInventory());  // the terminal packet must not be sent into it
    gHookTears = false;
    resetInventoryForTests(nullptr);
}

namespace {
std::vector<Packet> gSent;
bool capturePublish(Native&, const Packet& packet) noexcept {
    gSent.push_back(packet);
    return true;
}
}  // namespace

TEST_CASE("the worker sends a bridge's start, its end and the bridge each instance runs in") {
    Native native;
    Packet boot = base();
    resetInventoryForTests(&capturePublish);
    installInventory(native, boot);
    gSent.clear();
    diagctx::announceBridge(1);
    diagctx::announce(4, diagctx::InstanceKind::Clap, diagctx::InstanceOrigin::Foreign,
                      diagctx::InstanceMode::Bridged, InstanceState::Active, 0,
                      {"Reverb", {}, {}, {}}, 1);
    diagctx::announce(5, diagctx::InstanceKind::Clap, diagctx::InstanceOrigin::Foreign,
                      diagctx::InstanceMode::InProcess, InstanceState::Active, 0,
                      {"Local", {}, {}, {}});
    const std::string minted = diagctx::announceBridge(1, diagctx::BridgeHow::Signal, 11);
    CHECK(diagctx::announceBridge(0).empty());  // not a bridge: ignored
    CHECK(diagctx::announceBridge(2, diagctx::BridgeHow::None, 99).empty());  // no status, no id
    resetInventoryForTests(nullptr);
    REQUIRE(gSent.size() == 5);
    // The end by a signal names the report it may cause: the worker minted the id, and the packet
    // carries it, and the supervisor keeps it.
    REQUIRE(minted.size() == 32);
    CHECK(gSent[3].instance.length[3] == 32);
    CHECK(std::string(gSent[3].instance.text[3], 32) == minted);
    for (const std::size_t other : {0u, 1u, 2u, 4u}) CHECK(gSent[other].instance.length[3] == 0);
    CHECK(gSent[0].instance.event == static_cast<std::uint8_t>(EventKind::Bridge));
    CHECK(gSent[0].instance.bridge == 1);
    CHECK(gSent[0].instance.how == 0);
    CHECK(gSent[1].instance.bridge == 1);
    CHECK(gSent[2].instance.bridge == 0);
    CHECK(gSent[3].instance.how == static_cast<std::uint8_t>(diagctx::BridgeHow::Signal));
    CHECK(gSent[3].instance.status == 11);
    CHECK(gSent[4].instance.bridge == 2);
    CHECK(gSent[4].instance.status == 0);
    // What the worker sent is what the supervisor's inventory accepts, whole.
    Inventory inventory;
    for (const auto& packet : gSent) CHECK(inventory.apply(packet));
    CHECK(inventory.rejected() == 0);
    CHECK(inventory.bridge(1)->ended);
    CHECK(inventory.bridge(1)->incident == minted);
    CHECK(inventory.find(4, 0)->bridge == 1);
    CHECK(inventory.find(5, 0)->bridge == 0);
    CHECK(bridgesShape(inventory.bridgesJson(), inventory.pluginsJson(nullptr), 0));
}

namespace {
Packet nameChunk(std::uint32_t first, std::initializer_list<std::string_view> names) {
    Packet p = base();
    p.instance.event = static_cast<std::uint8_t>(diagctx::EventKind::Names);
    p.instance.cell = first;
    std::size_t i = 0;
    for (const auto name : names) setText(p, i++, name);
    return p;
}
TraceSnapshot traceWithCommands(std::initializer_list<diagctx::CommandCrumb> crumbs) {
    TraceSnapshot trace;
    trace.table = TableState::Read;
    trace.commands.assign(crumbs.begin(), crumbs.end());
    return trace;
}
}  // namespace

TEST_CASE("command names arrive four at a time, are checked, and map ids to names") {
    Inventory inventory;
    CHECK(inventory.commandName(0) == nullptr);
    REQUIRE(inventory.apply(nameChunk(0, {"track.add", "track.set-volume", "", "transport.play"})));
    REQUIRE(inventory.apply(nameChunk(8, {"a.b", "c.d"})));  // the chunk at 4 never came
    CHECK(*inventory.commandName(0) == "track.add");
    CHECK(*inventory.commandName(1) == "track.set-volume");
    CHECK(inventory.commandName(2) == nullptr);  // no name in that slot
    CHECK(*inventory.commandName(3) == "transport.play");
    CHECK(inventory.commandName(5) == nullptr);  // its chunk was lost: unknown, never invented
    CHECK(*inventory.commandName(9) == "c.d");
    CHECK(inventory.commandName(1u << 20) == nullptr);
    CHECK(inventory.rejected() == 0);
    // The same slot sent again replaces the name; the plug-in side is untouched by names.
    REQUIRE(inventory.apply(nameChunk(0, {"track.remove"})));
    CHECK(*inventory.commandName(0) == "track.remove");
    CHECK(inventory.empty());  // names are not plug-ins
}

TEST_CASE("a damaged name chunk is counted and teaches nothing") {
    Inventory inventory;
    std::size_t expected = 0;
    const auto refuse = [&](Packet p) {
        CHECK_FALSE(inventory.apply(p));
        CHECK(inventory.rejected() == ++expected);
    };
    refuse(nameChunk(2, {"a.b"}));                          // not a multiple of four
    refuse(nameChunk(diagctx::kMaxCommandNames, {"a.b"}));  // past the table
    refuse(nameChunk(0, {}));                               // nothing in it
    auto generation = nameChunk(0, {"a.b"});
    generation.instance.generation = 1;
    refuse(generation);
    auto image = nameChunk(0, {"a.b"});
    image.instance.image = 1;
    refuse(image);
    auto kind = nameChunk(0, {"a.b"});
    kind.instance.kind = 1;
    refuse(kind);
    auto fifth = nameChunk(0, {"a.b"});
    setText(fifth, 3, "x");
    fifth.instance.length[3] = 0;
    fifth.instance.length[2] = 65;  // longer than a slot's 64 bytes
    refuse(fifth);
    CHECK(inventory.commandName(0) == nullptr);
    // A name that is not a command name is left unknown but the rest of its chunk is kept.
    REQUIRE(inventory.apply(nameChunk(4, {"Has Space", "ok.name", "../etc", "ok-2"})));
    CHECK(inventory.commandName(4) == nullptr);
    CHECK(*inventory.commandName(5) == "ok.name");
    CHECK(inventory.commandName(6) == nullptr);
    CHECK(*inventory.commandName(7) == "ok-2");
    CHECK(inventory.rejected() == expected + 2);
}

TEST_CASE(
    "every byte of a name chunk, replaced, makes a chunk the inventory vouches for or refuses") {
    const auto good = nameChunk(4, {"track.add", "item.apply-fx", "x", "y.z"});
    for (std::size_t at = 0; at < sizeof(InstanceRecord); ++at)
        for (const unsigned char value : {0x00, 0x01, 0x20, 0x41, 0x7f, 0x80, 0xff}) {
            Packet damaged = good;
            reinterpret_cast<unsigned char*>(&damaged.instance)[at] = value;
            Inventory inventory;
            (void)inventory.apply(damaged);
            for (std::uint32_t id = 0; id < 16; ++id)
                if (const auto* name = inventory.commandName(id))
                    CHECK(diagctx::validCommandName(*name));  // whatever it keeps is a valid name
            CHECK(inventory.empty());
        }
}

TEST_CASE("the report's commands array names each entry, or says unknown, and validates strictly") {
    Inventory inventory;
    REQUIRE(inventory.apply(nameChunk(0, {"track.add", "transport.play"})));
    using diagctx::CommandCrumb;
    using diagctx::CommandOrigin;
    using diagctx::CommandState;
    const auto trace = traceWithCommands({{5, 0, CommandOrigin::Mcp, CommandState::Ended},
                                          {6, 1, CommandOrigin::Ui, CommandState::Ended},
                                          {7, 3, CommandOrigin::Script, CommandState::Begun}});
    const auto commands = inventory.commandsJson(&trace);
    REQUIRE(commands.size() == 3);
    const auto& list = commands.asArray();
    CHECK(list[0].find("seq")->asInt() == 5);
    CHECK(list[0].find("name")->asString() == "track.add");
    CHECK(list[0].find("origin")->asString() == "mcp");
    CHECK(list[0].find("state")->asString() == "ended");
    CHECK(list[1].find("name")->asString() == "transport.play");
    CHECK(list[2].find("name")->asString() == "unknown");  // id 3 was never announced
    CHECK(list[2].find("origin")->asString() == "script");
    CHECK(list[2].find("state")->asString() == "begun");
    CHECK(commandsShape(commands));
    CHECK(inventory.commandsJson(nullptr).size() == 0);
    CHECK(commandsShape(json::Value::array()));

    // Limits: always the standing one; more for damage, refusals and unnamed ids.
    auto limits = inventory.commandLimits(&trace);
    CHECK(limits.size() == 2);  // the standing text and "no name for id 3"
    auto noisy = trace;
    noisy.commandsDamaged = 1;
    noisy.commandsRefused = 2;
    CHECK(inventory.commandLimits(&noisy).size() == 4);
    TraceSnapshot unread;
    CHECK(inventory.commandLimits(&unread).size() == 1);
    CHECK(inventory.commandLimits(nullptr).size() == 1);

    // A capsule's table counts as read (the worker wrote it from its own), so the report says what
    // the listed commands are, and never that no table could be read.
    TraceSnapshot capsule;
    capsule.table = TableState::Read;
    capsule.commands = trace.commands;
    const auto readLimits = Inventory().commandLimits(&capsule);
    REQUIRE_FALSE(readLimits.empty());
    CHECK(readLimits[0].find("last 64 the model thread began") != std::string::npos);
    for (const auto& limit : readLimits)
        CHECK(limit.find("could not be read") == std::string::npos);

    // Shapes the writer never makes.
    const auto parse = [](std::string_view text) { return json::parse(text).value(); };
    const char* good = R"({"seq":1,"name":"a.b","origin":"ui","state":"begun"})";
    CHECK(commandsShape(parse(std::string("[") + good + "]")));
    const char* bad[] = {
        R"({"seq":0,"name":"a.b","origin":"ui","state":"begun"})",  // seq starts at 1
        R"({"seq":1,"name":"A.b","origin":"ui","state":"begun"})",  // not a command name
        R"({"seq":1,"name":"a b","origin":"ui","state":"begun"})",
        R"({"seq":1,"name":"","origin":"ui","state":"begun"})",
        R"({"seq":1,"name":"a.b","origin":"robot","state":"begun"})",  // closed sets
        R"({"seq":1,"name":"a.b","origin":"ui","state":"running"})",
        R"({"seq":1,"name":"a.b","origin":"ui"})",
        R"({"seq":1,"name":"a.b","origin":"ui","state":"begun","args":{}})",  // only four keys
        R"({"seq":"1","name":"a.b","origin":"ui","state":"begun"})",
        R"(["a.b"])"};
    for (const char* entry : bad) CHECK_FALSE(commandsShape(parse(std::string("[") + entry + "]")));
    // Not ascending, repeated, or more than 64.
    CHECK_FALSE(commandsShape(parse(std::string("[") + good + "," + good + "]")));
    std::string many = "[";
    for (int i = 1; i <= 65; ++i)
        many += std::string(i > 1 ? "," : "") + "{\"seq\":" + std::to_string(i) +
                ",\"name\":\"a.b\",\"origin\":\"ui\",\"state\":\"ended\"}";
    CHECK_FALSE(commandsShape(parse(many + "]")));
    CHECK_FALSE(commandsShape(parse("{}")));
}

namespace {
// A snapshot whose fault thread is at `faultModule` (-1: nowhere) and which holds two modules.
TraceSnapshot faultTrace(int faultModule) {
    TraceSnapshot trace;
    trace.table = TableState::Read;
    trace.modules.resize(2);
    trace.modules[0].id = "aaaa";
    trace.modules[1].id = "0123456789abcdef";
    trace.regions = {{0x1000, 0x2000, 0}, {0x5000, 0x6000, 1}};
    trace.threads.resize(3);
    trace.threads[0].role = Role::Fault;
    trace.threads[0].missing = Missing::None;  // walked: the report lists its frames
    trace.threads[1].role = Role::Main;
    trace.threads[2].role = Role::Other;
    Frame frame;
    frame.module = faultModule;
    trace.threads[0].walk.frames.push_back(frame);
    return trace;
}
Packet addAt(std::uint32_t cell, std::uint64_t image, InstanceState state = InstanceState::Active,
             std::uint8_t kind = static_cast<std::uint8_t>(diagctx::InstanceKind::Clap)) {
    auto p = add(cell, 0, state);
    p.instance.image = image;
    p.instance.kind = kind;
    return p;
}
void inside(TraceSnapshot& trace, std::size_t thread, std::uint32_t cell) {
    trace.threads[thread].entry = diagctx::Entry::Process;
    trace.threads[thread].entryCell = cell;
    trace.threads[thread].entryGeneration = 0;
}
// The attribution of a trace, checked for the validator's agreement on the way.
std::pair<std::string, long long> attributed(const Inventory& inventory,
                                             const TraceSnapshot& trace) {
    const auto attribution = inventory.attributionJson(&trace);
    const auto modules = modulesJson(trace);
    CHECK(attributionShape(attribution, inventory.pluginsJson(&trace),
                           inventory.commandsJson(&trace), threadsJson(trace), &modules));
    return {attribution.find("confidence")->asString(),
            attribution.find("plugin")->isNull() ? -1 : attribution.find("plugin")->asInt()};
}
}  // namespace

TEST_CASE("attribution: the faulting instruction in a plug-in's binary names that plug-in") {
    Inventory inventory;
    REQUIRE(inventory.apply(addAt(4, 0x5100)));  // id 0
    REQUIRE(inventory.apply(addAt(5, 0x1100)));  // id 1, in the other module
    const auto trace = faultTrace(1);
    CHECK(attributed(inventory, trace) == std::pair<std::string, long long>{"fault_in_image", 0});
    // The program's own module, or none at all, is not a plug-in's.
    CHECK(attributed(inventory, faultTrace(-1)).first == "unknown");
    CHECK(attributed(inventory, faultTrace(0)) ==
          std::pair<std::string, long long>{"fault_in_image", 1});
}

TEST_CASE(
    "attribution: several instances of one binary are told apart by the token, then by being "
    "alive") {
    Inventory inventory;
    REQUIRE(inventory.apply(addAt(4, 0x5100)));  // id 0
    REQUIRE(inventory.apply(addAt(5, 0x5200)));  // id 1, the same binary
    auto trace = faultTrace(1);
    // Two alive and nothing to choose between them: the image is the evidence, no plug-in named.
    CHECK(attributed(inventory, trace) == std::pair<std::string, long long>{"fault_in_image", -1});
    inside(trace, 0, 5);  // the faulting thread is inside the second one
    CHECK(attributed(inventory, trace) == std::pair<std::string, long long>{"fault_in_image", 1});
    // One gone: the one left alive.
    Packet remove = base();
    remove.instance.event = static_cast<std::uint8_t>(diagctx::EventKind::Remove);
    remove.instance.cell = 4;
    REQUIRE(inventory.apply(remove));
    CHECK(attributed(inventory, faultTrace(1)) ==
          std::pair<std::string, long long>{"fault_in_image", 1});
}

TEST_CASE("attribution: a library entry is blamed only when no instance can be") {
    Inventory inventory;
    const auto library = static_cast<std::uint8_t>(diagctx::InstanceKind::ClapLibrary);
    REQUIRE(inventory.apply(addAt(1, 0x5100, InstanceState::Active, library)));  // id 0
    CHECK(attributed(inventory, faultTrace(1)) ==
          std::pair<std::string, long long>{"fault_in_image", 0});
    REQUIRE(inventory.apply(addAt(2, 0x5100)));  // id 1: an instance from it
    CHECK(attributed(inventory, faultTrace(1)) ==
          std::pair<std::string, long long>{"fault_in_image", 1});
}

TEST_CASE("attribution: a token, concurrent activity, a recent command, or nothing") {
    Inventory inventory;
    REQUIRE(inventory.apply(addAt(4, 0x1100)));  // id 0: in no plug-in module the fault is in
    REQUIRE(inventory.apply(addAt(5, 0x1200)));  // id 1
    auto trace = faultTrace(0);
    trace.modules[0].id.reset();  // the fault is in a module with no identity
    CHECK(attributed(inventory, trace).first == "unknown");
    trace.commands = {{3, 0, diagctx::CommandOrigin::Mcp, diagctx::CommandState::Begun}};
    CHECK(attributed(inventory, trace) == std::pair<std::string, long long>{"recent_command", -1});
    inside(trace, 2, 5);  // another thread is inside instance 1
    CHECK(attributed(inventory, trace) ==
          std::pair<std::string, long long>{"concurrent_activity", 1});
    inside(trace, 1, 4);  // and a second thread inside instance 0: activity, nobody blamed
    CHECK(attributed(inventory, trace) ==
          std::pair<std::string, long long>{"concurrent_activity", -1});
    inside(trace, 0, 4);  // the faulting thread itself
    CHECK(attributed(inventory, trace) == std::pair<std::string, long long>{"active_token", 0});
    // A token for an instance the inventory never heard of names nobody.
    inside(trace, 0, 900);
    trace.threads[1].entry = diagctx::Entry::None;
    trace.threads[2].entry = diagctx::Entry::None;
    CHECK(attributed(inventory, trace) == std::pair<std::string, long long>{"recent_command", -1});
    // No capsule, or one that does not start with the fault thread: nothing is concluded.
    CHECK(inventory.attributionJson(nullptr).find("confidence")->asString() == "unknown");
    TraceSnapshot odd = trace;
    odd.threads[0].role = Role::Main;
    CHECK(attributed(inventory, odd).first == "unknown");
}

TEST_CASE("the attribution validator refuses a claim the report's own evidence does not support") {
    Inventory inventory;
    REQUIRE(inventory.apply(addAt(4, 0x5100)));
    auto trace = faultTrace(1);
    inside(trace, 2, 4);  // another thread is inside it
    const auto plugins = inventory.pluginsJson(&trace);
    auto commands = inventory.commandsJson(&trace);
    const auto parse = [](std::string_view text) { return json::parse(text).value(); };
    const auto threads = threadsJson(trace);
    const auto modules = modulesJson(trace);
    const auto accepts = [&](const char* text) {
        return attributionShape(parse(text), plugins, commands, threads, &modules);
    };
    CHECK(accepts(R"({"confidence":"fault_in_image","plugin":0})"));
    CHECK(accepts(R"({"confidence":"fault_in_image","plugin":null})"));
    CHECK(accepts(R"({"confidence":"concurrent_activity","plugin":0})"));
    CHECK(accepts(R"({"confidence":"unknown","plugin":null})"));
    CHECK_FALSE(
        accepts(R"({"confidence":"active_token","plugin":0})"));  // no entry on the fault thread
    CHECK_FALSE(accepts(R"({"confidence":"active_token","plugin":null})"));
    CHECK_FALSE(accepts(R"({"confidence":"fault_in_image","plugin":7})"));  // not a plug-in listed
    CHECK_FALSE(accepts(R"({"confidence":"fault_in_image","plugin":"0"})"));
    CHECK_FALSE(accepts(R"({"confidence":"unknown","plugin":0})"));
    CHECK_FALSE(accepts(R"({"confidence":"recent_command","plugin":null})"));  // no commands listed
    CHECK_FALSE(accepts(R"({"confidence":"certain","plugin":0})"));
    CHECK_FALSE(accepts(R"({"confidence":"unknown"})"));
    CHECK_FALSE(accepts(R"({"confidence":"unknown","plugin":null,"why":"x"})"));
    CHECK_FALSE(accepts(R"(["unknown"])"));
    commands = parse(R"([{"seq":1,"name":"a.b","origin":"ui","state":"begun"}])");
    CHECK(accepts(R"({"confidence":"recent_command","plugin":null})"));
    CHECK_FALSE(accepts(R"({"confidence":"recent_command","plugin":0})"));
    // A token needs the fault thread's own entry in that plug-in.
    auto owned = faultTrace(-1);
    inside(owned, 0, 4);
    const auto ownedPlugins = inventory.pluginsJson(&owned);
    const auto ownedThreads = threadsJson(owned);
    CHECK(attributionShape(parse(R"({"confidence":"active_token","plugin":0})"), ownedPlugins,
                           commands, ownedThreads, &modules));
    CHECK_FALSE(attributionShape(parse(R"({"confidence":"concurrent_activity","plugin":0})"),
                                 ownedPlugins, commands, ownedThreads, &modules));
}

TEST_CASE("the attribution validator holds fault_in_image to the plug-in's build and the fault") {
    Inventory inventory;
    REQUIRE(inventory.apply(addAt(4, 0x5100)));  // id 0: in module 1, which has an id
    REQUIRE(inventory.apply(addAt(5, 0x1100)));  // id 1: in module 0
    REQUIRE(inventory.apply(addAt(6, 0x9100)));  // id 2: in no module, so no build
    const auto parse = [](std::string_view text) { return json::parse(text).value(); };
    const auto check = [&](const TraceSnapshot& trace, const char* claim) {
        const auto modules = modulesJson(trace);
        return attributionShape(parse(claim), inventory.pluginsJson(&trace),
                                inventory.commandsJson(&trace), threadsJson(trace), &modules);
    };
    const auto fault1 = faultTrace(1);
    CHECK(check(fault1, R"({"confidence":"fault_in_image","plugin":0})"));
    // A plug-in with no build cannot have been where the fault was (round 38, F4).
    CHECK_FALSE(check(fault1, R"({"confidence":"fault_in_image","plugin":2})"));
    // Nor one whose build is another module than the fault's.
    CHECK_FALSE(check(fault1, R"({"confidence":"fault_in_image","plugin":1})"));
    // The fault thread's first frame must be in a module with an id...
    auto unnamed = faultTrace(1);
    unnamed.modules[1].id.reset();
    CHECK_FALSE(check(unnamed, R"({"confidence":"fault_in_image","plugin":0})"));
    CHECK_FALSE(check(unnamed, R"({"confidence":"fault_in_image","plugin":null})"));
    // ...and in some module at all, and a report without its fault thread says nothing.
    CHECK_FALSE(check(faultTrace(-1), R"({"confidence":"fault_in_image","plugin":null})"));
    auto noFrames = faultTrace(1);
    noFrames.threads[0].walk.frames.clear();
    CHECK_FALSE(check(noFrames, R"({"confidence":"fault_in_image","plugin":0})"));
    // With no plug-in named it is enough that a listed plug-in has the fault's module as its
    // build; with none that does, it is not.
    CHECK(check(fault1, R"({"confidence":"fault_in_image","plugin":null})"));
    CHECK(check(faultTrace(0), R"({"confidence":"fault_in_image","plugin":1})"));
    const auto fault1Modules = modulesJson(fault1);
    Inventory elsewhere;
    REQUIRE(elsewhere.apply(addAt(6, 0x9100)));  // the only plug-in has no build
    CHECK_FALSE(attributionShape(parse(R"({"confidence":"fault_in_image","plugin":null})"),
                                 elsewhere.pluginsJson(&fault1), elsewhere.commandsJson(&fault1),
                                 threadsJson(fault1), &fault1Modules));
    // A report with no `modules` at all (nothing was captured) supports no fault in an image,
    // and is refused without being read through a null pointer.
    CHECK_FALSE(attributionShape(parse(R"({"confidence":"fault_in_image","plugin":0})"),
                                 inventory.pluginsJson(&fault1), inventory.commandsJson(&fault1),
                                 threadsJson(fault1), nullptr));
}

TEST_CASE(
    "dropped counts what was left out, by kind, and the validator takes the old integer too") {
    Inventory inventory;
    auto trace = faultTrace(-1);
    trace.dropped = 5;
    trace.tableThreadsDropped = 2;
    trace.commandsDamaged = 1;
    trace.commandsRefused = 3;
    trace.unplacedEntries = 4;
    trace.tableScopeOverflow = 6;
    trace.tableUntracked = 7;
    inventory.noteDropped(8);
    CHECK_FALSE(
        inventory.apply(nameChunk(2, {"a.b"})));  // a rejected packet: one more message lost
    const auto dropped = inventory.droppedJson(&trace);
    CHECK(droppedShape(dropped));
    CHECK(dropped.find("threads")->asInt() == 5);
    CHECK(dropped.find("unregistered_threads")->asInt() == 2);
    CHECK(dropped.find("plugins")->asInt() == 0);
    CHECK(dropped.find("commands")->asInt() == 4);
    CHECK(dropped.find("cells")->asInt() == 10);
    CHECK(dropped.find("messages")->asInt() == 9);
    CHECK(dropped.find("untracked")->asInt() == 7);
    const auto none = inventory.droppedJson(nullptr);
    CHECK(none.find("threads")->asInt() == 0);
    CHECK(none.find("messages")->asInt() == 9);
    const auto parse = [](std::string_view text) { return json::parse(text).value(); };
    CHECK(droppedShape(parse("0")));
    CHECK(droppedShape(parse("12")));
    CHECK_FALSE(droppedShape(parse("-1")));
    CHECK_FALSE(droppedShape(parse("\"3\"")));
    CHECK_FALSE(droppedShape(parse("{}")));
    CHECK_FALSE(droppedShape(parse(
        R"({"threads":0,"unregistered_threads":0,"plugins":0,"commands":0,"cells":0,"messages":0})")));
    CHECK_FALSE(droppedShape(parse(
        R"({"threads":0,"unregistered_threads":0,"plugins":0,"commands":0,"cells":0,"messages":0,"untracked":-1})")));
    CHECK_FALSE(droppedShape(parse(
        R"({"threads":0,"unregistered_threads":0,"plugins":0,"commands":0,"cells":0,"messages":0,"untracked":"0"})")));
    CHECK_FALSE(droppedShape(parse(
        R"({"threads":0,"unregistered_threads":0,"plugins":0,"commands":0,"cells":0,"messages":0,"untracked":0,"extra":0})")));
}

TEST_CASE(
    "a bridged instance's place in its bridge is kept, found by bridge and cell, never shown") {
    Inventory inventory;
    REQUIRE(inventory.apply(bridgeEvent(2)));
    REQUIRE(inventory.apply(addIn(5, 1, 2)));
    REQUIRE(inventory.apply(addIn(6, 1, 3, "Other")));
    REQUIRE(inventory.apply(childEvent(5, 1, 4, 7, 0x55550000)));
    REQUIRE(inventory.apply(childEvent(6, 1, 4, 7, 0x66660000)));  // the same cell in bridge 3
    const auto* first = inventory.findInBridge(2, 4, 7);
    const auto* second = inventory.findInBridge(3, 4, 7);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->cell == 5);
    CHECK(second->cell == 6);
    CHECK(first->child.image == 0x55550000);
    CHECK(first->image == 0x7000);  // the host's own image is not touched by the bridge's
    CHECK(inventory.findInBridge(2, 4, 8) == nullptr);  // another generation of that cell
    CHECK(inventory.findInBridge(2, 5, 7) == nullptr);
    CHECK(inventory.findInBridge(0, 4, 7) == nullptr);  // bridge 0 is this process
    CHECK(inventory.findInBridge(9, 4, 7) == nullptr);
    // Nothing of it is in the report: not the cell, the generation or the address.
    const auto text = json::writeCanonical(inventory.pluginsJson(nullptr));
    CHECK(text.find("55550000") == std::string::npos);
    CHECK(text.find("1431633920") == std::string::npos);  // 0x55550000 in decimal
    CHECK(pluginsShape(inventory.pluginsJson(nullptr), 0));
    // Describing the instance again keeps where it is; a new incarnation of the cell does not.
    REQUIRE(inventory.apply(addIn(5, 1, 2)));
    CHECK(inventory.findInBridge(2, 4, 7));
    REQUIRE(inventory.apply(addIn(5, 2, 2)));
    CHECK(inventory.findInBridge(2, 4, 7) != nullptr);        // the retired one still answers...
    CHECK(inventory.findInBridge(2, 4, 7)->generation == 1);  // ...and it is the old one
}

TEST_CASE("a destroyed bridged instance is still found by its place, and a live one is preferred") {
    Inventory inventory;
    REQUIRE(inventory.apply(addIn(5, 1, 2)));
    REQUIRE(inventory.apply(childEvent(5, 1, 4, 7, 0x1000)));
    REQUIRE(inventory.apply(change(5, 1, EventKind::Remove, InstanceState::Destroyed)));
    const auto* retired = inventory.findInBridge(2, 4, 7);
    REQUIRE(retired);
    CHECK(retired->state == InstanceState::Destroyed);
    // The same place taken again (the host reuses its own cell as the bridge reuses its own).
    REQUIRE(inventory.apply(addIn(8, 1, 2, "Newer")));
    REQUIRE(inventory.apply(childEvent(8, 1, 4, 7, 0x1000)));
    const auto* live = inventory.findInBridge(2, 4, 7);
    REQUIRE(live);
    CHECK(live->cell == 8);
    CHECK(live->state != InstanceState::Destroyed);
    // Two live ones in the same place (the bridge's Remove was slow to reach us): the newer.
    REQUIRE(inventory.apply(addIn(10, 1, 2, "Newest")));
    REQUIRE(inventory.apply(childEvent(10, 1, 4, 7, 0x1000)));
    CHECK(inventory.findInBridge(2, 4, 7)->cell == 10);
}

TEST_CASE("a damaged Child record is refused and changes nothing") {
    const auto setup = [](Inventory& inventory) {
        REQUIRE(inventory.apply(addIn(5, 1, 2)));
        REQUIRE(inventory.apply(add(6, 1, InstanceState::Active)));  // in process
        auto claimed = addIn(7, 1, 2);  // says it is in a bridge, but not that it is bridged
        claimed.instance.mode = static_cast<std::uint8_t>(diagctx::InstanceMode::InProcess);
        REQUIRE(inventory.apply(claimed));
    };
    const Packet good = childEvent(5, 1, 4, 7, 0x1000);
    {
        Inventory inventory;
        setup(inventory);
        REQUIRE(inventory.apply(good));
        CHECK(inventory.rejected() == 0);
    }
    const auto refused = [&](auto&& change) {
        Packet damaged = good;
        change(damaged.instance);
        Inventory inventory;
        setup(inventory);
        const bool applied = inventory.apply(damaged);
        return !applied && inventory.rejected() == 1 && inventory.findInBridge(2, 4, 7) == nullptr;
    };
    CHECK(refused([](InstanceRecord& r) { r.cell = 9; }));        // never announced
    CHECK(refused([](InstanceRecord& r) { r.cell = 6; }));        // in process: nothing to say
    CHECK(refused([](InstanceRecord& r) { r.cell = 7; }));        // not announced as bridged
    CHECK(refused([](InstanceRecord& r) { r.generation = 2; }));  // another incarnation
    CHECK(
        refused([](InstanceRecord& r) { r.childCell = static_cast<std::uint32_t>(kMaxPlugins); }));
    CHECK(refused([](InstanceRecord& r) { r.childCell = 0xffffffffu; }));
    CHECK(refused([](InstanceRecord& r) { r.childGeneration = diagctx::kGenerationMask + 1; }));
    CHECK(refused([](InstanceRecord& r) { r.kind = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.origin = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.mode = 2; }));
    CHECK(refused([](InstanceRecord& r) { r.state = 2; }));
    CHECK(refused([](InstanceRecord& r) { r.bridge = 2; }));
    CHECK(refused([](InstanceRecord& r) { r.how = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.status = 1; }));
    CHECK(refused([](InstanceRecord& r) { r.length[0] = 1; }));
    // The child's fields mean nothing in any other event: an Add, a Bridge event, or a name chunk
    // carrying them is damaged.
    Inventory inventory;
    auto withCell = add(1, 0, InstanceState::Active);
    withCell.instance.childCell = 1;
    CHECK_FALSE(inventory.apply(withCell));
    auto withGeneration = addIn(1, 0, 2);
    withGeneration.instance.childGeneration = 1;
    CHECK_FALSE(inventory.apply(withGeneration));
    auto inBridgeEvent = bridgeEvent(3);
    inBridgeEvent.instance.childCell = 1;
    CHECK_FALSE(inventory.apply(inBridgeEvent));
    CHECK(inventory.bridges().empty());
}

TEST_CASE("every byte of a Child record, replaced, is refused or leaves the place in range") {
    const Packet good = childEvent(5, 1, 4, 7, 0x1000);
    for (std::size_t at = 0; at < sizeof(InstanceRecord); ++at)
        for (const unsigned char value : {0x00, 0x01, 0x07, 0x7f, 0x80, 0xff}) {
            Packet damaged = good;
            reinterpret_cast<unsigned char*>(&damaged.instance)[at] = value;
            Inventory inventory;
            REQUIRE(inventory.apply(addIn(5, 1, 2)));
            (void)inventory.apply(damaged);
            for (const auto* entry : inventory.entries()) {
                CHECK(entry->cell == 5);
                CHECK(entry->mode == diagctx::InstanceMode::Bridged);
                if (entry->child.set()) {
                    CHECK(entry->child.cell < kMaxPlugins);
                    CHECK(entry->child.generation <= diagctx::kGenerationMask);
                }
            }
            CHECK(inventory.entries().size() == 1);  // never a second entry out of one packet
        }
}

TEST_CASE(
    "only an end by a signal or an exception may name a report, and only by a real report id") {
    const std::string id = "0123456789abcdef0123456789abcdef";
    const auto ended = [&](diagctx::BridgeHow how, std::string_view text) {
        Packet p = bridgeEvent(3, how, how == diagctx::BridgeHow::None ? 0 : 11);
        std::memcpy(p.instance.text[3], text.data(), text.size());
        p.instance.length[3] = static_cast<std::uint8_t>(text.size());
        return p;
    };
    const auto accepted = [&](const Packet& p) {
        Inventory inventory;
        return inventory.apply(p) && inventory.rejected() == 0;
    };
    {
        Inventory inventory;
        REQUIRE(inventory.apply(ended(diagctx::BridgeHow::Signal, id)));
        CHECK(inventory.bridge(3)->incident == id);
    }
    CHECK(accepted(ended(diagctx::BridgeHow::Exception, id)));
    CHECK(accepted(ended(diagctx::BridgeHow::Signal, "")));      // an older worker sends none
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::Host, id)));  // never reported
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::Exit, id)));
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::None, id)));
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::Signal, id.substr(1))));  // 31 characters
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::Signal, id + "0")));
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::Signal, "0123456789ABCDEF0123456789abcdef")));
    CHECK_FALSE(accepted(ended(diagctx::BridgeHow::Signal, "0123456789abcdef0123456789abcde/")));
    auto withName = ended(diagctx::BridgeHow::Signal, id);
    withName.instance.length[0] = 1;  // text in any other field
    CHECK_FALSE(accepted(withName));
    // A second end does not replace the first's id.
    Inventory inventory;
    REQUIRE(inventory.apply(ended(diagctx::BridgeHow::Signal, id)));
    REQUIRE(inventory.apply(ended(diagctx::BridgeHow::Signal, "ffffffffffffffffffffffffffffffff")));
    CHECK(inventory.bridge(3)->incident == id);
}

// ---- a bridge's capture slot (topic 6c, step 6c.3) ----------------------------------------------
namespace {
namespace capture = supple::diagnostics::selfcapture;
using Resource = std::intptr_t;

// What this process holds open: descriptors on POSIX, handles on Windows. The slot cases compare
// it before and after, so a resource that is refused and not closed shows.
long openResources() {
#if defined(_WIN32)
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? static_cast<long>(count) : -1;
#else
    long count = 0;
    for (int fd = 0; fd < 4096; ++fd)
        if (::fcntl(fd, F_GETFD) != -1) ++count;
    return count;
#endif
}
void closeResource(Resource resource) {
    if (resource < 0) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(resource));
#else
    ::close(static_cast<int>(resource));
#endif
}
// An empty slot's header for bridge `ordinal`, claiming the slot's own capacity whatever object it
// is written into (a wrong-sized object with a right-looking header is a case to refuse).
capture::SlotHeader headerFor(std::uint32_t ordinal) {
    capture::SlotHeader header{};
    header.magic = capture::kSlotMagic;
    header.version = capture::kSlotVersion;
    header.state = capture::kSlotEmpty;
    header.capacity = static_cast<std::uint32_t>(capture::kSlotBytes);
    header.ordinal = ordinal;
    return header;
}
// A shared-memory object of `bytes` the way the host makes one (sealed on Linux), optionally with
// an empty slot's header for bridge `ordinal` written in it.
Resource memory(std::size_t bytes, bool header, std::uint32_t ordinal, bool seal = true) {
#if defined(_WIN32)
    (void)seal;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32),
                                        static_cast<DWORD>(bytes), nullptr);
    REQUIRE(section);
    if (header) {
        void* view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        REQUIRE(view);
        const auto written = headerFor(ordinal);
        std::memcpy(view, &written, sizeof written);
        UnmapViewOfFile(view);
    }
    return reinterpret_cast<Resource>(section);
#else
#if defined(__linux__)
    const int fd = ::memfd_create("slot-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
    char name[] = "/slottestXXXXXX";
    for (int i = 9; i < 15; ++i) name[i] = static_cast<char>('a' + (::getpid() + i * 7) % 26);
    const int fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) ::shm_unlink(name);
#endif
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(bytes)) == 0);
#if defined(__linux__)
    if (seal) REQUIRE(::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
#else
    (void)seal;
#endif
    if (header) {
        const auto slotHeader = headerFor(ordinal);
        REQUIRE(::pwrite(fd, &slotHeader, sizeof slotHeader, 0) ==
                static_cast<ssize_t>(sizeof slotHeader));
    }
    return fd;
#endif
}
Resource slotMemory(std::uint32_t ordinal) { return memory(capture::kSlotBytes, true, ordinal); }
// A second handle on the same object, as the bridge's own mapping is one.
Resource duplicateOf(Resource resource) {
#if defined(_WIN32)
    HANDLE copy = nullptr;
    REQUIRE(DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(resource),
                            GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS));
    return reinterpret_cast<Resource>(copy);
#else
    const int copy = ::dup(static_cast<int>(resource));
    REQUIRE(copy >= 0);
    return copy;
#endif
}
// What a bridge does at a fault: the capsule, then the state last (here, `state`). Through
// `writer`, a handle on the slot's object that is not the supervisor's.
void bridgeWrites(Resource writer, const std::vector<unsigned char>& capsule, std::uint32_t state) {
    std::vector<unsigned char> block(capture::kSlotBytes);
    capture::SlotHeader header{};
    header.magic = capture::kSlotMagic;
    header.version = capture::kSlotVersion;
    header.state = state;
    header.length = static_cast<std::uint32_t>(capsule.size());
    header.capacity = static_cast<std::uint32_t>(capture::kSlotBytes);
    header.ordinal = 1;
    std::memcpy(block.data(), &header, sizeof header);
    std::copy(capsule.begin(), capsule.end(), block.begin() + sizeof header);
#if defined(_WIN32)
    void* view = MapViewOfFile(reinterpret_cast<HANDLE>(writer), FILE_MAP_ALL_ACCESS, 0, 0,
                               capture::kSlotBytes);
    REQUIRE(view);
    std::memcpy(view, block.data(), block.size());
    UnmapViewOfFile(view);
#else
    REQUIRE(::pwrite(static_cast<int>(writer), block.data(), block.size(), 0) ==
            static_cast<ssize_t>(block.size()));
#endif
}
Packet slotEvent(std::uint16_t bridge) {
    Packet p = base();
    p.instance.event = static_cast<std::uint8_t>(diagctx::EventKind::Slot);
    p.instance.bridge = bridge;
    return p;
}
std::vector<unsigned char> sampleCapsule() {
    std::vector<unsigned char> capsule(300);
    for (std::size_t i = 0; i < capsule.size(); ++i) capsule[i] = static_cast<unsigned char>(i * 7);
    return capsule;
}
}  // namespace

TEST_CASE("a bridge's slot is kept for the bridge it names and read once the bridge is gone") {
    const long baseline = openResources();
    {
        Inventory inventory;
        REQUIRE(inventory.apply(bridgeEvent(1)));
        Resource slot = slotMemory(1);
        const Resource writer = duplicateOf(slot);  // the bridge's own end of the object
        REQUIRE(inventory.apply(slotEvent(1), &slot));
        CHECK(slot == -1);  // taken: the caller has nothing to close
        CHECK(inventory.slotsHeld() == 1);
        CHECK_FALSE(inventory.slotCapsule(1));  // empty: nothing was written
        bridgeWrites(writer, sampleCapsule(), capture::kSlotWriting);
        CHECK_FALSE(inventory.slotCapsule(1));  // died mid-write: nothing is used
        bridgeWrites(writer, sampleCapsule(), capture::kSlotComplete);
        const auto capsule = inventory.slotCapsule(1);
        REQUIRE(capsule);
        CHECK(*capsule == sampleCapsule());     // exactly the bytes the header's length says
        CHECK_FALSE(inventory.slotCapsule(2));  // another bridge has none
        closeResource(writer);
        CHECK(inventory.rejected() == 0);
    }
    // Destroying the inventory closes the slot it held.
    CHECK(openResources() == baseline);
}

#if !defined(_WIN32)
namespace {
// macOS cannot read(2) or pread(2) the shared-memory object it makes slots from, so there a slot
// is read through a read-only mapping (round 6c-a, a). Linux can prove the same code by being
// told to use it.
struct MappedReads {
    MappedReads() { readSlotsByMappingForTests(true); }
    ~MappedReads() {
#if defined(__linux__)
        readSlotsByMappingForTests(false);
#endif
    }
};
}  // namespace
TEST_CASE("a slot is read through a mapping where the object cannot be read (macOS)") {
    MappedReads mapped;
    const long baseline = openResources();
    {
        Inventory inventory;
        REQUIRE(inventory.apply(bridgeEvent(1)));
        Resource slot = slotMemory(1);
        const Resource writer = duplicateOf(slot);
        REQUIRE(inventory.apply(slotEvent(1), &slot));  // adopt reads the header by mapping
        CHECK(slot == -1);
        CHECK_FALSE(inventory.slotCapsule(1));  // empty
        bridgeWrites(writer, sampleCapsule(), capture::kSlotWriting);
        CHECK_FALSE(inventory.slotCapsule(1));  // died mid-write
        bridgeWrites(writer, sampleCapsule(), capture::kSlotComplete);
        const auto capsule = inventory.slotCapsule(1);
        REQUIRE(capsule);
        CHECK(*capsule == sampleCapsule());
        closeResource(writer);
    }
    // Nothing stays mapped or open: the mapping of a read is gone when it returns.
    CHECK(openResources() == baseline);
    {
        // The wrong size is refused before anything is mapped.
        Inventory inventory;
        REQUIRE(inventory.apply(bridgeEvent(1)));
        Resource small = memory(capture::kSlotBytes / 2, true, 1);
        CHECK_FALSE(inventory.apply(slotEvent(1), &small));
        CHECK(small != -1);  // still the caller's: it closes it
        closeResource(small);
    }
    CHECK(openResources() == baseline);
}
#endif

namespace {
// The bytes of a capsule a bridge's handler could have written: one image, the fault thread with
// one frame inside it, and a table copy.
std::vector<unsigned char> bridgeCapsuleBytes() {
    std::vector<unsigned char> bytes;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        bytes.insert(bytes.end(), b, b + n);
    };
    CapsuleHeader header{};
    header.magic = capture::kBridgeCapsuleMagic;
    header.moduleCount = 1;
    header.threadCount = 1;
    header.flags = capture::kCapsuleHasTable;
    header.reserved = 100;
    put(&header, sizeof header);
    capture::BridgeModule module{};
    module.start = 0x10000;
    module.end = 0x20000;
    module.idLength = 4;
    module.id[0] = 0xaa;
    module.id[1] = 0xbb;
    module.id[2] = 0xcc;
    module.id[3] = 0xdd;
    put(&module, sizeof module);
    CapsuleThread thread{};
    thread.role = static_cast<std::uint8_t>(supple::diagnostics::detail::Role::Fault);
    thread.frameCount = 1;
    put(&thread, sizeof thread);
    const std::uint64_t frame = 0x10100;
    put(&frame, sizeof frame);
    auto table = std::make_unique<diagctx::TableImage>();
    std::memset(table.get(), 0, sizeof(diagctx::TableImage));
    table->header.magic = diagctx::kTableMagic;
    table->header.version = diagctx::kTableVersion;
    put(table.get(), sizeof(diagctx::TableImage));
    return bytes;
}
}  // namespace

TEST_CASE(
    "what a bridge wrote about itself is read once it has ended by a signal or an exception") {
    const auto capsule = bridgeCapsuleBytes();
    // One bridge per way of ending; every one holds a complete, good capsule in its slot.
    const auto bridgeWith = [&](Inventory& inventory, diagctx::BridgeHow how,
                                std::uint32_t state = capture::kSlotComplete,
                                const std::vector<unsigned char>& bytes = bridgeCapsuleBytes()) {
        REQUIRE(inventory.apply(bridgeEvent(1)));
        Resource slot = slotMemory(1);
        const Resource writer = duplicateOf(slot);
        REQUIRE(inventory.apply(slotEvent(1), &slot));
        bridgeWrites(writer, bytes, state);
        closeResource(writer);
        if (how != diagctx::BridgeHow::None) REQUIRE(inventory.apply(bridgeEvent(1, how, 11)));
    };
    {
        Inventory inventory;
        bridgeWith(inventory, diagctx::BridgeHow::Signal);
        const auto trace = inventory.bridgeTrace(1);
        REQUIRE(trace);
        REQUIRE(trace->threads.size() == 1);
        CHECK(trace->threads[0].role == supple::diagnostics::detail::Role::Fault);
        REQUIRE(trace->modules.size() == 1);
        CHECK(trace->modules[0].id == std::string("aabbccdd"));
        CHECK_FALSE(inventory.bridgeTrace(2));  // no such bridge
    }
    {
        Inventory inventory;
        bridgeWith(inventory, diagctx::BridgeHow::Exception);
        CHECK(inventory.bridgeTrace(1));
    }
    // The host's own kill and an orderly exit are not the bridge's failure: nothing is read.
    for (const auto how :
         {diagctx::BridgeHow::Host, diagctx::BridgeHow::Exit, diagctx::BridgeHow::None}) {
        Inventory inventory;
        bridgeWith(inventory, how);
        CHECK_FALSE(inventory.bridgeTrace(1));
    }
    {  // died while writing: the state says so and nothing is used
        Inventory inventory;
        bridgeWith(inventory, diagctx::BridgeHow::Signal, capture::kSlotWriting);
        CHECK_FALSE(inventory.bridgeTrace(1));
    }
    {  // complete, but not a capsule a handler writes: the parser refuses it
        Inventory inventory;
        bridgeWith(inventory, diagctx::BridgeHow::Signal, capture::kSlotComplete, sampleCapsule());
        CHECK(inventory.slotCapsule(1));        // the slot is fine...
        CHECK_FALSE(inventory.bridgeTrace(1));  // ...its contents are not
        auto damaged = capsule;
        // The first byte of the table copy: a table this build did not write.
        damaged[sizeof(CapsuleHeader) + sizeof(capture::BridgeModule) + sizeof(CapsuleThread) +
                sizeof(std::uint64_t)] ^= 0xff;
        Inventory second;
        bridgeWith(second, diagctx::BridgeHow::Signal, capture::kSlotComplete, damaged);
        CHECK_FALSE(second.bridgeTrace(1));
    }
    {  // no slot was ever announced for it
        Inventory inventory;
        REQUIRE(inventory.apply(bridgeEvent(1)));
        REQUIRE(inventory.apply(bridgeEvent(1, diagctx::BridgeHow::Signal, 11)));
        CHECK_FALSE(inventory.bridgeTrace(1));
    }
}

TEST_CASE("a slot header that stopped being a slot's is never read") {
    Inventory inventory;
    REQUIRE(inventory.apply(bridgeEvent(1)));
    Resource slot = slotMemory(1);
    const Resource writer = duplicateOf(slot);
    REQUIRE(inventory.apply(slotEvent(1), &slot));
    // A length past the slot, a wrong capacity, a wrong magic, and a state past complete: each
    // is a plug-in writing where the bridge's handler should, and each reads as no capsule.
    auto write = [&](auto change) {
        std::vector<unsigned char> block(capture::kSlotBytes);
        capture::SlotHeader header{};
        header.magic = capture::kSlotMagic;
        header.version = capture::kSlotVersion;
        header.state = capture::kSlotComplete;
        header.length = 16;
        header.capacity = static_cast<std::uint32_t>(capture::kSlotBytes);
        header.ordinal = 1;
        change(header);
        std::memcpy(block.data(), &header, sizeof header);
#if defined(_WIN32)
        void* view = MapViewOfFile(reinterpret_cast<HANDLE>(writer), FILE_MAP_ALL_ACCESS, 0, 0,
                                   capture::kSlotBytes);
        REQUIRE(view);
        std::memcpy(view, block.data(), block.size());
        UnmapViewOfFile(view);
#else
        REQUIRE(::pwrite(static_cast<int>(writer), block.data(), block.size(), 0) ==
                static_cast<ssize_t>(block.size()));
#endif
    };
    write([](capture::SlotHeader&) {});
    CHECK(inventory.slotCapsule(1));  // the unchanged one is readable
    write([](capture::SlotHeader& h) { h.length = capture::kSlotBytes; });
    CHECK_FALSE(inventory.slotCapsule(1));
    write([](capture::SlotHeader& h) { h.capacity = 4096; });
    CHECK_FALSE(inventory.slotCapsule(1));
    write([](capture::SlotHeader& h) { h.magic = 0; });
    CHECK_FALSE(inventory.slotCapsule(1));
    write([](capture::SlotHeader& h) { h.state = 3; });
    CHECK_FALSE(inventory.slotCapsule(1));
    write([](capture::SlotHeader& h) { h.version = capture::kSlotVersion + 1; });
    CHECK_FALSE(inventory.slotCapsule(1));
    closeResource(writer);
}

TEST_CASE("a damaged Slot event is refused, counted, changes nothing and leaves nothing open") {
    const long baseline = openResources();
    {
        Inventory inventory;
        REQUIRE(inventory.apply(bridgeEvent(1)));
        REQUIRE(inventory.apply(bridgeEvent(2)));
        REQUIRE(inventory.apply(bridgeEvent(3, diagctx::BridgeHow::Signal, 11)));  // ended
        std::size_t refused = 0;
        // Applies `packet` with `resource` (made by `make`), which must be refused with the
        // resource still the caller's, then closed here: nothing is held and nothing leaked.
        const auto damaged = [&](const char* label, const Packet& packet, auto make) {
            CAPTURE(label);
            const long before = openResources();
            Resource resource = make();
            CHECK_FALSE(inventory.apply(packet, resource < 0 ? nullptr : &resource));
            closeResource(resource);
            CHECK(openResources() == before);
            CHECK(inventory.rejected() == ++refused);
        };
        const auto good = [] { return slotMemory(1); };
        damaged("no resource", slotEvent(1), [] { return Resource{-1}; });
        damaged("a bridge nobody started", slotEvent(9), [] { return slotMemory(9); });
        damaged("a bridge that has ended", slotEvent(3), [] { return slotMemory(3); });
        damaged("bridge 0", slotEvent(0), [] { return slotMemory(0); });
        damaged("the slot of another bridge", slotEvent(1), [] { return slotMemory(2); });
        damaged("memory too small", slotEvent(1),
                [] { return memory(capture::kSlotBytes / 2, true, 1); });
        damaged("memory too large", slotEvent(1),
                [] { return memory(capture::kSlotBytes * 2, true, 1); });
        damaged("memory with no header", slotEvent(1),
                [] { return memory(capture::kSlotBytes, false, 1); });
#if defined(__linux__)
        damaged("an unsealed memfd", slotEvent(1),
                [] { return memory(capture::kSlotBytes, true, 1, false); });
#endif
#if defined(_WIN32)
        damaged("an event is no memory", slotEvent(1), [] {
            return reinterpret_cast<Resource>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        });
#else
        damaged("a pipe is no memory", slotEvent(1), [] {
            int ends[2];
            REQUIRE(::pipe(ends) == 0);
            ::close(ends[1]);
            return Resource{ends[0]};
        });
#endif
        // A Slot event with any other field set, or text, is not one the worker writes.
        const auto spoiled = [&](const char* label, auto change) {
            Packet p = slotEvent(1);
            change(p.instance);
            damaged(label, p, good);
        };
        spoiled("a cell", [](InstanceRecord& r) { r.cell = 1; });
        spoiled("a generation", [](InstanceRecord& r) { r.generation = 1; });
        spoiled("a kind", [](InstanceRecord& r) { r.kind = 1; });
        spoiled("a state", [](InstanceRecord& r) { r.state = 1; });
        spoiled("an image", [](InstanceRecord& r) { r.image = 1; });
        spoiled("an end", [](InstanceRecord& r) { r.how = 1; });
        spoiled("a status", [](InstanceRecord& r) { r.status = 1; });
        spoiled("a child cell", [](InstanceRecord& r) { r.childCell = 1; });
        spoiled("text", [](InstanceRecord& r) { r.length[0] = 1; });
        CHECK(inventory.slotsHeld() == 0);  // not one of them was kept
        // The good one is taken, and a second for the same bridge is refused.
        Resource first = slotMemory(1);
        REQUIRE(inventory.apply(slotEvent(1), &first));
        CHECK(inventory.slotsHeld() == 1);
        damaged("a second slot", slotEvent(1), good);
        CHECK(inventory.slotsHeld() == 1);
        // A slot for the bridge that went on running is still taken after all those refusals.
        Resource other = slotMemory(2);
        REQUIRE(inventory.apply(slotEvent(2), &other));
        CHECK(inventory.slotsHeld() == 2);
        // A resource on a Slot event when the caller passes nowhere to put it is refused.
        Resource unwanted = slotMemory(2);
        CHECK_FALSE(inventory.apply(slotEvent(2), nullptr));
        closeResource(unwanted);
    }
    CHECK(openResources() == baseline);
}

TEST_CASE("a bridge that falls out of the list takes its slot with it") {
    const long baseline = openResources();
    {
        Inventory inventory;
        for (std::uint16_t id = 1; id <= kMaxBridges; ++id) {
            REQUIRE(inventory.apply(bridgeEvent(id)));
            Resource slot = slotMemory(id);
            REQUIRE(inventory.apply(slotEvent(id), &slot));
        }
        CHECK(inventory.slotsHeld() == kMaxBridges);
        // Bridge 1 ends; bridge 257 then has no room unless the oldest ended one goes.
        REQUIRE(inventory.apply(bridgeEvent(1, diagctx::BridgeHow::Signal, 11)));
        REQUIRE(inventory.apply(bridgeEvent(static_cast<std::uint16_t>(kMaxBridges + 1))));
        CHECK(inventory.bridge(1) == nullptr);
        CHECK(inventory.slotsHeld() == kMaxBridges - 1);
        CHECK_FALSE(inventory.slotCapsule(1));
        Resource late = slotMemory(static_cast<std::uint32_t>(kMaxBridges + 1));
        REQUIRE(inventory.apply(slotEvent(static_cast<std::uint16_t>(kMaxBridges + 1)), &late));
        CHECK(inventory.slotsHeld() == kMaxBridges);  // never more than the list holds
    }
    CHECK(openResources() == baseline);
}

namespace {
struct Seen {
    static inline int calls = 0, plain = 0;
    static inline Resource resource = -1;
    static inline Packet packet;
};
}  // namespace

TEST_CASE("the worker announces a slot once, with the resource beside the packet") {
    Seen::calls = Seen::plain = 0;
    Seen::resource = -1;
    resetInventoryForTests(
        [](Native&, const Packet&) noexcept {
            ++Seen::plain;
            return true;
        },
        [](Native&, const Packet& packet, std::intptr_t resource) noexcept {
            ++Seen::calls;
            Seen::resource = resource;
            Seen::packet = packet;
            return true;
        });
    Native native;
    installInventory(native, base());
    diagctx::announceSlot(0, 5);   // no bridge
    diagctx::announceSlot(3, -1);  // no resource
    CHECK(Seen::calls == 0);
    diagctx::announceSlot(3, 17);
    REQUIRE(Seen::calls == 1);
    CHECK(Seen::plain == 0);  // it never went as a plain packet
    CHECK(Seen::resource == 17);
    const auto& r = Seen::packet.instance;
    CHECK(Seen::packet.kind == Message::Instance);
    CHECK(r.event == static_cast<std::uint8_t>(diagctx::EventKind::Slot));
    CHECK(r.bridge == 3);
    CHECK((r.cell | r.generation | r.kind | r.origin | r.mode | r.state | r.how | r.status |
           r.childCell | r.childGeneration) == 0);
    CHECK(r.image == 0);
    for (const auto length : r.length) CHECK(length == 0);
    resetInventoryForTests(nullptr);
}

#if !defined(_WIN32)
namespace {
// One end of the channel as a worker, the other as the supervisor.
struct Channel {
    int pair[2]{-1, -1};
    Native sender, receiver;
    Channel() {
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        sender = Native{pair[0]};
        receiver = Native{pair[1]};
        receiver.exited = true;  // receive() then drains what is queued and ends, without waitpid
    }
    ~Channel() {
        ::close(pair[0]);
        ::close(pair[1]);
    }
    // Sends `packet` the way publishWithResource() does, but with `descriptors` attached (any
    // number) at the first byte, or after `plainFirst` plain bytes (a descriptor on a later one).
    void sendRaw(const Packet& packet, const std::vector<int>& descriptors,
                 std::size_t plainFirst = 0) {
        const auto* bytes = reinterpret_cast<const char*>(&packet);
        if (plainFirst)
            REQUIRE(::send(pair[0], bytes, plainFirst, 0) == static_cast<ssize_t>(plainFirst));
        iovec part{const_cast<char*>(bytes) + plainFirst, sizeof(packet) - plainFirst};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        std::vector<char> control(CMSG_SPACE(sizeof(int) * (descriptors.size() + 1)));
        if (!descriptors.empty()) {
            message.msg_control = control.data();
            message.msg_controllen = CMSG_SPACE(sizeof(int) * descriptors.size());
            cmsghdr* header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
            std::memcpy(CMSG_DATA(header), descriptors.data(), sizeof(int) * descriptors.size());
        }
        REQUIRE(::sendmsg(pair[0], &message, 0) == static_cast<ssize_t>(part.iov_len));
    }
};
bool sameObject(int a, int b) {
    struct stat x{}, y{};
    return ::fstat(a, &x) == 0 && ::fstat(b, &y) == 0 && x.st_dev == y.st_dev &&
           x.st_ino == y.st_ino;
}
}  // namespace

TEST_CASE(
    "the channel carries one descriptor with a Slot packet, and every other shape is refused") {
    const long baseline = openResources();
    {
        Channel channel;
        Packet packet;
        Resource got = -1;
        // The shape that is allowed: one descriptor, publishWithResource, a Slot packet.
        const Resource slot = slotMemory(1);
        REQUIRE(publishWithResource(channel.sender, slotEvent(1), slot));
        REQUIRE(receive(channel.receiver, packet, &got));
        REQUIRE(got >= 0);
        CHECK(got != slot);  // a copy in the receiving table
        CHECK(sameObject(static_cast<int>(got), static_cast<int>(slot)));
        CHECK((::fcntl(static_cast<int>(got), F_GETFD) & FD_CLOEXEC) != 0);
        CHECK(mayCarryResource(packet));
        CHECK(channel.receiver.refusedResources == 0);
        closeResource(got);
        // A Slot packet that came with none: nothing to adopt, nothing refused by the channel.
        REQUIRE(publish(channel.sender, slotEvent(1)));
        REQUIRE(receive(channel.receiver, packet, &got));
        CHECK(got == -1);
        CHECK(channel.receiver.refusedResources == 0);
        // Two descriptors on a Slot packet: the first is the packet's, the second is refused.
        const Resource second = slotMemory(1);
        channel.sendRaw(slotEvent(1), {static_cast<int>(slot), static_cast<int>(second)});
        REQUIRE(receive(channel.receiver, packet, &got));
        REQUIRE(got >= 0);
        CHECK(channel.receiver.refusedResources == 1);
        closeResource(got);
        // A descriptor on a packet that may carry none: closed, counted, the packet still read.
        REQUIRE(publishWithResource(channel.sender, add(1, 0, InstanceState::Active), slot));
        REQUIRE(receive(channel.receiver, packet, &got));
        CHECK(got == -1);
        CHECK(packet.instance.cell == 1);
        CHECK(channel.receiver.refusedResources == 2);
        // The same for the Terminal-style kinds: no packet but a Slot may carry one.
        Packet ready = base();
        ready.kind = Message::Ready;
        REQUIRE(publishWithResource(channel.sender, ready, slot));
        REQUIRE(receive(channel.receiver, packet, &got));
        CHECK(got == -1);
        CHECK(channel.receiver.refusedResources == 3);
        // A descriptor attached to a later byte of the packet: refused, the packet still whole.
        channel.sendRaw(slotEvent(1), {static_cast<int>(slot)}, 10);
        REQUIRE(receive(channel.receiver, packet, &got));
        CHECK(got == -1);
        CHECK(channel.receiver.refusedResources == 4);
        CHECK(valid(packet));
        // The caller who passes nowhere for it gets the same: a Slot packet's descriptor, closed.
        REQUIRE(publishWithResource(channel.sender, slotEvent(1), slot));
        REQUIRE(receive(channel.receiver, packet));
        CHECK(channel.receiver.refusedResources == 5);
        // A wrong protocol version on a Slot packet that carries one: the stream stops, the
        // descriptor is closed.
        Packet wrong = slotEvent(1);
        wrong.version = kProtocolVersion + 1;
        channel.sendRaw(wrong, {static_cast<int>(slot)});
        CHECK_FALSE(receive(channel.receiver, packet, &got));
        CHECK(got == -1);
        closeResource(slot);
        closeResource(second);
    }
    CHECK(openResources() == baseline);
}

TEST_CASE(
    "a Slot announced by the worker reaches the supervisor's inventory, and its capsule is read") {
    const long baseline = openResources();
    {
        Channel channel;
        resetInventoryForTests(nullptr);
        Packet boot = base();
        installInventory(channel.sender, boot);
        Inventory inventory;
        Packet packet;
        Resource got = -1;
        const auto next = [&] {
            REQUIRE(receive(channel.receiver, packet, &got));
            const bool taken = inventory.apply(packet, &got);
            closeResource(got);  // what the frontend's guard does with whatever was not taken
            got = -1;
            return taken;
        };
        diagctx::announceBridge(1);
        CHECK(next());
        const Resource slot = slotMemory(1);
        diagctx::announceSlot(1, slot);  // the worker keeps its own, closes it after
        CHECK(next());
        CHECK(inventory.slotsHeld() == 1);
        bridgeWrites(slot, sampleCapsule(), capture::kSlotComplete);  // the bridge's own end
        diagctx::announceBridge(1, diagctx::BridgeHow::Signal, 11);
        CHECK(next());
        const auto capsule = inventory.slotCapsule(1);
        REQUIRE(capsule);
        CHECK(*capsule == sampleCapsule());
        closeResource(slot);
        CHECK(inventory.rejected() == 0);
        resetInventoryForTests(nullptr);
    }
    CHECK(openResources() == baseline);
}
#endif
