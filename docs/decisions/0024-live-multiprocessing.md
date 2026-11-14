# ADR-0024: Live callback multiprocessing

**Status:** Accepted, 2026-10-07. Implementation follows in a separate commit.
Amends [ADR-0012](0012-anticipative-processing.md) §8 and clarifies the
syscall exception in [ADR-0013](0013-plugin-isolation.md).

**Related, 2026-10-08:** [ADR-0026](0026-background-thread-scheduling.md)
schedules the renderer and background pool real-time below the live helpers;
the live helpers' scheduling here is unchanged.

**Amendment, 2026-11-14:** [ADR-0081](0081-live-bridged-plugins.md) lets
the callback process bridged plug-ins on live nodes, with one bounded wait
per cycle; it states the exception to the callback's no-wait rule.

**Review correction, 2026-10-08:** the lifetime protocol below joins actual
claims, replacing pre-enrollment of every notified helper. A helper that has
not entered must not extend the cycle merely to observe that it has ended.

**Amendment, 2026-10-08:** [ADR-0028](0028-renderer-wakeups.md) replaces
renderer polling with block deadlines and event-driven wakes, extending the
nonblocking callback futex-wake exception to buffer-space notifications.

**Platform amendment, 2026-10-14:** [ADR-0035](0035-headless-platform-layer.md)
adds macOS/Windows mechanisms while retaining these Linux semantics.

**Native device amendment, 2026-10-14:** [ADR-0038](0038-native-device-audio.md)
adds PortAudio on macOS/Windows and device-derived, self-applied helper policies;
Linux mechanisms and callback publication/lifetime rules remain unchanged.

## Context

Four independent live Ferric instances exhaust one callback core at 64 and
128 frames. Anticipation cannot move monitored inputs into the future.
The background pool now parks within jobs (ADR-0023), which is appropriate
for render-ahead work but cannot be called by a device callback.

## Decision

1. **Separate live pool, conventional fork-join.** Each device segment (also
   split at loop, count-in and plan boundaries) publishes a preallocated DAG
   of its live nodes. With anticipation disabled it uses the whole graph.
   The callback and bounded helpers claim ready nodes, execute each exactly
   once, and publish successors through atomic dependency counters. The
   callback helps throughout. While a dependency or helper departure is
   outstanding it spins on atomics, with a CPU relax instruction: no wait,
   yield, sleep or other join syscall. A late in-flight node makes the cycle late;
   there is no discard, timeout, rerun or attempt to undo plug-in state.
2. **Explicit lifetime.** With the job closed and previous readers joined,
   publish its plain pointer/function/context fields, then open it with a
   sequentially consistent store before notifying selected helpers. A helper
   increments the active count (seq_cst), then checks open (seq_cst). Only an
   open job permits reading those fields; otherwise it skips them. After all
   accesses it decrements active with release. Once all nodes finish, the
   callback closes open (seq_cst) and spins on active (seq_cst) until zero.
   This store-load handshake requires seq_cst on both sides: either the
   callback sees the claim or the helper sees closed. Return need not wait
   for a helper preempted before claiming. A late wake can harmlessly enter
   the next open job. No helper may access a job after acknowledging its
   departure. Only after close/join may the callback collect
   outputs/meters, record inputs, relocate, reset, adopt a plan or acknowledge
   a newer generation. Existing main-thread plan reclamation therefore also
   covers helper accesses. Plugin lifecycle remains with the owning executor;
   helpers borrow its ownership token only for node processing.
3. **Bounded participation and idle parking.** Reuse the compiled dependency
   representation and conservative chain-cover bound, not the background
   pool's blocking executor. Select at most width minus one helpers and the
   configured capacity, capped by available CPUs minus one. Width-one jobs
   run directly on the callback, without wakes or queue traffic. Helpers
   park on individual native 32-bit futex words between jobs; they spin only
   while their claimed job is in flight. No post-cycle spinning is planned.
4. **Narrow wake primitive.** `supple::rt::wake` issues only a nonblocking
   private futex wake on an already published 32-bit atomic. The scoped
   RTSan exception surrounds only that syscall, not job setup, dependency
   handling or DSP. Its companion wait is for idle helpers only and must
   recheck the sequence after EINTR/spurious wakes. Slot sequence updates use
   release and observations use acquire; job ownership uses the stronger
   handshake in point 2. The futex itself conveys no C++ ordering.
   Notifications may coalesce while a helper is delayed; the open/active
   handshake, not a notification, grants access to job state. The callback
   does not notify the background renderer.
5. **Process-local scheduling.** Before enabling the pool, JACK helpers get
   SCHED_FIFO at the callback priority, preferring JACK's documented
   `jack_acquire_real_time_scheduling` API, then pthread scheduling within
   the process's existing RT limit. Verify the resulting policy/priority.
   Discover/setup scheduling off the callback. If safe RT setup or CPU
   capacity is unavailable, retain serial processing and report an actionable
   warning; never silently join lower-priority normal helpers from a FIFO
   callback. Dummy/manual backends use normal scheduling for portable tests.
   RT backends must implement this setup contract. Helper capacity respects
   the allowed CPU set; one allowed CPU disables parallel execution. No CPU
   governor, global graph setting, affinity tuning or system configuration
   changes are part of this decision. External runtime CPU-affinity changes
   require restarting the engine.
6. **Determinism and isolation.** Tasks read completed source buffers and
   write their own nodes; summation order, Console 9 adjacency, sidechains,
   auxiliary channels and all numeric policies remain unchanged. Transport,
   device input and frontier buffers are immutable during a job. SPSC output
   queues still have one producer: their executor after join. Synchronous
   bridge stages remain compensated pass-through with warnings on every
   callback-owned node, including nodes run by helpers. Offline/anticipative
   bridge processing is unchanged; no live bridge IPC or lifecycle calls.
7. **Control and rollout.** The existing Audio processing threads preference
   controls both pools' capacities (including each pool's submitting thread),
   on the next engine start. Automatic retains background capacity and
   enables bounded live participation once exact-output/sanitizer tests and
   repeated benchmarks pass. Limit 1 is serial for both pools. Explicit
   engine worker counts still override the app preference. The limit is per
   executor, not a total process thread limit; the pools never share jobs.

## Validation and consequences

Test exact output with serial, two and many execution threads, including
live/frontier graphs, sidechains, recording and transport/plan transitions.
Force helper delays before claiming, before execution and before departure
without timing thresholds. Prove replacement/reclamation waits for actual
claims, permits a delayed unclaimed helper, and excludes retired job access
after a late wake. Also exercise an old wake entering a new open job.
Run callback tests under RTSan and scheduling tests under TSan. Measure four
and eight independent live Ferrics plus a serial chain at 64/128, before and
after, and one short JACK four-live-Ferric run at 128. Compare a light live
stock graph with serial execution at both blocks to expose wake/join overhead.
Repeated wins are the gate for Automatic; inconclusive evidence goes back to review.

Fork-join adds wake and synchronization overhead to short parallel graphs.
A stalled plug-in/helper can stall its cycle indefinitely, as a stalled
serial plug-in can today. This does not promise a hard real-time deadline
on a general-purpose workstation. RT permissions may limit availability;
failures never change system policy or acquire elevated privileges.

## Alternatives considered

Calling the background pool would block the callback. Discarding a late
node would leave its plug-in state advanced and break equivalence. Permanent
busy-wait helpers waste cores between cycles. Running ordinary helpers under
a FIFO callback risks priority inversion. A pipelined live bridge changes
latency and remains a separate design.

JACK thread setup follows its public [client-thread API documentation](https://jackaudio.org/api/group__ClientThreads.html).
No third-party implementation source was consulted.

## Implementation (2026-10-08)

`LiveWorkers` implements the separate callback executor; core-base supplies
`rt::wake`/`rt::wait`. JACK prepares idle helpers after backend start and before
atomic pool publication. The existing Audio processing threads preference
controls both pools; `engine.status` exposes enabled live capacity and warnings.
Exact-output, forced-interleaving, RTSan and TSan checks pass. Repeated four/eight
live Ferric measurements show a substantial callback improvement; a short
128-frame JACK trial has zero xruns with FIFO-83 helpers. Serial-chain overload
and occasional paced parallel tails remain; Automatic enables live processing,
with no claim of a universal latency improvement or long-run stability.
