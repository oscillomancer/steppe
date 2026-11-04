# Spec: Pollard — the stock drum sampler

**Status:** Implemented: sixteen pads, the voice, choke groups, variation
per hit, eight extra outputs and the editor, with twenty-four factory hits
in seven kits. The drum sampler guide (`skill/guides/drum-sampler.md`)
describes it for agents.
**Plugin:** `plugins/pollard`, CLAP id `com.suppleaudio.pollard`,
instrument. A DSP script compiled natively (ADR-0047),
`plugins/pollard/script/pollard.cpp`.

Pollard plays sixteen pads, one sound each (a file of the user's or one of
its factory hits) on sixteen keys from C2 up, the General MIDI drum map's
kick to its ride. Each pad is one instance of Coppice's engine
(`coppice.md`): the region of its file, forward or reversed, one-shot or
gated, through an envelope and a state-variable filter, at its level and
pan, to the main output or one of eight more. Pads in a choke group cut
each other off; Vary makes each hit a little different. It has no
predecessor. Velocity layers, round robin across several files, a step
sequencer, swing, pad sends and effects are left out; Coppice plays one
sound across the keys.

## 1. Signal flow (per hit)

```
 key ── pad (key − First Key; outside 0..15 silent) ── choke: the group's voices fade over 10 ms
        │ its variation: pitch, level, start (Vary)
        ▼
   region (start..end, reversed or not) ── read (cubic, at 1x/2x/4x, the speed from Tune)
   ── svf (off, low, band, high pass) ── amp (envelope × velocity × level, pan)
        ▼
   the pad's output (Main, Out 1..8): its voices summed ── down to 1x ── −6 dB ── Output ── safety limit
```

- **The engine** is Coppice's, byte for byte: the text between the
  markers `// --- engine: shared with Pollard, byte for byte ---` and
  `// --- end of the engine ---` is the same in both scripts, and
  `plugin.pollard.engine-copy` fails when it is not. A change to the engine
  is made in both. Pollard uses it without loops, slices or the ladder.
- **Files** arrive resampled to the session's rate and are measured as
  they load, as in Coppice; a pad reads as many channels as its file has.
- **Oversampling** as Coppice's (`voice.oversampling`: `x1`, `x2` or
  `x4`; 0, 1 or 2 samples of latency, reported). Each output is brought down once, so a pad sent
  to Out 3 costs no more than one on the main output, and an output that
  has been silent for 4096 samples skips its downsampler.
- **Control** every 16 samples of a voice's life, glided between, so a
  render does not depend on the block size.
- **Voices:** 32 for all pads. A stolen voice fades out over a few
  milliseconds; a region's last millisecond fades out where it ends. The
  start has no fade, so a hit's attack stays sharp.
- **Choke.** A hit on a pad in a group (1 to 8) fades out every sounding
  voice of the group's pads over 10 ms, its own pad's included, so a
  repeated crash or open hat cuts itself off as well as the hats cutting
  each other.
- **Variation** (`Vary`, per pad): each hit moves its pitch by up to
  ±0.25 st × Vary, its level by up to ±2.5 dB × Vary, and its start up to
  1 ms × Vary later in the region, the three drawn from the pad's hit
  count since the plugin started or was reset, so a render is the same
  every time.
- **Level:** a fixed −6 dB before `output` leaves room for hits together.
  The factory hits peak at −1 dBFS, so the default kick at velocity 0.8
  peaks near −12.6 dBFS. Marram's safety limit then leaves each output
  untouched below −1 dBFS and bends it toward full scale above.
- **Outputs.** The main stereo output and eight more (`Out 1` to `Out 8`,
  each stereo, `Info.outputs`), so Steppe's `fx.add` makes the track 18
  channels wide (ADR-0021). A pad on an extra output leaves the main mix.
  The main output's meter is the main mix only.
- **Tail** (CLAP's `tail`): the longest gated release, or what is left of
  a hit that plays on at its speed, plus 20 ms.

## 2. Parameters

Keys are the patch keys; units as in every semantic plugin. Defaults in
**bold** (pads differ: §4). An inactive parameter keeps its value and is
left out of descriptions.

### 2.1 Each pad: `pad1` … `pad16`

The file is the property `padN` (§4); the parameters are `padN.<field>`,
grouped as "Pad N", named "Pad N Level" and so on.

| field | unit / options | range | default | applies |
|---|---|---|---|---|
| `level` | dB | −60..12 | the kit's | |
| `pan` | % | −100..100 | **0** | |
| `tune` | st (and hundredths): the file played faster or slower | −24..24 | the kit's | |
| `start` | % of the file | 0..100 | **0** | |
| `end` | % of the file | 0..100 | **100** | |
| `reverse` | the region from its end to its start | | **off** | |
| `play` | `one_shot` (whatever the key does), `gated` (released at the key's release) | | **one_shot** | |
| `attack` | ms | 0..500 | **0** | |
| `decay` | ms to silence; at its top (Full) no fade, the region plays to its end | 5..10000 | **Full** | |
| `release` | ms, once the key is up | 1..5000 | **100** | play gated |
| `filter` | `off`, `lowpass`, `bandpass`, `highpass` (state-variable) | | **off** | |
| `cutoff` | Hz | 20..20000 | **20000** | filter on |
| `resonance` | % | 0..100 | **0** | filter on |
| `velocity` | %: at 100, a soft hit is 40 dB below a hard one | 0..100 | **70** | |
| `vary` | %: each hit's pitch, level and start (§1) | 0..100 | **0** | |
| `choke` | `off`, `group1` … `group8` | | **off** | |
| `output` | `main`, `out1` … `out8` | | **main** | |

The region is start..end (an end before the start plays the frames
between them). The cutoff has one default, 20 kHz, which leaves a low pass
open; a band or high pass at 20 kHz passes little, so a patch that turns
one on sets the cutoff too. In the editor, choosing a type while the
cutoff is still at its default moves it to that type's open point (low
pass 20 kHz, band pass 1 kHz, high pass 20 Hz); a cutoff that has been
moved stays where it is.

### 2.2 Every pad's

| key | unit / options | range | default |
|---|---|---|---|
| `pads.key` | the key of pad 1; pad N is N − 1 keys above | 0..112 | **36** (C2) |
| `voice.oversampling` | `x1`, `x2`, `x4` (restarts) | | **x2** |
| `output` | dB, every output | −36..12 | **0** |

## 3. The kits and the factory hits

A pad's file is the property `padN`, a file reference: a path, or
`factory:<name>` for one of the hits inside the plugin
(`semantic-params-extension.md`, "Factory files"). Twenty-four hits, 48 kHz
16-bit mono FLAC, 633,818 bytes in all, are rendered from Marram by
`tools/factory-samples/render.py` from `plugins/pollard/samples/recipes.json`
(reproducibly: `--check` renders them again and compares the bytes);
first-party content, CC0 (`plugins/pollard/samples/LICENSE`). Each peaks at
−1 dBFS and starts with a 0.5 ms fade in, so a pad's start is silent.

| family | hits (`factory:<family>-<hit>.flac`) |
|---|---|
| `analog-` | `kick` 0.7 s, `boom` 2 s (a long tuned kick), `snare` 0.5, `rim` 0.15, `clap` 0.45 (three bursts and a tail), `hat` 0.2, `open-hat` 1, `tom` 0.6, `cowbell` 0.5, `cymbal` 2, `shaker` 0.22 |
| `fm-` | `kick` 0.6, `snare` 0.45, `hat` 0.2, `open-hat` 0.9, `tom` 0.6, `ride` 2.5, `zap` 0.4 |
| `hand-` | `conga` 0.5, `bongo` 0.35, `clave` 0.15, `woodblock` 0.2, `tambourine` 0.5, `cabasa` 0.25 |

Their names and sounds are permanent identifiers, as parameter keys are
(`plugins/AGENTS.md`). The script player has no factory files: there every
pad shows as missing; the built plugin plays them.

**The default kit** (Analog Kit), on the GM keys from C2:

| pad (key) | sound | level, tune | |
|---|---|---|---|
| 1 (36) | `analog-kick` | 0 dB | |
| 2 (37) | `analog-rim` | −6 | side stick |
| 3 (38) | `analog-snare` | −2 | |
| 4 (39) | `analog-clap` | −3 | |
| 5 (40) | `analog-snare` | −4, +3 st | snare 2 |
| 6, 8, 10, 12, 13, 15 (41 … 50) | `analog-tom` | −3, at −5, −3, −1, +1, +3, +5 st | the six GM toms |
| 7 (42) | `analog-hat` | −6 | closed hat, choke 1 |
| 9 (44) | `analog-hat` | −8, −2 st, decay 40 ms | pedal hat, choke 1 |
| 11 (46) | `analog-open-hat` | −6 | choke 1 |
| 14 (49) | `analog-cymbal` | −8 | crash |
| 16 (51) | `fm-ride` | −9 | ride |

**Factory kits:** Analog Kit (the default), Boom Kit (the long kick, the
toms lower and shorter), Dusty Kit (every pad through a resonant low pass,
Vary 35 %), FM Kit (the `fm-` hits, the zap on the side stick), Hand
Percussion (congas, bongos, claves, woodblocks, shaker, cabasa, tambourine
and cowbells, panned, no choke), Reverse Kit (snare 2, the open hat and the
crash reversed, the crash gated) and Multi-Out Kit (the kick on Out 1,
snares on Out 2, hats on Out 3, toms on Out 4, cymbals on Out 5, rim and
clap on Out 6).

## 4. The editor

The script's own, 1240 by 776 design units, drawn and edited in the script
(`editor()`, `draw()`, `pointer()`) in the stock look; the audio code sends
each pad's waveform, each voice's place, each pad's level now and the
output's peaks as `Shared` members.

- **Header**: the name and a pollard; what it plays ("sixteen pads from
  C2", following First Key); voices sounding of the limit, and the main
  output's meter.
- **Pads**: four rows of four, pad 1 at the bottom left as on a drum
  machine, each with its number and key, its tune when not 0 (pads that
  share a file, as the toms do, read apart by it), its choke group and
  output when set, `REV` when reversed, its waveform lit while it plays,
  and its file slot (`e.file()`: a click chooses, a right click opens its
  menu with the factory hits, a drop loads). Pressing a pad selects it and
  plays it (`playNote`), harder the higher it is pressed; letting go ends
  the note.
- **The selected pad**: its waveform with flags for start and end (drag;
  Shift finely; double click resets) and a line where each of its voices
  plays; a close-up 2.5 ms either side of its start; Sound (level, pan,
  tune, velocity, vary, one-shot or gated, reverse), Envelope (attack,
  decay, release, its shape drawn), Filter (off, LP, BP, HP; cutoff and
  resonance), Choke and Output.
- **Every pad's**: first key, oversampling, output.

Controls that do not apply are dimmed in place. Every dial drags (Shift:
finely), turns by the wheel and resets on a double click.

## 5. Performance

Release build, one core, 48 kHz, `clap-bench` (2026-10-29), whose notes
fall on keys 40 to 79, so about a third of them play a pad: 0.48 % of a
core for the default kit (5.17 refs), the factory kits 5.00 refs on
average (Hand Percussion 4.03 to Multi-Out Kit 5.82, which keeps six
outputs' downsamplers running). Coppice the same way: 0.24 % (2.28 refs);
Marram: 0.56 % (6.05 refs). The release `Pollard.clap` is 3,193,528 bytes,
the factory hits 633,818 of them.
