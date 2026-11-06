# Racks: modular patching

A rack is a patching space on a channel: modules joined by cables, like a
modular synthesizer. Use one when the sound is a *system* rather than a
preset: generative melodies and rhythms, modulation that crosses between
voices, effects with feedback loops, a plug-in played or processed by
modules. For a single instrument or effect, a plug-in is simpler; for DSP of
your own, write a module (`guides/modules.md`) and patch it here.

`generated/rack-modules.md` lists every built-in module's ports and knobs
with their ranges and defaults; read it before patching. `module.describe
{type}` gives the same for any type, the library's too.

## The loop

1. `rack.add {track}` makes a rack on a channel with an Out module (its
   result's `out`) that is the channel's input. Without `track`, it makes a
   new channel.
2. `rack.patch` adds modules by name and the cables between them in one
   step (below). `module.add` and `cable.add` do one at a time.
3. `rack.probe {rack, ports, notes?}` renders a copy offline and describes
   what flows out of each port: pitch and level for audio, gates per beat
   and duty, the notes a pitch visits, a CV's rate. Nothing changes. This
   is how you hear a patch; check every patch with it before rendering.
4. Adjust: `module.set {module, params}`, `cable.set {cable, amount}`,
   `module.set {module, preset}` (a module's factory preset).
5. Render or listen as for any track. Ask the human to listen when it
   matters.
6. Keep what works: `rack.preset.save {rack, name}`; later
   `rack.preset.load {rack, name}` puts it in any rack. A preset carries
   the source of your own modules it uses, and loading it on another
   machine installs them into that library.

## rack.patch

```json
{"rack": "rk_1",
 "modules": {"keys": {"type": "notes"},
             "osc":  {"type": "vco"},
             "filt": {"type": "filter", "params": {"cutoff": 300, "resonance": 30}},
             "env":  {"type": "adsr", "params": {"attack": 5, "decay": 250, "sustain": 40, "release": 300}},
             "amp":  {"type": "vca", "params": {"level": -12}}},
 "cables": [["keys.pitch", "osc.pitch"], ["keys.pitch", "filt.pitch"],
            ["osc.saw", "filt.in"], ["keys.gate", "env.gate"],
            ["env.envelope", "filt.cutoff", 3], ["env.envelope", "amp.cv"],
            ["filt.low", "amp.in"], ["amp.out", "mo_1.l"]]}
```

- A module is what `module.add` takes: `type` (a key from the reference,
  `"vco"`, or an id) or `plugin` (an `fx.scan` id), and `params`,
  `preset`, `label`, `x`, `y`, `source`, `device`.
- A cable is `[from, to]` or `[from, to, amount]`, or `{from, to, amount,
  color}`. An end is `"name.port"` or `"name.knob"`, or `"mo_N.port"` for
  a module already in the rack: the Out from `rack.add` is `"mo_1.l"` in a
  new rack.
- Modules without `x`, `y` are laid out left to right by signal flow.
- One bad name, port or value refuses the whole patch; the error's path
  names it (`/args/cables/3/to`). It returns the ids by name.

In a `batch`, a `${step.field}` reference must be the whole string, so a
cable end cannot be built from one: use `rack.patch`.

## Signals

| Kind | Range | Meaning |
|---|---|---|
| audio | ±1 | Sound. 1 is full scale. |
| pitch | octaves | 0 is C4, 1 is C5, −1/12 is B3. |
| gate | 0 or 1 | High at 0.5 and above, low below 0.25. A trigger is a short gate. |
| cv | −1..1 | Bipolar modulation (an LFO). |
| unipolar | 0..1 | An envelope, a velocity, a level. |
| phase | 0..1 | A ramp: an LFO's or the bar's position. |

An unpatched input reads its normal value (the reference says when it is
not 0: a VCA's `cv` is 1, so it passes until something closes it).

**Levels.** Modules work at full scale, and a chord sums its voices at the
Out, so racks run hot: keep the Out's peak at or below 0 dB (`rack.probe`
on the module feeding it). Lower a VCA's or a mixer's `level`, or the
Out's own `level` (`module.set {module: "mo_1", params: {level: -9}}`).
Delays and reverbs add their tails to the dry sound and can raise the peak
by several dB.

**Polyphony.** A cable carries up to 16 voices. Notes sends one voice per
held note (`voices`, default 8); modules after it run once per voice. A mono
cable reaches every voice of a poly module (one LFO moves every voice's
filter). The Out and a plugin module's audio inputs sum the voices; `poly`
merges, splits and sums cables.

**Modulation.** A cable into a knob moves it by `amount` times the signal:
in octaves for a frequency (`["env.envelope", "filt.cutoff", 3]` opens the
filter three octaves at the envelope's peak), otherwise in the knob's unit
(dB, %, ms, semitones). The default is an octave for a frequency, else half
the knob's range. The knob's own value is the centre. Switches take no
cables.

**Feedback.** A cable that closes a loop is allowed: it carries the
previous sample (`cable.add` reports `feedback: true`). Use it for
feedback FM, Karplus-like loops and self-patched chaos.

## The host modules

| Module | What it does |
|---|---|
| `out` | Audio to the DAW. Each Out is a channel's input: `rack.add` makes one its channel's; `track.set {track, record_rack: "mo_N"}` gives another channel another Out (the input menu lists them by label), so one rack can play several channels. |
| `in` | Audio from the DAW: send tracks to it with `route.add {from: "tr_2", to: "mo_7"}` (the In's id). A rack with an In and an Out is an effect. |
| `notes` | The rack channel's MIDI (its items and, when it monitors, live input) as `pitch`, `gate`, `velocity`, `pressure` per voice, plus `bend` and `modwheel`. `source` plays another track's MIDI; `device` a MIDI input always. |
| `transport` | The song's clock: `clock` at `division` (1/16), `bar` and `beat` triggers, their phases, `playing`, `reset`. Sequencers stay in time with the song through it (or follow the song themselves when nothing drives their clock). |
| `plugin` | A plug-in: `module.add {rack, plugin}`. Instruments play from `pitch` and `gate` (a voice per gate channel: key round(60 + 12 × pitch)). Audio in on `in_l`, `in_r`; out on `out_l`, `out_r`. Its knobs are its plug-in's: `fx.set`, `fx.patch.merge` on its `fx`; a cable into `"synth.cutoff"` (in `rack.patch`) or `fx_N.cutoff` modulates one. |

## Recipes

These were run and probed as written. Each starts from `rack.add {track}`;
the Out is then `mo_1`.

**A subtractive voice** (the `rack.patch` above): Notes plays a saw through
a low-pass filter that tracks the keyboard and opens with the envelope.
Probe `amp.out` with `notes: "voice v; 1: [C3 E3 G3]:h [D3 F3 A3]:h"`:
it reports the chord's pitch and the envelope's sweep.

**Generative bells**: the song's sixteenths through a Euclidean rhythm
clock a shift register; its melody, quantized to C minor, pings a low-pass
gate into a delay and a reverb.

```json
{"modules": {"clock": {"type": "transport"},
             "rhythm": {"type": "euclid", "params": {"steps": 16, "pulses": 7}},
             "melody": {"type": "turing", "params": {"flip": 15, "scale": 2}},
             "key": {"type": "quantizer", "params": {"scale": "minor"}},
             "osc": {"type": "vco"}, "gate": {"type": "lpg"},
             "echo": {"type": "delay"}, "room": {"type": "reverb", "params": {"mix": 25}}},
 "cables": [["clock.clock", "rhythm.clock"], ["rhythm.gate", "melody.clock"],
            ["melody.out", "key.pitch"], ["key.out", "osc.pitch"],
            ["osc.triangle", "gate.in"], ["rhythm.gate", "gate.ping"],
            ["gate.out", "echo.in_l"], ["echo.out_l", "room.in_l"], ["echo.out_r", "room.in_r"],
            ["room.out_l", "mo_1.l"], ["room.out_r", "mo_1.r"]]}
```

Then lower the Out: `module.set {module: "mo_1", params: {level: -9}}`.
`flip` sets how often the melody changes (0 locks a loop, 50 is random);
`pulses` the density. Probe `key.out` to see the notes it visits.

**A plug-in in a rack**: an instrument played by Notes, its output through
a filter an LFO sweeps.

```json
{"modules": {"keys": {"type": "notes"}, "synth": {"plugin": "com.suppleaudio.marram"},
             "sweep": {"type": "lfo", "params": {"rate": 0.5}},
             "filt": {"type": "filter", "params": {"cutoff": 800}}},
 "cables": [["keys.pitch", "synth.pitch"], ["keys.gate", "synth.gate"],
            ["keys.velocity", "synth.velocity"], ["synth.out_l", "filt.in"],
            ["sweep.out", "filt.cutoff", 2], ["filt.low", "mo_1.l"]]}
```

**A rack as an effect**: a frequency shifter swirled by a slow LFO into a
long delay, fed by a send from another track.

```json
{"modules": {"space": {"type": "in"}, "shift": {"type": "shift"},
             "echo": {"type": "delay", "params": {"feedback": 70, "mix": 100}},
             "drift": {"type": "lfo", "params": {"rate": 0.2}}},
 "cables": [["space.l", "shift.in"], ["shift.up", "echo.in_l"],
            ["drift.out", "shift.shift", 5],
            ["echo.out_l", "mo_1.l"], ["echo.out_r", "mo_1.r"]]}
```

Then `route.add {from: "tr_2", to: "<space's id>"}`.

## Generative building blocks

Tags in the reference group the modules. Some starting points:

- **Time**: `transport` (the song), `clock` (ratios, swing), `euclid`,
  `rhythm` (drum patterns by density), `polyrhythm` (four counts against a
  bar), `ratchet` (bursts), `chance` (coin tosses per gate), `logic`,
  `life` (an 8x8 Game of Life as gates).
- **Melody**: `turing` (a looping shift register), `random` (notes with
  memory), `markov` (note-to-note odds, learnable), `cellular` (a 1-D
  automaton), `lsystem` (a melody grown by rewriting rules), `sequencer`
  (sixteen steps, five walks), `looper` (records a CV and a gate in time
  and loops them), `arp`, `chord`, always through `quantizer` for a key.
- **Motion**: `lfo`, `function`, `stages` (a multi-stage envelope, LFO or
  gesture), `adsr`, `drift` (slow random per voice), `chaos` (Lorenz and
  Thomas attractors), `follower`, `slew`, `hold`.
- **Sound**: `vco`, `wavetable`, `operator` (FM), `harmonic`, `morph`,
  `phasor`, `noise`, `string` and `resonator` (physical models), `drum`,
  `granular`.
- **Shape**: `filter`, `ladder`, `lpg`, `vowel`, `comb`, `fold`, `drive`,
  `shaper`, `shift`, `pitchshift`, `chorus`, `delay`, `reverb`.
- **Utility**: `vca`, `mixer`, `scale` (attenuverters), `matrix` (a 4x4
  modulation matrix), `math`, `poly`,
  `pitch`, `switcher`, `macro` (eight performance knobs), `scope`.

Ideas that work: clock a melody source from a Euclidean rhythm so notes
and rests interlock; feed a slow `chaos` or `drift` into a filter and a
`turing`'s `flip` so the patch evolves; send a `follower` on one track's
audio (through an In) to another sound's VCA for ducking; patch a module's
output back into its own modulation input for feedback.

## Your own modules

`module.write {name, source}` compiles a module into the user's library
and makes it a type racks can use at once; `module.edit {name, edits}`
changes it and rebuilds every module playing it (a version that does not
compile keeps the last good one playing). `module.check {source}` compiles
without saving; `module.get {type}` shows any module's source;
`module.fork {type, id}` copies a built-in module into the library under a
new id to change it. The language is in `guides/modules.md`. Verify a new
module with `rack.probe` before using it.

## Control and automation

- `env.add {module, param, points}` automates a module's knob over the
  song (values in the knob's unit, a choice by its key).
- `module.learn.set {module, param, midi}` lets a MIDI controller turn a
  knob; `module.learn.list` and `module.learn.remove` manage them. A
  plugin module's knobs use `fx.learn.set` and `env.add {fx}`.
- `macro` gathers up to eight controls on one module: cable its outputs to
  the knobs you want to perform, then automate or learn the macro.
- `module.randomize {module, amount}` nudges a module's knobs.

## Limits

- Cables into a plug-in's parameters (`synth.cutoff` on a plugin module)
  are CLAP modulation: the parameter must be modulatable, at most 16 cables
  per plug-in, mono (a poly cable's first voice), and VST3 plug-ins take
  none. `amount` is a fraction of the parameter's range (default 0.5).
- A plug-in's latency inside a rack is not compensated.
- Racks do not follow a track's playrate changes.
- Recording an armed channel whose input is a rack's Out is not supported.
