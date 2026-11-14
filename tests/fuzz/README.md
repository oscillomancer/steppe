# Project and history fuzzing

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

`project.fuzz` feeds bytes to the parser used by `project.open`. Accepted
projects must satisfy the model invariants and survive a canonical text
round trip. The corpus contains files saved by the engine (empty, MIDI song,
FX, automation), plus a saved project with a route and a missing audio media
item. Missing media is valid project state.

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
64 random choices per seed in each executor (a few seconds). A shuffled schedule
covers track add/remove/move, FX add/remove/bypass/patch and latency changes,
audio and rack sends, items add/move/split/remove, tempo ramps, envelope points,
connected rack module add/remove/knobs/bypass, loop/seek, undo/redo and branch
switches. The seed also selects Console 9 and its deterministic dither seed.

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
checks the final renders and undo, then destroys its app/history so long runs
have bounded fixture memory. Native scheduling is intentionally unconstrained;
the seed reproduces commands and their arguments, not OS thread interleaving.
Finite output is checked for **every** callback sample, including samples past
any capture buffer; peak magnitude must not exceed 8 with this attenuating,
acyclic fixture graph. After adoption, every step must have one retained plan,
no pending plan handoffs, no retired FX/modules, and bounded pool/plan storage.
After stop the retained plan and handoff counts must be zero. LeakSanitizer
covers unreachable allocations; process RSS is not asserted because allocator
caches and sanitizer quarantine are not retired engine objects.

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
runs the `phase: edits` command/args rows in order, then performs the same final
checks once. Setup is fixed; `phase: finish` rows document final-check failures
and are not replayed as random edits. Remove irrelevant edit rows to shrink a
reproducer, keeping its ID dependencies valid. Engine findings belong in the
hand-back report with the seed and smallest reproducing journal; this harness
does not repair engine failures.

`SUPPLE_SOAK_CONTINUE=1` continues with the next seed/executor after a doctest
assertion so a timed diagnostic run can cover its entire requested duration.
Every failure still counts and the process exits nonzero; sanitizer aborts and
crashes remain fatal. Normal tests and replay stop at the first failed check.

`plan-soak-corpus/rack-level.jsonl` and `rack-level-ahead.jsonl` are known
failing one-edit reproducers:
changing the connected VCA's level from -30 to -27 makes the retained RT pool
differ from the saved project's offline render, in both executors. An empty
replay passes. The default seeds do not hit this final state; the journal is
opt-in diagnostic coverage, pending reset-contract triage by the lead. This
retained-RT null is an additional strict probe; the requested offline versus
reopened-file comparison passes in these cases.

The long run also found a valid reorder rejected as `tr.cycle` (first seed
17855668924378716666, both executors). The first move below passes on its own;
the second should reorder the buss's child but is rejected. Save this JSONL
and use `SUPPLE_SOAK_REPLAY` (set `anticipative: true` to exercise that executor):

```jsonl
{"schema":"supple.plan-soak/1","seed":"17855668924378716666","anticipative":false}
{"phase":"edits","command":"track.move","args":{"track":"tr_2","parent":"tr_3","index":0}}
{"phase":"edits","command":"track.move","args":{"track":"tr_2","parent":"tr_3","index":0}}
```

The harness's `tr_3` is a buss and `tr_2` starts under the master. Engine
processing and command behavior are unchanged; both findings are reported to
the lead rather than repaired here.
