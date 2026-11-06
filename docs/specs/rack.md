# Spec: modular racks

**Status:** In progress (ADR-0062). Sections marked *(planned)* are part of
the design but not yet built; `STATUS.md` says what is.
**Decision record:** ADR-0062.
**Code:** `modules/rack` (the rack runtime, MIT), `modules/stock` (stock
modules, MIT), `plugins/suite/runtime` and `plugins/suite/compiler` (the
module language), `host/engine` (model, commands, host modules, the rack
node), `daw/gui/app/rack_*` (the window).

---

## 1. Vocabulary

- **Rack**: a patching space, homed on a channel. `rk_N`.
- **Module**: an instance of a **module type** in a rack. `mo_N`.
- **Module type**: compiled module code, by its id
  (`com.suppleaudio.rack.vco`). Stock (built in), library (the user's
  `modules` folder), project (the project's `modules` folder) or host
  (written in C++ in the engine).
- **Port**: a module's input or output, by key (`in`, `pitch`, `out`).
- **Cable**: from an output port to an input port or a parameter. `cb_N`.
- **End**: `"<object>.<key>"`: `"mo_3.out"`, `"mo_4.cutoff"`, `"fx_9.cutoff"`.

## 2. Session model

Project schema `supple.project/2.7` adds three collections and a route end.
Fields equal to their defaults are omitted, as everywhere (session-model §5).

```json
"racks": {
  "rk_1": {"track": "tr_3", "name": "Bells"}
},
"modules": {
  "mo_1": {"rack": "rk_1", "type": "com.suppleaudio.rack.notes", "version": "1.0.0",
           "x": 40, "y": 60, "params": {"voices": 8}},
  "mo_2": {"rack": "rk_1", "type": "com.suppleaudio.rack.vco", "version": "1.0.0",
           "label": "Body", "x": 180, "y": 60, "params": {"shape": "saw", "fine": -7}},
  "mo_3": {"rack": "rk_1", "type": "com.suppleaudio.rack.out", "version": "1.0.0",
           "label": "Bells", "x": 520, "y": 60},
  "mo_4": {"rack": "rk_1", "type": "com.suppleaudio.rack.plugin", "version": "1.0.0",
           "x": 340, "y": 60, "fx": "fx_12"}
},
"cables": {
  "cb_1": {"from": "mo_1.pitch", "to": "mo_2.pitch"},
  "cb_2": {"from": "mo_2.saw", "to": "mo_4.in_l"},
  "cb_3": {"from": "mo_5.out", "to": "mo_2.fine", "amount": 12, "color": "violet"}
}
```

### 2.1 Rack

| Field | Meaning |
|---|---|
| `track` | Its home channel (required). Removing the track removes its racks. |
| `name` | Shown on its tab; `""` (default) shows the home track's label. |

### 2.2 Module

| Field | Meaning |
|---|---|
| `rack` | The rack it is in (required). |
| `type` | The module type's id (required). An unknown type is kept, silent, and reported (`rack.get` `problems`). |
| `version` | The type's version when the module was added or last updated. |
| `label` | The user's name for it (`""`: the type's name). An Out module's label is what the input menu lists. |
| `x`, `y` | Its top-left corner in the rack's space, design units (finite, ±1e6). |
| `params` | Knob values by key in physical units (a percent as 30, a choice by key, a toggle as a bool), as a semantic plugin's patch params. Missing keys are defaults; canonical: only non-defaults are written. |
| `bypass` | Bypassed: see §4.6. |
| `source` | Notes modules: the track whose MIDI items it plays (unset: the rack's home track). |
| `device` | Notes modules: a live MIDI input it always plays, armed or not, matched as a MIDI route's device (a case-insensitive substring; `""`: any input). Unset: none. |
| `fx` | Plugin modules (required there, refused elsewhere): its plug-in instance, an `fx_N` in no chain. |
| `learn` | MIDI learn on its parameters, as an FX's (`ParamLearn`). |

### 2.3 Cable

| Field | Meaning |
|---|---|
| `from` | `"mo_N.<output key>"` (required). |
| `to` | `"mo_N.<input key>"`, `"mo_N.<param key>"` or `"fx_N.<param key>"` for a plugin module's plug-in (required). |
| `amount` | Present exactly when `to` is a parameter: the movement at a value of 1, in the parameter's modulation unit (§3.4). |
| `color` | A track colour name or `"#rrggbb"`; `""` (default): by signal kind. |

Both ends are in the same rack. At most one cable joins the same two ends.

### 2.4 Routes with a module end

`RouteEnd` gains `module`:

- **From an Out module** to a channel: the channel's record input
  (`track.record.input`). The channel always hears it, armed or not, before
  its items and inserts (as timeline material, not as monitored input).
  `"from": {"module": "mo_3"}`.
- **To an In module** from any track: a send, with the route's tap, level,
  pan, mute, channel map and envelopes. `"to": {"module": "mo_9"}`. Sends
  into In modules are never Console 9 encoded (the rack is not a summing
  stage); they carry the tap's signal at the route's level.

### 2.5 Invariants (`checkInvariants`)

- Every rack's track exists and is a channel; every module's rack exists;
  cable ends' modules exist, are in one rack, and the from end's module is
  not the to end's when the type is known to have no feedback path (a module
  may patch into itself: that is a feedback cable).
- A plugin module's `fx` exists and is in no chain and no other module;
  every fx is in exactly one chain or one module.
- A route with a module end: from an Out module only to a channel (and is
  that channel's `record.input`), to an In module only from a track.
- No cycle through the mixer: a track that reaches a rack's In module
  (directly or through other routes and racks) is not fed by that rack's
  Out modules. (`rack.cycle`)
- Limits: 64 racks, 512 modules and 4096 cables per rack, 32 Out and 32 In
  modules per rack.

### 2.6 Migration

2.6 projects load unchanged (no racks); the writer writes 2.7.

## 3. Modules: the language

A module is a DSP script (ADR-0032) whose struct derives from `Module`.
Everything the language reference (`skill/guides/dsp-language.md`) says
holds, except where this section says otherwise. The module guide
(`skill/guides/modules.md`) is the reference agents read.

### 3.1 Identity

```cpp
static constexpr ModuleInfo info = {
    .name = "VCO", .id = "com.example.vco", .vendor = "Example", .version = "1.0.0",
    .description = "One line: what it is for.", .url = "",
    .tags = {"oscillator", "poly"},   // at most 4, lower case, digits and '-'
    .width = 0, .height = 0};         // design units; 0: from the automatic layout
```

The id follows `Info::id`'s rules. A library holds one module per id.

### 3.2 Ports

```cpp
Input  pitch = pitchIn("Pitch").help("1 per octave; 0 is C4.");
Input  fm    = cvIn("FM").help("...");
Input  sync  = gateIn("Sync");
Output saw   = audioOut("Saw");
```

| Factory | Signal |
|---|---|
| `audioIn`, `audioOut` | audio, ±1 |
| `pitchIn`, `pitchOut` | octaves from C4 |
| `gateIn`, `gateOut` | 0/1, high at ≥ 0.5 |
| `cvIn`, `cvOut` | −1..1 |
| `unipolarIn`, `unipolarOut` | 0..1 |
| `phaseIn`, `phaseOut` | 0..1 ramp |

Modifiers: `.help("...")`, `.normal(v)` (an input's value while nothing is
connected; 0 by default), `.in("Group")` (its section in the automatic
layout), `.withKey("key")`. A port's key is its member's name in
snake_case; keys of ports and parameters share one namespace per module.
At most 32 inputs, 32 outputs and 256 parameters.

| Method | |
|---|---|
| `in[c][i]` | Input channel `c` at frame `i`: a mono cable reaches every `c`; unconnected, the normal value; past a poly cable's channels, 0 |
| `in.channels()` | 0 unconnected, else the cable's channels (several cables: the widest) |
| `in.connected()` | Whether anything is connected |
| `out[c][i] = x` | Output channel `c` at frame `i` |
| `out.setChannels(n)` | 1..16, in `process()`; unset, `channels()` |
| `out.channels()` | |

Outputs arrive zeroed for each block.

**Declaration arrays.** `Params<N>`, `Inputs<N>` and `Outputs<N>` are direct
module members initialized from one parameter/port factory spec. Declare
one bank per line. `N` is a positive literal or constexpr int. Indexing is
zero-based, accepts a runtime int and yields an ordinary `Param`, `Input`
or `Output` reference; `.size()` is the fixed count. Element names append
`" 1"` .. `" N"` to the spec's name, and keys append `_1` .. `_N` to the
member's snake_case key (or the `.withKey()` stem). Display names do not
choose keys in either backend. A bank derives each parameter ID from the
complete element key; `.withId()` is refused. All other spec modifiers are
replicated; declaration-time element references in presets and activity
conditions require constant indices. Do not assign or copy banks in scripts;
helper parameters may be references.

Each module has at most **256 parameters total**, array elements included,
and **32 inputs and 32 outputs total**. An out-of-range bank index reports
a fault and returns a reset spare: parameters and input samples read zero,
output writes are discarded; additional channel/frame faults are reported
normally. Indexing never allocates. The flat metadata and automatic layout
see each element as an ordinary declaration.

### 3.3 The module base

| Method | |
|---|---|
| `sampleRate()` | |
| `maxBlockSize()` | 32: the rack's grid |
| `channels()` | The widest connected input or modulating cable, at least 1 |
| `transport()` | `Transport` at the block's start (playing, tempo, beats, barStart, time signature) |
| `seed()` | An `int` from the project's seed and the module's id: the same every render |
| `trace(...)` | As a plugin's |

Hooks: `prepare()`, `update()`, `process(int frames)` (`frames` 1..32),
and for its face `panel(Panel&)`, `draw(Canvas&)`, `pointer(const
Pointer&)`; `factoryPatches(Patches&)` for presets. `process()` is called
once per grid block, or once per frame for modules in a feedback loop.

### 3.4 Parameters and cables

Parameters are declared as a plugin's. In a module:

- `p`, `p.value()`: the knob, with its automation, at the block's start.
  `update()` runs when one changed, as in a plugin.
- `p.at(i)`: at frame `i`, glided when `.smoothed()`, plus the cables into
  it of channel 0.
- `p.at(i, c)`: the same for channel `c` (a mono cable reaches every
  channel).
- Toggles take no cables. A choice or integer moved by cables reads as a
  real number from `at()`; round it (`roundToInt`).

A cable moves a parameter by `amount × value`: for a frequency the amount is
in octaves (`value · 2^(amount·x)`), otherwise in the parameter's unit (a
percent in percent, dB, semitones, ms), clamped to the parameter's range.

**Helpers.** Member/free functions may take `const Input&`, `Output&`,
`Param&` and `const Param&`, including indexed elements. They preserve the
host bindings and obey the ordinary audio/UI, read-only-parameter and
evaluation-order rules.

**Defined bits.** `shl(x, n)`, `shr(x, n)`, `bit(x, n)` and `popcount(x)`
operate on the 32-bit representation of ints. Left shifts discard overflow;
right shifts zero-fill; `bit` returns 0/1. Counts outside 0..31 return zero
for shifts and bit reads. `popcount` counts ones, including the sign bit.
Ordinary shift operators retain their constant-count restrictions.

**Gates.** `Trigger::rose(x)` updates a hysteresis gate (rise at >=0.5,
fall below 0.25) and reports only a rising edge; `high()` reads it and
`clear()` resets it. `Pulse::fire()` restarts a 1 ms pulse, rounded up to a
whole frame at the instance's sample rate; `fire(frames)` sets an explicit
length (nonpositive clears). `next()` consumes one frame, returning 1 while
active and 0 afterwards; `clear()` resets it. Both are small copyable value
types, usually held in `Array<Trigger, 16>` / `Array<Pulse, 16>` for polyphony.

### 3.5 The face

Without `panel()`, the automatic layout (§4.5). With it:

```cpp
void panel(Panel& p) {
    p.size(120, 300);
    p.knob(tune, 60, 50);            // centre; optional size (24 small, 36 standard, 52 large)
    p.knob(fine, 60, 110, 24);
    p.choice(shape, 10, 150, 100, 20);
    p.toggle(sync, 10, 180, 100, 20);
    p.input(pitch, 30, 240);         // a jack's centre
    p.output(saw, 90, 240);
    p.display(10, 200, 100, 30);     // where draw() is expected to draw (a hint for layout checks)
}
```

`draw(Canvas&)` paints under the controls in the module's coordinates;
`pointer()` hears the mouse where no control is, as in a plugin editor.
`Shared` members carry what the audio code shows (sent ~60 times a second
while the rack window is open).

### 3.6 Refused in modules

`Source` members and routes, voice hooks and `Instrument` methods (poly
cables replace them), file members (*planned*), `setLatency`, `setTail`
(a rack has no tail of its own), `offline()`-dependent behaviour is allowed
but discouraged. Each refusal names the module way (`script.module.*`).

### 3.7 Code interface

`scriptc::moduleCode(std::shared_ptr<const Program>)` returns a retaining
adapter implementing `supple::script::detail::ModuleCode`; null for a plugin
or null Program. `NativeModuleCode<T>` implements the same interface for
C++ modules. It exposes `valid()` and `error()` for native description
validation: above 256 parameters or 32 ports per direction it retains the
inspectable description but refuses `create()` (null) and all hooks (null),
without throwing a validation exception.

`ModuleCode` (`supple/script_module.h`) describes the `ModuleInfoModel`,
parameters (`ParamModel`), ports (`PortModel`), shared state and factory
patches, and exposes create/destroy/copy and hooks `prepare`, `update`,
`process(ctx, self, frames)`, `panel`, `draw`, `pointer`. Array elements are
flat models with independent keys and offsets. `Program::kind()` reports
`PluginKind::Module`; its plugin `process()` hook is null for a module.
The runtime record is `supple::script::detail::Instance`: `PortBinding`
binds ports, `ParamBinding` binds parameters and cable modulation, and
`Instance::channels`, `transport`, `seed` carry the module's context. The
caller installs `CurrentScope` around hooks and sets the process budget to
`65536 * frames`.

### 3.8 The library

The user's modules live in the library, `<data>/<slug>/modules` (the app's
data folder; `AppOptions::moduleDir`, `<workdir>/modules` under
`--workdir`): `<name>/<name>.cpp`, one module each, the name 1–64
lower-case letters, digits, `_` or `-`. The engine compiles every one when
it starts (`loadModuleLibrary`); one that does not compile, or whose id a
built-in type or an earlier name has, is left out. `module.write`,
`module.edit` and `module.fork` save a file, compile it and, when it
compiles, put its type in the catalog in place of the old one
(`EngineApp::setModules`): the next plan builds new instances of every
module playing it, with their knobs (§4.1: catalog generations). A version
that does not compile is saved but leaves racks on the last that did. A
library module may not take a host or stock type's id (`module.fork`
copies one under a new id). Offline renders and background jobs use the
catalog of the moment they start (`OfflineSettings::modules`).

## 4. The rack runtime (`modules/rack`)

### 4.1 Types

- `rack::ModuleType`: one module type: its code (a `ModuleCode`, or a host
  module's descriptor), its description and its automatic layout.
- `rack::Catalog`: module types by id (stock, library, project, host), with
  generations so racks notice changed code.
- `rack::Instance`: one module's state (the struct, its runtime record,
  its parameter slots and telemetry). Lives across schedule rebuilds.
- `rack::Schedule`: an immutable compiled rack: order, feedback groups,
  cable buffers, port and parameter bindings. Built on the main thread.
- `rack::HostModule`: what a host module implements: `process(HostBlock&)`
  given its port buffers and the block's context.

### 4.2 Processing

`Schedule::process(const Block&)` (`SUPPLE_RT`): the host passes the
node's frames, its timeline position and a callback for per-grid-block
context (transport, MIDI). The schedule splits the frames on the 32-frame
grid of that position, and for each grid block:

1. evaluates parameter values (slots, automation curves the host gave,
   smoothing) and calls `update()` where they changed;
2. zeroes outputs, fills modulation buffers (`at(i, c)`) from cables;
3. runs modules in order: an acyclic module once for the block, a
   feedback group frame by frame;
4. publishes telemetry when due.

A module stopped by its budget, or failing, outputs silence until reset or
reloaded and is reported; the rest of the rack plays on.

### 4.3 Order and feedback

Kahn's algorithm over modules ordered by id; strongly connected components
(Tarjan) become feedback groups, each placed where its first module would
be. Inside a group modules run in id order, and a cable from a later to an
earlier (or the same) module reads the previous frame. Such cables are
`feedback: true` in `rack.get`.

### 4.4 Polyphony and buffers

Every output owns 16 × 32 doubles. An input with one cable reads the source
buffer directly; with several, a sum buffer. Channel counts are set before
dependents run (feedback reads the previous frame's count).

### 4.5 Automatic layout

Without `panel()`: width from the content, in columns of 44 units (a 36-unit
knob and its label); knobs in declaration order, by group, two to four
columns; then inputs, then outputs, in rows of jacks (22-unit jacks on a
36-unit pitch) labelled under them. A title strip of 24 units on top. The
layout is computed in the runtime (`rack::layout()`), so commands report
sizes and the GUI and a picture agree.

### 4.6 Bypass

A bypassed module does not run. Each output takes the first input of the
same signal kind not already passed through (in declaration order); outputs
with none are silent.

### 4.7 Telemetry

Per instance, a triple buffer the audio side fills at most every ~16 ms of
audio: each output port's last value and peak (channel 0) and channel
count, each parameter's last modulated value (channel 0), and the `Shared`
bytes. The GUI reads the newest; nothing waits.

## 5. Host modules

All `com.suppleaudio.rack.<name>`, version 1.0.0.

| Type | Ports | Parameters | Notes |
|---|---|---|---|
| `out` | in `l` (audio), `r` (audio, normalled to `l`) | `level` dB −60..12 (0) | Its label names it in the input menu; poly cables are summed |
| `in` | out `l`, `r` (audio) | `level` dB −60..12 (0) | Fed by sends (`route.add {to: {module}}`) |
| `notes` | out `pitch`, `gate`, `velocity` (unipolar), `pressure` (unipolar), `retrigger` (gate), poly; `bend` (cv), `modwheel` (unipolar) mono | `voices` 1..16 (8), `mode` (`rotate`, `reuse`, `reset`, `mono_last`, `mono_low`, `mono_high`), `bend_range` semitones 0..24 (2) | MIDI from `source` (default: the home track): its MIDI items and its live MIDI input when it monitors |
| `transport` | out `playing` (gate), `reset` (trigger at play and locate), `bar` and `beat` (triggers), `clock` (trigger at `division`), `bar_phase`, `beat_phase` (phase), `tempo` (cv: bpm / 300) | `division` (1/1 … 1/64, dotted, triplets; 1/16) , `swing` % 0..75 (0) | From the timeline: positions are exact at every frame |
| `plugin` | in `in_l`, `in_r` (audio, `in_r` normalled to `in_l`; poly cables summed), `pitch`, `gate`, `velocity` (poly, normal 0.8); out `out_l`, `out_r` (audio) | the plug-in's, as cable targets `fx_N.<key>` | Its `fx` is an ordinary FX instance in no chain; clicking the module opens its editor. Each gate channel is a voice: rising to 0.5 or above plays key round(60 + 12 × pitch) at its velocity, falling below 0.25 (or its cable going) ends it |

## 6. Engine integration (`host/engine`)

- **Node.** Each rack with at least one module compiles to a rack node in
  the plan's node list, topologically after the tracks that send to its In
  modules and before the channels its Out modules feed. Racks without a
  path to any output are not compiled. A rack node has no strip stages.
- **Edges.** A send to an In module is an edge into the rack node at its
  tap; a channel's record input from an Out module is an edge from the rack
  node into the channel's input sum (linear, no Console 9 stage).
- **Timing.** Read-ahead PDC treats the node as a strip with no chain
  latency. Its timeline shift is the node's; MIDI events and transport use
  it. The grid is aligned to the node's timeline position.
- **MIDI.** Notes modules receive the compiled MIDI events of their source
  tracks (items, loops, playrate), and live MIDI from the device when the
  source monitors it; the node is then live.
- **Live.** A rack is live when it consumes live MIDI or a live track sends
  to it; liveness propagates to what it feeds.
- **Parameters.** Knob edits on a running rack go to atomic slots (no
  compile). Envelopes on module parameters compile to curves the schedule
  samples on the grid. MIDI learn (`module.learn.*`) moves a knob through
  `module.set`, so it reaches the same slots.
- **Plugin modules.** Their instances come from the instance pool by fx id
  and run as stages of the rack node (`Node::rackStages`, prepared and
  automated as chain stages), in sub-blocks on the grid, with notes made
  from their pitch and gate cables. A cable into `fx_N.<key>` (a semantic
  key, a parameter id or its name) rides one of the plugin module's 16
  hidden knobs `mod_1`..`mod_16` (−1..1, never listed); once per grid
  block the module sends each parameter its cables' sum (first voice) times
  the parameter's CLAP range as a CLAP `param_mod` event, and 0 once when
  its last cable goes. The parameter must be modulatable; VST3 plug-ins take
  no modulation (a plan warning says so, as for a seventeenth cable).
  Their latency is not compensated inside the rack. A bridged plugin module
  in a live rack passes through, as on a live strip.
- **State.** Module instances are kept per rack across plans (keyed by
  module id and code generation) as plugin instances are; offline renders
  build their own from the session. What a plan gives a module (a plugin
  module's stage, buffers and modulation, an In module's buffers) lives in
  the plan and is attached on the audio thread each block, since the
  previous plan may still be running the same instance.
- **Errors.** `rack.cycle`, `rack.unknown_type`, `rack.port`, `rack.param`,
  `rack.limit` at edit time; plan warnings for missing types and plugin
  latency inside a rack.

## 7. Commands

Declared in `host/engine/src/rack_commands.cpp`; summaries ≤ 80 characters,
examples run as tests. Ends are strings: `"mo_3.out"`.

| Command | Kind | Args → result |
|---|---|---|
| `rack.add` | edit | `{track?, name?}` → `{rack, track, out}`: homes a rack on `track` (or a new channel named after it), adds an Out module and makes it the channel's input |
| `rack.remove` | edit | `{rack}` |
| `rack.set` | edit | `{rack, name?, track?}` |
| `rack.list` | query | `{}` → racks with their home tracks, module counts and Out modules |
| `rack.get` | query | `{rack, detail?: brief\|full}` → modules (id, type, label, changed params, position, size), cables (with `feedback`), Out modules and the channels they feed, problems |
| `rack.probe` | query | `{rack, ports: ["mo_N.<output>", …≤8], seconds? (≤10, 2), from_beat?, notes? (notation the rack's Notes modules play)}` → per port: peak, rms, dc, min, max; audio (the first 4 voices summed): dB and pitch (Hz, note, cents) when periodic; gates over the voices: count, per second, per beat, duty; pitch: the notes visited; CV: rate; a one-line description. An offline render of a copy with a listener (`ProbeModule`) on each port; nothing changes |
| `rack.patch` | edit | `{rack, modules: {name: {type\|plugin, params?, preset?, label?, x?, y?, source?, device?}}, cables?: [[from, to, amount?, color?] \| {from, to, amount?, color?}], x?, y?}` → `{modules: {name: id}, cables}`: ends `"name.port"`, `"name.knob"` (a plugin module's: its plug-in's) or `"mo_N.port"`; modules without a place laid out by signal flow; checked as `module.add` and `cable.add` check, all or nothing |
| `rack.preset.save` | action | `{rack, name, modules?, description?, replace?}` → `{path, modules, cables}`: the modules (default the whole rack) and the cables among them |
| `rack.preset.load` | edit | `{rack, name\|path, x?, y?, io?: join\|add}` → `{modules, cables, installed?, warnings?}`: one undo step; `installed` names the library modules it brought |
| `rack.preset.list` | query | `{query?}` → presets: name, path, description, module and cable counts, module types |
| `rack.preset.delete` | action | `{name}` → `{path}` |
| `module.list` | query | `{query?, tag?}` → types: id, name, tags, one line, origin |
| `module.describe` | query | `{type\|module}` → parameters (key, unit, range, default, help, group, choices), ports (key, signal, help, normal), size, presets |
| `module.add` | edit | `{rack, type\|plugin, label?, x?, y?, params?, preset?, source?, device?}` → `{module, type, x, y, width, height, inputs, outputs, knobs, fx?, plugin?}` |
| `module.remove` | edit | `{module}` (its cables; a plugin module's fx; an Out module's routes) |
| `module.set` | edit | `{module, params?, label?, x?, y?, bypass?, source?, preset?}` |
| `module.duplicate` | edit | `{modules, dx?, dy?, cables?}` → new ids |
| `module.randomize` | edit | `{module, amount?, seed?}` |
| `module.write` | action | `{name, source}` → `{name, path, ok, diagnostics, type?, reloaded?, playing?}`: `type` as `module.describe`; `reloaded` counts the modules rebuilt; `playing` when a broken version leaves the last good one in use |
| `module.edit` | action | `{type\|name, edits: [{old, new}]}` → as `module.write` (library modules only) |
| `module.check` | query | `{source\|name}` → `{ok, diagnostics, type?}` |
| `module.get` | query | `{type\|name}` → `{type, origin, source, name?, path?}` (host modules have none) |
| `module.fork` | action | `{type, id, name?}` → as `module.write`: the type's source under a new id, in the library |
| `module.learn.set` | edit | `{module, param, midi, mode?, soft_takeover?}` → `{param, control}`: a MIDI controller turns the knob along its travel (`rack::knobPosition`), one undo step per movement; not a plugin module's (`fx.learn.set`) |
| `module.learn.remove` | edit | `{module, param}` |
| `module.learn.list` | query | `{module?}` → `{learn: [{module, param, midi, control, mode?, soft_takeover?}]}` |
| `cable.add` | edit | `{from, to, amount?, color?}` → `{cable, feedback, warnings}` |
| `cable.remove` | edit | `{cable}` or `{from, to}` |
| `cable.set` | edit | `{cable, amount?, color?}` |

Errors carry `code`, `message`, `path` and a `hint` naming what exists
(`"mo_2 has inputs pitch, fm, sync"`).

A module's knob is automated with `env.add {module, param, points}`
(values in the knob's unit, a choice by its key); a plugin module's knobs
are its plug-in's (`env.add {fx}`). `preset` on `module.add` and
`module.set` is one of the type's factory presets (`module.describe`
lists them): every knob goes to it, then `params` apply on top. The app
reaches racks through `EngineApp::modules()` (the catalog),
`EngineApp::moduleInstance()` (a running module's telemetry) and
`supple/engine/racks.h`. `<slug>-engine modules --markdown` prints every
built-in type's ports and knobs (`moduleReference()`), committed as
`skill/generated/rack-modules.md` and checked by `engine.registry-drift`.

### 7.1 Rack presets

A rack preset is a group of modules and the cables among them, kept
apart from any project in `<rackPresetDir>/<name>.json`
(`AppOptions::rackPresetDir`; the app's is `<data>/<slug>/rack-presets`,
default `<workdir>/rack-presets`). A name is 1–200 characters without
`/ \ : * ? " < > |` or control characters, not ending in a space or a
dot. The file is canonical JSON:

```json
{"schema": "supple.rack-preset/1.0", "name": "Tremolo", "description": "…",
 "modules": [{"type": "com.suppleaudio.rack.vca", "version": "1.0.0", "label": "Amp",
              "x": 0, "y": 0, "params": {"level": -6}, "bypass": false},
             {"type": "com.suppleaudio.rack.plugin", "x": 220, "y": 0, "fx": {…}}],
 "cables": [{"from": {"module": 2, "port": "out"},
             "to": {"module": 1, "port": "gain", "plugin": true}, "amount": 0.3}],
 "sources": {"com.example.wobble": {"name": "wobble", "source": "#include …"}}}
```

Modules are by index, placed relative to the group's top-left. A cable's
end names a module by index and a port, or with `plugin: true` a
parameter of that plugin module's plug-in. A plugin module's `fx` is an
FX chain entry (the plug-in, its semantic patch or base64 state). What
belongs to a project stays out: a Notes module's source track (it plays
the rack's channel), MIDI learn, envelopes, sends and the channels an
Out feeds.

`sources` holds the source of every library or project module the preset
uses, by type id, with its library name, so one file carries a patch and
its custom modules (ADR-0062). A load first writes each one the catalog
lacks into the library (under its name, or `<name>-2`… when that name is
taken), as `module.write` does, and lists them in `installed`; one the
catalog has plays as installed, with a warning (`rack.preset_module`) when
its source differs, and one that does not compile here refuses the load.
Then a load checks everything else before the model changes. A module type or
plug-in that is not installed refuses the preset (`rack.unknown_type`,
`chain.plugin_missing`); a knob or port its type no longer has is left
out with a warning (`rack.preset_knob`, `rack.preset_cable`). With
`io: join` (the default) the preset's Out and In modules are the rack's
own, in order: their cables reach the rack's Outs and Ins, so a whole
patch saved from one rack sounds in a new one at once; extra ones are
added. `io: add` adds them all.

## 8. The rack window (`daw/gui`)

A separate, resizable utility window (`FloatingPanel`), like the routing
matrix's, with its placement in the UI sidecar. Opened by the toolbar's
Rack button, View → Rack (`ui.rack`), and a channel's menu (Show rack, Add
rack). Racks are tabs along its top, with `+` for a new rack (on the
selected channel, or a new one).

- **The space.** A plain background, nothing else, until modules are
  added. Middle-drag or Space-drag pans, Ctrl+wheel zooms at the pointer
  (25 %–400 %), the wheel scrolls. Per-rack view (offset, zoom) in the UI
  sidecar.
- **Modules** are panels placed anywhere, dragged freely by their body
  (not their controls); several are selected with a marquee or Ctrl+click
  and moved together. Right-click: rename, bypass, presets, duplicate,
  randomize, delete, show source (scripts), open editor (plugins).
- **Jacks** are circles, coloured by signal kind, lit by their level.
  Dragging from a jack draws a cable that follows the pointer; dropping on
  a jack of the other direction connects, on a knob makes a modulation
  cable (its amount set by dragging the ring that appears around the
  knob). Dragging a connected input's cable away moves or removes it.
  Cables sag, are drawn above modules, and fade while not hovered when
  many cross.
- **Knobs** drag vertically (Shift fine), double-click resets, show their
  modulated value as an arc; a right-click offers MIDI learn and
  automation.
- **Browser.** A searchable list of module types by tag (double-click or
  drag to add); host modules first, then stock, library, project.
- **Plugin modules** show the plug-in's name (or the module's label) and
  open its editor window on a click, as the mixer does.

## 9. Stock modules (`modules/stock`)

Each is one script, `modules/stock/<name>.cpp`, `com.suppleaudio.rack.<name>`,
following the stock conventions (physical units, a `.help()` per parameter
and port, groups, factory patches where they help). The catalogue is in
`modules/README.md`.
