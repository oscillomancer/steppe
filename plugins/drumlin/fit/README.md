# drumlin-fit

Fits Drumlin's recipes to recordings of a drum machine. Drumlin plays no
samples: each sound is a recipe (resonant modes, bands of filtered noise, a
clap's bursts, a hi-hat's loop, and the machine after them), and this tool
finds the recipe that sounds most like a set of recordings.

It renders with Drumlin's own engine, the script compiled here as C++ just as
the stock plugin compiles it, so what it fits is what the plugin plays. It is
built with the tests (`cmake --build build/release --target drumlin-fit`;
build it optimized, as the release preset does) and never run by them: the
recordings are not part of the repository, and nothing taken from them may
be. Only the fitted numbers go into the script.

## The machine it assumes

A recording is what a voice's EPROM held, played by its DAC at a clock: the
machine's tuning knob sets that clock, so each pitch number of a sample bank
is the same recording, faster or slower. A recipe's frequencies and times
are given at its `clock` (Tune 0); a recording of the machine at another
setting is matched by playing the recipe at that recording's clock. The DAC
holds its steps unsmoothed; it is the classic kit's 8-bit mu-law companded
one, or a linear one of `bits` bits (the studio kit's: 16). A low-pass
filter that closes after the hit, the output's coupling and a VCA follow,
in real time. See the script's opening comment and docs/specs/drumlin.md.

## Specs

A spec is a text file, one statement a line, `#` for comments:

| Line | Meaning |
|---|---|
| `set <field> <value>` | a recipe field held fixed (`modes 6`, `band1.shape 2`, `clock 22583.7`) |
| `free <field> <start> <low> <high> [log]` | a field the fit searches, from `start`, within the range (log: a ratio scale) |
| `free peak <start> <low> <high> log` | the recording's peak into the DAC; above 1 it clipped, as the classic kick did |
| `load <file>` | the values of an earlier fit's `.best` (they become the free fields' starts) |
| `target <wav> clock=<Hz> [weight=<w>] [decay=<ms>] [hit=<n>] [accent=1] [upto=<ms>]` | a recording to match: the clock its machine played it at, its weight, a hi-hat's VCA decay, the hit's number (a loop's starting point), its accent, and where its score stops (before a fade the recording was given) |
| `weights <hit> <body> <level> <wave> [<fine> [<crest> [<ring>]]]` | the score's parts (below); `fine`, `crest` and `ring` 0 if left out |
| `wave <ms>` | how much of the hit the waveform part compares |
| `window <before> <longest>` | seconds compared before the hit, and at most after it |
| `drive auto\|fixed` | whether the drive follows `peak` (auto, the default) or stays as set |

Fields are a recipe's members (`clock`, `length`, `start`, `strike`, `bend`,
`bendMs`, `level`, `vcfOpen`, `vcfShut`, `vcfMs`, `hpf`, `hold`, `decay`,
`accent`, `gate`, `gateMs`, `burstMs`, `burstGain`, `burstJitter`, and the
counts `modes`, `bands`, `bursts`, `loop`, `bits`), a mode's (`mode3.hz`, `.gain`,
`.fast`, `.slow`, `.split`, `.phase`) and a band's (`band0.hz`, `.q`, `.gain`,
`.delay`, `.attack`, `.fast`, `.slow`, `.split`, and `.shape`: 0 band pass, 1
low pass, 2 high pass).

## The score

Each render is lined up with its recording at the hit (the first sample past a
quarter of the peak) and compared in seven parts, each in dB:

- **hit**: band levels (28 bands from 150 Hz) in 6 ms frames around the hit;
- **body**: band levels (44 bands from 25 Hz) in 46 ms frames, later frames
  counting less;
- **level**: the level in 8 ms windows every 2 ms;
- **wave**: one minus the waveform's correlation over the hit's first `wave`
  milliseconds, times ten (for tonal sounds, whose phase can be matched);
- **fine**: the body's spectrum in 21.5 Hz bins, averaged over the whole
  body: what the bands blur, a pure tone standing out of noise or a
  cymbal's narrow peaks (for the hi-hat, whose loop the bands alone let
  turn into a few loud tones);
- **crest**: the peak over the RMS in 5 ms windows through the hit's first
  60 ms: how far its top is clipped flat, which no spectrum shows (the
  bands alone let a recipe be driven far past the DAC's full scale);
- **ring**: narrow peaks the render has and the recording does not (below).

Levels below the recording's loudest by 60 or 70 dB count as that floor.
The total is the weighted mean of hit, body, level, fine and crest, plus
wave and ring. Give every fit a ring weight (1 is enough); `score` reports
the ring even for a fit made without one, so an old recipe can be checked.

## Rings: the fault every fit has to be kept from

A **ring** is a thin, harsh, whistling tone in a render, usually above
2 kHz, that the recording does not have. Both kits' fits made them: the
classic hi-hat's loop and tambourine first, then most of the studio kit's
first fits (the snares, cross stick, hats, echo snare, block, cowbells,
tom, ducks and explosion), refitted with the ring part below. A listener
hears it at once; the other parts of the score do not.

- **What makes it.** A part of the recipe that is a pure tone where the
  drum's sound is noise: a *mode* (a decaying sine) put on a high partial,
  or a *band* narrower than the partial it stands for. A real drum's high
  partials (a snare's wires, a hat's metal, a room) are widened by noise
  and damping into peaks tens or hundreds of Hz wide; a sine is one bin
  wide, and a long-decaying one (the studio bright snare's 5.9 kHz mode
  rang 337 ms) whistles over everything else. A tone that is quiet but
  outlasts the noise around it rings too, once the noise has gone. Low
  modes can do the same, less harshly: a snare's or a room's fundamental
  is smeared in the recording (its pitch moves, its damping is uneven), and
  a mode on it that decays too slowly hums through the tail (the deep and
  echo snares' first fits, at 205 and 167 Hz).
- **Why the rest of the score misses it.** The band maps are too coarse to
  tell a tone from noise. The fine spectrum can, but it is a mean over
  some 900 bins: one bin 25 dB too high moves it by under 1 dB, so a
  search trades it for any small gain elsewhere.
- **The ring part.** In three spans after the hit (5 to 60 ms, 60 to
  250 ms, 250 ms to 1.5 s) it takes the spectrum of both, as fine as the
  span allows (up to 8192 points, 5.4 Hz bins), measures how far each bin
  stands above the mean of its third of an octave, and counts where the
  render's stands out more than the recording's by over 10 dB (a fixed
  noise seed's own random peaks reach about that), as the RMS over the
  worst handful, the worst span counting. Partials the recording really
  has (a cowbell's, a tom's) cost nothing where the render matches them.
  The fine bins matter: a tone stands further out of noise the finer the
  bins, and at the body's 21.5 Hz bins the first version of this part let
  tones the ear hears at once score under 2.
- **Reading it.** `score` reports each target's ring, with a ring weight
  or without. A render with no ring scores 0 to 2; above 3 at any target,
  listen for it; a whistling tone scores 4 to 20 (the bright snare's first
  fit: 18). Check a doubtful one at 8192 points over the same spans: the
  render's peak at that frequency, over its third-octave mean, against the
  recording's.
- **How to keep it out.** Give modes only the partials that ring in the
  recording too: a kick's, a tom's, a cowbell's, a block's, and a snare's
  shell below about 1.5 kHz. Make everything above that, and every partial
  of a hat, a cymbal, a clap or a room, bands of noise, never narrower than
  measured (`measure.py partials` gives the width). Fit with a ring weight
  (`weights … 1`), and read every fit's ring before its recipe goes into
  the script.
- **Then listen.** Listen above all for rings (above). The score is a
  guide, not the judge. A lower score can
  sound worse: a cymbal refit that scored better made the wrong partials
  the loudest, and it sounded higher pitched. `compare` shows what the
  score blurs: the strongest partials (a cymbal's pitch), how far the
  peaks stand above the floor (a ring), and the balance over time. Give
  the user `listen`'s file before a recipe goes into the script.

