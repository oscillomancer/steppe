# sync: timecode, MIDI clock, MTC, LTC and chasing

**Decisions:** ADR-0070 (and ADR-0045/0046 for the playrate it drives).
Library `libs/sync` (target `supple::sync`), namespace `supple::sync`,
headers under `supple/sync/`. GPL-3.0-or-later and DAW-only: it links
libltc (LGPL-3.0-or-later), so plug-ins, modules and the MIT libraries never
link it.

Status: the library is complete and tested; nothing in the engine calls it
yet.

## 1. Shared pieces (`common.h`, `dll.h`)

**Time** is always a device frame: the audio stream's sample count, as a
`double` so sub-sample positions survive. Nothing in the library reads a
clock.

**`MidiEvent`** `{frame, size, bytes[10]}`: one outgoing message at its exact
(fractional) device frame. A sample-accurate scheduler sends it at
`ceil(frame)` after any per-port offset. Events in a buffer are in
non-decreasing frame order; at one frame they keep their order (Stop, SPP,
Continue, Clock).

**`EventBuffer`** wraps caller-owned `std::span<MidiEvent>` storage; `push`
fails and sets `overflowed()` when full. No allocation. Size it with the
generators' `maxEventsPerBlock()`.

**`PositionMap`**: the engine's map from device frames to transport
position over one segment of continuous motion: beats for MIDI clock,
timeline seconds for MTC and LTC. `positionAt(frame)` must be
non-decreasing. `frameAt(position, lo, hi)` inverts it inside a bracket the
caller guarantees; the default bisects to the last bit (about 60
evaluations), and the engine should override it with its exact inverse.
`LinearMap` is a constant speed with an exact inverse.

**`Observation`** `{frame, position}`: an external position seen at a
device frame, in beats (clock) or timecode seconds (MTC, LTC: frame count
/ fps from 00:00:00:00). Receivers produce them; the chase tracker consumes
them.

### 1.3 `PeriodDll` and `LineFit`

`PeriodDll` is a second-order delay-locked loop on periodic arrival times:
for an event at t with e = t − t1: `t0 = t1; t1 += b·e + period;
period += c·e`, with w = 2π·B·T, b = √2·w, c = w² (ζ = 0.707; B the
bandwidth, T the period in seconds; w ≤ 0.5). Two events seed it. Its
bandwidth starts wide and narrows: B' = max(B, 1/(n·T)) after n events.
`time()` is the filtered time of the latest event.

`LineFit` is an incremental least-squares line, for rates measured over a
run of events.

## 2. Timecode (`timecode.h`)

| Rate | `rateName` | fps | Labels/s | Frames per day |
|---|---|---|---|---|
| `Fps23976` | `23.976` | 24000/1001 | 24 | 2 073 600 |
| `Fps24` | `24` | 24 | 24 | 2 073 600 |
| `Fps25` | `25` | 25 | 25 | 2 160 000 |
| `Fps2997Drop` | `29.97df` | 30000/1001 | 30, drop | 2 589 408 |
| `Fps2997` | `29.97` | 30000/1001 | 30 | 2 592 000 |
| `Fps30` | `30` | 30 | 30 | 2 592 000 |

- `toFrames(label, rate)` / `fromFrames(count, rate)`: exact, drop-frame
  included. Drop-frame skips labels `;00` and `;01` at the start of every
  minute not divisible by 10. Counts wrap at 24 hours (negative counts wrap
  backwards). `toFrames` clamps out-of-range fields and maps a skipped
  drop label to the next valid one; `valid()` tells them apart.
- `toSeconds(count, rate)` = count / fps; `framesAt(seconds, rate)` is its
  fractional inverse. Seconds are real time: one hour of 29.97 non-drop
  labels lasts 3603.6 s, of drop-frame 3599.9964 s.
- `TimecodeMapping {rate, start}`: timeline seconds 0 is label `start`.
  `framesAt(timelineSeconds)`, `timelineSeconds(frames)`, `labelAt()` (a
  1e-6-frame tolerance keeps exact frame starts on their frame).
- Text: `format()` writes `HH:MM:SS:FF`, `;` before the frames for
  drop-frame. `parse()` accepts `:`, `;`, `.` or `,`, two-digit fields,
  and `MM:SS:FF` or `SS:FF`; it validates for the rate. Errors
  `sync.timecode` and `sync.rate` carry a hint (the frame range, the drop
  rule, the rate names). `parseRate()` also accepts `29.97 DF`, `30df`,
  `29.97ndf`, `25fps` and the like.

## 3. MIDI clock (`midi_clock.h`)

Both ends: 24 clocks (F8) per quarter note. After Start (FA) the first
clock is beat 0; after Continue (FB) it is the Song Position Pointer's
position (F2 lsb msb, in 16ths); each further clock adds 1/24 beat.

### 3.1 `ClockGenerator`

- `start(frame, beats, out)`: at or before beat 0 (within 1e-9), Start, and
  the first clock at beat 0's frame. Otherwise SPP of the next 16th at or
  after the position (this one, when on it) and Continue, both at `frame`,
  and the first clock at that 16th's frame. Already running: as `locate`.
- `locate(frame, beats, out)`: running: Stop, then as `start`. Stopped: SPP
  alone.
- `stop(frame, out)`: Stop; nothing while stopped.
- `run(from, to, beats, out)`: a clock at every 1/24-beat crossing in
  `[from, to)`, at its exact frame. Contiguous calls emit each clock once;
  the block size does not change the frames beyond the inverse's
  precision. A full buffer defers the rest to the next call's `from`.
- SPP past 16383 sixteenths clamps; clocks still count from the true 16th.
- `maxEventsPerBlock(frames, maxBeatsPerFrame)`: the clocks at the highest
  speed plus Stop, SPP and Continue.
- The engine splits a block at a loop wrap: `run(from, wrap, mapA)`,
  `locate(wrap, mapB)`, `run(wrap, to, mapB)`.

### 3.2 `ClockReceiver`

`feed(frame, message)` takes complete messages (F8, FA, FB, FC, F2; others
ignored) and returns true when a clock while running produced an
observation. `update(frame)` checks the timeout once per block.

| Config | Default | Meaning |
|---|---|---|
| `bandwidthHz` | 0.3 | tempo loop bandwidth |
| `timeoutSeconds` | 0.25 | no clock this long (or 4 periods): not present, not locked |
| `lockClocks` | 24 | consecutive clocks within 25% of a period before locked |
| `recoverSeconds` | 2.0 | gaps up to this long while running count the missed clocks |

- `bpm()`: 60·sr / (24·period) from the loop; 0 before two clocks. A clock
  more than half a period off the prediction restarts the loop.
- `positionAt(frame)`: running, the latest clock's beat plus the elapsed
  fraction of a period (at most one clock); stopped, where Continue
  resumes.
- `lastObservation()`: the latest clock's filtered frame and beat.
- `running()` follows Start/Continue/Stop only: a signal loss leaves it
  alone (a master may count in before its first clock).
- `jumps()` counts Start, Continue and SPP changes.

Measured (tests, 48 kHz, ±1 ms uniform jitter, 123.4 bpm): locked after
0.51 s; tempo within 0.06% (worst 0.055 bpm); filtered clock times within
0.6 ms (worst 0.41 ms); a 0.6 s dropout keeps the position within a
quarter of a clock and re-locks within a second.

## 4. MIDI Time Code (`mtc.h`)

Quarter frame F1 `0ppp nnnn`: pieces 0..7 are frames low, frames high (1
bit), seconds low, seconds high (2 bits), minutes low, minutes high,
hours low, hours high (1 bit) with the rate code in bits 1–2 (0 = 24,
1 = 25, 2 = 29.97 drop, 3 = 30). `mtcRateCode()` maps 23.976 to 0 and
29.97 non-drop to 3. Full frame: `F0 7F 7F 01 01 hh mm ss ff F7`, hh =
code << 5 | hours.

### 4.1 `MtcGenerator`

- `start(frame, seconds, out)`: a full frame naming the label playing at
  the position, then quarter frames from the next even frame count at or
  after it. `locate` sends a full frame and restarts likewise; `stop` sends
  nothing.
- `run(from, to, seconds, out)`: piece p of the sequence naming even frame
  F at timeline position F + p/4 frames, at its exact device frame.
- `maxEventsPerBlock(frames, sampleRate, rate, maxSpeed)`.

### 4.2 `MtcReceiver`

`feed(frame, message)` takes quarter frames and full frames (any device
id) and returns true for a quarter frame once the position is known.

| Config | Default | Meaning |
|---|---|---|
| `expected` | none | the project's rate; decides 23.976/24 and 29.97/30 at once |
| `bandwidthHz` | 1.0 | quarter-frame loop (filtered observation times) |
| `timeoutSeconds` | 0.15 | no quarter frame this long: lost |
| `lockSequences` | 2 | consistent complete sequences before locked |

- Assembly runs in either direction: forwards a sequence completes at
  piece 7, backwards at piece 0, and piece p counts as F + p/4 frames
  either way, which accounts for the two frames a sequence takes. The
  count then advances (or retreats) a quarter frame per message; each
  complete sequence checks it.
- A skipped or repeated piece makes the position unknown until the next
  complete sequence; a reversal restarts lock; a sequence that disagrees
  with the count is a jump (`jumps()`), as is a full frame.
- Rate: the code; the expected rate when its code matches; else for codes
  0 and 3 a least-squares fit of quarter-frame arrivals over the first two
  seconds of lock decides, once until the signal is lost.
- `direction()`: +1, −1, or 0 when unknown.

Measured (48 kHz, ±1 ms jitter): locked after 0.12–0.16 s; filtered
positions within 0.6 ms (worst 0.44 ms at 24 fps); re-locked within 0.25 s
of a 0.4 s dropout.

## 5. LTC (`ltc.h`)

Over libltc 1.3.2. `create()` allocates and validates (`sync.ltc.config`:
sample rate 8–768 kHz, level −60..0 dBFS, queue 1..1024 frames;
`sync.ltc.alloc`). Per-block calls never allocate.

### 5.1 `LtcEncoder`

- Config: `sampleRate`, `mapping` (rate and start), `levelDb` (−18).
  Buffers are sized at creation for speeds from 1/8 to 8.
- `start(timelineSeconds)` / `locate`: LTC begins at the next frame
  boundary, silence before it (if the first render is already past it, at
  the boundary after). `stop()`: silence from the next render.
- `render(from, out, seconds)`: each LTC byte (a tenth of a frame) is
  stretched so it ends where the map puts it (beyond the block, the map's
  mean speed is extrapolated), so the signal follows playrate and tempo to
  the sample. A jump of more than a frame that `start` was not told about
  restarts at the next boundary. Output is a ±1 square wave scaled to the
  level, through libltc's 40 µs rise-time filter.
- Labels are packed here; the drop-frame flag is set only for 29.97df.

### 5.2 `LtcDecoder`

- Config: `sampleRate`, `expected`, `queueFrames` (32: the frames one
  `feed` may return, a second at 30 fps), `timeoutSeconds` (0.2),
  `lockFrames` (4).
- `feed(frame, samples)` returns the frames whose end fell in the samples:
  label, count, start and end sample, reverse flag, drop flag, level (dBFS)
  and an observation: forwards the next frame's start (count + 1) at the
  sample after the frame; backwards the frame's own start (count).
- Lock after four consecutive frames each one on from the last (either
  way); a break is a jump; 0.2 s without a frame loses the signal.
- Rate: the drop flag means 29.97df; else the expected rate; else the
  integer rate nearest the frame length until two seconds of consecutive
  frames, whose mean length then decides 23.976/24 and 29.97/30 once.

Measured: at 44.1, 48 and 96 kHz and every rate, every frame decodes from
the first; observations within 0.07 ms; frame starts within 3 samples of
the map. Frames decode without a break through −46 dBFS of noise with the
level rising 20 dB from 14 dB above it, at playrates 0.5–2 (libltc may miss
the first frame at 2×), and backwards.

## 6. Chase tracker (`chase.h`)

`observe(observation)` for each receiver observation; `update(frame,
localPosition, nominalSpeed)` once per block, after them. Positions are in
the observations' units; `nominalSpeed` is local units per second at
playrate 1 (bpm/60 for beats, 1 for timecode seconds; the engine adds the
project's timecode start to its timeline seconds). `jumped()` tells it a
receiver's jump count changed. `predict(frame)` is the estimated external
position (the relocate target: `predict(frame + seek latency)`).

**Output:** `state` (Idle, Acquiring, Relocating, Chasing, Locked), `rate`
(the playrate to apply; 1 while idle), `errorSeconds` (external − local,
positive when behind), `externalRate` (speed / nominal), `relocate`.

**Estimator:** x̂' = v + 2ζω·r, v' = ω²·r (ζ = 0.707, ω = 2π·`estimatorHz`),
integrated per observation with k1 = min(√2·ω·dt, 1), k2 = min(ω²·dt,
1/dt). `seedSeconds` of observations give the first speed. A residual over
`jumpSeconds` of external time, or `jumped()`, re-seeds the position and
keeps the speed.

**Control:** rate = v/nominal + e/Tp + I, I += e/(Tp·Ti)·dt only while
|e| < `integralBandSeconds` and the correction is inside the window; the
correction is clamped to ±`window`·v/nominal; the rate to [`minRate`,
`maxRate`]. While Relocating the rate is v/nominal and I resets.

**Relocation:** above `relocateAboveSeconds` for `relocateDelaySeconds`:
`relocate` once; re-armed below `relocateBelowSeconds`, or after
`retrySeconds` if still above.

**Lock:** within `lockSeconds` for `lockHoldSeconds` once settled
(`settleSeconds` of tracking): Locked; beyond `unlockSeconds`, or
relocating: not.

| Config | Default |
|---|---|
| `estimatorHz` | 0.5 |
| `phaseSeconds` (Tp) | 2.0 |
| `integralSeconds` (Ti) | 8.0 |
| `integralBandSeconds` | 0.02 |
| `window` | 0.1 |
| `minRate`, `maxRate` | 0.25, 4 |
| `relocateAboveSeconds`, `relocateBelowSeconds` | 0.25, 0.05 |
| `relocateDelaySeconds`, `retrySeconds` | 0.1, 1.0 |
| `lockSeconds`, `unlockSeconds`, `lockHoldSeconds` | 0.002, 0.010, 0.5 |
| `jumpSeconds` | 0.5 |
| `seedSeconds`, `settleSeconds` | 0.1, 1.0 |

**Stability:** crossover near 0.5 rad/s; the engine's playrate path (ramps
of at most 300 ms, the 150 ms low-pass, render-ahead: about 0.5 s) costs
about 15° of phase margin. Tests run the loop through a modelled delay of
0.3–0.5 s and the low-pass.

Measured (100 observations/s with ±1 ms jitter, 0.3 s delay + 150 ms
low-pass): 10 s behind: one relocate, locked at 3.5 s, then within 0.7 ms
and the rate within 0.07%; 0.1 s behind a source 2% fast: no relocate,
locked at 6.4 s, within 0.5 ms; through the MIDI clock receiver, a 130 bpm
master against a 120 bpm project: rate 1.0829 (130/120 = 1.0833), within
0.7 ms.

## 7. Real-time safety and determinism

Every per-block entry point is `SUPPLE_RT`: the generators' `start`,
`locate`, `stop`, `run`; the receivers' `feed`, `update`, `reset`; the LTC
`render`, `feed`, `update`; the tracker's `observe`, `update`, `predict`.
Their tests run under RealtimeSanitizer (`rtsan`). Construction, `create()`,
moves and destruction of the LTC classes, and text parsing and formatting,
are not real-time. Outputs depend only on the inputs: no clocks, no
randomness, no allocation.
