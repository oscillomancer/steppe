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
| `vco` | The analog-style workhorse: sine, triangle, saw, pulse and sub at once, exp and through-zero FM, hard sync, poly |
| `filter` | A multimode state-variable filter: low, band, high, notch and 24 dB low pass at once, pitch tracking, steady self-oscillation, drive |
| `noise` | White, pink, brown and blue noise, crackle and clocked digital noise (hiss, metallic buzz, steps), poly |
| `delay` | A stereo tape-style delay up to 10 s: free or synced, ping-pong, feedback past 100 %, tone, wow and flutter, a send and return to patch into the loop |
| `reverb` | A lush stereo reverb, an eight-line feedback delay network: size, decay to freeze, damping, diffusion, modulation, shimmer, pre-delay |
| `ladder` | A four-pole ladder low pass with saturating feedback and bass compensation; 12 dB and resonant high-pass taps, poly |
| `fold` | A west-coast wavefolder, 4x oversampled: fold, symmetry, stages; and a soft-clip output |
| `lpg` | A vactrol-style low-pass gate: a ping makes plucks and bongos of any source; CV mode, resonance, its envelope as an output, poly |
| `string` | A plucked string, a waveguide per channel: decay, damping, brightness, pluck position, stiffness, velocity and an exciter input |
| `resonator` | A 24-mode modal resonator: structure morphs string, beam, bar, bell and plate; brightness, decay, damping, position, stereo spread, poly |
| `drum` | An analog-style drum voice, eight models on a knob (kick, snare, tom, closed and open hat, clap, cowbell, rimshot): tune, tone, decay, snap, accent, poly |

### Modulation, sequencing, generative and utilities

| Module | What it is |
|---|---|
