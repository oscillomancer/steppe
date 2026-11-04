# Spec: MIDI model and the Steppe notation

**Status:** Implemented. The syntax may still be revised where agents that
have read only this document fail to write it correctly the first time.
Changes via ADR.
**Libraries:** `libs/midi` (`supple::midi`), `libs/notation`
(`supple::notation`). Non-RT.

The notation is how agents (and humans who like typing) write melodies,
chords, drum patterns, bass lines and controller moves. It compiles to MIDI
items and decompiles from them. It is designed around what LLMs do well
(named pitches, symbolic durations, short regular grids, explicit structure)
and against what they do badly (accumulating time across many events,
relative octaves, counting long strings). Its central safety feature is that
**every bar is numbered and must add up**: a counting mistake is a compile
error that names the bar and says how far off it is.

---

## Part A — MIDI model (`libs/midi`)

```cpp
namespace supple::midi {

struct Note {
    std::int64_t start = 0;      // ticks from item start, >= 0
    std::int64_t length = 0;     // ticks, >= 0
    std::uint8_t channel = 0;    // 0..15 (displayed 1..16)
    std::uint8_t pitch = 60;     // 0..127, C4 = 60
    std::uint8_t velocity = 96;  // 1..127
    std::uint8_t offVelocity = 0;// 0..127
};

enum class ControlKind : std::uint8_t { CC, ProgramChange, ChannelPressure, PolyPressure, PitchBend };
struct Control {
    std::int64_t tick = 0;
    ControlKind kind = ControlKind::CC;
    std::uint8_t channel = 0;
    std::uint8_t number = 0;     // CC number; PolyPressure: pitch; else 0
    std::int32_t value = 0;      // CC/prog/pressure 0..127; PitchBend -8192..8191
};

struct Sysex { std::int64_t tick = 0; std::vector<std::uint8_t> bytes; };  // includes F0 ... F7

struct Meter { int numerator = 4; int denominator = 4; };  // numerator 1..64, denominator 1,2,4,..,64

struct Sequence {
    int ppq = 960;                    // ticks per quarter note, 1..32767 (SMF limit)
    std::int64_t length = 0;          // item length in ticks (>= end of last event)
    std::vector<Note> notes;
    std::vector<Control> controls;
    std::vector<Sysex> sysex;

    void canonicalize();              // sort (see below), drop exact duplicates
    friend bool operator==(const Sequence&, const Sequence&);   // after canonicalize
};

// Standard MIDI File I/O, scoped to *clips*: one Sequence per call. (Full
// multi-track SMF import into tracks/tempo map is an engine feature
// built on readSmf with `track`.)
struct SmfInfo {
    int ppq = 960;
    int format = 0;
    int trackCount = 0;
    std::vector<std::pair<std::int64_t, Meter>> meters;      // tick, meter (from all tracks)
    std::vector<std::pair<std::int64_t, double>> tempos;     // tick, BPM
    std::vector<std::string> trackNames;
    int skippedEvents = 0;                                    // meta/other events not represented
};
struct SmfReadOptions { std::optional<int> track; };          // nullopt: merge all tracks
Result<Sequence> readSmf(std::span<const std::uint8_t> bytes, SmfInfo* info = nullptr, const SmfReadOptions& = {});
// Validates the Sequence (ranges, sysex framing, start+length overflow) and returns structured errors.
Result<std::vector<std::uint8_t>> writeSmf(const Sequence&, const std::vector<std::pair<std::int64_t, Meter>>& meters,
                                           const std::vector<std::pair<std::int64_t, double>>& tempos);  // format 0
}
```

Canonical order: notes by (start, pitch, channel, length, velocity,
offVelocity); controls by (tick, kind order as declared, channel, number,
value) — except that controls with identical (tick, kind, channel, number)
keep their relative order (it is meaningful: last wins); sysex by tick, then
original order. `readSmf` pairs note-on/note-off FIFO per (channel, pitch);
note-on velocity 0 is a note-off with offVelocity 0; unterminated notes end at
the last event tick; meta events other than tempo/meter/track name are counted
in `skippedEvents` (end-of-track is not counted). Zero-tempo metas and
malformed or unsupported time signatures are skipped without incrementing
that count. Time signatures require at least four payload bytes, numerator
1..64 and a power-of-two denominator 1..64. Valid meters are preserved
without normalization (e.g. 6/8 stays 6/8).

---

## Part B — the notation

### B.1 A first example

```
notation 1
meter 4/4

voice drums ch 10 grid s
1: kick  x...x...x...x...
1: clap  ....x.......x...
1: hat   ..x...x...x...x.
2-4 = 1

voice bass ch 1 vel 100
1: C2:e r:e C2:e r:e Eb2:e r:e G1:e Bb1:e
2: C2:e r:e C2:e r:e F2:e r:e Eb2:q
3-4 = 1-2

voice keys ch 2 vel 72
1: {Cm7}:h {Fm7}:h
2: {Abmaj7}:h {G7}:h
3-4 = 1-2
```

Four bars of a drum pattern, a bass line and a chord progression, all
checked bar by bar.

### B.2 Lexical rules

- UTF-8 text, one statement per line. `;` also ends a statement (handy in
  one-line JSON strings). `#` starts a comment to end of line.
- Tokens are separated by spaces/tabs. Keywords are lowercase.
- Bar numbers are limited to 100000, including bars introduced by `|`.
- Pitch names are case-insensitive on input (`c#4` = `C#4`); the canonical
  form is uppercase letter + accidentals.

### B.3 Document structure

```
document   := [ "notation 1" ] { header } { section }
header     := "meter" N "/" D [ "at" BAR ] | "ppq" INT | "tempo" NUMBER
            | "length" BARS [ "+" INT "t" ] | "pickup" DUR | "octave" INT
section    := voice | lane | events
```

- `notation 1` — format version; recommended first line; the canonical form
  always writes it.
- `meter 4/4` — bar length. In the DAW the host supplies the project's meter
  at the item's position; a `meter` line must then agree with it (else
  `notation.meter_mismatch`). With no host meter and no `meter` line, 4/4.
  A `meter` line may also appear *between bar lines inside a voice* only when
  standalone (sandbox); see B.10.
- `ppq 960` — tick resolution of the compiled item (default 960, or the
  host's), 1..32767. Every bar length must be a whole number of ticks
  (`ppq·4·N/D`), else `notation.tick_fraction`.
- `pickup q` — the item starts with an anacrusis of that duration: bar `0:`
  exists, holds exactly that duration, and precedes bar 1 (the item starts
  that much before bar 1's downbeat). Without `pickup`, bar 0 is an error.
  *(Amended 2026-09-26.)*
- `tempo 120` — only used when there is no host tempo map (the sandbox);
  otherwise it must agree with the host at the item start or it is a warning.
- `length 8` — item length in whole bars (bars counted from bar 1); `length 8+480t` adds ticks (fewer
  than one bar). Default: the end of the last written bar. The length must
  be ≥ the end of the last event (including layered `&` notes that extend
  past their bar); it may be shorter than the last written bar (trailing
  rests beyond it are simply cut). Without `length`, the item ends at the
  later of the last written bar's end and the last event's end rounded up to
  a bar line.  *(Amended 2026-09-26.)*

### B.4 Voices

**Implicit voice** *(Amended 2026-09-26, second round)*: bar lines that
appear before any `voice`/`lane`/`events` section belong to an implicit
`voice main` with default attributes, so a one-voice phrase needs no
declaration: `1: C4:q E4:q G4:h`. It is exactly equivalent to writing
`voice main` first (an explicit later `voice main` is then a duplicate-voice
error). The decompiler still writes every voice explicitly. A bar line that
cannot start the implicit voice (e.g. after a `lane` section) keeps its
error, whose hint must say to add a `voice NAME` line — never suggest a
header keyword for a line that starts with a bar number.

```
voice NAME [ch 1..16] [vel 1..127] [octave -1..8] [grid DUR [len DUR]] [accent 1..127] [ghost 1..127]
BAR ":" events [ "|" [ BAR ":" ] events ]...
BARS "=" BARS
```

A voice is an independent time cursor. Several voices may share a channel.
`NAME` is an identifier (`[a-z][a-z0-9_]*`). `vel` is the running velocity
(default 96). `grid` makes it a drum-grid voice (B.7).

Bar lines: `N:` starts bar N (1-based, relative to the item start). Bars in a
voice must appear in increasing order; gaps are silent. `|` on the same line
continues with the next bar: `1: C4:q D4:q E4:q F4:q | G4:w` is bars 1 and 2.
A number written immediately after `|` is accepted when it names the bar the
`|` starts (`1: … | 2: …` reads the same as `1: … | …`); any other number
there is an error (`notation.bar_after_pipe`). *(Amended 2026-10-01.)*

**Every written bar must sum exactly to the meter** (in ticks). Otherwise:

```
notation.bar_length (bass, line 7): bar 3 has 3 1/2 beats; 4/4 needs 4.
Hint: add r:e at the end, or end the bar with r:* to fill it.
```

Copies: `5-8 = 1-4` sets bars 5..8 of this voice to the content of bars
1..4; if the destination is longer it repeats cyclically (`9-16 = 1-4`
writes 1-4 twice). `2-4 = 1` repeats one bar. The source must already be
written; copies of copies are fine.

### B.5 Events in a bar

```
note    := PITCH ":" DUR [ ":" VEL [ "/" OFFVEL ] ] { ARTIC } [ "~" ] [ "&" ]
chord   := "[" PITCH { PITCH } "]" ":" DUR [ ":" VEL ... ] { ARTIC } [ "~" ] [ "&" ]
symbol  := "{" CHORDSYM "}" ":" DUR [ ":" VEL ... ] { ARTIC } [ "~" ] [ "&" ]
rest    := "r:" DUR | "r:*"
dynamic := "ppp" | "pp" | "p" | "mp" | "mf" | "f" | "ff" | "fff"
```

**Pitches.** Scientific pitch: letter `A`–`G`, accidentals `#`, `b`, `##`,
`bb`, octave `-1`..`9`; `C4` = 60 = middle C, `A4` = 69. `n60` gives a MIDI
number directly. Out-of-range pitches are errors.

**Durations** (fractions of a whole note, W = 4 × ppq ticks):

| token | value | ticks @960 |
|---|---|---|
| `w` | whole | 3840 |
| `h` | half | 1920 |
| `q` | quarter | 960 |
| `e` | eighth | 480 |
| `s` | sixteenth | 240 |
| `t` | thirty-second | 120 |
| `x` | sixty-fourth | 60 |

- Dots: `q.` = 1.5×, `q..` = 1.75×.
- Tuplets: a digit suffix `N` scales by P/N where P is the largest power of
  two below N: `e3` = triplet eighth (×2/3, 320 ticks), `q3` (640),
  `s5` = quintuplet sixteenth (×4/5, 192), `s6` (×4/6), `s7` (×4/7),
  `e9` (×8/9). Dots and tuplets do not combine.
- Sums: `h+e`, `q.+s` (one event lasting the total). Every `+` must be
  followed by a duration term; a trailing `+` is `notation.bad_duration`.
- Exact ticks: `700t`. (The decompiler uses this when nothing symbolic is
  exact; people and agents rarely need it.) `0t` is valid for notes
  (zero-length notes occur in real MIDI) but not for rests.
- A duration that is not a whole number of ticks at the item's ppq is an
  error (`notation.tick_fraction`), e.g. `x5` at ppq 96.

**Velocity.** `:NN` sets this event's velocity (1..127); the running voice
velocity (`vel`, or the last dynamic mark) applies otherwise. `:NN/MM` also
sets note-off velocity (default 0). Dynamics set the running velocity:
`ppp` 16, `pp` 32, `p` 48, `mp` 64, `mf` 80, `f` 96, `ff` 112, `fff` 127.

**Articulations** (compile-time conveniences; the decompiler never emits
them): `'` staccato (length × 1/2, minimum 1 tick), `>` accent (velocity +20,
max 127).

**Ties.** A trailing `~` ties this event to the next event in the same voice
that has the same pitch(es) and starts exactly where this one ends; they
compile to one note. Ties are how notes cross bar lines. A `~` with nothing
to tie to is an error (`notation.dangling_tie`).

**Layering.** A trailing `&` starts the event at the cursor but **does not
advance** it, so later events overlap it: `1: C2:w& E4:q G4:q E4:q G4:q`
holds a C2 under a moving line. Layered events do not count toward the bar
sum and may extend past the bar end.

**Rests.** `r:DUR` advances the cursor silently. `r:*` fills the rest of the
bar and may only be the last event of a bar (on an already full bar it is a
no-op).

**Chords.** `[C4 E4 G4]:h` — all pitches start together with the same
duration and velocity. Duplicate pitches are errors.

**Chord symbols.** `{Cm7}:h` voices a chord symbol deterministically: close
position, root position: the root in the voice's `octave` (default 3), each
further chord tone the interval above the root from the table (so 9ths etc.
sit above the octave), pitches sorted ascending. Slash chords `{C/E}` add
the bass note in the octave below the root. `octave` applies to chord
symbols only. Qualities (intervals in semitones from
the root):

| symbol | intervals | symbol | intervals |
|---|---|---|---|
| `C` / `Cmaj` | 0 4 7 | `Cm` / `Cmin` | 0 3 7 |
| `Cdim` | 0 3 6 | `Caug` / `C+` | 0 4 8 |
| `Csus2` | 0 2 7 | `Csus4` / `Csus` | 0 5 7 |
| `C6` | 0 4 7 9 | `Cm6` | 0 3 7 9 |
| `C7` | 0 4 7 10 | `Cmaj7` / `CM7` | 0 4 7 11 |
| `Cm7` / `Cmin7` | 0 3 7 10 | `CmMaj7` | 0 3 7 11 |
| `Cdim7` | 0 3 6 9 | `Cm7b5` / `Cø` | 0 3 6 10 |
| `C7sus4` | 0 5 7 10 | `Cadd9` | 0 4 7 14 |
| `C9` | 0 4 7 10 14 | `Cmaj9` | 0 4 7 11 14 |
| `Cm9` | 0 3 7 10 14 | `C7b9` | 0 4 7 10 13 |
| `C7#9` | 0 4 7 10 15 | `C11` | 0 7 10 14 17 |
| `C13` | 0 4 10 14 21 | `C5` | 0 7 |

Roots accept accidentals (`{F#m7}`, `{Bbmaj7}`). Unknown qualities are errors
listing the table's closest match.

### B.6 Examples of the event syntax

```
1: E4:q G4:e A4:e B4:q. A4:e           # dotted quarter + eighth
2: C5:h~ C5:e B4:e A4:q                # tie inside a bar
3: G4:w~                               # tie across the bar line ...
4: G4:h r:h                            # ... continues here: one 1.5-bar note
5: mf C4:e3 D4:e3 E4:e3 F4:q f G4:h    # triplets, dynamics
6: [C4 E4 G4]:q' [C4 E4 G4]:q' r:h     # staccato chords
7: C2:w& E4:q G4:q E4:q G4:q           # held bass under a line
8: C4:q:110> D4:q:70 r:*               # explicit velocities, accent, fill rest
```

### B.7 Drum grids

A voice with `grid DUR` holds step patterns instead of events:

```
voice drums ch 10 grid s len t vel 100 accent 127 ghost 40
1: kick  x...x...x...x...
1: snare ....x..o....x...
1: hat   x-x-X-x-x-x-X-x-
2-3 = 1
4: kick  x...x...x...x.x.
4: snare ....x.......xxxx
4: hat   x-x-x-x-x-x-x-x-
```

- Each row is `BAR: INSTRUMENT PATTERN`. Several rows may share a bar; each
  row is independent. Rows of a bar replace nothing else — they merge.
- The pattern has exactly one character per step; the step is the grid
  duration, so a 4/4 bar has 16 steps at `grid s`, 12 at `grid e3`, 8 at
  `grid e`. Spaces inside the pattern are ignored (`x... x... x... x...`).
  A wrong step count is an error that states the count needed.
- `BARS: INSTR PATTERN` with a range (`1-2: hat x-x-...`) takes a pattern of
  exactly that many bars' steps.
- Step characters: `.` or `-` silence; `x` hit at the voice velocity; `X`
  accent velocity; `o` ghost velocity; digits `1`–`9` velocities 15, 29, 43,
  57, 71, 85, 99, 113, 127.
- Each hit lasts `len` (default: one step).
- Instrument names (General MIDI; `n36` also works):

| name | note | name | note | name | note |
|---|---|---|---|---|---|
| `kick` | 36 | `kick2` | 35 | `rim` | 37 |
| `snare` | 38 | `clap` | 39 | `snare2` | 40 |
| `hat` | 42 | `pedal` | 44 | `ohat` | 46 |
| `tom_floor` | 41 | `tom_low` | 45 | `tom_mid` | 47 |
| `tom_high` | 50 | `crash` | 49 | `crash2` | 57 |
| `ride` | 51 | `ride_bell` | 53 | `china` | 52 |
| `splash` | 55 | `tamb` | 54 | `cowbell` | 56 |
| `shaker` | 70 | `clave` | 75 | `cabasa` | 69 |

  A host may supply a different drum map (e.g. the stock drum synth's), which
  then replaces this table; unknown names are errors with closest matches.

### B.8 Lanes (controllers, bend, pressure)

```
lane cc NUMBER|NAME [ch 1..16]
lane bend [ch 1..16] [range SEMITONES]      # default range 2
lane pressure [ch 1..16]
lane poly PITCH [ch 1..16]
POSITION VALUE [SHAPE]
```

Lane points are listed one per line (or `;`-separated), in non-decreasing
time order. Several points may share a position; they compile to events at
the same tick in the order written (the last one wins, as in MIDI).
*(Amended 2026-09-26.)*

- **Positions** are `BAR.BEAT` with an optional tick offset: `1.1`, `3.2`,
  `4.3+120t`. Beats count the meter's denominator unit (in 6/8, `2.4` is the
  4th eighth of bar 2).
- **Values:** CC, pressure 0..127; bend either raw `-8192..8191` or
  semitones with `st` (`+2st`, `-0.5st`), converted with the lane's range.
- **Shape** of the segment *arriving* at this point: `step` (default — jump
  at the point), `lin` (linear ramp from the previous point), `slow`
  (ease-in: quadratic), `fast` (ease-out). With x ∈ (0, 1] the fraction of
  the segment: lin `v0+(v1−v0)x`, slow `v0+(v1−v0)x²`, fast
  `v0+(v1−v0)(1−(1−x)²)`. Ramps compile to intermediate events at
  `t0 + k·max(1, ppq/16)` (integer division) for `t0 < t < t1`, values
  rounded half away from zero, **intermediate** events dropped when equal to
  the previous event's value; the endpoint event at `t1` with `v1` is always
  emitted. A `step` point always emits its event, even if the value repeats
  (the decompiler relies on this).
- CC names: `mod` 1, `breath` 2, `foot` 4, `porta_time` 5, `volume` 7,
  `pan` 10, `expression` 11, `sustain` 64, `porta` 65, `sostenuto` 66,
  `soft` 67, `legato` 68, `resonance` 71, `release` 72, `attack` 73,
  `cutoff` 74.

```
lane cc cutoff ch 1
1.1 20
3.1 110 lin
4.1 110
4.3 40 fast

lane cc sustain ch 1
1.1 127; 2.1 0; 2.1+10t 127; 4.4 0
```

### B.9 Events block

```
events [ch 1..16]
1.1 program 5
1.1 sysex F0 7E 7F 09 01 F7
```

`program 0..127` (bank select is `lane cc 0` / `lane cc 32`), `sysex` bytes in
hex including `F0`/`F7`.

### B.10 Meter changes

*(Amended 2026-09-26.)* Meter changes are header statements, not voice
content: `meter 3/4 at 5` applies from bar 5 to every voice (a plain
`meter N/D` is `at 1`). They must be in increasing bar order. Without a host
meter map (sandbox), they define the bar lengths. In the DAW, meter changes
belong to the project (`timesig.*` commands); the host's `meterAt` maps
local bars to project bars through the item's placement (bar 1 of the
notation sits on the project bar the command names), and every `meter` line
must agree with it (`notation.meter_mismatch`). GUI live compile and decompile
use the same project context: local bar 1 maps to the project bar containing
the item's start, and later local bars follow later project meter changes.
The command-side compiler and `midi.get` must supply that identical context.

```
meter 4/4
meter 3/4 at 3
voice lead
1: C4:w
2: D4:w
3: E4:h.
4: F4:q G4:q A4:q
```

---

## Part C — compile and decompile

```cpp
namespace supple::notation {

struct DrumMap { std::vector<std::pair<std::string, std::uint8_t>> names; };  // empty = GM table
struct CompileContext {
    std::optional<int> ppq;                 // host ppq; else document or 960
    std::function<midi::Meter(int bar)> meterAt;   // host meter map; null = standalone
    std::optional<DrumMap> drums;
};
struct Compiled {
    midi::Sequence sequence;                // canonicalized
    std::vector<midi::Meter> barMeters;     // meter per bar, bars 1..N
    std::optional<double> tempo;            // from a tempo header
    std::int64_t pickupTicks = 0;           // item starts this many ticks before bar 1 (`pickup`)
    std::vector<Warning> warnings;          // supple::schema::Warning shape: code, message, path("line:col")
};
Result<Compiled> compile(std::string_view text, const CompileContext& = {});

struct DecompileOptions {
    std::function<midi::Meter(int bar)> meterAt;   // null = 4/4 throughout
    bool drumGrids = true;                  // try grid voices for channel 10
    std::optional<DrumMap> drums;
};
Result<std::string> decompile(const midi::Sequence&, const DecompileOptions& = {});
}
```

### C.1 Errors

All errors are `supple::Error` with `code` `notation.*`, `loc` (line,
column), a message that names the voice and bar, and a hint. Compile
collects all errors (up to 50) as `details` of one `notation.invalid` error
(or returns the single error directly). Codes include:
`syntax`, `unknown_keyword` (closest keyword), `bad_pitch`,
`pitch_range`, `bad_duration` (lists valid tokens), `tick_fraction`,
`bar_length` (says how many beats the bar has and needs, and what single
rest would fix it), `bar_order`, `bar_overlap` (bar written twice),
`copy_source` (copy from unwritten bars), `dangling_tie`, `tie_mismatch`,
`fill_not_last`, `bar_after_pipe` (a number after `|` that is not the bar the
pipe starts: drop it, or start a new line with it),
`grid_steps` (states the step count needed), `grid_char`,
`unknown_drum` (closest names), `unknown_chord` (closest quality),
`velocity_range`, `channel_range`, `lane_order` (positions must not decrease),
`lane_value`, `position` (beat beyond the meter), `meter_mismatch`,
`meter_order`, `pickup_length` (bar 0 must equal the pickup), `no_pickup`
(bar 0 without a `pickup` header), `length_short` (explicit length before
the last event), `duplicate_voice`, `duplicate_pitch`.

### C.2 Canonical form (what `decompile` writes)

`decompile` must satisfy the **round-trip law**: for any `Sequence s`
(canonicalized), `compile(decompile(s)).sequence == s`, with the same ppq
and meter map. It also must be deterministic (same input → same bytes).
Bar starts use a cached prefix timeline; event grouping and voice assignment
use indexes while preserving the canonical ordering and existing output.
Canonical output, in order:

1. `notation 1`, `ppq N` (always), `meter N/D` (standalone only: the first
   bar's meter; changes written per B.10), `length N` or `length N+Mt` only
   when the item length is not the end of the last written bar.
2. Channel 10 as a grid voice named `drums` when `drumGrids` and every
   note on channel 10 fits one grid (try `s`, `e3`, `t`, `s3`, `x` in that
   order): all starts on steps, one common length (`len`), velocities that
   map to `x` (voice `vel` = most common velocity), `X`/`o` (the next two most
   common, if needed) or digits exactly; one row per instrument per bar,
   instruments in drum-map note order. If any note does not fit, channel 10
   decompiles like any other channel.
3. Per channel (ascending), notes are assigned to voices greedily: first
   group notes with identical start, length, velocity **and** off-velocity
   into chords (pitches ascending); then, in canonical order, each group goes
   to the lowest-numbered voice of that channel whose cursor is ≤ its start. A note that crosses a bar
   line is split and tied (`~`). Voices are named `c<ch>v<n>`
   (`c1v1`, `c1v2`), with `vel` set to the voice's most common velocity.
   Gaps become rests. Durations are written as the shortest correct token:
   single symbolic (`q`, `e.`, `e3`, ...), else a `+` sum of at most two
   symbolic values, else `Nt`. Velocities different from the voice's `vel`
   are written `:NN`; a non-zero off-velocity is written `:NN/MM` (with the
   on-velocity always present in that case). Token spelling preference when
   several are exact: plain (`w h q e s t x`), then dotted, double-dotted,
   tuplets (3, 5, 6, 7, 9); a `+` sum uses the largest possible first term.
   Copy lines refer to the earliest identical bar, merged into maximal
   ranges. No dynamics,
   articulations, chord symbols, layering or copies are emitted — except
   that a bar identical to an earlier bar of the same voice is written as a
   copy line (`5 = 1`, or merged ranges `5-8 = 1-4`), checked by recompile.
4. Lanes: one per (kind, channel, number), in order CC (by number), bend,
   pressure, poly (by pitch); every event is a `step` point written
   `BAR.BEAT[+Nt] VALUE` (events sharing a tick keep their order), except runs that `lin` reproduces exactly
   (verified by recompiling the run), which collapse to their endpoints.
   Bend is written raw.
5. `events` blocks per channel with programs and sysex.
6. Bars are written one per line as `N: tokens`, single spaces, no
   trailing spaces, trailing newline.

### C.3 Tests required

- A corpus under `libs/notation/tests/corpus/`: ≥ 40 hand-written documents
  covering every construct, each with an expected event listing (a simple
  text dump of the compiled Sequence) — the compiler's golden tests.
- ≥ 30 invalid documents, each with the expected error code, line/column
  and a check that the hint is non-empty and (where specified) contains the
  fix.
- Round-trip law as a property test: ≥ 2000 random Sequences (random notes
  with arbitrary ticks, overlaps, channels, velocities, CCs, bend, pressure,
  programs, sysex, several ppqs and meters) → decompile → compile → equal.
- SMF: read/write round trip; fixtures of format 0 and 1 files written by the
  test itself; running status; note-on velocity 0; unterminated notes.
- A libFuzzer target for `compile` (built with `SUPPLE_BUILD_FUZZERS`).

---

## Implementation notes

- A `#` starts a comment at a token boundary. Inside a token it remains part of
  a pitch or chord spelling, as in `F#4` and `{C7#9}`.
- With a `pickup`, `length B[+Nt]` counts the pickup and then B complete bars
  from bar 1. The item starts at tick 0, so bar 1 starts at `pickupTicks`.
- A lane's `+Nt` offset may span denominator beats but stays within its bar.
  This lets `1.1+1t` name tick 1 in a 6/8 bar at PPQ 1: the bar is three
  whole ticks even though individual eighth-note beat onsets can be fractional.
- A tie retains the first segment's note-on velocity and takes the final
  segment's note-off velocity. Intermediate segments emit no separate MIDI
  note. An exact duplicate controller write may be removed only when adjacent
  to the previous write at the same tick and address; removing it across an
  intervening value would change the final value.
- `SmfReadOptions::track` uses a zero-based track index. Its returned clip
  length is that track's end-of-track tick; `SmfInfo` still reports metadata
  found on all tracks. A standalone SMF F7 escape event is counted as skipped;
  F7 continuation chunks complete a preceding F0 sysex message.
