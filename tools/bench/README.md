# Capacity benchmarks (B1 / B2)

B1 builds the session through the engine command bus. B2 exports that model's
compiled notation MIDI and the plugins' own CLAP-saved states, then uses public
ReaScript APIs to generate and save an RPP. The recipe lives once in
`host/engine/bench/session.json`: 120 BPM, a four-bar eight-note chord loop,
a Marram pad (two oscillators and a sub, unison 3, a driven ladder;
until 2026-10-21 Orrery's synth.md §9 patch, with its chorus and reverb) →
non-flat four-band Yarrow → Sundew on
N playback tracks; an additional armed stereo-input track with the same
chain, monitoring on; Sundew catching peaks (Clean, 20:1) on the master. N excludes the
monitored track. Playback tracks use −18 dB faders. The export includes the
engine's pan law; REAPER receives it explicitly. Engine tracks use `track.add`
defaults and their default parent sends, so new strip types need no builder
rewrite. REAPER's anticipative FX preferences are left at their defaults.

Build an optimized tool and the three stock artifacts before capacity runs:

```sh
cmake --preset release
cmake --build --preset release -j6 --target engine-bench plugin-marram plugin-yarrow plugin-sundew
```

On the otherwise idle workstation, each command searches and independently
confirms the largest passing N, for 300 measured seconds **per trial**:

```sh
# B1: two searches, anticipative on and off.
SUPPLE_AUDIO_DEVICE_TEST=1 build/release/host/engine/engine-bench --device jack --search --anticipative both --seconds 300 --max-tracks 256 --report docs/benchmarks/b1.md
# B2: REAPER's default anticipative FX, same plugins/patch/MIDI.
SUPPLE_AUDIO_DEVICE_TEST=1 python3 tools/bench/run.py b2 --engine build/release/host/engine/engine-bench --device jack --search --seconds 300 --max-tracks 256 --report docs/benchmarks/b2.md
```

Both real modes require `SUPPLE_AUDIO_DEVICE_TEST` equal to exactly `1`.
Unset, empty, `0`, `01`, and all other values refuse before opening a device.
Both hosts make an ordinary client latency request at 48 kHz, using `--block
64` (default) or `--block 128`. Neither forces a graph quantum or rate; inherited
`PIPEWIRE_QUANTUM` / `PIPEWIRE_RATE` are rejected. A user's JACK configuration
or another client may determine the granted quantum. JSON records requested
and actual settings. A different grant is retained as fixed-run data but is
rejected by a requested-setting capacity search. Server loss, missing signal,
processing errors, warnings or timing-storage overflow invalidate B1 data.
Silence with anticipation misses is not a missing signal: the renderer fell so
far behind that its ring never filled, and the run counts as a failure.
B1 supplies silent monitored input, opens no physical capture ports, and
silences physical outputs after observing the DSP peak. B2's private JACK
configuration still monitors physical stereo input and plays output; use a
silent input and an appropriate playback level for a comparable stock trial.
No system configuration is written. Historical reports using forced 64-frame
quantum describe the older tooling, not the current request behavior.

Dry verification (no physical audio device):

```sh
SUPPLE_AUDIO_DEVICE_TEST=0 build/release/host/engine/engine-bench --device paced --search --anticipative both --seconds .2 --max-tracks 8 --report build/bench/b1-dry.md
SUPPLE_AUDIO_DEVICE_TEST=0 python3 tools/bench/run.py b2 --engine build/release/host/engine/engine-bench --device offline --tracks 2 --report build/bench/b2-dry.md
SUPPLE_BENCH_LUA_CHECK=build/release/host/engine/engine-bench-lua-check python3 -m unittest discover -s tools/bench -p 'test_*.py' -v
python3 -m unittest discover -s tools/reaper-check -p 'test_*.py' -v
```

`paced` supplies silent stereo input on a stand-in thread. Its `xruns` is null; `synthetic_over_budget` counts callback duration above
the block deadline, and `synthetic_late_wakes` counts scheduler catch-up resets; anticipation misses
also fail a trial, including when the monitored/master path is live. B1 and
B2 exclude activation and a one-second preroll from the measured interval.
Short dry runs prove operation, **not** five-minute capacity. Debug builds can have no passing N; the search records N=0 and exits
nonzero. The report marks the device, build facts and exact durations.

For short tail measurements and an optional installed effect module:

```sh
python3 tools/bench/run.py b1 --engine build/release/host/engine/engine-bench --block 128 --tracks 24 --anticipative on --seconds 12 --report /tmp/stock-tails.md
python3 tools/bench/run.py b1 --engine build/release/host/engine/engine-bench --block 64 --plugin-file "$HOME/.clap/Ferric.clap" --plugin-id com.suppleaudio.ferric --tracks 4 --live on --anticipative on --seconds 12 --report /tmp/effect-tails.md
```

The effect mode (B1 only) scans the supplied module, instantiates the requested
ID with its default state, and plays the same original four-second stereo
noise/tone/burst fixture on each track at −18 dB. The driver generates that
WAV with `tools/reaper-check/check.py::test_signal`, retaining its hash and the
module hash. `--live on` arms/monitors all effect tracks; `--fx-per-track N`
creates a serial chain for comparison with independent tracks. `--workers N`
controls each pool's helper capacity (-1 automatic, 0 caller only);
`live_helpers` reports the enabled live capacity after backend setup. Direct `engine-bench`
use additionally requires `--input WAV` (four seconds, stereo, 48 kHz).
No third-party binary is copied into or distributed with the repository.
Stock session export/B2 remains the original MIDI fixture.

Scheduling and isolation (B1 only, engine.md §2.2, ADR-0026):

- `--background auto|normal|fifo` and `--background-priority N` choose the
  renderer's and background pool's policy (`EngineOptions::background`).
  Auto follows the callback.
- `--callback-priority P` (paced only) runs the paced callback thread, and the
  live helpers, at SCHED_FIFO P, as a PipeWire JACK callback runs at 83.
  Without it, a loaded machine would starve the paced callback itself and
  confound renderer measurements. P must be within the process's
  `RLIMIT_RTPRIO`.
- `--isolation in_process|shared_bridge|dedicated` sets every FX's isolation
  before the engine starts.
- `--park-us N` (experimental, item D): live helpers idle N µs within a
  callback's job park instead of spinning (`EngineOptions::liveHelperParkUs`;
  0, the default, spins). Recorded as `park_us`.

A background scheduling fallback is a warning, so it invalidates the run.

B1 run schema **`supple.capacity-run/2.4`** adds page faults over the
interval: `audio_page_faults` as `engine.status` counts them (`scope`, `minor`,
`major`, `threads`; on Linux the callback's and the live helpers'),
`thp_opt_out` (`advised_only`, `all` or `off`, ADR-0084), and on Linux each
listed thread's `minor_faults` and `major_faults` (a thread that took any is
listed). `minor_faults` and `major_faults` at the top level stay the whole
process's. Schema 2.3 added `bridge_timing`, the
interval's live bridged calls and overrun classes as in
`engine.status.bridge_timing` (ADR-0081), and on Linux `threads`: for each
real-time thread of the bench and its bridge processes (and any other thread
that waited over 1 ms for a CPU), the kernel's view over the interval: `policy`
and kernel `prio`, `cpu_ms`, `run_delay_ms` (runnable but waiting for a CPU,
from `/proc/<tid>/schedstat`), `slices`, `voluntary` and `involuntary` switches
and `migrations`. With `kernel.sched_schedstats=1` each thread's statistics
are reset at the interval's start and it also reports `wait_max_ms` (the
longest single wait runnable for a CPU), `wait_sum_ms`, `wait_count` and
`exec_max_ms` (the longest run without a switch) over the interval.
`bridge_timing` also carries `wake_buckets` and `return_buckets`: how many
answered live bridged calls took under 10, 20, 50, 100, 200, 500, 1000 and
2000 µs, and at or over 2 ms, from post to the bridge seeing the call, and
from the bridge finishing to the host seeing the answer. Schema 2.2 retains the 2.1 fields and adds
`isolation`, the requested `background`, `background_priority` and
`callback_priority`, and `scheduling`: the engine's applied policy per thread
class (`callback`, `live`, `renderer`, `background`, each `policy` and
`priority`). Schema 2.1 added `live_helpers` (enabled capacity after scheduling
setup). The timing fields are
`callback` and `render_job` objects (samples, dropped, frames, p50/p99/p999/max/mean in microseconds,
load_percent and over_budget), `ring_low_water_frames`, `ring_observations`,
output_peak and process CPU/fault deltas. p999 means p99.9; quantiles use nearest
rank. Empty distributions/unsampled ring watermarks are null, not zero. Whole
JACK-period timing includes all engine chunks when granted quantum exceeds
requested block. Renderer timing includes graph execution/join, not pacing or
ring copies. The ring metric is the conservative published-minus-consumed
stream distance sampled before callback consumption after priming; it can lag
ring publication and is not an exact atomic view of every frontier ring.

`--metrics off` disables probe clocks/storage and reports null timing objects;
only the benchmark ever enables them. Paced deadline accounting still uses its
clock with metrics off. Default `--sample-capacity 500000` bounds each series;
the allowed range is 1..4000000. Overflow counts invalidate a run, so increase
the bound for long runs. Samples are collected without allocation or logging;
summaries are computed after stopping. An in-flight sample may complete after
the gate closes. Process CPU excludes child processes and includes model-thread
service; the driver retains exact argv and hashes. These short measurements
do not establish five-minute capacity or a performance improvement.

B2 always generates the project under Dummy audio, closes audio/MIDI, checks
every loaded CLAP identity and loads its opaque state using `clap_chunk`.
Offline mode executes `-renderproject`, validates an eight-second stereo
48 kHz WAV with finite, non-silent audio, and never starts playback on JACK.
It reuses `tools/reaper-check`'s allowlisted private config, license copy,
resource-path check, blocked servers, user-config fingerprints, timeout and
process-group cleanup. Real measurement uses a separate private JACK config;
no user's device preferences, caches or projects are copied or changed.
`--reaper /path/to/reaper` selects the executable. A desktop display is needed
for REAPER startup; no GUI input or proprietary source/assets are used.

The real B2 script polls
[GetUnderrunTime](https://www.reaper.fm/sdk/reascript/reascripthelp.html#GetUnderrunTime)
in deferred callbacks and counts changes to audio and media xrun timestamps.
These are observed events, **not an exact cumulative counter**: multiple
xruns between polls can collapse into one observation, and audio timestamps
are available only when the backend supplies them. Zero observed changes is
the acceptance predicate. Poll count and maximum poll gap, both last-event
timestamps, REAPER version and audio-device info are retained in the TSV/JSON.

Search doubles N to bracket failure, then bisects integer N and independently
repeats the candidate. This assumes capacity is monotonic on an idle machine.
A failed confirmation returns nonzero and is recorded as unconfirmed. If
`--max-tracks` passes, the result is **≥N**, not a claimed maximum; increase
that bound to find a failure. `--tracks N` without `--search` measures one
case. B2 offline mode has no capacity search.

Each invocation gets a fresh `build/bench/<UTC>-<random>/` with per-trial logs,
JSON, generated scripts, exported session, RPP/WAV and config fingerprints.
The Lua regression parses 24/512-track projects and a 256-FX chain with the
vendored Lua parser (CTest supplies its path; direct tests can use system
`luac -p`), exercising track and FX scopes beyond the 200-active-local limit.
Build `engine.tests` to also build this test-only parser.

The final `report.json` has schema `supple.capacity-report/1.0`, all trials and
confirmation flags, exact command arrays, kernel, PipeWire version, CPU and
all governor values (read-only), revision/dirty status and CLAP artifact
hashes. No wall-clock threshold decides a unit test. Markdown rows append to
`docs/benchmarks/b1.md` or `b2.md`; `--report` redirects dry runs elsewhere.
`--output-dir` chooses a new artifact directory. Tool failures retain a
structured error and logs. Reports/license copies under build are not committed.
