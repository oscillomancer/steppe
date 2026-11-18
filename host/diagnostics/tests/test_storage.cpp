// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "../src/background.h"
#include "../src/inventory.h"
#include "../src/private_files.h"
#include "../src/storage.h"
#include "supple/base/files.h"
using namespace supple;
using namespace supple::diagnostics::detail;
namespace fs = std::filesystem;
// Internal overload, deliberately absent from the storage interface.
namespace supple::diagnostics::detail {
Status pruneReports(const fs::path& root,
                    const std::function<void(const fs::path&, std::string_view)>& checkpoint);
}  // namespace supple::diagnostics::detail
namespace {
struct Root {
    fs::path path;
    Root() {
        auto id = incidentId();
        REQUIRE(id);
        path = fs::temp_directory_path() / (*id) / "app" / "diagnostics";
        REQUIRE(prepareDirectory(path));
    }
    ~Root() {
        std::error_code ec;
        fs::remove_all(path.parent_path().parent_path(), ec);
    }
};
Packet boot(unsigned id) {
    Packet p;
    p.role = 3;
    std::fill(p.incident.begin(), p.incident.begin() + 32, '0');
    p.incident[30] = "0123456789abcdef"[(id >> 4) & 15];
    p.incident[31] = "0123456789abcdef"[id & 15];
    return p;
}
unsigned reports(const fs::path& root) {
    unsigned n = 0;
    for (const auto& e : fs::directory_iterator(root))
        if (e.path().extension() == ".json") ++n;
    return n;
}
}  // namespace
TEST_CASE("diagnostic retention removes oldest reports but never markers or unrelated files") {
    Root root;
    auto marker = createMarker(root.path, boot(200));
    REQUIRE(marker);
    REQUIRE(writeFileAtomic(root.path / "unrelated.json", "{}"));
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (unsigned i = 0; i < 25; ++i) {
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false));
        const auto file = root.path / (std::string(boot(i).incident.data()) + ".json");
        // Explicit order independent of timestamp granularity or elapsed time.
        fs::last_write_time(file, oldest + std::chrono::seconds(i));
    }
    CHECK(reports(root.path) == 21);  // 20 reports plus unrelated.json
    CHECK_FALSE(fs::exists(root.path / (std::string(boot(0).incident.data()) + ".json")));
    CHECK(fs::exists(root.path / (std::string(boot(24).incident.data()) + ".json")));
    CHECK(fs::exists(root.path / (std::string(boot(200).incident.data()) + ".run")));
    releaseMarker(*marker);
    CHECK(fs::exists(root.path / "unrelated.json"));
}
TEST_CASE("diagnostics rejects malformed bootstrap and never serializes raw context") {
    Root root;
    auto p = boot(0);
    CHECK(valid(p));
    auto bad = p;
    bad.version = kProtocolVersion + 1;
    CHECK_FALSE(valid(bad));
    bad = p;
    bad.incident[4] = '/';
    CHECK_FALSE(valid(bad));
    bad = p;
    bad.role = 99;
    CHECK_FALSE(valid(bad));
    p.kind = Message::Terminal;
    p.pc = 0xfeedface;
    p.frame = 0xabcdef;
    REQUIRE(publishReport(root.path, boot(0), &p, 139, true));
    auto text = readTextFile(root.path / (std::string(boot(0).incident.data()) + ".json"));
    REQUIRE(text);
    CHECK(text->find("feedface") == std::string::npos);
    CHECK(text->find("4277009102") == std::string::npos);
    CHECK(text->find("abcdef") == std::string::npos);
    auto value = json::parse(*text);
    REQUIRE(value);
    CHECK(value->find("terminal_context")->asBool());
    CHECK(value->find("threads")->size() == 0);
}
TEST_CASE("a report with an inventory is published, validated and pruned like any other") {
    Root root;
    Inventory inventory;
    Packet add = boot(0);
    add.kind = Message::Instance;
    add.instance.cell = 3;
    add.instance.event = static_cast<std::uint8_t>(supple::diagctx::EventKind::Add);
    add.instance.kind = static_cast<std::uint8_t>(supple::diagctx::InstanceKind::Clap);
    add.instance.origin = static_cast<std::uint8_t>(supple::diagctx::InstanceOrigin::Foreign);
    add.instance.mode = static_cast<std::uint8_t>(supple::diagctx::InstanceMode::InProcess);
    add.instance.state = static_cast<std::uint8_t>(supple::diagctx::InstanceState::Loading);
    std::memcpy(add.instance.text[0], "Canary", 6);
    add.instance.length[0] = 6;
    REQUIRE(inventory.apply(add));
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false, nullptr, &inventory));
    const auto text = readTextFile(root.path / (std::string(boot(0).incident.data()) + ".json"));
    REQUIRE(text);
    const auto parsed = json::parse(*text);
    REQUIRE(parsed);
    const auto* plugins = parsed->find("plugins");
    REQUIRE(plugins);
    REQUIRE(plugins->size() == 1);
    CHECK(plugins->asArray()[0].find("name")->asString() == "Canary");
    CHECK(plugins->asArray()[0].find("state")->asString() == "loading");
    // Retention recognises it as an owned report: the oldest of 22 is pruned, which it only
    // does for a report whose every field it can validate.
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(root.path / (std::string(boot(0).incident.data()) + ".json"), oldest);
    for (unsigned i = 1; i < 22; ++i)
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false, nullptr, &inventory));
    CHECK(reports(root.path) == 20);
    CHECK_FALSE(fs::exists(root.path / (std::string(boot(0).incident.data()) + ".json")));
}
TEST_CASE(
    "a report of an earlier build, with a plain dropped count and no attribution, is still owned") {
    Root root;
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false));
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours(48);
    const auto seed = root.path / (std::string(boot(0).incident.data()) + ".json");
    auto text = privateRead(seed);
    REQUIRE(text);
    auto report = json::parse(*text).value();
    REQUIRE(report.find("attribution"));
    CHECK(report.find("attribution")->find("confidence")->asString() == "unknown");
    CHECK(report.find("dropped")->isObject());
    report["dropped"] = 0;
    REQUIRE(report.asObject().erase("attribution"));
    REQUIRE(privateRemove(seed));
    REQUIRE(privatePublish(seed, json::writeCanonical(report)));
    fs::last_write_time(seed, oldest);
    for (unsigned i = 1; i < 22; ++i)
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false));
    CHECK_FALSE(fs::exists(seed));  // pruned: the validator knows that shape
    CHECK(reports(root.path) == 20);
}

TEST_CASE("a report with bridges is published, validated, pruned, and kept when altered") {
    Root root;
    Inventory inventory;
    const auto apply = [&](auto&& change) {
        Packet p = boot(0);
        p.kind = Message::Instance;
        change(p.instance);
        REQUIRE(inventory.apply(p));
    };
    apply([](InstanceRecord& r) {
        r.event = static_cast<std::uint8_t>(supple::diagctx::EventKind::Bridge);
        r.bridge = 1;
    });
    apply([](InstanceRecord& r) {
        r.cell = 3;
        r.event = static_cast<std::uint8_t>(supple::diagctx::EventKind::Add);
        r.kind = static_cast<std::uint8_t>(supple::diagctx::InstanceKind::Clap);
        r.origin = static_cast<std::uint8_t>(supple::diagctx::InstanceOrigin::Foreign);
        r.mode = static_cast<std::uint8_t>(supple::diagctx::InstanceMode::Bridged);
        r.state = static_cast<std::uint8_t>(supple::diagctx::InstanceState::Active);
        r.bridge = 1;
    });
    apply([](InstanceRecord& r) {
        r.event = static_cast<std::uint8_t>(supple::diagctx::EventKind::Bridge);
        r.bridge = 1;
        r.how = static_cast<std::uint8_t>(supple::diagctx::BridgeHow::Signal);
        r.status = 11;
    });
    const auto path = [&](unsigned i) {
        return root.path / (std::string(boot(i).incident.data()) + ".json");
    };
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false, nullptr, &inventory));
    const auto text = readTextFile(path(0));
    REQUIRE(text);
    const auto parsed = json::parse(*text);
    REQUIRE(parsed);
    const auto* bridges = parsed->find("bridges");
    REQUIRE(bridges);
    REQUIRE(bridges->size() == 1);
    CHECK(bridges->asArray()[0].find("ended")->find("how")->asString() == "signal");
    CHECK(parsed->find("plugins")->asArray()[0].find("bridge")->asInt() == 1);
    CHECK(parsed->find("dropped")->find("bridges")->asInt() == 0);
    CHECK(reportShape(*parsed));
    // Retention only removes a report it can validate in full: this one is the oldest of 22.
    fs::last_write_time(path(0), fs::file_time_type::clock::now() - std::chrono::hours(48));
    for (unsigned i = 1; i < 22; ++i)
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false, nullptr, &inventory));
    CHECK(reports(root.path) == 20);
    CHECK_FALSE(fs::exists(path(0)));
    // A report whose bridges disagree with its plug-ins is not one retention may delete, and the
    // reader's tolerant check still reads it (it is only a section it does not understand).
    for (const auto& edit :
         {+[](json::Value& v) { v["bridges"].asArray()[0]["id"] = 2; },
          +[](json::Value& v) { v["bridges"].asArray()[0]["state"] = "running"; },
          +[](json::Value& v) { v["plugins"].asArray()[0]["bridge"] = 9; }}) {
        auto altered = *parsed;
        edit(altered);
        CHECK_FALSE(reportShape(altered));
    }
    // A report of an earlier build (no bridges, seven counts) is still a valid report.
    auto older = *parsed;
    older.asObject().erase("bridges");
    older["dropped"].asObject().erase("bridges");
    CHECK(reportShape(older));
}
namespace {
// A bridge whose one plug-in lived in it, and which then ended by `how`.
Inventory bridgeThatEnded(supple::diagctx::BridgeHow how, std::int32_t status,
                          std::string_view incident = {}) {
    using namespace supple::diagctx;
    Inventory inventory;
    const auto apply = [&](auto&& change) {
        Packet p = boot(0);
        p.kind = Message::Instance;
        change(p.instance);
        REQUIRE(inventory.apply(p));
    };
    apply([](InstanceRecord& r) {
        r.event = static_cast<std::uint8_t>(EventKind::Bridge);
        r.bridge = 1;
    });
    apply([](InstanceRecord& r) {
        r.cell = 3;
        r.event = static_cast<std::uint8_t>(EventKind::Add);
        r.kind = static_cast<std::uint8_t>(InstanceKind::Clap);
        r.origin = static_cast<std::uint8_t>(InstanceOrigin::Foreign);
        r.mode = static_cast<std::uint8_t>(InstanceMode::Bridged);
        r.state = static_cast<std::uint8_t>(InstanceState::Active);
        r.bridge = 1;
        std::memcpy(r.text[0], "Canary", 6);
        r.length[0] = 6;
    });
    apply([&](InstanceRecord& r) {
        r.event = static_cast<std::uint8_t>(EventKind::Bridge);
        r.bridge = 1;
        r.how = static_cast<std::uint8_t>(how);
        r.status = status;
        if (!incident.empty()) std::memcpy(r.text[3], incident.data(), incident.size());
        r.length[3] = static_cast<std::uint8_t>(incident.size());
    });
    return inventory;
}
std::vector<fs::path> jsonFiles(const fs::path& root) {
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(root))
        if (e.path().extension() == ".json") found.push_back(e.path());
    return found;
}
}  // namespace
TEST_CASE("a bridge's death is published as a report of its own, valid, with no run marker") {
    Root root;
    const auto inventory = bridgeThatEnded(supple::diagctx::BridgeHow::Signal, 11);
    REQUIRE(publishBridgeReport(root.path, boot(0), inventory, 1));
    const auto files = jsonFiles(root.path);
    REQUIRE(files.size() == 1);
    // Its own incident, not the run's, and nothing but the report is written for it.
    CHECK(files[0].stem().string() != std::string(boot(0).incident.data(), 32));
    for (const auto& e : fs::directory_iterator(root.path)) CHECK(e.path().extension() == ".json");
    const auto text = readTextFile(files[0]);
    REQUIRE(text);
    const auto parsed = json::parse(*text);
    REQUIRE(parsed);
    CHECK(parsed->find("incident")->asString() == files[0].stem().string());
    CHECK(parsed->find("kind")->asString() == "bridge_exit");
    CHECK(parsed->find("bridge")->asInt() == 1);
    CHECK(parsed->find("code")->asInt() == 11);
    CHECK(parsed->find("complete")->asBool() == false);
    CHECK(parsed->find("terminal_context")->asBool() == false);
    CHECK(parsed->find("threads")->size() == 0);
    CHECK(parsed->find("attribution")->find("confidence")->asString() == "bridge_exit");
    CHECK(parsed->find("attribution")->find("plugin")->asInt() == 0);
    CHECK(reportShape(*parsed));
    CHECK(reportShape(*parsed, true));
    // Retention keeps it beside twenty crash reports, since bridge reports have a budget of
    // their own, and removes it as the oldest once five newer bridge reports are there.
    fs::last_write_time(files[0], fs::file_time_type::clock::now() - std::chrono::hours(48));
    for (unsigned i = 1; i < 21; ++i)
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false));
    CHECK(reports(root.path) == 21);
    CHECK(fs::exists(files[0]));
    for (unsigned i = 0; i + 1 < kKeepBridgeReports; ++i)
        REQUIRE(publishBridgeReport(root.path, boot(0), inventory, 1));
    CHECK(fs::exists(files[0]));
    REQUIRE(publishBridgeReport(root.path, boot(0), inventory, 1));
    CHECK_FALSE(fs::exists(files[0]));
    CHECK(reports(root.path) == 20 + kKeepBridgeReports);
}
TEST_CASE("a bridge's report is written under the id the worker minted for it") {
    Root root;
    const std::string id = "00112233445566778899aabbccddeeff";
    const auto inventory = bridgeThatEnded(supple::diagctx::BridgeHow::Signal, 11, id);
    REQUIRE(publishBridgeReport(root.path, boot(0), inventory, 1));
    const auto files = jsonFiles(root.path);
    REQUIRE(files.size() == 1);
    CHECK(files[0].stem().string() == id);
    const auto parsed = json::parse(*readTextFile(files[0]));
    REQUIRE(parsed);
    CHECK(parsed->find("incident")->asString() == id);
    CHECK(reportShape(*parsed));
    // Not an id the run itself already has, and without one the supervisor makes its own.
    CHECK(id != std::string(boot(0).incident.data(), 32));
    Root other;
    const auto without = bridgeThatEnded(supple::diagctx::BridgeHow::Signal, 11);
    REQUIRE(publishBridgeReport(other.path, boot(0), without, 1));
    const auto made = jsonFiles(other.path);
    REQUIRE(made.size() == 1);
    CHECK(made[0].stem().string().size() == 32);
}
TEST_CASE("a bridge report is prepared in memory and written separately, to the same file") {
    Root root;
    const std::string id = "00112233445566778899aabbccddeeff";
    const auto inventory = bridgeThatEnded(supple::diagctx::BridgeHow::Signal, 11, id);
    const auto prepared = prepareBridgeReport(boot(0), inventory, 1);
    REQUIRE(prepared);
    CHECK(prepared->id == id);
    CHECK(jsonFiles(root.path).empty());  // nothing on disk yet: preparing only formats
    REQUIRE(writeBridgeReport(root.path, *prepared));
    const auto files = jsonFiles(root.path);
    REQUIRE(files.size() == 1);
    CHECK(files[0].stem().string() == id);
    CHECK(*readTextFile(files[0]) == prepared->text);
    CHECK_FALSE(writeBridgeReport(root.path, *prepared));  // never replaces a report
    // The same report publishBridgeReport makes (the two are one operation to its callers).
    Root together;
    REQUIRE(publishBridgeReport(together.path, boot(0), inventory, 1));
    REQUIRE(jsonFiles(together.path).size() == 1);
    CHECK(*readTextFile(jsonFiles(together.path)[0]) == prepared->text);
    // Not a bridge that is there and ended: nothing to prepare.
    CHECK_FALSE(prepareBridgeReport(boot(0), inventory, 2));
    const auto running = bridgeThatEnded(supple::diagctx::BridgeHow::None, 0);
    CHECK_FALSE(prepareBridgeReport(boot(0), running, 1));
}
// ---- a bridge report with the bridge's own capsule (topic 6c.4)
// -----------------------------------
namespace {
// Two bridges that ended by a signal, each with two members (Canary and Other); the worker's own
// "Host" plug-in sits at an address that is also inside Canary's image, in another process.
struct CapsuleWorld {
    Inventory inventory;
    TraceSnapshot trace;  // bridge 2's capsule
};
Packet instancePacket(std::uint32_t cell, std::uint16_t bridge, std::string_view name,
                      std::string_view id) {
    using namespace supple::diagctx;
    Packet p = boot(0);
    p.kind = Message::Instance;
    auto& r = p.instance;
    r.cell = cell;
    r.generation = 1;
    r.event = static_cast<std::uint8_t>(EventKind::Add);
    r.kind = static_cast<std::uint8_t>(InstanceKind::Clap);
    r.origin = static_cast<std::uint8_t>(InstanceOrigin::Foreign);
    r.mode = static_cast<std::uint8_t>(bridge ? InstanceMode::Bridged : InstanceMode::InProcess);
    r.state = static_cast<std::uint8_t>(InstanceState::Active);
    r.bridge = bridge;
    r.image = 0x55550010;  // the worker's own address: never looked up in a bridge's capsule
    std::memcpy(r.text[0], name.data(), name.size());
    r.length[0] = static_cast<std::uint8_t>(name.size());
    std::memcpy(r.text[2], "1.2", 3);
    r.length[2] = 3;
    std::memcpy(r.text[3], id.data(), id.size());
    r.length[3] = static_cast<std::uint8_t>(id.size());
    return p;
}
Packet childPacket(std::uint32_t cell, std::uint32_t childCell, std::uint16_t childGeneration,
                   std::uint64_t image) {
    using namespace supple::diagctx;
    Packet p = boot(0);
    p.kind = Message::Instance;
    p.instance.event = static_cast<std::uint8_t>(EventKind::Child);
    p.instance.cell = cell;
    p.instance.generation = 1;
    p.instance.childCell = childCell;
    p.instance.childGeneration = childGeneration;
    p.instance.image = image;
    return p;
}
Packet bridgeEnds(std::uint16_t bridge) {
    using namespace supple::diagctx;
    Packet p = boot(0);
    p.kind = Message::Instance;
    p.instance.event = static_cast<std::uint8_t>(EventKind::Bridge);
    p.instance.bridge = bridge;
    p.instance.how = static_cast<std::uint8_t>(BridgeHow::Signal);
    p.instance.status = 11;
    return p;
}
// The build ids the capsule's two images carry; Canary's image is the first, Other's the second.
TraceSnapshot capsuleOf(const char* canaryBuild, const char* otherBuild) {
    TraceSnapshot t;
    t.table = TableState::Read;
    t.modules.resize(3);
    t.modules[0].id = canaryBuild;
    t.modules[1].id = otherBuild;  // modules[2] has no identity: a system library
    t.regions = {
        {0x55550000, 0x55560000, 0}, {0x66660000, 0x66670000, 1}, {0x77770000, 0x77780000, 2}};
    t.threads.resize(1);
    t.threads[0].role = supple::diagnostics::detail::Role::Fault;
    t.threads[0].missing = Missing::None;
    t.threads[0].registered = supple::diagctx::ThreadRole::BridgeAudio;
    t.threads[0].worker = 1;
    Frame frame;
    frame.module = 0;
    frame.offset = 0x100;
    t.threads[0].walk.frames.push_back(frame);
    return t;
}
void world(CapsuleWorld& w, bool twin = false) {
    auto& i = w.inventory;
    REQUIRE(i.apply(instancePacket(1, 0, "Host", "com.acme.host")));
    for (const std::uint16_t bridge : {std::uint16_t{2}, std::uint16_t{3}}) {
        if (bridge == 3 && !twin) break;
        const std::uint32_t base = bridge == 2 ? 5 : 7;
        REQUIRE(i.apply(instancePacket(base, bridge, "Canary", "com.acme.canary")));
        REQUIRE(i.apply(instancePacket(base + 1, bridge, "Other", "com.acme.other")));
        REQUIRE(i.apply(childPacket(base, 4, 7, 0x55550000)));
        REQUIRE(i.apply(childPacket(base + 1, 9, 3, 0x66660000)));
        REQUIRE(i.apply(bridgeEnds(bridge)));
    }
    w.trace = capsuleOf("aabbccdd", "11223344");
}
json::Value bridgeReportOf(const CapsuleWorld& w, const TraceSnapshot* trace) {
    const auto prepared = prepareBridgeReport(boot(0), w.inventory, 2, trace);
    REQUIRE(prepared);
    auto parsed = json::parse(prepared->text);
    REQUIRE(parsed);
    return std::move(*parsed);
}
const json::Value* pluginNamed(const json::Value& report, std::string_view name) {
    for (const auto& p : report.find("plugins")->asArray())
        if (const auto* n = p.find("name"); n && n->asString() == name) return &p;
    return nullptr;
}
}  // namespace

TEST_CASE(
    "a bridge's own capsule puts its threads, modules and the plug-in it faulted in in the "
    "report") {
    CapsuleWorld w;
    world(w);
    const auto report = bridgeReportOf(w, &w.trace);
    CHECK(report.find("kind")->asString() == "bridge_exit");
    CHECK(report.find("capture")->asString() == "bridge");  // whose account it is
    CHECK(report.find("complete")->asBool() == false);
    CHECK(report.find("terminal_context")->asBool() == false);
    CHECK(report.find("threads")->size() == 1);
    CHECK(report.find("threads")->asArray()[0].find("role")->asString() == "fault");
    CHECK(report.find("modules")->size() == 3);
    // The members' builds are found in the bridge's capsule by the bridge's own addresses; the
    // worker's own plug-in sits at an address inside Canary's image and gets no build from it.
    CHECK(pluginNamed(report, "Canary")->find("build")->find("id")->asString() == "aabbccdd");
    CHECK(pluginNamed(report, "Other")->find("build")->find("id")->asString() == "11223344");
    CHECK(pluginNamed(report, "Host")->find("build") == nullptr);
    const auto* attribution = report.find("attribution");
    CHECK(attribution->find("confidence")->asString() == "fault_in_image");
    CHECK(attribution->find("plugin")->asInt() ==
          pluginNamed(report, "Canary")->find("id")->asInt());
    CHECK(reportShape(report));
    CHECK(reportShape(report, true));
    // Nothing of the capsule is an address.
    CHECK(json::writeCanonical(report).find("55550000") == std::string::npos);
}

TEST_CASE(
    "a bridge's capsule names a plug-in by its token when the fault is in code it cannot place") {
    CapsuleWorld w;
    world(w);
    auto& fault = w.trace.threads[0];
    fault.walk.frames[0].module = 2;  // a system library: no identity
    fault.entry = supple::diagctx::Entry::Process;
    fault.entryCell = 9;  // Other's cell in the bridge's own table
    fault.entryGeneration = 3;
    const auto report = bridgeReportOf(w, &w.trace);
    const auto* attribution = report.find("attribution");
    CHECK(attribution->find("confidence")->asString() == "active_token");
    const auto* other = pluginNamed(report, "Other");
    CHECK(attribution->find("plugin")->asInt() == other->find("id")->asInt());
    CHECK(other->find("executing")->asArray()[0].find("match")->asString() == "thread");
    CHECK(pluginNamed(report, "Canary")->find("executing") == nullptr);
    CHECK(reportShape(report));
    // The host's own instance in cell 9 of the worker is not the bridge's: only the bridge's cells
    // are looked up for a bridge's capsule.
    CHECK(pluginNamed(report, "Host")->find("executing") == nullptr);
}

TEST_CASE("a bridge's capsule that points at nobody leaves the report at the bridge_exit level") {
    CapsuleWorld w;
    world(w);
    auto& fault = w.trace.threads[0];
    fault.walk.frames[0].module = 2;  // no identity, and no token
    // Another thread was inside Canary (its cell in the bridge's table): activity, not blame.
    ThreadTrace other;
    other.role = supple::diagnostics::detail::Role::Other;
    other.missing = Missing::NotCollected;
    other.registered = supple::diagctx::ThreadRole::BridgeControl;
    other.entry = supple::diagctx::Entry::Create;
    other.entryCell = 4;
    other.entryGeneration = 7;
    w.trace.threads.push_back(other);
    const auto report = bridgeReportOf(w, &w.trace);
    CHECK(report.find("attribution")->find("confidence")->asString() == "bridge_exit");
    CHECK(report.find("threads")->size() == 2);
    CHECK(
        pluginNamed(report, "Canary")->find("executing")->asArray()[0].find("match")->asString() ==
        "other_thread");
    CHECK(reportShape(report));
}

TEST_CASE("a bridge report without a capsule is as it was, and the validator holds the two apart") {
    CapsuleWorld w;
    world(w);
    const auto bare = bridgeReportOf(w, nullptr);
    CHECK(bare.find("capture") == nullptr);
    CHECK(bare.find("modules") == nullptr);
    CHECK(bare.find("threads")->size() == 0);
    CHECK(bare.find("attribution")->find("confidence")->asString() == "bridge_exit");
    CHECK(reportShape(bare));
    const auto with = bridgeReportOf(w, &w.trace);
    const auto breaks = [&](const char* what, auto change, const json::Value& from) {
        auto copy = from;
        change(copy);
        INFO(what);
        CHECK_FALSE(reportShape(copy));
        CHECK_FALSE(reportShape(copy, true));
    };
    // A capsule's report that has lost its capsule, or has it without saying whose it is.
    breaks("threads without capture", [](json::Value& v) { v.asObject().erase("capture"); }, with);
    breaks("another account", [](json::Value& v) { v["capture"] = "worker"; }, with);
    breaks("capture of nothing", [](json::Value& v) { v["capture"] = json::Value(); }, with);
    breaks(
        "capture without threads",
        [](json::Value& v) {
            v["threads"] = json::Value::array();
            v.asObject().erase("modules");
        },
        with);
    breaks("capture without modules", [](json::Value& v) { v.asObject().erase("modules"); }, with);
    breaks(
        "a first thread that is not the fault thread",
        [](json::Value& v) { v["threads"].asArray()[0]["role"] = "other"; }, with);
    breaks(
        "threads in a report with no capture",
        [](json::Value& v) {
            v.asObject().erase("capture");
            v["attribution"]["confidence"] = "bridge_exit";
        },
        with);
    // The levels a bridge report may claim, and the evidence each is held to.
    breaks(
        "a worker's concurrent_activity",
        [](json::Value& v) {
            v["attribution"]["confidence"] = "concurrent_activity";
            v["attribution"]["plugin"] = json::Value();
        },
        with);
    breaks(
        "recent_command",
        [](json::Value& v) {
            v["attribution"]["confidence"] = "recent_command";
            v["attribution"]["plugin"] = json::Value();
        },
        with);
    breaks(
        "an unknown level",
        [](json::Value& v) {
            v["attribution"]["confidence"] = "unknown";
            v["attribution"]["plugin"] = json::Value();
        },
        with);
    breaks(
        "fault_in_image naming a plug-in of another build",
        [&](json::Value& v) {
            v["attribution"]["plugin"] = pluginNamed(v, "Other")->find("id")->asInt();
        },
        with);
    breaks(
        "fault_in_image naming the worker's own plug-in",
        [&](json::Value& v) {
            v["attribution"]["plugin"] = pluginNamed(v, "Host")->find("id")->asInt();
        },
        with);
    breaks(
        "fault_in_image with no build to match",
        [](json::Value& v) {
            for (auto& p : v["plugins"].asArray()) p.asObject().erase("build");
        },
        with);
    breaks(
        "active_token without a thread inside the plug-in",
        [](json::Value& v) { v["attribution"]["confidence"] = "active_token"; }, with);
    // Without a capsule the only level there is stays the only level.
    breaks(
        "fault_in_image without a capsule",
        [](json::Value& v) { v["attribution"]["confidence"] = "fault_in_image"; }, bare);
    breaks(
        "active_token without a capsule",
        [](json::Value& v) { v["attribution"]["confidence"] = "active_token"; }, bare);
    breaks(
        "capture on a report of the host's account",
        [](json::Value& v) { v["capture"] = "bridge"; }, bare);
}

TEST_CASE("the repeat rule tells builds apart where the version could not") {
    // Two bridges with the same members and the same status. Without their capsules they are one
    // repeat; with them they are one only when the builds are the same.
    const auto admit = [](const TraceSnapshot* first, const TraceSnapshot* second) {
        CapsuleWorld w;
        world(w, true);
        const bool one = w.inventory.admitBridgeReport(2, first);
        const bool two = w.inventory.admitBridgeReport(3, second);
        return std::make_pair(one, two);
    };
    CHECK(admit(nullptr, nullptr) == std::make_pair(true, false));  // as before
    const auto same = capsuleOf("aabbccdd", "11223344");
    const auto newer = capsuleOf("ddccbbaa", "11223344");
    CHECK(admit(&same, &same) == std::make_pair(true, false));
    CHECK(admit(&same, &newer) == std::make_pair(true, true));
}

TEST_CASE("background work is queued without waiting, runs in order off the caller's thread") {
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false, firstStarted = false;
    std::vector<int> order;
    std::vector<std::thread::id> threads;
    {
        BackgroundWriter writer;
        writer.post([&] {
            std::unique_lock lock(mutex);
            firstStarted = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });  // holds the thread until released
            order.push_back(1);
            threads.push_back(std::this_thread::get_id());
        });
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return firstStarted; });
        }
        // The first piece is still running; posting more returns at once, not after it.
        writer.post([&] {
            std::lock_guard lock(mutex);
            order.push_back(2);
        });
        writer.post([&] {
            std::lock_guard lock(mutex);
            order.push_back(3);
        });
        {
            std::lock_guard lock(mutex);
            CHECK(order.empty());  // none has finished: the posts did not wait for the first
            release = true;
        }
        changed.notify_all();
        writer.finish();  // drains what is queued, then joins
        CHECK(order == std::vector<int>{1, 2, 3});
        REQUIRE(threads.size() == 1);
        CHECK(threads[0] != std::this_thread::get_id());
        // After finish, work runs at once on the caller's thread; finish again is harmless.
        std::thread::id after;
        writer.post([&] { after = std::this_thread::get_id(); });
        CHECK(after == std::this_thread::get_id());
        writer.finish();
    }
}
TEST_CASE("a background writer that is destroyed first runs what it was given") {
    std::vector<int> done;
    {
        BackgroundWriter writer;
        for (int i = 0; i < 5; ++i) writer.post([&done, i] { done.push_back(i); });
    }  // the destructor finishes
    CHECK(done == std::vector<int>{0, 1, 2, 3, 4});
}
TEST_CASE("a bridge_exit report is valid only as the report of a bridge that died on its own") {
    Root root;
    const auto inventory = bridgeThatEnded(supple::diagctx::BridgeHow::Exception, -1073741819);
    REQUIRE(publishBridgeReport(root.path, boot(0), inventory, 1));
    const auto files = jsonFiles(root.path);
    REQUIRE(files.size() == 1);
    const auto good = *json::parse(*readTextFile(files[0]));
    REQUIRE(reportShape(good));
    const auto altered = [&](auto&& change) {
        auto v = good;
        change(v);
        return reportShape(v) || reportShape(v, true);
    };
    CHECK_FALSE(altered([](json::Value& v) { v["kind"] = "crash"; }));  // it has a bridge field
    CHECK_FALSE(altered([](json::Value& v) { v.asObject().erase("bridge"); }));
    CHECK_FALSE(altered([](json::Value& v) { v["bridge"] = 2; }));  // not a listed bridge
    CHECK_FALSE(altered([](json::Value& v) { v["bridge"] = 0; }));
    CHECK_FALSE(altered([](json::Value& v) { v["bridge"] = "1"; }));
    CHECK_FALSE(altered([](json::Value& v) { v["code"] = 12; }));  // not the bridge's code
    CHECK_FALSE(altered([](json::Value& v) { v["terminal_context"] = true; }));
    CHECK_FALSE(altered([](json::Value& v) { v["native_fatal_status"] = true; }));
    CHECK_FALSE(altered([](json::Value& v) { v.asObject().erase("bridges"); }));
    CHECK_FALSE(altered([](json::Value& v) { v.asObject().erase("attribution"); }));
    CHECK_FALSE(altered([](json::Value& v) { v["attribution"]["confidence"] = "unknown"; }));
    CHECK_FALSE(altered([](json::Value& v) { v["attribution"]["plugin"] = 5; }));
    CHECK_FALSE(altered([](json::Value& v) {  // the host's kill is not a death
        v["bridges"].asArray()[0]["ended"]["how"] = "host";
    }));
    CHECK_FALSE(
        altered([](json::Value& v) { v["bridges"].asArray()[0]["ended"]["how"] = "exit"; }));
    // A crash report is not one of these either: it may not name a bridge or claim the level.
    auto crash = good;
    crash["kind"] = "unexpected_exit";
    crash.asObject().erase("bridge");
    CHECK_FALSE(reportShape(crash));  // its attribution still says bridge_exit
    crash["attribution"]["confidence"] = "unknown";
    crash["attribution"]["plugin"] = json::Value();
    CHECK(reportShape(crash));
    // Only a bridge that is over, and ended, is published; nothing is written otherwise.
    Inventory running;
    Packet start = boot(0);
    start.kind = Message::Instance;
    start.instance.event = static_cast<std::uint8_t>(supple::diagctx::EventKind::Bridge);
    start.instance.bridge = 1;
    REQUIRE(running.apply(start));
    const auto before = jsonFiles(root.path).size();
    CHECK_FALSE(publishBridgeReport(root.path, boot(0), running, 1));
    CHECK_FALSE(publishBridgeReport(root.path, boot(0), inventory, 9));
    CHECK(jsonFiles(root.path).size() == before);
}
TEST_CASE("a report with command entries is published, validated, pruned, and kept when altered") {
    namespace dc = supple::diagctx;
    Root root;
    Inventory inventory;
    Packet names = boot(0);
    names.kind = Message::Instance;
    names.instance.event = static_cast<std::uint8_t>(dc::EventKind::Names);
    std::memcpy(names.instance.text[0], "track.add", 9);
    names.instance.length[0] = 9;
    REQUIRE(inventory.apply(names));
    TraceSnapshot trace;
    trace.table = TableState::Read;
    trace.threads.resize(1);
    trace.threads[0].role = Role::Fault;
    trace.commands = {{8, 0, dc::CommandOrigin::Mcp, dc::CommandState::Ended},
                      {9, 5, dc::CommandOrigin::Ui, dc::CommandState::Begun}};
    const auto file = [](unsigned i) { return std::string(boot(i).incident.data()) + ".json"; };
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false, &trace, &inventory));
    const auto text = readTextFile(root.path / file(0));
    REQUIRE(text);
    const auto parsed = json::parse(*text);
    REQUIRE(parsed);
    const auto* commands = parsed->find("commands");
    REQUIRE(commands);
    REQUIRE(commands->size() == 2);
    CHECK(commands->asArray()[0].find("name")->asString() == "track.add");
    CHECK(commands->asArray()[1].find("name")->asString() == "unknown");
    CHECK(commands->asArray()[1].find("state")->asString() == "begun");
    // A report whose commands were rewritten into another shape is kept, never pruned.
    auto changed = *parsed;
    changed["commands"].asArray()[0]["args"] = "canary";
    const auto altered = root.path / file(0);
    REQUIRE(privateRemove(altered));
    REQUIRE(privatePublish(altered, json::writeCanonical(changed)));
    fs::last_write_time(altered, fs::file_time_type::clock::now() - std::chrono::hours(48));
    for (unsigned i = 1; i < 22; ++i)
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false, &trace, &inventory));
    CHECK(fs::exists(altered));       // not a report it can vouch for
    CHECK(reports(root.path) == 21);  // twenty it owns, and the one it keeps
}
TEST_CASE("diagnostic byte retention applies independently of report count") {
    Root root;
    for (unsigned i = 0; i < 14; ++i) {
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false));
        const auto name = root.path / (std::string(boot(i).incident.data()) + ".json");
        auto text = readTextFile(name);
        REQUIRE(text);
        auto parsed = json::parse(*text);
        REQUIRE(parsed);
        auto v = std::move(*parsed);
        v["unavailable"] = json::Value::array();
        for (int n = 0; n < 128; ++n) v["unavailable"].asArray().push_back(std::string(31000, 'x'));
        const auto file = root.path / (std::string(boot(i).incident.data()) + ".json");
        REQUIRE(privateRemove(file));
        REQUIRE(privatePublish(file, json::writeCanonical(v)));
        fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(24) +
                                      std::chrono::seconds(i));
    }
    REQUIRE(pruneReports(root.path));
    CHECK(reports(root.path) == 13);
}
TEST_CASE("diagnostics refuses a linked owned directory") {
#if !defined(_WIN32)
    Root root;
    fs::path link = root.path.parent_path() / "linked";
    fs::create_directory_symlink(root.path, link);
    CHECK_FALSE(prepareDirectory(link));
#endif
}

TEST_CASE("private diagnostic files reject hard links and oversized payloads") {
    Root root;
    const auto file = root.path / "private.json";
    REQUIRE(privatePublish(file, "{}"));
    REQUIRE(privateRead(file));
#if !defined(_WIN32)
    CHECK((fs::status(file).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
          fs::perms::none);
#endif
    const auto alias = root.path / "alias.json";
    fs::create_hard_link(file, alias);
    CHECK_FALSE(privateRead(file));
    CHECK_FALSE(privateRead(alias));
    CHECK_FALSE(privateRemove(file));
    REQUIRE(fs::remove(alias));
    REQUIRE(privateRead(file));
    fs::resize_file(file, 4 * 1024 * 1024 + 1);
    CHECK_FALSE(privateRead(file));
}

TEST_CASE("report publication never removes an unowned pre-existing run marker") {
    Root root;
    const auto marker = root.path / (std::string(boot(0).incident.data()) + ".run");
    REQUIRE(writeFileAtomic(marker, "not created by this run"));
    CHECK_FALSE(createMarker(root.path, boot(0)));
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false));
    CHECK(fs::exists(marker));
}

TEST_CASE("diagnostic retention preserves malformed report-shaped entries") {
    Root root;
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false));
    const auto seed = root.path / (std::string(boot(0).incident.data()) + ".json");
    auto text = privateRead(seed);
    REQUIRE(text);
    auto parsed = json::parse(*text);
    REQUIRE(parsed);
    REQUIRE(privateRemove(seed));

    auto item = json::Value::array();
    item.asArray().push_back("not an object");
    auto objectItem = json::Value::array();
    objectItem.asArray().push_back(json::Value::object());
    auto badBuild = (*parsed)["build"];
    badBuild["dirty"] = "false";
    const std::pair<const char*, json::Value> invalid[]{
        {"role", "unknown"},
        {"kind", "unknown"},
        {"platform", "unknown"},
        {"architecture", "mips"},
        {"threads", item},
        {"plugins", item},
        {"commands", item},
        // Later-topic nested schemas are not implemented: do not authorize deletion.
        {"threads", objectItem},
        {"plugins", objectItem},
        {"commands", objectItem},
        {"code", -2147483649LL},
        {"code", 4294967296LL},
        {"cause", -1},
        {"cause", 4294967296LL},
        {"dropped", -1},
        {"dropped", objectItem},
        {"dropped", "3"},
        // Attribution must be one the report's own plug-ins and commands support.
        {"attribution", item},
        {"attribution", objectItem},
        {"attribution", json::parse(R"({"confidence":"active_token","plugin":0})").value()},
        {"attribution", json::parse(R"({"confidence":"sure","plugin":null})").value()},
        {"complete", true},
        {"unavailable", json::Value::array()},
        {"unavailable", objectItem},
        {"build", badBuild},
        {"future_field", true},
    };
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours(48);
    unsigned id = 128;
    for (const auto& [key, value] : invalid) {
        auto report = *parsed;
        report["incident"] = std::string(boot(id).incident.data());
        report[key] = value;
        const auto file = root.path / (std::string(boot(id++).incident.data()) + ".json");
        REQUIRE(privatePublish(file, json::writeCanonical(report)));
        fs::last_write_time(file, oldest);
    }
    for (unsigned i = 0; i < 21; ++i) {
        REQUIRE(publishReport(root.path, boot(i), nullptr, 31, false));
        fs::last_write_time(root.path / (std::string(boot(i).incident.data()) + ".json"),
                            oldest + std::chrono::hours(24) + std::chrono::seconds(i));
    }
    id = 128;
    for (const auto& [key, value] : invalid) {
        CAPTURE(key);
        CAPTURE(json::writeCanonical(value));
        CHECK(fs::exists(root.path / (std::string(boot(id++).incident.data()) + ".json")));
    }
    CHECK(reports(root.path) == 20 + std::size(invalid));
    CHECK_FALSE(fs::exists(root.path / (std::string(boot(0).incident.data()) + ".json")));
    CHECK(fs::exists(root.path / (std::string(boot(20).incident.data()) + ".json")));
}

TEST_CASE("diagnostic retention tolerates reports disappearing during metadata scan") {
    for (const std::string_view point : {"status", "size", "time"}) {
        CAPTURE(point);
        Root root;
        REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false));
        bool removed = false;
        CHECK(pruneReports(root.path, [&](const fs::path& path, std::string_view phase) {
            if (phase == point) {
                REQUIRE(privateRemove(path));
                removed = true;
            }
        }));
        CHECK(removed);
        CHECK(reports(root.path) == 0);
    }
}

TEST_CASE("concurrent diagnostic pruners can remove the same oldest report") {
    Root root;
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false));
    const auto file = root.path / (std::string(boot(0).incident.data()) + ".json");
    auto text = privateRead(file);
    REQUIRE(text);
    auto parsed = json::parse(*text);
    REQUIRE(parsed);
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(file, oldest);
    for (unsigned i = 1; i < 21; ++i) {
        (*parsed)["incident"] = std::string(boot(i).incident.data());
        const auto next = root.path / (std::string(boot(i).incident.data()) + ".json");
        REQUIRE(privatePublish(next, json::writeCanonical(*parsed)));
        fs::last_write_time(next, oldest + std::chrono::seconds(i));
    }
    SUBCASE("a genuine removal refusal still fails storage") {
        const auto alias = root.path / "retained-alias";
        const auto result =
            pruneReports(root.path, [&](const fs::path& path, std::string_view phase) {
                if (phase == "remove") fs::create_hard_link(path, alias);
            });
        CHECK_FALSE(result);
        CHECK(reports(root.path) == 21);
        CHECK(fs::exists(file));
        CHECK(fs::exists(alias));
    }
    SUBCASE("another pruner's removal is harmless") {
        std::barrier rendezvous(2);
        std::array<Status, 2> results;
        auto prune = [&](std::size_t index) {
            results[index] = pruneReports(root.path, [&](const fs::path&, std::string_view phase) {
                // Both snapshots include 21 reports and both select the same oldest.
                if (phase == "remove") rendezvous.arrive_and_wait();
            });
        };
        std::thread first(prune, 0);
        std::thread second(prune, 1);
        first.join();
        second.join();
        CHECK(results[0]);
        CHECK(results[1]);
        CHECK(reports(root.path) == 20);
        CHECK_FALSE(fs::exists(file));
    }
}

TEST_CASE("diagnostic removal is idempotent but preserves genuine refusal") {
    Root root;
    const auto file = root.path / "private.json";
    CHECK(privateRemove(file));
    REQUIRE(privatePublish(file, "{}"));
    CHECK(privateRemove(file));
    CHECK(privateRemove(file));
    REQUIRE(privatePublish(file, "{}"));
    const auto alias = root.path / "alias.json";
    fs::create_hard_link(file, alias);
    CHECK_FALSE(privateRemove(file));
    CHECK(fs::exists(file));
    CHECK(fs::exists(alias));
    CHECK_FALSE(privateRemove(root.path));
}

TEST_CASE("removal failure classification separates delete-pending from refusal") {
    constexpr std::uint32_t notFound = 2, pathNotFound = 3, denied = 5, sharing = 32;
    constexpr std::uint32_t deletePending = 0xC0000056u;  // STATUS_DELETE_PENDING
    constexpr std::uint32_t cannotDelete = 0xC0000121u;   // STATUS_CANNOT_DELETE
    constexpr std::uint32_t accessDenied = 0xC0000022u;   // STATUS_ACCESS_DENIED
    // Absent files are removed whatever the status says.
    CHECK(removalTargetGone(notFound, std::nullopt));
    CHECK(removalTargetGone(pathNotFound, std::nullopt));
    CHECK(removalTargetGone(notFound, accessDenied));
    // A delete-pending file is another remover's work in progress.
    CHECK(removalTargetGone(denied, deletePending));
    // Real refusals and unknown statuses stay errors.
    CHECK_FALSE(removalTargetGone(denied, accessDenied));
    CHECK_FALSE(removalTargetGone(denied, cannotDelete));
    CHECK_FALSE(removalTargetGone(denied, 0));
    CHECK_FALSE(removalTargetGone(denied, std::nullopt));
    CHECK_FALSE(removalTargetGone(sharing, deletePending));
    CHECK_FALSE(removalTargetGone(0, deletePending));
}

#if defined(_WIN32)
TEST_CASE("windows removal tolerates a pending deletion but not a refusal") {
    Root root;
    const auto file = root.path / "pending.json";
    REQUIRE(privatePublish(file, "{}"));
    // Another pruner has condemned the report and still holds its handle: every open
    // of the name now fails with ERROR_ACCESS_DENIED until that handle closes.
    HANDLE condemning = CreateFileW(file.c_str(), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(condemning != INVALID_HANDLE_VALUE);
    FILE_DISPOSITION_INFO disposition{TRUE};
    REQUIRE(SetFileInformationByHandle(condemning, FileDispositionInfo, &disposition,
                                       sizeof(disposition)));
    HANDLE probe =
        CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD probeError = GetLastError();
    CHECK(probe == INVALID_HANDLE_VALUE);  // the precondition: the name is delete-pending
    CHECK(probeError == ERROR_ACCESS_DENIED);
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    CHECK(privateRemove(file));
    CHECK(privateRemove(file));
    CloseHandle(condemning);
    CHECK_FALSE(fs::exists(file));
    CHECK(privateRemove(file));

    // A genuine refusal reports the same Win32 error with a different NT status.
    const auto readonly = root.path / "readonly.json";
    REQUIRE(privatePublish(readonly, "{}"));
    REQUIRE(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY));
    CHECK_FALSE(privateRemove(readonly));
    CHECK(fs::exists(readonly));
    REQUIRE(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_NORMAL));
    CHECK(privateRemove(readonly));
}
#endif

TEST_CASE("diagnostic pruning preserves genuine metadata errors") {
    Root root;
    REQUIRE(publishReport(root.path, boot(0), nullptr, 31, false));
    const auto result = pruneReports(root.path, [&](const fs::path& path, std::string_view phase) {
        if (phase == "size") {
            REQUIRE(privateRemove(path));
            REQUIRE(fs::create_directory(path));
        }
    });
#if defined(_WIN32)
    // MSVC's directory_entry answers size and time from the enumeration, so the entry
    // replaced after it was listed raises no error there; the directory is then not a
    // readable report and is skipped. A genuine refusal on Windows is the removal case
    // ("a genuine removal refusal still fails storage").
    CHECK(result);
#else
    CHECK_FALSE(result);
#endif
    CHECK(fs::is_directory(root.path / (std::string(boot(0).incident.data()) + ".json")));
}
