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
   `from_beat` starts a fresh render at that beat, so what a patch builds
   over time (a looper's recording, life's generations, a drift's path)
   needs one render from the start, up to 10 seconds.
4. Adjust: `module.set {module, params}`, `cable.set {cable, amount}`,
   `module.set {module, preset}` (a module's factory preset).
5. Render or listen as for any track. Ask the human to listen when it
   matters.
6. Print generative notes to MIDI: `rack.capture {rack, pitch, gate,
   velocity?, track, beats, quantize?}` renders the patch offline and writes
   what a pitch and gate pair plays (a note per voice) as a MIDI item on
   any channel. Edit it with `midi.edit`, read it with `midi.get`, play it
   through any instrument. The rack is not changed.
7. Keep what works: `rack.preset.save {rack, name}`; later
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
- Modules without `x`, `y` are laid out left to right by signal flow, in
  columns ordered to cross few cables, wrapped to about a window's shape.
  `rack.arrange {rack}` tidies a whole rack the same way (one undo step).
- A name labels its module on the face: `"kick"` shows "Kick", `"vco2"`
  "VCO 2", while a type's own key (`"vco"`) adds no label. Give names that
  say the role (`"bass_osc"`, `"hat_env"`) so a person can read the rack.
- One bad name, port or value refuses the whole patch; the error's path
  names it (`/args/cables/3/to`). It returns the ids by name.

In a `batch`, a step named with `"as": "p"` is read by later steps as
`${p.field}`, down a path: `${p.modules.loop}` is the id `rack.patch` gave
the module named `loop` (for `env.add {module}` or `module.set`). A
reference must be the whole string, so a cable end or a probe port cannot
be built from one: use `rack.patch` for cables, and run `rack.probe` after
the batch with the ids it returned.

## Copy and paste

`module.copy {modules: ["mo_3", "mo_4"]}` stores modules in that order and
all internal cables, including plug-in parameter cables. It captures the
plug-ins' current settings, labels, knobs, bypass, Notes source/device and
learn bindings. The clipboard belongs to this engine and survives deleting
the originals and undo; it is not saved in a project.

`rack.paste {rack: "rk_2", x: 400, y: 40}` recreates the clip with fresh ids,
returning `{modules: [...], cables: [...]}`. One undo removes the entire
paste. The clip's top-left is placed at x/y; omitted coordinates use free
space to the right, y=40. External routes and automation are not copied.
If a type, plug-in or Notes source is missing, paste returns an actionable
error and changes nothing. Restore it or copy a different selection.

`rack.levels {rack: "rk_1"}` returns `{modules: {mo_N: {port: level}}}` for
every module's known outputs, including outputs with no cable. Each level
is the latest rack block's peak absolute sample across all poly channels,
linear (1 is full scale; larger is allowed). A stopped or silent output reads
0. No telemetry subscription or visible rack window is needed. Anticipative
levels may lead the audio you hear; use them for activity, not timing.

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

<!-- host/engine/tests/test_rack_recipes.cpp patches every JSON block here as printed. -->

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

**Polyrhythmic drums**: four drums on Polyrhythm's lanes, 4 against 3
against 5 against 7 in a bar; Hits accents the kick and hat where lanes
meet.

```json
{"modules": {"lanes": {"type": "polyrhythm", "params": {"width": 10}},
             "kick": {"type": "drum", "params": {"model": "kick", "level": -6}},
             "clap": {"type": "drum", "params": {"model": "clap", "level": -6}},
             "rim": {"type": "drum", "params": {"model": "rimshot", "level": -9}},
             "hat": {"type": "drum", "params": {"model": "closed_hat", "level": -10}},
             "mix": {"type": "mixer", "params": {"pan_3": -45, "pan_4": 45, "master": -6}}},
 "cables": [["lanes.gate_1", "kick.trigger"], ["lanes.gate_2", "clap.trigger"],
            ["lanes.gate_3", "rim.trigger"], ["lanes.gate_4", "hat.trigger"],
            ["lanes.hits", "kick.accent"], ["lanes.hits", "hat.accent"],
            ["kick.out", "mix.in_1"], ["clap.out", "mix.in_2"], ["rim.out", "mix.in_3"],
            ["hat.out", "mix.in_4"], ["mix.left", "mo_1.l"], ["mix.right", "mo_1.r"]]}
```

Probed for 4 s: `lanes.any` 32 starts (16 a bar), `mix.left` peak −7 dB.
`count_1`…`count_4` set the lanes; `span` stretches them over 2 or 4 bars.

**An evolving melody**: an L-system tune in D dorian, looped every two bars
(`loop` 16 symbols), plucked through a low-pass gate into a dotted-eighth
delay; a slow drift cabled into `move_b` makes each pass a variation.

```json
{"modules": {"tune": {"type": "lsystem", "params": {"depth": 6, "loop": 16, "rest": "c",
                                                      "scale": "dorian", "root": "d", "octave": -1}},
             "wander": {"type": "drift", "params": {"rate": 0.2, "voices": 1}},
             "osc": {"type": "vco"}, "gate": {"type": "lpg", "params": {"decay": 180}},
             "amp": {"type": "vca", "params": {"level": -12}},
             "echo": {"type": "delay", "params": {"sync": "n8d"}},
             "room": {"type": "reverb", "params": {"mix": 25}}},
 "cables": [["tune.pitch", "osc.pitch"], ["osc.triangle", "gate.in"], ["tune.gate", "gate.ping"],
            ["wander.out", "tune.move_b", 3], ["gate.out", "amp.in"], ["amp.out", "echo.in_l"],
            ["echo.out_l", "room.in_l"], ["echo.out_r", "room.in_r"],
            ["room.out_l", "mo_1.l"], ["room.out_r", "mo_1.r"]]}
```

Probed for 10 s: `tune.start` 3, ten notes from A2 to C4 that change from
pass to pass (without the drift cable each pass repeats), `room.out_l`
peak −3 dB. `depth` and the `rule_` knobs grow other strings.

**A modulation web**: Matrix mixes a Lorenz attractor (A), a looping
Stages gesture (B) and a four-bar ramp (C) into a low drone's filter,
pulse width, level and reverb mix; D, unpatched, adds offsets.

```json
{"modules": {"storm": {"type": "chaos", "params": {"rate": 0.2}},
             "gesture": {"type": "stages", "params": {"mode": "loop", "speed": 0.5}},
             "sweep": {"type": "polyrhythm", "params": {"span": "bars_4", "count_1": 1}},
             "web": {"type": "matrix", "params": {"a_to_1": 50, "c_to_1": 80, "b_to_2": 60, "d_to_2": -30,
                                                  "b_to_3": 40, "d_to_3": 60, "a_to_4": 50}},
             "osc": {"type": "vco", "params": {"coarse": -24}},
             "filt": {"type": "filter", "params": {"cutoff": 250, "resonance": 55}},
             "amp": {"type": "vca", "params": {"level": -12}}, "room": {"type": "reverb"}},
 "cables": [["storm.x", "web.a"], ["gesture.out", "web.b"], ["sweep.ramp_1", "web.c"],
            ["web.out_1", "filt.fm"], ["web.out_2", "osc.pulse_width", 40],
            ["web.out_3", "amp.cv"], ["web.out_4", "room.mix", 30],
            ["osc.pulse", "filt.in"], ["filt.low", "amp.in"], ["amp.out", "room.in_l"],
            ["room.out_l", "mo_1.l"], ["room.out_r", "mo_1.r"]]}
```

Probed for 10 s: `web.out_2` ±0.3 at the gesture's 0.5 Hz, `web.out_3`
0.6 to 1, `filt.low` C2, `room.out_l` peak −8 dB. Turn `a_to_1`…`d_to_4`
to reroute; below 0 inverts.

**A song that writes itself**: Changes plays chords in D dorian under a
pad; Phrase writes calls and answers, fitted to each chord by Fit; Counter
plays a bass line against them; Form's sections open the pad's filter
with their energy and ask Phrase for a new call at each change.

```json
{"modules": {"song": {"type": "form", "params": {"bars": "bars_4"}},
             "prog": {"type": "changes", "params": {"key": "d", "mode": "dorian", "octave": -1}},
             "tune": {"type": "phrase", "params": {"key": "d", "mode": "dorian", "renew": 0}},
             "snap": {"type": "fit"}, "low": {"type": "counter", "params": {"key": "d", "mode": "dorian"}},
             "lead": {"type": "vco"}, "pluck": {"type": "lpg"},
             "bass": {"type": "vco", "params": {"coarse": -12}}, "thump": {"type": "lpg"},
             "pad": {"type": "vco"}, "dark": {"type": "filter", "params": {"cutoff": 300}},
             "mix": {"type": "mixer", "params": {"level_3": -12, "master": -9}}},
 "cables": [["prog.chord", "snap.chord"], ["tune.pitch", "snap.pitch"], ["song.change", "tune.write"],
            ["snap.out", "lead.pitch"], ["lead.triangle", "pluck.in"], ["tune.gate", "pluck.ping"],
            ["snap.out", "low.lead"], ["tune.gate", "low.lead_gate"],
            ["low.pitch", "bass.pitch"], ["bass.triangle", "thump.in"], ["low.gate", "thump.ping"],
            ["prog.chord", "pad.pitch"], ["pad.saw", "dark.in"], ["song.energy", "dark.cutoff", 4],
            ["pluck.out", "mix.in_1"], ["thump.out", "mix.in_2"], ["dark.low", "mix.in_3"],
            ["mix.left", "mo_1.l"], ["mix.right", "mo_1.r"]]}
```

Probed for 10 s: `snap.out` only the chords' tones, `low.interval` 3 to
16 semitones below the lead, `song.edge` then `song.a` (intro, verse),
`mix.left` peak −4 dB. Form's position is the song's bar, so a probe
`from_beat` lands in the right section; `plan` and `bars` reshape it.

## Generative building blocks

Tags in the reference group the modules. Some starting points:

- **Time**: `transport` (the song), `clock` (ratios, swing), `euclid`,
  `rhythm` (drum patterns by density), `polyrhythm` (four counts against a
  bar), `ratchet` (bursts), `chance` (coin tosses per gate), `logic`,
  `life` (an 8x8 Game of Life as gates), `bounce` (a bouncing ball's
  quickening hits), `dust` (random triggers with no clock, in bursts).
- **Melody**: `turing` (a looping shift register), `random` (notes with
  memory), `markov` (note-to-note odds, learnable), `cellular` (a 1-D
  automaton), `lsystem` (a melody grown by rewriting rules), `sequencer`
  (sixteen steps, five walks), `looper` (records a CV and a gate in time
  and loops them), `canon` (delayed, transposed copies of a line),
  `changes` (a chord progression in a key), `phrase` (calls and answers
  that come home), `counter` (a second line against a lead), `arp`,
  `chord`, always through `quantizer` for a key or `fit` for the chord
  playing.
- **Structure**: `form` (a song's sections over minutes: gates for verse,
  chorus and bridge, an energy arc, fills).
- **Motion**: `lfo`, `function`, `stages` (a multi-stage envelope, LFO or
  gesture), `adsr`, `drift` (slow random per voice), `chaos` (Lorenz and
  Thomas attractors), `swarm` (coupled LFOs that fall into step),
  `follower`, `slew`, `hold`.
- **Sound**: `vco`, `wavetable`, `operator` (FM), `harmonic`, `morph`,
  `phasor`, `noise`, `string` and `resonator` (physical models), `drum`,
  `granular`.
- **Shape**: `filter`, `ladder`, `lpg`, `vowel`, `comb`, `eq`, `fold`,
  `drive`, `shaper`, `shift`, `pitchshift`, `freeze` (a held spectrum),
  `vocoder` (a voice's bands on a carrier), `chorus`, `phaser`, `delay`,
  `reverb`, `glitch` (repeats, reverses, tape stops), `orbit` (a circling
  panner, stereo to eight outputs).
- **Dynamics**: `compressor` (its Key input ducks by another sound),
  `transient` (each hit's attack and sustain).
- **Utility**: `vca`, `mixer`, `scale` (attenuverters), `matrix` (a 4x4
  modulation matrix), `math`, `poly`,
  `pitch`, `switcher`, `macro` (eight performance knobs), `keygates`
  (notes to a gate per key: a drum kit played from notes), `scope`.

Ideas that work: clock a melody source from a Euclidean rhythm so notes
and rests interlock; feed a slow `chaos` or `drift` into a filter and a
`turing`'s `flip` so the patch evolves; duck a sound by one track's audio
(through an In) into a `compressor`'s Key, or a `follower` on it into the
sound's VCA; patch a module's output back into its own modulation input
for feedback.

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
