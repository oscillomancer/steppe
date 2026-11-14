# ADR-0076: Project files keep branches and their unmerged steps

**Status:** Accepted, 2026-11-14. Amends ADR-0008 (its 2026-09-26
clarification: "saving a project writes branch heads; history persistence is
optional"), which the code had not implemented. Contracts:
`docs/specs/session-model.md` (§5, §5.3, §5.4, §6.2, schema 2.14), the
`project.save`, `project.open`, `project.new` and `project.get` commands.

## Context

An agent proposes changes on a branch and a human reviews and merges them
(ADR-0008). Until now a save wrote only the current branch's head as the
project: branches, and the steps on them, were lost on quit, and saving while
an agent branch was current wrote that branch over main. ADR-0008 said saving
writes branch heads, `session-model.md` said only that undo history is not
persisted, and the code wrote one head: the three disagreed.

Heads alone are not enough to merge after reopening. A merge replays the
branch's logged invocations since its fork, compares what they touch against
the fork state, and uses each object's revision stamp to skip objects the
target has not changed. Files never stored revisions, so every reopened
object looked untouched to that shortcut, and real conflicts would be missed.
The branch's steps are also what the human reviews after reopening:
`history.list` on the branch, per-step conflicts, undo on the branch.

## Decision

1. **The document is main's head**, whichever branch is current. Readers that
   know nothing of branches (older builds, tools, templates) see main.
2. **A `branches` section** beside it (schema 2.14) keeps `current`,
   `next_version`, `main_hash`, `refs` (per branch: `head`, `base`, `redo`,
   `next_ids`, its ID high-water marks) and `versions`: each kept version's
   parents, label, origin, time, logged invocations, changes, created IDs,
   squash and merge bookkeeping, and its state as the objects in which it
   differs from main's head (`{set: {pointer: object}, remove: [pointer]}`,
   one entry per object by ID, blob by hash, or settings section). A project
   with main alone and nothing to undo writes no section.
3. **What a save keeps by default:** every branch head; every step of every
   branch since it left main (exact: labels, invocations, per-step state, so
   it stays reviewable, undoable and conflicts name their step); the fork it
   left from; and what earlier merges still need (a merge's source parent
   while that branch continues). Everything else is squashed with the
   history-budget code (ADR-0008, 2026-10-04): main's own history, parts of a
   branch that were merged already, and the lineage of merged branches that
   were deleted. Undo on main stops at its saved head; redo steps are not
   kept. A branch whose base was behind its fork with main (its earlier part
   merged) starts its undo at that fork.
4. **`project.save {history: true}`** keeps every version the session still
   retains (within `history_budget_bytes`), each branch's base and redo
   steps: undo and redo survive reopening. The GUI does not offer it yet.
5. **Opening** restores the branches and the current branch. Revisions are
   rebuilt: an object equal to its first parent's shares its entry and
   revision, others take the version's number, which keeps the merge
   shortcut sound and restores structural sharing in memory.
6. **Damaged or stale sections.** `main_hash` is the SHA-256 of main's
   canonical document. If it no longer matches (a program that does not keep
   branches changed main) or the section is malformed, the project opens main
   alone with a `project.history` warning. The first save over that file then
   keeps a copy of it as `<file>.pre-branches.bak` (`-2`, `-3`… if taken) and
   says so (`project.history_backup`); the file itself is untouched until then.
7. **Autosaves** write the same format. Autosave and the GUI's
   unsaved-changes check compare every branch's head and the current branch
   (`VersionStore::heads()`) with those last written, not only the current
   head: an edit on a branch that is not current at save time still counts,
   and an undo back to the saved heads is clean again.
8. Older files (2.13 and before) open as main alone, as they always did. An
   older build opening a 2.14 file sees main, keeps the section verbatim and,
   if it changes main, makes the section stale (6).

## Consequences

- Agent branches survive quitting, crashes (through autosaves) and reopening,
  and merge after reopening as they would have before.
- Logged invocations become a persisted format. A later change to a command's
  arguments that old logs do not satisfy makes a merge after reopening stop
  with an `invalid` conflict, unless the change migrates logged invocations.
- A file grows with each unmerged branch by the objects it changed and the
  arguments of its steps, plus one state per fork and merge kept. Saving
  serializes every kept version's state, on the main thread like any save.
- A merge (live or after reopening) can only replay what a step logged. A
  plug-in's own state change now logs the internal `fx.state.capture {fx,
  state}`, which finds the state by hash in any kept version; automation
  written during playback and recording commits still log nothing
  (`STATUS.md`, known limits).

## Alternatives considered

- **Heads only** (ADR-0008 as written): cannot merge after reopening by
  replay, loses the agent's steps, and needed revisions in the file anyway.
- **Squash each branch to one step:** smaller, but conflicts lose their step,
  `history.list` and undo on the branch lose the proposal's structure.
- **Full history always:** larger files for every project, and the undo
  history of main is rarely wanted after reopening (REAPER makes it optional).
- **Each version as a full document, or deltas along the parent chain:** full
  documents multiply the file; parent-chain deltas need one full root and make
  every state depend on its ancestors' entries. Deltas against main need main
  only, and main is the document.
- **A sidecar file for branches:** two files cannot be replaced atomically
  together, and a copied project file would lose its branches.
