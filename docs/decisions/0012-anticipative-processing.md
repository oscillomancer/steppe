# ADR-0012: Anticipative processing and the worker pool

**Status:** Accepted, 2026-09-26. Spec: `docs/specs/engine.md` §2.

**Amendment, 2026-10-07:** [ADR-0023](0023-background-worker-participation.md)
refines §6: graph-bounded participation, parking within jobs, explicit join
ownership and the app thread-limit preference.

**Amendment, 2026-10-07:** [ADR-0024](0024-live-multiprocessing.md) defines
live callback fork-join, helper scheduling and the nonblocking wake exception.

**Amendment, 2026-10-08:** [ADR-0026](0026-background-thread-scheduling.md)
runs the renderer and the pool's helpers real-time, below a real-time callback.

**Amendment, 2026-10-08:** [ADR-0028](0028-renderer-wakeups.md) replaces
renderer polling with block deadlines and event-driven wakes, extending the
nonblocking callback futex-wake exception to buffer-space notifications.

**Amendment, 2026-10-16:** [ADR-0045](0045-master-playrate.md) defines master
playrate with a shared timeline clock and source conversion in device-time
graphs; model/command support precedes DSP integration.

## Context

The engine runs every node inside the audio callback, on one core. Phase 4
makes anticipative processing (REAPER's model) the primary performance goal:
nodes whose output does not depend on anything known only now render ahead of
the playhead on worker threads in large blocks; only live paths run at the
device block size. Nothing is live yet (no recording or input monitoring, no
plugin GUIs in the engine), so today the whole graph is anticipative. The
design must still leave room for the live region.

## Decision

1. **One renderer.** A single thread at a time owns the plan, the plugin
   lifecycle (`start_processing`/`stop_processing`/`reset`/`process`), the
   render position and transport handling: the *renderer*. With
   anticipation off it is the audio callback (today's path, the reference);
   with anticipation on it is the engine's anticipation thread. Plugins are
   never touched by two threads at once, and CLAP's audio-thread calls come
   from whichever thread renders (clap-host marks it with
   `AudioThreadScope`).
2. **Whole-graph anticipation first.** The renderer renders the graph in
   render blocks (default 1024 frames) into an SPSC ring of *chunks*
   (`{epoch, start sample, frames, L, R}`), keeping it `render_ahead`
   ahead of the playhead (default 200 ms). The audio callback copies chunk
   frames to the device at the device block size and does nothing else. A
   callback that finds no chunk while the transport plays counts an
   *anticipation miss* and outputs silence (the first chunk after a
   start/seek is pre-roll, not a miss).
3. **Flush by epoch, not by clearing.** Seek and pause bump an epoch
   counter on the main thread. The callback drops chunks of older epochs at
   the start of each cycle (so a pause silences at once and a seek never
   plays stale audio); the renderer relocates and renders the new epoch.
   Neither side waits for the other on the audio thread. The pause position
   is where the listener was: the renderer waits (off the audio thread,
   bounded) until the callback has acknowledged the new epoch and resumes
   from the position it reports.
4. **Edits take effect at the render-ahead horizon.** A new plan is adopted
   by the renderer at its render position, after the audio already in the
   ring — like REAPER's anticipative FX — so edits never cause dropouts.
   (Live-flagged nodes, once they exist, bypass this.)
5. **Pacing without signals from the audio thread.** The renderer renders
   while the ring holds less than `render_ahead`; otherwise it sleeps half a
   render-block period. The audio callback never makes a system call to
   wake it.
6. **Worker pool.** `N = max(1, cores − 2)` threads (engine setting). One
   render block of the graph is a DAG over nodes: each node has an atomic
   count of unfinished source nodes; ready nodes go to a bounded lock-free
   queue; the submitting thread helps and returns when every node is done;
   idle workers park on an atomic wait. Each node sums its inputs in the
   fixed order and processes its edges, items and chain alone, so output is
   bit-identical for any thread count (benchmark B4). The offline renderer
   uses the same pool.
7. **Stop.** `Engine::stop()` ends the anticipation thread first; its last
   act is to stop every plugin (as the renderer). The backend's final
   zero-frame callback then finds nothing to stop; the contract stands for
   the in-callback renderer.
8. **Later (with recording and monitoring):** live classification
   (engine.md §2.1); rings per frontier edge from anticipative taps into
   live nodes; the audio callback renders the live subgraph, with the pool
   helping (the callback enqueues and helps, never blocks).

## Consequences

- Latency of an edit is up to `render_ahead` (200 ms) while playing; a seek
  starts after one render block is rendered (pre-roll).
- Rendering in 1024-frame blocks and in parallel is where the capacity gain
  comes from (B1 compares anticipation on/off).
- Tests that compare real-time output to offline renders wait for the ring
  (`Engine::awaitRendered`) before driving a manual backend.

## Alternatives considered

Rendering the anticipative graph on the audio thread with a larger internal
block (no gain from parallelism, and the block's full cost lands in one
callback); flushing rings on every edit (dropouts on every change); a lock
between the callback and the renderer (priority inversion on the audio
thread).

## Output-event handoff (review round 3)

Once a block's worker tasks finish, its owning executor copies parameter values
and gestures into its own preallocated SPSC queue for main. Renderer and callback
use separate queues: workers never concurrently publish to one SPSC producer.
Records include FX id, plan generation, transport epoch, latency-corrected
position and stream position; the main consumer can reject obsolete epochs and
align anticipative recording to what has played. Overflow is counted; idle flush
outputs join the main take API. A requested stopped render does not advance the
transport or publish audio chunks. See engine.md for the app consumer contract.

## Note (2026-11-21): pre-roll is a whole render block

"Starts after one render block is rendered" (Decision 2, Consequences) means a
whole render block of the epoch, and the callback's whole first cycle (at most
the render-ahead span). The callback had started on any chunk: a launch from
stop whose compensated tracks switch a few frames in cuts the renderer's first
chunks to those frames, so the first cycle played them and then silence, which
no counter showed, and the next cycles counted misses while the first whole
block rendered. The GUI lead's walk heard it as render gaps on a scene launch.
Now the pre-roll waits for the whole block (engine.md, "Anticipative
processing"), and a cycle that plays audio and then runs short counts a miss,
the cycle that starts the play included. A launch from stop starts about one
render block's time later; a plain start, a locate and a loop wrap do not
change.
