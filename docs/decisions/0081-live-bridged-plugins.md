# ADR-0081: Bridged plug-ins run on live nodes, with a bounded wait

**Status:** Accepted, 2026-11-14. Implementation follows in separate commits.
Amends [ADR-0013](0013-plugin-isolation.md) §5 (synchronous bridge calls
never run in the device callback) and the callback rules of
[ADR-0024](0024-live-multiprocessing.md) and
[ADR-0028](0028-renderer-wakeups.md) (no callback waits). Contracts:
`AGENTS.md` rule 2, `ARCHITECTURE.md` §3, `docs/specs/engine.md` §2.1 and §5.

## Context

A plug-in isolated in a bridge process (ADR-0013) is processed only offline
and on rendered-ahead nodes. On a callback-owned node (an armed or monitored
track, input FX, and every node when anticipation is off) it is passed
through with a warning: an armed bridged instrument is silent, bridged input
FX are not heard or recorded. Third-party plug-ins cannot be bridged by
default while that is so, and bridging them is how a crashing plug-in stops
taking the whole program down.

Processing a bridged plug-in in the callback means handing it a block and
waiting for its answer, which the real-time rule forbids: the callback never
blocks. An in-process plug-in runs in the callback for as long as it takes,
and a hung one stalls the callback for ever; a bridged one, waited for with
a bound, is strictly safer than that. A pipelined bridge never waits, but it
adds a device block of latency to each bridged stage, which a monitored
input cannot hide, and its output is no longer the in-process output.

## Decision

1. **Synchronous on live nodes.** A bridged stage on a callback-owned node
   (the callback thread or a live helper, ADR-0024) is processed as an
   in-process stage is: the executor writes the block into the instance's
   shared memory, wakes the bridge's audio thread and waits for its answer.
   No latency is added, delay compensation is unchanged and the output is
   bit-identical to in-process and to offline rendering. The bridge's
   lifecycle commands (start and stop processing, reset, flush) on those
   nodes go the same way.
2. **The exception, exactly.** On a callback-owned thread the only waits
   allowed are these, inside `rt::ScopedSanctionedSyscall`, in the bridge
   client's live command path:
   - one wake of the instance's request word (Linux `futex` wake, macOS
     address wake, Windows `SetEvent`), after a release store of the request;
   - waits on the instance's done word with a timeout no later than the
     cycle's bridge deadline (Linux `futex` wait, macOS address wait,
     Windows `WaitForSingleObject`), each rechecked before and after;
   - steady-clock reads for that deadline.
   Nothing else: no wait without that deadline, no process checks or kills,
   no allocation, lock, log or file access. Every other part of the rule is
   unchanged, and no other code may wait in the callback.
3. **The bound.** Each callback cycle has one bridge deadline: its start plus
   the larger of its period and 2 ms. The total of all bridged waits in a
   cycle therefore never exceeds it. A cycle that runs past its period is
   counted as an xrun (`engine.status.xruns`, `engine.xrun`), and one whose
   bridged waits made it late also counts in `engine.status.bridge_late`;
   at small buffers (2 ms is more than 64 frames at 48 kHz) a late bridged
   plug-in is reported, never hidden.
4. **A late stage.** When the deadline passes before a stage answers, that
   stage's output is silence for the cycle (its audio, its MIDI output) and
   its request stays outstanding. The next cycle waits for that answer,
   within its own deadline, before it writes the instance's shared memory
   again; an answer that comes is discarded, so the plug-in has processed
   every block in order and only that block's output is lost. After 50 ms
   of consecutive late cycles the instance fails (the existing failed node:
   silence, `fx.failed`, `fx.restart`). The failure is an atomic flag set in
   the callback; the main thread kills and reaps the process.
5. **Scheduling.** While a bridged instance is on a live node its bridge
   audio thread takes the live helpers' class (ADR-0024, ADR-0026, ADR-0038):
   on Linux the callback's real-time policy and priority, on macOS a
   time-constraint policy for the period, on Windows MMCSS "Pro Audio".
   Elsewhere it keeps the background class, so render-ahead work never runs
   above the callback. The bridge reports the class it got; `engine.status`
   names the bridged threads that were refused it.
6. **What stays.** Offline and rendered-ahead processing keep the ADR-0013
   background exception and deadlines unchanged. A pipelined mode is not
   part of this decision.

## Consequences

- Bridged plug-ins are heard on armed and monitored tracks, as input FX
  (recorded too) and with anticipation off, with no added latency.
- The callback can now wait on another process, for at most the bound. A
  slow bridged plug-in makes cycles late, as a slow in-process one does,
  and is counted; a hung one fails after 50 ms instead of stalling audio.
- Each bridged stage costs two context switches per cycle, serially along a
  chain. At small buffers that cost is a large share of the period; the
  measurements in the implementation report decide whether third-party
  plug-ins are bridged by default.
- The macOS bridge cannot join the device's audio workgroup (no Mach-port
  channel), so its thread may be scheduled less favourably than the live
  helpers at small buffers.
- The rtsan preset covers the live bridged path; only the scope in point 2
  may make system calls there.

*Amended 2026-11-14 (implementation):* ADR-0013 point 8's default is an
app preference, `plugin_isolation` (`in_process`, `shared_bridge`,
`dedicated`; shipped `in_process`). `inherit` resolves as an instance is
made: a stock plug-in in process, any other at the preference, in process
with an `engine.status` warning when no bridge is found. The project keeps
`inherit`, since isolation does not change the output, and an instance keeps
its process until it is made again. Making `shared_bridge` the shipped
default is a separate decision, gated on this ADR's measurements.

*Amended 2026-11-15 (review round 16):* point 4's "the plug-in has
processed every block in order" holds for the blocks the bridge was sent. A
block skipped while a late request is outstanding is not sent: its output is
silence and its audio input is lost, but its events reach the plug-in at the
start of the next block it processes, in order, so a note off, a pedal or a
parameter change is never lost. The carry is bounded (512 events); beyond it
the notes are dropped, each parameter keeps its latest value and the plug-in
is reset before that block, so no stale note survives. Lifecycle commands
asked for meanwhile (reset, stop, flush) are kept, once each, and sent in
that order before anything else (`docs/specs/engine.md`, live deadline).
A start the deadline passes is unanswered, not refused: the engine asks
again on the next cycle rather than leaving the stage passed through until
another plan.

*Amended 2026-11-15 (review round 19):* the reset that follows an overflow
is sent only when a note or MIDI event was dropped. A block's own events
(at most 2048) always all reach the plug-in: carried parameters it sets
again give way to its later values, and carried ones that do not fit
beside it wait for the next block. A carry full of distinct parameters
still takes a later value of one of them.
The device period that sizes §5's macOS class is carried as a duration
(protocol POSIX 24 / Windows 25), so an oversampled plug-in, activated at
2 or 4 times the device's rate, still asks for the device's period.

## Alternatives considered

- **Pipelined live bridging:** never waits, but adds a block of latency per
  bridged stage on monitored input and changes the output; kept as a
  possible opt-in later if the measurements show synchronous waits miss.
- **Keeping the pass-through:** bridged instruments stay silent when armed,
  so third-party plug-ins cannot be bridged by default.
- **A per-call deadline instead of per cycle:** a chain of bridged stages
  could then stall a cycle for a multiple of the bound.
- **Failing a stage on its first late cycle:** one scheduling hiccup would
  silence a healthy plug-in until it is restarted.
