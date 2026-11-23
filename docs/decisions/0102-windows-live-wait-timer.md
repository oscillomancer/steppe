# ADR-0102: Windows live bridge waits end on a high-resolution timer

**Status:** Accepted, 2026-11-23. Amends [ADR-0081](0081-live-bridged-plugins.md)
§2 and §3 on Windows. Contract: `docs/specs/engine.md` §5 (live deadline).

## Context

ADR-0081 lets the device callback wait for a bridged instance's answer up
to the cycle's bridge deadline: its start plus the larger of its period and
2 ms. On Windows that wait was `WaitForSingleObject` with the time left in
whole milliseconds, which ends on the system timer's tick: 15.6 ms by
default. Native CI measured the longest live wait at 15.7 ms against a
2.67 ms deadline (run 45139875012), so each late cycle blocked about six
periods, and four of them reached the 50 ms late rule that fails a hung
bridge. Steppe does not raise the system's timer resolution
(`timeBeginPeriod`): it is process-wide on older Windows and costs power
for every process (and the system is the user's, AGENTS.md rule 4).

## Decision

1. The bridge client creates a high-resolution waitable timer
   (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) with each instance's events,
   on the main thread at setup, and closes it with them.
2. A timed wait sets that timer to the time left (relative, 100 ns units)
   and waits on the done event and the timer together. It ends at the
   answer, or at the deadline plus the timer's lateness.
3. Without that timer (Windows before 10 1803, where creating one fails),
   a timed wait blocks in whole milliseconds only while more than two
   16 ms ticks are left, and for the rest spins on the done word, looking
   at the event without waiting, until the deadline.
4. Both stay inside `rt::ScopedSanctionedSyscall`, as the wait they replace.
   The background waits (ADR-0013) use the same path.

## The bound

In the callback a bridged wait ends by the cycle's bridge deadline plus the
high-resolution timer's lateness (the scheduler's, well under a period on
the runners measured), or, without that timer, at the deadline, the spin
having used the cycle's own time. Unbounded waits (no timeout) are
unchanged.

## Consequences

- A late cycle on Windows ends near its deadline, as on Linux and macOS, so
  the 50 ms late rule counts the same cycles everywhere.
- Without the timer the callback spins for at most its deadline (one
  period, or 2 ms), only while a bridge has not answered.
- Tests: on Windows, with and without the timer, a wait not woken times out
  and a woken one returns (outcomes only); the hung-bridge case prints the
  longest live wait, read from native CI rather than asserted.
