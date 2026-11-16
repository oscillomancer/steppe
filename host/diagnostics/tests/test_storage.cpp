// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include "../src/private_files.h"
#include "../src/storage.h"
#include "supple/base/files.h"
using namespace supple;
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
    bad.version = 2;
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
