# Pollard: drum sampler

Pollard (`com.suppleaudio.pollard`) is Steppe's drum sampler: sixteen pads
on sixteen keys from C2 (36) up, each one sound, a file or one of its
factory hits, played one-shot or gated through its own envelope and
filter, at its level and pan, to the main output or one of eight more.
Pads in a choke group cut each other off. The exact keys, units and ranges
come from `plugin_describe` (sandbox) or `fx_describe` (DAW); this guide is
about building a kit and playing it. The spec is `docs/specs/pollard.md`;
for one sound across the keys, use Coppice (`guides/sampler.md`).

## Playing it

The default kit sits on the General MIDI drum keys, so the notation's
drum names play it (`guides/notation.md`, Drums):

| pad | key | notation | default sound |
|---|---|---|---|
| 1 | 36 | `kick` | analog kick |
| 2 | 37 | `rim` | side stick |
| 3 | 38 | `snare` | snare |
| 4 | 39 | `clap` | clap |
| 5 | 40 | `snare2` | snare, 3 st up |
| 6, 8, 10, 12, 13, 15 | 41, 43, 45, 47, 48, 50 | `tom_floor`, `n43`, `tom_low`, `tom_mid`, `n48`, `tom_high` | one tom at −5 to +5 st |
| 7 | 42 | `hat` | closed hat (choke 1) |
| 9 | 44 | `pedal` | pedal hat (choke 1) |
| 11 | 46 | `ohat` | open hat (choke 1) |
| 14 | 49 | `crash` | crash |
| 16 | 51 | `ride` | ride |

Keys outside 36..51 are silent: `kick2` (35), `china`, `ride_bell`,
`tamb`, `cowbell`, `shaker`, `clave` and `cabasa` fall past the pads. Use
`n<key>` for the pad that holds the sound (the Hand Percussion kit puts its
tambourine on pad 11, `n46`), or move every pad with `pads.key`. Velocity
matters: at the default `velocity` 70 %, a soft hit is 28 dB below a hard
one; write accents (`X`) and ghosts (`o`).

## The kit

Each pad N has the file property `padN` and the parameters `padN.level`,
`padN.tune`, `padN.decay` and so on:

```json
{"properties": {"pad1": "factory:analog-boom.flac", "pad3": "/home/me/snare.wav"},
 "params": {"pad1.tune": -2, "pad3.level": -3, "pad3.vary": 30}}
```

Factory hits (`factory:<name>.flac`):

| family | hits |
|---|---|
| `analog-` | `kick`, `boom` (long, tuned), `snare`, `rim`, `clap`, `hat`, `open-hat`, `tom`, `cowbell`, `cymbal`, `shaker` |
| `fm-` | `kick`, `snare`, `hat`, `open-hat`, `tom`, `ride`, `zap` |
| `hand-` | `conga`, `bongo`, `clave`, `woodblock`, `tambourine`, `cabasa` |

A file of your own is an absolute path to a WAV, AIFF or FLAC. Factory
hits only play in the built plugin: in the script player (a scratch copy
of the script) every pad is missing until it gets a real path.

## A pad's sound

- **Tune** (st, hundredths too) plays the file faster or slower: one tom
  file makes a set of toms a few semitones apart, as the default kit does.
- **Start**, **End** (% of the file) and **Reverse** choose the region:
  a later start skips a soft attack; a reversed cymbal swells into a beat.
- **Play**: `one_shot` plays the region whatever the key does; `gated`
  fades through `release` once the key is up, so the note's length sets
  the hit's (a drum grid's hit lasts one step unless `len` says more).
- **Decay** fades the hit to silence in that time; at its top (Full) it
  plays to the region's end. A short decay tightens a hat or a snare.
- **Filter**: `lowpass`, `bandpass` or `highpass` with `cutoff` and
  `resonance`. The cutoff defaults to 20 kHz, open for a low pass but
  almost closed for a band or high pass: **when you turn on a band or high
  pass, set `cutoff` too** (a high pass at 100 to 300 Hz thins a hat or a
  snare's body). The editor moves an unmoved cutoff to 1 kHz or 20 Hz when
  a type is chosen there; a patch does not.
- **Vary** makes repeated hits differ a little: up to ±25 cents, ±2.5 dB
  and 1 ms of start at 100 %. 20 to 40 % takes the machine-gun edge off a
  hat or a snare roll; a render is still the same every time.

## Choke and outputs

- `padN.choke` `group1` … `group8`: a hit fades the group's other
  sounding pads, and its own earlier hit, in 10 ms. The default kit's
  hats are in group 1, so the closed and pedal hats stop the open one. Put
  a crash in a group of its own to stop it by hitting it again.
- `padN.output` `out1` … `out8` sends the pad to an extra stereo output
  and out of the main mix. In Steppe the track has 18 channels once
  Pollard is on it (`fx_add`); route the outputs from there to treat a
  kick or the toms on their own. The Multi-Out Kit spreads the kit over
  Out 1 to Out 6.

## Factory kits

| kit | what it shows |
|---|---|
| Analog Kit | the default: GM keys, toms from one file, hats choked |
| Boom Kit | a long tuned kick, lower and shorter toms, a tight hat |
| Dusty Kit | every pad through a resonant low pass, Vary 35 % |
| FM Kit | the FM hits, a zap on the side stick |
| Hand Percussion | congas, bongos, claves, woodblocks, shaker, cabasa, tambourine and cowbells, panned |
| Reverse Kit | snare 2, the open hat and the crash reversed, the crash gated |
| Multi-Out Kit | the kick, snares, hats, toms, cymbals, rim and clap on Out 1 to Out 6 |

## Pitfalls

- Silence on some hits: the keys fall outside 36..51, or past the pads
  after `pads.key` moved. `dsp.test`'s default notes (around C4) play no
  pad; give `notes` with pad keys.
- A band or high pass that seems to mute a pad: its cutoff is still at
  20 kHz.
- An open hat that rings under the closed one: they are not in the same
  choke group.
- A pad missing from the main mix: its `output` is an extra one.
- Many hits together much louder than one: Pollard leaves 6 dB of room,
  and a gentle limit bends the output above −1 dBFS; lower pad levels or
  `output`.
