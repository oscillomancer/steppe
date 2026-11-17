# ADR-0084: Audio threads take no page faults the app can prevent

**Status:** Accepted, 2026-11-15. Contracts: `AGENTS.md` rules 2 and 4,
`docs/specs/engine.md` (Audio-thread page faults). Number chosen above
ADR-0082 and ADR-0083, which other branches hold.

**Review correction, 2026-11-16 (round 21):** §1's opt-out reads the
process's mode first. A mode it already has (1, or 3) is kept and not undone
at stop; only a mode the engine set from 0 is released, and only while it is
still that mode, with a refused release reported. Bridges set it only from 0
(`docs/specs/engine.md`, Audio-thread page faults).

## Context

The item D measurements found callbacks of 100 ms and more, in process, with
the live helpers spinning or parked, and each such run had taken minor page
faults. A sampler counting each fault on its thread (a user-only
`perf_event_open` software event, as `perf_event_paranoid` 2 allows), with the
kernel's counters sampled every 5 ms, showed where they came from. The bench
was Yarrow on 8 tracks, live, at 128 frames, run for 30 s on a 10-CPU machine
under other builds' load, with `transparent_hugepage=always`.

- The faults land on long-lived heap memory, in `Engine::cycle`,
  `LiveWorkers::run` and the plug-in, and RSS does not grow. So nothing is
  touched for the first time, and no page is swapped out.
- Every in-interval fault on an audio thread falls within 5 ms of a
  khugepaged collapse (`thp_collapse_alloc`, every 10.24 s). In one run six
  helpers faulted in the same two 2 MiB heap ranges at the moment collapses
  rose by six.
- `main` (a5638ecf) behaves the same, with a 493 ms callback in one run. So
  does a full `mlockall`, since the kernel collapses locked pages too.
  `PR_SET_THP_DISABLE` for the process removes the faults.

khugepaged collapses a range under the process's address-space lock. An audio
thread that touches the range faults and waits for it. khugepaged is a nice-19
kernel thread; on a loaded machine it is starved of CPU while holding the
lock, and the lock has no priority inheritance. The heap is a candidate
because glibc grows it a little at a time, so it starts on small pages.

A second path, found by reading the code, would fault on the audio thread in any
session: the script runtime's `Buffer` used `calloc`, which for large sizes
returns fresh pages it does not write. A 16 MiB buffer had 1 of 4097 pages in
memory after `resize()`, so the callback's first writes, or a restart's
`memset`, faulted them in.

## Decision

1. **Linux: while the engine runs, the process stays out of transparent huge
   pages.** It calls `prctl(PR_SET_THP_DISABLE, 1, PR_THP_DISABLE_EXCEPT_ADVISED)`,
   or on kernels before 6.18, `prctl(PR_SET_THP_DISABLE, 1)`. It is
   applied with the other host-process controls and released at stop. Each
   bridge process applies it at its own start. It is a per-process setting,
   inherited by children and kept across exec, and it changes no system
   configuration (rule 4). There is no preference: no measurement showed a
   cost (in-process callback p99 119.2 against 118.2 µs over 14 interleaved
   pairs), and the failure it prevents is a dropout.
2. **Memory the audio path uses is written when it is prepared**, off the
   audio thread. Engine buffers are value-initialised containers. The script
   runtime's `Buffer` writes one byte per 4 KiB after `calloc`, through a
   volatile pointer that the compiler must keep.
3. **Page faults on the audio threads are visible.** `engine.status` reports
   `audio_page_faults`. On Linux it counts the callback's and the live
   helpers' faults, from `/proc/self/task/<tid>/stat`. The cycle records
   `pthread_self()` without a system call, and the main thread derives the
   tid from the thread's CPU clock id. The bench reports the same count, and
   each thread's.
4. **Memory locking stays an option.** It guards against reclaim, and against
   compaction where `compact_unevictable_allowed` is 0, but not against
   collapse, and it prefaults nothing (`MCL_ONFAULT`).

## Other platforms

- **macOS** has no background collapse of a running process's pages. Its
  memory compressor can compress pages left idle, and the first touch of
  fresh memory faults there too; point 2 covers the latter. It keeps no
  per-thread fault count, so `audio_page_faults` is the process's
  (`getrusage`; review correction 2026-11-16: the task's
  `TASK_EVENTS_INFO` counts instead, since `getrusage` there counts only page
  reclaims and native CI saw touched fresh pages not counted). There is no
  process-wide `mlockall`.
- **Windows** has no transparent huge pages: large pages are explicit,
  privileged and never pageable. Working-set trimming can make idle pages
  soft-fault back. `GetProcessMemoryInfo` counts the process's faults without
  telling minor from major, so `major` is omitted.

## Consequences

Without huge pages, the host's heap costs more TLB misses; none was measurable
in the bench. A plug-in that asks for huge pages with `MADV_HUGEPAGE` keeps
them on kernels from 6.18 and loses them on older ones. Faults that remain
possible are reclaim under memory pressure (memory locking addresses it), and
a plug-in's own first touches on the audio thread, which the host cannot
prevent but now counts.
