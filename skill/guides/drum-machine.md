# Drumlin: the drum machine

Drumlin (`com.suppleaudio.drumlin`) is Steppe's drum machine: sixteen pads,
each a synthesized sound of a classic machine (the LM-1's first, fitted to
recordings of a real one; no samples), each on a MIDI note of its own. The
exact keys, units and ranges are in `plugin_describe` / `fx_describe`; this
guide says what they do. For a kit from your own samples, use Pollard
(`guides/drum-sampler.md`).

## Which note plays which drum

Every pad has a `padN.key`, the MIDI note that plays it, beside its
`padN.sound` in `fx_describe`'s summary. The default kit sits on General
MIDI's drum keys, the notation's drum names, so a drum part written with
`voice drums ch 10 grid s` and `kick`, `snare`, `hat` … plays as written:

| pad | key | notation | default sound |
|---:|---:|---|---|
| 1 | 36 | `kick` | LM-1 Kick |
| 2 | 37 | `rim` | LM-1 Rimshot |
| 3 | 38 | `snare` | LM-1 Snare |
| 4 | 39 | `clap` | LM-1 Clap |
| 5 | 41 | `tom_floor` | LM-1 Tom, −5 st |
| 6 | 42 | `hat` | LM-1 Closed Hat (choke 1) |
| 7 | 45 | `tom_low` | LM-1 Tom, −2 st |
| 8 | 46 | `ohat` | LM-1 Open Hat (choke 1) |
| 9 | 47 | `tom_mid` | LM-1 Tom, +1 st |
| 10 | 49 | `crash` | Crash |
| 11 | 50 | `tom_high` | LM-1 Tom, +4 st |
| 12 | 51 | `ride` | Ride |
| 13 | 54 | `tamb` | LM-1 Tambourine |
| 14 | 56 | `cowbell` | LM-1 Cowbell |
| 15 | 63 | `n63` (GM's open high conga) | LM-1 Conga |
| 16 | 69 | `cabasa` | LM-1 Cabasa |

A key no pad has is silent (`kick2` 35, `snare2` 40, `pedal` 44, `china`,
`ride_bell`, `splash`, `crash2`, `shaker`, `clave`). Move a pad to any key
with `padN.key` (two pads on one key: the lower-numbered one plays). A
kit's patch that changes sounds keeps the keys unless it says otherwise, so
read `padN.key` and `padN.sound` before writing a part for a kit you did
not set up.

## A pad

```json
{"params": {"pad6.sound": "lm1_closed_hat", "pad6.decay": 60, "pad3.tune": 2, "pad3.vary": 30}}
```

- **Sound** (`padN.sound`): `lm1_kick`, `lm1_snare`, `lm1_rimshot`,
  `lm1_clap`, `lm1_closed_hat`, `lm1_open_hat`, `lm1_tom`, `lm1_conga`,
  `lm1_cabasa`, `lm1_tambourine`, `lm1_cowbell`, `crash`, `ride`. More
  machines' sounds will be added after these; keys never change.
- **Tune** (st, ±12): the machine's tuning knob. It plays the whole sound
  faster or slower, so up is also shorter and down longer, as on the LM-1;
  tune toms apart (−5, −2, +1, +4) rather than giving each its own sound.
- **Decay** (%, 25 … 400, 100 the machine's own): how long it rings. A
  hi-hat's is its VCA: the closed hat at 100 % is the LM-1's decay B, about
  32 % its A, 190 % its C and 275 % its D. For the others it scales the
  drum's own decay: below 100 % shorter and tighter, above it longer than
  the LM-1 could play.
- **Level** (dB), **Pan** (%), **Velocity** (%: how much quieter a soft hit
  is, 40 dB across the range at 100 %). The closed hat also reaches the
  LM-1's accent (about +6 dB, ringing a little longer) in the top 40 % of
  velocity.
- **Vary** (%): at 0 every hit is the same, as a sample player's is; above
  it each hit gets new noise and small changes of pitch and of each part's
  level, as a real drum's do. The hi-hats differ from hit to hit at any
  Vary, as the LM-1's did (its VCA opened on a looping recording wherever
  the loop happened to be).
- **Choke** (`off`, `group1` … `group8`): a hit fades the group's other
  sounding pads, and its own earlier hit, in 10 ms. The closed and open
  hats share group 1.
- **Output** (`main`, `out1` … `out8`): an extra stereo output, out of the
  main mix.

`output` (dB) sets every output's level; a gentle limit bends the output
above −1 dBFS.

## Sounds

The LM-1 sounds keep the machine's character: 8-bit companded samples at
the DAC's clock (about 20 kHz at Tune 0), the steps' images above it, the
kick's flat-topped (clipped) first cycles, the filter that closes after
kick, tom and conga hits, and the short lead-in before each hit that makes
the LM-1 sit a little behind the beat. A sound ends where the LM-1's EPROM
ended. The crash and ride were not LM-1 sounds; they come from the same
fitted recordings and the same kind of DAC.

## Troubleshooting

- Silence on some notes: no pad has that key (see the table), or a kit
  moved its pads; `fx_describe` shows every `padN.key`.
- A pad missing from the main mix: its `output` is an extra one.
- The open hat cut short: the closed hat chokes it (group 1).
