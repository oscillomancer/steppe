// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/crash-resolver/resolve.h>
#include <supple/json/parse.h>
#include <supple/json/write.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "images.h"

using namespace supple::diagnostics::resolver;
namespace fs = std::filesystem;

namespace {

struct Dir {
    fs::path path;
    explicit Dir(const char* name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
void write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}
void write(const fs::path& path, const std::string& text) {
    write(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::vector<std::uint8_t> elfWith(std::vector<std::uint8_t> id) {
    images::ElfSpec spec;
    spec.buildId = std::move(id);
    spec.functions = {{"plain_main", 0x1100, 0x40}, {"plain_bar", 0x1200, 0x20}};
    return images::elf(spec);
}

// One module, a fault thread with frames in and out of it, and a thread nobody could read.
std::string reportText(const std::string& format, const std::string& id) {
    return R"({"schema":"supple.crash-report/1.0","incident":"00000000000000000000000000000001",)"
           R"("role":"fixture","platform":"linux","architecture":"x86_64","kind":"crash",)"
           R"("build":{"version":"1","commit":"abc","dirty":false},)"
           R"("modules":[{"format":")" +
           format + R"(","id":)" + (id.empty() ? "null" : "\"" + id + "\"") +
           R"(}],"threads":[)"
           R"({"ordinal":0,"role":"fault","registered":"audio_live","worker":2,)"
           R"("state":"walked","stop":"end","frames":[)"
           R"({"module":0,"offset":"0x1110","return":false},)"
           R"({"module":0,"offset":"0x1201","return":true},)"
           R"({"module":null,"return":true},)"
           R"({"module":0,"offset":"0x9000","return":true}]},)"
           R"({"ordinal":1,"role":"main","state":"unavailable","reason":"ptrace_denied","frames":[]}]})";
}
const std::string kId = "deadbeef01020304";

}  // namespace

TEST_CASE("a report parses to the modules, threads and frames the resolver uses") {
    const auto report = parseReport(reportText("elf", kId));
    REQUIRE(report);
    CHECK(report->incident == "00000000000000000000000000000001");
    CHECK(report->kind == "crash");
    REQUIRE(report->modules.size() == 1);
    CHECK(report->modules[0].format == Format::Elf);
    CHECK(report->modules[0].id == kId);
    REQUIRE(report->threads.size() == 2);
    CHECK(report->threads[0].why == "end");
    CHECK(report->threads[1].why == "ptrace_denied");
    const auto& frames = report->threads[0].frames;
    REQUIRE(frames.size() == 4);
    CHECK(frames[0].offset == 0x1110);
    CHECK_FALSE(frames[0].isReturn);
    CHECK(frames[2].module == -1);
    const auto anonymous = parseReport(reportText("pe", ""));
    REQUIRE(anonymous);
    CHECK(anonymous->modules[0].id.empty());
}

TEST_CASE("a report with anything the resolver would have to guess at is refused with a pointer") {
    const std::string good = reportText("elf", kId);
    struct Case {
        const char* from;
        const char* to;
        const char* path;
    };
    const Case cases[] = {
        {"supple.crash-report/1.0", "other/1.0", "/schema"},
        {"\"role\":\"fixture\"", "\"role\":1", "/role"},
        {"\"format\":\"elf\"", "\"format\":\"coff\"", "/modules/0/format"},
        {"\"id\":\"deadbeef01020304\"", "\"id\":\"DEADBEEF01020304\"", "/modules/0/id"},
        {"\"id\":\"deadbeef01020304\"", "\"id\":\"abc\"", "/modules/0/id"},
        {"\"id\":\"deadbeef01020304\"", "\"id\":7", "/modules/0/id"},
        {"\"module\":0,\"offset\":\"0x1110\"", "\"module\":3,\"offset\":\"0x1110\"",
         "/threads/0/frames/0/module"},
        {"\"module\":0,\"offset\":\"0x1110\"", "\"module\":-1,\"offset\":\"0x1110\"",
         "/threads/0/frames/0/module"},
        {"\"offset\":\"0x1110\"", "\"offset\":\"1110\"", "/threads/0/frames/0/offset"},
        {"\"offset\":\"0x1110\"", "\"offset\":\"0x\"", "/threads/0/frames/0/offset"},
        {"\"offset\":\"0x1110\"", "\"offset\":\"0xzz\"", "/threads/0/frames/0/offset"},
        {"\"offset\":\"0x1110\"", "\"offset\":\"0x11111111111111111\"",
         "/threads/0/frames/0/offset"},
        {"\"offset\":\"0x1110\",\"return\":false", "\"offset\":\"0x1110\"", "/threads/0/frames/0"},
        {"\"state\":\"walked\"", "\"state\":\"running\"", "/threads/0/state"},
        {"\"stop\":\"end\",", "", "/threads/0"},
        {"\"role\":\"main\"", "\"role\":\"god\"", "/threads/1/role"},
        {"\"registered\":\"audio_live\"", "\"registered\":\"other\"", "/threads/0/registered"},
        {"\"registered\":\"audio_live\"", "\"registered\":3", "/threads/0/registered"},
        {"\"worker\":2", "\"worker\":0", "/threads/0/worker"},
        {"\"worker\":2", "\"worker\":65536", "/threads/0/worker"},
        {"\"worker\":2", "\"worker\":\"2\"", "/threads/0/worker"},
    };
    for (const auto& c : cases) {
        auto text = good;
        const auto at = text.find(c.from);
        REQUIRE_MESSAGE(at != std::string::npos, c.from);
        text.replace(at, std::string(c.from).size(), c.to);
        const auto report = parseReport(text);
        REQUIRE_MESSAGE(!report, c.to);
        CHECK(report.error().code == "report.invalid");
        CHECK_MESSAGE(report.error().path == c.path, c.to);
        CHECK_FALSE(report.error().hint.empty());
    }
    CHECK(parseReport("").error().code == "report.invalid");
    CHECK(parseReport("[]").error().code == "report.invalid");
    CHECK(parseReport(std::string(kMaxReportBytes + 1, ' ')).error().code == "report.invalid");
}

TEST_CASE("every prefix of a report is refused or parsed, never trusted blindly") {
    const std::string text = reportText("elf", kId);
    for (std::size_t length = 0; length < text.size(); ++length)
        CHECK_FALSE(parseReport(std::string_view(text).substr(0, length)));
    CHECK(parseReport(text));
}

TEST_CASE("a report file is read only when it is a regular file of a sane size") {
    Dir dir("crash-resolver-report");
    write(dir.path / "r.json", reportText("elf", kId));
    CHECK(readReport(dir.path / "r.json"));
    CHECK(readReport(dir.path / "missing.json").error().code == "report.unreadable");
    CHECK(readReport(dir.path).error().code == "report.unreadable");
    write(dir.path / "big.json", std::string(kMaxReportBytes + 1, ' '));
    CHECK(readReport(dir.path / "big.json").error().code == "report.unreadable");
    write(dir.path / "bad.json", "{");
    CHECK(readReport(dir.path / "bad.json").error().code == "report.invalid");
}

TEST_CASE("scan finds images by content in files and folders, in a fixed order, past decoys") {
    Dir dir("crash-resolver-scan");
    write(dir.path / "b" / "deep" / "app.debug", elfWith({0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4}));
    write(dir.path / "a" / "other", elfWith({9, 9, 9, 9}));
    write(dir.path / "a" / "notes.txt", std::string("not an image"));
    write(dir.path / "empty", std::string());
#if !defined(_WIN32)
    std::error_code ec;
    fs::create_symlink(dir.path / "b" / "deep" / "app.debug", dir.path / "link", ec);
    if (!ec) fs::create_directory_symlink(dir.path / "b", dir.path / "dirlink", ec);
#endif
    ScanStats stats;
    const auto found = scan({dir.path}, stats);
    REQUIRE(found.size() == 2);
    CHECK(found[0].path.filename() == "other");  // "a" sorts before "b", links are skipped
    CHECK(found[1].path.filename() == "app.debug");
    CHECK(found[1].image.id == kId);
    CHECK_FALSE(stats.truncated);
    ScanStats single;
    const auto direct = scan({dir.path / "b" / "deep" / "app.debug", dir.path / "missing"}, single);
    CHECK(direct.size() == 1);

    ScanStats few;
    ScanLimits one;
    one.maxFiles = 1;
    scan({dir.path}, few, one);
    CHECK(few.truncated);
    CHECK(few.examined == 1);
    ScanStats crowded;
    ScanLimits narrow;
    narrow.maxDirectoryEntries = 2;
    CHECK(scan({dir.path}, crowded, narrow).empty());  // the top folder holds more than 2 entries
    CHECK(crowded.truncated);
    ScanStats shallow;
    ScanLimits flat;
    flat.maxDepth = 0;
    CHECK(scan({dir.path}, shallow, flat).size() == 0);
    CHECK(shallow.truncated);
}

TEST_CASE("frames are named by the file whose build id matches, return addresses by the call") {
    Dir dir("crash-resolver-resolve");
    write(dir.path / "decoy", elfWith({1, 2, 3, 4}));
    write(dir.path / "app", elfWith({0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4}));
    ScanStats stats;
    const auto candidates = scan({dir.path}, stats);
    const auto report = parseReport(reportText("elf", kId));
    REQUIRE(report);
    const auto resolution = resolve(*report, candidates);
    REQUIRE(resolution.modules.size() == 1);
    CHECK(resolution.modules[0].state == ModuleState::Resolved);
    CHECK(resolution.modules[0].file.filename() == "app");
    const auto& frames = report->threads[0].frames;
    CHECK(resolution.name(frames[0]) == "plain_main");
    CHECK(resolution.locate(frames[0])->second == 0x10);
    // 0x1201 follows a call that ended the previous instruction at 0x1200: that is plain_bar,
    // and the distance shown is that of the return address itself.
    CHECK(resolution.name(frames[1]) == "plain_bar");
    CHECK(resolution.locate(frames[1])->second == 1);
    CHECK_FALSE(resolution.name(frames[2]));  // generated code
    CHECK_FALSE(resolution.name(frames[3]));  // outside every function

    const auto text = formatText(*report, resolution);
    CHECK(text.find("resolved") != std::string::npos);
    CHECK(text.find("app+0x1110  plain_main + 0x10") != std::string::npos);
    CHECK(text.find("(generated code)") != std::string::npos);
    CHECK(text.find("(no symbol)") != std::string::npos);
    CHECK(text.find("thread 1 main: unavailable (reason: ptrace_denied)") != std::string::npos);
    CHECK(text.find("thread 0 fault (audio_live): walked (stop: end)") != std::string::npos);

    const auto json = formatJson(*report, resolution);
    CHECK(json.find("schema")->asString() == "supple.crash-symbols/1.0");
    CHECK(json.find("threads")->at(0).find("registered")->asString() == "audio_live");
    CHECK(json.find("threads")->at(0).find("worker")->asInt() == 2);
    CHECK_FALSE(json.find("threads")->at(1).find("registered"));
    const auto& jframes = *json.find("threads")->at(0).find("frames");
    CHECK(jframes.at(0).find("function")->asString() == "plain_main");
    CHECK(jframes.at(0).find("offset")->asString() == "0x1110");
    CHECK(jframes.at(1).find("displacement")->asInt() == 1);
    CHECK(jframes.at(2).find("module")->isNull());
    CHECK_FALSE(jframes.at(3).find("function"));
    const auto& module = json.find("modules")->at(0);
    CHECK(module.find("state")->asString() == "resolved");
    CHECK(module.find("functions")->asInt() == 2);
    // The output is itself valid JSON.
    CHECK(supple::json::parse(supple::json::writeCanonical(json)));
}

TEST_CASE("a module with no file, no id or no symbols is reported as such, never guessed") {
    Dir dir("crash-resolver-states");
    write(dir.path / "app", elfWith({0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4}));
    images::PeSpec noExports;
    noExports.exportTable = false;
    write(dir.path / "app.dll", images::pe(noExports));
    ScanStats stats;
    const auto candidates = scan({dir.path}, stats);

    // The same id under another format is not a match.
    auto report = parseReport(reportText("macho", kId));
    REQUIRE(report);
    CHECK(resolve(*report, candidates).modules[0].state == ModuleState::NotFound);
    report = parseReport(reportText("elf", "0102"));
    REQUIRE(report);
    CHECK(resolve(*report, candidates).modules[0].state == ModuleState::NotFound);
    report = parseReport(reportText("elf", ""));
    REQUIRE(report);
    CHECK(resolve(*report, candidates).modules[0].state == ModuleState::NoIdentity);
    // Matched by id, but a PE image without exports has no names to give.
    report = parseReport(reportText("pe", "123456789abcdef001020304050607080000002a"));
    REQUIRE(report);
    const auto pe = resolve(*report, candidates);
    CHECK(pe.modules[0].state == ModuleState::NoSymbols);
    CHECK(pe.modules[0].file.filename() == "app.dll");
    CHECK_FALSE(pe.modules[0].error.empty());
    CHECK_FALSE(pe.name(report->threads[0].frames[0]));
    const auto text = formatText(*report, pe);
    CHECK(text.find("no_symbols") != std::string::npos);
    CHECK(text.find("(no symbol)") != std::string::npos);
    // A file that went away between the scan and the read.
    report = parseReport(reportText("elf", kId));
    REQUIRE(report);
    fs::remove(dir.path / "app");
    CHECK(resolve(*report, candidates).modules[0].state == ModuleState::Unreadable);
}

namespace {
// What a stripped release binary keeps: a few exported names in .dynsym and none of the
// functions the report's frames are in. Same build id as `elfWith`'s images.
std::vector<std::uint8_t> strippedWith(std::vector<std::uint8_t> id) {
    images::ElfSpec spec;
    spec.buildId = std::move(id);
    spec.symtab = false;
    spec.functions = {{"exported_entry", 0x1800, 0x10}};
    return images::elf(spec);
}
const std::vector<std::uint8_t> kIdBytes{0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4};
}  // namespace

TEST_CASE("a stripped binary does not hide its own debug file") {
    // The stripped file sorts first, as `app` before `app.debug` and as an install folder
    // before a symbols folder do; the debug file carries the same build id.
    Dir dir("crash-resolver-shadow");
    write(dir.path / "app", strippedWith(kIdBytes));
    write(dir.path / "app.debug", elfWith(kIdBytes));
    const auto report = parseReport(reportText("elf", kId));
    REQUIRE(report);
    ScanStats stats;
    const auto inFolder = scan({dir.path}, stats);
    REQUIRE(inFolder.size() == 2);
    CHECK(inFolder[0].path.filename() == "app");
    // Across two --symbols arguments, in either order, the file that names the frames wins.
    std::vector<Candidate> twoRoots = scan({dir.path / "app"}, stats);
    for (auto& c : scan({dir.path / "app.debug"}, stats)) twoRoots.push_back(std::move(c));
    auto reversed = twoRoots;
    std::reverse(reversed.begin(), reversed.end());
    for (const std::vector<Candidate>* candidates :
         std::initializer_list<const std::vector<Candidate>*>{&inFolder, &twoRoots, &reversed}) {
        const auto resolution = resolve(*report, *candidates);
        REQUIRE(resolution.modules.size() == 1);
        CHECK(resolution.modules[0].state == ModuleState::Resolved);
        CHECK(resolution.modules[0].file.filename() == "app.debug");
        CHECK(resolution.name(report->threads[0].frames[0]) == "plain_main");
        CHECK(resolution.name(report->threads[0].frames[1]) == "plain_bar");
    }
}

TEST_CASE("a module is resolved only if it names something in the report") {
    Dir dir("crash-resolver-unnamed");
    write(dir.path / "app", strippedWith(kIdBytes));  // names a function the frames are not in
    ScanStats stats;
    const auto candidates = scan({dir.path}, stats);
    const auto report = parseReport(reportText("elf", kId));
    REQUIRE(report);
    const auto resolution = resolve(*report, candidates);
    REQUIRE(resolution.modules.size() == 1);
    // The file matched and has names, but none of this report's frames got one: not "resolved".
    CHECK(resolution.modules[0].state == ModuleState::NoSymbols);
    CHECK(resolution.modules[0].file.filename() == "app");
    CHECK(resolution.modules[0].error.find("none of the") != std::string::npos);
    CHECK_FALSE(resolution.name(report->threads[0].frames[0]));
    CHECK(formatText(*report, resolution).find("no_symbols") != std::string::npos);
    // A module that no frame is in cannot be judged by its frames: its names are enough.
    Report bare;
    bare.modules.push_back({Format::Elf, kId});
    CHECK(resolve(bare, candidates).modules[0].state == ModuleState::Resolved);
}

TEST_CASE("a module named many times is loaded once, and the total is bounded") {
    Dir dir("crash-resolver-many");
    write(dir.path / "app", elfWith(kIdBytes));
    ScanStats stats;
    const auto candidates = scan({dir.path}, stats);
    // A report may name one image in all of its 256 module entries: one table serves them all.
    Report many;
    for (int i = 0; i < 256; ++i) many.modules.push_back({Format::Elf, kId});
    const auto resolution = resolve(many, candidates);
    REQUIRE(resolution.modules.size() == 256);
    for (const auto& module : resolution.modules) {
        CHECK(module.state == ModuleState::Resolved);
        CHECK(module.table.get() == resolution.modules[0].table.get());
    }
    // Different images each load their own, until the total of functions held is spent.
    write(dir.path / "other", elfWith({1, 2, 3, 4}));
    const auto both = scan({dir.path}, stats);
    Report two;
    two.modules.push_back({Format::Elf, kId});
    two.modules.push_back({Format::Elf, "01020304"});
    Limits budget;
    budget.maxTotalSymbols = 3;  // each image has two functions
    const auto limited = resolve(two, both, budget);
    CHECK(limited.modules[0].state == ModuleState::Resolved);
    CHECK(limited.modules[1].state == ModuleState::Unreadable);
    CHECK(limited.modules[1].error.find("total") != std::string::npos);
    CHECK(resolve(two, both).modules[1].state == ModuleState::Resolved);
}

TEST_CASE("a return address at offset zero has no call before it") {
    Report report;
    report.modules.push_back({Format::Elf, kId});
    Resolution resolution;
    resolution.modules.resize(1);
    ReportFrame frame;
    frame.module = 0;
    frame.isReturn = true;
    CHECK_FALSE(resolution.name(frame));
    frame.module = 7;
    CHECK_FALSE(resolution.name(frame));
}
