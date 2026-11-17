# Local diagnostics (ADR-0083)

## Delivery boundary

The approved nine-topic sequence is delivered in steps with a review stop after each.
Topic 4 (thread traces) has landed for Linux and is written for Windows, which native CI
has yet to prove. macOS in-process capture is written and unproven until native CI runs
it. The offline resolver is delivered and tested on Linux; its Windows and macOS builds are
unproven until native CI runs them. Until later topics land, engine inventory, bridge
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
the main thread, then the rest by thread id. Each is `{ordinal, role, state, frames,
stop | reason}` plus the optional `registered` and `worker` below; `ordinal` is its index,
never an OS thread id. `role` is `fault` for the faulting thread; for every other thread it
is the role the process registered it as (see Thread roles), `model` for the main thread
if it is not registered, else `other`. (`main` is what reports written before thread roles
called the model thread, and stays valid.) `state` is
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
blocking call in it that the kernel does not restart by itself return `EINTR` once, during the
hold: `epoll_wait`, and `read` on an inotify descriptor (ptrace(2), BUGS), where most other
calls are restarted transparently; code that treats `EINTR` as fatal misbehaves there. A thread that cannot reach its
stop by the deadline (a killable or uninterruptible kernel wait, such as the vfork wait
inside `posix_spawn`) stays attached,
reported `deadline`, and is never stopped by the collector afterwards. The guarantee is
therefore not that nothing stays attached but that nothing can block the worker's exit:
once the worker dies, the supervisor's wait reaps every thread it still traces (a traced
thread is not reaped by anyone else, and the worker is not reported until it is) and
detaches any that stops, handing a pending signal back. If the worker's exit is observed
during collection, its real status is kept for the report. The `diagnostics.ptrace-faults`
test (Linux, skipped in sanitized builds and under restricted tracing) preloads a shim
that makes the supervisor's ptrace calls fail or wait on process state: a refused
`PTRACE_INTERRUPT`, a thread that dies while held stopped, and a main thread that exits
while attached. In each the supervisor must finish and the report must say what was not
read. Module identity is
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

macOS has no external collector: reading another process needs `task_for_pid` and an
entitlement, neither of which is used. The worker reads its own threads in the terminal
handler, after the terminal packet has gone out, with Mach calls on its own task only
(`task_threads`, `thread_suspend`, `thread_get_state` and `thread_resume` on its own
threads, `mach_vm_read_overwrite` on its own memory). It allocates nothing on the heap,
takes no lock and formats nothing: the capsule is static storage of fixed size (128 threads,
128 frames, 256 modules), memory that cannot be read fails the read and ends the walk
(`unreadable`), and dyld's image list is read as plain memory, never through a dyld
call. `task_threads` returns its list in kernel-mapped memory that is given back with
`vm_deallocate`; that is the only allocation and it is not the heap. Each other thread is
suspended only while its registers and stack are read and is resumed before the next, on every
path; a thread whose suspension or state call failed is `unavailable` with `thread_exited`,
`access_denied` or `registers_unreadable`, and threads left when the budget ends are
`deadline`. The calling thread is the `fault` entry: from the signal's registers, or from
its own frame for terminate and abort, which carry none. The worker keeps to its one
second: thread reads end at 400 ms; listing dyld's images and finding the module of each
frame end at 450 ms (after that, frames carry no identity, which the parser accepts); the
capsule is sent by 500 ms (a `Message::Trace` packet carrying the byte count, then the
bytes, each wait bounded) and the wait for the acknowledgement gets the rest. An image's
header is read once, and what it said is kept, so a stack of addresses outside every image
cannot make the handler read the same headers repeatedly. The supervisor reads the
capsule for 600 ms from when it starts collecting, which is after the worker's clock starts, so the
send ends with a margin of at least 50 ms before the supervisor gives up (a compile-time
check ties the two). It validates it strictly (counts, roles, reasons and ranges, the fault entry first
and only once, the dropped-thread count agreeing with its flag and plausible, the bytes consumed
exactly); a capsule that is missing, late, truncated or malformed leaves the fault thread
`unavailable` with `not_collected`, and the report is otherwise unchanged. The addresses
in the capsule never leave the supervisor's parser: a frame is reported as an index into
`modules` and an offset, the address minus the image's slide (its link-time address).
Module identity is the Mach-O `LC_UUID` of each image dyld lists, found from the
image's header in the worker's memory. System libraries in the dyld shared cache, if dyld
does not list them, are in no module: their frames carry no identity, and because the
chain was validated in the worker, a return address outside every module is kept as such
and the walk goes on. Frames need frame pointers: the first-party code and the stock plug-ins
have them (see Frame pointers), system frames are best effort. On arm64 pointer-authentication
bits are stripped from return addresses.

## Thread roles and the execution table (topic 5)

Library placement: the table lives in the GPL library `diagnostics-context`
(`libs/diagnostics-context/`, header `supple/diagctx/table.h`). It is under `libs/` because
command-bus, also there, links it and a `libs/` library does not depend on host products;
it is not in `core-base` or `modules/rack`, which are MIT and plug-in-linked, so no stock
plug-in, module or MIT library links it (`tools/license-check` enforces that by leaving it
out of its MIT list). Today `diagnostics` and `engine` link it; clap-host and command-bus
join in the steps that use it.

The table is one static, zero-filled, page-aligned block per process (18,464 bytes: a
32-byte header, 64 thread slots, 1,024 instance cells and a 64-entry command ring, 16
bytes each; the cells and ring are laid out now and used from later steps). It holds
integers and enums only, never a string, a pointer into model or plug-in memory or
anything a user typed, and carries a magic and a version. It is not in the binary's file
image (it is `.bss`).

An owned thread registers once while cold, next to `nameThread`:
`registerThread(role, worker)` claims a slot (compare-and-swap, so threads may register
concurrently; a full table counts `threadsDropped` and the thread is reported as `other`),
stores the role, then publishes the operating-system thread id (Linux and Windows thread id,
macOS Mach thread port name) last, so a reader never pairs an id with a stale role. The
slot is released when the thread exits, so an id that is reused is not mislabelled. It is
idempotent per thread. A hook, set by `diagnostics`, runs on each newly registered thread
and installs its alternate signal stack (so the engine does not link `diagnostics`). The
roles are a closed set: `model` (the main thread, registered when terminal handling is
installed), `audio_render` (the anticipative renderer), `audio_live` and
`audio_background` (the pool helpers, numbered from 1), `audio_offline`, `recorder`,
`midi_output`. Not registered, so `other`: the audio driver's callback thread (created by
the driver; registering it would need a thread-id call or TLS write on the audio path),
the dummy backend's thread (it stands in for that driver thread, and tests should see it
as one), `main-wake` (in MIT `core-base`), the sandbox and bridge children, and any thread
a plug-in or the OS creates.

The report labels each thread with its role. The fault thread keeps `role: "fault"` and
adds `registered` with the role it was registered as; a thread of a numbered pool carries
`worker` (1 to 65535) next to its role. `registered` and `worker` appear only where they
say something, and retention validates them: `registered` only on the fault thread and
only a registrable role, `worker` only on a thread with a role.

How the supervisor gets the table: the worker's `ready` packet carries the table's address
(in the packet's `pc` field, which a Ready packet reuses: it is an address, not a program
counter, and the code reaches it only through `setTableAddress` and `tableAddress`; the
private channel is not a place for anything else secret). A later `Table` message would take
over that role. When the terminal
packet arrives, Linux and Windows read the table with the same bounded remote read the stack
walk uses (`process_vm_readv`, `ReadMemory` on Windows), once, right after the faulting
thread's walk and before any other thread is stopped. It is read even if the walk spent the
deadline: one read, 18 KiB. If the budget ends, what is cut is the traces of the other
threads (`deadline`), never the table; their roles are still known. The table is untrusted
input: it is used only if its magic and version are this build's, a slot with a free,
claiming or unknown role labels nothing, and an address that cannot be read is a stated
limit ("the execution table could not be read, so threads have no roles") with every thread
still traced, labelled by position as before. macOS has no remote read: the in-process
capture labels each thread from the live table's slots (atomic loads, no lock or
allocation) and the capsule carries the result in each thread's header (`registered`,
`worker`). The capsule wire format is version 2 for that (magic `CTC2`); it carries no copy
of the table yet, which the steps that record execution entries add.

Threads other than the faulting one keep running while the table is read, so a later step's
instance cells are each read atomically but describe the moment of the read, not the moment of
the fault; only the faulting thread is parked. The thread slots do not have that problem:
they change only when a thread starts or ends.

## Offline resolver (topic 4)

`resolve-crash [--json] --symbols PATH... REPORT.json` names the functions in a local
report's thread traces. It is a separate library (`crash-resolver`, `host/diagnostics/resolver`)
and a command line tool that the application never links, runs or ships with a crash path:
reports stay free of names, and naming happens afterwards, on a machine that has the files
of the build that crashed. It reads the report and the files and folders it is given and
nothing else: it writes no file, opens no network connection, consults no symbol server and
reads no environment symbol path. A folder is searched in sorted order, without following
links, to 8 levels, 20000 files and 100000 entries per folder; a limit that stops the search
is said on standard error.

A report module is matched to a file by format and `id` (ELF build ID, Mach-O `LC_UUID`,
PE CodeView GUID and age, computed exactly as the collectors compute them), so the file
must be the build's own, or a separate symbol file carrying the same id (an ELF debug file,
a dSYM's DWARF file). A module with no `id`, or no file with its id, is reported as
`no_identity` or `not_found`; a matched file without symbols is `no_symbols`; none of them is
guessed at. Several files can carry one id: a stripped binary and its separate debug file
do, and the stripped one, which keeps a few exported names, usually comes first in search
order. So up to eight files with the id are read, and the one that names the most of the
report's frames in that module wins, the earlier one on a tie. A module is `resolved` only if
it named at least one of those frames (or no frame of the report is in it); a file with names
that covers none of them is `no_symbols`, with the reason stated.
A report may name one image in many module entries; its symbol table is loaded once per
distinct id and shared, and the functions held across all modules of one report are limited
to 16 million (a module read after that is `unreadable` and says so).
The scan only reads headers and may be long past when a file is read, so each file's identity
is read again from the handle its symbols are read from; one that no longer holds the module's
image (a rebuild rewrote it) is `unreadable`, "the file changed after it was scanned".

Function names only: no DWARF, no line tables, no PDB. ELF reads `.symtab`, else `.dynsym`
(function and indirect-function symbols in executable sections); Mach-O reads `LC_SYMTAB`
entries defined in `__TEXT` code sections, dropping the C underscore, from a thin 64-bit
image or from each slice of a universal file; PE reads exported names, so a Windows image
without exports resolves no names until a later topic adds PDB support. Only 64-bit
little-endian images are read, as the collectors read them. A symbol's extent is its size
where the format has one and otherwise runs to the next symbol or the end of its section; an
address outside every extent has no name rather than the nearest one. Itanium names are
demangled where the platform has a demangler, others are shown as stored. A frame with
`return` true is looked up at offset-1 and shown at its own offset, as the report specifies.

Every file is hostile input. All reads go through a source that refuses any range outside the
file; counts, sizes and offsets from headers are checked against the file size and fixed limits
before they allocate or read (files to 16 GiB, 4M functions, one string table 256 MiB, 4096
bytes per name, 65535 ELF sections, 4096 Mach-O load commands and 1 MiB of them, 8 universal
slices, 131072 exports); a file over a limit is refused with `symbols.too_large`, except that
more than 4M functions is read up to the limit and marked `truncated`. The report is parsed
with a size limit of 4 MiB, a depth and value limit, and validated field by field
(`report.invalid` with a JSON pointer, `report.unreadable` for the file); fields it does not
use are ignored. Tests build images in memory and run the reader over every prefix of each, over
every byte replaced by extreme values and over reads that fail part way, under the
sanitizers.

On Windows the tool takes its arguments as wide strings (`wmain`), so a report or symbol
folder under a name outside the ANSI code page opens.
Text output escapes every control byte, DEL and C1 control (U+0080 to U+009F) in what it
prints, function names, file names and report strings alike, as `\xNN`, since a hostile symbol
file or report must not be able to drive the terminal of the person running the tool.
Output is text, or with `--json` `supple.crash-symbols/1.0`: `{incident, kind, role, platform,
modules: [{format, id|null, state, file?, functions?, truncated?, error?}], threads:
[{ordinal, role, registered?, worker?, state, stop|reason, frames: [{module|null, offset?, return, function?,
displacement?}]}]}` (`state`: resolved, no_symbols, unreadable, no_identity, not_found). It
carries local file paths, since the person running it supplied them, and is never stored in
the crash folder. Exit status: 0 when the report was read (unresolved modules included), 1
for an unreadable or invalid report, 2 for a bad command line.

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
frontend code. Other owned threads register through the execution table, which also gives them the
alternate stack (see Thread roles and the execution table); unregistered
foreign/driver threads have best-effort context and no stack-exhaustion promise.
A thread's alternate stack is unmapped at its exit only after the previous one was
restored; if the restore fails the 64 KiB stays mapped, since the kernel answers a signal
frame pushed onto an unmapped enabled stack with a forced SIGILL and no handler.
No callback hook, TLS registration or sanitizer suppression is added. Sanitized
builds retain their handlers and emit exit-only summaries.

Existing non-default POSIX fatal handlers are preserved (ours is not installed over
them). On Windows the last-chance filter is always installed, replacing whatever top-level
filter was there, and chains to it after the handoff, returning what it returns. An exception code `0xE06D7363`
(a C++ exception nobody caught) is not claimed at all: the filter hands it to the prior one
untouched, so the runtime turns it into `std::terminate`, which is recorded as terminate
(cause 3) and chains the program's handler, as on Linux and macOS. An EXE
linked with the MSVC runtime always has such a filter (the runtime's own, which turns an
uncaught C++ exception into `std::terminate`), so declining to replace an existing one
would never have installed in the product; chaining keeps both that filter and any other
that would have run, and with them the user's own OS crash reporting. `installed` (the
worker's terminal coverage) is true on Windows only when the filter really is the
process's top-level filter. After capture POSIX restores/re-raises the fatal default.
A plug-in that later replaces the top-level filter without chaining still takes the
capture away; that is a stated limit. Terminate records only an enum, then chains the previously installed handler;
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
