# ADR-0096: Modulation: racks drive parameters through modulation routes

**Status:** Accepted, 2026-11-19. Extends ADR-0062 (modular racks) and
ADR-0093 (values on a running plan). Built in steps M1 to M4 (below).

Contracts: `specs/engine.md` §1.4.1, `specs/rack.md` §5–6,
`specs/session-model.md` "Modulation (schema 2.17, ADR-0096)".

## Context

Nate asked for modulators: any parameter can be modulated, and racks are the
sources. Racks already hold every source the feature needs: LFOs, random
and chaos modules, an envelope follower on audio sent into an In module,
note-triggered envelopes from Notes, macro knobs, and the song position from
Transport. Inside a rack, a cable into a plugin module's parameter already
becomes a CLAP `param_mod` event on the rack's 32-frame grid (rack.md §6,
"Plugin modules"). Three things are missing:

- **A way out.** Only audio (Out) and MIDI (Note Out) leave a rack today.
  No control signal can reach a parameter outside it.
- **A model object** that says which signal moves which parameter, by how
  much.
- **Engine rules** that keep this inside the engine's laws: deterministic
  renders, real time bit-identical to offline in both executors and through
  the bridge at any block size, and nothing on the audio path that
  allocates, locks or blocks.

The working design is `headline-core.md` §2 and its amendments M-a to M-k,
checked against the engine in `reports/core-check-lead.md`.

## Decision

### Source: the Mod Out module

A new host module, `com.suppleaudio.rack.mod-out` (alias `mod_out`), sits in
any rack beside Out and Note Out.

- **Inputs.** It has 16 inputs, `in_1` to `in_16` (cv, normal 0), and no
  parameters.
- **What an input carries.** Its value is channel 0 (the first voice) of
  the cables it receives, summed as any rack input sums them and clamped to
  −1..1.
- **When the value is taken.** It is sampled at each point of the rack's
  grid and held until the next one, as a plugin module's modulation is.
- **Names.** An input has no stored name. It shows the label of the module
  whose output feeds it (the first cable by id), or "Mod N" when unpatched.
- **Range.** An input reports 0..1 when every cable into it comes from a
  unipolar, gate or phase output, and −1..1 otherwise. The range is
  information for lanes, lists and telemetry; nothing scales by it.
- **Bypass.** A bypassed Mod Out holds 0 on every input.
- **Limit.** A rack holds at most 32 Mod Outs, as it holds 32 Outs.

A rack whose only way out is an enabled modulation route counts as having an
output: its Mod Outs set the node's latency as routed Outs do (rack.md §6,
"Timing").

### Route: the modulation object

A new kind, **`mr_*`**, in a new top-level collection `modulations`. The
prefix `mo` already names rack modules, and `Id::parse` would map both to
`Kind::Module`.

```json
"modulations": {
  "mr_1": {"source": {"module": "mo_7", "input": "in_1"},
           "target": {"object": "fx_3", "param": "cutoff"}, "depth": 0.25}
}
```

| Field | Meaning |
|---|---|
| `source` | `{module, input}`: a Mod Out module and one of its inputs. |
| `target` | The envelope addressing (`EnvTarget`, session-model §1.9), stored as an envelope's is: `{object, param}`, the object an `fx_*`, `tr_*` or `sd_*`. |
| `depth` | The offset at a signal of 1, in the target's unit (below). |
| `enabled` | Default true. |
| `lane` | Whether the target's lane is shown. Default true; undoable like an envelope's `visible`. |

- **Sharing.** One input may feed many routes, and many routes may sum on
  one target. Two routes with the same source input and target are refused
  (`modulation.duplicate`).
- **Cascades.** Removing the source module, or the target FX, track or
  route, removes the routes that use it. The command reports each removed
  route in a `modulation.removed` warning.
- **Persistence.** Routes are ordinary model objects: undo, history, diff,
  merge by replay and branches treat them as they treat envelopes.

### Depth and the base

- **The offset.** offset = signal × depth, where signal is −1..1 or 0..1.
  Routes on one target sum.
- **Depth units.**
  - A plug-in parameter: a fraction of its CLAP range, −1..1. The
    `param_mod` amount is depth × signal × (max − min), as the plugin module
    already sends it.
  - A mixer target: dB for track volume and send level, −48..48; percent
    for pan and width, −200..200.
  - `EnvMode::Trim` is not used for any of this.
- **Automation stays the base.** The base is whatever plays without
  modulation: the static value, the envelope, the write hold's hand.
  Modulation is an offset on top and never writes the base, the model or an
  envelope.
- **Write modes.** Touch, latch and write record the base only.
  - For a plug-in this holds by construction: CLAP modulation is
    non-destructive, so the plug-in's own value, its GUI and its output
    events keep the base.
  - For a mixer target, the engine adds the offset after the base and then
    clamps the sum to the target's range. The model, the fader and the write
    pass see the base alone.

### Targets

- **Step M1.** CLAP parameters flagged `CLAP_PARAM_IS_MODULATABLE`, of an
  FX in a track's chain or input chain, in process and bridged.
- **Step M2.** Mixer targets: a track's volume, pan and width, and a send's
  level (a route's `volume`, between strips or as a rack send).
- **Refused with a structured error and a hint.**
  - VST3 parameters. The VST3 host never sets the flag
    (`vst3_host.cpp`, parameter info), and it drops `ParamMod` events.
  - CLAP parameters without the flag, or flagged read-only or bypass.
  - Take FX and the FX of plugin modules: later.
  - Switches: mute, solo, bypass, phase invert, mono.
  - Playrate and tempo.
  - A route that closes a cycle.
- **Where flags are checked.**
  - `modulation.add` and `modulation.set` check the instance's parameter
    list (the one `fx.describe` reports).
  - Model invariants know no plug-ins: a file whose route points at a
    parameter that is not, or is no longer, modulatable loads. Every
    compile checks the flags again when it binds the instance. Such a route
    is inert and the plan warns (`modulation.target`). An FX whose plug-in
    is missing makes its routes inert the same way.
- **Latency.** No CLAP flag says a parameter changes latency: a plug-in
  reports latency only after it has changed. When a target reports a
  latency change while it is modulated:
  - the engine disables every route into that FX's modulated parameters and
    warns (`modulation.latency`), and the plan rebuild that the latency
    change causes compiles without them;
  - the routes stay disabled (engine state, not model state) until one of
    them is edited, its FX is replaced, or the project is reopened;
  - `modulation.list` reports such a route as `inert: "latency"`.
- **Step M4: an envelope gives the base.** Any automatable parameter
  becomes modulatable through its envelope, and VST3 and unmarked CLAP
  parameters take the summed value:
  - write modes record the base only;
  - `modulate` creates a one-point envelope at the current value when there
    is none.

### Order, cycles and liveness

- **Order.** A modulation route is an edge from the source rack's node to
  the target's node:
  - the target's node is the track for an FX in a chain and for a track's
    mixer targets;
  - for a send's level it is the node that applies that level (the source
    strip, or the rack for a rack send).
  - The edge joins `topoOrder`, the worker pool's dependencies and the
    latency solve.
- **Cycles.** The model's cycle check counts the edge too, so `modulation.
  add` refuses a cycle at edit time (`modulation.cycle`, naming the route
  that closes it). The compile never meets one.
- **Self-modulation stays refused.** A follower on a track's own signal
  driving that track's insert is a cycle. A later ADR may allow it only with
  a mechanism shown identical at every block size in both executors; a
  delay of "one block" is not acceptable.
- **Liveness, in both directions.** A modulation edge behaves like a MIDI
  edge:
  - a live source makes its target live, as any edge does;
  - a live target pulls its source live, as a MIDI edge's destination does
    (`plan.cpp`, the aux and MIDI frontier rule), because frontier rings
    carry only a node's main stereo pair.
  - So the source and the target of a route always run in the same
    executor, and no modulation crosses the frontier.
  - The plan warns (`modulation.live`) when a route moves a node into the
    live region. The cost it names: arming a modulated track runs its
    source rack in the callback, and everything that rack feeds. The rack's
    own audio inputs can stay rendered and reach it through the frontier
    rings.

### Alignment and delivery

- **Alignment.** A modulation edge is compensated exactly like a sidechain
  into the target stage (ADR-0051). The value the rack computed for
  timeline sample t reaches the target when the target processes t.
  - The edge's tap latency is the Mod Out's latency inside the rack,
    computed as an Out's.
  - Its `before` is the latency of the target's chain ahead of the target
    stage (the whole chain for a mixer target).
  - The edge carries a delay line of held values, sized at compile.
  - If the source's path is late, the target track waits for it, exactly as
    a sidechain makes its destination wait.
- **Plug-in targets.**
  - **When.** At each point of the target stage's absolute 32-sample
    automation grid (`kAutomationStep`), the same points its `Param`
    events use.
  - **What.** The stage sends one `PARAM_MOD` per modulated parameter: the
    sum of its enabled routes' signal × depth × range.
  - **Order.** At a frame that also carries an automation event, the
    `Param` event comes first.
  - **Block size.** These positions are functions of the timeline position
    alone, so every block size, either executor and the bridge deliver the
    same events at the same samples.
- **The send-on-change memo.** A value is sent only when it differs from the
  last one sent to that parameter.
  - **It survives compiles.** It is carried across compiles by (fx, param),
    with the other carried state (§1.6 of the engine spec). A parameter that
    a new plan no longer modulates (a route removed, disabled or made inert,
    a source rack deleted) is sent one 0 at the new plan's first grid point.
  - **It resets** on the instance's activation, on a reset
    (`host::Instance::resets()`, a seek or a play start), on a changed
    instance, and on a patch or state applied to it. After a reset the
    first grid point sends every modulated parameter, so a real-time play
    from a position and an offline render from there both start from an
    empty memo.
  - **Patch edits compile.** For that reason a patch edit to a modulated FX
    compiles instead of taking ADR-0092's patch path, as an FX with an
    active envelope does.
- **Event capacity.** A stage reserves `block / 32 + 2` events per modulated
  parameter beside its lanes' share. Events dropped for capacity, and the
  bridge's 2048-event cut, are counted (step M2) and reported as a plan
  warning instead of disappearing.
- **Mixer targets (M2).**
  - **Interpolation.** The offset at a frame is linear between the values
    at the target grid points either side of it.
  - **Lookahead.** A mixer target's edge has one more grid interval
    (32 frames) of compensation, so the later grid value exists when the
    interval plays. Interpolated offsets therefore stay aligned with the
    plug-in targets' grid values.
  - **Clamping.** The sum of base and offset is clamped to the target's
    range per sample, after automation and before the gain ramps.
- **Handing a rack between executors.** While the other executor still owns
  a rack's modules, its plan-local results are silenced (rack.md §6,
  "State"). In that branch a Mod Out instead holds its last value, and the
  emitter sends nothing for that block, so a reclassification never sends a
  spurious 0.
- **While stopped.** Racks run on while stopped (rack.md §6), and so do the
  plug-ins they modulate. A locate restarts every module, so what follows a
  locate is the offline render's. A Transport-locked source therefore plays
  what renders. Random modules take `seed()` from the project seed and the
  module id.

### Values on a running plan

- **Depth and `enabled` are mix values** (ADR-0060, ADR-0093). Each route
  has a slot that `syncMix` publishes and the emitter reads at block start,
  so a depth drag never compiles.
- **Executor gating.** Source and target share an executor (above), so a
  slot has one reader. ADR-0093's rule for edits made one after the other
  in different executors still applies.
- **What compiles.** Adding, removing, retargeting or changing a route's
  source compiles. `lane` belongs to the GUI and never changes the sound;
  until M2 gives routes their slots, a `lane` change compiles like any
  route edit, and from M2 it rides the slots' path without compiling.

### Freeze

The routes whose target is in a frozen track are part of that track's freeze
dependency hash, as its envelopes are. Editing them fails with `track.frozen`.
Their source racks are not part of the hash, just as a sidechain's source
track is not. While frozen, the print plays and the routes are inert.

### Commands

- **Step M1.**
  - `modulation.add {source, target, depth, enabled?, lane?}`;
    `modulation.set {modulation, …}`; `modulation.remove {modulation}`;
    `modulation.list {track?, fx?, rack?}` (q), which reports each route's
    signal range and why it is inert.
  - Errors: `modulation.source`, `modulation.target`,
    `modulation.duplicate`, `modulation.cycle`, `modulation.depth`,
    `modulation.limit` (4096 routes per project), and `track.frozen`.
  - `session.describe` lists routes per target track.
  - `fx.describe` reports `modulatable` per parameter.
- **Step M2.** `modulate {target, source: lfo|follower|random|macro|{module,
  input}, depth?, from?}` does the whole gesture:
  - it reuses the home track's first rack that holds a Mod Out ("Modulators"
    is only the default name for a new one);
  - it adds the source module from a preset and makes the route;
  - a follower takes `from`, the track it listens to, and the command makes
    the send into the rack's In; a cycle (the target's own track) is
    refused with a hint.
- **Telemetry (M2).** Telemetry publishes each modulated target's last
  offset and each route's range.

### Steps

| Step | Who | What |
|---|---|---|
| M1 | lead | Mod Out; the `mr` kind (schema 2.17 in the expected order, renumbered by order of landing); edges, cycles, liveness and alignment; the `PARAM_MOD` emitter with memo carry, resets and capacity; flag checks; `modulation.*`; `fx.describe` `modulatable` |
| M2 | lead | Mixer targets; depth and enabled as mix values; `modulate`; telemetry; the drop and bridge counters |
| M2b | builder | The other stock plug-ins mark their continuous parameters modulatable (never ones that restart the plug-in or change latency) |
| M3 | gui-lead | The parameter menu, the `mod` slot, rings and bars, lanes keyed by target, the FX parameter list |
| M4 | lead | Modulation through an envelope (above) |

## Consequences

- **What exists today.** After M1, modulation reaches CLAP plug-ins that
  mark parameters modulatable. Most stock plug-ins mark none until M2b, and
  third-party VST3 and unmarked CLAP parameters wait for M4.
- **Cost of a route.** Each route costs:
  - a delay line of held doubles, sized by its compensation;
  - event capacity on its target stage;
  - at most one event per grid point per modulated parameter (32 per
    1024-frame render block).
  - A sweep on 64 parameters of one bridged plug-in fills the bridge's 2048
    events per render block. The counter makes that visible.
- **Latency.** A follower whose source path is late delays its target track,
  as a sidechain does.
- **Live region.** Arming a modulated track moves its source rack into the
  callback (`modulation.live` says so).
- **Determinism.** Renders stay deterministic, and real time stays equal to
  offline in both executors, at any block size and through the bridge.
  Every position involved is a function of the timeline position, the memo
  resets with the plug-in, and source and target share an executor.
- **Platforms.** No platform-specific code is added. Native CI must pass on
  Linux, macOS and Windows before each step merges.
