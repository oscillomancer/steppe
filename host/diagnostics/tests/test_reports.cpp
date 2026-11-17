// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include "../src/inventory.h"
#include "../src/private_files.h"
#include "../src/storage.h"
#include "supple/base/files.h"
#include "supple/diagnostics/reports.h"
#include "supple/json/json.h"
using namespace supple;
using namespace supple::diagnostics;
using namespace supple::diagnostics::detail;
namespace fs = std::filesystem;
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
std::string name(unsigned id) { return std::string(boot(id).incident.data(), 32); }
fs::path file(const Root& root, unsigned id, const char* extension) {
    return root.path / (name(id) + extension);
}
// A report of the kind the supervisor writes when it saw no terminal packet.
void report(const Root& root, unsigned id, const Inventory* inventory = nullptr,
            const TraceSnapshot* trace = nullptr) {
    REQUIRE(publishReport(root.path, boot(id), nullptr, 31, false, trace, inventory));
}
// A report text of a build that knows more: `edit` changes the parsed report, which is written
// back under the same name.
void rewrite(const Root& root, unsigned id, const std::function<void(json::Value&)>& edit) {
    auto text = readTextFile(file(root, id, ".json"));
    REQUIRE(text);
    auto value = json::parse(*text);
    REQUIRE(value);
    edit(*value);
    REQUIRE(privateRemove(file(root, id, ".json")));  // publishing does not replace on Windows
    REQUIRE(privatePublish(file(root, id, ".json"), json::writeCanonical(*value) + "\n"));
}
void age(const Root& root, unsigned id, int hours) {
    fs::last_write_time(file(root, id, ".json"),
                        fs::file_time_type::clock::now() - std::chrono::hours(hours));
}
// A run that ended without anyone removing its marker: created, then its holder gone.
void staleMarker(const Root& root, unsigned id) {
    auto handle = createMarker(root.path, boot(id));
    REQUIRE(handle);
    releaseMarker(*handle);
}
TraceSnapshot faultIn(const std::string& moduleId) {
    TraceSnapshot trace;
    trace.table = TableState::Read;
    trace.modules.resize(1);
    trace.modules[0].id = moduleId;
    trace.regions = {{0x5000, 0x6000, 0}};
    trace.threads.resize(1);
    trace.threads[0].role = Role::Fault;
    trace.threads[0].missing = Missing::None;
    Frame frame;
    frame.module = 0;
    trace.threads[0].walk.frames.push_back(frame);
    return trace;
}
Packet plugin(std::uint32_t cell, std::uint64_t image, std::string_view pluginName) {
    Packet p = boot(0);
    p.kind = Message::Instance;
    auto& r = p.instance;
    r.cell = cell;
    r.event = static_cast<std::uint8_t>(diagctx::EventKind::Add);
    r.kind = static_cast<std::uint8_t>(diagctx::InstanceKind::Clap);
    r.origin = static_cast<std::uint8_t>(diagctx::InstanceOrigin::Foreign);
    r.mode = static_cast<std::uint8_t>(diagctx::InstanceMode::InProcess);
    r.state = static_cast<std::uint8_t>(diagctx::InstanceState::Active);
    r.image = image;
    std::memcpy(r.text[0], pluginName.data(), pluginName.size());
    r.length[0] = static_cast<std::uint8_t>(pluginName.size());
    std::memcpy(r.text[1], "Acme", 4);
    r.length[1] = 4;
    return p;
}
const ReportSummary* find(const ReportListing& listing, unsigned id) {
    for (const auto& r : listing.reports)
        if (r.incident == name(id)) return &r;
    return nullptr;
}
}  // namespace
TEST_CASE("an application with no diagnostics folder yet has an empty listing") {
    const auto missing = fs::temp_directory_path() / *incidentId() / "none" / "diagnostics";
    auto listing = listReports(missing);
    REQUIRE(listing);
    CHECK(listing->reports.empty());
    CHECK(listing->unreported.empty());
    CHECK(listing->liveRuns == 0);
}
TEST_CASE("a listing summarises each report, newest first, and reads one back unchanged") {
    Root root;
    report(root, 1);
    report(root, 2);
    report(root, 3);
    age(root, 1, 3);
    age(root, 3, 2);
    age(root, 2, 1);
    auto listing = listReports(root.path);
    REQUIRE(listing);
    REQUIRE(listing->reports.size() == 3);
    CHECK(listing->reports[0].incident == name(2));
    CHECK(listing->reports[1].incident == name(3));
    CHECK(listing->reports[2].incident == name(1));
    const auto& first = listing->reports[0];
    CHECK(first.kind == ReportKind::UnexpectedExit);
    CHECK(first.role == "fixture");
    CHECK_FALSE(first.acknowledged);
    CHECK(first.attribution == "unknown");
    CHECK_FALSE(first.culprit);
    CHECK(first.pluginCount == 0);
    CHECK_FALSE(first.version.empty());
    CHECK(first.path == file(root, 2, ".json"));
    auto text = readReport(name(2), root.path);
    REQUIRE(text);
    CHECK(*text == *readTextFile(file(root, 2, ".json")));
    CHECK_FALSE(readReport(name(9), root.path));
    CHECK_FALSE(readReport("../" + name(2), root.path));
    CHECK_FALSE(readReport("not an incident", root.path));
}
TEST_CASE("a crash report names its plug-in, its vendor and its binary") {
    Root root;
    Inventory inventory;
    REQUIRE(inventory.apply(plugin(4, 0x5100, "Canary")));
    const auto trace = faultIn("0123456789abcdef");
    report(root, 1, &inventory, &trace);
    auto listing = listReports(root.path);
    REQUIRE(listing);
    REQUIRE(listing->reports.size() == 1);
    const auto& r = listing->reports[0];
    CHECK(r.attribution == "fault_in_image");
    REQUIRE(r.culprit);
    CHECK(r.culprit->name == "Canary");
    CHECK(r.culprit->vendor == "Acme");
    CHECK(r.culprit->buildId == "0123456789abcdef");
    CHECK(r.pluginCount == 1);
}
TEST_CASE("a report of a newer build is listed, never hidden or refused") {
    Root root;
    report(root, 1);
    report(root, 2);
    report(root, 3);
    // A kind and a top-level field this build does not know.
    rewrite(root, 1, [](json::Value& v) {
        v["kind"] = "bridge_exit";
        v["bridges"] = json::Value::array();
    });
    // An attribution level it does not know.
    rewrite(root, 2, [](json::Value& v) {
        v["attribution"]["confidence"] = "bridge_exit";
        v["attribution"]["plugin"] = json::Value();
    });
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->ignored == 0);
    REQUIRE(listing->reports.size() == 3);
    CHECK(find(*listing, 1)->kind == ReportKind::Other);
    CHECK(find(*listing, 2)->attribution == "other");
    CHECK(find(*listing, 3)->kind == ReportKind::UnexpectedExit);
    CHECK(readReport(name(1), root.path));
}
TEST_CASE("a damaged report is counted and left alone; its known parts must hold") {
    Root root;
    report(root, 1);
    report(root, 2);
    report(root, 3);
    report(root, 4);
    rewrite(root, 1, [](json::Value& v) { v["role"] = "toaster"; });            // a closed set
    rewrite(root, 2, [](json::Value& v) { v["threads"] = json::Value(7LL); });  // a nested shape
    rewrite(root, 3, [](json::Value& v) { v["incident"] = name(9); });          // not its name
    REQUIRE(privatePublish(root.path / "00000000000000000000000000000099.json", "{not json"));
    REQUIRE(privatePublish(root.path / "notes.json", "{}"));  // not a report's name: not counted
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->reports.size() == 1);
    CHECK(listing->ignored == 4);
    CHECK_FALSE(readReport(name(1), root.path));
    CHECK_FALSE(acknowledge(name(1), root.path));
    CHECK(fs::exists(file(root, 1, ".json")));
}
TEST_CASE("a report counts only once its run's marker is released") {
    Root root;
    auto handle = createMarker(root.path, boot(1));
    REQUIRE(handle);
    report(root, 1);  // published by the supervisor while the run's marker is still held
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->reports.empty());
    CHECK(listing->pending == 1);
    CHECK(listing->liveRuns == 1);
    CHECK(listing->unreported.empty());
    CHECK_FALSE(readReport(name(1), root.path));
    CHECK_FALSE(acknowledge(name(1), root.path));
    releaseMarker(*handle);  // the holder lets go, the marker is still there
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->reports.size() == 1);
    CHECK(listing->pending == 0);
    CHECK(listing->liveRuns == 0);
    CHECK(listing->unreported.empty());  // it has a report
    CHECK(readReport(name(1), root.path));
    // Or the marker is gone.
    report(root, 2);
    removeMarker(root.path, boot(1));
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->reports.size() == 2);
}
TEST_CASE("a marker with no holder and no report is an unreported run; a held one is live") {
    Root root;
    staleMarker(root, 1);
    auto live = createMarker(root.path, boot(2));
    REQUIRE(live);
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->liveRuns == 1);
    REQUIRE(listing->unreported.size() == 1);
    CHECK(listing->unreported[0].incident == name(1));
    CHECK(listing->unreported[0].role == "fixture");
    CHECK_FALSE(listing->unreported[0].acknowledged);
    CHECK(listing->reports.empty());
    releaseMarker(*live);
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->liveRuns == 0);
    CHECK(listing->unreported.size() == 2);
}
TEST_CASE("a marker still being created is live, however its lock looks") {
    Root root;
    // The creator takes its lock before it writes: an empty marker is one being created.
    {
        std::ofstream(file(root, 1, ".run"));
    }
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->liveRuns == 1);
    CHECK(listing->unreported.empty());
}
TEST_CASE("acknowledging records the sighting and changes nothing else") {
    Root root;
    report(root, 1);
    report(root, 2);
    staleMarker(root, 3);
    const auto before = *readTextFile(file(root, 1, ".json"));
    REQUIRE(acknowledge(name(1), root.path));
    REQUIRE(acknowledge(name(1), root.path));  // idempotent
    REQUIRE(acknowledge(name(3), root.path));
    CHECK_FALSE(acknowledge(name(9), root.path));  // no such incident, no orphan file
    CHECK_FALSE(fs::exists(file(root, 9, ".ack")));
    CHECK(*readTextFile(file(root, 1, ".json")) == before);
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(find(*listing, 1)->acknowledged);
    CHECK_FALSE(find(*listing, 2)->acknowledged);
    REQUIRE(listing->unreported.size() == 1);
    CHECK(listing->unreported[0].acknowledged);
    // An acknowledgement that is not the one this build writes does not count.
    REQUIRE(privatePublish(file(root, 2, ".ack"), "{\"schema\":\"supple.crash-ack/1.0\"}\n"));
    REQUIRE(privatePublish(file(root, 4, ".ack"), "stray"));
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK_FALSE(find(*listing, 2)->acknowledged);
}
TEST_CASE("retention removes a report's acknowledgement with it") {
    Root root;
    report(root, 0);
    REQUIRE(acknowledge(name(0), root.path));
    age(root, 0, 48);
    for (unsigned i = 1; i < 22; ++i) report(root, i);
    CHECK_FALSE(fs::exists(file(root, 0, ".json")));
    CHECK_FALSE(fs::exists(file(root, 0, ".ack")));
}
TEST_CASE("an acknowledgement of a report that stays is kept by retention") {
    Root root;
    report(root, 1);
    REQUIRE(acknowledge(name(1), root.path));
    for (unsigned i = 2; i < 6; ++i) report(root, i);
    CHECK(fs::exists(file(root, 1, ".ack")));
}
TEST_CASE("a folder that is not the user's own is refused") {
#if !defined(_WIN32)
    Root root;
    fs::path link = root.path.parent_path() / "linked";
    fs::create_directory_symlink(root.path, link);
    CHECK_FALSE(listReports(link));
    CHECK_FALSE(readReport(name(1), link));
    CHECK_FALSE(acknowledge(name(1), link));
#endif
}
TEST_CASE("deleting an incident removes its report, acknowledgement and idle marker only") {
    Root root;
    report(root, 1);
    report(root, 2);
    staleMarker(root, 1);  // a run that left both
    staleMarker(root, 3);
    auto live = createMarker(root.path, boot(4));
    REQUIRE(live);
    REQUIRE(acknowledge(name(1), root.path));
    REQUIRE(deleteReport(name(1), root.path));
    CHECK_FALSE(fs::exists(file(root, 1, ".json")));
    CHECK_FALSE(fs::exists(file(root, 1, ".ack")));
    CHECK_FALSE(fs::exists(file(root, 1, ".run")));
    CHECK(fs::exists(file(root, 2, ".json")));
    CHECK_FALSE(deleteReport(name(1), root.path));  // gone: no such incident now
    REQUIRE(deleteReport(name(3), root.path));      // an unreported run
    CHECK_FALSE(fs::exists(file(root, 3, ".run")));
    CHECK_FALSE(deleteReport(name(4), root.path));  // live
    CHECK(fs::exists(file(root, 4, ".run")));
    CHECK_FALSE(deleteReport(name(9), root.path));
    CHECK_FALSE(deleteReport("../" + name(2), root.path));
    CHECK(fs::exists(file(root, 2, ".json")));
    releaseMarker(*live);
}
TEST_CASE("deleting all leaves live runs, reports in the making and files it cannot read") {
    Root root;
    report(root, 1);
    report(root, 2);
    REQUIRE(acknowledge(name(2), root.path));
    staleMarker(root, 3);
    auto live = createMarker(root.path, boot(4));
    REQUIRE(live);
    auto making = createMarker(root.path, boot(5));
    REQUIRE(making);
    report(root, 5);
    rewrite(root, 1, [](json::Value& v) { v["role"] = "toaster"; });  // damaged: not ours to delete
    report(root, 6);
    auto removed = deleteAllReports(root.path);
    REQUIRE(removed);
    CHECK(*removed == 3);  // 2, 3 and 6
    CHECK(fs::exists(file(root, 1, ".json")));
    CHECK_FALSE(fs::exists(file(root, 2, ".json")));
    CHECK_FALSE(fs::exists(file(root, 2, ".ack")));
    CHECK_FALSE(fs::exists(file(root, 3, ".run")));
    CHECK(fs::exists(file(root, 4, ".run")));
    CHECK(fs::exists(file(root, 5, ".json")));
    CHECK_FALSE(fs::exists(file(root, 6, ".json")));
    releaseMarker(*live);
    releaseMarker(*making);
}
