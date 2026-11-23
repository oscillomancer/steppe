# ADR-0026: Background audio thread scheduling

**Status:** Accepted, 2026-10-08. Amends [ADR-0012](0012-anticipative-processing.md)
(the renderer and pool ran at normal priority), [ADR-0023](0023-background-worker-participation.md)
("no priority changes") and [ADR-0013](0013-plugin-isolation.md) (the bridge
audio thread's scheduling; protocol version 10).

**Platform amendment, 2026-10-14:** [ADR-0035](0035-headless-platform-layer.md)
adds macOS/Windows mechanisms while retaining these Linux semantics.

**Native device amendment, 2026-10-14:** [ADR-0038](0038-native-device-audio.md)
adds PortAudio on macOS/Windows and device-derived, self-applied helper policies;
Linux mechanisms and callback publication/lifetime rules remain unchanged.

**macOS note, 2026-11-23:** rule 3's "on the main thread" does not hold on
macOS without a device policy: macOS takes a thread's QoS only from the
thread itself, so the pool's helpers and the renderer each set it on
themselves as they start, through one all-or-none startup barrier, and every
one of them goes back to normal if any refuses (`specs/engine.md`, "How it is
applied"). The rule's outcome is unchanged.

## Context

The renderer renders up to 200 ms ahead of the device callback. It works
with the background pool's helpers, and calls bridged plug-ins synchronously.
All of these ran at normal scheduling, nice 0. Meanwhile the JACK callback
and the live helpers (ADR-0024) run SCHED_FIFO at the server's priority
(83 under PipeWire). On a busy desktop (a build, a browser) a normal-priority
renderer gets only a fair share of each CPU. If a job's helpers are
descheduled, the job overruns, the ring drains, and an anticipation miss
becomes an audible dropout. A bridged plug-in's audio thread at normal
priority causes priority inversion behind any renderer that waits for it.

Three facts about the target workstation shaped the choice:

- **Raising nice does not help.** With the cgroup CPU controller (systemd
  desktops), nice only counts against threads in the same cgroup, and
  terminals and services get their own. A build in another terminal is
  unaffected by Quire's nice.
- **The ceiling is 50.** The kernel runs its threaded interrupt handlers
  (disk, GPU, network) and its display commit workers at SCHED_FIFO 50.
  Render bursts of several milliseconds above them would delay I/O and
  compositor frames.
- **Other applications' audio comes next.** rtkit grants their audio threads
  low real-time priorities (at most 20). A browser's audio playback must not
  glitch because Quire renders ahead.

## Decision

1. **A fixed low real-time level.**
   - The renderer and the background pool's helpers run SCHED_FIFO at
     `Engine::kBackgroundPriority` = **10**.
   - It is lowered to one below the callback's priority when that is lower,
     and to `RLIMIT_RTPRIO`.
   - Below 1 they stay normal.
   - Any FIFO level beats normal-priority desktop load equally, so the lowest
     useful level is the friendliest. It sits below the callback, the live
     helpers and the kernel's interrupt threads, and no higher than the audio
     threads rtkit grants other applications. The 200 ms render-ahead never
     needs to outrank those.
   - Equal-priority round robin brings nothing: the kernel's 100 ms timeslice
     is ten times a render job, and the CPU cap below keeps the pool smaller
     than the CPU count.
2. **Only under a real-time callback.**
   - Elevation follows the callback: a normal callback (the dummy backend, a
     JACK server without RT) leaves everything normal.
   - The engine never elevates beyond the callback's class.
   - `EngineOptions::background` forces the choice for benchmarks and tests.
3. **Process-local, checked, all or nothing.** On the main thread, after the
   backend starts, the engine sets the policy with
   `pthread_setschedparam` and verifies it. It refuses to elevate, leaves
   every background thread normal, and reports an actionable warning when:
   - the priority would fall below 1;
   - `RLIMIT_RTTIME` is finite (rtkit-managed processes: a renderer catching
     up can run longer than that without blocking and would be killed);
   - the renderer plus its helpers would leave fewer than two of the allowed
     CPUs to the rest of the system (a sustained overload must not take every
     CPU);
   - or any thread fails to take the policy.

   It does not use JACK's thread API here: on systems without an RT limit it
   goes through rtkit, which requires a finite `RLIMIT_RTTIME`. Threads end at
   `stop()`, so nothing is restored. CPU affinity is never changed.
4. **No inversion through the bridge.**
   - A bridged instance's audio thread takes the engine's background policy
     when processing starts.
   - Protocol version 10 adds `audioPolicy` and `audioPriority` to the
     shared-memory header. The host writes them before `StartProcessing`,
     from a process-wide value the engine sets while it runs (normal
     otherwise). The bridge applies them on its audio thread.
   - A bridge thread runs only while its caller waits for it, so it adds no
     concurrency beyond the pool.
5. **Reported and named.**
   - `engine.start` and `engine.status` report each class's applied policy
     (`scheduling`: callback, live, renderer, background).
   - Every audio thread the host creates carries a role name (`audio-render`,
     `audio-bg-N`, `audio-live-N`, `audio-bridge`, …).

## Consequences

- Renderer jobs no longer compete with normal-priority load for CPU time, as
  long as the session fits the pool.
- In steady state the pool is busy for one job per render block, a few
  milliseconds, then parks. Normal tasks on those CPUs wait up to one job
  while the scheduler moves them elsewhere.
- In sustained overload the pool takes its CPUs almost continuously, but at
  least two allowed CPUs stay free of it. Kernels with RT throttling enabled
  also cap it.
- Systems without real-time permission keep the previous behaviour, with a
  warning when the callback itself is real-time.
- An offline render that runs while the engine plays shares bridge
  processes' policy; its bridge threads still run only while it waits.
- A bridge audio thread that cannot take the policy stays at normal priority
  silently (known limit): there is no reporting path back to the host. The
  bridge inherits the host's limits, so it needs an outside change to the
  bridge process to happen; a header read-back could report it later.

## Alternatives considered

- Nice levels: ineffective across cgroups.
- FIFO at the callback's priority minus 20 (63 under PipeWire): above the
  kernel's interrupt threads.
- JACK's `jack_acquire_real_time_scheduling`: on rtkit systems it implies a
  finite `RLIMIT_RTTIME`.
- Priority-inheritance futexes for the bridge: a protocol rewrite.
- A spawn-time bridge argument: bridges outlive engine restarts.
- Restricting threads to the faster cores: the pool is larger than that set,
  and the RT scheduler already places FIFO threads on the least loaded CPUs.
- A Preferences override: no demonstrated need.

## Measurements (2026-10-08)

**Setup**
- Release build of this decision on Intel Core Ultra 5 225F (10 CPUs).
- A paced 48 kHz, 128-frame device whose callback runs SCHED_FIFO 83, as
  PipeWire runs JACK's.
- 1024-frame render blocks, 200 ms ahead, 12 measured seconds per run, two
  runs per cell alternating policies (A–B–B–A).
- Load: one busy loop per CPU, either in the benchmark's cgroup (L1) or in a
  separate transient user scope (L2).
- A miss is a 2.67 ms callback that found no rendered audio, so 4500 in one
  run means the whole run was silent.

| Session | Load | Normal: misses, job p99, ring low-water | FIFO 10: misses, job p99, ring low-water |
|---|---|---|---|
| 24 stock tracks | none | 0, 10.2 ms, 8960 | 0, 9.6 ms, 8960 |
| 24 stock tracks | L1 | 9002, 45.9 ms, 0 | 0, 9.2 ms, 8960 |
| 24 stock tracks | L2 | 811, 35.5 ms, 0 | 0, 9.1 ms, 8960 |
| 32 stock tracks | L1 | 9005, 57.3 ms, 0 | 0, 12.2 ms, 8832 |
| 32 stock tracks | L2 | 9004, 39.0 ms, 0 | 0, 12.0 ms, 8832 |
| 4 Ferric, shared bridge | L1 | 0, 22.5 ms, 7680 | 0, 6.6 ms, 9088 |

**Real JACK** (PipeWire granted 48 kHz/128; 24 stock tracks under L1):
- Normal background: every callback of the run missed (4500), with zero JACK
  xruns, because the callback itself kept time.
- FIFO 10: no misses, job p99 9.5 ms.

**Desktop cost** (a normal-priority 1 ms sleep probe, p99 / worst):

| Desktop | Normal background | FIFO background |
|---|---|---|
| Idle, Firefox open | 0.17 / 1.9 ms | 0.58 / 8.8 ms (0.2% of wakes later than 5 ms) |
| Every CPU busy (L1) | 0.39 / 7.5 ms | 6.0 / 8.6 ms |

A probe at SCHED_RR 1, the compositor's class, stayed within 1.3 ms in every
cell. While every CPU is busy, someone waits: with normal threads it was the
audio, with FIFO 10 it is ordinary tasks, for up to about one render job.

**Other checks**
- SCHED_RR 10 matched FIFO: job p99 9.24 against 9.20 ms, no misses. The
  engine and bench option that forced it was removed afterwards.
- Restricting the same four-thread pool to the six fastest CPUs changed job
  p99 by 2% (14.3 against 14.6 ms), with equal headroom. Affinity stays as
  inherited.
- Restricting the whole default pool to six CPUs tripped the CPU cap: the
  engine warned, stayed at normal priority, and missed as normal does. Since
  then the default pools size from the allowed CPU mask, so a restricted
  process no longer trips the cap by default.
