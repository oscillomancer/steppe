// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <set>

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

// --- saved logs replay only edits (review of item A, finding 1) -----------------------------

TEST_CASE("a saved branch step that is not an edit is refused at open and at replay") {
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    const cmd::Origin unit{cmd::Origin::Type::Test, "unit", ""};
    auto run = [&](const char* command, std::string_view args) {
        auto r = d.execute({command, json::parse(args).value(), unit});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    };
    run("track.add", R"({"name": "Drums"})");
    REQUIRE(p.app->versions().createBranch("agent/claude/bass", "main"));
    REQUIRE(p.app->switchBranch("agent/claude/bass"));
    run("track.add", R"({"name": "Bass"})");
    REQUIRE(p.app->switchBranch("main"));
    run("track.add", R"({"name": "Keys"})");  // main moves on: a merge replays the branch
    REQUIRE(p.app->save("song.json"));
    const fs::path victim = p.root / "victim.txt";
    REQUIRE(writeFileAtomic(victim, "precious"));

    // Someone else's file: the branch's step became a save over another file.
    auto doc = json::parse(*readTextFile(p.root / "song.json")).value();
    bool replaced = false;
    for (auto& v : doc["branches"]["versions"].asObject())
        if (const auto* list = v.value.find("invocations"); list && list->size()) {
            json::Value step = json::Value::object();
            step["command"] = "project.save";
            step["args"] = json::Value::object();
            step["args"]["path"] = victim.string();
            v.value["invocations"] = json::Value::array();
            v.value["invocations"].push_back(std::move(step));
            replaced = true;
        }
    REQUIRE(replaced);
    REQUIRE(writeFileAtomic(p.root / "crafted.json", json::writeCanonical(doc)));
    std::vector<schema::Warning> warnings;
    REQUIRE(p.app->open("crafted.json", &warnings));
    CHECK(hasWarning(warnings, "project.history"));
    CHECK(p.app->versions().branches() == std::vector<std::string>{"main"});
    CHECK(*readTextFile(victim) == "precious");

    // And should one reach a replay, it is refused there too.
    json::Value saveArgs = json::Value::object();
    saveArgs["path"] = victim.string();
    auto replayed = p.app->replay(registry, p.app->head(), {{"project.save", saveArgs}},
                                  p.app->versions().nextVersion());
    REQUIRE_FALSE(replayed);
    CHECK(replayed.error().code == "history.command");
    CHECK(*readTextFile(victim) == "precious");
}

TEST_CASE("every edit command a saved branch may replay has been audited") {
    // A saved branch can name any of these, and a merge runs it (ADR-0076,
    // "Replayable steps"). Each was checked to change only the session and at
    // most add new files to the project's media folder. A new edit command
    // must be checked the same way before it joins this list.
    // Reading files is no new power: a project's media may name any path. Each
    // writes only new files in the media folder (item.add copies, renders,
    // freezes); rack.preset.load installs nothing while replaying.
    static const std::set<std::string> audited{"audio.quantize",
                                               "audio.warp.detach",
                                               "audio.warp.reset",
                                               "autoitem.add",
                                               "autoitem.duplicate",
                                               "autoitem.remove",
                                               "autoitem.set",
                                               "autoitem.unpool",
                                               "cable.add",
                                               "cable.remove",
                                               "cable.set",
                                               "env.add",
                                               "env.points.set",
                                               "env.remove",
                                               "env.set",
                                               "fx.add",
                                               "fx.chain.load",
                                               "fx.copy",
                                               "fx.learn.remove",
                                               "fx.learn.set",
                                               "fx.move",
                                               "fx.param.set",
                                               "fx.patch.merge",
                                               "fx.patch.set",
                                               "fx.preset.load",
                                               "fx.remove",
                                               "fx.set",
                                               "fx.state.capture",
                                               "group.add",
                                               "group.members.set",
                                               "group.remove",
                                               "group.set",
                                               "item.add",
                                               "item.apply-fx",
                                               "item.consolidate.commit",
                                               "item.duplicate",
                                               "item.explode-takes",
                                               "item.heal",
                                               "item.mix-to-track.commit",
                                               "item.move",
                                               "item.paste",
                                               "item.remove",
                                               "item.set",
                                               "item.split",
                                               "marker.add",
                                               "marker.remove",
                                               "marker.set",
                                               "midi.edit",
                                               "midi.write",
                                               "module.add",
                                               "module.duplicate",
                                               "module.learn.remove",
                                               "module.learn.set",
                                               "module.randomize",
                                               "module.remove",
                                               "module.set",
                                               "project.set",
                                               "rack.add",
                                               "rack.arrange",
                                               "rack.capture.commit",
                                               "rack.paste",
                                               "rack.patch",
                                               "rack.preset.load",
                                               "rack.remove",
                                               "rack.set",
                                               "region.add",
                                               "region.remove",
                                               "region.set",
                                               "render.preset.load",
                                               "route.add",
                                               "route.learn.remove",
                                               "route.learn.set",
                                               "route.remove",
                                               "route.set",
                                               "take.activate",
                                               "take.add",
                                               "take.extract",
                                               "take.hits.set",
                                               "take.remove",
                                               "take.set",
                                               "take.stretch.add",
                                               "take.stretch.move",
                                               "take.stretch.remove",
                                               "tempo.set",
                                               "timesig.remove",
                                               "timesig.set",
                                               "track.add",
                                               "track.duplicate",
                                               "track.freeze.commit",
                                               "track.learn.remove",
                                               "track.learn.set",
                                               "track.move",
                                               "track.remove",
                                               "track.set",
                                               "track.template.load",
                                               "track.unfreeze",
                                               "transport.playrate.apply-to-tempo",
                                               "transport.playrate.set",
                                               "vca.add",
                                               "vca.learn.remove",
                                               "vca.learn.set",
                                               "vca.members.set",
                                               "vca.remove",
                                               "vca.set"};
    cmd::Registry registry;
    registerEngineCommands(registry);
    std::set<std::string> edits;
    for (const cmd::CommandInfo* info : registry.all())
        if (info->kind == cmd::Kind::Edit && info->name != "batch") edits.insert(info->name);
    std::string listed;
    for (const auto& name : edits) listed += "\"" + name + "\", ";
    INFO(listed);
    CHECK(edits == audited);
}
