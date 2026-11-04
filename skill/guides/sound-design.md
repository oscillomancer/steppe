# Marram: synthesizer

Marram (`com.suppleaudio.marram`) is Steppe's synthesizer: up to four
oscillators, two filters, drive and an amp per voice, with three envelopes,
three LFOs, four macros and the player's own controllers to move them. Its
patch is `supple.marram-patch/1.0`: physical values under `params`, flat
keys (`filter1.cutoff`), and `routes` for modulation. The exact keys, units
and ranges come from `plugin_describe` (sandbox) or `fx_describe` (DAW); this
guide is about *how to get to a sound*. The workflow below uses sandbox
tools; in the DAW use `fx_*` patch tools and `render_mix` / `analyze_compare`
(see the host table in `SKILL.md`). The optimizer is sandbox-only.

Effects are not part of it. Put the stock effects after it in the chain:
Bracken for echoes (`guides/delay.md`), Ghyll for space
(`guides/reverb.md`), Pollia for phasing (`guides/phaser.md`), Teasel
for heavier saturation (`guides/drive.md`), Yarrow for EQ
(`guides/eq.md`).

## The division of labour

You choose **structure**: which oscillators and of what type, which
filters, whether there is drive, and which **routes** connect sources to
parameters. The **optimizer** tunes the continuous numbers against a
target. You are good at the first and bad at nudging twenty coupled knobs;
the optimizer is the opposite. So: start from the factory sound nearest to
what you want, change its structure, measure, and hand the numbers to
`optimize_start`. It accepts only active float parameters whose flags include
`modulatable` and exclude `readonly`; check `plugin_describe` with
`detail: "full"`. Fixed envelope amounts and unison detune are set manually.

## Anatomy in one screen

```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "voice.unison": 3, "voice.unison_detune": "12 ct",
   "osc1.type": "analog", "osc1.wave": "saw",
   "osc2.type": "sub", "osc2.wave": "sine", "osc2.level": "-9 dB",
   "filter1.type": "ladder", "filter1.cutoff": "900 Hz", "filter1.resonance": "30 %",
   "filter1.env": "+2.5 oct",
   "amp.attack": "5 ms", "amp.decay": "400 ms", "amp.sustain": "60 %", "amp.release": "300 ms",
   "env2.decay": "250 ms", "env2.sustain": 0},
 "routes": {"velocity>filter1.cutoff": {"amount": "+1 oct"}}}
```

- **Oscillators** `osc1`..`osc4`, each `oscN.type`: `analog` (`wave` saw,
  square, pulse with `width`, triangle, sine; osc2..4 can `sync` to osc1),
  `wavetable` (`table`: harmonic_ramp, pwm, formant, bell, organ, digital,
  spectral_comb; `position` sweeps its 16 frames; harmonic_ramp and
  digital are the analog saw at 0 %, digital then jumping through crushed,
  sparse and 1-bit waves), `fm` (a sine operator:
  `ratio`, `feedback`), `noise` (`color`), `sub` (`octave` −1 or −2), or
  `off`. Osc 1 starts as an analog saw, the others off. `oscN.to` sends it to
  `filter1`, `filter2`, `both` or `direct` (around the filters).
- **FM** between any two: `oscN.fm` is the depth (an index: peak phase swing
  in radians) and `oscN.fm_from` the modulator. A modulator at its minimum
  level (−60 dB) is silent and still modulates: that is how a pure carrier
  and modulator pair is made.
- **Filters** `filter1`, `filter2`: `ladder` (warm low-pass, `slope` db12 or
  db24), `svf` (`mode` lowpass, bandpass, highpass, notch), `comb`
  (`cutoff` is its pitch; `feedback`, `damping`), `formant` (`vowel` 0..4 for
  a, e, i, o, u, blended; `shift`). Filter 1 starts as a ladder, filter 2 off.
  `filters.routing` is `serial` or `parallel` while both are on.
- **Fixed modulation**, no routes needed: `filterN.env` (octaves at Env 2's
  peak), `filterN.keytrack` (% from C4), `filterN.velocity`, `env2.pitch`
  (semitones at the peak, every oscillator), `env2.fm` (how much of every FM
  depth follows Env 2), `amp.velocity`.
- **Saturation** is the ladder's: `filterN.drive` pushes it harder (its
  level is held about even). For grit after a filter, set filter 2 to a
  ladder wide open (`cutoff` 20 kHz, `resonance` 0 %, `slope` db12) and
  drive it. Marram has no effects: chain Teasel, Bracken, Ghyll after it.
- **Voice**: `mode` poly, mono or legato; `polyphony`, `glide`, `unison`
  (copies of each analog, wavetable and FM oscillator) with `unison_detune`
  and `unison_spread`, `drift` (slow pitch and cutoff wander), `oversampling`.

In `patch_merge`, omitted keys keep their values; a merge changes only what
it names. `patch_set` replaces the patch, resetting omitted parameters to
defaults. A parameter that does not apply (filter 2's cutoff while filter 2
is off) is kept and listed under `inactive`: switch its module on first.

## Routes

`"<source>><parameter>": {"amount": …}`, optionally `"via": "<source>"`
to scale it by a second source (vibrato under the mod wheel).

- **Sources**, per voice: `amp`, `env2`, `env3` (0..1), `lfo1`..`lfo3`
  (−1..1), `velocity`, `key` (−1..1 across the keyboard, 0 at middle C),
  `random` (per note), `pressure`, `slide`. For all voices: `macro1`..`macro4`
  (knobs to automate) and `modwheel`, `breath`, `expression`, `aftertouch`,
  `pitchbend`.
- **Parameters routes move**: `voice.tune`; each oscillator's `level`, `pan`,
  `fine` (reaching ±4800 ct), `width`, `position`, `ratio`, `feedback`, `fm`;
  each filter's `cutoff`, `resonance`, `feedback`, `damping`, `vowel`,
  `shift`, `drive`; `amp.level`, `amp.pan`;
  the envelopes' times; each LFO's `rate`. Choices, switches, unison, glide
  and the output are not routed.
- **The amount is in the parameter's unit** at the source's full value:
  `oct` for frequencies (cutoff, LFO rate), `ct` for `voice.tune` and `fine`,
  `dB` for levels, `%` for percentages, `ms` for envelope times.
  `"env3>osc2.fine": {"amount": "+24 st"}` sweeps two octaves.
- Env 3 exists only for routes; Env 2 has its fixed amounts and is a source
  too. An LFO's `mode` is `note` (each note's own, from its start; `delay`
  fades it in) or `free` (one for every voice, running on); `sync` with a
  `division` follows the song.

## Start from a factory sound

`preset_load {instance, name, source: "factory"}`, then
`patch_get {instance, detail: "changed"}` to see what it sets. Measure loudness
with your phrase: its notes, velocity and duration affect the result.

| Factory sound | Structure worth borrowing |
|---|---|
| Dune Pad | two detuned saws, unison 3, slow Env 2 on a ladder, a free 0.12 Hz LFO on the cutoff; mod wheel opens it |
| Pluck Bass | saw and a sine sub, retriggered; Env 2 snaps the cutoff (`filter1.env` +3.5 oct) |
| Acid Line | legato with glide, ladder at 80 % resonance, an open ladder in filter 2 driving it |
| Supersaw | unison 7 at 28 ct, 12 dB ladder; mod wheel opens it |
| Sub Bass | one retriggered sine, no filter |
| Tine Piano | FM: a −60 dB modulator at 1× and a quiet one at 14×; `env2.fm` decays the bark; velocity deepens it |
| Glass Bell | FM at ratio 3.5 (inharmonic), long decays |
| Vowel Lead | formant filter; a delayed vibrato on `voice.tune`; a slow LFO and the mod wheel move the vowel |
| Strings | unison 5 with drift, saw and pulse through an SVF, delayed vibrato |
| Noise Snare | noise through two SVFs and a triangle body whose pitch Env 2 drops |
| Drawbar Organ | the organ wavetable through a driven open ladder, a free LFO on pan and tune (rotary shimmer) |
| Wobble Bass | a synced LFO in eighths on the cutoff; mod wheel speeds it |
| Sync Lead | osc2 synced to a silent osc1; Env 3 sweeps its pitch |
| Pulse Pad | two pulses whose widths two LFOs move |
| Table Sweep | Env 3 and a drifting LFO through a wavetable; Macro 1 darkens it |

## The workflow

1. `plugin_load` Marram, load the nearest factory sound, and `patch_merge`
   the structure you want.
2. `render` a phrase that exercises the sound (a held chord for pads,
   repeated short notes for basses and plucks) with `"analyze": "summary"`.
3. Change **one idea** at a time with `patch_merge`, render, and `compare`
   with the previous render. `compare` also reports `loss.total`, the score
   `optimize_start` minimizes (lower is closer to the reference).
4. When the structure is right, run `optimize_start` with the 3–8 parameters
   that matter (`filter1.cutoff`, `env2.decay`) and a target: a reference
   render or file, or a profile of descriptor ranges. Wait with
   `exec {command: "job.wait", args: {job}}`; once `status` is `"done"`, use
   `patch_merge {instance, patch: <result.patch>}`.
5. `patch_get {instance, detail: "changed"}` is the short form to report;
   `plugin_describe {instance, detail: "summary"}` gives a prose summary for
   the human, routes included.

## Which knob moves which number

| You want | Change | Watch |
|---|---|---|
| brighter / darker | `filter1.cutoff`, `filter1.env`, the wave or table | `spectral.centroid_hz`, band levels |
| more bite on attack | `filter1.env` and `env2.decay` | centroid trajectory, `envelope.attack_ms` |
| softer start | `amp.attack` | `envelope.attack_ms` |
| shorter / longer tail | `amp.decay`, `sustain`, `release` | `envelope.decay20_ms`, release |
| wider | `voice.unison` with `unison_spread`, `oscN.pan` | `stereo.width` |
| thicker | `unison_detune`, a detuned second oscillator, `drift` | band levels, flatness |
| more weight | a `sub` oscillator, `drive` | `sub` and `low` bands |
| movement | an LFO or Env 3 routed to cutoff, `position`, `width` or `voice.tune` | `spectral.flux` |
| inharmonic, metallic | `fm` with a non-integer `ratio`, the `bell` table, a `comb` filter | `harmonic.inharmonicity` |
| vowel-like | the `formant` filter or table; route an LFO or the mod wheel to `vowel` or `position` | band peaks |
| playable expression | `velocity`, `modwheel`, `pressure` routed to cutoff, level or FM depth | renders at two velocities |

## Pitfalls

- **Switch a module on before its settings matter.** Settings of an `off`
  oscillator or filter are kept but do nothing; the result's `inactive` names
  them.
- **Routes to what does not move** (a choice, unison, the output) are refused;
  the error lists the parameters routes reach. An amount past what the
  parameter can move is refused with its unit and reach.
- **Use the fixed amounts first.** An envelope on the cutoff is
  `filter1.env`, not a route; routes are for everything else.
- **Loudness:** don't win by getting louder; the optimizer leaves `output`
  alone for this reason. Compare at matched loudness.
- **Resonance:** on the ladder 40 % is a gentle bump, 70 % clearly resonant,
  and it sings near 100 %. On the SVF 40 % is about Q 1.4, 70 % about Q 5.6.
- **Oversampling:** `x2` suits almost everything; `x4` for heavy FM, sync
  or drive, `x1` when CPU matters more than aliasing.
  Changing it restarts the plugin.

## Recipes (starting points, not answers)

Each is a complete patch for `patch_set`, the factory sound named after it;
the line after it names the parameters worth optimizing.

**Warm pad** (Dune Pad)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "voice.unison": 3, "voice.unison_spread": "70 %", "voice.drift": "25 %",
   "osc1.fine": "-7 ct", "osc2.type": "analog", "osc2.fine": "+7 ct",
   "filter1.cutoff": "1.2 kHz", "filter1.resonance": "25 %", "amp.attack": "600 ms",
   "amp.decay": "1.2 s", "amp.release": "1.5 s", "env2.attack": "1.5 s",
   "env2.decay": "3 s", "env2.sustain": "30 %", "env2.release": "2 s",
   "lfo1.rate": "0.12 Hz", "lfo1.mode": "free"},
 "routes": {"lfo1>filter1.cutoff": {"amount": "+0.6 oct"},
            "modwheel>filter1.cutoff": {"amount": "+1.5 oct"}}}
```
Optimize: `filter1.cutoff`, `env2.attack`, `env2.decay`, `amp.attack`.

**Plucky bass with a filter snap** (Pluck Bass)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "osc1.level": "1 dB", "osc1.retrigger": true, "osc2.type": "sub",
   "osc2.level": "-2 dB", "osc2.wave": "sine", "osc2.retrigger": true,
   "filter1.cutoff": "600 Hz", "filter1.resonance": "30 %", "filter1.keytrack": "50 %",
   "filter1.env": "+3.5 oct", "amp.decay": "450 ms", "amp.sustain": "50 %",
   "amp.release": "120 ms", "amp.velocity": "60 %", "env2.decay": "250 ms",
   "env2.sustain": "0 %", "env2.release": "200 ms"}}
```
Optimize: `filter1.cutoff`, `filter1.resonance`, `env2.decay`, `amp.decay`.
Set `filter1.env` manually.

**Acid bass (legato, glide, resonant snap)** (Acid Line)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "voice.mode": "legato", "voice.glide": "70 ms", "osc1.level": "0 dB",
   "osc1.retrigger": true, "filter1.cutoff": "320 Hz", "filter1.resonance": "80 %",
   "filter1.env": "+3 oct", "filter1.velocity": "+1 oct", "filter2.type": "ladder",
   "filter2.cutoff": "20 kHz", "filter2.resonance": "0 %", "filter2.slope": "db12",
   "filter2.drive": "9 dB", "amp.level": "+1.2 dB", "amp.decay": "300 ms",
   "amp.sustain": "70 %", "amp.release": "60 ms", "env2.decay": "220 ms",
   "env2.sustain": "0 %"}}
```
Optimize: `filter1.cutoff`, `env2.decay`, `filter2.drive`.
Set `filter1.env` manually.

**Sub bass (a clean fundamental)** (Sub Bass)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "osc1.level": "-10 dB", "osc1.wave": "sine", "osc1.retrigger": true,
   "filter1.type": "off", "amp.attack": "3 ms", "amp.decay": "300 ms",
   "amp.sustain": "100 %", "amp.release": "120 ms", "amp.velocity": "30 %"}}
```
Optimize: usually nothing; `"filter1.type": "ladder"` wide open with
`filter1.drive` adds audible harmonics.

**Supersaw lead** (Supersaw)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "voice.unison": 7, "voice.unison_detune": "28 ct", "voice.unison_spread": "90 %",
   "osc1.level": "-4.5 dB", "filter1.cutoff": "6 kHz", "filter1.resonance": "5 %",
   "filter1.slope": "db12", "filter1.env": "+1 oct", "amp.attack": "5 ms",
   "amp.decay": "500 ms", "amp.sustain": "85 %", "amp.release": "400 ms",
   "env2.decay": "800 ms", "env2.sustain": "40 %"},
 "routes": {"modwheel>filter1.cutoff": {"amount": "+2 oct"}}}
```
Optimize: `filter1.cutoff`. Set `voice.unison_detune` manually.

**Metallic bell (inharmonic partials)** (Glass Bell)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "osc1.type": "fm", "osc1.level": "-7 dB", "osc1.fm": 3.5, "osc1.retrigger": true,
   "osc2.type": "fm", "osc2.level": "-60 dB", "osc2.ratio": 3.5, "osc2.retrigger": true,
   "filter1.type": "off", "amp.attack": "1 ms", "amp.decay": "6 s",
   "amp.sustain": "0 %", "amp.release": "2.5 s", "env2.attack": "0 ms",
   "env2.decay": "2.5 s", "env2.sustain": "0 %", "env2.release": "2.5 s",
   "env2.fm": 70.0}}
```
Optimize: `osc2.ratio`, `osc1.fm`, `env2.decay`, `amp.decay`.

**FM electric piano** (Tine Piano)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "osc1.type": "fm", "osc1.level": "-1 dB", "osc1.fm": 2.5, "osc1.retrigger": true,
   "osc2.type": "fm", "osc2.level": "-60 dB", "osc2.retrigger": true, "osc3.type": "fm",
   "osc3.level": "-34 dB", "osc3.ratio": 14.0, "osc3.retrigger": true,
   "filter1.type": "off", "amp.attack": "1 ms", "amp.decay": "4.5 s",
   "amp.sustain": "0 %", "amp.release": "450 ms", "amp.velocity": "70 %",
   "env2.attack": "0 ms", "env2.decay": "1.6 s", "env2.sustain": "15 %",
   "env2.release": "400 ms", "env2.fm": 100.0, "lfo1.rate": "3.5 Hz",
   "lfo1.mode": "free"},
 "routes": {"lfo1>amp.pan": {"amount": "+35 %", "via": "modwheel"},
            "velocity>osc1.fm": {"amount": 1.5}}}
```
Optimize: `osc1.fm`, `osc3.level`, `env2.decay`, `amp.decay`.

**Noise snare** (Noise Snare)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "osc1.type": "noise", "osc1.level": "0 dB", "osc2.type": "analog",
   "osc2.level": "-3 dB", "osc2.coarse": -5, "osc2.wave": "triangle",
   "osc2.to": "direct", "osc2.retrigger": true, "filter1.type": "svf",
   "filter1.cutoff": "6 kHz", "filter1.env": "+0 oct", "filter2.type": "svf",
   "filter2.cutoff": "350 Hz", "filter2.mode": "highpass", "amp.attack": "0 ms",
   "amp.decay": "190 ms", "amp.sustain": "0 %", "amp.release": "120 ms",
   "amp.velocity": "60 %", "env2.attack": "0 ms", "env2.decay": "45 ms",
   "env2.sustain": "0 %", "env2.pitch": "+12 st"}}
```
Optimize: `filter1.cutoff`, `amp.decay`, `osc2.level`.

**Vowel lead (a formant filter that moves)** (Vowel Lead)
A slow free LFO moves the vowel between e and i (each vowel has a loudness of
its own, so a wider sweep swells and fades).
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "voice.mode": "legato", "voice.glide": "40 ms", "voice.unison": 2,
   "voice.unison_detune": "8 ct", "voice.unison_spread": "30 %", "osc1.level": "-2 dB",
   "filter1.type": "formant", "filter1.vowel": 1.5, "filter1.keytrack": "30 %",
   "amp.attack": "30 ms", "amp.decay": "400 ms", "amp.sustain": "85 %",
   "lfo1.rate": "5.5 Hz", "lfo1.delay": "350 ms", "lfo2.rate": "0.3 Hz",
   "lfo2.mode": "free", "lfo2.phase": "90 deg"},
 "routes": {"lfo1>voice.tune": {"amount": "+25 ct"},
            "modwheel>filter1.vowel": {"amount": 2.0},
            "lfo2>filter1.vowel": {"amount": 0.5}}}
```
Optimize: `filter1.vowel`, `lfo2.rate`.

**String ensemble** (Strings)
```json
{"schema": "supple.marram-patch/1.0",
 "params": {
   "voice.unison": 5, "voice.unison_detune": "18 ct", "voice.unison_spread": "80 %",
   "voice.drift": "35 %", "osc1.level": "-9 dB", "osc2.type": "analog",
   "osc2.level": "-13 dB", "osc2.wave": "pulse", "osc2.width": "30 %",
   "filter1.type": "svf", "filter1.cutoff": "3.5 kHz", "filter1.keytrack": "40 %",
   "filter1.env": "+0 oct", "amp.attack": "350 ms", "amp.decay": "1 ms",
   "amp.sustain": "100 %", "amp.release": "900 ms", "lfo1.rate": "5.2 Hz",
   "lfo1.delay": "600 ms"},
 "routes": {"lfo1>voice.tune": {"amount": "+10 ct"},
            "modwheel>filter1.cutoff": {"amount": "+1 oct"}}}
```
Optimize: `filter1.cutoff`, `amp.attack`. Set `voice.unison_detune` manually.
