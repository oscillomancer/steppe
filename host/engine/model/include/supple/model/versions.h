// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Versions, undo/redo, branches and merge by replay (ADR-0008,
// session-model §6). Every committed transaction is a Version holding the
// resulting immutable Session plus what produced it (the resolved command
// invocations, for replay) and what it changed.
//
// Branches (`main`, `agent/<agent>/<topic>`, user branches) each have a head,
// the version they started from (undo stops there) and a redo stack.
// Merging replays the source branch's transactions onto the target: IDs the
// branch created are remapped to whatever the replay created (matched by
// creation order), and an object the target changed since the fork is a
// concurrent-edit conflict. On any conflict the target is left untouched.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "supple/json/json.h"
#include "supple/model/session.h"

namespace supple::model {

struct Invocation {
    std::string command;
    json::Value args;
};

struct TxnRecord {
    std::string label;
    std::string origin;                   // "mcp:claude-code", "gui", "lua:tidy.lua", …
    std::vector<Invocation> invocations;  // resolved (IDs, not selectors), in order
    // Consecutive commits with the same non-empty key replace the head
    // instead of stacking (one fader gesture = one undo step).
    std::string coalesceKey;
};

struct Version {
    std::uint64_t id = 0;
    std::vector<std::uint64_t> parents;  // none (root), one, or two (the last step of a merge)
    std::shared_ptr<const Session> session;
    TxnRecord txn;
    std::vector<Change> changes;  // relative to parents[0]
    std::vector<Id> created;      // IDs this version added, in creation order
    std::uint64_t timeMs = 0;
    std::uint64_t squashedSteps = 0;  // represented by one earlier-history step
    // Fields touched by the combined steps, including edits later reverted.
    std::map<Id, std::set<std::string>> squashedFields;
    // On the steps a merge replays: the source version this step replays,
    // and source ID -> target ID for every object the merge (and the earlier
    // merge it continued) has given a different ID so far.
    std::uint64_t replayOf = 0;
    std::map<Id, Id> mergeRemap;
};

struct Conflict {
    enum class Kind : std::uint8_t { Invalid, ConcurrentEdit };
    Kind kind;
    std::size_t step = 0;       // index into the replayed transactions (0 = first after the fork)
    std::uint64_t version = 0;  // the branch version being replayed
    std::optional<Id> object;   // the contested object (on the target), if any
    std::string message;
};

struct MergeOutcome {
    bool fastForward = false;
    bool upToDate = false;   // nothing to merge
    std::uint64_t head = 0;  // the target's head afterwards (unchanged on conflicts)
    std::vector<Conflict> conflicts;
    std::map<Id, Id> remap;  // branch ID -> ID on the target, including earlier merges'
};

// Re-executes a transaction's invocations on `base` (the command layer
// provides this), building the result as `version`.
struct Replayed {
    Session session;
    std::vector<Change> changes;
    std::vector<Id> created;  // in creation order
};
using Replayer = std::function<Result<Replayed>(
    const Session& base, const std::vector<Invocation>& invocations, std::uint64_t version)>;
// A command's argument JSON Schema (the command layer provides it; null for an
// unknown command). Merges rewrite only what these schemas declare as IDs.
using ArgSchemas = std::function<const json::Value*(std::string_view command)>;

class VersionStore {
  public:
    // `initial` becomes version `initial.version` on branch "main".
    explicit VersionStore(Session initial);

    const std::string& branch() const { return current_; }
    std::shared_ptr<const Session> head() const { return head(current_); }
    std::shared_ptr<const Session> head(std::string_view branch) const;
    std::uint64_t headVersion(std::string_view branch) const;
    // The version number the next commit must use (EditTxn(base, nextVersion())).
    std::uint64_t nextVersion() const { return next_; }

    // Commits on the current branch. `session.version` must be nextVersion().
    Result<std::uint64_t> commit(Session session, TxnRecord txn, std::vector<Change> changes,
                                 std::vector<Id> created, std::uint64_t timeMs = 0);

    // Model-thread metadata completion: no version, revision or undo step.
    // Drop if branch no longer has an item using the expected media. Update
    // every stored version with that id and file identity; leased snapshots
    // remain immutable.
    bool updateMediaHash(Id id, const MediaSource& expected, std::string_view hash,
                         std::string_view branch);

    bool canUndo() const;
    bool canRedo() const;
    Status undo();  // history.nothing_to_undo
    Status redo();  // history.nothing_to_redo
    // The current branch's versions, newest first (up to `limit`).
    std::vector<const Version*> history(std::size_t limit = 50) const;
    const Version* version(std::uint64_t id) const;
    // Model-thread lease for a render/clipboard snapshot. Holding it keeps the
    // version addressable even after its branch/redo tail is discarded.
    std::shared_ptr<const Session> pin(std::uint64_t id) const;
    std::uint64_t retainedBytes() const;  // estimate of unique retained objects/payloads
    bool historyTruncated() const;
    void enforceBudget();  // model thread; leases and branch/merge boundaries survive
    void collectUnused();  // reclaim unreachable versions without active leases
    // Every version on every branch, oldest first.
    void forEachVersion(const std::function<void(const Version&)>& visit) const;

    Status createBranch(const std::string& name, std::string_view from);  // at from's head
    // At an earlier version: in the current branch's first-parent history,
    // else in another branch's (taking that branch's ID counters).
    Status createBranchAt(const std::string& name, std::uint64_t version);
    Status switchBranch(std::string_view name);
    Status deleteBranch(std::string_view name);  // not "main", not the current branch
    std::vector<std::string> branches() const;

    MergeOutcome merge(std::string_view from, std::string_view into, const Replayer& replay,
                       const ArgSchemas& schemas, std::uint64_t timeMs = 0);

  private:
    struct Branch {
        std::uint64_t head = 0, base = 0;
        std::vector<std::uint64_t> redo;
        Counters allocated;  // high-water marks in this branch lineage
        mutable std::shared_ptr<const Session> view = {};
    };
    Result<Branch*> find(std::string_view name);
    const Branch* lookup(std::string_view name) const;
    std::set<std::uint64_t> ancestors(std::uint64_t start) const;     // including start
    bool descends(std::uint64_t from, std::uint64_t ancestor) const;  // by first parents
    std::uint64_t forkPoint(std::uint64_t a, std::uint64_t b) const;

    std::map<std::uint64_t, Version> versions_;
    // Metadata replacements must keep versions addressable while an older
    // immutable snapshot is still leased.
    std::map<std::uint64_t, std::vector<std::weak_ptr<const Session>>> metadataLeases_;
    std::map<std::string, Branch, std::less<>> branches_;
    std::string current_ = "main";
    std::uint64_t next_ = 1;
};

// Rewrites every JSON string equal to a remapped ID (used for replayed
// arguments): IDs have a unique textual form, so exact string matches are IDs.
// Rewrites the IDs in `args` through `remap`, guided by the arguments' JSON
// Schema: only strings (and object keys) whose schema pattern is an ID pattern
// ("^tr_[0-9]+$", "^[a-z]{2}_[0-9]+$") change; names, notes and other text that
// happen to look like IDs stay as written.
json::Value remapIds(const json::Value& args, const json::Value& schema,
                     const std::map<Id, Id>& remap);

}  // namespace supple::model
