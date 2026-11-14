// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>

#include "../../../tests/file_platform.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/json.h"
#include "supple/model/project_file.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

TEST_CASE("autosave numbering preserves other files and cannot overflow") {
    struct Root {
        fs::path path =
            fs::temp_directory_path() / ("supple-autosave-" + std::to_string(test::processId()));
        Root() { fs::remove_all(path); }
        ~Root() { fs::remove_all(path); }
    } root;
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    app.setFixedClock(1'000'000);
    REQUIRE(app.save("project.json"));
    const auto dir = app.autosaveDir();
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(app.projectPath(), now - std::chrono::seconds(60));
    auto write = [&](const std::string& name) {
        REQUIRE(writeFileAtomic(dir / name, "preserved"));
        fs::last_write_time(dir / name, now);
    };
    auto save = [&] {
        auto transaction = app.beginTransaction({cmd::Origin::Type::Test, "unit", ""}, "Edit");
        REQUIRE(transaction);
        auto project = app.txn().project();
        project.name = "Changed";
        app.txn().setProject(project);
        REQUIRE(app.commitTransaction(*transaction));
        app.setFixedClock(1'060'000);
        app.tick();
    };
    SUBCASE("only numbered autosaves participate in discovery and rotation") {
        const std::vector<std::string> backups{"autosave-1-backup.json", "autosave-2.json.json",
                                               "autosave-999999999999999999999999.json"};
        for (const auto& name : backups) write(name);
        CHECK(app.newerAutosave(app.projectPath()).empty());
        for (int n = 1; n <= 6; ++n) write("autosave-" + std::to_string(n) + ".json");
        save();
        for (const auto& name : backups) {
            auto contents = readTextFile(dir / name);
            REQUIRE(contents);
            CHECK(*contents == "preserved");
        }
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
        CHECK_FALSE(fs::exists(dir / "autosave-2.json"));
        CHECK(fs::exists(dir / "autosave-7.json"));
    }
    SUBCASE("the next index exceeds a signed 32-bit integer") {
        write("autosave-2147483647.json");
        save();
        CHECK(fs::exists(dir / "autosave-2147483648.json"));
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
    }
    SUBCASE("an exhausted index reports a failure without writing or pruning") {
        write("autosave-18446744073709551615.json");
        save();
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
        CHECK(app.nextServiceDelayMs() == 60'000);
        bool reported = false;
        for (const auto& [topic, value] : app.takeEvents())
            if (topic == "engine.error" && value.isString() &&
                value.asString().find("autosave.index") != std::string::npos)
                reported = true;
        CHECK(reported);
        auto contents = readTextFile(dir / "autosave-18446744073709551615.json");
        REQUIRE(contents);
        CHECK(*contents == "preserved");
    }
}

// --- project files keep branches (ADR-0076) -------------------------------------------------

namespace {

struct Project {
    fs::path root =
        fs::temp_directory_path() / ("supple-branches-" + std::to_string(test::processId()));
    std::unique_ptr<EngineApp> app;
    Project() {
        fs::remove_all(root);
        auto created = EngineApp::create({.workdir = root});
        REQUIRE(created);
        app = std::move(*created);
        app->setFixedClock(1'000'000);
    }
    ~Project() {
        app.reset();
        fs::remove_all(root);
    }
    void rename(const std::string& name) {
        auto transaction = app->beginTransaction({cmd::Origin::Type::Test, "unit", ""}, "Rename");
        REQUIRE(transaction);
        auto project = app->txn().project();
        project.name = name;
        app->txn().setProject(project);
        REQUIRE(app->commitTransaction(*transaction));
    }
    std::string nameOn(std::string_view branch) const {
        return app->versions().head(branch)->project->name;
    }
    // Main is "Main"; agent/claude/mix is "Mix", with the agent branch current.
    void diverge() {
        rename("Main");
        REQUIRE(app->versions().createBranch("agent/claude/mix", "main"));
        REQUIRE(app->switchBranch("agent/claude/mix"));
        rename("Mix");
    }
};

bool hasWarning(const std::vector<schema::Warning>& warnings, std::string_view code) {
    for (const auto& w : warnings)
        if (w.code == code) return true;
    return false;
}

}  // namespace

TEST_CASE("a saved project is main's head and brings back its branches and current branch") {
    Project p;
    p.diverge();
    REQUIRE(p.app->save("song.json"));  // saved while the agent branch is current
    auto text = readTextFile(p.root / "song.json");
    REQUIRE(text);
    auto document = model::parseProject(*text);
    REQUIRE(document);
    CHECK(document->project->name == "Main");

    REQUIRE(p.app->newProject({}));
    std::vector<schema::Warning> warnings;
    REQUIRE(p.app->open("song.json", &warnings));
    CHECK_FALSE(hasWarning(warnings, "project.history"));
    CHECK(p.app->versions().branches() == std::vector<std::string>{"agent/claude/mix", "main"});
    CHECK(p.app->versions().branch() == "agent/claude/mix");
    CHECK(p.app->head().project->name == "Mix");
    CHECK(p.nameOn("main") == "Main");
    REQUIRE(p.app->undo());  // the branch's step survives, undoable
    CHECK(p.app->head().project->name == "Main");
}

TEST_CASE("an edit on another branch counts as a change to autosave") {
    Project p;
    REQUIRE(p.app->save("song.json"));
    p.app->tick();
    p.diverge();
    REQUIRE(p.app->switchBranch("main"));
    p.app->setFixedClock(1'060'000);  // an autosave keeps the branches too
    p.app->tick();
    const auto autosave = p.app->autosaveDir() / "autosave-1.json";
    REQUIRE(fs::exists(autosave));
    auto opened = model::parseProjectFile(*readTextFile(autosave));
    REQUIRE(opened);
    CHECK(opened->store.branches() == std::vector<std::string>{"agent/claude/mix", "main"});
    CHECK(opened->store.head("agent/claude/mix")->project->name == "Mix");

    // Then only the branch changes: main's head stays, and it autosaves again.
    const auto mainHead = p.app->versions().headVersion("main");
    REQUIRE(p.app->switchBranch("agent/claude/mix"));
    p.rename("Mix 2");
    REQUIRE(p.app->switchBranch("main"));
    CHECK(p.app->versions().headVersion("main") == mainHead);
    p.app->setFixedClock(1'120'000);
    p.app->tick();
    REQUIRE(fs::exists(p.app->autosaveDir() / "autosave-2.json"));
}

TEST_CASE("saving over a file whose branches did not load keeps a copy of it once") {
    Project p;
    p.diverge();
    REQUIRE(p.app->save("song.json"));
    // A program that does not keep branches changed main: they no longer fit.
    auto doc = json::parse(*readTextFile(p.root / "song.json")).value();
    doc["project"]["name"] = "Changed elsewhere";
    const std::string stale = json::writeCanonical(doc) + "\n";
    REQUIRE(writeFileAtomic(p.root / "song.json", stale));

    std::vector<schema::Warning> warnings;
    REQUIRE(p.app->open("song.json", &warnings));
    REQUIRE(hasWarning(warnings, "project.history"));
    CHECK(p.app->versions().branches() == std::vector<std::string>{"main"});
    CHECK(p.app->head().project->name == "Changed elsewhere");

    warnings.clear();
    REQUIRE(p.app->save("song.json", &warnings));
    CHECK(hasWarning(warnings, "project.history_backup"));
    const auto backup = p.root / "song.json.pre-branches.bak";
    REQUIRE(fs::exists(backup));
    CHECK(*readTextFile(backup) == stale);
    warnings.clear();
    REQUIRE(p.app->save("song.json", &warnings));  // once: the next save just saves
    CHECK_FALSE(hasWarning(warnings, "project.history_backup"));
    CHECK_FALSE(fs::exists(p.root / "song.json.pre-branches-2.bak"));
}
