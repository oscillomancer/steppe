# Stock effects: implementation notes

**The parameters, names and DSP intent are in `docs/specs/stock-effects.md`**;
where a decision here changes that spec, it is written there too. This file
adds what maintainers need beyond it: the shared layout, real-time rules,
tests, GUI and skill deliverables, and per-plugin implementation notes.

Code identity uses the functional ids; each display name is defined once,
in the plugin's `cmake/Identity.cmake` (for a scripted stock plugin,
ADR-0047, in its script's `Info` and its `product.cmake`).

| id (dir, code, CLAP id suffix) | display name | CLAP id | State magic | AU subtype | Features |
|---|---|---|---|---|---|
| `yarrow` (`plugins/yarrow`) | Yarrow | `com.suppleaudio.yarrow` | `LzjE` (the player's for the id) | `Yarw` | audio-effect, equalizer |
| `sundew` (`plugins/sundew`) | Sundew | `com.suppleaudio.sundew` | `pqwP` (the player's for the id) | `Sndw` | audio-effect, compressor |
| `treeline` (`plugins/treeline`) | Treeline | `com.suppleaudio.treeline` | `tdOY` (the player's for the id) | `Trln` | audio-effect, limiter |
| `gorse` (`plugins/gorse`) | Gorse | `com.suppleaudio.gorse` | `dEQG` (the player's for the id) | `Gors` | audio-effect, transient-shaper |
| `hickory` (`plugins/hickory`) | Hickory | `com.suppleaudio.hickory` | `iEKh` (the player's for the id) | `Hkry` | audio-effect, utility |
| `sorrel` (`plugins/sorrel`) | Sorrel | `com.suppleaudio.sorrel` | `sTMn` (the player's for the id) | `Sorl` | audio-effect, gate |
| `teasel` (`plugins/teasel`) | Teasel | `com.suppleaudio.teasel` | `BLWw` (the player's for the id) | `Tesl` | audio-effect, distortion |
| `bracken` (`plugins/bracken`) | Bracken | `com.suppleaudio.bracken` | `PzKJ` (the player's for the id) | `Brkn` | audio-effect, delay |
| `ghyll` (`plugins/ghyll`) | Ghyll | `com.suppleaudio.ghyll` | `DIow` (the player's for the id) | `Ghyl` | audio-effect, reverb |
| `lichen` (`plugins/lichen`) | Lichen | `com.suppleaudio.lichen` | `cwHJ` (the player's for the id) | `Lchn` | audio-effect, analyzer |
| `marram` (`plugins/marram`) | Marram | `com.suppleaudio.marram` | `XZXx` (the player's for the id) | `Mrrm` | instrument, synthesizer |
| `withy` (`plugins/withy`) | Withy | `com.suppleaudio.withy` | `aSho` (the player's for the id) | `Wthy` | audio-effect, chorus |
| `pollia` (`plugins/pollia`) | Pollia | `com.suppleaudio.pollia` | `EGJQ` (the player's for the id) | `Poll` | audio-effect, phaser |
| `umber` (`plugins/umber`) | Umber | `com.suppleaudio.umber` | `auff` (the player's for the id) | `Umbr` | audio-effect, distortion, tape |
| `sedge` (`plugins/sedge`) | Sedge | `com.suppleaudio.sedge` | `RkzT` (the player's for the id) | `Sedg` | audio-effect, analyzer |
| `gain` (`plugins/gain`) | Gain | `com.suppleaudio.gain` | `sRvm` (the player's for the id) | `Gain` | audio-effect, utility |
| `hornbeam` (`plugins/hornbeam`) | Hornbeam | `com.suppleaudio.hornbeam` | `hCbo` (the player's for the id) | `Hrnb` | audio-effect, distortion, amp-simulator |
| `coppice` (`plugins/coppice`) | Coppice | `com.suppleaudio.coppice` | `Idrr` (the player's for the id) | `Cpce` | instrument, sampler |
| `pollard` (`plugins/pollard`) | Pollard | `com.suppleaudio.pollard` | `ntDE` (the player's for the id) | `Plrd` | instrument, drum-machine, sampler |
| `drumlin` (`plugins/drumlin`) | Drumlin | `com.suppleaudio.drumlin` | `WkjN` (the player's for the id) | `Drml` | instrument, drum-machine, synthesizer |

Marram, Coppice, Pollard and Drumlin are instruments; their specs,
implementation notes included, are `docs/specs/marram.md`,
`docs/specs/coppice.md`, `docs/specs/pollard.md` and `docs/specs/drumlin.md`.

## 1. The shape every effect shares

(2026-10-20) No stock effect has this shape any more: each is a DSP script
(ADR-0047; `plugins/README.md` says how to change one). This section
records the hand-written design they replaced. Orrery, the synthesizer,
was the last plugin built this way (through `SemanticAdapter`, after
`FlatSemantic` went with its last user); it and `plugins/common`'s plugin
support were removed on 2026-10-21, when Marram replaced it.

### 1.1 Layout

Each stock effect defines its display name only in
`plugins/<id>/cmake/Identity.cmake`: `set(<PREFIX>_DISPLAY_NAME "<Name>")`.
The engine receives `SUPPLE_<PREFIX>_DISPLAY_NAME` as a public compile
definition; its name constant and the plugin's artifact/bundle/test paths
use that value. `supple_add_plugin(<id> <PREFIX>)` reads the identity file
itself; an optional third argument names artifacts for template products
without that file. CLAP ids, state magics and functional code identities
stay fixed when the display name changes.

Like Orrery (`plugins/orrery`):

- `plugins/<id>/engine/` — the SDK-free core, a monorepo library
  `supple::<id>-engine` (namespace `supple::<id>`), built and tested by the
  monorepo with its warnings and all sanitizers (label `rt` on its tests):
  - `include/<id>/params.h` — the one `core-params` table (`ParamSpec`s:
    permanent CLAP ids, keys, units, ranges, curves, tags, descriptions) and
    the `patch::FlatPatchType` (`"supple.<id>-patch/1.0"`, no seed);
  - `include/<id>/processor.h` + `src/` — the DSP: `prepare(sampleRate,
    maxFrames)`, `reset()`, `setParam(index, physical)` (RT), `process(in, out,
    frames[, key])` (RT, `SUPPLE_RT`), `latency()`, plus pure functions the
    GUI and tests use (e.g. the EQ's magnitude response);
  - `describe()` — the compact prose (§1.6);
  - `tests/` — the monorepo tests of §1.7.
- `plugins/<id>/` — a copy of the template made as `plugins/orrery` is
  (read its `STEPPE.md`: identity renames, vendored dependency paths,
  `supple_plugin_add_monorepo_libraries()`, format-gate and license-check
  handling, `supple_add_plugin(<id> <PREFIX>)` in `plugins/CMakeLists.txt`).
  The template is an effect already: keep its audio-port and process
  structure. Its `*_clap.cpp` product sections forward
  to `supple::clapx::ParamStore` / `ClapParams` / `FlatSemantic`
  (`plugins/common`) and to the engine; `STEPPE.md` lists every deviation.
  The shared template infrastructure stays byte-identical apart from names.
- The template's DSP header (`dsp/include/<id>/Processor.h`) is replaced by
  the engine library, as in Orrery.

A change to one plugin stays in its directory. DSP primitives that two
plugins need move to `libs/core-dsp` (the shared oversampler is an
example), with their tests.

### 1.2 Parameters and state

- Keys are exactly `stock-effects.md`'s (flat snake case, `bandN.` for EQ
  bands); groups follow the spec's grouping. Units are canonical (`Hz`, `dB`, `ms`, `percent`,
  `ratio`); curves per ADR-0010 (log for frequencies and times). Every float
  gets a `smoothingMs` (20 unless stated) and tags from the role vocabulary.
- Read-only meters are `kReadonly` output parameters, updated from the
  audio thread through the `ParamStore` and reported to the host with
  `CLAP_EVENT_PARAM_VALUE` output events at most every 50 ms.
- Patch and state: `FlatSemantic` (framed canonical JSON, transactional
  load); the extension's `describe` returns §1.6's prose.
- The table passes `params::validateTable` in a test; ids are dense from 0 in
  table order and never renumbered afterwards (append only).

### 1.3 Real-time rules

- `process` never allocates, locks, logs or waits; everything is sized in
  `prepare` (max block and the largest sample rate's delay lines).
- **Block-size invariance:** for the same input and the same parameter events
  (by sample offset), the output is bit-identical whatever the host's block
  partition (1, 7, 64, 4096 frames). Smoothing and coefficient updates run per
  sample (or on a fixed internal grid of 16 samples counted from `reset`, never
  per host block).
- Any sample rate 22 050–384 000 Hz; frequencies above 0.45·fs are clamped
  internally (the parameter keeps its value).
- EQ replaces non-finite input samples with zero before recursive/FIR state.
  Hickory (the utility) makes its working values finite before any filter
  or delay state, while at rest its output keeps the input's bits.
- Denormals: `dsp::DenormalGuard` around processing; recursive states are
  also flushed to zero below 1e-30 when the input has been silent.
- Parameter changes are smoothed (no zipper noise); a switch that cannot be
  smoothed (a filter type, a detector mode) crossfades over 10 ms.
- Mono (1-in/1-out) works where the template supports it; stereo is the
  default configuration.
- Latency is reported through `clap.latency`. Latency-changing parameters
  (lookahead, linear phase) are stepped and not automatable
  (`stock-effects.md` §0) and follow the template's restart contract.

### 1.4 Determinism

No randomness. The same state, input and events give the same output across
runs, processes and sample-rate-independent block partitions (§1.3).

### 1.5 GUI

Product-owned `Editor.cpp`/`Theme.h`/`Widgets.h` usage (the template's widgets
and window stay as shipped). Each plugin's layout is in its section below;
all read names, units and formatted values from the table, and every edit is
a gesture-bracketed parameter change.
`tools/gui_preview` renders at least two scenarios per plugin to PNG for review.
Design size ≤ 900 × 560. The look follows the product theme used by Orrery
(`plugins/orrery/plugin/gui/Theme.h`): same palette and type, so the stock
plugins read as one family. The scripted stock plugins that replace them
(ADR-0047) take the scripts' warm palette instead (`stock-effects.md` §0).

### 1.6 describe()

At most 300 characters for `changedOnly`, listing only what is not at its
default, in signal-flow order, with units — e.g. the EQ: `high-pass 80 Hz
24 dB/oct → bell 250 Hz −3 dB Q 1.2 (mid) → high shelf 10 kHz +2 dB; output
−1 dB`. Full detail may list everything.

### 1.7 Tests

In the engine library (monorepo, all presets):

- table validation, patch round trip (defaults and a full non-default patch),
  merge errors with paths;
- **null test:** default parameters pass audio through bit-exactly (EQ with
  every band flat or off, utility, analyzer) or within the plugin's stated
  tolerance (compressor below threshold: unchanged within 1e-6, delayed by
  its latency);
- **block-size invariance** (§1.3) with automation events;
- **sample-rate sweep** 44.1/48/88.2/96/192 kHz: the documented response
  holds at every rate (per-plugin checks below);
- **automation stress:** every parameter jumping to random values every
  1–64 samples for 5 s of noise: finite, bounded output (no NaN/Inf, peak
  below +40 dBFS), no denormal stalls;
- **state:** set random values, save, load into a fresh instance, identical
  output;
- the per-plugin behaviour tests below, each with a measured tolerance.

In the plugin copy (as Orrery): `host_smoke`, `editor_input`, the template's host contract, and the clap-validator run
(`plugin.<id>.validator`, label `slow`).

### 1.8 Skill and docs

- `skill/guides/<id>.md`: what the plugin is for, every key with its unit
  and range, three short recipes (e.g. the EQ: remove rumble, carve a
  masking region reported by `analyze.masking`, de-harsh), and honest limits.
  Linked from `skill/SKILL.md` and `skill/guides/mixing.md`.
- `plugins/<id>/STEPPE.md` (deviations from the template) and a STATUS row.

## 2. EQ — Yarrow [`yarrow`] (`stock-effects.md` §1)

A DSP script (ADR-0047), `plugins/yarrow/script/yarrow.cpp`; the
spec section holds the designs and measured accuracy. Notes for maintainers:

- **Audio.** `update()` reads the parameters into eight `Setting`s and, when
  auto gain is on, works out its offset; `control()` runs every 16 samples
  (a counter kept across blocks) and takes structure changes once a band
  has faded out, glides frequency, gain and Q, follows dynamic bands and
  redesigns a band only when its values moved; `process()` runs each band
  that is on or fading, in index order, placing it on both channels, the
  mid or side (encoded around that band only) or one channel, then listen,
  then the output gain. The first `update()` after `prepare()` (or a reset)
  snaps every glide, so a render after `reset()` matches a fresh instance.
  Non-finite input is zeroed before any band.
- **Editor.** Its own members only: the band settings read from the
  parameters each repaint, each band's sections designed once per repaint,
  the transform's tables and buffers, and the pointer's state. The audio
  code shares the output ring (`SharedArray<float, 32768>`), its head and a
  running sample count (the analyser's clock), peaks, each band's live gain
  and detector level, and the auto-gain offset. Edits go through
  `change()`/`act()`, which select a band's parameter by index.
- **Tests.** `plugin.yarrow.response` measures bells, cuts and shelves
  through the built plugin; `plugin.yarrow.catalog` keeps `Catalog.cpp` in
  step; the template's suite, reset, unload, semantic conformance and
  clap-validator checks run as for every stock plugin. During development,
  `dsp.test` with `automation` (no excess step on type, channel or enable
  switches) and `dsp.preview` with `pointer` steps covered clicks and every
  gesture.

## 3. Compressor — Sundew [`sundew`] (`stock-effects.md` §2)

A DSP script (ADR-0047), `plugins/sundew/script/sundew.cpp`; the
spec section holds the laws and ballistics. Notes for maintainers:

- **Audio.** `Law` holds a character's static law and its one-pole steps at
  one sample rate (`set()` in `update()`, again when the tempo moves a synced
  release); `Cell` is one channel's gain cell, stepped once per sample with
  that channel's detector level. The feedback cells (Opto, FET) take the
  key's level before their own gain and solve the implicit one-pole with
  three Newton steps, so the loop is stable at any attack and ratio.
  `process()` per sample: key (input or connected sidechain, high-pass,
  mid/side) → level (peak or mean square, then link) → cells → gain on the
  lookahead-delayed audio → colour → mix → match → output → listen. A change
  of character settles the new cells on the old amount, or (to or from
  Bloom) at rest with `bridge`, a 30 ms glide of the old gain. `unplayed`
  zeroes the match until audio plays, so a render depends only on its values.
- **Lookahead** is a `.restarts()` choice: `prepare()` reads it and reports
  the latency, so a change waits for the host's restart, and the delay in use
  is always the one reported. The key is never delayed (the detector looks
  ahead); listening to the key reads it through its own delay.
- **Editor.** Its own members only: a `Law` and a `Cell` for the
  ballistics plot and the character icons (computed once, and again only
  when a value the plot depends on moves), the needle's motion and peak, the
  dot's trail and the pointer's state. The audio code shares the history
  (512 columns of 10 ms: input and compressed peaks, gain change, bar or
  beat), the key level, the gain change, peaks, the match and loudness, the
  tempo, the bar length and whether the external key is live.
- **Tests.** The template's suite, reset (also `--stepped`), unload,
  semantic conformance, clap-validator and the catalog check run as for
  every stock plugin. During development, `dsp.test` measured the static
  law of every character (within 0.3 dB of the set ratio at 6 and 24 dB
  over), external-key ducking (−21.4 dB for −21.6 expected), the latency
  of each lookahead, onset peaks with and without lookahead, the match
  converging on the dry loudness, and no excess step when switching
  character (including to and from Bloom), threshold, mix, output, stereo,
  key filter, match, colour and sync; `dsp.preview` with `pointer` steps
  covered every gesture.

## 12. Limiter — Treeline [`treeline`] (`stock-effects.md` §10)

A DSP script (ADR-0047), `plugins/treeline/script/treeline.cpp`; the
spec section holds the detector, the gain path and the loudness meters.
Notes for maintainers:

- **Audio.** `TruePeak` is one channel's detector (the phases are designed
  in `prepare()`); `Hold` the monotonic queue; `Box` a running sum that is
  summed afresh every 65536 samples so rounding never builds up; `Bell`
  three boxes in a row. `process()` per sample: drive → detector →
  requirement → `listenTo()` (sharpness and the bass guard) → hold →
  two-stage release → both bells and their blend → the delayed audio →
  the output's own detector and K-weighting → listen. The guarantee rests
  on each bell's span being no longer than the hold and on the delay
  matching the broad bell's span: keep them together when tuning.
- **No second stage.** A gain moving within a millisecond widens the
  signal's spectrum, so in principle the product can pass the ceiling
  between samples. A second, 0.5 ms stage on the output was tried: it
  never took more than 0.001 dB on any test material, while costing a third
  of the CPU and a millisecond of latency, so it went. What decided the
  accuracy was the interpolator's length (32 taps per phase read up to
  0.5 dB low on noise-like hits; 48 do not).
- **Loudness.** `closeBlock()` every 100 ms: the 30-block ring and the
  300-block recent rings (momentary energies, short-term values, true
  peaks), then either the recent readings (two passes over the ring for
  the integrated gate; the LRA's gated values insertion-sorted, at most
  300) or, while accumulating, the integrated histogram (gate recomputed
  from bin sums: no block list grows) and the LRA histogram.
  `restartLoudness()` on the transport's start; `restartTotals()` when
  `accumulate` changes (in `update()`).
- **Editor.** Its own members only: eased values for the engine plots (the
  first frame starts where the audio is, so a preview shows the moment),
  the gain bar's held peak and the pointer's state. The audio code shares
  the history (1024 columns of 10 ms: input and output envelopes, the
  reduction and its sustained part, the momentary loudness, bar or beat) and
  the engine's and meters' values. The gain-to-target button edits `gain`
  as a click on a knob would.
- **Tests.** The template's suite, reset (also `--stepped`), unload,
  semantic conformance, clap-validator and the catalog check run as for
  every stock plugin; `plugin.treeline.response` drives a sine, a sweep,
  noise and an impulse 12 dB into a −3 dB ceiling at 44.1, 48 and 96 kHz
  (each peaks at −3.0) and checks a sine under the ceiling passes
  unchanged. At 88.2 and 192 kHz too the ceiling held; latency is 199,
  215, 376, 407 and 791 samples at 44.1, 48, 88.2, 96 and 192 kHz. During development, on
  synthetic club, acoustic, dense and true-peak material driven 12 dB:
  the output's true peak, read offline with the same detector, stayed
  within 0.001 dB of the ceiling; against BS.1770's example meter within
  that meter's own +0.17 dB passband ripple; an ideal (24 kHz brickwall)
  reconstruction reads up to 0.5 dB higher only on material with strong
  content above 21 kHz (clipped noise). A 50 Hz sine under 7 dB of
  reduction at a 20 ms release had 0.0003 % THD with the bass guard and
  0.0055 % without. CPU: about 2 % of a core in a release build at 48 kHz
  stereo (four interpolators).

## 9. Transient shaper — Gorse [`gorse`] (`stock-effects.md` §4)

A DSP script (ADR-0047), `plugins/gorse/script/gorse.cpp`; the spec
section holds the detector, the law and the hit view. It keeps Flint's
design (a held peak, fast and slow followers in dB, one part at a time) and
changes what Flint got wrong in use. Notes for maintainers:

- **The hold.** Flint measured a cycle between any two local maxima. On
  real material, noise or a quantized waveform's ripple near its crest
  makes maxima every few samples, so its hold sat at 1 ms and a bass line
  or a kick's body rode through the detector as onsets: on 16-bit drums a
  kick's tail read as onsets every cycle (sustain −6 dB gave −0.5 dB). The
  hold now counts crests only across a real dip, follows a leading low band
  for a whole cycle (`listenLow()`), and is never shorter than `length`
  (a voice-like 180 Hz pulse took +11.9 dB of attack at +12 before that,
  0.0 after).
- **The law.** A fall counts three times as readily as a rise (`kTail`),
  since a decay's lead is a few dB where an onset's is tens: on a kick
  decaying 78 dB/s, sustain −6 dB now gives about −4 dB at default
  settings. The slow follower stays within 30 dB of the fast one
  (`kSpread`). To watch the law at work, have a scratch copy
  (`plugins/README.md`) write o and fast − slow to its output in
  place of the audio, and render material through it.
- **Lookahead.** Flint's 1 ms one-pole let the first millisecond through:
  an attack cut missed the click it was meant to soften. Two boxes ramp the
  gain over the 1 ms lookahead and land on the onset. `low` focus hears
  through about 2 ms of filter delay, so its gain lands about a millisecond
  after the onset; it is for the kick's body, not its click.
- **The clip** moved to the end (after mix and output), where its
  minimum-phase oversampler cannot comb against the dry path, and runs only
  while it is on or fading. Its output is clamped to ±1: on a snare driven
  24 dB over, the decimator rang to +7.5 dBFS.
- **Hits.** `startHit()` on a trigger (closing the hit being captured),
  `captureStep()` per sample on the delayed input and the mixed output,
  `closeBin()` per point (a point shorter than a sample repeats the last),
  `finishHit()` into the average (a capture under 10 ms is a double
  trigger and is dropped), `measureHits()` for the meters. The arrays reach
  the editor only when a hit finished.
- **Editor.** Its own: the model hit (`refreshModel()`, recomputed when
  attack, sustain, length or sensitivity change; 16 kHz steps for 501 ms),
  the eased points it draws, and the pointer's state. The colour of a
  point is the sign of the mean o over it.
- **Tests.** The suite every stock plugin has (reset, also `--stepped`;
  unload; semantic conformance; clap-validator; catalog), and
  `plugin.gorse.response`: a sine starting from silence is a hit with no
  tail; with attack +6 its onset peaks exactly 6 dB higher at −12 dBFS
  (44.1, 48, 96 kHz) and at −32 dBFS, its steady part passes at 0.0 dB and
  0.0 % THD; driven 24 dB into the clip, a sine, a sweep, noise and an
  impulse peak at 0.0 dBFS. During development on synthetic drums (kick,
  snare, hats at 120 BPM), attack ±6 changed every hit's first 2 ms by
  ±6.00 dB, at −6 and at −26 dBFS alike, and defaults passed bit-exactly.

## 4. Utility — Hickory [`hickory`] (`stock-effects.md` §8)

A DSP script (ADR-0047), `plugins/hickory/script/hickory.cpp`. It
keeps Plumb's controls and laws (the linear balance, bass mono as a
Linkwitz–Riley high-pass on the side only, bit-exact at rest) and changes
the rest. Notes for maintainers:

- **Order.** Polarity → channels → DC block → delay → width and bass mono
  (M/S) → balance → gain and mute. Polarity comes first (Plumb flipped
  after the routing), so `mono` with `invert_right` sums a flipped pair
  correctly. Swap is a `channels` option rather than a switch of its own,
  which removes Plumb's redundant combinations (swap with left-to-both).
- **At rest.** Each stage that is at rest leaves the sample alone, and a
  sample no stage touched is not written back: the output is the input's
  bits, non-finite values included. Working values are made finite before
  any filter or the delay line sees them, and those filters run all the
  time, so switching the DC block or bass mono on fades in settled state.
- **Ramps.** The routing matrix, polarity, the DC block, bass mono and
  mute crossfade over 10 ms (`Ramp`, which jumps while unplayed); gain,
  width and balance glide over 20 ms (`.smoothed()`). A new delay time
  crossfades from the old tap over 20 ms, and a change that arrives during
  a crossfade waits for it to end, so automation never jumps.
- **Delay** is whole samples (`roundToInt`), so a delayed signal is an
  exact copy; it is not reported as latency, since lining a track up
  against another is the point.
- **DC block** is a 12 dB/oct Butterworth high-pass at 5 Hz (Plumb's was a
  one-pole): 0.02 dB down at 20 Hz.
- **Readouts.** 100 ms chunks of LL, RR, LR, MM, SS and the peak, summed
  over the last 30 (`closeChunk()`): `output_peak`, `correlation` (0 in
  silence), `level_balance` (R over L) and `side_level` (S over M, −60 when
  mono).
- **Editor.** The image is 1024 points of (M, S), one every 1/12000 s,
  scaled so their spread fills a third of the circle (and their peaks no
  more than twice it). Width by frequency is ten octave band-passes
  (Q 1.414, 31 Hz to 16 kHz) on each output channel, their correlation
  over 250 ms; an octave fades with its level. A press there moves the
  crossover and turns bass mono on; a horizontal drag across the image
  changes the width. The warning under the image shows while the 300 ms
  correlation is below −0.3.
- **Tests.** The suite every stock plugin has (reset, also `--stepped`,
  which leaves alone a switch that silences the plugin by itself, here
  `mute`; unload; semantic conformance; clap-validator; catalog), and
  `plugin.hickory.response`: a sine passes at 0.00 dB, gain −6 and +12.5
  measure exactly, gain −60 (−inf), mute and `side` of identical channels
  are silent, mono with width 0 and bass mono leaves identical channels at
  0.00 dB, and the DC block takes 0.02 dB at 20 Hz. During development on
  stereo test tones, bass mono at 150 Hz lowered the side at 50 Hz by
  38.3 dB and left it at 1 kHz unchanged, with the mono sum unchanged to
  float rounding; width 0 and 200, every routing, the flips and a 10 ms
  delay (an exact shift) measured as specified, and the readouts read
  correlation −0.71 on a flipped pair, a −6.02 dB balance where the right
  was 6 dB down, and −60 dB of side at width 0.

## 5. Analyzer — Lichen [`lichen`] (`stock-effects.md` §9)

A DSP script (ADR-0047), `plugins/lichen/script/lichen.cpp`. It
replaced Gauge (`com.suppleaudio.analyzer`, a template plugin with a
measurement worker and the `get_telemetry` extension member) with a new
design. Notes for maintainers:

- **It never writes the block.** Every measurement reads `io` into locals
  (non-finite samples become 0 there only). `product.cmake` sets
  `PRODUCT_TRANSPARENT` (template framework 1.27.0), so host-smoke demands
  output equal to input, float and double, in place and not; nothing that
  makes the plugin write its output can pass.
- **Readouts are the interface.** Every measurement an agent can use is a
  `.meter()` parameter, read in the DAW with `fx.meters` (live, or as the
  last `render.mix` ended), in the sandbox with `param.get` or `dsp.test`'s
  `meters`. Gauge's JSON telemetry needed its own extension member and was
  never read by the engine; read-only parameters reach every host.
- **The audio thread does the analysis**, all of it cheap per sample: the
  true peak's interpolation only near the highest peaks (§9 of the spec),
  K-weighting, sums; two 16k transforms a frame every 85 ms (the runtime's
  `Stft`, prepared for analysis only, so no inverse transform runs) and a
  2k one for the centroid. Histograms replace stored history (loudness in tenths of a LU,
  the centroid in 300 log steps), so a play of any length costs the same.
- **Readouts as the offline analysis defines them.** The named bands, the
  tilt, the centroid, width, balance, correlation and mono loss follow
  `core-analysis.md`, so a live reading and an analysis report of the same
  audio agree (a 16 s test mix: bands within 0.7 dB, tilt −4.5 against
  −4.8 dB/oct, PLR 16.8 against 16.9, LRA 1.6 against 1.5).
- **Tests.** The suite every stock plugin has (the reset check compares
  readouts as well as audio, which is what it checks here), and
  `plugin.lichen.response`: a −12 dBFS 1 kHz sine in both channels passes
  at 0.000 dB and reads −11.99 LUFS, −12.00 dBTP, crest 3.01 dB,
  correlation 1, width 0, mono loss 0, the mid band 0 dB of the whole, 2 s
  measured and no key. Release build, one core, stereo 48 kHz, dense noise:
  2.0 %.

## 7. Saturation — Teasel [`teasel`] (`stock-effects.md` §5)

A DSP script (ADR-0047), `plugins/teasel/script/teasel.cpp`. It
replaced Kiln (`com.suppleaudio.drive`) with a new design. Notes for
maintainers:

- **Everything oversampled.** The band split, the dry signal and the clean
  bands run at the high rate with the curve, so mixing and splitting
  never comb against the oversampling filters. `phase` picks the script
  runtime's minimum-phase half-bands or its linear-phase ones (a restart);
  at 1x the oversampler is prepared at a factor of 1 and passes samples
  through (before the runtime took 1, a restart from 4x to 1x went on at
  4x). Oversampling stops at 768 kHz inside.
- **Filters about to come in run beforehand.** A low-pass switched in
  from a cold state (the high cut, the band's upper crossover) outputs
  nothing at first, a click; each runs while its parameter's target is
  inside its range. Whether they run is decided each frame, never each
  block, so renders do not depend on block size (the feedback loop's too).
- **Rest offset.** Each curve's output less f(bias) (its value with no
  signal), at once: a bias otherwise steps from 0 on the first sample and
  rings for seconds in the feedback loop. Tape seeds its direction memory
  from the first sample for the same reason.
- **Feedback** is scaled by 1 / the curve's steepest slope over ±1 about
  the bias (fuzz has a gain of 4; crush's steps count as tanh's), so the
  loop's gain stays under 1, and lets go over 200 ms once the input has
  been digitally silent for a second: without it, fuzz held a ring at
  −13 dB and crush a quantisation limit cycle at −62 dB forever.
- **Auto gain** is a table over drive, rebuilt in `update()` when style,
  character or bias change, read per sample at the modulated drive; it
  holds the reference level, not the program's (no level riding).
- **Readouts.** `thd` and `even` come from a probe (a 100 Hz sine through
  a curve of its own at the base rate, harmonics by correlation over 20
  cycles); `distortion` from running moments of the curve's input and
  output. Both are meters, never audio.
- **Tests.** The suite every stock plugin has, and
  `plugin.teasel.response`: dry (mix 0) a −12 dBFS sine passes at
  0.00 dB, and −6.00 with output −6; `soft` at −12 dB with auto gain
  keeps 0.00; `clip` without auto gain below its ceiling is the drive
  (+6.00); driving only the side leaves a centred sine at 0.00, and a band
  above 2 kHz leaves 100 Hz at 0.00 under 24 dB of drive; dry at 8x with
  linear phase a 10 kHz sine passes at 0.00; and the `thd` readout is
  within 0.1 % of `dsp.test`'s own THD (its `meters`) for tape at 6 dB and
  tube at 12 dB with bias. Host-smoke's
  latency check (quality) caught the 1x restart above. Measurements are in
  `stock-effects.md` §5. Release build, one core, stereo 48 kHz, pink
  noise: 1.9 % at the defaults (4x), 0.9 % at 1x, 3.0 % at 8x, 5.0 % at
  16x; 4.4 % with band, feedback, dynamics, motion, emphasis and a cut on,
  13.8 % with all of that at 16x.

## 8. Gate / expander — Sorrel [`sorrel`] (`stock-effects.md` §3)

A DSP script (ADR-0047), `plugins/sorrel/script/sorrel.cpp`. It
replaced Sluice (`com.suppleaudio.gate`) with a new design. Notes for
maintainers:

- **Two clocks.** The detector and the state run on the key as it
  arrives, a lookahead ahead of the audio. Each sample's target gain goes
  into a line (stored 200 dB up, so the line's first zeros read as closed);
  the gain the audio now playing gets is the largest target of the next
  `attack` + 1 ms (a monotonic-queue peak hold, at most the lookahead) for
  rising, and the current one for falling. That is what lands the opening
  a millisecond before the threshold crossing and starts the fall exactly
  where the state closed.
- **Shapes.** The gate opens along a raised cosine in amplitude (its
  largest step at 1 ms is 0.033 of full scale per sample at 48 kHz);
  Expand rises along a one-pole in amplitude; both fall at a steady rate
  in dB, its speed eased in over 3 ms. A gain the closed range asks for is
  never passed under, and −80 dB (the range's minimum) is −90 dB and then
  zero.
- **Follow** reads the detector's level once a millisecond from a ring of
  2048. Measuring only up to the close crossing reads an abrupt stop as no
  fall at all (the level is still high there), hence the 20 ms after it.
- **Suggestion.** Tried on drum spill, a ringing tom, a sung line over
  hiss, tone bursts and a bass note: splitting the histogram in two
  (Otsu) split a soft passage from a loud one as readily as noise from
  sound, and walking up from the noise's peak to where the counts fall
  found no edge, since decays fill the levels above the noise. Measuring
  the noise's spread from below did not fail on any of them (snare spill
  −39.5, tom −35.5, vocal −34.5, bursts over −70 dBFS noise −48.5, bass
  −43.5 dBFS, each 12 to 14 dB over its floor).
- **Lookahead** is a choice (`off`, 1, 2, 5 ms) with `.restarts()`;
  latency is the lookahead in samples at the running rate.
- **Editor.** The history's out trace is what the gate passes, whatever
  listen plays. The SET pill takes `suggested_threshold`; the gain lane
  maps 0 to −80 dB and fills from the range up.
- **Tests.** The suite every stock plugin has, and
  `plugin.sorrel.response`: a −12 dBFS sine passes at 0.00 dB with and
  without lookahead; under a −6 dB threshold it holds at −40 dB, and at
  range −80 it is silent; Expand at 2:1 measures −6 dB; a 2 kHz key
  high-pass keeps a 100 Hz sine from opening it. The reset check's
  stimulus ends in a quiet quarter so that its S1 (threshold −60) and
  defaults differ for a gate. During development, on the material above:
  every hard onset passed bit-exact through its first 3 ms, and a ghost
  note's rising noise burst no longer lost 4 dB at its onset; with the
  default range the gaps were 40 dB down; the 40 Hz note, a tone wobbling
  ±2 dB around the threshold and the vocal phrases each opened it once;
  Follow closed a tom ringing at 30 dB/s at about 120 dB/s (the clamp) where
  the fixed release fell at 400 dB/s; about 2 % of a core in the player.

## 10. Delay — Bracken [`bracken`] (`stock-effects.md` §6)

A DSP script (ADR-0047), `plugins/bracken/script/bracken.cpp`. It
replaced Echelon (`com.suppleaudio.delay`) with a new design. Notes for
maintainers:

- **Two halves a loop.** Groove needs every second echo moved while the
  pair stays on the beat, which one line cannot do; two lines in a ring,
  each with its own pass, can, and each echo still gets exactly one pass
  of the tone and medium. Linear behaviour is a single line's; only the
  guards differ, so a runaway's two halves can each sit near full scale,
  and the echoes leave through a ceiling.
- **Reads before writes.** Each half reads before the loop writes this
  sample, so a time of T samples reads T back (`Ring::read`, at least 2
  for the cubic); freeze reads whole samples and hands them on untouched,
  which is why a held loop never dulls (float storage, no interpolation,
  no pass).
- **Heads.** One pair of crossfading heads does three things: a pitch
  shift (each sweeps its window), a fade to a new time (each takes it at
  its window's start) and plain reading (both at one place: one read).
  Reversed playback cannot use them: two overlapping backward heads play
  each slice twice, at different times. It has its own heads, one slice
  at a time with a 10 ms crossfade at the joins (`Slices`).
- **The diffusers' delay** (20.2 ms together) is taken off the read, so
  diffusion moves nothing in time and 0 % is the direct read to float
  rounding; under 23.2 ms they cannot fit and the read goes direct.
- **Switches** (medium, cuts at their range ends, freeze, reverse, routing,
  the diffusers' path) glide per sample, decided per frame, so renders do
  not depend on block size; a medium or a reversed pass that falls silent
  is cleared, so it comes back the same however long it was away.
- **The tail** is `setTail()`'s (the language gained it for this plugin,
  with framework 1.25.2): it follows the tempo in `process()`, the time and
  feedback in `update()`.
- **Tests.** The suite every stock plugin has, and
  `plugin.bracken.response`: dry (mix 0) a −12 dBFS sine passes at
  0.00 dB, −6.00 with output −6; one echo 1 ms late, alone, is the sine at
  0.00 dB, and −12.00 ducked by 12 dB while it plays; `echo_time` reads
  375 ms for 1/8 D at 120 BPM and `decay` 2491.4 ms for 250 ms at 50 %.
  Measurements are in `stock-effects.md` §6. Release build, one core,
  stereo 48 kHz, noise: 1.4 % at the defaults; 2.2 % tape with modulation;
  2.0 % reversed; 2.9 % with reverse, shift, diffusion, analog, drive,
  modulation and fades at once.

## 11. Reverb — Ghyll [`ghyll`] (`stock-effects.md` §7)

A DSP script (ADR-0047), `plugins/ghyll/script/ghyll.cpp`. It
replaced Nave (`com.suppleaudio.reverb`) with a new design. Notes for
maintainers:

- **One model, two copies.** `Room` holds everything that follows from the
  parameters (images, per-capsule tap delays and band gains, octave RT60s,
  modes, levels, C80/D50). The audio code keeps one and rebuilds it in
  `update()`; the editor keeps its own and rebuilds it every repaint, so
  what is drawn is exactly what is heard.
- **Energy, not taste.** Levels are physical: the tail's gain comes from
  Sabine's reverberant energy for the distance less the reflections', over
  the lines' impulse energy g²/16(1 − g²); the modes share the tail's
  energy below the crossover by modal density. A change to the lines or
  the injection that alters their energy needs the calibration re-measured
  (an impulse, `late` alone, energy in the 500 Hz and 1 kHz octaves; a
  single-sample impulse loses high-frequency energy at fractional reads,
  which is why the injection and the allpasses read whole samples).
- **Whole samples.** Settled reflections read whole samples (a fraction
  would dull them, up to −4 dB at 16 kHz with linear reads); their targets
  are whole samples, so a glide starts and ends on one.
- **Glides.** The reflections glide 50 ms (and read between samples) for
  0.4 s after a change; the lines' lengths and the allpasses' 150 ms; the
  lines' loss filters step a quarter of the way every 32 samples (a TDF-II
  biquad redesigned at once leaves a transient); freeze, mono and the
  cuts' ends glide per sample, decided per frame, so renders do not depend
  on block size.
- **Tests.** The suite every stock plugin has, and
  `plugin.ghyll.response`: dry (mix 0) a −12 dBFS sine passes at
  0.00 dB, −6.00 with output −6; `distance` reads 10.002 m for two places
  10 m apart on the plan; `rt60` reads Eyring's 5276.2 ms for a 10 m
  concrete cube. Measurements are in `stock-effects.md` §7. Release build,
  one core, stereo 48 kHz, noise: 3.3 % (the same in a booth or a
  cathedral; 2.6 % with `mono`).

## 13. Chorus — Withy [`withy`] (`stock-effects.md` §11)

A DSP script (ADR-0047), `plugins/withy/script/withy.cpp`, with no
predecessor. Notes for maintainers:

- **The interpolator is the sound.** A modulated read crosses samples
  hundreds of times a second, so any interpolator whose level depends on
  where it reads between samples (linear, cubic) amplitude-modulates the
  highs at that rate. The sinc table is built in `prepare()` (no built-in
  needed; ADR-0032's 2026-10-20 rule) and checked in two ways: a fixed
  fractional delay and a delay moving at constant speed, both flat to
  0.005 dB at 12 kHz. Keep the rows summing to 1 (the lows' level).
- **Glides that sit at their target.** Every switch weight steps toward
  its target with `approach()`, which never passes it. An earlier version
  stepped down when it sat at 1, so the weight flipped between 1 and
  1 − step every sample: a ±0.3 dB ripple at 12 kHz with the triangle,
  found only by measuring the highs' envelope.
- **Depth in cents.** The swings are worked out from the depth and the
  rate every 16 frames (continuous, so block size does not matter) and
  glided per sample; the readouts are computed from what the voices do,
  after the 25 ms cap and bloom's speed, not from the knob.
- **Levels.** The voices' normalisation and the style trims were measured
  on pink noise limited to 30 Hz–18 kHz and on notes: pink noise down to
  1 Hz has a third of its power where the voices and the dry are the same
  signal and misleads both the trims and the mix law.
- **The editor's response** is computed from what the audio code sends
  (each voice's delay and side gains, normalisation included) and the
  parameters, with the same mix law and feedback scaling, averaging power
  over each drawn point's band so dense combs read as their depth.
- **Tests.** The suite every stock plugin has, and `plugin.withy.response`:
  dry (mix 0) a −12 dBFS 1 kHz sine passes at 0.00 dB, −6.00 with output
  −6; one voice held still (vibrato, depth 0) at 0.00 dB; a 60 Hz sine
  under a full vibrato with low keep at 1 kHz at 0.00 dB; `detune` reads
  the depth, `sweep` 1.107 ms (sine) and 1.636 ms (triangle) for 12 ct at
  1 Hz, `rate_now` 2 Hz for a quarter note at 120 BPM. Measurements are in
  `stock-effects.md` §11. In the player (release sandbox), stereo 48 kHz,
  noise: 1.5 % at the defaults, 3.9 % with eight voices, 5.0 % with eight
  voices, the bucket brigade, shimmer, drift, low keep and high cut.

## 14. Phaser — Pollia [`pollia`] (`stock-effects.md` §12)

A DSP script (ADR-0047), `plugins/pollia/script/pollia.cpp`. A new
stock plugin with no predecessor. Notes for maintainers:

- **Trapezoidal stages.** Each all-pass is 2·lp − x of a one-pole
  integrator (g = tan(πf/fs)), which keeps its state meaningful while the
  corner moves, so a sweep at 20 Hz or a `steps` jump does not zipper or
  ring. Corners are computed every 8 frames (a `tan` per stage) and each G
  steps linearly between, on a frame counter that only `prepare()` resets,
  so the ticks fall on the same frames at any block size. Stages that
  start (a larger count) are set at once, off the tick; the lamp and the
  stages' glides move on only at ticks.
- **The barber pole is a turn, not a crossfade.** The usual barber pole
  crossfades several sweeping chains, which thins the notches mid-fade.
  Pollia keeps the chain still and turns the wet's phase against the dry's
  through a 90-degree pair, which slides every notch continuously at full
  depth. The pair's corners (`pairCorner`) are a minimax design in the
  warped domain u = tan(πf/fs), scaled to the geometric centre of 10 Hz
  and 20 kHz (or 0.45 fs) at the running rate, so one table serves every
  sample rate; seven sections a path hold 90° ± 0.34°, a −50 dB notch. The
  dry passes the pair's in-phase path too: only the turn may differ
  between the two. The feedback loop stays around the chain, before the
  turn, so `feedback` sounds the same in every motion.
- **Photocell.** The lamp and cell run at the corner ticks: brightness is
  linear in the motion and the conductance's octaves are its log, which
  is what makes the sweep lopsided; the darkening time grows as the cell
  darkens. In rise and fall the motion's depth fades to 0 and the lamp
  rests at the brightness of the centre (2^−range/2), so the stages sit at
  the centre in either response.
- **Feedback's level**: 80 % of the dB the loop adds to white noise
  ((P₀/P)^0.4 in amplitude) is taken back, computed per frame from the smoothed `feedback` and
  `mix`; the editor's response uses the same formula.
- **The editor's history** is rows of 1/24 s (96: 4 s) the audio code
  writes into shared arrays (each side's centre offset, the turn, the barber share,
  the envelope); the editor finds each row's notches once (200 log-spaced
  points, the response computed from the same formulas, minima below
  −8 dB refined by a parabola) and again, 24 rows a paint, newest first,
  when a setting that shapes the response moves.
- **Tests.** The suite every stock plugin has, and
  `plugin.pollia.response`: dry 0.00 dB, −6.00 with output −6; two stages
  at 1 kHz cancel a 1 kHz sine (−188 dB), leave −16.08 dB with feedback 50%
  (the loop's analytic response) and −6.02 with keep below at 1 kHz; the
  wet alone 0.00 dB held still, and the sine's level kept (−12.03 dBFS)
  while the phase turns; `centre_now` 1000 Hz with range 0. Measurements
  are in `stock-effects.md` §12. In the player (JIT), stereo 48 kHz: 1.2 %
  at 4 stages, 1.3 % at the defaults, 3.1 % at 24 stages with spread and
  feedback, 3.6 % with the barber pole, photocell, envelope and keep below
  on top.

## 15. Tape — Umber [`umber`] (`stock-effects.md` §13)

A DSP script (ADR-0047), `plugins/umber/script/umber.cpp`. A new stock
plugin with no predecessor. Notes for maintainers:

- **Fitted, not guessed.** The constants come from small fits kept with
  the script's comments: the squareness and reference field to the
  datasheet's third harmonic against level, the speed trims to its 1% and
  3% points at 30, 15 and 7.5 ips, the highs' ceilings to its saturation
  output at 10 and 16 kHz, the hiss to its A-weighted noise. Changing the
  oxide, the amplifiers or the highs' stage moves them; recheck with
  `plugin.umber.response` and sweeps (dsp.test: 1 kHz THD at reference
  level, −15 dBFS peak, +4, +9, +12.5 dB; the most a 10 and a 16 kHz sine
  get out).
- **The highs saturate on their own.** The first version lifted the highs
  into the oxide (the record curve's zeros), so a loud cymbal saturated
  the whole oxide for an instant and took the mids down with it: on music
  8 dB more distortion between 500 Hz and 4 kHz than a JA model with an AC
  bias carrier at 768 kHz (Ferric), measured band by band against the same
  chain at −24 dB drive. A slope limit after the oxide (the flux's
  gradient) fixed that but aliased badly (its corners: −44 dBc at 4x for
  a 15 kHz sine at −6 dBFS, −18 at 200%). The split (lows and highs from
  bilinear one-poles, which sum exactly; a tanh on the highs only; lows
  minus highs, an allpass) keeps both: the mids never meet the highs'
  curve, and a tanh's harmonics fall fast enough (−88 dBc at 4x). Depth
  layers (a surface and a deeper half driven by a low-passed field) were
  tried and dropped: within 1 dB once the highs had their own stage.
- **The oxide's step.** Jiles–Atherton's irreversible magnetization is
  stiff when the loop is narrow (the recommended bias leaves k of 1e-5):
  an explicit RK4 diverges. The step relaxes toward the anhysteretic value
  by exp(−|Δh|/k) instead, exact for the curve held over the step, so it
  is stable at any k; past |Δh|/k = 20 it sets the value directly.
- **Amplifiers.** Both offsets have the same sign, so their second
  harmonics add (opposite signs cancelled them to −90 dBc). They are
  oversampled with the tape, the playback amp after the moving read.
- **Glides without clicks.** Every continuous colour glides 30 ms and its
  curves are redesigned every 32 frames. A filter that switches in (the
  head's top low-pass, as the tape's loss crosses 0.45 fs) always runs,
  so it comes in from a live state; azimuth's window and spread glide per
  frame, interpolated across the oversampled substeps like the
  transport's read.
- **Azimuth as the gap hears it.** A low-pass at the sinc's −3 dB point
  skipped small angles (0 dB at 0.1°, the sinc −0.2) and overshot large
  ones (−7.7 dB at 0.5° at 12 kHz, the sinc −4.9); one track read late
  spent the read's reach twice as fast as splitting the spread. Averaging
  8 reads across the window is the sinc to within 0.3 dB to 1°, and costs
  nothing while the azimuth is 0.
- **Low latency.** 9 samples leaves 73 µs of reach at 48 kHz; the
  default wow needs about 120 µs. The wow, then the flutter, are scaled to
  fit 80% of it (the estimate of what they need is within 3% of the
  measured peak for wow, conservative for flutter), never squashed: the
  first version's tanh on the displacement turned deep wow into jerks
  (the flutter band from 0.005 to 0.023% with wow alone).
- **Block invariance.** Everything that moves (the transport's sines,
  sways and random parts, the glides, the meters' refresh, the switch
  fade) counts frames, not blocks; the counter and phases reset only in
  `prepare()`. The switch clears every stage in the frame the output
  reaches silence and feeds that frame nothing.
- **Cost.** The oversampled loop (record amp, trim, the standard's lows,
  the oxide, the highs' stage, the playback lows, the moving read, the
  playback amp) is most of it, so the cost goes with `oversampling`: in
  the dev player about 3.4% of a core at 2x, 6% at 4x, 10% at 8x, 18% at
  16x (stereo, 48 kHz).
- **Tests.** The suite every stock plugin has, and
  `plugin.umber.response`: quiet sines at 1 and 10 kHz 0.00 dB, output
  −6 dB, the drive's +6 dB without auto gain, the head bump +1.93 dB at
  60 Hz and 0 with `head_bump` 0, the VU at 0 for a sine at reference
  level, the third harmonic about 0.1% there and 3% at +12.5 dB, and a
  10 kHz sine 12 dB over reference down about 3 dB (the highs' ceiling).

## 16. Tuner — Sedge [`sedge`] (`stock-effects.md` §14)

A DSP script (ADR-0047), `plugins/sedge/script/sedge.cpp`. A new stock
plugin with no predecessor. Notes for maintainers:

- **Kept simple on purpose.** An earlier tuner (Lodestone, never shipped)
  wandered on plucked strings and had a polyphonic mode that did not work
  well; the user asked for one that behaves as REAPER's ReaTune does: one
  detector, a window to trade answering soon for listening long, and the
  display holding while nothing is pitched. No onset detection, median
  filters or adaptive smoothing. Three things were added only once tests
  showed the need: the fundamental's low-pass before the fine stage (the
  overtones made every note read sharp and sag), the second agreeing
  reading before a new note shows (without it a string with a weak
  fundamental flashed octave errors), and the test that the sound still
  sounds at the window's end (without it the note held after a stop was
  off by up to 7 cents). That test measures whole periods: an eighth of a
  40 ms window is shorter than a period of B0, and a slice that short is
  quiet or loud by its phase (B0 took 1.4 s to show). Add machinery only
  for a failure seen on real or test strings.
- **It writes the block only to mute.** `PRODUCT_TRANSPARENT` makes
  host-smoke demand output equal to input with Mute off; the mute ramp
  touches `io` only while it is below 1.
- **The work is spread.** A reading runs as steps (copy, one coarse lag,
  pick, copy, filter, the fine lags), each frame doing up to a share sized
  so a reading finishes in about half a hop, and none more than about
  50 000 loop turns: the player's budget is 65 536 a frame, and a block may
  be one frame. The copy runs in chunks of 8192 for 192 kHz.
- **Readings are frame-counted.** Hops and steps count frames, not blocks,
  so the readouts are the same at any block size; `report()` runs at the
  end of each block from what is shown.
- **The editor reads `Shared` values**, not the readouts, which reach
  hosts at their meter cadence; the shared ones arrive about 120 times a
  second.
- **Tests.** The suite every stock plugin has, and
  `plugin.sedge.response`: a 445 Hz sine passes at 0.0 dB and reads A4
  +19.55 cents at 445.00 Hz; an equal-tempered E4 under Werckmeister III on
  C reads −1.98 cents (the temperament's E is 1.955 above equal with A at
  0). Plucked-string measurements (§14 of the spec) were made with
  synthetic strings in a scratch harness, not kept as tests.

## 17. Gain — Gain [`gain`] (`stock-effects.md` §15)

A DSP script (ADR-0047), `plugins/gain/script/gain.cpp`. A new stock
plugin with no predecessor, the simplest of them. Notes for maintainers:

- **The glide is the framework's**, not the script's own: `.smoothed(20)`
  gives `gain.at(i)` per sample, snapped on the first block after
  `reset()` and started at each automation event's sample, so the block,
  state and reset checks hold with no code in the script. The linear gain
  is recomputed only while the glide moves.
- **It writes the block only while the gain is not 1**, so 0 dB is a
  passthrough bit for bit (the REAPER check's default matches bypass).
  It is not a `PRODUCT_TRANSPARENT` product: host-smoke moves its one
  parameter off 0 dB.
- **Tests.** The suite every stock plugin has, and `plugin.gain.response`:
  a sine's gain reads exactly 0, +6, +6.02, −12.5, +32 and −32 dB (to the
  hundredth `dsp.test` reports), and a
  jump from −32 to +32 dB on a 50 Hz sine at its peak stepping the output
  no more than 0.005 between two samples (sent straight through, about
  0.6).

## 18. Amp and cabinet — Hornbeam [`hornbeam`] (`stock-effects.md` §16)

A DSP script (ADR-0047), `plugins/hornbeam/script/hornbeam.cpp`. A new stock
plugin with no predecessor. Notes for maintainers:

- **No recorded data, by rule.** The cabinet is a physical model and must
  stay one: no impulse responses, convolution or measured curves, and
  nothing taken from other cabinet simulators (Airwindows' Cabs2 was a
  reference for the kind of plugin only; none of its code or constants is
  used). The tone stack's coefficients (`stackAnalog`) are the nodal
  analysis of the circuit with the source and load resistances, solved
  symbolically and kept as the expanded polynomials in the pots' shares; the
  part values per voice are the circuit's, not fitted.
- **The plot is the audio.** `cabAt` (and `loadAt` for the amp alone)
  evaluates exactly the filters `front()` and `hear()` run: the coil's
  first-order admittance and the air shelf at their bilinear maps, each
  mode's state-variable filter at its prewarped frequency, the back EMF a
  sample late, each tap's delay with its linear interpolation, the far
  sources' low-passes and the capsule's sections. Change one side and the
  other with it: an impulse through the cabinet alone (`chain` `cab`, −40
  dBFS) matched the traced `cabAt` within 0.001 dB at 8 frequencies, three
  settings, 44.1, 48 and 96 kHz (a scratch check, `trace()` since removed).
  The editor draws the cone and the mic from the same `Model` and `Taps`.
- **The level trims are fitted.** `Voice::trim` (dB at gain 0, 25 .. 100 %)
  makes up what the level table's static model misses. They were fitted on a
  synthetic guitar DI (Karplus-Strong strings with a pickup's resonance, a
  pentatonic riff peaking at −6 dBFS, −22 dBFS RMS) through `chain` `amp` at
  `master` 30, to equal levels across voices and gain; refit them (and
  `kOutFull`, `kOutAmp`, the factory patches' `output`) after changing a
  stage, a curve or the level table, then check strummed chords too.
- **Cost.** At the defaults 13.7 refs in the release build (`clap-bench`;
  Teasel 15.7 the same run). It got there by running the first stage's
  filters (before any curve) and the output transformer (its harmonics are
  the bass's) at the session's rate, the supply's follower once a frame, one
  fourth root for the pair, three distance bins a ring and two taps per
  neighbouring speaker, skipping sources and capsule sections with nothing
  to do, and by `channels` `mono` (one amp on the left input) as the
  default: stereo processing, an amp per side, costs 25.0 at the defaults.
  Under constant automation of the mic (29.0 refs, Teasel 27.7) the
  paths and the cabinet's normalization are redone every 128 frames of a
  glide.
- **The speaker's nonlinearity runs at the session's rate**: its force
  terms follow the coil's excursion, which the moving mass low-passes, so
  what it adds is the bass's harmonics and sidebands near what is already
  there; on a low E at full scale its aliases stayed at the measurement's
  floor (−97.7 dBc) while its THD reached 3.8 %.
- **Words in the editor are drawn by `words()`**, a switch on a group and an
  index, written while a local `Array<const char*, N>` initialised from
  string literals crashed the script compiler (since fixed); it stays as it
  is.
- **Tests.** The suite every stock plugin has, and
  `plugin.hornbeam.response`: the cabinet alone (closed 4x12, vintage,
  dynamic 2.5 cm at 30 %) at 100 Hz, 1, 3 and 8 kHz to its model's values
  and below 0.01 % THD at −30 dBFS; `mix` 0 passes the input at 0.0 dB;
  crunch's distortion under 5 % at a quarter of the gain and over 15 % at
  three quarters.

## 6. Engine and benchmark use

Once built, the plugins are in the build's `plugins/clap/`; the engine
finds them when its CLAP search path includes that directory. B1 proper
uses the EQ and the compressor on every track and the compressor as a peak
catcher on the master (docs/benchmarks/); the MCP arrangement acceptance test gains a mixing pass
with them in a later round.
