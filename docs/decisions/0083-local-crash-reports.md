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
