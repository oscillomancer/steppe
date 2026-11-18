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
Supervision failure (a collector that crashes) degrades diagnostics without killing a healthy
worker; a supervisor that ends without saying so is the launcher's stop (below).

### Supervisor end (ADR-0091)

A supervisor that goes away is one of two things, and the worker tells which. Supervision
failure (the collector crashes) leaves the healthy worker running without capture, as
above. A launcher's stop (a hard kill of the supervisor: `SIGKILL`, `TerminateProcess`,
Task Manager, a service manager's escalation) stops the worker too, so the control socket
and the endpoint lock (ADR-0089) are released and a restart succeeds. A graceful stop is
unchanged: POSIX forwards `SIGINT` and `SIGTERM` from the supervisor to the worker; Windows
forwards nothing and the supervisor ignores Ctrl-C, which reaches the worker directly.

*What the supervisor says.* Before its own fatal paths end it, the supervisor tells the
worker "collector failed". POSIX: handlers for `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and
`SIGABRT` (the worker's set; a signal a sanitizer or another owner already handles stays
theirs) run on an alternate stack, so a stack overflow still reports. Each sends the one
byte `F` with `MSG_DONTWAIT | MSG_NOSIGNAL` and re-raises with the default action; the
supervisor's `std::terminate` ends in `abort`, which the set covers. Windows: the top-level
exception filter (with a 64 KiB stack guarantee on the supervisor's main thread) and the
abort signal set `collectorFailed`, one atomic in the channel's shared block, and chain to
what was there. A supervisor that exits normally has already seen the worker exit and sends
nothing.

*Everything that flows from supervisor to worker.* The boot packet, once, before the worker
starts (POSIX: in the socket's buffer, read by `adopt`; Windows: the shared block); the
acknowledgement, which only a worker in its terminal handler waits for (POSIX: the byte `A`;
Windows: an event); and the notice (POSIX: the byte `F`; Windows: `collectorFailed`).
Nothing else is ever sent.

*How the worker watches.* One thread starts right after the Ready packet, on a trusted
channel, on every platform and in sanitizer builds. It is never on the audio path, is not in
the execution table (a report shows it as an `other` thread parked in its wait), takes the
alternate-stack hook so a fault on it is reported, and on POSIX runs with every signal
blocked except the synchronous faults, so a process-directed `SIGTERM` never lands on it.
POSIX: `poll` on the channel, then `recv(MSG_PEEK | MSG_DONTWAIT)`; a per-call flag, so the
descriptor's blocking mode, which a capsule transfer changes on the same open file
description, decides nothing. End of file, or a reset (a supervisor killed with packets of
ours unread leaves one), is the supervisor gone. Any byte ends the watching, and it is only
peeked, never consumed: `F` is collector loss; `A` means the worker is already in its
terminal handler and its one-second wait must not be disturbed; any other byte is not
understood and the watcher does nothing. Windows: a wait on the supervisor's process handle,
a `SYNCHRONIZE` duplicate the supervisor made of itself whose process id `adopt` checked
against the boot packet (so it cannot be a reused id); once it signals, `collectorFailed`
decides. Once any terminal handler has begun the watcher does nothing: a crashing worker does
not also start a stop.

*The stop.* For the launcher's stop the worker does what a forwarded `SIGTERM` does: POSIX
`kill(getpid(), SIGTERM)`, Windows `raise(SIGTERM)`, which runs the handler the program
installed (the model loop of `serve` and of the GUI turn it into an orderly shutdown; a
program that has not installed one yet gets the default action). The bound is ten seconds
from the request on the monotonic clock, measured by the watcher, and then the worker ends
hard: POSIX `_exit(128 + SIGTERM)`, Windows `TerminateProcess` with the same code. It is
not `abort`, which would run the crash handlers and publish a report to nobody. The hard end
runs no destructor: the bridge children of a hard-ended worker end with it (POSIX: each sees
its control socket hang up, Windows: they are in a kill-on-close job); the endpoint lock is
released by the system when the process ends (POSIX `flock`; Windows closes the handle during
process rundown, a moment the system chooses, so a restart that races it can still be
refused once, as ADR-0089 notes). The run marker follows the same split: when the worker's
normal stop completes (the program returns and the frontend resumes), the worker removes its
own marker (from an exit handler, below), which the dead supervisor cannot, so a later start does not read the run as an
unexpected end (POSIX unlinks the file while still holding its lock; Windows closes the
handle, which was not opened for deletion, and then removes the file); on the hard-exit path
nothing runs and the marker stays, as the end it is.

*Marker removal and teardown.* The worker removes its own marker, after a launcher's stop, from
an exit handler registered when the marker is created (and only once `main` has returned: an
`exit()` from elsewhere removes nothing). Exit handlers and static destructors run in the reverse
of the order they were set up, so the handler runs after the destructors of everything built
later than that point (the engine, the program's own statics) and before those of statics built
earlier (global objects constructed before `main`, handlers registered before the frontend ran).
What is still not recorded: after a launcher's stop there is no supervisor, so a crash in the
teardown that runs after the handler leaves no report and no marker, and a crash in the teardown
before it leaves the marker and no report (a later start reads it as an unexpected end, with no
stack). A teardown that runs past the stop's bound is ended by the hard exit, which runs no
handler, so the marker stays and it reads as the end it is.

*Known limits.* A fast-fail or a stack overflow too deep for the filter to run (Windows) sets
no flag and reads as the launcher's stop; a supervisor ended by the kernel (the out-of-memory
killer) leaves no notice on any platform and reads the same. Before the Ready packet there is
no watcher, so a kill in the first milliseconds leaves what it always left. The program that
is stopped this way ends as `SIGTERM` ends it: a GUI session is not asked about unsaved
changes. A closed terminal (`SIGHUP` of the supervisor, which has no handler) is also an
external end and stops the worker, which gets the same hangup from the terminal anyway. The
tests decide each case by outcome, never by time: collector loss (the supervisor crashes by a
fault, an abort, or a stack overflow: the worker concludes "collector" and goes on serving),
the supervisor killed (the worker concludes "launcher", takes the stop and is gone), the
supervisor killed with a worker whose stop never finishes (gone within the bound, which the
fixture shortens), and the watcher on a channel a test holds (end of file, the notice, an
acknowledgement byte, an unexpected byte and garbage; the byte is left unconsumed).

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
only after a terminal event or unexpected worker exit (and, from topic 6a, one
report for each plug-in bridge process that dies by itself while the worker lives, under an
incident of its own and with no marker); ordinary acknowledged
returns remove the marker. An active run is not an abandoned incident.

Report schema `supple.crash-report/1.0`: `incident`, `build` (version/commit/dirty),
`platform`, `architecture`, `role`, `kind`, `code`, `native_fatal_status`, `complete`, `unavailable`,
`modules`, `threads`, `plugins`, `bridges` (topic 6a), `commands`, `dropped`, and for a
`bridge_exit` report `bridge` and `attribution`. Strict size/type/enum validation and
bounded allowlisted formatting apply. Phase-2 reports are honest exit summaries;
phase 3 can attest terminal context but persists no raw address/register bytes.
Thread frames added in topic 4 require executable module identity plus relative
PC, never arbitrary stack words. Cap 4 MiB/report, 128 threads, 128 frames/thread.
Readable bounded software names are allowed; personal/project data and arguments
are forbidden. Never create a minidump to sanitize later.

Atomic private publication uses native wide Windows paths and existing atomic
write/durability primitives. Retention prunes oldest validated owned reports until
count <=20 and total <=50 MiB; `bridge_exit` reports (topic 6a) have a budget of their own, the
newest `kKeepBridgeReports` (5), and do not count among the 20, so bridge deaths never evict the
report that makes a plug-in a suspect (the 50 MiB bound covers both). Validation checks the implemented role/kind/platform/architecture
enums, native code/cause bounds, nonnegative drop counters and incomplete-report
reasons. The thread, plug-in and command arrays have nested schemas (topics 4, 5c and 5d)
that are validated field by field; a report whose arrays have another shape is kept, never
authority to delete another file.
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

### Reading reports (topic 7a)

`supple/diagnostics/reports.h` is how an application reads what its earlier runs left; the
frontend writes and this reads, both through the private-file checks (owner, no links, 4 MiB).
`defaultDirectory()` is the folder above (`localData/slug/diagnostics`); every function takes `{}`
for it. Nothing here changes a report: a report is never rewritten.

*Listing.* `listReports` returns `reports` (newest first, by file time), `unreported`, and
counts `liveRuns`, `pending` and `ignored`. A missing folder is an empty listing; one that is a
link or not the user's own is an error (`diagnostics.storage`). A report is summarised by incident,
role, kind (`crash`, `unexpected_exit`, `bridge_exit`, or `other`), platform, the build that wrote it, file time,
`acknowledged`, the attribution level, the plug-in the attribution names (its own strings, its
own `plugin_id`, its `kind` and `origin` as written, the plug-in `format` the kind stands for
(`clap`, `vst3`) and its build id, any of them possibly empty) and the plug-in count. Reading is tolerant where a newer
build writes more: a kind this build does not know is `other`, an attribution level it does not
know is `other`, and top-level fields it does not know are ignored; nested parts it does know
must still validate. Retention keeps the strict check, so a report it cannot fully read is kept
and never authority to delete. A report-named file that is not a valid report (its own `incident`
is its name) is counted in `ignored`, never listed, never touched.

*When a report counts (T8).* A run marker is *held* when another handle holds its lock, and also
when it is empty and under a minute old, because the creator locks before it writes: an empty
marker is one being created. An older empty one (a creator that failed between creating the file
and its first write, which now removes the file itself on a failure it sees) is judged by its lock
like any other, so it cannot read as held for ever; so is an empty one dated in the future (the clock
was set back), which is not young. A marker nobody holds is *free*: the run is over. A report whose marker is held is
still being published and counts in `pending`, not in `reports`; `readReport` and `acknowledge`
refuse it (`diagnostics.no_report`). A report with no marker, or a free one, counts. A free marker
with no report is an `unreported` run: the supervisor died or could not write, and the list says
so with the role the marker recorded. Held markers are `liveRuns` (this process's own among them).
*The supervisor's hold.* The worker's marker is released when the worker dies, which is before
the supervisor has formatted and published the report. The supervisor therefore takes a lock of
its own, `<incident>.pub` (the same file shape and probe), before it starts the worker, and holds
it until the report is published and the worker's marker removed, in that order. A run is held
when its marker or its hold is held, unless the marker is already gone; the reader probes the
hold first, so a run that just ended cannot read as free. Between the worker's death and the
report's publication a run is therefore live (no unreported run, no "no report was written"
notice, `acknowledge`, `settle` and `delete` refused), and from publication to the end of the
supervisor its report is `pending`. A supervisor that dies leaves both files, free: the run is
unreported, deleting it removes both, and a hold with no marker is removed by the next start. A
worker that finds no supervisor left (ADR-0091) removes the hold with its own marker. A hold that
could not be made leaves the older, narrower guarantee.
POSIX probes with a shared `flock` that is released at once, Windows with a shared byte-range
lock (`LockFileEx`) on the byte the holder locked exclusively; a Windows marker being removed by
its own worker is briefly free, so a reader started at that instant can list that run as
unreported (when a supervisor is alive its hold covers that instant). Report retention never touches markers. Markers of runs that ended without a report are bounded
on their own: the newest 20 (by file time) are kept and older ones removed with their `.ack` and
`.settled`, when a
report is published and when a run creates its own marker (a supervisor that died publishes
nothing, so the next start is the moment). A held marker and a marker beside a report are never
removed by it; deleting a run (P11) still removes its marker. The same pass removes an `.ack` or a
`.settled` whose report and run marker are both gone, which only a write that raced a deletion
leaves behind.

*Acknowledgement (T2).* `acknowledge(incident)` writes `<incident>.ack`, canonical JSON
`{"schema":"supple.crash-ack/1.0","incident":…}`, beside the report or the unreported run's
marker, for an incident that exists and whose run is over; it is idempotent, and a file there
that is not a valid acknowledgement does not count and is replaced. Retention removes a report's
`.ack` with it. An acknowledgement of an unreported run stays with its marker.

*Suspects and settling (topic 7d, P4-P8, Q1).* `suspects(directory)` answers which plug-ins the last
reports point at, derived from the files alone. The *window* is the newest `kSuspectReports` (3)
valid reports whose run is over (T8), by file time, acknowledged or not: acknowledging means the
user has seen a report, not that the suspicion is resolved, so a Dismiss never ends one. Files
are ordered by name and time first and opened newest first until three valid ones are found, at
most `kSuspectAttempts` (8) of them, so a folder of damaged files costs a bounded read. A report
in the window yields a suspect when its attribution is `fault_in_image` or `active_token`, it
names a plug-in with an own `plugin_id` and a format this build knows, and it is not settled;
every other level, a plug-in with no id, an unreadable report and a report still being published
yield none, but those that are valid still occupy a place in the window. The exception is a
`bridge_exit` report (topic 6a): a bridge's death is not a run that closed, so it neither names a
suspect nor takes a place in the window, and a loop of bridge crashes cannot push a real crash out
of it. This holds for a bridge report that names a plug-in in its image too (decided, 2026-11-18:
"Bridge self-capture"). A suspect is keyed on
`(format, plugin_id, build id)`; a report with no build id (an `active_token` attribution, every
VST3) is keyed on the version too, so two versions are two suspects and none is an exact build.
The suspect carries the name, vendor, version and `origin` of its newest report (P8 decides on
`origin`: a `stock` plug-in is a Steppe fault, for which the caller offers no bridge), the
strongest level among its reports, and its `incidents`, newest first, with `incidentTimes`, each
report's file time in the same order. The engine's check merges the incidents of a build suspect and a
version suspect of one plug-in (a CLAP plug-in can have both) by those times, so the list it shows
is newest first across them.
`settle(incident)` records that the suspicion a report raised is resolved (the user chose to load
the plug-in normally, or a run loaded it normally and ended well): `<incident>.settled`, canonical
JSON `{"schema":"supple.crash-settled/1.0","incident":…}`, written like `.ack` (idempotent; one that
is not valid does not count and is replaced). It needs a valid report whose run is over
(`diagnostics.no_report` otherwise), so nothing is settled for a run that left no report, and a
settled report keeps its place in the window: an older report does not slide back in. The report is
never rewritten and the acknowledgement is not touched. `settle(suspect)` settles each of the
suspect's incidents and keeps going past a failure. Retention, deleting a report and
`deleteAllReports` remove the `.settled` with the report; deleting the reports is how a user clears
the evidence (P11). The load overrides that act on a suspect (`plugin.override`) are in
`docs/specs/engine.md` §5, and so is the rest of the engine's part (below).

*Safe-load in the engine (topic 7d, design note (c), (d), (e)).* The reader names plug-ins from
reports alone; the engine decides whether the suspicion applies to the plug-in installed now
(`host/engine/src/suspects.h`, `SuspectCheck`), and it never prompts or overrides by itself.
- *Identity.* A suspect applies to an installed plug-in of the same format and id when the file's
  build id is the report's `build.id` (`same`), looking at every image of a universal Mach-O file;
  a different build is not offered (E10). A suspect with no build id (an `active_token`, VST3)
  applies when the installed version is the one the report names (`version`). A file that cannot
  be read, is not an image, or is beyond the budget is asked about (`unreadable`, E11): at most 3
  files, 250 ms from the first read, headers only, each read once. A string compare of ids comes
  first; no file is read for a plug-in no suspect names. The CLAP file is the plug-in's path, or a
  macOS bundle's executable (`host::pluginImageFile`); a VST3 has no build id to compare. The
  parsers are `libs/image-identity`'s, tested on synthetic ELF, Mach-O (also universal) and PE
  bytes on every platform.
- *`project.plugins {path | session | autosave, template?, autosave_file?}`* reads a project file
  (or, with `template`, a template folder or file) with the parser `project.open` uses and loads
  nothing: every branch's head, not
  the history steps behind them, each `(format, id)` once, as the catalogue resolves it, with
  `uses`, `branches`, `mode`, `stock`, `bridgeable`, `resolved` (null when not installed or the
  catalogue is empty: it answers from the catalogue as it stands and never scans,
  `catalogue_empty` says so), and `suspect` (`build` same, version or unreadable, `attribution`,
  `incidents`) when the check above says a report applies. Its `hash` is the SHA-256 of the bytes
  read; `project.open {expect_hash}` refuses other bytes with `project.changed`, so the check and
  the load read one file. Recovery is checked the same way, before it opens anything, because
  after a crash the likeliest next step is `project.recover`: `session` reads the autosave
  `project.recover {session}` would open (the newest in the left-behind folder that parses, none
  meaning recordings only and so no plug-ins), `autosave: true` the one `project.recover
  {autosave: true}` would (`autosave_file` names one, validated as there, errors
  `recover.no_autosave` and `recover.unknown_session` as there). The result's `source` says
  which (`file`, `template`, `session`, `autosave`) and `autosave` names the file read: passed to
  `project.recover` as `autosave_file`, the check and the load read one file; a session has no
  such argument, and its folder is not written by anyone while it waits. Exactly one of `path`,
  `session` and `autosave` is given (`project.plugins_args` otherwise).
- *`project.open`* adds the warning `project.suspect_plugin` (path `/plugins/<id>`) for each
  suspect still on the normal load and loads normally. A suspect already bridged or disabled
  this run raises none, and a settled one is not a suspect. `project.recover` (both sources) adds the same warning to its result's `warnings`, from the
  autosave it is about to load. A stock plug-in's warning says it looks
  like a product fault and offers disable only. The check costs no scan unless a suspect exists
  and the project holds plug-ins. A service (`--mcp`, `serve`, never a one-shot `exec`) writes the
  same text once per plug-in per run on standard error. Remembered rules do not apply headless,
  and nothing here changes how anything loads.
- *Settling by a clean run (E12, Q8).* `EngineApp::settleCleanSuspects()`, which the services call
  when they end normally, settles the reports of a plug-in that ran in this process, activated,
  loaded normally, in this project or an earlier one of the run, when it is the build (or version)
  the report names. A bridged plug-in, a disabled one, one never activated and one whose file
  could not be checked settle nothing (E13), and a run that dies settles nothing, since the call
  is only made on the way out. Only a report older than the run's first sight of the plug-in
  active is settled: the GUI and a headless engine share the folder, and a report another process
  wrote while this one ran says nothing this run can clear.

*Commands.* The engine exposes the reader to agents (P9) as `crash.list {limit?}` (the listing:
reports, unreported runs, `live_runs`, `reports_pending`, `ignored`, each list capped by `limit`,
default 50, with its `_total`), `crash.read {incident}` (the report as written),
`crash.acknowledge {incident}`, `crash.settle {incident}` (what `settle` does: the suspicion of a
plug-in ends, the report stays), `crash.delete {incident}` and `crash.clear` (what
`deleteAllReports` does). The folder is `AppOptions::diagnosticsDir`, empty meaning `<workdir>/diagnostics`; the apps
pass `defaultDirectory()` when they run without `--workdir`, so a test never reads the user's own
reports. A report the reader refuses is `crash.no_report` at `/args/incident`, with a hint to list
first. `writeSampleReport` makes the one report the command examples and tests start from.

*Restart notice (topic 7b, P1, P10).* `lastRunNotice(directory)` decides what a restart says: it
lists the folder's names, probes the markers and reads each acknowledgement, which finds the
unacknowledged reports and unacknowledged unreported runs whose run is over, newest first by file
time (a run's marker time is when it started), and says nothing when there are none. Without a
role it parses only the newest `kNoticeReportsRead` (3, P4) reports; the ones behind them are
counted by file name without being read, so a file that is not a valid report still counts as an
earlier close there, while an unreadable report among the three read is not announced. Unreported
runs cost no read.

*The notice is for one app (F6, decision of the co-director).* Given a `role`, `lastRunNotice`
covers only the incidents of that role: the GUI's banner those of `gui`, a headless service's
stderr line those of `engine`, so that neither announces, and neither's dismissal acknowledges,
the other's. A run's role is what its marker says; a report's is what it says, which means
reading it, so with a role the notice reads at most the newest `kNoticeRoleReads` (8)
unacknowledged report files (not three, and not counted by name): older ones, and any incident
whose role cannot be read, belong to no notice and wait for a later start. The `RestartNotice`
names its `role`, and the engine's text says which app closed ("Steppe engine closed unexpectedly
last time"). `acknowledgeNotice` marks only the incidents its notice covers, which are of its
role. An explicit `acknowledge(incident)` (the command `crash.acknowledge` with a named incident)
still works for any role, and the listing, the viewer and the suspect window (7d) cover every
role. A `bridge_exit` report is never part of a notice: the application did not close. It is
skipped when the notice reads a report (and does not use up one of the role filter's eight reads);
a notice with no role filter counts a report it did not read by name, so callers filter by role,
as the GUI and the engine do. A report gives `Kind::Report`; the headline names the plug-in only when
the attribution is `fault_in_image` or `active_token` and the report names one, and the detail says
the report was saved here, was sent nowhere, and that the report cannot prove the plug-in was the cause; any other
attribution, and a kind this build does not know, gets the plain sentence. An unreported run gives
`Kind::NoReport` and says why that happens. The other unacknowledged incidents are counted in
`earlier` and named in `earlierNote` (a sentence of its own, so that a banner can leave it to the
viewer; `noticeText` joins it last), and `incidents` lists every one the notice covers, newest first, so that
`acknowledgeNotice` marks them all, unreported runs included, by name and without parsing, so the
run is not announced again. The product's name comes from `product::kName`. The wording is
provisional (P12). The GUI shows it as a banner (gui.md). A headless service (`<slug>-engine --mcp`
or the RPC server, never a one-shot `exec`) writes the same kind of text for engine runs, plus how to read and dismiss it
(`crash.list`, `crash.acknowledge`), once to stderr at start, never to a protocol stream, never a
prompt; the agent decides what to do. Nothing in it acknowledges by itself.

*Deleting (P11).* `deleteReport(incident)` removes a finished incident's report, its `.ack`, its `.settled` and,
when nobody holds it, its run marker (that is how an unreported run is dismissed); the marker goes
last, so a deletion that stopped half way can be repeated. `deleteAllReports()` does that for every
incident the listing shows as over and returns the count. A live run, a report still being
published and a file that is not a valid report are never deleted by either: a file this build
cannot read is not its to remove.

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
Frames come from the OS unwinder (`StackWalk64` over each image's unwind data, x64 only;
other Windows architectures report the faulting thread `unsupported`), with memory read
through `ReadProcessMemory`. The unwinder's function-table lookup is answered by the
collector itself: it reads the image's exception directory (`.pdata`, the sorted
`RUNTIME_FUNCTION` entries) from the worker's memory through the same reader, validating
every offset against `SizeOfImage` and searching with at most 32 reads of 12 bytes, and
hands the unwinder the entry. No module file is opened and nothing is loaded into DbgHelp
(the first run that walked threads showed DbgHelp's own lookup finding no function table
for any module it had loaded without symbols, so every frame was taken for a leaf and the
walk ended in data). An address with no image, or with no entry, ends the walk as before.
All DbgHelp work is serialized under one lock, with no symbol path, no environment
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
modules are those the frames fall in, then those of the images the process announced for plug-in
instances (`announcedImage`, kept per cell with or without a sink), so a loaded plug-in on no
stack still has a build; the capsule is sent by 500 ms (a `Message::Trace` packet carrying the byte count, then the
bytes, each wait bounded; both ends put the descriptor in non-blocking mode for the
transfer and restore its flags, because `MSG_DONTWAIT` alone does not make every kernel
refuse to wait for buffer space, and a send to a supervisor that is not reading must end
at its deadline) and the wait for the acknowledgement gets the rest. An image's
header is read once, and what it said is kept, so a stack of addresses outside every image
cannot make the handler read the same headers repeatedly. The supervisor reads the
capsule for 600 ms from when it receives the terminal packet, and the worker takes its own
clock before it publishes that packet, so the supervisor's deadline always falls after the
worker's, however long the worker is descheduled after the write; the send ends with a margin of at least 50 ms before the supervisor gives up (a compile-time
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

Library placement of the capture code (topic 6): the wire packets (`native.h`), the trace capsule
and its parser (`capsule.h`, `trace.h`, `trace.cpp`), the frame-pointer walker and the
per-platform stack readers (`trace_linux.cpp`, `trace_windows.cpp`, `trace_other.cpp`,
`capsule_posix.cpp`, `capture_macos.cpp`) are the GPL library `diagnostics-capture`
(`libs/diagnostics-capture/`, headers `supple/diagcap/`, still namespace
`supple::diagnostics::detail`). They moved there from `host/diagnostics` unchanged, so that a
process that only has to capture its own failure can link them without the supervisor, the
frontend or the report store; `host/diagnostics` links the library and keeps the frontend, the
channel (`native_posix.cpp`, `native_windows.cpp`), the inventory, the storage and the reader. The
library is closed to the permissive tree by the same two checks as `diagnostics-context` (below),
with `supple/diagcap/` as the include to reject. Its tests are its own executable,
`diagnostics-capture.tests` (the walker, module identity, the capsule and its parser, under
`libs/diagnostics-capture/tests/`); what the supervisor does with a trace (publishing, retention,
the inventory's limits) stays in `diagnostics.tests`.

Library placement: the table lives in the GPL library `diagnostics-context`
(`libs/diagnostics-context/`, header `supple/diagctx/table.h`). It is under `libs/` because
command-bus, also there, links it and a `libs/` library does not depend on host products;
it is not in `core-base` or `modules/rack`, which are MIT and plug-in-linked, so no stock
plug-in, module or MIT library links it. Two checks enforce that: `tools/license-check`
rejects an include of `supple/diagctx/` or a mention of the target in any file of the
permissive tree (`plugins/`, `modules/`, the MIT libraries), which covers plug-ins built in
isolation; and `cmake/LicenseIsolation.cmake` walks, at the end of configuration, the link
graph of every library and module built from those directories and fails on one that reaches
the library, directly or through another target (executables are not checked: a test tool
that links the GPL host is a GPL combined work, not an MIT library). Today `diagnostics`,
`engine`, `clap-host` and `command-bus` link it.

The table is one static, zero-filled, page-aligned block per process (74,816 bytes: a
32-byte header, 256 thread slots of 32 bytes, 1,024 instance cells of 64 bytes, one cache
line each, and a 64-entry command ring of 16 bytes, written by the command dispatcher from
step 5d). It
holds integers and enums only, never a string, a pointer into model or plug-in memory or
anything a user typed, and carries a magic and a version. It is not in the binary's file
image (it is `.bss`). The 256 slots are sized for the real maximum of owned threads: the
fixed ones (model, render, MIDI output, recorder, one offline render), a live pool and a
background pool of up to 64 helpers each and an export's own pool of up to 63. A 32-CPU
machine runs 92 of them during an export.

An owned thread registers once while cold, next to `nameThread`:
`registerThread(role, worker)` claims a slot (compare-and-swap, so threads may register
concurrently), stores the role, the ordinal and the thread's stack bounds, then publishes
the operating-system thread id (Linux and Windows thread id, macOS Mach thread port name)
last, so a reader never pairs an id with a stale role. The stack bounds (`pthread_getattr_np`,
`GetCurrentThreadStackLimits`, `pthread_get_stackaddr_np` with `pthread_get_stacksize_np`)
let a stack address find an owned thread without the mapping list; a thread the system will
not describe records none. The slot is released when the thread exits: its contents are
cleared first and its id last, so the slot becomes claimable only after it is clean and a
thread that claims it at once cannot have its role wiped by the leaving thread (a test seam
between the two steps pins the order). Registration is idempotent per thread. A hook, set by
`diagnostics`, runs once on each thread that registers and installs its alternate signal
stack (so the engine does not link `diagnostics`); it runs whether or not the thread got a
slot, so a thread the full table turned away keeps its stack-overflow protection. A full
table counts `threadsDropped` once per thread, the thread is reported as `other`, and a
report read from such a table carries the limit "threads beyond the execution table's
capacity have no role and may show as other". The roles are a closed set: `model` (the main
thread, registered when terminal handling is installed), `audio_render` (the anticipative
renderer), `audio_live` and `audio_background` (the pool helpers, numbered from 1),
`audio_offline`, `recorder`, `midi_output`, and, from topic 6c, `bridge_control` (a bridge
process's command loop, its main thread) and `bridge_audio` (the thread that runs a plug-in's
`process` in a bridge, one per activated instance, numbered from 1 in creation order and wrapping
past 65535). A bridge process registers them in its own execution table, a different one from the
worker's: `diagnostics-context` is linked into the bridge executable, the roles are registered
once while cold (the control thread before the bridge reads a command, an audio thread before its
first block) and released when the thread ends, and the entry scopes `host::Instance` already
marks are the bridge's entries too. The table's version is 2 for the two roles; the worker's table
and a bridge's never meet in one report, and the supervisor reads a bridge's roles only from that
bridge's capsule (6c.4: until then nothing reads them). Not registered, so `other`: the audio driver's
callback thread (created by the driver; registering it would need a thread-id call or TLS
write on the audio path), the dummy backend's thread (it stands in for that driver thread,
and tests should see it as one), `main-wake` (in MIT `core-base`), the sandbox child, and any
thread a plug-in or the OS creates, in a bridge too.

The report labels each thread with its role. The fault thread keeps `role: "fault"` and
adds `registered` with the role it was registered as; a thread of a numbered pool carries
`worker` (1 to 65535) next to its role. `registered` and `worker` appear only where they
say something, and retention validates them: `registered` only on the fault thread and
only a registrable role, `worker` only on a thread with a role. A thread that was inside a
plug-in or module entry says which in `entry` (see Execution scopes); a thread inside none
carries no `entry`.

How the supervisor gets the table: the worker's `ready` packet carries the table's address
(in the packet's `pc` field, which a Ready packet reuses: it is an address, not a program
counter, and the code reaches it only through `setTableAddress` and `tableAddress`; the
private channel is not a place for anything else secret). A later `Table` message would take
over that role. The Windows channel keeps one slot per message kind and hands them over
Ready, then Terminal, then Expected (`nextSharedSlot`), so a worker that publishes Ready and
faults before the supervisor wakes still has its table address known when the terminal packet
is handled; POSIX's stream keeps the order by itself. When the terminal
packet arrives, Linux and Windows read the table with the same bounded remote read the stack
walk uses (`process_vm_readv`, `ReadMemory` on Windows), once, right after the faulting
thread's walk and before any other thread is stopped. It is read even if the walk spent the
deadline: one read, 73 KiB. If the budget ends, what is cut is the traces of the other
threads (`deadline`), never the table; their roles are still known. The table is untrusted
input: it is used only if its magic and version are this build's, a slot with a free,
claiming or unknown role labels nothing, and an address that cannot be read is a stated
limit ("the execution table could not be read, so threads have no roles") with every thread
still traced, labelled by position as before. macOS has no remote read: the in-process
capture labels each thread from the live table's slots (atomic loads, no lock or
allocation) and the capsule carries the result in each thread's header (`registered`,
`worker`, `entry`, with the cell and generation of the entry) and the table's counters in the
capsule header. The capsule wire format is version 3 for that (magic `CTC3`); it carries no
copy of the table: what the cells are comes from the inventory messages (topic 5c), which
are sent as they happen and need nothing from the failing process at the crash.

### Execution scopes (topic 5b)

Each in-process plug-in instance, and each loaded plug-in library (a CLAP module or a VST3
module), owns one cell of the table: `acquireCell` when it is created or opened, `releaseCell`
after everything about it is gone. A cell has two words, `audio` for the entries the audio
thread makes and `main` for the rest, and a *scope* marks one of them for as long as a thread
is inside an entry. A word packs the entry kind (8 bits), a generation (12 bits, counting the
cell's reuses) and the scope's stack address divided by 16 (44 bits, so below 2^48; an address
the word cannot hold is recorded as none and counted in `scopeOverflow`). The generation is a
hint a reader checks against the inventory, never an identity: it repeats after 4,096 reuses.

Entries are a closed set of at most 20 kinds, and the report names them: `create` (the
plug-in's init, create and extension lookups), `destroy`, `activate`, `deactivate`,
`start_processing`, `stop_processing`, `process`, `flush`, `reset` (all five on the audio word),
`params` (reads, writes and conversions), `state`, `semantic` (schema, patch, describe, status,
factory patches), `editor_picture`, `trace`, `gui` (a native editor window), `main_thread`
(`on_main_thread`, timers and file-descriptor callbacks), `library_open` (the library's load,
its `clap_entry.init` and the factory lookup), `library_close` and `bridge_call`. An isolated
instance (running in a bridge child) records every entry as `bridge_call`: the host thread is
waiting on the child, and the child's own table is a later step. Every call into the plug-in
goes through a `host::Instance` method, so the scopes sit in those methods (and in
`Library::open`, `~Library` and the VST3 module load, which are outside any instance); the
offline render, both real-time executors, the sandbox and the rack's plug-in module all reach
the plug-in only through them.

The `audio` word needs no guard. The `main` word could see two threads at once if the host
ever overlapped main-thread entries of one instance (an editor opened on one thread while
another reads a parameter), and interleaved writes there could leave a stale word naming an
entry nobody is in, which matching would attach to the wrong thread. So a main scope first
claims the cell's `mainOwner` word with one compare-and-swap (the thread's token, released
when its outermost scope ends); a thread that finds another owner records nothing, and the
owner's nested scopes still record. The audio scopes never touch it. A scope writes its word on entry and puts the previous word back on exit, so scopes nest (a
plug-in that calls back into the host; `create` around the library's open) and a word is idle
exactly while no thread is inside an entry of that instance. The cost on the audio path is two
stores, one on entry and one on exit, each a release store: a plain `mov` on x86-64 and a
`stlr` on arm64, with nothing before or after them. Nothing stronger is needed because a word
has one writer at a time (the thread inside the entry: the executors never run one instance's
audio entries concurrently) and the reader only looks after that thread has been stopped, so it
neither races the writer nor needs the store to be seen in any order against others (a
stopped thread's writes are complete, and stopping it is itself a barrier). Release, and not
relaxed, is what keeps a word from being seen before the writes it announces by a reader that
does run alongside (an in-process observer, as the tests are); no fence and no
sequentially consistent store is used, since nothing orders two words against each other.
Each cell fills a cache line of its own: with four 16-byte
cells to a line, scopes of instances processed at the same moment by different workers
measured 6.0, 5.7 and 9.1 ns each for 2, 4 and 8 workers, against 1.75, 1.74 and 1.84 ns with
a line to each cell (`SUPPLE_SCOPE_BENCH=1`, release preset, informational: no test decides
anything by it); the padded layout, one worker alone, adds 0.8 ns per scope.

*Tying an entry to a thread.* The supervisor (and, on macOS, the worker's capture) ties a word
to a thread when its recorded address lies on the thread's stack at or above the thread's stack
pointer. A registered thread's stack is the bounds in its slot; any other thread's (the audio
driver's callback thread, which stays unregistered) is the mapping that holds its stack
pointer (`/proc/<pid>/maps` on Linux, the allocation `VirtualQueryEx` finds on Windows, the
region `mach_vm_region` finds on macOS). Of several entries on one stack the one with the lowest
address is the innermost, and that is the entry the thread is reported in; the outer ones are
still counted as tied. The address is the frame address (`__builtin_frame_address(0)`) of the function that
declares the scope, taken in that function and passed to the scope's constructor as a defaulted
argument, never taken inside the constructor: a constructor that is not inlined (a Debug
build) has a frame of its own that is gone when it returns and that the entry's calls reuse. On
every architecture the frame address is the function's frame-pointer register (x86-64: the slot
holding the saved rbp; arm64: the frame record), which stays at or above the function's stack
pointer while it runs, and every frame of a function it calls lies wholly below the caller's
stack pointer. So a thread inside an entry has its stack pointer at or below the recorded
address, and a scope in a called function has a strictly lower address than the scope that calls
it, which is why the lowest is the innermost. Nothing else is promised: where the frame address
sits among the function's locals is the compiler's choice (arm64 compilers differ), so nothing
compares it with a local's address, and two scopes in one function share an address and cannot
be told apart; the host's scopes each sit in a function of their own. A scope that is not a
local records an address on no stack, and a reader does not tie it to a thread. Another thread's word is not quiescent while the table is read, so Linux and Windows read
each candidate word again once the thread is stopped and accept it only if both reads agree
(the fault thread is parked in its handler and needs no second read); macOS stops the thread
before it reads, and one read does the work of two. An entry left out is one the thread entered
after the table was read. A word that no thread read could claim (its thread was not read, it
names no address, or the address lies on no stack, which a forged address is) is counted, and
the report says so in a limit rather than guessing: "some plug-in entries were open that no
thread read could be tied to". The report also says, as limits, when instances found no cell
(`untracked`) and when scopes could not record an address (`scopeOverflow`).

The report shows only `entry`, the kind, in the thread's object. The cell and generation stay
in the process and the supervisor (the inventory, below, says what each cell is, and the
report refers to an instance by an id of its own), and no address or stack bound reaches a
report.

Threads other than the faulting one keep running while the table is read, so a later
step's instance cells describe the moment of the read, not the moment of the fault, and a
remote read promises nothing about a word being copied whole (`process_vm_readv` and
`ReadProcessMemory` are plain copies; only macOS's in-process loads are atomic). The reader
therefore reads a cell again after the thread it matches is stopped and accepts only an
entry both reads agree on; a generation or an ordinal in a cell is a hint checked against
the inventory, never an identity. Only the faulting thread is parked, and the thread slots
do not have this problem: they change only when a thread starts or ends.

### Plug-in inventory (topic 5c)

A cell says that a thread is inside an entry of some instance; the inventory says what the
instance is. `host::Instance` and the two library loaders announce each CLAP or VST3 instance,
and each loaded library, through `supple/diagctx/inventory.h` (`announce`, `announceState`,
`announceRemove`; cold calls that do nothing while no sink is installed, which is the case in
every process that runs no supervisor). An instance is announced `loading`, with its descriptor
strings, before any code of the plug-in runs (a crash in the library's load or in the plug-in's
constructor still finds it); again, with the module it came from, once its library is open;
`inactive` when its creation ends; `active` and `inactive` as it is activated and deactivated;
and removed, just before its cell is given back. A library has an entry of its own
(`clap_library`, `vst3_library`: `loading` while its code is being loaded, `active` once open,
removed when it is unloaded), so a crash inside a plug-in's load that has not yet said what
plug-in it is still shows as a library being loaded. Rack modules have no cell yet, so none are
announced; their identity rules (allowlisted ids for host and stock modules, an ordinal only
for library and project modules, never a name, source, parameter or port) are in the origin
field's closed set and applied when modules get cells.

*Which threads announce.* Creating, activating, deactivating and destroying an instance or a
library: the model thread, the plan compiler, a scan or sandbox session thread. Never `process`,
`reset`, a flush, or start/stop of processing. Two checks hold that. The announcement functions
carry no `nonblocking`, so clang's function-effects analysis rejects a call from a `SUPPLE_RT`
function at compile time. And every call takes a lock, with a sink installed or not, so under
the `rtsan` preset an announcement reached from an audio path aborts even in a process with no
sink: `rtsan-announce-check` (a program that announces inside a `SUPPLE_RT` function, registered
`WILL_FAIL`, beside a control that makes the same calls outside one).

*Transport.* The diagnostics frontend installs the sink after it has sent Ready, and only on a
trusted channel. Each announcement becomes one `Instance` packet (protocol version 2: the
packet grew by the `InstanceRecord`, fixed-size plain fields, and by the worker's count of
events dropped so far; the `InstanceRecord` later gained the bridge fields of topic 6a, with no
change of version since both ends are one executable; 400 bytes, under 512). The send never waits for the supervisor. POSIX
writes it to the stream socket in one non-blocking send (the descriptor is put in non-blocking
mode for the call and its flags restored, as for the capsule: `MSG_DONTWAIT` alone does not make
macOS refuse a full buffer instead of waiting); a stream socket may take part of a
message (macOS does), so the rest of a part-sent packet is finished with a bounded wait
(250 ms, `poll`), since a stream cannot take a part back; if even that fails the stream is torn
and the supervisor treats the next packet as damaged, as it does for any. When it fails the
sink closes for good (the drop counted), and announcements queued behind it give up at once, so
a process waits for a stalled supervisor at most once and never once per announcement. The Windows channel
keeps `Instance` packets in a ring of 256 whole packets next to its one-slot-per-kind
hand-over: the worker advances `head` after it filled a slot, the supervisor `tail` after it
copied one out, and the supervisor drains the ring before it takes a slot, so that the
terminal packet is handled after every event sent before it. A full ring or socket drops the
event and counts it. One send runs at a time. Once a terminal handoff begins, no further event
is sent (`closeInventory`: a flag and a counter of sends in progress, waited out for as long as
a send can take, the 250 ms a part-sent packet is given plus 50 ms for scheduling; the close
publishes that 250 ms as one deadline, before it closes the gate, and a send finishing a part-sent
packet stops at its own bound or at that deadline, whichever is earlier, so a sender preempted
after the gate cannot go on after the close has given up; the loop (`finishPartSend`, POSIX) checks the
deadline before every attempt, so one already past stops it before it sends anything, and it is
tested on its own on a socket pair): on macOS the
capsule follows the terminal packet on the same stream, and another thread's packet inside it
would corrupt it, as would the terminal packet written into the rest of a part-sent one. If a
send left the stream torn, the terminal packet is not sent at all, since the supervisor would read
it as the end of the torn packet and stop: the process then dies with no terminal packet and no
hold (on macOS no capsule either), and the supervisor reports it from its exit status alone. The count of dropped events rides on every
`Instance` packet and on the terminal packet; the supervisor keeps the largest it saw.

*Supervisor.* `Inventory` applies each packet. Every field is untrusted: a record whose cell
is not in the table, whose generation does not fit 12 bits, whose enums are outside their
closed sets, or whose lengths exceed 64, and an event for an instance it was never told about
(its announcement was lost, or the generation is not the cell's), is counted and changes
nothing, so nothing is invented from half a description. A cell announced again in the same
generation replaces the description (same id); in the next generation it first retires
whoever held it, whether or not that one said it left. There is one live entry per cell (1,024)
and a ring of the last 64 retired entries, which stay in the report marked `destroyed`; older
ones are counted. The ids in the report are the order in which instances were first announced,
a number of the report's own that says nothing about a cell.

*Plug-in strings* (name, vendor, version, id) are cut to 64 bytes by the sender and cleaned by
the supervisor: text that is not valid UTF-8, and every control character (C0, DEL, C1), each
become one `?`. They are untrusted, readable names ADR-0083 allows; they are not proof of
anything, and the validator checks them again before a report counts as one the product wrote.
What is announced is a plug-in's own descriptor strings and nothing else of the user's: no
path, project name, parameter, preset or text typed into Steppe. For a plug-in the user
exported (`dsp.export` builds a CLAP plug-in from their script) the descriptor is text the user
or their agent chose, so a name given to such a plug-in appears in a report when it was loaded
at the crash; it is the plug-in's name, not a project's, and is cut and cleaned like any other.

*Module of an instance.* Each announcement of an open library or instance carries an address
inside the plug-in's binary (the CLAP entry's `init` function). It never reaches a report; the
supervisor finds the executable region that holds it among those it read for the module list
and reports that module's id and format as `build`, so a fault in that image can be matched to
the instance. Without a terminal capsule there are no regions and `build` is absent; VST3
instances and bridged instances (their code is in the child, or in a module the SDK loads
without telling the host its image) carry no address, so they have no `build`.

*Report.* `plugins` is an array in id order, each entry an object with `id`, `kind` (`clap`,
`vst3`, `clap_library`, `vst3_library`), `origin` (`foreign`; `host`, `stock`, `library` and
`project` are for modules), `mode` (`in_process`, `bridged`), `state` (`loading`, `active`,
`inactive`, `destroyed`), optionally `build` (`{format, id}`), `name`, `vendor`, `version` and
`plugin_id` (each at most 64 bytes, only when sent), and `executing` when a thread was inside
the instance: one `{entry, thread, match}` for each thread whose innermost entry (tied by the
rule above) is in this instance, `thread` being the thread's ordinal in the report and `match`
`thread` for the faulting thread and `other_thread` for any other. What the report concludes
from this (which instance to blame, with what confidence) is the attribution of a later step;
`plugins` is the evidence. The `unavailable` list says when events were lost (damaged or
unmatched, dropped by the worker, or beyond the last 64 destroyed instances), so the list is
never taken for complete without that being said. The strict validator checks every key,
enum, string (length, UTF-8, no control character), the ids (strictly increasing), `build`
(a known format and lowercase hex) and each `executing` entry (a known entry name, a thread
of the report, a known match).

### Bridge processes (topic 6a)

A bridged plug-in runs in a child process the host started (ADR-0013). A report says which
child each instance ran in, and whether that child was still there.

*Announcing.* `diagctx::Event` and the packet's `InstanceRecord` carry `bridge`: 0 is this
process, n >= 1 is the n-th bridge process the worker started, never reused within a run. An
instance's `Add` carries the bridge it runs in. A new event kind `Bridge` says a bridge started
(`how` none, no status) or ended (`how` `exit`, `signal`, `exception` or `host`, and the native
`status`: the POSIX signal or exit code, the Windows status). `announceBridge` takes the same
lock as every announcement, so none can run on an audio path (the `rtsan` check covers it); the
host calls it only from the thread that spawns a bridge and the main thread that observes its
end.

*Host side* (`host/clap-host/src/bridge_diagnostics.*`, `BridgeProcess`). `BridgeProcess::spawn`
takes the next ordinal and announces the start. `Instance::create` makes the bridge process
before it announces the instance, so the instance's first announcement (`loading`, before its
`Create` goes to the child) already names the bridge it runs in. A bridge's end is announced once,
from `reap()` on the main thread (the one place the host observes it), or from the bridge's
destructor when nobody reaped it, never from `fail()` or `abandon()` (which can run on any thread,
or on an audio path). `hostKilled_` is set inside `killBridge()`, before the kill, so every host
kill (a hung or abandoned bridge, a protocol violation, a shutdown) is told from the operating
system's: the end is `host`, with the status the host's kill gives (POSIX `SIGKILL`; Windows exit
code 1), which says nothing about the plug-in, unless the status is another fatal one (below). A bridge the host lets go
of while it still runs is also `host`, unless it is then found to have ended by a signal or an
exception the host did not cause (a crash whose dump was still being written when the host let go
is alive until it is done): that is its own death. Otherwise POSIX says `signal` with the signal number, or
`exit` with code 0 (the exit code is not read); Windows says `exception` for a status with the
fatal severity bits set (`0xC0000000`) and `exit` for any other, with the exit code. When both
happen (the host kills a bridge that was already dying: its core or dump still being written when a
failed call or a missed deadline made the host kill it) the status tells the order: a host kill
ends the process with `SIGKILL` (exit code 1 on Windows), so any other fatal status is the
bridge's own fault, which came first (the kill only cut the dump short), and the end is `signal`
or `exception` with that status. The rule rests on an assumption: the kernel keeps the original
signal as the exit status when a core dump is cut short by a later kill (observed on Linux; not
checked on macOS), and on Windows that `TerminateProcess` and `TerminateJobObject` leave an
earlier exception's status alone when the process is already ending (by reading only). The
fallback if a platform reports the kill instead: the status reads `SIGKILL` (exit code 1), the end
is `host` and no report is written, which is how the host classified every kill before. The 65536th bridge of a run has no ordinal (it reads as this process): a stated limit.

*A report when the worker lives.* A bridge that ends by a signal or an exception (not by the
host's kill, not by an exit: the exit code is not read on POSIX) while the worker goes on gets a
report of its own, `kind` `bridge_exit`. (On Windows an `exit` includes a bridge ended by `raise(SIGSEGV)`,
and may include one ended by `abort()` or an uncaught `std::terminate`: the C runtime ends such a
process with exit code 3 and no exception, so the host cannot tell it from an orderly exit and
writes no report. Left to the runtime, `abort()` does not end a bridge at once on Windows: the first native run
(CI 44242073297, the Debug runtime) read an `abort()` as `host`, status 1, which is the host's own kill:
the bridge had not ended when the host's deadline killed it, and the likeliest cause (the log has no
more than the status; the build flags and the other test programs' own settings point the same way) is the
debug runtime's "abort() has been called" dialog or an error-reporting wait, which nobody answers on a
runner. So the bridge sets its runtime at start (`wmain`): no abort message, no report on abort, runtime errors and
assertions to standard error, `SetErrorMode` for the system's own boxes, and a `SIGABRT` handler that ends the process
at once with `0xC0000409`, a fast-fail's status, which is fatal by its severity bits and so is read as the
bridge's own fault (exception, with a report). A plug-in's `abort()` and an uncaught `std::terminate`
(which calls `abort()`) therefore read as an exception. This reaches the runtime the bridge shares with its
plug-ins (the DLL one); a plug-in with a static runtime has an `abort()` of its own that the bridge's
settings do not reach, and `raise(SIGSEGV)` is not an abort (the runtime ends the process with code 3, an exit,
no report: a known gap, STATUS.md). The settings are unproven until the `abort()` case of
`test_bridge_diagnostics.cpp` (the crash fixture's mode 6) has run on Windows CI: it expects the exception and
the status `0xC0000409` there, and names what it got otherwise. A bridge ended by an access
violation, a stack overflow or `__fastfail` ends with an exception status and is reported.) The supervisor writes it from its inventory when it
applies the `Bridge` end event, under a new incident id, with no run marker or hold, so a reader
counts it the moment it is there. The worker's own run, marker and eventual report are untouched.
The report has `bridge` (the ordinal), `code` (the native status, the same as `bridges[].ended.code`),
no threads, no commands, `complete` false, `terminal_context` false, `native_fatal_status` false, and
`unavailable` says the bridge process's threads were not collected: the report is the host's
account of it, nothing the bridge itself captured (a later step). `attribution` is
`{confidence: "bridge_exit", plugin}`, the plug-in the only member of the bridge that had not been
destroyed, or null when there is none or there are several (the host thread's own `bridge_call`
entry is not read for it yet). `bridge_exit` is weaker than `fault_in_image`: it names no
instruction. It belongs to a `bridge_exit` report only, and a `bridge_exit` report carries no other
level. At most 3 are written per worker run, and none for a bridge whose members (plug-in id and
version, or the name when it has no id) and native status repeat one already reported (until the
bridge's own modules are read, those stand in for a build); the others are counted in
`dropped.bridge_reports` of every later report of the run, with a line in `unavailable`, and so is
an end by a signal or an exception that had no room in the list of 256 running bridges (it is never
considered, so it could not have a report). There is no `Message::BridgeEnd` and no watcher: the `Bridge` end event travels in order on the same
channel as the other inventory events, so the supervisor alone decides, from what it has applied,
whether the death is reported. The worker names the report, though: when it announces an end by a
signal or an exception (the only ends that may be reported) and a supervisor is there, it mints the
incident id (`diagctx::announceBridge` returns it; 32 lowercase hex characters), sends it in the
end event's text (the `id` field of the identity, the only text a `Bridge` event may carry, and
only on those two ends), and the supervisor writes the report under that id, or under one of its
own when the worker sent none. The id is therefore known to the application before the report is
written, and names nothing when the supervisor writes none (a repeat, the run's limit, a folder it
cannot write): a reader of the id handles a missing report. `Instance::bridgeIncident()` gives it
for a bridged instance once the bridge's end was observed, and the engine puts it in the failed fx's
`engine.status.failed_errors[]` entry and in `fx.failed` as `incident` (docs/specs/engine.md). A
bridge death does not wake the model by itself. The
report is formatted from the inventory as it is when the end is applied (in memory) and written,
with the folder's retention pass (which reads every report in it), on a writer thread of the
supervisor's own (`BackgroundWriter`, `host/diagnostics/src/background.h`), so the packets behind
the end, among them the worker's own terminal packet, do not wait for the disk. The writer runs its
work in order; the supervisor drains it and joins it before it writes the worker's own report and
before it ends, so no bridge report is lost to the exit and two retention passes never overlap. A report
that cannot be written prints one line to the supervisor's standard error. The strict validator
holds the kind to its parts: `bridge` is a listed bridge that ended by `signal` or `exception`
with the report's `code`, `attribution` is the `bridge_exit` level and its plug-in a listed member
of that bridge that was not destroyed, and no other kind has `bridge` or that level.

*Supervisor.* `Inventory` keeps a list of bridges by ordinal, at most 256: a bridge is made by
its start, or by an instance or an end that names it when the start was lost, and an end after
an end changes nothing. A record that has a `how` or `status` where only a `Bridge` event may
(an `Add`, a name chunk), or a `Bridge` event with anything else set, is damaged: counted and
ignored. Past 256, the oldest bridge that has ended falls out and is counted in
`dropped.bridges`.

*Report.* `plugins[].bridge` is the ordinal, present only for an instance that ran in a bridge.
`bridges` is an array in ordinal order of `{id, state, ended?, instances}`: `state` is `running`
or `ended`; `ended` (exactly when ended) is `{how, code}`; `instances` is the ids of the
`plugins` that ran in it. Without a bridge it is an empty array. Reports of earlier builds have
neither key, and the reader treats a report without them as one with no bridges. The strict
validator holds the two sections to each other: ordinals strictly increasing from 1, the closed
sets, an integer code in 32 bits, every listed instance a listed plug-in whose `bridge` is that
ordinal, and every plug-in's `bridge` a listed one unless `dropped.bridges` says some were left
out.

### The child's identity (topic 6b)

A bridged instance exists twice: as the host's own cell (its entry is `bridge_call`, announced as
above) and as a cell in the bridge process's own execution table, where the plug-in's real entries
run. The bridge says where the second is when it answers `Create` (bridge protocol 26 on POSIX,
27 on Windows, `docs/specs/sandbox.md`), and the host passes it on so that what a bridge's table
says can be matched to the instance the report lists.

*Announcing.* A new event kind `Child` (`diagctx::announceChild`) carries the host's `cell` and
`generation` for the instance and, new in `diagctx::Event` and in the packet's `InstanceRecord`,
`childCell` and `childGeneration` (the instance's cell and that cell's generation in the bridge's
table) and, in `image`, an address in the plug-in's binary in the bridge's address space. It is
announced once, by the thread that creates the instance, after the bridge answered `Create`; it
takes the same lock as every announcement, so none can run on an audio path. The fields mean
nothing in any other event: an `Add`, a `Bridge` event or a name chunk with a `childCell` or a
`childGeneration` is damaged.

*Supervisor.* The `Inventory` keeps the three values on the instance's entry as private matching
data (`PluginEntry::child`), and `findInBridge(bridge, cell, generation)` answers which instance
of the report was at that place of that bridge's table (a live one first, the newest if two claim
it, then the latest destroyed one). A `Child` event is refused, counted and ignored unless it
names an instance that was announced, with that generation, as bridged and in a bridge, with both
cells and generations in the table's range and nothing else in the record set. Describing an
instance again (`Add` with the same generation) keeps where it is; a new incarnation of the cell
does not inherit it. Nothing of it is ever in a report: not the cell, not the generation, and not
the address, which belongs to another process and must never be looked up among the worker's own
modules. Nothing reads it yet: 6c matches a bridge's capsule with it ("Bridge self-capture").

### Bridge self-capture (topic 6c)

**Status: 6c.3 built, the rest is contract.** Built: the roles (6c.1), the bridge's own capture
(6c.2: the handlers, the CTC5 capsule, the cold module list and the slot's protocol, the writer's
side), and the carriage of the slot (6c.3): the host hands each bridge its slot (`AttachCaptureSlot`,
bridge protocol 27, 28 on Windows: `docs/specs/sandbox.md`) and tells the supervisor with a `Slot`
event (channel protocol 3), which keeps the slot with the bridge's entry and can copy its capsule
out (`Inventory::slotCapsule`). Exercised for real on Linux, from a real plug-in fault in a real
bridge to the capsule the supervisor read; the Windows and macOS code compiles and is proven by
native CI only. Built in 6c.4a: the parser (`parseBridgeCapsule`, below). Not built: labelling
the capsule and writing it into the report (6c.4b onward): nothing reads what the supervisor holds
yet, so a report is still today's.
*Why the bridge captures itself.* The worker's capture works because the supervisor, its parent,
reads the stopped worker while it waits in its handler. A bridge is a child of the worker and is
connected to no supervisor; by the time anyone looks it is dead. Nothing can be read remotely, so
the bridge must describe its own death, in its own fault handler, into storage that already
exists and outlives it, without anything that can block. The goal is narrow: the fault thread's
frames, which of the bridge's plug-ins the fault instruction lies in (with that plug-in's build
id), and the roles and entries of the bridge's registered threads. The result is a
`bridge_exit` report that can say `fault_in_image`. It is not a crash dump and holds no stack or
register bytes, as the ADR forbids.

*The slot (decision 1).* One shared-memory region per bridge process, the **capture slot**,
created by the host in the worker before the bridge's `Create` and shared with two readers:
the bridge, which writes it, and the supervisor, which reads it. Memory only, never a disk file
(the ADR forbids temporary memory dumps): a `memfd` sealed against shrink and grow on Linux (the
slot size is a compile-time constant, 256 KiB, checked against the capsule's maximum plus the
execution table's 74,816 bytes), an anonymous `shm_open` object unlinked at once with its size set
once on macOS, a pagefile-backed file mapping on Windows. Layout: a 64-byte header
(`magic` "CSLT", `version` 1, `state`, `length` of the capsule, `capacity` (the slot's size, which a
reader checks against the one it knows), the bridge's `ordinal`) followed by the capsule. `state` is one
atomic 32-bit word: 0 empty, 1 writing, 2 complete; the handler sets 1, writes, then publishes 2
with a release store, so a reader that sees 2 sees the bytes, and a reader that sees 1 knows the
bridge died mid-write and uses nothing. The slot is written once per bridge (a re-entering
handler terminates, as the worker's does).

*Handing it over.* Not by command line or by inheritance, so no spawn path changes and nothing
extra leaks to the plug-ins' own children. One new process-level operation of the bridge protocol,
`AttachCaptureSlot` (instance 0, appended after the existing operations, payload `u32 version`,
answered before any `Create`): POSIX sends the descriptor with `SCM_RIGHTS` like the notification eventfd
(the received copy is `CLOEXEC`); Windows follows the convention of the notification events: the
host duplicates the mapping handle into the bridge with `DuplicateHandle` and sends the
recipient-owned handle value, which the bridge validates (a section handle, the expected size)
before mapping. Built as specified, and the host attaches only the slot it has initialized, then
announces it (below) once the bridge has taken it, so a `Slot` event always names a slot a bridge
can write. Missing, extra, duplicate or wrong-type resources and wrong versions are
structured errors, as for the subscription. A bridge that does not attach (an older bridge, a
refusal, or a death before the operation) leaves today's report exactly. The bridge protocol and
the Windows native version each rise by one with this operation (`docs/specs/sandbox.md`), and a
host refuses a bridge that does not know it only for capture: the bridge still works, with no
capsule.

*The supervisor reads it.* The worker announces the slot to the supervisor in order on the
inventory channel (a new event kind `Slot`, bridge ordinal, one resource: POSIX a descriptor in
the packet's ancillary data, Windows a handle value the supervisor duplicates with the process
handle it already holds for the worker, as the notification convention does) and the
supervisor keeps it with the bridge's entry. This is the one new capability of the private
channel: its POSIX stream carries fixed-size packets only, so a packet that has a resource is sent
with `sendmsg` and a single descriptor as ancillary data (attached to the packet's first byte,
so a partial send that is finished later still delivers it; a received copy is `CLOEXEC`; any
other ancillary data, a second descriptor or a descriptor on a packet that may carry none is
damage, counted and closed), and the Windows channel's packet has a handle-value field that no
other event uses. The channel's protocol version rises by one (`native.h`: 3; `Packet::resource`, the handle value
of the Windows packet, is 0 on POSIX). The alternative, if the
review of this step prefers no new channel capability: the worker reads the complete slot at the
bridge's end and forwards it as chunked packets on the existing channel under the existing
nonblocking send and deadline rules; it costs the worker a bounded send of up to 256 KiB from the
thread that observes the end, which is why it is not the recommendation. No capsule bytes pass through the worker: the worker
only says, as today and in order, that the bridge ended. When the supervisor applies that end it
reads the slot: Linux with `pread` of at most the slot size into a bounded buffer (never a map: a
mapping that someone shrank is a fault, a read is an error; the memfd is sealed against shrinking
anyway), macOS and Windows by mapping read-only an object of fixed extent and copying (macOS makes
the slot with `shm_open`, an object XNU lets nobody `read` or `pread`, only map; its size is set
once, before the slot is handed over, and every read checks it again before it maps). The supervisor takes the slot's descriptor only for a bridge
the end event names and closes it on every path (a count of open slots is bounded by the 256
bridges the inventory keeps, and the oldest ended one falls out with its slot). A slot that is
empty, still `writing`, wrong in magic, version or length, or whose bytes the parser refuses,
changes nothing: the report is today's. The supervisor writes a bridge report from the slot on
the same background writer as today's, so nothing waits on the disk.

*The `Slot` event as built (6c.3).* One new `EventKind` (`Slot`, 7) whose record carries only
`bridge` (every other field and every text length is zero, or the record is damaged) and one
resource beside the packet. The worker's sink sends the resource it is given and never closes it
(`diagctx::announceSlot(bridge, resource)`, a cold call by the thread that started the bridge, after
the bridge answered the attach); the host closes its own descriptor at once on POSIX, since a
descriptor in flight keeps the object, and holds its handle until the bridge's process object goes
on Windows. The only packet that may carry a resource is a `Slot` event (`mayCarryResource`);
`receive` hands the supervisor that packet's resource, and refuses and counts (`refusedResources`,
added to the inventory's `rejected`) every other shape: a descriptor on any other packet, a second
descriptor, control data of another kind, a descriptor attached to any byte but the packet's first
(the first read of a packet takes one byte, so the first byte's descriptor is told from a later
one's even when the sends were merged), and on Windows a handle that cannot be duplicated from the
worker or a value on a packet that may carry none (never duplicated). A refused descriptor is
closed where it is found. The inventory takes the resource only for a `Slot` event that names a
bridge that started and has not ended, once, and only after `CaptureSlot::adopt` found it to be
shared memory of exactly the slot's size (POSIX: a regular object, on Linux a memfd sealed against
shrinking and growing; Windows: a section whose whole extent is that, found by mapping it and
asking the region) with a slot's header for that bridge; anything else leaves the resource with the
caller, which closes it (the frontend does this at the end of every turn of its loop). The slot is
held with the bridge's entry and closed with it, so no more than 256 are open and the oldest ended
bridge takes its slot out with it. `slotCapsule(bridge)` copies the capsule out of a complete slot:
Linux `pread` of the header and then `length` bytes (never a mapping), macOS a read-only mapping of
the object and Windows a read-only view of the section, each copied and unmapped (the tests can make
POSIX read by mapping: `readSlotsByMappingForTests`); it gives nothing for an empty or still-`writing` slot or a header
that stopped being a slot's. *Windows, not proven:* the supervisor duplicates the handle from the
worker when it reads the packet, which may be after the worker closed it (the bridge's process
object went) and the value was reused; the mapping check and the ordinal in the header decide
whether what it got is the slot, and anything else is refused, but the race itself is unexercised.

*The parser as built (6c.4a).* `parseBridgeCapsule(bytes, size)` in `diagnostics-capture`
(`trace.h`), beside `parseCapsule`, with which it shares the thread reader (every role, reason,
cell, generation and frame count validated; the fault thread first and the only one). It holds the
bytes to what a handler writes and gives nothing otherwise: the `CTC5` magic, flags that are only
the three the writer sets and always the table's, at least one and at most 128 threads, at most 256
modules, `dropped` agreeing with its flag, no `unplaced` count (the parser works it out), no
commands and no damaged or refused ones, modules whose range is not empty, whose identity is at most
32 bytes with zeroes after it and whose reserved word is zero, the table copy whole and one this
build wrote (`imageValid`), and no byte left over. Frames become a module index and an offset; the
identity of a module is the lowercase hex of its id bytes (none for length 0). The header's
`reserved` (the fault thread's id, low 32 bits) is any value: it only decides which slot of the
table is left out. **Listing the other threads.** A capsule of exactly one thread (what Linux and
Windows write) is completed from the table copy: every other slot with an id (not free, not being
claimed, not repeated) and a registered role, in the table's order, as a thread of role `other`,
`unavailable` with reason `not_collected`, with its registered role and worker number, and the
entry it was inside as the table says: the open word of lowest address among those whose stack
address lies in the thread's recorded stack (a thread that recorded none has none). Past 128
threads the rest are counted in `dropped`. A capsule of several threads (macOS reads them all) is
taken as it is and lists nothing from the table. The entries of the capsule's own threads and these
are the words the table copy shows; an open word that no thread claimed, or that has no address, is
counted in `unplacedEntries` (and so in `dropped.cells`), as for the worker's. A word the capsule's
fault thread names but that the copy no longer shows as it was is left unplaced, never invented.
Tested with hand-built capsules (every damage above, every truncation, a byte flipped anywhere in
the first 800) and with the bytes of a real fault in a real bridge process.

*Trust (decision 2).* The capsule is untrusted input of the same kind as the worker's. A bridge
runs third-party code that can write the slot as well as the bridge's own handler can, so the
existing capsule parser and its caps are the only reader (strict counts, roles, ranges, bytes
consumed exactly), no address from the slot leaves the parser (a frame is a module index and an
offset, as for the worker), and the worst a hostile plug-in can do is mislabel its own bridge's
report. A bridge never writes anything outside its slot, and the supervisor never trusts a length
from it beyond the slot size it knows. The report records that a bridge capsule is
bridge-reported (`capture: "bridge"`, below) so a reader weighs it so.

*What the capsule holds (CTC5).* The capsule of topic 4 (header, modules, threads with frames
and roles, command ring: `CTC4`), in the same compact order, with three differences, plus a copy of
the bridge's own execution table, 74,816 bytes. The differences: the module record is 64 bytes (bias,
start, end, a build id of up to 32 bytes and its length, 0 for an image with no identity), since an
ELF note or a CodeView record is not always 16 bytes; the header's `reserved` word holds the fault
thread's id (low 32 bits), by which the parser leaves that thread out when it lists the table's
others; and `flags` bit 4 says the table copy follows the commands (a bridge writes no commands,
`commandCount` is 0, and its `unplaced` count is 0: the parser works it out from the table copy).
The table is
copied from the handler straight into the slot (never through a stack buffer, which a 64 KiB
signal stack could not hold) with relaxed loads, one for each word of the table. The parser checks the table's magic and
version as the worker's table is checked, and uses it as the worker's table is used: a thread's
role and entry (the cell and generation of the instance it was inside), nothing else. The
fault thread is walked from the signal context or the exception record, frames are raw
addresses, and the walker is the frame-pointer walker of `diagnostics-capture` over a reader that
cannot fault (`process_vm_readv` on the own process on Linux, `mach_vm_read_overwrite` on the own
task); on Windows, where frames are not found by frame pointers, it is the OS unwinder
(`RtlVirtualUnwind`) over the thread's own stack, after checking the stack pointer against the
stack's limits before each step. **Other threads' frames** are collected only where the
platform's in-process code already does it: macOS walks the own task's threads as the worker
does; Linux and Windows record the other registered threads by role and entry from the table copy
with `state: unavailable` and reason `not_collected`: the capsule holds only the fault thread, and
the parser (6c.4) lists the table's other registered threads itself, each entry worked out from the
thread's recorded stack range, leaving out the slot whose id is the header's. Collecting their frames needs a thread
handshake (a signal per thread on Linux, suspend and context on Windows) that this topic does
not take on: the fault thread is what attribution needs.

*Modules.* The handler does no module discovery: `/proc/self/maps`, `dl_iterate_phdr` and the
Windows loader list are not safe or bounded there. Instead the bridge records, while cold, the
identity of the images it loads on purpose: its own executable at start, and each plug-in's
library when the plug-in is created (text range, load bias, build id: the ELF note, Mach-O UUID or
CodeView record, read by the code the supervisor's reader already has, `image-identity`), in a
static list of 256 entries that the handler copies into the capsule's `modules`. A frame in an
image the bridge did not record (a system library, the loader) has no module identity, which the
parser already accepts ("a return address outside every module is kept as such"). macOS adds what
dyld lists, as the worker's capture does. A fault inside a plug-in's code is therefore labelled
with the plug-in's own build, which is all `fault_in_image` needs; a fault in a system library
called by it is `active_token`, as for the worker. *Not proven:* that every platform can read the
build id cold and cheaply at `Create`; the fallback is that the entry has no identity, and a
bridge whose plug-in has none produces today's report.

*Thread roles (decision 4).* Two registered roles join the closed set: `bridge_control`, the
thread that runs the bridge's command loop (the process's main thread), and `bridge_audio`, the
thread that runs a plug-in's `process` (today the `audio-bridge` thread of each instance's slot),
numbered from 1 in creation order in `worker` like the pools. Every other thread in the bridge, the
plug-in's own and the system's, stays `other`. This changes four places together, in step 6c.1: the
execution table's version (the role is stored by number), `threadRoleName` and the roles the
reader's validator accepts and the viewer's `roleName` (and the viewer's text), and the paragraph
above that lists which threads are registered ("the sandbox and bridge children" leave the list of
unregistered ones). Roles are stored by the process that registers them, so the worker and a bridge
share the closed set but never each other's table: the supervisor reads a bridge's roles only from
that bridge's capsule. Registration happens when the thread starts and off every audio path;
the plug-in call entries use the same atomic scopes as the worker's (`host::Instance`, table
`enter` and `leave`: stores only), so the `rtsan` check on the bridge's audio entry stays as it is.

*Where the handlers are.* The bridge's `main` installs, once and before it reads any command
(after the `--scan` branch, which loads a plug-in only to read its description),
the handlers of `diagnostics-capture`'s `selfcapture::install` (no frontend and no supervisor):
the signals SEGV, BUS, ILL, FPE and ABRT on POSIX, each only where its action is still the default
(a sanitizer owns them in its builds, and the bridge then works with no capsule, as the worker
does), with an alternate stack for each registered thread (the one the worker's handler uses,
`registerThreadAlternateStack`); the last-chance filter, the `SIGABRT` handler and the terminate
hook on Windows (the runtime settings of `8336fe69` stay, and the new `SIGABRT` handler ends the
process with the same status after capturing); and the terminate hook on both. The own executable
is recorded when the handlers go in, and each plug-in's image when it is created. A handler writes the capsule into the slot and then **chains to the previous
action so the exit status stays the fatal one**: the host's classification of the end
(`signal`, `exception`, `host`) and its kill rules are unchanged by capture, and a
`bridge_exit` report without a capsule stays exactly as today's. *Not done in 6c.2:* closing the Windows gap in which
`raise(SIGSEGV)` ends a process with an exit code (a `bridge_exit` that reads as an `exit`, so no
report). It needs a `SIGSEGV` handler, and a C runtime handler on Windows also receives the
hardware faults of the main thread that the last-chance filter now sees with their real context;
keeping that context needs the runtime's exception pointers, which have not been tried here. *One capture a process, and a second failing thread waits (round 6c-a).* The first thread to fail
claims the capture (one atomic compare-and-swap, with its thread id); a thread that fails while it
is being made does not end the process under it, which would leave the slot `writing`: it waits,
sleeping a millisecond at a time (`nanosleep`, `Sleep`), until the capture has ended or until
200 ms after its own handler began, and only then lets the process die as it would have (re-raise,
`abort`, or the chained filter). The claiming thread failing again inside its own handler does not
wait for itself. This needs the POSIX handlers to be reachable by a second fault, so they are
installed without `SA_RESETHAND` (the handler restores the default action itself when it re-raises;
a fault on the claiming thread inside the handler meets its own signal blocked, which the kernel
answers by ending the process). The wait is the only one the handler ever makes.

*The bound against the host's deadlines, and a kill that lands in the handler (round 6c-a).* The
handler's 200 ms is a tenth of the host's shortest failure deadline (a `process` call fails after
2 s by default, and a lifecycle call after 5/2 of the deadline plus 2 s, ADR-0013), so a bridge
that is failing is gone, re-raised, long before the host's first deadline on it can run out, and
the host's deadlines never wait for a capsule. When a kill does land inside the handler (the
handler hung, or the machine stalled it past the host's deadline), the host's own kill ends the
process: SIGKILL on POSIX, `TerminateProcess` with exit code 1 on Windows. The classification
(`classifyBridgeEnd`) reads that as the host's end (`how: host`), which has no `bridge_exit`
report, whatever the slot holds (`writing`, or a complete capsule nobody reads): a host kill is
the host's doing and the one kind of end this report is not for. The exception is a bridge whose
status is another fatal one (it died of its own fault first and the host's kill only cut a dump
short): that keeps the fault's status and is reported, as before.

*Known limit, Windows: a fault while the loader's function-table lock is held.* The Windows walk
asks `RtlLookupFunctionEntry` for each frame's unwind data, and that call can take the lock that
guards the process's function tables (taken while an image loads or a dynamic function table is
added or removed). A fault in a thread that holds it, or one that another thread holds while this
one faults, makes the handler wait on it: the handler's 200 ms is checked between frames, not
inside the call, so it cannot interrupt it. The bridge then hangs until the host's deadline kills
it and the end reads as a host end (above): no report, and today's behaviour. It needs a fault at
the instant of a load or a JIT registration, which is rare; no mitigation is built (the options are
unwind data cached for the recorded images while cold, or a frame-pointer walk on Windows too).
Unproven either way: no Windows machine was available to try it.

The
handler's bound is its own: 200 ms from the fault to a published slot, measured on the monotonic
clock, and a slot not complete by then is left `writing`. The host's kill deadlines are never
extended to wait for a capsule, as the ADR requires.

*What the report says.* A bridge's `bridge_exit` report gains `threads` (the fault thread first,
the rest by role and entry as above), `modules`, and `capture: "bridge"` (the origin of the
threads; absent on a report from the host's account alone), and its `complete` is true only when
the capsule's threads are all walked. `attribution` stays `{confidence, plugin}`: when the fault
thread's first frame lies in a module that is the `build` of a bridge member (the build found in the
capsule's modules by the 6b `image` address of each member, matched with `findInBridge` and the
cell and generation of the thread's entry) the confidence is `fault_in_image`, the plug-in the one
the thread was inside or the only one alive, under the same rules as the worker's 5e; the
validator allows `fault_in_image` and `active_token` in a `bridge_exit` report when the report
carries a bridge capsule and holds them to the same evidence rules as the worker's. A report with
no capsule keeps the `bridge_exit` level only. A bridge member's `build` is no longer null for a bridge
that captured: the repeat rule (members and status) uses it instead of the stand-in.

*One rule for suspects (decision 3, decided 2026-11-18).* A bridged plug-in's crash while the
application keeps running does **not** count as a suspect and never causes the safe-load question,
even when its report names the plug-in by `fault_in_image` with an exact build: the application
did not close, and the plug-in already runs apart from it. The report, its validator, the
reader's summary and the viewer carry the attribution all the same, so a user can read it. The
rule is kept as one function in one file, `bridgeReportCounts(summary)` in
`host/diagnostics/src/reports.cpp`, called where the window is read (`suspects`), and it returns
false: a `bridge_exit` report neither names a suspect nor takes a place in the window, as the
section on suspects above says, and nothing else in the reader consults the kind. A later
decision to count bridge reports would change that function and that sentence only. A bridge
report is never a restart notice either (`lastRunNotice` and the role scan skip it).

*Tests the damage cases need (POSIX and Windows, each).* The operation and the `Slot` event
refuse, with a structured error or a counted and closed resource and no effect on anything else:
an extra resource, a missing resource, a duplicate attach, a resource of the wrong type or size (a
descriptor that is not a sealed memory object of the expected size, a handle that is not a section
of it), a wrong version, a trailing payload, and a resource on a packet that may carry none. Each
case also shows that no descriptor or handle is left open afterwards.

*Plan and review stops.* 6c.0 this contract (review stop A). 6c.1 thread roles. 6c.2 the bridge
captures itself (handlers, CTC5, the cold module list). 6c.3 the host carries the slot (the
operation, the `Slot` event, the protocol and wire bumps) (review stop B, with Linux working end
to end). 6c.4 the supervisor reads and reports (parser, labels, attribution, validator, reader,
viewer). 6c.5 tests on the three platforms and the sanitizers (review stop C). Native CI is the
only proof of the macOS in-process walk of a child, the Windows filter in a bridge and the
Windows handle duplication. A missing, late or malformed capsule is always today's report.

### Command breadcrumbs (topic 5d)

The report says which registered commands the model thread had begun. `libs/command-bus`
links the execution table and its dispatcher writes the table's ring of 64 entries
(`supple/diagctx/commands.h`). An entry is three integers: the command's id, the category of
its origin and a state. Nothing else is recorded, ever: no argument, result, error text,
label, path, script text, client or agent string, gesture, duration, or the command log's
copy. The dispatcher writes an entry after the name resolved to a registered command (an
unknown name or version, or text that is not a command, never enters, so a caller cannot put
text of its own into a report) and before the arguments are validated, and marks it `ended`
on every path out, whether the command succeeded or failed. A command run inside another (the
steps of a batch, a script's calls) is an entry of its own, in order, inside the outer's. A
command that nests more than 64 deep or wide overwrites its own entry, which is then left as it
was.

*The name table.* The id is the command's index in registration order. When the process's
dispatcher is built (the registry is complete), the names go to the supervisor once, through
the inventory sink and as `Instance` packets with the `Names` event: four names to a packet,
`cell` the id of the first (a multiple of four), the four text slots holding the names. The
rest of the record is zero. A name is 1 to 64 bytes of `[a-z0-9_.-]`; others are not announced.
Command names are first-party text declared in C++. A name burst of about sixty packets can fill
a small socket buffer (macOS) before the supervisor wakes, so a name packet that finds the
channel full waits for room, with one budget of 250 ms for the whole table, after which the
rest is dropped and counted: those ids read `unknown`. This is the only place an announcement
waits for a send that did not start, and it runs once, at startup, on the thread that built the
dispatcher.

*One recorder, one writer.* The first dispatcher built in a process claims the ring
(`claimCommands`); later ones neither announce nor record, since their ids would mean other
names than the table the supervisor holds. The ring belongs to the thread registered as
`Model`. Until one has registered, the first thread to begin a command stands in; the Model
thread takes the ring from it when it begins its first command (so a dispatch off the model
thread at startup cannot lock it out for the life of the process), and from then on a command
begun on another thread is not recorded and is counted in the table's `commandsDropped`. A
stand-in that was mid-write at the hand-over can leave one damaged entry, which the reader
counts. Writing an entry is four release stores on the model thread (empty the
slot, fill it, label it with its sequence, count it); no lock, call, allocation or message.
A thread stopped between two of them leaves a slot the reader skips, or one past the count that
it accepts whole. A parsed capsule counts as a table that was read: the worker took its
counters and commands from its own table.

*Reading.* The supervisor reads the ring with the rest of the table (Linux and Windows: the
bounded remote read; macOS: the worker copies the entries into the capsule, whose version is
now 4, after the threads, and `parseCapsule` stays strict). `readCommands` keeps an entry only
if its slot is where its sequence belongs, it lies within the last 64 of the ring's count (or
one past it), and its enums are in range; anything else that was used is counted as damaged and
left out. The entries come out oldest first. On macOS the ring is copied after the other
threads have been read, so the newest entry can be missing or still `begun`.

*Report.* `commands` is an array of at most 64 objects `{seq, name, origin, state}`, in
ascending `seq`: `name` is the command's name, or `unknown` when this supervisor has no name
for the id; `origin` one of `ui`, `script`, `mcp`, `rpc`, `test`, `internal`, `agent` (the
dispatcher's origin types, `lua` being `script`); `state` `begun` (the command had not
finished: the crash was in or under it) or `ended`. `unavailable` always says that these are the
last 64 the model thread began and that a command on another thread is not listed, and adds
when entries were damaged, when commands were refused for coming from another thread, when a
listed command has no name, and, with no readable table, that no commands are listed. The strict
validator checks the shape (four keys, a valid name or `unknown`, closed sets, rising `seq`,
at most 64). A listed command is evidence about what the model was doing, not about the
cause: a crash in a plug-in's audio thread has the model's last commands too.

### Attribution and dropped counts (topic 5e)

`attribution` is `{"confidence", "plugin"}`: what the report's own evidence says about which
plug-in the crash belongs to, and how strongly. The rules below are the one place the policy
lives (`Inventory::attributionJson`); they are tried in order and the first that holds decides.
`plugin` is the `id` of a listed plug-in, or `null`.

1. `fault_in_image`: the faulting instruction (the first frame of the fault thread) is in a
   module that is the `build` of a listed plug-in: plug-in code was running, whatever the table
   says. `plugin` is the one the fault thread was inside by token if that is among them;
   otherwise the only one still alive, or the only instance if none is, or the only entry; with
   several instances of one binary and nothing to choose between them it is `null` (the image
   is the evidence, no instance is named). An instance is preferred to its library entry.
2. `active_token`: the fault thread was inside an entry of a listed plug-in (the host marked
   it), and the fault is not in a plug-in's image: the fault is likely in host code the plug-in
   called, or in a binary this report could not identify. `plugin` is that plug-in.
3. `concurrent_activity`: only other threads were inside entries of listed plug-ins. This is
   activity, not blame: `plugin` is the one when exactly one plug-in was active, `null` when
   several were.
4. `recent_command`: no plug-in is implicated and `commands` is not empty. `plugin` is `null`.
5. `unknown`: none of the above, or no terminal capsule to conclude from. `plugin` is `null`.

A fault in the program's own code with no plug-in involved is therefore `recent_command` or
`unknown`; the report does not say "not a plug-in", since a plug-in's own thread, one the host
has no cell for, can still be the cause. The validator holds a report to its own evidence:
the confidence is one of the five, `plugin` is `null` or a listed id, `active_token` needs a
plug-in with an `executing` entry matched to the fault thread, `concurrent_activity` one matched
to another thread, `recent_command` a non-empty `commands` and no plug-in, and `unknown` none.
`fault_in_image` needs the first frame of the fault thread (`threads[0]`, role `fault`) in a
module of `modules` whose id is the `build.id` of the plug-in named, or of a listed plug-in
when `plugin` is `null`: a plug-in without a `build` is never blamed for being where the fault
was.

`dropped` is an object of non-negative counts of what was left out: `threads` (omitted from the
report beyond its limit), `unregistered_threads` (threads the execution table had no slot for,
which appear with the role `other`), `plugins` (destroyed instances beyond the last 64),
`commands` (ring entries that were damaged, and commands begun on a thread that did not own the
ring), `cells` (entries open in the table that no thread read could be tied to, and scopes whose
stack address the table cannot hold), `messages` (inventory and name packets the worker could not
send, or the supervisor refused), `untracked` (instances that found no cell) and, from topic 6a,
`bridges` (bridge processes beyond the last 256) and `bridge_reports` (bridge deaths not reported
separately, above). The validator takes the plain integer older reports hold, an object without
`bridges` or `bridge_reports`, and a report without `attribution`, as reports it still owns; any other shape is kept and never authorizes deletion.

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

What an image is (the `Source` reader, `Limits`, `Identity` and `identify`, and the ELF, Mach-O
and PE header parsers that compute an id) is the GPL library `image-identity`
(`libs/image-identity/`, header `supple/imageid/image.h`, still namespace
`supple::diagnostics::resolver`), moved out of the resolver so that the application
can compare an installed plug-in file with a report's `build.id` (topic 7d) without linking the
resolver, its symbol readers or its search. `crash-resolver` links it and keeps the symbol
tables, `lookup`, `demangle` and the report resolution. Like `diagnostics-capture` it is
DAW-only: `tools/license-check` and `cmake/LicenseIsolation.cmake` reject its use from
`plugins/`, `modules/` and the MIT libraries. The code is moved as it was, but the ELF, Mach-O and PE
helpers that were local to one file are now `resolver::detail` functions with external linkage,
declared in `supple/imageid/parse.h` so that the resolver can reuse them. That header is internal
to the library and the resolver, not an interface: nothing else includes it. The library's tests are
its own executable, `image-identity.tests`; the synthetic ELF, Mach-O and PE image builders they use
(`libs/image-identity/tests/images.h`) are shared with `crash-resolver.tests` and `engine.tests`.

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
existing watcher, with a separate pending/ready/unavailable incident association
(not built in topic 6a: see "A report when the worker lives").
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
frame pushed onto an unmapped enabled stack with a forced SIGILL and no handler. The
`diagnostics.altstack-restore` test (Linux, unsanitized) preloads a library that makes the
restore fail and checks that the teardown abort is still captured with the stack mapped.
No callback hook, TLS registration or sanitizer suppression is added. Sanitized
builds retain their handlers and emit exit-only summaries.

Existing non-default POSIX fatal handlers are preserved (ours is not installed over
them). On Windows the last-chance filter is always installed, replacing whatever top-level
filter was there, and chains to it after the handoff, returning what it returns, except
that `EXCEPTION_CONTINUE_EXECUTION` becomes `EXCEPTION_CONTINUE_SEARCH`: the terminal packet
has gone out, so the fault stays fatal and is never resumed. An exception code `0xE06D7363`
(a C++ exception nobody caught) is not claimed at first: the filter hands it to the prior one
untouched, so the runtime turns it into `std::terminate`, which is recorded as terminate
(cause 3) and chains the program's handler, as on Linux and macOS. If the prior filter
returns instead (the code is raised by something that is not a C++ throw, or the prior
filter is not the runtime's, or there is none), nobody is handling it and it is captured
as a fault (cause 2) like any other exception, with the prior filter's answer used once,
not asked for again. The MSVC runtime keeps the terminate handler per thread, so on
Windows every thread that registers with the diagnostics (the execution table's thread
hook) gets the hook too, after the worker's main thread has installed it, and says
terminate (cause 3); a thread that never registers keeps what the runtime gives it, which
the `thread-throw` and `raw-thread-throw` fixtures record on native CI (cause 3 if the
handler is process-wide, cause 4, abort, if it is per thread). An EXE
linked with the MSVC runtime always has such a filter (the runtime's own, which turns an
uncaught C++ exception into `std::terminate`), so declining to replace an existing one
would never have installed in the product; chaining keeps both that filter and any other
that would have run, and with them the user's own OS crash reporting. `installed` (the
worker's terminal coverage) is true on Windows once the filter is set (setting it cannot
fail; it is not read back, because reading means clearing it for a moment). After capture POSIX restores/re-raises the fatal default.
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
