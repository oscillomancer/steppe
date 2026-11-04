# Writing music in Steppe notation

Steppe notation is plain text that compiles to MIDI. You write numbered bars;
each bar must add up exactly to the meter, and the compiler tells you when it
doesn't. Full specification: `generated/notation-reference.md`.

## The shape of a document

```
notation 1
meter 4/4

voice bass ch 1 vel 100
1: C2:e r:e C2:e r:e Eb2:e r:e G1:e Bb1:e
2: C2:e r:e C2:e r:e F2:e r:e Eb2:q
3-4 = 1-2
```

- `voice NAME` starts an independent line of music (`ch` MIDI channel 1–16,
  `vel` default velocity 1–127). Several voices can share a channel.
- `N:` starts bar N. **Write every bar's events so they add up to the
  meter.** `|` starts the next bar on the same line: `1: C4:w | D4:w`. Writing
  the new bar's number right after `|` is also accepted (`1: C4:w | 2: D4:w`);
  a number that is not the next bar is an error.
- `5-8 = 1-4` copies bars; `9-16 = 1-4` repeats them (twice).
- `#` starts a comment. `;` can replace a newline.

## Notes, rests, durations

`PITCH:DURATION` — `C4` is middle C (MIDI 60), sharps `#`, flats `b`:
`F#3`, `Bb2`, `Eb5`. `n60` is MIDI note 60 directly.

| token | length | beats in 4/4 |
|---|---|---|
| `w` | whole | 4 |
| `h` | half | 2 |
| `q` | quarter | 1 |
| `e` | eighth | 1/2 |
| `s` | sixteenth | 1/4 |
| `t` | thirty-second | 1/8 |

- Dotted: `q.` = 1.5 beats, `e.` = 3/4 beat.
- Triplets: `e3` (three fill one beat), `q3` (three fill two beats).
- Sums: `h+e` (2.5 beats, one note).
- Rest: `r:q`. **`r:*` fills whatever is left of the bar** — end a bar with
  it when you only care about the notes at the start.

## Chords, chord symbols, velocity, ties, overlaps

- `[C4 E4 G4]:h` — explicit chord.
- `{Cm7}:h`, `{F#m7b5}:q`, `{C/E}:h` — chord symbol, voiced in close root
  position around octave 3 (`voice keys octave 4` moves it up).
- `C4:q:110` — velocity for one note. Dynamics `pp p mp mf f ff` change the
  voice's velocity from that point.
- `C4:q>` accent (+20 velocity), `C4:q'` staccato (half length).
- `G4:w~` then `G4:h` in the next bar — **tie**: one note across the bar
  line. A note may not spill past its bar without a tie.
- `C2:w& E4:q G4:q E4:q G4:q` — `&` holds C2 while the melody continues
  (the held note doesn't count toward the bar sum).

## Drums

```
voice drums ch 10 grid s
1: kick  x...x...x...x...
1: snare ....x.......x...
1: hat   x.x.x.x.x.x.x.x.
2-3 = 1
4: kick  x...x...x...x.x.
4: snare ....x.......xxxx
4: hat   x.x.x.x.x.x.x.x.
```

- `grid s` = one character per sixteenth → **16 characters per 4/4 bar**
  (`grid e3`: 12, `grid e`: 8). Spaces are ignored, so `x... x... x... x...`
  is easier to count.
- `x` hit, `X` accent, `o` ghost note, `.` or `-` nothing, `1`–`9`
  velocity levels.
- Names: `kick snare rim clap hat pedal ohat ride ride_bell crash splash
  china tom_high tom_mid tom_low tom_floor tamb cowbell shaker clave`.

## Controllers

```
lane cc cutoff ch 1
1.1 20
3.1 110 lin
4.3 40 fast
```

Positions are `bar.beat` (`4.3` = bar 4, beat 3; `+120t` adds ticks).
`lin` / `slow` / `fast` ramp from the previous point; without a shape the
value jumps. Names: `mod expression sustain volume pan cutoff resonance
attack release breath`; or a number (`lane cc 74`). `lane bend` takes
`+2st` style values. These are MIDI controllers — a stock plugin's own
parameters are automated with envelopes, not CC lanes.

## Before you send it — check each bar

The most common mistakes, in order:

1. **A bar that doesn't add up.** Count beats per bar, especially with dotted
   notes and triplets (three `e3` = one beat; individual triplet durations
   may be combined with other durations). When in doubt, end the bar with `r:*`.
2. **A long note crossing a bar line without `~`.** Split it and tie.
3. **Wrong grid length.** `grid s` in 4/4 is exactly 16 steps per bar.
4. **Octave slips.** Bass usually lives in octaves 1–2, melodies 4–5,
   chords 3–4. `C4` is middle C.
5. **Chord and melody in one voice.** Use separate voices (or `&`).

If the compiler rejects your text, its error names the voice and bar and
says how many beats the bar has and needs — fix that bar only.
