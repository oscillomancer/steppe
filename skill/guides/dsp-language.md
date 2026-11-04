# DSP scripts: the language

The canonical reference for DSP scripts (ADR-0032): plugins written as one
C++ file, compiled when they are saved and run by the player plugin. The
same file also compiles as ordinary C++ against `<supple/script.h>`, which
is how a script becomes a standalone plugin, and it means the same thing
both ways. Workflow and commands: `guides/dsp-workflow.md`.

## A script

```cpp
#include <supple/script.h>

// A tilt equaliser: highs up and lows down around a pivot, or the reverse.
struct Tilt : Effect {
    static constexpr Info info = {.name = "Tilt", .id = "com.example.tilt", .vendor = "Example"};

    Param tilt = decibels("Tilt", -6, 6, 0).help("Positive brightens: highs up, lows down.");
    Param pivot = frequency("Pivot", 200, 5000, 1000).help("Where the two shelves meet.");
    Param output = decibels("Output", -12, 12, 0).smoothed(20);

    Array<Biquad, 2> low, high;

    void update() {
        for (int c = 0; c < 2; ++c) {
            low[c].lowShelf(pivot, 0.707, -tilt * 0.5);
            high[c].highShelf(pivot, 0.707, tilt * 0.5);
        }
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) {
                double x = high[c].process(low[c].process(io[c][i]));
                io[c][i] = x * dbToGain(output.at(i));
            }
    }
};
```

- The first line is `#include <supple/script.h>`, the only include there is.
- One struct derives from `Effect` (audio in, audio out) or `Instrument`
  (notes in, audio out; see Instruments below): the plugin. Others are
  plain structs it holds as members. `NoteEffect` is refused: scripts send
  no notes yet.
- `static constexpr Info info` is its identity; `Param` members are its
  parameters; `prepare()`, `update()` and `process(Block&)` are the hooks.
- Every member has a starting value (`double phase = 0.0;`), or is an object
  that starts empty (`Biquad`, `DelayLine`, `Array<...>`).
- Numeric literals use a decimal point (`1.25`, `1.25f`) regardless of the
  host's locale. Float literals round directly to float; underflow can give zero.

## Identity: `Info`

| Field | Meaning |
|---|---|
| `name` | Shown in hosts and in `plugin.list` |
| `id` | Reverse DNS, `com.example.tilt-eq`: words of letters, digits and `-` joined by `.` (no `_`). Saved sessions find the plugin by it: never change it once the plugin is used |
| `version` | Major, minor, patch: `"1.0.0"` (the default) |
| `vendor`, `description`, `url` | As they say |
| `features` | What the plugin is, as CLAP features after the main category (which the kind gives): `{"reverb"}`, `{"equalizer", "stereo"}`; lower case, digits and `-`, at most four. Hosts sort plugins by them, and Steppe's plugin browser shows a stock plugin's first one after its name |
| `channels` | Main bus widths offered, the first the default: `{2, 1}` (the default) is stereo, then mono |
| `sidechain` | `true`: a key input as wide as the main bus (`io.sidechain(c)`) |
| `outputs` | Names of extra outputs (`io.output(bus, c)`) |
| `voices` | An instrument's voices, 1 to 64 (8 when not given) |
| `latency` | Samples of delay the plugin adds, reported to the host |
| `tail` | Samples of output after the input stops; `-1` forever (feedback). `setTail()` when it follows the settings |

## Parameters

A parameter is a member made from a description:
`Param gain = decibels("Gain", -24, 24, 0);`. The description sets its name,
range, default and unit; Steppe, hosts and agents see it in that unit (a
patch says `"gain": -3`), exactly as with the stock plugins.

| Factory | Unit, travel |
|---|---|
| `decibels(name, min, max, def)` | dB, linear |
| `frequency(name, min, max, def)` | Hz, logarithmic |
| `milliseconds(name, min, max, def)` | ms; logarithmic when `min > 0`, else cubic |
| `percent(name, def, min = 0, max = 100)` | given in percent, **read as a fraction** (30 reads 0.3) |
| `ratio(name, min, max, def)` | a ratio (4 is 4:1), logarithmic |
| `semitones`, `cents`, `octaves`, `degrees` `(name, min, max, def)` | as named, linear |
| `integer(name, min, max, def)` | a whole number; read `.index()` |
| `choice(name, {"Low Pass", "High Pass"}, def)` | an option; read `.index()` (0, 1, ...) |
| `toggle(name, def)` | on or off; read `.on()` |
| `continuous(name, min, max, def, Unit::...)` | anything else: `Unit::Bpm`, `Unit::Samples`, `Unit::Meters` (a distance: a room's size), or no unit (`Unit::None`, the default) |

Modifiers chain: `decibels("Output", -24, 12, 0).smoothed(20).help("...")`.
Ranges and smoothing times must be finite, power-curve exponents finite and
positive, and integer parameter ranges must fit in `int`.

| Modifier | Effect |
|---|---|
| `.help("one sentence")` | The tooltip and the schema's description. Write one for every parameter |
| `.in("Group")` | A section in the editor and in hosts |
| `.smoothed(ms)` | The framework glides it per sample; read `p.at(i)` in `process` |
| `.offAtMin()` | Its minimum means off ("-inf dB"); `.gain()` is exactly 0 there |
| `.meter()` | An output the plugin reports with `p.set(v)`: read-only, never saved |
| `.modulated()` | The host may modulate it, and so may the patch's routes from global sources: read `p.routed(i)` (see Modulation) |
| `.notAutomatable()` | As named |
| `.restarts()` | Changing it restarts the plugin (the host stops and starts it), so `prepare()` runs again with the new value and may `setLatency()` for it: a lookahead. Not automatable |
| `.polyModulated()` | An instrument's: modulated per note by the host and by routes from any source; each voice reads `p.forVoice(v)` |
| `.routeRange(min, max)` | How far routes may move it, past its own range: a fine tune of ±100 ct that an envelope sweeps by octaves, `.routeRange(-4800, 4800)` |
| `.role({"level"})` | What it does, for agents: `level`, `time`, `brightness` ... |
| `.keys({"db12", "db24"})` | A choice's patch keys, one per option, where its labels make poor ones (see below) |
| `.curved(Curve::Log)`, `.curved(Curve::Pow, k)` | Knob travel |
| `.activeWhen(on)`, `.activeWhen(mode, {"Bell", "Notch"})` | It applies only while the toggle `on` is on, or the choice `mode` is one of these options (see below) |
| `.activeUnless(on)`, `.activeUnless(mode, {"Low Cut"})` | The other way about: while `on` is off, or `mode` is none of these |
| `.activeAbove(voices, 1)`, `.activeBelow(depth, 90)` | It applies only while the number (continuous or integer) is above, or below, the value: in its unit as a patch says it (a percent as 30) |
| `.withKey("key")`, `.withId(n)` | Override the patch key or the CLAP id (see below) |

Reading: a `Param` reads as a `double` in its unit (`gain * 0.5`), with
`.value()`, `.at(i)` (the glide at frame `i`), `.index()`, `.on()`,
`.gain()` (dB as linear gain) and, in `update()`, `.changed()`.

**When a parameter applies.** A setting that only matters sometimes says
so: a band's controls while the band is on, a shelf's gain but not a cut's,
a free rate unless synced.

```cpp
Param on = toggle("Band On", true);
Param shape = choice("Shape", {"Bell", "Low Shelf", "Low Cut"}, 0).activeWhen(on);
Param gain = decibels("Gain", -24, 24, 0).activeWhen(on).activeUnless(shape, {"Low Cut"});
```

A number's conditions say a range, strictly: unison's detune and spread with
`.activeAbove(voices, 1)`, an LFO's rate with `.activeAbove(depth, 0)`, and
both together for a window.

Each call adds a condition and all must hold. A condition names a toggle, a
choice (its options by label or key) or a number declared above, and the
parameter it names must apply too: `gain` above needs no `.activeWhen(on)` of its own if
`shape`'s already has one, but saying so does no harm. At most 4 per
parameter. Hosts and agents see which parameters apply (the schema's
`"active": false`, described in its `active_when`), descriptions leave the
others out, and the editor dims their controls (the automatic one too). The audio code is the
script's: it still reads every value, and an inactive one keeps its value
for when it applies again.

**Factory patches** are declared in the plugin struct, in patch values (a
percent as 30, though the audio code reads it as 0.3):

```cpp
void factoryPatches(Patches& p) {
    p.add("Flat").set(tilt, 0).set(pivot, 1000).set(output, 0);
    p.add("Warm").set(tilt, -3).set(pivot, 800).set(output, 0);
    p.add("Air").set(tilt, 3).set(pivot, 2500).set(output, -1);
}
```

Each patch is `p.add("Name")` and `.set(member, value)` calls with a
constant, a bool or a choice's key (`.set(shape, "soft")`), `.route()`
calls for its routes (see Modulation), and `.file(sample, "factory:pad.flac")`
for a file member's factory file (see Files); a parameter left out takes its
default. Nothing else belongs here (no loops or locals), and
the compiler checks names, ranges and choices.

**Keys and IDs.** A parameter's patch key is its member's name in
snake_case (`lowCut` is `low_cut`), and its CLAP id is a hash of the key.
Renaming a member changes both, and sessions that saved the old one lose
that value: give the new name `.withKey("old_key")` to keep it. A choice's
options have keys too, each its label in snake_case (`"Low Pass"` is
`low_pass`, and a label that starts with a digit gains a `p`: `"12 dB/oct"`
is `p12_db_oct`). Where labels make poor keys, name them: a unit before the
number, as the stock plugins do, `choice("Slope", {"12 dB/oct", "24 dB/oct"},
0).keys({"db12", "db24"})`. Patches may give a choice by key or by label. A member
cannot share its factory's name, as in C++ (`Param ratio = ratio(...)`
does not compile): write `Param slope = ratio("Ratio", 1, 20, 4).withKey("ratio")`.

Follow the stock plugins: physical units with sensible ranges, a name a
person would say ("Cutoff", not "Freq1"), a `.help()` line for each, `.in()`
groups once there are more than five or six, and `.smoothed()` on anything
that scales audio directly (levels, mix) so that moving it does not click.

## Hooks

| Hook | When | What goes there |
|---|---|---|
| `void prepare()` | Before audio starts, and when the sample rate or block size changes. `sampleRate()` and `maxBlockSize()` are known | Allocation (`DelayLine::prepare`, `Buffer::resize`), seeding, anything sized from the sample rate |
| `void update()` | After `prepare()`, then at the start of a block whenever a parameter changed (blocks are split at each change, so automation is sample-accurate) | Turning parameters into coefficients; `p.changed()` says which moved |
| `void process(Block& io)` | Every block | The audio |
| `void editor(Editor& e)`, `void draw(Canvas& g)`, `void pointer(const Pointer& p)` | The editor's (optional) | Its controls, its drawing, and what the pointer does to the drawing: see The editor |

There is no `reset()` to write: the runtime returns every member to what
`prepare()` left, by itself, including the tail set there. A hot reload also
keeps the new code's prepared tail until `update()` or `process()` changes it.
The script never calls a hook. Values that
glide or fade of the script's own should jump to where they are going until
the first block plays (a `bool unplayed = true;` set in `prepare()` and
cleared at the top of `process()`): a host sets a fresh instance's values
after `update()` has already run once, and a render must depend only on the
values it is given, as `.smoothed()` parameters do by themselves.

`Block`: `io.channels`, `io.frames` (a block may be a single frame),
`io[c][i]` (read the input, write the output, in place), `io.transport`
(`playing`, `tempo`, `beats`, `barStart`, time signature),
`io.sidechain(c)[i]` (the key input with Info's `.sidechain`; silent until
`io.sidechainConnected`, which is false while nothing is routed to the key
in Steppe and in hosts that deactivate an unused port; a host that always
feeds the port reports it connected, so a plugin there may want a choice
parameter for its key), `io.output(bus, c)[i]`.
From any member function: `sampleRate()`, `maxBlockSize()`, `channels()`,
`offline()` (the host renders a bounce; it may change between blocks, and
a script that sounds different offline no longer renders the same as it
plays). In `prepare()`, `setLatency(samples)` reports a delay the plugin
adds that depends on the sample rate (an `Oversampler`'s, a lookahead),
where Info's `.latency` is fixed. A delay a parameter chooses (a
lookahead switch) needs `.restarts()` on that parameter: the latency
changes only across a restart, which `prepare()` sees. A tail that follows
the settings or the song, as a delay's follows its time, feedback and the
tempo, is `setTail(samples)` (`-1` forever) from `prepare()`, `update()` or
`process()`: the host hears of each change, and a render ends when it has
passed.

**Tracing.** `trace("name", value)` in `prepare()`, `update()`, `process()`
or a voice hook records a value at the block's start, `trace("name", value,
i)` at its frame `i`, for whoever is debugging: `dsp_test` returns each
name's last, lowest and highest values over the signal (`traces`), and says
when one was NaN. The name is a string literal of up to 47 characters, at
most 16 names a plugin. While nothing traces, a call costs a check, so a
trace left in is harmless, but take them out before the plugin ships.

Saving a script while it plays swaps the new code in with a 10 ms crossfade
when the plugin is otherwise the same. When a save changes what the plugin
says of itself (its name, parameters, keys, help, patches, buses, latency or
tail), the plugin is redefined instead: the host restarts it, which may be
heard.

## Instruments

An instrument derives from `Instrument`. The runtime gives each note to one
of its voices (stealing the oldest when all are busy, holding released
notes while the sustain pedal is down), and calls its voice hooks. The
script keeps each voice's sound in its own array, indexed the same way:

```cpp
#include <supple/script.h>

// A polyphonic synth: a saw per voice through a low-pass filter and an
// envelope, and an LFO for the patch's routes to move the filter with. With
// Glide above 0 it plays one note at a time, sliding.
struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 8};

    Param attack = milliseconds("Attack", 1, 2000, 5).help("How fast a note rises.");
    Param release =
        milliseconds("Release", 5, 5000, 300).help("How long a note fades once its key is up.");
    Param cutoff = frequency("Cutoff", 200, 16000, 3000)
                       .polyModulated()
                       .help("The low-pass filter's corner, which routes move for each voice.");
    Param rate = frequency("LFO Rate", 0.1, 20, 4).help("How fast the LFO swings.");
    Param voices = integer("Voices", 1, 8, 8).help("How many notes sound at once.");
    Param glide = milliseconds("Glide", 0, 1000, 0)
                      .help("The slide from note to note; above 0, one note at a time.");
    Param level = decibels("Level", -48, 0, -12).smoothed(20).help("The output level.");

    // A sine for each note, from its start: "lfo>cutoff" in the patch's
    // routes makes a wah of it, "velocity>cutoff" opens harder notes.
    Source lfo = voiceSource("LFO").help("A sine for each note, from its start.");

    struct Voice {
        Oscillator osc;
        Adsr env;
        Biquad filter;
        double phase = 0.0;  // the LFO's
        int tick = 0;        // frames until the filter follows its routes again
    };
    Array<Voice, 8> voice;

    void factoryPatches(Patches& p) {
        p.add("Wah").set(cutoff, 900).route("lfo>cutoff", 1.5).route("velocity>cutoff", 1);
    }

    void prepare() {
        for (Voice& v : voice) v.osc.setShape(Oscillator::Shape::Saw);
    }

    void update() {
        for (Voice& v : voice) v.env.set(attack, 200, 0.7, release);
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
        setVoiceLimit(voices.index());
    }

    // Voice v starts a note. retrigger is false when it glides on from the
    // note before (Legato): its envelope and LFO keep going.
    void startVoice(int v, bool retrigger) {
        if (!retrigger) return;
        voice[v].env.gate(true);
        voice[v].phase = 0.0;
        voice[v].tick = 0;
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                // Every 16 frames of the note: the LFO on, then the cutoff
                // with its routes from the sources as they are now.
                if (s.tick == 0) {
                    lfo.set(v, sin(twoPi * s.phase));
                    s.filter.lowpass(cutoff.forVoice(v), 0.707);
                    s.phase = fmod(s.phase + rate * 16 / sampleRate(), 1.0);
                }
                s.tick = (s.tick + 1) % 16;
                s.osc.setFrequency(voiceHz(v, i));
                const double tone = s.filter.process(s.osc.next());
                const double x = tone * s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!s.env.active()) voiceFinished(v);
        }
    }
};
```

| Hook | When |
|---|---|
| `void startVoice(int v)` | Voice `v` starts a note. Or `startVoice(int v, bool retrigger)`: `retrigger` is false in Legato mode for a note that glides on from the one sounding |
| `void releaseVoice(int v)` | Its key is up (and no pedal holds it): start the release |
| `void stopVoice(int v)` | (optional) Silence it at once: stolen for a new note, or choked |

`process()` receives `io` silent and adds every voice into it. Once a
voice's release has died away it calls `voiceFinished(v)`, which tells the
host that note is over: a voice never finished keeps its note open forever.

| Method | Gives |
|---|---|
| `voiceCount()` | Info's `.voices` (every voice there is; a limit plays fewer of them) |
| `voicePlaying(v)` | From `startVoice` until `voiceFinished` |
| `voiceKey(v)`, `voiceChannel(v)`, `voiceVelocity(v)` | The note: MIDI key, channel, velocity 0..1 |
| `voicePitch(v, i)`, `voiceHz(v, i)` | At frame `i`: semitones (key, glide and pitch bend), and Hz |
| `voiceVolume(v)`, `voicePan(v)` | The note's expressions as the host sends them: a gain 0..4 (1 until sent), 0..1 (0.5, the centre) |
| `voiceVibrato(v)`, `voiceExpression(v)`, `voiceBrightness(v)`, `voicePressure(v)` | 0..1, 0 until sent (MPE arrives as these; a tuning expression is in `voicePitch`) |
| `setVoiceMode(VoiceMode::Poly)` | Or `Mono` (the newest key held, returning to the last), `Legato` (Mono, gliding without restarting) |
| `setGlide(ms)`, `setBendRange(semitones)` | The slide between notes (Mono, Legato); the bend's reach (2) |
| `setVoiceLimit(n)` | Poly mode plays at most `n` voices at once, 1 to `voiceCount()`: a Voices parameter. Lowering it stops the voices above it (through `stopVoice`) |

## Modulation: sources and routes

A plugin's modulators are its own code; where their values go is the
patch's. A **source** is a member the script sets, and the patch's
**routes** connect sources to parameters, each with an amount:

```cpp
Param cutoff = frequency("Cutoff", 200, 16000, 3000).polyModulated();
Source lfo = voiceSource("LFO").help("A sine for each note, from its start.");
// in process(), every 16 frames of a voice:
lfo.set(v, sin(twoPi * phase));
filter.lowpass(cutoff.forVoice(v), 0.707);  // the cutoff with its routes, now
```

```json
"routes": {"lfo>cutoff": {"amount": "+1.5 oct"}, "velocity>cutoff": {"amount": 1}}
```

| Declaration | A source |
|---|---|
| `Source s = source("Macro 1")` | One value for the whole plugin; `s.set(x)` |
| `Source s = voiceSource("LFO 1")` | An instrument's: a value per voice, `s.set(v, x)` (or `s.set(x)` for every voice) |
| `.unipolar()` | Its values are 0..1 (an envelope); otherwise -1..1 (an LFO). Values are kept to the range |
| `.help("...")`, `.withKey("lfo1")` | As a parameter's; the routes name a source by its key (the member's name in snake_case) |

At most 32. `s.get(v)` reads one back. An instrument also has these sources
of its own, which the script never sets: `velocity`, `key` (-1 at key 0, 0 at
middle C, 1 at key 120: an amount of 5 oct tracks a cutoff an octave per
octave), `random` (new for each note), `pressure` and `slide` (the note's
pressure and brightness expressions) per voice, and `modwheel`, `breath`,
`expression` (CC 11), `aftertouch` and `pitchbend` for all voices.

Routes move a parameter marked `.modulated()` (from global sources) or, in
an instrument, `.polyModulated()` (from any source). The audio code reads
the moved value where it uses it: `p.forVoice(v)` for a voice, `p.routed(i)`
at frame `i` otherwise. Each is the parameter's value plus each route's
amount times its source's value (and its `via` source's), from the values
the sources hold at that moment, kept to the parameter's range (or its
`.routeRange()`). So set a source before reading what it moves, and read
moved values as often as they should move: per frame, or every few frames
of a voice. Nothing is read at a fixed rate for you, and `value()` and
`at(i)` never include routes.

An amount is in the parameter's unit at the source's full value: dB for a
level, semitones or cents for a pitch, percent for a percent; a frequency's
is in octaves. A patch may write it with a unit that converts (`"12 st"` for
an octave, `"50 ct"`). A route through a second source, `{"amount": 0.2,
"via": "modwheel"}`, scales by that source too: vibrato under the mod wheel.
A source with a value per voice cannot move a global parameter. At most 64
routes. Factory patches route too, by key:
`p.add("Wah").set(cutoff, 900).route("lfo>cutoff", 1.5).route("velocity>cutoff", 1)`,
and `.route("lfo>pitch", 0.2, "modwheel")` through a second source.

The schema lists the sources (`"sources"`) and, for each parameter routes
move, the unit of its amounts and whether a voice's sources reach it
(`"route"`); routes are saved with the plugin and its presets, and a
plugin's description in prose ends with them. In the editor they are a list
(the automatic editor's, or `e.routes(...)` in a script's own): a menu for
a route's source, parameter and via source, and its amount a bar to drag,
turn with the wheel or type into after a double click.

## Files

A sampler's sample or a reverb's room is a file the user (or an agent)
picks, declared as a member of the plugin struct like a parameter:

```cpp
AudioFile sample = audioFile("Sample").help("The sound every key plays.");
Convolver room = impulseResponse("Room").help("A recording of a space's echo.");
```

Its path is a property of the plugin, saved with it and set in a patch,
`{"properties": {"sample": "/home/me/kick.wav"}}` (the key is the member's
name in snake_case, or `.withKey("...")`). File keys use lowercase letters,
digits and underscores, without dots. The runtime loads WAV, AIFF or FLAC
off the audio thread, resamples to the session's rate, and hands
it over at the start of a block; until then, and with no file, `loaded()`
is false. Not in `prepare()`: files arrive after it.

| Object | Use |
|---|---|
| `AudioFile` | `loaded()`, `channels()`, `frames()`; `sample(c, i)` (0 outside; a channel past the file's last reads its last), `read(c, position)` between samples (cubic), for playing at another speed; `peak(c, from, to)` and `rms(c, from, to)` of frames [from, to), measured when the file loads, so the whole file costs about what a few hundred `sample()` calls do (a waveform overview, onsets, normalizing) |
| `Convolver` | `process(c, x)`: channel `c` through the impulse response (its first 20 s), no latency; 0 until loaded. `loaded()`, `length()` |

Files are members of the plugin struct only, one per member (no arrays),
and never assigned: read them, or keep what you need in members of your
own.

**Factory files.** A plugin built from a script (a stock plugin, or a
source export) may carry audio inside its binary, listed in its
`product.cmake` as `PRODUCT_ASSETS`. A file property names one as
`"factory:<name>"` (`"factory:pad.flac"`) instead of a path, and it plays in
any host on any system. `audioFile("Sample").withDefault("factory:pad.flac")`
starts every new instance with it, and a factory patch picks one with
`.file(sample, "factory:keys.flac")`; a patch or an agent may set one as it
sets a path, and the schema lists those the plugin has. A source export
does not add `PRODUCT_ASSETS` itself: list the files there. The script
player carries none: there a factory default is reported missing and
nothing loads (`loaded()` is false), a patch's `factory:` path is refused,
and the plugin built from the script plays them. Once
shipped, a factory file's name and sound are permanent, like a parameter's
key: sessions keep the name and play whatever it holds.

## The editor

With no editor code a plugin gets the automatic editor: every parameter, by
group (`.in()`), with meters, and a plugin with routes their list. To draw its own, it adds `editor()`, `draw()`
or both, and `pointer()` to make its drawing respond to the mouse:

| Hook | When | What goes there |
|---|---|---|
| `void editor(Editor& e)` | Once, as the editor opens (and after a reload) | Its size and its controls |
| `void draw(Canvas& g)` | At each repaint | What lies under the controls: panels, graphs, meters, text |
| `void pointer(const Pointer& p)` | As the pointer acts where no control is (needs `draw()`) | Handles to drag, shapes to click: edits parameters with `Param::edit()` |

```cpp
#include <supple/script.h>

struct Level : Effect {
    static constexpr Info info = {.name = "Level", .id = "com.example.level"};

    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level out.");
    Shared<double> peak;  // what the editor shows, sent by the audio code
    double held = 0.0;    // the audio code's own

    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i)
            for (int c = 0; c < io.channels; ++c) {
                io[c][i] *= dbToGain(gain.at(i));
                held = max(abs(io[c][i]), held * 0.9999);
            }
        peak.set(gainToDb(held));
    }
    void editor(Editor& e) {
        e.size(240, 180);
        e.knob(gain, 20, 50, 88, 100);
    }
    void draw(Canvas& g) {
        g.panel(8, 8, 224, 164, "Level");
        g.meter(peak.get(), 150, 50, 20, 100);
        g.fillStyle(Theme::textMuted);
        g.monoFont(11);
        g.fillText(format("%.1f dB", peak.get()), 140, 166);
    }
};
```

Editor code runs on the host's main thread, in a copy of the plugin of its
own. There each `Param` reads the value being heard (with the host's
modulation; a `.meter()` what the plugin reports) and each `Shared` member
what the audio code last sent; any other member it uses is the editor's own
(a zoom, a hover). A member both editor code and `prepare()` or `process()`
use is refused, so the audio code shares what the editor shows:

| Member | Audio code | Editor code |
|---|---|---|
| `Shared<double> level;` (or `float`, `int`, `bool`) | `level.set(x)` | `level.get()` |
| `SharedArray<float, 256> scope;` (an `Array`) | `scope[i] = x` | `scope[i]` |

They are members of the plugin struct, start at zero, and arrive together,
as of one moment, about 120 times a second while the editor is open. Editor
code never changes them, and changes a parameter only as a user would:
through a control, or by `edit()` in `pointer()`.

Controls, `e.knob(p, x, y, w, h)` and the like, are the framework's own,
bound to a parameter and labelled with its name, in design units (the
window scales everything): `knob`, `slider` (along its longer side),
`toggle`, `choice` (buttons for up to four options, a menu for more),
`readout` (shown, not edited: a `.meter()`), `xyPad(across, up, ...)`,
`envelope(attack, decay, sustain, release, ...)`, `file(member, ...)` (an
`AudioFile` or `Convolver`: its name; a click chooses, a dropped file is
taken), `keyboard(lowKey, highKey, ...)` (an instrument's) and
`meters(...)` (the input and output peak meters), and `routes(...)` (a
plugin with routes: the list of them, each with its source, the parameter it
moves, its amount and its via source, to change and add to; 29 units a
row under a 22-unit header, scrolling when they do not fit). `e.size(w, h)`: 480 by 320 unless given.

`Canvas` is HTML canvas's 2D context, as methods:

| Calls | |
|---|---|
| `fillStyle(c)` `strokeStyle(c)` `globalAlpha(a)` `lineWidth(w)` `lineCap(LineCap::Round)` `lineJoin(LineJoin::Round)` `linearGradient(x0, y0, x1, y1, from, to)` `radialGradient(x, y, r0, r1, from, to)` | Styles, until changed or `restore()`d. A colour is `0xRRGGBB` or one of the palette's: `Theme::ground` `surface` `raised` `recess` `line` `lineLit` `textFaint` `textMuted` `text` `accent` `accent2` `modulation` `alert`. Fill and stroke start as `Theme::text` |
| `fillRect` `strokeRect` `clearRect` `(x, y, w, h)` | Each a path of its own |
| `beginPath()` `moveTo` `lineTo` `bezierCurveTo` `quadraticCurveTo` `arc(x, y, r, start, end)` `arcTo` `rect` `roundRect(x, y, w, h, r)` `ellipse(x, y, rx, ry)` `circle(x, y, r)` `closePath()`, then `fill()` or `stroke()` | Paths. Angles in radians, 0 to the right, clockwise |
| `save()` `restore()` `translate` `rotate` `scale` `clip(x, y, w, h)` | Transforms and a rectangular clip |
| `font(size)` `boldFont(size)` `monoFont(size)` `serifFont(size)` `serifItalicFont(size)` `textAlign(TextAlign::Center)` `textBaseline(TextBaseline::Middle)` `fillText(text, x, y)` `measureText(text)` | Text: Inter, `monoFont` (Plex Mono) for numbers, `serifFont` (Fraunces SemiBold) for a name or heading, `serifItalicFont` for an aside |
| `panel(x, y, w, h, "Title")` `well` `meter(levelDb, ...)` `frequencyGrid(x, y, w, h, lowHz, highHz)` `levelGrid(x, y, w, h, lowDb, highDb, stepDb)` | The editor's furniture, in its look |
| `width()` `height()` | The editor's size |

Text is a string literal, `format("%.1f dB", x)` (printf's number
conversions, up to four values), or a parameter's `p.text()` (its value as
hosts show it) and `p.name()`; it lasts until the hook returns, so a local
`const char*` or `Array<const char*, N>` holds it but a member cannot, in
an `Array` or a struct either. `hzToX(hz, lowHz, highHz, x, w)`
and `dbToY(db, lowDb, highDb, y, h)` place a graph's points on the grids'
lines. To draw a filter's response, give the editor a `Biquad` of its own,
designed from the parameters in `draw()`, and plot its `responseDb(hz)`.
Editor code may loop freely within a generous budget; a hook that spends it
stops, and the editor says where. `dsp.preview` saves the editor as a PNG.

`pointer(p)` hears what the mouse does where no control is, and the editor
repaints after each call. `p.action()` is what happened:

| `PointerAction::` | |
|---|---|
| `Press` | The main button went down here (`p.doubleClick()`: the second of two). The pointer is the script's until the release |
| `Drag` | It moved with that button down, wherever it went: `p.dx()`, `p.dy()` since the last |
| `Release` | The button came up (or the editor lost focus mid-drag) |
| `Move` | It moved with the button up: a hover, over controls too |
| `Wheel` | The wheel turned here: `p.wheel()` notches, positive up |
| `Leave` | It left the editor |

`p.x()`, `p.y()` are where, in design units; `p.over(x, y, w, h)` and
`p.within(x, y, radius)` test it; `p.shift()` (fine adjustment, by custom),
`p.ctrl()` and `p.alt()` are the keys held. `xToHz(x, lowHz, highHz, gx, gw)`
and `yToDb(y, lowDb, highDb, gy, gh)` undo `hzToX` and `dbToY`. A parameter
changes as a user turning its knob would change it: `p.edit(value)` (in its
units, kept in range), `p.editNormalized(position)` (0 to 1, as its knob
travels; `p.normalized()` reads it) or `p.reset()` (its default). The host
records it, the audio code hears it, and `value()` has it at once. A
press's edits, through its drags to its release, are one gesture, however
many parameters they change; a wheel's are one per burst, and
edits on a `Move` or a `Leave` do nothing. Only `pointer()` (and what it
calls) may edit; `draw()` runs with no user acting. An instrument's
`pointer()` may also play itself, as its keyboard does:
`playNote(key, velocity, on)` (velocity 0 to 1) on a `Press`, `Drag` or
`Release` reaches the audio code as a host's note does, through
`startVoice()`; a note still on when the press ends is ended then, so
none is left sounding. A drum pad played where it is clicked is
`playNote(36 + pad, velocity, true)` on its press. A hover or a drag
state is a member of the editor's own:

```cpp
    bool held = false;  // the editor's own

    void pointer(const Pointer& p) {
        if (p.action() == PointerAction::Press) held = p.within(nodeX(), nodeY(), 10);
        if (p.action() == PointerAction::Drag && held) {
            cutoff.edit(xToHz(p.x(), 20, 20000, 20, 440));
            resonance.edit(resonance - p.dy() * (p.shift() ? 0.002 : 0.02));
        }
        if (p.action() == PointerAction::Release) held = false;
    }
```

The `bell` example drags an EQ band's node. `dsp.preview` takes pointer
actions (a click, a drag, a turn of the wheel) before it draws, and says
what they edited, so a script's `pointer()` can be checked without a
mouse.

## Types and statements

- Numbers: `double` (use it by default), `float`, `int`, `bool`. Integer
  arithmetic wraps; float arithmetic stays float, mixed promotes to double.
- `enum class Mode { A, B };`, named `Mode::A`.
- `struct`s with members, default member values and member functions
  (`const` ones too). Free functions. `constexpr` globals and
  `static constexpr` members for constants. As in C++, a free function or
  a global constant is used only below its declaration (a member function
  may call any member).
- `Array<T, N>`: fixed size, any element type, `= {1.0, 0.5}`, `.size()`,
  `.fill(v)`, range-based `for`. `Buffer<T>`: numbers, sized by
  `resize(n)` in `prepare()`, a member only.
- Locals have a value when declared: `double y = 0.0;`, `auto& v = voices[i];`.
  References bind to the selected variable or member; a `const` reference
  to a converted number holds a temporary value. Assignments evaluate the
  right-hand value before selecting the target, as in C++20.
  A function's local storage, including temporaries, is limited to 64 KiB,
  and the functions along any chain of calls to 256 KiB together; keep
  larger arrays and structs as plugin members.
- Expressions, blocks and types nest at most a few hundred deep (a chain
  such as `a + b + c` counts a level per link).
- `if`/`else`, `switch`, `for`, range-based `for`, `while`, `do`, `break`,
  `continue`, `return`, `? :`.
- Conversions: `static_cast<float>(x)` between numbers; float to int only
  through `toInt` (toward zero), `floorToInt`, `ceilToInt`, `roundToInt`,
  which are defined for every input.

Not in the subset, each refused with a hint: pointers and `nullptr`, C arrays
(`Array` instead), `new`/`delete`, `std::` and other includes (the runtime has
its own `sin`, `min`, `clamp`...), templates, lambdas, `virtual`, inheritance
other than the plugin's, constructors and destructors, `class` (write
`struct`), operator overloading, exceptions, `goto`, recursion, static locals,
unions, `typedef`/`using`, macros and the preprocessor, `sizeof`, namespaces,
`if` with an initializer, `if constexpr`, the comma operator (except in a
`for`'s step), character literals, `constexpr` functions, `noexcept` with a
condition (plain `noexcept` is fine), and declaring a function apart from
its definition.

## Defined behaviour

A script cannot do what C++ leaves undefined. The compiler refuses:

- integer `/` or `%` by anything but a positive constant: use `idiv(a, b)`
  and `imod(a, b)` (0 when `b` is 0), or `wrap(i, n)` for an index;
- shifts by anything but a constant 0 to 31;
- a float stored in an int without one of the conversions above;
- two operands of one expression that both change the same thing
  (`r.next() - r.next()` on one `Random`), or where one changes what the
  other uses, the two sides of an assignment included (`a[++i] = i`:
  compilers disagree on it);
- a variable or member without a starting value;
- constant int arithmetic that overflows (`2147483647 + 1`); values computed
  at run time wrap.

At run time, invalid indices are reported with their source line. An index
outside a numeric `Array`, `Buffer` or channel reads 0; enum elements read
the value with underlying integer 0. An `Array` of trivially copyable
aggregates (plain structs of numbers/enums, including nested arrays) resets
its spare element to the type's default-constructed value on **each invalid
access**, including declared member defaults. Writes affect only that spare
and are discarded by the next invalid access. A reference to the spare
aliases it and observes the next reset too.

An `Array` whose elements hold resources (`Buffer`, `DelayLine`,
`Oversampler`, `Stft`, or structs containing them) instead keeps a persistent
spare: invalid accesses share its state, including earlier writes. Resetting
such an element could allocate or free on the audio thread. Valid elements
are never touched by invalid accesses. These rules are identical in native,
JIT and interpreter execution.

Each hook has a
budget of loop iterations and calls (65,536 per frame in `process`); a
script that spends it is stopped and silent until it is reset or reloaded,
and the log names the line.

What `prepare()` sizes may hold 1 GiB per instance, all resources together:
a `Buffer` its elements, a `DelayLine` its ring (the delay rounded up to a
power of two, in doubles), an `Stft` 64 bytes per sample of its size, an
`Oversampler` 16 KiB. That is far beyond any delay, table or transform a
plugin needs (a 10-second stereo delay at 192 kHz is about 34 MB). A
request past it is refused before anything is allocated, and the script is
stopped and silent (`script.runtime.memory`) until it is prepared again; a
saved revision that asks for more is not loaded, and the previous version
keeps playing.

These guards are the player's. A source export (`dsp_export` with
`format: "source"`) is ordinary C++: invalid indices follow the same spare
rules and are reported without a line, but there is no budget, so a loop that never ends
hangs the plugin. Test a script in the player before exporting its source.

Inspect a loaded instance with `fx_describe` (DAW) or `plugin_describe`
(sandbox): its `problems` report loop budgets, memory refused in
`prepare()`, invalid indices and failed reloads with a source line when
known and a corrective hint. `silent: true` means that problem stopped the
audio. Audio observations clear on reset, activation or adoption of
replacement code (refused memory only on the last two); reads never clear
them.

## Real time

`process()`, and everything it calls, never allocates: `DelayLine::prepare`
and `Buffer::resize` belong in `prepare()`, and the compiler says so
anywhere else. There is no network or clock access, and files come only
through `AudioFile` and `Convolver` members.

## The runtime library

Math (all in double; `min`, `max`, `clamp`, `abs` also take ints): `sin`
`cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `exp` `exp2`
`log` `log2` `log10` `pow` `sqrt` `cbrt` `floor` `ceil` `trunc` `round`
`fmod` `hypot` `abs` `min` `max` `clamp` `lerp(a, b, t)` `sign` `isFinite`,
and the constants `pi`, `twoPi`.

Conversions: `dbToGain(db)`, `gainToDb(g)` (floored at -120), `mtof(note)`,
`ftom(hz)`, `toInt` `floorToInt` `ceilToInt` `roundToInt`, `idiv` `imod`
`wrap`. `compressorGainDb(levelDb, thresholdDb, ratio, kneeDb)` is a
compressor's curve: the gain change in dB (0 or less) for a level, with a
soft knee `kneeDb` wide (0 is hard).

DSP objects (each starts silent; `clear()` returns it there):

| Object | Use |
|---|---|
| `Biquad` | `lowpass` `highpass` `bandpass` `notch` `allpass` `(hz, q)`, `peak` `lowShelf` `highShelf` `(hz, q, gainDb)`; `process(x)`; `responseDb(hz)` |
| `OnePole` | `setCutoff(hz)`; `lowpass(x)`, `highpass(x)` |
| `EnvelopeFollower` | A compressor's detector: `set(attackMs, releaseMs)`; `process(x)` gives the level (linear, rectified); `level()` |
| `DelayLine` | `prepare(maxSamples)` in `prepare()`; `push(x)` once per frame; `read(delaySamples)` (fractional, 0 is the sample just pushed) |
| `Oscillator` | `setShape(Oscillator::Shape::Saw)` (Sine, Saw, Square, Triangle, anti-aliased), `setFrequency(hz)`, `setPulseWidth`, `setPhase`, `next()` |
| `Adsr` | `set(attackMs, decayMs, sustain, releaseMs)`, `gate(on)`, `next()`, `active()` |
| `Oversampler` | For anything nonlinear, one per channel: `prepare(factor)` (1, 2, 4, 8, 16; 1 passes samples through) in `prepare()`; per sample `up(x)`, then `set(k, f(sample(k)))` for `k < factor()`, then `down()`; `latency()` (report it with `setLatency`). Minimum-phase filters, 3 to 5 samples of latency; `prepare(factor, Oversampler::Phase::Linear)` for linear phase (no phase change, so a dry signal delayed by `latency()` mixes exactly; 65 to 75 samples) |
| `Stft` | Spectral effects, one per channel: `prepare(size, overlap)` in `prepare()` (`1024, 4`); per sample `if (s.push(x))` change bins `0 .. bins()-1`, then `y = s.pull()`, `latency()` samples late (report it). Bins: `magnitude(k)` (a sine of amplitude 1 reads 1), `phase(k)`, `real(k)`, `imag(k)`, `set(k, re, im)`, `setPolar(k, mag, phase)`, `scale(k, gain)`, `binHz(k)`. An analyser that only reads bins calls `prepareAnalysis(size, overlap)` instead, at about half the cost: `push(x)` alone each sample, bins readable until the next `push()`, no output |
| `Random` | `seed(n)`, `next()` (0..1), `bipolar()`, `gaussian()`, `below(n)`; the same sequence everywhere for a seed |
| `Noise` | `seed(n)`; `white()`, `pink()`, `brown()` |

Coefficients are cheap to set in `update()`; setting them per sample (for a
smoothed cutoff, say) works too.

## Compiler messages

Each message has a line, a column, a code and a hint that says what
to write instead:

```
tilt.cpp:12:9: error: integer division needs a positive constant divisor [script.ub.division]
    hint: use idiv(a, b) or imod(a, b)
```

Fix the first error first: later ones often follow from it. The codes group
by area: `script.parse.*` (syntax), `script.unsupported.*` (outside the
subset), `script.type.*`, `script.name.*`, `script.call.*`,
`script.plugin.*`, `script.param.*` and `script.source.*` (the plugin's
shape), `script.ub.*`
(undefined in C++), `script.rt.*` (real-time rules), `script.editor.*`,
`script.shared.*` and `script.format.*` (the editor's rules).
