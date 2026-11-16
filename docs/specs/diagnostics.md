# Local diagnostics (ADR-0083)

## Delivery boundary

The approved nine-topic sequence stops after terminal context (topic 3) for review.
Until later topics land, inventory/backtraces, bridge correlation, restart UI,
hang diagnostics and final release packaging are contracts, not working features.
No crash path attempts a save or normal teardown. No automatic upload exists.

## Frontend/bootstrap contract

`diagnostics::runFrontend(argc, argv, Role, Main)` enters supervision before
frontend initialization. GUI and headless are separate sessions. The public
executable launches itself as a worker using a private inherited native channel;
bootstrap version/magic and kernel endpoint ownership are checked before an
internal worker flag is accepted. When kernel credential inspection is denied,
the owner-checked inherited socket is used only for ready/return acknowledgement;
no worker marker or terminal handler is enabled. Invalid/unavailable bootstrap
degrades to ordinary frontend operation without diagnostic privileges. Hidden bootstrap arguments are removed before
ordinary parsing. User arguments/stdio and handled return status survive. Unused
supervisor stdio handles are closed after spawn so a dead worker closes its pipe.
Supervision failure degrades diagnostics without killing a healthy worker.

A fixed native packet is bounded below PIPE_BUF; version/magic/type/length and
role are validated. Only the inherited peer can send it; there is no named network
listener. `ready` follows private marker/terminal setup, `expected_return` follows
Main's return; terminal cause overrides expected return. The supervisor observes
OS process exit independently. POSIX checks/reaps the worker while parking on
channel readiness; a forked helper retaining the channel cannot defer collection,
and already queued terminal/return packets are drained before classification.
If inherited SIGCHLD policy has auto-reaped the worker, a validated expected-return
packet supplies its handled return code, unless a terminal packet overrides it.
The supervisor does not change that inherited SIGCHLD disposition. An unexpected exit without terminal packet remains
unknown, never guessed into a signal or plug-in attribution. Sanitizer deaths
remain visible and do not get converted into successful app returns.

## Storage and schema

`paths::localData()` returns an uncreated platform root: XDG_DATA_HOME or
HOME/.local/share on Linux; HOME/Library/Application Support on macOS;
LOCALAPPDATA on Windows. No temporary/roaming fallback. Product slug/diagnostics
is private, separate from project/recovery storage. Missing/denied storage retains
a stderr diagnostic and normal app behavior. No user work enters a marker.

A `supple.crash-run/1.0` marker contains only run/build/platform/role, is exclusively
created and owned/locked by its live worker. The supervisor publishes a report
only after a terminal event or unexpected worker exit; ordinary acknowledged
returns remove the marker. An active run is not an abandoned incident.

Report schema `supple.crash-report/1.0`: `incident`, `build` (version/commit/dirty),
`platform`, `architecture`, `role`, `kind`, `code`, `native_fatal_status`, `complete`, `unavailable`,
`threads`, `plugins`, `commands`, `dropped`. Strict size/type/enum validation and
bounded allowlisted formatting apply. Phase-2 reports are honest exit summaries;
phase 3 can attest terminal context but persists no raw address/register bytes.
Thread frames added in topic 4 require executable module identity plus relative
PC, never arbitrary stack words. Cap 4 MiB/report, 128 threads, 128 frames/thread.
Readable bounded software names are allowed; personal/project data and arguments
are forbidden. Never create a minidump to sanitize later.

Atomic private publication uses native wide Windows paths and existing atomic
write/durability primitives. Retention prunes oldest validated owned reports until
count <=20 and total <=50 MiB. Validation checks the implemented role/kind/platform/architecture
enums, native code/cause bounds, nonnegative drop counters and incomplete-report
reasons. Thread/plugin/command arrays must be empty until their later-topic nested
schemas are implemented; nonempty arrays do not authorize retention deletion.
Malformed/link/foreign entries are neither followed
nor treated as authority to delete another file. Run markers/locks and recovery
are not report-retention inputs. Concurrent pruning tolerates entries disappearing
at metadata reads or removal; only not-found errors are benign, while genuine
metadata/removal errors still fail storage. Namespace power-cut durability on Windows has
the same unprivileged limitations as ordinary project publication.

## Terminal behavior

POSIX SEGV/BUS/ILL/FPE/ABRT and Windows unhandled SEH use a fixed reserved capsule.
Terminate records an enum and enters the native fatal path (POSIX abort,
Windows fast fail), without inspecting the exception. Owned
threads reserve alternate/guaranteed stack while cold. POSIX handler operations
are restricted to the async-signal-safe native list. No allocation, file work,
formatting, unwinding, engine cleanup, GUI or plug-in calls. A native terminal-only
handoff/wait has a one-second capture budget and hard five-second bound; failure
of the collector never strands a suspended process. Re-entry terminates. Restore/
forward OS fatal handling; never continue a faulting callback. Mach capability
transfer is unprivileged, with explicit signal/exit-only fallback on refusal.
Windows fail-fast/replaced filters and foreign stack exhaustion may lack context.
Do not override sanitizer-owned terminal diagnostics.

Sources: [POSIX signal safety](https://man7.org/linux/man-pages/man7/signal-safety.7.html),
[Microsoft last-chance filter](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter),
[Apple Mach IPC restrictions](https://developer.apple.com/documentation/xcode/conforming-to-mach-ipc-security-restrictions).

## Later integration contracts

Register inventory/command names off RT; preallocated lock-free execution tokens
cover DSP and lifecycle calls. Bridge death still wakes the model through its
existing watcher, with a separate pending/ready/unavailable incident association.
Never delay established bridge kills to collect diagnostics. Next-start safe-load
selection precedes project instantiation; one-time bridged/disabled choices persist
only on opt-in. Existing recovery remains a separate explicit offer. Hangs use
5 s challenges/30 s suspicion, injected-clock tests, never automatic termination.
No network client, symbol download, global OS policy or new dependency.

## Topic-3 implemented boundary

Native fatal signals and Windows last-chance SEH use one preallocated terminal
capsule, a verified lock-free reentry claim and a native handoff. Acknowledgement
wait is at most one second; the worker is never suspended and never waits for
file writing/symbolication. `registerCurrentThread()` reserves a guarded alternate
stack (POSIX) or a stack guarantee (Windows), while cold; main registers before
frontend code. Other owned-thread integration belongs to topic 5; unregistered
foreign/driver threads have best-effort context and no stack-exhaustion promise.
No callback hook, TLS registration or sanitizer suppression is added. Sanitized
builds retain their handlers and emit exit-only summaries.

Existing non-default fatal handlers/SEH filters are preserved. After capture,
POSIX restores/re-raises the fatal default and Windows continues OS exception
search. Terminate records only an enum before POSIX abort or Windows fast fail. Aborts, asserts, teardown
faults, registered worker faults and collector loss are isolated fixtures. Raw
PC/frame values remain only in private IPC; the staged report records availability
and cause, without claiming a backtrace. Fail-fast and forced exits can lack a
capsule.

**macOS uses signal/exit fallback, not external Mach capture.** Its Mach chain is
untouched. Public posix_spawnattr_setexceptionports_np can bootstrap a later
receiver, but safe prior-handler forwarding, enhanced Mach restrictions and a
bounded outstanding-reply/collector-failure protocol need signed native proof.
Mach-only events without BSD signal delivery remain metadata-only. See ADR-0083.
This is an explicit delivery limit, not a claim of complete Mach coverage.

A captured terminal capsule sets kind `crash`. Without one, kind remains
`unexpected_exit`, even when the OS status is fatal; `native_fatal_status`
records that status independently. An exit code cannot establish a cause. On Windows, an exit value matching a
validated expected-return packet is a handled return even when its high bits
resemble NTSTATUS; terminal capsules still override expected return. Without a
capsule, coincident handled-return and teardown statuses cannot be distinguished.

Windows GUI console attachment happens in the launcher entry before spawning;
redirected handles retain the original frontend rules. After spawn, only the
supervisor ignores Ctrl-C, so the worker retains its inherited policy and the
supervisor can propagate the worker's actual exit. Windows terminate sends its
minimal capsule then uses the native fast-fail intrinsic, without CRT formatting
or allocator dependence. Native Windows verification remains required.
