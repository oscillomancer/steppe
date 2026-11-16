# ADR-0035: Platform mechanisms for the headless core

**Status:** Accepted, 2026-10-14. Amends the mechanisms in ADR-0024,
ADR-0025, ADR-0026, ADR-0028, ADR-0029 and ADR-0030; their Linux behavior
and publication/lifetime rules are retained.

**Hosting amendment, 2026-10-14:** [ADR-0036](0036-native-plugin-hosting.md)
adds native in-process loaders and a standalone supervised scanner, while
retaining the Linux audio bridge and the remaining engine/device/GUI gates.

**Audio-file amendment, 2026-10-14:** native Windows recording/recovery and
RF64 publication lift the POSIX audio-file gate; see engine.md's Native audio
files contract. Device audio, bridge and GUI remain separate gates.

**Native device amendment, 2026-10-14:** [ADR-0038](0038-native-device-audio.md)
adds PortAudio on macOS/Windows and device-derived, self-applied helper policies;
Linux mechanisms and callback publication/lifetime rules remain unchanged.

## Decision

The first port target is the headless engine, dummy/offline backends,
command bus, control transport, sandbox and MCP. macOS targets arm64 and
14.4 or later; Windows targets clang-cl and Windows 10 1803 or later.
Audio renders remain deterministic within each platform and equivalent
between its executors; cross-platform bit identity is not required.
Hosting/scanning, the bridge, device backends and GUI embedding are separate
ports. This layer does not claim those subsystems work outside Linux.

- `rt::wait`, `waitUntil` and `wake` keep their 32-bit atomic and one-waiter
  contract. Linux uses the existing private futex calls; macOS uses public
  `os_sync_wait_on_address` APIs with private flags and relative monotonic
  timeouts; Windows uses WaitOnAddress/WakeByAddressSingle. Only the wake
  call is sanctioned under RTSan. Waits stay off the device callback.
  These are process-local primitives: a bridge must supply its own shared
  wake mechanism (Darwin shared flags or a Windows cross-process event).
- `Waitable` borrows a descriptor on POSIX or a manual-reset event HANDLE
  on Windows. `waitFor` takes a readiness snapshot with an optional timeout:
  poll on POSIX, WaitForMultipleObjects on Windows. Windows currently admits
  64 unique handles. Native socket events may be represented by event handles;
  ownership, socket registration and reset belong to their producer.
  A bridge can pass/duplicate an event handle without changing consumers.
- MainThreadWake preserves its atomic pending/sequence relay. Its non-RT
  thread signals eventfd on Linux, a nonblocking close-on-exec pipe on macOS,
  or an unnamed manual-reset event on Windows. Audio producers never write
  a descriptor or signal an event handle directly. `fd()` remains a POSIX
  compatibility accessor; new code uses `waitable()` and checks `valid()`.
- ModelLoop retains its Linux signal/wake pipe. Elsewhere it uses the same
  native event abstraction. Watch cancellation, posted tasks and absolute
  deadlines preserve ADR-0030. MCP watches the same typed sources. On
  Windows a bounded 64 KiB synchronous input reader wakes the serving thread;
  cancellation and join finish before returning. Serving-thread dispatch
  and partial/continuous-input fairness stay unchanged. Interactive Windows
  console reads can require a keypress at teardown; pipe/file input is the
  supported MCP transport for this stage. Windows signal handlers claim the
  wake handle before using it; teardown clears it and joins existing claims.
- Control sockets remain AF_UNIX and newline JSON-RPC, with Winsock on
  Windows. Socket handles retain pointer width, errors map to the existing
  structured transport errors, and close-on-exec/noninheritance and SIGPIPE
  suppression are platform-specific. Endpoint locking uses flock or
  LockFileEx. Cleanup checks device/inode or volume/file identity. macOS's
  sun_path is 104 bytes, including the terminator. Windows paths are UTF-8;
  access follows the inherited ACL of the user profile directory instead of
  POSIX modes. Explicit socket paths require a suitably private directory.
- DSP nodes use MXCSR FTZ/DAZ on x86 and FPCR FZ on arm64, restoring the
  complete control register after each node. Lifecycle scope is unchanged.
  Stack first-touch uses platform stack bounds and the existing 128 KiB
  top-down touch with a 512 KiB remaining-stack check.
- Linux naming, affinity sizing, FIFO policies and rlimit checks are unchanged.
  Windows `High` means verified THREAD_PRIORITY_HIGHEST; macOS `High` means
  user-interactive QoS, applied by the owning thread. `High` is not a FIFO
  or real-time guarantee. Attempts to set another thread's QoS fail with an
  actionable error; the engine falls back to normal. Device backends must
  provide safe helper setup before enabling live parallelism. Memory locking
  with future/on-fault semantics and CPU idle-latency requests are Linux-only;
  other platforms report ENOTSUP when these opt-in settings are applied.
- Engine file helpers retain Linux reflink/link publication and stat identities.
  macOS copies into temporary files and keeps link publication, with its native
  timestamp fields. Windows publishes temporary files through a non-replacing
  MoveFileEx and uses volume/file ID, size and last-write time for media caches.
  Serialized media hashes and formats are unchanged.
- `paths::{home,config,data,cache,runtime}` supplies platform roots, with no
  product name and no filesystem mutation. Linux retains XDG/HOME rules;
  macOS uses Library/Application Support and Library/Caches; Windows uses
  APPDATA and LOCALAPPDATA. The product slug is appended by callers.

## Validation and remaining gates

Portable tests cover wake publication/rearm/shutdown, native event readiness,
path selection, model-thread cancellation and timers, control handshake,
command/error delivery, address ownership and path bounds, and MCP partial
input/deadline/buffer fairness. A new arm64 test checks nested FPCR restoration.
Linux-only tests inspect mincore/madvise, affinity, rlimits, raw descriptor
backpressure and Unix signal interruption. Native macOS/Windows CI is required
before declaring those implementations operational. CMake/toolchain gates and
shared compiler fixes are owned by the CI port.

API references: [Apple address waits](https://developer.apple.com/documentation/os/os_sync_wait_on_address),
[Windows address waits](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress),
[Apple QoS](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/PrioritizeWorkWithQoS.html),
[Windows AF_UNIX](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/).

## Build integration (2026-10-14)

Native core-base implementations/tests, control-protocol and standalone MCP/proxy
build independently of the hosting port. The former broad platform-services gate
is narrowed and renamed `SUPPLE_HOST_SERVICES`: hosting/scanning, bridge, sandbox,
engine/device backends, GUI/agent and plugin wrappers remain Linux-only. The
Windows POSIX audio-file gate is unchanged. Linux target/test coverage is retained.

**2026-10-16 note:** The Windows minimum is now Windows 10 1903 (ADR-0044).

**2026-11-16 note:** [ADR-0087](0087-control-boundary-budgets.md) adds local
control/MCP budgets, explicit request delivery provenance, fd-backed output
supervision and runtime-directory validation. Its staged implementation retains
Windows inherited ACLs and the existing audio-thread rules.
