# Local diagnostics (ADR-0083)

## Delivery boundary

The approved nine-topic sequence is delivered in steps with a review stop after each.
Topic 4 (thread traces) has landed for Linux and is written for Windows, which native CI
has yet to prove; macOS in-process capture and the offline resolver follow. Until later topics land, engine inventory, bridge
correlation, restart UI, hang diagnostics and final release packaging are contracts,
not working features.
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
`modules`, `threads`, `plugins`, `commands`, `dropped`. Strict size/type/enum validation and
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
metadata/removal errors still fail storage. On Windows a report another pruner has
condemned but whose deleting handle is still open fails to open with
`ERROR_ACCESS_DENIED`, the code a real refusal also gives, so that code counts as
already removed only when the NT status of the same call (`RtlGetLastNtStatus`) is
`STATUS_DELETE_PENDING`; any other status, or no status, stays an error. POSIX needs no
such case: unlink is immediate and a lost race is `ENOENT`. Namespace power-cut durability on Windows has
the same unprivileged limitations as ordinary project publication.

## Terminal behavior

POSIX SEGV/BUS/ILL/FPE/ABRT and Windows unhandled SEH use a fixed reserved capsule.
Terminate publishes its distinct cause before chaining the handler returned by
`std::set_terminate` at installation (abort if null or if it returns), without
inspecting the exception. This is ordinary terminate code, not signal context:
the previous handler would have run anyway and is the sole sanctioned foreign
call there. Signal/SEH handlers never invoke it. The capsule code is 0 because
the chained handler chooses the exit path; its exit status is preserved. Owned
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

## Thread traces (topic 4)

`modules` lists the executable images seen in traces: `{format: elf|macho|pe, id}`, `id`
the lowercase-hex build ID (ELF note, Mach-O UUID or CodeView GUID+age) or null when the
image has none. Names and paths are never recorded; a local resolver maps an id to a name
and symbols. `threads` has at most 128 entries, in order: the faulting thread (`fault`),
the main thread (`main`), then the rest by thread id. Each is `{ordinal, role, state,
frames, stop | reason}`; `ordinal` is its index, never an OS thread id. `state` is
`walked` with `stop` (`end`, `depth`, `unreadable`, `not_executable`, `bad_frame_pointer`,
`cycle`, `deadline`), or `unavailable` with `reason` (`ptrace_denied`, `access_denied`, `thread_exited`,
`deadline`, `registers_unreadable`, `maps_unreadable`, `unsupported`, `not_collected`) and
no frames. A frame is `{module, offset, return}`: `module` an index into `modules`,
`offset` the lowercase `0x` hex address minus the module's load bias (the link-time
address a symbolizer expects), `return` false only for a thread's stopped pc (symbolize
return addresses at offset-1). Executable memory with no file (JIT) is `{module: null,
return}` with no offset. At most 128 frames per thread; `dropped` counts threads omitted
beyond 128. Retention validates all of this strictly: a report with any other shape is
preserved, never authority to delete.

Walking follows the frame-pointer chain `{previous fp, return address}` (x86-64 and arm64)
from the registers of the stopped thread. Every return address must lie in an executable
mapping; frame pointers must be 8-aligned, at or above the stack pointer for the first
frame, strictly increasing and less than 1 GiB apart; arm64 return addresses lose their
pointer-authentication bits. The first violation ends the walk with its `stop` reason.
Only the 16-byte frame records are read, never other stack words. A function without a
frame (optimized foreign code, a libc wrapper at a system call) leaves its own caller's
frame missing, and a JIT or hostile chain can end a walk early: that is the best-effort
limit for third-party code, stated in `unavailable` whenever a trace exists.

Linux collection runs in the supervisor inside the worker's terminal hold. The bound does
not grow: the collector has a 600 ms budget against the worker's one-second wait, and on
expiry the remaining threads are `unavailable` with reason `deadline`. The supervisor is
the worker's parent, so Yama's default `ptrace_scope` 1 permits it to trace as an
ancestor; no sysctl, capability or `PR_SET_PTRACER_ANY` is used or needed. Scope 2 and
3, a non-dumpable worker, an existing tracer or a seccomp filter refuse, and each thread
then reports its reason. The collector stops one thread at a time with `PTRACE_SEIZE` and
`PTRACE_INTERRUPT`, reads registers, walks, and detaches it (passing back any pending
signal) before the next; a faulting thread whose capsule has registers (a signal or
exception) is not stopped, since it is parked in its handler, and is walked from them. The
terminate and abort paths capture no registers, only the calling thread's id: that thread
is the `fault` entry and, being parked in its handler too, is stopped and read like any
other, so its real stack is reported as the fault entry and not under `main` or `other`.
Its hold therefore waits out interruptions: the one-second wait for the acknowledgement
resumes after `EINTR` for the time that remains. When even the thread id is unknown, the
fault entry is `unavailable` with `registers_unreadable`. Interrupting a thread makes a
blocking call in it (`epoll_wait`, `read`, `nanosleep`) return `EINTR` once, during the
hold; code that treats `EINTR` as fatal misbehaves there. A thread that cannot reach its
stop by the deadline (a killable or uninterruptible kernel wait, such as the vfork wait
inside `posix_spawn`) stays attached,
reported `deadline`, and is never stopped by the collector afterwards. The guarantee is
therefore not that nothing stays attached but that nothing can block the worker's exit:
once the worker dies, the supervisor's wait reaps every thread it still traces (a traced
thread is not reaped by anyone else, and the worker is not reported until it is) and
detaches any that stops, handing a pending signal back. If the worker's exit is observed
during collection, its real status is kept for the report. Module identity is
read from the ELF headers in the target's memory through `process_vm_readv`, so a deleted
or replaced file cannot mislead it. Formatting, JSON and file work happen after the
acknowledgement.

Windows collection also runs in the supervisor inside the terminal hold, on the same
600 ms budget. The supervisor created the worker, so its process handle already allows
suspending threads and reading memory: no privilege, policy or debug right is raised.
Threads come from a toolhelp snapshot filtered to the worker's process id; the main
thread is the earliest created. Each other thread is suspended, its context read, and
resumed before the next; a faulting thread whose capsule has registers is not suspended,
since it is parked in its last-chance filter, and is walked from the pc, stack and frame
pointers in the capsule. A terminate or abort capsule has none, only the calling thread's
id, and that thread is suspended and read like any other, as the `fault` entry.
A thread the OS refuses reports `access_denied`; one that exited, `thread_exited`.
Module images load lazily, as a walk meets them, and only while the budget remains: after
the deadline no new module file is opened, and a load already started (a slow disk, a virus
scan, a network share) cannot be interrupted, which can cut a trace short but never holds
the worker longer than its one-second wait.
Frames come from the OS unwinder (`StackWalk64` over each image's unwind data, x64 only;
other Windows architectures report the faulting thread `unsupported`), with memory read
through `ReadProcessMemory`. All DbgHelp work is serialized under one lock and loads
module images from their local files without symbols: no symbol path, no environment
symbol path, no symbol server, no network. A module's file path stays inside the
supervisor and is never recorded. Executable regions come from the worker's committed
executable pages; a region with no image is generated code. Module identity is the PE
CodeView record read from the image in the worker's memory: the PDB GUID in
symbol-server order followed by the age as eight hex digits (40 lowercase hex characters),
and `offset` is the address minus the load bias (load address minus the image's
preferred base), the link-time address a symbolizer expects, as for ELF. The PDB path is
never read. Stack-overflow and fail-fast faults keep the limits stated for the terminal hook:
fail-fast bypasses the last-chance filter and yields no trace.

## Frame pointers

The topic-4 walkers follow frame-pointer chains, so first-party code on Linux and
macOS is built with `-fno-omit-frame-pointer` and, where the compiler accepts it,
`-mno-omit-leaf-frame-pointer`, in every preset (`cmake/Defaults.cmake`). The stock
plug-ins receive the same flags through the parent build's `CMAKE_<LANG>_FLAGS`;
no template file changes. Everything compiled inside that plug-in sub-build gets them,
vendored third-party sources fetched there included (a small speed cost, no change of
behavior). Code built elsewhere, such as foreign plug-ins and system libraries, keeps
whatever its builders produced: walked best effort and validated, never assumed. Windows
uses the OS unwinder and takes no flag.

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
search. Terminate records only an enum, then chains the previously installed handler;
its deliberate message/exit behavior survives. A subsequent native fatal signal
restores/re-raises the default without overwriting the terminate capsule. Aborts, asserts, teardown
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
minimal capsule before chaining the previous runtime/user handler, just as POSIX
does. Product capture performs no CRT formatting or allocation; the explicitly
sanctioned prior handler retains its own behavior. Native Windows verification
remains required.
