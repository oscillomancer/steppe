# ADR-0089: A server owns its control endpoint by holding its lock

**Status:** Accepted, 2026-11-16. Amends ADR-0087 (D6). Contract:
`docs/specs/command-bus.md`, transport limits.

## Context

ADR-0087 left the endpoint lock as it was: taken with an unbounded wait, held
only while startup inspected and bound the socket path, then released. Whether
another server was alive was decided by a nonblocking connect probe.

Native macOS CI showed the probe cannot decide that. macOS refuses a connect to
a listener whose backlog is full exactly as it refuses a connect to a dead
socket file. A second server therefore judged a live but busy server stale,
removed its socket and bound over it.

## Decision

1. A server takes an exclusive lock on `<socket>.lock` before it inspects the
   socket path and keeps it until it has stopped and removed its socket.
2. The lock is taken without waiting. A lock held elsewhere is
   `rpc.address_in_use`, whatever the socket answers.
3. The lock, not the connect probe, proves a live owner. The probe still runs
   with the lock in hand and still refuses a connected, pending or (on Linux
   and Windows) full listener, which protects a listener that holds no lock.
4. The operating system releases the lock when its owner dies, so a crashed
   server's socket is reclaimed by the next start. On Windows the lock is
   unlocked explicitly before its handle closes: closing alone releases it at a
   time the system chooses, and a successor that never waits must find it free.
5. The lock file is never removed, which avoids a race between unlinking and
   relocking it.

## Consequences

The unbounded wait ADR-0087 recorded is gone: startup either owns the endpoint
or returns at once. Two servers started together leave exactly one owner.

On macOS a listener that does not hold the lock is protected only while its
backlog has room. Every product server holds the lock, so this concerns
foreign listeners on a product socket path only.

On Windows the system drops a dead process's lock promptly but not at a
guaranteed moment, so a start made the instant after a crash can be refused
with `rpc.address_in_use`; a retry succeeds. Linux and macOS release it as the
process dies.

A lock file remains beside each socket path after the server stops.
