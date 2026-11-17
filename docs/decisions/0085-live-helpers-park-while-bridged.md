# ADR-0085: Live helpers park while a bridged call blocks

**Status:** Accepted, 2026-11-16. Amends [ADR-0024](0024-live-multiprocessing.md)
§3 (helpers spin while their job is in flight) for jobs with bridged calls
under [ADR-0081](0081-live-bridged-plugins.md). Contracts: `AGENTS.md` rule 2,
`docs/specs/engine.md` §2.1 (live pool) and §5 (live deadline).

## Context

Under ADR-0024 a live helper that finds no ready node spins until its job
ends. That is right for in-process work, where a node finishes in
microseconds and a parked helper would wake late. With bridged plug-ins on
live nodes (ADR-0081), a node can instead wait up to the cycle's deadline for
another process. The helpers idle behind it keep spinning at the callback's
real-time priority, so they hold CPUs the bridge's thread, or the waiting
thread once answered, may need. An experiment (e36f0781) parked helpers after
a set idle time in any job; it changed in-process jobs too, and needed a
tuning value.

## Decision

1. **Park only while a bridged call blocks.** Each live pool keeps a count
   of its job's threads blocked on a bridged answer
   (`host::LiveBridgeWaiters`, set on the callback and on each helper for the
   job). A call under a live deadline that is not answered within its spin
   counts itself for as long as it then blocks (`host::LiveBridgeBlocked`).
   An idle helper parks on its own futex word only while that count is
   nonzero; otherwise it spins as before. The callback never parks.
2. **Wakes.** A blocked call's end wakes every parked helper. A thread that
   makes a node ready wakes as many parked helpers as nodes it pushed, best
   effort: one parking at that moment may miss it, and then runs no later
   than the blocked call's end, while the pusher runs what it pushed. The
   helper sets its parked bit, fences, then reads the count; the call
   decrements the count, fences, then reads the bits. So either the helper
   sees no blocked call and does not wait, or the call's end sees it. A job's
   last node finishes after every wait in it, so no helper is parked past its
   job, and the join of ADR-0024 §2 is unchanged.
3. **In-process jobs are unchanged.** With no bridged call blocking, an idle
   helper reads the count once per spin (relaxed), and a pusher reads the
   parked bits once (relaxed), from cache lines nothing writes. Nothing else
   differs from ADR-0024, and the in-process callback's p99 is the same
   (measured below).
4. **The sanctioned wait.** The helper's wait within a job joins the
   syscalls `AGENTS.md` rule 2 sanctions: on a helper only, only while a
   bridged call in the same job blocks, inside `rt::ScopedSanctionedSyscall`.
   Its wake is the nonblocking `rt::wake` of ADR-0024 §4.

## Consequences

- A blocked bridged call leaves the CPUs of the idle helpers free. When it is
  answered, the thread that waited goes on with its node and the ones it
  makes ready; a woken helper joins after a futex wake and a context switch.
- Only waits under a live deadline count; bridged calls without one
  (ADR-0013) are not counted. Live helpers take the callback's deadline for
  the job (ADR-0081).
- Helpers beyond the 64th never park (the parked set is one 64-bit word).
  The pool has at most usable CPUs minus one, so only a machine with more
  than 65 CPUs has such helpers, and they spin as before.
- The experiment's `EngineOptions::liveHelperParkUs` and the bench's
  `--park-us` are removed.

## Measurements

Release builds of 562c5583 (helpers spin) and this change, interleaved, 30 s
each, Yarrow live on 8 tracks at 128 frames with the callback at FIFO 80, on
a 10-CPU Linux machine with other builds running (load 1.6–2.9):

- In process, 8 pairs: callback p99 211.6 µs with the old pool and 209.3 µs
  with the new (medians 208.5 and 206), no page faults on audio threads.
- Shared bridge, 6 pairs, means: callback p99 376.5 → 328.5 µs, max 1054 →
  647 µs, longest bridged wait 997 → 402 µs, bridge wakes over 200 µs 280 →
  61 per run, live helpers' CPU time 7.3 → 5.2 s, no late calls in either.
  The new pool's helpers switch voluntarily about a third more often: they
  park.
