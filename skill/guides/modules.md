# Modules: the language

A module is one DSP script whose struct derives from `Module`. The checked
C++ subset, DSP objects, parameter factories, evaluation order, real-time
rules and Canvas API are those of `guides/dsp-language.md` (ADR-0032).
This guide gives the module's shape (ADR-0062); it replaces the plugin's
identity, buses, voices and editor layout.

The file compiles unchanged with the JIT or as C++ against
`<supple/script.h>`. One module per file:

```cpp
#include <supple/script.h>

struct Amplifier : Module {
    static constexpr ModuleInfo info = {
        .name = "Amplifier", .id = "com.example.amplifier", .vendor = "Example",
        .version = "1.0.0", .description = "A polyphonic level control.",
        .tags = {"amplifier", "utility"}};

    Input in = audioIn("In").help("The signal to scale.");
    Param level = percent("Level", 50).smoothed(10).help("How much passes.");
    Output out = audioOut("Out").help("The scaled signal.");

    void process(int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = in[c][i] * level.at(i, c);
    }
};
```

## Identity

Declare `static constexpr ModuleInfo info` inside the module struct.
Members follow this order:

| Field | Meaning |
|---|---|
| `name`, `id` | Display name and permanent reverse-DNS id, as a plugin's `Info` |
| `vendor`, `version` | Author and major.minor.patch version; version defaults to `"1.0.0"` |
| `description`, `url` | One sentence about its use, and a link |
| `tags` | At most four words, lower case letters, digits and `-`: `{"filter", "poly"}` |
| `width`, `height` | Face size in design units; 0 (default) takes the automatic layout's size |

An id and every parameter and port key identify saved patches. Keep them
once shared. Stock module ids are `com.suppleaudio.rack.<name>`; a fork
gets an id of its own.

## Ports and signals

Declare each `Input` or `Output` separately as a direct member of the
module struct. No arrays of ports or ports inside helper structs.

| Input factory | Output factory | Convention |
|---|---|---|
| `audioIn(name)` | `audioOut(name)` | Audio, nominally ±1; 1 is 0 dBFS at a rack output |
| `pitchIn(name)` | `pitchOut(name)` | Octaves from C4, 261.63 Hz; 1 per octave |
| `gateIn(name)` | `gateOut(name)` | 0 or 1; high at 0.5, triggers 1 ms high |
| `cvIn(name)` | `cvOut(name)` | Bipolar modulation, −1..1 |
| `unipolarIn(name)` | `unipolarOut(name)` | 0..1: levels, envelopes, probability |
| `phaseIn(name)` | `phaseOut(name)` | A 0..1 ramp per cycle |

`portSpec(name, SignalKind::Audio, false)` is the general factory:
`SignalKind` has `Audio`, `Pitch`, `Gate`, `Cv`, `Unipolar`, `Phase`;
the bool says output (`true`) or input (`false`). Choose the factory
matching the member's direction. Kinds describe meaning; they do not
clamp the signal.

Modifiers: `.help("sentence")`, `.in("Group")`, `.withKey("stable_key")`
and `.normal(value)`. A disconnected input reads its normal (default 0).
A port's key is its member's name in snake_case unless `.withKey()` gives
one. Ports and parameters share one key namespace. A module has at most
32 inputs and 32 outputs.

`in[c][i]` reads a channel's frame. `in.channels()` is 0 unconnected,
otherwise the widest cable into it; `in.connected()` says whether there
is a cable. Several cables into an input sum. Outputs arrive zeroed;
write `out[c][i]`. Indices outside 0..15 or outside the current frames
are reported; invalid sample reads are 0 and writes go to a spare sample.

## Polyphony

Cables carry up to 16 channels. A mono input reaches every channel; past
a poly input's width, valid channels read 0. An unconnected input's
normal reaches every channel. `channels()` is the widest connected input
or parameter cable, at least 1. Outputs start at that width each block;
`out.setChannels(n)` chooses another, clamped to 1..16;
`out.channels()` reads it.

Loop over channels and keep state separately in `Array<State, 16>`.
Seeding `Random` or `Noise` in `prepare()` with `seed()` makes the sequence
repeat on every render. The seed follows the project's seed and the
module's id. Give separate generators distinct derived seeds when needed.

## Parameters and cables

Declare parameters as in a plugin: physical units, ranges, defaults,
help, groups and smoothing. Every non-toggle parameter is a cable target
without a `.modulated()` declaration. Omit `.polyModulated()`, the
plugin voice declaration; module channels are selected by `at(i, c)`.

| Read | Value |
|---|---|
| `p`, `p.value()` | Knob and automation at the block's start |
| `p.at(i)` | Glided knob plus cables on channel 0 at frame `i` |
| `p.at(i, c)` | Glided knob plus cables on channel `c` at frame `i` |

A mono parameter cable reaches every channel; past a poly cable's width,
`at()` reads the knob and its glide alone. Outside the block's frame
range it reads the knob. Unlike port indexing, these fallbacks report no
out-of-range error. Percent parameters still read as fractions (50 reads
0.5). `.index()` and `.on()` read the knob; use `roundToInt(p.at(i, c))`
when a choice or integer should follow cables.

A cable's `amount` is the change at a signal value of 1: octaves for a
frequency (`knob * exp2(amount * signal)`), otherwise the parameter's
physical unit (percent, dB, milliseconds, semitones). Several cables sum;
the moved value stays in the parameter's range. Toggles take no cables.
Read `at(i, c)` where a cable should move DSP per frame: a level, cutoff
or rate. `update()` follows knob changes; it is not called per cable sample.

## Bit operations and gates

Use `shl(x, n)`, `shr(x, n)`, `bit(x, n)` and `popcount(x)` on `int`s.
They operate on the 32-bit representation, including the sign bit.
`shl` shifts left and discards overflow bits; `shr` shifts right with zero
fill. `bit` gives bit `n` as 0 or 1; `popcount` counts set bits (32 for -1).
Shifts and bit reads give 0 for `n < 0` or `n >= 32`. The ordinary shift
operators keep their existing constant-count checks.

`Trigger t` starts low. `t.rose(x)` updates its hysteresis state, returning
true on a rising edge: high at `x >= 0.5`, low at `x < 0.25`, unchanged
between. `t.high()` reads its state; `t.clear()` returns it low.

`Pulse p` starts silent. `p.fire()` restarts a 1 ms pulse at the current
sample rate, rounded up to a whole frame (45 frames at 44.1 kHz).
`p.fire(frames)` chooses an explicit length; zero or negative clears it.
Each `p.next()` returns 1 while consuming a frame, then 0; `p.clear()`
clears it. Both are small, copyable value types. Keep one per channel in
`Array<Trigger, 16>` and `Array<Pulse, 16>`.

Member and free helper functions may take `const Input&`, `Output&`,
`Param&` or `const Param&`, preserving the member's host binding and the
ordinary audio/UI and evaluation-order checks.

## Hooks and the rack's grid

| Hook | Use |
|---|---|
| `void prepare()` | Size Buffers and DelayLines, seed randomness, prepare DSP |
| `void update()` | Knobs changed; `p.changed()` identifies them |
| `void process(int frames)` | Read input ports and write output ports for `frames` frames |
| `void panel(Panel& p)` | Optional controls and jacks on the face |
| `void draw(Canvas& g)` | Optional drawing beneath the controls |
| `void pointer(const Pointer& p)` | Optional interaction with the drawing |
| `void factoryPatches(Patches& p)` | Named knob presets |

The runtime calls hooks; scripts do not call them. `sampleRate()`,
`maxBlockSize()` (32), `channels()`, `transport()`, `seed()` and
`trace(name, value[, i])` are available. `transport()` gives `playing`,
`tempo`, `beats`, `barStart`, `timeSigNumerator`, `timeSigDenominator` at
the block's start. `offline()` exists; making sound depend on it defeats
the same-render guarantee.

The rack processes blocks of at most 32 frames on a grid aligned to the
timeline. It samples knobs, updates and transport on that grid, regardless
of the host block size. Acyclic cables have no delay: a module's output
reaches its dependents in the same block. In a feedback group, modules
run one frame at a time in fixed id order. Cables going backwards in that
order, including self-feedback, carry the previous frame (ADR-0062 §5).
A process loop has a budget of `65536 * frames`; exhaustion stops that
module, silences its outputs and reports the problem.

## The face

Without `panel()`, controls and ports get an automatic layout grouped by
`.in()`. Most modules need nothing else. A custom face places controls and
jacks in design units from its top-left:

```cpp
#include <supple/script.h>

struct Monitor : Module {
    static constexpr ModuleInfo info = {
        .name = "Monitor", .id = "com.example.monitor", .tags = {"utility"}};
    Input in = audioIn("In").help("The signal to inspect.");
    Param level = percent("Level", 100).smoothed(10).help("Output level.");
    Output out = audioOut("Out").help("The scaled input.");
    Shared<double> last;

    void process(int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = in[c][i] * level.at(i, c);
        last.set(out[0][frames - 1]);
    }
    void panel(Panel& p) {
        p.size(120, 220);
        p.knob(level, 60, 55);
        p.input(in, 30, 110);
        p.output(out, 90, 110);
        p.display(10, 145, 100, 50);
    }
    void draw(Canvas& g) {
        g.fillStyle(Theme::text);
        g.fillText(format("%.3f", last.get()), 20, 170);
    }
    void factoryPatches(Patches& p) {
        p.add("Unity").set(level, 100);
        p.add("Half").set(level, 50);
    }
};
```

`p.knob(param, x, y[, size])` and jacks use their centres; knobs default
to 36 units (24 small, 52 large). `slider`, `toggle`, `choice` and
`readout` take `(param, x, y, w, h)`. `input` and `output` take the port
member and `(x, y)`. `display(x, y, w, h)` reserves drawing space.

The face runs in UI context, in its own copy. Audio state reaches it
through `Shared<T>` or `SharedArray<T, N>`. Parameter edits belong in
`pointer()` with `edit`, `editNormalized` or `reset`, exactly as a plugin's.
`factoryPatches()` contains only `p.add("Name").set(member, physicalValue)`
chains; omitted knobs take defaults. A choice may be set by its key.

## Refused and conventions

Modules refuse `Source` members and preset routes (`Patches::route`):
connect ports and parameters with rack cables. Voice hooks and
`Instrument` methods are replaced by polyphonic pitch and gate inputs.
`AudioFile` and `Convolver` members are not yet in modules. There is no
`setLatency`, `setTail`, `process(Block&)` or `editor()`; use ports,
`process(int frames)` and `panel(Panel&)`. Refusals use `script.module.*`
codes and give a hint naming the module way. Plugins in turn cannot use
module ports, `Panel`, `at(i, c)`, `transport()` or `seed()`.

Stock modules follow `modules/AGENTS.md`: original MIT code, one
`stock/<name>.cpp`, matching member and display names or explicit keys,
help on every port and parameter, musical defaults, grouped controls,
smoothing on audio levels. Gate inputs use hysteresis (rise at ≥0.5,
fall below 0.25); trigger outputs stay high for 1 ms. Keep per-channel
state independent, use `seed()` for repeatability, and size memory in
`prepare()`. Every stock module is tested against the JIT and interpreter
for the same bits as its native C++ build.
