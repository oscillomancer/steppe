# ADR-0083: Local crash reports and external supervision

**Status:** Accepted, 2026-11-15 (maintainer choices following builder-crash-plan).

## Context

Autosaves and recording journals preserve data but do not diagnose a fatal exit.
The GUI/model and audio threads can fail inside foreign code. A damaged process
cannot safely save a project, unwind arbitrary frames, call plug-ins or format a
report. Reports must not expose the user's work or introduce a network service.

## Decision

One external supervisor per GUI/headless frontend launches the worker before
SDL, plug-ins, EngineApp or devices start. Public names, arguments, stdio and exit
status survive the launcher change. The supervisor opens no devices or plug-ins.
A dead supervisor must not kill a healthy worker; a dead worker cannot keep its
protocol stdout open through the supervisor. Bridge/scanner children later share
the frontend supervisor through a separate, versioned private bootstrap, without
unsolicited frames or changed deadlines on the existing control stream
(ADR-0013/0031). No system service, debugger entitlement, elevation or global
configuration is installed. Development layout lands with the supervisor;
package layouts follow before release.

A worker creates an exclusive private run marker before risky startup. Expected
return is explicitly acknowledged; an observed fatal exit overrides an earlier
acknowledgement, including teardown crashes. Handled startup failures remain
ordinary errors. Unclosed markers mean unexpected termination, not proof of a
particular signal, OOM or plug-in fault. Report collection is independent of
recovery and never erases scratch sessions, autosaves, journals or media.

Fatal signals (SEGV/BUS/ILL/FPE/ABRT), Windows last-chance SEH and terminate/abort
use a reserved terminal capsule and native handoff. macOS establishes an external
Mach exception receiver where public unprivileged capability transfer succeeds,
forwarding prior exception handling; explicit signal/exit-only fallback applies
when unavailable. Foreign threads and replaced handlers may provide less context.
Stack space is reserved while owned threads are cold. No first-use TLS allocation,
stack registration or loader query is allowed in a live callback.

The terminal boundary is the sole exception permitting an audio thread's final
native diagnostic handoff. Normal SUPPLE_RT rules remain absolute. The handler
uses preallocated POD, always-lock-free scalar publication and audited native
operations only (POSIX async-signal-safe calls). No allocation/free, mutex,
stdio/JSON/logging, file open/write, engine/project/plugin calls, backtrace,
demangling, loader inspection, exception text, normal cleanup or continue-DSP.
Re-entry terminates; no indefinite stopped process. Capture budget is one second;
terminal handoff has a hard five-second bound even if the collector disappears.
No handler waits for symbolication. Fail-fast, SIGKILL, power loss and very early
faults may yield only metadata/unclosed markers. SIGINT/SIGTERM retain graceful
semantics. Sanitizer-owned handlers keep their diagnostics.

No new dependency. First-party bounded external walkers and OS SDK facilities
provide best-effort owned/foreign frames; missing permission/unwind data is
explicit. ELF build IDs + separate debug files, Mach-O UUID + dSYM and PE
CodeView/PDB GUID-age identify manual symbolication artifacts. Symbols stay out
of packages for now and are retained as matching build artifacts. No symbol
server, environment symbol path, source server or automatic download is used.
Raw dumps, stack/register bytes and temporary memory dumps are forbidden.

Reports are canonical `supple.crash-report/1.0` JSON with explicit completeness:
build/version/dirty identity, platform/architecture, role, incident ID, cause,
thread-role ordinals and bounded module-relative executable PCs, readable plug-in
software names/IDs and hosting mode, command **names/categories only**, and drop
counters. Allowlisted bounded fields exclude project names/paths/content,
audio/MIDI/state/parameters, arguments, Lua/DSP source, arbitrary exception/log
text, environment, machine/user identity and absolute module/source paths.
Readable third-party software names are approved, escaped and bounded; they are
not proof of guilt. Fault-PC/module and active execution-token evidence determines
confidence. Shared-container membership or a recent command alone never proves
one plug-in caused corruption. Unknown attribution remains unknown.

Reports live only under the application's **local non-roaming data** folder:
Linux XDG_DATA_HOME (fallback ~/.local/share), macOS ~/Library/Application Support,
Windows LOCALAPPDATA, then product slug/diagnostics. Never APPDATA on Windows,
project/cwd, a shared temporary file or a system crash folder. Empty roots fail
honestly; no elevation or remote destination is requested. User-mapped/mounted
folders retain the user's filesystem policy; the application does not transmit.
Exclusive owner-private files, ownership/link checks, bounded reads and atomic
publication apply. A report is at most 4 MiB, 128 threads, 128 frames/thread;
truncation/unavailable reasons are required. Keep at most **20 reports or 50 MiB**,
whichever is reached first; remove oldest full reports, independently of recovery.
Active run locks are never pruned as old reports. No uploader, auto-share or
submission API exists. The no-upload promise is application-scoped: the OS's own
crash reporting remains exactly as the user configured it, with its exception
chain preserved.

Before any suspect binary is loaded at next start, the GUI offers normal load,
a one-time dedicated bridge, or disabled without instantiating code; remember
only by explicit opt-in and binary identity. Preserve the project/state, show the
effective override and avoid a silent project/history edit. Unsupported native
modules offer disabled/replace, never a fictitious bridge. Then offer existing
ADR-0078/0080 recovery independently. State “The app closed unexpectedly. Report
saved at …” only when a validated report exists; otherwise explain its absence.
Headless notices go to stderr, never protocol stdout. Dismissal/acknowledgement
is distinct from deleting recovery data. Live concurrent runs are excluded.

Hang challenges every five seconds, suspicion after thirty seconds, one diagnostic
per episode; injected clocks test the state machine. Hangs are diagnostics-only,
never automatic kills. Sleep/resume/idle/long work are distinct. Existing bridge
2 s processing/7 s lifecycle deadlines remain unchanged. User-requested force close
is distinct from Discard and preserves existing recovery checkpoints.

## Consequences and delivery

Implementation follows nine topics from builder-crash-plan: contracts; supervision/
run storage; terminal context; filtered traces/symbols; engine inventory/tokens;
bridge correlation; pre-load GUI/recovery; hang diagnostics; release packaging/
symbol artifacts. **Stop after topic 3 for review.** Each topic covers all platforms
with isolated native death fixtures. Native CI and signed macOS capability/
collector-loss feasibility remain explicit gates; Linux success proves none of
Mach/SEH behavior. Metadata-only fallbacks are useful and must be tested too.

## Alternatives considered

In-process report formatting violates terminal safety. Ordinary minidumps retain
private memory. Crashpad/libunwind add dependencies and maintenance without solving
privacy by default. Automatic restart/disable or upload is outside the decision.

*Implementation note, 2026-11-15, topic 3:* macOS ships the approved signal/exit
fallback, leaving the Mach exception chain untouched. The public spawn exception
port bootstrap is a candidate, but bounded outstanding-reply cancellation and
signed/hardened forwarding are not proven. Mach-only events without a BSD signal
therefore have exit metadata only. No task_for_pid grant, special entitlement,
private API or suspended-process design is introduced. Native feasibility remains
a gate before any later Mach receiver replaces this fallback.

*Maintainer clarification, 2026-11-16, round-22 finding 2:* the terminate hook
wraps and chains the handler returned by `std::set_terminate` at installation.
It publishes the distinct terminate cause before invoking that handler (abort if
null, or if it returns). This runs as ordinary terminate code, not in a signal
handler; calling the previous handler, which would have run anyway, is the one
sanctioned foreign call in this context. Signal/SEH handlers never call it.
The previous handler retains its behavior, including a deliberate exit status.
Terminate capsules use code 0 because the chained handler chooses the exit path;
no exception text is inspected. Re-entry terminates without chaining again.
Sanitizers retain ownership and are not wrapped.

*Maintainer decision, 2026-11-16, review round 29 finding 1:* the Windows last-chance
filter is always installed and chains to the filter it replaced after the terminal
handoff, replacing the earlier preserve-only policy. The MSVC runtime's startup always
installs its own top-level filter, so declining to replace an existing one meant the
filter was never installed in a real executable while terminal coverage still reported
true. The chained filter (and with it the user's OS crash reporting) still runs; the
fault stays fatal; "installed" now means the filter is really in place.

*Note, 2026-11-16, review round 31:* the filter leaves an escaping C++ exception
(`0xE06D7363`) to the runtime's filter, so it is recorded as terminate and the program's
terminate handler runs, as on the other platforms; a chained filter's
`EXCEPTION_CONTINUE_EXECUTION` is returned as `EXCEPTION_CONTINUE_SEARCH`, so the fault
stays fatal; and "installed" no longer reads the filter back, since `SetUnhandledExceptionFilter`
cannot fail and the read briefly clears it.

*Maintainer decisions, 2026-11-16, topic 4 (thread traces):* (1) External thread reads
happen inside the existing terminal hold; the bound does not grow, and an expired budget
gives a partial report with a stated reason, never a longer hold. (2) macOS captures in
process in the terminal handler (the approved terminal-only exception): the faulting
thread's frame-pointer chain from the signal context, then best effort the other threads
of the own task through `task_threads`/`thread_get_state` on `mach_task_self`, reading
memory only through a fault-proof `mach_vm_read_overwrite` of the own task, into the
preallocated capsule, with fixed caps and no allocation, locks or stdio; a failing Mach
call marks that thread unavailable. No task port of another process, entitlement or
privilege is used; this refines the topic-3 macOS signal/exit fallback once implemented.
(3) First-party code on Linux and macOS, and the stock plug-ins through the parent's
compiler flags, keep frame pointers; third-party frames stay best effort and validated.
On Linux the supervisor traces the worker as its ancestor under Yama; nothing
system-wide changes. (4) Symbolication needs function names only, no DWARF line
information, in a separate offline resolver with a strictly bounded reader. (5) The
release-symbol archive waits for topic 9. See `docs/specs/diagnostics.md`.

*Note, 2026-11-16, topic 5a (the execution table) and review rounds 31 and 32:* five points
of the first step were confirmed by the maintainer. (1) The table's address rides in the Ready
packet's `pc` field, named in code by `setTableAddress` and `tableAddress`; if a later step adds
real messages it moves to a `Table` message. (2) The dummy backend's thread (`audio-dummy`)
stays unregistered, since it stands in for the audio driver's unregistered callback thread.
(3) The macOS capsule carries roles but no copy of the table until the step that records
execution entries. (4) The fault thread keeps `role: "fault"` and gains `registered` instead of
a fault flag on every thread. (5) The sandbox's render thread is not registered. The slot
protocol changed after review: a slot's contents are cleared before its id, a thread the table
turned away still gets its alternate stack, the table has 256 32-byte slots that carry stack
bounds, and a Windows Ready packet is handed over before Terminal. *Maintainer decision on a
killed supervisor:* collector loss leaves the healthy app alive; a hard-killed supervisor does
not take the app with it, no kill-on-close job is added, and the cost is a stated limit in
`docs/specs/diagnostics.md` and `STATUS.md`.

*Note, 2026-11-16, topic 5b (execution scopes):* plug-in and module entries are marked in the
table by scopes inside `host::Instance`, `Library::open` and the VST3 module load, and a thread
in an entry is reported with `entry` (a closed set of kinds), added to `supple.crash-report/1.0`
as approved. Three points differ from the plan of 5a. (1) Each instance cell fills a cache line
(the table is 74,816 bytes): with four 16-byte cells to a line, concurrent instances measured 3
to 5 times the cost per scope, which the maintainer's decision to pad if visible settles. (2)
The macOS capsule (now `CTC3`) carries each thread's attribution and the table's counters, not
a copy of the table: the worker reads its own table while the thread is suspended, so the copy
is not needed until the inventory has something to resolve, and it is added then. (3) The
license-isolation check does not cover executables, since the test tools under `plugins/` link
the GPL host already and are combined works; it covers every library and module built from the
permissive tree.

*Note, 2026-11-17, ADR-0091 (the launcher's stop):* the sentence that a dead supervisor must
not kill a healthy worker narrows to a supervisor that failed as a collector. A supervisor
that ends without saying so, as a launcher's hard kill leaves it, is a stop request: the
worker stops as `SIGTERM` stops it and ends hard after ten seconds. See
`0091-supervisor-end-and-launcher-stop.md` and `docs/specs/diagnostics.md`, "Supervisor end".

*Note, 2026-11-17, review round 35 (Windows):* the terminate hook is also installed on each
thread that registers with the diagnostics, since the MSVC runtime keeps the handler per
thread, and an exception with the code `0xE06D7363` that the prior filter returns without
handling is captured as a fault instead of being lost. Both are in `docs/specs/diagnostics.md`
and rest on native CI to run them.

*Note, 2026-11-17, topics 5c to 5e (inventory, command breadcrumbs, attribution):* `plugins`,
`commands`, `attribution` and `dropped` are filled and validated inside `supple.crash-report/1.0`,
as approved, and the checks in `docs/specs/diagnostics.md` apply. Points that differ from the
plan of 5a. (1) A loaded plug-in library has an inventory entry of its own (`clap_library`,
`vst3_library`), so a crash in a plug-in's load that has not yet said what plug-in it is still
shows what was loading. (2) `build` is not computed in the worker: each announcement carries an
address inside the plug-in's binary (never reported), and the supervisor finds the module that
holds it among the regions it read. (3) The packet grew to 392 bytes (protocol version 2) rather
than adding a second framing; the sink closes for good if a half-sent packet cannot be finished,
so a process waits for a stalled supervisor at most once. (4) Command names go to the supervisor
once, when the process's first dispatcher is built; the ring holds ids, origin categories and
states only. The macOS capsule (now `CTC4`) carries the command ring's entries, and still not a
copy of the table. (5) Plug-in ids in a report are the order of first announcement, not cell
numbers. (6) `dropped` is an object with seven counts (`unregistered_threads` is added to the
plan's six, since threads omitted from the report and threads the table had no slot for are
different things); a report of an earlier build with a plain count, or none of `attribution`, is
still a report the validator recognises. (7) Rack modules are not announced yet and have no
cell; a crash inside one reports no module entry, a stated limit in `STATUS.md`.

*Note, 2026-11-17 (review 38b, F1):* the worker's marker is released when the worker dies, before the
supervisor publishes the report, so the supervisor also holds a lock of its own on the incident
(`<incident>.pub`) from before the worker starts until the report is published and the marker removed. A
reader counts a run as live while either is held. Contract: `docs/specs/diagnostics.md` "When a report
counts (T8)".

*Note, 2026-11-17 (topic 7d, safe-load):* a suspicion of a plug-in ends by *settling* a report
(`<incident>.settled`, `crash.settle`), not by acknowledging it: seen is not resolved. The engine
compares the installed plug-in file's build id with the report's before anyone is asked
(`project.plugins`, also for what `project.recover` would open; the `project.suspect_plugin` warning of
`project.open` and `project.recover`), never prompts, never changes
how a plug-in loads by itself, and settles what a run that ended well loaded normally in process.
`plugin.override` (ADR-0081 note) is how a run bridges or disables a plug-in. Contracts:
`docs/specs/diagnostics.md` "Safe-load in the engine", `docs/specs/engine.md` §5.

*Note, 2026-11-17 (topic 6a, bridge processes):* a report says which bridge process each plug-in ran
in and how each bridge ended (`exit`, `signal`, `exception`, or `host` when the host killed it, which
the host records before every kill), and a bridge that dies by a signal or an exception while the
application lives gets a report of its own, kind `bridge_exit`, under an incident with no run marker:
at most 3 per run, none that repeats one already reported. It is the host's account of the death, not
a capture of the bridge, and it is not a restart notice or a place in the suspect window. No protocol
change, no change to the bridge. Instead of the `Message::BridgeEnd`, the worker-minted incident id
and the worker's pending/ready watcher sketched under "Later integration contracts", the `Bridge` end
event travels in order on the inventory channel and the supervisor, applying it, mints the incident id
and writes the report; a bridge death does not wake the model by itself. Bridge reports have their own retention budget (the
newest 5), so a plug-in whose bridge dies in every run cannot push out the crash report that makes it
a suspect. Contracts:
`docs/specs/diagnostics.md` "Bridge processes (topic 6a)".

*Note, 2026-11-17 (topic 6b, the child's identity):* the bridge answers `Create` with the instance's
cell and generation in its own execution table and an address in the plug-in's binary (bridge
protocol 26, Windows 27, ADR-0013's note), and the host announces them in a `Child` event. The
supervisor keeps them as private matching data and never reports them; the bridge's own threads
are registered with topic 6c, where something reads them. Contracts: `docs/specs/diagnostics.md`
"The child's identity (topic 6b)".
