# ADR-0008: Session model, stable IDs, undo and branches

**Status:** Accepted, 2026-09-26. Detailed contract lands in
`docs/specs/session-model.md` before Phase 4.

## Context

Principles 2 and 3: the session is a queryable tree with stable IDs; commands
group into transactions; undo covers everything including plugin state;
sessions branch so an agent can propose changes that a human A/B-compares and
then merges or discards. Principle 5: offline render is deterministic.

## Decision

### Immutable, versioned model

The session is an immutable value tree. Nodes are `std::shared_ptr<const T>`
and edits copy only the path from the changed node to the root (structural
sharing). Large payloads — MIDI event arrays, plugin state blobs, envelope
point arrays, peak data references — are shared immutable chunks, so a
version costs a few pointer copies.

A **version** is `{root, parent(s), transaction record, origin, label,
timestamp}`. Every `edit` command (or `batch`) applied to a branch head
produces one new version. The model thread is the only writer; readers (GUI,
queries, renderers) hold a version pointer and never see a partial edit.

### Stable IDs

Every addressable object (track, item, take, fx instance, envelope, send,
marker, region, tempo/meter event, branch) has an ID of the form
`<kind>_<n>`: `tr_12`, `it_40`, `tk_41`, `fx_7`, `en_3`, `sd_2`, `mk_5`,
`rg_1`. `n` comes from a per-kind counter stored in the session, never
reused, including after undo (each lineage retains allocation high-water counters). Short sequential IDs are cheap in LLM context and hard to mistype.
Events inside containers (MIDI notes, envelope points) are not objects: they
are addressed by the containing object plus a selector (time range, pitch,
index in canonical order), or replaced wholesale via notation/point lists.

### Undo

Undo history is per branch and linear (REAPER semantics): undo moves the
branch head to the previous version, redo forward. Plugin state is in the
model: stock plugins' semantic parameter values are model fields; third-party
parameter changes arrive as `fx.param.set` commands (GUI gestures coalesce to
one undo step); opaque plugin state changes (`clap.state` dirty notification,
preset load inside the plugin GUI) are captured on the model thread as a
`fx.state.capture` version. Automation playback never creates versions.

### Branches

A branch is a named ref to a version. `main` is what the human works on.
An agent creates `agent/<agent>/<topic>` from a version of `main`, applies
batches, and asks for review. Review shows the branch diff (object-level,
rendered compactly) and supports A/B:

- **Offline A/B (default):** render both versions offline over the same
  range and play the renders back with sample-aligned instant switching.
  Deterministic rendering (Principle 5) makes this exact.
- **Live A/B:** the engine re-targets its graph from one version to the
  other by reconciling (diffing trees, keeping unchanged plugin instances,
  loading changed state), for short edits where re-rendering is overkill.

**Accept** fast-forwards `main` if it has not moved since the fork.
Otherwise the branch's transactions are **replayed** onto `main`'s head: each
logged invocation is re-validated and re-applied. IDs created on the branch
that collide with IDs created on `main` meanwhile are remapped (creation
commands log the ID they produced; later arguments referencing it are
rewritten immediately after each creation, even within one batch — schema types identify ID fields). An invocation that fails, or
that edits a non-structural field also modified on `main` since the fork, is a **conflict**; the merge stops and reports conflicts
with both values, and the human chooses. Project metadata participates as the
`project` singleton; equal new project-field values do not conflict. **Reject** deletes the ref.

### Engine separation

The audio engine never sees the model. The model thread compiles a version
into an immutable `RenderPlan` (graph topology, buffer assignments, delay
compensation, parameter/automation data in audio-ready form), hands it to the
audio thread with a wait-free pointer swap, and reclaims the old plan on the
model thread after the audio thread acknowledges it.

## Consequences

Undo, redo, branch, diff and A/B are cheap, uniform and testable (edit
functions are pure; tests can fuzz random command sequences and check undo
round-trips by comparing trees). The cost is disciplined immutability in the
model code and a reconciler between model versions and live plugin instances.

## Alternatives considered

- Mutable model + command-pattern undo (inverse operations): every command
  needs a correct inverse, and branches become copies. Error-prone.
- `immer` persistent collections (BSL-1.0, allowed): real HAMTs/RRB vectors;
  kept as an upgrade path if profiling shows vector copies matter. Plain
  `shared_ptr<const>` + `std::vector` of children is simpler for many agents
  to work with.
- Random 64-bit/UUID IDs: collision-free merges without remapping, but long,
  token-expensive and error-prone for LLMs.
- Three-way field-level merge instead of replay: more automatic, but replay
  re-validates with the same rules the agent faced and keeps merge semantics
  explainable.

## Clarifications (2026-09-26, from design review)

- **Memory bounds.** Undo history is bounded per branch by a configurable
  byte budget (default 1 GiB of estimated *unique* retained data). The
  project setting `history_budget_bytes` controls it; old undo steps beyond
  the bound are squashed into one earlier-history step. A separate version
  count cap is not implemented. Large
  payloads (plugin state blobs, MIDI chunks, envelope chunks) are immutable
  and deduplicated by content hash, so re-capturing identical state costs
  nothing. Unreachable versions are freed immediately (reference counting).
  Saving a project writes branch heads; history persistence is optional
  (`project.save {history: true}`), capped by the same budget.
  *(2026-11-14: implemented by ADR-0076, which also keeps each branch's
  unmerged steps by default so that it merges after reopening.)*
- **Coalescing.** Continuous gestures (a fader drag, an agent sweeping a
  value through many `fx.param.set` calls in one batch) coalesce into one
  version per gesture/batch, not one per event.

### Unreachable history reclamation (2026-10-04)

Versions outside every branch head/base/redo ancestry are reclaimed after
edits and branch deletion. Render jobs and the clipboard hold explicit session
leases (`VersionStore::pin`); unreachable collection traces their version
and ancestry until the lease is released. Budget enforcement may still combine
unleased older steps, while preserving leased versions and open branch boundaries. The subsequent budget phase squashes reachable linear undo steps as described
below.


### History budget and squashing (2026-10-04)

After commits, merges and branch deletion, estimate the retained unique object,
MIDI, blob, envelope and automation payloads by shared pointer identity; shared
collection storage is counted once. Include snapshot and replay-log metadata.
If the current project's `history_budget_bytes` (default 1 GiB) is exceeded,
combine the oldest eligible linear steps into their child's `earlier history`
step, dropping intermediate snapshots and retaining the combined replay log,
changes, created IDs and the union of edited field names (including edits
reverted to the fork value, for merge conflict detection). Undo/redo can cross that step as one operation.
`history.list` exposes `truncated`, each entry's `squashed_steps`, and estimated
`retained_bytes`/`budget_bytes`.

Leased versions, branch heads/bases, redo targets, forks and merge nodes remain
addressable and exact; their required snapshots and retained replay logs may
exceed the budget. No payload is discarded from a live session. The bound is an
estimate of model/history data, not a process RSS or media-cache limit. Releasing
a lease allows later budget enforcement to squash formerly protected steps.

*Clarified 2026-11-15:* retained-byte estimates are maintained incrementally by
shared identity, with the same counts and oldest-first retention decisions.
Ordinary append commits do not remeasure earlier snapshots. Session-model §6.2
describes the size index and its deterministic work counter.
