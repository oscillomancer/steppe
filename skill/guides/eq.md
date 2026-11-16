# Yarrow: equaliser

Use `com.suppleaudio.yarrow` for rumble removal, tonal shaping, cuts around
regions reported by `analyze.masking`, de-essing and taming resonances that
come and go (dynamic bands), and mid/side work. Patches use
`supple.yarrow-patch/1.0`, with physical values under `params`; a merge
changes only the keys it names. A fresh instance is flat: every band is off.

Each `bandN` below means all eight bands, N = 1…8. Bands run in index order.

| Key | Unit and range | Default |
|---|---|---|
| `bandN.enabled` | bool | false |
| `bandN.type` | bell, low_shelf, high_shelf, low_cut, high_cut, notch, tilt | 1 low_cut, 2 low_shelf, 3–6 bell, 7 high_shelf, 8 high_cut |
| `bandN.freq` | Hz, 20…20000, logarithmic | 30, 100, 250, 800, 2500, 6000, 10000, 18000 |
| `bandN.gain` | dB, −30…+30 (bells, shelves, tilt) | 0 |
| `bandN.q` | 0.1…30, logarithmic: width for bells and notches, resonance for shelves (0.25…2) and cuts | 0.71, bells 1 |
| `bandN.slope` | db6, db12, db18, db24, db36, db48 (cuts) | db24 |
| `bandN.channel` | stereo, mid, side, left, right | stereo |
| `bandN.dynamic` | bool: a bell or shelf acts only while its region is loud | false |
| `bandN.threshold` | dB, −60…0: the region's level where a dynamic band starts | −30 |
| `scale` | %, 0…200: multiplies every bell, shelf and tilt gain | 100 |
| `auto_gain` | bool: offsets the output by the curve's average effect | false |
| `output` | dB, −24…+24 | 0 |
| `listen` | 0…8: plays only band N's region (0 off); for people, not mixes | 0 |

Turn a band on with its settings in one merge. Units may be strings
(`"80 Hz"`, `"-3 dB"`); a choice may be given by key or label (`"db24"` or
`"24 dB/oct"`).

Remove rumble:

```json
{"params":{"band1.enabled":true,"band1.type":"low_cut","band1.freq":"80 Hz","band1.slope":"db24"}}
```

Carve a masking region: after measuring a conflict around 250 Hz, cut the less
important source, then render and compare again:

```json
{"params":{"band3.enabled":true,"band3.type":"bell","band3.freq":250,"band3.gain":-3,"band3.q":1.2}}
```

De-ess, or tame a resonance that comes and goes: a dynamic bell rests at 0 dB
and moves toward its gain only while its region is above the threshold (by
three quarters of the overshoot, never past its gain). Set the threshold a
few dB under the region's level on the loud moments:

```json
{"params":{"band6.enabled":true,"band6.type":"bell","band6.freq":6500,"band6.gain":-8,"band6.q":2,"band6.dynamic":true,"band6.threshold":-34}}
```

Mono the low end and open the top of the sides, leaving the centre alone:

```json
{"params":{"band1.enabled":true,"band1.type":"low_cut","band1.freq":140,"band1.slope":"db12","band1.channel":"side",
           "band7.enabled":true,"band7.type":"high_shelf","band7.freq":9000,"band7.gain":1.5,"band7.channel":"side"}}
```

Make a whole curve subtler or stronger without touching its bands:
`{"params":{"scale":50}}` halves every gain (cuts and notches are unchanged).

Factory patches (`exec {"command": "fx.patch.list"}`, `exec {"command": "fx.patch.set"}`): Flat, Clean Up, Vocal
Polish, De-Ess, Air and Weight, Mono Bass, Telephone.

Limits. Latency is zero and the phase is the analog prototypes' (minimum
phase); there is no linear-phase mode. Magnitudes follow the analog
prototypes to 20 kHz within 0.35 dB for bells and 0.19 dB for shelves at
±12 dB (wider at ±30 dB; wide bells above 5 kHz are the worst case). Shelf
resonance stops at Q 2, tilt at 1. A band's frequency is designed at most at
0.45 × the sample rate; the patch keeps the value asked for. Auto gain is a
static estimate over a typical mix's spectrum, not a loudness match: measure
loudness after large changes. Gains add up and there is no ceiling: check
true peak after big boosts. A side band does nothing to a mono track. The
analyser (in the editor) shows the output only, as a smoothed long-term
spectrum; it is not a meter on the audio path.
