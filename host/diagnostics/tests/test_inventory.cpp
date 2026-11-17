// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include "../src/inventory.h"
#include "../src/native.h"
#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
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
    std::uint32_t sent = 0;
    for (; sent < 100000; ++sent) {
        auto packet = add(sent % kMaxPlugins, 0, InstanceState::Active);
        packet.instance.image = sent;
        if (!publish(sender, packet)) break;
    }
    REQUIRE(sent > 10);      // something fit
    REQUIRE(sent < 100000);  // and the buffer did fill: the send refused instead of waiting
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
    CHECK(attributionShape(attribution, inventory.pluginsJson(&trace),
                           inventory.commandsJson(&trace)));
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
    const auto accepts = [&](const char* text) {
        return attributionShape(parse(text), plugins, commands);
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
    CHECK(attributionShape(parse(R"({"confidence":"active_token","plugin":0})"), ownedPlugins,
                           commands));
    CHECK_FALSE(attributionShape(parse(R"({"confidence":"concurrent_activity","plugin":0})"),
                                 ownedPlugins, commands));
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
