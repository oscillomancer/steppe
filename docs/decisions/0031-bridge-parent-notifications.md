# ADR-0031: Bridge-to-parent notifications and idle audio parking

**Status:** Accepted, 2026-10-08. Amends ADR-0013; builds on ADR-0029's
core-base relay and coordinates with ADR-0030's main-thread scheduling.
Contracts: sandbox spec §1 and engine spec §5.

**Amendment, 2026-10-14:** [ADR-0037](0037-native-plugin-bridge.md) enables
native macOS/Windows bridge transport, shared wakes and child ownership.

## Context

A bridge services its own callbacks, timers and descriptors on events, but
its parent used periodic Service calls and a 30 ms editor request poll to
learn about the resulting changes. Removing the app poll would strand those
notifications. Child audio threads also woke every 100 ms solely to inspect
quit. The synchronous control channel cannot accept unsolicited frames, and
sharing the child's own relay fd would let the two readers steal wakes.

## Decision

1. Protocol v11 adds one separate outward nonblocking eventfd per process,
   passed parent-to-child using SCM_RIGHTS in a mandatory versioned subscription
   before Create, and a process-level ACK operation. Validate payloads and
   descriptor counts/type; reject mismatches even for inactive instances.
   Keep existing synchronous frames and operation numbers, appending the two
   operations. No asynchronous frames or new command schema are needed.
2. Child main captures newly produced parent-visible notifications, idle
   outputs, editor requests and restart/flush/latency/tail transitions into
   retained snapshots and advances a publication generation. Capturing at the
   bridge boundary preserves the in-process callback paths. Only new work
   signals; retained flags, queries and repaint-only timers do not. The child
   writes once and latches until ACK. Audio producers keep using atomic queues
   and core-base's `rt::wake` relay; no audio fd writes are added.
3. One parked parent watcher per BridgeProcess observes that outward fd,
   pidfd/control HUP and an internal failure/stop relay. It forwards to the
   subscribed app relays, deduplicating shared-instance subscriptions.
   `Instance::setMainThreadWake` works for remote instances, including an
   initial snapshot. Main ACKs a drained notification before refreshing all
   registered shared-process members, including retired/editor instances.
   Per-instance consumers drain proxy caches. New work during snapshotting
   schedules another event; ACK never consumes the work.
4. Remote editor request reads consume snapshots; remote nextTimerMs is -1.
   Child timers remain child deadlines, with no replacement parent poll.
5. An atomic parent failure also notifies the internal core-base relay, so
   malformed output from an alive child wakes main without requiring death.
   Death/HUP is published once and its level-ready watches are disabled.
   Synchronize subscriber removal with forwarding, join the watcher before
   closing descriptors, and keep the internal relay alive through that join.
   Setup failure is structured/actionable, never a timer fallback.
6. Idle child audio uses an indefinite shared futex wait. Shutdown publishes
   quit, advances the request word and wakes before joining; audio rechecks
   quit before cmd access. Advancing the word closes stop-before-wait races.
   Control HUP follows this shutdown path. Parent IPC deadlines and bounded
   in-flight failure checks retain ADR-0013's existing safety limits.

## Consequences and validation

The app sees only its existing relay fd; no engine/frontend bridge-watch API
is added. Snapshot/output caches are bounded with drop accounting and preserve
clear/rescan ordering. The process watcher plus internal failure relay cost
two parked parent threads; a shared watcher can consolidate them later without
changing the app contract. Explicit servicing without subscription remains
available, and scan-only bridge mode is unchanged.

Validate publication/ACK races and no query feedback with counters, shared
members, pre-subscription outputs, remote GUI requests, live-child protocol
failure and idle death, malformed/version/descriptor boundaries, subscriber
lifetime and stop-before/during-park interleavings. No pass/fail threshold is
based on elapsed time. Run relevant host tests under dev, RTSan and TSan;
retain exact-output coverage in the existing bridge/hosting tests.

*Amended 2026-11-15:* [ADR-0083](0083-local-crash-reports.md) adds external local crash diagnostics.
Existing bridge deadlines, notification framing and recovery ownership remain authoritative.
