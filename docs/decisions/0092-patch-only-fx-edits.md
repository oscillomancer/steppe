# ADR-0092: Patch-only FX edits reach the running instances without a plan

**Status:** Accepted, 2026-11-17. Amends the scope of ADR-0060.

Contracts: `specs/engine.md` §1.6.

## Context

A semantic plug-in's patch edit (`fx.patch.merge`, `fx.patch.set`, a plug-in
publishing its own patch) commits a version that `Engine::load` compiled into
a whole new plan. The compile's only effect on the audio was one call: the
instance pool's `get()` applying the new patch to the FX's existing instance
on the main thread (`setPatch`, one `SetPatch` request when bridged). The
plan that came out equalled the previous one apart from its generation.
After route E stages 1 and 2 that compile still cost about 4.4 ms at
96 tracks, and it grows with the session.

ADR-0060 already lets level and pan edits change a running plan. Its rules,
comparing whole objects with only the edit class's fields taken from the
new version, carry over to patches.

## Decision

An edit that changes only FX `patch` values, each FX having a patch before
and after, applies those patches to the existing instances and publishes no
plan.

- **Detection** (`patchOnly`, `mix_runtime.cpp`). Every other collection of
  the session has the same storage, and each changed FX is compared whole
  with its patch taken from the new version. A field added to the model
  later therefore compiles; it can never pass as a patch edit. An unchanged
  session compiles, as a maintenance reload.
- **Guards.** The edit compiles as before when any of these hold:
  - a guard of ADR-0060's path holds (a deferred or rate-seeking edit, a
    queued scalar, a plug-in notification reload, another project
    directory);
  - an edited FX has no instance the pool can patch: none made yet, failed,
    waiting on a restart, made for another plug-in or isolation, or
    configured from a state blob rather than a patch;
  - an edited FX belongs to a frozen track;
  - an edited FX has an active envelope. A new plan resends each automated
    value at its first block, which would cover the patch's value; the
    running plan would not;
  - any instance has automation the plug-in cleared. A compile lets that
    automation play again (`resetAutomation`).
- **Application.** `InstancePool::applyPatch` applies the patch as `get()`
  would (the canonical text, `CLAP_SUPPLE_SET_REPLACE`), then services the
  instance. If the plug-in then reports anything a plan holds, the edit
  compiles, and the compile finds the patch already applied. That covers
  a rescan of the parameter list (`CLAP_PARAM_RESCAN_INFO` or `_ALL`, or a
  rescan without flags) or a clear, a latency change, a restart request or
  a failed one, a change of activation, and changed ports (audio channels,
  note ports, key or auxiliary channels).
  - A rescan of values or their text is not one of them. A plan holds a
    plug-in's parameter list (lane ids, ranges, read-only flags), not its
    values, and a CLAP plug-in reports such a rescan after every patch:
    the stock plug-ins call `rescan(CLAP_PARAM_RESCAN_VALUES)` each time.
- **When it is heard.** Nothing changes. The patch reaches the instance at
  the same point as before, on the main thread inside `load`. A plan swap
  neither drops chunks already rendered ahead nor renders them again: the
  renderer hands over at its own position. So a node the callback runs
  plays the patch from its next block, and a node the renderer runs plays
  it after the chunks it had already rendered, up to the render-ahead time,
  with or without a plan. The test is an edit while playing compared
  sample for sample with the same edit compiled, in both executors, with a
  live and a rendered gain.
- **Plug-in outputs.** The app drops plug-in outputs whose plan generation
  is not the published one. That way, values a plug-in reported before an
  edit are never committed over it. A patch-only edit moves no generation,
  so it moves a patch sequence instead.
  - The sequence is odd while patches are being applied. Each executor
    reads it before processing a block, one atomic load, and stamps the
    block's outputs with it.
  - The app drops outputs whose stamp is not the current value, as it drops
    a replaced plan's.
  - Without this, a value the plug-in reported just before the edit was
    committed to the model and acknowledged, while the instance played the
    edit's value.
- **Counts.** `Engine::patchEdits()` counts the edits taken this way. The
  bridge requests are those of the compile it replaces (one `SetPatch` plus
  the service).

## Consequences

- A patch edit costs a model commit, the patch's serialization and its
  application, whatever the session's size. Measured with the stock synth
  (40 `fx.patch.merge` edits, release build, thread CPU time, median), at 96
  tracks: `Engine::load` 158 µs instead of 4968 µs with a shared bridge, and
  226 µs instead of 6355 µs in process. The whole commit is 0.85 to 0.96 ms
  instead of 5.6 to 7.5 ms, and the model commit (about 0.38 ms) is now its
  largest part. A bridged edit's wait for its `SetPatch` answer is not CPU
  time and is not in these numbers.
- Offline renders still compile in full and are the reference. The real-time
  output of a patch-only edit is bit-identical to a compiled reload at the
  same block.
- Wet, bypass, mute and solo still compile (route E stage 4).
- Two cases stay as they were:
  - A change the plug-in reports only later, for example a bridged
    instance's latency notification arriving after its `SetPatch` answer,
    arrives as it did through `Engine::service`'s notification reload.
  - Flush outputs of a stopped plug-in are drained on the main thread and
    stamped when drained, not when produced, as with a compiled edit.
- Native macOS and Windows CI must pass before merging.
