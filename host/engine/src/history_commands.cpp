// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// History commands: undo, redo, history.list and branch.* (session-model §5).
#include "command_helpers.h"

namespace supple::engine {

namespace {

// --- render / history ------------------------------------------------------------------------

struct HistoryResult {
    std::int64_t version = 0;
    std::string label;
};

struct HistoryEntry {
    std::int64_t version = 0;
    std::string label, origin;
    std::uint64_t squashedSteps = 0;
};

struct HistoryList {
    std::vector<HistoryEntry> versions;
    bool canUndo = false, canRedo = false, truncated = false;
    std::uint64_t retainedBytes = 0, budgetBytes = 0;
};

}  // namespace

void registerHistoryCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- history
    auto historyResult = [] {
        return schema::object<HistoryResult>("history_result")
            .field("version", &HistoryResult::version, schema::integer())
            .field("label", &HistoryResult::label, schema::string())
            .build();
    };
    for (bool undo : {true, false}) {
        Command<NoArgs, HistoryResult> c;
        c.name = undo ? "history.undo" : "history.redo";
        c.kind = Kind::Action;
        c.exposure = undo ? cmd::Exposure::McpTool : cmd::Exposure::McpExec;
        c.summary =
            undo ? "Undo the last change on the current branch." : "Redo the last undone change.";
        c.description =
            undo ? "Steps the current branch back one version. The result is the version that is "
                   "current afterwards and its label: the step before the one undone, not the "
                   "undone step (history.list shows both)."
                 : "Steps the current branch forward one version. The result is the version that "
                   "is current afterwards and its label: the step that was redone.";
        c.tags = {"history"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = historyResult();
        c.examples = {{undo ? "Undo" : "Redo", j("{}"),
                       j(R"({"version": "<any>", "label": "<any>"})"),
                       undo ? "song" : "song_undone"}};
        c.run = [undo](const NoArgs&, Context& ctx) -> Result<HistoryResult> {
            EngineApp& app = engine::app(ctx);
            SUPPLE_TRY(undo ? app.undo() : app.redo());
            const model::Version* v =
                app.versions().version(app.versions().headVersion(app.versions().branch()));
            return HistoryResult{static_cast<std::int64_t>(v->id), v->txn.label};
        };
        addChecked(registry, std::move(c));
    }
    [&] {
        Command<NoArgs, HistoryList> c;
        c.name = "history.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List the current branch's recent versions (newest first).";
        c.tags = {"history"};
        c.args = schema::object<NoArgs>("no_args").build();
        auto entry = schema::object<HistoryEntry>("history_entry")
                         .field("version", &HistoryEntry::version, schema::integer())
                         .field("label", &HistoryEntry::label, schema::string())
                         .field("origin", &HistoryEntry::origin, schema::string())
                         .field("squashed_steps", &HistoryEntry::squashedSteps,
                                schema::integerOf<std::uint64_t>())
                         .build();
        c.result = schema::object<HistoryList>("history_list")
                       .field("versions", &HistoryList::versions, schema::arrayOf(entry))
                       .field("can_undo", &HistoryList::canUndo, schema::boolean())
                       .field("can_redo", &HistoryList::canRedo, schema::boolean())
                       .field("truncated", &HistoryList::truncated, schema::boolean())
                       .field("retained_bytes", &HistoryList::retainedBytes,
                              schema::integerOf<std::uint64_t>())
                       .field("budget_bytes", &HistoryList::budgetBytes,
                              schema::integerOf<std::uint64_t>())
                       .build();
        c.examples = {
            {"List", j("{}"),
             j(R"({"versions": "<any>", "can_undo": true, "can_redo": false, "truncated": false, "retained_bytes": "<any>", "budget_bytes": 1073741824})"),
             "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<HistoryList> {
            EngineApp& app = engine::app(ctx);
            HistoryList out;
            for (const model::Version* v : app.versions().history(20))
                out.versions.push_back({static_cast<std::int64_t>(v->id), v->txn.label,
                                        v->txn.origin, v->squashedSteps});
            out.canUndo = app.versions().canUndo();
            out.canRedo = app.versions().canRedo();
            out.truncated = app.versions().historyTruncated();
            out.retainedBytes = app.versions().retainedBytes();
            out.budgetBytes = app.head().project->historyBudgetBytes;
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- branches
    struct BranchCreateArgs {
        std::string name;
        std::optional<std::string> from;
        std::optional<std::int64_t> version;
        bool switchTo = true;
    };
    struct BranchName {
        std::string name;
    };
    struct BranchList {
        std::vector<std::string> branches;
        std::string current;
    };
    struct BranchDiffArgs {
        std::string a, b;
    };
    struct DiffEntry {
        std::string op, id;
        std::vector<std::string> fields;
    };
    struct BranchDiff {
        std::vector<DiffEntry> changes;
    };
    struct BranchMergeArgs {
        std::string from;
        std::optional<std::string> into;
    };
    struct BranchMergeResult {
        bool fastForward = false, upToDate = false;
        std::int64_t version = 0;
        json::Value remapped;
    };
    auto branchName = [] {
        return schema::string().pattern("^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$",
                                        "a branch name like agent/claude/darker");
    };
    [&] {
        Command<BranchCreateArgs, BranchName> c;
        c.name = "branch.create";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start a branch from a branch's head or an earlier version, and switch to it.";
        c.description =
            "Agents: work on agent/<you>/<topic> branches, then branch.diff and branch.merge into "
            "main. "
            "Merging replays your edits onto main even if main moved; conflicting edits are "
            "reported.";
        c.tags = {"branch"};
        c.args = schema::object<BranchCreateArgs>("branch_create_args")
                     .field("name", &BranchCreateArgs::name, branchName())
                     .field("from", &BranchCreateArgs::from, branchName())
                     .field("version", &BranchCreateArgs::version,
                            schema::integer().min(1).doc(
                                "Start at this version of the current branch's history "
                                "(history.list; a render result's version) instead of a head"))
                     .field("switch", &BranchCreateArgs::switchTo, schema::boolean())
                     .defaultValue(true)
                     .build();
        c.result = schema::object<BranchName>("branch_name")
                       .field("name", &BranchName::name, schema::string())
                       .build();
        c.errors = {{"branch.exists", "a branch with that name exists",
                     "pick another name or switch to it"}};
        c.examples = {{"An agent branch", j(R"({"name": "agent/claude/darker"})"),
                       j(R"({"name": "agent/claude/darker"})"), "song"}};
        c.run = [](const BranchCreateArgs& a, Context& ctx) -> Result<BranchName> {
            EngineApp& app = engine::app(ctx);
            if (a.version) {
                if (a.from)
                    return Error("branch.args", "give from or version, not both", "/version",
                                 "version starts the branch at a version of the current branch");
                SUPPLE_TRY(
                    app.versions().createBranchAt(a.name, static_cast<std::uint64_t>(*a.version)));
            } else
                SUPPLE_TRY(
                    app.versions().createBranch(a.name, a.from.value_or(app.versions().branch())));
            if (a.switchTo) SUPPLE_TRY(app.switchBranch(a.name));
            return BranchName{a.name};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchName, BranchName> c;
        c.name = "branch.switch";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Make another branch current (edits, undo and playback follow it).";
        c.tags = {"branch"};
        c.args = schema::object<BranchName>("branch_name")
                     .field("name", &BranchName::name, branchName())
                     .build();
        c.result = schema::object<BranchName>("branch_name")
                       .field("name", &BranchName::name, schema::string())
                       .build();
        c.examples = {{"Back to main", j(R"({"name": "main"})"), j(R"({"name": "main"})"), "song"}};
        c.run = [](const BranchName& a, Context& ctx) -> Result<BranchName> {
            SUPPLE_TRY(app(ctx).switchBranch(a.name));
            return BranchName{a.name};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, BranchList> c;
        c.name = "branch.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List branches and the current one.";
        c.tags = {"branch"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<BranchList>("branch_list")
                       .field("branches", &BranchList::branches, schema::arrayOf(schema::string()))
                       .field("current", &BranchList::current, schema::string())
                       .build();
        c.examples = {
            {"List", j("{}"), j(R"({"branches": ["main"], "current": "main"})"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<BranchList> {
            EngineApp& app = engine::app(ctx);
            return BranchList{app.versions().branches(), app.versions().branch()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchDiffArgs, BranchDiff> c;
        c.name = "branch.diff";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "What differs between two branch heads: objects added, removed, changed (fields).";
        c.tags = {"branch"};
        c.args = schema::object<BranchDiffArgs>("branch_diff_args")
                     .field("a", &BranchDiffArgs::a, branchName())
                     .field("b", &BranchDiffArgs::b, branchName())
                     .build();
        auto entry = schema::object<DiffEntry>("diff_entry")
                         .field("op", &DiffEntry::op, schema::string())
                         .field("id", &DiffEntry::id, schema::string())
                         .field("fields", &DiffEntry::fields, schema::arrayOf(schema::string()))
                         .build();
        c.result = schema::object<BranchDiff>("branch_diff")
                       .field("changes", &BranchDiff::changes, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Main against itself", j(R"({"a": "main", "b": "main"})"),
                       j(R"({"changes": []})"), "song"}};
        c.run = [](const BranchDiffArgs& a, Context& ctx) -> Result<BranchDiff> {
            EngineApp& app = engine::app(ctx);
            auto ha = app.versions().head(a.a), hb = app.versions().head(a.b);
            if (!ha) return Error("branch.unknown", "no branch " + a.a, "/a", "see branch.list");
            if (!hb) return Error("branch.unknown", "no branch " + a.b, "/b", "see branch.list");
            BranchDiff out;
            static constexpr const char* kOps[] = {"add", "remove", "update"};
            for (auto& d : model::diff(*ha, *hb))
                out.changes.push_back(
                    {kOps[static_cast<int>(d.op)], d.id.str(), std::move(d.fields)});
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchMergeArgs, BranchMergeResult> c;
        c.name = "branch.merge";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Merge a branch into another (default: main) by replaying its edits.";
        c.description =
            "Fast-forwards when the target has not moved. Otherwise replays the branch's edits; "
            "IDs it "
            "created are renumbered where needed (see remapped). If both sides changed the same "
            "field "
            "of an object, nothing is merged and the error lists each conflict.";
        c.tags = {"branch"};
        c.args = schema::object<BranchMergeArgs>("branch_merge_args")
                     .field("from", &BranchMergeArgs::from, branchName())
                     .field("into", &BranchMergeArgs::into, branchName())
                     .build();
        c.result = schema::object<BranchMergeResult>("branch_merge")
                       .field("fast_forward", &BranchMergeResult::fastForward, schema::boolean())
                       .field("up_to_date", &BranchMergeResult::upToDate, schema::boolean())
                       .field("version", &BranchMergeResult::version, schema::integer())
                       .field("remapped", &BranchMergeResult::remapped, schema::any())
                       .build();
        c.errors = {{"branch.conflict", "both sides changed the same thing",
                     "details: one per conflict with the object"}};
        c.examples = {
            {"Merge an agent branch", j(R"({"from": "agent/claude/darker"})"),
             j(R"({"fast_forward": true, "up_to_date": false, "version": "<any>", "remapped": {}})"),
             "branch_ahead"}};
        c.run = [](const BranchMergeArgs& a, Context& ctx) -> Result<BranchMergeResult> {
            EngineApp& app = engine::app(ctx);
            const std::string into = a.into.value_or("main");
            auto out = app.mergeBranch(ctx.service<const cmd::Registry>(), a.from, into);
            if (!out.conflicts.empty()) {
                Error e("branch.conflict", std::to_string(out.conflicts.size()) +
                                               " conflict(s) merging " + a.from + " into " + into +
                                               "; nothing was merged");
                e.hint = "change one side (or undo the edit on the branch) and merge again";
                for (const auto& cf : out.conflicts)
                    e.details.emplace_back(cf.kind == model::Conflict::Kind::ConcurrentEdit
                                               ? "merge.concurrent_edit"
                                               : "merge.invalid",
                                           cf.message, "/steps/" + std::to_string(cf.step),
                                           cf.object ? "object " + cf.object->str() : "");
                return e;
            }
            BranchMergeResult r;
            r.fastForward = out.fastForward;
            r.upToDate = out.upToDate;
            r.version = static_cast<std::int64_t>(out.head);
            r.remapped = json::Value::object();
            for (const auto& [from, to] : out.remap) r.remapped[from.str()] = to.str();
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchName, Empty> c;
        c.name = "branch.delete";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Delete a branch (not main, not the current one).";
        c.tags = {"branch"};
        c.args = schema::object<BranchName>("branch_name")
                     .field("name", &BranchName::name, branchName())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Delete", j(R"({"name": "agent/claude/darker"})"), j("{}"), "branch_ahead"}};
        c.run = [](const BranchName& a, Context& ctx) -> Result<Empty> {
            SUPPLE_TRY(app(ctx).versions().deleteBranch(a.name));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
