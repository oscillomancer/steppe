// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Crash safe-load, the engine's part (topic 7d): whether a report's suspicion applies to the
// installed plug-in, project.plugins, the project.suspect_plugin warning, project.open's
// expect_hash, settling by a clean run. No test waits for a clock: time is injected.
#include <doctest/doctest.h>
#include <fcntl.h>
#include <supple/base/files.h>
#include <supple/diagnostics/reports.h>
#include <supple/engine/app.h>
#include <supple/engine/realtime.h>
#include <supple/imageid/image.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include "../../diagnostics/resolver/tests/images.h"
#include "bridge_patience.h"
#include "suspects.h"

using namespace supple;
using namespace supple::engine;
using supple::engine::detail::SuspectCheck;
using supple::engine::detail::SuspectOptions;
using supple::engine::detail::SuspicionVerdict;
namespace fs = std::filesystem;
namespace diag = supple::diagnostics;
namespace imageid = supple::diagnostics::resolver;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kGain = "com.suppleaudio.test.gain";

struct Root {
    fs::path path;
    explicit Root(const std::string& tag = "a", bool keep = false)
        : path(fs::temp_directory_path() /
               ("supple-suspects-" + std::to_string(::getpid()) + "-" + tag)) {
        if (!keep) fs::remove_all(path);
    }
    ~Root() { fs::remove_all(path); }
};

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

host::PluginInfo installed(std::string version = "1.0", fs::path file = "/nowhere/plugin.clap") {
    host::PluginInfo info;
    info.format = "clap";
    info.id = "com.acme.canary";
    info.name = "Canary";
    info.version = std::move(version);
    info.path = std::move(file);
    return info;
}

diag::Suspect suspect(std::string build, std::string version = "1.0",
                      std::string attribution = "fault_in_image",
                      std::vector<std::string> incidents = {"a1"}) {
    diag::Suspect s;
    s.format = "clap";
    s.pluginId = "com.acme.canary";
    s.buildId = std::move(build);
    s.name = "Canary";
    s.version = std::move(version);
    s.origin = "foreign";
    s.attribution = std::move(attribution);
    s.incidents = std::move(incidents);
    return s;
}

// A check whose file reads and clock the test holds.
struct Rig {
    std::map<fs::path, std::optional<std::vector<std::string>>> files;
    std::vector<fs::path> reads;
    std::chrono::steady_clock::time_point clock{};
    SuspectOptions options() {
        SuspectOptions o;
        o.now = [this] { return clock; };
        o.readIds = [this](const fs::path& file) {
            reads.push_back(file);
            const auto it = files.find(file);
            return it == files.end() ? std::nullopt : it->second;
        };
        return o;
    }
};

}  // namespace

TEST_CASE("a suspect applies to the installed build, and to no other") {
    Rig rig;
    rig.files["/nowhere/plugin.clap"] = std::vector<std::string>{"aaaa"};
    {
        SuspectCheck check({suspect("aaaa")}, rig.options());
        const auto v = check.check(installed());
        CHECK(v.build == SuspicionVerdict::Build::Same);
        CHECK(v.incidents == std::vector<std::string>{"a1"});
        CHECK(v.attribution == "fault_in_image");
    }
    {  // E10: the plug-in was reinstalled, a different build: not offered
        SuspectCheck check({suspect("bbbb")}, rig.options());
        CHECK_FALSE(check.check(installed()).applies());
    }
    {  // a universal binary holds the id in any one of its images
        rig.files["/nowhere/plugin.clap"] = std::vector<std::string>{"1111", "bbbb"};
        SuspectCheck check({suspect("bbbb")}, rig.options());
        CHECK(check.check(installed()).build == SuspicionVerdict::Build::Same);
    }
    {  // another id or format is not this plug-in
        auto other = suspect("aaaa");
        other.pluginId = "com.acme.other";
        auto vst = suspect("aaaa");
        vst.format = "vst3";
        SuspectCheck check({other, vst}, rig.options());
        const auto before = rig.reads.size();
        CHECK_FALSE(check.check(installed()).applies());
        CHECK(rig.reads.size() == before);  // no file was read for a plug-in nothing names
    }
}

TEST_CASE("an installed file that cannot be checked is asked about, not guessed") {
    Rig rig;  // E11: no entry for the file: it cannot be read
    SuspectCheck check({suspect("aaaa")}, rig.options());
    const auto v = check.check(installed());
    CHECK(v.build == SuspicionVerdict::Build::Unreadable);
    CHECK(v.applies());
    CHECK(v.incidents == std::vector<std::string>{"a1"});
}

TEST_CASE("the read budget: a few files, a short time, then asked about") {
    Rig rig;
    for (const char* name : {"one", "two", "three", "four"})
        rig.files[std::string("/nowhere/") + name + ".clap"] = std::vector<std::string>{"bbbb"};
    std::vector<diag::Suspect> suspects;
    for (const char* id : {"one", "two", "three", "four"}) {
        auto s = suspect("aaaa");
        s.pluginId = id;
        suspects.push_back(s);
    }
    auto plug = [](const char* id) {
        auto info = installed("1.0", std::string("/nowhere/") + id + ".clap");
        info.id = id;
        return info;
    };
    SUBCASE("at most three files") {
        SuspectCheck check(suspects, rig.options());
        CHECK_FALSE(check.check(plug("one")).applies());
        CHECK_FALSE(check.check(plug("two")).applies());
        CHECK_FALSE(check.check(plug("three")).applies());
        CHECK(check.check(plug("four")).build == SuspicionVerdict::Build::Unreadable);
        CHECK(rig.reads.size() == 3);
    }
    SUBCASE("a file is read once however many plug-ins share it") {
        SuspectCheck check(suspects, rig.options());
        auto same = plug("one");
        CHECK_FALSE(check.check(same).applies());
        CHECK_FALSE(check.check(same).applies());
        CHECK(rig.reads.size() == 1);
    }
    SUBCASE("at most 250 ms") {
        SuspectCheck check(suspects, rig.options());
        CHECK_FALSE(check.check(plug("one")).applies());  // the clock starts at the first read
        rig.clock += std::chrono::milliseconds(251);
        CHECK(check.check(plug("two")).build == SuspicionVerdict::Build::Unreadable);
        CHECK(rig.reads.size() == 1);
    }
}

TEST_CASE("a report with no build is matched on the version") {
    Rig rig;
    auto token = suspect("", "1.2", "active_token");
    SUBCASE("the version the report names") {
        SuspectCheck check({token}, rig.options());
        const auto v = check.check(installed("1.2"));
        CHECK(v.build == SuspicionVerdict::Build::Version);
        CHECK(v.attribution == "active_token");
        CHECK(rig.reads.empty());  // nothing to compare in a file
    }
    SUBCASE("another version is another plug-in") {
        SuspectCheck check({token}, rig.options());
        CHECK_FALSE(check.check(installed("1.3")).applies());
    }
    SUBCASE("the strongest level and every incident, once") {
        rig.files["/nowhere/plugin.clap"] = std::vector<std::string>{"aaaa"};
        SuspectCheck check({token, suspect("aaaa", "1.2", "fault_in_image", {"b2", "a1"})},
                           rig.options());
        const auto v = check.check(installed("1.2"));
        CHECK(v.build == SuspicionVerdict::Build::Same);
        CHECK(v.attribution == "fault_in_image");
        CHECK(v.incidents == std::vector<std::string>{"a1", "b2"});
    }
}

TEST_CASE("the default reader reads the build ids of ELF, Mach-O and PE files on every platform") {
    Root root;
    fs::create_directories(root.path);
    auto write = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        const fs::path file = root.path / name;
        std::ofstream(file, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return file;
    };
    auto idOf = [](const std::vector<std::uint8_t>& bytes) {
        const auto images = imageid::identify(imageid::MemorySource(bytes));
        REQUIRE_FALSE(images.empty());
        return images.front().id;
    };
    const auto elf = images::elf({});
    const auto macho = images::macho({});
    const auto pe = images::pe({});
    const auto universal =
        images::fat({images::macho({}),
                     images::macho({.uuid = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}})});
    struct Case {
        const char* name;
        const std::vector<std::uint8_t>* bytes;
    };
    for (const Case& c : {Case{"elf.clap", &elf}, Case{"macho.clap", &macho}, Case{"pe.clap", &pe},
                          Case{"fat.clap", &universal}}) {
        CAPTURE(c.name);
        const fs::path file = write(c.name, *c.bytes);
        const std::string id = idOf(*c.bytes);
        REQUIRE_FALSE(id.empty());
        {
            SuspectCheck check({suspect(id)});
            CHECK(check.check(installed("1.0", file)).build == SuspicionVerdict::Build::Same);
        }
        {
            SuspectCheck check({suspect("00ff00ff")});
            CHECK_FALSE(check.check(installed("1.0", file)).applies());
        }
    }
    {  // the second slice of a universal binary counts
        const std::string second = imageid::identify(imageid::MemorySource(universal)).back().id;
        SuspectCheck check({suspect(second)});
        CHECK(check.check(installed("1.0", root.path / "fat.clap")).build ==
              SuspicionVerdict::Build::Same);
    }
    {  // not an image, and not there
        const fs::path junk = write("junk.clap", {1, 2, 3, 4, 5});
        SuspectCheck check({suspect("aaaa")});
        CHECK(check.check(installed("1.0", junk)).build == SuspicionVerdict::Build::Unreadable);
        CHECK(check.check(installed("1.0", root.path / "missing.clap")).build ==
              SuspicionVerdict::Build::Unreadable);
    }
}

namespace {

struct Bus {
    Root root;
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    // `session`: an unsaved project kept in unsaved/<session>, as the GUI keeps its own;
    // `keep`: the root already holds an earlier run's files.
    explicit Bus(bool scan = true, const std::string& tag = "a", bool saysOnStderr = false,
                 const std::string& session = {}, bool keep = false)
        : root(tag, keep) {
        host::setBridgeExecutable(BRIDGE_PATH);
        registerEngineCommands(registry);
        AppOptions o;
        o.workdir = root.path;
        o.chainDir = root.path / "chains";
        o.presetDir = root.path / "presets";
        o.templateDir = root.path / "templates";
        o.projectTemplateDir = root.path / "project-templates";
        o.pluginPath = {kFixture};
        o.warnSuspectsOnStderr = saysOnStderr;
        if (!session.empty()) o.scratchDir = root.path / "unsaved" / session;
        auto made = EngineApp::create(o, {.console9 = false});
        REQUIRE(made);
        app = std::move(*made);
        if (!session.empty()) {
            app->setFixedClock(1'000'000);
            REQUIRE(app->newProject({}));  // its autosave interval starts on that clock
        }
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        if (scan) REQUIRE(app->plugins());
    }
    // The run ends without discarding its project, as a crash does.
    void die() {
        bus.reset();
        app.reset();
    }
    Result<json::Value> run(std::string command, std::string args) {
        return bus->execute({std::move(command), j(args), kTest});
    }
    json::Value call(std::string command, std::string args) {
        auto r = run(std::move(command), std::move(args));
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    }
    // A project with the gain on one track and the sine on another, saved as `name`.
    fs::path song(const char* name = "song.json") {
        call("batch", R"({"steps": [
            {"command": "track.add", "args": {"name": "Lead"}, "as": "t"},
            {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
            {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "com.suppleaudio.test.gain"}},
            {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice lead; 1: C4:w"}}]})");
        const fs::path file = root.path / name;
        REQUIRE(app->save(file));
        return file;
    }
    std::string quoted(const fs::path& p) { return json::writeCompact(json::Value(p.string())); }
    // The catalogue entry the app resolves the gain to.
    host::PluginInfo gain() {
        auto info = app->lookupPlugin(kGain);
        REQUIRE(info);
        return *info;
    }
    // The installed gain, as described to the catalogue with another file behind it.
    void installAt(const fs::path& file) {
        auto list = std::vector<host::PluginInfo>(app->plugins()->begin(), app->plugins()->end());
        for (auto& p : list)
            if (p.id == kGain) p.path = file;
        app->installPlugins(std::move(list), {});
    }
    std::string crash(const char* incident, const std::string& build) {
        REQUIRE(diag::writeSampleCrash(incident, "Test Gain", "Supple Audio", app->diagnosticsDir(),
                                       {.id = kGain, .version = gain().version, .buildId = build}));
        return incident;
    }
    json::Value gainEntry(json::Value listing) {
        for (std::size_t i = 0; i < listing["plugins"].size(); ++i) {
            json::Value item = listing["plugins"].at(i);
            if (item["id"].asString() == kGain) return item;
        }
        FAIL("the gain is not in the project");
        return {};
    }
};

std::vector<std::uint8_t> imageWithId(std::uint8_t seed) {
    return images::elf({.buildId = {seed, 2, 3, 4, 5, 6, 7, 8}});
}
std::string idOfImage(const std::vector<std::uint8_t>& bytes) {
    return imageid::identify(imageid::MemorySource(bytes)).front().id;
}
void putFile(const fs::path& file, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}
bool hasWarning(json::Value opened, std::string_view code) {
    if (!opened.find("warnings")) return false;
    json::Value list = *opened.find("warnings");
    for (std::size_t i = 0; i < list.size(); ++i) {
        json::Value item = list.at(i);
        if (item["code"].asString() == code) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("project.plugins names a project's plug-ins and the suspect among them, loading none") {
    Bus f;
    const fs::path song = f.song();
    const fs::path dll = f.root.path / "installed" / "gain.clap";
    const auto image = imageWithId(1);
    putFile(dll, image);
    f.installAt(dll);  // the catalogue entry now points at a file the test controls
    // Nothing named the plug-in yet.
    auto first = f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}");
    CHECK(first["plugins"].size() == 2);  // the sine and the gain, once each
    CHECK(first["hash"].asString().rfind("sha256:", 0) == 0);
    CHECK_FALSE(first["catalogue_empty"].asBool());
    auto gain = f.gainEntry(first);
    CHECK(gain["format"].asString() == "clap");
    CHECK(gain["uses"].asInt() == 1);
    CHECK(gain["branches"].size() == 1);
    CHECK(gain["resolved"]["path"].asString() == dll.string());
    CHECK(gain["mode"].asString() == "normal");
    CHECK(gain["suspect"].isNull());

    SUBCASE("a report for this build") {
        const auto incident = f.crash("00000000000000000000000000000001", idOfImage(image));
        auto listing = f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}");
        auto entry = f.gainEntry(listing);
        REQUIRE_FALSE(entry["suspect"].isNull());
        CHECK(entry["suspect"]["build"].asString() == "same");
        CHECK(entry["suspect"]["attribution"].asString() == "fault_in_image");
        REQUIRE(entry["suspect"]["incidents"].size() == 1);
        CHECK(entry["suspect"]["incidents"].at(0).asString() == incident);
        // The other plug-in is not named by any report.
        for (std::size_t i = 0; i < listing["plugins"].size(); ++i) {
            json::Value item = listing["plugins"].at(i);
            if (item["id"].asString() != kGain) CHECK(item["suspect"].isNull());
        }
        // Seen is not resolved (Q1): acknowledging changes nothing.
        f.call("crash.acknowledge", R"({"incident": ")" + incident + "\"}");
        CHECK_FALSE(
            f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"))["suspect"]
                .isNull());
        // E7: settled clears it, and the report stays.
        f.call("crash.settle", R"({"incident": ")" + incident + "\"}");
        CHECK(
            f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"))["suspect"]
                .isNull());
        CHECK(f.call("crash.list", "{}")["reports_total"].asInt() == 1);
        // E14: deleting clears too.
    }
    SUBCASE("a report for another build is not this plug-in (E10)") {
        f.crash("00000000000000000000000000000001", idOfImage(imageWithId(7)));
        CHECK(
            f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"))["suspect"]
                .isNull());
    }
    SUBCASE("a file that cannot be checked is asked about (E11)") {
        f.crash("00000000000000000000000000000001", idOfImage(image));
        fs::remove(dll);
        auto entry = f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"));
        REQUIRE_FALSE(entry["suspect"].isNull());
        CHECK(entry["suspect"]["build"].asString() == "unreadable");
    }
    SUBCASE("a protected plug-in says its mode") {
        f.call("plugin.override",
               std::string(R"({"plugin": ")") + kGain + R"(", "mode": "disable"})");
        CHECK(f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"))["mode"]
                  .asString() == "disable");
    }
    SUBCASE("deleting the report clears it (E14)") {
        const auto incident = f.crash("00000000000000000000000000000001", idOfImage(image));
        f.call("crash.delete", R"({"incident": ")" + incident + "\"}");
        CHECK(
            f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"))["suspect"]
                .isNull());
    }
}

TEST_CASE("project.plugins reads a project template, a folder or a file") {
    Bus f;
    f.song();
    const fs::path folder = f.root.path / "template";
    REQUIRE(f.app->writeTemplate(folder));
    for (const fs::path& where : {folder, folder / "project.json"}) {
        CAPTURE(where.string());
        auto listing =
            f.call("project.plugins", R"({"path": )" + f.quoted(where) + R"(, "template": true})");
        CHECK(listing["plugins"].size() == 2);
        CHECK(listing["path"].asString() == (folder / "project.json").string());
        CHECK(listing["hash"].asString().rfind("sha256:", 0) == 0);
    }
}

TEST_CASE("project.plugins answers from the catalogue as it stands, never scanning") {
    Bus f;
    const fs::path song = f.song();
    Bus empty(false, "b");  // no scan yet: the catalogue is empty
    auto listing = empty.call("project.plugins", R"({"path": )" + empty.quoted(song) + "}");
    CHECK(listing["catalogue_empty"].asBool());
    REQUIRE(listing["plugins"].size() == 2);
    for (std::size_t i = 0; i < listing["plugins"].size(); ++i) {
        json::Value item = listing["plugins"].at(i);
        CHECK(item["resolved"].isNull());
        CHECK(item["suspect"].isNull());  // not installed here: nothing to protect
    }
    // It did not scan: asking for the catalogue does, and then the same call resolves.
    REQUIRE(empty.app->plugins());
    CHECK_FALSE(
        empty.call("project.plugins", R"({"path": )" + empty.quoted(song) + "}")["catalogue_empty"]
            .asBool());
    // A bad file is an error with the path, and a template is read too.
    auto bad = empty.run("project.plugins", R"({"path": "/nonexistent/song.json"})");
    CHECK_FALSE(bad);
}

TEST_CASE("project.open warns of a suspect and loads it normally, unless the run is protected") {
    Bus f;
    const fs::path song = f.song();
    const fs::path dll = f.root.path / "installed" / "gain.clap";
    const auto image = imageWithId(1);
    putFile(dll, image);
    f.installAt(dll);
    const auto incident = f.crash("00000000000000000000000000000001", idOfImage(image));
    const std::string open = R"({"path": )" + f.quoted(song) + "}";

    auto opened = f.call("project.open", open);
    CHECK(hasWarning(opened, "project.suspect_plugin"));
    // The project is open all the same, the plug-in not overridden: the engine did not decide.
    CHECK(f.call("plugin.overrides", "{}")["overrides"].size() == 0);
    CHECK(f.app->head().fx.size() == 2);
    std::string text;
    for (std::size_t i = 0; i < opened["warnings"].size(); ++i) {
        json::Value item = opened["warnings"].at(i);
        if (item["code"].asString() == "project.suspect_plugin") text = item["message"].asString();
    }
    CHECK(text.find(incident) != std::string::npos);
    CHECK(text.find("plugin.override") != std::string::npos);
    CHECK(text.find("crash.settle") != std::string::npos);

    // E8/E9: a bridge or a disable keeps the evidence, and the warning stops (it is protected).
    for (const char* mode : {"bridge", "disable"}) {
        f.call("plugin.override",
               std::string(R"({"plugin": ")") + kGain + R"(", "mode": ")" + mode + R"("})");
        CHECK_FALSE(hasWarning(f.call("project.open", open), "project.suspect_plugin"));
        CHECK_FALSE(f.gainEntry(f.call("project.plugins", open))["suspect"].isNull());
    }
    // Normal again, and still a suspect until it is settled.
    f.call("plugin.override", std::string(R"({"plugin": ")") + kGain + R"(", "mode": "normal"})");
    CHECK(hasWarning(f.call("project.open", open), "project.suspect_plugin"));
    f.call("crash.settle", R"({"incident": ")" + incident + "\"}");
    CHECK_FALSE(hasWarning(f.call("project.open", open), "project.suspect_plugin"));
}

#if !defined(_WIN32)
namespace {
// What standard error receives while `body` runs.
template <class Body>
std::string standardError(const fs::path& scratch, Body body) {
    std::fflush(stderr);
    const int saved = ::dup(2);
    const int file = ::open(scratch.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(saved >= 0);
    REQUIRE(file >= 0);
    ::dup2(file, 2);
    ::close(file);
    body();
    std::fflush(stderr);
    ::dup2(saved, 2);
    ::close(saved);
    std::ifstream in(scratch);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST_CASE(
    "a service says once per plug-in on standard error; one-shot runs and tests say nothing") {
    for (const bool service : {true, false}) {
        CAPTURE(service);
        Bus f(true, service ? "s" : "t", service);
        const fs::path song = f.song();
        const fs::path dll = f.root.path / "installed" / "gain.clap";
        const auto image = imageWithId(1);
        putFile(dll, image);
        f.installAt(dll);
        f.crash("00000000000000000000000000000001", idOfImage(image));
        const fs::path scratch = f.root.path / "stderr.txt";
        const std::string open = R"({"path": )" + f.quoted(song) + "}";
        const auto first = standardError(scratch, [&] { f.call("project.open", open); });
        const auto again = standardError(scratch, [&] { f.call("project.open", open); });
        if (service) {
            CHECK(first.find("Test Gain") != std::string::npos);
            CHECK(first.find("crash.settle") != std::string::npos);
            CHECK(std::count(first.begin(), first.end(), '\n') == 1);  // one line
            CHECK(again.empty());                                      // once per run
        } else {
            CHECK(first.empty());
            CHECK(again.empty());
        }
    }
}
#endif

TEST_CASE("a stock plug-in that is a suspect is offered no bridge") {
    Bus f;
    const fs::path song = f.song();
    const auto info = f.gain();
    const auto ids = imageid::identify(imageid::FileSource(info.path));
    if (ids.empty() || ids.front().id.empty()) {
        MESSAGE("the fixture library has no build id on this toolchain; the stock case is skipped");
        return;
    }
    f.crash("00000000000000000000000000000001", ids.front().id);
    // The fixture sits in no stock directory, so it is bridgeable; point the stock directory at it.
    f.app->instances().setStockDir(info.path.parent_path());
    auto entry = f.gainEntry(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}"));
    CHECK(entry["stock"].asBool());
    CHECK_FALSE(entry["bridgeable"].asBool());
    REQUIRE_FALSE(entry["suspect"].isNull());
    auto refused = f.run("plugin.override",
                         std::string(R"({"plugin": ")") + kGain + R"(", "mode": "bridge"})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "plugin.override_unavailable");
}

TEST_CASE("project.open reads the bytes project.plugins checked, or says the file changed") {
    Bus f;
    const fs::path song = f.song();
    auto checked = f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}");
    const std::string hash = checked["hash"].asString();
    CHECK(f.call("project.open",
                 R"({"path": )" + f.quoted(song) + R"(, "expect_hash": ")" + hash + "\"}")
              .find("name") != nullptr);
    // The file changes after the check (another save, an editor): the load refuses it.
    f.call("track.add", R"({"name": "Pad"})");
    REQUIRE(f.app->save(song));
    auto refused = f.run("project.open",
                         R"({"path": )" + f.quoted(song) + R"(, "expect_hash": ")" + hash + "\"}");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "project.changed");
    CHECK(refused.error().hint.find("project.plugins") != std::string::npos);
    // Without the hash it opens, as before.
    CHECK(f.run("project.open", R"({"path": )" + f.quoted(song) + "}"));
}

TEST_CASE("a run that ended well settles what it loaded normally, and nothing it protected") {
    Bus f;
    const fs::path song = f.song();
    const auto info = f.gain();
    const auto ids = imageid::identify(imageid::FileSource(info.path));
    if (ids.empty() || ids.front().id.empty()) {
        MESSAGE(
            "the fixture library has no build id on this toolchain; the clean-run case is skipped");
        return;
    }
    const std::string build = ids.front().id;
    auto unsettled = [&] { return diag::suspects(f.app->diagnosticsDir()).value().size(); };
    auto play = [&](EngineApp& app) {
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(app.startEngine(backend, {.sampleRate = 48000, .blockSize = 64},
                                test::patient({.anticipative = false})));
        f.call("transport.play", "{}");
        for (int i = 0; i < 40; ++i) {
            backend.run(1);
            app.tick();
        }
        f.call("engine.stop", "{}");
    };
    f.crash("00000000000000000000000000000001", build);
    REQUIRE(unsettled() == 1);

    SUBCASE("E12: loaded normally, activated: settled") {
        f.app->settleCleanSuspects();  // nothing ran yet
        CHECK(unsettled() == 1);
        play(*f.app);
        f.app->settleCleanSuspects();
        CHECK(unsettled() == 0);
        CHECK(f.call("crash.list", "{}")["reports_total"].asInt() == 1);  // the report stays
    }
    SUBCASE("E13: a bridged run does not settle") {
        f.call("plugin.override",
               std::string(R"({"plugin": ")") + kGain + R"(", "mode": "bridge"})");
        play(*f.app);
        f.app->settleCleanSuspects();
        CHECK(unsettled() == 1);
    }
    SUBCASE("E13: a disabled one never ran, so does not settle") {
        f.call("plugin.override",
               std::string(R"({"plugin": ")") + kGain + R"(", "mode": "disable"})");
        play(*f.app);
        f.app->settleCleanSuspects();
        CHECK(unsettled() == 1);
    }
    SUBCASE("never activated: not settled (Q8)") {
        REQUIRE(f.app->instances().size() == 0);
        f.app->settleCleanSuspects();
        CHECK(unsettled() == 1);
    }
    SUBCASE("what an earlier project ran counts for the run") {
        play(*f.app);
        f.call("project.new", "{}");  // the pool that ran it is replaced
        f.app->settleCleanSuspects();
        CHECK(unsettled() == 0);
    }
    SUBCASE("another build of the plug-in does not settle the report") {
        REQUIRE(diag::deleteAllReports(f.app->diagnosticsDir()));
        f.crash("00000000000000000000000000000002", "00ff00ff00ff00ff");
        play(*f.app);
        f.app->settleCleanSuspects();
        CHECK(unsettled() == 1);  // the installed build is not the one that crashed
    }
}

TEST_CASE("recovery is checked before it opens: project.plugins reads the autosave recover would") {
    Bus f;
    f.app->setFixedClock(1'000'000);
    const fs::path dll = f.root.path / "installed" / "gain.clap";
    const auto image = imageWithId(1);
    putFile(dll, image);
    f.installAt(dll);
    // The saved file holds the sine alone; the autosave after it also holds the gain.
    const std::string track = f.call("track.add", R"({"name": "Lead"})")["track"].asString();
    f.call("fx.add", R"({"track": ")" + track + R"(", "plugin": "Test Sine"})");
    const fs::path song = f.root.path / "song.json";
    REQUIRE(f.app->save(song));
    f.call("fx.add", R"({"track": ")" + track + R"(", "plugin": ")" + kGain + R"("})");
    f.app->setFixedClock(1'060'000);
    f.app->tick();
    REQUIRE(fs::exists(f.root.path / "song.json-data" / "autosave-1.json"));
    REQUIRE(f.app->open(song));
    const std::string recoverOnly = R"({"autosave": true})";

    auto before = f.call("project.plugins", recoverOnly);
    CHECK(before["source"].asString() == "autosave");
    CHECK(before["autosave"].asString() == "autosave-1.json");
    CHECK(before["plugins"].size() == 2);
    CHECK(f.gainEntry(before)["suspect"].isNull());
    CHECK(f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}")["plugins"].size() == 1);

    const auto incident = f.crash("00000000000000000000000000000001", idOfImage(image));
    SUBCASE("the autosave names a plug-in the file does not: it is the one asked about") {
        auto file = f.call("project.plugins", R"({"path": )" + f.quoted(song) + "}");
        CHECK(file["plugins"].size() == 1);  // the file names no suspect
        auto listing = f.call("project.plugins", recoverOnly);
        auto entry = f.gainEntry(listing);
        REQUIRE_FALSE(entry["suspect"].isNull());
        CHECK(entry["suspect"]["build"].asString() == "same");
        CHECK(entry["suspect"]["incidents"].at(0).asString() == incident);
        // Nothing was instantiated or opened to find out.
        CHECK(f.app->head().fx.size() == 1);
        CHECK(f.app->instances().size() == 0);
    }
    SUBCASE("project.recover itself warns, and loads the autosave normally") {
        auto r =
            f.call("project.recover", R"({"autosave": true, "autosave_file": "autosave-1.json"})");
        CHECK(hasWarning(r, "project.suspect_plugin"));
        CHECK(f.app->head().fx.size() == 2);
    }
    SUBCASE("a protected plug-in is not warned of again") {
        f.call("plugin.override",
               std::string(R"({"plugin": ")") + kGain + R"(", "mode": "disable"})");
        CHECK(f.gainEntry(f.call("project.plugins", recoverOnly))["mode"].asString() == "disable");
        auto r = f.call("project.recover", recoverOnly);
        CHECK_FALSE(hasWarning(r, "project.suspect_plugin"));
    }
    SUBCASE("a settled report is no longer a suspect") {
        f.call("crash.settle", R"({"incident": ")" + incident + "\"}");
        CHECK(f.gainEntry(f.call("project.plugins", recoverOnly))["suspect"].isNull());
        CHECK_FALSE(hasWarning(f.call("project.recover", recoverOnly), "project.suspect_plugin"));
    }
    SUBCASE("the source is exactly one, and a recovery that is not there says so") {
        for (const char* bad : {"{}", R"({"autosave": true, "session": "x"})",
                                R"({"path": "a.json", "autosave": true})",
                                R"({"autosave_file": "autosave-1.json", "path": "a.json"})",
                                R"({"session": "x", "template": true})"})
            CHECK(f.run("project.plugins", bad).error().code == "project.plugins_args");
        CHECK(f.run("project.plugins", R"({"autosave": true, "autosave_file": "autosave-9.json"})")
                  .error()
                  .code == "recover.no_autosave");
        CHECK(f.run("project.plugins", R"({"session": "nope"})").error().code ==
              "recover.unknown_session");
        // Once the project is as saved there is nothing newer to recover.
        REQUIRE(f.app->save(song));
        CHECK(f.run("project.plugins", recoverOnly).error().code == "recover.no_autosave");
    }
}

TEST_CASE("a left-behind unsaved project is checked before project.recover {session} opens it") {
    const std::string left = "20261114-090000-4242";
    const auto image = imageWithId(1);
    Bus earlier(true, "s", false, left);
    {
        const fs::path dll = earlier.root.path / "installed" / "gain.clap";
        putFile(dll, image);
        earlier.installAt(dll);
        const std::string track =
            earlier.call("track.add", R"({"name": "Vocal"})")["track"].asString();
        earlier.call("fx.add", R"({"track": ")" + track + R"(", "plugin": ")" + kGain + R"("})");
        earlier.app->setFixedClock(1'060'000);
        earlier.app->tick();
        REQUIRE(fs::exists(earlier.root.path / "unsaved" / left / "autosave-1.json"));
    }
    earlier.die();
    Bus f(true, "s", false, "20261114-100000-1", true);
    f.installAt(earlier.root.path / "installed" / "gain.clap");
    const auto incident = f.crash("00000000000000000000000000000001", idOfImage(image));
    const std::string args = R"({"session": ")" + left + "\"}";

    auto listing = f.call("project.plugins", args);
    CHECK(listing["source"].asString() == "session");
    CHECK(listing["autosave"].asString() == "autosave-1.json");
    auto entry = f.gainEntry(listing);
    REQUIRE_FALSE(entry["suspect"].isNull());
    CHECK(entry["suspect"]["incidents"].at(0).asString() == incident);
    CHECK(f.app->head().fx.empty());  // nothing opened
    CHECK(f.run("project.plugins", R"({"session": "nope"})").error().code ==
          "recover.unknown_session");

    auto r = f.call("project.recover", args);
    CHECK(hasWarning(r, "project.suspect_plugin"));
    CHECK(f.app->head().fx.size() == 1);
}
