# Umber: tape

Use `com.suppleaudio.umber` for what a tape machine does to a sound: the
faint rounding of a mix's peaks on a well-kept modern machine, a drum bus
pushed into tape, a fatter low end from the head bump, warm electronics,
or a worn, warbling cassette. Patches use `supple.umber-patch/1.0`, with
physical values under `params`; a merge changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `drive` | dB, −24…24: how hard the signal meets the tape; at 0 dB a sine at −18 dBFS RMS records at the tape's reference level (0 VU) | 0 |
| `tape` | dB, 5…13: the tape's headroom, its 3% distortion point over reference: 12.5 a modern high-output tape, 9 a classic, 5 an early one (each lower is softer and noisier) | 12.5 |
| `bias` | %, 50…150 of the recommended bias: under is brighter and grittier (distortion at low levels too), over darker and cleaner | 100 |
| `record_amp` | %, 0…100: the class-A record amplifier: second harmonic, about −80 dB at reference at 15%, −47 at 50%, −30 at 100% | 15 |
| `hf_saturation` | %, 0…200: how soon the highs saturate: 100 the tape's own, 200 6 dB sooner, 0 6 dB later; the response stays flat | 100 |
| `record_hf` | dB, −6…6: the record side's HF trim, a shelf about 4 kHz at 15 ips that follows the speed (2 kHz at 7.5, 8 at 30): up writes the highs harder (brighter, and they saturate sooner) | 0 |
| `auto_gain` | on: the playback level falls as the drive rises, so the level holds | on |
| `speed` | `ips30`, `ips15`, `ips7_5`, `ips3_75` (inches per second) | ips15 |
| `emphasis` | `nab`, `iec`: the equalisation standard; not at 30 ips (AES) | iec |
| `head_bump` | %, 0…200: the playback head's swell in the lows and the ripples above it; 100 is a real head's (+2 dB at 15 ips, +1.5 at 30, +2.5 at 7.5) | 100 |
| `bump_freq` | Hz, 30…120: where the bump peaks at 15 ips (doubles at 30, halves at 7.5) | 57 |
| `azimuth` | degrees, 0…1: the playback head's tilt: highs lost within each track (−1.6 dB at 12 kHz at 0.3° at 15 ips), the two tracks apart in time (55 µs there, so the mono sum combs); 0.02 a well-aligned machine | 0 |
| `head_magnetism` | %, −100…100: a magnetized record head: even harmonics from the tape itself (−39 dB at 0 VU at 100%) and DC noise, a grain that stays in silence (with `mod_noise` on) | 0 |
| `dropouts` | %, 0…100: how often the oxide loses the head (100: about four a second) | 0 |
| `drop_depth` | %, 0…100: how deep each dropout goes: the tape lifts off the head, the highs going first and the dullness sweeping down and back (at 100% the deepest take 10 kHz down 40 dB, 1 kHz 9) | 30 |
| `print_through` | dB, −80…−30: the echo of the reel's next layer one turn later (1.4 s at 15 ips), a band about 1 kHz at 15 ips that moves with the speed; −56 a modern tape, and softer tapes print more (10 dB more at `tape` 5); −80 is off | −80 (off) |
| `crosstalk` | dB, −80…−20: the lows leaking between the tracks; −55 a two-track machine; −80 is off | −80 (off) |
| `wow` | %, 0…2: slow pitch drift as a peak speed error at 15 ips (× √(15/ips) at other speeds: 0.7 at 30, 1.4 at 7.5, 2 at 3.75); 0.02 a studio machine | 0.02 |
| `flutter` | %, 0…1: faster pitch waver (capstan, idler, friction), at 15 ips, scaled with speed as `wow` | 0.02 |
| `scrape` | %, 0…100: the tape's vibration over the heads, a kilohertz jitter that roughens the highs: 10 a good machine (inaudible on music), 50 a worn one, 100 a badly worn, dirty transport (a plain grit on sustained highs, −35 dB around a 10 kHz tone) | 10 |
| `repro_amp` | %, 0…100: the class-A playback amplifier, as `record_amp` | 15 |
| `repro_hf` | dB, −6…6: the playback side's HF trim, placed as `record_hf`'s (brighter or darker, saturation unchanged) | 0 |
| `hiss` | dB, −30…12: the machine's noise floor, the real tape's hiss at this speed and the playback electronics' low rumble beneath it; −30 is off | −30 (off) |
| `mod_noise` | dB, −30…12 against the real tape's modulation noise (grain riding the signal); −30 is off | 0 (real) |
| `hum` | dB, −100…−40 against the reference level: the power supply's ripple (a buzz at twice the mains) and the playback head's pickup of the mains; −85 a well-earthed machine, −65 a poorly earthed one; −100 is off | −100 (off) |
| `mains` | `hz50`, `hz60`: the hum's frequency | hz60 |
| `output` | dB, −24…24 | 0 |
| `oversampling` | `x1`, `x2`, `x4`, `x8`, `x16`: how many times the session rate the tape runs at; best when that is 176 to 384 kHz (4x or 8x at 44.1 and 48 kHz, 2x or 4x at 88.2 and 96); restarts the plugin | x4 |
| `latency` | `full` (about 8.5 ms: no phase change from the oversampling, deep wow) or `low` (9 samples, for tracking: wow scaled down to fit); restarts the plugin | full |
| `vu` | dB, readonly: the record level against reference, as a VU meter reads it | |
| `headroom` | dB, readonly: how far the loudest recent peak stayed under the tape's 3% limit; negative is over | |
| `saturation` | dB, readonly: how much the tape took off the loudest recent peaks | |

## Levels: what the drive means

The tape is calibrated as a machine is. A sine at −18 dBFS RMS (−15 dBFS
peak) reads 0 VU and records at reference level, where a modern tape adds
about 0.1% third harmonic. Distortion then rises about 2 dB per dB: 1% at
+9 dB, 3% (the tape's limit) at about +13, and from there it saturates
quickly. The highs have a limit of their own, lower at slower speeds: a
10 kHz sine gets out at most about 10 dB over reference at 15 ips,
−1 dB at 7.5 ips; a loud cymbal saturates there without pulling the rest
of the mix down with it.

So set `drive` by the meters, not by ear alone. Loud passages around
0 VU give the gentle rounding of a well-run session; `headroom` near 0 dB
means the peaks touch the limit; `saturation` says how much they lost.
A mix at about −18 LUFS needs no drive; a mastered track at −10 LUFS needs
about −8 dB to sit in the same place.

## Recipes

The subtle one, for a mix bus or a master (the Mastering patch): a modern
machine at 30 ips, clean electronics, a good transport, real hiss and a
well-earthed machine's hum. Check
`saturation` reads a fraction of a dB on the loudest moments:

```json
{"params":{"speed":"ips30","drive":-2,"record_amp":12,"repro_amp":12,"azimuth":0.01,"wow":0.017,"flutter":0.021,"scrape":8,"crosstalk":-60,"hiss":0,"hum":-85}}
```

Rounding peaks without any pitch movement (drums, a bus):

```json
{"params":{"speed":"ips15","drive":5,"wow":0,"flutter":0,"scrape":0}}
```

Hot drums into older tape, NAB for the low end, the electronics working:

```json
{"params":{"tape":9,"emphasis":"nab","drive":9,"record_amp":30,"repro_amp":25,"head_bump":120,"output":1.5}}
```

Second harmonic without much tape (warm electronics):

```json
{"params":{"drive":2,"record_amp":55,"repro_amp":45,"output":0.5}}
```

A worn machine, and a cassette:

```json
{"params":{"tape":5,"speed":"ips7_5","bias":85,"drive":4,"azimuth":0.12,"head_magnetism":40,"wow":0.18,"flutter":0.11,"scrape":50,"dropouts":45,"drop_depth":50,"print_through":-48,"crosstalk":-38,"hiss":4,"hum":-65,"output":3.5}}
```

```json
{"params":{"tape":5,"speed":"ips3_75","drive":-3,"azimuth":0.08,"wow":0.06,"flutter":0.05,"scrape":35,"dropouts":15,"hiss":0,"hum":-75,"output":3}}
```

Tracking through it live: `"latency":"low"` with `"oversampling":"x2"`
(both restart the plugin, so set them before playing).

Factory patches: Mastering, Mix Bus, Peak Rounding, Tracking, Hot Drums,
Fat Bass, Saturated, Warm Electronics, Underbiased, Cassette, Worn
Machine, Warped, Clean Transfer (`preset_list`/`exec {"command": "fx.patch.list"}`).

## What each part does

- **Speed** moves the head bump (57 Hz at 15 ips by default, 114 at 30,
  28 at 7.5), saturates the highs sooner as it slows, loses more top, and
  hisses and wavers more.
- **Emphasis** (below 30 ips): NAB lifts the lows into the tape below
  50 Hz, so bass saturates sooner, and records the highs with more
  headroom but plays them back with a little more hiss; IEC the reverse.
- **Bias** at 100% leaves the tape almost free of hysteresis: clean at low
  levels, rounding only near the limit. Lower bias opens the loop: quiet
  sounds turn grainy, the highs lift, and below about 70% everything gets
  quieter. Higher bias darkens and cleans.
- **The amplifiers** add second harmonic (the tape's own is third), rising
  1 dB per dB of level; both at 15% sit near −77 dB at 0 VU.
- **Auto gain** keeps the quiet level, not the loudness: heavy drive still
  comes out quieter, because the tape takes the peaks off. Trim `output`
  by ear or with an analysis.
- **Hiss** is off by default. At 0 dB it is the real tape's: about
  −78.5 dBFS A-weighted at 15 ips (IEC), with the electronics' rumble
  under it (about 10 dB more between 20 and 60 Hz than the hiss alone;
  NAB, which cuts the lows on playback, has less). Modulation noise is on, at the
  real tape's: it rides the signal and vanishes in silence unless the head
  is magnetized.

## Limits

- It changes level in the lows (the head bump) even when nothing
  saturates; set `head_bump` to 0 for a flat low end.
- The highs' stage is an allpass at low level: the level is flat, the
  phase turns around 5 kHz (15 ips), as a machine's does.
- Low latency holds wow to the 9 samples there are: about 40% of the
  default wow, less of deeper settings; deep wow needs `full`.
  Azimuth past about 0.3° (at 15 ips) is held there too.
- At 1x and 2x, loud highs alias; 4x is clean but for extreme drive, 8x
  for everything.
- Switching `speed` or `emphasis` while playing drops out for about 15 ms
  (the machine restarts clear); automate them between notes. `wow`,
  `flutter`, `scrape` and `mains` step too; everything else glides.
- The plugin reports a tail only for print-through (one turn of the reel,
  about 1.5 s at 15 ips): a host that stops processing in silence may
  stop the hiss and hum with it.

See [stock spec §13](../../docs/specs/stock-effects.md#13-tape--umber-umber).
