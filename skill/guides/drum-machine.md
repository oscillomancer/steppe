# Drumlin: the drum machine

Drumlin (`com.suppleaudio.drumlin`) is Steppe's drum machine: it
synthesizes realistic drums, without samples, on sixteen pads, each on a
MIDI note of its own. Its sounds come in two kits, classic and studio. The
exact keys, units and ranges are in `plugin_describe` / `fx_describe`; this
guide says what they do. For a kit from your own samples, use Pollard
(`guides/drum-sampler.md`).

## Which note plays which drum

Every pad has a `padN.key`, the MIDI note that plays it, beside its
`padN.sound` in `fx_describe`'s summary. The default kit sits on General
MIDI's drum keys, the notation's drum names, so a drum part written with
`voice drums ch 10 grid s` and `kick`, `snare`, `hat` … plays as written:

| pad | key | notation | default sound (Classic Kit) | Studio Kit |
|---:|---:|---|---|---|
| 1 | 36 | `kick` | Classic Kick | Studio Kick |
| 2 | 37 | `rim` | Classic Rimshot | Studio Cross Stick |
| 3 | 38 | `snare` | Classic Snare | Studio Snare |
| 4 | 39 | `clap` | Classic Clap | Studio Clap |
| 5 | 41 | `tom_floor` | Classic Tom, −5 st | Studio Tom, −5 st |
| 6 | 42 | `hat` | Classic Closed Hat (choke 1) | Studio Closed Hat (choke 1) |
| 7 | 45 | `tom_low` | Classic Tom, −2 st | Studio Tom, −2 st |
| 8 | 46 | `ohat` | Classic Open Hat (choke 1) | Studio Open Hat (choke 1) |
| 9 | 47 | `tom_mid` | Classic Tom, +1 st | Studio Tom, +1 st |
| 10 | 49 | `crash` | Classic Crash | Classic Crash |
| 11 | 50 | `tom_high` | Classic Tom, +4 st | Studio Tom, +4 st |
| 12 | 51 | `ride` | Classic Ride | Classic Ride |
| 13 | 54 | `tamb` | Classic Tambourine | Classic Tambourine |
| 14 | 56 | `cowbell` | Classic Cowbell | Studio Open Cowbell |
| 15 | 63 | `n63` (GM's open high conga) | Classic Conga | Studio Block |
| 16 | 69 | `cabasa` | Classic Cabasa | Classic Cabasa |

The factory kits (patches): Classic Kit (the default), Classic Tight,
Classic Low, Classic High, Classic Exact (Vary 0), Classic Percussion, Classic
Separate Outs, Studio Kit and Studio Percussion (the studio cowbells on the
tom keys; the block, the ducks and the explosion on pads 13 to 16).

A key no pad has is silent (`kick2` 35, `snare2` 40, `pedal` 44, `china`,
`ride_bell`, `splash`, `crash2`, `shaker`, `clave`). Move a pad to any key
with `padN.key` (two pads on one key: the lower-numbered one plays). A
kit's patch that changes sounds keeps the keys unless it says otherwise, so
read `padN.key` and `padN.sound` before writing a part for a kit you did
not set up.

## A pad

```json
{"params": {"pad6.sound": "studio_closed_hat", "pad6.decay": 60, "pad3.tune": 2, "pad3.vary": 30}}
```

- **Sound** (`padN.sound`). The classic kit: `classic_kick`,
  `classic_snare`, `classic_rimshot`, `classic_clap`, `classic_closed_hat`,
  `classic_open_hat`, `classic_tom`, `classic_conga`, `classic_cabasa`,
  `classic_tambourine`, `classic_cowbell`, `classic_crash`, `classic_ride`.
  The studio kit: `studio_kick`, `studio_snare`, `studio_bright_snare`,
  `studio_deep_snare`, `studio_tight_snare`, `studio_echo_snare` (a snare
  with a long room), `studio_cross_stick`, `studio_clap`,
  `studio_closed_hat`, `studio_open_hat`, `studio_tom`, `studio_block`,
  `studio_muted_cowbell`, `studio_open_cowbell`, `studio_cowbell`,
  `studio_dark_cowbell`, `studio_short_duck`, `studio_long_duck` (short,
  bright, vocal blips) and `studio_explosion` (a long, noisy boom). More
  machines' sounds will be added after these.
- **Tune** (st, ±12): the machine's tuning knob. It plays the whole sound
  faster or slower, so up is also shorter and down longer, as on the
  machines; tune toms apart (−5, −2, +1, +4) rather than giving each its
  own sound.
- **Decay** (%, 25 … 400, 100 the recording's own): how long it rings. A
  classic hi-hat's is its VCA: the classic closed hat at 100 % is the
  machine's decay B, about 32 % its A, 190 % its C and 275 % its D. For the
  others it scales the drum's own decay: below 100 % shorter and tighter,
  above it longer than the machine could play.
- **Level** (dB), **Pan** (%), **Velocity** (%: how much quieter a soft hit
  is, 40 dB across the range at 100 %). The classic closed hat also
  reaches the machine's accent (about +6 dB, ringing a little longer) in
  the top 40 % of velocity.
- **Vary** (%, 50 by default): at 0 every hit is the same, as a sample
  player's is; above it each hit gets new noise and small changes of pitch and of each part's
  level, as a real drum's do. The classic hi-hats differ from hit to hit at
  any Vary, as the machine's did (its VCA opened on a looping recording
  wherever the loop happened to be).
- **Choke** (`off`, `group1` … `group8`): a hit fades the group's other
  sounding pads, and its own earlier hit, in 10 ms. The closed and open
  hats share group 1.
- **Output** (`main`, `out1` … `out8`): an extra stereo output, out of the
  main mix.

`output` (dB) sets every output's level; a gentle limit bends the output
above −1 dBFS.

## Sounds

The classic sounds are gritty and lo-fi: 8-bit companded samples at a DAC
clock of about 20 kHz at Tune 0, the steps' images above it, the kick's
flat-topped (clipped) first cycles, a filter that closes after kick, tom
and conga hits, and a short lead-in before each hit that makes the kit sit
a little behind the beat.

The studio sounds are cleaner and fuller (a linear 16-bit DAC at about
44 kHz at Tune 0, nothing after it): the snares in five characters, the
tom one drum to tune apart as the classic tom is. The studio kit has no
cymbals; its factory kits use the classic crash and ride.

## Troubleshooting

- Silence on some notes: no pad has that key (see the table), or a kit
  moved its pads; `fx_describe` shows every `padN.key`.
- A pad missing from the main mix: its `output` is an extra one.
- The open hat cut short: the closed hat chokes it (group 1).
