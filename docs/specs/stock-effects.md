# Spec: stock effects

**Status:** Implemented. Each plugin's section records its decisions;
`docs/specs/effects.md` holds the implementation notes.
**Common rules:** ADR-0010 (semantic params, value domains), ADR-0011
(template instantiation), ADR-0047 (stock plugins written as DSP scripts),
`docs/specs/core-params.md`, `core-patch.md`,
`semantic-params-extension.md`.

## 0. Conventions shared by every stock effect

- **Enum keys** start with a lowercase letter (core-params validation):
  numeric choices take a unit prefix — `db12`, `ms5`, `x4` — while their
  display labels keep the numbers ("12 dB/oct", "5 ms", "4x"). Tables below
  that still show digit-led keys mean these prefixed forms. A scripted stock
  plugin names them with `.keys({...})` (ADR-0047).
- **Patch form:** flat (core-patch `FlatPatchType`): `"params": {"<key>":
  value}` with dotted keys (`band2.freq`). One form for every effect, so an
  agent learns it once. Keys and units are the parameter table.
- **Names:** the template plugins carry craft nouns; their scripted
  replacements are named after plants and the natural world (ADR-0047).
  Code identifiers use the functional id in brackets. CLAP ids
  `com.suppleaudio.<id>`.
- **Look (scripted stock plugins):** the scripts' palette, warm and earthy
  and distinct from the host's graphite: walnut panels lit a little from
  above, a soil-dark graph floor, parchment text, clay (`Theme::accent`) and
  sage (`Theme::accent2`); the name and panel headings in Fraunces
  (`serifFont`, asides in `serifItalicFont`), labels in Inter, numbers in
  Plex Mono. Colours a plugin adds (band colours, curves) keep to the same
  earth and meadow tones.
- **Every effect:** mono/stereo configs (plus sidechain input where
  listed), float/double, sample-accurate automation, block-size invariant
  (null −120 dBFS across 1..4096), denormal-safe, deterministic (seeded where
  random), latency reported exactly (with compensated bypass), `mix`
  (dry/wet, latency-aligned) and `output` (dB) where meaningful, a
  `describe()` sentence, RT-safe under rtsan, CLAP validator clean, loads
  and automates in REAPER (`tools/reaper-check`).
- **Latency-changing parameters** (lookahead, oversampling, linear phase)
  are stepped, not automatable, and follow the template's restart contract
  (`request_restart`, reactivate with the new latency).
- **Non-finite inputs:** Bracken, Ghyll, Pollia, Umber and Withy replace NaN and infinities
  with zero before their detectors, filters and delay lines, so one bad
  sample cannot poison subsequent audio. `stock-nonfinite.tests` checks
  mono and stereo against the same input with those samples zeroed.
- **`reset()` clears processing state and keeps the parameter values** (CLAP's
  contract). The sandbox and the engine reuse one instance across renders and
  across seeks, so a render must depend only on the state set for it:
  smoothers snap to their targets on the first block after `reset()`,
  delay/time lines start at their value, and nothing of the previous render
  survives. `clap-reset-check` (`plugins/common/tools/clap_reset_check.cpp`,
  registered as `plugin.<id>.reset` for every stock plugin) renders
  state S1, then S2 after `reset()`, and requires that S2 render to be
  bit-identical to a fresh instance holding S2 — at block 256 and again at 64.
  Its stimulus is a 220 Hz tone with a noise burst, the last quarter 40 dB
  down, so a gate or an expander has a quiet passage to act on. Scripted
  stock plugins (ADR-0047) also run it with `--stepped`
  (`plugin.<name>.reset-stepped`): every switch and choice moved off its
  default in both states, so a plugin whose default is a passthrough still
  shows its processing state; a switch that silences the plugin by itself
  (a mute) stays at its default, since the reference must carry signal. Finding which
  switches mute the plugin costs one fresh instance and a second of audio per switch or choice
  (Pollard 82, Marram 62), so the verdict for each is made once and kept for the second block
  size; with the two block sizes that is most of what the run costs, and a Pollard or Marram run
  takes about eight seconds in dev, about ten times that under `tsan`, whose tests get a
  600 s limit (`plugin.<name>.reset*` otherwise 180 s).
- **Block-size invariance under automation.** The real-time engine matches
  the offline render only for plugins whose output does not depend on the
  host's block (`docs/specs/engine.md`). `clap-block-check`
  (`plugins/common/tools/clap_block_check.cpp`, registered as
  `plugin.<name>.blocks` for every scripted stock plugin) renders two
  seconds of noise under a tone with 400 seeded jumps of the writable,
  automatable parameters, as sample-accurate events (and seeded notes for
  an instrument), once in blocks of 4096 and once in blocks of 1, 7, 64,
  513, 2 and 1000 frames, and requires the two to be bit-identical. A
  run-time product has no `core-tests`, whose test of the same claim reads
  the parameter table at compile time.
- **Damaged saved state.** A host hands a plugin whatever bytes a project
  or preset holds. `clap-state-check`
  (`plugins/common/tools/clap_state_check.cpp`, registered as
  `plugin.<name>.state` for every scripted stock plugin) saves the
  plugin's defaults, two seeded random settings and up to four of its
  factory patches, damages each 600 ways from a fixed seed (bits flipped,
  cut short, bytes inserted, awkward lengths, runs repeated) and loads
  them all. A refused load must leave the state as it was; an accepted one
  must save, load into a fresh instance and save again byte for byte, then
  process finite audio. Under the sanitizer presets nothing may be
  reported. A run-time product has no template `state-fuzz`, which builds
  its states from the parameter table at compile time.
- **Readonly telemetry parameters** (gain reduction, levels, readouts) exist
  for generic hosts and agents (`fx.meters`). `clap-reset-check` compares
  them too: after `reset()` a plugin's read-only values at the end of a
  render must equal a fresh instance's, and for a plugin that only listens
  (Lichen, whose audio is the same in any state) that comparison is the
  check.
- **Tests beyond the template's:** frequency/phase response against analytic
  targets, THD/aliasing measurements where nonlinear, envelope timing
  against spec, sidechain behaviour, parameter-sweep automation stress, and
  golden renders (tolerance-aware null) for every factory preset.

## 1. EQ — "Yarrow" [`yarrow`]

An eight-band equaliser with zero latency, written as a DSP script
(ADR-0047: `plugins/yarrow/script/yarrow.cpp`). Built for the
analysis-driven workflow ("cut 250 Hz by 3 dB, Q 1.2" is one merge patch)
and for hands: dynamic bands, mid/side placement, band listening, gain
scaling, and a spectrum analyser that favours bass resolution.

| key | type | range | curve | default |
|---|---|---|---|---|
| `bandN.enabled` (N = 1..8) | bool | | | off (a fresh instance is flat) |
| `bandN.type` | enum | `bell`, `low_shelf`, `high_shelf`, `low_cut`, `high_cut`, `notch`, `tilt` | | 1 `low_cut`, 2 `low_shelf`, 3–6 `bell`, 7 `high_shelf`, 8 `high_cut` |
| `bandN.freq` | Hz | 20..20000 (designed at most 0.45 fs) | log | 30, 100, 250, 800, 2500, 6000, 10000, 18000 |
| `bandN.gain` | dB | −30..+30 (bells, shelves, tilt) | linear | 0 |
| `bandN.q` | none | 0.1..30 (shelves use 0.25..2, tilt 0.25..1) | log | 0.71 (shelves, cuts), 1 (bells) |
| `bandN.slope` | enum | `db6`, `db12`, `db18`, `db24`, `db36`, `db48` (cuts) | | `db24` |
| `bandN.channel` | enum | `stereo`, `mid`, `side`, `left`, `right` | | `stereo` |
| `bandN.dynamic` | bool | bells and shelves only | | off |
| `bandN.threshold` | dB | −60..0 | linear | −30 |
| `scale` | % | 0..200: every bell, shelf and tilt gain times this | linear | 100 |
| `auto_gain` | bool | output offset by the static curve's average effect | | off |
| `output` | dB | −24..+24, smoothed 20 ms | linear | 0 |
| `listen` | int | 0..8: plays only band N's region; not automatable | | 0 |

DSP: every section is a second-order (or first-order) filter in transposed
direct form II, in double precision. Poles sit where the analog prototype's
are (impulse invariance); zeros are fitted to the prototype's magnitude,
exact at DC and at the band's frequency (a shelf's zero frequency below
0.6·Nyquist), with the Nyquist term chosen by least squares of the relative
power error at 16 log-spaced frequencies from a quarter of that frequency
up. Bells use the RBJ analog prototype (boost and cut mirror each other); a
cut, and a shelf designed past its corner, is the opposite design inverted,
so poles always lie below the corner. Cuts are matched Butterworth
cascades (odd orders end with a matched first-order section); `q` scales
the most resonant section (0.71 is Butterworth). Notch: poles from `q`,
zeros on the unit circle, unity gain at DC. Tilt: low shelf −gain/2 and
high shelf +gain/2 at `freq`. A band's section count is fixed by its type
and slope, and a gain type at 0 dB is sections that pass the input exactly.
Reported latency is zero. Measured against the analog prototypes through
20 kHz at 44.1 and 48 kHz (25 centres 10 Hz–10 kHz, gains in 3 dB steps,
2049 log-spaced probes): bells with Q 0.3..4 within 0.35 dB at ±12 dB and
0.54 dB at ±30 dB; shelves (Q 0.25..1) within 0.19 dB at ±12 dB and 0.42 dB
at ±30 dB. Wide bells above 5 kHz are the worst case; resonant shelves are
limited to Q 2, where the fit stays within 0.95 dB at ±30 dB.

Every 16 samples, on a grid kept across host blocks, frequencies glide in
log frequency, gains in dB and Q in log over 25 ms, and a band redesigns
only while something moves. Turning a band on or off, or changing its type,
slope or channel, fades the band out over 12 ms, switches and clears its
state, and fades it back in.

Dynamic bands follow the level of their own region at their input (a
band-pass at the band's frequency and Q for bells, the low- or high-pass
side of a shelf), detected per channel as the band is placed (both stereo
channels linked), with a 3 ms attack and a 90 ms release (both longer
below 100 and 200 Hz). Above the threshold the band moves toward its gain
by three quarters of the overshoot, with a 6 dB soft knee, never past its
gain; below it the band rests at 0 dB.

Auto gain offsets the output by the negative of the curve's mean power gain
across 30 Hz–16 kHz (40 log-spaced points, weighted toward the midrange by
two second-order roll-offs at 90 Hz and 7 kHz), with dynamic bands at rest,
within ±18 dB, gliding over 150 ms. It is a static estimate, not a loudness
match.

Listen plays the band's region: a band-pass at its frequency and Q for
bells, notches and tilts, the low-pass side for low shelves and low cuts
(what a low cut removes), the high-pass side for high shelves and high cuts,
faded in and out over 12 ms.

Editor (1000 × 624, the script's own drawing; band colours clay,
marigold, ochre, moss, sage, dusk, heather and madder; a yarrow head beside
the name): the response graph over
most of the window, log 20 Hz–20 kHz with a ±6/12/24/30 dB scale; the summed
curve (every band that is on, whatever channel it works on, plus the
output gain) over the output's spectrum; numbered nodes for the eight bands
(switched-off bands faint, still draggable); the selected band shaded; a
dynamic band drawn at full reach with a ring where its gain is now; a
channel badge under mid, side, left and right bands; input and output peak
meters; a panel for the selected band (switch, listen, type, channel,
frequency, gain, Q, slope, dynamics with a live detector meter and
threshold) and one for the output (gain scale, output, auto gain, peak).
Drag a node for frequency and gain (Q for cuts and notches); wheel for Q
(Alt: slope for cuts); Shift for fine moves; Ctrl snaps to semitones (A4 =
440 Hz); Alt-drag listens to the band; double-click a node to switch it off,
empty graph to add a band there (a cut below 32 Hz and above 15 kHz, a bell
between). Hovering reads out frequency, note and cents.

The analyser runs in the editor, never on the audio thread: the audio code
keeps the newest 32768 samples of the output (mono) in a shared ring; at
most every 40 ms of new audio the editor transforms them (Hann window,
1.46 Hz bins at 48 kHz), smooths each of 420 log-spaced points over a
sixteenth of an octave where bins are dense and interpolates in dB (cubic)
where they are sparse, averages power over about half a second, rounds peaks
with a five-point binomial kernel across neighbouring points, tilts 4.5 dB
per octave about 1 kHz, and eases the drawn line toward that (rising with a
50 ms and falling with a 120 ms time constant). It is drawn as one smooth
line (Catmull-Rom through the points), unfilled, from −96 dB at the floor to
0 dB at the top; silence lies along the floor.

Factory patches: Flat, Clean Up, Vocal Polish (auto gain), De-Ess, Air and
Weight (auto gain), Mono Bass (side cut), Telephone.

## 2. Compressor — "Sundew" [`sundew`]

A compressor with six characters and zero latency unless lookahead is on,
written as a DSP script (ADR-0047: `plugins/sundew/script/sundew.cpp`).
A character is a whole gain cell (detector, law and ballistics), not a
preset; ratio, attack and release mean the same in each. Main stereo or mono
in and out, and a key input as wide as the main bus.

| key | type | range | curve | default |
|---|---|---|---|---|
| `character` | enum | `clean`, `glue`, `opto`, `fet`, `vari_mu`, `bloom` | | `clean` |
| `threshold` | dB | −60..0 (Bloom: the level it lifts below) | linear | −18 |
| `ratio` | ratio | 1..20 (Vari-Mu: the ratio it grows toward) | log | 3 |
| `attack` | ms | 0.05..250 | log | 10 |
| `release` | ms | 5..3000 (ignored while `sync` is on) | log | 150 |
| `sync` | enum | `off`, `n32`, `n16`, `n8`, `n4`, `n2`, `bar` | | `off` |
| `knee` | dB | 0..24, full width | linear | 6 |
| `range` | dB | 1..40: the deepest cut, or Bloom's highest lift | linear | 24 |
| `key` | enum | `internal`, `external` | | `internal` |
| `key_hpf` | Hz | 20..800, 12 dB/oct on the key; 20 is off | log | 20 |
| `link` | % | 0..100 | linear | 100 |
| `stereo` | enum | `lr`, `ms` | | `lr` |
| `lookahead` | enum | `off`, `ms1`, `ms2`, `ms5`; restarts, not automatable | | `off` |
| `listen` | enum | `off`, `key`, `delta`; not automatable | | `off` |
| `colour` | % | 0..100 | linear | 0 |
| `output` | dB | −24..+24, smoothed 20 ms | linear | 0 |
| `mix` | % | 0..100, smoothed 20 ms | linear | 100 |
| `match` | bool | | | off |
| `gain_change` | dB | readonly, −48..+24: the largest change in the last block | | |
| `loudness_change` | dB | readonly, −24..+24: output against input, before the match | | |

Detector. The key is the input, or the sidechain when `external` and the
host reports it connected (otherwise the input), high-passed when
`key_hpf` is above 20 Hz, in left/right or mid/side (mid = (L+R)/2, side =
(L−R)/2) as `stereo` says. Each channel's level is in dB: the sample's
magnitude (Clean, FET) or a mean square with a one-pole of 3 ms (Glue),
5 ms (Opto), 8 ms (Vari-Mu) or 12 ms (Bloom), plus 3.01 dB so a sine reads
its peak. Link: each channel's level moves toward the louder by `link`.
The audio (not the key) is delayed by the lookahead, which is the reported
latency.

Law: an amount in dB, 0 or more, from the level L, threshold T, ratio R
and a soft knee (the quadratic corner of max(0, d) over the knee's width;
Glue adds 3 dB to it, Opto 10, Vari-Mu 14, FET halves it), rounded into
`range` over up to 3 dB. Feedforward: (1 − 1/R)·over. Feedback (Opto,
FET): (R − 1)·over, where L is the output's level, so the static ratio is R
as for the others; FET multiplies it by 1 + over/12. Vari-Mu: the ratio at
an overshoot is 1 + (R − 1)(0.25 + 0.75(1 − e^(−over/9))). Bloom lifts:
(1 − 1/R)·under, faded to nothing over 24 dB below 30 dB under the
threshold, so noise and silence are not raised.

Ballistics, on the amount, with one-poles of the given time constant:

- Clean: a decoupled peak detector: the amount held and released with
  `release`, then smoothed with `attack`, so a steady tone gets a steady gain.
- Glue: attack and release, and a slow stage (350 ms up, 6 × release
  down, at most 8 s) whose 75 % holds the amount when compression lasted:
  short peaks recover with `release`, sustained passages slowly.
- Opto: feedback, solved for the output it makes each sample (three Newton
  steps on the implicit one-pole), so a fast loop never rings. Attack scales
  from 1.5× (small steps) to 0.5× (12 dB and more); a memory stage (600 ms
  up, 12 × release down) holds 60 % of the reduction: a fast drop, then a
  slow tail, longer after long compression.
- FET: feedback, solved as Opto; release quickens with depth
  (release / (1 + amount/8)).
- Vari-Mu: two stages in series at 0.47 × attack and release: S-shaped
  movement.
- Bloom: attack is how fast the lift gives way to a louder sound, release
  how fast it returns.

`sync` sets the release to a note value at the host tempo (a bar from the
time signature): a third of the note's length, so 95 % of the way back
within it. A change of character keeps the gain where it was: the new cell
starts from the old amount, or, between cutting and lifting, at rest with a
30 ms glide from the old gain.

Colour adds the character's second and third harmonics to the compressed
signal (even/odd coefficients: Glue 0.02/−0.10, Opto 0.06/−0.03, FET
0.05/−0.16, Vari-Mu 0.11/−0.05; Clean and Bloom none), times `colour` and
0.35 + 0.65·min(1, amount/10 dB), the input to the polynomial limited to
±1.5 and the DC its even part makes removed (8 Hz). Then `mix` against the
delayed dry signal, the match, and `output`. Listen plays the key (delayed
as the audio) or the delta (dry minus compressed, before mix and output).
Every switch crossfades over 10 ms from what is heard to what is chosen;
a switch during a fade starts from the blend heard.

Match measures the dry and the mixed signal's power (one-poles of 600 ms)
and, while the input is above −57 dBFS, moves an offset toward their ratio
(within ±24 dB) over a second; the offset applies when `match` is on and
glides out over 40 ms when it is turned off. It starts at 0 dB.

Editor (1000 × 700, the script's own drawing; a sundew leaf beside the
name; each character has a colour: Clean linen, Glue moss, Opto
marigold, FET clay, Vari-Mu heather, Bloom madder):

- **History.** Four seconds scrolling: the input as a faint line over the
  compressed signal as a filled level (both peak per 10 ms, −48..0 dB),
  the gain change in the character's colour from the top (Bloom's lift from
  the floor) at twice the level scale, bar and beat lines while the
  transport plays, and the threshold, dragged up and down, wheeled, or
  reset by double-click.
- **Transfer.** The steady input/output curve the character makes, with
  unity, the threshold, a dot and its trail where the key is now, the ratio
  and the effective knee. A drag moves threshold across and ratio up and
  down; the wheel changes ratio. Beside it a gain-change bar.
- **Key strip.** Key, key high-pass, link, left/right or mid/side,
  lookahead and listen.
- **Character list.** Each with a small plot of its own ballistics.
- **Dynamics.** Threshold, ratio, attack, release, knee and range; the
  ballistics plot: a hit, a rest, a held note and a rest played through a
  cell of the character's own, each part given its share of the width;
  dragging across a note sets attack, across a rest release; the sync
  choices with the release they give at the tempo.
- **Output.** A moving-coil gain-change meter on a lamp-lit face (−20 to
  +6 dB, a peak tick), the loudness change and what the match adds,
  output, mix and colour (dimmed where the character adds none), the
  match switch; input and output peak meters in the header.

Knobs drag vertically (Shift fine), wheel, and reset on a double click.

Factory patches: Clean Start, Vocal Leveler (Opto, match), Drum Bus Glue
(Glue, key HPF 90 Hz, mix 75 %, match), Snare Bite (FET), Parallel Smash
(FET, mix 30 %), Mix Bus Warmth (Vari-Mu, range 6 dB), Bass Steady
(Opto), Kick Duck (Clean, external key, 1/8 release), Detail Bloom
(Bloom), Mid-Side Master (Glue, mid/side, link 50 %, match), Peak Catcher
(Clean, 1 ms lookahead, 20:1).

Not a limiter: there is no ceiling and no true-peak detection. Clean at
20:1 with lookahead holds sample peaks closely; a ceiling belongs to
Treeline (§10).

## 3. Gate / expander — "Sorrel" [`sorrel`]

A gate that means to leave no trace on what it lets through, written as a
DSP script (ADR-0047: `plugins/sorrel/script/sorrel.cpp`), with a
key input and readouts that say where its threshold belongs. Main stereo or
mono in and out, a stereo "Sidechain" input; latency is the lookahead.

| key | type | range | curve | default |
|---|---|---|---|---|
| `mode` | enum | `gate`, `expand` | | `gate` |
| `threshold` | dBFS | −80..0: the key level above which it opens | linear | −40 |
| `hysteresis` | dB | 0..24: how far under the threshold it closes (gate) | linear | 6 |
| `range` | dB | −80..0: the gain while closed; −80 is silence (−inf) | linear | −40 |
| `ratio` | ratio | 1.2..10 (expand) | log | 2 |
| `attack` | ms | 0.05..50: the opening's length | log | 1 |
| `hold` | ms | 0..1000: open after the level falls under the close level (gate); a falling gain held this long (expand) | cubic | 20 |
| `release` | ms | 10..3000: the time to turn down 60 dB | log | 150 |
| `follow` | bool | close at the sound's own rate of decay (gate) | | true |
| `lookahead` | enum | `off`, `ms1`, `ms2`, `ms5`; a restart, not automatable | | `ms2` |
| `key` | enum | `internal`, `external` (the sidechain, when connected) | | `internal` |
| `key_hpf` | Hz | 20..2000 on the key; 20 is off | log | 20 |
| `key_lpf` | Hz | 100..20000 on the key; 20000 is off | log | 20000 |
| `listen` | enum | `off`, `key`, `delta` (what it takes away); not automatable | | `off` |
| `gain` | dB | readonly, −80..0: the lowest gain of the last block | | |
| `noise_floor` | dBFS | readonly: the commonest key level of the quietest 30 % of the last 10 s | | |
| `loud_level` | dBFS | readonly: the key level exceeded 5 % of the last 10 s | | |
| `suggested` | dBFS | readonly: see below | | |
| `open_time` | % | readonly: the share of the last 10 s it was open | | |
| `openings` | | readonly: how many times it opened in the last 10 s | | |

- **Detector.** The louder channel of the key (12 dB/oct Butterworth
  high- and low-pass when set; the low-pass clamps to 0.45 fs), held at its
  peak over a sliding 10 ms, so no cycle of a 50 Hz note reads as a fall.
  An external key that is not connected falls back to the input.
- **State.** Opens when the level rises above `threshold`; closes once it
  has stayed under `threshold − hysteresis` for `hold`. The state runs in
  both modes: it gives `open_time` and `openings`.
- **Gain.** `gate`: open is 0 dB, closed is `range`. `expand`: each dB
  under the threshold becomes `ratio` dB, with a 6 dB knee, down to
  `range`. Opening (gate) is a raised cosine in amplitude over `attack`;
  in Expand the gain approaches the curve in amplitude, 95 % of the way in
  `attack`. With lookahead the audio is delayed and the gain rises for
  anything the key asks within the next `attack` + 1 ms (at most the
  lookahead): the opening ends a millisecond before the level crosses the
  threshold, so an onset passes whole with its rise, and the noise before
  it stays down. Closing is a steady fall in dB at 60 dB per `release`,
  its speed eased in over 3 ms, to the range; −80 fades to −90 dB, then
  exact zeros. Expand holds a falling gain for `hold` before it falls.
- **Follow** (gate): the fall takes the rate the key's level fell at from
  50 ms before it went under the close level to 20 ms after (or the end of
  a shorter hold), clamped to a quarter to four times the release's rate.
  A sound that stops dead closes fast; a ringing tom fades as it rang.
- **Listen.** `key` plays the filtered key, delayed with the audio;
  `delta` plays input − output, what the gate takes away. Every switch
  crossfades over 10 ms from what is heard to what is chosen; a switch
  during a fade starts from the blend heard.
- **Readouts.** The detector's level each millisecond goes into a
  histogram of 1 dB bins (−96..0 dBFS; digital silence is skipped), kept in
  100 chunks of 100 ms. Every 100 ms: `noise_floor` is the commonest level
  among the quietest 30 % (smoothed over ±2 dB); the noise's peaks reach
  twice as far above it as the quietest 2 % lie below it, and at least
  4 dB; `suggested` puts the close level 2 dB above those peaks
  (`threshold − hysteresis` = peaks + 2), kept 3 dB under `loud_level`. The
  levels between the noise and the sound are full of the sound's own
  decays, so the suggestion is measured from below. All read −96 until
  300 ms of sound have been heard. It assumes quiet stretches exist; on
  material that never pauses it reads the quiet parts as the floor.
- With every sample above the threshold the output is the input, delayed
  by the lookahead, bit for bit.

Editor (1000 × 720): four seconds of what came in, what goes out and, in
rose, what was taken, with the key when it is filtered or external, and
the threshold and close lines (drag either; a press elsewhere sets the
threshold); the gain under it (drag for the range); the histogram of the
key's last ten seconds on the same scale, coloured above the threshold,
in the hysteresis band and under the close level, with the suggestion
marked and SET to take it, and the readouts; a transfer plot with the
gate's open and closed branches and the jumps between them (or Expand's
curve) and a dot for now; a sketch of one sound's gain (the opening
landing before the onset, the hold, the fade, and Follow's span); key and
listen controls. The header's sprig of wood sorrel folds its leaves as the
gate closes.

Factory patches: Clean Start, Snare Spill, Kick Tight, Tom Ring, Vocal
Pauses (expand), Amp Hiss, Full Silence, Keyed Chop (external key).

## 4. Transient shaper — "Gorse" [`gorse`]

Raises or softens each hit's onset and its tail, the same at any level,
written as a DSP script (ADR-0047: `plugins/gorse/script/gorse.cpp`).
Main stereo or mono in and out, one gain for every channel (stereo-linked),
a fixed 1 ms lookahead.

| key | type | range | curve | default |
|---|---|---|---|---|
| `attack` | dB | −12..+12, smoothed 20 ms: the change at a clear onset | linear | 0 |
| `sustain` | dB | −12..+12, smoothed 20 ms: the change through a clear decay | linear | 0 |
| `length` | ms | 1..100, smoothed 20 ms: the detector's time scale | log | 10 |
| `sensitivity` | % | 0..100, smoothed 20 ms | linear | 50 |
| `focus` | enum | `full`, `low`, `mid`, `high`: what the detector hears; not automatable | | `full` |
| `clip` | bool | the soft clip, last of all; automatable (it fades over 10 ms) | | false |
| `mix` | % | 0..100, smoothed 20 ms: shaped against dry, in time | linear | 100 |
| `output` | dB | −24..+24, smoothed 20 ms, before the clip | linear | 0 |
| `gain_change` | dB | readonly, −12..+12: the largest change in the last block | | |
| `onset_change` | dB | readonly, −12..+12: the recent hits' onsets, at the most | | |
| `tail_change` | dB | readonly, −12..+12: the recent hits' tails, on average | | |

Detector. The key is the input, or with `focus` the input through two
biquads: `low` two 150 Hz low-passes, `mid` a 250 Hz high-pass and a 4 kHz
low-pass, `high` two 5 kHz high-passes (Q 0.707 each). Channels take the
louder rectified sample, held over the last H samples (a monotonic queue),
H the longest of:

- **the cycle:** the span between the last two crests that had a dip below
  half of each between them, plus an eighth (1..50 ms). Crests without such
  a dip (a quantized waveform's ripple near its top, a hit's own decay)
  do not count;
- **the low band:** while the key's mono sum through two 200 Hz low-passes
  holds more than 20 % of its power (20 ms means) and rising crossings
  (hysteresis 0.3 × its RMS, 20 to 400 Hz) keep coming within 50 ms, its
  period plus an eighth: a saw or a pulse dips once a cycle;
- **`length`** (at most 50 ms).

So a held tone, a bass line, a voice or a saw reads as a steady level and
gets no gain (under 0.03 dB with attack and sustain at +12 on a 60 Hz +
1 kHz tone, a 55 Hz saw, a 180 Hz voice-like pulse); a chord's beating is
a real movement and moves it a little (1.4 dB at +12/+12).

Followers, in dB of the held level (the floor 120 dB under the loudest
level since reset; at the first sound both start 60 dB under it): fast
(attack `length`/100, release `length`), slow (attack `length`, release
4 × `length`), the slow one never more than 30 dB under the fast one, so a
hit after silence is not treated as longer than one over a bed. With
d = fast − slow and s = `sensitivity`:

- o = d·s/5 above 0 (a 10 dB lead is a full onset at 50 %), 3·d·s/5 below
  (a fall a third as steep is a full tail), clamped to −1..1.
- The gain asked for is `attack`·o while o > 0, −`sustain`·o while o < 0:
  one part at a time, so never more than 12 dB.

Gain path. The gain asked for passes two moving averages whose ramp spans
the lookahead (1 ms, at least 2 samples), and applies to the input delayed
by the lookahead: a change asked for at an onset has fully arrived when the
onset plays, from its first sample, a cut included. Then mix (dry + mix ×
(shaped − dry)), output, and the clip. Latency: the lookahead plus the
clip's oversampler, 52 samples at 48 kHz (48 at 44.1, 100 at 96, 196 at
192), the same with the clip off.

Clip. At four times the rate (the suite's minimum-phase oversampler): straight
to −6 dBFS, then 0.5 + 0.5·tanh((|x| − 0.5)/0.5), never reaching 0 dBFS;
coming down, any sample the filters ring past full scale (a noisy hit
driven 20 dB over) is held at it. Off, the signal takes a plain delay of
the same length; switching crossfades over 10 ms.

Defaults pass the input bit-exactly, delayed by the latency.

The hits (for the editor and the meters): a hit is o reaching 0.5 (an
onset the shaping treats as half of one or more), after it was under 0.1,
at least 20 ms after the last. From the lookahead before it to 500 ms after (or the next hit),
256 points on a square-law time axis (16 for the lookahead, 240 after the
onset) take the peaks of the delayed input and the mixed output and the
mean of o; each hit, in dB from its own peak, joins an average (each new
hit weighs 0.3). `onset_change` is the average's largest change where
o ≥ 0, `tail_change` its mean change where o < 0 up to 300 ms.

Editor (1000 × 700, the script's own drawing; a sprig of gorse beside the
name):

- **The hit.** The average of the recent hits, onset-aligned, on the
  square-law axis (the lookahead, then 0 to 500 ms) and in dB from the
  hit's peak (+12..−48): what came in (a line), what goes out (filled), and
  between them the change, gorse yellow where the onset made it, moss where
  the tail did. A dotted line where the next hit usually comes. Until hits
  play, a model hit (a sharp onset decaying 100 dB a second) through the
  same law. Length's line can be dragged; dragging left of it sets attack,
  right of it sustain (and the wheel); double-click resets. The onset's and
  the tail's change read at the top right.
- **History.** Four seconds (5 ms columns): the input's and the output's
  peak envelopes (+6..−42 dBFS), a dot at each hit the detector found, bar
  and beat lines while the transport plays, and a lane of the gain (±12 dB)
  in the colour of the part that made it.
- **Shape.** Attack and sustain (bipolar knobs), length, sensitivity and
  focus, with what the detector hears.
- **Output.** Mix, output, and the clip's curve with its switch.

Knobs drag vertically (Shift fine), wheel, and reset on a double click.

Factory patches: Punch (+6, −2), Kick Thump (+6, 15 ms, low), Snare Crack
(+7, −3, 6 ms, mid), Tighten (sustain −9, 12 ms, 60 %), Soften Attack (−6,
5 ms), Room Bloom (−2, +6, 30 ms, 70 %), Drum Bus Snap (+4, −2, mix 60 %,
clip).

## 5. Saturation — "Teasel" [`teasel`]

Saturation and distortion from a trace of warmth on a master to folding,
crushing and tuned feedback, written as a DSP script (ADR-0047:
`plugins/teasel/script/teasel.cpp`). Main stereo or mono in and out;
latency is the oversampler's (0 to 5 samples).

| key | type | range | curve | default |
|---|---|---|---|---|
| `style` | enum | `soft`, `tape`, `tube`, `transformer`, `transistor`, `diode`, `clip`, `fuzz`, `fold`, `rectify`, `crush`, `alias` | | `tape` |
| `drive` | dB | −12..48: gain into the curve | linear | 6 |
| `character` | % | 0..100: each curve's own (below) | linear | 30 |
| `bias` | % | −100..100: an offset of up to ±0.5 before the curve | linear | 0 |
| `auto_gain` | bool | hold a −18 dBFS RMS signal's level | | true |
| `dynamics` | % | −100..100: drive follows the attacks (+) or the sustain (−) | linear | 0 |
| `motion` | % | 0..100: the LFO's depth | linear | 0 |
| `motion_rate` | Hz | 0.05..20 (free) | log | 1 |
| `motion_sync` | enum | `free`, `bars4`, `bars2`, `bar1`, `n2`, `n4`, `n8`, `n16`, `n32`, `n4t`, `n8t`, `n16t`, `n4d`, `n8d` | | `free` |
| `motion_shape` | enum | `sine`, `triangle`, `ramp_up`, `ramp_down`, `square`, `random`, `drift` | | `sine` |
| `motion_target` | enum | `drive`, `character`, `bias`, `pitch` | | `drive` |
| `feedback` | % | −95..95 | linear | 0 |
| `feedback_pitch` | Hz | 30..4000 | log | 220 |
| `band_low` | Hz | 20..20000; 20 is off | log | 20 |
| `band_high` | Hz | 20..20000; 20000 is off | log | 20000 |
| `emphasis` | dB | −12..12 | linear | 0 |
| `low_cut` | Hz | 20..2000 on the wet; 20 is off | log | 20 |
| `high_cut` | Hz | 1000..20000 on the wet; 20000 is off | log | 20000 |
| `mix` | % | 0..100 | linear | 100 |
| `output` | dB | −24..24 | linear | 0 |
| `channels` | enum | `stereo`, `mid`, `side` | | `stereo` |
| `quality` | enum | `x1`, `x2`, `x4`, `x8`, `x16`; a restart | | `x4` |
| `phase` | enum | `minimum`, `linear`: the oversampling filters; a restart | | `minimum` |
| `listen` | enum | `off`, `band`, `delta`; not automatable | | `off` |
| `thd` | % | readonly: see the probe | | |
| `even` | % | readonly: see the probe | | |
| `distortion` | % | readonly: see below | | |
| `drive_now` | dB | readonly: the drive this moment | | |
| `input_level`, `output_level` | dBFS | readonly: peaks, falling 20 dB/s | | |

- **Path.** Each channel (left and right, or mid and side) is upsampled
  (the script runtime's half-bands, minimum phase, 3 to 5 samples of
  latency, or with `phase` linear, 65 to 75; `quality` 1x to 16x, but no
  further than 768 kHz inside, so 16x is 8x at 96 kHz) and everything below
  runs at the high rate, the clean bands and the dry signal too, so neither
  mixing nor the band split combs. Linear phase turns no phase at all: dry,
  the output is the input delayed.
- **Band.** Linkwitz-Riley 24 dB/oct crossovers at `band_low` and
  `band_high` (`band_high` at least 1.05 × `band_low`); the low band takes
  the high crossover's all-pass so the three sum flat (0.00 dB, 50 Hz to
  10 kHz, measured). Only the middle band is driven. Filters about to come
  in run beforehand, so none starts cold.
- **Emphasis.** A first-order tilt about 1 kHz, lows −E/2 and highs +E/2
  dB, before the curve and its exact inverse (numerator and denominator
  swapped) after.
- **Curve.** u = g·tilt(x) + feedback; y = f(u + b), b = bias/2, minus
  f(b) (the curve's resting offset, removed at once so a bias neither
  steps nor rings in the feedback), then a 6 Hz DC blocker. alg(x, p) =
  x / (1 + |x|^p)^(1/p). With c the character:
  - soft: lerp(cubic, quintic, c); cubic |x| − 4|x|³/27 to 1 at 1.5,
    quintic |x| − 0.08192|x|⁵ to 1 at 1.25 (odd, only the third harmonic
    below the knee).
  - tape: tanh(u − 0.35·c·d·tanh⁴(u)), d the direction the signal moves
    (smoothed over 0.2 ms): a hysteresis loop near saturation only.
  - tube: triode(u − 2c·q); triode(u) = u/√(1+u²) for u ≥ 0, else
    1.4·alg(u/1.4, 4); q, the grid's charge, rises by 4·max(0, u − 0.3)
    and leaks with a 50 ms time constant (sag; blocking when pushed).
  - transformer: the flux φ = leak·φ + β·x (β = 2π·f_core/rate, f_core =
    40·10^c Hz, leak 4 Hz); y = x + (e − leak·e₋₁)/β with e =
    core(φ + b) − φ − core(b), core(v) = 0.35v + 0.65·alg(v, 2.5): the
    input less the derivative of what saturation took from the flux, so
    the bass saturates first and higher notes pass cleaner.
  - transistor: alg(u, 2 + 10c^1.5), the negative side 0.92·alg(u/0.92, p).
  - diode: 1 − e^(−u) for u ≥ 0, else −t(1 − e^(u/t)), t = 1 + 2c.
  - clip: hard at ±1 with a quadratic knee of width c (exact ceiling).
  - fuzz: a dead zone of 0.04c, then tanh(4u) (0.85·tanh(4u/0.85) below).
  - fold: lerp(sin u, triangle(u), c), the triangle folding at ±1.
  - rectify: u, or −c·u below zero, through 1.5·tanh(·/1.5).
  - crush: tanh(u) rounded to 2^(bits−1) levels, bits = 1 + 15(1 − c)^1.5.
  - alias: tanh(u) held at 20000·0.0125^c Hz (20 kHz to 250 Hz).
- **Style changes** crossfade the old curve into the new (which starts
  afresh) over 20 ms, raised cosine, each with its own auto gain.
- **Feedback.** The curve's output (before auto gain), a period of the
  pitch earlier (at the high rate), through a 6 kHz one-pole low-pass, a
  20 Hz high-pass and tanh, times `feedback` and 1 / the curve's steepest
  slope over ±1 about the bias (fuzz's 4; crush counts as tanh), is added
  to u: the loop's gain stays under 1. Once the input has been digitally
  silent for a second the loop lets go over 200 ms, so a ring (or crush's
  quantisation) never outlasts the music.
- **Dynamics.** A fast (0.3/15 ms) and a slow (25/250 ms) peak follower
  of the louder input channel; the drive moves by `dynamics` × (fast −
  slow in dB, ±24), faded out below −70 dBFS (the slow follower).
- **Motion.** −1..1, at `motion_rate`, or locked to the song's beats while
  it plays (time signature from the host) with `motion_sync`; random holds
  a value per cycle and drift glides between them with a raised cosine,
  each a function of the cycle's number (squares modulo 65521). At 100 %:
  ±24 dB of drive, ±50 % character, ±100 % bias, ±2 octaves of pitch.
  The drive with dynamics and motion is clamped to −36..84 dB.
- **Auto gain.** For the curve as set (without memory: tape as tanh, tube
  without sag, alias without hold, transformer as unity), the gain that
  brings a Gaussian signal of −18 dBFS RMS back to −18 dBFS RMS after the
  curve (its mean removed): 64 points over ±5σ, at each dB of drive from
  −36 to 84, rebuilt when style, character or bias change, read at the
  drive of the moment (dynamics and motion included), glided over 10 ms.
  Transformer holds the mids' level: a saturating bass loses level.
- **Cuts and mix.** `low_cut` and `high_cut` (12 dB/oct) on the driven
  band's output; out = dry + mix × (wet − dry). `listen`: `band` plays the
  driven band's output alone, `delta` mix × (wet − dry); every switch
  crossfades over 10 ms from the blend heard. `channels` changes dip the
  output for 3 ms each way.
- **The probe** (`thd`, `even`): a 100 Hz sine at −18 dBFS RMS through its
  own copy of the curve (base rate, style, drive, character and bias as
  set, without dynamics or motion); every 20 cycles its first 16 harmonics
  by correlation: `thd` = √(1 − P₁/P), P the output's variance (harmonics
  and noise as a share of the RMS, so it stays under 100 % when the
  fundamental vanishes), `even` the even harmonics' share of P₂..P₁₆.
- **`distortion`**: the music's own, from running moments (300 ms) of the
  curve's input u and output y once a base-rate frame: √(1 − cov²/(var u ·
  var y)), the share of y not a scaled copy of u. On a sine it equals the
  probe's THD (measured within 1 % of `dsp.test`'s THD, below).

Measured (dev build, 48 kHz):

| check | result |
|---|---|
| `thd` against `dsp.test`'s THD, 100 Hz at −18 dBFS RMS | soft/tape/tube/diode/transistor at 6 and 18 dB: within 1.5 % of each other (tape 6 dB 1.090 / 1.091 %) |
| auto gain, pink noise at −18 dBFS RMS, drive 0..36 | within 0.4 dB but fold at 36 dB (−3.8: its energy above Nyquist is filtered) and transformer (−1.1 at 12 dB, −4.1 at 24) |
| band 200 Hz–3 kHz, mix 50 %, near-linear | 0.00 dB from 50 Hz to 10 kHz |
| aliasing, 5 kHz at −6 dBFS, 24 dB drive, worst 20 Hz–20 kHz | clip −18 / −44.5 / −70 dBc and fold +3 / −25 / −53 dBc at 1x / 4x / 16x |
| `feedback` 95 %, 30 Hz, drive 48, bias 60 %, every style | silence stays silent; rectify peaks reach +6.7 dBFS |
| an impulse into `feedback` 95 % at 220 Hz, drive 12 | every style below −120 dBFS within 1.5 s |
| automation of style, band, cuts, emphasis, channels | no step above the unautomated render's |
| mix 0 against the input, music, 4x, 20 Hz–20 kHz | the level is flat either way; the residual from the phase: minimum 9.5 dB below the input (34 dB up to 5 kHz, none above), linear 91 dB below |
| latency, minimum / linear phase | 0 / 0 at 1x, 3 / 65 at 2x, 4 / 72 at 4x, 5 / 74 at 8x, 5 / 75 at 16x |

Editor (1080 × 780): the spectrum of the input and the output (16384
points, a twelfth of an octave, tilted 4.5 dB an octave), with what the
curve adds filled in the style's family colour (straw, embers, the
teasel's lilac), the band's edges to drag (between them, both together);
tone and feedback knobs, the pitch as a note; the twelve styles, each
with its curve drawn small; the curve as set over the input's recent
range, with a glow where the music has been (a histogram of half a second)
and its path over the last 20 ms, and faintly the curve at the drive of the
moment; the probe's harmonics, second to twelfth; the readouts; dynamics
with a sketch of one hit and the drive it gets; the LFO's wave with a dot
where it is, its shapes, targets, depth, rate and sync; mix, output, auto
gain, channels, oversampling and its phase, listen, and the latency in
samples and milliseconds. The header's teasel head warms with
what the curve adds and its spines reach with the drive.

Factory patches (the two Master ones with linear phase): Warm Start (the defaults), Bus Tape, Master Sheen, Mid Warmth, Side
Air, Iron Bass, Bass Growl, Drum Crunch, Sustain Fuzz, Master Clipper,
Lo-Fi Sampler, Bitcrushed, Octave Up, Folded Pulse, Ringing Throat, Broken
Radio.

## 6. Delay — "Bracken" [`bracken`]

Echoes from a clean slapback to a smeared, shifting, reversed wash, written
as a DSP script (ADR-0047: `plugins/bracken/script/bracken.cpp`).
Main stereo or mono in and out (mono plays as `mono` routing); no latency.

| key | type | range | curve | default |
|---|---|---|---|---|
| `sync` | enum | `free`, `n32`, `n16t`, `n16`, `n16d`, `n8t`, `n8`, `n8d`, `n4t`, `n4`, `n4d`, `n2t`, `n2`, `n2d`, `n1` | | `n4` |
| `time` | ms | 1..5000: with `sync` free | log | 400 |
| `groove` | % | −50..50: every second echo later (+) or earlier | linear | 0 |
| `offset` | % | −50..50: the right side's times against the left's | linear | 0 |
| `time_changes` | enum | `bend`, `fade` | | `bend` |
| `feedback` | % | 0..110 | linear | 40 |
| `routing` | enum | `stereo`, `ping_pong`, `mono` | | `stereo` |
| `low_cut` | Hz | 20..2000 in the loop; 20 is off | log | 80 |
| `high_cut` | Hz | 1000..20000 in the loop; 20000 is off | log | 9000 |
| `diffusion` | % | 0..100 | linear | 0 |
| `shift` | st | −12..12: each echo's pitch from the one before | linear | 0 |
| `reverse` | bool | each echo reversed | | false |
| `medium` | enum | `clean`, `tape`, `analog` | | `clean` |
| `age` | % | 0..100: the medium's wear | linear | 30 |
| `drive` | dB | 0..24: saturation in the loop | linear | 0 |
| `modulation` | % | 0..100: ±4 ms at 100 | linear | 0 |
| `mod_rate` | Hz | 0.05..10 | log | 0.8 |
| `duck` | dB | 0..24 | linear | 0 |
| `freeze` | bool | hold the loop | | false |
| `width` | % | 0..200: the echoes' | linear | 100 |
| `input` | dB | −48..12 into the echoes; −48 is closed | linear | 0 |
| `mix` | % | 0..100 | linear | 25 |
| `output` | dB | −24..24 | linear | 0 |
| `echo_time` | ms | readonly: the time between echoes now | | |
| `decay` | ms | readonly: see below | | |
| `duck_now` | dB | readonly: the ducking this moment | | |
| `echo_level`, `input_level`, `output_level` | dBFS | readonly: peaks, falling 20 dB/s | | |

- **Time.** T is the note's length at the host's tempo (120 BPM without
  one), or `time`. Each side's loop is two halves in a ring: A = T(1 + g)
  and B = T(1 − g), g = `groove`, so echoes land at A, 2T, 2T + A, 4T…;
  the right side's halves are × (1 + `offset`) in `stereo`, and in
  `ping_pong` B (the bounce back from the right) is. Every half is clamped
  to 1 ms..5 s. `bend`: each half's time follows its target through two
  70 ms one-pole lags (tape changing speed); `fade`: each of the two read
  heads takes the new time at the start of its window, so a change
  crossfades over 30 to 60 ms without a bend.
- **Lines.** Float, each sized for 5 s + 2.5 s (reversed slices) + the
  modulation's reach; reads are 4-point cubic Hermite, at least 2 samples
  back. Two heads half a 60 ms window apart, each sin² over its window (the
  two sum to 1): with no shift and nothing held both read one place, a
  single read. `shift` r = 2^(st/12) sweeps each head through its window
  at 1 − r samples a sample (the window shortens for short times so a head
  never reaches the newest sample).
- **A pass** (each echo, once per half): the diffusers (four allpasses of
  2.31, 3.73, 5.87 and 8.29 ms, gains ±0.72 × `diffusion`, alternating
  sign, the read taken 20.2 ms earlier so the echo stays on time; under
  23.2 ms the read goes direct, crossfading over 20 ms); `low_cut` and
  `high_cut` (12 dB/oct, each faded in and out over 10 ms at its end of the
  range); the medium; drive y = f(gx)/g, g from `drive`, faded in over its
  first dB (0 dB is exactly no drive).
  - clean: a 12 dB/oct low-pass at 20·(1 − 0.6a²) kHz (faded in over age
    0..25 %), rounding to 24 − 13a² bits; curve x/(1 + x⁴)^¼.
  - tape: a one-pole low-pass at 19·(1 − 0.7a^0.8) kHz; curve tanh.
  - analog: a bucket brigade of 10000·(1 − 0.7a) stages clocked at
    stages / 2T: 12 dB/oct low-passes at 0.4 × the clock (at most 18 kHz)
    either side of a hold at the clock rate (none at or above the host's
    rate); curve (tanh(x + 0.3) − tanh 0.3)/(1 − tanh² 0.3), then a 2 Hz
    DC blocker.
  Medium changes crossfade over 30 ms; a medium that falls silent is
  cleared, so it starts afresh.
- **Feedback.** Each half hands on guard(`feedback` × its echo), guard
  untouched to 0.6 and easing to 1.0 above (tanh); A takes the input
  (× `input`) as well. The first echo never passes the guard.
- **Routing.** `stereo`: each side's loop takes its own input and plays
  both halves; `ping_pong`: the left loop takes (L + R)/2, A plays left and
  B right; `mono`: the left loop takes the sum and plays both halves to
  both sides. A change dips the echoes for 8 ms each way; the right loop is
  cleared when a routing that leaves it silent takes over.
- **Reverse.** Each half also reads backwards, in slices as long as its
  time (at most 2.5 s; with a shift, the slice spans its reach at the
  shifted speed), centred on the echo's time, through a pass of its own;
  each slice fades in as the last fades out over 10 ms. The loop itself
  stays forward, so every echo is reversed. Toggling crossfades over 30 ms.
- **Modulation.** Added to every read, at most 45 % of the half's time:
  clean a sine, analog a triangle, tape 0.7 sine + 0.3 a random value per
  cycle glided with a raised cosine, plus age's own wow (0.35 ms at
  0.55 Hz, random) and flutter (0.06 ms at 7.1 Hz); the right side a
  quarter cycle on.
- **Freeze** (20 ms crossfades): the input is shut out and each half reads
  a whole number of samples (its time rounded as freeze began) and hands
  it on untouched, so the held loop repeats exactly; the echoes still pass
  through the tone and medium on their way out. Unfrozen, the halves go on
  from their times now.
- **Out.** Each side's echoes through a ceiling (untouched to 0.8, easing
  to 1.0: two halves held near full scale would sum past it), `width` (mid
  and side, 100 % exactly nothing), ducking, then dry × min(1, 2(1 −
  `mix`)) + echoes × min(1, 2·`mix`), × `output`: the dry stays full to
  50 %.
- **Duck.** The input's peak (1 ms up, 60 ms down), in dB: the echoes go
  down by `duck` × clamp((level + 60)/30, 0, 1) dB, rising in 5 ms and
  letting go over 150 ms.
- **Tail and decay.** A pass's gain at a frequency is the cuts' and the
  medium's (analytic). Each loop (one per side, or the one ping-pong or
  mono loop) alternates two halves, after groove and offset and their
  clamps; a pass takes their mean time at the geometric mean of their
  gains. `decay` is the time the slowest loop's echoes take to fall 60 dB
  at 1 kHz with the feedback as set (drive aside), 600 s when held or past
  the loop's losses. The tail (`setTail`) is the time to fall 80 dB at the
  slowest of 60 Hz, 250 Hz, 1 kHz and 4 kHz in any loop, plus 1.5 × the
  longest half, a reversed slice and the diffusers' ring (0.5 s ×
  `diffusion`): forever when frozen or past the losses. It follows the
  tempo as it changes.

Measured (dev build, 48 kHz):

| check | result |
|---|---|
| a 1 kHz burst, 250 ms, feedback 50 %, cuts off, age 0 | each echo 6.02 dB below the last, on the sample; `decay` 2.491 s |
| `groove` 33.3 %, `ping_pong`, 250 ms | left at 333.25, 833.25 ms; right at 500, 1000 ms |
| a 0.4 s pluck reversed, 500 ms, feedback 50 % | each echo once, swelling to where the pluck began |
| `shift` 12, a 440 Hz tone | echoes at 873, 1740, 3474 Hz (the shifter's 16.7 Hz line grid about 880, 1760, 3520) |
| freeze after 0.8 s (tape, age 60, shift, diffusion, modulation) | the loop's level and centroid unchanged to 0.01 dB over 5 s |
| `feedback` 110 %, drums, each medium | bounded: peaks 0.0 dBFS at drive 0, −20 to −17 dBFS at 24 dB (the curve holds the loop lower) |
| duck 12 dB under a held note | the echoes 12 dB down, back within 0.3 s of its end |
| automation of time (bend, fade), sync, routing, medium, reverse, freeze, cuts, input | no step above the unautomated render's (shift and steep glides raise the material's own) |
| the render after a 1 s sine, 1/16, feedback 30 % | ends 0.37 s after the echoes fall below −80 dBFS |

Editor (1080 × 780): the echoes to come after a sound, as stems along the
time (left up, right down), their height the level, browning as the loop
darkens them, smeared with diffusion, a swell before each when reversed,
each shifted echo's pitch above it; grid lines in beats and bars when
synced; a playhead from the last sound that came in, lighting each echo
as it passes. Dragged across, the time (by octaves free, a note value per
40 units synced), up and down, the feedback. The time as a note or in
ms with the other beside it, the note grid, bend or fade, the time,
groove and offset; the loop's knobs and each echo's tone (the 1st, 2nd,
4th and 8th echo's response); routing and direction; the media, age and
drive with what age does in numbers, the modulation's wave; a strip of the
last 4 s of input, echoes and ducking; the output knobs and freeze. The
header's frond has a leaflet for each echo, lit as the playhead passes,
and browns with age.

Factory patches: Quarter Echo (the defaults), Vocal Slap, Dotted Throw,
Swung Ping-Pong, Wide Double, Tape Echo, Bucket Brigade, Dub Spiral,
Shimmer, Falling Steps, Reverse Swell, Ambient Smear, Early Digital,
Centre Echo.

## 7. Reverb — "Ghyll" [`ghyll`]

A real room built from its walls, written as a DSP script (ADR-0047:
`plugins/ghyll/script/ghyll.cpp`). The reflections, the decay of
each octave, the room's modes and the level all follow from the room's
size, its surfaces and the two places in it. Main stereo or mono in and
out; no latency. The dry signal is the direct sound: at `mix` 50 % the room
is as loud against it as at the listener's place.

| key | type | range | curve | default |
|---|---|---|---|---|
| `room_width`, `room_depth`, `room_height` | m | 1.5..60, 1.5..100, 2..40 | log | 17, 25, 11 |
| `walls`, `floor`, `ceiling` | enum | `stone`, `concrete`, `brick`, `plaster`, `wood`, `glass`, `carpet`, `curtains`, `audience`, `acoustic`, `open` | | `plaster`, `audience`, `wood` |
| `decay` | % | 25..400: the reverberation time against the room's | log | 100 |
| `scatter` | % | 0..100: how rough the surfaces are | linear | 40 |
| `source_x`, `source_y`, `listener_x`, `listener_y` | % | 0..100 across from the left wall, back from the front wall | linear | 44, 22, 54, 44 |
| `mic` | enum | `ortf`, `spaced`, `head`, `mono` | | `ortf` |
| `predelay` | ms | 0..250 | cubic | 0 |
| `early`, `late` | dB | −30..12 against the room's own; −30 is off | linear | 0 |
| `low_cut`, `high_cut` | Hz | 20..1000 (20 off), 1000..20000 (20000 off), on the room | log | 20, 20000 |
| `width` | % | 0..200: the room's | linear | 100 |
| `freeze` | bool | hold the tail | | false |
| `mix` | % | 0..100 | linear | 25 |
| `output` | dB | −24..24 | linear | 0 |
| `rt60`, `rt60_low`, `rt60_high` | ms | readonly: the RT60 at 500 Hz–1 kHz, 125 Hz, 4 kHz | | |
| `distance`, `critical_distance` | m | readonly | | |
| `first_reflection` | ms | readonly: after the direct sound, predelay included | | |
| `clarity` | dB | readonly: C80 | | |
| `definition` | % | readonly: D50 | | |
| `input_level`, `output_level`, `room_level` | dBFS | readonly: peaks, falling 20 dB/s | | |

- **The room.** A box W × D × H; c = 343 m/s. The source stands 1.5 m up,
  the listener's ears 1.7 m (both kept 0.3 m from floor and ceiling and
  0.25 m from the walls); the listener faces the source. Each material
  absorbs a share α of the sound in each octave (125 Hz…4 kHz; 8 kHz takes
  4 kHz's), with in-room values for the hard surfaces (doors, windows and
  fittings take a little more than a laboratory sample):

  | material | 125 | 250 | 500 | 1k | 2k | 4k |
  |---|---|---|---|---|---|---|
  | stone | .03 | .03 | .03 | .035 | .04 | .05 |
  | concrete | .04 | .04 | .04 | .05 | .06 | .08 |
  | brick | .04 | .04 | .045 | .05 | .06 | .08 |
  | plaster | .10 | .07 | .05 | .045 | .045 | .05 |
  | wood | .18 | .12 | .10 | .08 | .07 | .07 |
  | glass | .20 | .08 | .05 | .04 | .03 | .03 |
  | carpet | .02 | .06 | .14 | .37 | .60 | .65 |
  | curtains | .14 | .35 | .55 | .72 | .70 | .65 |
  | audience | .60 | .74 | .88 | .96 | .93 | .85 |
  | acoustic | .50 | .70 | .80 | .90 | .90 | .85 |
  | open | 1 | 1 | 1 | 1 | 1 | 1 |

  The air absorbs 0.44, 1.31, 2.73, 4.66, 9.86, 32.8 and 117 dB/km in the
  octaves 125 Hz…8 kHz (20 °C, 50 %), m = that / 4343 per metre.
- **Decay.** Each octave's RT60 is Eyring's, 0.161 V / A with
  A = (−S ln(1 − ᾱ) + 4mV) / `decay`, ᾱ the area-weighted absorption; so
  `decay` scales every octave's time exactly, and it scales the
  reflections' losses with it (each surface's reflection ^ 1/`decay`, the
  air's / `decay`). The critical distance is √(Q A / 16π), Q = 1.92 (the
  source's directivity, 0.7 + 0.3 cos θ: a voice or an instrument).
- **Reflections.** The 62 mirror images off one, two and three surfaces.
  Each arrives (r − r₀)/c after the direct sound (r₀ source to listener,
  0.5 m at least) plus `predelay`, at r₀/r, × each surface's √(1 − α) per
  band and the air's e^(−m(r − r₀)/2), × the source's pattern along the
  path it left by (the image's direction folded back through its mirrors;
  0.7 + 0.3 cos θ, θ from its direction toward the listener, which it faces),
  × the capsule's pattern from its direction over its pattern toward the
  source (the dry is the direct sound each hears). Three bands: below
  500 Hz (α the mean of 125 and 250), 500 Hz–4 kHz (500, 1k, 2k), above
  (4k; the air the geometric mean of 4 and 8 kHz's), split by one-poles at
  500 Hz and 4 kHz into one line. Scatter s takes a share 1 − (1 − s·k)ⁿ of
  an n-th order path into a blur (three allpasses, gain 0.6, 5, 8.3 and
  13.1 % of the mean free path's time, 13 % longer for the right
  microphone), k = 0.3, 0.75, 1 for the three bands. Settled, every path
  reads a whole sample; when the room changes the paths glide (50 ms) and
  read between samples until 0.4 s after the last change.
- **Microphones.** `ortf`: cardioids 17 cm apart, 55° either side of the
  source; `spaced`: omnis 60 cm apart; `head`: ears 17.5 cm apart, each
  shadowed by a sphere (Brown and Duda's one-pole, one-zero model, its gain
  taken at 200 Hz, 1.3 kHz and 6.5 kHz for the bands); `mono`: one omni.
- **The tail.** Sixteen lines, each a prime number of samples near
  T × 0.62 × 2.6^(k/15) (ranks shuffled), T the mean free path's time
  4V/Sc, at least 20 ms per second of mid-band RT60 (a small, long room
  would ring otherwise) and 8 to 110 ms. A fast Walsh–Hadamard transform
  mixes them (× ¼); each line wanders ±0.25 ms (less in rooms under 20 ms
  of free path) at 0.13 to 0.9 Hz, read by a cubic. Each line loses its
  length's share of each octave's 60 dB through a low shelf (200 Hz), two
  high shelves (2.5 kHz and 7 kHz, the air) and a gain, fitted in three
  rounds to the targets at 125 Hz, 1 kHz, 4 kHz and 8 kHz; a change of
  loss steps the designs a quarter of the way every 32 samples. The input
  (the dry's mid) enters at the first-order reflections' mean time less the
  shortest line, through four allpasses (gain 0.62; 11, 17, 27 and 41 % of
  T). Its level: the reverberant energy for the distance, 16πr₀² / (Q A),
  less the reflections' (an omni's, mid band), a quarter of it at the
  least; a cardioid hears a third of it against its direct sound, a head
  all of it. The lines give an impulse g² / 16(1 − g²) (g² the mid-band
  energy kept per pass of the mean line), scaled to that. The tail's side
  is high-passed at 250 Hz (`ortf`), 150 Hz (`spaced`) or 500 Hz (`head`),
  as the capsules' coherence has it; `mono` folds it to the middle.
- **Modes.** Below the Schroeder frequency 2000 √(RT60₁₂₅ / V), held to
  30..250 Hz and lowered until at most 48 fit, the room's own modes
  replace the tail (Butterworth crossover, 12 dB/oct): each a resonator at
  f = c/2 √((nx/W)² + (ny/D)² + (nz/H)²), decaying at the RT60 there
  weighted by (Σ εᵢAᵢ) / (2 ΣAᵢ), εᵢ 2 along an axis it runs on and 1
  across (an axial mode meets the side walls only at grazing incidence and
  rings longer), heard as cos(nπx/L) at the source × at each capsule over
  the mode's norm, × 1/√(modal density). Scaled so that, averaged over the
  room, they carry the tail's energy below the crossover.
- **Freeze** (50 ms): the input is shut out of the reflections, the tail
  and the modes; the lines' losses fade to none and their wander to
  nothing, so each reads a whole sample and the tail repeats exactly; the
  modes ring without decay.
- **Out.** Reflections × `early` and tail × `late` (each fading to nothing
  over its last 6 dB), `low_cut` and `high_cut` (12 dB/oct, faded in and
  out over 10 ms at their range ends), `width`, then dry × min(1, 2(1 −
  `mix`)) + room × min(1, 2·`mix`), × `output`.
- **Readouts.** The RT60s and distances as above; `clarity` C80 and
  `definition` D50 from the energies an omni hears (the direct sound 1,
  each reflection, the tail as an exponential from its entry).
- **Tail** (`setTail`): `predelay` + the first reflection + 0.2 s + the
  slowest octave's time to fall 80 dB; forever when frozen.

Measured (dev build, 48 kHz):

| check | result |
|---|---|
| the defaults (a 17 × 25 × 11 m hall, 1.29 s at 1 kHz), T30 of the impulse response per octave, 125 Hz…8 kHz | 1.57, 1.43, 1.34, 1.29, 1.28, 1.15, 0.73 s against 1.55, 1.51, 1.37, 1.29, 1.29, 1.17, 0.75 predicted; the same within 3 % at 44.1 and 96 kHz |
| the tail's energy (an omni, reflections off), seven rooms from 3 × 3 × 2.5 m to 30 × 60 × 20 m and decay 30 to 300 % | 0.85 to 1.19 of its target at 500 Hz–1 kHz |
| the tail's spectrum, 0.15–0.6 s: the spread of its dB magnitude about its trend (5.6 dB is a dense random tail) | 5.7 to 6.2 dB in the hall, a tiled bathroom and an echo chamber |
| the left and right tails, `ortf` | correlation 0.04 |
| modes, a 5 × 6 × 2.6 m living room, the 31.5 Hz octave | 24 dB down with the listener in the middle (the first axial modes' nodes), 3 dB up in a corner; the 63 Hz octave 8 dB down at a quarter of the room |
| freeze after 1 s of notes | each band's energy unchanged to 0.3 dB over 6 s |
| automation of every switch, the room's size and places, `decay`, `scatter`, `predelay`, cuts and levels | no step at the change above the unautomated render's; a louder or brighter room raises the material's own |

## 8. Utility — "Hickory" [`hickory`]

The plain stereo tool, written as a DSP script (ADR-0047:
`plugins/hickory/script/hickory.cpp`): level, polarity, routing,
width, balance, bass mono, a DC block and a delay to line tracks up, with
readouts an agent can check a render against. Main stereo or mono in and
out, zero latency. On a mono bus only gain, mute, `invert_left`, the DC
block and the delay apply.

| key | type | range | curve | default |
|---|---|---|---|---|
| `gain` | dB | −60..+24, smoothed 20 ms; −60 is silence (−inf) | linear | 0 |
| `mute` | bool | silence, faded over 10 ms | | false |
| `delay` | ms | 0..50, whole samples; not latency | cubic | 0 |
| `channels` | enum | `stereo`, `swap`, `left`, `right`, `mono`, `side` | | `stereo` |
| `width` | % | 0..200, smoothed 20 ms: the side's level | linear | 100 |
| `balance` | % | −100..100, smoothed 20 ms; positive lowers the left | linear | 0 |
| `invert_left`, `invert_right` | bool | polarity, before the routing | | false |
| `bass_mono` | bool | the side below `mono_below` removed | | false |
| `mono_below` | Hz | 30..500: bass mono's crossover | log | 120 |
| `dc_block` | bool | a 12 dB/oct high-pass at 5 Hz | | false |
| `output_peak` | dBFS | readonly, −120..24: the highest sample peak of the last 3 s | | |
| `correlation` | | readonly, −1..1: left against right over the last 3 s; 0 in silence | | |
| `level_balance` | dB | readonly, −24..24: right over left over the last 3 s | | |
| `side_level` | dB | readonly, −60..12: side over mid over the last 3 s; −60 is mono | | |

Order: polarity → `channels` → DC block → delay → width and bass mono →
balance → gain and mute.

- `channels`: `swap` exchanges the sides; `left` and `right` put one on
  both; `mono` is (L+R)/2 on both and `side` (L−R)/2 on both.
- Width scales S = (L−R)/2 and leaves M = (L+R)/2: 0 % is mono, 200 %
  doubles the side.
- Bass mono: the side through a 4th-order Linkwitz–Riley high-pass
  (two Butterworth sections at `mono_below`); the mid is untouched, so the
  mono sum is exact and there is no latency. At a 150 Hz crossover the
  side at 50 Hz falls 38.3 dB and at 1 kHz is unchanged.
- Balance is linear: L × (1 − max(0, b)), R × (1 + min(0, b)) for b in
  −1..1; centre leaves both at unity.
- Switches (routing, polarity, DC block, bass mono, mute) crossfade over
  10 ms; a delay change crossfades between the two times over 20 ms. All
  are automatable.
- With every control at rest the output is the input, bit for bit
  (non-finite samples included); a non-finite sample never reaches a
  filter's or the delay's state.
- The readouts are sums over 30 chunks of 100 ms, reported each block.
- The tail (`setTail`) is the delay (both times while it crossfades), plus
  750 ms while the DC block is on or fading out and five periods of
  `mono_below` while bass mono applies on a stereo bus; zero at rest.

Editor (900 × 580): an image of where the sound sits (the mid up, the side
across, scaled to the level), with the correlation and the left-right
balance of the last 300 ms under it and a warning while the correlation is
under −0.3; width by frequency: the correlation of each octave from 31 Hz
to 16 kHz, from mono at the top through unrelated to inverted, faded with
its level, with bass mono's crossover dragged there (a press turns it on;
a double-click off); level (gain, delay, mute, DC block) and stereo
(routing, width, balance, polarity, the crossover) panels. A horizontal
drag across the image changes the width.

Factory patches: Unity, Headroom (−6 dB), Mono Check, Side Solo, Bass Mono
(120 Hz), Wider, Mono Bass (140 %, 150 Hz), Narrower (60 %), Flip Right,
DC Clean.

## 9. Analyzer — "Lichen" [`lichen`]

An analyser for people and agents, written as a DSP script (ADR-0047:
`plugins/lichen/script/lichen.cpp`). The audio code reads the block
and never writes it, so every sample leaves as it came (host-smoke checks it
bit for bit as a transparent product, float and double, in place and not).
Main stereo or mono; no latency, no tail. Its measurements are read-only
parameters, so a host, the editor and an agent (`fx.meters`, `dsp.test`'s
`meters`, the sandbox's `param.get`) all read the same values.

| key | type | range | curve | default |
|---|---|---|---|---|
| `target` | dB (LUFS) | −30..−5: the editor's reference | linear | −14 |
| `restart_on_play` | bool | the readings since play restart when play starts | | true |
| `accumulate` | bool | off, the play's readings cover its last 60 s; on, all of it | | false |
| `reset` | bool | any change starts every reading afresh | | false |
| `slope` | enum | `flat`, `db3`, `db4_5`: the tilt the spectrum is drawn with | | `db4_5` |
| `integrated`, `short_term`, `momentary` | LUFS | readonly, −70..10 (−70: nothing) | | |
| `lra` | LU | readonly, 0..40 | | |
| `short_term_max`, `momentary_max` | LUFS | readonly | | |
| `true_peak` | dBTP | readonly, −70..20 | | |
| `sample_peak` | dBFS | readonly | | |
| `plr`, `crest` | dB | readonly, 0..70 | | |
| `overs`, `clipped` | int | readonly counts | | |
| `dc_offset` | dBFS | readonly, −120..0 | | |
| `correlation` | | readonly, −1..1 | | |
| `width`, `low_width` | % | readonly | | |
| `balance` | dB | readonly, −30..30 | | |
| `mono_loss` | LU | readonly, −10..40 | | |
| `centroid` | Hz | readonly | | |
| `tilt` | dB/oct | readonly, −24..24 | | |
| `sub`, `low`, `low_mid`, `mid`, `high_mid`, `presence`, `air` | dB | readonly, −80..0, of the whole | | |
| `key` | enum | readonly: `none`, `c_major` … `b_major`, `c_minor` … `b_minor` (flats `d_flat_major`, sharps `f_sharp_minor`) | | |
| `key_strength` | % | readonly | | |
| `measured` | ms | readonly: the audio the play's readings cover | | |

- **The play.** `momentary` and `short_term` follow the audio now. Every
  other reading covers "the play": from when play starts (when
  `restart_on_play`, otherwise added to the last) to when it stops, after
  which they hold. Without `accumulate` only its last 60 s count: the
  loudness gates are rebuilt each 100 ms from the last 600 blocks, and the
  other sums are kept per second, the oldest dropped as each second ends
  (59 to 60 s of them). Switching `accumulate` starts every reading
  afresh. With the transport never started (a live input, the
  sandbox's `dsp.test`) they keep gathering until `reset` changes. Offline
  renders play the transport, so a render's readings cover the render, up
  to the last 100 ms refresh (the host's meter cadence can lag further).
  Since-play figures hold only the play's own samples: a loudness block
  joins them once its window lies wholly after the restart, while the
  rolling readings (momentary, short-term, the spectrum) carry on.
- **Loudness** is BS.1770-5 / EBU R128 as Treeline measures it: K-weighting
  from the analogue prototypes at any rate, 100 ms energy blocks, momentary
  400 ms, short-term 3 s, integrated gated at −70 LUFS and −10 LU over
  histograms of tenths of a LU, LRA from short-term values gated at −70 and
  −20 LU between the 10th and 95th percentiles. `mono_loss` is the
  integrated loudness less that of the mono fold-down fed to both channels
  (that fold-down's own gated measure, against −70 when it cancels), as the
  offline analysis defines it.
- **Peaks.** The true peak is the signal interpolated at three points between
  each pair of samples (32-tap Kaiser-windowed sinc per phase, β 8) with
  each top refined by a parabola: within 0.003 dB of a sine's peak to
  19.5 kHz at 48 kHz. The interpolation runs only where a sample near it is
  within 6 dB of the highest peak so far or of full scale (an inter-sample
  peak of real audio rises at most about 3 dB above its neighbours).
  `overs` counts each run above 0 dBTP once; `clipped` counts runs of three
  or more samples at |x| ≥ 0.999 on a channel. `crest` is the sample peak
  against the RMS of both channels; `plr` the true peak less integrated.
  `dc_offset` is the larger channel's mean.
- **Stereo** over the play: Pearson's correlation (1 for silence), width as
  side ÷ (mid + side) energy, balance as 10 log10(E_R / E_L): the offline
  analysis's definitions. `low_width` is the same width from the transform's
  mid and side power below 250 Hz.
- **The spectrum.** Two transforms of about a third of a second (16384 at
  44.1 and 48 kHz, 32768 at 96 kHz and above), Hann windowed, a frame every
  quarter of one, give each bin's left, right and cross power, hence mid
  and side. The named bands (sub 20–60 Hz, low 60–250, low mid 250–500, mid
  500–2 k, high mid 2–4 k, presence 4–6 k, air 6–20 k) and the tilt (the
  least-squares slope of the 31 ISO third-octave levels against octaves,
  100 Hz–10 kHz) are the offline analysis's, from the mid's power summed
  over the play's non-silent frames (bins as intervals, fractional at the
  band edges). The centroid is the offline one too: the median over the
  play of each non-silent ~43 ms frame's magnitude centroid (a transform of
  its own on the mid, 2048 at 48 kHz). A frame is silent below −70 dB mean
  square.
- **Harmony.** Each bin from 80 Hz to 2.5 kHz adds its mid magnitude to its
  nearest pitch class, weighted 1 at the semitone's centre falling to 0
  halfway; each non-silent frame's twelve shares count alike. The key is the
  one of 24 whose Albrecht–Shanahan (2013) profile correlates best with the
  play's shares, reported when it correlates at least 0.5, at least five
  pitch classes hold 5 % of the whole and two seconds have played
  (otherwise `none`); `key_strength` is that correlation. On test
  progressions in all 24 keys it named 22; the two misses were a close
  relative (E minor read as C major, G minor as C minor).
- **The editor** (1080 × 800): in the header, the state (listening,
  measuring the play, holding it) and Treeline's clock, which switches
  `accumulate`: unlit, "last 60 s"; lit, the time the play's readings
  cover; Restart on Play and Reset. Loudness against the target (four usual
  targets as chips, the value dragged or wheeled); the last minute of
  short-term and momentary loudness and true peak with the target and the
  loudness range shaded, beside the play's short-term distribution with the
  range bracketed; the spectrum now (mid, filled, and side), its peak and the
  play's average, tilted as `slope` says, with the named bands under their
  stretch and the centroid marked, a hover giving the frequency, note and
  levels; a vectorscope, correlation and balance now (bars) and over the
  play (ticks), width, low width, mono loss and the correlation of each
  third octave; the pitch classes round the circle of fifths with the key
  and the next likeliest two; and a health list (true peak, overs, clipping,
  DC, phase, mono fold-down, bass width) lit by how much each matters.
- **Cost:** about 2 % of a core at 48 kHz on dense material (release build).

## 10. Limiter — "Treeline" [`treeline`]

A true-peak limiter for the end of a mix, written as a DSP script
(ADR-0047: `plugins/treeline/script/treeline.cpp`). Three controls
shape the sound; attack, hold and release adapt to the music. Main stereo
or mono in and out, one gain for every channel (stereo-linked), fixed
latency.

| key | type | range | curve | default |
|---|---|---|---|---|
| `gain` | dB | 0..24, smoothed 30 ms: drive into the ceiling | linear | 0 |
| `ceiling` | dB | −24..0, true peak, smoothed 30 ms | linear | −1 |
| `release` | ms | 10..1000: the peaks' recovery | log | 120 |
| `target` | dB (LUFS) | −24..−6: what the loudness readings compare against | linear | −14 |
| `listen` | enum | `output`, `matched`, `delta`; not automatable | | `output` |
| `accumulate` | bool | not automatable: the meters total everything since it was switched on or play started, instead of following the last 30 s | | false |
| `gain_reduction` | dB | readonly, −48..0: the deepest reduction in the last block | | |
| `short_term_loudness` | dB (LUFS) | readonly, −70..0: the output, 3 s | | |
| `integrated_loudness` | dB (LUFS) | readonly, −70..0: the output over the last 30 s, or since play started while accumulating | | |
| `true_peak` | dB (dBTP) | readonly, −70..+6: the output's highest in the last 2 s, or since play started while accumulating | | |
| `loudness_range` | dB (LU) | readonly, 0..40: EBU R128 LRA over the last 30 s, or since play started while accumulating | | |

Detector. The input times `gain`, each channel's true peak read at the
samples and at three points between each pair (a 48-tap windowed sinc per
phase, Kaiser β 8, each phase summing to 1), every local top refined by the
vertex of the parabola through it and its neighbours: within 0.01 dB of
the reconstructed peak for tones to 20 kHz at 48 kHz. A sample's peak is
its own and the highest between it and either neighbour; channels take the
louder. The requirement is min(0, ceiling − peak) in dB.

Gain path, per sample, in dB:

- **Hold.** The lowest requirement of the last H samples (a monotonic
  queue): H is the broad attack plus 3 ms or, while a bass line leads and
  its period is longer, that period (at most 45 ms more).
- **Release, two stages.** A plateau follows the held value (80 ms toward
  more reduction, 10 × `release` toward less, at least 0.4 s and at most
  4 s). The gain is the held value or, when that asks for less, a recovery:
  toward the plateau at `release` while below it, toward 0 dB at the
  plateau's pace above it.
- **Attack, two curves.** Three moving averages in a row (lengths 42 %, 33 %
  and the rest of span + 2) make a bell-shaped curve over the span: a
  broad one over 4 ms and a sharp one over 1 ms, the sharp one fed 3 ms
  later so both land on the same sample. Each output is an average of
  values no higher than the requirement it lands on, so neither overshoots,
  and nor does any blend of them. The blend's sharp share follows how
  sudden the moment is: the detector's 1 ms power against its 50 ms power,
  3 dB above mapping to 0 and 9 dB to 1, rising in 0.5 ms and falling over
  40 ms.
- **Bass guard.** The mono sum through two 110 Hz low-passes; its period
  from rising crossings (hysteresis 0.3 × its RMS), 25 to 300 Hz, eased;
  it leads while the low band holds more than 30 % of the power (30 ms
  means) and crossings keep coming (within 50 ms). The guard eases in and
  out over 40 ms.

The gain applies to the input delayed by the broad span − 1 + 24 samples,
the latency: 215 samples at 48 kHz (199 at 44.1 kHz). Read by the same
detector, the output stays at the ceiling (within 0.001 dB).

Loudness (BS.1770-5, EBU Tech 3341/3342). K-weighting at any rate, from the
analogue prototypes of the standard's 48 kHz filters through the bilinear
transform, on the output (and on the dry input, for `matched`); channels
summed with weight 1. 100 ms blocks: momentary over 4, short-term over 30.
Integrated: momentary blocks above −70 LUFS, gated again 10 LU under their
loudness. LRA: short-term values (once 3 s have played) above −70 LUFS and
20 LU under their own loudness, the 95th percentile minus the 10th. Two
modes:

- **Recent** (`accumulate` off, the default): integrated and LRA from the
  last 300 blocks (30 s); true peak the highest of the last 2 s (it falls
  2 s after a peak is last reached); samples over the ceiling counted
  until 5 s pass without one. Peak to loudness uses the last 30 s' true
  peak.
- **Accumulating** (`accumulate` on): integrated and LRA from histograms
  of 0.1 LU bins, and true peak and overs, since it was switched on; the
  editor shows the running time.

The recent window keeps rolling in either mode; switching the mode
restarts the accumulated totals, and starting the transport restarts both.
The totals hold only audio after their restart: their own 100 ms blocks,
K-weighting and true-peak history start from silence there.

Listen: `matched` turns the output down by the dry input's short-term
loudness against the output's (−30..+6 dB, gliding over 400 ms, back over
40 ms); `delta` plays what was taken (the delayed driven input minus the
output, divided by the drive), crossfading over 10 ms.

Editor (1000 × 700, the script's own drawing; a mountainside whose trees stop
at a dashed line beside the name):

- **History.** Eight seconds scrolling (10 ms columns, peak envelopes with
  a 120 ms fall), +9..−27 dBFS: the driven input's true peak, filled in clay
  where it rises above the ceiling (what was held down); the output's true
  peak filled in linen; the momentary loudness in marigold; the target,
  dotted; the ceiling, a sage line; bar and beat lines while the transport
  plays. Under it a lane of gain reduction (0..−9 dB): the sustained part
  (the plateau) in moss, the peaks' part in clay. Drag the ceiling or the
  target line; drag elsewhere or wheel for gain; double-click resets.
- **Loudness.** A clock beside the heading switches accumulating: lit,
  with the running time; dim, with "last 30 s". Integrated (large), how far from the target and what a
  service normalizing to it would do, a button that sets the gain the
  difference suggests, momentary and short-term bars with the target,
  true peak, loudness range, peak to loudness, short-term and samples over
  the ceiling; the target's slider.
- **Limiter.** Gain, ceiling and release knobs; the listen choice.
- **Engine.** The attack curve in use between the broad and the sharp one,
  the bass it follows and the hold that spans it, the release (the path
  back to rest from the last peak's reduction, held half a second, if the
  music let go there: quickly to the sustained part, in clay, then over
  seconds; on a scale that grows with the reduction, and a faint example
  at rest), and a gain-reduction bar in its two parts with a held peak.

Knobs drag vertically (Shift fine), wheel, and reset on a double click.

Factory patches: Transparent Master (+3 dB, −1 dBTP), Streaming Loud (+6,
−1, 90 ms), Club Master (+9, −0.3, 60 ms, target −8), Broadcast Safe (+2,
−2, 200 ms, target −23), Acoustic Gentle (+2, −1, 300 ms, target −16), Bus
Peak Catcher (0, −3, 50 ms).

## 11. Chorus — "Withy" [`withy`]

Voices a few milliseconds late, each swaying sharp and flat, from a still
widening to a choir of eight and a vibrato, written as a DSP script
(ADR-0047: `plugins/withy/script/withy.cpp`). Main stereo or mono in
and out (mono hears each voice's two sides summed); no latency.

| key | type | range | curve | default |
|---|---|---|---|---|
| `style` | enum | `classic`, `dimension`, `ensemble`, `multi`, `vibrato` | | `classic` |
| `voices` | int | 2..8: with `multi` | | 4 |
| `delay` | ms | 1..40: the voices at rest (not with `vibrato`) | log | 7 |
| `width` | % | 0..200: the voices' | linear | 100 |
| `sync` | enum | `free`, `bars4`, `bars2`, `bar1`, `n2`, `n4`, `n8`, `n16`, `n32`, `n4t`, `n8t`, `n16t`, `n4d`, `n8d` | | `free` |
| `rate` | Hz | 0.03..12: with `sync` free | log | 0.6 |
| `depth` | ct | 0..100: each voice's pitch swing either way | pow 2 | 8 |
| `shape` | enum | `sine`, `triangle` | | `triangle` |
| `shimmer` | % | 0..100: a 6.1 Hz flutter, as a share of the depth | linear | 0 |
| `drift` | % | 0..100: a slow random wander, as a share of the depth | linear | 0 |
| `bloom` | ms | 0..2000: the sway's return after each onset; 0 is off (with `sync` free) | cubic | 0 |
| `medium` | enum | `clean`, `bbd` | | `clean` |
| `age` | % | 0..100: the bucket brigade's wear (with `bbd`) | linear | 30 |
| `high_cut` | Hz | 1000..20000 on the voices; 20000 is off | log | 20000 |
| `low_keep` | Hz | 20..1000: below it dry, each side as it came in; 20 is off | log | 20 |
| `feedback` | % | −80..80 | linear | 0 |
| `mix` | % | 0..100 (not with `vibrato`) | linear | 50 |
| `output` | dB | −24..24 | linear | 0 |
| `detune` | ct | readonly: the largest voice's pitch swing now | | |
| `sweep` | ms | readonly: the furthest voice's delay swing either way now | | |
| `rate_now` | Hz | readonly: the sway's rate, synced or free | | |
| `mono_loss` | dB | readonly: the output's fold to mono against the input's, 300 ms | | |
| `input_level`, `output_level` | dBFS | readonly: peaks, falling 20 dB/s | | |

- **Lines.** One per channel, double precision, 100 ms long, written
  twice over so a read never wraps. Every voice reads through a 16-tap
  Kaiser-windowed sinc (β 8, passing 94 % of the band), 256 rows a sample
  with a linear step between rows, each row summing to 1; the two lines
  are read together. A cubic's level at 15 kHz falls 2.5 dB between
  samples, so a sweeping voice warbles its highs at the rate it crosses
  samples (hundreds of hertz); the sinc's falls under 0.01 dB.
- **Styles.** Each voice has a gain into each side, a place in the cycle,
  a rate and a delay against `delay`:
  - classic: two, left and right, half a cycle apart;
  - dimension: two half a cycle apart, each side its own voice plus the
    other's × −0.3, then +0.35 dB;
  - ensemble: three a third of a cycle apart, left, centre (× 0.707 each
    side) and right, each flutter at its own rate (× 1, 1.09, 0.92);
  - multi: N voices at `delay` × (0.65 … 1.35), evenly; places evenly
    across ±0.9 (equal-power pan), taken from the edges inward in turn;
    phases 0.618 apart; rates ± 11 % (fixed per voice; 1 when synced);
    −0.45 dB per doubling of N;
  - vibrato: one voice on both sides, no dry, read at the swing + 1 ms.
  Each side's voices are scaled by one over the root of the sum of their
  squared gains (they are mostly unlike one another): the voices alone sit
  within a dB of the input. A change of style or of `voices` dips the
  voices over 8 ms, lays them out afresh at the bottom and brings them
  back; the vibrato's mix glides over the same 8 ms.
- **Depth.** `depth` is the peak pitch swing, so the delay swing follows
  from it: r = 2^(depth/1200) − 1, A = r / (f · s), s = 2π for the sine and
  2π·0.995 / asin(0.995) for the triangle, f the voice's rate. The triangle
  is asin(0.995 sin 2πφ) / asin(0.995): the pitch holds sharp, then flat,
  and turns over 1.6 % of a cycle (no click). Shimmer adds a sine at 6.1 Hz
  (× the voice's factor) of the depth × `shimmer`; drift a raised-cosine
  glide between random values, one per cycle at 0.37 f, of the depth ×
  `drift`. All three together are held to 25 ms either way (scaled down
  together), and a voice's delay to the swing + 1 ms at least: past those
  `detune` and `sweep` read what is heard. The swings and delays glide
  (120 ms) to targets worked out every 16 frames; a shape change
  crossfades the two over 10 ms.
- **Sync.** A note value's cycle at the song's tempo and bar; while the
  song plays, each voice's phase is drawn toward the song's place in the
  cycle (time constant 1.5 s; at once before the first block), so a
  locate never jumps the delay. Stopped, it runs free at that rate.
- **Bloom.** Onsets are a fast envelope (0.5 ms up, 30 ms down) above
  twice a slow one (30 ms, 300 ms) and −50 dBFS, re-armed below 1.3 times.
  At an onset the sway's speed falls to 0 in 4 ms, holds 20 ms and returns
  over `bloom` (raised cosine): the delay stays where it was, so the pitch
  is exact while held and the swing grows back with the speed. Every
  motion (sway, shimmer, drift) slows with it. Off while synced.
- **Bucket brigade.** 512 × 0.25^age stages, clocked at stages / 2d for
  each voice's delay d: a 12 dB/oct low-pass at 0.4 × the clock (at most
  19 kHz) per voice, redesigned every 16 frames, so a longer delay is
  darker and the band breathes with the sweep. Into the line the curve
  (tanh(gx + 0.25) − tanh 0.25) / (1 − tanh² 0.25) / g, g = 0.35 + 1.15 ×
  age; on each voice white noise (seeded per voice and side) at
  −66 + 28 × age dB times the line's input envelope (2 ms up, 60 ms down),
  the compander's hiss, silent in silence; a 4 Hz DC blocker on the
  voices. The medium crossfades over 30 ms.
- **Loop and tone.** What is fed back is the mean of each side's voices,
  high-passed at 20 Hz and guarded (untouched to −6 dBFS, easing toward
  full scale), × `feedback`; the voices are then scaled by
  √(1 − 0.6 fb²) so the level holds as the feedback rises. `high_cut` is a
  12 dB/oct low-pass on the voices; it and `low_keep` fade in and out over
  10 ms at their ends of the range.
- **Low keep and mix.** A Linkwitz–Riley crossover (two Butterworths each
  way) at `low_keep`: the lows pass dry, the highs feed the lines and the
  dry by the mix, so low + high × cos(πm/2) + voices × sin(πm/2) — at mix 0
  the crossover's allpass sum, at any mix the lows exactly as they came
  in. The mix is equal-power: the voices and the dry are unlike enough
  that 50 % of each is as loud as either. Width is the voices' mid and
  side (100 % exactly nothing); `output` last.
- **Tail.** `setTail`: the longest delay (89 ms: the deepest centre,
  40 ms × 1.35, plus the widest swing and 10 ms) times the passes the
  feedback needs to fall 90 dB, plus one, and 50 ms more.

Measured (release sandbox's player, 48 kHz):

| check | result |
|---|---|
| a 1 kHz sine, mix 0; output −6; one voice held still (vibrato, depth 0) | 0.00, −6.00, 0.00 dB |
| a 440 Hz tone, the voices alone, 8 ct triangle at 0.6 Hz | each voice −8.2..+8.3 ct, held at ±7.9 between turns |
| a 440 Hz tone, 20 ct sine at 3 Hz | ±20.4 ct |
| a 12 kHz tone, 20 ct; 16 kHz, 40 ct sine at 2 Hz | level steady within 0.005 dB; 0.01 dB |
| pink noise (30 Hz–18 kHz), the voices alone against the input | classic −0.02, dimension 0.00, ensemble +0.49, multi of 4 +0.22, of 8 +0.95 dB; mix 50 −0.32 dB |
| a centred source, the voices still, mix 50 | comb notches −21 dB classic, −13 dimension, −11 ensemble |
| `mono_loss`, centred pink noise, defaults per style | classic −1.2, dimension −2.2, ensemble −0.7, multi −0.4 dB |
| bucket brigade, 7 ms, pink noise | age 0: −1.5 dB at 12 kHz; age 30: −1.8 at 8 kHz, −5.4 at 12 kHz; age 100: −3.9 at 4 kHz, −13 at 8 kHz; age 30 at 2 ms: −1.1 at 16 kHz |
| bucket brigade, a −12 dBFS 440 Hz tone, age 0 / 30 / 100 | second harmonic −39 / −34 / −27 dB, hiss 1.5–8 kHz −73 / −65 / −49 dB under the tone |
| vibrato 30 ct at 5.2 Hz, bloom 450 ms, a held note | within 1.5 ct of the note from 25 to 150 ms after the onset, the full ±30 ct by 0.6 s |
| `n4` at 120 BPM, playing, the voices alone | flat for the first and last quarter of each beat, sharp between, as the phase says, from the first beat to the fifth |
| block sizes 1, 13, 64, 4096 against 256 | identical free; −150 dB synced (the host's beat position) |
| feedback 50 to 80 %, −60 %, pink noise and notes | level within ±1.1 dB of none |
| full-scale noise, feedback 80 %, eight voices at 2 ms | peaks +4 dBFS, bounded |
| automation of every parameter | no step above the unautomated render's |
| factory patches against the dry | within ±1.2 dB on notes; on pink noise within ±0.5 dB but Warped Record (−1.9 dB, its aged band) |

Editor (1064 × 724): the strands, each voice's delay over the last 3 s,
drawn over a dark edge so they read as woven, the delay set dashed behind
them and the input's level along the foot (bloom flattens the strands at
each note); dragged across for the rate (by octaves free, a note value per
40 units synced), up and down for the depth, the wheel for the depth. The
readouts: detune, sweep, rate (with the note value), mono loss, each
voice's pitch now on a flat-to-sharp line, bloom's hold. The voices around
the listener, each at its place and further back as it reads later (a
crossed voice's turned-over share a ring on the other side). The styles as
a list with a small picture of each, the voice count, delay and width; the
sync grid, rate, depth, one voice's motion over two cycles with its shape,
shimmer, drift and bloom; the medium, the response now (stereo and folded
to mono, each point the power across its own band) with the low keep and
high cut as handles to drag, age, high cut, low keep and feedback; mix and
output. The header's withies are a basket weave, four stakes and a weaver
for each voice (up to three), each bending as its voice sways.

Factory patches: Classic Chorus (the defaults), Subtle Width, Deep
Dimension, Bucket Brigade, Fast Warble, String Ensemble, Twelve-String,
Thick Doubler, Choir of Eight, Bass Chorus, Vibrato, Singer's Vibrato,
Warped Record, Flanged Swirl.

## 12. Phaser — "Pollia" [`pollia`]

Notches swept through the sound, from a slow four-stage swirl to a
resonant sweep, a photocell vibe's throb, an envelope phaser and a barber
pole whose notches rise or fall forever, written as a DSP script
(ADR-0047: `plugins/pollia/script/pollia.cpp`). Main stereo or mono
in and out; no latency.

| key | type | range | curve | default |
|---|---|---|---|---|
| `stages` | integer | 1..24 all-pass stages; every two make a notch | | 6 |
| `centre` | Hz | 30..10000: the stages at the middle of the sweep, smoothed 30 ms | log | 700 |
| `spread` | oct | 0..6: the stages staggered over this span, smoothed 60 ms | linear | 0 |
| `feedback` | % | −95..95, smoothed 20 ms | linear | 0 |
| `motion` | enum | `sweep`, `rise`, `fall` | | `sweep` |
| `range` | oct | 0..8: the sweep's reach, top to bottom (sweep only) | linear | 3 |
| `sync` | enum | `free`, `bars8`, `bars4`, `bars2`, `bar1`, `n2`, `n4`, `n8`, `n16`, `n2d`, `n4d`, `n8d`, `n2t`, `n4t`, `n8t` | | `free` |
| `rate` | Hz | 0.01..20: with `sync` free, smoothed 50 ms | log | 0.3 |
| `shape` | enum | `sine`, `triangle`, `ramp_up`, `ramp_down`, `steps`, `drift` (sweep only) | | `sine` |
| `phase` | degrees | 0..360: the motion moved along its cycle, smoothed 50 ms | linear | 0 |
| `stereo` | degrees | 0..180: the right side's motion ahead, smoothed 50 ms | linear | 0 |
| `response` | enum | `direct`, `photocell` | | `direct` |
| `envelope` | oct | −4..4: the stages moved by the playing at its loudest, smoothed 30 ms | linear | 0 |
| `attack` | ms | 1..500 | log | 10 |
| `release` | ms | 10..3000 | log | 200 |
| `keep_below` | Hz | 20..1000: below passes untouched; 20 is off, smoothed 30 ms | log | 20 |
| `mix` | % | 0..100: 50 the deepest notches, 100 the wet alone, smoothed 20 ms | linear | 50 |
| `output` | dB | −24..24, smoothed 20 ms | linear | 0 |
| `centre_now` | Hz | readonly: the left side's stages this moment | | |
| `envelope_now` | % | readonly: the envelope, 100 at the recent peak | | |
| `input_level`, `output_level` | dBFS | readonly: peaks, falling 20 dB/s | | |

- **Stages.** First-order all-passes, trapezoidal (topology-preserving:
  ap = 2·lp − x of a one-pole with g = tan(πf/fs)), so a corner can move
  every frame without zipper. Stage k of n sits at centre × 2^(offset +
  spread × (k/(n − 1) − 0.5)), clamped to 5 Hz..min(22 kHz, 0.47 fs);
  each place glides over 30 ms when the count or spread changes. The
  corners' targets are set every 8 frames and each corner's G = g/(1 + g)
  steps linearly to its target over those frames. A change of `stages`
  runs the longer chain for 20 ms and crossfades its taps at the old and
  new counts; stages that start take their corners at once, and stages
  that stop are cleared.
- **The loop.** u = x + `feedback` × guard(c), c the chain's last output
  (a frame late); guard is untouched to 0.6 and eases to 1.0 (tanh).
  out = x(1 − m) + c·m. Feedback adds wet power 1/(1 − fb²) on noise; the
  output (not the kept lows) is scaled by (P₀/P)^0.4, P = (1 − m)² +
  m²/(1 − fb²), P₀ the same at fb 0, so feedback leaves white noise's
  level within 0.2 dB and the peaks still ring.
- **Motion.** One cycle position per frame: from the song position when
  synced and playing (quarter notes / the note's beats, so a sweep lands
  the same way on every play), else running at `rate` (synced and
  stopped: at the note's rate at the tempo, 120 BPM without one). `phase`
  is added; the right side adds `stereo`. The sweep's position p =
  0.5 + 0.5 × shape(cycle): `ramp_up`/`ramp_down` return over the last 4%
  of a cycle as a cosine; `steps` holds a hashed value per cycle,
  gliding to it over the first 6%; `drift` glides between them over the
  whole cycle (raised cosine).
  - direct: the centre's offset = `range` × (p − 0.5) + e, e the
    envelope's octaves.
  - photocell: a lamp's light follows (g_min + (1 − g_min)p) × 2^e,
    g_min = 2^−range, through a 12 ms lag; the cell's conductance follows
    the light in 5 ms when brightening and 30 + 170 ms × (1 − conductance)
    when darkening (darker, slower); the offset = log2(conductance) +
    range/2. Linear light through a logarithmic law: the stages dwell
    high and pass quickly through the low end, and the cell's slow
    darkening leans the sweep.
  - `rise`, `fall` (a barber pole): the stages stay at the centre (plus
    the envelope; the sweep fades out over 30 ms). The chain's output and
    the dry pass a 90-degree pair, two chains of seven first-order
    all-passes whose phases differ by 90° ± 0.34° over four decades of
    the warped frequency tan(πf/fs) (10 Hz..20 kHz at 44.1 kHz, wider at
    higher rates; designed for Pollia by a minimax fit); the wet becomes
    I cos θ − Q sin θ against the dry's I, θ turning one cycle per cycle
    of the motion (down for rise, up for fall), so every notch slides to
    the next's place at full depth: the wet is frequency-shifted by
    `rate`, and the dry's phase (not its level) follows the pair's. The
    loop stays around the chain alone, so feedback acts as in a sweep.
    Changing between sweep and rise or fall crossfades 30 ms.
- **Envelope.** The louder channel's level through a follower (`attack`,
  `release`, one-pole), in dB, against a reference that takes each new
  peak at once and falls 4 dB/s (floored at −60 dBFS): e = clamp(1 +
  (level − reference)/24, 0, 1), faded out between −60 and −66 dBFS, ×
  `envelope` octaves. No threshold: a note as loud as the recent peak
  moves the full amount, one 24 dB quieter none.
- **Keep below.** A 12 dB/oct low-pass (Q 0.5) at `keep_below` splits
  the input; its part passes untouched and the rest is phased (x − low, so
  the two sum back exactly). Off at 20 Hz; the filter always runs.
- **Tail.** 60 dB of the loop at its slowest stage: n/(2π f_low) per pass
  × 6.9/−ln|feedback| passes (one without feedback), plus 50 ms, at most
  10 s; f_low from the centre, half the range and spread and the whole
  envelope below it.

Measured (dev player and the built plugin, 48 kHz):

| check | result |
|---|---|
| 4 stages at 1 kHz, range 0, white noise | notches at 410 and 2391 Hz (theory 414.6 and 2393 Hz with the bilinear warp) |
| 2 stages at 1 kHz, a 1 kHz sine | −188 dB; feedback 50%: −16.08 dB, the loop's analytic value |
| rise, 8 stages, spread 4, 0.5 Hz, white noise, 44.1/48/96 kHz | the deepest notches −35 to −55 dB throughout, each reaching the next's place in 2 s |
| rise, mix 100, a −12 dBFS sine | the level kept to 0.03 dB as the phase turns |
| feedback −95..95, 6 stages, white noise | the level within 0.2 dB of feedback 0 |
| white noise, 4 stages at 800 Hz, range 4, a 1 Hz sine sweep: direct and photocell | direct: the lowest notch 93..1335 Hz, symmetric; photocell: rises in 0.4 s, falls in 0.6 s, the lamp's lag keeping it off the bottom at this rate |
| plucks at 0, −6 and −12 dB half a second apart, envelope +3 oct, range 0 | the stages 3.0, 2.6 and 2.2 octaves up at each (the reference falls 2 dB in each gap), falling between |
| automation of stages, motion, response, keep below, centre, spread, shape, sync, phase | no step above the unautomated render's (feedback's rise is the louder loop's own) |
| block sizes 1, 7, 64, 4096 (barber pole with photocell, envelope, keep below; steps synced) | bit-identical |

Editor (1040 × 720): the response now (the left side over the right when
`stereo` is set) with a bead at each notch, the sweep's reach shaded and
each stage's corner marked; under it, where the notches went over the
last 1 to 4 s (about a cycle and a half of the motion) as trails across
time (waves for a sweep, diagonals for a barber pole, jumps for an
envelope; the right side's in heather; feedback's peaks in clay).
Dragged across, the centre (following the pointer); up and down, the
range; the wheel, the feedback. The motion panel: sweep, rise or fall, the
wave with the motion's place on it (with photocell, the path the cell
takes it on, simulated as the audio runs it; a turning barber pole in
rise and fall), the rate and a note grid, the shapes, phase and stereo, and direct
or photocell with the lamp lit as the cell sees it. Stages (the counts,
where the stages lie, centre, range, spread, feedback), the envelope (its
last 4 s, amount, attack, release) and output (keep below, mix, output,
the notches now). The header's mark is a cluster of marble berries whose
sheen turns with the motion and brightens with the sound.

Factory patches: Swirl (the defaults), Classic Four, Resonant Sweep,
Hollow Twelve, Photocell Throb, Lamp Vibrato, Endless Rise, Falling
Spiral, Envelope Funk, Two-Bar Sweep, Stepped Notches, Bass Phase.

## 13. Tape — "Umber" [`umber`]

A tape machine, modelled part by part from the oxide up rather than copied
from any one machine: from the faint rounding of peaks a well-kept modern
machine gives a mix to a worn, underbiased, warbling cassette, written as a
DSP script (ADR-0047: `plugins/umber/script/umber.cpp`). Main stereo or
mono in and out. Latency: 8 ms plus the oversampler's (456 samples at
48 kHz with 4x), or with `latency` low 9 samples at 44.1 and 48 kHz.
Tail: with print-through on, one turn at the speed plus 0.1 s; otherwise
none (hiss, hum and DC noise are the machine's own, not the input's).

| key | type | range | curve | default |
|---|---|---|---|---|
| `drive` | dB | −24..24: the record level; at 0 a sine at −18 dBFS RMS records at reference level, smoothed 30 ms | linear | 0 |
| `tape` | dB | 5..13: the tape's headroom, its 3% point over reference (12.5 modern, 9 classic, 5 early), gliding 30 ms | linear | 12.5 |
| `bias` | % | 50..150 of the recommended bias, gliding 30 ms | linear | 100 |
| `record_amp` | % | 0..100: the class-A record amplifier's drive, gliding | linear | 15 |
| `hf_saturation` | % | 0..200: the highs' ceiling, ×2^(1 − x) (100 the tape's own), gliding | linear | 100 |
| `record_hf` | dB | −6..6: the record side's HF trim, a shelf about 4 kHz at 15 ips (following the speed), gliding | linear | 0 |
| `auto_gain` | bool | the playback level falls as the drive rises; toggling crossfades over 20 ms | | on |
| `speed` | enum | `ips30`, `ips15`, `ips7_5`, `ips3_75` | | `ips15` |
| `emphasis` | enum | `nab`, `iec` (not at 30 ips: AES) | | `iec` |
| `head_bump` | % | 0..200 of a real head's, gliding | linear | 100 |
| `bump_freq` | Hz | 30..120: the bump's peak at 15 ips, gliding; applies while `head_bump` > 0 | log | 57 |
| `azimuth` | ° | 0..1: the playback head's tilt, gliding | pow 2 | 0 |
| `head_magnetism` | % | −100..100: a magnetized record head's steady field (even harmonics, DC noise), gliding | linear | 0 |
| `dropouts` | % | 0..100: 4x² a second | pow 2 | 0 |
| `drop_depth` | % | 0..100; applies while `dropouts` > 0 | linear | 30 |
| `print_through` | dB | −80..−30 against the signal, smoothed 20 ms; −80 is off | linear | −80 |
| `crosstalk` | dB | −80..−20, smoothed 20 ms; −80 is off | linear | −80 |
| `wow` | % | 0..2: peak speed error at 15 ips, slow (× √(15/ips) at other speeds) | pow 3 | 0.02 |
| `flutter` | % | 0..1: peak speed error at 15 ips, fast (× √(15/ips)) | pow 3 | 0.02 |
| `scrape` | % | 0..100: kilohertz jitter, 380 ns RMS × x^1.25 at 15 ips | pow 2 | 10 |
| `repro_amp` | % | 0..100: the class-A playback amplifier's drive, gliding | linear | 15 |
| `repro_hf` | dB | −6..6: the playback side's HF trim, as `record_hf`'s, gliding | linear | 0 |
| `hiss` | dB | −30..12 against the tape's own noise, smoothed 20 ms; −30 is off | linear | −30 |
| `mod_noise` | dB | −30..12 against the tape's own modulation noise, smoothed 20 ms; −30 is off | linear | 0 |
| `hum` | dB | −100..−40 against reference level, smoothed 20 ms; −100 is off | linear | −100 |
| `mains` | enum | `hz50`, `hz60`; applies while `hum` is on | | `hz60` |
| `output` | dB | −24..24, smoothed 20 ms | linear | 0 |
| `oversampling` | enum | `x1`, `x2`, `x4`, `x8`, `x16` (restarts) | | `x4` |
| `latency` | enum | `full`, `low` (restarts) | | `full` |
| `vu` | dB | readonly: the record level against reference, as a VU meter reads it | | |
| `headroom` | dB | readonly: the 3% limit over the loudest recent peak field, held 0.3 s | | |
| `saturation` | dB | readonly: what the oxide took off the loudest recent peaks | | |

The model was fitted to published data for current studio tape (RTM
SM911's and ATR Master Tape's datasheets: third harmonic at reference
level 0.04, 0.10 and 0.28% at 30, 15 and 7.5 ips, 1% at +10, +9 and +6 dB,
3% at +13, +12.5 and +10 dB, saturation output at 10 kHz 11.5, 10.5 and
0.5 dB and at 16 kHz 9.0 and 7.5 (12.5 kHz −3.5 at 7.5 ips) over
reference, A-weighted noise about 60 dB under it, print-through −56 dB),
and to the common figures for heads and transports (a head bump near
4 Hz per ips, 0.03 to 0.05% weighted wow and flutter on a good machine).
Where nothing was published (the amplifiers' harmonics, scrape's level,
crosstalk), the values are a modern machine's as judged, and say so.

- **Calibration.** Reference level (0 VU, the datasheets' 320 nWb/m) is
  a sine at −18 dBFS RMS at `drive` 0. The field it records at is, at the
  three fitted tapes (12.5, 9, 5 dB), 0.1978, 0.2969 and 0.4440 in the
  oxide's units, times −0.7, −0.2, +2.75 and +5.0 dB at 30, 15, 7.5 and
  3.75 ips: the tape's limit at mid frequencies falls as the wavelength
  shortens. The output is divided by the same field, so a quiet signal
  keeps its level; `auto_gain` divides by the drive's gain as well.
- **The tape.** Between the fitted tapes the constants follow the line
  through them (the field and pinning in log, squareness, hiss and
  modulation noise and the loss factor 1, 1.1, 1.3 linearly in the
  headroom); the switch between them glides, so `tape` is automatable.
- **Amplifiers.** Each is (tanh(a·x + o) − tanh o)/(a·(1 − tanh² o)), x
  the signal over the reference peak, a = 0.2·amount³, o 0.35 (record)
  and 0.25 (playback): unit gain for small signals, second harmonic about
  −80 dBc each at reference level at 15%, −47 at 50%, −30 at 100%,
  rising 1 dB per dB; a 2 Hz coupling takes out the offset's DC. Both run
  oversampled: the record amp before the record curve, the playback amp
  after the moving read. At 0% a stage passes the input, its coupling
  still following it.
- **Record and playback equalisation.** The record side's HF trim
  (first-order shelf, geometric centre 4 kHz·v, v = ips/15: each speed's
  equalisation has its own trim, so its reach follows the wavelength; at
  most 0.3 fs'), then the standard's lows:
  with NAB below 30 ips, the 3180 µs lift, held to +10 dB below 15.8 Hz,
  which playback undoes exactly. The standard's highs act through the
  highs' ceiling (below). fs' is the oversampled rate; sections are
  first-order, bilinear with both corners prewarped.
- **The oxide.** The anhysteretic magnetization is tanh(u + q·u³), the
  two-state (single-domain needle) Langevin form, q the tape's squareness
  (0.24, 0.16, 0.08 at the fitted tapes); below |u + q·u³| = 0.3 its series
  to the ninth power stands in (within 2e-8). Irreversible magnetization is
  pinned until the field moves toward the curve, then relaxes to it by
  exp(−|Δh|/k), an exact step that stays stable however narrow the loop; a
  reversible part (0.3 of the gap) follows at once. k is 1e-5, 3e-5, 1e-4
  at the fitted tapes (×3 at 7.5 ips, ×8 at 3.75), × 10^(5(1 − b)) under
  the recommended bias b and × max(0.2, 1 − 0.6(b − 1)) over it: at b = 1
  the loop is closed for any audible signal, at 0.5 it opens into an
  unbiased tape's crossover grain. `head_magnetism` adds a steady field of
  1.5 × the reference field × its value (its offset taken out of the
  flux), so the tape itself makes even harmonics, and its steady
  magnetization carries the modulation noise (DC noise, below).
- **The highs' saturation.** The flux splits at 5 kHz·v (v = ips/15)
  into lows and highs, each through two bilinear one-poles (so the two
  sum to the signal exactly); the highs are lifted by a first-order zero
  at 9 kHz·v, meet c·tanh(x/c), and are lowered again; output = lows −
  highs, an allpass at low level. c, in reference peaks, is 1.6, 3.4,
  1.32, 0.54 by speed, × the NAB/IEC curves' ratio at 10 kHz with NAB
  (+2.8 dB at 15 ips), ÷ the tape's loss², × 2^(1 − `hf_saturation`):
  fitted to the saturation outputs above. Because the mids never pass the
  highs' curve, a loud cymbal saturates the highs alone (with the highs
  lifted into the oxide instead, it squashed the mids with it: 8 dB more
  distortion between 500 Hz and 4 kHz on music).
- **Bias** also tilts the highs (a high shelf at 5 kHz·v, Q 0.6) by ΔS ×
  (1 − b)/0.3 under it, back to 0 at b = 0.4, and −ΔS × (b − 1)/0.3 over
  it, ΔS 1.5, 3, 6.5, 9 dB by speed (the datasheet's ΔS10); every
  frequency falls 6((0.7 − b)/0.2)² dB below b = 0.7 and 0.8(b − 1)/0.5 dB
  over 1.
- **The playback head.** A bump at `bump_freq`·v of 1.5, 2.0, 2.5,
  2.75 dB by speed × `head_bump` (a peak of Q 1.2 and 1.07 × that gain, so
  that with the rest it peaks there), and the contour effect's undulation
  above it, one every bump frequency, smaller each time: a dip at 2 times
  (Q 1.8, −0.8 ×, about −1 dB at 15 ips), a ripple at 3 times (Q 3.4,
  +0.38 ×, about +0.4 dB), a second dip at 4 times (Q 3.5, −0.15 ×, about
  −0.2 dB). Bumps are larger at the slower speeds as machines aligned at
  100 Hz measure (at 30 ips the alignment trims the bump and leaves the
  roll-off below it). Then a 12 dB/oct high-pass
  (Q 0.75) at 0.36 × the bump's frequency × min(1, `head_bump`) and no
  lower than 5 Hz, the aligned top end, a 12 dB/oct low-pass (Q 0.75) at
  30, 24, 17, 10.5 kHz by speed over the tape's loss factor where it falls
  under 0.45 fs (at 44.1 and 48 kHz the top at 30 and 15 ips is flat),
  bias's tilt and the playback HF trim (as the record side's). Filters
  that are switched out still run, so they come in from a live state.
- **Azimuth** θ, in the moving read at the oversampled rate: the tilted
  gap crosses each 2 mm track at a slant, so it hears the track spread
  over a window of 2 mm·tan θ/v, read as the average of 8 Lagrange reads
  across it (a sinc with its nulls); the two track centres, 4 mm apart,
  are 4 mm·tan θ/v apart in time, the left track's window half that
  later, the right's half earlier. Window and spread glide per frame,
  interpolated across the oversampled steps; together they take at most
  0.6 of the read's reach, past which both shrink together (with low
  latency, beyond about 0.3° at 15 ips), in the response plot too.
- **The tape's condition.** Dropouts at 4·`dropouts`² a second, each
  `drop_depth` × (0.3 to 1) deep, 6 to 66 ms long (a raised sine), the
  right side 0.6 to 1 of the left's (one speck crosses both tracks): the
  tape lifts up to 40 µm × the depth, and its spacing loss (54.6 dB per
  wavelength of lift, corner v/(2πd)) sweeps down from the top and back as
  two one-poles at 0.76 of that corner; the level down up to 6 dB with it
  (flux that is missing). The deepest event at 15 ips: 10 kHz −40 dB,
  1 kHz −9, 200 Hz −5. Print-through: the played signal low-passed at
  3 kHz·v and kept at about 12 kHz, read one turn of a half-wound
  10.5-inch reel later (0.534 m/v: 1.4 s at 15 ips), through a band-pass
  at 1 kHz·v (Q 0.5: the stray field reaches the next layer best at
  wavelengths about 2π times the base film's thickness), at
  `print_through`'s level, +1.33 dB per dB of `tape` under 12.5 (softer
  tapes print more: 10 dB more at 5).
  Crosstalk: the lows (one-pole at 300 Hz·v) moved between the tracks,
  left + g·(low right − low left) and the reverse, so a centred sound keeps
  its level.
- **Transport.** Wow and flutter grow as the tape slows, as machines'
  specifications do (about 0.04, 0.05, 0.08% at 30, 15, 7.5 ips): their
  knobs are the speed error at 15 ips, × √(15/ips). The speed error is
  `wow` × (0.5 sin(reel, 0.6 Hz·v) +
  0.25 sin(take-up, 0.5 Hz·v) + 0.3 wander) + `flutter` × (0.45
  sin(capstan, 4.85 Hz·v) + 0.2 sin(idler, 11 Hz·v) + 0.35 friction); each
  sine's depth drifts toward 1 ± 0.45 (uniform), drawn every 0.25 s, gliding
  over 0.7 s; the wander is noise held 32 frames through two poles at
  0.5 Hz·v, the friction the same between 6 and 25 Hz·v, each scaled to
  unit RMS. Its integral (leaking over 4 s) is the tape's displacement;
  scrape adds 380 ns × `scrape`^1.25 × √(15/ips) RMS of noise through a
  band-pass at 3 kHz (Q 0.9). The displacement is bounded softly (linear to
  80% of the read's reach, then easing to it). The read's reach is 8 ms
  less 1.5 samples, or with low latency 9 samples less the oversampler's
  less 1.5; what the wow and flutter need (their parts' displacements at
  the deepest sway, the random parts' at three times their RMS) is fitted
  to 80% of it by scaling the wow, then the flutter, keeping their shapes.
  The read is a 4-point Lagrange at the oversampled rate, exact (a whole
  sample) when nothing moves. The sines turn as rotations, renormalized
  every 32 frames, on a frame count only `prepare()` resets.
- **Noise.** Hiss: white noise through the playback curve (the standard's
  zero at 1/(2πτ), τ 17.5 µs at 30 ips, IEC 35, 70, 90 µs, NAB 50, 50,
  90 µs; NAB's inverse lift) and a one-pole loss at 10 kHz·v, scaled so
  that through the IEC curve and the aligned top it measures, A-weighted,
  −63, −60.5, −58, −55 dB under reference by speed, +0, +3.5, +8.5 dB at
  the fitted tapes (NAB, with more lift of the highs on playback, comes
  out 2.5 dB noisier at 15 ips), × `hiss`. With it, the playback
  electronics' own noise: white at the head, so through the playback
  curve's integration a first-order fall from 25 Hz to the standard's
  corner (and NAB's playback cut of the lows), a 12 dB/oct high-pass at
  15 Hz below; fixed, 8 dB over a modern tape's hiss at 50 Hz at 15 ips
  IEC, 26 dB under it A-weighted. Modulation noise: the magnetization
  × (1 + n), its steady part from `head_magnetism` included (DC noise:
  the grain stays when the signal stops), then that part's offset taken
  out; n uniform noise through two poles at 1.5 kHz·v at −66, −61,
  −55 dB RMS at the fitted tapes, × `mod_noise`. Hum, the same in both
  channels, at `hum` RMS against reference: the power supply's ripple on
  the mains' even harmonics (2, 4 … 16: 1, 0.45, 0.30, 0.22, 0.14, 0.11,
  0.07, 0.05, a buzz into the low kilohertz) and the playback head's
  magnetic pickup on the odd ones (1, 3, 5, 7: 0.6, 0.21, 0.09, 0.036),
  that part through NAB's playback lows. The balance is fixed and a
  little uneven, as a machine's is (not random per instance: offline
  renders must match the live output). Harmonics are rotations of the
  fundamental, renormalized every 32 frames.
- **Changes.** `drive` glides per sample over 30 ms; `output`,
  `print_through`, `crosstalk`, `hiss`, `hum` and `mod_noise` over 20 ms,
  and `auto_gain` crossfades over 20 ms. The settings marked gliding move
  30 ms toward their values, the curves redesigned every 32 frames while
  they move (the hiss's calibration when the tape lands). `wow`,
  `flutter` and `scrape` step; a dropout takes its depth as it begins. A
  new `speed` or `emphasis` fades the output out over 4 ms; at silence
  the new curves go in and every stage starts clear; the input fades in
  over 4 ms, so the tape fills again from nothing (about 16 ms of silence
  at full latency). A choice changed again meanwhile switches once more.
- **Oversampling.** As `oversampling` says, at any session rate (the
  user does the arithmetic; the editor shows the rate inside and that
  176 to 384 kHz is best); linear phase at full latency, minimum phase
  at low.
- **Readouts.** `vu`: the record level rectified through two poles of
  65 ms, × π/(2√2), against reference RMS; `headroom`: the field at 3%
  (the tape's headroom over its reference field; the midband's, the
  highs having their own ceiling) over the peak field (oversampled), held
  0.3 s; `saturation`: the field over the flux at
  the peaks above 5% of the limit, in dB, decaying over 0.3 s.

Measured (dev player and the built plugin, 48 kHz, 4x, wow, flutter,
scrape and modulation noise off unless said):

| check | result |
|---|---|
| third harmonic (THD), 1 kHz, 15 ips IEC, at −20, 0, +4, +9, +12.5, +13 dB re reference | 0.002, 0.094, 0.25, 0.97, 2.8, 3.2 % (datasheet: —, 0.10, 0.25, 1.0, 3.0 at +12.5) |
| the same at 30 ips | 0.002, 0.084, 0.22, 0.84 (1.12 at +10), 2.4, 2.8 % (datasheet: 0.04 at reference, 1 % at +10, 3 % at +13) |
| the same at 7.5 ips | 0.007, 0.20, 0.57 (0.99 at +6), 2.4 (3.2 at +10), 6.5 % (datasheet: 0.28, 1 % at +6, 3 % at +10) |
| `tape` 9 and 5, 15 ips: 3 % reached at | about +9 and +4.5 dB |
| `head_magnetism` 25, 50, 100 %, 1 kHz at 0 VU, amplifiers off | second harmonic −53, −47, −39 dBc; in silence with `mod_noise` at 0, DC noise −65 and −59.5 dB re reference RMS at 50 and 100 % (the SM911 sheet's DC noise: −56 dB) |
| `azimuth` 0.1, 0.3, 0.5, 1°, 15 ips, at 12 kHz | −0.17, −1.59, −4.79, −20.7 dB (the exact sinc: −0.17, −1.61, −4.85, −21.0); left after right by 18.3, 55.0, 91.6, 183.3 µs; with low latency the same to 0.3°, then held near 0.32° |
| second harmonic, both amplifiers at 15 %, at −3 VU | −77 dBc, rising 1 dB per dB |
| saturation output (the most a sine gets out), 10 / 16 kHz | 30 ips +12.8 / +9.1, 15 ips +10.0 / +6.8, 7.5 ips +10 kHz −1.3, 12.5 kHz −3.5 dB re reference (datasheet 11.5 / 9.0, 10.5 / 7.5, 0.5, −3.5) |
| a 60 Hz sine at −9 dBFS under a 2 kHz one at −24 dBFS | the 2 kHz's sidebands −34 dBc (a JA model with an AC bias carrier at 768 kHz: −27) |
| distortion on two test mixes (peaks −3 dBFS) by band, 500 Hz to 4 kHz | 24 to 28 dB under the music, as that JA model gives (23 to 28); with the highs lifted into the oxide instead, 13 to 18 |
| aliasing: a sine's largest other component, 20 Hz to 20 kHz; 15 kHz at −6 dBFS / 18 kHz at −9 / 9 kHz at 7.5 ips +6 dB drive | 1x −9, 2x −44, 4x −98, 8x −108 dBc / 4x −94 / 4x −56, 8x −99; 5 kHz at 0 dBFS with +12 dB drive: 4x −43, 8x −83 |
| quiet sines (−35 dBFS), 15 ips | 0.00 dB at 1 and 10 kHz; +1.93 dB at 60 Hz |
| the head, quiet noise: peak, dip, ripple, second dip | 15 ips +1.9 dB near 57 Hz, −1.0 at 115, +0.4 at 170, −0.2 at 230; 30 ips +1.5 at 115, −12.6 dB at 20 Hz; 7.5 ips +2.5 at 30 |
| `repro_hf` +6 dB at 2, 4, 8 kHz | 7.5 ips +3.0, +4.8, +5.8; 15 ips +1.3, +3.1, +5.1; 30 ips +0.4, +1.3, +3.6 dB |
| `hum` −60 dB, 60 Hz | −60.0 dB re reference RMS; 120 Hz −63, 60 Hz −67, 240 Hz −69, 360 Hz −73 … 960 Hz −89; NAB 60 Hz −69 |
| hiss at 0 dB, A-weighted (dBFS) | −78.5 (15 ips IEC), −76.0 (NAB), −81.0 (30 ips), −64.5 (`tape` 5, 3.75 ips) |
| the electronics' noise in the floor, 20–60 / 60–200 Hz / above 200 Hz | +10.5 / +4 / 0 dB at 15 ips IEC (−96.1, −97.9 dBFS); +11.7 / +5.3 at 30 ips; +1 / 0 at 3.75 ips, where the tape's hiss covers it |
| wow and flutter at the defaults, a 3 kHz tone | 0.013 % RMS, 0.038 % peak at 15 ips (RMS 0.010 at 30, 0.021 at 7.5, 0.027 at 3.75); with low latency 0.011 % RMS (the wow held to 40 %); wow 0.5 % alone, low latency: scaled, the flutter band 0.000 % |
| scrape, sidebands 100 Hz to 5 kHz off a 10 kHz tone (modulation noise on) | −60 dBc at 10 % (15 ips), −63 (30 ips); −48 at 30 %; −43 at 50 %; −35 at 100 % |
| the Mastering patch on two test mixes (peaks −3 dBFS) | the peaks rounded 0.6 and 1.4 dB, the nonlinear part 36 and 31 dB under the music |
| automation: each parameter jumped mid-sine | no step above the screen's threshold (largest excess 0.010, wow to 2 %) |
| cost (dev player, stereo, 48 kHz) | 3.4 % of a core at 2x, 5.5 to 6 % at 4x, 9.3 to 10 % at 8x, 16 to 18 % at 16x |
| latency | 456 samples at 48 kHz (4x, full), 9 with `latency` low |
| block sizes (`plugin.umber.blocks`) | bit-identical |

Editor (1200 × 880): the machine's face, with the supply and take-up
reels turning at the tape's speed (their packs trading tape over a
30-minute reel), the tape over erase, record and play heads and between
the capstan and its roller, two VU meters with a peak lamp for the 3%
limit, and the speed, standard, tape and oversampling. Record: drive,
tape and bias, the amplifier, HF saturation, the record HF trim and the
output; the headroom and the peaks' rounding; auto gain. Heads and tape:
the bump and its frequency, azimuth, head magnetism, dropouts and their
depth, print-through, crosstalk. Magnetism, the centre: a scope of the
field recorded (the record level, held back by the latency) against the
flux played back (the playback side before the output's level), both
low-passed at 2 kHz and taken at about 8 kHz, the last 384 points drawn
as straight lines in 24 runs brightening to the newest, with a lamp at
the newest; each axis scales to the trail on its own (up at once, down
over about half a second), with the reference level and the tape's 3%
point marked when the field reaches them, and the trail fades when the
input stops. Every control reaches it: the oxide's curve and loop, the
amplifiers' bend, the head bump's phase, the transport's motion, the
noise. Dragged across, the drive; up and down, the bias. Under it the
oversampling (with the rate inside and the best range) and latency.
Playback: speed, emphasis, the response at −20 VU and at +10 VU (the
head, bias's tilt and the trims, each frequency's describing gain
through the oxide's curve and the highs' ceiling), the playback
amplifier and HF trim, hiss and modulation noise. Transport: wow,
flutter, scrape and hum, the tape's displacement over four seconds
(with what the wow is held to at low latency), the mains. The header's
mark is a reel with an umber pack.

Factory patches: Mastering, Mix Bus, Peak Rounding, Tracking, Hot Drums,
Fat Bass, Saturated, Warm Electronics, Underbiased, Cassette, Worn
Machine, Warped, Clean Transfer; on a test mix within 0.9 LU of the dry
signal, on drums down to −1.7 LU for the heaviest.

## 14. Tuner — "Sedge" [`sedge`]

A chromatic tuner, written as a DSP script (ADR-0047:
`plugins/sedge/script/sedge.cpp`). It passes every sample through as it
came (host-smoke checks it bit for bit as a transparent product) unless
`mute` is on, which silences the output to tune on stage. Main stereo or
mono, the channels' mean is what it listens to; no latency, no tail. What
it hears is reported as read-only parameters, so the editor, a host and an
agent (`fx.meters`, `dsp.test`'s `meters`) read the same note.

| key | type | range | curve | default |
|---|---|---|---|---|
| `reference` | Hz | 415..466: the pitch of A4 | log | 440 |
| `temperament` | enum | `equal`, `just`, `pythagorean`, `meantone`, `werckmeister3`, `vallotti` | | `equal` |
| `key` | enum | `c`, `d_flat`, `d`, `e_flat`, `e`, `f`, `f_sharp`, `g`, `a_flat`, `a`, `b_flat`, `b`: the note the temperament is built on; applies unless `equal` | | `c` |
| `window` | ms | 10..200: how much sound each reading listens to | log | 40 |
| `tolerance` | ct | 1..10: within this either way the note is in tune | linear | 3 |
| `threshold` | dBFS | −90..−20: below this input peak nothing is read | linear | −60 |
| `mute` | bool | silence, faded over 10 ms | | false |
| `note` | enum | readonly: `none` until a note is heard, then `c` … `b` as `key`'s | | |
| `octave` | int | readonly, 0..8: the note's octave (A4 is the A above middle C) | | |
| `cents` | ct | readonly, −50..50: the pitch against the note, positive sharp | | |
| `frequency` | Hz | readonly, 0..4000: the pitch heard; 0 until one is | | |

- **One detector.** Each reading takes the last `window` of the input
  (DC removed, low-passed by an eighth-order Butterworth at 2.4 kHz) and
  finds the lag where it best repeats: McLeod's normalised square
  difference, over lags for 25 Hz to 2.2 kHz (A0 to C7) at about 6 kHz
  (the sample rate divided by a whole number). The period is the first
  key maximum (the highest point of each rise above zero after the curve
  first dips below it) within 0.9 of the highest; its parabolic top
  refines it. The stretch is then copied at the full rate, low-passed by
  an eighth-order Butterworth at 1.25 times the pitch found (from rest,
  with up to four periods before it to settle), and the period refined
  there, a lag at a time to the highest and a parabola through the three
  about it. Reading the fundamental alone matters: a string's overtones
  run sharp of it and fade first, so a period read from all of them reads
  sharp and sags as the note dies (on test strings 1 to 3 cents sharp,
  2 to 5 cents of sag over a note; with the fundamental alone 0.2 and
  about 1, the attack's own sharpness settling).
- **When a reading counts.** Readings come every 15 ms, or a quarter of the
  window when that is longer. One counts when the input's peak (falling
  20 dB/s) is at or above `threshold`, the curve's peak at the period is
  at least 0.85, and the sound is still sounding at the window's end: its
  newest whole periods, an eighth of the window or more, hold at least half
  the power of all the reading compares. A pluck's scrape, silence, a note
  dying into noise and a window a note stops in do not count, and the
  display holds what it showed. (Without the last test, what the display
  held after a note was cut off depended on where the cut fell: −6 to −14
  cents for a string at −6.7; with it, within 0.7 of the string, rarely
  2 to 4 on a bass's B0.) A reading of the note shown
  (within half a semitone) moves the display toward it with a 50 ms time
  constant; a reading of another note is shown once the next agrees with
  it (within 0.3 semitones), so one stray reading (an octave on a string
  with a weak fundamental) never shows as a note.
- **Notes.** The pitch is placed against the nearest note as `temperament`
  tunes it on `key`, with A4 at `reference` in every temperament (the
  others' offsets are moved so A's is 0). Just: 5-limit ratios from the
  key (16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8).
  Pythagorean: eleven pure fifths, the wolf on the ninth from the key
  (G♯–E♭ on C). Meantone: quarter-comma, fifths a quarter of the syntonic
  comma narrow, the wolf there too. Werckmeister III: the fifths from the
  key to the sixth degree's (C–G, G–D, D–A on C) and the seventh's
  (B–F♯) a quarter of the Pythagorean comma narrow. Vallotti: the six
  from the fourth (F–C to E–B on C) a sixth of it narrow. Notes are named
  with flats but F♯.
- **Measured** on synthetic plucked strings at 44.1, 48 and 96 kHz
  (overtones stretched and fading first, the attack 4 cents sharp, pick
  noise): with the default window a new note shows 30 to 60 ms after it is
  plucked (guitar E2 to E4), 80 ms where the fundamental is 15 dB under the
  second harmonic, 95 to 125 ms for a bass's B0 and E1; a pluck of the note
  shown keeps it at once; settled, within 0.3 cents of the fundamental; no
  wrong note shown on any. A string swept 35 cents in 3 s is followed
  0.5 cents behind. A 200 ms window on a bass takes 230 ms to show a new
  note and smears a re-pluck for a few hundred milliseconds: the window
  trades answering soon for listening long.
- **Cost:** 0.85 % of a core at 48 kHz in the release build (1.3 % with a
  100 ms window); the work of each reading is spread over the frames before
  the next.

Editor (780 × 572): in the header, the input's level with `threshold` on it
(dragged, wheeled) and Mute. The reading: the note large, with its octave
and a flat or sharp, in the blades' green while in tune; the cents to the
left, coloured by how far off (green within `tolerance`, amber within
three times it or 10 cents, rust past that), and the hertz to the right;
under the note "in tune", "flat", "sharp" or "holding the last note". Then
the cells. Above, a tuft of 21 fine cells a cent apart from −10 to +10,
tallest in the middle, those within `tolerance` tinted; the one nearest the
pitch lights, with an arrow past the end beyond 10 cents. Below, the
coarse row across ±50 cents: a wide middle cell for the tolerance and
eight a side sharing the rest, the one the pitch is in lit, and under it a
mark at the pitch exactly. Everything dims while the display holds.
Under it two panels. Tuning: the temperament and the key as two rows of
pills of one width, each named above (the keys dimmed under equal), and A4
(in tenths of a hertz). Listening: the window, the threshold and the
tolerance. The mark is a tuft of sedge with a seed spike.

Factory patches: Concert A440, A442, Baroque A415, Bass (a 100 ms
window), Stage, Muted, Werckmeister III, Vallotti, Meantone.

## 15. Gain — "Gain" [`gain`]

A level change and nothing else, written as a DSP script (ADR-0047:
`plugins/gain/script/gain.cpp`), for the point in a chain a fader cannot
reach. Named for what it does, the one stock plugin that is not named
after the natural world (ADR-0047). Main stereo or mono in and out, the
same gain on every channel; no latency, no tail.

| key | type | range | curve | default |
|---|---|---|---|---|
| `gain` | dB | −32..+32, smoothed 20 ms | linear | 0 |

- **Smoothing.** The framework's glide (`.smoothed(20)`): a one-pole in
  dB with a 20 ms time constant, run per sample, starting at the sample a
  change arrives (automation events split the block), and snapping to the
  target once within a billionth of the range of it. Every stock level
  glides this way (Hickory's `gain`, the `output`s), so the plugins agree
  with each other. The engine's fader instead ramps linearly in amplitude
  over 10 ms for a live move and follows automation's curve per sample
  without a ramp, since its automation is a continuous curve; a plugin's
  automation arrives as discrete values from any host, so it must glide
  through them. In dB a glide moves evenly to the ear, from −32 to +32 as
  from −6 to 0: a 64 dB jump steps a 50 Hz sine at most 0.0041 between two
  samples at 48 kHz (its own slope at +32 dB), against about 0.6 sent
  straight through.
- At 0 dB the input passes as it came, bit for bit: the block is written
  only while the gain is not exactly 1. Non-finite samples are multiplied
  like any other (Gain keeps no state for them to reach).
- Not a transparent product for host-smoke (`PRODUCT_TRANSPARENT`): its
  smoke parameter is `gain` itself, which host-smoke moves off 0 dB.

Editor (180 × 186): the knob, lit from 0 dB at the top to the value, with
a notch over 0 dB, and its value under it in Plex Mono, the "dB" smaller
and muted (`+6.0 dB`); no name or labels. A drag anywhere in the window turns it (200 units the whole
range, Shift a fifth of that), the wheel by 0.5 dB a notch, a double click
returns it to 0 dB.

No factory patches: 0 dB is its only neutral setting, and any other is a
number an agent or a user types.

## 16. Amp and cabinet — "Hornbeam" [`hornbeam`]

A guitar amplifier and the cabinet it plays into, written as a DSP script
(ADR-0047: `plugins/hornbeam/script/hornbeam.cpp`), every part a model of the
circuit or the physics: no impulse responses, convolution or recorded data
of any kind. Main stereo or mono in and out; latency is the amp's
oversampler's (0 to 5 samples), the same in every chain so a switch never
moves the timing.

| key | type | range | curve | default |
|---|---|---|---|---|
| `voice` | enum | `clean`, `chime`, `crunch`, `lead`, `bass` | | `chime` |
| `gain` | % | 0..100: the gain pot's travel (audio taper) | linear | 45 |
| `bass`, `mid`, `treble` | % | 0..100: the tone stack's pots (bass and treble audio taper, mid linear) | linear | 50, 55, 60 |
| `presence` | % | 0..100: the power amp's feedback lifted above 3 to 4 kHz | linear | 40 |
| `sag` | % | 0..100: how far the supply droops under load | linear | 40 |
| `master` | % | 0..100: the power amp's drive, −20..+20 dB | linear | 55 |
| `cabinet` | enum | `open1x12`, `open2x12`, `closed2x12`, `closed4x12`, `closed4x10`, `closed1x15` | | `open2x12` |
| `speaker` | enum | `alnico`, `vintage`, `modern`, `heavy` | | `alnico` |
| `mic` | enum | `dynamic`, `condenser`, `ribbon` | | `dynamic` |
| `position` | % | 0..100: the cap's centre to the cone's edge | linear | 30 |
| `distance` | m | 0.01..1: from the plane of the cone's rim | log | 0.04 |
| `room` | % | 0..100: how hard the floor and walls are | linear | 20 |
| `chain` | enum | `full`, `amp` (the amp into a speaker's load), `cab` (the cabinet driven clean) | | `full` |
| `channels` | enum | `mono` (the left input, heard on both sides), `stereo` (an amp for each side) | | `mono` |
| `mix` | % | 0..100, latency aligned | linear | 100 |
| `output` | dB | −24..24 | linear | 0 |
| `quality` | enum | `x1`, `x2`, `x4`, `x8`, `x16`: the amp's oversampling (at most 768 kHz inside); a restart | | `x4` |
| `input_level`, `output_level` | dBFS | readonly: peaks, falling 20 dB/s | | |
| `distortion` | % | readonly: what the amp's curves add (below) | | |
| `sag_now` | dB | readonly: the supply's droop this moment | | |

- **Calibration.** 0 dBFS at the input is 2 V at the guitar's jack and a
  preamp grid's bias is 1.5 V, so a DI peaking at −6 dBFS drives the first
  stage about as a humbucker does.
- **Preamp.** By the voice, two to four triode stages and a cathode
  follower. A stage: the coupling capacitor's high-pass (8 to 120 Hz), the
  cathode's bypass as a low shelf (none, or 3 to 6 dB down below 220 to
  400 Hz), the gain into it, its curve, a Miller low-pass (8 to 22 kHz), and
  it inverts. The curve is Child's law in units of the bias:
  ((v + 1.7)^1.5 − 1.7^1.5) / (1.5 √1.7), cut off at −1.7, with v the grid
  held back above 0.8 by grid current, 0.8 + e / (1 + 3e); a cold stage
  (the lead's second) sits 0.6 below. Grid current charges the coupling
  capacitor by the peaks' excess (in about a millisecond, leaking away at the
  coupling's own time), shifting the bias colder by up to 1.5 times the
  stage's share (0.3 to 0.8). A follower is unity up to 1, held above it.
  The gain pot is an audio taper (10 % at half travel) with the voice's
  bright capacitor (0, 120 or 470 pF) across its upper part:
  g (1 + s(1 − g)RC) / (1 + s g(1 − g)RC), R = 1 MΩ. The first stage's
  filters run before the oversampler.
- **Tone stack.** The passive three-knob network: the treble capacitor to the
  treble pot, whose wiper is the output; the slope resistor to the bass and
  mid capacitors; the treble pot's bottom to the bass pot, which runs to the
  mid pot. Solved by nodal analysis with the source's resistance (the plate,
  38 kΩ, or a follower's 1.5 kΩ) and the next grid's 1 MΩ: a third-order
  transfer function whose coefficients are polynomials in the three pots'
  shares, so the knobs lean on each other as real ones do (the mid at 0 is
  the deepest scoop, the bass loads the treble). Bilinear at the oversampled
  rate, redesigned every 16 frames of a glide. Parts by voice (treble,
  bass, mid pot; slope; treble, bass, mid capacitor): clean 250 k, 250 k,
  10 k, 100 k, 250 p, 100 n, 47 n; chime 250 k, 1 M, 25 k, 56 k, 470 p, 22 n,
  47 n; crunch 220 k, 1 M, 22 k, 33 k, 470 p, 22 n, 22 n; lead 250 k, 1 M,
  25 k, 47 k, 250 p, 22 n, 22 n; bass 250 k, 1 M, 25 k, 56 k, 250 p, 47 n,
  47 n.
- **Level.** The preamp's level against gain is held: a Gaussian at −18 dBFS
  RMS through the stages as curves (the stack at its log-averaged level at
  noon) gives the gain that brings it to 0.1 RMS into the power amp, at 65
  points of the gain's travel, and a trim per voice fitted on a guitar's DI
  covers what the static model misses (the filters between stages, the
  grid's charge). Below a fifth of the travel the level is no longer made
  up: the pot fades the amp toward silence, as a real one does. Measured on
  a synthetic DI (−22 dBFS RMS, peaks −6): within 0.3 dB from 25 to 100 %
  gain in every voice; strummed chords within ±3 dB across gain and master.
- **Power amp.** Presence is a first-order high shelf of up to 8 dB above
  3 to 4 kHz before the pair (less feedback above it: more highs, and more
  distortion of them). The push-pull pair is u / (1 + 2(1 − h)u² + u⁴)^¼
  (h by voice: 0, class A, to 0.9) with an offset for a little second
  harmonic, under a supply that sags: B = max(0.35, 1 − sag × depth × e),
  e the output's level (4 ms up, 50 to 120 ms down by voice; depth 0.2 to
  0.5), the drive over B and the ceiling under it. `master` drives it from
  −20 to +20 dB; below half way the level is made up (the pair stays clean
  at the same loudness), above it 60 % of the drive is, so the amp grows
  denser and a little louder up to its ceiling. Then the output
  transformer, as Teasel's (§5: the flux integrated, the derivative of what
  saturation took from it taken away) with a cheaper core, 0.4v + 0.6v /
  √(1 + v²), at 20 to 40 Hz by voice, at the session's rate since its
  harmonics are the bass's.
- **Speaker.** A motor and a cone of five masses on springs: the voice coil,
  the apex (cap and inner cone, 0 to 0.38 of the radius), two rings (to 0.62
  and 0.84) and the rim with half the surround, each of the paper's areal
  density times its area plus a share of the air load; links between them
  tuned by speaker (coil to apex 4 to 7 kHz, outward to 1.1 to 2.2 kHz),
  the spider at the coil and the surround at the rim (60/40 of the
  suspension, which with the moving mass gives the resonance fs), and a
  closed box's air on every ring by its area (ρc²/V, the volume doubled for
  lining and leaks, shared by the box's speakers). Solved into five modes
  (Jacobi rotations), each a state-variable filter driven by the force at
  the coil: the lowest is the cone as a piston at its resonance, the rest
  the breakup. Mode damping: 1/(2 Qms), then each breakup mode's loss
  factor. The force is Bl times the coil's current, through Re (plus the
  amp's output impedance) and the coil's inductance with its eddy currents'
  loss resistance across it; the back EMF a sample late. The air load falls
  away above ka ≈ 1, a first-order shelf on the force. Bl falls and the
  suspension stiffens with the coil's excursion (Bl / (1 + 0.4 (x/X)²), the
  spring's force K x (x/X)², X the speaker's limit): a cranked amp's lows
  compress and thicken, an alnico speaker soonest. The speakers, at
  12 inches (fs, Qms, Qes, Re, Le, rated power, X): alnico 75 Hz, 4, 1.3,
  6.4 Ω, 0.45 mH, 15 W, 2.5 mm (a light, bright cone); vintage 75, 5,
  0.85, 6.4, 0.65, 25 W, 3 mm (soft, damped, rolling off early); modern 85,
  6, 0.6, 6.4, 0.7, 60 W, 3.5 mm (stiff, peaking 2.5 to 5 kHz); heavy 62,
  7, 0.38, 6.0, 0.9, 200 W, 5 mm (stiff, damped, extended). A 10 or 15 inch
  cone scales its masses, radius, links and resonance.
- **Amp to speaker.** The amp drives the speaker through its output
  impedance (clean 3 Ω, chime 10, crunch 5, lead 4, bass 1.5), which raises
  the speaker's Q at its resonance (the thump in the box) and lifts the
  coil's inductive roll-off. At the ceiling the speaker sees √(2 P × 8 Ω)
  for the voice's power (40, 30, 50, 100, 300 W), divided among the box's
  speakers. `chain` `cab` drives it from a voltage source (no output
  impedance, no amp, no oversampler: the input delayed by the latency, 0 dBFS
  as 5 V, about 3 W); `amp` outputs the amp's voltage across a vintage
  speaker in a closed 4x12 as its load (V − Zo i), for an impulse response
  elsewhere.
- **Microphone.** The cone is a cone: the cap sits its depth (4.2 to 5 cm at
  12 inches) behind the rim. Each ring's surface, in 4 radial and 12
  angular steps across the half the mic faces, reaches the mic at
  (pos × radius, distance) by its own path (the capsule's 8 mm radius the
  shortest), as Rayleigh's integral has it: dA / (2πr), delayed r/c. They
  are binned by distance into 3 taps a ring, each with a pressure weight and
  a gradient weight: a mic hears α of the pressure and 1 − α of its gradient
  along its axis, (1 + c/(jωr)) cos θ per path, the c/(jωr) part through
  one leaky integrator (8 Hz): the proximity effect of every path at its own
  distance. Then the far paths, from the cone's far field (Σ area ×
  acceleration): the box's other speakers (two taps each, their near and far
  halves, through a low-pass where ka sin θ = 1.6), an open back's rear wave
  (inverted, round the nearest side, 700 Hz) or a closed box's nearest edge
  (inverted, 0.35, 600 Hz), the floor (0.85 × `room`, 3 kHz), and for the
  figure eight the wall 1.5 m behind the mic (0.7 × `room`). The earliest
  path is taken as no delay. Then the capsule: dynamic (cardioid α 0.5; a
  115 Hz high-pass, Q 0.6; +5 dB at 5.2 kHz; −1 dB at 2 kHz; a 12.5 kHz
  low-pass), condenser (cardioid; 55 Hz; +2.5 dB at 11 kHz; 20 kHz), ribbon
  (figure eight α 0; 20 Hz; +1 dB at 200 Hz; a 7.5 kHz low-pass, Q 0.55).
  The cabinet's gain is set to unity over 100 Hz to 5 kHz (equal weight per
  octave), recomputed with the paths every 128 frames of a glide.
- **What it sounds like** (cab only, measured from the audio): a closed
  4x12's box resonance is near 105 Hz (vintage); close-miked with the
  dynamic it is within 3 dB from about 110 to 400 Hz; the breakup peaks
  between 2 and 5 kHz move with the speaker; it falls 20 to 25 dB in the
  octave above 6 kHz; the cap's centre is brightest, with
  notches that move with the mic; the cone's edge is darker from 3 kHz; at
  30 cm the other speakers comb, and at 1 m a 4x12 beams. An open back
  thins the lows at a distance and keeps them close up. The plot in the
  editor is the same model evaluated at the session's rate, every filter's
  response exactly as the audio has it: on impulses through the cabinet it
  matches the audio to within 0.001 dB (8 frequencies, 3 settings, 44.1, 48
  and 96 kHz).
- **Switching** voice, cabinet, speaker, mic, chain or channels dips the
  output for 5 ms, sets up afresh and comes back in 5 ms.
- **`distortion`**: for each curve (the stages and the pair) the share of
  its output not a scaled copy of its input, from running moments over
  300 ms, combined as √(1 − Π(1 − share²)); the filters between the curves
  do not count.

Measured (48 kHz):

| check | result |
|---|---|
| aliasing, E6 (1318.5 Hz) at −6 dBFS, worst component 20 Hz–20 kHz, x1 / x2 / x4 / x8 | High Gain Rhythm −32 / −36 / −51 / −64 dBc; Classic Crunch −58 / −66 / −86 / −100 dBc |
| the speaker's excursion (at the session's rate), low E at 0 dBFS, cab only | THD up to 3.8 % (alnico, open 1x12), 0.22 % at −12 dBFS; aliases ≤ −97.7 dBc, the measurement's floor at every level |
| the cabinet alone, −30 dBFS | THD below 0.01 % |
| factory patches on a guitar DI at −22 dBFS RMS | within 1.5 dB of the input's level |
| cost (clap-bench, release) | defaults 13.7 refs (Teasel's 15.7 the same run), factory patches 13.3 on average (17.6), high gain 18 to 19, cab only 4.5 to 5.1, automated 29.0 (27.7); with `channels` stereo the defaults cost 25.0 |

Editor (1080 × 768): the header's hornbeam leaf, the name and the input and
output meters. The amp: the voices as pills, gain, bass, mid, treble,
presence, sag and master as dials, and the tone stack's response with
presence and the bright cap as the amp has them (at its oversampled rate),
with the distortion and sag readouts. The cabinet: the cone from the front,
its rings lit by how much of what the mic hears comes from each (the taps'
weights), the mic where `position` puts it (dragged across, wheeled for
distance), the box small with its speakers and the miked one; the speaker
from the side with the mic on a log ruler of its distance (dragged); the
response of the cabinet and mic (or, amp only, of the load), and pills for
the cabinet, speaker and mic, and room. The footer: chain, channels, mix,
output, oversampling and the latency.

Factory patches: Edge of Breakup (the defaults), Glass Clean, Sparkle
Clean, Chime, Classic Crunch, Singing Lead, Hot Rod, High Gain Rhythm,
Scooped Metal, Ribbon Room, Small Combo Squash, Bass Amp, Bass Grit (with
the DI at 40 %), Cab Only 4x12, Cab Only 1x12 Open, Amp Only.

## 17. Acceptance

A plugin is done when its tests, the CLAP validator, the RT sanitizer run,
a REAPER load and automation check, and its skill page exist.
