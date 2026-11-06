# Modules

The modules of Steppe's modular racks (ADR-0062, `docs/specs/rack.md`). A
module is one C++ file in the DSP script language, a struct deriving from
`Module`: its ports, its knobs and its `process()`. Steppe compiles a module
the moment it is saved; stock modules are also compiled into the app.
Sharing a module is sharing its file. The language: `skill/guides/modules.md`.
The rules for this folder: `AGENTS.md`.

```cpp
#include <supple/script.h>

struct Fold : Module {
    static constexpr ModuleInfo info = {.name = "Fold", .id = "com.example.fold",
                                        .tags = {"waveshaper"}};
    Input in = audioIn("In").help("What it folds.");
    Param folds = percent("Folds", 30).help("How far past the edge the wave folds.");
    Output out = audioOut("Out").help("The folded wave.");

    void process(int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = sin(in[c][i] * (1 + 8 * folds.at(i, c)));
    }
};
```

## Stock modules

All `com.suppleaudio.rack.<name>`. Host modules (written in the engine:
Out, In, Notes, Transport, Plugin) are in `docs/specs/rack.md` §5.

### Sources, filters, shapers and effects

| Module | What it is |
|---|---|
| `vca` | An amplifier a CV opens, exponential or linear |

### Modulation, sequencing, generative and utilities

| Module | What it is |
|---|---|
| `adsr` | An envelope with curved stages, gated, triggered or looping, with an end trigger |
| `lfo` | Seven shapes, free or locked to the song position, morphable, with poly spread |
| `sequencer` | Sixteen steps of notes, gates, lengths (ties) and chances; five walks; clock or song sixteenths |
| `random` | Clocked random notes and three gates with a looping memory that mutates by Change; scales; poly |
| `function` | Rise and fall with a curve: an envelope, a cycling LFO, a slew limiter or a follower |
| `quantizer` | Pitch to the nearest note of 22 scales or a custom one, transposed in degrees; keyboard face |
| `clock` | Six ratios (/16 to x8) of the song, a free tempo or an incoming clock, with swing and a reset out |
| `euclid` | A Euclidean rhythm with its own accent spread, gate, accent and off-beat outputs, circle face |
| `turing` | Shift Register: a looping random register, locked to chaotic by Flip, with a CV and four bit gates |
| `chance` | A coin toss per gate to A or B, latched as a random switch, or a burst of ratchets; poly |
| `chord` | A note to a poly chord: 22 types or a key's own thirds, inversions, voicings and a strum |
| `arp` | An arpeggiator over held poly notes: six orders, octaves, gate length, latch |
| `slew` | A slew limiter and portamento, linear to exponential, that can leave leaps alone; poly |
| `hold` | Sample and hold or track and hold, noise when nothing is in, a value per voice; poly |
