// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include "../../../tests/file_platform.h"
#include "recording.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/json/json.h"
#include "supple/model/project_file.h"
#include "supple/model/schemas.h"
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

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
        write("autosave-9223372036854775807.json");
        save();
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
        CHECK(app.nextServiceDelayMs() == 60'000);
        bool reported = false;
        for (const auto& [topic, value] : app.takeEvents())
            if (topic == "engine.error" && value.isString() &&
                value.asString().find("autosave.index") != std::string::npos)
                reported = true;
        CHECK(reported);
        auto contents = readTextFile(dir / "autosave-9223372036854775807.json");
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

TEST_CASE("first save checkpoints media before publication and keeps originals on uncertain sync") {
    Project p;
    auto created = EngineApp::create({.workdir = p.root, .scratchDir = p.root / "scratch"});
    REQUIRE(created);
    p.app = std::move(*created);
    p.app->setCopyImportedMedia(true);
    audio::AudioData wave;
    wave.sampleRate = 48000;
    wave.channels = {{.25f, -.25f}};
    const auto external = p.root / "take.wav";
    REQUIRE(audio::writeWav(external, wave));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *p.app);
    auto args = json::Value::object();
    auto track = bus.execute({"track.add", args, {cmd::Origin::Type::Test, "sync", ""}});
    REQUIRE(track);
    args["track"] = track->find("track")->asString();
    args["audio_file"] = external.string();
    REQUIRE(bus.execute({"item.add", args, {cmd::Origin::Type::Test, "sync", ""}}));
    const auto original = p.app->mediaDir() / "take.wav";
    REQUIRE(fs::exists(original));
    const auto project = p.root / "saved" / "project.json";
    fs::create_directory(project.parent_path());
    const auto media = project.parent_path() / "media";
    const auto realMedia = fs::weakly_canonical(media);
    bool failPublished = false;
    SUBCASE("successful checkpoint") {}
    SUBCASE("a failed post-publication checkpoint retains both copies") { failPublished = true; }
    bool data = false, directory = false;
    struct ResetHook {
        ~ResetHook() {
            setFileSyncHookForTesting({});
            setDurabilityHookForTesting({});
        }
    } reset;
    auto checkpoint = [&](const fs::path& path) {
        if (path != original && path.extension() == ".wav" && fs::is_regular_file(path))
            data = true;
        if (path == realMedia) directory = true;
        if (path == project) {
            CHECK(data);
            CHECK(directory);
            CHECK(fs::exists(original));
            if (fs::exists(project) && failPublished) return false;
        }
        return true;
    };
    setFileSyncHookForTesting(checkpoint);
    setDurabilityHookForTesting([&](const fs::path& path, bool) { return checkpoint(path); });
    const auto saved = p.app->save(project);
    if (failPublished) {
        REQUIRE_FALSE(saved);
        CHECK(saved.error().code == "project.sync");
        CHECK(fs::exists(original));
    } else {
        REQUIRE(saved);
        CHECK_FALSE(fs::exists(original));
    }
    CHECK(fs::exists(project));
    REQUIRE(fs::exists(media / "take.wav"));
    CHECK(sha256File(media / "take.wav").value() == sha256File(external).value());
}

TEST_CASE("save as preserves audio referenced only by a saved branch regardless of filename") {
    Project p;
    std::string name;
    SUBCASE("ordinary media") { name = "take.wav"; }
    SUBCASE("a user filename resembling a staging marker") { name = "take.copy-tmp"; }
    fs::create_directories(p.root / "source");
    fs::create_directories(p.root / "first");
    fs::create_directories(p.root / "second");
    const auto original = p.root / "source" / name;
    audio::WavStreamWriter writer;
    REQUIRE(writer.open(original, 48000, 1));
    const std::vector<float> samples(128, .25f);
    REQUIRE(writer.append(samples.data(), samples.size()));
    REQUIRE(writer.close());
    REQUIRE(p.app->save(p.root / "first" / "project.json"));
    p.app->setCopyImportedMedia(true);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *p.app);
    auto call = [&](std::string command, json::Value args) {
        auto result = bus.execute(
            {command, std::move(args), {cmd::Origin::Type::Test, "media-branches", ""}});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    };
    auto args = json::Value::object();
    const auto track = call("track.add", args)["track"].asString();
    args["track"] = track;
    args["audio_file"] = original.string();
    call("item.add", args);
    args = json::Value::object();
    args["name"] = "kept";
    args["switch"] = false;
    call("branch.create", args);
    args = json::Value::object();
    call("history.undo", args);
    REQUIRE(p.app->head().items.empty());
    REQUIRE(p.app->head().media.empty());
    REQUIRE(p.app->save(p.root / "first" / "project.json"));
    REQUIRE(p.app->save(p.root / "second" / "project.json"));
    CHECK(fs::exists(p.root / "first" / "media" / name));
    REQUIRE(fs::exists(p.root / "second" / "media" / name));
    REQUIRE(p.app->open(p.root / "second" / "project.json"));
    REQUIRE(p.app->switchBranch("kept"));
    REQUIRE(p.app->head().items.size() == 1);
    REQUIRE(p.app->head().media.size() == 1);
    CHECK(p.app->head().media.begin()->value->path == name);
    const auto kept = sha256File(p.root / "second" / "media" / name);
    REQUIRE(kept);
    CHECK(*kept == sha256File(original).value());
}

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
    // Written as saves are (review of item A, finding 6): nothing half-done
    // is left beside it.
    for (const auto& e : fs::directory_iterator(p.root))
        CHECK_MESSAGE(e.path().filename().string().find(".tmp") == std::string::npos,
                      e.path().string());
    warnings.clear();
    REQUIRE(p.app->save("song.json", &warnings));  // once: the next save just saves
    CHECK_FALSE(hasWarning(warnings, "project.history_backup"));
    CHECK_FALSE(fs::exists(p.root / "song.json.pre-branches-2.bak"));
}

TEST_CASE("the pre-branches backup is synced and never replaces another backup") {
    // Re-review of item A fixes, finding 1: two apps that opened the same file
    // can choose the same free name; the second must not replace the first's
    // backup. And the backup must be synced before the file is replaced.
    Project p;
    p.diverge();
    REQUIRE(p.app->save("song.json"));
    auto doc = json::parse(*readTextFile(p.root / "song.json")).value();
    doc["project"]["name"] = "Changed elsewhere";
    const std::string stale = json::writeCanonical(doc) + "\n";
    REQUIRE(writeFileAtomic(p.root / "song.json", stale));
    REQUIRE(p.app->open("song.json"));
    const fs::path first = p.root / "song.json.pre-branches.bak";
    const fs::path second = p.root / "song.json.pre-branches-2.bak";

    SUBCASE("a failed sync stops the save, the file untouched") {
        setFileSyncHookForTesting([&](const fs::path& f) { return f != first; });
        std::vector<schema::Warning> warnings;
        auto saved = p.app->save("song.json", &warnings);
        setFileSyncHookForTesting({});
        REQUIRE_FALSE(saved);
        CHECK(saved.error().code == "project.history_backup");
        CHECK(*readTextFile(p.root / "song.json") == stale);
        CHECK_FALSE(fs::exists(first));
    }
    SUBCASE("another app publishes the same name first") {
        std::vector<fs::path> synced;
        setFileSyncHookForTesting([&](const fs::path& f) {
            synced.push_back(f);
            // The other app wins the race for the free name meanwhile.
            if (f == first && !fs::exists(first)) std::ofstream(first) << "the other app's";
            return true;
        });
        std::vector<schema::Warning> warnings;
        auto saved = p.app->save("song.json", &warnings);
        setFileSyncHookForTesting({});
        REQUIRE(saved);
        CHECK(*readTextFile(first) == "the other app's");  // kept
        REQUIRE(fs::exists(second));
        CHECK(*readTextFile(second) == stale);
        REQUIRE(hasWarning(warnings, "project.history_backup"));
        // Synced before the project file itself was replaced.
        auto at = [&](const fs::path& f) { return std::find(synced.begin(), synced.end(), f); };
        REQUIRE(at(second) != synced.end());
        REQUIRE(at(p.root / "song.json") != synced.end());
        CHECK(at(second) < at(p.root / "song.json"));
    }
}

// --- saved logs replay only edits (review of item A, finding 1) -----------------------------

namespace {
std::vector<fs::path> filesUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
         it.increment(ec))
        if (it->is_regular_file()) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

TEST_CASE("a replayed merge writes files only inside the project's own folder") {
    // Re-review of the item A fixes, finding 2: item.apply-fx is an edit, and
    // replayed it rendered into whatever media_dir named, even outside the
    // project; a merge that failed afterwards left the file there.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    const cmd::Origin unit{cmd::Origin::Type::Test, "unit", ""};
    auto run = [&](const char* command, std::string_view args) {
        auto r = d.execute({command, json::parse(args).value(), unit});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    p.app->setupFixture(d, "recording_interrupted");
    run("project.recover", "{}");  // it_1: an audio item on tr_1
    // Its hash is finalized in the background, rewriting the sidecar through
    // a temporary file: wait for it, so no file snapshot below races it.
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
         p.app->head().media.begin()->value->hash.empty() &&
         std::chrono::steady_clock::now() < deadline;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        d.flushJobEvents();
    }
    REQUIRE_FALSE(p.app->head().media.begin()->value->hash.empty());
    REQUIRE(p.app->save("song.json"));
    auto other = run("track.add", R"({"name": "Other"})");
    const std::string otherTrack = other["track"].asString();
    REQUIRE(p.app->versions().createBranch("agent/claude/fx", "main"));
    REQUIRE(p.app->switchBranch("agent/claude/fx"));
    run("item.apply-fx", R"({"item": "it_1"})");
    run("track.set", R"({"track": ")" + otherTrack + R"(", "name": "Renamed"})");
    REQUIRE(p.app->switchBranch("main"));

    SUBCASE("media_dir outside the project: refused before anything is written") {
        const fs::path outside =
            p.root.parent_path() / ("supple-outside-" + std::to_string(test::processId()));
        fs::remove_all(outside);
        auto t = p.app->beginTransaction(unit, "Media elsewhere");
        REQUIRE(t);
        auto project = p.app->txn().project();
        project.mediaDir = outside.string();
        p.app->txn().setProject(project);
        REQUIRE(p.app->commitTransaction(*t));
        run("track.add", R"({"name": "Keys"})");  // main moved: the merge replays
        const auto version = p.app->head().version;
        auto merged = d.execute(
            {"branch.merge", json::parse(R"({"from": "agent/claude/fx"})").value(), unit});
        REQUIRE_FALSE(merged);
        CHECK(merged.error().code == "branch.conflict");
        REQUIRE(merged.error().details.size() == 1);
        CHECK(merged.error().details[0].message.find("outside the project folder") !=
              std::string::npos);
        CHECK(p.app->head().version == version);
        CHECK(filesUnder(outside).empty());
        CHECK_FALSE(fs::exists(outside));
        fs::remove_all(outside);
        // Round-3 review, finding 2: the refusal left the replay's
        // transaction installed, so the next edit failed (history.nested).
        run("track.add", R"({"name": "After"})");
    }
    SUBCASE("nothing is made outside the project, not even for a moment") {
        // Co-director, after round 3: the folder was made before it was
        // checked (and removed after). A read-only outside parent shows it:
        // making a folder there would fail first, with another error.
        const fs::path outside =
            p.root.parent_path() / ("supple-outside-" + std::to_string(test::processId()));
        fs::remove_all(outside);
        fs::create_directories(outside);
        fs::permissions(outside, fs::perms::owner_read | fs::perms::owner_exec);
        auto t = p.app->beginTransaction(unit, "Media in a new folder elsewhere");
        REQUIRE(t);
        auto project = p.app->txn().project();
        project.mediaDir = (outside / "new" / "media").string();
        p.app->txn().setProject(project);
        REQUIRE(p.app->commitTransaction(*t));
        run("track.add", R"({"name": "Keys"})");
        auto merged = d.execute(
            {"branch.merge", json::parse(R"({"from": "agent/claude/fx"})").value(), unit});
        fs::permissions(outside, fs::perms::owner_all);
        REQUIRE_FALSE(merged);
        REQUIRE(merged.error().details.size() == 1);
        CHECK(merged.error().details[0].message.find("is outside the project folder") !=
              std::string::npos);
        CHECK(fs::is_empty(outside));
        run("track.add", R"({"name": "After"})");
        fs::remove_all(outside);
    }
    SUBCASE("through a symlink that leaves the project: refused the same way") {
        const fs::path outside =
            p.root.parent_path() / ("supple-outside-" + std::to_string(test::processId()));
        fs::remove_all(outside);
        fs::create_directories(outside);
        std::error_code ec;
        fs::create_directory_symlink(outside, p.root / "linked", ec);
        if (ec) {
            fs::remove_all(outside);
            return;  // no symlinks here (Windows without the privilege)
        }
        auto t = p.app->beginTransaction(unit, "Media through a link");
        REQUIRE(t);
        auto project = p.app->txn().project();
        project.mediaDir = "linked";
        p.app->txn().setProject(project);
        REQUIRE(p.app->commitTransaction(*t));
        run("track.add", R"({"name": "Keys"})");
        auto merged = d.execute(
            {"branch.merge", json::parse(R"({"from": "agent/claude/fx"})").value(), unit});
        REQUIRE_FALSE(merged);
        CHECK(merged.error().code == "branch.conflict");
        CHECK(filesUnder(outside).empty());
        fs::remove_all(outside);
    }
    SUBCASE("a missing folder and .. cannot hide a link out of the project") {
        // Round-3 review, finding 1: weakly_canonical stops at the missing
        // component and normalizes the rest lexically.
        const fs::path outside =
            p.root.parent_path() / ("supple-outside-" + std::to_string(test::processId()));
        fs::remove_all(outside);
        fs::create_directories(outside);
        std::error_code ec;
        fs::create_directory_symlink(outside, p.root / "link", ec);
        if (ec) {
            fs::remove_all(outside);
            return;
        }
        auto t = p.app->beginTransaction(unit, "Media through a missing folder");
        REQUIRE(t);
        auto project = p.app->txn().project();
        project.mediaDir = "missing/../link/new-media";
        p.app->txn().setProject(project);
        REQUIRE(p.app->commitTransaction(*t));
        run("track.add", R"({"name": "Keys"})");
        auto merged = d.execute(
            {"branch.merge", json::parse(R"({"from": "agent/claude/fx"})").value(), unit});
        REQUIRE_FALSE(merged);
        CHECK(merged.error().code == "branch.conflict");
        CHECK(filesUnder(outside).empty());
        CHECK_FALSE(fs::exists(outside / "new-media"));
        CHECK_FALSE(fs::exists(p.root / "missing"));
        run("track.add", R"({"name": "After"})");
        fs::remove_all(outside);
    }
    SUBCASE("a new media folder inside the project is made, and removed if the merge fails") {
        auto t = p.app->beginTransaction(unit, "Media in a new folder");
        REQUIRE(t);
        auto project = p.app->txn().project();
        project.mediaDir = "new/nested/media";
        p.app->txn().setProject(project);
        REQUIRE(p.app->commitTransaction(*t));
        run("track.remove", R"({"track": ")" + otherTrack + R"("})");  // the later step fails
        auto merged = d.execute(
            {"branch.merge", json::parse(R"({"from": "agent/claude/fx"})").value(), unit});
        REQUIRE_FALSE(merged);
        CHECK_FALSE(fs::exists(p.root / "new"));
    }
    SUBCASE("a merge that fails after a replayed render removes the file it wrote") {
        const auto before = filesUnder(p.root);
        run("track.remove", R"({"track": ")" + otherTrack + R"("})");  // the later step fails
        auto merged = d.execute(
            {"branch.merge", json::parse(R"({"from": "agent/claude/fx"})").value(), unit});
        REQUIRE_FALSE(merged);
        CHECK(merged.error().code == "branch.conflict");
        auto after = filesUnder(p.root);
        after.erase(std::remove_if(after.begin(), after.end(),
                                   [](const fs::path& f) {
                                       return f.filename().string().starts_with("autosave");
                                   }),
                    after.end());
        CHECK(after == before);
    }
}

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
                                               "env.write.commit",
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
                                               "record.commit",
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

TEST_CASE("a recording committed on a branch reaches main through a replayed merge") {
    // Out of scope of item A, found then: a recording's commit logged no
    // step, so a merge that replays the branch dropped the takes.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    const cmd::Origin unit{cmd::Origin::Type::Test, "unit", ""};
    auto run = [&](const char* command, std::string_view args) {
        auto r = d.execute({command, json::parse(args).value(), unit});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    p.app->setupFixture(d, "recording_interrupted");  // tr_1 + rec/Vocal-1.wav, on main
    REQUIRE(p.app->versions().createBranch("agent/claude/take", "main"));
    REQUIRE(p.app->switchBranch("agent/claude/take"));
    run("project.recover", "{}");  // the take is committed as a recording stop commits it
    REQUIRE(p.app->head().items.size() == 1);
    const model::Item branchItem = *p.app->head().items.begin()->value;
    const std::string branchMedia = p.app->head().media.begin()->value->path;
    REQUIRE(p.app->switchBranch("main"));
    run("track.add", R"({"name": "Keys"})");  // main moves on: the merge replays the branch
    SUBCASE("live") {}
    SUBCASE("after saving and reopening: record.commit is a saved step that may replay") {
        REQUIRE(p.app->save("song.json"));
        std::vector<schema::Warning> warnings;
        REQUIRE(p.app->open("song.json", &warnings));
        CHECK_FALSE(hasWarning(warnings, "project.history"));
        REQUIRE(p.app->switchBranch("main"));
    }
    auto merged = run("branch.merge", R"({"from": "agent/claude/take"})");
    CHECK_FALSE(merged["fast_forward"].asBool());
    const model::Session& main = p.app->head();
    REQUIRE(main.items.size() == 1);
    const model::Item& item = *main.items.begin()->value;
    CHECK(item.track == branchItem.track);
    CHECK(item.position == branchItem.position);
    CHECK(item.length == branchItem.length);
    REQUIRE(main.media.size() == 1);
    // The same file, named relative to the media folder once there is one.
    auto resolved = [&](const std::string& path) {
        const fs::path f(path);
        return fs::weakly_canonical(f.is_relative() ? p.app->mediaDir() / f : f);
    };
    CHECK(resolved(main.media.begin()->value->path) == resolved(branchMedia));
}

// --- unsaved projects left behind (ADR-0078) ------------------------------------------------

namespace {

struct Unsaved {
    fs::path root =
        fs::temp_directory_path() / ("supple-unsaved-" + std::to_string(test::processId()));
    fs::path folder(const std::string& name) const { return root / "unsaved" / name; }
    Unsaved() { fs::remove_all(root); }
    ~Unsaved() { fs::remove_all(root); }
    // An app whose unsaved project lives in unsaved/<name>, as the GUI's does.
    std::unique_ptr<EngineApp> run(const std::string& name) const {
        AppOptions o;
        o.workdir = root / "work";
        o.scratchDir = folder(name);
        auto created = EngineApp::create(o);
        REQUIRE(created);
        (*created)->setFixedClock(1'000'000);
        REQUIRE((*created)->newProject({}));  // its autosave interval starts on that clock
        return std::move(*created);
    }
};

void rename(EngineApp& app, const std::string& name) {
    auto transaction = app.beginTransaction({cmd::Origin::Type::Test, "unit", ""}, "Rename");
    REQUIRE(transaction);
    auto project = app.txn().project();
    project.name = name;
    app.txn().setProject(project);
    REQUIRE(app.commitTransaction(*transaction));
}

const cmd::Origin kUnit{cmd::Origin::Type::Test, "unit", ""};

}  // namespace

TEST_CASE("an unsaved project autosaves into its own folder, which the first save empties") {
    Unsaved u;
    auto app = u.run("20261114-090000-1");
    const fs::path own = u.folder("20261114-090000-1");
    CHECK(fs::exists(own / "session.lock"));  // held from the start
    CHECK(app->autosaveDir() == own);
    app->tick();
    CHECK_FALSE(fs::exists(own / "autosave-1.json"));  // nothing changed yet
    rename(*app, "Sketch");
    CHECK(app->nextServiceDelayMs() == 60'000);
    app->setFixedClock(1'060'000);
    app->tick();
    REQUIRE(fs::exists(own / "autosave-1.json"));
    auto opened = model::parseProjectFile(*readTextFile(own / "autosave-1.json"));
    REQUIRE(opened);
    CHECK(opened->store.head()->project->name == "Sketch");

    REQUIRE(app->save(u.root / "song" / "song.json"));
    CHECK_FALSE(fs::exists(own));  // its autosaves and lock went with the first save
    CHECK(app->autosaveDir() == u.root / "song" / "song.json-data");
    REQUIRE(app->newProject({}));  // a new unsaved project takes the folder again
    CHECK(fs::exists(own / "session.lock"));
    app->clearScratch();
    CHECK_FALSE(fs::exists(own));
}

TEST_CASE("an unsaved project a run left behind is listed, recovered and deleted") {
    Unsaved u;
    cmd::Registry registry;
    registerEngineCommands(registry);
    const std::string left = "20261114-090000-4242";
    auto earlier = u.run(left);
    {
        cmd::Dispatcher d(registry, *earlier);
        rename(*earlier, "Sketch");
        REQUIRE(d.execute({"track.add", json::parse(R"({"name": "Vocal"})").value(), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
        REQUIRE(fs::exists(u.folder(left) / "autosave-1.json"));
        // Then a take is recorded and never committed: rec/Vocal-1.wav.
        earlier->setupFixture(d, "recording_take_file");
    }
    auto app = u.run("20261114-100000-1");
    // While the earlier run is alive its folder is its own.
    CHECK(app->recoverableSessions().empty());
    CHECK(app->recoverableSessionCount() == 0);
    CHECK(app->recoverSession(left).error().code == "recover.busy");
    CHECK(app->deleteSession(left).error().code == "recover.busy");

    earlier.reset();  // the run ends without discarding its project, as a crash does
    REQUIRE(fs::exists(u.folder(left)));
    const auto sessions = app->recoverableSessions();
    REQUIRE(sessions.size() == 1);
    const RecoverableSession& s = sessions.front();
    CHECK(s.session == left);
    CHECK(s.name == "Sketch");
    CHECK(s.tracks == 1);
    CHECK(s.branches == std::vector<std::string>{"main"});
    CHECK(s.recordings == 1);
    CHECK(s.bytes > 0);
    CHECK(s.autosavedMs.has_value());
    CHECK(s.startedMs.has_value());
    CHECK(app->recoverableSessionCount() == 1);
    CHECK(app->head().tracks.size() == 1);  // the master: listing loads nothing

    for (const std::string bad : {"", "..", "../work", "20261114-100000-1", "nope"})
        CHECK(app->recoverSession(bad).error().code == "recover.unknown_session");

    auto recovered = app->recoverSession(left);
    REQUIRE_MESSAGE(recovered, (recovered ? std::string() : recovered.error().toString()));
    CHECK(recovered->autosave == "autosave-1.json");
    REQUIRE(recovered->recovered.size() == 1);
    CHECK(recovered->recovered.front().track == "tr_1");
    CHECK(app->projectPath().empty());  // still unsaved
    CHECK(app->head().project->name == "Sketch");
    CHECK(app->head().items.size() == 1);
    CHECK(app->options().scratchDir == u.folder(left));
    CHECK_FALSE(fs::exists(u.folder("20261114-100000-1")));  // this run's own, unused
    REQUIRE(app->undo());  // the recordings came back as their own step
    CHECK(app->head().items.size() == 0);
    CHECK(app->head().tracks.size() == 2);
    REQUIRE(app->redo());

    // The folder is this run's now: another run does not list it, and the
    // autosaves continue there.
    auto other = u.run("20261114-110000-2");
    CHECK(other->recoverableSessions().empty());
    rename(*app, "Sketch 2");
    app->setFixedClock(1'120'000);
    app->tick();
    CHECK(fs::exists(u.folder(left) / "autosave-2.json"));

    app->clearScratch();  // discarded after all
    CHECK_FALSE(fs::exists(u.folder(left)));
}

TEST_CASE("a damaged autosave falls back to an older one; a folder with nothing readable") {
    Unsaved u;
    auto app = u.run("20261114-100000-1");
    model::Project old;
    old.name = "Older";
    const fs::path a = u.folder("20261114-080000-7");
    REQUIRE(writeFileAtomic(a / "autosave-1.json", model::serializeProjectFile(model::VersionStore(
                                                       model::Session::create(old)))));
    REQUIRE(writeFileAtomic(a / "autosave-2.json", "{\"schema\": \"not a project\"}"));
    const fs::path b = u.folder("20261114-085000-8");
    REQUIRE(writeFileAtomic(b / "autosave-1.json", "garbage"));

    std::vector<RecordingWarning> warnings;
    const auto sessions = app->recoverableSessions(&warnings);
    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0].name == "Older");
    CHECK(sessions[1].name.empty());
    CHECK_FALSE(sessions[1].autosavedMs.has_value());
    CHECK(warnings.size() == 2);

    auto damaged = app->recoverSession("20261114-085000-8");
    REQUIRE_FALSE(damaged);
    CHECK(damaged.error().code == "recover.damaged");
    CHECK(fs::exists(b));  // refused: nothing changed
    REQUIRE(app->deleteSession("20261114-085000-8"));
    CHECK_FALSE(fs::exists(b));

    auto r = app->recoverSession("20261114-080000-7");
    REQUIRE(r);
    CHECK(r->autosave == "autosave-1.json");
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings.front().code == "recover.autosave");
    CHECK(app->head().project->name == "Older");
    CHECK(app->recoverableSessions().empty());
}

TEST_CASE("folders left behind with nothing in them go when an app starts, after an hour") {
    Unsaved u;
    const auto now = fs::file_time_type::clock::now();
    const fs::path stale = u.folder("20261114-070000-5"), fresh = u.folder("20261114-075900-6"),
                   kept = u.folder("20261114-071000-9");
    fs::create_directories(stale / "media" / "rec");
    REQUIRE(writeFileAtomic(stale / "session.lock", ""));
    fs::last_write_time(stale, now - std::chrono::hours(2));
    fs::create_directories(fresh);
    REQUIRE(writeFileAtomic(kept / "media" / "rec" / "take.wav", "audio"));
    fs::last_write_time(kept, now - std::chrono::hours(2));
    auto app = u.run("20261114-080000-1");
    CHECK_FALSE(fs::exists(stale));
    CHECK(fs::exists(fresh));  // a run starting now may not hold its lock yet
    CHECK(fs::exists(kept));   // something in it: never deleted unasked
    const auto sessions = app->recoverableSessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().session == "20261114-071000-9");
    CHECK(sessions.front().recordings == 0);  // no sidecar: not a recording
}

TEST_CASE("a recovered autosave's saved steps are checked like an opened file's") {
    // ADR-0076 decision 9 applies to every project file the engine reads:
    // an autosave left behind is someone's file too.
    Unsaved u;
    cmd::Registry registry;
    registerEngineCommands(registry);
    const std::string left = "20261114-090000-77";
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::parse(R"({"name": "Drums"})").value(), kUnit}));
        REQUIRE(earlier->versions().createBranch("agent/claude/bass", "main"));
        REQUIRE(earlier->switchBranch("agent/claude/bass"));
        REQUIRE(d.execute({"track.add", json::parse(R"({"name": "Bass"})").value(), kUnit}));
        REQUIRE(earlier->switchBranch("main"));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
        REQUIRE(fs::exists(u.folder(left) / "autosave-1.json"));
    }
    const fs::path autosave = u.folder(left) / "autosave-1.json";
    const fs::path victim = u.root / "victim.txt";
    REQUIRE(writeFileAtomic(victim, "precious"));
    auto doc = json::parse(*readTextFile(autosave)).value();
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
    REQUIRE(writeFileAtomic(autosave, json::writeCanonical(doc)));

    auto app = u.run("20261114-100000-1");
    const auto sessions = app->recoverableSessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().branches == std::vector<std::string>{"main"});
    auto recovered = app->recoverSession(left);
    REQUIRE_MESSAGE(recovered, (recovered ? std::string() : recovered.error().toString()));
    CHECK(std::any_of(recovered->warnings.begin(), recovered->warnings.end(),
                      [](const RecordingWarning& w) { return w.code == "project.history"; }));
    CHECK(app->versions().branches() == std::vector<std::string>{"main"});
    CHECK(*readTextFile(victim) == "precious");

    // Round-4 review, finding 2: the first save released the folder and
    // deleted the only raw copy of those branches. It is kept beside the
    // saved file, as project.open's first save keeps one.
    const std::string raw = json::writeCanonical(doc);
    std::vector<schema::Warning> saveWarnings;
    REQUIRE(app->save(u.root / "song" / "song.json", &saveWarnings));
    CHECK(hasWarning(saveWarnings, "project.history_backup"));
    const fs::path kept = u.root / "song" / "song.json.pre-branches.bak";
    REQUIRE(fs::exists(kept));
    CHECK(*readTextFile(kept) == raw);
    CHECK_FALSE(fs::exists(u.folder(left)));  // released once the copy is out
}

TEST_CASE("a session with no readable autosave recovers only recordings it can place") {
    // Round-4 review, finding 1: an empty rec/ folder made a session whose
    // autosaves were all damaged recover as a blank project, and the first
    // save then deleted the unreadable autosave.
    Unsaved u;
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");
    const std::string left = "20261114-085000-8";
    const fs::path folder = u.folder(left);
    REQUIRE(writeFileAtomic(folder / "autosave-1.json", "garbage"));

    SUBCASE("an empty recordings folder: refused, nothing touched") {
        fs::create_directories(folder / "media" / "rec");
        auto r = app->recoverSession(left);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.damaged");
        CHECK(app->head().project->name == "Current work");
        CHECK(*readTextFile(folder / "autosave-1.json") == "garbage");
    }
    SUBCASE("a WAV without a usable sidecar: refused the same way") {
        fs::create_directories(folder / "media" / "rec");
        REQUIRE(writeFileAtomic(folder / "media" / "rec" / "Vocal-1.wav", "not audio"));
        REQUIRE(writeFileAtomic(folder / "media" / "rec" / "Vocal-1.rec.json", "{}"));
        auto r = app->recoverSession(left);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.damaged");
        CHECK(app->head().project->name == "Current work");
    }
    SUBCASE("a real recording: placed on a new track; the unreadable autosave kept on save") {
        cmd::Registry registry;
        registerEngineCommands(registry);
        {
            // A run that recorded a take and stopped before any autosave.
            auto earlier = u.run("20261114-084000-5");
            cmd::Dispatcher d(registry, *earlier);
            earlier->setupFixture(d, "recording_take_file");
        }
        fs::rename(u.folder("20261114-084000-5") / "media", folder / "media");
        auto r = app->recoverSession(left);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        REQUIRE(r->recovered.size() == 1);
        CHECK(app->head().items.size() == 1);
        std::vector<schema::Warning> saveWarnings;
        REQUIRE(app->save(u.root / "song" / "song.json", &saveWarnings));
        CHECK(hasWarning(saveWarnings, "project.history_backup"));
        const fs::path kept = u.root / "song" / "song.json.unreadable-autosave.bak";
        REQUIRE(fs::exists(kept));
        CHECK(*readTextFile(kept) == "garbage");
    }
}

TEST_CASE("a saved project's newer autosave is opened only when asked, keeping the file's path") {
    // ADR-0078 decision 6: offered, never loaded unasked; it stays unsaved.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    const std::string saved = *readTextFile(p.root / "song.json");
    auto none = d.execute({"project.recover", json::parse(R"({"autosave": true})").value(), kUnit});
    REQUIRE_FALSE(none);
    CHECK(none.error().code == "recover.no_autosave");

    p.rename("Edited after the save");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    REQUIRE(std::any_of(fs::directory_iterator(p.root / "song.json-data"), fs::directory_iterator(),
                        [](const fs::directory_entry& e) {
                            return e.path().filename().string().starts_with("autosave-");
                        }));
    REQUIRE(p.app->open("song.json"));  // as after a crash: the file as saved
    CHECK(p.app->head().project->name == "Saved");
    auto info = d.execute({"project.get", json::Value::object(), kUnit});
    REQUIRE(info);
    CHECK((*info)["autosave_available"].asBool());

    auto mixed = d.execute(
        {"project.recover", json::parse(R"({"autosave": true, "session": "x"})").value(), kUnit});
    REQUIRE_FALSE(mixed);
    CHECK(mixed.error().code == "recover.autosave_args");
    auto alone = d.execute(
        {"project.recover", json::parse(R"({"autosave_file": "autosave-1.json"})").value(), kUnit});
    REQUIRE_FALSE(alone);
    CHECK(alone.error().code == "recover.autosave_args");
    auto elsewhere = d.execute(
        {"project.recover",
         json::parse(R"({"autosave": true, "autosave_file": "../song.json"})").value(), kUnit});
    REQUIRE_FALSE(elsewhere);
    CHECK(elsewhere.error().code == "recover.no_autosave");

    auto r = d.execute({"project.recover", json::parse(R"({"autosave": true})").value(), kUnit});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK((*r)["autosave"].asString().starts_with("autosave-"));
    CHECK(p.app->head().project->name == "Edited after the save");
    CHECK(p.app->projectPath() == p.root / "song.json");
    CHECK(*readTextFile(p.root / "song.json") == saved);  // untouched until saved
    REQUIRE(p.app->save("song.json"));
    CHECK(
        model::parseProjectFile(*readTextFile(p.root / "song.json"))->store.head()->project->name ==
        "Edited after the save");
}

TEST_CASE("a saved project's recovered autosave keeps what it could not restore through the save") {
    // The round-4 rules for sessions, for a saved project's newer autosave:
    // rejected branches and unreadable autosaves are kept beside the file.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    auto run = [&](const char* command, std::string_view args) {
        auto r = d.execute({command, json::parse(args).value(), kUnit});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    run("track.add", R"({"name": "Drums"})");
    REQUIRE(p.app->versions().createBranch("agent/claude/bass", "main"));
    REQUIRE(p.app->switchBranch("agent/claude/bass"));
    run("track.add", R"({"name": "Bass"})");
    REQUIRE(p.app->switchBranch("main"));
    REQUIRE(p.app->save("song.json"));
    p.rename("After the save");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    const fs::path data = p.root / "song.json-data";
    REQUIRE(fs::exists(data / "autosave-1.json"));
    // Its branch step became one no file may replay: main alone comes back.
    auto doc = json::parse(*readTextFile(data / "autosave-1.json")).value();
    for (auto& v : doc["branches"]["versions"].asObject())
        if (const auto* list = v.value.find("invocations"); list && list->size()) {
            json::Value step = json::Value::object();
            step["command"] = "project.save";
            step["args"] = json::Value::object();
            step["args"]["path"] = (p.root / "victim.txt").string();
            v.value["invocations"] = json::Value::array();
            v.value["invocations"].push_back(std::move(step));
        }
    const std::string crafted = json::writeCanonical(doc);
    REQUIRE(writeFileAtomic(data / "autosave-1.json", crafted));
    REQUIRE(writeFileAtomic(data / "autosave-2.json", "garbage"));  // newer, unreadable
    REQUIRE(p.app->open("song.json"));

    auto r = run("project.recover", R"({"autosave": true})");
    CHECK(r["autosave"].asString() == "autosave-1.json");
    CHECK(p.app->head().project->name == "After the save");
    CHECK(p.app->versions().branches() == std::vector<std::string>{"main"});
    std::vector<schema::Warning> warnings;
    REQUIRE(p.app->save("song.json", &warnings));
    CHECK(hasWarning(warnings, "project.history_backup"));
    REQUIRE(fs::exists(p.root / "song.json.pre-branches.bak"));
    CHECK(*readTextFile(p.root / "song.json.pre-branches.bak") == crafted);
    REQUIRE(fs::exists(p.root / "song.json.unreadable-autosave.bak"));
    CHECK(*readTextFile(p.root / "song.json.unreadable-autosave.bak") == "garbage");
    CHECK_FALSE(fs::exists(p.root / "victim.txt"));
}

namespace {
// A session left behind whose autosave keeps a branch; main's name was then
// changed by hand, so the branches no longer fit and are rejected at
// recovery (kept as .pre-branches). Returns the autosave's bytes.
std::string leaveRejectedBranches(const Unsaved& u, const std::string& left) {
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        REQUIRE(earlier->versions().createBranch("topic", "main"));
        REQUIRE(earlier->switchBranch("topic"));
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        REQUIRE(earlier->switchBranch("main"));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    const fs::path file = u.folder(left) / "autosave-1.json";
    auto doc = json::parse(*readTextFile(file)).value();
    doc["project"]["name"] = "Changed by hand";
    const std::string raw = json::writeCanonical(doc);
    REQUIRE(writeFileAtomic(file, raw));
    return raw;
}

}  // namespace

TEST_CASE("a recovered unsaved session keeps what it writes inside its own folder") {
    // Round-5 review, finding 1: replays skipped the media check for every
    // unsaved project, but a recovered session's state came from a file.
    Unsaved u;
    cmd::Registry registry;
    registerEngineCommands(registry);
    const std::string left = "20261114-090000-31";
    const fs::path outside = u.root / "outside";
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        REQUIRE(d.execute(
            {"item.add",
             json::parse(R"({"track": "tr_1", "notation": "voice lead; 1: C4:q r:*"})").value(),
             kUnit}));
        REQUIRE(earlier->versions().createBranch("topic", "main"));
        REQUIRE(earlier->switchBranch("topic"));
        REQUIRE(d.execute({"item.apply-fx", json::parse(R"({"item": "it_1"})").value(), kUnit}));
        REQUIRE(earlier->switchBranch("main"));
        // No command sets media_dir: only a file not written by this program
        // names a folder outside the session.
        auto t = earlier->beginTransaction(kUnit, "Media elsewhere");
        REQUIRE(t);
        auto project = earlier->txn().project();
        project.mediaDir = outside.string();
        earlier->txn().setProject(project);
        REQUIRE(earlier->commitTransaction(*t));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    auto app = u.run("20261114-100000-1");
    cmd::Dispatcher d(registry, *app);
    auto recovered = app->recoverSession(left);
    REQUIRE_MESSAGE(recovered, (recovered ? std::string() : recovered.error().toString()));
    REQUIRE(app->versions().branches().size() == 2);
    CHECK(app->projectPath().empty());

    SUBCASE("the file's outside media folder is not used") {
        CHECK(std::any_of(recovered->warnings.begin(), recovered->warnings.end(),
                          [](const RecordingWarning& w) { return w.code == "recover.media_dir"; }));
        CHECK(app->head().project->mediaDir == "media");
        auto merged =
            d.execute({"branch.merge", json::parse(R"({"from": "topic"})").value(), kUnit});
        REQUIRE_MESSAGE(merged, (merged ? std::string() : merged.error().toString()));
        CHECK_FALSE(fs::exists(outside));
        CHECK(std::any_of(
            fs::directory_iterator(u.folder(left) / "media"), {},
            [](const fs::directory_entry& e) { return e.path().extension() == ".wav"; }));
    }
    SUBCASE("a replay is held to the session folder") {
        auto t = app->beginTransaction(kUnit, "Media elsewhere again");
        REQUIRE(t);
        auto project = app->txn().project();
        project.mediaDir = outside.string();
        app->txn().setProject(project);
        REQUIRE(app->commitTransaction(*t));
        auto merged =
            d.execute({"branch.merge", json::parse(R"({"from": "topic"})").value(), kUnit});
        REQUIRE_FALSE(merged);
        CHECK(merged.error().code == "branch.conflict");
        REQUIRE(merged.error().details.size() == 1);
        CHECK(merged.error().details[0].message.find("outside the project folder") !=
              std::string::npos);
        CHECK_FALSE(fs::exists(outside));
    }
}

TEST_CASE("a recovery whose preservation copy fails changes nothing") {
    // Round-5 review, finding 2: a failed .keep copy was only a warning, and
    // the next save deleted the only source of the rejected branches.
    Unsaved u;
    const std::string left = "20261114-090000-32";
    const std::string raw = leaveRejectedBranches(u, left);
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");
    setFileSyncHookForTesting([](const fs::path& f) { return f.extension() != ".keep"; });
    auto failed = app->recoverSession(left);
    setFileSyncHookForTesting({});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "recover.keep");
    CHECK(app->head().project->name == "Current work");
    CHECK(*readTextFile(u.folder(left) / "autosave-1.json") == raw);
    for (const fs::path& f : filesUnder(u.root)) CHECK(f.extension() != ".keep");
    CHECK(app->recoverableSessions().size() == 1);  // still there, still left behind

    auto recovered = app->recoverSession(left);  // once the copy can be made
    REQUIRE_MESSAGE(recovered, (recovered ? std::string() : recovered.error().toString()));
    REQUIRE(app->save(u.root / "song" / "song.json"));
    CHECK(*readTextFile(u.root / "song" / "song.json.pre-branches.bak") == raw);
}

TEST_CASE("an autosave written after a save is newer than the file whatever the file clock") {
    // Native CI on Windows: a save and the autosave after it in one tick of
    // the file clock had the same time, so the autosave was never offered. A
    // coarse clock (FAT's two seconds) or one set back does the same; here
    // the file's time is ahead of the clock the autosave is written by.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    const fs::path file = p.root / "song.json";
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(30));
    p.rename("Edited after the save");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    REQUIRE(fs::exists(p.root / "song.json-data" / "autosave-1.json"));
    REQUIRE(p.app->open("song.json"));
    auto info = d.execute({"project.get", json::Value::object(), kUnit});
    REQUIRE(info);
    CHECK((*info)["autosave_available"].asBool());
    auto r = d.execute({"project.recover", json::parse(R"({"autosave": true})").value(), kUnit});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(p.app->head().project->name == "Edited after the save");
}

TEST_CASE("the newest autosave is the highest numbered, whatever the file clock") {
    // Round-12 review, finding 1: dated after a future-dated file, two
    // autosaves had one time, and the offer named the older one.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    const fs::path file = p.root / "song.json";
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::hours(2));
    p.rename("First autosave");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    p.rename("Second autosave");
    p.app->setFixedClock(1'120'000);
    p.app->tick();
    REQUIRE(fs::exists(p.root / "song.json-data" / "autosave-2.json"));
    // As a clock set back between them leaves them: both after the file,
    // the older edits dated later.
    fs::last_write_time(p.root / "song.json-data" / "autosave-1.json",
                        fs::last_write_time(file) + std::chrono::hours(1));
    fs::last_write_time(p.root / "song.json-data" / "autosave-2.json",
                        fs::last_write_time(file) + std::chrono::minutes(30));
    REQUIRE(p.app->open("song.json"));
    CHECK(p.app->newerAutosave(p.app->projectPath()).filename() == "autosave-2.json");
    SUBCASE("the offered one, by name") {
        const std::string offered = p.app->newerAutosave(p.app->projectPath()).filename().string();
        auto r = p.app->recoverAutosave(offered);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(p.app->head().project->name == "Second autosave");
    }
    SUBCASE("the newest, unnamed") {
        auto r =
            d.execute({"project.recover", json::parse(R"({"autosave": true})").value(), kUnit});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(p.app->head().project->name == "Second autosave");
    }
}

TEST_CASE("a save holds what the autosaves before it hold: none of them is offered after it") {
    // Round-12 review, finding 2: an autosave dated after a future-dated
    // file stayed newer than a later save of newer edits, and recovering it
    // replaced them.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    const fs::path file = p.root / "song.json";
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::hours(2));
    p.rename("Autosaved");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    REQUIRE(fs::exists(p.root / "song.json-data" / "autosave-1.json"));
    fs::last_write_time(p.root / "song.json-data" / "autosave-1.json",
                        fs::last_write_time(file) + std::chrono::hours(1));
    p.rename("Saved later");
    REQUIRE(p.app->save("song.json"));
    REQUIRE(p.app->open("song.json"));
    auto info = d.execute({"project.get", json::Value::object(), kUnit});
    REQUIRE(info);
    CHECK_FALSE((*info)["autosave_available"].asBool());
    auto r = d.execute({"project.recover", json::parse(R"({"autosave": true})").value(), kUnit});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "recover.no_autosave");
    auto named = d.execute(
        {"project.recover",
         json::parse(R"({"autosave": true, "autosave_file": "autosave-1.json"})").value(), kUnit});
    REQUIRE_FALSE(named);
    CHECK(named.error().code == "recover.no_autosave");
    CHECK(p.app->head().project->name == "Saved later");
    // The next autosave numbers on past what the file holds, and is offered.
    p.rename("Autosaved after");
    p.app->setFixedClock(1'200'000);
    p.app->tick();
    REQUIRE(p.app->open("song.json"));
    CHECK(p.app->newerAutosave(p.app->projectPath()).filename() == "autosave-2.json");
}

TEST_CASE("a file saved before autosave numbers were kept in it still goes by file time") {
    // A 2.14 file has no autosaved_through: its autosaves are compared by
    // write time, as before, the highest numbered of the newer ones first.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    const fs::path file = p.root / "song.json";
    auto doc = json::parse(*readTextFile(file)).value();
    doc.asObject().erase("autosaved_through");
    doc["schema"] = "supple.project/2.14";
    REQUIRE(writeFileAtomic(file, json::writeCanonical(doc) + "\n"));
    const fs::path data = p.root / "song.json-data";
    const auto saved = fs::last_write_time(file);
    REQUIRE(p.app->open("song.json"));
    p.rename("Older");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    REQUIRE(fs::exists(data / "autosave-1.json"));
    fs::last_write_time(data / "autosave-1.json", saved - std::chrono::seconds(10));
    REQUIRE(p.app->open("song.json"));
    CHECK(p.app->newerAutosave(p.app->projectPath()).empty());
    fs::last_write_time(data / "autosave-1.json", saved + std::chrono::seconds(10));
    CHECK(p.app->newerAutosave(p.app->projectPath()).filename() == "autosave-1.json");
}

TEST_CASE("a saved project's autosave recovery whose preservation copy fails changes nothing") {
    // Round-5 review, finding 2, for project.recover {autosave: true}.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    p.rename("After the save");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    const fs::path data = p.root / "song.json-data";
    REQUIRE(writeFileAtomic(data / "autosave-9.json", "garbage"));  // newer, unreadable
    REQUIRE(p.app->open("song.json"));
    setFileSyncHookForTesting([](const fs::path& f) { return f.extension() != ".keep"; });
    auto failed =
        d.execute({"project.recover", json::parse(R"({"autosave": true})").value(), kUnit});
    setFileSyncHookForTesting({});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "recover.keep");
    CHECK(p.app->head().project->name == "Saved");
    CHECK(*readTextFile(data / "autosave-9.json") == "garbage");
    for (const fs::path& f : filesUnder(p.root)) CHECK(f.extension() != ".keep");
}

TEST_CASE("a recording placement would refuse cannot justify replacing current work") {
    // Round-5 review, finding 3: the preflight counted a recording whose
    // placement is outside the timeline, so recovery replaced current work
    // with an empty "Recovered" channel.
    Unsaved u;
    cmd::Registry registry;
    registerEngineCommands(registry);
    const std::string left = "20261114-090000-33";
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        earlier->setupFixture(d, "recording_take_file");
    }
    REQUIRE(writeFileAtomic(u.folder(left) / "autosave-1.json", "garbage"));
    fs::path wav;
    for (const fs::path& f : filesUnder(u.folder(left)))
        if (f.extension() == ".wav") wav = f;
    REQUIRE_FALSE(wav.empty());
    auto side = engine::detail::readSidecar(wav);
    REQUIRE(side);
    side->startSample = INT64_MAX;
    side->segments.clear();
    REQUIRE(engine::detail::writeSidecar(wav, *side));
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");
    auto r = app->recoverSession(left);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "recover.damaged");
    CHECK(app->head().project->name == "Current work");
    CHECK(*readTextFile(u.folder(left) / "autosave-1.json") == "garbage");
}

namespace {
// Every file under `root`, relative, with its digest.
std::map<fs::path, std::string> digests(const fs::path& root) {
    std::map<fs::path, std::string> out;
    for (const fs::path& f : filesUnder(root))
        out[f.lexically_relative(root)] = sha256File(f).value();
    return out;
}

// A recording a crash left in `rec` (sidecar first, as the recorder writes
// them), one second long, on tr_1 at the timeline's second second.
void leaveRecording(const fs::path& wav) {
    fs::create_directories(wav.parent_path());
    audio::WavStreamWriter writer;
    std::vector<float> block(96000, .2f);
    REQUIRE(engine::detail::writeSidecar(wav, {{model::Kind::Track, 1}, 48000, 48000, {}, 0, 0}));
    REQUIRE(writer.open(wav, 48000, 2));
    REQUIRE(writer.append(block.data(), 48000));
    REQUIRE(writer.close());
}
}  // namespace

TEST_CASE("recovery never reads or repairs recordings through a link out of the session") {
    // Round-6 review, finding 1: <session>/media was itself a link out of
    // the session; resetting media_dir to "media" did not help, and the
    // recordings there were recovered and their headers rewritten.
    Unsaved u;
    const std::string left = "20261114-090000-41";
    const fs::path outside = u.root / "outside-session";
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    leaveRecording(outside / "rec" / "take.wav");
    fs::remove_all(u.folder(left) / "media");
    fs::create_directory_symlink(outside, u.folder(left) / "media");
    const auto before = digests(outside);
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");
    const auto sessions = app->recoverableSessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().recordings == 0);

    SUBCASE("with a readable autosave: the project comes back, the recordings do not") {
        auto recovered = app->recoverSession(left);
        REQUIRE_MESSAGE(recovered, (recovered ? std::string() : recovered.error().toString()));
        CHECK(recovered->recovered.empty());
        CHECK(std::any_of(
            recovered->warnings.begin(), recovered->warnings.end(),
            [](const RecordingWarning& w) { return w.code == "recover.media_outside"; }));
        CHECK(app->head().items.size() == 0);
        CHECK(app->recoverable().empty());
        CHECK_FALSE(app->recover({}, {}));
    }
    SUBCASE("without one: nothing to put back, nothing changed") {
        REQUIRE(writeFileAtomic(u.folder(left) / "autosave-1.json", "garbage"));
        auto r = app->recoverSession(left);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.damaged");
        CHECK(app->head().project->name == "Current work");
    }
    CHECK(digests(outside) == before);
}

TEST_CASE("recovery keeps a media folder that resolves inside the session, whatever its spelling") {
    // Round-6 review, finding 3: any ".." in media_dir reset it, though the
    // folder it names is in the session, and its recordings went missing.
    Unsaved u;
    const std::string left = "20261114-090000-42";
    const std::string setting = "intermediate/../audio";
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        auto t = earlier->beginTransaction(kUnit, "Media folder");
        REQUIRE(t);
        auto project = earlier->txn().project();
        project.mediaDir = setting;
        earlier->txn().setProject(project);
        REQUIRE(earlier->commitTransaction(*t));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    fs::create_directories(u.folder(left) / "intermediate");
    leaveRecording(u.folder(left) / "audio" / "rec" / "take.wav");
    auto app = u.run("20261114-100000-1");
    auto recovered = app->recoverSession(left);
    REQUIRE_MESSAGE(recovered, (recovered ? std::string() : recovered.error().toString()));
    CHECK(app->head().project->mediaDir == setting);
    CHECK(std::none_of(recovered->warnings.begin(), recovered->warnings.end(),
                       [](const RecordingWarning& w) { return w.code == "recover.media_dir"; }));
    CHECK(recovered->recovered.size() == 1);
    CHECK(app->head().items.size() == 1);
}

TEST_CASE("a named autosave must be newer than the file as the project was opened") {
    // Round-6 review, finding 2: a name offered for one project opened
    // another project's older autosave of the same name.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    p.rename("Saved");
    REQUIRE(p.app->save("song.json"));
    const fs::path data = p.root / "song.json-data";
    p.rename("Autosaved");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    REQUIRE(fs::exists(data / "autosave-1.json"));
    auto args = json::parse(R"({"autosave": true, "autosave_file": "autosave-1.json"})").value();

    SUBCASE("older than the file as opened: refused") {
        p.rename("Saved after it");
        REQUIRE(p.app->save("song.json"));  // holds autosave-1's work and more
        REQUIRE(p.app->open("song.json"));
        auto r = d.execute({"project.recover", args, kUnit});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.no_autosave");
        CHECK(p.app->head().project->name == "Saved after it");
    }
    SUBCASE("newer than the file as opened, though saved over since: opened") {
        REQUIRE(p.app->open("song.json"));
        p.rename("Saved over");
        REQUIRE(p.app->save("song.json"));  // the question before opening it offers Save
        auto r = d.execute({"project.recover", args, kUnit});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(p.app->head().project->name == "Autosaved");
    }
}

TEST_CASE("recovery opens no file through a link out of the session") {
    // Round-8 review, finding 2: the WAV was confined but its sidecar was
    // read through a link to placement metadata outside the session.
    Unsaved u;
    const std::string left = "20261114-090000-43";
    const fs::path folder = u.folder(left);
    const fs::path wav = folder / "media" / "rec" / "take.wav";
    const fs::path elsewhere = u.root / "outside-sidecar" / "other.wav";
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    leaveRecording(wav);
    fs::create_directories(elsewhere.parent_path());
    REQUIRE(engine::detail::writeSidecar(elsewhere,
                                         {{model::Kind::Track, 1}, 480000, 48000, {}, 0, 0}));
    fs::remove(engine::detail::sidecarPath(wav));
    fs::create_symlink(engine::detail::sidecarPath(elsewhere), engine::detail::sidecarPath(wav));
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");

    SUBCASE("a sidecar link: the take is not placed from it") {
        CHECK(app->recoverableSessions().front().recordings == 0);
        auto r = app->recoverSession(left);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(r->recovered.empty());
        CHECK(app->head().items.size() == 0);
    }
    SUBCASE("a sidecar link and no readable autosave: nothing to put back, nothing changed") {
        REQUIRE(writeFileAtomic(folder / "autosave-1.json", "damaged"));
        auto r = app->recoverSession(left);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.damaged");
        CHECK(app->head().project->name == "Current work");
    }
}

TEST_CASE("recovery reads no autosave and opens no lock through a link out of the session") {
    // Round-8 review, finding 2, for the other files recovery opens.
    Unsaved u;
    const std::string left = "20261114-090000-44";
    const fs::path folder = u.folder(left);
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");
    SUBCASE("an autosave link") {
        const fs::path outsideAutosave = u.root / "outside-autosave.json";
        fs::rename(folder / "autosave-1.json", outsideAutosave);
        fs::create_symlink(outsideAutosave, folder / "autosave-1.json");
        auto r = app->recoverSession(left);
        CHECK_FALSE(r);
        CHECK(app->head().project->name == "Current work");
    }
    SUBCASE("a session.lock link") {
        fs::remove(folder / "session.lock");
        const fs::path target = u.root / "outside-lock";
        fs::create_symlink(target, folder / "session.lock");
        CHECK(app->recoverableSessions().empty());
        CHECK_FALSE(fs::exists(target));
    }
#if !defined(_WIN32)
    SUBCASE("a session.lock named pipe") {
        // Round-10 native CI (Windows took the link): the lock is a plain file.
        fs::remove(folder / "session.lock");
        REQUIRE(::mkfifo((folder / "session.lock").c_str(), 0600) == 0);
        CHECK(app->recoverableSessions().empty());
        CHECK(fs::is_fifo(fs::symlink_status(folder / "session.lock")));
    }
#endif
}

TEST_CASE("an autosave offered after a first save or a save as survives saving first") {
    // Round-8 review, finding 1: only opening a project recorded the file the
    // offer is made against; after a first save or a save as, Save in the
    // question made the offered autosave look older than the file.
    bool saveAs = false;
    SUBCASE("first saved this run") {}
    SUBCASE("saved as another file this run") { saveAs = true; }
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    const fs::path file = p.root / "song.json";
    const auto t0 = fs::file_time_type::clock::now() - std::chrono::hours(2);
    if (saveAs) {
        REQUIRE(p.app->save("previous.json"));
        REQUIRE(p.app->open("previous.json"));
    }
    p.rename("Saved");
    // The save publishes a file dated two hours ago, as a coarse clock might.
    setFileSyncHookForTesting([&](const fs::path& f) {
        for (const auto& e : fs::directory_iterator(f.parent_path()))
            if (e.path().filename().string().starts_with(f.filename().string() + "."))
                fs::last_write_time(e.path(), t0);
        return true;
    });
    auto saved = p.app->save(file);
    setFileSyncHookForTesting({});
    REQUIRE(saved);
    p.rename("Autosaved");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    const fs::path offered = p.app->newerAutosave(file);
    REQUIRE_FALSE(offered.empty());
    fs::last_write_time(offered, t0 + std::chrono::hours(1));
    p.rename("Current work");
    REQUIRE(p.app->save(file));  // Save in the question
    auto args = json::Value::object();
    args["autosave"] = true;
    args["autosave_file"] = offered.filename().string();
    auto r = d.execute({"project.recover", args, kUnit});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(p.app->head().project->name == "Autosaved");
}

TEST_CASE(
    "an unsaved session's MIDI journals come back on their track, or on the Recovered channel") {
    // ADR-0080 with ADR-0078: a session a crash left behind holds the
    // journal of a MIDI take that was never committed.
    Unsaved u;
    const std::string left = "20261114-090000-51";
    const fs::path folder = u.folder(left);
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::parse(R"({"name": "Keys"})").value(), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    // One note at 1 s, half a second long, on tr_1; the last line torn.
    const std::string header =
        R"({"schema":"supple.midi-recording/1.0","track":"tr_1","sample_rate":48000,)"
        R"("start_sample":48000,"latency":0,"device":true,"routed":false,"shift":0,)"
        R"("loop":[0,0]})"
        "\n";
    auto journal = [&](const std::string& body) {
        fs::create_directories(folder / "media" / "rec");
        REQUIRE(writeFileAtomic(folder / "media" / "rec" / "Keys-1.midi.jsonl", header + body));
    };
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");

    SUBCASE("with a readable autosave: on the track it was recorded on") {
        journal("[0,144,60,100]\n[24000,128,60,0]\n{\"captured\":48000}\n[30000,144");
        CHECK(app->recoverableSessions().front().recordings == 1);
        auto r = app->recoverSession(left);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        REQUIRE(r->recovered.size() == 1);
        CHECK(r->recovered.front().track == "tr_1");
        REQUIRE(app->head().midi.size() == 1);
        const auto& seq = app->head().midi.begin()->value->sequence;
        REQUIRE(seq.notes.size() == 1);
        CHECK(seq.notes[0].pitch == 60);
        CHECK(app->recoverable().empty());
        REQUIRE(app->save(u.root / "song" / "song.json"));
        CHECK_FALSE(fs::exists(folder));  // the journal went with the first save
    }
    SUBCASE("without one: on the Recovered channel") {
        REQUIRE(writeFileAtomic(folder / "autosave-1.json", "garbage"));
        journal("[0,144,60,100]\n[24000,128,60,0]\n{\"captured\":48000}\n");
        auto r = app->recoverSession(left);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        REQUIRE(r->recovered.size() == 1);
        const auto* track = app->head().tracks.find(*model::Id::parse(r->recovered.front().track));
        REQUIRE(track);
        CHECK(track->name == "Recovered");
        CHECK(app->head().midi.size() == 1);
    }
    SUBCASE("a journal with nothing played and no autosave: nothing to put back") {
        REQUIRE(writeFileAtomic(folder / "autosave-1.json", "garbage"));
        journal("{\"captured\":48000}\n");
        CHECK(app->recoverableSessions().front().recordings == 0);
        auto r = app->recoverSession(left);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.damaged");
        CHECK(app->head().project->name == "Current work");
    }
}

#if !defined(_WIN32)
TEST_CASE("recovery waits on no named pipe in a session a crash left behind") {
    // Round-9 review: a WAV's sidecar that was a named pipe resolved inside
    // the session, and recovery waited in open() for a writer. A pipe in
    // the place of any file recovery reads is passed over; it stays.
    Unsaved u;
    const std::string left = "20261114-090000-61";
    const fs::path folder = u.folder(left);
    const fs::path rec = folder / "media" / "rec";
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run(left);
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    REQUIRE(fs::exists(folder / "autosave-1.json"));
    auto app = u.run("20261114-100000-1");
    rename(*app, "Current work");
    fs::path pipe;
    SUBCASE("a sidecar") {
        leaveRecording(rec / "take.wav");
        pipe = engine::detail::sidecarPath(rec / "take.wav");
        fs::remove(pipe);
    }
    SUBCASE("a recording's WAV") {
        leaveRecording(rec / "take.wav");
        pipe = rec / "pipe.wav";
        REQUIRE(engine::detail::writeSidecar(pipe, {{model::Kind::Track, 1}, 0, 48000, {}, 0, 0}));
    }
    SUBCASE("a MIDI journal") {
        fs::create_directories(rec);
        pipe = rec / "Keys-1.midi.jsonl";
    }
    SUBCASE("a newer autosave") { pipe = folder / "autosave-2.json"; }
    REQUIRE(::mkfifo(pipe.c_str(), 0600) == 0);
    std::int64_t listed = -1;
    CHECK_FALSE(
        test::waitsOnFifo(pipe, [&] { listed = app->recoverableSessions().front().recordings; }));
    // Only the real take beside the pipe, and none whose sidecar is the pipe.
    const std::int64_t real = fs::is_regular_file(engine::detail::sidecarPath(rec / "take.wav"));
    CHECK(listed == real);
    std::optional<Result<SessionRecovery>> r;
    CHECK_FALSE(test::waitsOnFifo(pipe, [&] { r = app->recoverSession(left); }));
    REQUIRE(r);
    REQUIRE_MESSAGE(*r, (*r ? std::string() : r->error().toString()));
    CHECK(static_cast<std::int64_t>((*r)->recovered.size()) == real);
    for (const auto& took : (*r)->recovered)
        CHECK_FALSE(took.file.ends_with(pipe.filename().string()));
    CHECK(fs::is_fifo(fs::symlink_status(pipe)));
}
#endif

namespace {
std::string journalHeader() {
    return R"({"schema":"supple.midi-recording/1.0","track":"tr_1","sample_rate":48000,)"
           R"("start_sample":48000,"latency":0,"device":true,"routed":false,"shift":0,)"
           R"("loop":[0,0]})"
           "\n";
}
}  // namespace

TEST_CASE("a MIDI journal's 8-bit data puts nothing back, replaces nothing, saves nothing") {
    // Round-10 review, finding 1: pitch 255 was recovered, saved (the journal
    // then deleted) and refused by the schema on reopen.
    cmd::Registry registry;
    registerEngineCommands(registry);
    SUBCASE("an unsaved session with no readable autosave: current work stays") {
        Unsaved u;
        {
            auto earlier = u.run("earlier");
            cmd::Dispatcher d(registry, *earlier);
            REQUIRE(d.execute({"track.add", *json::parse(R"({"name":"Keys"})"), kUnit}));
            earlier->setFixedClock(1'060'000);
            earlier->tick();
        }
        REQUIRE(writeFileAtomic(
            u.folder("earlier") / "media/rec/Keys-1.midi.jsonl",
            journalHeader() + "[0,144,255,100]\n[24000,128,255,0]\n{\"captured\":48000}\n"));
        REQUIRE(writeFileAtomic(u.folder("earlier") / "autosave-1.json", "damaged"));
        auto app = u.run("current");
        rename(*app, "Current work");
        auto r = app->recoverSession("earlier");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "recover.damaged");
        CHECK(app->head().project->name == "Current work");
    }
    SUBCASE("a saved project: what is put back saves and reopens") {
        Project p;
        cmd::Dispatcher d(registry, *p.app);
        REQUIRE(d.execute({"track.add", *json::parse(R"({"name":"Keys"})"), kUnit}));
        const fs::path saved = p.root / "saved/song.json";
        REQUIRE(p.app->save(saved));
        const fs::path journal = p.app->recordDir() / "Keys-1.midi.jsonl";
        // A good note, then a damaged one: the good one only.
        REQUIRE(writeFileAtomic(journal, journalHeader() + "[0,144,60,100]\n[100,128,60,0]\n"
                                                           "{\"captured\":24000}\n"
                                                           "[30000,144,255,100]\n"
                                                           "{\"captured\":48000}\n"));
        auto r = p.app->recover({}, {});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        REQUIRE(p.app->head().midi.size() == 1);
        const auto& notes = p.app->head().midi.begin()->value->sequence.notes;
        REQUIRE(notes.size() == 1);
        CHECK(notes.front().pitch == 60);
        REQUIRE(p.app->save(saved));
        auto opened = p.app->open(saved);
        CHECK_MESSAGE(opened, (opened ? std::string() : opened.error().toString()));
    }
}

TEST_CASE("record.commit refuses MIDI that is not a channel message of 7-bit data") {
    // A replayed record.commit is read from a project's history: the same
    // file boundary as the journal (round-10 review, finding 1).
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
    auto commit = [&](std::string_view bytes) {
        auto args = json::parse(
            std::string(R"({"takes":[{"track":"tr_1","file":"","hash":"","start_sample":0,)") +
            R"("frames":48000,"sample_rate":48000,"midi":true,"loop_start":0,"loop_end":0,)" +
            R"("overflow_frames":0,"segments":[{"file_frame":0,"timeline":0}],)" +
            R"("events":[{"timeline":0,"run":0,"voice":-1,"bytes":)" + std::string(bytes) +
            R"(}]}],"punch":{"enabled":false,"start":0,"end":0}})");
        REQUIRE(args);
        return d.execute({"record.commit", *args, kUnit});
    };
    for (const char* bytes : {"[144,255,100]", "[144,60,128]", "[248,0,0]", "[112,60,0]",
                              "[144,60]", "[144,60,100,0]"}) {
        INFO(bytes);
        auto r = commit(bytes);
        REQUIRE_FALSE(r);
        INFO(r.error().toString());
        CHECK(r.error().code == "record.midi");
        CHECK(r.error().path == "/args/takes/0/events/0/bytes");
        CHECK(p.app->head().midi.empty());
    }
    auto good = commit("[144,60,100]");  // the same arguments, a note-on
    REQUIRE_MESSAGE(good, (good ? std::string() : good.error().toString()));
    CHECK(p.app->head().midi.size() == 1);
}

TEST_CASE("record.commit refuses clocks and runs no recording can have") {
    // As the journal reader (round 10, finding 2): every clock within 2^48
    // frames, so placement's sums stay inside int64, and a run that fits the
    // 32 bits a recorded event keeps it in, never truncated into another.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
    struct Field {
        std::string key, value;
    };
    auto commit = [&](const Field& bad) {
        std::map<std::string, std::string> f = {{"start_sample", "0"},    {"frames", "48000"},
                                                {"loop_start", "0"},      {"loop_end", "0"},
                                                {"overflow_frames", "0"}, {"sample_rate", "48000"},
                                                {"file_frame", "0"},      {"segment_timeline", "0"},
                                                {"timeline", "0"},        {"run", "0"}};
        f[bad.key] = bad.value;
        auto args = json::parse(
            R"({"takes":[{"track":"tr_1","file":"","hash":"","start_sample":)" + f["start_sample"] +
            R"(,"frames":)" + f["frames"] + R"(,"sample_rate":)" + f["sample_rate"] +
            R"(,"midi":true,"loop_start":)" + f["loop_start"] + R"(,"loop_end":)" + f["loop_end"] +
            R"(,"overflow_frames":)" + f["overflow_frames"] + R"(,"segments":[{"file_frame":)" +
            f["file_frame"] + R"(,"timeline":)" + f["segment_timeline"] +
            R"(}],"events":[{"timeline":)" + f["timeline"] + R"(,"run":)" + f["run"] +
            R"(,"voice":-1,"bytes":[144,60,100]}]}],"punch":{"enabled":false,"start":0,"end":0}})");
        REQUIRE(args);
        return d.execute({"record.commit", *args, kUnit});
    };
    const std::string far = "4611686018427387904", farBack = "-4611686018427387904";  // 2^62
    const std::string past = "281474976710657";                                       // 2^48 + 1
    const std::vector<std::pair<Field, std::string>> cases = {
        {{"start_sample", far}, "/args/takes/0/start_sample"},
        {{"start_sample", farBack}, "/args/takes/0/start_sample"},
        {{"frames", past}, "/args/takes/0/frames"},
        {{"loop_start", far}, "/args/takes/0/loop_start"},
        {{"loop_end", far}, "/args/takes/0/loop_end"},
        {{"loop_start", "-1"}, "/args/takes/0/loop_start"},
        {{"overflow_frames", far}, "/args/takes/0/overflow_frames"},
        {{"sample_rate", "0"}, "/args/takes/0/sample_rate"},
        {{"file_frame", far}, "/args/takes/0/segments/0/file_frame"},
        {{"segment_timeline", farBack}, "/args/takes/0/segments/0/timeline"},
        {{"timeline", far}, "/args/takes/0/events/0/timeline"},
        {{"timeline", farBack}, "/args/takes/0/events/0/timeline"},
        {{"run", "4294967296"}, "/args/takes/0/events/0/run"},  // 2^32 was run 0
    };
    for (const auto& [bad, path] : cases) {
        INFO(bad.key << " = " << bad.value);
        auto r = commit(bad);
        REQUIRE_FALSE(r);
        INFO(r.error().toString());
        CHECK(r.error().code == "command.invalid_args");
        REQUIRE(r.error().details.size() == 1);
        const Error& field = r.error().details[0];
        CHECK(field.code == "schema.out_of_range");
        CHECK(field.path == path);
        CHECK(field.hint.starts_with("valid range: "));
        CHECK(p.app->head().midi.empty());
    }
    auto good = commit({"timeline", "281474976710656"});  // 2^48 itself: the bound holds
    REQUIRE_MESSAGE(good, (good ? std::string() : good.error().toString()));
    CHECK(p.app->head().midi.size() == 1);
}

TEST_CASE("record.commit refuses a punch or a loop no recording can have") {
    // Round-12 review, finding 3: a finite punch end too large for a frame
    // count made the clip empty, and the call succeeded without the take. A
    // loop that ends before it starts is refused as in a journal's header.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
    struct Args {
        std::string punch = R"({"enabled":true,"start":0,"end":1})";
        std::string loopStart = "0", loopEnd = "0";
    };
    auto commit = [&](const Args& a) {
        auto args = json::parse(
            std::string(R"({"takes":[{"track":"tr_1","file":"","hash":"","start_sample":0,)") +
            R"("frames":48000,"sample_rate":48000,"midi":true,"loop_start":)" + a.loopStart +
            R"(,"loop_end":)" + a.loopEnd +
            R"(,"overflow_frames":0,"segments":[{"file_frame":0,"timeline":0}],)" +
            R"("events":[{"timeline":0,"run":0,"voice":-1,"bytes":[144,60,100]},)" +
            R"({"timeline":24000,"run":0,"voice":-1,"bytes":[128,60,0]}]}],"punch":)" + a.punch +
            "}");
        REQUIRE(args);
        return d.execute({"record.commit", *args, kUnit});
    };
    const std::vector<std::tuple<Args, std::string, std::string>> cases = {
        {{R"({"enabled":true,"start":0,"end":1e300})"}, "record.punch", "/args/punch/end"},
        {{R"({"enabled":true,"start":-1e300,"end":1})"}, "record.punch", "/args/punch/start"},
        {{R"({"enabled":false,"start":0,"end":1e300})"}, "record.punch", "/args/punch/end"},
        {{R"({"enabled":true,"start":1,"end":1})"}, "record.punch", "/args/punch/end"},
        {{R"({"enabled":true,"start":2,"end":1})"}, "record.punch", "/args/punch/end"},
        {{R"({"enabled":true,"start":0,"end":1})", "48000", "24000"},
         "record.loop",
         "/args/takes/0/loop_end"},
        {{R"({"enabled":true,"start":0,"end":1})", "24000", "24000"},
         "record.loop",
         "/args/takes/0/loop_end"},
    };
    for (const auto& [args, code, path] : cases) {
        INFO(args.punch << " loop " << args.loopStart << ".." << args.loopEnd);
        auto r = commit(args);
        REQUIRE_FALSE(r);
        INFO(r.error().toString());
        CHECK(r.error().code == code);
        CHECK(r.error().path == path);
        CHECK_FALSE(r.error().hint.empty());
        CHECK(p.app->head().midi.empty());
    }
    auto good = commit({});  // the same take, punched in at 0..1 s
    REQUIRE_MESSAGE(good, (good ? std::string() : good.error().toString()));
    CHECK(p.app->head().midi.size() == 1);
    Args looped;  // and [0, 0] means no loop, as in a journal
    looped.loopStart = "0";
    looped.loopEnd = "96000";
    auto withLoop = commit(looped);
    REQUIRE_MESSAGE(withLoop, (withLoop ? std::string() : withLoop.error().toString()));
}

TEST_CASE("a recovered take whose Note Out route is gone keeps its voices apart") {
    // Round-10 review, finding 3: dropping the missing route also dropped
    // each voice's identity, so two overlapping notes of one pitch paired as
    // one (both 480 ticks instead of 960).
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
    const fs::path saved = p.root / "saved/song.json";
    REQUIRE(p.app->save(saved));
    detail::MidiCapture c;
    c.track = {model::Kind::Track, 1};
    c.rate = 48000;
    c.start = 0;
    c.midi.routed = true;
    c.captured = 48000;
    auto event = [&](std::int64_t at, std::uint8_t status, model::Id route, int voice) {
        detail::CapturedMidiEvent e;
        e.at = at;
        e.bytes[0] = status;
        e.bytes[1] = 60;
        e.bytes[2] = status == 0x90 ? 100 : 0;
        e.route = route;
        e.voice = voice;
        c.events.push_back(e);
    };
    const model::Id gone{model::Kind::Route, 99}, alsoGone{model::Kind::Route, 98};
    SUBCASE("two voices of one route") {
        event(0, 0x90, gone, 1);
        event(12000, 0x90, gone, 2);
        event(24000, 0x80, gone, 1);
        event(36000, 0x80, gone, 2);
    }
    SUBCASE("the same voice number on two routes") {
        event(0, 0x90, gone, 1);
        event(12000, 0x90, alsoGone, 1);
        event(24000, 0x80, gone, 1);
        event(36000, 0x80, alsoGone, 1);
    }
    // What stop committed: the take as captured, voices kept apart.
    const RecordedTake stopped = detail::placeMidi(c);
    REQUIRE(p.app->commitTakes({stopped}, "reference stop"));
    REQUIRE(p.app->head().midi.size() == 1);
    const midi::Sequence expected = p.app->head().midi.begin()->value->sequence;
    REQUIRE(expected.notes.size() == 2);
    CHECK(expected.notes[0].length == expected.notes[1].length);
    REQUIRE(p.app->open(saved));  // the routes and the take are not in it
    auto h = *json::parse(journalHeader());
    h["start_sample"] = 0;
    h["device"] = false;
    h["routed"] = true;
    std::string journal = json::writeCompact(h) + "\n";
    for (const auto& e : c.events) {
        auto row = json::Value::array();
        row.push_back(static_cast<long long>(e.at));
        for (auto byte : e.bytes) row.push_back(static_cast<long long>(byte));
        row.push_back(e.route.str());
        row.push_back(static_cast<long long>(e.voice));
        journal += json::writeCompact(row) + "\n";
    }
    journal += "{\"captured\":48000}\n";
    REQUIRE(writeFileAtomic(p.app->recordDir() / "Keys-1.midi.jsonl", journal));
    auto r = p.app->recover({}, {});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    REQUIRE(p.app->head().midi.size() == 1);
    const midi::Sequence actual = p.app->head().midi.begin()->value->sequence;
    REQUIRE(actual.notes.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(actual.notes[i].start == expected.notes[i].start);
        CHECK(actual.notes[i].length == expected.notes[i].length);
    }
}

TEST_CASE("a save that moves or copies the media retires the journal where the take now is") {
    // Round-10 review, finding 4: cleanup deleted the journal's old path,
    // so the copy in the new media folder was offered again after saving.
    const std::string body = "[0,144,60,100]\n[24000,128,60,0]\n{\"captured\":48000}\n";
    cmd::Registry registry;
    registerEngineCommands(registry);
    SUBCASE("a first save moves an unsaved session's media") {
        Unsaved u;
        {
            auto earlier = u.run("earlier");
            cmd::Dispatcher d(registry, *earlier);
            REQUIRE(d.execute({"track.add", *json::parse(R"({"name":"Keys"})"), kUnit}));
            earlier->setFixedClock(1'060'000);
            earlier->tick();
        }
        const fs::path old = u.folder("earlier") / "media/rec/Keys-1.midi.jsonl";
        REQUIRE(writeFileAtomic(old, journalHeader() + body));
        auto app = u.run("current");
        auto recovered = app->recoverSession("earlier");
        REQUIRE(recovered);
        REQUIRE(recovered->recovered.size() == 1);
        const fs::path saved = u.root / "saved/song.json";
        REQUIRE(app->save(saved));
        CHECK_FALSE(fs::exists(app->recordDir() / old.filename()));
        CHECK(app->recoverable().empty());
        REQUIRE(app->open(saved));
        CHECK(app->head().midi.size() == 1);
        CHECK(app->recoverable().empty());
    }
    SUBCASE("a save as copies a saved project's media") {
        Project p;
        cmd::Dispatcher d(registry, *p.app);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        const fs::path first = p.root / "first/song.json";
        REQUIRE(p.app->save(first));
        const fs::path old = p.app->recordDir() / "Keys-1.midi.jsonl";
        REQUIRE(writeFileAtomic(old, journalHeader() + body));
        REQUIRE(p.app->recover({}, {}));
        const fs::path second = p.root / "second/song.json";
        REQUIRE(p.app->save(second));
        CHECK_FALSE(fs::exists(p.app->recordDir() / old.filename()));
        CHECK(p.app->recoverable().empty());
        // The first file does not hold the take: its folder keeps the
        // journal, as it keeps a recording's WAV, and offers it there.
        CHECK(fs::exists(old));
        REQUIRE(p.app->open(first));
        CHECK(p.app->head().midi.empty());
        CHECK(p.app->recoverable().size() == 1);
    }
    SUBCASE("a save as into a folder that already holds the same journal leaves that one") {
        // Round-11 review, finding 1: the copy reused the identical file
        // already there, and the save retired it although another project's
        // file there does not hold its take.
        Project p;
        cmd::Dispatcher d(registry, *p.app);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        const fs::path first = p.root / "first/song.json";
        REQUIRE(p.app->save(first));
        const fs::path old = p.app->recordDir() / "Keys-1.midi.jsonl";
        REQUIRE(writeFileAtomic(old, journalHeader() + body));
        const fs::path other = p.root / "second/other.json";  // a project already there
        REQUIRE(writeFileAtomic(other, *readTextFile(first)));
        const fs::path theirs = p.root / "second/media/rec/Keys-1.midi.jsonl";
        REQUIRE(writeFileAtomic(theirs, journalHeader() + body));
        REQUIRE(p.app->recover({}, {}));
        REQUIRE(p.app->save(p.root / "second/song.json"));
        CHECK(fs::exists(theirs));
        CHECK(fs::exists(old));
        REQUIRE(p.app->open(other));
        CHECK(p.app->head().midi.empty());
        CHECK(p.app->recoverable().size() == 1);
    }
}

TEST_CASE("replacing an unsaved project without saving discards its journals with its folder") {
    // Round-10 review, finding 6, decided for ADR-0078: an unsaved project's
    // folder goes when it is replaced without saving (after the app asks),
    // MIDI journals included; ADR-0080 names this as its one exception.
    Unsaved u;
    cmd::Registry registry;
    registerEngineCommands(registry);
    {
        auto earlier = u.run("earlier");
        cmd::Dispatcher d(registry, *earlier);
        REQUIRE(d.execute({"track.add", *json::parse(R"({"name":"Keys"})"), kUnit}));
        earlier->setFixedClock(1'060'000);
        earlier->tick();
    }
    const fs::path journal = u.folder("earlier") / "media/rec/Keys-1.midi.jsonl";
    REQUIRE(writeFileAtomic(journal, journalHeader() + "[0,144,60,100]\n[24000,128,60,0]\n"
                                                       "{\"captured\":48000}\n"));
    auto app = u.run("current");
    auto recovered = app->recoverSession("earlier");
    REQUIRE(recovered);
    REQUIRE(recovered->recovered.size() == 1);
    REQUIRE(fs::exists(journal));
    SUBCASE("a new project") {
        model::Project replacement;
        replacement.name = "Replacement";
        REQUIRE(app->newProject(replacement));
    }
    SUBCASE("opening another project") {
        Project other;
        const fs::path file = other.root / "other/song.json";
        REQUIRE(other.app->save(file));
        REQUIRE(app->open(file));
    }
    CHECK_FALSE(fs::exists(journal));
    // Its folder is emptied; what may stay is the lock this run holds on it.
    if (fs::exists(u.folder("earlier")))
        for (const auto& e : fs::recursive_directory_iterator(u.folder("earlier")))
            CHECK(e.path().filename() == "session.lock");
    CHECK(app->recoverableSessions().empty());
}

TEST_CASE("a save that cannot read the autosave folder publishes nothing") {
    // Round-13 review, finding 1: the scan stopped at the error, so the file
    // was published with a count short of the autosave beside it, and that
    // older autosave was later offered (and recovered) over the newer file.
    Project p;
    p.rename("Saved");
    const auto file = p.root / "song.json";
    REQUIRE(p.app->save(file));
    const auto saved = readTextFile(file);
    REQUIRE(saved);
    p.rename("Autosaved");
    p.app->setFixedClock(1'060'000);
    p.app->tick();
    const auto dir = p.app->autosaveDir();
    REQUIRE(fs::exists(dir / "autosave-1.json"));
    struct Restore {
        fs::path path;
        ~Restore() {
            std::error_code ec;
            fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
        }
    } restore{dir};
    fs::permissions(dir, fs::perms::none);
    std::error_code probe;
    fs::directory_iterator listing(dir, probe);
    if (!probe) {  // Windows, or a user whom permissions do not stop
        fs::permissions(dir, fs::perms::owner_all);
        MESSAGE("an unreadable folder cannot be made here");
        return;
    }
    p.rename("Newer");
    auto refused = p.app->save(file);
    fs::permissions(dir, fs::perms::owner_all);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "project.autosave_folder");
    CHECK(refused.error().path == dir.string());
    CHECK_FALSE(refused.error().hint.empty());
    CHECK(readTextFile(file).value() == *saved);  // nothing published
    // Readable again, the save goes through and holds that autosave.
    REQUIRE(p.app->save(file));
    REQUIRE(p.app->open(file));
    CHECK(p.app->head().project->name == "Newer");
    CHECK(p.app->newerAutosave(file).empty());
    CHECK_FALSE(p.app->recoverAutosave());
}

TEST_CASE("autosave numbers stay within what a project file can hold") {
    // Round-13 review, finding 2: autosave-9223372036854775808.json was read
    // as a number and written through int64 as a negative count, which the
    // reopened file then ignored (file times decided again).
    Project p;
    const auto file = p.root / "song.json";
    REQUIRE(p.app->save(file));
    const auto dir = p.app->autosaveDir();
    auto through = [&] {
        auto raw = json::parse(readTextFile(file).value());
        REQUIRE(raw);
        REQUIRE((*raw)["autosaved_through"].isInt());
        return (*raw)["autosaved_through"].asInt();
    };
    SUBCASE("a name above the largest count is not an autosave") {
        fs::create_directories(dir);
        REQUIRE(fs::is_directory(dir));
        REQUIRE(writeFileAtomic(dir / "autosave-9223372036854775808.json", "preserved"));
        REQUIRE(p.app->save(file));
        CHECK(through() == 0);
        std::vector<schema::Warning> warnings;
        REQUIRE(p.app->open(file, &warnings));
        CHECK_FALSE(hasWarning(warnings, "project.autosaved_through"));
        CHECK(p.app->newerAutosave(file).empty());
        CHECK(readTextFile(dir / "autosave-9223372036854775808.json").value() == "preserved");
    }
    SUBCASE("the largest count round-trips, and the next autosave reports exhaustion") {
        fs::create_directories(dir);
        REQUIRE(fs::is_directory(dir));
        REQUIRE(writeFileAtomic(dir / "autosave-9223372036854775807.json", "preserved"));
        REQUIRE(p.app->save(file));
        CHECK(through() == INT64_MAX);
        std::vector<schema::Warning> warnings;
        REQUIRE(p.app->open(file, &warnings));
        CHECK_FALSE(hasWarning(warnings, "project.autosaved_through"));
        p.rename("Edited");
        p.app->setFixedClock(1'060'000);
        p.app->tick();
        bool reported = false;
        for (const auto& [topic, value] : p.app->takeEvents())
            if (topic == "engine.error" && value.isString() &&
                value.asString().find("autosave.index") != std::string::npos)
                reported = true;
        CHECK(reported);
    }
    SUBCASE("the model writes no count above the largest") {
        auto doc = model::projectFileToJson(p.app->versions(), false, UINT64_MAX);
        CHECK(doc["autosaved_through"].asInt() == INT64_MAX);
        auto opened = model::projectFileFromJson(doc);
        REQUIRE(opened);
        CHECK(opened->autosavedThrough == std::uint64_t{INT64_MAX});
    }
}

TEST_CASE("retrying a failed save as retires the journal copy the first attempt made") {
    // Round-13 review, finding 4: the copy a failed save as made was found
    // already there by the retry, so it was not this save's to retire and
    // stayed offered for recovery after the file saved.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
    REQUIRE(p.app->save(p.root / "first/song.json"));
    const auto old = p.app->recordDir() / "Keys-1.midi.jsonl";
    REQUIRE(writeFileAtomic(
        old, journalHeader() + "[0,144,60,100]\n[24000,128,60,0]\n{\"captured\":48000}\n"));
    REQUIRE(p.app->recover({}, {}));
    const auto second = p.root / "second/song.json";
    setFileSyncHookForTesting([&](const fs::path& f) { return f != second; });
    auto failed = p.app->save(second);
    setFileSyncHookForTesting({});
    REQUIRE_FALSE(failed);
    REQUIRE_FALSE(fs::exists(second));
    const auto copy = second.parent_path() / "media/rec" / old.filename();
    REQUIRE(fs::exists(copy));
    REQUIRE(p.app->save(second));
    CHECK_FALSE(fs::exists(copy));
    CHECK(p.app->recoverable().empty());
    REQUIRE(p.app->open(second));
    CHECK(p.app->head().midi.size() == 1);
    CHECK(p.app->recoverable().empty());
    CHECK(fs::exists(old));  // the first project's journal stays its own
}

TEST_CASE("a save as retried through another spelling of the path retires its own copy") {
    // Round-14 review: the copies a failed save as made were kept by their
    // literal paths, so a retry as second/./song.json (or through a link)
    // did not know its own journal copy, left it offered, and accepting
    // the recovery duplicated the take.
    for (const bool link : {false, true}) {
        INFO("through " << std::string(link ? "a directory link" : "a dot segment"));
        Project p;
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher d(registry, *p.app);
        REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
        REQUIRE(p.app->save(p.root / "first/song.json"));
        const auto old = p.app->recordDir() / "Keys-1.midi.jsonl";
        REQUIRE(writeFileAtomic(
            old, journalHeader() + "[0,144,60,100]\n[24000,128,60,0]\n{\"captured\":48000}\n"));
        REQUIRE(p.app->recover({}, {}));
        const auto second = p.root / "second/song.json";
        setFileSyncHookForTesting([&](const fs::path& f) { return f.filename() != "song.json"; });
        auto failed = p.app->save(second);
        setFileSyncHookForTesting({});
        REQUIRE_FALSE(failed);
        const auto copy = second.parent_path() / "media/rec" / old.filename();
        REQUIRE(fs::exists(copy));
        fs::path retry = p.root / "second/./song.json";
        if (link) {
            std::error_code ec;
            fs::create_directory_symlink(p.root / "second", p.root / "alias", ec);
            if (ec) {
                MESSAGE("skipped: no directory link here (" << ec.message() << ")");
                continue;
            }
            retry = p.root / "alias/song.json";
        }
        REQUIRE(p.app->save(retry));
        CHECK_FALSE(fs::exists(copy));
        CHECK(p.app->recoverable().empty());
        REQUIRE(p.app->open(second));
        CHECK(p.app->head().midi.size() == 1);
        CHECK(p.app->recoverable().empty());
        CHECK(fs::exists(old));
    }
}

TEST_CASE("a save as through a link and .. does not take another folder's journal for its copy") {
    // Round-17 review: base/link/../dest and base/dest are one path once
    // `..` is removed by spelling, but with link pointing into another
    // folder they are two folders. A save as that failed under the first
    // made its journal copy there; the retry under the second found an
    // identical journal already in base/dest, which it did not make, and
    // deleted it. Only the file system says whether two paths are one file.
    Project p;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *p.app);
    REQUIRE(d.execute({"track.add", json::Value::object(), kUnit}));
    REQUIRE(p.app->save(p.root / "first/song.json"));
    const auto old = p.app->recordDir() / "Keys-1.midi.jsonl";
    REQUIRE(writeFileAtomic(
        old, journalHeader() + "[0,144,60,100]\n[24000,128,60,0]\n{\"captured\":48000}\n"));
    REQUIRE(p.app->recover({}, {}));
    fs::create_directories(p.root / "base");
    fs::create_directories(p.root / "elsewhere/child");
    std::error_code ec;
    fs::create_directory_symlink(p.root / "elsewhere/child", p.root / "base/link", ec);
    if (ec) {
        MESSAGE("skipped: no directory link here (" << ec.message() << ")");
        return;
    }
    const auto first = p.root / "base/link/../dest/song.json";  // elsewhere/dest
    setFileSyncHookForTesting([](const fs::path& f) { return f.filename() != "song.json"; });
    auto failed = p.app->save(first);
    setFileSyncHookForTesting({});
    REQUIRE_FALSE(failed);
    const auto made = first.parent_path() / "media/rec" / old.filename();
    REQUIRE(fs::exists(made));
#if defined(_WIN32)
    // Windows removes `..` by spelling before it follows any link (Win32
    // path normalization), so there base/link/../dest is base/dest: one
    // folder, not two, and the save went where Windows says the path points.
    // A retry under another spelling of one folder is the previous test's.
    CHECK(fs::exists(p.root / "base/dest/media/rec" / old.filename()));
    CHECK_FALSE(fs::exists(p.root / "elsewhere/dest"));
    MESSAGE("skipped: `..` after a link is removed by spelling here, so the paths are one folder");
    return;
#endif
    REQUIRE(fs::exists(p.root / "elsewhere/dest/media/rec" / old.filename()));

    const auto retry = p.root / "base/dest/song.json";
    const auto theirs = retry.parent_path() / "media/rec" / old.filename();
    fs::create_directories(theirs.parent_path());
    REQUIRE(fs::copy_file(old, theirs));
    REQUIRE(made.lexically_normal() == theirs.lexically_normal());
    REQUIRE_FALSE(fs::equivalent(made, theirs));
    const auto before = sha256File(theirs);
    REQUIRE(before);

    REQUIRE(p.app->save(retry));
    CHECK(fs::exists(theirs));  // not this save's copy: it stays
    if (fs::exists(theirs)) CHECK(sha256File(theirs).value() == *before);
    CHECK(fs::exists(made));  // the failed attempt's copy, in a folder no file uses
    CHECK(fs::exists(old));   // the first project's journal stays its own
}

TEST_CASE("an unsaved audio journal recovers loops without a sidecar or autosave") {
    Unsaved u;
    const std::string left = "20261114-090000-82";
    const auto wave = u.folder(left) / "media" / "rec" / "Audio-1.wav";
    fs::create_directories(wave.parent_path());
    audio::WavStreamWriter writer;
    REQUIRE(writer.open(wave, 48000, 1));
    std::array<float, 160> data{};
    REQUIRE(writer.append(data.data(), data.size()));
    REQUIRE(writer.close());
    const auto journal = detail::audioJournalPath(wave);
    REQUIRE(writeFileAtomic(
        journal,
        R"({"schema":"supple.audio-recording/1.0","track":"tr_9","sample_rate":48000,"start_sample":48000,"latency":0,"channels":1,"loop":[48000,48064]})"
        "\n"
        "{\"run\":[64,48000]}\n{\"captured\":128,\"frames\":128}\n"));
    auto app = u.run("20261114-100000-1");
    auto sessions = app->recoverableSessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].recordings == 1);
    auto recovered = app->recoverSession(left);
    REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().toString()));
    REQUIRE(recovered->recovered.size() == 1);
    CHECK(recovered->recovered[0].takes.size() == 2);
    CHECK(app->head().media.begin()->value->frames == 128);
    REQUIRE(app->save(u.root / "saved" / "song.json"));
    CHECK_FALSE(fs::exists(app->recordDir() / "Audio-1.audio.jsonl"));
    CHECK(audio::inspectWavStream(app->recordDir() / "Audio-1.wav")->fileFrames == 160);
    REQUIRE(app->open(u.root / "saved" / "song.json"));
    CHECK(app->head().media.begin()->value->frames == 128);
    CHECK(app->head().takes.size() == 2);
}
