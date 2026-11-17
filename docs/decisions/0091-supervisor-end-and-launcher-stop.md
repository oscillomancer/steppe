# ADR-0091: A supervisor that ends without saying so is the launcher's stop

**Status:** Accepted, 2026-11-17. Amends ADR-0083 (the sentence that a dead supervisor must
not kill a healthy worker). Contract: `docs/specs/diagnostics.md`, "Supervisor end".

## Context

ADR-0083 split every program into a supervisor and a worker and decided that losing the
supervisor must never take the worker down: unsaved work is worth more than crash capture.
That is right for a collector that fails. It is wrong for a launcher that stops the program.

Launchers escalate. A service manager, a test harness or a user sends a graceful stop,
waits, and then ends the process they started with `SIGKILL` or `TerminateProcess`. The
process they started is the supervisor. On Windows every programmatic stop is
`TerminateProcess` of that one process. The worker survives it, keeps its control socket
and its endpoint lock (ADR-0089), and a restart of the program fails with
`rpc.address_in_use`. Review round 35 showed it, and that a hard kill in a terminal leaves a
GUI running with no one supervising it.

The two events can be told apart. A collector that fails does so through the supervisor's
own fatal handlers, which run before the process dies. A launcher's kill leaves nothing.

## Decision

1. The supervisor's own fatal handlers tell the worker before the process dies. On POSIX
   the handlers for `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGABRT` (the same set
   the worker's cover, on an alternate stack, so a stack overflow still reports) send one
   byte, `F`, and re-raise. On Windows the top-level exception filter and the abort signal
   set one flag in the channel's shared block, `collectorFailed`, and chain on.
2. A worker watches its channel from one thread started after its Ready packet, never on
   the audio path. When the supervisor is gone:
   - with the notice, it is **collector loss**: the worker keeps running, as ADR-0083
     decided;
   - without it, it is **the launcher's stop**: the worker takes the stop path a forwarded
     `SIGTERM` takes (POSIX `kill(getpid(), SIGTERM)`, Windows `raise(SIGTERM)`), which the
     programs' model loop turns into an orderly shutdown, and if the process has not
     ended within ten seconds it ends hard (`_exit(128 + SIGTERM)`, `TerminateProcess`).
3. Anything else seen on the channel ends the watching and does nothing: the watcher
   never consumes a byte, never spins, and never stops the program on a signal it does not
   understand. A worker whose terminal handler has begun is not also stopped.
4. The decision is made from what happened and never from timing. The ten seconds is
   only the bound on a stop that does not finish.

## Consequences

A hard kill of the supervisor, from any launcher on any platform, stops the program: the
control socket and endpoint lock are released and a restart succeeds. A collector crash
still leaves the worker running without crash capture, as before.

When the worker's stop completes it removes its own `.run` marker, which the dead supervisor
cannot (decided on review, 2026-11-17), so that a later start does not read an orderly stop as an
unexpected end. On the hard-exit path the marker stays.

The program that is stopped this way ends as `SIGTERM` ends it today. A GUI session does not
ask about unsaved changes and does not offer a save, exactly as when `SIGTERM` stops it
today; the maintainer accepted that and it is recorded in `STATUS.md` as known behaviour.

Known limits, stated because they read as the launcher's stop: on Windows a fast-fail
(`__fastfail`, `RaiseFailFastException`) runs no filter, and a stack overflow too deep
for the filter to run does not set the flag; on any platform a supervisor ended by the
kernel (the out-of-memory killer) leaves no notice. Before the Ready packet no watcher
exists, so a supervisor killed in the first milliseconds leaves what it left before. The
watcher thread appears in a report's thread list as an `other` thread parked in a wait.
