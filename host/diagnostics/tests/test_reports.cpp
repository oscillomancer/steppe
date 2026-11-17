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
#include "supple/product.h"
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
    trace.threads[0].role = detail::Role::Fault;
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
TEST_CASE("a report counts only once its run is over, in the order the product publishes") {
    Root root;
    // The worker makes its marker; the supervisor took its own hold before the worker started.
    auto hold = createPublishHold(root.path, boot(1));
    REQUIRE(hold);
    auto handle = createMarker(root.path, boot(1));
    REQUIRE(handle);
    releaseMarker(*handle);  // the worker dies: its marker is free, the supervisor is publishing

    SUBCASE("before the report exists, the run is live, not one that wrote nothing (F1)") {
        auto listing = listReports(root.path);
        REQUIRE(listing);
        CHECK(listing->liveRuns == 1);
        CHECK(listing->unreported.empty());
        auto notice = lastRunNotice(root.path);
        REQUIRE(notice);
        CHECK_FALSE(notice->has_value());  // nothing to say yet, certainly not "no report"
        // A Dismiss or crash.acknowledge now is refused, so the report does not arrive
        // acknowledged; and a deletion does not take the marker from under the supervisor.
        CHECK_FALSE(acknowledge(name(1), root.path));
        CHECK_FALSE(deleteReport(name(1), root.path));
        auto cleared = deleteAllReports(root.path);
        REQUIRE(cleared);
        CHECK(*cleared == 0);
        CHECK(fs::exists(file(root, 1, ".run")));
        CHECK(fs::exists(file(root, 1, ".pub")));
    }
    SUBCASE("the report is published while the hold is still there") {
        report(root, 1);
        auto listing = listReports(root.path);
        REQUIRE(listing);
        CHECK(listing->reports.empty());
        CHECK(listing->pending == 1);
        CHECK(listing->liveRuns == 1);
        CHECK(listing->unreported.empty());
        CHECK_FALSE(readReport(name(1), root.path));
        CHECK_FALSE(acknowledge(name(1), root.path));
        CHECK_FALSE(settle(name(1), root.path));
        CHECK_FALSE(deleteReport(name(1), root.path));
        auto notice = lastRunNotice(root.path);
        REQUIRE(notice);
        CHECK_FALSE(notice->has_value());
        auto found = suspects(root.path);
        REQUIRE(found);
        CHECK(found->empty());
        // The supervisor finishes: it removes the worker's marker, then its own hold.
        removeMarker(root.path, boot(1));
        removePublishHold(root.path, boot(1), *hold);
        hold = Result<std::intptr_t>(std::intptr_t{-1});
        listing = listReports(root.path);
        REQUIRE(listing);
        CHECK(listing->reports.size() == 1);
        CHECK(listing->pending == 0);
        CHECK(listing->liveRuns == 0);
        CHECK(listing->unreported.empty());
        CHECK(readReport(name(1), root.path));
        CHECK_FALSE(fs::exists(file(root, 1, ".pub")));
    }
    SUBCASE("a supervisor that died holds nothing: the run left no report") {
        releaseMarker(*hold);  // dead, with the files still there
        auto listing = listReports(root.path);
        REQUIRE(listing);
        CHECK(listing->liveRuns == 0);
        REQUIRE(listing->unreported.size() == 1);
        CHECK(listing->unreported[0].incident == name(1));
        CHECK(deleteReport(name(1), root.path));
        CHECK_FALSE(fs::exists(file(root, 1, ".run")));
        CHECK_FALSE(fs::exists(file(root, 1, ".pub")));  // nothing left of it
        hold = Result<std::intptr_t>(std::intptr_t{-1});
    }
    SUBCASE("a hold with no marker, its supervisor gone, is cleaned up at the next start") {
        releaseMarker(*hold);
        hold = Result<std::intptr_t>(std::intptr_t{-1});
        REQUIRE(privateRemove(file(root, 1, ".run")));
        REQUIRE(fs::exists(file(root, 1, ".pub")));
        auto next = createMarker(root.path, boot(2));  // a later run starts
        REQUIRE(next);
        CHECK_FALSE(fs::exists(file(root, 1, ".pub")));
        releaseMarker(*next);
    }
    if (hold && *hold >= 0) removePublishHold(root.path, boot(1), *hold);
}
TEST_CASE("a report counts once its run's marker is gone, or released when no one else holds") {
    Root root;
    auto handle = createMarker(root.path, boot(1));
    REQUIRE(handle);
    report(root, 1);  // the marker is still held (no supervisor hold: one that could not be made)
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->reports.empty());
    CHECK(listing->pending == 1);
    releaseMarker(*handle);  // the holder lets go, the marker is still there
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->reports.size() == 1);
    CHECK(listing->pending == 0);
    CHECK(listing->unreported.empty());  // it has a report
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
TEST_CASE("unreported runs are listed newest first by the time read once, ties by incident") {
    Root root;
    for (unsigned id : {1, 2, 3, 4}) staleMarker(root, id);
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(file(root, 1, ".run"), now - std::chrono::hours(3));
    fs::last_write_time(file(root, 2, ".run"), now - std::chrono::hours(1));
    fs::last_write_time(file(root, 3, ".run"), now - std::chrono::hours(2));
    fs::last_write_time(file(root, 4, ".run"), now - std::chrono::hours(2));
    auto listing = listReports(root.path);
    REQUIRE(listing);
    REQUIRE(listing->unreported.size() == 4);
    std::vector<std::string> order;
    for (const auto& run : listing->unreported) order.push_back(run.incident);
    CHECK(order == std::vector<std::string>{name(2), name(4), name(3), name(1)});
    for (std::size_t i = 1; i < listing->unreported.size(); ++i)
        CHECK(listing->unreported[i - 1].started >= listing->unreported[i].started);
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
TEST_CASE("an empty marker that stays empty is a run that ended, not one being created") {
    // A marker is empty for the instant between its creation and its first write. One left empty
    // (the creator failed in between) must not read as held for ever: it could never be listed,
    // deleted or pruned.
    Root root;
    {
        std::ofstream(file(root, 1, ".run"));
    }
    fs::permissions(file(root, 1, ".run"), fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);  // as createMarker makes it
    fs::last_write_time(file(root, 1, ".run"),
                        fs::file_time_type::clock::now() - std::chrono::minutes(5));
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->liveRuns == 0);
    REQUIRE(listing->unreported.size() == 1);
    CHECK(listing->unreported[0].incident == name(1));
    CHECK(deleteReport(name(1), root.path));
    CHECK_FALSE(fs::exists(file(root, 1, ".run")));
    // Held for real, however old: a creator that stalled still holds its lock.
    auto live = createMarker(root.path, boot(2));
    REQUIRE(live);
    fs::last_write_time(file(root, 2, ".run"),
                        fs::file_time_type::clock::now() - std::chrono::minutes(5));
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->liveRuns == 1);
    releaseMarker(*live);
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
    // Acknowledging replaces such a file (the only way it is replaced on Windows, where
    // publishing never overwrites): the incident is acknowledged afterwards.
    REQUIRE(acknowledge(name(2), root.path));
    listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(find(*listing, 2)->acknowledged);
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
TEST_CASE("deleting a report file this build cannot read removes nothing, whatever its marker") {
    // A newer build's report this one cannot validate, whose run ended and left its marker (a
    // crash during teardown leaves it): not this build's to remove (P11, review 38b F3).
    Root root;
    staleMarker(root, 7);
    REQUIRE(privatePublish(file(root, 7, ".json"), "{\"not\":\"a report\"}"));
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(listing->ignored == 1);
    CHECK(listing->unreported.empty());
    CHECK_FALSE(deleteReport(name(7), root.path));
    CHECK(fs::exists(file(root, 7, ".json")));
    CHECK(fs::exists(file(root, 7, ".run")));
    auto cleared = deleteAllReports(root.path);
    REQUIRE(cleared);
    CHECK(*cleared == 0);
    CHECK(fs::exists(file(root, 7, ".json")));
    // The dismissal of an unreported run, with no report file at all, still works.
    staleMarker(root, 8);
    CHECK(deleteReport(name(8), root.path));
    CHECK_FALSE(fs::exists(file(root, 8, ".run")));
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
namespace {
std::optional<RestartNotice> noticeFor(const Root& root) {
    auto notice = lastRunNotice(root.path);
    REQUIRE(notice);
    return *notice;
}
}  // namespace
TEST_CASE("a restart says nothing when there is nothing unacknowledged") {
    Root root;
    CHECK_FALSE(noticeFor(root));
    report(root, 1);
    REQUIRE(acknowledge(name(1), root.path));
    staleMarker(root, 2);
    REQUIRE(acknowledge(name(2), root.path));
    auto live = createMarker(root.path, boot(3));  // this run: not an ended one
    REQUIRE(live);
    CHECK_FALSE(noticeFor(root));
    releaseMarker(*live);
}
TEST_CASE("a restart notice names the report, the plug-in it points at, or the missing report") {
    Root root;
    report(root, 1);
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->kind == RestartNotice::Kind::Report);
    CHECK(notice->incidents == std::vector<std::string>{name(1)});
    CHECK(notice->earlier == 0);
    CHECK(notice->plugin.empty());
    CHECK(notice->headline.find("closed unexpectedly") != std::string::npos);
    CHECK(notice->headline.find("last time") != std::string::npos);
    CHECK(notice->detail.find("not sent anywhere") != std::string::npos);
    REQUIRE(acknowledge(name(1), root.path));
    // A crash inside a plug-in's binary names it.
    REQUIRE(writeSampleCrash(name(2), "Canary", "Acme", root.path));
    notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->plugin == "Canary");
    CHECK(notice->headline.find("Canary by Acme was running when") == 0);
    CHECK(notice->detail.find(
              "The report points at that plug-in but cannot prove it was the cause.") !=
          std::string::npos);
    CHECK(noticeText(*notice).find(notice->headline) == 0);
    REQUIRE(acknowledge(name(2), root.path));
    // A run that left no report says so.
    staleMarker(root, 3);
    notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->kind == RestartNotice::Kind::NoReport);
    CHECK(notice->headline.find("no crash report was written") != std::string::npos);
    CHECK(notice->incidents == std::vector<std::string>{name(3)});
}
TEST_CASE("the product's own name is in the notice, whatever it is called") {
    Root root;
    report(root, 1);
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->headline.starts_with(std::string(supple::product::kName)));
}
TEST_CASE("the newest incident leads, the earlier ones are counted, and dismissing covers all") {
    Root root;
    report(root, 1);
    report(root, 2);
    staleMarker(root, 3);
    age(root, 1, 5);
    age(root, 2, 3);
    fs::last_write_time(file(root, 3, ".run"),
                        fs::file_time_type::clock::now() - std::chrono::hours(1));
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->kind == RestartNotice::Kind::NoReport);  // the run without a report is newest
    CHECK(notice->incidents == std::vector<std::string>{name(3), name(2), name(1)});
    CHECK(notice->earlier == 2);
    CHECK(notice->detail.find("Steppe also closed unexpectedly 2 times before that.") !=
          std::string::npos);
    REQUIRE(acknowledgeNotice(*notice, root.path));
    CHECK_FALSE(noticeFor(root));
    // Nothing was deleted or changed by it.
    CHECK(fs::exists(file(root, 1, ".json")));
    CHECK(fs::exists(file(root, 3, ".run")));
    // One earlier reads in the singular.
    report(root, 4);
    report(root, 5);
    age(root, 4, 2);
    notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->earlier == 1);
    CHECK(notice->detail.find("Steppe also closed unexpectedly once before that.") !=
          std::string::npos);
}
TEST_CASE("an unexpected report a newer build wrote still counts for the notice") {
    Root root;
    report(root, 1);
    rewrite(root, 1, [](json::Value& v) { v["kind"] = "bridge_exit"; });
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->kind == RestartNotice::Kind::Report);
}

TEST_CASE("the notice reads the newest three reports and counts the rest by name") {
    Root root;
    for (unsigned id = 1; id <= 6; ++id) {
        report(root, id);
        age(root, id, static_cast<int>(10 - id));  // 6 is the newest
    }
    // The three older ones are not valid reports: if the notice parsed them it would drop them.
    for (unsigned id = 1; id <= 3; ++id) {
        REQUIRE(privateRemove(file(root, id, ".json")));
        REQUIRE(privatePublish(file(root, id, ".json"), "not json"));
        age(root, id, static_cast<int>(10 - id));
    }
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->incidents ==
          std::vector<std::string>{name(6), name(5), name(4), name(3), name(2), name(1)});
    CHECK(notice->earlier == 5);
    // Dismissing covers the ones it did not read, without reading them.
    REQUIRE(acknowledgeNotice(*notice, root.path));
    CHECK_FALSE(noticeFor(root));
    CHECK(fs::exists(file(root, 1, ".ack")));
}
TEST_CASE("an unreadable report among the newest three is not announced") {
    Root root;
    report(root, 1);
    report(root, 2);
    age(root, 1, 5);
    age(root, 2, 1);
    REQUIRE(privateRemove(file(root, 2, ".json")));
    REQUIRE(privatePublish(file(root, 2, ".json"), "not json"));
    age(root, 2, 1);
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->incidents == std::vector<std::string>{name(1)});
}
TEST_CASE("dismissing the notice for an unreported run acknowledges it for good") {
    Root root;
    staleMarker(root, 1);
    auto notice = noticeFor(root);
    REQUIRE(notice);
    CHECK(notice->kind == RestartNotice::Kind::NoReport);
    REQUIRE(acknowledgeNotice(*notice, root.path));
    CHECK_FALSE(noticeFor(root));
    // The run is announced once, not again at the next start.
    CHECK_FALSE(noticeFor(root));
    CHECK(fs::exists(file(root, 1, ".ack")));
}
namespace {
// A marker nobody holds, written without `createMarker` (which prunes as it goes).
void rawMarker(const Root& root, unsigned id) {
    REQUIRE(privatePublish(file(root, id, ".run"), "{\"schema\":\"supple.crash-run/1.0\"}\n"));
}
}  // namespace
TEST_CASE("a notice covers the incidents of its own role, and dismissing it touches no other") {
    Root root;
    auto asRole = [&](unsigned id, diagnostics::Role role) {
        auto b = boot(id);
        b.role = static_cast<unsigned>(role);
        return b;
    };
    REQUIRE(publishReport(root.path, asRole(1, diagnostics::Role::Gui), nullptr, 31, false));
    REQUIRE(publishReport(root.path, asRole(2, diagnostics::Role::Engine), nullptr, 31, false));
    {  // an engine run that left no report, and a GUI one
        auto engine = createMarker(root.path, asRole(3, diagnostics::Role::Engine));
        REQUIRE(engine);
        releaseMarker(*engine);
        auto gui = createMarker(root.path, asRole(4, diagnostics::Role::Gui));
        REQUIRE(gui);
        releaseMarker(*gui);
    }
    age(root, 1, 9);
    age(root, 2, 7);
    fs::last_write_time(file(root, 3, ".run"),
                        fs::file_time_type::clock::now() - std::chrono::hours(5));
    fs::last_write_time(file(root, 4, ".run"),
                        fs::file_time_type::clock::now() - std::chrono::hours(3));

    auto gui = lastRunNotice(root.path, diagnostics::Role::Gui);
    REQUIRE(gui);
    REQUIRE(*gui);
    CHECK((*gui)->role == "gui");
    CHECK((*gui)->incidents == std::vector<std::string>{name(4), name(1)});  // its own two
    CHECK((*gui)->headline.find(std::string(product::kName) + " closed") == 0);
    auto engine = lastRunNotice(root.path, diagnostics::Role::Engine);
    REQUIRE(engine);
    REQUIRE(*engine);
    CHECK((*engine)->role == "engine");
    CHECK((*engine)->incidents == std::vector<std::string>{name(3), name(2)});
    // The engine's text says which app closed.
    CHECK((*engine)->headline.find(std::string(product::kName) + " engine closed") == 0);
    // No role: every incident, as before.
    auto all = lastRunNotice(root.path);
    REQUIRE(all);
    REQUIRE(*all);
    CHECK((*all)->role.empty());
    CHECK((*all)->incidents.size() == 4);

    // Dismissing the banner acknowledges the GUI's incidents and none of the engine's.
    REQUIRE(acknowledgeNotice(**gui, root.path));
    auto listing = listReports(root.path);
    REQUIRE(listing);
    CHECK(find(*listing, 1)->acknowledged);
    CHECK_FALSE(find(*listing, 2)->acknowledged);
    for (const auto& run : listing->unreported)
        CHECK(run.acknowledged == (run.incident == name(4)));
    gui = lastRunNotice(root.path, diagnostics::Role::Gui);
    REQUIRE(gui);
    CHECK_FALSE(*gui);
    engine = lastRunNotice(root.path, diagnostics::Role::Engine);
    REQUIRE(engine);
    REQUIRE(*engine);
    CHECK((*engine)->incidents.size() == 2);
    // An explicit acknowledgement by name still works for any role.
    REQUIRE(acknowledge(name(2), root.path));
    engine = lastRunNotice(root.path, diagnostics::Role::Engine);
    REQUIRE(engine);
    REQUIRE(*engine);
    CHECK((*engine)->incidents == std::vector<std::string>{name(3)});
    // An incident whose role cannot be read belongs to no one: not announced to a role.
    REQUIRE(privatePublish(file(root, 5, ".json"), "{not json"));
    {
        std::ofstream(file(root, 6, ".run"));
    }
    fs::permissions(file(root, 6, ".run"), fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    fs::last_write_time(file(root, 6, ".run"),
                        fs::file_time_type::clock::now() - std::chrono::minutes(5));
    engine = lastRunNotice(root.path, diagnostics::Role::Engine);
    REQUIRE(engine);
    REQUIRE(*engine);
    CHECK((*engine)->incidents == std::vector<std::string>{name(3)});
    all = lastRunNotice(root.path);
    REQUIRE(all);
    REQUIRE(*all);
    CHECK((*all)->incidents.size() >= 2);
}
TEST_CASE("with a role, the notice reads only the newest few report files") {
    Root root;
    auto engineBoot = [&](unsigned id) {
        auto b = boot(id);
        b.role = static_cast<unsigned>(diagnostics::Role::Engine);
        return b;
    };
    const unsigned total = static_cast<unsigned>(kNoticeRoleReads) + 4;
    for (unsigned id = 1; id <= total; ++id) {
        REQUIRE(publishReport(root.path, engineBoot(id), nullptr, 31, false));
        age(root, id, static_cast<int>(100 - id));  // a higher id is newer
    }
    auto notice = lastRunNotice(root.path, diagnostics::Role::Engine);
    REQUIRE(notice);
    REQUIRE(*notice);
    // The newest kNoticeRoleReads are covered; the rest wait for a later start.
    CHECK((*notice)->incidents.size() == kNoticeRoleReads);
    REQUIRE(acknowledgeNotice(**notice, root.path));
    notice = lastRunNotice(root.path, diagnostics::Role::Engine);
    REQUIRE(notice);
    REQUIRE(*notice);
    CHECK((*notice)->incidents.size() == 4);
}
TEST_CASE("only the newest twenty unreported runs keep their markers") {
    Root root;
    // A marker beside a report and a marker held by a live run are not unreported runs. They are
    // made first: creating a marker prunes.
    report(root, 100);
    auto beside = createMarker(root.path, boot(100));
    REQUIRE(beside);
    releaseMarker(*beside);
    auto live = createMarker(root.path, boot(101));
    REQUIRE(live);
    constexpr unsigned kRuns = 24;
    for (unsigned id = 1; id <= kRuns; ++id) {
        rawMarker(root, id);
        fs::last_write_time(file(root, id, ".run"),
                            fs::file_time_type::clock::now() - std::chrono::hours(100 - id));
    }
    REQUIRE(acknowledge(name(1), root.path));
    REQUIRE(pruneMarkers(root.path));
    for (unsigned id = 1; id <= kRuns; ++id) {
        CAPTURE(id);
        CHECK(fs::exists(file(root, id, ".run")) == (id > kRuns - kKeepUnreportedRuns));
    }
    CHECK_FALSE(fs::exists(file(root, 1, ".ack")));  // goes with its marker
    CHECK(fs::exists(file(root, 100, ".run")));
    CHECK(fs::exists(file(root, 101, ".run")));
    CHECK(fs::exists(file(root, 100, ".json")));
    releaseMarker(*live);
}
TEST_CASE("starting a run prunes the runs that ended without a report") {
    Root root;
    for (unsigned id = 1; id <= kKeepUnreportedRuns + 3; ++id) {
        rawMarker(root, id);
        fs::last_write_time(file(root, id, ".run"),
                            fs::file_time_type::clock::now() - std::chrono::hours(100 - id));
    }
    auto mine = createMarker(root.path, boot(200));
    REQUIRE(mine);
    std::size_t markers = 0;
    for (const auto& entry : fs::directory_iterator(root.path))
        if (entry.path().extension() == ".run") ++markers;
    CHECK(markers == kKeepUnreportedRuns + 1);  // the newest twenty and this run's own
    releaseMarker(*mine);
}

// --- Suspects (topic 7d, P4-P8). The window is read from file times, which the tests set; no
// test waits for a clock.
namespace {
struct Plug {
    std::string name = "Canary", version = "1.2", id = "com.acme.canary";
    std::string build = "0123456789abcdef";  // empty: the report records no build for it
    std::string level = "fault_in_image";
    std::string kind = "clap", origin = "foreign";
};
// A report that points at `plug`, `hoursAgo` old (the newest has the smallest number).
void crashing(const Root& root, unsigned id, const Plug& plug, int hoursAgo) {
    Inventory inventory;
    auto p = plugin(4, 0x5100, plug.name);
    const auto text = [&](std::size_t i, const std::string& s) {
        std::memcpy(p.instance.text[i], s.data(), s.size());
        p.instance.length[i] = static_cast<std::uint8_t>(s.size());
    };
    text(2, plug.version);
    text(3, plug.id);
    p.instance.kind =
        static_cast<std::uint8_t>(plug.kind == "vst3"           ? diagctx::InstanceKind::Vst3
                                  : plug.kind == "clap_library" ? diagctx::InstanceKind::ClapLibrary
                                                                : diagctx::InstanceKind::Clap);
    REQUIRE(inventory.apply(p));
    // The attribution comes from the trace, as a real report's does: a fault in the plug-in's
    // binary, the fault thread inside the instance, another thread inside it, or nothing.
    TraceSnapshot trace = faultIn(plug.build);
    if (plug.level != "fault_in_image") {
        trace.modules[0].id.reset();  // the fault is in a module with no identity
        if (!plug.build.empty()) {    // and the plug-in's own binary is another module
            trace.modules.resize(2);
            trace.modules[1].id = plug.build;
            trace.regions = {{0x1000, 0x2000, 0}, {0x5000, 0x6000, 1}};
        }
    }
    trace.threads.resize(2);
    trace.threads[1].role = detail::Role::Main;
    if (plug.level == "active_token") {
        trace.threads[0].entry = diagctx::Entry::Process;
        trace.threads[0].entryCell = 4;
    }
    if (plug.level == "concurrent_activity") {
        trace.threads[1].entry = diagctx::Entry::Process;
        trace.threads[1].entryCell = 4;
    }
    report(root, id, &inventory, &trace);
    rewrite(root, id, [&](json::Value& v) { v["plugins"].asArray()[0]["origin"] = plug.origin; });
    age(root, id, hoursAgo);
}
std::vector<Suspect> suspectsOf(const Root& root) {
    auto found = suspects(root.path);
    REQUIRE(found);
    return *found;
}
}  // namespace
TEST_CASE("a report that points at a plug-in makes it a suspect, keyed on its build") {
    Root root;
    CHECK(suspectsOf(root).empty());
    crashing(root, 1, {}, 1);
    const auto found = suspectsOf(root);
    REQUIRE(found.size() == 1);
    CHECK(found[0].format == "clap");
    CHECK(found[0].pluginId == "com.acme.canary");
    CHECK(found[0].buildId == "0123456789abcdef");
    CHECK(found[0].name == "Canary");
    CHECK(found[0].vendor == "Acme");
    CHECK(found[0].version == "1.2");
    CHECK(found[0].origin == "foreign");
    CHECK(found[0].attribution == "fault_in_image");
    CHECK(found[0].incidents == std::vector<std::string>{name(1)});
    // The listing gives the same fields to a viewer.
    auto listing = listReports(root.path);
    REQUIRE(listing);
    REQUIRE(listing->reports[0].culprit);
    CHECK(listing->reports[0].culprit->pluginId == "com.acme.canary");
    CHECK(listing->reports[0].culprit->kind == "clap");
    CHECK(listing->reports[0].culprit->format == "clap");
    CHECK(listing->reports[0].culprit->origin == "foreign");
}
TEST_CASE("a missing folder, or one that is not the user's own, has no suspects or an error") {
    const auto none = suspects(fs::temp_directory_path() / "supple-no-such-diagnostics-folder");
    REQUIRE(none);
    CHECK(none->empty());
#if !defined(_WIN32)
    Root root;
    fs::path link = root.path.parent_path() / "linked";
    fs::create_directory_symlink(root.path, link);
    CHECK_FALSE(suspects(link));
    CHECK_FALSE(settle(name(1), link));
#endif
}
TEST_CASE(
    "reports of one build are one suspect; the build, or without one the version, tells apart") {
    Root root;
    crashing(root, 1, {}, 3);
    crashing(root, 2, {}, 2);
    Plug rebuilt;
    rebuilt.build = "aaaaaaaaaaaaaaaa";
    crashing(root, 3, rebuilt, 1);
    auto found = suspectsOf(root);
    REQUIRE(found.size() == 2);
    CHECK(found[0].buildId == "aaaaaaaaaaaaaaaa");  // the newest evidence first
    CHECK(found[0].incidents == std::vector<std::string>{name(3)});
    CHECK(found[1].buildId == "0123456789abcdef");
    CHECK(found[1].incidents == std::vector<std::string>{name(2), name(1)});
    // An active_token report records no build; two versions are two suspects, one version one.
    Root second;
    Plug token;
    token.build = "";
    token.level = "active_token";
    crashing(second, 1, token, 3);
    crashing(second, 2, token, 2);
    token.version = "1.3";
    crashing(second, 3, token, 1);
    found = suspectsOf(second);
    REQUIRE(found.size() == 2);
    CHECK(found[0].buildId.empty());
    CHECK(found[0].version == "1.3");
    CHECK(found[1].version == "1.2");
    CHECK(found[1].incidents.size() == 2);
    CHECK(found[1].attribution == "active_token");
    // The strongest level among a key's reports is the one reported.
    Root mixed;
    Plug seen;
    seen.level = "active_token";  // the same build, found by a token rather than by the fault
    crashing(mixed, 1, {}, 2);
    crashing(mixed, 2, seen, 1);
    found = suspectsOf(mixed);
    REQUIRE(found.size() == 1);
    CHECK(found[0].incidents.size() == 2);
    CHECK(found[0].attribution == "fault_in_image");  // in either order
    crashing(mixed, 3, seen, 0);
    REQUIRE(deleteReport(name(1), mixed.path));
    found = suspectsOf(mixed);
    REQUIRE(found.size() == 1);
    CHECK(found[0].attribution == "active_token");
}
TEST_CASE("only fault_in_image and active_token naming a matchable plug-in yield a suspect") {
    Root root;
    for (const char* level : {"concurrent_activity", "unknown"}) {
        Plug p;
        p.level = level;
        crashing(root, 1, p, 3);
        CAPTURE(level);
        CHECK(suspectsOf(root).empty());
        REQUIRE(deleteReport(name(1), root.path));
    }
    Plug noId;
    noId.id = "";  // the plug-in sent no id of its own: nothing to match a project to
    crashing(root, 1, noId, 3);
    CHECK(suspectsOf(root).empty());
    REQUIRE(deleteReport(name(1), root.path));
    report(root, 1);  // an unexpected exit with no attribution
    CHECK(suspectsOf(root).empty());
    REQUIRE(deleteReport(name(1), root.path));
    // A VST3 and a library entry map to their format; origin is carried for P8.
    Plug vst;
    vst.kind = "vst3";
    vst.level = "active_token";
    vst.build = "";
    vst.origin = "stock";
    crashing(root, 1, vst, 3);
    Plug lib;
    lib.kind = "clap_library";
    lib.id = "com.acme.lib";
    crashing(root, 2, lib, 2);
    const auto found = suspectsOf(root);
    REQUIRE(found.size() == 2);
    CHECK(found[0].format == "clap");
    CHECK(found[1].format == "vst3");
    CHECK(found[1].origin == "stock");
}
TEST_CASE("the window is the newest three valid reports, acknowledged or not, and it slides") {
    Root root;
    Plug other;
    other.id = "com.acme.other";
    other.build = "bbbbbbbbbbbbbbbb";
    crashing(root, 1, {}, 40);  // the oldest: the suspect
    for (unsigned id = 2; id <= 3; ++id) crashing(root, id, other, 40 - static_cast<int>(id));
    auto found = suspectsOf(root);
    REQUIRE(found.size() == 2);
    // A fourth report pushes the oldest out of the window: it is derived, no file changes.
    crashing(root, 4, other, 1);
    found = suspectsOf(root);
    REQUIRE(found.size() == 1);
    CHECK(found[0].pluginId == "com.acme.other");
    CHECK(fs::exists(file(root, 1, ".json")));
    // Reports that name nobody still count toward the three (E4).
    Root quiet;
    crashing(quiet, 1, {}, 40);
    for (unsigned id = 2; id <= 4; ++id) {
        report(quiet, id);
        age(quiet, id, 40 - static_cast<int>(id));
    }
    CHECK(suspectsOf(quiet).empty());
    // Acknowledging, one report or all, changes nothing (E15).
    Root seen;
    crashing(seen, 1, {}, 2);
    crashing(seen, 2, {}, 1);
    const auto before = suspectsOf(seen);
    REQUIRE(acknowledge(name(1), seen.path));
    REQUIRE(acknowledge(name(2), seen.path));
    const auto after = suspectsOf(seen);
    REQUIRE(after.size() == 1);
    CHECK(after[0].incidents == before[0].incidents);
    CHECK(after[0].incidents.size() == 2);
}
TEST_CASE("a report that is damaged or still being written is not part of the window") {
    Root root;
    crashing(root, 1, {}, 5);
    crashing(root, 2, {}, 4);
    crashing(root, 3, {}, 3);
    // A damaged newer file is skipped, not counted as one of the three.
    report(root, 4);
    rewrite(root, 4, [](json::Value& v) { v["role"] = "toaster"; });
    age(root, 4, 1);
    auto found = suspectsOf(root);
    REQUIRE(found.size() == 1);
    CHECK(found[0].incidents.size() == 3);
    // A newer report whose run is still live is not counted either (T8).
    auto handle = createMarker(root.path, boot(5));
    REQUIRE(handle);
    Plug other;
    other.id = "com.acme.other";
    crashing(root, 5, other, 0);
    found = suspectsOf(root);
    REQUIRE(found.size() == 1);
    CHECK(found[0].pluginId == "com.acme.canary");
    CHECK_FALSE(settle(name(5), root.path));
    CHECK_FALSE(fs::exists(file(root, 5, ".settled")));
    releaseMarker(*handle);  // the run is over: the report counts
    CHECK(suspectsOf(root).size() == 2);
}
TEST_CASE("only a limited number of files is opened looking for the newest three") {
    Root root;
    crashing(root, 1, {}, 100);
    for (unsigned id = 2; id < 2 + kSuspectAttempts; ++id) {
        report(root, id);
        rewrite(root, id, [](json::Value& v) { v["role"] = "toaster"; });
        age(root, id, 90 - static_cast<int>(id));
    }
    CHECK(suspectsOf(root).empty());  // the good, old one is behind the attempts that were made
}
TEST_CASE("settling resolves a suspicion without touching the report or the acknowledgement") {
    Root root;
    crashing(root, 1, {}, 3);
    crashing(root, 2, {}, 2);
    Plug other;
    other.id = "com.acme.other";
    crashing(root, 3, other, 1);
    const auto reportText = *readTextFile(file(root, 1, ".json"));
    auto found = suspectsOf(root);
    REQUIRE(found.size() == 2);
    const Suspect canary = found[1];
    REQUIRE(canary.incidents.size() == 2);
    REQUIRE(acknowledge(name(1), root.path));
    REQUIRE(settle(canary, root.path));
    REQUIRE(settle(canary, root.path));  // idempotent
    CHECK(fs::exists(file(root, 1, ".settled")));
    CHECK(fs::exists(file(root, 2, ".settled")));
    CHECK_FALSE(fs::exists(file(root, 3, ".settled")));
    CHECK(*readTextFile(file(root, 1, ".json")) == reportText);
    CHECK(fs::exists(file(root, 1, ".ack")));
    CHECK_FALSE(fs::exists(file(root, 2, ".ack")));
    found = suspectsOf(root);
    REQUIRE(found.size() == 1);
    CHECK(found[0].pluginId == "com.acme.other");
    // The marker says what it is.
    auto marker = json::parse(*readTextFile(file(root, 1, ".settled")));
    REQUIRE(marker);
    CHECK((*marker)["schema"].asString() == "supple.crash-settled/1.0");
    CHECK((*marker)["incident"].asString() == name(1));
    // A settled report keeps its place in the window: an older one does not slide back in.
    Root crowded;
    crashing(crowded, 1, {}, 40);
    crashing(crowded, 2, other, 3);
    crashing(crowded, 3, other, 2);
    crashing(crowded, 4, other, 1);
    CHECK(suspectsOf(crowded).size() == 1);
    REQUIRE(settle(name(4), crowded.path));
    CHECK(suspectsOf(crowded).size() == 1);  // the two others still yield; incident 1 is not back
    CHECK(suspectsOf(crowded)[0].incidents.size() == 2);
    // A settlement that is not the one this build writes does not count.
    Root forged;
    crashing(forged, 1, {}, 1);
    REQUIRE(
        privatePublish(file(forged, 1, ".settled"), "{\"schema\":\"supple.crash-settled/1.0\"}\n"));
    CHECK(suspectsOf(forged).size() == 1);
    REQUIRE(settle(name(1), forged.path));  // replaces it
    CHECK(suspectsOf(forged).empty());
}
TEST_CASE("only a valid finished report can be settled") {
    Root root;
    staleMarker(root, 3);  // a run that ended with no report
    report(root, 2);
    rewrite(root, 2, [](json::Value& v) { v["role"] = "toaster"; });
    CHECK_FALSE(settle(name(9), root.path));  // nothing there
    CHECK_FALSE(settle(name(3), root.path));  // a run with no report has nothing to settle
    CHECK_FALSE(settle(name(2), root.path));  // not a valid report
    CHECK_FALSE(settle("../" + name(2), root.path));
    CHECK_FALSE(settle("not an incident", root.path));
    for (unsigned id : {9u, 3u, 2u}) CHECK_FALSE(fs::exists(file(root, id, ".settled")));
}
TEST_CASE("deleting a report takes its settlement with it, and the suspicion with the report") {
    Root root;
    crashing(root, 1, {}, 2);
    crashing(root, 2, {}, 1);
    REQUIRE(settle(name(1), root.path));
    REQUIRE(deleteReport(name(1), root.path));
    CHECK_FALSE(fs::exists(file(root, 1, ".settled")));
    CHECK(suspectsOf(root).size() == 1);
    REQUIRE(deleteReport(name(2), root.path));
    CHECK(suspectsOf(root).empty());
    crashing(root, 3, {}, 1);
    REQUIRE(settle(name(3), root.path));
    auto removed = deleteAllReports(root.path);
    REQUIRE(removed);
    CHECK(*removed == 1);
    CHECK_FALSE(fs::exists(file(root, 3, ".settled")));
}
TEST_CASE("retention removes a settlement with its report and cleans up strays") {
    Root root;
    report(root, 0);
    REQUIRE(settle(name(0), root.path));
    age(root, 0, 48);
    for (unsigned i = 1; i < 22; ++i) report(root, i);
    CHECK_FALSE(fs::exists(file(root, 0, ".json")));
    CHECK_FALSE(fs::exists(file(root, 0, ".settled")));
    // A settlement or acknowledgement whose report and run are both gone (a write that raced a
    // deletion) is removed at the next prune; one beside a report or a run marker stays.
    Root strays;
    report(strays, 1);
    REQUIRE(settle(name(1), strays.path));
    REQUIRE(acknowledge(name(1), strays.path));
    staleMarker(strays, 2);
    REQUIRE(acknowledge(name(2), strays.path));
    REQUIRE(privatePublish(file(strays, 7, ".settled"), "stray"));
    REQUIRE(privatePublish(file(strays, 8, ".ack"), "stray"));
    REQUIRE(privatePublish(strays.path / "notes.settled", "kept"));  // not an incident's name
    REQUIRE(pruneMarkers(strays.path));
    CHECK_FALSE(fs::exists(file(strays, 7, ".settled")));
    CHECK_FALSE(fs::exists(file(strays, 8, ".ack")));
    CHECK(fs::exists(file(strays, 1, ".settled")));
    CHECK(fs::exists(file(strays, 1, ".ack")));
    CHECK(fs::exists(file(strays, 2, ".ack")));
    CHECK(fs::exists(strays.path / "notes.settled"));
}
TEST_CASE("a pruned unreported run takes its settlement and acknowledgement") {
    Root root;
    for (unsigned id = 1; id <= kKeepUnreportedRuns + 2; ++id) {
        REQUIRE(privatePublish(file(root, id, ".run"), "{\"schema\":\"supple.crash-run/1.0\"}\n"));
        fs::last_write_time(file(root, id, ".run"),
                            fs::file_time_type::clock::now() - std::chrono::hours(100 - id));
    }
    REQUIRE(privatePublish(file(root, 1, ".settled"), "stray"));
    REQUIRE(acknowledge(name(1), root.path));
    REQUIRE(pruneMarkers(root.path));
    CHECK_FALSE(fs::exists(file(root, 1, ".run")));
    CHECK_FALSE(fs::exists(file(root, 1, ".ack")));
    CHECK_FALSE(fs::exists(file(root, 1, ".settled")));
}
