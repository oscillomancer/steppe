# Coppice: sampler

Coppice (`com.suppleaudio.coppice`) is Steppe's sampler: one sound, a file
or one of its factory samples, played pitched across the keys with a
loop (classic), whole at each key (one-shot), or cut into slices, one a key
(slice). Each voice then has one filter, an amp envelope and Env 2, with
two LFOs, four macros and the player's controllers to move them through
`routes`, as Marram's (`guides/sound-design.md`). The exact keys, units and
ranges come from `plugin_describe` (sandbox) or `fx_describe` (DAW); this
guide is about choosing the sound and the mode. The spec is
`docs/specs/coppice.md`.

## The sound

The sound is the file property `sample` in the patch, a path or a factory
sample:

```json
{"properties": {"sample": "factory:break.flac"}, "params": {"sample.mode": "slice"}}
```

| factory sample | what | root |
|---|---|---|
| `factory:keys.flac` (the default) | an FM electric piano's note, 4 s | C4 (60) |
| `factory:pad.flac` | a detuned saw pad, stereo, 4 s | C3 (48) |
| `factory:pluck.flac` | two saws plucked through a ladder, 1.8 s | C4 (60) |
| `factory:bass.flac` | a saw bass, 2.2 s | C2 (36) |
| `factory:bell.flac` | an FM bell, 3 s | C5 (72) |
| `factory:voice.flac` | a formant "a" with a delayed vibrato, 4 s | C4 (60) |
| `factory:break.flac` | two bars of drums at 100 BPM, 4.8 s | |

A file of your own is an absolute path to a WAV, AIFF or FLAC; it is
resampled to the session's rate as it loads. **Set `sample.root` to the
file's pitch** (60 is middle C): Coppice does not detect it, and does not
read a WAV's `smpl` chunk. Sedge (`guides/tuner.md`) tells you the note of
a sound you render. Factory samples only play in the built plugin: in the
script player (a scratch copy of the script) a `factory:` path is refused,
so give a scratch copy a real path.

## Choose the mode

| You want | `sample.mode` | then |
|---|---|---|
| an instrument across the keys | `classic` | `loop.mode` `forward` or `ping_pong` for a sound that sustains while held |
| a hit or a phrase that plays out whatever the key does | `one_shot` | `sample.keytrack` off for the same pitch on every key |
| a break or a phrase chopped onto keys | `slice` | `slice.by`, `slice.key`, `slice.play` |

**Classic.** `sample.start` and `sample.end` (% of the file) set the
region. A loop (`loop.start`, `loop.end`, also %) holds while the key is
down; then the amp releases, or with `loop.release` ("Play On") playback
runs on past the loop to the region's end. Forward loops crossfade
(`loop.crossfade`, ms), and `loop.snap` (on by default) moves both ends to
zero crossings: start with a long, steady part of the sound and a 30 to
100 ms crossfade, then listen for a pulse at the loop's rate. Ping-pong
needs no crossfade and suits pads and strings.

**Slice.** Slices start at `slice.key` (36, C2) and go up a key each; a key
past the last slice is silent, and the default notes of `dsp.test` (around
C4) may fall past them, so play keys from `slice.key` up (`notes` with
`key`). `transients` cuts at the hits (`slice.sensitivity`: more finds
quieter hits); `grid` cuts `slice.count` equal parts, right for a loop cut
to its bar. `slice.play`: `trigger` plays a slice to its end, `gate` while
the key is held, `thru` on through the rest of the region while held. With
`voice.mode` `mono` each slice cuts off the last, the classic chop. A
slice keeps its pitch; `sample.transpose` and `voice.tune` still move it.

**Reverse** plays the region (and its loop) backwards; **Normalize**
brings the file's peak to 0 dBFS, for a quiet recording.

## Routes worth knowing

Sources and destinations are Marram's, fewer of them (`plugin_describe`
lists them). Particular to a sampler:

- `"velocity>sample.start": {"amount": "10 %"}`: harder notes start later,
  past the attack; a negative amount starts softer notes later.
- `"random>sample.start": {"amount": "3 %"}`: each note starts a little
  elsewhere, so repeated notes are not identical.
- `"lfo1>voice.tune"` under `"via": "modwheel"`: vibrato on the wheel.
- `env2.pitch` (a fixed amount): a pitch drop on each note, a drum's thud.

## Factory patches

| patch | what it shows |
|---|---|
| Tine Keys | the keys through a ladder that key tracking and velocity open |
| Air Pad | the pad on a forward loop with a 300 ms crossfade; a slow free LFO on the cutoff under the mod wheel |
| Ping-Pong Pad | the pad on a ping-pong loop |
| Reverse Swell | the pad reversed as a one-shot, swelling into the note |
| Harp | the pluck through a ladder Env 2 opens, a long release |
| Glide Bass | the bass, legato with glide, through a ladder Env 2 opens |
| Chime | the bell; vibrato on the mod wheel |
| Chopped Break | the break cut at its transients, mono |
| Gated Break | the break on a 16-slice grid, gate |
| Break Thru | the break's slices playing on through it, mono |
| Vowel Chop | the voice on an 8-slice grid |
| First Hit | the break's first 6 % as a one-shot, the same pitch on every key |

## Pitfalls

- A sound in the wrong key: the root is wrong. Render A4 and read
  `pitch_hz` (dsp.test's `note` signal reports it).
- A click at the loop: lengthen `loop.crossfade`, or move the loop into a
  steadier part; ping-pong for a sound whose ends do not match.
- Slices that cut a hit in two: lower `slice.sensitivity`, or use `grid`
  on a loop cut to its bar.
- A chord much louder than a note: Coppice leaves 9 dB of room, so a full
  chord of a loud sample still reaches the safety limit above −1 dBFS;
  lower `sample.gain` or `output`.
