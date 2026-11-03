// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/model/versions.h>
#include <ostream>

using namespace supple;
using namespace supple::model;

namespace {

// A toy command language for the replayer: track.add {name}, track.set
// {track, volume_db?, mute?}, track.remove {track}.
Result<Replayed> apply(const Session& base, const std::vector<Invocation>& inv,
                       std::uint64_t version) {
    EditTxn t(base, version);
    std::vector<Id> created;
    for (const auto& i : inv) {
        if (i.command == "track.add") {
            Track tr;
            tr.name = i.args.find("name")->asString();
            tr.parent = kMaster;
            const Id id = t.create(tr);
            t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(id); });
            created.push_back(id);
        } else if (i.command == "track.set" || i.command == "track.remove") {
            auto id = Id::parse(i.args.find("track")->asString());
            if (!id || !t.exists<Track>(*id))
                return Error("track.unknown", "no such track", "/track");
            if (i.command == "track.remove") {
                t.modify<Track>(kMaster, [&](Track& m) {
                    m.children.erase(std::find(m.children.begin(), m.children.end(), *id));
                });
                t.remove<Track>(*id);
            } else {
                t.modify<Track>(*id, [&](Track& tr) {
                    if (const auto* n = i.args.find("name")) tr.name = n->asString();
                    if (const auto* v = i.args.find("volume_db")) tr.volumeDb = v->asDouble();
                    if (const auto* m = i.args.find("mute")) tr.mute = m->asBool();
                });
            }
        } else {
            return Error("command.unknown", "unknown command " + i.command);
        }
    }
    auto changes = t.changes();
    auto s = t.commit();
    if (!s) return std::move(s).error();
    return Replayed{std::move(*s), std::move(changes), std::move(created)};
}

// The toy commands' argument schemas: "track" is an ID, "name" is text.
const json::Value* schemas(std::string_view command) {
    static const json::Value add =
        json::parse(R"({"type": "object", "properties": {"name": {"type": "string"}}})").value();
    static const json::Value set = json::parse(R"({"type": "object", "properties": {
        "track": {"type": "string", "pattern": "^tr_[0-9]+$"}, "name": {"type": "string"},
        "volume_db": {"type": "number"}, "mute": {"type": "boolean"}}})")
                                       .value();
    if (command == "track.add") return &add;
    if (command == "track.set" || command == "track.remove") return &set;
    return nullptr;
}

Invocation inv(std::string command, std::string_view args) {
    return {std::move(command), json::parse(args).value()};
}

// Runs invocations on the current head and commits them.
std::uint64_t run(VersionStore& vs, std::vector<Invocation> invs, std::string key = {}) {
    auto r = apply(*vs.head(), invs, vs.nextVersion());
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    TxnRecord txn{invs.front().command, "test", invs, std::move(key)};
    auto v = vs.commit(std::move(r->session), txn, std::move(r->changes), std::move(r->created));
    REQUIRE(v);
    return *v;
}

const Track* track(const VersionStore& vs, std::string_view branch, std::string_view id) {
    return vs.head(branch)->tracks.find(*Id::parse(id));
}

}  // namespace

TEST_CASE("undo and redo are linear per branch; a new edit clears redo") {
    VersionStore vs(Session::create());
    CHECK_FALSE(vs.canUndo());
    CHECK(vs.undo().error().code == "history.nothing_to_undo");
    run(vs, {inv("track.add", R"({"name": "A"})")});
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -3})")});
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -6})")});
    CHECK(track(vs, "main", "tr_1")->volumeDb == -6.0);
    REQUIRE(vs.undo());
    REQUIRE(vs.undo());
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
    REQUIRE(vs.redo());
    CHECK(track(vs, "main", "tr_1")->volumeDb == -3.0);
    run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
    CHECK_FALSE(vs.canRedo());
    CHECK(vs.redo().error().code == "history.nothing_to_redo");
    CHECK(vs.history().size() == 4);  // mute, -3, add, initial
    REQUIRE(vs.undo());
    REQUIRE(vs.undo());
    REQUIRE(vs.undo());
    CHECK(vs.head()->tracks.size() == 1);
    CHECK(vs.head()->next[Kind::Track] == 2);  // allocated ids survive undo
    run(vs, {inv("track.add", R"({"name":"B"})")});
    CHECK(vs.head()->tracks.contains(*Id::parse("tr_2")));
}

TEST_CASE("coalescing: one gesture is one undo step") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name": "A"})")});
    for (int i = 1; i <= 5; ++i)
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -)" + std::to_string(i) + "}")},
            "fader:tr_1");
    CHECK(track(vs, "main", "tr_1")->volumeDb == -5.0);
    CHECK(vs.history().size() == 3);  // gesture, add, initial
    CHECK(vs.history().front()->txn.invocations.size() == 5);
    REQUIRE(vs.undo());
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
}

TEST_CASE("branches: isolated heads, fast-forward merge") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name": "A"})")});
    REQUIRE(vs.createBranch("agent/claude/darker", "main"));
    CHECK(vs.createBranch("agent/claude/darker", "main").error().code == "branch.exists");
    REQUIRE(vs.switchBranch("agent/claude/darker"));
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);  // main is untouched
    CHECK_FALSE(vs.canRedo());
    REQUIRE(vs.undo());
    CHECK_FALSE(vs.canUndo());  // undo stops where the branch began
    REQUIRE(vs.redo());
    auto m = vs.merge("agent/claude/darker", "main", apply, schemas);
    CHECK(m.conflicts.empty());
    CHECK(m.fastForward);
    CHECK(track(vs, "main", "tr_1")->volumeDb == -9.0);
    CHECK(vs.merge("agent/claude/darker", "main", apply, schemas).upToDate);
    CHECK(vs.deleteBranch("main").error().code == "branch.protected");
    CHECK(vs.deleteBranch("agent/claude/darker").error().code == "branch.current");
    REQUIRE(vs.switchBranch("main"));
    CHECK(vs.deleteBranch("agent/claude/darker"));
    CHECK(vs.switchBranch("nope").error().code == "branch.unknown");
}

TEST_CASE("a branch starts at a version of the current or of another branch") {
    VersionStore vs(Session::create());
    const auto first = run(vs, {inv("track.add", R"({"name": "A"})")});
    run(vs, {inv("track.add", R"({"name": "B"})")});
    REQUIRE(vs.createBranch("other", "main"));
    REQUIRE(vs.switchBranch("other"));
    const auto theirs = run(vs, {inv("track.add", R"({"name": "C"})")});
    REQUIRE(vs.switchBranch("main"));
    REQUIRE(vs.createBranchAt("early", first));  // the current branch's history
    CHECK(vs.headVersion("early") == first);
    REQUIRE(vs.createBranchAt("from-other", theirs));  // only in "other"'s history
    CHECK(vs.headVersion("from-other") == theirs);
    REQUIRE(vs.switchBranch("from-other"));
    run(vs, {inv("track.add", R"({"name": "D"})")});  // counters follow "other": no reused id
    CHECK(track(vs, "from-other", "tr_4"));
    CHECK(vs.createBranchAt("nowhere", 999).error().code == "history.version");
}

TEST_CASE("merge by replay remaps IDs created on both sides") {
    VersionStore vs(Session::create());
    REQUIRE(vs.createBranch("topic", "main"));
    run(vs, {inv("track.add", R"({"name": "Main track"})")});  // tr_1 on main
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});  // tr_1 on the branch too
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -3})")});
    auto m = vs.merge("topic", "main", apply, schemas);
    REQUIRE_MESSAGE(m.conflicts.empty(), (m.conflicts.empty() ? "" : m.conflicts[0].message));
    CHECK_FALSE(m.fastForward);
    CHECK(m.remap.at(*Id::parse("tr_1")) == *Id::parse("tr_2"));
    CHECK(track(vs, "main", "tr_1")->name == "Main track");
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
    CHECK(track(vs, "main", "tr_2")->name == "Branch track");
    CHECK(track(vs, "main", "tr_2")->volumeDb == -3.0);  // the edit followed the remap
    CHECK(vs.head("main")->tracks.find(kMaster)->children.size() == 2);  // no false conflict
    CHECK(vs.merge("topic", "main", apply, schemas).upToDate);
    REQUIRE(vs.switchBranch("main"));
    REQUIRE(vs.undo());  // undo steps back through the replayed transactions
    CHECK(track(vs, "main", "tr_2")->volumeDb == 0.0);
}

TEST_CASE("a branch merged again keeps the IDs its earlier merge remapped") {
    auto setup = [] {
        VersionStore vs(Session::create());
        REQUIRE(vs.createBranch("topic", "main"));
        run(vs, {inv("track.add", R"({"name": "Main track"})")});  // tr_1 on main
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.add", R"({"name": "Branch track"})")});            // tr_1 on the branch
        REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());  // now tr_2
        return vs;
    };
    {
        VersionStore vs = setup();
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
        auto m = vs.merge("topic", "main", apply, schemas);
        REQUIRE_MESSAGE(m.conflicts.empty(), (m.conflicts.empty() ? "" : m.conflicts[0].message));
        CHECK(m.remap.at(*Id::parse("tr_1")) == *Id::parse("tr_2"));
        CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
        CHECK(track(vs, "main", "tr_2")->volumeDb == -9.0);
    }
    {
        VersionStore vs = setup();  // the target's own edit to the merged track conflicts
        REQUIRE(vs.switchBranch("main"));
        run(vs, {inv("track.set", R"({"track": "tr_2", "volume_db": -1})")});
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
        auto m = vs.merge("topic", "main", apply, schemas);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].object == Id::parse("tr_2"));
        CHECK(track(vs, "main", "tr_2")->volumeDb == -1.0);
    }
    {
        VersionStore vs = setup();  // a branch of the branch merged in between
        REQUIRE(vs.createBranch("topic2", "topic"));
        REQUIRE(vs.switchBranch("topic2"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -5})")});
        REQUIRE(vs.merge("topic2", "main", apply, schemas).conflicts.empty());
        CHECK(track(vs, "main", "tr_2")->volumeDb == -5.0);
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -7})")});
        auto m = vs.merge("topic", "main", apply, schemas);
        REQUIRE(m.conflicts.size() == 1);  // both changed the volume since the fork
        CHECK(track(vs, "main", "tr_2")->volumeDb == -5.0);
    }
}

TEST_CASE("a later merge compares against the branch's own fork state") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name": "Shared"})")});
    REQUIRE(vs.createBranch("topic", "main"));
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -1})")});  // main only
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
    CHECK(vs.merge("topic", "main", apply, schemas).conflicts.size() == 1);
    CHECK(track(vs, "main", "tr_1")->volumeDb == -1.0);
}

TEST_CASE("merging a branch into a branch that merged it through another keeps identities") {
    VersionStore vs(Session::create());
    REQUIRE(vs.createBranch("topic", "main"));
    REQUIRE(vs.createBranch("target", "main"));
    run(vs, {inv("track.add", R"({"name": "Main track"})")});  // tr_1 on main
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});  // tr_1 here, tr_2 on main
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    REQUIRE(vs.switchBranch("target"));
    run(vs, {inv("track.set", R"({"track": "tr_0", "mute": true})")});
    REQUIRE(vs.merge("main", "target", apply, schemas).conflicts.empty());
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -7})")});
    REQUIRE(vs.merge("topic", "target", apply, schemas).conflicts.empty());
    CHECK(track(vs, "target", "tr_1")->volumeDb == 0.0);
    CHECK(track(vs, "target", "tr_2")->volumeDb == -7.0);
}

TEST_CASE("a merge continues after replayed steps that survived an undo") {
    VersionStore vs(Session::create());
    REQUIRE(vs.createBranch("topic", "main"));
    run(vs, {inv("track.add", R"({"name": "Main track"})")});
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -3})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    REQUIRE(vs.switchBranch("main"));
    REQUIRE(vs.undo());  // the volume step only: the replayed track stays
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    CHECK(vs.head("main")->tracks.size() == 3);  // no second copy of the track
    CHECK(track(vs, "main", "tr_2")->volumeDb == -3.0);
}

TEST_CASE("a merge finds the IDs of replayed steps that budget squashing combined") {
    Project project;
    project.historyBudgetBytes = 1;
    VersionStore vs(Session::create(project));
    REQUIRE(vs.createBranch("topic", "main"));
    run(vs, {inv("track.add", R"({"name": "Main track"})")});
    REQUIRE(vs.switchBranch("topic"));
    const auto created = run(vs, {inv("track.add", R"({"name": "Branch track"})")});
    REQUIRE(vs.createBranch("leaf", "topic"));
    const auto lease = vs.pin(run(vs, {inv("track.set", R"({"track": "tr_1", "name": "B"})")}));
    run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    bool combined = true;  // the replayed creation step is gone, its effect is not
    vs.forEachVersion([&](const Version& v) { combined = combined && v.replayOf != created; });
    CHECK(combined);
    REQUIRE(vs.switchBranch("main"));
    REQUIRE(vs.undo());  // the merge node: the leaf's fork is now held only as squashed
    REQUIRE(vs.switchBranch("leaf"));
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
    const auto later = vs.merge("leaf", "main", apply, schemas);
    REQUIRE_MESSAGE(later.conflicts.empty(),
                    (later.conflicts.empty() ? "" : later.conflicts[0].message));
    CHECK(vs.head("main")->tracks.size() == 3);  // no second copy of the branch track
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
    CHECK(track(vs, "main", "tr_2")->volumeDb == -9.0);
}

TEST_CASE("a squashed step reached through a merge of a merge keeps the branch's IDs") {
    Project project;
    project.historyBudgetBytes = 1;
    VersionStore vs(Session::create(project));
    REQUIRE(vs.createBranch("topic", "main"));
    REQUIRE(vs.createBranch("target", "main"));
    run(vs, {inv("track.add", R"({"name": "Main track"})")});
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});
    REQUIRE(vs.createBranch("leaf", "topic"));
    const auto lease = vs.pin(run(vs, {inv("track.set", R"({"track": "tr_1", "name": "B"})")}));
    run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());  // tr_1 -> tr_2
    REQUIRE(vs.switchBranch("target"));
    run(vs, {inv("track.set", R"({"track": "tr_0", "volume_db": -1})")});
    REQUIRE(vs.merge("main", "target", apply, schemas).conflicts.empty());
    REQUIRE(vs.switchBranch("leaf"));
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
    const auto later = vs.merge("leaf", "target", apply, schemas);
    REQUIRE_MESSAGE(later.conflicts.empty(),
                    (later.conflicts.empty() ? "" : later.conflicts[0].message));
    CHECK(track(vs, "target", "tr_1")->volumeDb == 0.0);
    CHECK(track(vs, "target", "tr_2")->volumeDb == -9.0);
}

TEST_CASE("an older branch still finds the target ID of an object removed since") {
    Project project;
    project.historyBudgetBytes = 1;
    VersionStore vs(Session::create(project));
    REQUIRE(vs.createBranch("topic", "main"));
    const auto mainLease = vs.pin(run(vs, {inv("track.add", R"({"name": "Main track"})")}));
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});
    REQUIRE(vs.createBranch("leaf", "topic"));
    const auto lease = vs.pin(run(vs, {inv("track.set", R"({"track": "tr_1", "name": "B"})")}));
    run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());  // tr_1 -> tr_2
    run(vs, {inv("track.remove", R"({"track": "tr_1"})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    run(vs, {inv("track.set", R"({"track": "tr_0", "volume_db": -2})")});
    REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
    REQUIRE(vs.switchBranch("leaf"));
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
    CHECK(vs.merge("leaf", "main", apply, schemas).conflicts.size() == 1);  // removed on main
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
}

TEST_CASE("merges compare names as text and remove unchanged remapped objects") {
    auto setup = [] {
        VersionStore vs(Session::create());
        REQUIRE(vs.createBranch("topic", "main"));
        run(vs, {inv("track.add", R"({"name": "Main track"})")});  // tr_1 on main
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.add", R"({"name": "tr_1"})")});  // tr_2 on main, named tr_1
        REQUIRE(vs.merge("topic", "main", apply, schemas).conflicts.empty());
        return vs;
    };
    {
        VersionStore vs = setup();
        REQUIRE(vs.switchBranch("main"));
        run(vs, {inv("track.set", R"({"track": "tr_2", "name": "tr_2"})")});
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "name": "Renamed"})")});
        CHECK(vs.merge("topic", "main", apply, schemas).conflicts.size() == 1);
        CHECK(track(vs, "main", "tr_2")->name == "tr_2");
    }
    {
        VersionStore vs = setup();
        run(vs, {inv("track.remove", R"({"track": "tr_1"})")});
        const auto m = vs.merge("topic", "main", apply, schemas);
        REQUIRE_MESSAGE(m.conflicts.empty(), (m.conflicts.empty() ? "" : m.conflicts[0].message));
        CHECK(track(vs, "main", "tr_1"));
        CHECK_FALSE(track(vs, "main", "tr_2"));
    }
}

TEST_CASE("merge conflicts: same field, removed object, failed replay; target untouched") {
    auto setup = [] {
        VersionStore vs(Session::create());
        run(vs, {inv("track.add", R"({"name": "A"})")});
        REQUIRE(vs.createBranch("topic", "main"));
        return vs;
    };
    {
        VersionStore vs = setup();
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -1})")});
        const auto headBefore = vs.headVersion("main");
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")});
        auto m = vs.merge("topic", "main", apply, schemas);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].kind == Conflict::Kind::ConcurrentEdit);
        CHECK(m.conflicts[0].object == Id::parse("tr_1"));
        CHECK(m.conflicts[0].message.find("volume_db") != std::string::npos);
        CHECK(vs.headVersion("main") == headBefore);
    }
    {
        VersionStore vs = setup();  // different fields of one object merge cleanly
        run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")});
        auto m = vs.merge("topic", "main", apply, schemas);
        CHECK(m.conflicts.empty());
        CHECK(track(vs, "main", "tr_1")->mute);
        CHECK(track(vs, "main", "tr_1")->volumeDb == -2.0);
    }
    {
        VersionStore vs = setup();
        run(vs, {inv("track.remove", R"({"track": "tr_1"})")});
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")});
        auto m = vs.merge("topic", "main", apply, schemas);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].message.find("removed") != std::string::npos);
    }
    {
        VersionStore vs = setup();
        run(vs, {inv("track.add", R"({"name": "B"})")});
        REQUIRE(vs.switchBranch("topic"));
        run(vs, {inv("track.add", R"({"name": "C"})")});
        auto failing = [](const Session&, const std::vector<Invocation>&,
                          std::uint64_t) -> Result<Replayed> {
            return Error("render.failed", "replay refused");
        };
        auto m = vs.merge("topic", "main", failing, schemas);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].kind == Conflict::Kind::Invalid);
        CHECK(track(vs, "main", "tr_2")->name == "B");
    }
}

TEST_CASE("remapIds rewrites exact IDs in ID-typed fields") {
    const std::map<Id, Id> remap{{*Id::parse("tr_1"), *Id::parse("tr_7")}};
    const json::Value schema = json::parse(R"({"type": "object", "properties": {
        "track": {"type": "string", "pattern": "^tr_[0-9]+$"},
        "list": {"type": "array", "items": {"type": "string", "pattern": "^[a-z]{2}_[0-9]+$"}},
        "n": {"type": "integer"}}})")
                                   .value();
    auto v =
        json::parse(R"({"track": "tr_1", "list": ["tr_1", "tr_10", "x tr_1"], "n": 1})").value();
    CHECK(json::writeCompact(remapIds(v, schema, remap)) ==
          R"({"track":"tr_7","list":["tr_7","tr_10","x tr_1"],"n":1})");
}

TEST_CASE("coalescing never erases a version another branch references (review round 3)") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name": "A"})")});
    const auto v1 =
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -1})")}, "fader:tr_1");
    REQUIRE(vs.createBranch("topic", "main"));
    const auto v2 =
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")}, "fader:tr_1");
    CHECK(v2 != v1);  // not coalesced: topic's head is v1
    REQUIRE(vs.version(vs.headVersion("topic")) != nullptr);
    CHECK(track(vs, "topic", "tr_1")->volumeDb == -1.0);
    CHECK(track(vs, "main", "tr_1")->volumeDb == -2.0);
    // Later gestures on main coalesce again (nothing else points at v2).
    const auto v3 =
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -3})")}, "fader:tr_1");
    CHECK(vs.version(v2) == nullptr);
    CHECK(track(vs, "main", "tr_1")->volumeDb == -3.0);
    REQUIRE(vs.undo());
    CHECK(track(vs, "main", "tr_1")->volumeDb == -1.0);
    (void)v3;
}

TEST_CASE("merge remaps only schema-declared IDs (review round 3)") {
    VersionStore vs(Session::create());
    REQUIRE(vs.createBranch("topic", "main"));
    run(vs, {inv("track.add", R"({"name": "Main track"})")});  // tr_1 on main
    REQUIRE(vs.switchBranch("topic"));
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});           // tr_1 on the branch
    run(vs, {inv("track.set", R"({"track": "tr_1", "name": "tr_1"})")});  // literal text
    auto m = vs.merge("topic", "main", apply, schemas);
    REQUIRE(m.conflicts.empty());
    CHECK(track(vs, "main", "tr_2")->name == "tr_1");  // the name is text, not a reference
    CHECK(track(vs, "main", "tr_1")->name == "Main track");

    const json::Value schema =
        json::parse(
            R"({"type": "object", "$defs": {"id": {"type": "string", "pattern": "^tr_[0-9]+$"}},
        "properties": {"targets": {"type": "array", "items": {"$ref": "#/$defs/id"}},
                       "note": {"type": "string"},
                       "gains": {"type": "object", "propertyNames": {"pattern": "^tr_[0-9]+$"},
                                 "additionalProperties": {"type": "number"}},
                       "item": {"oneOf": [
                           {"type": "object", "properties": {"kind": {"const": "text"}, "value": {"type": "string"}}},
                           {"type": "object", "properties": {"kind": {"const": "ref"}, "value": {"type": "string", "pattern": "^[a-z]{2}_[0-9]+$"}}}]}}})")
            .value();
    const json::Value args =
        json::parse(R"({"targets": ["tr_1", "tr_3"], "note": "tr_1", "gains": {"tr_1": -3},
        "item": {"kind": "ref", "value": "tr_1"}})")
            .value();
    const json::Value text = json::parse(R"({"item": {"kind": "text", "value": "tr_1"}})").value();
    const std::map<Id, Id> remap{{*Id::parse("tr_1"), *Id::parse("tr_2")}};
    json::Value out = remapIds(args, schema, remap);
    CHECK(out["targets"].asArray()[0].asString() == "tr_2");
    CHECK(out["targets"].asArray()[1].asString() == "tr_3");
    CHECK(out["note"].asString() == "tr_1");
    CHECK(out.find("gains")->find("tr_2") != nullptr);
    CHECK(out["item"]["value"].asString() == "tr_2");
    CHECK(remapIds(text, schema, remap)["item"]["value"].asString() == "tr_1");
}

TEST_CASE("unreachable redo and deleted branch versions release storage, leases keep them live") {
    VersionStore vs(Session::create());
    const auto a = run(vs, {inv("track.add", R"({"name":"A"})")});
    const auto abandoned = run(vs, {inv("track.set", R"({"track":"tr_1","name":"discarded"})")});
    std::weak_ptr<const Session> weak = vs.pin(abandoned);
    REQUIRE(vs.undo());
    run(vs, {inv("track.set", R"({"track":"tr_1","name":"new"})")});
    CHECK_FALSE(vs.version(abandoned));
    CHECK(weak.expired());
    REQUIRE(vs.version(a));
    REQUIRE(vs.createBranch("scratch", "main"));
    REQUIRE(vs.switchBranch("scratch"));
    const auto branchVersion = run(vs, {inv("track.set", R"({"track":"tr_1","name":"pinned"})")});
    auto lease = vs.pin(branchVersion);
    weak = lease;
    REQUIRE(vs.switchBranch("main"));
    REQUIRE(vs.deleteBranch("scratch"));
    REQUIRE(vs.version(branchVersion));
    CHECK_FALSE(weak.expired());
    lease.reset();
    vs.collectUnused();
    CHECK_FALSE(vs.version(branchVersion));
    CHECK(weak.expired());
    CHECK_FALSE(vs.pin(branchVersion));
}

TEST_CASE("an open branch retains its fork history after another branch discards redo") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name":"A"})")});
    const auto fork = run(vs, {inv("track.set", R"({"track":"tr_1","name":"fork"})")});
    REQUIRE(vs.createBranch("open", "main"));
    REQUIRE(vs.undo());
    run(vs, {inv("track.set", R"({"track":"tr_1","name":"other"})")});
    REQUIRE(vs.version(fork));
    REQUIRE(vs.deleteBranch("open"));
    CHECK_FALSE(vs.version(fork));
}

TEST_CASE("coalescing retains leased old versions without adding an undo step") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name":"A"})")});
    const auto old = run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":-3})")}, "gesture");
    auto lease = vs.pin(old);
    run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":-6})")}, "gesture");
    REQUIRE(vs.version(old));
    CHECK(vs.history().size() == 3);
    REQUIRE(vs.undo());
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0);
    lease.reset();
    vs.collectUnused();
    CHECK_FALSE(vs.version(old));
}

TEST_CASE("history budget squashes oldest steps, retains leases and round-trips undo redo") {
    Project project;
    project.historyBudgetBytes = 1;
    VersionStore vs(Session::create(project));
    const auto first = run(vs, {inv("track.add", R"({"name":"A"})")});
    std::weak_ptr<const Session> old = vs.pin(first);
    const auto second = run(vs, {inv("track.set", R"({"track":"tr_1","name":"B"})")});
    CHECK(old.expired());
    CHECK_FALSE(vs.version(first));
    CHECK(vs.historyTruncated());
    REQUIRE(vs.history().size() == 2);
    CHECK(vs.history().front()->txn.label == "earlier history");
    CHECK(vs.history().front()->squashedSteps == 1);
    CHECK(vs.history().front()->txn.invocations.size() == 2);
    auto lease = vs.pin(second);
    run(vs, {inv("track.set", R"({"track":"tr_1","name":"C"})")});
    REQUIRE(vs.version(second));
    CHECK(lease->tracks.find(Id{Kind::Track, 1})->name == "B");
    REQUIRE(vs.undo());
    CHECK(vs.headVersion("main") == second);
    CHECK(track(vs, "main", "tr_1")->name == "B");
    REQUIRE(vs.redo());
    CHECK(track(vs, "main", "tr_1")->name == "C");
    lease.reset();
    vs.enforceBudget();
    CHECK_FALSE(vs.version(second));
    REQUIRE(vs.undo());
    CHECK_FALSE(track(vs, "main", "tr_1"));
    CHECK(vs.historyTruncated());  // the combined step is now a redo target
    REQUIRE(vs.redo());
    CHECK(track(vs, "main", "tr_1")->name == "C");
    CHECK(vs.head()->next[Kind::Track] > 1);
}

TEST_CASE(
    "history payload estimate deduplicates large shared sources and squashing releases them") {
    VersionStore vs(Session::create());
    auto commitBlob = [&](const std::string& key) {
        auto state = *vs.head();
        state.version = vs.nextVersion();
        state.blobs.clear();
        auto source = std::make_shared<MidiSource>();
        source->sequence.length = 960;
        source->sequence.notes.resize(8000);
        state.midi = IdMap<MidiSource>().with(Id{Kind::Midi, 1}, state.version, source);
        state.next[Kind::Midi] = 2;
        auto data = std::make_shared<Blob>();
        data->data.resize(128 * 1024);
        state.blobs.emplace(key, data);
        REQUIRE(vs.commit(std::move(state), {key, "test", {}, {}}, {}, {}));
        return std::weak_ptr<const Blob>(data);
    };
    auto a = commitBlob("a");
    const auto one = vs.retainedBytes();
    for (int i = 0; i < 3; ++i) {
        auto state = *vs.head();
        state.version = vs.nextVersion();
        REQUIRE(vs.commit(std::move(state), {"shared", "test", {}, {}}, {}, {}));
    }
    CHECK(vs.retainedBytes() < one + 16 * 1024);  // shared blob and MIDI payload counted once
    auto b = commitBlob("b");
    auto state = *vs.head();
    auto project = *state.project;
    project.historyBudgetBytes = one + 16 * 1024;
    state.project = std::make_shared<const Project>(project);
    state.version = vs.nextVersion();
    REQUIRE(vs.commit(std::move(state), {"budget", "test", {}, {}}, {}, {}));
    CHECK(a.expired());
    CHECK_FALSE(b.expired());
    CHECK(vs.retainedBytes() <= project.historyBudgetBytes);
}

TEST_CASE("budgeted branches retain fork boundaries and combined replay preserves merges") {
    Project p;
    p.historyBudgetBytes = 1;
    VersionStore vs(Session::create(p));
    run(vs, {inv("track.add", R"({"name":"A"})")});
    REQUIRE(vs.createBranch("other", "main"));
    const auto fork = vs.headVersion("main");
    run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":-3})")});
    run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":-6})")});
    REQUIRE(vs.switchBranch("other"));
    run(vs, {inv("track.set", R"({"track":"tr_1","mute":true})")});
    run(vs, {inv("track.set", R"({"track":"tr_1","name":"branch"})")});
    REQUIRE(vs.version(fork));
    auto merged = vs.merge("other", "main", apply, {});
    REQUIRE(merged.conflicts.empty());
    CHECK(track(vs, "main", "tr_1")->volumeDb == -6);
    CHECK(track(vs, "main", "tr_1")->mute);
    CHECK(track(vs, "main", "tr_1")->name == "branch");
    REQUIRE(vs.switchBranch("other"));
    REQUIRE(vs.undo());
    CHECK(vs.headVersion("other") == fork);
    REQUIRE(vs.redo());
    CHECK(track(vs, "other", "tr_1")->name == "branch");
}

TEST_CASE("squashed edits that return to the fork value still conflict with target edits") {
    Project project;
    project.historyBudgetBytes = 1;
    VersionStore vs(Session::create(project));
    run(vs, {inv("track.add", R"({"name":"A"})")});
    REQUIRE(vs.createBranch("agent", "main"));
    run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":-6})")});
    REQUIRE(vs.switchBranch("agent"));
    run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":-3})")});
    run(vs, {inv("track.set", R"({"track":"tr_1","volume_db":0})")});
    const auto target = vs.headVersion("main");
    auto result = vs.merge("agent", "main", apply, schemas);
    CHECK_FALSE(result.conflicts.empty());
    CHECK(vs.headVersion("main") == target);
    CHECK(track(vs, "main", "tr_1")->volumeDb == -6);
}

TEST_CASE("media hash completion preserves snapshots, revisions, caches and open transactions") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name":"Recorded"})")});
    REQUIRE(vs.createBranch("unrelated", "main"));
    EditTxn recording(*vs.head(), vs.nextVersion());
    MediaSource media{"rec/take.wav", "", "wav", 48000, 1, 48000};
    const Id source = recording.create(media);
    Take take;
    take.source = source;
    const Id takeId = recording.create(take);
    Item item;
    item.track = *Id::parse("tr_1");
    item.takes = {takeId};
    item.activeTake = takeId;
    item.length = 1;
    recording.create(item);
    const auto recordVersion = vs.nextVersion();
    auto recorded = recording.commit();
    REQUIRE(recorded);
    REQUIRE(vs.commit(std::move(*recorded), {"record", "test", {}, {}}, recording.changes(),
                      recording.created()));
    const auto snapshot = vs.pin(recordVersion);
    const auto revision = snapshot->media.entry(source)->rev;
    REQUIRE(vs.createBranch("copy", "main"));
    REQUIRE(vs.createBranch("temporary", "main"));
    REQUIRE(vs.switchBranch("temporary"));
    const auto leasedVersion = run(vs, {inv("track.set", R"({"track":"tr_1","mute":true})")});
    auto lease = vs.pin(leasedVersion);
    // An unrelated lineage can reuse md_1 for a different file.
    REQUIRE(vs.switchBranch("unrelated"));
    EditTxn other(*vs.head(), vs.nextVersion());
    MediaSource different = media;
    different.path = "rec/other.wav";
    CHECK(other.create(different) == source);
    auto otherSession = other.commit();
    REQUIRE(otherSession);
    REQUIRE(vs.commit(std::move(*otherSession), {"other", "test", {}, {}}, other.changes(),
                      other.created()));
    REQUIRE(vs.switchBranch("main"));
    // Force a cached head view: undo retains the allocator high-water marks.
    run(vs, {inv("track.add", R"({"name":"Temporary"})")});
    REQUIRE(vs.undo());
    const auto cached = vs.head();
    CHECK(cached != snapshot);
    EditTxn pending(*cached, vs.nextVersion());
    pending.modify<Track>(*Id::parse("tr_1"), [](Track& track) { track.mute = true; });
    const auto next = vs.nextVersion();
    const auto history = vs.history().size();
    const std::string hash = "sha256:" + std::string(64, 'a');
    CHECK(vs.updateMediaHash(source, media, hash, "main"));
    CHECK(vs.nextVersion() == next);
    CHECK(vs.history().size() == history);
    CHECK(vs.canRedo());
    CHECK(vs.head()->media.find(source)->hash == hash);
    CHECK(vs.head("copy")->media.find(source)->hash == hash);
    CHECK(vs.head("unrelated")->media.find(source)->hash.empty());
    CHECK(vs.head()->media.entry(source)->rev == revision);
    REQUIRE(vs.deleteBranch("temporary"));
    CHECK(vs.version(leasedVersion));  // old snapshot still leases the rewritten version
    CHECK(lease->media.find(source)->hash.empty());
    lease.reset();
    vs.collectUnused();
    CHECK_FALSE(vs.version(leasedVersion));

    CHECK(snapshot->media.find(source)->hash.empty());
    CHECK(cached->media.find(source)->hash.empty());
    auto committed = pending.commit();
    REQUIRE(committed);
    REQUIRE(vs.commit(std::move(*committed), {"mute", "test", {}, {}}, pending.changes(), {}));
    CHECK(vs.head()->media.find(source)->hash == hash);
    REQUIRE(vs.undo());
    CHECK(vs.head()->media.find(source)->hash == hash);
    REQUIRE(vs.undo());  // remove the record step
    CHECK_FALSE(vs.updateMediaHash(source, media, hash, "main"));
    // An import can have already populated the head while the recording job
    // was pending. Its older stored version still needs the same completion.
    VersionStore imported(*snapshot);
    EditTxn known(*imported.head(), imported.nextVersion());
    known.modify<MediaSource>(source, [&](auto& value) { value.hash = hash; });
    auto knownSession = known.commit();
    REQUIRE(knownSession);
    REQUIRE(
        imported.commit(std::move(*knownSession), {"import", "test", {}, {}}, known.changes(), {}));
    CHECK(imported.updateMediaHash(source, media, hash, "main"));
    CHECK(imported.version(recordVersion)->session->media.find(source)->hash == hash);
}

TEST_CASE("history budget preserves leases of snapshots replaced by media hash completion") {
    Project project;
    project.historyBudgetBytes = 1;
    VersionStore vs(Session::create(project));
    EditTxn recording(*vs.head(), vs.nextVersion());
    Track track;
    track.parent = kMaster;
    const Id channel = recording.create(track);
    recording.modify<Track>(kMaster, [&](Track& master) { master.children.push_back(channel); });
    const MediaSource media{"rec/take.wav", "", "wav", 48000, 1, 48000};
    const Id source = recording.create(media);
    Take take;
    take.source = source;
    const Id takeId = recording.create(take);
    Item item;
    item.track = channel;
    item.takes = {takeId};
    item.activeTake = takeId;
    item.length = 1;
    recording.create(item);
    auto session = recording.commit();
    REQUIRE(session);
    const auto recorded = vs.commit(std::move(*session), {"record", "test", {}, {}},
                                    recording.changes(), recording.created());
    REQUIRE(recorded);
    auto lease = vs.pin(*recorded);
    REQUIRE(vs.updateMediaHash(source, media, "sha256:" + std::string(64, 'a'), "main"));
    run(vs, {inv("track.set", R"({"track":"tr_1","name":"renamed"})")});
    REQUIRE(vs.version(*recorded));
    CHECK(lease->media.find(source)->hash.empty());
    REQUIRE(vs.undo());
    CHECK(vs.headVersion("main") == *recorded);
    REQUIRE(vs.redo());
    lease.reset();
    vs.enforceBudget();
    CHECK_FALSE(vs.version(*recorded));
}
