# Hornbeam: guitar amp and cabinet

Use `com.suppleaudio.hornbeam` to turn a guitar's or a bass's DI into a
miked amp: from glassy cleans through edge of breakup and crunch to high
gain, into an open or closed cabinet heard through a dynamic, condenser or
ribbon mic placed on the cone. Its cabinet alone (`chain` `cab`) puts a
pedal's or a preamp's signal through a speaker; its amp alone (`chain` `amp`)
feeds an impulse response elsewhere. Patches use `supple.hornbeam-patch/1.0`,
with physical values under `params`; a merge changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `voice` | `clean` (two stages, a scooped stack, firm power), `chime` (class A, no feedback: chime and bloom), `crunch` (two stages and a follower), `lead` (four tight stages, one biased cold), `bass` (deep iron, headroom) | chime |
| `gain` | %, 0…100: the preamp's gain pot (audio taper); the level holds as it turns, below 20% the amp fades toward silence | 45 |
| `bass`, `mid`, `treble` | %, 0…100: the passive tone stack's pots, which lean on each other as real ones do: `mid` 0 is the deepest scoop | 50, 55, 60 |
| `presence` | %, 0…100: the power amp's top end (less feedback above 3 to 4 kHz: more highs, and more of them distorted) | 40 |
| `sag` | %, 0…100: the supply drooping under load: compression and bloom after loud notes | 40 |
| `master` | %, 0…100: the power amp's drive; below 50 it stays clean at the same level, above it breaks up, denser and a little louder | 55 |
| `cabinet` | `open1x12`, `open2x12`, `closed2x12`, `closed4x12`, `closed4x10`, `closed1x15` | open2x12 |
| `speaker` | `alnico` (light, bright, chiming; breaks up early), `vintage` (warm, smooth, rolls off early), `modern` (forward upper mids, tight), `heavy` (flat, extended, clean at high power) | alnico |
| `mic` | `dynamic` (cardioid, presence rise, its own low cut), `condenser` (cardioid, open and extended), `ribbon` (figure eight, soft top, most proximity) | dynamic |
| `position` | %, 0…100: where the mic points: the cap's centre (bright, peaky) to the cone's edge (dark) | 30 |
| `distance` | m, 0.01…1: close is full and direct (more proximity bass); back off and the cone's paths blend, the other speakers comb and the room comes in | 0.04 |
| `room` | %, 0…100: how hard the floor (and, for the ribbon, the wall behind it) reflects | 20 |
| `chain` | `full`, `amp` (the amp's voltage into a speaker's load, for an IR elsewhere), `cab` (the cabinet driven clean, no amp at all) | full |
| `channels` | `mono` (the left input through one amp, heard on both sides), `stereo` (an amp per side, twice the cost) | mono |
| `mix` | %, 0…100: the dry DI under the amp, latency aligned | 100 |
| `output` | dB, −24…24 | 0 |
| `quality` | `x1` … `x16`: the amp's oversampling; restarts the plugin | x4 |
| `distortion` | %, readonly: what the amp's curves add to the music now | |
| `sag_now` | dB, readonly: the supply's droop this moment | |
| `input_level`, `output_level` | dBFS, readonly: peaks | |

## Levels

0 dBFS at the input is 2 V at the guitar's jack: a DI peaking around −6 to
−12 dBFS drives the amp as a guitar would. Gain changes the distortion, not
the loudness (within a fraction of a dB on a guitar part); master changes
the power amp's breakup and adds a few dB above half way. Every factory
patch comes out within about 1.5 dB of a DI's level. Read `distortion` to
set gain by a number: about 5% is clean with a little hair, 15–30% edge of
breakup to crunch, over 60% high gain.

## Recipes

Edge of breakup (the defaults): `{"params": {}}`.

A clean with sparkle, a little room:

```json
{"params":{"voice":"clean","gain":55,"bass":45,"mid":35,"treble":70,"presence":55,"master":45,"cabinet":"open1x12","position":20,"distance":0.03}}
```

Classic crunch into a closed 4x12:

```json
{"params":{"voice":"crunch","gain":55,"mid":65,"master":60,"cabinet":"closed4x12","speaker":"vintage","position":35,"distance":0.025}}
```

Tight high gain for rhythm (keep `sag` low, the mic close and near the cap):

```json
{"params":{"voice":"lead","gain":70,"mid":50,"presence":55,"sag":20,"master":50,"cabinet":"closed4x12","speaker":"modern","position":20,"distance":0.02,"room":10}}
```

A bass amp with the DI blended under it:

```json
{"params":{"voice":"bass","gain":85,"bass":60,"mid":60,"master":75,"cabinet":"closed1x15","speaker":"heavy","position":50,"distance":0.08,"mix":60}}
```

The cabinet only, for a pedal's or a preamp's signal:

```json
{"params":{"chain":"cab","cabinet":"closed4x12","speaker":"vintage","distance":0.025,"room":10}}
```

## Placing the mic

- Darker or brighter without touching the amp: move `position` (0 bright
  and peaky, 60–90 darker and smoother) before reaching for `treble`.
- Fizz on high gain: move `position` out (40–70), try the `vintage`
  speaker, or lower `presence`; the cabinet already falls 20 to 25 dB in
  the octave above 6 kHz.
- Boom close up is proximity: a ribbon or condenser at 2–5 cm swells the
  lows; back off to 10–30 cm or use the dynamic.
- Air and size: `distance` 0.3–1 m with `room` 40–70 (an open back thins
  the lows at a distance, as a real one does).

## Checking it

`analyze_track` before and after, or `exec {"command": "dsp.test"}` with `audio_file` (a DI)
and its `meters` (`distortion`, `sag_now`). The editor draws the
cabinet's response as the audio has it, so `exec {"command": "dsp.preview"}` with a patch shows
what a mic move does to the spectrum.
