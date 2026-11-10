# ADR-0013: Plugin process isolation

**Status:** Accepted, 2026-09-26 (dedicated and shared bridge modes implemented). Spec: `docs/specs/engine.md` §5,
`docs/specs/session-model.md` §1.7 (`FxInstance.isolation`).

**Amendment, 2026-10-07:** [ADR-0024](0024-live-multiprocessing.md) defines
live callback fork-join, helper scheduling and the nonblocking wake exception.

**Amendment, 2026-11-10:** [ADR-0068](0068-plugin-midi-output.md) carries
plug-in note and MIDI output through the data plane and passes the host
sink's room to the bridge (protocol POSIX 20 / Windows 21).

**Amendment, 2026-10-08:** [ADR-0026](0026-background-thread-scheduling.md)
gives a bridge's audio thread the engine's background scheduling (protocol
version 10).

**Amendment, 2026-10-08:** [ADR-0031](0031-bridge-parent-notifications.md)
adds protocol v11 parent notification/rearm and event-driven idle audio shutdown.

**Hosting amendment, 2026-10-14:** [ADR-0036](0036-native-plugin-hosting.md)
adds native in-process loaders and a standalone supervised scanner, while
retaining the Linux audio bridge and the remaining engine/device/GUI gates.

**Amendment, 2026-10-14:** [ADR-0037](0037-native-plugin-bridge.md) enables
native macOS/Windows bridge transport, shared wakes and child ownership.

**Editor amendment, 2026-10-14:** [ADR-0040](0040-native-plugin-editors.md)
adds native local editors, Windows bridged embedding and Cocoa bridge-owned
floating windows.

## Context

Phase 4 acceptance: a crashing test plugin in an isolation mode does not take
down the engine. The model already carries `isolation: inherit | in_process
| shared_bridge | dedicated` per fx. The engine talks to plugins through
`host::Instance` (clap-host) everywhere: plans, the instance pool, patch
validation, offline renders. With anticipation (ADR-0012) plugins run on the
renderer and worker threads, not in the device callback.

## Decision

1. **Same interface, two backings.** `host::Instance` keeps its public API;
   its implementation moves behind an internal backing interface with two
   implementations: *local* (today's in-process CLAP host) and *bridged* (a
   proxy to an instance in a bridge process). `Instance::create(info, host,
   mode)` picks one. Plans, the pool and the engine do not change.
2. **The bridge executable** (`<slug>-bridge`) hosts plugins with the same
   clap-host library (local backing inside). Dedicated mode: one process per
   instance. Shared-bridge mode: one process for every instance in that mode
   (a crash takes down all of them — the user's choice).
3. **Control plane:** a Unix `socketpair` created at spawn (the child's end
   inherited as a known fd), carrying length-prefixed binary messages for
   the main-thread API (create, activate/deactivate, params, text, semantic
   schema/patch/describe, state, latency/tail, notifications and log
   drain). Requests have one monotonic end-to-end deadline covering nonblocking
   socket sends and receives; the bridge runs them on
   its main thread and services its plugins' main-thread requests itself.
4. **Data plane:** one `memfd` region per instance, mapped by both sides:
   a header (request/done sequence numbers, frames, flags, result), the
   transport, input and output audio (channels × max frames), an input event
   arena and an output event arena (fixed capacity; overflow dropped and
   counted as today). Lifecycle calls that CLAP puts on the audio thread
   (`start_processing`, `stop_processing`, `reset`, active params flush) travel as
   commands in the same header, so they run on the bridge's audio thread in
   order with `process`. Inactive params flush runs on the main thread.
   Both idle flush paths retain their outputs in a bounded queue, drained on
   main through `TakeIdleOutputs` on the control plane.
5. **Synchronous processing with a deadline.** The caller (the renderer or a
   worker) writes inputs, bumps the request sequence, wakes the bridge's
   audio thread (futex on a 32-bit word in the shared region), and waits for
   the done sequence — spinning briefly, then a futex wait bounded by a
   deadline (2 s for `process`, 7 s for lifecycle commands: long enough that
   a loaded machine never gets a healthy plugin killed). Output is bit-identical with the
   in-process backing, and PDC is unchanged. Live nodes pass bridged stages through with a warning;
   synchronous bridge calls never run in the device callback. A pipelined
   live mode remains deferred.
6. **Failure.** The parent watches each bridge with a `pidfd` and the control
   socket. A bridge that dies, or misses the deadline and is then found dead
   or killed, turns its instances *failed*: `process()` outputs silence and
   returns success (the rest of the session keeps playing), `failed()` is
   true with the signal, and the engine reports `fx.failed` on the main
   thread. A hung bridge (alive past the deadline) is killed and failed the
   same way. `fx.restart {fx}` re-creates the instance
   from the model's last known patch/state (the model is the source of
   truth; nothing is lost that was committed).
7. **Test plugin.** The clap-host fixtures gain `com.suppleaudio.test.crash`:
   an effect that dereferences null in `process()` when its `crash`
   parameter is on (and can hang instead, for the deadline path).
8. **Defaults.** `inherit` resolves to the project's default isolation
   (`in_process` until the bridge has shipped a release of testing).

## Consequences

- Each bridged `process()` costs two context switches; the benchmark (B6)
  measures it against in-process for 16 instances.
- GUI embedding across processes (X11 XID to `gui.set_parent`) rides the
  control plane later; parameter automation and state already do.
- Sanitizer presets build the bridge too; the data plane is lock-free on
  both sides except the futex waits.

## Alternatives considered

Pipelined by default (adds a block of latency to every bridged plugin; no
need while the whole graph is anticipative); one shared memory region for a
whole bridge (simpler mapping, but one instance's overflow or crash of the
parser could corrupt another's buffers); sockets for audio (copies and
syscalls per block for the data, not just the wake-up).

## Implementation notes (2026-09-26)

- `host/clap-host/src/bridge_protocol.{h,cpp}` (frames, SCM_RIGHTS, the
  shared-memory Header/Layout, futex helpers), `bridge_client.{h,cpp}`
  (`RemoteInstance`: spawn with `posix_spawn`, pidfd, control requests with
  a timeout, audio-thread commands with a 2 s deadline (lifecycle commands 7 s); kills go through
  `pidfd_send_signal`, so a recycled pid is never signalled; the exit status
  is reaped on the main thread), `bridge/bridge_main.cpp` (`<slug>-bridge`).
  `Instance::create(info, host, isolated)`; every `Instance` method forwards
  when bridged. The one audio-thread syscall exception is scoped with
  `rt::ScopedSanctionedSyscall` (rtsan).
- Latency and tail come back with every `Service` reply (a restart inside
  the bridge may change them without a notification), with whether a restart
  waits for processing to stop (protocol version 7).
- Engine: `InstancePool` creates bridged instances for `dedicated` and (for
  now) `shared_bridge`; replaced or restarted instances are retired and freed
  at the next safe prune. `fx.set {isolation}`, `fx.restart {fx}`,
  `engine.status.failed_fx`, event `fx.failed {fx, reason}`.
- Tests: bridged ≡ in-process (params, text, patches, errors, state,
  latency restart, bit-identical processing with sample-accurate events);
  crash → failed + silent, calls fail with `host.bridge_failed`; hang →
  killed at the deadline; missing bridge → clean error; the Phase 4
  acceptance test (a session plays on while an isolated plugin crashes; only
  its node goes silent); crash → `fx.failed` → `fx.restart` over the bus.
- Shared bridge: each control frame carries an instance id. `Create` allocates
  one id; `Destroy` removes just that instance. The process stays ready for a
  later `Create` even after its last `Destroy`, and exits when the host closes
  the control socket. Each instance has its own memfd and
  bridge audio thread. `InstancePool` holds one `BridgeProcess` for all
  `shared_bridge` instances and creates a new process after a crash; dedicated
  instances continue to own independent processes. The shared process's
  failure state is observed by every member instance.
- Shared mode has one synchronous control channel. A slow plugin callback or
  control request stalls control calls for the other instances; a timeout
  fails the whole shared process. Use `dedicated` for plugins whose slow
  control callbacks need isolation from other effects.
- Protocol v6 retains the v5 operation numbers and appends `TakeIdleOutputs`.
  Its shared-memory output-event ABI is extended before v6's first release:
  the pointer-free 40-byte `OutputEvent` retains the full CLAP flags and
  note/parameter target `(port, channel, key, note_id)`, including -1 wildcards.
  `Layout` uses the new size for its output arena and following audio offsets;
  both endpoints must be built together. There is no released v6 ABI to preserve.
  Idle flush output queues hold 1024 events, dropping newest on overflow;
  `TakeIdleOutputs` drains FIFO events and the dropped count since the previous
  drain. Its reply encodes `u64 dropped`, `u32 count`, then each event's
  `u8 kind`, `u32 time`, `u32 paramId`, `f64 value`, `i32 noteId`, `u32 flags`,
  `i16 port`, `i16 channel`, `i16 key` without native padding. Count is bounded
  at 1024; invalid kinds, truncation or trailing data are protocol violations.
  Flush event times are 0. Process output storage and idle queues are separate;
  a main-thread drain never consumes the process arena.
  Error payloads carry `code`, `message`, `path`, `hint`, then a count and
  recursively encoded `details` (decoder depth limited to 32). Malformed
  replies fail the operation with `bridge.malformed` and fail the shared
  process, like a crash. Malformed requests receive a `bridge.malformed`
  error without invoking the operation. `Service` also carries an optional
  structured restart activation error; `EditorRequests` carries show/hide.
  After `paramsRescanFlags`, `Service` carries `u8 processRequested`, then
  `u32 clearCount` and ordered `{u32 paramId, u32 clearFlags}` pairs. The child
  consumes the wake request; the proxy keeps it in an atomic until its audio
  owner consumes it. Both the reply and pending proxy list are bounded at
  4096 clears; exceeding the bound reports `bridge.notifications_limit`.
  A rescan supersedes earlier clears, including those retained by the proxy.
  Each request uses a single steady-clock deadline across the complete send
  and reply, including partial transfers. A missed deadline kills and fails
  the process; it is never renewed when another fragment arrives.
- Editors (protocol v6, gap log #39): `EditorOpen {parent XID, scale}`,
  `EditorResize`, `EditorClose`, `EditorRequests`. The bridge embeds the
  plug-in's editor (CLAP gui or VST3 IPlugView) straight into the host's X11
  window — window ids are the X server's, so no reparenting proxy is needed —
  and runs the editor's timers and watched descriptors in its own poll loop.
  The host polls the editor's requests (resize, closed, show, hide) every 30 ms while it is
  open; a failed bridge reports the editor closed, and the window goes with it.
  `Create` replies whether the plug-in has an embeddable editor. Both apps find
  `<slug>-bridge` beside themselves or in the build tree
  (`locateBridgeExecutable`).
- Current live limit: the synchronous bridge is never processed or given
  lifecycle calls from the device callback. Bridged stages on live nodes pass
  through with their compensated delay, and the plan warns with the FX id and
  `isolation: in_process` for monitoring. Offline and rendered-ahead processing
  remain synchronous. Not yet: pipelined live bridging, B6 overhead benchmark.

## Supervised discovery (2026-10-04)

Third-party discovery also uses the bridge: `<slug>-bridge --scan <module>`
loads one CLAP binary or inspects one VST3 bundle on its main thread and writes
a canonical `supple.plugin-scan/1.0` JSON reply (`plugins` or structured `error`).
This scan-only mode does not create a control socket, audio thread or shared
memory. Plugin stdout is redirected to `/dev/null`; the reply uses a saved
stdout descriptor so plugin init/deinit logging cannot corrupt it.

The parent starts one child per canonical module in search-path order with
`posix_spawn`, a private process group and no inherited descriptors above
stderr. Stdin/stderr use `/dev/null`. It drains stdout nonblocking, bounded at
1 MiB by default, with one steady-clock deadline (default 5 seconds, configurable
through `host::ScanOptions`). A signal exit, deadline, excessive output or
invalid JSON/metadata is a structured per-module failure, not a failure of the
whole catalogue. Every exit path closes the pipe, kills the process group
(including descendants holding stdout), and reaps the leader. The leader is
observed using `waitid(WNOWAIT)` and remains unreaped until cleanup, preventing
its PID from being recycled before the group kill.

Successes and persistent failures are atomically cached as canonical JSON in
`$XDG_CACHE_HOME/<slug>/plugin-scan` (fallback `$HOME/.cache/<slug>/plugin-scan`).
Records compare the complete canonical module path, file size and mtime. VST3
bundles compare a sorted manifest of regular files with sizes and mtimes,
including the binary and `moduleinfo.json`; linked folders are followed with
directory-cycle detection. A filename hash is only an index:
the record's full key must match. VST3 load paths retain the discovered bundle
alias: the SDK derives the binary filename from its bundle name, so blindly
loading its canonical directory can break a symlinked install. Cache metadata
uses canonical paths internally and is rebound to the current alias after
validation. Corrupt cache records cause a rescan; module changes during a probe
prevent writing its result to the cache. Spawn/I/O
failures are transient and are not cached. Cached failed modules are skipped
until a file change, including when the bridge is later unavailable.

The build's own stock binaries beside the app can still scan in process.
`ScanOptions::forceInProcess` is a test hook that bypasses supervision and the
cache. If the executable is missing/non-executable, new probes fall back to
in-process scanning with a stderr warning and a structured warning in the
report, preserving the available catalogue.

The compatible `scan(paths)` result still contains only available plugins.
`scan(paths, options, report)` and `lastScanReport()` expose failed module paths,
structured reasons, whether each failure was cached, warnings, and the number
of cache hits. Catalogue owners must retain the report alongside their plugin
list and expose failures/warnings to callers; a later unrelated scan on the
same thread replaces `lastScanReport()`. Sandbox `Session::pluginScanReport()`
retains its catalogue report. EngineApp also retains its catalogue report. Engine `plugin.list`/`plugin.scan`
and sandbox `plugin.list` expose it as the `scan` result object, including for
an empty available catalogue.
