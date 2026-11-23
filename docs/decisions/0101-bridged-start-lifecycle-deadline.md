# ADR-0101: A bridged start in the callback fails at the lifecycle deadline

**Status:** Accepted, 2026-11-22. Amends [ADR-0081](0081-live-bridged-plugins.md)
§4 for a start. Contracts: `docs/specs/engine.md` §2.1 (live deadline) and §5.

## Context

ADR-0081 asks a bridged instance's start in the device callback, with the
cycle's bridge deadline, and its 2026-11-15 amendment made a start that
passes the deadline unanswered rather than refused: the engine asks again
the next cycle. Each later cycle then waited for that answer up to its own
deadline, and counted as late under §4's rule for blocks, so a freshly
spawned bridge that took 50 ms of wall time to answer its first start was
killed as "stopped responding" before it ever processed. On loaded Windows
runners it happened (native CI on codirector/engine-tests-portable 4809de6c:
100 cycles, 5 late, then failed), and so could inserting a plug-in on a
loaded machine. Those cycles also ran late for no audio: until it starts,
the stage passes its input through.

## Decision

1. A start posted under a live deadline that passes unanswered stays
   outstanding, as before, and its post time is kept.
2. It is not judged by §4's late rule: that cycle's wait counts as a late
   cycle, but it does not start the 50 ms of consecutive late calls.
3. Until it is answered the callback only looks at the instance's done word
   (one atomic load) when the engine asks again, never waits for it, so
   those cycles are not late. Stop, reset and flush asked meanwhile are kept
   and sent once the answer is seen, as behind a late block.
4. It fails as hung (the existing failed node; the main thread kills the
   bridge when it reaps) once the lifecycle deadline has passed since it was
   posted: 5/2 of the bridge deadline plus 2 s (7 s by default), the same
   deadline a start made off the callback has (ADR-0013 §5).

The callback path adds one atomic load and a steady-clock read, both
already allowed by ADR-0081 §2; no new wait or system call.

## Consequences

- A slow-starting healthy bridge plays once it answers, however long that
  took within the lifecycle deadline; its stage passes audio through
  meanwhile, as before, and cycles are not made late by it.
- A bridge that never answers its start fails after up to 7 s instead of
  50 ms; its stage passes audio through until then.
- A running bridge that stops answering blocks still fails after 50 ms of
  late cycles (ADR-0081 §4, unchanged).
- Tests: a start held at a gate (lifecycle_gate.h) over 200 cycles is not
  failed and those cycles are not late; a start never answered fails as hung
  once the lifecycle deadline passes (a 1 ms bridge deadline: about 2 s);
  the hung-plug-in case waits for its start by the engine's outcome, not by
  a cycle count.
