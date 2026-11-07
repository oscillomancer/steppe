# ADR-0062: Modular racks

**Status:** Accepted, 2026-11-05.

Contracts: `docs/specs/rack.md` (model, runtime, engine, commands, GUI),
`skill/guides/modules.md` (the module language), `skill/guides/rack.md`
(patching, for agents). Builds on ADR-0008 (session model), ADR-0014 (live
nodes), ADR-0020/0021 (auxiliary channels), ADR-0032 (DSP scripting),
ADR-0047 (stock plugins as scripts), ADR-0051 (read-ahead PDC). Amends
ADR-0033's licence map (`modules/`).

**2026-11-07 amendment:** [ADR-0065](0065-independent-record-inputs.md)
separates a channel's MIDI
input from its audio input, fixing the rack home-channel monitoring contract.
The older single-input representation still loads. With both configured,
recording takes MIDI; see the current [rack](../specs/rack.md) and
[session-model](../specs/session-model.md) contracts.

## Context

Steppe's sound design so far happens inside plugins: a synth's patch, an
effect's settings. Modular environments (a Eurorack case, VCV Rack, Bitwig's
Grid) give another kind of ceiling: small modules joined by cables, where
any signal can drive any other, sequencing and synthesis are one thing, and
a patch can generate music on its own. Bitwig showed what such an
environment gains from living inside a DAW: notes from the track, the
transport as a clock, automation and modulation crossing the boundary, the
result on a mixer channel like any instrument.

Steppe already has most of what such an environment needs, but in pieces:
a DSP language that agents write fluently and that compiles at once (the
JIT) or as C++ (exports, stock plugins); an engine with deterministic,
block-size-invariant rendering; a typed command bus with undo, branches and
merges; MCP for agents. What it lacks is a place where small pieces of DSP
are wired together at run time, and a way for that place to feed the mixer.

Constraints found while surveying the code:

- A channel's only audio input is its record input, a route from device
  inputs (`RouteEnd::hwChannels`) or a MIDI device. Sends go to busses and
  masters only, so that every source passes a channel's Console 9 encode
  once (ADR-0019). A modular environment's outputs must therefore arrive
  where a record input does, on channels.
- Offline renders are deterministic and the real-time output bit-identical
  to them in both executors (engine.md §3). Stock plugins earn this by
  being block-size invariant; a patch of modules written by anyone must
  earn it by construction.
- The plugin `Code` interface and the glue are bound to a plugin's shape
  (buses, a parameter table, CLAP state). A module has ports, many of them,
  and polyphonic cables; it is not a plugin.
- The user asked for CLAP plugins inside the environment, opened in their
  own window as in the mixer.

## Decision

### 1. Racks, modules and cables are session objects

A **rack** is a patching space. A project has any number (at most 64), each
**homed on a channel** (`rack.track`), Bitwig style: by default the rack
plays the home channel's MIDI, its first output feeds the home channel's
input, and its parameters automate on the home channel's lanes. Removing
the home channel removes its racks.

A **module** is one instance of a module type in a rack: its type (a
module id, `com.suppleaudio.rack.vco`), its knob values (a canonical patch
object in physical units, as a semantic plugin's), its label, its free
position in the rack's space, its bypass, and for host modules their
settings. A **cable** joins one module's output port to another module's
input port, or to one of its **parameters** with an amount: every
non-toggle parameter of every module is a modulation target, without code
in the module.

They are first-class session objects with stable IDs (`rk_N`, `mo_N`,
`cb_N`), so undo, branches, object-level diffs and merges, the command log
and agent branches cover patching like everything else. An agent proposes
a patch on a branch, and the person A/B-renders it.

Routes gain a **module end** (`RouteEnd::module`): a channel's record input
may be a route *from* an Out module (the rack's audio arrives where a
device input does, before the channel's items and inserts, so it is
encoded once by the channel), and a track may send *to* an In module (with
the route's tap, level, mute and automation). The input menu lists every
Out module beside the device's inputs.

### 2. Signals

Cables carry doubles, up to **16 channels** each (polyphony, VCV style: a
mono cable reaches every channel of a poly input). Conventions, which the
stock modules and the guides follow and which `cable.add` checks for
warnings, never errors:

| Kind | Range | Meaning |
|---|---|---|
| audio | ±1 nominal | 1.0 is 0 dBFS at an Out module |
| pitch | octaves | 0 is C4 (261.63 Hz), 1 per octave |
| gate | 0 or 1 | high at 0.5 and above; triggers are 1 ms gates |
| cv | −1..1 | bipolar modulation |
| unipolar | 0..1 | envelopes, levels, probabilities |
| phase | 0..1 | a ramp per cycle (Bitwig's phase signals) |

A cable into a parameter moves it by `amount × value`, the amount in the
parameter's unit at a value of 1 (octaves for a frequency, dB for a level,
percent for a percent: the route amounts of ADR-0032's modulation
amendment), clamped to its range. Several cables into one input or
parameter sum.

### 3. Modules are DSP scripts of their own kind

A module type is a DSP script whose struct derives from `Module`: the same
language, compiler, runtime library, checks, editor canvas and guarantees
as plugins (ADR-0032), with a module's shape:

```cpp
struct Fold : Module {
    static constexpr ModuleInfo info = {.name = "Fold", .id = "com.example.fold",
                                        .tags = {"waveshaper"}};
    Input in = audioIn("In");
    Param folds = percent("Folds", 30).help("How far past the edge the wave folds.");
    Output out = audioOut("Out");

    void process(int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = sin(in[c][i] * (1 + 8 * folds.at(i, c)));
    }
};
```

- Ports are members from factories by signal kind (`audioIn`, `pitchIn`,
  `gateIn`, `cvIn`, `unipolarIn`, `phaseIn` and the `...Out` six), with
  `.help()`, `.normal(v)` (the value read while unconnected), `.in(group)`
  and `.withKey()`. A port's key is its member's name in snake_case, as a
  parameter's is. At most 32 inputs and 32 outputs.
- `channels()` is the widest connected input or modulating cable (at least
  1); outputs take that many channels unless `setChannels(n)` says.
- `p.value()` is the knob (and its automation) at the block's start;
  `p.at(i)` and `p.at(i, c)` add the cables of channel `c` (glided when
  `.smoothed()`), so every module is modulatable without code.
- Hooks: `prepare()`, `update()`, `process(int frames)`; for the face,
  `panel(Panel&)` (where controls and jacks sit; an automatic layout
  without it), `draw(Canvas&)` and `pointer(const Pointer&)` exactly as a
  plugin editor's, with `Shared` members for what the audio code shows;
  `factoryPatches()` for presets. `seed()` gives each module a seed that
  follows the project's seed and the module's id, so generative patches
  render the same every time.
- Not in modules (refused with a hint): `Source`, notes and voices (poly
  cables replace them), file members (a later amendment), `setLatency`.

A module is **one `.cpp` file**: its identity is inside it, so sharing a
module is sharing a file. The user's module library is
`<data>/<slug>/modules/<name>/<name>.cpp`, compiled by the JIT when saved
and swapped into racks that use it (`module.write`, `module.edit`, as
`dsp.write` does for plugins). **Stock modules** live in `modules/stock/`
at the repository root, MIT licensed like the plugins, compiled into the
DAW as C++ (as stock plugins are) and verified bit-identical to the JIT by
differential tests; their source ships with the app, so `module.fork`
copies one into the library under a new id to change it.

**Sharing patches.** A rack preset (`supple.rack-preset/1.0`) holds
modules, their settings and positions, and the cables among them, plus the
source of every non-stock module it uses, so one file carries a patch and
its custom modules. A project likewise keeps a copy of each library module
it uses in its folder (`<project>/modules/`), as it keeps its media, and
prefers it on open.

### 4. Host modules

A few module types are the host's, written in C++ in the engine, because
they reach what a script cannot: **Out** (stereo to a channel's input,
named: "Lead" is what the input menu lists), **In** (a track's send),
**Notes** (MIDI to poly pitch, gate, velocity and pressure, from the home
channel's or another track's MIDI items and live input), **Transport**
(play, reset, bar and beat clocks and phases, tempo, all from the
timeline), and **Plugin**: a CLAP or VST3 plugin inside the patch. A plugin
module owns an ordinary `FxInstance` (the session invariant becomes "every
fx in exactly one chain or one module"), so `fx.*`, presets, isolation,
automation, MIDI learn and the plugin's editor window apply unchanged; its
face is its name (renamable) and its jacks, and a click opens its window.
An instrument plugin plays notes made from pitch and gate cables; every
plugin parameter is a cable target (CLAP parameter modulation). The rack
runtime sees host modules through one interface, so it stays free of the
engine.

### 5. The rack runtime

`modules/rack` (MIT, depending only on the script runtime and compiler)
runs a rack: it compiles the modules and cables into a schedule, owns the
cable buffers, binds ports and parameters, and calls the modules' hooks.

- **Order and feedback.** Modules run in topological order with zero delay
  along cables. A cycle (a strongly connected component) is allowed: its
  modules run frame by frame, in a fixed order by id, and the cables that
  go backwards in that order carry the previous frame. `rack.get` marks
  them `feedback`; the GUI draws them so.
- **A fixed grid.** The rack processes sub-blocks of at most 32 frames on a
  grid aligned to the timeline position the node processes, whatever the
  host's block. Parameter values, `update()` and the transport are sampled
  on that grid, so a rack renders the same in real time, in either
  executor and offline, even if a module is not itself block-size
  invariant: determinism is the runtime's, not each author's.
- **Real time.** `process` and everything the runtime does there follow
  ADR-0032's rules; schedules, buffers and instances are made on the main
  thread. Each module runs under the script budget; one that spends it is
  silent and reported, the rest of the rack plays on.
- **State across edits.** Module instances live across schedule rebuilds:
  adding a module or a cable keeps every other module's state (an LFO's
  phase, a reverb's tail). Knob edits reach running instances through
  atomic slots without a rebuild. A module whose code changes starts fresh
  in the new code.

### 6. Engine integration

Each rack compiles to one node of the render plan, before the channels it
feeds and after the tracks that send to it. Its In modules read route
edges, its Out modules write buffers that edges deliver to channels'
inputs, linearly, before their items and inserts. **2026-11-07 amendment:** [ADR-0064](0064-rack-latency.md) supersedes
the original uncompensated rack-node timing policy. A rack is live when it
plays live MIDI or a live track sends to it, and liveness propagates as
for strips. Its MIDI comes from the source tracks' compiled events at the
rack node's own timeline shift; its transport from the plan's time map at
each grid point. Module parameters are envelope targets (`{object:
"mo_3", param: "cutoff"}`), automate on the home channel's lanes, and learn
MIDI controllers like plug-in parameters. Offline renders build their own
instances from the session, as they do plugins. Telemetry (port levels,
modulated knob positions, `Shared` state) travels to the GUI through lossy
triple buffers, never touching the signal.

A cycle through the mixer (a track that sends to a rack whose Out feeds
it, directly or further on) is refused at edit time (`rack.cycle`), as
route cycles are.

### 7. Commands

`rack.*` (add, remove, set, get, list, probe, preset save/load/list),
`module.*` (add, remove, set, move, duplicate, randomize, describe, list,
write, edit, check, get, fork) and `cable.*` (add, remove, set), in the
engine, declared once in C++ with examples, structured errors and
generated references. Port and parameter ends are written `"mo_3.cutoff"`.
`module.add` places a module beside the last unless given a position, and
returns its ports and parameters, so an agent can connect the next cable
without another query. `rack.probe` renders a rack offline for a few
seconds (with notes if asked) and reports each requested port's signal in
words and numbers (level, DC, pitch, gate rate and duty, range), so an
agent debugs a patch without listening, as `dsp.test` lets it debug a
plugin.

### 8. The rack window

A rack window is a separate, resizable utility window like the routing
matrix's, opened from the toolbar, the View menu and a channel's menu. It
is an empty, pannable and zoomable space until modules are added; modules
are placed anywhere and dragged freely; inputs and outputs are round jacks
that cables are dragged to and from (to a knob, for modulation, with a
ring for its amount). Racks are tabs. A module browser searches the stock,
library and project modules. Plugin modules show their name and open their
editor on a click. `docs/specs/gui.md` §Rack has the details.

### 9. Licensing

`modules/` (the module runtime, the stock modules, their tests) is MIT, as
`plugins/` is, so modules can be shared and a rack could one day be
exported as a plugin. The engine's host modules, the model, commands and
GUI are GPL-3.0-or-later with the rest of the DAW.

## Consequences

Racks reuse the DSP language, the compiler, the command bus, undo,
branches and the engine's scheduling instead of growing parallel versions
of them. An agent that can write a plugin can write a module, and a module
is far smaller. The engine gains a node kind and the session three object
kinds and a route end, with migration from schema 2.6. Module authors get
determinism without effort; the price is a 32-frame control grid, and
frame-by-frame processing inside feedback loops, which costs CPU. A rack
runs on one thread; several racks run in parallel. **2026-11-07 amendment:** [ADR-0064](0064-rack-latency.md) removes
the original rack latency limit; parallel paths inside a patch remain unaligned.

## Alternatives considered

- **The rack as a plugin** (a "rack player" CLAP on a channel): reuses
  hosting, but the patch would live in plugin state, outside undo, diffs,
  merges and commands, and its outputs could reach other channels only as
  a multi-out instrument's auxiliary channels through sends to busses,
  which Console 9 forbids for channels.
- **One cable delay everywhere** (VCV): order-independent and simple, but
  a chain of ten modules lags ten samples, and processing frame by frame
  everywhere is several times slower than block processing.
- **Free block sizes** with authors responsible for invariance, as with
  plugins: every agent-written module would have to get it right, and a
  feedback cable's delay would depend on the host's block.
- **Modules as plugins in a separate format** (a C ABI of their own): a
  second language and toolchain for authors, and no JIT iteration.
- **Explicit CV inputs only** (VCV): every module would declare an input
  and an attenuverter per parameter; cables into parameters give every
  knob modulation and keep modules small.
