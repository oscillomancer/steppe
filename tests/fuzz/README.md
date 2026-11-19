# Project and history fuzzing

## Registered command input boundaries

`engine-command-inputs.tests` and the `registered sandbox argument mutations`
case in `sandbox.tests` use fixed seed `201423361`. They derive mutations from
every registered argument schema, including optional fields, nested objects,
array elements, unions, references, required members and numeric bounds. Failed
cases print the seed, command/version, case number, target pointer and complete
JSON arguments. No timing threshold decides correctness.

Every decoder-rejected input also goes through the real dispatcher: errors must
be structured and project bytes/version (or sandbox session state) must stay
unchanged. The engine repeats the corpus stopped and while recording with each
executor, interleaving manual dummy blocks and project saves. These are portable
ordinary CI tests, including sanitizer presets; no real device is opened.

An independent conservative oracle rejects obvious type/range/required-member
contract violations even if a decoder starts accepting them. It does not enforce
unknown schema keywords or unit-string patterns. Existing optional decoders
accept null as absent without advertising null in their exported inner schema;
null still goes through each decoder but is exempt from this partial oracle.
Schema-accepted mutations are counted and never blindly executed: arbitrary
plug-in JSON and device/file/script actions need domain fixtures. This is a
schema boundary regression test, not exhaustive semantic command fuzzing.

```sh
cmake --build --preset dev --target engine-command-inputs.tests sandbox.tests -j3
ctest --preset dev -R '^engine-command-inputs.tests$' --output-on-failure
build/dev/host/sandbox/sandbox.tests '-tc=registered sandbox argument mutations*'
```

Configure the fuzz targets with Clang and run them against their seed corpora.
The fuzz tree instruments every library for coverage (`fuzzer-no-link` in the
global compile flags), not only the harness executables: `project.fuzz` and
`history.fuzz` link the ordinary engine/model libraries, and without it
libFuzzer sees no edges inside them (configure warns when the flag is
missing). `undefined` next to `address` makes signed overflow and other UB a
finding:

```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link \
  -DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link \
  -DSUPPLE_BUILD_FUZZERS=ON -DSUPPLE_SANITIZE=address,undefined \
  -DSUPPLE_BUILD_GUI=OFF -DSUPPLE_BUILD_PLUGINS=OFF
cmake --build build/fuzz --target project.fuzz history.fuzz -j6
ASAN_OPTIONS=detect_leaks=0 build/fuzz/tests/fuzz/project.fuzz \
  -max_total_time=120 build/fuzz/tests/fuzz/project-corpus
ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0 \
  build/fuzz/tests/fuzz/history.fuzz -max_total_time=120 \
  build/fuzz/tests/fuzz/history-corpus
```

`project.fuzz` feeds bytes to the parser used by `project.open`, with the
engine's rule for which saved steps may replay (ADR-0076). Accepted
projects must satisfy the model invariants and survive a canonical text
round trip (with history kept). Then it walks what undo, redo and merges
walk in what was restored: every branch is undone to its base and redone to
its end, each head checked, and each branch is merged into main with a
replayer that changes nothing (the history walks are the real ones; a walk
that does not end shows up as a timeout). The corpus contains files saved by
the engine (empty, MIDI song, FX, automation), plus a saved project with a
route and a missing audio media item, one with an unmerged agent branch
(`song_branches.json`, its `branches` section) and the same branch undone
once and saved with `history: true` (`song_branches_redo.json`, a redo
step). Missing media is valid project state; a branches section that does
not restore opens main alone and must then round-trip as main.

`history.fuzz` maps bytes to real bus commands on a fresh `EngineApp`,
including batches, failed calls, undo/redo and branch operations. After each
call it checks every branch head's invariants, and checks that failed calls
did not move the head. At the end it undoes to the branch base, redoes to the
final canonical text, then saves and reopens. The seeded property test in
`engine.tests` uses the same sequence runner. Route edits are absent from the
current engine command catalogue, so the project corpus covers routes.

Fixture commands in the harnesses use checked arithmetic (`toy.add` refuses
a total that would overflow with `toy.overflow`), so any sanitizer report is
a finding in the code under test, never in the fixture.
`bus-corpus/overflow.json` seeds that case.

Clang ASan reports an allocation/deallocation mismatch in libstdc++'s
`std::stable_sort` temporary buffer (`get_temporary_buffer` uses `operator
new`/`free` in this toolchain). The history fuzz command disables only that
check; the remaining ASan checks stay active.

## Playback plan-swap soak

`engine-plan-soak.tests` is an ordinary doctest/CTest target, labelled `engine;rt`
on Linux, macOS and Windows. It uses the real command bus and engine, fixture
CLAPs and stock rack modules, with a paced dummy audio thread and two helpers.
It opens no audio device or window. Its normal budget is two fixed seeds,
64 random choices per seed in each executor (a small fixed budget). A shuffled schedule
covers track add/remove/move, FX add/remove/bypass/patch and latency changes,
audio and rack sends, items add/move/split/remove, tempo ramps, envelope points,
connected rack module add/remove/knobs/bypass, loop/seek, undo/redo and branch
switches. The seed also selects Console 9 and its deterministic dither seed.
The fixed setup's rack plays its VCA through a plug-in module (the gain
fixture, latent, at half wet); the FX bypass and patch edits reach that
plug-in too, but it is never removed. The engine runs with
`EngineOptions::verifyPlans`: every compile is checked against a second
compile of the same session, and a difference fails the edit with
`engine.plan_differs` and the first differing line of the plan dump.

```sh
cmake --preset dev
cmake --build build/dev --target engine-plan-soak.tests -j5
TMPDIR=/tmp ctest --test-dir build/dev -R '^engine-plan-soak[.]tests$' --output-on-failure
SUPPLE_SOAK_SECONDS=660 build/dev/host/engine/engine-plan-soak.tests
```

Use the same target with `asan`, `tsan` or `rtsan`, after configuring that
preset. For example, a three-minute ASan/UBSan/LeakSanitizer run:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  SUPPLE_SOAK_SECONDS=180 build/asan/host/engine/engine-plan-soak.tests
RTSAN_OPTIONS=halt_on_error=1 SUPPLE_SOAK_SECONDS=180 \
  build/rtsan/host/engine/engine-plan-soak.tests
```

`SUPPLE_SOAK_SECONDS` (unsigned integer, 0..86400; default 0) extends the run
by whole epochs, cycling through seeds. Every epoch covers both executors,
checks intermediate and final renders and undo, then destroys its app/history so long runs
have bounded fixture memory. Native scheduling is intentionally unconstrained;
the seed reproduces commands and their arguments, not OS thread interleaving.
Finite output is checked for **every** callback sample, including samples past
any capture buffer; peak magnitude must not exceed 8 with this attenuating,
acyclic fixture graph. After adoption, every step must have one retained plan,
no pending plan handoffs, no retired FX/modules, and bounded pool/plan storage.
After stop the retained plan and handoff counts must be zero. LeakSanitizer
covers unreachable allocations; process RSS is not asserted because allocator
caches and sanitizer quarantine are not retired engine objects.

An initial audio checkpoint and one after every settled edit compare 1024
frames from the running engine with a private offline render of that project.
Anchors cycle through beats 0, 1, 2 and 3 in its current tempo map. The dummy
driver parks between callbacks, outside the RT entry; the model thread locates
to the anchor and disables looping, then drives a fixed number of paced
callbacks on the **same audio thread**, waiting for renderer readiness before
each one. The engine and pooled DSP instances stay running. Comparison accounts
for output latency; afterwards the previous loop and position are restored.
Before locating, four complete 128-frame callbacks after parking drain the
specified 10 ms swap/wet fade. An adoption acknowledgement can arrive at the
start of an in-flight block; cycles counted before adoption do not establish
that its fade has finished.
Capture storage is prepared while parked, never allocated in the RT entry.
These windows detect wrong audio in intermediate project states that a later
edit repairs. Their locate resets DSP history: they do not assert equality for
arbitrary uninterrupted playback history or the pre-locate swap crossfade.

**Swaps that change nothing (route E stage 5).** After the final checks each
scenario plays its settled project states again, in its executor, on a manual
dummy device with a fixed loop (0 to 0.25 s): a new state every 24 callbacks.
It plays them twice: once as they are, and once with two plan swaps that
change nothing after each edit, a marker added two callbacks after the edit
(inside its 10 ms ramps) and removed at callback 13. Both runs must match
sample for sample. At each load and each service point the anticipative
renderer has first adopted the latest plan and rendered all it may (decided
by its state), and the harness services until nothing more is published, so
both runs take every plan, and every step of a plug-in restart, at the same
position. (A restart's second step waits for the plug-in to stop, which the
renderer does on its own thread when it adopts the plan without it; servicing
before that adoption let the restart land two callbacks apart in the two
runs, seen once under ASan with other tests running.)
A failure prints `plan-soak NO-CHANGE SWAP AUDIBLE`, the first differing
frame, the project state's index, the callback within its step and the
journal row of the edit that made that state. Removing the carry of a rack's
plug-in stages (`af08ccfd`) fails this pass in both executors at the first
swap; none of the soak's other checks catches it.

Every correctness decision uses readiness, callback/sample counts or values.
No readiness assertion uses a timed `awaitPlan`/`awaitRendered` result. A
60-second handshake watchdog instead prints `plan-soak WATCHDOG ABORT`, the
stage, seed/executor and replay path, then exits **124**, without an audio
mismatch verdict or continuing to the next seed. Numeric bit mismatches print
`plan-soak AUDIO MISMATCH`, the channel and first differing window frame.
Wall time only paces the driver, aborts hung handshakes, names scratch folders
and controls the optional duration budget; normal coverage has fixed seeds and
operation counts. A watchdog abort is incomplete coverage, not a passing run.

The final project is saved and rendered offline, then reopened in a fresh app
and rendered again, through the last item's end plus 100 ms: every float's bits
must match. A manual dummy backend then
reuses the original app's RT pool and nulls against that reference after the
plan's latency. Both branches undo to their original serialized content,
excluding **only** `project.next_ids`; those allocation high-water marks must
never decrease within either branch (ADR-0008).

Every epoch prints its decimal seed, executor and JSONL journal path. Commands
are flushed **before** dispatch, so crashes and sanitizer aborts retain the
last attempted operation. Failures print the executed prefix and preserve the
fixture directory; successful fixtures are removed. To reproduce or minimise:

```sh
SUPPLE_SOAK_SEED=20261114 SUPPLE_SOAK_EXECUTOR=ahead SUPPLE_SOAK_STEPS=23 \
  build/dev/host/engine/engine-plan-soak.tests
SUPPLE_SOAK_REPLAY=/tmp/supple-plan-soak-EXAMPLE/replay.jsonl \
  build/dev/host/engine/engine-plan-soak.tests
```

`SUPPLE_SOAK_SEED` chooses the first epoch's unsigned 64-bit decimal seed;
without a duration it runs only that seed. `SUPPLE_SOAK_STEPS` is 1..4096
(default 64); shrinking it preserves the random prefix. `SUPPLE_SOAK_EXECUTOR`
is `callback`, `ahead`, or `both` (default). `SUPPLE_SOAK_KEEP_TRACE=1` preserves
successful journals too. Replay takes its seed and executor from the header,
runs `phase: edits` command/args rows in order, then performs the same final
checks once. New journals interleave `phase: checkpoint` rows (`index`,
`start_frame`, `frames: 1024`). If any are present, replay runs exactly those
checkpoints; otherwise legacy command-only journals get the automatic initial
and per-edit windows. Setup is fixed; `phase: finish` rows document final-check
failures and are not replayed as random edits. Remove irrelevant edit/checkpoint
rows to shrink a reproducer, keeping ID dependencies and the failing window's
sample coordinates intact. Engine findings belong in the
hand-back report with the seed and smallest reproducing journal; this harness
does not repair engine failures.

`SUPPLE_SOAK_CONTINUE=1` continues with the next seed/executor after a doctest
assertion so a timed diagnostic run can cover its entire requested duration.
Every failure still counts and the process exits nonzero; sanitizer aborts and
crashes remain fatal. Normal tests and replay stop at the first failed check.

Each file in `plan-soak-corpus/` is its own ctest entry,
`engine-plan-soak.replay.<file name>`, run by every preset and native CI with
`SUPPLE_SOAK_REPLAY` set to it (`host/engine/CMakeLists.txt` finds the files,
so a new one is a test once CMake reconfigures). Each takes about a second in
`dev`, `gcc` and `rtsan` and up to 6 s under ASan and TSan. The files are
checked out with LF endings on every platform (`.gitattributes`): the replay
reads them line by line.

`plan-soak-corpus/retirement-checkpoint.jsonl` minimises a harness fault found
under ASan and TSan. Its optional header `probe: retirement-checkpoint` selects
a deterministic schedule probe: publish one replacement plan, acknowledge it
in the renderer, then release the preceding plan in a manual callback at the
checkpoint's readiness check. The seed records the original failing run; no
musical commands are needed to reproduce the ordering. Collection before that
readiness check legitimately leaves two plans, so checking for one immediately
afterwards was a harness error. The checkpoint now observes both acknowledgements
before collection and retries if service publishes another generation. The
strict one-plan assertion is unchanged. This schedule regression also runs in
the default test budget.

`plan-soak-corpus/bypass-checkpoint.jsonl` minimises a checkpoint false positive
first seen at seed 202611150122: an intentional bypass fade was still active
when the RT window was compared with a fully bypassed offline plan. Its schedule
probe leaves exactly 96 of the 480 fade samples after three callbacks; without
the post-adoption drain, comparison fails at frame 1. The corrected checkpoint
drains the fade by sample count and passes. This regression also runs by default.

`plan-soak-corpus/transient-window.jsonl` keeps a window between two reversible
track-name edits. It passes normally. During discrimination testing, a temporary
finite 0.5 gain error in paced output, active only during the first edit, passed
the old final-only harness and failed this intermediate comparison in both
executors. The injection was removed; the corpus retains the replay coordinates.

`plan-soak-corpus/rack-level.jsonl` and `rack-level-ahead.jsonl` are one-edit
regressions for the fixed rack locate-reset mismatch: changing the connected
VCA's level from -30 to -27 now preserves the retained-RT/offline null in both
executors. The engine restarts rack modules at a locate, just as it resets
plug-ins. The final retained-RT null remains an additional strict probe beyond
the offline-versus-reopened-file comparison.

The long run also found a valid reorder rejected as `tr.cycle` (first seed
17855668924378716666, both executors). This is now fixed: both moves below
succeed, including the reorder under the current buss. Save this JSONL
and use `SUPPLE_SOAK_REPLAY` (set `anticipative: true` to exercise that executor):

```jsonl
{"schema":"supple.plan-soak/1","seed":"17855668924378716666","anticipative":false}
{"phase":"edits","command":"track.move","args":{"track":"tr_2","parent":"tr_3","index":0}}
{"phase":"edits","command":"track.move","args":{"track":"tr_2","parent":"tr_3","index":0}}
```

The harness's `tr_3` is a buss and `tr_2` starts under the master. The portable
command regression also covers sibling-folder moves, self/descendant cycle
refusal and exact undo restoration.
