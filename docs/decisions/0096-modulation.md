# ADR-0096: Modulation: racks drive parameters through modulation routes

**Status:** Accepted, 2026-11-19. Extends ADR-0062 (modular racks) and
ADR-0093 (values on a running plan). Built in steps M1 to M4 (below).

Contracts: `specs/engine.md` §1.4.1, `specs/rack.md` §5–6,
`specs/session-model.md` "Modulation (schema 2.16, ADR-0096)".

*Amended 2026-11-19 after review round 51 (amendments G-c, G-d and M-l):*
files that went through an older build open (§"Older builds"); every route
is an edge whatever `enabled` says; a reset marks the send memo stale
rather than emptying it; the schema is 2.16.

*Amended 2026-11-20 (the co-director's decision on gui-lead's M3
hand-back):* `modulate` refuses only a real cycle. A follower is refused
when the target already feeds the track it would hear, which no rack
avoids. A rack that would close a loop by itself is passed over for the
next candidate, or for a new Modulators rack. Such a rack either already
drives the track a follower hears, or already hears the target. An
explicitly chosen `rack` is still refused. The Modulate menu can offer
every follower that is not a real cycle.

*Amended 2026-11-20 (step M4 as built):* a route through an envelope is
bound to that parameter's automation lane (§"Targets", Step M4).
- **What the lane sends.** It sends `clamp(base + Σ depth × signal ×
  range)` at its usual emission points: its first frame, each point, and
  each 32-sample grid point while a route is bound. While the source rack
  runs in the other executor, the last sum holds.
- **VST3.** A VST3's lane runs in its normalized domain, so its range is 1.
- **What qualifies.** Stepped and enum parameters stay refused, as do
  read-only and bypass ones.
- **Commands.**
  - `modulation.add` and `modulation.set` refuse a target without an
    active envelope (`modulation.envelope`, with an `env.add` hint).
  - `modulation.list` reports such a route as `inert: "envelope"`.
  - `fx.describe` reports `modulatable_by_envelope`, and the Modulate
    menu enables for it too.
  - `modulate` adds a one-point envelope at bar 1, at the parameter's
    current value. That is the live instance's value when the engine
    runs, else the saved one.
- **Recording.** A write mode records what the plug-in reports minus the
  route's current offset, clamped to the range.

*Amended 2026-11-26 (review rounds 208 F2, 210 and 212):* a plug-in
target is also sent every modulated parameter at a run's first frame (an
offline render's first block, a locate), on the grid or not, the memo
made stale there, and at a loop wrap's first frame; an adoption keeps to
the grid, so a route an edit adds while playing first sends at the next
grid point, as does an input chain whose track is armed while playing.
Both executors' locates clear the routes' delays and the mixer targets'
interpolation. "At the first grid point" in §"Alignment and delivery"
reads "at the run's first frame" for a plug-in target. The frames before
a run's first grid point hold the routes' values there (each Mod Out
restarts at 0 and holds it to its first grid point), as an offline
render from there does. A wrap restarts nothing (the rack keeps its
modules' state and the routes' delays carry on), so it resends the sums
carried over it: the loop goes on, unlike a render from the loop start.
Before, a plug-in kept across a seek what it was sent before it, up to
31 frames, and the in-callback seek kept a mixer target's interpolation.

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

A rack whose only way out is a modulation route, enabled or not, counts as
having an output: its Mod Outs set the node's latency as routed Outs do
(rack.md §6, "Timing").

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
  (`modulation.duplicate`). (21 Nov 2026: a second enabled route from the same
  kind of source onto a target is answered with the warning
  `modulation.stacked`, naming the routes already there.)
- **Cascades.** Removing the source module, or the target FX, track or
  route, removes the routes that use it. The command reports each removed
  route in a `modulation.removed` warning.
- **Persistence.** Routes are ordinary model objects: undo, history, diff,
  merge by replay and branches treat them as they treat envelopes.

### Older builds

A build that predates schema 2.16 keeps `modulations` as an unknown section
but drops the `mr` counter from `next_ids`, and it may delete a route's
source module or target, or add a send that closes a cycle through a route.
Such a file never fails to open:

- a missing or low `mr` counter is derived from the highest `mr` id, with
  a `project.counter_derived` warning (the code ADR-0095 gives `cx`); a
  saved branch missing it allocates past every saved version's highest
  `mr` id, so no two branches give one id to different routes;
- a route whose source or target is gone or no longer fits, or that closes
  a cycle, is dropped with a `project.modulation_dropped` warning naming
  it and why.

The same rule holds for every new kind (amendment G-c).

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
  - Until step M2, a route to a mixer target (from a later build's file, or
    written by hand) is inert in the same way, reported as `target`.
- **Latency.** No CLAP flag says a parameter changes latency: a plug-in
  reports latency only after it has changed. When a target reports a
  latency change while it is modulated:
  - the engine disables every route into that FX's modulated parameters and
    warns (`modulation.latency`), and the plan rebuild that the latency
    change causes compiles without them;
  - the routes stay disabled (engine state, not model state) until one of
    them is edited, its FX is replaced, or the project is reopened;
  - `modulation.list` reports such a route as `inert: "latency"`.
- **Step M4: an envelope gives the base.** Any automatable, continuous
  parameter becomes modulatable through its envelope, and VST3 and
  unmarked CLAP parameters take the summed value (built as the dated note
  above says):
  - write modes record the base only;
  - `modulate` creates a one-point envelope at the current value when there
    is none.

### Order, cycles and liveness

- **Order.** Every modulation route, enabled or not, is an edge from the
  source rack's node to the target's node (amendment M-l): `enabled` only
  zeroes its contribution, so turning it on without a compile finds the
  order, the compensation, the executor and the source's values in place:
  - the target's node is the track for an FX in a chain and for a track's
    mixer targets;
  - for a send's level it is the node that applies that level (the source
    strip, or the rack for a rack send).
  - The edge joins `topoOrder`, the worker pool's dependencies and the
    latency solve.
- **Cycles.** The model's cycle check counts the edge too, so `modulation.
  add` refuses a cycle at edit time (`modulation.cycle`, naming the route
  that closes it). The compile never meets one. A rack modulating its own
  rack send's level is a cycle of one node (the level is applied at the
  rack) and is refused with a hint that says so; modulate the destination's
  volume instead.
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
    stage: 0 for an input-chain stage, which is outside compensation; for a
    mixer target the whole chain less one grid interval (Lookahead below).
  - The edge carries a delay line of held values, sized at compile.
  - If the source's path is late, the target track waits for it, exactly as
    a sidechain makes its destination wait.
  - **Under a master playrate** the graph runs in device frames
    (ADR-0045), and so does all of this: the edge's delay, the rack's grid
    where Mod Out samples and the target stage's 32-frame grid are counted
    in device frames, as a sidechain's delay and a plug-in's `Param` grid
    are. A Mod Out's value follows the timeline only through what its
    modules read (Transport, curves), which already map it.
- **Plug-in targets.**
  - **When.** At each point of the target stage's absolute 32-sample
    automation grid (`kAutomationStep`), the same points its `Param`
    events use.
  - **What.** The stage sends one `PARAM_MOD` per modulated parameter: the
    sum of its routes' signal × depth × range, a disabled route adding 0.
  - **Order.** At a frame that also carries an automation event, the
    `Param` event comes first.
  - **Block size.** These positions are functions of the stage's absolute
    position alone (the timeline's at unity rate), so every block size, either executor and the bridge deliver the
    same events at the same samples.
- **The send-on-change memo.** A value is sent only when it differs from the
  last one sent to that parameter.
  - **It survives compiles.** It is carried across compiles by (fx, param),
    with the other carried state (§1.6 of the engine spec). A parameter that
    a new plan no longer modulates (a route removed or made inert, a source
    rack deleted) is sent one 0 at the new plan's first grid point, and its
    entry is dropped. A disabled route adds 0, so its parameter's sum
    changes and is sent like any other change.
  - **It goes stale, never empty** (amendment M-l), on the instance's
    activation, on a reset (`host::Instance::resets()`, a seek or a play
    start), on a changed instance, and on a patch or state applied to it.
    The next grid point sends the current sum of every modulated parameter
    whatever the memo holds, and 0 to every other parameter the memo still
    lists, then drops those entries. A plug-in may keep its modulation
    across a reset (the stock template clears it only on activation), so
    emptying the memo would lose the 0 of a route removed in the same
    adoption as a seek. A real-time play from a position and an offline
    render from there therefore send every modulated parameter at the first
    grid point, and the real-time run also clears what a fresh instance
    never had.
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
  - **Lookahead.** A mixer target's edge delivers each value one grid
    interval (32 frames) early: its `before` is the chain's latency less
    32, so the later grid value exists when the interval plays. Interpolated offsets therefore stay aligned with the
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
  source compiles. `lane` belongs to the GUI and never changes the sound,
  but a `lane` change compiles like any route edit, as an envelope's
  `visible` does today. A shortcut that skips the compile (as amendment
  C-f's `contextOnly` does for the context) is possible later and is not
  part of this ADR.

### Freeze

The routes whose target is in a frozen track are part of that track's freeze
dependency hash, as its envelopes are. Editing or removing them fails with
`track.frozen`. Their source racks are not part of the hash, just as a
sidechain's source track is not: removing a route's source module, or its
rack or home track, removes the route even while the target is frozen, and
the print keeps playing. While frozen, the print plays and the routes are
inert.

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
    refused with a hint (amended 2026-11-20: only a real cycle; a rack
    that would close a loop is passed over, see the note above).
- **Telemetry (M2).** Telemetry publishes each modulated target's last
  offset and each route's range.

### Steps

| Step | Who | What |
|---|---|---|
| M1 | lead | Mod Out; the `mr` kind (schema 2.16, by order of landing, G-d); older builds' files (G-c); edges, cycles, liveness and alignment; the `PARAM_MOD` emitter with memo carry, resets and capacity; flag checks; `modulation.*`; `fx.describe` `modulatable` |
| M2 | lead | Mixer targets; depth and enabled as mix values; `modulate`; telemetry; the drop and bridge counters |
| M2b | builder | The other stock plug-ins mark their continuous parameters modulatable (never ones that restart the plug-in or change latency) |
| M3 | gui-lead | One parameter menu, the same in the mixer and the track panel: MIDI learn…, Modulate ▸ (greyed where `fx.describe` reports the parameter not `modulatable`, M-b), Show lane. MIDI learn moves off the bare right-click into that menu, with a dated note on ADR-0058 and the manual's change (M-k). The `mod` slot lists the racks of the strip's track that hold a Mod Out. Rings and bars, lanes keyed by target, the FX parameter list. gui.md and the ADR-0058 note come with M3 |
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
  goes stale with the plug-in, and source and target share an executor.
- **Platforms.** No platform-specific code is added. Native CI must pass on
  Linux, macOS and Windows before each step merges.

*Note (2026-11-19, on merging the context track):* the saved-branch counter rule is now one rule for every kind, not `mr` alone: `VersionStore::restore` raises a branch's counters to those of every version its lineage reaches (head history and redo steps), which covers the case above (session-model, "Modulation"). An id a branch removed after a fork is still held by its own earlier versions, so the raise covers it.
