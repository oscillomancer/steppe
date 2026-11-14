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
