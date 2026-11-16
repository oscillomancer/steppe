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
OS process exit independently. An unexpected exit without terminal packet remains
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
`platform`, `architecture`, `role`, `kind`, `code`, `complete`, `unavailable`,
`threads`, `plugins`, `commands`, `dropped`. Strict size/type/enum validation and
bounded allowlisted formatting apply. Phase-2 reports are honest exit summaries;
phase 3 can attest terminal context but persists no raw address/register bytes.
Thread frames added in topic 4 require executable module identity plus relative
PC, never arbitrary stack words. Cap 4 MiB/report, 128 threads, 128 frames/thread.
Readable bounded software names are allowed; personal/project data and arguments
are forbidden. Never create a minidump to sanitize later.

Atomic private publication uses native wide Windows paths and existing atomic
write/durability primitives. Retention prunes oldest validated owned reports until
count <=20 and total <=50 MiB. Malformed/link/foreign entries are neither followed
nor treated as authority to delete another file. Run markers/locks and recovery
are not report-retention inputs. Namespace power-cut durability on Windows has
the same unprivileged limitations as ordinary project publication.

## Terminal behavior

POSIX SEGV/BUS/ILL/FPE/ABRT and Windows unhandled SEH use a fixed reserved capsule.
Terminate records an enum then aborts, without inspecting the exception. Owned
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
