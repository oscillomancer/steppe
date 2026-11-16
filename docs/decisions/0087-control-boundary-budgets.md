# ADR-0087: Bounded local control input and output

**Status:** Accepted, 2026-11-16. Amends ADR-0007, ADR-0035 and ADR-0079.

## Context

The control socket and MCP endpoint are local, same-user robustness boundaries.
The control audit demonstrated retained disconnected work, parsed-memory
amplification, blocked model service on stdout, ambiguous retry provenance,
oversized error replies and linked runtime-directory permission changes.
Client compatibility may be broken to enforce explicit limits.

## Decision

Implement these independently reviewable changes in order D4, D5, D7, D3, D1,
D2, D6. Specs and STATUS describe the implemented stage in each commit.

- **D4:** RPC call results carry local delivery provenance: unsent means no
  complete newline-terminated request was transmitted, sent means it may have
  executed. Handshake delivery does not imply delivery of the requested command.
  Retry only proved-unsent requests; remote error codes never authorize replay.
- **D5:** Request IDs are at most 1,024 serialized JSON bytes. Refuse longer IDs
  at the request boundary with a null-ID structured error. Recheck final fallback
  response bounds; never truncate an ID.
- **D7:** Optional decoders continue accepting null as omitted. Exported schemas
  deliberately describe a narrower client subset on this point and do not grow.
- **D3:** Untrusted RPC/MCP frames admit at most 1,000,000 JSON values, counted
  before constructing the next value. Exceeding the budget yields a structured
  error. Trusted file parsing retains its existing defaults. Avoid redundant
  params copies; do not catch bad_alloc.
- **D1:** One server-wide budget holds at most 1,024 posted-but-unexecuted
  requests and 64 MiB of frame bytes, including disconnected owners. Release on
  execution or destruction. Refuse excess work with structured rpc.busy and a
  retry-later hint when the peer is alive. Admitted requests still execute after
  disconnect. Each peer retains the existing 16-request limit and at most 64
  subscriptions; no additional fairness scheme.
- **D2:** FD-backed MCP has one writer thread and a bounded 64 MiB output queue.
  Model-thread enqueue never waits for I/O or queue space. Saturation pauses
  stdin admission until output drains, without dropping replies or stopping model
  service. EOF/exit joins the writer on every platform. The stream/ostream API
  remains synchronous and is for tests/embedding only.
- **D6:** Refuse linked or foreign-owned product runtime directories. POSIX opens
  without following links, verifies ownership and changes mode through the owned
  handle. Windows refuses reparse points and retains inherited ACLs from ADR-0035.
  Errors name the path and give corrective instructions. A server holds its
  endpoint lock from startup until it has stopped, and takes it without waiting:
  a lock held elsewhere is `rpc.address_in_use`. The lock, not the connect
  probe, proves a live owner, because macOS refuses a connect to a full backlog
  exactly as it refuses a dead socket.

## Consequences

These are off-RT transport/model operations; no audio callback allocates, locks,
blocks or performs file/network I/O. No dependency, elevation or system-wide
configuration is added. Linux, macOS and Windows share limits and delivery rules;
native implementations and tests cover their I/O/lifecycle mechanisms.

Budgets bound admission, not execution cost of arbitrary valid commands. A sent
but unanswered edit may have executed. Resource errors must not silently discard
admitted work or trigger replay. Native CI remains required before merge.
